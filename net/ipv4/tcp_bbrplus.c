/* Bottleneck Bandwidth and RTT (BBR) congestion control
 *
 * BBR congestion control computes the sending rate based on the delivery
 * rate (throughput) estimated from ACKs. In a nutshell:
 *
 *   On each ACK, update our model of the network path:
 *      bottleneck_bandwidth = windowed_max(delivered / elapsed, 10 round trips)
 *      min_rtt = windowed_min(rtt, 10 seconds)
 *   pacing_rate = pacing_gain * bottleneck_bandwidth
 *   cwnd = max(cwnd_gain * bottleneck_bandwidth * min_rtt, 4)
 *
 * The core algorithm does not react directly to packet losses or delays,
 * although BBR may adjust the size of next send per ACK when loss is
 * observed, or adjust the sending rate if it estimates there is a
 * traffic policer, in order to keep the drop rate reasonable.
 *
 * Here is a state transition diagram for BBR:
 *
 *             |
 *             V
 *    +---> STARTUP  ----+
 *    |        |         |
 *    |        V         |
 *    |      DRAIN   ----+
 *    |        |         |
 *    |        V         |
 *    +---> PROBE_BW ----+
 *    |      ^    |      |
 *    |      |    |      |
 *    |      +----+      |
 *    |                  |
 *    +---- PROBE_RTT <--+
 *
 * A BBR flow starts in STARTUP, and ramps up its sending rate quickly.
 * When it estimates the pipe is full, it enters DRAIN to drain the queue.
 * In steady state a BBR flow only uses PROBE_BW and PROBE_RTT.
 * A long-lived BBR flow spends the vast majority of its time remaining
 * (repeatedly) in PROBE_BW, fully probing and utilizing the pipe's bandwidth
 * in a fair manner, with a small, bounded queue. *If* a flow has been
 * continuously sending for the entire min_rtt window, and hasn't seen an RTT
 * sample that matches or decreases its min_rtt estimate for 10 seconds, then
 * it briefly enters PROBE_RTT to cut inflight to a minimum value to re-probe
 * the path's two-way propagation delay (min_rtt). When exiting PROBE_RTT, if
 * we estimated that we reached the full bw of the pipe then we enter PROBE_BW;
 * otherwise we enter STARTUP to try to fill the pipe.
 *
 * BBR is described in detail in:
 *   "BBR: Congestion-Based Congestion Control",
 *   Neal Cardwell, Yuchung Cheng, C. Stephen Gunn, Soheil Hassas Yeganeh,
 *   Van Jacobson. ACM Queue, Vol. 14 No. 5, September-October 2016.
 *
 * There is a public e-mail list for discussing BBR development and testing:
 *   https://groups.google.com/forum/#!forum/bbr-dev
 *
 * NOTE: BBR might be used with the fq qdisc ("man tc-fq") with pacing enabled,
 * otherwise TCP stack falls back to an internal pacing using one high
 * resolution timer per TCP socket and may use more resources.
 */
#include <linux/module.h>
#include <net/tcp.h>
#include <linux/inet_diag.h>
#include <linux/inet.h>
#include <linux/random.h>
#include <linux/win_minmax.h>

/* Scale factor for rate in pkt/uSec unit to avoid truncation in bandwidth
 * estimation. The rate unit ~= (1500 bytes / 1 usec / 2^24) ~= 715 bps.
 * This handles bandwidths from 0.06pps (715bps) to 256Mpps (3Tbps) in a u32.
 * Since the minimum window is >=4 packets, the lower bound isn't
 * an issue. The upper bound isn't an issue with existing technologies.
 */
#define BW_SCALE 24
#define BW_UNIT (1 << BW_SCALE)

#define BBRPLUS_SCALE 8 /* scaling factor for fractions in BBR (e.g. gains) */
#define BBRPLUS_UNIT (1 << BBRPLUS_SCALE)

/* BBR has the following modes for deciding how fast to send: */
enum bbrplus_mode {
    BBRPLUS_STARTUP,    /* ramp up sending rate rapidly to fill pipe */
    BBRPLUS_DRAIN,  /* drain any queue created during startup */
    BBRPLUS_PROBE_BW,   /* discover, share bw: pace around estimated bw */
    BBRPLUS_PROBE_RTT,  /* cut inflight to min to probe min_rtt */
};

/* BBR congestion control block */
struct bbrplus {
    u32 min_rtt_us;         /* min RTT in min_rtt_win_sec window */
    u32 min_rtt_stamp;          /* timestamp of min_rtt_us */
    u32 probe_rtt_done_stamp;   /* end time for BBRPLUS_PROBE_RTT mode */
    struct minmax bw;   /* Max recent delivery rate in pkts/uS << 24 */
    u32 rtt_cnt;        /* count of packet-timed rounds elapsed */
    u32     next_rtt_delivered; /* scb->tx.delivered at end of round */
    u64 cycle_mstamp;        /* time of this cycle phase start */
    u32     mode:3,          /* current bbrplus_mode in state machine */
        prev_ca_state:3,     /* CA state on previous ACK */
        packet_conservation:1,  /* use packet conservation? */
        restore_cwnd:1,      /* decided to revert cwnd to old value */
        round_start:1,       /* start of packet-timed tx->ack round? */
        cycle_len:4,         /* phases in this PROBE_BW gain cycle */
        tso_segs_goal:7,     /* segments we want in each skb we send */
        idle_restart:1,      /* restarting after idle? */
        probe_rtt_round_done:1,  /* a BBRPLUS_PROBE_RTT round at 4 pkts? */
        unused:8,
        lt_is_sampling:1,    /* taking long-term ("LT") samples now? */
        lt_rtt_cnt:7,        /* round trips in long-term interval */
        lt_use_bw:1;         /* use lt_bw as our bw estimate? */
    u32 lt_bw;           /* LT est delivery rate in pkts/uS << 24 */
    u32 lt_last_delivered;   /* LT intvl start: tp->delivered */
    u32 lt_last_stamp;       /* LT intvl start: tp->delivered_mstamp */
    u32 lt_last_lost;        /* LT intvl start: tp->lost */
    u32 pacing_gain:10, /* current gain for setting pacing rate */
        cwnd_gain:10,   /* current gain for setting cwnd */
        full_bw_cnt:3,  /* number of rounds without large bw gains */
        cycle_idx:3,    /* current index in pacing_gain cycle array */
        has_seen_rtt:1, /* have we seen an RTT sample yet? */
        unused_b:5;
    u32 prior_cwnd; /* prior cwnd upon entering loss recovery */
    u32 full_bw;    /* recent bw, to estimate if pipe is full */
    /* For tracking ACK aggregation: */
    u64 ack_epoch_mstamp;   
    /* start of ACK sampling epoch */
    u16 extra_acked[2];     
    /* max excess data ACKed in epoch */
    u32 ack_epoch_acked:20, /* packets (S)ACKed in sampling epoch */
        extra_acked_win_rtts:5, /* age of extra_acked, in round trips */
        extra_acked_win_idx:1,  /* current index in extra_acked array */
        unused1:6;
};

#define CYCLE_LEN   8   /* number of phases in a pacing gain cycle */

/* Window length of bw filter (in rounds): */
static const int bbrplus_bw_rtts = CYCLE_LEN + 2;
/* Window length of min_rtt filter (in sec): */
static const u32 bbrplus_min_rtt_win_sec = 10;
/* Minimum time (in ms) spent at bbrplus_cwnd_min_target in BBRPLUS_PROBE_RTT mode: */
static const u32 bbrplus_probe_rtt_mode_ms = 200;
/* Skip TSO below the following bandwidth (bits/sec): */
static const int bbrplus_min_tso_rate = 1200000;

/* We use a high_gain value of 2/ln(2) because it's the smallest pacing gain
 * that will allow a smoothly increasing pacing rate that will double each RTT
 * and send the same number of packets per RTT that an un-paced, slow-starting
 * Reno or CUBIC flow would:
 */
static const int bbrplus_high_gain  = BBRPLUS_UNIT * 2885 / 1000 + 1;
/* The pacing gain of 1/high_gain in BBRPLUS_DRAIN is calculated to typically drain
 * the queue created in BBRPLUS_STARTUP in a single round:
 */
static const int bbrplus_drain_gain = BBRPLUS_UNIT * 1000 / 2885;
/* The gain for deriving steady-state cwnd tolerates delayed/stretched ACKs: */
static const int bbrplus_cwnd_gain  = BBRPLUS_UNIT * 2;

enum bbrplus_pacing_gain_phase {
    BBRPLUS_BW_PROBE_UP     = 0,
    BBRPLUS_BW_PROBE_DOWN   = 1,
    BBRPLUS_BW_PROBE_CRUISE = 2,
};


/* The pacing_gain values for the PROBE_BW gain cycle, to discover/share bw: */
static const int bbrplus_pacing_gain[] = {
    BBRPLUS_UNIT * 5 / 4,   /* probe for more available bw */
    BBRPLUS_UNIT * 3 / 4,   /* drain queue and/or yield bw to other flows */
    BBRPLUS_UNIT, BBRPLUS_UNIT, BBRPLUS_UNIT,   /* cruise at 1.0*bw to utilize pipe, */
    BBRPLUS_UNIT, BBRPLUS_UNIT, BBRPLUS_UNIT    /* without creating excess queue... */
};
/* Randomize the starting gain cycling phase over N phases: */
static const u32 bbrplus_cycle_rand = 7;

/* Try to keep at least this many packets in flight, if things go smoothly. For
 * smooth functioning, a sliding window protocol ACKing every other packet
 * needs at least 4 packets in flight:
 */
static const u32 bbrplus_cwnd_min_target = 4;

/* To estimate if BBRPLUS_STARTUP mode (i.e. high_gain) has filled pipe... */
/* If bw has increased significantly (1.25x), there may be more bw available: */
static const u32 bbrplus_full_bw_thresh = BBRPLUS_UNIT * 5 / 4;
/* But after 3 rounds w/o significant bw growth, estimate pipe is full: */
static const u32 bbrplus_full_bw_cnt = 3;

/* "long-term" ("LT") bandwidth estimator parameters... */
/* The minimum number of rounds in an LT bw sampling interval: */
static const u32 bbrplus_lt_intvl_min_rtts = 4;
/* If lost/delivered ratio > 20%, interval is "lossy" and we may be policed: */
static const u32 bbrplus_lt_loss_thresh = 50;
/* If 2 intervals have a bw ratio <= 1/8, their bw is "consistent": */
static const u32 bbrplus_lt_bw_ratio = BBRPLUS_UNIT / 8;
/* If 2 intervals have a bw diff <= 4 Kbit/sec their bw is "consistent": */
static const u32 bbrplus_lt_bw_diff = 4000 / 8;
/* If we estimate we're policed, use lt_bw for this many round trips: */
static const u32 bbrplus_lt_bw_max_rtts = 48;

/* Gain factor for adding extra_acked to target cwnd: */
static const int bbrplus_extra_acked_gain = BBRPLUS_UNIT;
/* Window length of extra_acked window. Max allowed val is 31. */
static const u32 bbrplus_extra_acked_win_rtts = 10;
/* Max allowed val for ack_epoch_acked, after which sampling epoch is reset */
static const u32 bbrplus_ack_epoch_acked_reset_thresh = 1U << 20;
/* Time period for clamping cwnd increment due to ack aggregation */
static const u32 bbrplus_extra_acked_max_us = 100 * 1000;

/* Each cycle, try to hold sub-unity gain until inflight <= BDP. */
static const bool bbrplus_drain_to_target = true;   /* default: enabled */

extern bool tcp_snd_wnd_test(const struct tcp_sock *tp,
                 const struct sk_buff *skb,
                 unsigned int cur_mss);

/* Do we estimate that STARTUP filled the pipe? */
static bool bbrplus_full_bw_reached(const struct sock *sk)
{
    const struct bbrplus *bbrplus = inet_csk_ca(sk);

    return bbrplus->full_bw_cnt >= bbrplus_full_bw_cnt;
}

static void bbrplus_set_cycle_idx(struct sock *sk, int cycle_idx)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    bbrplus->cycle_idx = cycle_idx;
    bbrplus->pacing_gain = bbrplus->lt_use_bw ?
                            BBRPLUS_UNIT : bbrplus_pacing_gain[bbrplus->cycle_idx];
}

u32 bbrplus_max_bw(const struct sock *sk);
u32 bbrplus_inflight(struct sock *sk, u32 bw, int gain);
u32 bbrplus_max_bw(const struct sock *sk);

static void bbrplus_drain_to_target_cycling(struct sock *sk,
                                                    const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u32 elapsed_us =
                tcp_stamp_us_delta(tp->delivered_mstamp, bbrplus->cycle_mstamp);
    u32 inflight, bw;
    if (bbrplus->mode != BBRPLUS_PROBE_BW)
        return;

    /* Always need to probe for bw before we forget good bw estimate. */
    if (elapsed_us > bbrplus->cycle_len * bbrplus->min_rtt_us) {
        /* Start a new PROBE_BW probing cycle of [2 to 8] x min_rtt. */
        bbrplus->cycle_mstamp = tp->delivered_mstamp;
        bbrplus->cycle_len = CYCLE_LEN - prandom_u32_max(bbrplus_cycle_rand);
        bbrplus_set_cycle_idx(sk, BBRPLUS_BW_PROBE_UP);  /* probe bandwidth */
        return;
    }
    /* The pacing_gain of 1.0 paces at the estimated bw to try to fully
     * use the pipe without increasing the queue.
     */
    if (bbrplus->pacing_gain == BBRPLUS_UNIT)
        return;
    inflight = rs->prior_in_flight;  /* what was in-flight before ACK? */
    bw = bbrplus_max_bw(sk);
    /* A pacing_gain < 1.0 tries to drain extra queue we added if bw
     * probing didn't find more bw. If inflight falls to match BDP then we
     * estimate queue is drained; persisting would underutilize the pipe.
     */
    if (bbrplus->pacing_gain < BBRPLUS_UNIT) {
        if (inflight <= bbrplus_inflight(sk, bw, BBRPLUS_UNIT))
            bbrplus_set_cycle_idx(sk, BBRPLUS_BW_PROBE_CRUISE); /* cruise */
        return;
    }
    /* A pacing_gain > 1.0 probes for bw by trying to raise inflight to at
     * least pacing_gain*BDP; this may take more than min_rtt if min_rtt is
     * small (e.g. on a LAN). We do not persist if packets are lost, since
     * a path with small buffers may not hold that much. Similarly we exit
     * if we were prevented by app/recv-win from reaching the target.
     */
    if (elapsed_us > bbrplus->min_rtt_us &&
            (inflight >= bbrplus_inflight(sk, bw, bbrplus->pacing_gain) ||
            rs->losses ||         /* perhaps pacing_gain*BDP won't fit */
            rs->is_app_limited || /* previously app-limited */
            !tcp_send_head(sk) || /* currently app/rwin-limited */
            !tcp_snd_wnd_test(tp, tcp_send_head(sk), tp->mss_cache))) {
            bbrplus_set_cycle_idx(sk, BBRPLUS_BW_PROBE_DOWN);  /* drain queue */
            return;
    }
}


/* Return maximum extra acked in past k-2k round trips,
 * where k = bbrplus_extra_acked_win_rtts.
 */
static u16 bbrplus_extra_acked(const struct sock *sk)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    return max(bbrplus->extra_acked[0], bbrplus->extra_acked[1]);
}


/* Return the windowed max recent bandwidth sample, in pkts/uS << BW_SCALE. */
u32 bbrplus_max_bw(const struct sock *sk)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    return minmax_get(&bbrplus->bw);
}

/* Return the estimated bandwidth of the path, in pkts/uS << BW_SCALE. */
static u32 bbrplus_bw(const struct sock *sk)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    return bbrplus->lt_use_bw ? bbrplus->lt_bw : bbrplus_max_bw(sk);
}

/* Return rate in bytes per second, optionally with a gain.
 * The order here is chosen carefully to avoid overflow of u64. This should
 * work for input rates of up to 2.9Tbit/sec and gain of 2.89x.
 */
static u64 bbrplus_rate_bytes_per_sec(struct sock *sk, u64 rate, int gain)
{
    rate *= tcp_mss_to_mtu(sk, tcp_sk(sk)->mss_cache);
    rate *= gain;
    rate >>= BBRPLUS_SCALE;
    rate *= USEC_PER_SEC;
    return rate >> BW_SCALE;
}

/* Convert a BBR bw and gain factor to a pacing rate in bytes per second. */
static u32 bbrplus_bw_to_pacing_rate(struct sock *sk, u32 bw, int gain)
{
    u64 rate = bw;

    rate = bbrplus_rate_bytes_per_sec(sk, rate, gain);
    rate = min_t(u64, rate, sk->sk_max_pacing_rate);
    return rate;
}

/* Initialize pacing rate to: high_gain * init_cwnd / RTT. */
static void bbrplus_init_pacing_rate_from_rtt(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u64 bw;
    u32 rtt_us;

    if (tp->srtt_us) {      /* any RTT sample yet? */
        rtt_us = max(tp->srtt_us >> 3, 1U);
        bbrplus->has_seen_rtt = 1;
    } else {             /* no RTT sample yet */
        rtt_us = USEC_PER_MSEC;  /* use nominal default RTT */
    }
    bw = (u64)tp->snd_cwnd * BW_UNIT;
    do_div(bw, rtt_us);
    sk->sk_pacing_rate = bbrplus_bw_to_pacing_rate(sk, bw, bbrplus_high_gain);
}

/* Pace using current bw estimate and a gain factor. In order to help drive the
 * network toward lower queues while maintaining high utilization and low
 * latency, the average pacing rate aims to be slightly (~1%) lower than the
 * estimated bandwidth. This is an important aspect of the design. In this
 * implementation this slightly lower pacing rate is achieved implicitly by not
 * including link-layer headers in the packet size used for the pacing rate.
 */
static void bbrplus_set_pacing_rate(struct sock *sk, u32 bw, int gain)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u32 rate = bbrplus_bw_to_pacing_rate(sk, bw, gain);

    if (unlikely(!bbrplus->has_seen_rtt && tp->srtt_us))
        bbrplus_init_pacing_rate_from_rtt(sk);
    if (bbrplus_full_bw_reached(sk) || rate > sk->sk_pacing_rate)
        sk->sk_pacing_rate = rate;
}

/* Return count of segments we want in the skbs we send, or 0 for default. */
static u32 bbrplus_tso_segs_goal(struct sock *sk)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    return bbrplus->tso_segs_goal;
}

static void bbrplus_set_tso_segs_goal(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u32 min_segs;

    min_segs = sk->sk_pacing_rate < (bbrplus_min_tso_rate >> 3) ? 1 : 2;
    bbrplus->tso_segs_goal = min(tcp_tso_autosize(sk, tp->mss_cache, min_segs),
                 0x7FU);
}

/* Save "last known good" cwnd so we can restore it after losses or PROBE_RTT */
static void bbrplus_save_cwnd(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    if (bbrplus->prev_ca_state < TCP_CA_Recovery && bbrplus->mode != BBRPLUS_PROBE_RTT)
        bbrplus->prior_cwnd = tp->snd_cwnd;  /* this cwnd is good enough */
    else  /* loss recovery or BBRPLUS_PROBE_RTT have temporarily cut cwnd */
        bbrplus->prior_cwnd = max(bbrplus->prior_cwnd, tp->snd_cwnd);
}

static void bbrplus_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    if (event == CA_EVENT_TX_START && tp->app_limited) {
        bbrplus->idle_restart = 1;
        bbrplus->ack_epoch_mstamp = tp->tcp_mstamp;
        bbrplus->ack_epoch_acked = 0;

        /* Avoid pointless buffer overflows: pace at est. bw if we don't
         * need more speed (we're restarting from idle and app-limited).
         */
        if (bbrplus->mode == BBRPLUS_PROBE_BW)
            bbrplus_set_pacing_rate(sk, bbrplus_bw(sk), BBRPLUS_UNIT);
    }
}

/* Find target cwnd. Right-size the cwnd based on min RTT and the
 * estimated bottleneck bandwidth:
 *
 * cwnd = bw * min_rtt * gain = BDP * gain
 *
 * The key factor, gain, controls the amount of queue. While a small gain
 * builds a smaller queue, it becomes more vulnerable to noise in RTT
 * measurements (e.g., delayed ACKs or other ACK compression effects). This
 * noise may cause BBR to under-estimate the rate.
 *
 * To achieve full performance in high-speed paths, we budget enough cwnd to
 * fit full-sized skbs in-flight on both end hosts to fully utilize the path:
 *   - one skb in sending host Qdisc,
 *   - one skb in sending host TSO/GSO engine
 *   - one skb being received by receiver host LRO/GRO/delayed-ACK engine
 * Don't worry, at low rates (bbrplus_min_tso_rate) this won't bloat cwnd because
 * in such cases tso_segs_goal is 1. The minimum cwnd is 4 packets,
 * which allows 2 outstanding 2-packet sequences, to try to keep pipe
 * full even with ACK-every-other-packet delayed ACKs.
 */
static u32 bbrplus_bdp(struct sock *sk, u32 bw, int gain)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u32 bdp;
    u64 w;

    /* If we've never had a valid RTT sample, cap cwnd at the initial
     * default. This should only happen when the connection is not using TCP
     * timestamps and has retransmitted all of the SYN/SYNACK/data packets
     * ACKed so far. In this case, an RTO can cut cwnd to 1, in which
     * case we need to slow-start up toward something safe: TCP_INIT_CWND.
     */
    if (unlikely(bbrplus->min_rtt_us == ~0U))   /* no valid RTT samples yet? */
        return TCP_INIT_CWND;  /* be safe: cap at default initial cwnd*/

    w = (u64)bw * bbrplus->min_rtt_us;

    /* Apply a gain to the given value, then remove the BW_SCALE shift. */
    bdp = (((w * gain) >> BBRPLUS_SCALE) + BW_UNIT - 1) / BW_UNIT;

    return bdp;
}

static u32 bbrplus_quantization_budget(struct sock *sk, u32 cwnd, int gain)
{

    /* Allow enough full-sized skbs in flight to utilize end systems. */
    cwnd += 3 * bbrplus_tso_segs_goal(sk);

    return cwnd;
}

/* Find inflight based on min RTT and the estimated bottleneck bandwidth. */
u32 bbrplus_inflight(struct sock *sk, u32 bw, int gain)
{   
    u32 inflight;
    inflight = bbrplus_bdp(sk, bw, gain);
    inflight = bbrplus_quantization_budget(sk, inflight, gain);
    return inflight;

}

/* Find the cwnd increment based on estimate of ack aggregation */
static u32 bbrplus_ack_aggregation_cwnd(struct sock *sk)
{
    u32 max_aggr_cwnd, aggr_cwnd = 0;
    if (bbrplus_extra_acked_gain && bbrplus_full_bw_reached(sk)) {
        max_aggr_cwnd = ((u64)bbrplus_bw(sk) * bbrplus_extra_acked_max_us)
            / BW_UNIT;
        aggr_cwnd = (bbrplus_extra_acked_gain * bbrplus_extra_acked(sk))
            >> BBRPLUS_SCALE;
        aggr_cwnd = min(aggr_cwnd, max_aggr_cwnd);
    }
    return aggr_cwnd;
}


/* An optimization in BBR to reduce losses: On the first round of recovery, we
 * follow the packet conservation principle: send P packets per P packets acked.
 * After that, we slow-start and send at most 2*P packets per P packets acked.
 * After recovery finishes, or upon undo, we restore the cwnd we had when
 * recovery started (capped by the target cwnd based on estimated BDP).
 *
 * TODO(ycheng/ncardwell): implement a rate-based approach.
 */
static bool bbrplus_set_cwnd_to_recover_or_restore(
    struct sock *sk, const struct rate_sample *rs, u32 acked, u32 *new_cwnd)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u8 prev_state = bbrplus->prev_ca_state, state = inet_csk(sk)->icsk_ca_state;
    u32 cwnd = tp->snd_cwnd;

    /* An ACK for P pkts should release at most 2*P packets. We do this
     * in two steps. First, here we deduct the number of lost packets.
     * Then, in bbrplus_set_cwnd() we slow start up toward the target cwnd.
     */
    if (rs->losses > 0)
        cwnd = max_t(s32, cwnd - rs->losses, 1);

    if (state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery) {
        /* Starting 1st round of Recovery, so do packet conservation. */
        bbrplus->packet_conservation = 1;
        bbrplus->next_rtt_delivered = tp->delivered;  /* start round now */
        /* Cut unused cwnd from app behavior, TSQ, or TSO deferral: */
        cwnd = tcp_packets_in_flight(tp) + acked;
    } else if (prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery) {
        /* Exiting loss recovery; restore cwnd saved before recovery. */
        bbrplus->restore_cwnd = 1;
        bbrplus->packet_conservation = 0;
    }
    bbrplus->prev_ca_state = state;

    if (bbrplus->restore_cwnd) {
        /* Restore cwnd after exiting loss recovery or PROBE_RTT. */
        cwnd = max(cwnd, bbrplus->prior_cwnd);
        bbrplus->restore_cwnd = 0;
    }

    if (bbrplus->packet_conservation) {
        *new_cwnd = max(cwnd, tcp_packets_in_flight(tp) + acked);
        return true;    /* yes, using packet conservation */
    }
    *new_cwnd = cwnd;
    return false;
}

/* Slow-start up toward target cwnd (if bw estimate is growing, or packet loss
 * has drawn us down below target), or snap down to target if we're above it.
 */
static void bbrplus_set_cwnd(struct sock *sk, const struct rate_sample *rs,
             u32 acked, u32 bw, int gain)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u32 cwnd = 0, target_cwnd = 0;

    if (!acked)
        return;

    if (bbrplus_set_cwnd_to_recover_or_restore(sk, rs, acked, &cwnd))
        goto done;

    /* If we're below target cwnd, slow start cwnd toward target cwnd. */
    target_cwnd = bbrplus_bdp(sk, bw, gain);
    ////
    /* Increment the cwnd to account for excess ACKed data that seems
     * due to aggregation (of data and/or ACKs) visible in the ACK stream.
     */
    target_cwnd += bbrplus_ack_aggregation_cwnd(sk);
    ////
    target_cwnd = bbrplus_quantization_budget(sk, target_cwnd, gain);
    if (bbrplus_full_bw_reached(sk))  /* only cut cwnd if we filled the pipe */
        cwnd = min(cwnd + acked, target_cwnd);
    else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
        cwnd = cwnd + acked;
    cwnd = max(cwnd, bbrplus_cwnd_min_target);

done:
    tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);   /* apply global cap */
    if (bbrplus->mode == BBRPLUS_PROBE_RTT)  /* drain queue, refresh min_rtt */
        tp->snd_cwnd = min(tp->snd_cwnd, bbrplus_cwnd_min_target);
}

/* End cycle phase if it's time and/or we hit the phase's in-flight target. */
static bool bbrplus_is_next_cycle_phase(struct sock *sk,
                    const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    bool is_full_length =
        tcp_stamp_us_delta(tp->delivered_mstamp, bbrplus->cycle_mstamp) >
        bbrplus->min_rtt_us;
    u32 inflight, bw;

    /* The pacing_gain of 1.0 paces at the estimated bw to try to fully
     * use the pipe without increasing the queue.
     */
    if (bbrplus->pacing_gain == BBRPLUS_UNIT)
        return is_full_length;      /* just use wall clock time */

    inflight = rs->prior_in_flight;  /* what was in-flight before ACK? */
    bw = bbrplus_max_bw(sk);

    /* A pacing_gain > 1.0 probes for bw by trying to raise inflight to at
     * least pacing_gain*BDP; this may take more than min_rtt if min_rtt is
     * small (e.g. on a LAN). We do not persist if packets are lost, since
     * a path with small buffers may not hold that much.
     */
    if (bbrplus->pacing_gain > BBRPLUS_UNIT)
        return is_full_length &&
            (rs->losses ||  /* perhaps pacing_gain*BDP won't fit */
             inflight >= bbrplus_inflight(sk, bw, bbrplus->pacing_gain));

    /* A pacing_gain < 1.0 tries to drain extra queue we added if bw
     * probing didn't find more bw. If inflight falls to match BDP then we
     * estimate queue is drained; persisting would underutilize the pipe.
     */
    return is_full_length ||
        inflight <= bbrplus_inflight(sk, bw, BBRPLUS_UNIT);
}

static void bbrplus_advance_cycle_phase(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);


    bbrplus->cycle_idx = (bbrplus->cycle_idx + 1) & (CYCLE_LEN - 1);
    bbrplus->cycle_mstamp = tp->delivered_mstamp;
    bbrplus->pacing_gain = bbrplus_pacing_gain[bbrplus->cycle_idx];
}

/* Gain cycling: cycle pacing gain to converge to fair share of available bw. */
static void bbrplus_update_cycle_phase(struct sock *sk,
                   const struct rate_sample *rs)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    if (bbrplus_drain_to_target) {
        bbrplus_drain_to_target_cycling(sk, rs);
        return;
    }

    if ((bbrplus->mode == BBRPLUS_PROBE_BW) && !bbrplus->lt_use_bw &&
        bbrplus_is_next_cycle_phase(sk, rs))
        bbrplus_advance_cycle_phase(sk);
}

static void bbrplus_reset_startup_mode(struct sock *sk)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    bbrplus->mode = BBRPLUS_STARTUP;
    bbrplus->pacing_gain = bbrplus_high_gain;
    bbrplus->cwnd_gain   = bbrplus_high_gain;
}

static void bbrplus_reset_probe_bw_mode(struct sock *sk)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    bbrplus->mode = BBRPLUS_PROBE_BW;
    bbrplus->pacing_gain = BBRPLUS_UNIT;
    bbrplus->cwnd_gain = bbrplus_cwnd_gain;
    bbrplus->cycle_idx = CYCLE_LEN - 1 - prandom_u32_max(bbrplus_cycle_rand);
    bbrplus_advance_cycle_phase(sk);    /* flip to next phase of gain cycle */
}

static void bbrplus_reset_mode(struct sock *sk)
{
    if (!bbrplus_full_bw_reached(sk))
        bbrplus_reset_startup_mode(sk);
    else
        bbrplus_reset_probe_bw_mode(sk);
}

/* Start a new long-term sampling interval. */
static void bbrplus_reset_lt_bw_sampling_interval(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    bbrplus->lt_last_stamp = div_u64(tp->delivered_mstamp, USEC_PER_MSEC);
    bbrplus->lt_last_delivered = tp->delivered;
    bbrplus->lt_last_lost = tp->lost;
    bbrplus->lt_rtt_cnt = 0;
}

/* Completely reset long-term bandwidth sampling. */
static void bbrplus_reset_lt_bw_sampling(struct sock *sk)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    bbrplus->lt_bw = 0;
    bbrplus->lt_use_bw = 0;
    bbrplus->lt_is_sampling = false;
    bbrplus_reset_lt_bw_sampling_interval(sk);
}

/* Long-term bw sampling interval is done. Estimate whether we're policed. */
static void bbrplus_lt_bw_interval_done(struct sock *sk, u32 bw)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u32 diff;

    if (bbrplus->lt_bw) {  /* do we have bw from a previous interval? */
        /* Is new bw close to the lt_bw from the previous interval? */
        diff = abs(bw - bbrplus->lt_bw);
        if ((diff * BBRPLUS_UNIT <= bbrplus_lt_bw_ratio * bbrplus->lt_bw) ||
            (bbrplus_rate_bytes_per_sec(sk, diff, BBRPLUS_UNIT) <=
             bbrplus_lt_bw_diff)) {
            /* All criteria are met; estimate we're policed. */
            bbrplus->lt_bw = (bw + bbrplus->lt_bw) >> 1;  /* avg 2 intvls */
            bbrplus->lt_use_bw = 1;
            bbrplus->pacing_gain = BBRPLUS_UNIT;  /* try to avoid drops */
            bbrplus->lt_rtt_cnt = 0;
            return;
        }
    }
    bbrplus->lt_bw = bw;
    bbrplus_reset_lt_bw_sampling_interval(sk);
}

/* Token-bucket traffic policers are common (see "An Internet-Wide Analysis of
 * Traffic Policing", SIGCOMM 2016). BBR detects token-bucket policers and
 * explicitly models their policed rate, to reduce unnecessary losses. We
 * estimate that we're policed if we see 2 consecutive sampling intervals with
 * consistent throughput and high packet loss. If we think we're being policed,
 * set lt_bw to the "long-term" average delivery rate from those 2 intervals.
 */
static void bbrplus_lt_bw_sampling(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u32 lost, delivered;
    u64 bw;
    u32 t;

    if (bbrplus->lt_use_bw) {   /* already using long-term rate, lt_bw? */
        if (bbrplus->mode == BBRPLUS_PROBE_BW && bbrplus->round_start &&
            ++bbrplus->lt_rtt_cnt >= bbrplus_lt_bw_max_rtts) {
            bbrplus_reset_lt_bw_sampling(sk);    /* stop using lt_bw */
            bbrplus_reset_probe_bw_mode(sk);  /* restart gain cycling */
        }
        return;
    }

    /* Wait for the first loss before sampling, to let the policer exhaust
     * its tokens and estimate the steady-state rate allowed by the policer.
     * Starting samples earlier includes bursts that over-estimate the bw.
     */
    if (!bbrplus->lt_is_sampling) {
        if (!rs->losses)
            return;
        bbrplus_reset_lt_bw_sampling_interval(sk);
        bbrplus->lt_is_sampling = true;
    }

    /* To avoid underestimates, reset sampling if we run out of data. */
    if (rs->is_app_limited) {
        bbrplus_reset_lt_bw_sampling(sk);
        return;
    }

    if (bbrplus->round_start)
        bbrplus->lt_rtt_cnt++;  /* count round trips in this interval */
    if (bbrplus->lt_rtt_cnt < bbrplus_lt_intvl_min_rtts)
        return;     /* sampling interval needs to be longer */
    if (bbrplus->lt_rtt_cnt > 4 * bbrplus_lt_intvl_min_rtts) {
        bbrplus_reset_lt_bw_sampling(sk);  /* interval is too long */
        return;
    }

    /* End sampling interval when a packet is lost, so we estimate the
     * policer tokens were exhausted. Stopping the sampling before the
     * tokens are exhausted under-estimates the policed rate.
     */
    if (!rs->losses)
        return;

    /* Calculate packets lost and delivered in sampling interval. */
    lost = tp->lost - bbrplus->lt_last_lost;
    delivered = tp->delivered - bbrplus->lt_last_delivered;
    /* Is loss rate (lost/delivered) >= lt_loss_thresh? If not, wait. */
    if (!delivered || (lost << BBRPLUS_SCALE) < bbrplus_lt_loss_thresh * delivered)
        return;

    /* Find average delivery rate in this sampling interval. */
    t = div_u64(tp->delivered_mstamp, USEC_PER_MSEC) - bbrplus->lt_last_stamp;
    if ((s32)t < 1)
        return;     /* interval is less than one ms, so wait */
    /* Check if can multiply without overflow */
    if (t >= ~0U / USEC_PER_MSEC) {
        bbrplus_reset_lt_bw_sampling(sk);  /* interval too long; reset */
        return;
    }
    t *= USEC_PER_MSEC;
    bw = (u64)delivered * BW_UNIT;
    do_div(bw, t);
    bbrplus_lt_bw_interval_done(sk, bw);
}

/* Estimate the bandwidth based on how fast packets are delivered */
static void bbrplus_update_bw(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u64 bw;

    bbrplus->round_start = 0;
    if (rs->delivered < 0 || rs->interval_us <= 0)
        return; /* Not a valid observation */

    /* See if we've reached the next RTT */
    if (!before(rs->prior_delivered, bbrplus->next_rtt_delivered)) {
        bbrplus->next_rtt_delivered = tp->delivered;
        bbrplus->rtt_cnt++;
        bbrplus->round_start = 1;
        bbrplus->packet_conservation = 0;
    }

    bbrplus_lt_bw_sampling(sk, rs);

    /* Divide delivered by the interval to find a (lower bound) bottleneck
     * bandwidth sample. Delivered is in packets and interval_us in uS and
     * ratio will be <<1 for most connections. So delivered is first scaled.
     */
    bw = (u64)rs->delivered * BW_UNIT;
    do_div(bw, rs->interval_us);

    /* If this sample is application-limited, it is likely to have a very
     * low delivered count that represents application behavior rather than
     * the available network rate. Such a sample could drag down estimated
     * bw, causing needless slow-down. Thus, to continue to send at the
     * last measured network rate, we filter out app-limited samples unless
     * they describe the path bw at least as well as our bw model.
     *
     * So the goal during app-limited phase is to proceed with the best
     * network rate no matter how long. We automatically leave this
     * phase when app writes faster than the network can deliver :)
     */
    if (!rs->is_app_limited || bw >= bbrplus_max_bw(sk)) {
        /* Incorporate new sample into our max bw filter. */
        minmax_running_max(&bbrplus->bw, bbrplus_bw_rtts, bbrplus->rtt_cnt, bw);
    }
}

/* Estimate when the pipe is full, using the change in delivery rate: BBR
 * estimates that STARTUP filled the pipe if the estimated bw hasn't changed by
 * at least bbrplus_full_bw_thresh (25%) after bbrplus_full_bw_cnt (3) non-app-limited
 * rounds. Why 3 rounds: 1: rwin autotuning grows the rwin, 2: we fill the
 * higher rwin, 3: we get higher delivery rate samples. Or transient
 * cross-traffic or radio noise can go away. CUBIC Hystart shares a similar
 * design goal, but uses delay and inter-ACK spacing instead of bandwidth.
 */
static void bbrplus_check_full_bw_reached(struct sock *sk,
                      const struct rate_sample *rs)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u32 bw_thresh;

    if (bbrplus_full_bw_reached(sk) || !bbrplus->round_start || rs->is_app_limited)
        return;

    bw_thresh = (u64)bbrplus->full_bw * bbrplus_full_bw_thresh >> BBRPLUS_SCALE;
    if (bbrplus_max_bw(sk) >= bw_thresh) {
        bbrplus->full_bw = bbrplus_max_bw(sk);
        bbrplus->full_bw_cnt = 0;
        return;
    }
    ++bbrplus->full_bw_cnt;
}

/* If pipe is probably full, drain the queue and then enter steady-state. */
static void bbrplus_check_drain(struct sock *sk, const struct rate_sample *rs)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    if (bbrplus->mode == BBRPLUS_STARTUP && bbrplus_full_bw_reached(sk)) {
        bbrplus->mode = BBRPLUS_DRAIN;  /* drain queue we created */
        bbrplus->pacing_gain = bbrplus_drain_gain;  /* pace slow to drain */
        bbrplus->cwnd_gain = bbrplus_high_gain; /* maintain cwnd */
    }   /* fall through to check if in-flight is already small: */
    if (bbrplus->mode == BBRPLUS_DRAIN &&
        tcp_packets_in_flight(tcp_sk(sk)) <= bbrplus_inflight(sk, bbrplus_max_bw(sk), BBRPLUS_UNIT))
        bbrplus_reset_probe_bw_mode(sk);  /* we estimate queue is drained */
}


/* Estimates the windowed max degree of ack aggregation.
 * This is used to provision extra in-flight data to keep sending during
 * inter-ACK silences.
 *
 * Degree of ack aggregation is estimated as extra data acked beyond expected.
 *
 * max_extra_acked = "maximum recent excess data ACKed beyond max_bw * interval"
 * cwnd += max_extra_acked
 *
 * Max extra_acked is clamped by cwnd and bw * bbrplus_extra_acked_max_us (100 ms).
 * Max filter is an approximate sliding window of 10-20 (packet timed) round
 * trips.
 */
 static void bbrplus_update_ack_aggregation(struct sock *sk,
                                                    const struct rate_sample *rs)
 {
    u32 epoch_us, expected_acked, extra_acked;
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    if (!bbrplus_extra_acked_gain || rs->acked_sacked <= 0 ||
        rs->delivered < 0 || rs->interval_us <= 0)
        return;
    if (bbrplus->round_start) {
        bbrplus->extra_acked_win_rtts = min(0x1F,
                                        bbrplus->extra_acked_win_rtts + 1);
        if (bbrplus->extra_acked_win_rtts >= bbrplus_extra_acked_win_rtts) {
            bbrplus->extra_acked_win_rtts = 0;
            bbrplus->extra_acked_win_idx = bbrplus->extra_acked_win_idx ?0 : 1;
            bbrplus->extra_acked[bbrplus->extra_acked_win_idx] = 0;
        }   
    }
    /* Compute how many packets we expected to be delivered over epoch. */
    epoch_us = tcp_stamp_us_delta(tp->delivered_mstamp,
                                    bbrplus->ack_epoch_mstamp);
    expected_acked = ((u64)bbrplus_bw(sk) * epoch_us) / BW_UNIT;
    /* Reset the aggregation epoch if ACK rate is below expected rate or
     * significantly large no. of ack received since epoch (potentially
     * quite old epoch).
     */
    if (bbrplus->ack_epoch_acked <= expected_acked ||
        (bbrplus->ack_epoch_acked + rs->acked_sacked >=
        bbrplus_ack_epoch_acked_reset_thresh)) {
        bbrplus->ack_epoch_acked = 0;
        bbrplus->ack_epoch_mstamp = tp->delivered_mstamp;
        expected_acked = 0;
    }
    /* Compute excess data delivered, beyond what was expected. */
    bbrplus->ack_epoch_acked = min(0xFFFFFU,
                                bbrplus->ack_epoch_acked + rs->acked_sacked);
    extra_acked = bbrplus->ack_epoch_acked - expected_acked;
    extra_acked = min(extra_acked, tp->snd_cwnd);
    if (extra_acked > bbrplus->extra_acked[bbrplus->extra_acked_win_idx])
        bbrplus->extra_acked[bbrplus->extra_acked_win_idx] = extra_acked;
}

/* The goal of PROBE_RTT mode is to have BBR flows cooperatively and
 * periodically drain the bottleneck queue, to converge to measure the true
 * min_rtt (unloaded propagation delay). This allows the flows to keep queues
 * small (reducing queuing delay and packet loss) and achieve fairness among
 * BBR flows.
 *
 * The min_rtt filter window is 10 seconds. When the min_rtt estimate expires,
 * we enter PROBE_RTT mode and cap the cwnd at bbrplus_cwnd_min_target=4 packets.
 * After at least bbrplus_probe_rtt_mode_ms=200ms and at least one packet-timed
 * round trip elapsed with that flight size <= 4, we leave PROBE_RTT mode and
 * re-enter the previous mode. BBR uses 200ms to approximately bound the
 * performance penalty of PROBE_RTT's cwnd capping to roughly 2% (200ms/10s).
 *
 * Note that flows need only pay 2% if they are busy sending over the last 10
 * seconds. Interactive applications (e.g., Web, RPCs, video chunks) often have
 * natural silences or low-rate periods within 10 seconds where the rate is low
 * enough for long enough to drain its queue in the bottleneck. We pick up
 * these min RTT measurements opportunistically with our min_rtt filter. :-)
 */

static void bbrplus_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    bool filter_expired;

    /* Track min RTT seen in the min_rtt_win_sec filter window: */
    filter_expired = after(tcp_jiffies32,
                   bbrplus->min_rtt_stamp + bbrplus_min_rtt_win_sec * HZ);
    if (rs->rtt_us >= 0 &&
        (rs->rtt_us <= bbrplus->min_rtt_us || filter_expired)) {
        bbrplus->min_rtt_us = rs->rtt_us;
        bbrplus->min_rtt_stamp = tcp_jiffies32;
    }

    if (bbrplus_probe_rtt_mode_ms > 0 && filter_expired &&
        !bbrplus->idle_restart && bbrplus->mode != BBRPLUS_PROBE_RTT) {
        bbrplus->mode = BBRPLUS_PROBE_RTT;  /* dip, drain queue */
        bbrplus->pacing_gain = BBRPLUS_UNIT;
        bbrplus->cwnd_gain = BBRPLUS_UNIT;
        bbrplus_save_cwnd(sk);  /* note cwnd so we can restore it */
        bbrplus->probe_rtt_done_stamp = 0;
    }

    if (bbrplus->mode == BBRPLUS_PROBE_RTT) {
        /* Ignore low rate samples during this mode. */
        tp->app_limited =
            (tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
        /* Maintain min packets in flight for max(200 ms, 1 round). */
        if (!bbrplus->probe_rtt_done_stamp &&
            tcp_packets_in_flight(tp) <= bbrplus_cwnd_min_target) {
            bbrplus->probe_rtt_done_stamp = tcp_jiffies32 +
                msecs_to_jiffies(bbrplus_probe_rtt_mode_ms);
            bbrplus->probe_rtt_round_done = 0;
            bbrplus->next_rtt_delivered = tp->delivered;
        } else if (bbrplus->probe_rtt_done_stamp) {
            if (bbrplus->round_start)
                bbrplus->probe_rtt_round_done = 1;
            if (bbrplus->probe_rtt_round_done &&
                after(tcp_jiffies32, bbrplus->probe_rtt_done_stamp)) {
                bbrplus->min_rtt_stamp = tcp_jiffies32;
                bbrplus->restore_cwnd = 1;  /* snap to prior_cwnd */
                bbrplus_reset_mode(sk);
            }
        }
    }
    bbrplus->idle_restart = 0;
}

static void bbrplus_update_model(struct sock *sk, const struct rate_sample *rs)
{
    bbrplus_update_bw(sk, rs);
    bbrplus_update_ack_aggregation(sk, rs);
    bbrplus_update_cycle_phase(sk, rs);
    bbrplus_check_full_bw_reached(sk, rs);
    bbrplus_check_drain(sk, rs);
    bbrplus_update_min_rtt(sk, rs);
}

static void bbrplus_main(struct sock *sk, const struct rate_sample *rs)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);
    u32 bw;

    bbrplus_update_model(sk, rs);

    bw = bbrplus_bw(sk);
    bbrplus_set_pacing_rate(sk, bw, bbrplus->pacing_gain);
    bbrplus_set_tso_segs_goal(sk);
    bbrplus_set_cwnd(sk, rs, rs->acked_sacked, bw, bbrplus->cwnd_gain);
}

static void bbrplus_init(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    bbrplus->prior_cwnd = 0;
    bbrplus->tso_segs_goal = 0;  /* default segs per skb until first ACK */
    bbrplus->rtt_cnt = 0;
    bbrplus->next_rtt_delivered = 0;
    bbrplus->prev_ca_state = TCP_CA_Open;
    bbrplus->packet_conservation = 0;

    bbrplus->probe_rtt_done_stamp = 0;
    bbrplus->probe_rtt_round_done = 0;
    bbrplus->min_rtt_us = tcp_min_rtt(tp);
    bbrplus->min_rtt_stamp = tcp_jiffies32;

    minmax_reset(&bbrplus->bw, bbrplus->rtt_cnt, 0);  /* init max bw to 0 */

    bbrplus->has_seen_rtt = 0;
    bbrplus_init_pacing_rate_from_rtt(sk);

    bbrplus->restore_cwnd = 0;
    bbrplus->round_start = 0;
    bbrplus->idle_restart = 0;
    bbrplus->full_bw = 0;
    bbrplus->full_bw_cnt = 0;
    bbrplus->cycle_mstamp = 0;
    bbrplus->cycle_idx = 0;
    bbrplus->cycle_len = 0;
    bbrplus_reset_lt_bw_sampling(sk);
    bbrplus_reset_startup_mode(sk);
    bbrplus->ack_epoch_mstamp = tp->tcp_mstamp;
    bbrplus->ack_epoch_acked = 0;
    bbrplus->extra_acked_win_rtts = 0;
    bbrplus->extra_acked_win_idx = 0;
    bbrplus->extra_acked[0] = 0;
    bbrplus->extra_acked[1] = 0;

    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static u32 bbrplus_sndbuf_expand(struct sock *sk)
{
    /* Provision 3 * cwnd since BBR may slow-start even during recovery. */
    return 3;
}

/* In theory BBR does not need to undo the cwnd since it does not
 * always reduce cwnd on losses (see bbrplus_main()). Keep it for now.
 */
static u32 bbrplus_undo_cwnd(struct sock *sk)
{
    return tcp_sk(sk)->snd_cwnd;
}

/* Entering loss recovery, so save cwnd for when we exit or undo recovery. */
static u32 bbrplus_ssthresh(struct sock *sk)
{
    bbrplus_save_cwnd(sk);
    return TCP_INFINITE_SSTHRESH;    /* BBR does not use ssthresh */
}

static size_t bbrplus_get_info(struct sock *sk, u32 ext, int *attr,
               union tcp_cc_info *info)
{
    if (ext & (1 << (INET_DIAG_BBRINFO - 1)) ||
        ext & (1 << (INET_DIAG_VEGASINFO - 1))) {
        struct tcp_sock *tp = tcp_sk(sk);
        struct bbrplus *bbrplus = inet_csk_ca(sk);
        u64 bw = bbrplus_bw(sk);

        bw = bw * tp->mss_cache * USEC_PER_SEC >> BW_SCALE;
        memset(&info->bbr, 0, sizeof(info->bbr));
        info->bbr.bbr_bw_lo     = (u32)bw;
        info->bbr.bbr_bw_hi     = (u32)(bw >> 32);
        info->bbr.bbr_min_rtt       = bbrplus->min_rtt_us;
        info->bbr.bbr_pacing_gain   = bbrplus->pacing_gain;
        info->bbr.bbr_cwnd_gain     = bbrplus->cwnd_gain;
        *attr = INET_DIAG_BBRINFO;
        return sizeof(info->bbr);
    }
    return 0;
}

static void bbrplus_set_state(struct sock *sk, u8 new_state)
{
    struct bbrplus *bbrplus = inet_csk_ca(sk);

    if (new_state == TCP_CA_Loss) {
        struct rate_sample rs = { .losses = 1 };

        bbrplus->prev_ca_state = TCP_CA_Loss;
        bbrplus->full_bw = 0;
        bbrplus->round_start = 1;   /* treat RTO like end of a round */
        bbrplus_lt_bw_sampling(sk, &rs);
    }
}

static struct tcp_congestion_ops tcp_bbrplus_cong_ops __read_mostly = {
    .flags      = TCP_CONG_NON_RESTRICTED,
    .name       = "bbrplus",
    .owner      = THIS_MODULE,
    .init       = bbrplus_init,
    .cong_control   = bbrplus_main,
    .sndbuf_expand  = bbrplus_sndbuf_expand,
    .undo_cwnd  = bbrplus_undo_cwnd,
    .cwnd_event = bbrplus_cwnd_event,
    .ssthresh   = bbrplus_ssthresh,
    .tso_segs_goal  = bbrplus_tso_segs_goal,
    .get_info   = bbrplus_get_info,
    .set_state  = bbrplus_set_state,
};

static int __init bbrplus_register(void)
{
    BUILD_BUG_ON(sizeof(struct bbrplus) > ICSK_CA_PRIV_SIZE);
    return tcp_register_congestion_control(&tcp_bbrplus_cong_ops);
}

static void __exit bbrplus_unregister(void)
{
    tcp_unregister_congestion_control(&tcp_bbrplus_cong_ops);
}

module_init(bbrplus_register);
module_exit(bbrplus_unregister);

MODULE_AUTHOR("Van Jacobson <vanj@google.com>");
MODULE_AUTHOR("Neal Cardwell <ncardwell@google.com>");
MODULE_AUTHOR("Yuchung Cheng <ycheng@google.com>");
MODULE_AUTHOR("Soheil Hassas Yeganeh <soheil@google.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("TCP BBR (Bottleneck Bandwidth and RTT)");
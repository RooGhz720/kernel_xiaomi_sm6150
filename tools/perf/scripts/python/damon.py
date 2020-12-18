# SPDX-License-Identifier: GPL-2.0
#
# Author: SeongJae Park <sj@kernel.org>

from __future__ import print_function

import argparse
import os
import subprocess
import sys
import time
import tempfile

sys.path.append(os.environ['PERF_EXEC_PATH'] + \
        '/scripts/python/Perf-Trace-Util/lib/Perf/Trace')

from perf_trace_context import *
from Core import *

# For intensive print() calls, 'IOError: [Errno 11] Resource temporarily
# unavailable' triggered.  This function handles the error.
# Note: The output should be oneline.
def pr_safe(*args):
    while True:
        try:
            print(*args)
            return
        except IOError:
            time.sleep(0.1)
            print('\r', end='')

class Region:
    start = None
    end = None
    nr_accesses = None

    def __init__(self, start, end, nr_accesses):
        self.start = start
        self.end = end
        self.nr_accesses = nr_accesses

class Snapshot:
    monitored_time = None
    target_id = None
    regions = None

    def __init__(self, monitored_time, target_id):
        self.monitored_time = monitored_time
        self.target_id = target_id
        self.regions = []

class Record:
    start_time = None
    snapshots = None

    def __init__(self, start_time):
        self.start_time = start_time
        self.snapshots = []

plot_data_file = None
plot_data_path = None
orig_stdout = None
def plot(gnuplot_cmd):
    sys.stdout = orig_stdout
    plot_data_file.flush()
    plot_data_file.close()
    subprocess.call(['gnuplot', '-e', gnuplot_cmd])
    os.remove(plot_data_path)

def trace_begin():
    pass

parser = None
def trace_end():
    if args.report_type == 'raw':
        print_record_raw(record, args.sz_bytes)
    elif args.report_type == 'wss':
        range_parsed = [int(x) for x in args.wss_range.split(',')]
        if len(range_parsed) != 3:
            pr_safe('wrong --wss-range value')
            parser.print_help()
            exit(1)
        percentile_range = range(*range_parsed)
        print_wss_dist(record, args.wss_sort, percentile_range, args.sz_bytes)

        if args.plot:
            if args.wss_sort == 'time':
                xlabel = 'runtime (percent)'
            else:    # 'size'
                xlabel = 'percentile'
            ylabel = 'working set size (bytes)'

            term = args.plot.split('.')[-1]
            gnuplot_cmd = '''
            set term %s;
            set output '%s';
            set key off;
            set xlabel '%s';
            set ylabel '%s';
            plot '%s' with linespoints;''' % (term, args.plot, xlabel, ylabel,
                    plot_data_path)
            plot(gnuplot_cmd)
    elif args.report_type == 'record-profile':
        print_record_profile(record)
    elif args.report_type == 'heatmap':
        print_heatmap(record)

args = None
record = None
nr_read_regions = 0
def damon__damon_aggregated(event_name, context, common_cpu, common_secs,
        common_nsecs, common_pid, common_comm, common_callchain, target_id,
        nr_regions, start, end, nr_accesses, perf_sample_dict):
    global record
    global nr_read_regions

    time = common_secs * 1000000000 + common_nsecs
    if not record:
        record = Record(time)

    if nr_read_regions == 0:
        snapshot = Snapshot(time, target_id)
        record.snapshots.append(snapshot)

    snapshot = record.snapshots[-1]
    snapshot.regions.append(Region(start, end, nr_accesses))

    nr_read_regions += 1
    if nr_read_regions == nr_regions:
        nr_read_regions = 0

def sz_region(region):
    return region[1] - region[0]

def bigger_region(prev_big, region):
    if not prev_big or sz_region(prev_big) < sz_region(region):
        return region
    return prev_big

def default_heatmap_attrs(record):
    profiles = get_record_profile(record)

    biggest_regions = {}
    biggest = None
    for prof in profiles.values():
        region = [prof.addr_start]
        for idx, gap in enumerate(prof.gaps):
            region.append(gap[0])
            biggest = bigger_region(biggest, region)
            region = [gap[1]]
        region.append(prof.addr_end)
        biggest = bigger_region(biggest, region)

        biggest_regions[prof.target_id] = biggest

    tid = sorted(biggest_regions.keys(),
            key=lambda x: sz_region(biggest_regions[x]))[-1]
    prof = profiles[tid]
    time_range = [prof.time_start, prof.time_end]
    addr_range = biggest_regions[tid]
    return tid, time_range, addr_range

class HeatPixel:
    time = None
    addr = None
    heat = None

    def __init__(self, time, addr, heat):
        self.time = time
        self.addr = addr
        self.heat = heat

def add_heats(snapshot, duration, pixels, time_unit, space_unit, addr_range):
    """Add heats in a monitoring 'snapshot' of specific time 'duration' to
    the corresponding heats 'pixels'.
    """
    pixel_sz = time_unit * space_unit

    for region in snapshot.regions:
        start = max(region.start, addr_range[0])
        end = min(region.end, addr_range[1])
        if start >= end:
            continue

        fraction_start = start
        addr_idx = int((fraction_start - addr_range[0]) / space_unit)
        while fraction_start < end:
            fraction_end = min((addr_idx + 1) * space_unit + addr_range[0],
                    end)
            heat = region.nr_accesses * duration * (
                    fraction_end - fraction_start)

            pixel = pixels[addr_idx]
            heat += pixel.heat * pixel_sz
            pixel.heat = heat / pixel_sz

            fraction_start = fraction_end
            addr_idx += 1

def heat_pixels_from_snapshots(snapshots, time_range, addr_range, resols):
    """Get heat pixels for monitoring snapshots."""
    time_unit = (time_range[1] - time_range[0]) / float(resols[0])
    space_unit = (addr_range[1] - addr_range[0]) / float(resols[1])

    pixels = [[HeatPixel(int(time_range[0] + i * time_unit),
                    int(addr_range[0] + j * space_unit), 0.0)
            for j in range(resols[1])] for i in range(resols[0])]

    if len(snapshots) < 2:
        return pixels

    for idx, shot in enumerate(snapshots[1:]):
        start = snapshots[idx].monitored_time
        end = min(shot.monitored_time, time_range[1])

        fraction_start = start
        time_idx = int((fraction_start - time_range[0]) / time_unit)
        while fraction_start < end:
            fraction_end = min((time_idx + 1) * time_unit + time_range[0], end)
            add_heats(shot, fraction_end - fraction_start, pixels[time_idx],
                    time_unit, space_unit, addr_range)
            fraction_start = fraction_end
            time_idx += 1
    return pixels

def heatmap_plot_ascii(pixels, time_range, addr_range, resols):
    highest_heat = None
    lowest_heat = None
    for snapshot in pixels:
        for pixel in snapshot:
            if not highest_heat or highest_heat < pixel.heat:
                highest_heat = pixel.heat
            if not lowest_heat or lowest_heat > pixel.heat:
                lowest_heat = pixel.heat
    if not highest_heat or not lowest_heat:
        return
    heat_unit = (highest_heat + 1 - lowest_heat) / 9

    colorsets = {
        'gray':[
            [232] * 10,
            [237, 239, 241, 243, 245, 247, 249, 251, 253, 255]],
        'flame':[
            [232, 1, 1, 2, 3, 3, 20, 21,26, 27, 27],
            [239, 235, 237, 239, 243, 245, 247, 249, 251, 255]],
        'emotion':[
            [232, 234, 20, 21, 26, 2, 3, 1, 1, 1],
            [239, 235, 237, 239, 243, 245, 247, 249, 251, 255]],
        }
    colors = colorsets[args.heatmap_ascii_color]
    for snapshot in pixels:
        chars = []
        for pixel in snapshot:
            heat = int((pixel.heat - lowest_heat) / heat_unit)
            heat = min(heat, len(colors[0]) - 1)
            bg = colors[0][heat]
            fg = colors[1][heat]
            chars.append(u'\u001b[48;5;%dm\u001b[38;5;%dm%d' %
                    (bg, fg, heat))
        pr_safe(''.join(chars) + u'\u001b[0m')
    color_samples = [u'\u001b[48;5;%dm\u001b[38;5;%dm %d ' %
            (colors[0][i], colors[1][i], i) for i in range(10)]
    pr_safe('# temparature:', ''.join(color_samples) + u'\u001b[0m')
    pr_safe('# x-axis: space (%d-%d: %s)' % (addr_range[0], addr_range[1],
        format_sz(addr_range[1] - addr_range[0], False)))
    pr_safe('# y-axis: time (%d-%d: %fs)' % (time_range[0], time_range[1],
        (time_range[1] - time_range[0]) / 1000000000))
    pr_safe('# resolution: %dx%d' % (len(pixels[1]), len(pixels)))

def print_heatmap(record):
    tid = args.heatmap_target
    time_range = args.heatmap_time_range
    addr_range = args.heatmap_space_range
    resols = args.heatmap_res

    if not tid or not time_range or not addr_range:
        dtid, dtime_range, daddr_range = default_heatmap_attrs(record)
    if not tid:
        tid = dtid
    if not time_range:
        time_range = dtime_range
    if not addr_range:
        addr_range = daddr_range

    snapshots = [s for s in record.snapshots if s.target_id == tid]
    pixels = heat_pixels_from_snapshots(snapshots, time_range, addr_range,
            resols)

    if args.heatmap_plot_ascii:
        heatmap_plot_ascii(pixels, time_range, addr_range, resols)
        return

    for snapshot in pixels:
        for pixel in snapshot:
            addr = pixel.addr
            if not args.heatmap_abs_addr:
                addr -= addr_range[0]
            time = pixel.time
            if not args.heatmap_abs_time:
                time -= time_range[0]
            pr_safe('%s\t%s\t%s' % (time, addr, pixel.heat))

    if args.plot:
        term = args.plot.split('.')[-1]
        plot_xrange = [x for x in time_range]
        if not args.heatmap_abs_time:
            plot_xrange = [x - time_range[0] for x in plot_xrange]
        plot_yrange = [y for y in addr_range]
        if not args.heatmap_abs_addr:
            plot_yrange = [y - addr_range[0] for y in plot_yrange]

        gnuplot_cmd = '''
        set term %s;
        set output '%s';
        set key off;
        set xrange [%f:%f];
        set yrange [%f:%f];
        set xlabel 'Time (ns)';
        set ylabel 'Address (bytes)';
        plot '%s' using 1:2:3 with image;''' % (term, args.plot,
                plot_xrange[0], plot_xrange[1],
                plot_yrange[0], plot_yrange[1], plot_data_path)
        plot(gnuplot_cmd)

class RecordProfile:
    target_id = None
    time_start = None
    time_end = None
    addr_start = None
    addr_end = None
    gaps = None
    nr_snapshots = None

    def __init__(self, target_id, time_start):
        self.target_id = target_id
        self.time_start = time_start
        self.gaps = []
        self.nr_snapshots = 0

    def __str__(self):
        lines = ['id: %s' % self.target_id]
        lines.append('time: %s-%s (%s)' % (self.time_start, self.time_end,
            self.time_end - self.time_start))
        lines.append('nr_snapshots: %d' % self.nr_snapshots)
        lines.append('addr_space: %s-%s (%s)' % (self.addr_start,
            self.addr_end, self.addr_end - self.addr_start))
        for idx, gap in enumerate(self.gaps):
            lines.append('space_gap%d: %s-%s' % (idx, gap[0], gap[1]))
        return '\n'.join(lines)

    def __repr__(self):
        return self.__str__()

def is_overlap(region1, region2):
    if region1[1] < region2[0]:
        return False
    if region2[1] < region1[0]:
        return False
    return True

def overlapping_region_of(region1, region2):
    return [max(region1[0], region2[0]), min(region1[1], region2[1])]

def overlapping_regions(regions1, regions2):
    overlap_regions = []
    for r1 in regions1:
        for r2 in regions2:
            if is_overlap(r1, r2):
                r1 = overlapping_region_of(r1, r2)
        overlap_regions.append(r1)
    return overlap_regions

def get_record_profile(record):
    profiles = {}
    for snapshot in record.snapshots:
        tid = snapshot.target_id
        monitored_time = snapshot.monitored_time
        if not tid in profiles:
            profiles[tid] = RecordProfile(tid, monitored_time)
        prof = profiles[tid]
        prof.nr_snapshots += 1
        prof.time_end = monitored_time

        last_addr = None
        gaps = []
        for region in snapshot.regions:
            if not prof.addr_start:
                prof.addr_start = region.start
            prof.addr_start = min(prof.addr_start, region.start)
            prof.addr_end = max(prof.addr_end, region.end)

            if last_addr and last_addr != region.start:
                gaps.append([last_addr, region.start])
            last_addr = region.end
        if not prof.gaps:
            prof.gaps = gaps
        else:
            prof.gaps = overlapping_regions(prof.gaps, gaps)
    return profiles

def print_record_profile(record):
    for prof in get_record_profile(record).values():
        if prof.nr_snapshots <= 1:
            continue
        prof.time_end += (prof.time_end - prof.time_start) / (
                prof.nr_snapshots - 1)
        pr_safe(prof)

def format_sz(number, sz_bytes):
    if sz_bytes:
        return '%d' % number

    if number > 1<<40:
        return '%.3f TiB' % (number / (1<<40))
    if number > 1<<30:
        return '%.3f GiB' % (number / (1<<30))
    if number > 1<<20:
        return '%.3f MiB' % (number / (1<<20))
    if number > 1<<10:
        return '%.3f KiB' % (number / (1<<10))
    return '%d B' % number

def print_record_raw(record, sz_bytes):
    pr_safe('start_time:', record.start_time)
    for snapshot in record.snapshots:
        pr_safe('relative_time:',
                snapshot.monitored_time - record.start_time)
        pr_safe('target_id:', snapshot.target_id)
        pr_safe('nr_regions:', len(snapshot.regions))
        for region in snapshot.regions:
            pr_safe('%x-%x (%s): %d' % (region.start, region.end,
                format_sz(region.end - region.start, sz_bytes),
                region.nr_accesses))
        pr_safe()

def print_wss_dist(record, sort_key, percentile_range, sz_bytes):
    wsss = []

    for snapshot in record.snapshots:
        wss = 0
        for region in snapshot.regions:
            if region.nr_accesses > 0:
                wss += region.end - region.start
        wsss.append(wss)

    if sort_key == 'size':
        wsss.sort()

    for i in percentile_range:
        idx = int(len(wsss) * i / 100)
        if idx >= len(wsss):
            idx = -1
        pr_safe('%d %s' % (i, format_sz(wsss[idx], sz_bytes)))

def main():
    global args
    global parser
    global plot_data_path
    global plot_data_file
    global orig_stdout

    parser = argparse.ArgumentParser()
    parser.add_argument('report_type',
            choices=['raw', 'wss', 'record-profile', 'heatmap'],
            help='report type')
    parser.add_argument('--sz-bytes', action='store_true',
            help='report size in bytes')
    parser.add_argument('--plot', metavar='<output file>',
            help='visualize the wss distribution')

    parser.add_argument('--wss-sort', choices=['size', 'time'], default='size',
            help='sort working set sizes by')
    parser.add_argument('--wss-range', metavar='<begin,end,interval>',
            default='0,101,5',
            help='percentile range (begin,end,interval)')

    parser.add_argument('--heatmap-target', metavar='<target id>',
            help='id of monitoring target for heatmap')
    parser.add_argument('--heatmap-plot-ascii', action='store_true',
            help='visualize in ascii art')
    parser.add_argument('--heatmap-ascii-color',
            choices=['gray', 'flame', 'emotion'], default = 'gray',
            help='color theme for temperatures')
    parser.add_argument('--heatmap-res', metavar='<resolution>', type=int,
            nargs=2, default=[800, 600],
            help='resolutions for time and space axises')
    parser.add_argument('--heatmap-time-range', metavar='<time>', type=int,
            nargs=2,
            help='start and end time of the heatmap')
    parser.add_argument('--heatmap-space-range', metavar='<address>', type=int,
            nargs=2,
            help='start and end address of the heatmap')
    parser.add_argument('--heatmap-abs-addr', action='store_true',
            help='display in absolute addresses')
    parser.add_argument('--heatmap-abs-time', action='store_true',
            help='display in absolute time')

    args = parser.parse_args()

    if args.report_type in ['wss', 'heatmap'] and args.plot:
        file_type = args.plot.split('.')[-1]
        supported = ['pdf', 'jpeg', 'png', 'svg']
        if not file_type in supported:
            pr_safe('not supported plot file type.  Use one in',
                    supported)
            exit(-1)

        args.sz_bytes = True
        plot_data_path = tempfile.mkstemp()[1]
        plot_data_file = open(plot_data_path, 'w')
        orig_stdout = sys.stdout
        sys.stdout = plot_data_file

    if (args.report_type == 'heatmap' and args.heatmap_plot_ascii and
            args.heatmap_res[1] > 300):
        args.heatmap_res = [40, 80]

if __name__ == '__main__':
    main()

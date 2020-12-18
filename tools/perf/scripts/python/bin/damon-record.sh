#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# This is more convenient version of 'perf script record damon <command>'.
# While the command assumes DAMON will be turned on by the user, this do that
# instead.  That is, this command starts the command, turn DAMON on for the
# process, and record the trace events.

pr_usage()
{
	>&2 echo "Usage: $0 [OPTION]... <command>"
	>&2 echo
	>&2 echo "OPTION"
	>&2 echo "  --sampling <interval>	Sampling interval (us)"
	>&2 echo "  --aggregate <interval>	Aggregate interval (us)"
	>&2 echo "  --update <interval>	Regions update interval (us)"
	>&2 echo "  --min-reg <nr>		Minimum number of regions"
	>&2 echo "  --max-reg <nr>		Maximum number of regions"
}

# Default values (intervals are in us)
sampling_interval=5000
aggregate_interval=100000
regions_update_interval=1000000
min_nr_regions=10
max_nr_regions=1000
cmd=""

debugfs_dir=$(mount | grep -e "type debugfs" | awk '{print $3}')
if [ -z "$debugfs_dir" ]
then
	>&2 echo "debugfs not found"
	exit 1
fi

damon_dir="$debugfs_dir/damon"
if [ ! -d "$damon_dir" ]
then
	>&2 echo "damon dir not found"
	exit 1
fi

if [ $# -lt 1 ]
then
	pr_usage
	exit 1
fi

while [ $# -ne 0 ]
do
	case $1 in
	"--sampling")
		if [ $# -lt 2 ]
		then
			>&2 echo "<interval> not given"
			pr_usage
			exit 1
		fi
		sampling_interval=$2
		shift 2
		continue
		;;
	"--aggregate")
		if [ $# -lt 2 ]
		then
			>&2 echo "<interval> not given"
			pr_usage
			exit 1
		fi
		aggregate_interval=$2
		shift 2
		continue
		;;
	"--update")
		if [ $# -lt 2 ]
		then
			>&2 echo "<interval> not given"
			pr_usage
			exit 1
		fi
		regions_update_interval=$2
		shift 2
		continue
		;;
	"--min_reg")
		if [ $# -lt 2 ]
		then
			>&2 echo "<nr> not given"
			pr_usage
			exit 1
		fi
		min_nr_regions=$2
		shift 2
		continue
		;;
	"--max_reg")
		if [ $# -lt 2 ]
		then
			>&2 echo "<nr> not given"
			pr_usage
			exit 1
		fi
		max_nr_regions=$2
		shift 2
		continue
		;;
	*)
		if [ $# -lt 1 ]
		then
			>&2 echo "<command> not given"
			pr_usage
			exit 1
		fi
		cmd="$*"
		break
		;;
	esac
done

if [ -z "$cmd" ]
then
	pr_usage
	exit 1
fi

orig_attrs=$(cat "$damon_dir/attrs")
attrs="$sampling_interval $aggregate_interval $regions_update_interval"
attrs="$attrs $min_nr_regions $max_nr_regions"

echo "$attrs" > "$damon_dir/attrs"

$cmd &
cmd_pid=$!

echo "$cmd_pid" > "$damon_dir/target_ids"
echo "on" > "$damon_dir/monitor_on"

perf record -e damon:damon_aggregated &
perf_pid=$!

sigint_trap()
{
	kill 2 "$cmd_pid"
	kill 2 "$perf_pid"
	echo "$orig_attrs" > "$damon_dir/attrs"
	exit
}

trap sigint_trap INT

>&2 echo "Press Control+C to stop recording"

while :;
do
	on_off=$(cat "$damon_dir/monitor_on")
	if [ "$on_off" = "off" ]
	then
		kill 2 $perf_pid
		echo "$orig_attrs" > "$damon_dir/attrs"
		break
	fi
	sleep 1
done

#!/bin/bash
# leak.sh [rounds]: RSS, threads and handles of Calculator after each Standard <-> Graphing round.
. "$(dirname "$0")/common.sh"
rounds=${1:-10}
perf_launch $PERF_OUT/leak.log; sleep 25
pid=$(perf_app_pid)
rss() { awk '/VmRSS/{print int($2/1024)}' /proc/$pid/status; }
echo "start rss $(rss) MB"
for r in $(seq 1 $rounds); do
    perf_nav graphing; sleep 4
    xdotool mousemove 960 136 click 1; sleep 1; xdotool type "x^2"; xdotool key Return; sleep 2
    xdotool mousemove 400 400; for i in 1 2 3 4 5; do xdotool click 4; sleep 0.1; done; sleep 1
    perf_nav standard; sleep 3
    xdotool mousemove 300 700 click 1; xdotool type "123+456="; sleep 1
    echo "round $r rss $(rss) MB threads $(ls /proc/$pid/task | wc -l) fds $(ls /proc/$pid/fd | wc -l)"
done
perf_stop

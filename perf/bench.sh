#!/bin/bash
# bench.sh tag: CPU per phase (idle, typing, hover, graph idle/drag/wheel, resize) of Calculator's session, with
# the presenter's per-second summaries (WINEDEBUG=+dcompperf). Results in $PERF_OUT/<tag>.bench.
. "$(dirname "$0")/common.sh"
tag=${1:-bench}; L=$PERF_OUT/$tag.log; R=$PERF_OUT/$tag.bench; : > $R
HZ=$(getconf CLK_TCK)
APPSID=""
cpu() {
    [ -z "$APPSID" ] && APPSID=$(awk '{print $6}' /proc/$(perf_app_pid)/stat 2>/dev/null)
    local t=0 p stat
    for p in $(pgrep -s "$APPSID" 2>/dev/null) $(pgrep -f "$WINE_BUILD/server/wineserver"); do
        stat=$(sed 's/^.*) //' /proc/$p/stat 2>/dev/null) || continue
        set -- $stat; [ -n "${13}" ] && t=$((t + ${12} + ${13}))
    done
    echo $t
}
phase_start() { P=$1; C0=$(cpu); T0=$(date +%s.%N); N0=$(wc -l < $L); }
phase_end() {
    local c1=$(cpu) t1=$(date +%s.%N) n1=$(wc -l < $L)
    local wall=$(echo "$t1 - $T0" | bc) cs=$(echo "scale=2; ($c1 - $C0) / $HZ" | bc)
    local frames=$(sed -n "$((N0+1)),${n1}p" $L | grep -o "unchanged, [0-9]* frames" | awk '{f+=$2} END{print f+0}')
    printf "%-8s wall %5.1fs cpu %5.2fs (%3.0f%% of a core) frames %d\n" $P $wall $cs $(echo "100*$cs/$wall" | bc -l) $frames | tee -a $R
}
RS() { WINEPREFIX=$UWP_PREFIX WINEDEBUG=-all $WINE_BUILD/wine "$(dirname "$0")/winresize.exe" Calculator $1 $2 >/dev/null; }
[ -x "$(dirname "$0")/winresize.exe" ] || x86_64-w64-mingw32-gcc -o "$(dirname "$0")/winresize.exe" "$(dirname "$0")/winresize.c"
WINEDEBUG="fixme-all,err+all,+dcompperf" perf_launch $L
sleep 30
phase_start idle; sleep 20; phase_end
phase_start typing; xdotool mousemove 300 700 click 1; for i in $(seq 1 30); do xdotool type --delay 0 "$((i % 10))"; sleep 0.15; done; xdotool key Escape; sleep 2; phase_end
phase_start hover; for r in 1 2 3; do for x in $(seq 20 20 940); do xdotool mousemove $x $((400 + (x % 300))); sleep 0.02; done; done; sleep 2; phase_end
perf_nav graphing; sleep 10
xdotool mousemove 960 136 click 1; sleep 2; xdotool type "x^2"; xdotool key Return; sleep 5
phase_start g-idle; sleep 10; phase_end
args=""; for i in $(seq 1 300); do args="$args mousemove_relative -- $(( (i/30)%2 ? -6 : 6 )) 1 sleep 0.016"; done
phase_start g-drag; xdotool mousemove 400 400 mousedown 1; xdotool $args; xdotool mouseup 1; sleep 1; phase_end
phase_start g-wheel; xdotool mousemove 400 400; for i in $(seq 1 20); do xdotool click $(( i <= 10 ? 4 : 5 )); sleep 0.1; done; sleep 2; phase_end
phase_start resize; for s in "1000 700" "1280 800" "900 650" "1280 800"; do RS $s; sleep 3; done; phase_end
echo "crashes=$(grep -c 'Unhandled page' $L)" | tee -a $R
perf_stop

#!/bin/bash
# profile.sh tag: sample Calculator while typing, switching to Graphing and dragging the graph (wineuwp's
# WINE_UWP_PROFILE sampler), one symbolized report per interaction.
. "$(dirname "$0")/common.sh"
tag=${1:-profile}; P=$PERF_OUT/$tag; rm -f $P.*
WINE_UWP_PROFILE=$P WINE_UWP_PROFILE_MS=${PROFILE_MS:-2} perf_launch $P.log
sleep 25
reset() { touch $P.reset; sleep 0.5; }
dump() { touch $P.dump; sleep 1; cp $(ls $P.[0-9]* | head -1) $P-$1.prof; }
reset; xdotool mousemove 300 700 click 1; for i in $(seq 1 30); do xdotool type --delay 0 "$((i % 10))"; sleep 0.15; done; xdotool key Escape; sleep 1; dump typing
reset; perf_nav graphing; sleep 4; dump switch
xdotool mousemove 960 136 click 1; sleep 1; xdotool type "x^2"; xdotool key Return; sleep 3
args=""; for i in $(seq 1 300); do args="$args mousemove_relative -- $(( (i/30)%2 ? -6 : 6 )) 1 sleep 0.016"; done
reset; xdotool mousemove 400 400 mousedown 1; xdotool $args; xdotool mouseup 1; sleep 1; dump drag
perf_stop
for p in typing switch drag; do echo "===== $p"; python3 "$(dirname "$0")/profsym.py" $P-$p.prof ${TOP:-15}; done

#!/bin/bash
# startup.sh: seconds from launch until the keypad shows, and until the window stops changing.
. "$(dirname "$0")/common.sh"
T0=$(date +%s.%N); perf_launch $PERF_OUT/startup.log
first=""; prev=""; stable=0
for i in $(seq 1 600); do
    sleep 0.1
    px=$(import -display $DISPLAY -window root -crop 1x1+400+500 -depth 8 txt:- 2>/dev/null | tail -1 | grep -o "#[0-9A-F]\{6\}")
    [ -z "$first" ] && [ -n "$px" ] && [ "$px" != "#000000" ] && [ "$px" != "#2060C0" ] && first=$(echo "$(date +%s.%N) - $T0 - 4" | bc)
    h=$(import -display $DISPLAY -window root -crop 1280x800+4+30 -resize 64x40 -depth 8 rgb:- 2>/dev/null | md5sum)
    if [ -n "$first" ] && [ "$h" = "$prev" ]; then stable=$((stable+1)); else stable=0; fi
    prev=$h; [ $stable -ge 10 ] && break
done
echo "first content ${first}s, settled $(echo "$(date +%s.%N) - $T0 - 5" | bc)s (the 4 s prefix shutdown excluded)"
perf_stop

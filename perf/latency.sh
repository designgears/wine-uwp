#!/bin/bash
# latency.sh: input-to-screen latency of typing, hover and switching to Graphing (latency.py polls the screen).
. "$(dirname "$0")/common.sh"
perf_launch $PERF_OUT/latency.log; sleep 25
for mode in keys hover switch; do python3 "$(dirname "$0")/latency.py" $mode; done
perf_stop

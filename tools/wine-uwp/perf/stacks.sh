#!/bin/bash
# stacks.sh [profile-prefix]: sample where a running (or frozen) app is, so a hang can be read after
# the fact.
#
# Preferred path: launch the app with WINE_UWP_PROFILE=<prefix> (wineuwp's in-process sampler, which
# suspends each thread and walks its stack from inside the process, so no ptrace rights are needed);
# this script then asks it for a dump and symbolizes it. Falls back to eu-stack/gdb, which only work
# when ptrace is allowed for non-descendants (kernel.yama.ptrace_scope = 0).
prefix=${1:-${WINE_UWP_PROFILE:-/tmp/wine-uwp-perf/desktop}}
pid=$(pgrep -f CalculatorApp.exe | tail -1)
[ -z "$pid" ] && { echo "no CalculatorApp.exe running"; exit 1; }

if [ -d "$(dirname "$prefix")" ] && compgen -G "$prefix.[0-9]*" >/dev/null || [ -e "$prefix.log" ]; then
    touch "$prefix.dump"; sleep 2
    dump=$(ls -t "$prefix".[0-9]* 2>/dev/null | head -1)
    if [ -n "$dump" ]; then
        echo "profile dump $dump"
        python3 "$(dirname "$0")/profsym.py" "$dump" "${TOP:-20}"
        exit 0
    fi
fi

echo "no profile dump (start the app with WINE_UWP_PROFILE=<prefix> for hangs); trying a debugger"
if command -v eu-stack >/dev/null; then eu-stack -p "$pid" 2>&1
else gdb -p "$pid" -batch -ex "thread apply all bt" 2>&1; fi

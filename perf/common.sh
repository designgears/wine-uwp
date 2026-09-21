# Shared settings for the performance scripts. Source it; override any of these in the environment.
. "$(dirname "${BASH_SOURCE[0]:-$0}")/../env.sh"
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# WINE_BUILD comes from env.sh (sourced above), which reconciles it with WINE_BIN_DIR.
UWP_PREFIX=${UWP_PREFIX:-${WINEPREFIX}}
PERF_OUT=${PERF_OUT:-/tmp/wine-uwp-perf}
export DISPLAY=${PERF_DISPLAY:-:98}
unset WAYLAND_DISPLAY
mkdir -p "$PERF_OUT"

# kill the prefix and start Calculator; $1 log file, rest of the environment as given
perf_launch() {
    local log=$1
    WINEPREFIX=$UWP_PREFIX $WINE_BUILD/server/wineserver -k 2>/dev/null; sleep 4
    rm -f "$log"
    ( cd "$REPO" && WINEDEBUG="${WINEDEBUG:--all}" WINE_D3D_CONFIG=renderer=vulkan WINE_UWP_WINDOW_SIZE=1280x800 \
      WINE_UWP_ACTIVATE_ON_VIEW=1 UWP_TIMEOUT=${UWP_TIMEOUT:-600} UWP_LOG="$log" \
      setsid nohup timeout $(( ${UWP_TIMEOUT:-600} + 20 )) "$(dirname "${BASH_SOURCE[0]}")/../run.sh" >/dev/null 2>&1 & )
}
perf_stop() { WINEPREFIX=$UWP_PREFIX $WINE_BUILD/server/wineserver -k 2>/dev/null; }
# the Calculator process (the one with the most threads)
perf_app_pid() { for q in $(pgrep -f -i 'calculatorapp'); do echo "$(ls /proc/$q/task 2>/dev/null | wc -l) $q"; done | sort -rn | head -1 | cut -d' ' -f2; }
# switch pages through the navigation menu (1280x800 window at the top left of the display)
perf_nav() { case $1 in standard) y=166;; scientific) y=206;; graphing) y=246;; programmer) y=286;; date) y=326;; esac
             xdotool mousemove 26 84 click 1; sleep 3; xdotool mousemove 76 $y click 1; }

#!/usr/bin/env bash
# Run a staged UWP app (or a probe) in the wine-uwp prefix, for testing.
# Usage: ./run.sh [exe] [args...]     default exe: the stage's UWP_EXE (identity.env)
#
# The launch itself is Wine's wineuwp-launch.exe, the same program that starts an installed app from a desktop entry:
# it reads the stage's identity.env (and launch.env, or launch-defaults.env plus the frameworks beside the stage),
# creates the AppContainer storage key, starts CoreMessagingRegistrar and runs the app from the stage directory.
# What stays here is the host side: which Wine and prefix, a clean wineserver, the prefix's overrides, the display
# driver, a timeout and the log.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]:-$0}")/./env.sh"
export WINEPREFIX=${WINEPREFIX}
# Tests run on a headless Xvfb (DISPLAY=:98), but a desktop session also exports WAYLAND_DISPLAY and Wine prefers
# winewayland.drv when it is set. Unsetting it is not enough (libwayland falls back to $XDG_RUNTIME_DIR/wayland-0),
# so wineuwp-launch also pins the graphics driver to x11 (--graphics below).
unset WAYLAND_DISPLAY
# Wine's normal channels by default: +loaddll,+combase,+winrt costs about 6 s of a 10 s startup.
export WINEDEBUG=${WINEDEBUG:-fixme+all,err+all}
if [ -x "$WINE_BUILD/wine" ]; then export PATH="$WINE_BUILD:$PATH"; fi
# A run killed by a timeout leaves this prefix's explorer.exe /desktop and the svchost.exe hosting
# CoreMessagingRegistrar behind, and they wreck the next activation race. wineserver -k is scoped to WINEPREFIX;
# a pkill on the image name would also kill other prefixes on this machine.
wineserver -k 2>/dev/null

"$(dirname "$0")/install-overrides.sh" >/dev/null
# UWP_STAGE names a directory under ${UWP_BIN}/stage (see stage-app.sh).
S=${UWP_BIN}/stage/${UWP_STAGE:-calculator}
# Where an app that installs others (the Xbox app, through winegameplatform and appxinstall.dll) puts them, and the
# Windows files the frameworks come from.
export WINE_UWP_GAMES_DATA=${WINE_UWP_GAMES_DATA:-${UWP_BIN}/xbox-games}
export WINE_UWP_WINDOWS_TREE=${WINE_UWP_WINDOWS_TREE:-${WIN11:-${UWP_BIN}/win11}}

launch=(wineuwp-launch --stage "$S" --defaults "$WINE_UWP_ROOT/launch-defaults.env" --graphics x11)
exe=${1:-${UWP_EXE:-}}; shift || true
[ -n "$exe" ] && launch+=(--exe "$exe")
# Full unfiltered log always goes to $UWP_LOG for post-mortem (module map, SEH context).
UWP_LOG=${UWP_LOG:-/tmp/wine-uwp-last.log}
# UWP_VDESKTOP=WxH runs the app inside a Wine virtual desktop of that size.
if [ -n "${UWP_VDESKTOP:-}" ]; then launch=(explorer "/desktop=uwp,$UWP_VDESKTOP" "${launch[@]}"); fi
(cd "$S" && stdbuf -oL -eL timeout "${UWP_TIMEOUT:-60}" wine "${launch[@]}" "$@") > "$UWP_LOG" 2>&1
status=$?
# The timeout ends the launcher; the app is its child, so end the prefix with it as a timeout always did.
[ $status = 124 ] && wineserver -k 2>/dev/null
grep -vE "MESA|pci id|winemac|Loaded .*system32" "$UWP_LOG"
exit $status

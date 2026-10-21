#!/usr/bin/env bash
# Put the Windows pieces UWP apps need into the prefix: Wine's wineuwp-setup does it from inside the prefix (the
# DLLs, fonts, MUI files and metadata from the Windows tree, the registrations, the classes Wine serves instead of
# Windows, the WebView2 runtime). This keeps the host's half: which Wine and prefix, the EAC runtime, and waiting for
# the prefix to settle before the classes a prefix update may rewrite are checked.
#   install-overrides.sh [LIST]    LIST: the Windows DLLs to install as native overrides (wineuwp-setup has the default)
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]:-$0}")/./env.sh"
export WINEPREFIX=${WINEPREFIX}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)
# Runtime downloads are explicit (packaging/install-eac-runtime.sh --download).
# Once available, attach it to new prefixes even when nothing else changed.
if [ -n "${PROTON_EAC_RUNTIME:-}" ]; then
  "$HERE/packaging/install-eac-runtime.sh" >/dev/null
fi
WINE_BIN=$WINE_BIN_DIR   # from env.sh; always ends in a slash
# The Windows file tree: uwp-bin/win11 (copied from an installation) or one harvested from an ISO.
WIN11=${WIN11:-${UWP_BIN}/win11}
WV2_ROOT=${WEBVIEW2_RUNTIME:-${UWP_BIN}/webview2/rt/Program Files (x86)/Microsoft/EdgeWebView}
SETUP=(--windows "$WIN11" --build "${WINE_BIN%/}" --data "$HERE")
[ -d "$WV2_ROOT" ] && SETUP+=(--webview2 "$WV2_ROOT")
[ -n "${WINE_UWP_FORCE_OVERRIDES:-}" ] && SETUP+=(--force)
# a prefix older than the program finds it only after an update
[ -f "$WINEPREFIX/drive_c/windows/system32/wineuwp-setup.exe" ] || "${WINE_BIN}wine" wineboot -u >/dev/null 2>&1
rc=0
# Wine's own start-up chatter (Mesa probing the display) is not ours to show
quiet() { grep -v '^MESA\|^pci id' >&2 || true; }
WINEDEBUG=${WINEDEBUG:--all} "${WINE_BIN}wine" wineuwp-setup "${SETUP[@]}" ${1:+"$1"} 2> >(quiet) || rc=$?
[ $rc = 3 ] || exit $rc
# A prefix update that lands after those writes re-registers Wine's own classes and can take back a class Wine serves
# from another DLL. Wait for it, then check; forcing one with wineboot -u here would leave a services.exe/explorer.exe
# set behind on every launch, which breaks the activation race.
for attempt in 1 2 3 4; do
  "${WINE_BIN}wineserver" -w 2>/dev/null || true
  WINEDEBUG=${WINEDEBUG:--all} "${WINE_BIN}wine" wineuwp-setup --verify "${SETUP[@]}" 2> >(quiet) && exit 0
done
echo "WARNING: shim registration did not stick" >&2

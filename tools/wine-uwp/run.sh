#!/usr/bin/env bash
# Run a staged UWP app (or a probe) in the wine-uwp prefix with useful tracing.
# Usage: tools/wine-uwp/run.sh [exe] [args...]     default exe: CalculatorApp.exe in the stage dir
set -uo pipefail
export WINEPREFIX=$HOME/.wine-uwp
# Wine's normal channels by default: the +loaddll,+combase,+winrt tracing that debugging needs costs
# about 6 s of the 10 s startup (4 s without it). Set WINEDEBUG to get it back.
export WINEDEBUG=${WINEDEBUG:-fixme+all,err+all}
# wined3d's OpenGL path stalls XAML's first frame on the NVIDIA desktop (Mesa EGL cannot open the DRI
# device, the CS thread falls back to a "compatible pixel format"); the Vulkan renderer works on both
# the desktop and the Xvfb test display.
# Skip DLL_THREAD_ATTACH/DETACH into native DLLs: see Findings-M4.md, loader-lock deadlocks.
export WINE_SKIP_NATIVE_THREAD_CALLS=${WINE_SKIP_NATIVE_THREAD_CALLS:-1}
export WINE_D3D_CONFIG=${WINE_D3D_CONFIG:-renderer=vulkan}
# Patched Wine build (see docs/LinuxPort/Findings-M1.md), falls back to system wine.
WINE_BUILD=${WINE_BUILD:-$HOME/src/wine-build}
if [ -x "$WINE_BUILD/wine" ]; then export PATH="$WINE_BUILD:$PATH"; fi
# DLLs where Wine's builtin must yield to the extracted Windows copy. Wine also ships these names.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-windows.ui.xaml=n;windows.ui=n;twinapi.appcore=n;coremessaging=n;windows.applicationmodel=n;wintypes=n;threadpoolwinrt=n;msvcp_win=n;bcp47mrm=n;bcp47langs=n;profapi=n;rometadata=n;windows.globalization=n;msftedit=n;msxml6=n}"
# A run killed by a timeout leaves this prefix's explorer.exe /desktop and the svchost.exe hosting
# CoreMessagingRegistrar behind; they accumulate across runs and quietly wreck every later
# measurement, because the activation is a race and a loaded machine loses it. Clear them with
# wineserver -k, which is scoped to WINEPREFIX - a pkill on the image name would also kill other
# prefixes running on this machine.
wineserver -k 2>/dev/null

"$(dirname "$0")/install-overrides.sh" >/dev/null
# CoreMessaging.dll blocks in WaitServiceState until its registrar service reports SERVICE_RUNNING,
# so make sure the service is up before the app asks for a CoreDispatcher.
if ! wine sc query CoreMessagingRegistrar 2>/dev/null | grep -q RUNNING; then
  wine net start CoreMessagingRegistrar >/dev/null 2>&1
  for _ in $(seq 1 15); do
    wine sc query CoreMessagingRegistrar 2>/dev/null | grep -q RUNNING && break
    sleep 1
  done
fi
S=$HOME/uwp-bin/stage/calculator
cd "$S"
# Package identity consumed by the patched Wine (ntdll/kernelbase read these).
export WINE_UWP_PACKAGE_FULL_NAME=${WINE_UWP_PACKAGE_FULL_NAME:-Microsoft.WindowsCalculator.Dev_0.365.1.0_x64__8wekyb3d8bbwe}
export WINE_UWP_PACKAGE_FAMILY_NAME=${WINE_UWP_PACKAGE_FAMILY_NAME:-Microsoft.WindowsCalculator.Dev_8wekyb3d8bbwe}
export WINE_UWP_PACKAGE_PATH=${WINE_UWP_PACKAGE_PATH:-"Z:$(echo "$S" | sed 's|/|\\|g')"}
export WINE_UWP_AUMID=${WINE_UWP_AUMID:-Microsoft.WindowsCalculator.Dev_8wekyb3d8bbwe!App}
# Frame title for the hosted CoreWindow (its own text is the entry id, "App"), and the launch size;
# a window covering the whole screen cannot be resized through the frame.
export WINE_UWP_WINDOW_TITLE=${WINE_UWP_WINDOW_TITLE:-Calculator}
export WINE_UWP_WINDOW_SIZE=${WINE_UWP_WINDOW_SIZE:-1280x800}
# Framework dependencies: "fullname|family|path;..." (GetCurrentPackageInfo returns them after the main package)
if [ -z "${WINE_UWP_PACKAGE_DEPENDENCIES:-}" ] && [ -d "$HOME/uwp-bin/stage/frameworks" ]; then
  deps=""
  for d in "$HOME"/uwp-bin/stage/frameworks/*/; do
    full=$(basename "$d"); name=${full%%_*}; pub=${full##*__}
    deps="$deps$full|${name}_$pub|Z:$(echo "${d%/}" | sed 's|/|\\|g');"
  done
  export WINE_UWP_PACKAGE_DEPENDENCIES="$deps"
fi
# Package graph DLL search path: Windows adds every framework package root to the process search
# path, which is how an app resolves framework DLLs it does not carry itself.
if [ -d "$HOME/uwp-bin/stage/frameworks" ]; then
  fwpath=""
  for d in "$HOME"/uwp-bin/stage/frameworks/*/; do fwpath="$fwpath${fwpath:+:}${d%/}"; done
  export WINEPATH="${WINEPATH:+$WINEPATH:}$fwpath"
fi
exe=${1:-CalculatorApp.exe}; shift || true
# Full unfiltered log always goes to $UWP_LOG for post-mortem (module map, SEH context).
UWP_LOG=${UWP_LOG:-/tmp/wine-uwp-last.log}
# UWP_VDESKTOP=WxH runs the app inside a Wine virtual desktop of that size (e.g. under Xvfb, where
# `import -display :N -window root` can screenshot it regardless of the host session).
if [ -n "${UWP_VDESKTOP:-}" ]; then
  timeout "${UWP_TIMEOUT:-60}" wine explorer "/desktop=uwp,$UWP_VDESKTOP" "$exe" "$@" > "$UWP_LOG" 2>&1
else
  timeout "${UWP_TIMEOUT:-60}" wine "$exe" "$@" > "$UWP_LOG" 2>&1
fi
status=$?
grep -vE "MESA|pci id|winemac|Loaded .*system32" "$UWP_LOG"
exit $status

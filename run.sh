#!/usr/bin/env bash
# Run a staged UWP app (or a probe) in the wine-uwp prefix with useful tracing.
# Usage: ./run.sh [exe] [args...]     default exe: CalculatorApp.exe in the stage dir
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]:-$0}")/./env.sh"
export WINEPREFIX=${WINEPREFIX}
# Tests run on a headless Xvfb (DISPLAY=:98), but a desktop session also exports WAYLAND_DISPLAY
# and Wine prefers winewayland.drv when it is set - which would put the app on the real desktop
# compositor instead of the test display. Unsetting it is NOT enough: libwayland then falls back to
# $XDG_RUNTIME_DIR/wayland-0 and connects to the same compositor anyway (you can see it in
# "err:waylanddrv:wayland_process_init ... compositor doesn't support ..."). Name the driver.
unset WAYLAND_DISPLAY
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
# WINE_BUILD comes from env.sh, which reconciles it with WINE_BIN_DIR.
if [ -x "$WINE_BUILD/wine" ]; then export PATH="$WINE_BUILD:$PATH"; fi
# DLLs where Wine's builtin must yield to the extracted Windows copy. Wine also ships these names.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-windows.ui.xaml=n;windows.ui=n;twinapi.appcore=n;coremessaging=n;windows.applicationmodel=n;wintypes=n;threadpoolwinrt=n;msvcp_win=n;bcp47mrm=n;bcp47langs=n;profapi=n;rometadata=n;windows.globalization=n;msftedit=n;msxml6=n;cryptowinrt=n}"
# A run killed by a timeout leaves this prefix's explorer.exe /desktop and the svchost.exe hosting
# CoreMessagingRegistrar behind; they accumulate across runs and quietly wreck every later
# measurement, because the activation is a race and a loaded machine loses it. Clear them with
# wineserver -k, which is scoped to WINEPREFIX - a pkill on the image name would also kill other
# prefixes running on this machine.
wineserver -k 2>/dev/null

"$(dirname "$0")/install-overrides.sh" >/dev/null
# Wine picks the graphics driver from this key; there is no environment override. Pin it to x11 so
# a run cannot land on the desktop compositor (see the WAYLAND_DISPLAY note above).
wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d x11 /f >/dev/null 2>&1
# CoreMessaging.dll blocks in WaitServiceState until its registrar service reports SERVICE_RUNNING,
# so make sure the service is up before the app asks for a CoreDispatcher.
if ! wine sc query CoreMessagingRegistrar 2>/dev/null | grep -q RUNNING; then
  wine net start CoreMessagingRegistrar >/dev/null 2>&1
  for _ in $(seq 1 15); do
    wine sc query CoreMessagingRegistrar 2>/dev/null | grep -q RUNNING && break
    sleep 1
  done
fi
# UWP_STAGE names a directory under ${UWP_BIN}/stage (see stage-app.sh); the identity defaults below
# only make sense for the calculator, so a different stage carries its own identity file.
S=${UWP_BIN}/stage/${UWP_STAGE:-calculator}
cd "$S"
# stage-app.sh writes identity.env for packages staged straight out of a Windows install.
if [ -f "$S/identity.env" ]; then . "$S/identity.env"; fi
# Package identity consumed by the patched Wine (ntdll/kernelbase read these).
export WINE_UWP_PACKAGE_FULL_NAME=${WINE_UWP_PACKAGE_FULL_NAME:-Microsoft.WindowsCalculator.Dev_0.365.1.0_x64__8wekyb3d8bbwe}
export WINE_UWP_PACKAGE_FAMILY_NAME=${WINE_UWP_PACKAGE_FAMILY_NAME:-Microsoft.WindowsCalculator.Dev_8wekyb3d8bbwe}
# Always computed from $S, never taken from identity.env: the package path is just the stage
# directory in Windows form, and a stale copy of it is unfixable from inside the app. Staged
# identity.env files used to bake this in, which is what broke every app when this tree moved out
# of the home directory - the app looked for its package at a path that no longer existed and died
# just after CoreWindow with nothing but an RtlReportException to show for it.
export WINE_UWP_PACKAGE_PATH="Z:$(echo "$S" | sed 's|/|\\|g')"
export WINE_UWP_AUMID=${WINE_UWP_AUMID:-Microsoft.WindowsCalculator.Dev_8wekyb3d8bbwe!App}
# Frame title for the hosted CoreWindow (its own text is the entry id, "App"), and the launch size;
# a window covering the whole screen cannot be resized through the frame.
export WINE_UWP_WINDOW_TITLE=${WINE_UWP_WINDOW_TITLE:-Calculator}
export WINE_UWP_WINDOW_SIZE=${WINE_UWP_WINDOW_SIZE:-1280x800}
# Framework dependencies: "fullname|family|path;..." (GetCurrentPackageInfo returns them after the main package)
if [ -z "${WINE_UWP_PACKAGE_DEPENDENCIES:-}" ] && [ -d "${UWP_BIN}/stage/frameworks" ]; then
  deps=""
  for d in ${UWP_BIN}/stage/frameworks/*/; do
    full=$(basename "$d"); name=${full%%_*}; pub=${full##*__}
    deps="$deps$full|${name}_$pub|Z:$(echo "${d%/}" | sed 's|/|\\|g');"
  done
  export WINE_UWP_PACKAGE_DEPENDENCIES="$deps"
fi
# Package graph DLL search path: Windows adds every framework package root to the process search
# path, which is how an app resolves framework DLLs it does not carry itself.
if [ -d "${UWP_BIN}/stage/frameworks" ]; then
  fwpath=""
  for d in ${UWP_BIN}/stage/frameworks/*/; do fwpath="$fwpath${fwpath:+:}${d%/}"; done
  export WINEPATH="${WINEPATH:+$WINEPATH:}$fwpath"
fi
# Windows creates a per-package key under the user's AppContainer storage when the package is
# registered, and that is where Windows.Globalization writes ApplicationLanguages.PrimaryLanguageOverride:
# without it the setter fails with 0x80070032 and Feedback Hub dies on the unhandled error (Findings-M4).
# Staging a package is our registration step, so create it here - grep user.reg rather than shelling out
# to reg.exe on every launch. reg.exe must not run in the app's environment set up above: from the stage
# directory with the frameworks on WINEPATH it exits with 5 and writes nothing, which is why every fresh
# prefix failed its first launch (Feedback Hub, and Shapr3D from the packaged setup; Findings-M4 09-29).
APPC='Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Storage'
if ! grep -qF "[$APPC\\\\$WINE_UWP_PACKAGE_FAMILY_NAME\\\\ResourcesConfig]" "$WINEPREFIX/user.reg" 2>/dev/null; then
  (cd "$WINEPREFIX" && env -u WINEPATH -u WINEDLLOVERRIDES -u WINE_UWP_PACKAGE_FULL_NAME -u WINE_UWP_PACKAGE_FAMILY_NAME \
      -u WINE_UWP_PACKAGE_PATH -u WINE_UWP_AUMID -u WINE_UWP_PACKAGE_DEPENDENCIES wine reg add "HKCU\\Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Storage\\$WINE_UWP_PACKAGE_FAMILY_NAME\\ResourcesConfig" /f >/dev/null 2>&1) ||
    echo "run.sh: could not create the AppContainer storage key for $WINE_UWP_PACKAGE_FAMILY_NAME" >&2
fi

exe=${1:-${UWP_EXE:-CalculatorApp.exe}}; shift || true
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

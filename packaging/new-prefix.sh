#!/usr/bin/env bash
# Build a wine-uwp prefix from nothing, from one Windows file tree.
#
#   packaging/new-prefix.sh WIN11_TREE PREFIX
#
# WIN11_TREE is shaped like uwp-bin/win11 (Windows/System32, Windows/Fonts, Windows/Globalization,
# Windows/System32/config/SOFTWARE), e.g. the output of packaging/harvest/harvest.py. Steps, each printed
# as "STAGE <what>" for the setup wizard:
#   1. wineboot the new prefix with our Wine build
#   2. generate the WinRT class and COM interface registrations from the tree's SOFTWARE hive and import
#      them (gen-classes-reg.py, gen-interfaces-reg.py; install-overrides.sh does the CLSID and
#      single-module ones itself)
#   3. resolve the private symbols wineuwp needs for these DLLs (packaging/harvest/resolve_symbols.py)
#   4. install-overrides.sh with WIN11 pointing at the tree
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
tree=$(realpath "$1")
prefix=$(realpath -m "$2")
. "$here/env.sh"
export WINEPREFIX=$prefix WIN11=$tree
export PATH="$WINE_BUILD:$PATH"
unset WAYLAND_DISPLAY
PY=${PY:-$UWP_BIN/venv/bin/python}
[ -f "$tree/Windows/System32/config/SOFTWARE" ] || { echo "no SOFTWARE hive in $tree" >&2; exit 1; }

echo "STAGE Creating the Wine prefix"
mkdir -p "$prefix"
wineboot -i >/dev/null 2>&1
wineserver -w

echo "STAGE Registering the CoreMessaging service"
# every CoreMessaging endpoint registers with this service: Windows' own CoreMessaging.dll hosted by Wine's
# svchost (Findings-M2, "Running the genuine CoreMessagingRegistrar"). Without it CoreMessaging fail-fasts with
# 0x80070424 (ERROR_SERVICE_DOES_NOT_EXIST) as the first view starts.
svc='HKLM\System\CurrentControlSet\Services\CoreMessagingRegistrar'
wine reg add "$svc" /v DisplayName /d CoreMessaging /f >/dev/null
wine reg add "$svc" /v ErrorControl /t REG_DWORD /d 1 /f >/dev/null
wine reg add "$svc" /v ImagePath /t REG_EXPAND_SZ /d '%SystemRoot%\system32\svchost.exe -k LocalServiceNoNetwork' /f >/dev/null
wine reg add "$svc" /v ObjectName /d LocalSystem /f >/dev/null
wine reg add "$svc" /v Start /t REG_DWORD /d 3 /f >/dev/null
wine reg add "$svc" /v Type /t REG_DWORD /d 32 /f >/dev/null
wine reg add 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\Svchost' /v LocalServiceNoNetwork \
    /t REG_MULTI_SZ /d CoreMessagingRegistrar /f >/dev/null   # the svchost group named in ImagePath
wine reg add "$svc\\Parameters" /v ServiceDll /t REG_EXPAND_SZ /d '%SystemRoot%\system32\CoreMessaging.dll' /f >/dev/null

# Wine picks its graphics driver from this key only; pin X11 (Xwayland on a Wayland desktop), as run.sh does on
# every launch, for launchers that do not go through run.sh (wineuwp-launch.exe)
wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d x11 /f >/dev/null

echo "STAGE Registering Windows Runtime classes"
"$PY" "$here/gen-classes-reg.py" "$tree/Windows/System32/config/SOFTWARE" "$tree/Windows/System32" \
      "$tree/wine-uwp-classes.reg" | tail -1
wine reg import "$tree/wine-uwp-classes.reg" >/dev/null 2>&1

echo "STAGE Registering COM interfaces"
"$PY" "$here/gen-interfaces-reg.py" "$tree/Windows/System32/config/SOFTWARE" "$tree/Windows/System32" \
      "$tree/wine-uwp-interfaces.reg" | tail -1
wine reg import "$tree/wine-uwp-interfaces.reg" >/dev/null 2>&1
wineserver -w

echo "STAGE Resolving Windows symbols"
# the few private addresses wineuwp needs, for exactly the DLLs in this tree (Microsoft's public PDBs)
python3 "$here/packaging/harvest/resolve_symbols.py" "$tree/Windows/System32" \
        "$prefix/drive_c/windows/wineuwp-symbols.txt"

echo "STAGE Installing Windows components"
"$here/install-overrides.sh"
wineserver -w
echo "STAGE Prefix ready: $prefix"

#!/usr/bin/env bash
# Build a wine-uwp prefix from nothing, from one Windows file tree.
#
#   packaging/new-prefix.sh WIN11_TREE PREFIX
#
# WIN11_TREE is shaped like uwp-bin/win11 (Windows/System32, Windows/Fonts, Windows/Globalization,
# Windows/System32/config/SOFTWARE), e.g. the output of packaging/harvest/harvest.py. Steps, each printed
# as "STAGE <what>" for the setup wizard:
#   1. wineboot the new prefix with our Wine build
#   2. the prefix's own registrations, and the WinRT class and COM interface registrations generated from the
#      tree's SOFTWARE hive (Wine's wineuwp-setup --new-prefix; install-overrides.sh does the rest)
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
[ -f "$tree/Windows/System32/config/SOFTWARE" ] || { echo "no SOFTWARE hive in $tree" >&2; exit 1; }

echo "STAGE Creating the Wine prefix"
mkdir -p "$prefix"
# no wine-mono or wine-gecko: .NET Native apps are native code, and without an installed copy wineboot would stop
# at a download dialog (the AppImage has none; the host usually has them system-wide)
WINEDLLOVERRIDES="mscoree=;mshtml=" wineboot -i >/dev/null 2>&1
wineserver -w

echo "STAGE Registering Windows Runtime classes and COM interfaces"
# Wine's wineuwp-setup, from inside the prefix: the CoreMessagingRegistrar service (every CoreMessaging endpoint
# registers with it; without it CoreMessaging fail-fasts as the first view starts), the x11 graphics driver for
# launchers that do not go through run.sh, and the tree's in-process WinRT classes and COM interfaces, generated from
# its SOFTWARE hive into the tree (wine-uwp-classes.reg, wine-uwp-interfaces.reg) and imported
WINEDEBUG=${WINEDEBUG:--all} wine wineuwp-setup --new-prefix --windows "$tree" --build "$WINE_BUILD" --data "$here" \
    2> >(grep -v '^MESA\|^pci id' >&2 || true)
wineserver -w

echo "STAGE Resolving Windows symbols"
# the few private addresses wineuwp needs, for exactly the DLLs in this tree (Microsoft's public PDBs)
python3 "$here/packaging/harvest/resolve_symbols.py" "$tree/Windows/System32" \
        "$prefix/drive_c/windows/wineuwp-symbols.txt"

echo "STAGE Installing Windows components"
"$here/install-overrides.sh"
wineserver -w
echo "STAGE Prefix ready: $prefix"

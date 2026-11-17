#!/usr/bin/env bash
# Install a UWP release of Minecraft for Windows (Bedrock, up to 1.21.51; later releases are GDK) from Microsoft's
# own servers into its own data directory. Playing needs a Microsoft account that owns Minecraft.
# Usage: packaging/install-minecraft-uwp.sh DATADIR [VERSION]     (default: the newest UWP release)
#
# Steps: the version's update id from MCMrARM's mc-w10-versiondb, the package from FE3 (fe3-download.sh), the Store
# Engagement framework (fetch-store-engagement.sh), DXVK/vkd3d-proton (install-d3d-runtime.sh), then wineuwp-install,
# which takes the other frameworks from Minecraft's Store category and applies profiles/<family>.env.
# Run it with: UWP_STAGE=<DATADIR relative to uwp-bin/stage, e.g. ../mc-data/stage/minecraft> ./run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
. "$here/env.sh"
[ $# -ge 1 ] || { echo "usage: $0 DATADIR [VERSION]" >&2; exit 2; }
data=$(mkdir -p "$1" && cd "$1" && pwd); want=${2:-}
category=d25480ca-36aa-46e6-b76b-39608d49558c   # Minecraft for Windows' Store category

db=$(curl -fsSL https://raw.githubusercontent.com/MCMrARM/mc-w10-versiondb/master/versions.json.min)
read -r version id < <(python3 -I -c '
import json, sys
db = json.loads(sys.argv[1]); want = sys.argv[2]
rows = [r for r in db if (r[0] == want if want else r[2] == 0)]
if not rows: sys.exit("no such version in mc-w10-versiondb: " + (want or "(release)"))
print(rows[-1][0], rows[-1][1])' "$db" "$want")
pkg=$data/Minecraft.Windows_$version.appx
echo "Minecraft for Windows $version (update $id)"
[ -s "$pkg" ] || "$here/packaging/fe3-download.sh" "$id" "$pkg"
"$here/packaging/fetch-store-engagement.sh" "$data"
"$here/packaging/install-d3d-runtime.sh" --download

export PATH="$WINE_BUILD:$PATH"
wine wineuwp-install "$pkg" "$data" --windows "${WIN11:-$UWP_BIN/win11}" --stage minecraft --category "$category"
cat <<MSG

Installed in $data/stage/minecraft. Its profile binds Xbox Live to Minecraft's title: the first sign-in in the game
asks for one more approval in the browser (or run: wine $UWP_BIN/build-probes/probe-xboxauth.exe clientdevice 0000000048183522).
MSG

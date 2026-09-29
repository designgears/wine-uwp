#!/usr/bin/env bash
# Install a UWP app (Shapr3D) into a Bottles bottle, running on the wine-uwp runner.
#
#   wine-uwp bottles PACKAGE [BOTTLE-NAME]        (this script, from an installed wine-uwp or its runner tarball)
#
# Needs the Windows components already prepared (the setup wizard's first half, or `wine-uwp harvest ISO`), and
# Bottles, from Flathub (com.usebottles.bottles) or installed natively. Steps:
#
#   1. copy this wine-uwp tree into Bottles' runners/ as wine-uwp-<version> (Bottles lists any directory there)
#   2. bottles-cli new: a bottle on that runner, from a recipe that keeps DXVK, VKD3D and the rest off - they
#      replace d3d11/dxgi, which wine-uwp's composition depends on
#   3. copy the Windows components into the bottle's C:\wine-uwp: Bottles' Flatpak sandbox sees nothing outside its
#      own data, and the registry refers to those files by path at run time. Inside drive_c, because a path through
#      the prefix directory itself (Z:\...\bottles\NAME\...) does not resolve for Windows.Storage: the app died on
#      an unhandled 0x80070002 walking to its own package folder
#   4. inside Bottles' sandbox, so only one wineserver ever drives the bottle: new-prefix.sh on the bottle, then
#      install-app.py into it
#   5. bottles-cli add: the program is the app's wineuwp-launch.exe, which sets up the package identity itself
set -euo pipefail
# this script is share/wine-uwp/packaging/bottles/ in an installed wine-uwp tree (dist, runner tarball)
tree=$(cd "$(dirname "$(readlink -f "$0")")/../../../.." && pwd)
share=$tree/share/wine-uwp
[ -x "$tree/bin/wine" ] || { echo "run this from an installed wine-uwp (bin/wine-uwp bottles), not the checkout" >&2; exit 1; }
package=$(realpath "${1:?usage: wine-uwp bottles PACKAGE [BOTTLE-NAME]}")
name=${2:-Shapr3D}
data=${WINE_UWP_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/wine-uwp}
windows=${WIN11:-$data/windows}
# prepared by the Flatpak's setup instead?
flatpak_windows=$HOME/.var/app/io.github.wine_uwp.WineUWP/data/wine-uwp/windows
[ -d "$windows" ] || [ ! -d "$flatpak_windows" ] || windows=$flatpak_windows
[ -f "$windows/.wine-uwp-harvested" ] || [ -f "$windows/Windows/System32/config/SOFTWARE" ] ||
  { echo "no Windows components in $windows: run the setup (wine-uwp setup) or wine-uwp harvest ISO first" >&2; exit 1; }
runner=wine-uwp-$("$tree/bin/wine" --version 2>/dev/null | sed 's/^wine-//; s/ .*//')

if flatpak info com.usebottles.bottles >/dev/null 2>&1; then
  bottles=$HOME/.var/app/com.usebottles.bottles/data/bottles
  inside() { flatpak run --filesystem="$(dirname "$package")":ro --command=bash com.usebottles.bottles -c "$1"; }
  cli() { flatpak run --command=bottles-cli com.usebottles.bottles "$@"; }
elif command -v bottles-cli >/dev/null; then
  bottles=${XDG_DATA_HOME:-$HOME/.local/share}/bottles
  inside() { bash -c "$1"; }
  cli() { bottles-cli "$@"; }
else
  echo "Bottles is not installed (flatpak install flathub com.usebottles.bottles)" >&2; exit 1
fi
echo "STAGE Installing the $runner runner into Bottles"
mkdir -p "$bottles/runners"
rm -rf "$bottles/runners/$runner"
cp -a "$tree" "$bottles/runners/$runner"
R=$bottles/runners/$runner

echo "STAGE Creating the bottle $name"
bottle=$bottles/bottles/$name
if [ ! -f "$bottle/bottle.yml" ]; then
  recipe=$bottles/wine-uwp-environment.yml
  cp "$share/packaging/bottles/wine-uwp-environment.yml" "$recipe"
  cli new --bottle-name "$name" --environment custom --custom-environment "$recipe" --runner "$runner" --arch win64
fi
[ -f "$bottle/bottle.yml" ] || { echo "Bottles did not create $bottle" >&2; exit 1; }

echo "STAGE Copying the Windows components into the bottle"
W=$bottle/drive_c/wine-uwp
mkdir -p "$W"
cp -a --reflink=auto "$windows"/. "$W/windows"/
rm -f "$W"/windows/*.imported

echo "STAGE Setting up Windows components in the bottle"
q() { printf '%q' "$1"; }
inside "export WINEPREFIX=$(q "$bottle") WINE_BUILD=$(q "$R/bin") SHIM_DIR=$(q "$R/share/wine-uwp/shims") PY=python3 \
               UWP_BIN=$(q "$W") WINEDEBUG=-all
        $(q "$R/share/wine-uwp/packaging/new-prefix.sh") $(q "$W/windows") $(q "$bottle") &&
        python3 $(q "$R/share/wine-uwp/packaging/install-app.py") $(q "$package") $(q "$W") \
                --windows $(q "$W/windows")" | tee "$W/setup.log" | grep -E '^(STAGE|INSTALLED)'
app=$(grep -a '^INSTALLED ' "$W/setup.log" | tail -1 | cut -d' ' -f2)
[ -n "$app" ] || { echo "the app did not install; see $W/setup.log" >&2; exit 1; }
title=$(sed -n "s/^export WINE_UWP_WINDOW_TITLE='\(.*\)'/\1/p" "$W/stage/$app/identity.env")

echo "STAGE Adding $title to the bottle"
cli add -b "$name" -n "$title" -p "$W/stage/$app/wineuwp-launch.exe" --no-d7vk --no-dxvk --no-vkd3d \
    --no-dxvk-nvapi
echo
echo "STAGE Done: open Bottles, bottle \"$name\", and run \"$title\""

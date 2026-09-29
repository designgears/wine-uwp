#!/usr/bin/env bash
# Build the wine-uwp Flatpak and a single-file bundle of it.
#
#   packaging/flatpak/build.sh [--install]
#
# Needs org.flatpak.Builder, org.gnome.Sdk//50 and org.freedesktop.Sdk.Extension.mingw-w64//25.08 from Flathub
# (flatpak install --user flathub ...). Output: uwp-bin/flatpak/repo (an OSTree repo) and
# uwp-bin/flatpak/wine-uwp.flatpak (flatpak install --user wine-uwp.flatpak). --install also installs it for this
# user. The first build compiles Wine and takes a while; later ones reuse flatpak-builder's cache.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/../.." && pwd)
. "$repo_root/env.sh"
out=${FLATPAK_OUT:-$UWP_BIN/flatpak}
id=io.github.wine_uwp.WineUWP
mkdir -p "$out"

# the wine-uwp module's source: scripts, wizard, shims
"$repo_root/packaging/build-dist.sh" --no-wine "$here/.build/dist" | grep -v '^STAGE Done'

builder=(flatpak run --filesystem="$repo_root" --filesystem="$out" org.flatpak.Builder)
"${builder[@]}" --user --force-clean --ccache --state-dir="$out/state" --repo="$out/repo" \
  ${1:+$([ "$1" = --install ] && echo --install)} "$out/build" "$here/$id.yml"
flatpak build-bundle "$out/repo" "$out/wine-uwp.flatpak" "$id"
echo "bundle: $out/wine-uwp.flatpak ($(du -h "$out/wine-uwp.flatpak" | cut -f1))"

#!/usr/bin/env bash
# Package the Flatpak's build of wine-uwp as a Bottles runner: wine-uwp-<wine version>.tar.xz.
#
#   packaging/bottles/make-runner.sh [OUTDIR]        (after packaging/flatpak/build.sh)
#
# The Flatpak's /app is exactly a runner: bin/wine, lib/wine, and share/wine-uwp with the setup tools, shims and
# bin/wine-uwp. It is built in the GNOME SDK, so it runs inside Bottles' Flatpak sandbox (GNOME 50) as well as on the
# host; a Wine built on the host itself may need a newer glibc than Bottles has. To install it into Bottles and set
# up a bottle, unpack it anywhere and run bin/wine-uwp bottles PACKAGE (packaging/bottles/install-in-bottles.sh).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../../env.sh"
files=${FLATPAK_OUT:-$UWP_BIN/flatpak}/build/files
[ -x "$files/bin/wine" ] || { echo "no Flatpak build at $files (run packaging/flatpak/build.sh first)" >&2; exit 1; }
out=$(realpath -m "${1:-$UWP_BIN/bottles}")
version=$("$files/bin/wine" --version | sed 's/^wine-//; s/ .*//')
name=wine-uwp-$version
mkdir -p "$out"
rm -rf "$out/$name"
cp -a "$files" "$out/$name"
# Flatpak-only pieces
rm -rf "$out/$name"/{manifest.json,share/applications,share/metainfo,share/icons,share/app-info,share/runtime,lib/debug}
tar -C "$out" -cJf "$out/$name.tar.xz" "$name"
echo "runner: $out/$name ($(du -sh "$out/$name" | cut -f1)), $out/$name.tar.xz ($(du -h "$out/$name.tar.xz" | cut -f1))"

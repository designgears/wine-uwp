#!/usr/bin/env bash
# Provide the Direct3D 12 runtime GDK games need: DXVK's dxgi/d3d11 and vkd3d-proton's d3d12/d3d12core (x64).
# Usage: packaging/install-d3d-runtime.sh [--download]
#
# Wine's own d3d12 lacks features these games test for at start (Halo Infinite: Tiled Resources Tier 2), and
# vkd3d-proton presents through DXVK's dxgi. The result is $UWP_BIN/ext/d3d-runtime/x86_64-windows with the four
# DLLs; packaging/install-app.py links them next to each game executable that imports d3d12.dll and lists them as
# native in the game's launch.env, so the Xbox app and the UWP apps keep Wine's builtins.
#
# Sources, in order: WINE_UWP_DXVK and WINE_UWP_VKD3D_PROTON (directories holding x64/ or x86_64-windows/), an
# installed Proton Experimental, earlier downloads in $UWP_BIN/ext, and with --download the pinned releases below.
# No network operation happens without --download.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
. "$here/env.sh"
download=0
case ${1:-} in
  --download) download=1 ;;
  '') ;;
  *) echo "usage: $0 [--download]" >&2; exit 2 ;;
esac

DXVK_VERSION=3.1.1
DXVK_SHA256=40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608
VKD3D_VERSION=3.0.1
VKD3D_SHA256=3cf2315522af5e43605ef6d3c41dad91387040bf97199934f3f7ab76caaa2f0c

# the x64 directory of a DXVK or vkd3d-proton tree holding all of the named DLLs, or nothing
x64_dir() {
  local root=$1 sub dll; shift
  for sub in x64 x86_64-windows; do
    for dll in "$@"; do [ -s "$root/$sub/$dll" ] || continue 2; done
    echo "$root/$sub"; return 0
  done
  return 1
}

steam=${XDG_DATA_HOME:-$HOME/.local/share}/Steam/steamapps/common
find_runtime() {  # name env-dir proton-subdir ext-dir dlls...
  local name=$1 envdir=$2 proton=$3 ext=$4; shift 4
  local candidate
  for candidate in "$envdir" "$steam/Proton - Experimental/files/lib/wine/$proton" \
      "$HOME/.steam/steam/steamapps/common/Proton - Experimental/files/lib/wine/$proton" "$ext"; do
    [ -n "$candidate" ] || continue
    x64_dir "$candidate" "$@" && return 0
  done
  return 1
}

fetch() {  # url sha256 dest-dir
  local work
  work=$(mktemp -d "$UWP_BIN/ext/.d3d-install.XXXXXX")
  curl -sS --fail --location --retry 2 -o "$work/archive" "$1"
  echo "$2  $work/archive" | sha256sum --check --status || { rm -rf -- "$work"; echo "checksum mismatch: $1" >&2; exit 1; }
  mkdir "$work/out"
  case $1 in
    *.zst) tar --zstd -xf "$work/archive" -C "$work/out" ;;
    *) tar -xzf "$work/archive" -C "$work/out" ;;
  esac
  rm -rf -- "$3"
  mv "$work/out/"* "$3"
  rm -rf -- "$work"
}

mkdir -p "$UWP_BIN/ext"
dxvk_ext=$UWP_BIN/ext/dxvk-$DXVK_VERSION
vkd3d_ext=$UWP_BIN/ext/vkd3d-proton-$VKD3D_VERSION
if ! dxvk=$(find_runtime dxvk "${WINE_UWP_DXVK:-}" dxvk "$dxvk_ext" dxgi.dll d3d11.dll); then
  [ "$download" = 1 ] || { echo "DXVK absent; rerun with --download or set WINE_UWP_DXVK." >&2; exit 1; }
  fetch "https://github.com/doitsujin/dxvk/releases/download/v$DXVK_VERSION/dxvk-$DXVK_VERSION.tar.gz" \
        "$DXVK_SHA256" "$dxvk_ext"
  dxvk=$(x64_dir "$dxvk_ext" dxgi.dll d3d11.dll)
fi
if ! vkd3d=$(find_runtime vkd3d-proton "${WINE_UWP_VKD3D_PROTON:-}" vkd3d-proton "$vkd3d_ext" d3d12.dll d3d12core.dll); then
  [ "$download" = 1 ] || { echo "vkd3d-proton absent; rerun with --download or set WINE_UWP_VKD3D_PROTON." >&2; exit 1; }
  fetch "https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v$VKD3D_VERSION/vkd3d-proton-$VKD3D_VERSION.tar.zst" \
        "$VKD3D_SHA256" "$vkd3d_ext"
  vkd3d=$(x64_dir "$vkd3d_ext" d3d12.dll d3d12core.dll)
fi

out=$UWP_BIN/ext/d3d-runtime/x86_64-windows
mkdir -p "$out"
for dll in dxgi.dll d3d11.dll; do ln -sfn "$(realpath "$dxvk/$dll")" "$out/$dll"; done
for dll in d3d12.dll d3d12core.dll; do ln -sfn "$(realpath "$vkd3d/$dll")" "$out/$dll"; done
printf 'DXVK: %s\nvkd3d-proton: %s\nD3D runtime: %s\n' "$dxvk" "$vkd3d" "$out"

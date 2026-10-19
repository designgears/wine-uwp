#!/usr/bin/env bash
# Install Proton's EAC bridge without restarting Wine or installing a Windows driver.
# Usage: WINEPREFIX=/path/to/prefix packaging/install-eac-runtime.sh [--download]
# Prefer an existing PROTON_EAC_RUNTIME or Steam installation; --download uses
# Lutris's published runtime bundle when neither is available.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
. "$here/env.sh"
download=0
case ${1:-} in
  --download) download=1 ;;
  '') ;;
  *) echo "usage: $0 [--download]" >&2; exit 2 ;;
esac
valid_runtime() {
  local file
  for file in v2/lib64/easyanticheat.dll v2/lib64/easyanticheat.so \
      v2/lib64/easyanticheat_x64.dll v2/lib64/easyanticheat_x64.so \
      v2/lib32/easyanticheat_x86.dll v2/lib32/easyanticheat_x86.so; do
    [ -s "$1/$file" ] || return 1
  done
}
runtime=${PROTON_EAC_RUNTIME:-}
if [ -n "$runtime" ]; then
  valid_runtime "$runtime" || { echo "Incomplete EAC runtime: $runtime" >&2; exit 1; }
else
  for candidate in "$UWP_BIN/ext/ProtonEasyAntiCheatRuntime" \
      "${XDG_DATA_HOME:-$HOME/.local/share}/Steam/steamapps/common/Proton EasyAntiCheat Runtime" \
      "$HOME/.steam/steam/steamapps/common/Proton EasyAntiCheat Runtime"; do
    if valid_runtime "$candidate"; then runtime=$candidate; break; fi
  done
fi
if [ -z "$runtime" ]; then
  [ "$download" = 1 ] || { echo "EAC runtime absent; rerun with --download or set PROTON_EAC_RUNTIME." >&2; exit 1; }
  mkdir -p "$UWP_BIN/ext"
  work=$(mktemp -d "$UWP_BIN/ext/.eac-install.XXXXXX")
  trap 'rm -rf -- "$work"' EXIT
  curl --fail --location --retry 2 \
    https://github.com/lutris/buildbot/releases/download/2022.06.23/eac_runtime-20240114.tar.xz \
    -o "$work/runtime.tar.xz"
  echo "8c711a3dd0906fdedd2fdcdedc7b33e09eff5aa22fe2b2dffcc046f02e194957  $work/runtime.tar.xz" | sha256sum --check --status
  tar -xJf "$work/runtime.tar.xz" -C "$work"
  valid_runtime "$work/eac_runtime"
  runtime="$UWP_BIN/ext/ProtonEasyAntiCheatRuntime"
  # Do not replace an incomplete user installation silently.
  [ ! -e "$runtime" ] || { echo "Incomplete runtime already exists: $runtime" >&2; exit 1; }
  mv "$work/eac_runtime" "$runtime"
fi
runtime=$(realpath "$runtime")
target="$WINEPREFIX/drive_c/Program Files (x86)/Proton EasyAntiCheat Runtime"
mkdir -p "$(dirname "$target")"
if [ -L "$target" ]; then
  [ "$(readlink -f "$target")" = "$runtime" ] || ln -sfnT "$runtime" "$target"
elif [ -e "$target" ]; then
  echo "Refusing to replace existing directory: $target" >&2
  exit 1
else
  ln -s "$runtime" "$target"
fi
printf 'EAC runtime: %s\nPrefix link: %s\n' "$runtime" "$target"
echo 'run.sh will expose PROTON_EAC_RUNTIME and its Wine DLL search paths on the next launch.'

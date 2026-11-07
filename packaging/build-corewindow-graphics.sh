#!/usr/bin/env bash
# Historical name retained for installer compatibility. Download stock DXVK;
# CoreWindow adaptation is provided by wineuwp, not by a DXVK source patch.
# Usage: build-corewindow-graphics.sh OUTPUT
set -euo pipefail
mkdir -p "${1:?Fresh output directory required}"
output=$(cd "$1" && pwd)
if [[ -e "$output/dxvk" ]]; then
    echo 'Choose a fresh output directory; an existing runtime will not be overwritten.' >&2
    exit 1
fi
version=3.1.1
archive_sha256=40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608
url="https://github.com/doitsujin/dxvk/releases/download/v$version/dxvk-$version.tar.gz"
work=$(mktemp -d "$output/.download.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
curl --fail --location --retry 2 --output "$work/dxvk.tar.gz" "$url"
printf '%s  %s\n' "$archive_sha256" "$work/dxvk.tar.gz" | sha256sum --check --status
mkdir "$work/unpacked"
tar -xzf "$work/dxvk.tar.gz" -C "$work/unpacked"
test -s "$work/unpacked/dxvk-$version/x64/dxgi.dll"
test -s "$work/unpacked/dxvk-$version/x64/d3d11.dll"
mv "$work/unpacked/dxvk-$version" "$output/dxvk"
cd "$output"
printf 'Official DXVK %s\nURL %s\nArchive SHA256 %s\nNo source modifications or compilation.\n' \
    "$version" "$url" "$archive_sha256" > SOURCE-MANIFEST.txt
sha256sum dxvk/x64/dxgi.dll dxvk/x64/d3d11.dll > SHA256SUMS
sha256sum --check SHA256SUMS
printf '%s\n' \
    "Downloaded official DXVK $version to $output/dxvk." \
    'Use WINE_UWP_DXVK_COREWINDOW=1 with Wine-UWP for CoreWindow games.' \
    'Supply vkd3d-proton separately, for example with packaging/install-d3d-runtime.sh.'

#!/usr/bin/env bash
# Build wine-uwp-x86_64.AppImage from the Flatpak's build (packaging/flatpak/build.sh first).
#
#   packaging/appimage/build.sh [OUTDIR]
#
# The Flatpak's /app is Wine, the installer, 7-Zip and share/wine-uwp, built in the freedesktop SDK; the AppImage
# adds the freedesktop runtime's Python (the setup tools need only its standard library) and the libraries hosts do
# not reliably have (bundle_libs.py). Everything bundled needs at most GLIBC_2.38, so the AppImage runs on
# Ubuntu 24.04, Debian 13, Fedora 39 and newer. The host provides glibc, its GPU drivers (GL, Vulkan), X11/Wayland,
# fontconfig/freetype, audio and D-Bus.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
. "$repo/env.sh"
files=${FLATPAK_OUT:-$UWP_BIN/flatpak}/build/files
[ -x "$files/bin/wine" ] || { echo "no Flatpak build at $files (run packaging/flatpak/build.sh first)" >&2; exit 1; }
rt=$(flatpak info --user --show-location org.freedesktop.Platform//25.08 2>/dev/null ||
     flatpak info --show-location org.freedesktop.Platform//25.08)/files
out=$(realpath -m "${1:-$UWP_BIN/appimage}")
app=$out/AppDir
tool=$UWP_BIN/tools/appimagetool
[ -x "$tool" ] || { mkdir -p "$(dirname "$tool")"
  curl -sfL -o "$tool" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
  chmod +x "$tool"; }

echo "STAGE Assembling the AppDir"
rm -rf "$app"; mkdir -p "$app/usr/lib/bundled"
cp -a "$files"/. "$app/usr"/
rm -rf "$app/usr"/{manifest.json,share/app-info,share/runtime,lib/debug}

echo "STAGE Adding Python"
py=$(basename "$(readlink -f "$rt/bin/python3")")
cp "$rt/bin/$py" "$app/usr/bin/python3"
ver=${py#python}
cp -a "$rt/lib/python$ver" "$app/usr/lib/"
rm -rf "$app/usr/lib/python$ver"/{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3,site-packages,config-*}
find "$app/usr/lib/python$ver" -name __pycache__ -prune -exec rm -rf {} +

echo "STAGE Bundling libraries"
elves=("$app/usr/bin/python3" "$app/usr/bin/7zz" "$app/usr/bin/wine-uwp-installer" "$app/usr/bin/wine" "$app/usr/bin/wineserver")
while IFS= read -r f; do elves+=("$f"); done < <(find "$app/usr/lib/wine/x86_64-unix" "$app/usr/lib/python$ver/lib-dynload" -name '*.so')
python3 "$here/bundle_libs.py" "$rt/lib/x86_64-linux-gnu" "$app/usr/lib/bundled" \
  --also libgnutls.so.30 --also libusb-1.0.so.0 --also libavcodec.so.61 --also libavformat.so.61 \
  --also libavutil.so.59 --also libgssapi_krb5.so.2 "${elves[@]}"

echo "STAGE Desktop integration"
install -m755 "$here/AppRun" "$app/AppRun"
install -m755 "$here/host-run" "$app/usr/bin/host-run"
ln -sf host-run "$app/usr/bin/xdg-open"
cp "$repo/packaging/flatpak/io.github.wine_uwp.WineUWP.svg" "$app/wine-uwp.svg"
ln -sf wine-uwp.svg "$app/.DirIcon"
sed -e 's/^Icon=.*/Icon=wine-uwp/' \
  "$repo/packaging/flatpak/io.github.wine_uwp.WineUWP.desktop" > "$app/wine-uwp.desktop"

echo "STAGE Packing"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream "$app" "$out/wine-uwp-x86_64.AppImage" >/dev/null
echo "appimage: $out/wine-uwp-x86_64.AppImage ($(du -h "$out/wine-uwp-x86_64.AppImage" | cut -f1))"

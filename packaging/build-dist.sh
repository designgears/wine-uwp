#!/usr/bin/env bash
# Assemble an installable wine-uwp tree from this checkout: our Wine build, installed, plus the
# scripts that set up and run apps. A product built on wine-uwp adds its own setup UI to it (bin/wine-uwp-installer;
# shapr3d-linux does), and `wine-uwp setup` runs that UI when there is one.
#
#   packaging/build-dist.sh OUT            everything
#   packaging/build-dist.sh --no-wine OUT  only share/wine-uwp and bin/wine-uwp (an SDK build compiles Wine itself)
#
# OUT/bin                   wine, wineserver, ... and the wine-uwp launcher
# OUT/lib/wine              Wine's modules
# OUT/share/wine-uwp        run.sh, install-overrides.sh, the .reg files, packaging/
#
# Nothing Microsoft's goes in: the Windows files are harvested on the user's machine by the setup.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
. "$here/env.sh"
with_wine=1
[ "$1" = --no-wine ] && { with_wine=; shift; }
out=$(realpath -m "$1")
rm -rf "$out"; mkdir -p "$out/bin"
if [ -n "$with_wine" ]; then
  [ -x "$WINE_BUILD/wine" ] || { echo "no Wine build at $WINE_BUILD" >&2; exit 1; }
  stage=$(mktemp -d "$(dirname "$out")/.wine-install.XXXXXX")
  trap 'rm -rf "$stage"' EXIT
  echo "STAGE Installing Wine"
  make -C "$WINE_BUILD" -j"$(nproc)" install DESTDIR="$stage" >/dev/null
  # the build was configured with some --prefix; take whatever directory holds bin/wine
  root=$(dirname "$(dirname "$(find "$stage" -path '*/bin/wine' -type f | head -1)")")
  cp -a "$root"/. "$out"/
  rm -rf "$out/include" "$out/share/man" "$out/share/applications"
  # the SDK side of Wine (compilers, import libraries) is not needed to run anything
  rm -f "$out"/bin/{function_grep.pl,widl,winebuild,winecpp,winedump,wineg++,winegcc,winemaker,wmc,wrc}
  find "$out/lib/wine" -name '*.a' -delete
  # debug info is most of the size; the build tree keeps it for crash analysis (AGENTS.md, minidump.py)
  if [ -z "${KEEP_DEBUG:-}" ]; then
    echo "STAGE Stripping"
    find "$out/lib/wine" -name '*.so' -exec strip --strip-debug {} +
    find "$out/lib/wine" -path '*-windows/*' -type f -exec x86_64-w64-mingw32-strip --strip-debug {} + 2>/dev/null || true
    strip --strip-debug "$out"/bin/wine "$out"/bin/wineserver 2>/dev/null || true
  fi
fi

echo "STAGE Installing wine-uwp"
share=$out/share/wine-uwp
mkdir -p "$share/packaging"
cp "$here"/{env.sh,run.sh,install-overrides.sh,launch-defaults.env,globalization.reg,wineuwp.reg,mrt-single-module.reg} "$share"/
cp -r "$here"/packaging/{harvest,new-prefix.sh,install-app.py,inhibit-idle.py,install-eac-runtime.sh,install-d3d-runtime.sh} "$share/packaging"/
find "$share" -name __pycache__ -prune -exec rm -rf {} +
install -m755 "$here/packaging/wine-uwp" "$out/bin/wine-uwp"
git -C "$here" rev-parse --short HEAD > "$share/VERSION"
echo "STAGE Done: $out ($(du -sh "$out" | cut -f1))"

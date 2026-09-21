#!/usr/bin/env bash
# Stage any installed UWP package out of a mounted Windows partition, the way stage.sh stages the
# calculator. The point is to run something other than the calculator without hand-editing run.sh:
# everything app-specific (identity, entry exe, window title) is read from the package manifest and
# written next to the staged files as identity.env, which run.sh sources.
#
# Usage: ./stage-app.sh <package-family-or-full-name> [stage-name]
#   e.g. ./stage-app.sh Microsoft.ZuneMusic media-player
# WINDOWSAPPS overrides where installed packages are read from (default: the /mnt/win mount).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]:-$0}")/./env.sh"
WA=${WINDOWSAPPS:-/mnt/win/Program Files/WindowsApps}
FWSRC=${UWP_BIN}/win11/Program\ Files/WindowsApps
[ -d "$WA" ] || { echo "no WindowsApps at $WA (mount the Windows partition first)" >&2; exit 1; }

pat=$1
src=$(ls -d "$WA"/${pat}_*_x64__* 2>/dev/null | head -1) || true
[ -n "${src:-}" ] || { echo "no x64 package matching $pat under $WA" >&2; exit 1; }
full=$(basename "$src")
name=${full%%_*}; pub=${full##*__}
stage=${2:-$(echo "${name##*.}" | tr 'A-Z' 'a-z')}
S=${UWP_BIN}/stage/$stage

# The manifest is UTF-16 or UTF-8 depending on the packaging tool; strip NULs and read it flat.
man=$(tr -d '\0' < "$src/AppxManifest.xml" | tr '>' '>\n')
exe=$(echo "$man" | grep -oE 'Executable="[^"]*"' | head -1 | sed 's/.*="//;s/"//' | tr '\\' '/')
appid=$(echo "$man" | grep -oE '<Application[^>]* Id="[^"]*"' | head -1 | sed 's/.*Id="//;s/"//')
title=$(echo "$man" | grep -oE 'DisplayName>[^<]*' | head -1 | sed 's/.*>//')
case "$title" in ms-resource:*|'') title=${name##*.};; esac

echo "package : $full"
echo "exe     : $exe   (app id $appid)"

rm -rf "$S"; mkdir -p "$S"
cp -r "$src"/. "$S"/ 2>/dev/null || true
chmod -R u+w "$S"

# Framework payload the package expects app-local, exactly as stage.sh does for the calculator.
cp "$FWSRC"/Microsoft.NET.Native.Runtime.2.2_*_x64__*/*.dll "$S"/ 2>/dev/null || true
cp "$FWSRC"/Microsoft.NET.Native.Framework.2.2_*_x64__*/SharedLibrary.dll "$S"/ 2>/dev/null || true
cp "$FWSRC"/Microsoft.VCLibs.140.00_*_x64__*/*.dll "$S"/ 2>/dev/null || true
cp ${UWP_BIN}/win11/Windows/System32/mrt100.dll "$S"/ 2>/dev/null || true

cat > "$S/identity.env" <<EOF
export WINE_UWP_PACKAGE_FULL_NAME=$full
export WINE_UWP_PACKAGE_FAMILY_NAME=${name}_$pub
# deliberately not written: run.sh computes the package path from the stage directory, so a moved
# or renamed tree keeps working. Baking it here is what broke every app once.
export WINE_UWP_AUMID=${name}_$pub!$appid
export WINE_UWP_WINDOW_TITLE='$title'
export UWP_EXE='$exe'
EOF
echo "staged  -> $S   (run: UWP_STAGE=$stage ./run.sh)"

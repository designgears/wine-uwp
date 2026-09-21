#!/usr/bin/env bash
# Export *our* Wine changes into this repo as patches.
#
# The Wine tree (~/src/wine) is shared with another session and is deliberately left uncommitted,
# and AGENTS.md says to commit only files in this repo. Taken literally that means several thousand
# lines of our own work live nowhere but one working tree - one `git checkout` away from gone, and
# invisible to anyone reading this repo. This bridges the two: nothing is committed in the Wine
# tree, but our diffs are committed *here*, where they belong.
#
# Only files this project owns are exported. The exclusions come straight from AGENTS.md's
# "files that are not yours" list plus what the other session is visibly working on; everything
# else in `git status` over there is theirs and must not be copied into our history.
set -euo pipefail

WINE=${WINE_SRC:-$HOME/src/wine}
OUT=$(cd "$(dirname "$0")/../../docs/LinuxPort/wine-patches" 2>/dev/null && pwd) \
  || OUT=$(dirname "$0")/../../docs/LinuxPort/wine-patches
mkdir -p "$OUT"

# Ours. Keep this list explicit: a glob would silently start exporting the other session's work
# the first time they touch a neighbouring file.
OURS=(
  dlls/wineuwp/main.c
  dlls/dcomp/composition.c
  dlls/dcomp/device.c
  dlls/windows.storage.applicationdata/applicationdata.c
  dlls/shell32/shellord.c
)

base=$(git -C "$WINE" rev-parse HEAD)
subject=$(git -C "$WINE" log -1 --format=%s)

rm -f "$OUT"/*.patch
total=0
for f in "${OURS[@]}"; do
  if ! git -C "$WINE" diff --quiet -- "$f" 2>/dev/null; then
    name=$(echo "$f" | tr '/' '-').patch
    git -C "$WINE" diff -- "$f" > "$OUT/$name"
    n=$(git -C "$WINE" diff --numstat -- "$f" | awk '{print $1"+/"$2"-"}')
    printf '  %-56s %s\n' "$f" "$n"
    total=$((total + 1))
  else
    printf '  %-56s (unchanged)\n' "$f"
  fi
done

cat > "$OUT/BASE.txt" <<EOF
Wine commit these patches apply to:
  $base
  $subject

Regenerate with: tools/wine-uwp/export-wine-patches.sh
Apply with:      cd ~/src/wine && git apply docs/LinuxPort/wine-patches/*.patch
EOF
echo "exported $total patch(es) to docs/LinuxPort/wine-patches, base $base"

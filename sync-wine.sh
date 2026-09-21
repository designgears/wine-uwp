#!/usr/bin/env bash
# Keep this project's Wine source in *this* repo, as real files.
#
# The Wine tree ($WINE_SRC, default ~/src/wine) is shared with another session and is not ours to
# commit in. That constraint used to mean our Wine work was exported as patches - which captured
# only the *uncommitted* delta, about 12% of it. The other 88% existed nowhere but that tree's
# commit history. This replaces that: the files we own are vendored here in full, at their real
# paths under wine/, so they are greppable, diffable and reviewable like any other source.
#
# wine/FILES is the authoritative, explicit list of what we own. Never make it a glob.
#
#   ./sync-wine.sh --check      diff repo against the tree; exit 1 if they differ  (default)
#   ./sync-wine.sh --from-tree  pull the tree's current contents into the repo (then commit)
#   ./sync-wine.sh --to-tree    push the repo's contents out into the tree
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]:-$0}")/env.sh"

REPO=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)
VENDOR=$REPO/wine
LIST=$VENDOR/FILES
MODE=${1:---check}

[ -f "$LIST" ] || { echo "no $LIST"; exit 2; }
[ -d "$WINE_SRC" ] || { echo "WINE_SRC=$WINE_SRC does not exist"; exit 2; }
mapfile -t FILES < <(grep -v '^\s*#' "$LIST" | grep -v '^\s*$')

n=0 diff=0 missing=0
for f in "${FILES[@]}"; do
  src=$WINE_SRC/$f; dst=$VENDOR/$f
  case $MODE in
    --from-tree)
      [ -f "$src" ] || { echo "  MISSING in tree: $f"; missing=$((missing+1)); continue; }
      mkdir -p "$(dirname "$dst")"
      cmp -s "$src" "$dst" || { cp -f "$src" "$dst"; n=$((n+1)); } ;;
    --to-tree)
      [ -f "$dst" ] || { echo "  MISSING in repo: $f"; missing=$((missing+1)); continue; }
      # Only write when the bytes actually differ: an unnecessary cp updates the mtime and makes
      # the shared tree rebuild, which is not ours to cause.
      mkdir -p "$(dirname "$src")"
      cmp -s "$dst" "$src" || { cp -f "$dst" "$src"; n=$((n+1)); } ;;
    --check)
      n=$((n+1))
      if [ ! -f "$src" ]; then echo "  only in repo: $f"; missing=$((missing+1))
      elif [ ! -f "$dst" ]; then echo "  only in tree: $f"; missing=$((missing+1))
      elif ! cmp -s "$src" "$dst"; then
        echo "  differs: $f ($(diff <(cat "$dst") <(cat "$src") | grep -c '^[<>]') lines)"; diff=$((diff+1))
      fi ;;
    *) echo "usage: $0 [--check|--from-tree|--to-tree]"; exit 2 ;;
  esac
done

# Record what upstream commit the vendored tree corresponds to, so the Wine side is reconstructible.
if [ "$MODE" = --from-tree ]; then
  {
    echo "Vendored from the shared Wine tree."
    echo
    echo "  wine HEAD:     $(git -C "$WINE_SRC" rev-parse HEAD)"
    echo "                 $(git -C "$WINE_SRC" log -1 --format=%s)"
    # The shared tree is a *shallow* clone, so origin/master is not fetched and merge-base fails.
    # Its single root commit is the upstream release it was cut from.
    base=$(git -C "$WINE_SRC" rev-list --max-parents=0 HEAD | head -1)
    echo "  upstream base: $base"
    echo "                 $(git -C "$WINE_SRC" log -1 --format='%s (%ad)' --date=short "$base")"
    echo "  captured:      $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "Files listed in wine/FILES are ours. Everything else in that tree - combase, ntdll,"
    echo "win32u, user32, kernelbase, server/, include/ - belongs to the other session."
  } > "$VENDOR/BASE.txt"
fi

case $MODE in
  --check) echo "checked $n file(s): $diff differing, $missing missing"; [ $((diff+missing)) -eq 0 ] || exit 1 ;;
  *) echo "$MODE: $n file(s) written, $missing missing" ;;
esac

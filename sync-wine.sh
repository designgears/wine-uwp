#!/usr/bin/env bash
# Keep this project's Wine source in *this* repo, as real files.
#
# The Wine checkout ($WINE_SRC, default ~/src/wine) lives outside this repo because it is a large
# build tree that predates it. Our work in it used to be exported as patches, which was wrong twice
# over: `git diff` sees only the *uncommitted* delta while most of the work was already committed in
# that tree, and a bogus "files that are not yours" list excluded another ~5,000 lines. Between them
# the export captured 2,700 of the ~49,000 lines this project has added to Wine. The files are now vendored here in full, at their real
# paths under wine/, so they are greppable, diffable and reviewable like any other source.
#
# wine/FILES is the authoritative, explicit list. Never make it a glob.
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
    echo "Vendored from the Wine checkout at \$WINE_SRC."
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
    echo "wine/FILES lists every file in that tree this project has changed. The only things left"
    echo "out are generated: autom4te.cache/, the generated configure, and *~ / *.orig / *.rej."
  } > "$VENDOR/BASE.txt"
fi

# A file changed in the tree but absent from wine/FILES is work that exists nowhere else: it is
# neither diffed nor copied, and --check used to pass anyway. That is how 41 committed lines of
# appxdeploymentclient/package.c and a dozen of the newer files went unvendored. Compare against
# the upstream base - committed, uncommitted and untracked changes alike.
unlisted=0
if [ "$MODE" = --check ]; then
  base=$(git -C "$WINE_SRC" rev-list --max-parents=0 HEAD | head -1)
  while read -r f; do
    case $f in configure|autom4te.cache/*|*~|*.orig|*.rej) continue ;; esac
    printf '%s\n' "${FILES[@]}" | grep -qxF "$f" && continue
    echo "  unlisted: $f"; unlisted=$((unlisted+1))
  done < <( { git -C "$WINE_SRC" diff --name-only "$base"; \
              git -C "$WINE_SRC" ls-files --others --exclude-standard; } | sort -u )
fi

case $MODE in
  --check) echo "checked $n file(s): $diff differing, $missing missing, $unlisted unlisted"
           [ $((diff+missing+unlisted)) -eq 0 ] || exit 1 ;;
  *) echo "$MODE: $n file(s) written, $missing missing" ;;
esac

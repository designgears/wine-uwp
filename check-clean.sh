#!/usr/bin/env bash
# Nothing of Microsoft's goes into this repository: no Windows DLLs or EXEs, no app packages or their payload, no
# Windows images, fonts or metadata. Those are harvested on the user's machine (packaging/harvest) or live in
# gitignored uwp-bin/. This refuses them.
#
#   ./check-clean.sh            the files staged for commit (githooks/pre-commit runs this)
#   ./check-clean.sh --all      every tracked file
#
# Refused: any PE image (MZ...PE), Windows/app formats by extension, fonts (unless listed in .clean-allow, one
# path glob per line), and anything over 5 MB. Enable the hook once per clone: git config core.hooksPath githooks
set -uo pipefail
cd "$(git rev-parse --show-toplevel)"
if [ "${1:-}" = --all ]; then list() { git ls-files -z; }; else list() { git diff --cached --name-only --diff-filter=ACMR -z; }; fi
allowed() { [ -f .clean-allow ] || return 1; local p; while IFS= read -r p; do [ -n "$p" ] && [[ $1 == $p ]] && return 0; done < .clean-allow; return 1; }
bad=0
while IFS= read -r -d '' f; do
  [ -f "$f" ] || continue
  why=
  case ${f,,} in
    *.dll|*.exe|*.sys|*.winmd|*.pri|*.mui|*.appx|*.appxbundle|*.msix|*.msixbundle|*.eappx|*.p7x|*.cab|*.wim|*.esd|*.iso|*.msu|*.pdb|*.xbf|*.efi) why="Windows/app file type" ;;
    *.ttf|*.ttc|*.otf|*.fon) allowed "$f" || why="font (list it in .clean-allow if its licence allows)" ;;
  esac
  if [ -z "$why" ] && [ "$(od -An -c -N2 "$f" 2>/dev/null | tr -d " ")" = MZ ]; then
    off=$(od -An -tu4 -j60 -N4 "$f" 2>/dev/null | tr -d ' ')
    [ -n "$off" ] && [ "$(od -An -c -j"$off" -N2 "$f" 2>/dev/null | tr -d ' ')" = PE ] && why="PE image"
  fi
  [ -z "$why" ] && [ "$(stat -c %s "$f")" -gt 5242880 ] && why="over 5 MB"
  [ -n "$why" ] && { echo "refused: $f ($why)" >&2; bad=1; }
done < <(list)
[ $bad = 0 ] || echo "check-clean: nothing of Microsoft's (or any large binary) belongs in git; see check-clean.sh" >&2
exit $bad

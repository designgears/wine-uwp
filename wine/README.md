# Our Wine source

The files in here are **the real thing**, not diffs: full copies of the Wine source files this
project owns, at their real paths, vendored out of the shared tree.

## Why they live here

The Wine tree (`$WINE_SRC`, default `~/src/wine`) is shared with another session and is not ours to
commit in. This used to be handled by exporting our changes as patches. That was wrong in a way that
took a while to notice: `git diff` only sees the **uncommitted** delta, and most of our Wine work had
already been *committed* in that tree. The patches captured 2,700 lines. Our actual Wine work is
**22,684 lines across 64 files** - so about 88% of it existed nowhere but a shared, uncommitted
working tree's reflog, invisible to anyone reading this repo.

Two of the biggest files do not exist upstream at all - `dlls/dcomp/composition.c` (14,021 lines) and
`dlls/wineuwp/main.c` (3,793 lines) are wholly ours. Calling those "patches to Wine" was never
accurate; they are our source code that happens to build inside a Wine tree.

## Using it

```
./sync-wine.sh --check      # are repo and tree identical? (exit 1 if not)
./sync-wine.sh --from-tree  # tree -> repo, after doing work in the Wine tree; then commit here
./sync-wine.sh --to-tree    # repo -> tree, to reconstruct the tree from this repo
```

Both directions skip byte-identical files, so `--to-tree` will not churn mtimes and trigger a
rebuild in a tree that is not ours.

`FILES` is the authoritative list of what we own. It is **explicit and must never become a glob** -
a glob would start sweeping the other session's work into our history the first time they touched a
neighbouring file. Ownership follows AGENTS.md's "files that are not yours" list: `dlls/kernelbase`,
`dlls/ntdll`, `dlls/win32u`, `dlls/user32`, `dlls/combase`, `dlls/d3d11`, `dlls/windows.graphics`,
`dlls/wintypes`, `server/` and `include/` are theirs and are deliberately absent, along with the
~5,000 lines of changes in them.

`BASE.txt` records the Wine commit and the upstream release the vendored copies correspond to. Note
the shared tree is a *shallow* clone, so `origin/master` is not fetched and `merge-base` does not
work there; the root commit is the upstream release.

## Licensing

Files that modify existing Wine sources keep their original headers. Wine is LGPL-2.1-or-later and
these copies are redistributed under the same terms; the new files added by this project are
contributed on the same basis so they can go upstream unchanged.

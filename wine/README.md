# Our Wine source

The files in here are **the real thing**, not diffs: full copies of the Wine source files this
project owns, at their real paths, vendored out of the shared tree.

## Why they live here

The Wine checkout (`$WINE_SRC`, default `~/src/wine`) is a large build tree that predates this repo
and stays outside it. The source we write in it belongs here.

This used to be handled by exporting patches, which was wrong twice over:

- `git diff` only sees the **uncommitted** delta, and most of this project's Wine work had already
  been *committed* in that tree.
- The exporter's file list came from a claim in AGENTS.md that the tree was "shared with another
  session" and that `combase`, `ntdll`, `win32u`, `user32`, `kernelbase`, `server/` and `include/`
  were someone else's. **There is no other session** - all 116 commits are one author - so that
  excluded ~5,000 lines of our own work.

Between them the export captured 2,700 lines of the **49,264** this project has added to Wine
(against upstream Release 11.17, across 196 files, 29 of which do not exist upstream) - about 5%.

Two of the biggest files do not exist upstream at all - `dlls/dcomp/composition.c` (14,021 lines)
and `dlls/wineuwp/main.c` (3,793 lines) are wholly ours. Calling those "patches to Wine" was never
accurate; they are our source code that happens to build inside a Wine tree.

## Using it

```
./sync-wine.sh --check      # are repo and tree identical? (exit 1 if not)
./sync-wine.sh --from-tree  # tree -> repo, after doing work in the Wine tree; then commit here
./sync-wine.sh --to-tree    # repo -> tree, to reconstruct the tree from this repo
```

Both directions skip byte-identical files, so `--to-tree` will not churn mtimes and trigger a
rebuild in a tree that is not ours.

`FILES` is the authoritative list, and is **explicit rather than a glob** so that adding a file to
the repo stays a deliberate act. The only things left out are generated or editor-backup files:
`autom4te.cache/`, the generated `configure` (regenerate it with `autoreconf`; `configure.ac` *is*
vendored), and `*~` / `*.orig` / `*.rej`.

`BASE.txt` records the Wine commit and the upstream release the vendored copies correspond to. Note
the shared tree is a *shallow* clone, so `origin/master` is not fetched and `merge-base` does not
work there; the root commit is the upstream release.

## Licensing

Files that modify existing Wine sources keep their original headers. Wine is LGPL-2.1-or-later and
these copies are redistributed under the same terms; the new files added by this project are
contributed on the same basis so they can go upstream unchanged.

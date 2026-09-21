# wine-uwp

Running packaged UWP apps, with no source and no symbols, under a patched Wine. The target is
**Shapr3D**; the apps here are test subjects chosen because they fail in the ways Shapr3D will.

| App | State |
| --- | --- |
| Calculator | Works. The byte-identical regression baseline |
| Feedback Hub | Works, interactive |
| Media Player | Plays audio from its own UI |
| Camera | Renders, paints its own crash page |
| Solitaire | Renders its splash screen; does not reach the game |
| Windows Security | Blank |

Start with **[AGENTS.md](AGENTS.md)** - it is the accumulated "how to investigate this, and the
mistakes that already cost a day". `docs/Findings-M*.md` is the full log, including the approaches
that did not work, which is most of them.

## Running something

```sh
DISPLAY=:98 ./run.sh                              # Calculator
DISPLAY=:98 UWP_STAGE=microsoftsolitairecollection ./run.sh
```

Always on Xvfb `:98`, never a real display. `env.sh` resolves the paths; override `UWP_BIN`,
`WINEPREFIX`, `WINE_SRC` or `WINE_BIN_DIR` if your layout differs.

## What is not in here

The Wine *build* (`~/src/wine`, `~/src/wine-build`) is shared with another workstream and is not
part of this repo. Our Wine **source is** - `wine/` holds the 120 files this project owns, in full,
at their real paths (~22,700 lines; `dlls/dcomp/composition.c` and `dlls/wineuwp/main.c` do not
exist upstream at all). `sync-wine.sh` moves them in either direction and `wine/BASE.txt` records
the upstream release they sit on. **You need a Wine tree at that base, plus `./sync-wine.sh
--to-tree`, to reproduce the environment.** The other workstream's changes - combase, ntdll,
win32u, user32, kernelbase, server/, include/ - are deliberately not here.

`uwp-bin/` (staged apps, borrowed Windows DLLs, PDBs) and `prefix/` (the live Wine prefix) live in
this directory but are gitignored - about 4.8G of data that is not source.

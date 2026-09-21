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

The Wine tree itself (`~/src/wine`, `~/src/wine-build`) is shared with another workstream and is
not part of this repo. Our changes to it are exported as patches in `wine-patches/`
(`export-wine-patches.sh` regenerates them; `wine-patches/BASE.txt` records the commit they apply
to). **You need that tree at the recorded base, plus these patches, to reproduce the environment.**

`uwp-bin/` (staged apps, borrowed Windows DLLs, PDBs) and `prefix/` (the live Wine prefix) live in
this directory but are gitignored - about 4.8G of data that is not source.

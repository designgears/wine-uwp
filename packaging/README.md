# Packaging wine-uwp

The runtime that turns a Wine build plus this repository into something that installs and runs a packaged UWP app,
with nothing of Microsoft's in it. The Windows components come from Microsoft's own Windows 11 evaluation image,
which is downloaded and harvested on the user's machine.

A product on top of this (a setup UI, a Flatpak, an AppImage, branding) lives in its own repository.
[shapr3d-linux](../../shapr3d-linux) is the one for Shapr3D. It builds against a wine-uwp checkout.

## Pieces

| File | What |
| --- | --- |
| `harvest/download_iso.py` | Microsoft's evaluation ISO, resumable, checked against the pinned SHA-256 of the tested build |
| `harvest/harvest.py`, `harvest/manifest.txt` | extracts the listed files (and framework package directories) with 7-Zip, no root |
| `harvest/resolve_symbols.py` | PDB publics from `msdl.microsoft.com` for the DLLs actually harvested |
| `new-prefix.sh` | a working prefix from a harvested tree |
| `install-app.py` | an `.msixbundle`/`.msix` into `stage/<app>`, with frameworks, `identity.env`, `launch.env`, icon, URI schemes; `--info` previews one |
| `../shims/launch/wineuwp-launch.cpp` | `wineuwp-launch.exe`: starts an installed app from a plain `wine` command |
| `make-test-bundle.py` | rebuilds an `.msixbundle` from a staged package, to test installs without the real download |
| `wine-uwp` | the command: `setup`, `run`, `install`, `harvest`, `new-prefix`, `activate URI`, `bottles` |
| `build-dist.sh` | an installed tree from this checkout (host Wine build + everything above) |
| `bottles/` | `install-in-bottles.sh` and the bottle recipe, used by `wine-uwp bottles` |

All user data lives in one directory: `$WINE_UWP_DATA`, by default `$XDG_DATA_HOME/wine-uwp`. It holds `windows/`
(the harvested components), `prefix/`, `stage/` (apps and frameworks), `logs/` and, while downloading, `iso/`.

## Setting up from the command line

```
packaging/build-dist.sh OUT          # OUT/bin/wine-uwp, Wine, the scripts and shims
OUT/bin/wine-uwp harvest Windows.iso # or download one: packaging/harvest/download_iso.py OUT.iso
OUT/bin/wine-uwp new-prefix
OUT/bin/wine-uwp install App.msixbundle
OUT/bin/wine-uwp run
```

`wine-uwp setup` runs a product's setup UI (`bin/wine-uwp-installer`) when the tree has one, and otherwise prints
these steps.

## Bottles

With a runner made from a product's Flatpak build (`bottles/make-runner.sh` in shapr3d-linux), unpack it anywhere,
prepare the Windows components once (`bin/wine-uwp harvest ISO`), then:

```
bin/wine-uwp bottles App.msixbundle [BOTTLE-NAME]
```

The script:

1. copies the runner into Bottles' `runners/`;
2. creates the bottle with `bottles-cli` from `bottles/wine-uwp-environment.yml`, which keeps DXVK and VKD3D off -
   they would replace the Direct3D and DXGI that wine-uwp's composition needs;
3. copies the Windows components into the bottle, because Bottles' sandbox sees nothing outside its own data;
4. builds the prefix and installs the app inside that sandbox, so one wineserver drives the bottle;
5. registers the app's `wineuwp-launch.exe` as the bottle's program.

## Keeping Microsoft's files out

`../check-clean.sh` refuses PE images, app packages and payload, Windows images, metadata and fonts, and anything
over 5 MB. `githooks/pre-commit` runs it; enable it once per clone with `git config core.hooksPath githooks`.

# Packaging wine-uwp

Two ways to hand wine-uwp to someone: a **Flatpak** with a setup wizard, and a **Bottles runner** with a script that
sets up a bottle. Both carry the same pieces and neither contains anything of Microsoft's. The Windows components
come from Microsoft's own Windows 11 evaluation image, which the setup downloads on the user's machine.

## What the user sees

The wizard (`setup/wine_uwp_setup.py`, GTK 4 + libadwaita; screenshots in `docs/screenshots/setup/`) walks through:

1. **Welcome**: what the three steps are.
2. **Windows components**: download from Microsoft (7.1 GB, resumable), or use an ISO already at hand. It checks free
   space and can keep or delete the image afterwards.
3. **Preparing**:
   - download and SHA-256 check;
   - extraction of about 2,700 files, plus the five framework packages Shapr3D declares;
   - the Wine prefix, component registration, and Microsoft's public symbols for the private addresses `wineuwp`
     looks up.

   Each step shows its state, with a progress bar and the tools' full output under Details.
4. **Package**: the user picks Shapr3D's `.msixbundle`. The wizard shows the app's icon, version, publisher and
   whether every framework it needs is present.
5. **Installing**, then **ready**: Launch, and (outside the Flatpak) a menu entry plus the `shapr3d:` link handler,
   which is how the browser sign-in comes back to the app.

Every finished step leaves a marker, so a closed or failed setup resumes where it stopped.

## Pieces

| File | What |
| --- | --- |
| `harvest/download_iso.py` | Microsoft's evaluation ISO, resumable, checked against the pinned SHA-256 of the tested build |
| `harvest/harvest.py`, `harvest/manifest.txt` | extracts the listed files (and framework package directories) with 7-Zip, no root |
| `harvest/resolve_symbols.py` | PDB publics from `msdl.microsoft.com` for the DLLs actually harvested |
| `new-prefix.sh` | a working prefix from a harvested tree |
| `install-app.py` | an `.msixbundle`/`.msix` into `stage/<app>`, with frameworks, `identity.env`, `launch.env`, icon, URI schemes; `--info` previews one |
| `../shims/launch/wineuwp-launch.cpp` | `wineuwp-launch.exe`: starts an installed app from a plain `wine` command |
| `wine-uwp` | the command: `setup`, `run`, `install`, `harvest`, `new-prefix`, `activate URI`, `bottles` |
| `build-dist.sh` | an installed tree from this checkout (host Wine build + everything above) |
| `flatpak/` | the manifest (`io.github.wine_uwp.WineUWP`), desktop file, metainfo, icon, `build.sh` |
| `bottles/` | `make-runner.sh` (runner tarball from the Flatpak build), `install-in-bottles.sh`, the bottle recipe |

All user data lives in one directory: `$XDG_DATA_HOME/wine-uwp`. Inside the Flatpak that is
`~/.var/app/io.github.wine_uwp.WineUWP/data/wine-uwp`. It holds `windows/` (the harvested components), `prefix/`,
`stage/` (apps and frameworks), `logs/` and, while downloading, `iso/`.

## Building

```
./build.sh                                  # shims and wineuwp-launch.exe (clang-cl, xwin SDK)
packaging/flatpak/build.sh --install        # Flatpak: builds Wine 11.17 + wine/ in the GNOME 50 SDK
packaging/bottles/make-runner.sh            # uwp-bin/bottles/wine-uwp-11.17.tar.xz from that build
```

The Flatpak build needs `org.flatpak.Builder`, `org.gnome.Sdk//50` and
`org.freedesktop.Sdk.Extension.mingw-w64//25.08` from Flathub (user installation). Wine comes from the upstream
tarball with `wine/` laid over it and `configure` regenerated. Tarball plus overlay equals `~/src/wine`, checked
file by file on 09-29.

The shims are built on the host (they need the Windows SDK headers) and go in as files. The runner is the
Flatpak's `/app`, so it links against the GNOME runtime rather than the host's newer glibc, and runs inside Bottles'
sandbox.

## Bottles

Unpack the runner anywhere, prepare the Windows components once (`bin/wine-uwp setup` up to the package page, or
`bin/wine-uwp harvest ISO`), then:

```
bin/wine-uwp bottles Shapr3D.msixbundle [BOTTLE-NAME]
```

The script:

1. copies the runner into Bottles' `runners/`;
2. creates the bottle with `bottles-cli` from `bottles/wine-uwp-environment.yml`, which keeps DXVK and VKD3D off -
   they would replace the Direct3D and DXGI that wine-uwp's composition needs;
3. copies the Windows components into the bottle, because Bottles' sandbox sees nothing outside its own data;
4. builds the prefix and installs the app inside that sandbox, so one wineserver drives the bottle;
5. registers the app's `wineuwp-launch.exe` as the bottle's program.

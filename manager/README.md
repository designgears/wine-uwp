# wine-uwp-manager

A desktop app for everything installed under wine-uwp: the apps across data directories, their settings, installs,
logs and Xbox Live accounts. It goes through the tree's own tools - `run.sh`, `wineuwp-install`, `profiles/`,
`winexboxauth` - so it never has a second idea of how an app runs. Rust and Slint, as the Shapr3D installer.

    cargo build --release        # then target/release/wine-uwp-manager

It finds the wine-uwp tree from `WINE_UWP_ROOT`, its own location (the repository, or `share/wine-uwp` next to an
installed `bin/`), or the current directory, and resolves `env.sh` for the defaults.

- **Library**: every `stage/*/identity.env` in `uwp-bin/` and `uwp-bin/*/` (plus `datadirs=` in its config), with the
  package's icon. Play starts `run.sh` with the app's stage, prefix and Wine build; Stop runs `wineserver -k` in the
  app's prefix. Running apps are found by `WINE_UWP_PACKAGE_FULL_NAME` in `/proc`, so ones started elsewhere count.
- **Overview**: the prefix and Wine build per data directory, and `WINEDEBUG`/`DISPLAY` per app.
- **Environment**: the effective environment, layer by layer (identity, defaults, profile, `local.env`) with where each
  value came from, and an editor for the stage's `local.env`.
- **Logs**: each run started here logs to `~/.local/state/wine-uwp/logs/<stage>-<time>.log`.
- **Install**: `wineuwp-install` from a Store id, a file or a URL, with its progress; and recipes - profiles with
  `#! name:`, `#! install: <script>` and `#! datadir:` header lines, whose script gets DATADIR [VERSION].
- **Accounts**: per prefix, the account `winexboxauth` keeps and the title clients approved
  (`probe-xboxauth.exe status`), and an approval for a client an app's profile names (`WINE_UWP_XBOX_TITLE_APP`).

Its settings are `~/.config/wine-uwp/manager.conf`, `key=value`: `prefix:<datadir>`, `build:<datadir>`,
`winedebug:<stage>`, `display:<stage>`, `datadirs=<dir>:<dir>`.

# App profiles

`profiles/<PackageFamilyName>.env` holds what one app needs under wine-uwp, whatever the install. It is loaded
automatically:

- `wineuwp-launch --defaults FILE` (as `run.sh` starts apps) reads `profiles/` beside `FILE`, so the repository's
  copy is always current;
- `wineuwp-install` copies the profile into the stage as `profile.env`, which an installed app's own
  `wineuwp-launch.exe` reads.

Order: `identity.env`, the defaults (or `launch.env`), the profile, then the stage's `local.env`, which stays the
install's own last word.

The format is `launch.env`'s: one `NAME=value` per line, `#` comments, an optional `export `, values taken
literally (nothing is expanded). `NAME+=value` appends to the current value with a `;` - use it for
`WINEDLLOVERRIDES`, so a profile adds to the defaults' list rather than repeating it.

Keys the installer reads:

| Key | Effect |
| --- | --- |
| `WINE_UWP_NATIVE_D3D=1` | link DXVK (`dxgi`, `d3d11`) and vkd3d-proton (`d3d12`, `d3d12core`) beside each executable that imports them, as for GDK games (`packaging/install-d3d-runtime.sh --download` provides them) |

Everything else is an environment variable for the app; the ones profiles use:

| Key | Effect |
| --- | --- |
| `WINE_UWP_DXVK_COREWINDOW=1` | Wine's adapter lets DXVK create a swap chain for the app's CoreWindow |
| `WINE_UWP_XBOX_TITLE_APP`, `WINE_UWP_XBOX_TITLE_ID` | bind the app's Xbox Live tokens to this MSA client and title (`winexboxauth`); approve the client once with an interactive sign-in or `probe-xboxauth.exe clientdevice CLIENT` |

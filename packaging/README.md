# Packaging wine-uwp

The runtime that turns a Wine build plus this repository into something that installs and runs a packaged UWP app,
with nothing of Microsoft's in it. The Windows components come from Microsoft's own Windows 11 evaluation image,
which is downloaded and harvested on the user's machine.

A product on top of this (a setup UI, an AppImage, branding) lives in its own repository.
[shapr3d-linux](https://github.com/designgears/shapr3d-linux) is the one for Shapr3D. It builds against a wine-uwp checkout.

## Pieces

| File | What |
| --- | --- |
| `harvest/download_iso.py` | Microsoft's evaluation ISO, resumable, checked against the pinned SHA-256 of the tested build |
| `harvest/harvest.py`, `harvest/manifest.txt` | extracts the listed files (and framework package directories) with 7-Zip, no root |
| `harvest/resolve_symbols.py` | PDB publics from `msdl.microsoft.com` for the DLLs actually harvested |
| `new-prefix.sh` | a working prefix from a harvested tree |
| `install-app.py` | an `.msixbundle`/`.msix` into `stage/<app>`, with frameworks, `identity.env`, `launch.env`, icon, URI schemes; `--info` previews one |
| Wine's `programs/wineuwp-launch` | `wineuwp-launch.exe`: starts an installed app from a plain `wine` command; the installer copies it into each stage |
| `make-test-bundle.py` | rebuilds an `.msixbundle` from a staged package, to test installs without the real download |
| `inhibit-idle.py` | runs an app with the desktop kept from dimming, locking or suspending (D-Bus: portal, ScreenSaver, GNOME/MATE session) |
| `wine-uwp` | the command: `setup`, `run`, `install`, `harvest`, `new-prefix`, `activate URI` |
| `build-dist.sh` | an installed tree from this checkout (host Wine build + everything above) |

All user data lives in one directory: `$WINE_UWP_DATA`, by default `$XDG_DATA_HOME/wine-uwp`. It holds `windows/`
(the harvested components), `prefix/`, `stage/` (apps and frameworks), `logs/` and, while downloading, `iso/`.

## Setting up from the command line

```
packaging/build-dist.sh OUT          # OUT/bin/wine-uwp, Wine, the scripts and shims
OUT/bin/wine-uwp harvest Windows.iso # or download one: packaging/harvest/download_iso.py OUT.iso
OUT/bin/wine-uwp new-prefix
OUT/bin/wine-uwp install App.msixbundle
OUT/bin/wine-uwp install-store 9PCDNBHR11MR  # or a Store app by Store id: signs in, downloads, and opens an encrypted (EAppx) package with your licence (add --allow-older to take an older version when the newest one's licence is refused)
OUT/bin/wine-uwp run
```

`wine-uwp setup` runs a product's setup UI (`bin/wine-uwp-installer`) when the tree has one, and otherwise prints
these steps.

## Keeping Microsoft's files out

`../check-clean.sh` refuses PE images, app packages and payload, Windows images, metadata and fonts, and anything
over 5 MB. `githooks/pre-commit` runs it; enable it once per clone with `git config core.hooksPath githooks`.

## Optional runtime components

These recipes build separate components. They do not install DLLs or change a prefix.
Application-specific installation and launch settings belong in the consumer's integration project.
See [UWP API checks](../docs/UWP-API-checks.md) for probe coverage and validation limits.

### Push notifications

Run `packaging/build-push-notifications.sh WINE_BUILD WINE_SOURCE OUTPUT` against an already built x64 Wine 11.20 tree.
The default compiler is `x86_64-w64-mingw32-gcc`. Set `PE_CC` to override it.
The script downloads unchanged sources from
[WineGDK b53268302fd0b7479a4cb83d4c7719ba96eb28ad](https://github.com/BedrockNative/WineGDK/tree/b53268302fd0b7479a4cb83d4c7719ba96eb28ad).
It checks them against `packaging/winegdk-push.sha256` and retains the LGPL license in the source cache.
The output is `OUTPUT/wineuwp-push-notifications.dll`.

The component supplies PushNotificationChannelManager activation.
Its channel request completes asynchronously with WPN_E_PLATFORM_UNAVAILABLE and a null channel.
It does not supply a working notification channel.
The consumer reported a successful Linux build of the renamed module from `96f92cf`.
Its factory and async error checks passed through the generated Lutris/umu launcher in a fresh candidate prefix.
The tested artifact's SHA-256 and validation limits are in [UWP API checks](../docs/UWP-API-checks.md#integration-validation-november-6-2026).

To select the component in a test prefix:

1. Preserve the existing `DllPath` value for the following key:
   `HKLM\Software\Microsoft\WindowsRuntime\ActivatableClassId\Windows.Networking.PushNotifications.PushNotificationChannelManager`.
2. Set that value to the DLL's absolute Windows path.
3. Include `wineuwp-push-notifications=n` in the DLL overrides.
4. Restore the previous value after the temporary test.

Do not run the component's registration export. It also registers unrelated connectivity classes.
Build `probes/probe-push-notifications.c` with the generated header in `OUTPUT` and the Wine build/source include directories.

### CoreWindow graphics

Run `packaging/build-corewindow-graphics.sh OUTPUT` with a fresh output directory.
Despite the historical name, this now downloads the official DXVK 3.1.1 release,
verifies its pinned archive SHA256, and extracts it without modifying any files.
It requires curl, tar and sha256sum. It does not compile DXVK or vkd3d-proton.
The x64 DLLs are `OUTPUT/dxvk/x64/dxgi.dll` and `d3d11.dll`. The upstream release
includes its license. `SHA256SUMS` and `SOURCE-MANIFEST.txt` record the downloaded inputs.

For a native CoreWindow app, set `WINE_UWP_DXVK_COREWINDOW=1` alongside
`WINE_UWP_DRIVE_FIRST_VIEW=1`. Before returning CoreApplication, Wine-UWP installs
an adapter in the loaded DXGI factory's shared COM method table. The adapter obtains
the HWND through `ICoreWindowInterop` and forwards to DXVK's public
`CreateSwapChainForHwnd`. Other factory methods and COM object identities stay with
DXVK. No DXVK source, instructions or DLL files are modified. The broker and DXGI
remain loaded for the lifetime of the process. Without the opt-in, Wine-UWP does not
load DXGI or change its factory table.

This replaces `dxvk-corewindow.patch`. Installing stock DXVK without enabling the
Wine adapter still leaves `CreateSwapChainForCoreWindow` unimplemented. The adapter
covers the HWND presentation path, not a complete CoreWindow DXGI implementation;
for example, it does not add `IDXGISwapChain1::GetCoreWindow`.

Supply the application's vkd3d-proton runtime separately. The existing
`packaging/install-d3d-runtime.sh` accepts `WINE_UWP_DXVK=OUTPUT/dxvk` and
`WINE_UWP_VKD3D_PROTON=PATH`. Preserve a title's tested vkd3d-proton version.

`probes/probe-corewindow-swapchain.c` checks the real factory, adapter GetParent path,
references, null arguments, and dummy-composition ResizeBuffers1. Its header lists
the compile command and baseline/adapter invocations. Run it in a separate prefix
with stock DLLs beside the executable and native DXGI/D3D12 overrides.
`tests/test_dxgi_corewindow.py` separately checks forwarding and installation errors
under ASan/UBSan with mocked COM and Win32 calls. Host checks alone do not establish
Wine ABI or application compatibility.

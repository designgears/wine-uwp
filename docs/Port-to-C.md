# Porting wine-uwp to C

Decided on 2026-10-19: everything this project makes is written in C, the language Wine is written in. That covers the
runtime pieces first (the installer, then the WinRT stand-ins under `shims/`, currently C++), then the launch scripts,
then the investigation tooling. Each port replaces its original only when it does everything the original did, tested
the same way, and the original is then deleted in the same commit. Nothing is ported by wrapping the old code.

## Order

1. **Installer** (`packaging/install-app.py`, 1340 lines) -> C inside Wine. The first because it crosses a process
   boundary today: the Gaming Services stand-in runs `sh -c "python3 install-app.py ..."` and scrapes its log for
   progress, kills it by a reported PID to cancel, and gets only text for errors.
2. **Shims** (`shims/*`, ~30k lines of C++) -> C Wine DLLs in `~/src/wine/dlls/`, vendored by `sync-wine.sh` like the
   rest of our Wine source. Largest first by risk: gameplatform (installs, queue), xboxauth, gameruntime,
   appinstallmanager, then the small ones. `shims/common/storeservices.h` becomes a C module shared by them.
3. **Launch and setup scripts** (`run.sh`, `install-overrides.sh`, `stage-app.sh`, `packaging/*.sh`,
   `packaging/wine-uwp`) -> C programs (`programs/` in the Wine tree) or Wine's own setup (`wine.inf`, `wineboot`).
4. **Tooling** (`*.py`, `regress.sh`, ...) -> C tools, last.

## 1. The installer

### Shape

- `dlls/appxinstall/`: the installer as a library with a C API. Gaming Services and AppInstallManager call it
  directly, on their own thread, and get progress through a callback, pause and cancel through function calls, and
  failures as HRESULTs.
Test with `/tmp/cmp-install.sh PACKAGE` in the 10-19 session: both installers into empty data directories, then
`diff -r`. The test prefix needs a `wineboot -u` once so Wine finds the new DLL and program (it installs their stubs);
start the program by name (`wine wineuwp-install`), not by its build-tree path. Wine's zlib has no default allocators:
set `zalloc`/`zfree` or `inflateInit2` fails with `Z_STREAM_ERROR`.

- `programs/wineuwp-install/`: a thin command-line front end over the library with the options and stdout protocol
  `install-app.py` has today (`PID`, `STAGE`, `PROGRESS`, `INSTALLED`, `--info` JSON, `--latest`), for
  `packaging/wine-uwp` and the shapr3d-linux setup UI (`wine wineuwp-install.exe ...`) until those are ported too.

### Building blocks (what Wine has, what we add)

| Need | Source |
| --- | --- |
| HTTP range reads, retries with backoff | WinHTTP |
| zip (central directory, zip64, stored/deflate, CRC32) | ours; inflate from Wine's bundled zlib (`$(ZLIB_PE_LIBS)`, as cabinet and wininet use it) |
| raw DEFLATE (EAppx footers, bundle manifests) | zlib |
| XML (AppxManifest, AppxBundleManifest, AppxBlockMap, `.appinstaller`, GDK Layout) | xmllite |
| SHA-256 | bcrypt |
| AES-128-XTS (EAppx 512-byte units; XVD 4096-byte pages) | ours over bcrypt AES-ECB: Wine's bcrypt has no XTS (`bcrypt_main.c`, XTS_AES is an empty entry). Tested against IEEE 1619 vectors |
| file locks, staging `.new`/`.old`, publish by rename | LockFileEx, MoveFileEx |
| reflink of an installed copy (`seed_from_installed`) | CopyFile for now; a reflink-aware copy in Wine later |

### Formats, each with its test

| Path | Test package |
| --- | --- |
| plain `.appx`/`.msix` and bundles (zip) | Calculator bundle; `packaging/make-test-bundle.py` output |
| EAppx/EMsix (`EXPH`, `EXBH`), XTS 512 | an encrypted Store app |
| MSIXVC/XVD (GDK), XTS 4096, regions, SegmentMetadata | Bad Magpie, paused partial installs |

Every output file (`identity.env`, `launch.env`, `.wine-uwp-chunks.json`, `.wine-uwp-icon.*`,
`.wine-uwp-protocols`, `.wine-uwp-appinstaller`, frameworks under `stage/frameworks`) is compared byte for byte with
what `install-app.py` writes for the same package before the Python is deleted. `packaging/test-install-staging.py`'s
cases become tests of the C installer.

### Known quirks to keep or fix deliberately (from the line-by-line map)

- `children()` matches all descendants by local name, not direct children.
- the XVD mutable-data page count is read as one byte at 0x470: verify against a real package before porting.
- `UWP_EXE` is written unquoted in identity.env: quote it in the port (names with spaces).
- the stage name is validated after the final path is built: validate first.

## Status

| Piece | State |
| --- | --- |
| Installer | `dlls/appxinstall` + `programs/wineuwp-install` (10-19): plain packages, bundles and `.appinstaller` install byte for byte as `install-app.py` (Calculator .msix, 282 files; Gaming Services bundle, 729 files; only the data-directory path differs in launch.env). Not yet: EAppx, MSIXVC/GDK (`.wine-uwp-chunks.json`, native D3D DLL links), reuse of an installed copy (needs a reflink-aware copy in Wine), and the callers (gameplatform, appinstallmanager, packaging/wine-uwp) still run the Python |
| Shims | not started |
| Scripts | not started |
| Tooling | not started |

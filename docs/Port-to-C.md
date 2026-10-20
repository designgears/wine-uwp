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
- the XVD mutable-data page count at 0x470 **is** one byte: Bad Magpie has 1 there and 257 as a u32.
- `UWP_EXE` is written unquoted in identity.env: quote it in the port (names with spaces).
- the stage name is validated after the final path is built: validate first.

## Status

| Piece | State |
| --- | --- |
| Installer | `dlls/appxinstall` + `programs/wineuwp-install` (10-19/20). Every format `install-app.py` reads installs the same: plain packages, bundles and `.appinstaller` (Calculator .msix, 282 files; Gaming Services bundle, 729 files), GDK/XVD (Bad Magpie, 750 files, `.wine-uwp-chunks.json` identical; `--tags`/`--languages` region filters; native D3D/concrt140 placement and the game's `WINEDLLOVERRIDES`, checked on a synthetic package) and EAppx/EXBH (a synthetic encrypted package and bundle made with OpenSSL, including encrypted-then-compressed files; no real one on disk yet). Only the data-directory path differs in `launch.env`; `--info` output is byte-identical. Pause/resume and cancel are tested over HTTP. **`shims/gameplatform` calls `appx_install` directly** (no Python, no log scraping; progress by callback, pause/cancel by flags): the Xbox app installed Bad Magpie through it and the game starts (`docs/screenshots/xbox-install-c-installer-ready.png`, `badmagpie-c-installer-running.png`). `shims/appinstallmanager` is switched too: the Store updated itself 22401 -> 22609 through it (`docs/screenshots/msstore-c-installer-updated.png`). That needed a Unix side (`unixlib.c`): the running Store holds its stage directory open as its working directory, so a Windows rename of it is a sharing violation; `move_dir` falls back to a host `rename()`. The old copy (`NAME.old`) stays until the next install while the app has its DLLs mapped. Reuse of an installed copy of the same package works by reflink (the Unix side's `clone_tree`; a second Bad Magpie install reports `reusing the installed files` and matches a fresh one). `packaging/wine-uwp install|info|latest` run `wineuwp-install`. **`install-app.py` stays until `../shapr3d-linux`'s setup UI (`installer/src/main.rs`) calls `wine-uwp install|info|latest` instead of the Python**; then it and `test-install-staging.py` go |
| Shims | 10 of 32. `appmodelruntime` -> `kernelbase` `GetPackageFamilyNameFromToken` (which also retires `wineuwp`'s run-time import repair); `useractivitychannel` -> `dlls/windows.applicationmodel.useractivities` (new `windows.applicationmodel.useractivities.idl`); `toastnotifier` -> `dlls/windows.ui.notifications`. Not named `wpnapps`, where Windows registers it: a builtin of that name took over Windows' own wpnapps and its ScheduledToastNotification, and Solitaire stopped (3/3 bad until renamed). `probe-useractivity` and `probe-toast` GREEN, Solitaire 3/3. Before: 7 of 32. `advertisingid` -> `kernelbase` (`api-ms-win-appmodel-advertisingid-l1-1-0` in the schema; Solitaire passes 3/3), `dmprocessxml` -> `dlls/dmprocessxmlfiltered`, `integritypolicy` -> `dlls/windows.system.profile.integritypolicy` (not `windows.security.integrity`, which took over SmartAppControlPolicy and AppActivationPolicy from Windows' DLL; the Store 22609 reaches Home with it loaded, `docs/screenshots/msstore-22609-wine-integrity.png`), `gamingservice` -> `programs/wineuwp-gamingservice` (`net start GamingServices` runs it). `dmprocessxmlfiltered` was not yet loaded in that Store session (delay-loaded). Before: 3 of 32. `mrt100` is gone without a replacement: it raised the exception `mrt100_app.dll`'s delay-load `__except` expects, standing in for two Wine bugs now fixed in `ntdll`/`kernelbase` (the failure hook got the import's ordinal word instead of its name and faulted; a declining DLL hook returned NULL instead of reaching the system hook, which now raises Windows' `0x505`). Calculator starts with no `mrt100.dll` at all (`docs/screenshots/calculator-without-mrt100.png`). Regression on `prefix-test`/`wine-build-wow64`: calc AE 3924, fh blank, kb no Calculator - identical with the `ntdll` change reverted, so those belong to that prefix/build, not the change; mp and sol pass. `netisolation` and `devmgmtpolicy` were DLLs dropped into system32 under apiset names; they are Wine's `firewallapi` and `policymanager` now (the DLLs Windows maps those apisets to), with entries in `apisetschema.spec`. Shapr3D on `prefix-test` with the old files removed reaches its online Recents page with `firewallapi` loaded (`docs/screenshots/shapr3d-firewallapi-home.png`); `policymanager` resolves through the schema (`LoadLibrary`/`GetProcAddress` probe) but the ink stack did not load in a sketch session, so the app has not called it. A new Wine module is found only after a prefix update: touching the build's `loader/wine.inf` makes every prefix update itself once on its next start |
| Scripts | not started |
| Tooling | not started |

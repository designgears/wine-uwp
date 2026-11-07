# UWP API checks

These probes test reusable API behavior in Wine-UWP.
Run them against an isolated prefix and build.
The source changes use Wine 11.20 as their base.
Build recipes and DLL selection are in [packaging/README.md](../packaging/README.md#optional-runtime-components).

## Probe coverage

| Source under `probes/` | Checks |
| --- | --- |
| `probe-device-enumeration.c` | Device-kind display enumeration, driver property, Name, null output and an empty query |
| `probe-corewindow-swapchain.c` | CoreWindow HWND, reference lifetime, null arguments and dummy-composition ResizeBuffers1 |
| `probe-package-info.c` | Optional count in GetCurrentPackageInfo and GetCurrentPackageInfo2, with a synthetic package identity |
| `probe-known-folder.c` | LocalAppData with KF_FLAG_NO_APPCONTAINER_REDIRECTION matches the ordinary lookup |
| `probe-store-updates.c` | Completed empty update list, typed interfaces, iteration and E_BOUNDS |
| `probe-fileio.c` | Binary buffer roundtrip, byte writes, truncation, empty writes and read-only errors |
| `probe-basic-properties.c` | File and folder size and modification time against Win32 metadata |
| `probe-folder-picker.c` | Explicit folder override, invalid paths, no fallback and optional manual dialog cancellation |
| `probe-file-query.c` | Recursive name ordering, an empty directory, shallow enumeration and ranged results |
| `probe-controller-user.c` | Default-user identity, local authentication and E_POINTER with a connected controller |
| `probe-controller-factory.c` | Factory retention, public callbacks and lookup, plus STA/agile hotplug modes for apartment and re-entry checks |
| `probe-uwp-audio.c` | Audio selectors, endpoint enumeration, default-ID matching and WASAPI playback |
| `probe-push-notifications.c` | Factory activation, async error status, error code and null channel result |
| `probe-mediaplayer-empty-play.cpp` | Play/Pause/repeated Play with no source, a null source, an empty list and an optional file URI |

The storage and folder-picker probes share `probes/storage-probe.h`.
Each source retains its own assertions.
The controller-user probe and the factory probe's `--controller` mode return 77 when no controller is available.
That result is a skip, not a pass.
The MediaPlayer probe needs a file URI to run its fourth case.
These standalone probes are not CMake targets or distribution payloads.
Some require Wine-generated WinRT headers. Compile commands, where present, are in the source headers.

## Runtime contracts and limits

DeviceInformation's Device-kind query needs cfgmgr32 device records and the driver software-key lookup.
An interface-only query cannot supply those records.
Driver versions are Wine's Windows compatibility values, not Linux Mesa versions.
Non-XAML FrameworkView startup must receive SetWindow through SetWindowAndGetDispatcher.

Package query count outputs are optional, including during the initial size query.
Wine already returns the unredirected known folder, so accepting the no-redirection flag preserves that behavior.
Store update enumeration currently returns an empty typed collection for the local installation.
This does not implement Microsoft Store update discovery.

File queries retain their options before scheduling work.
An SRW lock protects acquisition and replacement of the options reference.
The existing async parameter owner releases the retained options.
This prevents ApplyNewQueryOptions from freeing options that an operation still uses.
StorageAccessCache remains process-local.

The controller-user getter uses Windows.System.User.GetDefault.
Hardware-ID registration retains each factory and dispatches matching providers through the public IGameController interface.
Wine's built-in controllers still require its private IGameControllerImpl initialization.
External controllers retain their own interface identity in callbacks and factory-controller lookup.
This implements registration and controller lifecycle callbacks. It does not add raw HID/XUSB report delivery or XUSB-type registration.
GIP registration and the controller-user getter remain unchanged.

An empty MediaPlayer accepts Play without starting playback.
The Windows reference implementation queues media-engine errors and returns S_OK.
Wine's [mfmediaengine test_Play](https://github.com/wine-mirror/wine/blob/30a8ee48ce207e29905eb6334ffe0279c5e6a89d/dlls/mfmediaengine/tests/mfmediaengine.c#L679) also expects S_OK without a source.
The local change affects only playback_play's no-path branch.

## Source provenance

The storage implementation adapts [Wine Ultimate f2613f31](https://github.com/ZhymabekRoman/wine-ultimate/blob/f2613f31efff40bfa9a11e938beb0c34101a6607/dlls/windows.storage/storage.c)
and [WineCoreUAP d7870edf](https://github.com/Luminoso-256/winefork/tree/d7870edf465403a1c172299ba82de8370d9012a8/dlls/windows.storage/WineCoreUAP).
The audio enumeration implementation adapts Appx Portable v2026.11.4.
Source comments retain attribution and licenses.

The ownership review compared these revisions:

- [Wine 30a8ee48](https://github.com/wine-mirror/wine/tree/30a8ee48ce207e29905eb6334ffe0279c5e6a89d/dlls/windows.storage) lacks this file-query implementation.
- [Valve Wine e8ca5806](https://github.com/ValveSoftware/wine/blob/e8ca5806eec488f5916e83d655ffc5349b6b1702/dlls/windows.gaming.input/manager.c) returns E_NOTIMPL for hardware-ID registration, as upstream Wine does.
- [WineGDK b5326830](https://github.com/BedrockNative/WineGDK/tree/b53268302fd0b7479a4cb83d4c7719ba96eb28ad) has the same registration stub and missing assertions in its platform probe.
  Its StorageFolder implementation does not replace this query implementation.
- WineCoreUAP assigns query options without retaining them or synchronizing replacement.
  Its different query and async implementation does not supply a directly reusable ownership fix.
- Wine Ultimate supplies the earlier buffer-write design, but not the query ownership fix.

These checked revisions did not supply a suitable replacement for the local ownership fix or the probe assertions.
This finding does not cover every published fork.

## Validation

The earlier isolated Linux build compiled both Wine architectures.
The x64 API probes passed before the subsequent ownership, registration and probe-assertion corrections.
Display enumeration also passed in x86.
The x86 FileIO probe could not activate its class in that prefix. Compilation does not establish x86 behavior.
The empty-player regression failed ten assertions before its runtime correction and passed afterward in x64.
Both architectures compiled that correction.
The native Windows playback DLL crashed during isolated object creation and was not a usable replacement.

Run `python3 tests/test_winrt_regressions.py` for the host checks.
The script extracts production C and compiles it with Clang, AddressSanitizer and UndefinedBehaviorSanitizer.
Mocks replace COM interfaces and Wine scheduling.
The storage check executes the callback prefix that reads query options.
The count check covers ownership during scheduling, not directory counting.
The optional `--ref REVISION` requires a revision with the current source paths.

The current source passes 42 host cases:

- Six replace options before or during worker execution across ranged, default and count queries.
- Three check cleanup when async creation fails.
- Four cover a valid push result and null manager, operation or information results.
- Sixteen check folder-picker routing, path rejection, unset behavior and reference cleanup.
- Thirteen check controller-factory registration and dispatch.
  They cover activation without a device, existing and later devices, duplicates, nonmatching IDs, failed creation and invalid public interfaces.
  They also check reference cleanup, public interface identity, callback re-entry and private initialization for Wine's built-in controllers.
  Two cases remove a provider during controller creation or its added callback.

Earlier source failed ten host checks, including three AddressSanitizer use-after-free reports.
Host checks do not compile complete Wine DLLs or establish Windows ABI compatibility.

### Integration validation, November 6, 2026

The consumer reported these results for `96f92cf15bddba920c9240407846b61bc99b6287`.
Its Wine source is unchanged from `70cd8cb6d857820736ff8964e3e04a428e7f13c8`.
The corrected windows.storage and windows.gaming.input compiled for x86_64 and i386 on Steam Deck.
Only the isolated candidate received those DLLs. The working installation remained unchanged.

The consumer compiled the generic storage, push-notification and controller-user probes from `96f92cf`.
The probes ran through the generated Lutris/umu launcher in a fresh candidate prefix.
The following checks passed:

- File and folder BasicProperties.
- Recursive, empty and ranged file queries.
- Binary writes, truncation and read-only errors.
- Factory activation and asynchronous error results from the renamed notification module.
- Controller-user association.

The first controller run returned SKIP 77 because no gamepad was available.
A second run passed with a neutral temporary uinput device.
The consumer reported no input injection or target application launch.
A separate hardware-token probe also passed. That result does not establish custom-controller factory support.

`packaging/build-push-notifications.sh` produced `wineuwp-push-notifications.dll` with this reported SHA-256:

```text
67109b15b43d28bdbfcf26cbdec9e633f58048a52e73f0f191fb408b462f95c2
```

This records the tested artifact, not a guaranteed checksum for every rebuild.
The consumer's integration project retains the runtime logs and launcher evidence.
The supplied report establishes compilation for both architectures, but does not establish x86 execution coverage for the probes.
The existing app regression suite and application-level validation remain pending.
API results do not establish application compatibility or durable storage.


### Hardware-factory registration correction, November 6, 2026

A consumer's isolated native-factory probe established a dependency on successful hardware-ID registration during activation.
Changing only windows.gaming.input changed the result from E_NOTIMPL with a null factory to S_OK with a nonnull factory.
The consumer retains that native-component evidence outside this repository.
The earlier host assertion checked only the chosen E_NOTIMPL fallback. It did not test activation compatibility.

The checked Wine, Valve Wine, WineGDK, WineCoreUAP and Wine Ultimate sources still contain the hardware-registration stub.
No checked implementation supplied a replacement.
The public [factory contract](https://learn.microsoft.com/en-us/uwp/api/windows.gaming.input.custom.icustomgamecontrollerfactory.creategamecontroller?view=winrt-26100)
returns an inspectable object.
The [controller lookup contract](https://learn.microsoft.com/en-us/uwp/api/windows.gaming.input.custom.gamecontrollerfactorymanager.trygetfactorycontrollerfromgamecontroller?view=winrt-26100)
requires custom controllers to implement IGameController.
Neither contract requires Wine's private initialization interface.

The correction retains registrations for the process lifetime and ignores duplicate registrations.
It creates one controller per matching provider and factory, including devices already present during registration.
Callbacks and lookup return the external object's public interface.
The manager releases its controller references after removal. Caller-held references can outlive removal.
Failed creation and invalid public interfaces produce no added callback.

The new host fixture uses extracted manager functions with mock COM objects and providers.
All 24 host cases pass with AddressSanitizer and UndefinedBehaviorSanitizer.
The E_NOTIMPL baseline fails ten controller cases. The earlier registration implementation fails nine.
Linux compilation and isolated execution subsequently passed, as recorded below.
Application validation of the corrected runtime remains pending.

In an isolated Wine source/build tree, copy the changed source and rebuild the module:

```sh
cp "$WINE_UWP/wine/dlls/windows.gaming.input/manager.c" "$WINE_SRC/dlls/windows.gaming.input/manager.c"
make -C "$WINE_BUILD" -j2 dlls/windows.gaming.input/all
x86_64-w64-mingw32-gcc -std=c11 -O2 -I"$WINE_BUILD/include" -I"$WINE_SRC/include" \
    "$WINE_UWP/probes/probe-controller-factory.c" -o "$OUT/probe-controller-factory.exe" \
    -lole32 -lruntimeobject -luuid
```

Use the corresponding build tree and compiler for i386.
Run the probe through the candidate launcher, first without arguments and then with `--controller`.
The second mode returns SKIP 77 if no controller is available.
It enumerates controllers and checks registration, public callbacks, duplicate handling and bidirectional lookup.
It neither reads nor injects controller input.
Hotplug, removal, failure cleanup and callback re-entry have host coverage only until Linux validation supplies those results.

### Hardware-factory Linux validation, November 6, 2026

The consumer reported these results for `4c5471b0bc0f358081729595ad6532c2d4853ead`.
The existing Wine 11.20 build compiled windows.gaming.input for x86_64 and i386.
The consumer also compiled `probe-controller-factory.exe` and ran both modes in a separate Linux API prefix.

```text
registration: PASS matching=0 created=0 added=0 private_queries=0 failures=0
--controller: PASS matching=1 created=1 added=1 removed=0 private_queries=0 failures=0
```

The matching-controller mode checks public callback identity and bidirectional lookup.
The consumer's native-component activation probe also returned S_OK with a nonnull factory.
The consumer retains the logs and native-component evidence outside this repository.

These are the reported DLL SHA-256 values:

| Architecture | SHA-256 |
| --- | --- |
| x86_64 | `919e5f4cbc050245ad315c8bf3353228ce0e73439ca87c52109410d1da2e2abc` |
| i386 | `a7090e2a10730b1286fcd3fed303e30ce0448f0c1436a65d9671de2c3a04ea15` |

The hashes identify the tested build artifacts. They do not guarantee identical output from every rebuild.
The report establishes compilation for both architectures, but does not identify the probe's execution architecture.
Removal did not occur during the reported probe run.
Hotplug, removal, failure cleanup and callback re-entry still have host coverage only.
At report time, the consumer's application still used the previous input DLL.
Application validation of the corrected DLL and the complete app regression suite remain pending.


### Explicit folder override, November 6, 2026

`WINE_UWP_PICKER_FOLDER` optionally supplies the folder returned by FolderPicker.PickSingleFolderAsync.
Set it to an existing absolute Windows folder path before the application calls the picker.
Drive paths such as `C:\Data\Exports` and UNC paths are accepted.
The value is literal. The picker does not expand environment variables or create directories.
The consumer chooses any default or previously selected custom location.
The override applies to every FolderPicker request in the process.

A valid override returns the selected StorageFolder even when `WINE_UWP_PICKER_NODIALOG` disables dialogs.
An unset override preserves the normal dialog, or a null result when NODIALOG disables it.
An invalid override returns a failed HRESULT with a null operation. It never falls back to a dialog or another folder.
Empty, relative and drive-relative values return ERROR_INVALID_NAME.
Values of 260 or more UTF-16 code units return ERROR_FILENAME_EXCED_RANGE instead of using truncated data.
Missing folders and files propagate the StorageFolder lookup error. A file returns ERROR_DIRECTORY.
StorageAccessCache remains process-local. This change does not persist access tokens or select a default directory.

The upstream Wine tree at [c42c1532](https://github.com/wine-mirror/wine/tree/c42c1532a8b8a767d5e23b527ea874368c202ed0)
has no storage-picker implementation to reuse.
The checked [WineGDK picker](https://github.com/BedrockNative/WineGDK/tree/b53268302fd0b7479a4cb83d4c7719ba96eb28ad/dlls/microsoft.windows.storage.pickers)
uses portal or shell dialogs. Its start-folder settings do not supply an automatic result.
The existing Wine-UWP file and save overrides provide the local pattern.
The folder override reuses StorageFolder lookup and the existing async-result helper.

All 40 host cases pass under AddressSanitizer and UndefinedBehaviorSanitizer.
The previous source at `d4358d3` fails 13 of the 16 new folder cases.
The three unset-behavior cases pass before and after the change.
These tests extract the real picker method but mock Win32, storage lookup and async creation.
They establish routing and cleanup, not filesystem behavior, Windows ABI compatibility or visible dialog behavior.
The Mac checkout has no Wine build tree or MinGW compiler.
Subsequent consumer compilation and API results appear below.
The full application regression suite remains pending.

Build in the consumer's isolated Wine source and build trees:

```sh
cp "$WINE_UWP/wine/dlls/windows.storage.pickers/main.c" "$WINE_SRC/dlls/windows.storage.pickers/main.c"
make -C "$WINE_BUILD" -j2 dlls/windows.storage.pickers/all
x86_64-w64-mingw32-gcc -std=c11 -O2 -Werror=implicit-function-declaration \
    -I"$WINE_BUILD/include" -I"$WINE_SRC/include" \
    "$WINE_UWP/probes/probe-folder-picker.c" -o "$OUT/probe-folder-picker.exe" \
    -lole32 -lruntimeobject -luuid
```

Use the corresponding build tree and compiler for i386.
Install the resulting DLL only in the isolated candidate runtime.
Run the probe through that candidate launcher with a 30-second timeout.
For a direct Wine build, use the isolated prefix explicitly:

```sh
LC_ALL=C.UTF-8 WINEPREFIX="$TEST_PREFIX" timeout 30s "$WINE_BUILD/wine" "$OUT/probe-folder-picker.exe"
```

The probe creates and removes temporary folder and file fixtures.
It checks a folder path with spaces and Unicode, missing paths, a file, relative paths, an oversized value and unset cancellation.
Invalid-path checks run with dialogs enabled. A timeout is a failure and can indicate an unintended dialog fallback.
The default run needs no interaction.
Use `--dialog-cancel` separately on a test display, then manually cancel the dialog to check the unset interactive path.
Do not count an unexecuted manual check as a pass.


### Review corrections and consumer validation, November 6, 2026

Independent review found that hardware factories crossed apartments as raw pointers.
The device-monitor thread also lacked COM initialization.
The candidate uses Wine's existing RoGetAgileReference API for factories and external controllers.
It resolves each interface in the calling apartment and initializes the monitor with RoInitialize.
The monitor balances that call with RoUninitialize when it exits.

The manager retains controller snapshots under its lock, then resolves interfaces and calls external objects after releasing the lock.
A reserved provider/factory pair prevents duplicate creation during callback re-entry.
Removal during creation suppresses unpublished callbacks.
Removal during an added callback defers the removed callback until the added callback returns.
The two helpers already exposed by cfgmgr32 and the shellpath flag handling remain unchanged.

All 42 host checks pass under AddressSanitizer and UndefinedBehaviorSanitizer.
The host tests assert that marshaling, factory calls and callbacks occur without the manager lock.
Their mock agile references check ownership only. They do not establish COM apartment behavior.

The public controller probe adds `--sta-hotplug VID PID` and `--agile-hotplug VID PID`.
The STA factory and controller deliberately do not expose IAgileObject.
The probe checks callback apartments, reentrant registration and lookup, COM activation inside callbacks, and lookup from a separate MTA thread.
It calls the returned controller's IsWireless property to exercise the resolved interface.
It does not read controller input or inject input.

Use a separate candidate runtime and prefix. Build both changed Wine files together:

```sh
cp "$WINE_UWP/wine/dlls/windows.gaming.input/manager.c" "$WINE_SRC/dlls/windows.gaming.input/manager.c"
cp "$WINE_UWP/wine/dlls/windows.gaming.input/main.c" "$WINE_SRC/dlls/windows.gaming.input/main.c"
make -C "$WINE_BUILD" -j2 dlls/windows.gaming.input/all
x86_64-w64-mingw32-gcc -std=c11 -O2 -Werror=implicit-function-declaration \
    -I"$WINE_BUILD/include" -I"$WINE_SRC/include" \
    "$WINE_UWP/probes/probe-controller-factory.c" -o "$OUT/probe-controller-factory.exe" \
    -lole32 -lruntimeobject -luuid
```

Repeat compilation with the i386 build tree and compiler.
The tested i686 SDK's runtimeobject import library lacks `RoGetAgileReference@16`.
Link `"$WINE_BUILD/dlls/combase/i386-windows/libcombase.a"` before `-lole32 -lruntimeobject -luuid` for that build.
This changes only the link command, not the probe source.
The probe uses `COWAIT_DEFAULT`: Wine 11.20 pumps classic STA COM messages with that value.
The initial probe did not compile because Wine 11.20's headers lack `COWAIT_DISPATCH_CALLS` and `COWAIT_DISPATCH_WINDOW_MESSAGES`.
Prepare the isolated prefix with the complete native Windows WinMetadata set in each architecture's system-directory view.
For a 64-bit prefix, i686 file access to System32 is redirected to SysWOW64.
Verify that both views contain the same complete metadata set before treating an STA probe failure as a manager regression.
The reduced metadata files shipped by Wine are insufficient for this probe. Copying only Windows.Gaming.winmd is also insufficient because its interfaces depend on other metadata.
Keep these Windows files in ignored runtime data, never in the source repository. Preserve and restore the original metadata when testing in a reused isolated prefix.
Run the existing registration and `--controller` modes first.
For each hotplug mode, select VID/PID values with no matching device initially connected.
Run the probe with a 100-second external timeout.
After READY_ADD, create a matching neutral test device. After READY_REMOVE, remove that device.
The probe prints CONTROLLER_APARTMENT_PASS only after its callback and lookup checks finish.
A timeout, marshaling error or missing callback is a failure.
See the reported execution results below. Fresh independent reviews of the final commit remain required.

The picker probe now includes wchar.h and checks path formatting and fixture setup errors.
The consumer compiled source SHA-256 `3ecc35cbdfb7432e9b2b703fad0e6db8175fca2dc63b82097102da0674f36896` for x86_64 and i686 with implicit declarations treated as errors.
The first runtime attempt failed during Unicode fixture creation, before any picker API case.
The consumer reported that LC_ALL=C.UTF-8 resolved that failure in the isolated Linux environment.
Use that UTF-8 locale for the probe. GetTempPathW's directory must already exist.

With the corrected probe and locale, the consumer reported passing x86_64 and i686 API runs against the `a0ba1e8` picker DLLs.
Both runs passed all printed cases, null-output and missing-directory assertions, and returned zero.
The baseline returned null override results and crashed on the null-output case.
The consumer restored its baseline DLLs after the test and retained the logs outside this repository.
Later diagnostic edits separate the ASCII fixture root from its Unicode child and escape path code units in failure output.
The consumer compiled and ran that final diagnostic source, SHA-256 `1a14606b80a4385cd13bbde3ecffef47844c9a082f0d7a9d2196f48f85c44e75`, successfully on x86_64 and i686 with `LC_ALL=C.UTF-8`.
Its x86_64 `--dialog-cancel` run opened the actual Browse for Folder dialog with overrides unset.
Escape returned S_OK with a null result, and the probe printed FOLDER_PICKER_PASS and exited zero.
Interactive cancellation has no i686 execution result.

### Controller apartment execution, November 6, 2026

These are consumer-reported isolated Linux results. The runtime DLLs were restored after testing.
No input was sent. Each successful hotplug run attached a matching neutral device only after READY_ADD and removed it after READY_REMOVE.
No virtual device remained afterward.

The tested source SHA-256 values are:

| Source | SHA-256 |
| --- | --- |
| `wine/dlls/windows.gaming.input/manager.c` | `269f38920b4e849dc6f0f7f3ba4539918afcd27f4bfcd9cf000255a685f3d87e` |
| `wine/dlls/windows.gaming.input/main.c`, initial runtime builds | `2990d787c741bcfa4716478158c6e2e0fcd693d6d8be3e43e31e930ba0560642` |
| `wine/dlls/windows.gaming.input/main.c`, final source | `6890438c0f3f96c483c4a0a068639b2fdbf00078c239a1da55195eaa7df22c85` |
| `probes/probe-controller-factory.c` | `931e39fea6c3e911ed8aa2683451502d181d8948458a570e630453f0ff9dd23b` |

Both Wine architectures compiled and linked. The unchanged probe linked for both architectures after the i686 import-library correction above.
The final main.c removes one blank line at EOF from the initially tested file, with no token changes. The consumer also compiled that final source for both architectures.
The live COM tests used the earlier main.c, not the final rebuilt DLLs. The manager and probe sources were unchanged.

| windows.gaming.input DLL | x86_64 SHA-256 | i686 SHA-256 |
| --- | --- | --- |
| Live-tested build, main.c `2990d787` | `09081089f270dc75d951830e0e7e37c528da387d5f8eeb5da0d0ecbed860e591` | `2afd364cf538f1302a4c7578583c8970cba4386a391303dc4b77202d64eb7c8e` |
| Final build, main.c `6890438c` | `3b9ac963026a9764bf861e755d28931af352b6616f7af448fde87c98f31b5f3d` | `fc25c8ec4780626a882251faa392c996c5740b70cb4316b583a5df76deb9a034` |

| Architecture | Factory mode | Result |
| --- | --- | --- |
| x86_64 | STA | PASS. Creation, added, IsWireless and removed callbacks ran on owner thread 36. |
| x86_64 | Agile | PASS. Factory callbacks ran on thread 308 and worker IsWireless on 340; owner was 36. |
| i686 | Agile | PASS. Callback and lookup threads were 316 and 348; owner was 252. |
| i686 | STA, original reduced metadata | FAIL before READY_ADD. RegisterCustomFactoryForHardwareId returned E_NOINTERFACE. No device was created. |
| i686 | STA, complete native metadata | PASS. Creation, added, IsWireless and removed callbacks ran on owner thread 248. |

Each passing run reported `created=1 added=1 removed=1 private_queries=0 failures=0` and exit zero.
The i686 STA log reported `StdMarshalImpl_MarshalInterface` failing to create an interface stub and `CoMarshalInterface` failing for `ICustomGameControllerFactory`, IID `{69a0ae5e-758e-4cbe-ace6-62155fe9126f}`, with `0x80004002`.
Neither architecture had an explicit registration for that IID in the inspected prefix registry.
The consumer's diagnostic found that x86_64 metadata lookup succeeded and CoGetPSClsid selected the metadata factory.
The i686 lookup failed, and CoGetPSClsid returned `0x80040155`.
The x86_64 process loaded 20 native metadata files with 14566 types and 8098 interfaces.
The i686 process loaded 10 reduced files with 253 types and 141 interfaces; its redirected SysWOW64 view lacked Windows.Gaming.winmd.
This establishes a metadata setup difference, rather than proving that the marshaler excludes x86.

Temporarily adding only Windows.Gaming.winmd to the isolated SysWOW64 view let the unchanged i686 STA probe reach READY_ADD and CreateGameController on owner thread 248.
It then failed to marshal IGameController, IID `{1baf6522-5f64-42c5-8267-b9fe2215bfbd}`, with E_NOTIMPL `0x80004001` during stub creation.
OnGameControllerAdded did not arrive, and the wait failed with `0x80010115`.
The consumer restored the metadata, device fixture and API runtime afterward.

Finally, the consumer temporarily supplied the complete 20-file native metadata set from System32 in the isolated SysWOW64 view.
The unchanged candidate and probe then passed i686 STA creation, added callback, MTA lookup, IsWireless and removal, with every STA callback on owner thread 248.
It reported `created=1 added=1 removed=1 private_queries=0 failures=0` and exit zero.
The metadata trace showed fallback generation for the factory, provider, GameController and InputSink interfaces despite unavailable registered proxy classes.
This isolates the two earlier failures to the incomplete metadata fixture: the first lacked the factory metadata; the second lacked dependencies after only the gaming file was supplied.
The consumer restored the original 10-file SysWOW64 metadata tree and API DLLs and removed the device fixture.
Its retained evidence includes `com-review-269f389/i686--sta-hotplug-complete-system-metadata.log` and `hotplug-results-i686-full-metadata.json`.
Both architectures now have passing actual STA and agile calls with the documented metadata prerequisite.
No marshaler or setup code changed to produce these results, and the restored reduced i686 fixture remains insufficient for STA execution.
The final independent reviews and complete application regression suite remain pending.

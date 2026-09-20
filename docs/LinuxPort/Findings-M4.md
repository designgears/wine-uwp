# Findings M4 — framework theme lookup unblocked

Continues Findings-M3.md, with the same Windows 10.0.26200.9168 binaries and Wine build.
This findings number is a debugging checkpoint, not completion of the composition milestone M4.

## Fixed blocker

Calculator now resolves the embedded WinUI theme
`Microsoft.UI.Xaml/Themes/21h1_themeresources.xbf` through the private MRT interfaces XAML uses.
It no longer reports `Cannot locate resource` for the framework theme.

The prior diagnosis that XAML dropped the authority was incorrect. `+iertutil` tracing shows
`IUriBuilder` preserves `Microsoft.UI.Xaml.2.8` when converting `ms-appx` to `ms-resource`.
XAML's filesystem lookup was a fallback after the private MRT lookup failed.

The process loaded **two physical copies of MrmCoreR.dll**:

- Private COM activation: `C:\windows\system32\mrmcorer.dll`.
- WinRT activation: `Z:\home\bobby\uwp-bin\win11\Windows\System32\MrmCoreR.dll`.

With both copies present, debugging the framework lookup caught a freed-memory read in
`CResourceManagerInternal::GetResourceIndex` (MRT RVA 0x1a1b6, profile pointer `feeefeeefeeefeee`).
Changing the five remaining extraction-path registrations to the prefix copy eliminated this
failure. The exact cross-copy ownership mechanism has not been established; do not claim it is
an authority parser or package-dependency defect.

`mrt-single-module.reg` now pins the resource manager COM class, resource context COM class,
and four WinRT classes to the prefix copy. `install-overrides.sh` imports it on every launch.
Both registry generators preserve that path for MRT so regenerating registrations does not
reintroduce the split. Other modules retain their existing paths.

## Validation

`tools/wine-uwp/build.sh` builds the extended `probe-mrt`. It follows XAML's pinned private ABI:
initialize manager, open framework map, open Files subtree, get named theme resource, clone the
resource context, and resolve a candidate. Each step returns S_OK and the candidate is non-null.
It also compares the module containing the private manager's QueryInterface with the module
containing the WinRT ResourceManager factory's QueryInterface.

Run:

```sh
WINEDEBUG=-all tools/wine-uwp/run.sh ~/uwp-bin/build-probes/probe-mrt.exe
```

Before adding the persistent repair, restoring the old WinRT ResourceManager registration made
this check exit 2 with `FAIL: private MRT and WinRT loaded different module copies`.
After the repair it exits 0 and prints `PASS: private MRT and WinRT share one module`.
The failure checks are intentionally tied to the pinned Windows build and staged WinUI package.

The real Calculator launch was also rechecked with a temporary external Windows debugger:
XAML opens the framework map and Files subtree, looks up the exact theme XBF, and resolves its
candidate, all with HRESULT 0 (twice). This is positive evidence of clearing the blocker, not
merely absence of its old error message. No Microsoft binaries were patched on disk; the
breakpoints were confined to the debugged process.

Local diagnostic logs: `/tmp/mrt-regression-red.log`, `/tmp/mrt-regression-green.log`,
`/tmp/mrt-final-launch.log`, `/tmp/mrt-final-debug.log`. These are temporary, not repo artifacts.

## Stopping point at the time (resolved, see below)

Calculator still does not render successfully. After theme resolution, it raises an access
violation at Windows.UI.Xaml RVA **0x1665ef**, the vtable AddRef lookup in a folded
`ctl::ComPtr<T>::InternalAddRef` implementation, then .NET Native fail-fasts with code **0x1007**.
Multiple template symbols share this address, so the nearest symbol does not identify the
object type. In `/tmp/mrt-final-debug.log`, the module base is `0x6ffff9780000` and the fault is
`0x6ffff98e65ef`.

Next step: capture the caller stack and object/vtable registers at that access violation to
identify which interface is invalid. Do not resume investigating package URI parsing: the
framework theme candidate is now proven to resolve inside Calculator itself.

## Duplicate module copies were a general defect, not an MRT one

The same split that broke MRT also loaded **two copies of `Windows.UI.Xaml.Controls.dll`**: the prefix
copy by bare name, and the extraction-path copy through the generated `ActivatableClassId` map. The
extraction copy was the only module unloaded in the whole run, and the access violation at
Windows.UI.Xaml RVA 0x1665ef was a `ctl::ComPtr<T>::InternalAddRef` on a vtable that had been in it.

The pinning is now derived instead of hand-written. `gen-single-module-reg.py` reads the generated
class map and rewrites `DllPath` to `C:\windows\system32\<name>` for every class whose DLL is
**byte-identical** between the prefix and the extraction directory. That criterion matters: for names
Wine also ships, `C:\windows\system32` holds Wine's own builtin, so redirecting there would silently
change which implementation serves the class. 977 classes across 13 modules are pinned.
`install-overrides.sh` regenerates and imports it on every launch, and `gen-classes-reg.py` applies the
same rule at the source so regeneration cannot reintroduce the split.

With that in place the module is loaded once, nothing is unloaded, and the 0x1665ef fault is gone.

## Other defects fixed while chasing the next fault

- **`msvcrt` did not export `__CxxFrameHandler4`.** `Windows.UI.dll` imports it from `msvcrt.dll`, so
  its C++ exception unwinding ran through Wine's unimplemented-function stub. Wine has the
  implementation in `dlls/msvcrt/handler4.c` but only compiles it for CRT 140+, so msvcrt now forwards
  the export to ucrtbase. `__C_specific_handler_noexcept`, which `CoreMessaging` imports, was missing
  the same way and now forwards to `__C_specific_handler`.
- **`EnumUILanguagesW` rejected `MUI_THREAD_LANGUAGES`.** Wine only accepted `MUI_LANGUAGE_ID` and
  `MUI_LANGUAGE_NAME` and returned `ERROR_INVALID_FLAGS` for anything else. `bcp47mrm` calls it with
  `MUI_LANGUAGE_NAME | MUI_THREAD_LANGUAGES` and throws a C++ exception when it fails. It now
  enumerates the thread's UI language list through `GetThreadPreferredUILanguages` for that flag.
  This removed all three C++ throws from the launch.
- **`OpenGlobalizationUserSettingsKey` had the wrong signature.** The native entry point takes
  `(access, reserved, out key)` with the output in the third argument, verified in `KernelBase.dll`
  (`mov rbx, r8` / `test r8, r8` at entry). Wine declared it `(name, access, key, reserved)`, so the
  `KEY_READ` mask was formatted as a string, which is what the `IsBadStringPtrW` access violations in
  the log were. It now opens `HKCU\Control Panel\International`.
- **`Control Panel\International\User Profile` did not exist** in the prefix. `globalization.reg`
  creates it with a `Languages` list, imported by `install-overrides.sh`.

## Root cause: RtlQueryPackageClaims overran the caller's stack

`rdi` was not clobbered by a callee failing to restore it. `DXamlCore::InitializeImpl` saves the
caller's `rdi` with `push rdi`, and something wrote the value 4 over that saved slot, so the epilogue
restored 4.

The writer was Wine's own `RtlQueryPackageClaims`. It filled the `PS_PKG_CLAIM` output as two
`ULONG64` values:

```c
pkg_claim[0] = PSM_ACTIVATION_TOKEN_PACKAGED_APPLICATION | PSM_ACTIVATION_TOKEN_DEVELOPMENT_APP;
pkg_claim[1] = PACKAGE_ORIGIN_DEVELOPER_UNSIGNED;   /* 4 */
```

`PS_PKG_CLAIM` is two **32-bit** fields, eight bytes in total. The second store therefore landed eight
bytes past the caller's structure, which is where the saved register sat. The fix is to write the two
`ULONG` fields, so the function touches exactly the eight bytes it owns.

This is worth stating plainly because the symptom pointed away from the cause: a stack slot in one
XAML frame was corrupted by an out-parameter overrun in a completely unrelated ntdll call made much
deeper, and the failure only surfaced as an access violation two calls later.

### How it was found

Guessing was exhausted after ruling out C++ unwinding, Wine builtin exports, the compact proxy tables
and duplicate modules. What found it was a hardware watchpoint armed from inside Wine:
`ScheduleCoreMessagingDispatch` in user32 reads `WINE_UWP_WATCH` and arms a write watchpoint on that
address for the calling thread, with a vectored handler that logs the faulting instruction and the
slot's new value and then continues. `WINE_UWP_BREAK` does the same with up to four execute
breakpoints at `windows.ui.xaml.dll` offsets, and `RtlIsMultiSessionSku` in ntdll carries the same
helper for other modules through `WINE_UWP_BREAK_MOD` and `WINE_UWP_BREAK_OFF`. Together they gave,
in two runs, first the exact call that returned with a bad register and then the exact instruction
that wrote the value. Both are env-gated debugging aids and should be removed before upstreaming.

## Further fixes after that

- **`ext-ms-win-mrmcorer-resmanager-l1-1-0` was unmapped.** `GetStringValueForManifestField` is
  exported by `MrmCoreR.dll`; the API set now points there.
- **`GetCurrentPackageInfo3` mishandled two package info types.** `KernelBase` treats 0x10 as a
  package-graph generation counter, clearing the count and reporting a four byte buffer, and 0x11 as
  package *aliases*, a different query entirely. Wine fell through to the package info path for both.
- **`GetPackageResourcesProperty` takes five arguments**, the fifth being an optional flags output,
  and it serves the localizable package properties. It now answers them from the manifest, so
  `ms-resource:` references reach the caller exactly as on Windows and are resolved through MRT.

## Cross-apartment marshalling (this is what fixed the wrong thread)

The view work ran on the wrong thread because nothing in the process
ever marshalled. `CoGetStdMarshalEx` was a Wine semi-stub that handed
back the **free-threaded** marshaler:

```
0290:CoMarshalInterface {638bb2db-…} (ICoreApplicationView), MSHLFLAGS_TABLESTRONG
0290:CoGetStdMarshalEx …: semi-stub, using the free-threaded marshaler
0290:StdGlobalInterfaceTable_RegisterInterfaceInGlobal Cookie is 0xf100
028c:StdGlobalInterfaceTable_GetInterfaceFromGlobal 0xf100 … ppv=00007FFFFE8375E0
```

The STA view thread registered the view in the GIT and the MTA
activation thread got the *same raw pointer* back, so every call ran
inline on the caller's thread. The apartments were right all along —
`0290`/`02ac` are STAs (`CoInitializeEx` model 6), the rest are the
MTA — only the marshaler was wrong.

`CoGetStdMarshalEx` now returns a real aggregatable inner object whose
`IMarshal` delegates to `CoGetStandardMarshal`
(`dlls/combase/marshal.c`). With it, `GetInterfaceFromGlobal` builds a
proxy, `CoGetPSClsid` resolves `{2e5500b6-…}` to
`OneCoreUAPCommonProxyStub.dll`, and calls are dispatched into the
owning apartment.

Two things had to be fixed for that to work:

- `StdProxy_Construct` read `ProxyInfo->pNamesArray[Index]`
  unconditionally. Compact OS proxy tables (TableVersion 0x10) carry no
  names, so this was a null dereference the moment a real proxy was
  built. Guarded, like the stub side already was.
- `wineuwp`'s `ICoreWindowFactory` was being proxied *into* the MTA, so
  `PrivateCreateCoreWindow` ran on an RPC worker thread. It has no
  thread affinity and must create the CoreWindow on the calling view
  thread, so it now aggregates a free-threaded marshaler and answers
  `IMarshal`/`IAgileObject`.

Two more user32 exports turned up once the view thread was doing the
work: `SetWindowServicesDestroyCallback` (ordinal 2536) and
`EnableResizeLayoutSynchronization` (ordinal 2615), both no-op TRUE.

## Through activation and into OnLaunched

With marshalling real, the app model runs the way it does on Windows and the failures moved into
the app itself. Each of these was found by breaking on the failing call, symbolising the caller,
and implementing what it actually wanted.

| Gap | Where | What it was |
| --- | --- | --- |
| `dataexchange.CoreDragDropManager.GetForCurrentView` | `dlls/dataexchange/main.c` | returned `E_NOTIMPL`, which failed `CJupiterWindow::SetCoreWindow`. Now finds the calling thread's `Windows.UI.Core.CoreWindow` with `EnumThreadWindows` and reuses `create_core_dragdrop_manager` |
| `user32.SetWindowServicesDestroyCallback` (2536), `EnableResizeLayoutSynchronization` (2615), `GetResizeDCompositionSynchronizationObject` (2614) | `dlls/user32/message.c` | no-op stubs |
| `user32.GetProcessUIContextInformation` (2521) | `dlls/user32/message.c` | reports the immersive context for a packaged process (`GetCurrentPackageId`), which is what `windows.storage` asks about |
| Classic in-proc CLSIDs | `tools/wine-uwp/gen-clsid-reg.py` | `WindowServerFactory` `{4d3bd4b7-…}` is the unmarshal class of a custom marshaler; without HKCR\CLSID the first cross-apartment CoreWindow call died in `get_unmarshaler_from_stream`. 857 classes registered |
| `combase.DllGetActivationFactory` | `dlls/combase/roapi.c` | `Windows.Foundation.Diagnostics.AsyncCausalityTracer` lives in combase; .NET asks for it the first time an app starts a `Task`. All-no-op, plus `IAgileObject` |
| `Windows.Storage.ApplicationDataContainer` | `dlls/windows.storage.applicationdata/applicationdata.c` | `LocalSettings` was `E_NOTIMPL`. Now a container whose `Values` is a `PropertySet` (process lifetime, not yet persisted) |
| `Windows.UI.UIContext` | `dlls/dcomp/device.c` | `WindowServer::get_UIContext` asks dcomp for it and fail-fasts if it is missing. Implemented with the private statics interface `{97473389-…}` (slot 7 makes one for a view) and `IUIContext` `{f21e14c1-…}` |
| `Windows.System.User`, `SplashScreen` | `dlls/wineuwp/main.c` | `args.User.NonRoamableId` and `args.SplashScreen` are dereferenced without a null check by `App.OnLaunched` |

Two things about the projection are worth remembering, because they cost the most time:

- **.NET builds the managed wrapper from `IInspectable::GetRuntimeClassName` of the object's
  canonical `IUnknown`.** The splash screen was originally another vtable on the args object;
  the projection resolved it back to the args' canonical IUnknown, read
  "LaunchActivatedEventArgs", and `args.SplashScreen` threw `InvalidCastException`. It is now a
  separate object. twinapi still QIs `ISplashScreen` on the args, so the args hand out that same
  object — deliberately not a view of themselves.
- **Objects wineuwp hands out have no thread affinity and must not be proxied.** The window
  factory, the launch args, the navigation client, the async action, the user and the splash
  screen all aggregate a free-threaded marshaler now. Without it the window factory ran
  `PrivateCreateCoreWindow` on an RPC worker thread, and the args turned every QI the app made
  into a marshalling failure.

The stand-in navigation client's per-slot return values are no longer guessed: they are read out
of CoreUIComponents' own vtable, where most entries are COMDAT-folded onto a bare `return S_OK`
or `return E_NOTIMPL`.

## Two debugging tools that made the rest possible

**Managed stack traces out of a .NET Native app.** `RoOriginateLanguageException` is handed the
managed exception object itself. QI it for `IStringable` and call `ToString` and you get the .NET
exception text and stack. That is how "Object reference not set to an instance of an object"
turned into `System.Runtime.InteropServices.McgMarshal.ThrowOnExternalCallFailed`, i.e. *a WinRT
call returned a failing HRESULT*, not a managed null at all. `WINE_UWP_TRACE_ORIGINATE=<hex>` or
`=all` enables it, in `dlls/combase/roerror.c`.

**Single-step to the instruction that produced an HRESULT.** The breakpoint helper gained
`WINE_UWP_BREAK_WATCH=<hex>`: on a breakpoint hit it sets the trap flag and steps until `rax`
holds that value (or the watched function returns, so each call is stepped on its own), then
prints the last 256 instruction addresses. `WINE_UWP_BREAK_STOPRVA=<rva>` stops at an address
instead. Without it, finding the origin of an `E_POINTER` five frames deep inside XAML meant one
build-and-run cycle per stack level; with it, one run gives the exact instruction and the path
into it.

## From OnLaunched to MainPage

Each of these was the next thing in the way, in order:

| Gap | Where | What it was |
| --- | --- | --- |
| `Windows.System.User` | `dlls/wineuwp/main.c` | `args.User.NonRoamableId` is the first line of `App.OnLaunched` |
| `SplashScreen` identity | `dlls/wineuwp/main.c` | see below |
| `AsyncCausalityTracer` | `dlls/combase/roapi.c` | asked for on the first `Task.Run` |
| `ApplicationDataContainer` | `dlls/windows.storage.applicationdata` | `LocalSettings` |
| `user32.GetProcessUIContextInformation` | `dlls/user32/message.c` | `windows.storage` asks whether the process is immersive |
| `Windows.UI.UIContext` | `dlls/dcomp/device.c` | `WindowServer::get_UIContext` fail-fasts without it |
| private `IDWriteTextAnalyzer` | `dlls/dwrite/analyzer.c` | see below |

**The splash screen taught the projection rule.** .NET builds the managed wrapper from
`IInspectable::GetRuntimeClassName` of an interface pointer's *canonical `IUnknown`*. The splash
screen started as another vtable on the launch args; the projection resolved it back to the args'
canonical IUnknown, read "LaunchActivatedEventArgs", and `args.SplashScreen` threw
`InvalidCastException`. It is a separate object now — and because twinapi separately QIs
`ISplashScreen` on the args, the args hand out that same separate object rather than a view of
themselves.

**The DWrite one is the interesting one.** `new Frame { FlowDirection = ... }` failed with
`E_POINTER`. FlowDirection is an inherited text-formatting property, so setting it runs
`CDependencyObject::EnsureTextFormatting` → `TextFormatting::CreateDefault` →
`CTextCore::GetFontAndScriptServices` → `DWriteFontAndScriptServices::Initialize`, which creates
an `IDWriteFactory6` (Wine has it), calls `CreateTextAnalyzer`, and then QIs the analyzer for
`{b7e6163e-7f46-43b4-84b3-e4e6249c365e}` — one digit off the public `IDWriteTextAnalyzer`, an
undocumented revision of it. Wine returned `E_NOINTERFACE`, which XAML turned into `E_POINTER`
several frames up. Wine's analyzer now answers that IID with the same object and carries twelve
extra vtable slots that report which one was called, so a method past `IDWriteTextAnalyzer2` shows
up in the log instead of jumping off the end of the table. Nothing has called one yet.

## Where it stands

Calculator now runs its own `OnLaunched` to completion and navigates:

- `args.User`, `args.SplashScreen`, `Application.Resources`, `SizeHelper`,
  `ApplicationView.GetForCurrentView()`, `SetPreferredMinSize`, `TryResizeView`,
  `ApplicationData.Current.LocalSettings` all work;
- `new Frame { FlowDirection = … }` works, and so does XAML's whole text-formatting
  initialisation;
- `Frame.Navigate(typeof(MainPage))` is reached and `Windows.UI.Xaml.Controls.Page` is created;
- no managed exception is thrown anywhere.

It then fail-fasts, deterministically, in `CCoreServices::GetCompositor` →
`WindowsGraphicsDeviceManager::EnsureDCompDevice`, which activates
**`Windows.UI.Composition.Compositor`**. That class lives in dcomp.dll and Wine returns
`REGDB_E_CLASSNOTREG`, so WinUI cannot get a compositor while building MainPage.

That is the whole remaining story for first pixels, and it is a project rather than a gap:

- The native dcomp.dll cannot be substituted. Its only two missing dependencies looked cheap
  (`IsDeveloperModeEnabled` from `api-ms-win-appmodel-unlock-l1-1-0`, and
  `Microsoft.Internal.WarpPal.dll`, which is 110 KB and was extracted), but WarpPal and dcomp
  together need about forty `NtDComposition*` / `NtFlipObject*` win32u syscalls — the kernel-side
  compositor.
- So `Windows.UI.Composition` has to be implemented in Wine. A `Compositor` that can hand out
  visuals, a `CompositionTarget` and brushes would let XAML build its tree; putting the result on
  screen is the step after that.

`probe-mrt` and `probe-avl` still pass.

Do not reopen D3D QI, MRT, or SDDL.

## First window on screen (2026-09-10)

`Windows.UI.Composition.Compositor` now exists in Wine (`dlls/dcomp/composition.c`) and the app
runs from activation to a mapped 1280x800 `Windows.UI.Core.CoreWindow` with no managed exception
and no fail-fast. The window is still blank: nothing is presented yet.

The chain that had to be closed, in order, each step found from a run rather than guessed:

1. **The compositor's private interfaces.** XAML reaches the compositor through
   `{22118adf-23f1-4801-bcfa-66cbf48cc51b}::CreateInteropDevice`, then QIs a fixed sequence of
   undocumented IIDs. Answering *every* IID with a probe object is wrong: WinUI then believes Mica
   is available and fails `BackdropMaterial.ApplyToRootOrPageBackground` with `E_XAMLPARSEFAILED`.
   Answering only the two device IIDs is also wrong - `GetCompositor` fail-fasts (`0xc0000602`).
   The list that works is the nine interfaces in `compositor_required_ifaces[]`, refusing
   `{0d8fb190-f122-5b8d-9fdd-543b0d8eb7f3}` so WinUI takes its no-backdrop fallback.
   `WINE_UWP_COMPOSITOR_PROBE=all` restores the answer-everything behaviour that found the list.

2. **The interop composition device is a real vtable, not a probe.** `{d14b6158-c3fa-4bce-9c1f-b61d8665eab0}`
   and `{e7894c70-af56-4f52-b382-4b3cd263dc6f}` are `IDCompositionDesktopDevicePartner` revisions;
   dcomp.dll maps both onto one flattened vtable (IDCompositionDevice2, then
   IDCompositionDesktopDevice, then partner extensions). The slot names were recovered from
   `InteropCompositor::Api`'s `s_InterfaceParts` table and vtables in `dcomp.pdb` - see the table in
   `composition.c`. Slot 6 is `CreateVisual`; returning `E_NOTIMPL` there becomes a
   `NullReferenceException` in .NET Native and kills `CalculatorApp.TitleBar`.

3. **dcomp ordinal 1019** is a forwarder to `win32u.NtCreateCompositionInputSink`, which Wine stubs.
   XAML calls it when it attaches hit-testing to a visual. It takes a 0xe8-byte descriptor and an
   out handle; a closable handle of our own is enough.

4. **`Windows.UI.Composition.CompositionCapabilities`** has to exist or WinUI throws while building
   any backdrop brush. IIDs and vtable order come from `CompositionCapabilities::Api` in dcomp.pdb.
   Effects are reported unsupported, which is what makes XAML take its plain-brush path.

5. **GIT registration of a WinRT delegate.** XAML registers
   `ITypedEventHandler<CoreWindow, WindowActivatedEventArgs>` `{18026348-8619-4c7b-b534-ced45d9de219}`
   in the global interface table. No proxy is registered for it anywhere on Windows - not in
   OneCoreUAPCommonProxyStub.dll, not in the registry - because Windows marshals parameterized
   WinRT types from metadata. Wine's GIT now (a) registers an object that answers `IAgileObject`
   directly, and (b) falls back to a direct reference when no proxy exists, since an in-process GIT
   entry has nothing to send over the wire. Wine's free-threaded marshaler also now answers
   `IAgileObject`, which is what aggregating it is for.

6. **Showing the window.** On Windows the shell maps an activated app's CoreWindow; nothing plays
   that role here, so `wineuwp` shows it itself once `Activate` returns (`show_core_window`).

## Through OnLaunched with no exception (2026-09-10)

`OnLaunched` used to throw a `KeyNotFoundException` that escaped into
`FrameworkApplicationGenerated::OnLaunchedProtected`, so `Window.Current.Activate()` never ran.
Four more gaps had to be closed, again each one found from a run:

7. **`Windows.Globalization.NumberFormatting.DecimalFormatter`** (new
   `dlls/windows.globalization/numberformatting.c`). `LocalizationSettings` builds one through
   `IDecimalFormatterFactory::CreateDecimalFormatter` and derives the locale's digits, decimal
   separator and grouping from it. The IIDs and the vtable order come from
   `WinMetadata\Windows.Globalization.winmd`; separators come from `GetLocaleInfoEx`, so the
   answers match the rest of Wine. Note `IActivationFactory` and `IDecimalFormatterFactory` need
   *separate* vtables - sharing one puts `CreateDecimalFormatter` at slot 7 while the caller calls
   slot 6, which silently writes the new object over the caller's first argument.

8. **`DisplayInformation.GetForCurrentView`** (new `dlls/windows.graphics/displayinformation.c`).
   A `Pivot` cannot be created without it: XAML asks the view for its dpi and orientation. It also
   has to answer `IWeakReferenceSource` - WinUI takes a weak reference and fail-fasts otherwise -
   so `weakref.c` was copied in from `dlls/geolocation`.

9. **`ApplicationDataContainerSettings` is not a plain PropertySet** (new
   `dlls/windows.storage.applicationdata/settings.c`). Looking up a missing setting *succeeds* and
   returns null on Windows, where a PropertySet fails with `E_BOUNDS`. Calculator's
   `ThemeHelper.InitializeAppTheme` reads `LocalSettings.Values["SelectedAppTheme"]` with a plain
   indexer on first launch, and the `E_BOUNDS` became the `KeyNotFoundException`. The settings are
   now a thin wrapper over the PropertySet that changes that one answer.

10. **`IDCompositionDeviceInternal` {6b556968-2d25-11e2-99f6-8bc36088709b}** is real, not a probe:
    `DCompTreeHost::UpdateAtlasHint` calls `HintSize`, and XAML calls `TelemetrySetApplicationId`
    with the AUMID. Answering it with the numbered probe table was actively harmful, because probe
    slots 3-5 are `GetIids`/`GetRuntimeClassName`/`GetTrustLevel` and would have written through
    non-pointer arguments. dcomp ordinal **2502** (`DCompositionCreateAnimationStats`) also had to
    exist; Wine's spec had no entry at all, so the delay-load abort killed the process.

The app now runs from activation to a mapped window and an idle message loop with **no managed
exception, no fail-fast and no XAML parse error**. `Window.Current.Activate()` is reached.

Where it stops now: XAML creates exactly one visual and never asks for a composition target, a
surface factory or a swap chain, so there is no presentation path and the window is blank white.
`dxgi` sees only `EnumAdapters`. That is the next thread to pull.

Known non-fatal gaps in the same run: `Windows.Globalization.CalendarIdentifiers`,
`Windows.System.Power.PowerManager` and `Microsoft.UI.Private.Controls.CBSPackageDetector` are not
implemented, and two WinRT delegates fall back to the GIT direct-reference path.

## DCompTreeHost has a compositor; it still has no target (2026-09-10)

`DCompTreeHost::EnsureDCompDevice` (Windows.UI.Xaml RVA 0x1c6674) is a separate path from
`WindowsGraphicsDeviceManager::EnsureDCompDevice` (the GetCompositor one). A run with `+dcomp`
shows it now completes:

- GetProcAddress(`dcomp.dll`, "DCompositionCreateDevice3") then ordinal **1038**
  (`DCompositionCreateTargetForHandle`); the function pointer is stored at host+0x1c0.
- QI of the interop device for `{e7894c70}` (partner device2, host+0x200).
- QI `{b403ca50-7f8c-4e83-985f-cc45060036d8}` — public `ICompositor` — into host+0x1c8.
  Wine already answered this; the QI succeeds. It is not in `compositor_required_ifaces[]`
  because `compositor_QueryInterface` handles it directly.
- QI `{7bd36c9a}` `ICompositorPrivate1` into host+0x190.
- `ComPtr::As` `ICompositor2` `{735081dc}` into host+0x1d0.
- QI `{48ea31ad}` / `{7a38b2bd}` into host+0x1d8 / 0x1e0.
- `ComPtr::As` `{9cbd9312}` (listed here as ICompositorPrivate4) into host+0x1e8 and
  `{509eab63}` (Private5) into host+0x1f0.
- QI `{25297d5c}` `ICompositorInterop` into host+0x1f8.
- Interop-device slots 35 and 32: `DisableD2DStatePreservation` and
  `EnableWhitePixelOptimization(TRUE)`.
- `As` `IDCompositionDeviceInternal` `{6b556968}` into host+0x198.

All of those QIs succeed. The window is still blank because nothing then creates a
composition target.

Two presentation paths exist, and this run takes neither far enough:

1. **WinRT visuals.** `DCompTreeHost::CreateContainerVisual` (0x1be42c) calls
   `ICompositor` slot 9 (`CreateContainerVisual`) on the pointer at host+0x1c8. Callers are
   `HWCompTreeNodeWinRT::CreateContentVisual` and the WinRT render walk. Those functions
   were not entered; ICompositor create methods were never invoked. They now return
   recording `ContainerVisual` / `SpriteVisual` / `CompositionColorBrush` /
   `CompositionTarget` / `CompositionPropertySet` objects (separate vtables, getters fill
   out-params, `IContainerVisual::get_Children` returns a `VisualCollection`) so that path
   will not `E_NOTIMPL` when it is.

2. **Handle / island target.** `DCompTreeHost::CreateTargetForHandle` (0x9fcca4) is
   `(HANDLE, IDCompositionTarget**)` and tail-calls the ordinal-1038 pointer from step 1.
   `CoreWindowIslandAdapter::CreateCompositionIsland` (0xa00a80) instead calls
   `ICompositor` slot 25, which is `CreateTargetForCurrentView`. Neither ran.
   `EnsureResources` (0x1c62b8, only caller
   `WindowsGraphicsDeviceManager::WaitForD3DDependentResourceCreation`) would call
   `ICompositorInterop::CreateGraphicsDevice` (slot 5, offset 0x28) into host+0x210.

`ICompositorInterop` was previously a probe. The probe vtable is IInspectable-shaped, so
slot 5 is `GetTrustLevel`, not `CreateGraphicsDevice` — the same class of bug as
`IDCompositionDeviceInternal`. It is now a real `IUnknown` vtable on the compositor
(`CreateCompositionSurfaceForHandle` / `CreateCompositionSurfaceForSwapChain` /
`CreateGraphicsDevice`). `CreateGraphicsDevice` hands back an
`ICompositionGraphicsDevice` whose `CreateDrawingSurface` fills its out-parameter.

Ordinal 1038 is no longer a Wine stub: `DCompositionCreateTargetForHandle` returns a
recording target. `DCompositionCreateSurfaceHandle` is a named export that hands out a
closable event handle.

Still only one interop `CreateVisual` per launch (from `DCompVisual::Create`, used by
`SetTargetHelper`). No `CreateTargetForHwnd`, no `CreateTargetForHandle`, no
`CreateTargetForCurrentView`, no `CreateContainerVisual`. Next is whoever is supposed to
attach the CoreWindow to a target — `EnsureCompositionIslandCreated` /
`ICompositorWithCompositionIslands` / `CreateTargetForHandle` with a surface handle from
the CoreWindow.

## OneCore transform mode was why there was no island (same day)

Wine's `user32.IsOneCoreTransformMode` always returned FALSE. XAML caches that in
`XamlOneCoreTransforms::s_enabled` (`EnsureInitialized` at RVA 0x3c5690). With the bit
off:

- `DCompTreeHost::EnsureDCompDevice` takes the classic HWND branch (slots 32/35) and
  never calls `CreateVisualTreeCompositionIslandAdapter` (0x9fcd04, the call at
  EnsureDCompDevice+0x3de).
- `CJupiterWindow::SetCoreWindow` never calls `EnsureCompositionIslandCreated`.

Packaged UWP CoreWindow composition goes through islands, not `CreateTargetForHwnd`.
`IsOneCoreTransformMode` now returns TRUE when `WINE_UWP_PACKAGE_FULL_NAME` is set.

That path runs: `CreateVisualTreeCompositionIslandAdapter` constructs
`CoreWindowIslandAdapter`, which QIs CoreWindow for `{a7ff5b48-94e0-40e6-8a80-228482b688ba}`
(`ICoreWindow_CompositionIslands`) and calls `ICompositor::CreateTargetForCurrentView`
(slot 25). That now returns a recording `ICompositionTarget`.

It then fail-fasts with `RoFailFastWithErrorContextInternal2(0x80070490)` —
`HRESULT_FROM_WIN32(ERROR_NOT_FOUND)` — at `CreateCompositionIsland+0xd6` (RVA 0xa00b56):
slot 6 of the CoreWindow island interface returned NOT_FOUND. Native `windows.ui.dll`
implements that interface (`CompositionIslandBridge`); the method is looking for a
composition-island object our compositor does not yet provide
(`ICompositorWithCompositionIslands` / `ICompositionIslandStatics` /
`ICompositionIslandEnvironment`). That is the next probe.

`WINEDEBUG=+dcomp` log: `/tmp/wine-uwp-island.log`.

## CoreWindow island slot 6 is get_CompositionIsland (same day)

`ICoreWindow_CompositionIslands` `{a7ff5b48-94e0-40e6-8a80-228482b688ba}` vtable in
`windows.ui.dll` (`WindowServer`):

| slot | method |
| --- | --- |
| 6 | `get_CompositionIsland` → `IVisualTreeIsland**` |
| 7 | `get_CompositionIslandEnvironment` |
| 8 | `ConfigureCompositionIsland(ICompositor*, IVisualTreeIsland**)` |
| 9 | `DestroyCompositionIsland` |

`get_CompositionIsland` returns `0x80070490` when `WindowServer+0xc08` is still null —
the island was never configured. Native `ICompositor::CreateTargetForCurrentView`
configures the current CoreWindow's island as a side effect; our recording target did
not. XAML then get_CompositionIsland and fail-fasts.

`CreateTargetForCurrentView` now: `ICoreWindowStatic::GetForCurrentThread`, QI the
islands iface, call slot 8 `ConfigureCompositionIsland`. That runs
`CompositionIslandBridge::InitializeCompositor`, which:

1. `As` `ICompositorWithCompositionIslands` `{d1d6b44a-72fa-4fa1-ba27-a5f207cb37ca}`
   (`CreateVisualIslandSite` slot 6, `CreateVisualTreeIsland` slot 7).
2. `As` `ICompositorDesktopInterop` `{29e691fa-4567-4dca-b319-d0f207eb6807}`
   (`CreateDesktopWindowTarget` slot 3, `EnsureOnThread` slot 4).
3. `CreateDesktopWindowTarget(CoreWindow HWND, FALSE, …)`.
4. `As` `ICompositionTargetPartner` `{da5d6502-31dd-5bfe-bd07-99f4e5bb5098}` on that
   target (GetRoot/SetRoot, same layout as `ICompositionTarget`).
5. `CreateContainerVisual` and `CreateVisualIslandSite`.

Without (4) it fail-fasts in `InitializeCompositor+0x17d`. With the island ifaces in
place, configure completes: `+dcomp` shows `CreateDesktopWindowTarget`,
`CreateContainerVisual`, `CreateVisualIslandSite`, then interop `CreateVisual` /
`GetFrameStatistics` / `WaitForCommitCompletion`.

It then fail-fasts with `E_POINTER` (`0x80004003`) while **tearing down**
(`DCompTreeHost::CloseAndReleaseInteropCompositor`). The earlier stowed error is
`CommitMainDevice+0x21f`, which calls interop-device **slot 69** (vtbl+0x228). The
vtable only went to slot 67, so that call was garbage. Unnamed slots 60–75 now
return `S_OK`.

Logs: `/tmp/wine-uwp-island2.log` (Partner QI missing), `/tmp/wine-uwp-island4.log`
(configure succeeded, commit slot 69).

After slot 69 returns `S_OK`, `CommitMainDevice` no longer fail-fasts. DXGI/D3D11/D2D
come up (`DXGID3D10CreateDevice`, `d2d_device_create_device_context`). Then one interop
`CreateVisual` and XAML tears down with `E_POINTER` (`0x80004003`) in
`CloseAndReleaseInteropCompositor`. That destructor `E_POINTER` is the next probe;
the island is configured.

`/tmp/wine-uwp-island5.log`.

### CloseAndRelease RealClose vs CreateVisual

`DCompTreeHost::CloseAndReleaseInteropCompositor` (RVA `0x66be1c`) does:

1. `*(host+0x200)` → vtbl `+0x20` (partner-device slot 4 = `WaitForCommitCompletion`)
2. same object → vtbl `+0x30` (partner-device slot 6 = `CreateVisual`)

On `IInteropCompositorPartner` `{0e779a72-3f2a-502d-a7a9-bf99988cfe4d}` that same
`+0x30` is `RealClose` (no out-param). Host+0x200 still holds the `{e7894c70}` device,
so the second call lands on our `CreateVisual` with a null/`0` out-pointer →
`E_POINTER` → `RoFailFast`. Stack was
`FrameworkView::Uninitialize` → … → `DCompTreeHost` dtor →
`ReleaseResources(false)` → `CommitMainDevice` then `CloseAndRelease`.

Fix in `composition.c`:

- `device_CreateVisual`: if `!out`, treat as RealClose and return `S_OK`
- QI for `IInteropCompositorPartner` with a small vtbl whose slot 6 is `RealClose`

With that, island6 runs the full `UWP_TIMEOUT` (exit 124) with no fail-fast:
`CreateVisual as RealClose (null out): ignored`, then `INavigationClient` slots 23/24.
The compositor is still closed — the next probe is **why `FrameworkView::Uninitialize`
runs right after island configure / first D3D bring-up**, before any live present.

`/tmp/wine-uwp-island6.log`.

### FrameworkView tears down with `FrameworkViewState == 2`

After RealClose no longer fail-fasts, the view still dies. Stack:

`shcore` callback → fn@twinapi`0xc410` → `WaitForViewReady` →
`UninitializeView` → `CoreApplicationView::Uninitialize` →
`FrameworkView::Uninitialize` → `DCompTreeHost` dtor → `CloseAndRelease`.

At `UninitializeView` entry, agile-container `+0x108` (`FrameworkViewState`) is
**`0x2`**. Successful `SetWindowAndGetDispatcher` would have written **`3`** at
`+0x28dab`; `CoreApplicationView::Load` would then write **`4`**. Neither write
runs before teardown:

- `FrameworkView::SetWindow` is entered (island configure / D3D happen inside it).
- During SetWindow, `CoEndProcessEvents` runs, then the `0xc410` waiter wakes.
- `WaitForViewReady` either `WaitAndProcessEvents` or
  `CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS, INFINITE, 1, &container+0x40, …)`.
- On return, `0xc410` sees state `!= 4` and calls `UninitializeView`.
- State is still `2`, so SetWindow never reached the state=`3` store — teardown is
  nested/racing with SetWindow, or SetWindow never successfully returns.

`CoreApplicationView::Load` (RVA `0x37f80`) is never hit. `FrameworkView::Load` at
`0x539360` is a shared `xor eax,eax; ret` thunk (not a useful breakpoint).

`WaitForViewReady` (and the `0xc410` caller) run on the UI thread and pump
messages, nesting `SetWindow` (island configure / `CoEndProcessEvents`). When the
wait returns, state is still `2` or at best `3` (post-SetWindow); the caller then
requires **`4`** (post-`CoreApplicationView::Load`) and calls `UninitializeView`.
So the wait is waking **before Load**. Ready handle at `container+0x40` is a real
Wine handle (`0x334` in island15), not null.

`WaitAndProcessEvents` QIs the dispatcher for `{34fff979-bd36-4bb1-82d2-785a605b81fb}`
and calls vtbl+0x18. Alternate path: `CoWaitForMultipleHandles(8=COWAIT_DISPATCH_CALLS,
INFINITE, 1, &container+0x40, …)` — Wine’s `CoWaitForMultipleHandles` does not map
`COWAIT_DISPATCH_CALLS`.

### Premature `SignalViewReady` and island-site AV

`WaitAndProcessMessages` (`CDispatcher`, cloaked IID `{34fff979}`) waits on
`container+0x40`. That event is set by `SignalViewReady`, which during SetWindow is
invoked from a WIL failure-callback path (`GetContextAndNotifyFailure`) after an
**`EXCEPTION_ACCESS_VIOLATION`** (`c0000005`) in dcomp.

The AV was in `collection_InsertAtBottom` AddRef: `CreateVisualIslandSite` returned a
**container visual**, XAML treated it as `IVisualIslandSite`, and island-site calls
through the container vtbl corrupted the child pointer (`vtbl == 0x2300000000`).

Fix: `CreateVisualIslandSite` / `CreateVisualTreeIsland` return recording
`comp_object`s; container visuals no longer claim `IVisualIslandSite`. Slot stubs that
look like Create*/getters fill out-params with nested recording objects.

Next: `DllGetActivationFactory(CompositionIsland)` then
`RoGetActivationFactory(..., ICompositionIslandStatics {77fcc60e})` → `E_NOINTERFACE`.
Factory now answers that IID with `ConnectIsland` / `DisconnectIsland` stubs.

Island26: no AV, no fail-fast, CompositionIsland activates, D3D comes up, process
survives `UWP_TIMEOUT` (exit 124). Compositor still closes via RealClose before
`CoreApplicationView::Load` (state never reaches 4). Next probe: why
`WaitAndProcessMessages` returns once the island path succeeds (without the AV-driven
`SignalViewReady`).

Logs: `/tmp/wine-uwp-island15.log` … `/tmp/wine-uwp-island26.log`.

### Island30–39: ConnectIsland vtbl mixup, then past SetWindow

Island30–33 still NX-faulted right after `AddVisualIdentifier` with
`info[0]=8` at the `"VisualTreeIsland"` string and `rdx=0x4448000044a00000`
(Size 1280×800). Enlarging the recording `comp_object` vtbl to 4096 did **not**
help: a VEH dump showed `VisualIslandSite->vtbl` had been **overwritten** with the
address of a newly created `CompositionIsland` object. Loading “AddRef” from that
fake vtbl read the island’s `kind` string pointer and jumped there.

Root cause: `ICompositionIslandStatics` `{77fcc60e}` was served from the same vtbl
as `IActivationFactory`, with `ActivateInstance` at slot 6. Real statics put
`ConnectIsland` at slot 6 (after `IInspectable`). XAML’s
`ConnectIsland(site, island)` therefore hit `ActivateInstance(site)` and wrote
`*site = new CompositionIsland`, clobbering the site’s vtbl field.

Fix: separate activation and statics objects/vtbls; QI returns the matching one.
Also:

- `is_out_pointer` rejects non-canonical / float-looking bit patterns so Size in
  `rdx` is not probed as an out-param.
- `NtQueryCompositionInputSinkLuid` implemented in `win32u` (was
  `EXCEPTION_WINE_STUB` → fail-fast).
- `IDisplayInformation3/4/5` + stub `AdvancedColorInfo` so
  `CJupiterControl::UpdateHdr` no longer fail-fasts on `E_NOINTERFACE`
  `{3a5442dc}` (`IDisplayInformation5`).

Island39: process survives full `UWP_TIMEOUT` (exit 124), **no fail-fast**, **no
`RealClose`**, `ConnectIsland` traces correctly, `GetAdvancedColorInfo` runs,
`CreateWindowInBand` creates a 1280×800 `Windows.UI.Core.CoreWindow` named
`App` (visible X window; screenshot was black). Composition still only recorded.

Island44–46: `CreateDesktopWindowTarget` now stores the CoreWindow HWND on the
composition target; `put_Root` and interop-device `Commit` call
`present_desktop_hwnd`, which `ShowWindow`s and `FillRect`s the client with
`RGB(0x20,0x60,0xc0)`. Screenshot of the `App` window samples
`srgb(32,96,192)` across the surface — **first pixels on screen**. Not Calculator
chrome yet: the fill is a stand-in until visuals/surfaces are actually composed.

Logs: `/tmp/wine-uwp-island30.log` … `/tmp/wine-uwp-island46.log`,
`docs/LinuxPort/calc-present46.png`.

Also: `DCompositionCreateDevice{,2,3}` now share `dcomp_create_device` with the
WinRT interop device (answering public `IDCompositionDevice*`). In island49 those
exports were still **never called** — XAML uses `CreateInteropDevice` only.
Per-frame commit is partner **slot 69** (`CommitMainDevice`), not public `Commit`;
present is hooked there too. Still no `CreateSpriteVisual` / `CreateDrawingSurface`
/ DXGI swapchain in the run: content loads (`Frame`, `CalculatorManagerWrapper`,
`ContentControl`) but the HW visual tree is property-bag `CreateVisual` objects
without a raster path to the HWND yet.

Island51–53: `device_CreateVisual` now returns a real `IDCompositionVisual`-shaped
object (float `SetOffsetX/Y`, `SetContent`, `AddVisual`) instead of a recording
stub. Rejecting unknown partner IIDs fail-fasted (`{92179cc0}` → `E_NOINTERFACE`
→ `0xc0000409` from CalculatorApp); QI is permissive again. After that, the run
survives and activates **`Microsoft.UI.Xaml.Controls.NavigationView`**,
`VisualStateManager`, and `DataTemplateSelector` — UI chrome is being constructed.
Still only one interop `CreateVisual` (input hit-test path); no `SetOffset` traffic
on it yet. CommitMainDevice (slot 69) fires; HWND stays the blue stand-in.

Island54: InteropVisual::Api vtbl order recovered from native `dcomp.dll`
(`??_7Api@InteropVisual@…`); Wine’s `CreateVisual` object now matches through
slot 46 (`SetWidth`/`SetHeight` at 36/37, `RealClose` at 46). Still only one
`CreateVisual` per run (input/hit-test), with no `SetOffset`/`SetWidth` traffic
on it. `NavigationView` / `VisualStateManager` still activate. No
`CreateSpriteVisual` / `CreateDrawingSurface` / `CreateGraphicsDevice` in the
whole launch — the XAML→composition render walk is not producing a paintable
tree yet. `{0d8fb190}` on the compositor is backdrop-related; answering it with
a probe and `E_NOTIMPL` on slot 6 yields `0x802b000a` (leave it as
`E_NOINTERFACE` so Mica falls back).

Next probe: why `EnsureResources` / `CreateGraphicsDevice` never runs after
`d3d11_xaml_device_enable` returns S_OK, and what gates
`HWCompTreeNodeWinRT::CreateContentVisual`.


## Island63–71: composition tick path (RefreshAlignedClock → MessageTimer)

Hardware `WINE_UWP_BREAK_*` DRs are **per-thread**. `CompositorScheduler::Startup` does
`ThreadCreate(RenderThreadMainStatic)` successfully, but early probes never saw
`RenderThreadMain` because the new thread had no DRs. Fix: call `uwp_rearm_breaks()` from
`RtlUserThreadStart` (and `default_thread_init_func`) so CreateThread workers inherit breaks.

With that:

| Symbol | Hits (island69) |
| --- | --- |
| `RenderThreadMain` | 6 |
| `CallCoInitializeMTA` → `CoInitializeEx(MTA)` | 6 |
| `RenderThreadFrame` | 12 |
| `UIThreadScheduler::QueueTick` | 6 (on the **render** thread) |

`CXcpDispatcher::QueueTick` also hits (island71: 6). It enqueues a `0x402` message and calls
`IMessageTimer::SetRelative(0,0)` (vtbl +0x50; Init used `SetIsRepeating(0)` at +0x28).

**Never hit:** `MessageTimerCallbackStatic`, `CXcpDispatcher::Tick`,
`UIThreadScheduler::BeginTick`, `CCoreServices::Tick`, `WaitForD3DDependentResourceCreation`,
`EnsureResources`, `CreateContentVisual`, `HWWalk::Render`.

So the render thread is alive and queues ticks, but CoreMessaging’s `MessageTimer` never
delivers the callback onto the UI thread — that is the gate before any XAML HW walk /
`CreateGraphicsDevice` / chrome pixels.

`ScheduleCoreMessagingDispatch` / `NtUserInitThreadCoreMessagingIocp2` remain semi-stubs
(post `WM_NULL` / attach IOCP). They run at dispatcher init, not on each `SetRelative`.
Next: make the MessageTimer → UI callback path actually fire (CoreUI dispatch / IOCP drain /
wait integration).


## Island72–78: MessageTimer armed but CoreUI EventLoop idle

`IMessageTimer::SetRelative(0,0)` returns **S_OK** (6× per launch, matching QueueTick).
`WaitCollection::RegisterWait` also runs (6×). **`OnTimerSignaled` never runs.**

`CoMsgWaitInProcessEvents` was blocking forever (`INFINITE`) on 1–2 handles. For hosted UWP,
combase now caps that to **16 ms** so the pump cycles (thousands of calls/sec). That alone does
**not** deliver MessageTimer callbacks.

CoreUI `EventLoop::Run` / `Callback_DoGeneralWait` / `PrepareToWait@EventLoopBridge` show **0**
hits — the WaitCollection has registered timer waits, but no EventLoop is waiting on them.
Next: `Win32EventLoopBridge::{Callback_Wait,Callback_Run,DrainSignaledWaits}` and whatever
should start the bridge from CoreWindow / CoreDispatcher.


## Island78–85: UI pump alive; CoreUI timer still orphaned

`CoMsgWaitInProcessEvents` now caps INFINITE→16ms for UWP and cycles hard (thousands of
calls). `Windows.UI!CDispatcher::ProcessMessage` hits ~8k/20s. `CreateVisual` rose from 1→4.

Still **never**: `MessageTimerCallbackStatic`, `DrainSignaledWaits`, `PeekForNewEvents`,
`OnTimerSignaled`, `BeginTick`, `CCoreServices::Tick`.

Root shape: `IMessageTimer::SetRelative(0,0)` → `SetWaitableTimer(due=0)` +
`WakeDispatchThreadIfNecessary`, but the wake path no-ops when Windows.UI owns
`CoMsgWait` (CoreUI `is_waiting` flag clear). The waitable timer is **not** in the
`CoMsgWait` handle array (only `CDispatcher+0x308` wake event). Pulsing that wake
event on TIMEOUT and posting `WM_NULL` to thread HWNDs did **not** produce
`DrainSignaledWaits`.

Next: make `WakeDispatchThreadIfNecessary`’s SetEvent actually run (set is_waiting from
CoMsgWait, or SetEvent the wake handle from SetWaitableTimerEx when due is immediate),
or find how `ProcessMessage` is supposed to poll CoreUI timers and hook that.


## Island86–92: DrainCoreMessaging is the missing ProcessMessage step

`CDispatcher::DrainCoreMessaging` (RVA `0x433ec`) is the API that should flush CoreUI
MessageTimers into XAML. It is **only** called from `ProcessEvents`, never from the hot
`WaitAndProcessMessages` → `ProcessMessage` loop (which runs ~10k times per launch).

Attempts to trampoline `ProcessMessage` → `DrainCoreMessaging` (with/without null-check /
soft-fail of `RoFailFastWithErrorContext`) fail-fasted early (exit 5). Reverted
`Windows.UI.dll` to the pristine extract.

**Still in wine (keep):**
- `uwp_rearm_breaks()` from `RtlUserThreadStart` (per-thread HW breaks for CreateThread workers)
- `CoMsgWaitInProcessEvents` UWP INFINITE→16ms cap + wake-handle pulse + thread HWND `WM_NULL`
- `ScheduleCoreMessagingDispatch` posts to the adapter HWND

**Next concrete step:** make `DrainCoreMessaging` safe to call from `ProcessMessage` (why
`[dispatcher+0x300]` QI/method fail-fasts), or wire `WakeDispatchThreadIfNecessary` so the
wake event in `CoMsgWait` is actually signaled when `SetRelative(0,0)` runs — then confirm
`MessageTimerCallbackStatic` → `BeginTick` → `CreateContentVisual`.


## Island93–95: proxy wake alone is not enough

Added `Local\WineUwpCoreMsgWake` proxy event: `CoMsgWaitInProcessEvents` waits on it;
`SetWaitableTimerEx` pulses it when `when <= 0` (MessageTimer due-now). UI pump wakes more,
but still **no** `MessageTimerCallbackStatic` — because `ProcessMessage` never calls
`DrainCoreMessaging`.

Binary trampolines from `ProcessMessage` / post-`CoMsgWait` into `DrainCoreMessaging`
(even with null-check + soft-fail of its `RoFailFastWithErrorContext` paths) still
fail-fast early. Reverted `Windows.UI.dll` to pristine.

**Blocker for chrome:** get a safe `DrainCoreMessaging` (or equivalent
`UserAdapter::DrainCoreMessagingQueue`) into the `WaitAndProcessMessages` loop without
fail-fast, then `BeginTick` / `CreateContentVisual` should follow.

## Island96–107: Drain reaches HostModeRun; timer still orphaned

`Windows.UI` ProcessMessage cave (after ~50 warmups) calls `DrainCoreMessaging` →
`IMessageLoopExtensions::Run(2)` → `Callback_HostModeRun` → `RunIntegratedLoop` →
`DrainSignaledWaits`. **DrainSignaledWaits hits, OnTimerSignaled stays 0.**

Root cause: `DrainSignaledWaits` only walks a **pre-built signaled-wait list** filled when a
CoreUI wait includes the MessageTimer handle. `CoMsgWaitInProcessEvents` never waits on that
handle, so the list is empty. Proxy wake + Drain alone cannot deliver the callback.

Abandonment soft-nops (`FailWithHR` / `ForWin32` → `xor eax,eax; ret`) convert the old INT3
fail-fast into a fall-through page fault inside `DrainSignaledIocpWaits` when
`GetQueuedCompletionStatus` fails (Wine IOCP path).

## Island108–109: due-now MessageTimer → OnTimerSignaled (works)

**CoreMessaging binary patches (source extract + prefix copy):**

1. Soft-fail Abandonment/FailFast entries (`0x1060`, `0x1f78`, `0x903c0`, `0x903a0`, `0x8c8e8`,
   `0x1008`) jump to a cave that skips trailing `int3` bytes and returns 0.
2. `SetRelative@Dispatcher` success path (`0x54f3c`): when due==0, enqueue `Timeout*` into BSS
   slot `0x10d000`.
3. `DrainSignaledWaits` entry (`0x23a70`): dequeue and call WaitCallback thunk `0x551f0`
   (`OnTimerSignaled`) **on the UI/HostModeRun thread**.
4. Island109: `DrainSignaledIocpWaits` GQCS failure (`je 0x24085` at `0x23cfb`) retargeted to
   cleanup `0x23d30` so HostModeRun no longer die-faults.

**Result:** `SetRelative` → enqueue → `DrainSignaledWaits` → `OnTimerSignaled` confirmed.
EXIT:0. Keep Windows.UI ProcessMessage→Drain trampoline.

## Island110–115: MessageTimerCallback / OnTick run; Tick skipped (null surface)

| Symbol | Hits |
|---|---|
| `MessageTimerCallbackStatic` (`0x4a78b0`) | 2 |
| `BeginTick@UIThreadScheduler` (`0x4b7430`) | 2 |
| `OnTick@CXcpBrowserHost` (`0x1c7b50`) | 2 |
| `EndTick@UIThreadScheduler` (`0x4cb720`) | 2 |
| `CCoreServices::Tick` (`0x1c6b28`) | **0** |
| `CreateContentVisual` (`0x1be350`) | **0** |
| `SetWindowSize@CXcpBrowserHost` (`0x196ee0`) | **0** |

`OnTick` early-outs because `[[browserHost+0x48]+0x28]` (window/surface size object) is
**NULL** (break at `0x1c7bde`, rax=0). `SetWindowSize` / `EnsureCorrectWindowSize` never run.
Expected chain: CoreWindow size → `OnCoreWindowSizeChanged@CJupiterWindow` (`0x196520`) →
`OnJupiterWindowSizeChanged` → `SetWindowSize` → fills `[host+0x48]+0x28` dims → `OnTick` can
call `Tick` / draw.

NOP of the zero-dimension `jbe`s (island114) did not help (pointer is null, not zero-sized)
and caused `0xc0000409`; reverted `Windows.UI.Xaml.dll` to `.bak-island114`.

`add_SizeChanged` registers handlers, but `SendWindowSizeEvent` / `OnWindowSizeEvent` /
`WindowServer::WndProc` never run (no WM_SIZE delivery to CoreWindow).

**Island121–124 workaround:** OnTick cave calls `EnsureCorrectWindowSize(host, 800, 600, …)`
when `[[host+0x48]+0x28]` is null. That creates `WindowsPresentTarget` via
`CreateWorker` (S_OK). Afterward OnTick **passes the dimension checks** (`after_dims_ok` /
`past_dim_checks` hit). Still **no** `NWDrawTree` / `CCoreServices::Tick` /
`CreateContentVisual` yet — blocked further inside OnTick before `call NWDrawTree`
(`0x2ade0`).

**Island126–129:** `[presentObj+0x238] != 0` diverted OnTick into an alternate path that
skipped `NWDrawTree`. NOPing the `jne 0x1c7e85` at `0x1c7c5f` forces the main draw path:

| Symbol | Hits (island128/129) |
|---|---|
| `call NWDrawTree` / `NWDrawTree@CCoreServices` (`0x2ade0`) | 2 |
| `CCoreServices::Tick` (`0x1c6b28`) | 2 |
| `EnsureResources@DCompTreeHost` (`0x1c62b8`) | 2 |
| `CreateContentVisual` (`0x1be350`) | **0** |

Tick then faults in `GetCurrentCoreWindowNoRef@DXamlServices` (`0x3fc68c`):
`[coreWindowIsh+0x70]` with a NULL intermediate. Soft-returning NULL from that helper
only moves the crash into `CoreWindowIslandAdapter` ctor (`0xa008d4`) which does not
tolerate a null `ICoreWindow`. Restored the original helper.

**Next concrete step:** make `DXamlServices::GetCurrentCoreWindowNoRef` return a real
`ICoreWindow` (wire the activated CoreWindow into the DXaml core / island adapter), then
recheck `CreateContentVisual` / chrome. Keep the MessageTimer enqueue/drain patches and
the OnTick size + NWDrawTree force-path until replaced by proper size events.

## Debugging aids to remove before upstreaming

`WINE_UWP_WATCH`, `WINE_UWP_BREAK`, `WINE_UWP_BREAK_MOD` / `WINE_UWP_BREAK_OFF` /
`WINE_UWP_BREAK_WATCH` / `WINE_UWP_BREAK_STOPRVA`, `WINE_UWP_TRACE_ORIGINATE` (including the
`IStringable` dump in `RoOriginateLanguageException`), `uwp_rearm_breaks` called from
`LdrLoadDll`, `WINE_UWP_ACTIVATE_ON_VIEW`, `WINE_UWP_FEATURES`, `WINE_UWP_DEVICE_FAMILY`,
`WINE_UWP_COMPOSITOR_PROBE`.

## Island131–132: SetCoreWindow never runs

`GetCurrentCoreWindowNoRef` → `DXamlCore::GetCurrent` then `[core+0x30]+0x68]+0x70`.
Fault at `+0x70` means the CoreWindow slot inside DXamlCore is unset.

`SetCoreWindow` variants probed (all **0** hits):

- `DXamlCore::SetCoreWindow` (`0x3bb344`)
- `Window::SetCoreWindow` (`0x3bb8b4`)
- `CJupiterWindow::SetCoreWindow` (`0x3bea8c`)
- `XamlPalSetCoreWindow` (`0x584884`)

**Next:** find who should call `SetCoreWindow` during FrameworkView/CoreApplicationView
activation (island vs classic CoreWindow) and make that path run so
`GetCurrentCoreWindowNoRef` succeeds → `CoreWindowIslandAdapter` / `CreateContentVisual`.

## Island133–142: SetWindow never called; CoreWindow null in DXaml

`FrameworkView::Initialize` runs; `SetWindow` / `Run` / twinapi
`SetWindowAndGetDispatcher` / `ActivateInternal` / `CreateCoreWindow` do **not**.
`CoreWindow::GetForCurrentThread` works (14 hits) — a CoreWindow exists on the UI
thread — but `DXamlCore::SetCoreWindow` never stores it.

Null-safe `GetCurrentCoreWindowNoRef` + null-tolerant `CoreWindowIslandAdapter` ctor
avoids the `+0x70` fault. Tick / `EnsureResources@DCompTreeHost` still run; next fault
at XAML RVA **0x26f36a** (identify and continue). `CreateContentVisual` still 0.

**Next:** either force `FrameworkView::SetWindow` / `ConfigureCoreWindow` with
`GetForCurrentThread`'s CoreWindow after Initialize, or clear the 0x26f36a null and
keep pushing the HW EnsureVisual → CreateContentVisual path.

## Island147: CreateContentVisual hits (OneCoreTransforms forced off)

`XamlOneCoreTransforms::IsEnabled` (RVA `0x80b60`) forced to return FALSE
(`xor eax,eax; ret`). That avoids the OneCore-only
`RegisterScaleChangeNotifications` path that needs unset `DXamlCore+0x2f0`.

| Symbol | Hits |
|---|---|
| `CCoreServices::Tick` | 2 |
| `EnsureResources@DCompTreeHost` | 6 |
| `EnsureVisual@HWCompTreeNodeWinRT` | 2 |
| **`CreateContentVisual@HWCompTreeNodeWinRT`** | **2** |

Next fault at XAML RVA **0x2b0419**. Chrome still not confirmed on screen — keep
pushing past this fault; reconsider OneCoreTransforms once SetCoreWindow is wired.

## Island148–149: past CreateContentVisual; next null vtable

After null-checking the failed QI in `GeneralImageRenderContent` (RVA `0x2b0415`
cave), CreateContentVisual still runs (2×). Next crash is **execute at address 0x2**
(null vtable call). Chrome not yet confirmed visually; keep clearing null interface
paths and restore proper `SetCoreWindow` / OneCoreTransforms once the tree presents.

## Island160–173: Activate unblocked past E_POINTER; CreateCoreWindow runs

### Crash stabilisers (Wine)

1. **dcomp `comp_object_nx_veh`**: execute-at-`0x2` worker faults (OleAut32 return
   `[rsp]=…+0x67c30`, `rcx=0`) now dump `[rsp]` before `[rcx]`, then soft-continue via
   `nx_soft_ret` when `rip < 0x10000`.
2. **dcomp `device_CreateVisual`**: reject `out` with `(ULONG_PTR)out < 0x10000` (was writing
   through `out==(void**)2` → UI write fault at address 2).
3. **combase `ftmarshaler_Release`**: skip outer Release when outer vtbl looks like a USER
   handle (`vtbl==0x10001`). FTM object itself had its vtbl overwritten with an HWND.

With (1)+(2)+(3): process reaches **EXIT:0** instead of EXIT:5.

### Activate E_POINTER (not factory `+0x78`)

`ApplicationActivationFactory::Activate` does load `[this+0x78]` and reaches
`GetWindowFactory` / `wf_CreateFromCookie` / `cfg_CreateSplashScreen`. The persistent
`0x80004003` was **`EnsureCoreApplicationInitialized`** (`Xaml` RVA `0x3bb095`):

- `FrameworkApplication::GetCurrentNoRef` → NULL
- `ShouldTryLoadAppXaml` → TRUE → E_POINTER

Same stack: `FrameworkView::Initialize` (`0x324944`). Often on a **worker** thread spawned
during Activate, not on the view STA.

### View-thread delivery

- `WINE_UWP_ACTIVATE_ON_VIEW` + `CoreDispatcher::RunAsync`: posts work that HostModeRun/Drain
  never runs → Activate never called.
- `EnumWindows` to find the CoreWindow thread **deadlocks** against the busy UI thread.
- **Toolhelp + `QueueUserAPC`** onto other process threads: APC runs on the view thread, but
  Activate nested inside an alertable wait **deadlocks** (Activate waits for UI work the UI
  thread cannot run while inside the APC).

### XAML soft patches (island171–173)

| Patch | Effect |
|---|---|
| `ShouldTryLoadAppXaml` (`0x3bb250`) → `xor eax,eax; ret` | skips E_POINTER branch in EnsureCoreApplicationInitialized |
| `FrameworkView::Initialize` after `GetCurrentNoRef` (`0x324986` → cave `0xc6cda0`) | null-safe: if FA is NULL, jump to epilogue `0x3249cc` |

### Island173 result (MTA Activate + patches)

- `do_activate` → splash → **`wf_CreateCoreWindow` / `PrivateCreateCoreWindow` → S_OK**
- Navigation client attached; CoreWindow HWND shown (`SetWindowFeedbackSetting` stubs).
- **Still no** `FrameworkView::SetWindow` / `ConfigureCoreWindow` / `DXamlCore::SetCoreWindow`
  break hits.
- Activate never logs success/failure; shortly after CreateCoreWindow:
  `nav_stub_23/24`, `DestroyActivationObject`, `CoRegisterServerShutdownDelay`, EXIT:0.

**Next:** find why `SetWindowAndGetDispatcher` does not call `FrameworkView::SetWindow`
after a successful `CreateCoreWindow` (twinapi break on `0x288a0` / wait-state), or why
activation tears down via `DestroyActivationObject` first. Keep NX/CreateVisual/FTM
guards and the two XAML soft patches.


## Island174–182: ConfigureCoreWindow works; SetWindow still skipped

### Twinapi path

Hardware breaks show `Activate` + `ActivateForeground` hit; **`SetWindowAndGetDispatcher`
(`0x288a0`) never hits**. Window creation goes through
`CoreApplicationView::CreateCoreWindow` (`0x2c600`) → wineuwp `wf_CreateCoreWindow` →
`PrivateCreateCoreWindow` (S_OK), then `RegisterPresentationEvents` / nav stubs /
`DestroyActivationObject`. That path does not call `FrameworkView::SetWindow`.

### wineuwp: force ConfigureCoreWindow (island176–179)

After `PrivateCreateCoreWindow` + nav attach, call the same sequence as
`FrameworkView::SetWindow`:

- `DXamlCore::GetCurrent` (`Xaml+0x1134d4`)
- `DXamlCore::ConfigureCoreWindow(core, window, TRUE)` (`Xaml+0x3bd0b4`)

**Confirmed:** ConfigureCoreWindow and `SetCoreWindow` (`0x3bb344`) break hits;
`ConfigureCoreWindow … hr 0` on the create thread. `FrameworkApplication::GetCurrentNoRef`
is NULL on that thread → skip `InvokeOnWindowCreated`.

Also publish the `ICoreWindow*` into XAML BSS mailbox **`0xfd3450`**. 
`GetCurrentCoreWindowNoRef` cave (`0xc6cc80`) returns that pointer when the DXamlCore
slot is empty (canonical check `> 0x10000`).

### Remaining gap for chrome

`CreateContentVisual` ticks (~2) finish **before** Activate's CreateCoreWindow, so the
main UI thread's DXamlCore never sees the mailbox/configure in time. APC broadcast of
Configure onto other threads usually sees `GetCurrent == NULL` (wrong thread) or never
runs. Pre-Activate `GetForCurrentThread` APC wiring did not stick.

**Next:** ensure ConfigureCoreWindow / mailbox publish runs on the **main UI thread**
before or during the first ticks (e.g. from an OnTick/ProcessMessage hook, or make the
MessageTimer keep firing after Activate so GetCurrentCoreWindowNoRef picks up the mailbox),
and/or make `SetWindowAndGetDispatcher` actually run so FrameworkView::SetWindow +
InvokeOnWindowCreated happen on the right thread with FrameworkApplication present.

## Island183–204: post-window ticks reach the WinRT hardware tree

Island183 confirms that the forced create path now calls both
`DXamlCore::ConfigureCoreWindow` and `DXamlCore::SetCoreWindow` successfully after
`PrivateCreateCoreWindow`. The window is also published to the XAML mailbox. The first
two ticks still precede that publication, but the later XAML instrumentation now drives
another render walk after the window exists.

That later walk crossed the old gate:

- `DCompTreeHost::EnsureResources` calls `ICompositorInterop::CreateGraphicsDevice`;
- `HWCompTreeNodeWinRT::CreateContentVisual` runs;
- the compositor creates `ContainerVisual` and `SpriteVisual` objects;
- `UpdateTreeChildren` asks the container's private `InteropVisual` interface for its
  visual collection and enumerates that collection;
- XAML QIs the WinRT visual for public `IDCompositionVisual` and reaches
  `CommitMainDevice` twice per XAML core.

Three concrete compatibility fixes were needed:

1. `ftmarshaler_Release` also saw an even USER-handle-shaped vtable (`0x10002`). The
   existing odd-handle check only caught `0x10001`; all low non-pointer vtable values are
   now rejected.
2. The compositor now accepts private `{ae47e78a-...}`, queried by
   `WUCBrushManager::EnsureResources` after graphics-device creation, and WinRT visuals
   expose private `InteropVisual` `{fe93b735-...}` with `GetVisualCollection` at slot 45.
3. `{4eed7dce-...}` is `IIterable<Visual>`, not another view of `IVisualCollection`.
   Returning the collection vtable made `First` call `get_Count` and corrupted the
   iterator pointer. It now has a separate iterable vtable and a real recording iterator.
   The same interop visual also answers public `IDCompositionVisual` `{4d93059d-...}`.

The existing XAML null-safe iterable cave had another latent defect: its non-null path
jumped back to RVA `0x1c43f6`, in the middle of the overwritten guarded-call sequence.
It now loads `IIterable::First`, calls the CFG dispatch thunk, and resumes at
`0x1c43fb` (`mov ebx,eax`). Binary edits must preserve file length and refresh the PE
checksum; otherwise .NET Native fail-fasts with code `0x1007` before the cave executes.

Island203/204 exit normally with no XAML fail-fast or access violation and show the
full sequence above on both XAML cores. The remaining private sprite IID
`{3b3e3dcf-...}` is refused but is non-fatal in this path.

The remaining presentation gap is now later and narrower: with OneCore transforms still
forced off, neither render core retains a desktop/island composition target. The visual
tree is constructed and committed, but `last_desktop_hwnd` is unset and the recording
renderer has nowhere to present it. Next: restore the island target after CoreWindow is
wired, or explicitly associate the post-window compositor with the shown CoreWindow HWND,
then render the recorded WinRT visual tree rather than only the interop `DCompVisual` list.

Logs: `/tmp/wine-uwp-island183.log`, `/tmp/wine-uwp-island186.log`,
`/tmp/wine-uwp-island203.log`, `/tmp/wine-uwp-island204.log`.

Regression check at this checkpoint: `probe-avl` still passes. `probe-mrt` currently does
not: its private framework-map lookup returns `0x80070490` under `WINEDEBUG=-all`, and a
traced run exposes a freed-looking string pointer inside the single loaded `MrmCoreR`.
Calculator itself still gets through MRT and builds the XAML hardware tree, so this is a
probe/setup regression to resolve separately; do not record the MRT probe as passing for
island204.

## Island205: DLL audit and hosted CoreWindow presentation target

A deterministic presentation check now lives at
`tools/wine-uwp/check-presentation.sh`. It requires graphics-device, container-visual,
sprite-visual, and `CommitMainDevice` signals, then stays red until an HWND target is
actually presented. View-thread activation and `+loaddll` tracing make the relevant race
repeatable enough for the short retry loop.

The full loader trace (`/tmp/wine-uwp-dll-audit.log`) found **no** module-not-found,
import-DLL, `STATUS_DLL_NOT_FOUND`, or entry-point-not-found errors. The native modules
that matter at this boundary are present and loaded: `Windows.UI.Xaml`, `Windows.UI`,
`twinapi.appcore`, `CoreMessaging`, `MrmCoreR`, `bcp47mrm`, `CoreUIComponents`,
`InputHost`, `UiaManager`, and `WindowManagementAPI`; Wine's builtin `dcomp` is loaded as
intended. The extracted System32 inventory also contains `Windows.ApplicationModel`,
`Windows.Globalization`, and `Microsoft.Internal.WarpPal`. `bcp47langs.dll` and
`bcp47mrm.dll` are absent from the extraction source directory but already exist in the
prefix, and the latter is observed loading natively. Re-extracting or adding more DLL
overrides is therefore not justified by this failure.

Native `dcomp.dll` is not a shortcut. Its import table requires 19
`NtDComposition*` calls, 21 `NtFlipObject*` calls, and private HWND-compositor entry
points, so substituting it would only move the blocker into missing kernel compositor
infrastructure.

Restoring the original `XamlOneCoreTransforms::IsEnabled` prologue was also tested as a
three-byte, checksum-correct experiment. It regressed before visual creation with an
illegal instruction in native XAML. The island204 binary was then restored
byte-for-byte and the classic-transform baseline again committed the tree. Reopening
the OneCore branch now would repeat the earlier scale/island work rather than solve this
presentation boundary.

The actual timing is: `CommitMainDevice` occurs before `PrivateCreateCoreWindow`, and
there is no later commit. Polling for a CoreWindow from dcomp at commit time therefore
cannot work. Instead, wineuwp now passes the already-discovered CoreWindow HWND to a
private builtin-dcomp hook immediately after `PrivateCreateCoreWindow`. This models the
shell-side association absent from the in-process host without creating another target,
thread, timer, or native dependency. A green run records:

```text
CommitMainDevice
running Activate on view thread
__wine_dcomp_set_target_window hosted CoreWindow hwnd 0x1006a
presented desktop hwnd 0x1006a 1280x800 root 0
published CoreWindow ... to XAML mailbox
```

This is a real 1280×800 HWND presentation, but it is not yet Calculator chrome. The
public `CompositionTarget` root remains NULL because this classic XAML path records its
tree through interop `IDCompositionVisual` objects. The fallback renderer currently
walks those globally recorded interop visuals. Next work should establish which recorded
visual owns the XAML root and connect that root explicitly; it should not add more DLLs
or revive the OneCore path without new evidence.

Verification: both modified Wine modules build cleanly, the presentation check reaches
green, `probe-avl` passes with zero problems, and `probe-mrt` still independently crashes
in `CompareStringOrdinal` with a freed-looking pointer (exit 5). The MRT regression is
unchanged and must not be conflated with the now-proven composition-target handoff.

## First content pixels: the app background brush paints (2026-09-11)

The vc205/island206 session (post-Findings, 00:22–00:45) left the tree in three states this
session had to untangle before continuing:

- The XAML binary carried one more 2-byte experiment on top of `.bak-island205`: byte
  `0x2b041c` in `GeneralImageRenderContent` changed `0x90` (NOP) back to `0x48`, restoring the
  original 64-bit `mov rdi,[rax+0x58]` that the island148 cave had truncated to a 32-bit load.
  That state (`.bak-island206`, kept) reaches further than island205 and is the working baseline.
- `check-presentation.sh` was red under its own env: with `+loaddll` and the XAML-break env the
  run often stalled before CommitMainDevice or Activate at all. Run-to-run timing variance is
  large; single runs flip between "presents", "stalls pre-activation", and "Activate fails".

### Activate fails with E_APPLICATION_EXITING (diagnosed, not fixed)

`do_activate` failures print `hr 0x8000001a`. That constant is `E_APPLICATION_EXITING`
(SDK winerror.h). XAML never produces it; twinapi.appcore does, in exactly two places:

| Site | Function | Code |
| --- | --- | --- |
| RVA 0x850d2 | `CoreApplication::AddViewForCurrentThread` +0xb2 | `mov ebx, 0x8000001a` |
| RVA 0x8e2be | `CoreApplication::AddWindowThreadMapping` +0x11e | `mov ebx, 0x8000001a` |

Both load the CoreApplication lifecycle state at `+0x2c0` and fail when it equals 2. The
writer is `CoreApplication::RunInternal`: right after calling `PsmShutdownApplication` it does
`mov eax,2; xchg eax,[this+0x2c0]` (RVA 0x330ce). So whenever the first RunInternal/teardown
cycle completes before the APC-delivered Activate runs, the app is marked exiting and Activate
fails. `MainViewClosedAppExiting`/`ExitingOnMainViewActivationFailure` only report telemetry.
Not fixed; it did not reproduce in four later attempts. If the check ever prints
`Activate failed, hr 0x8000001a` again, this is the mechanism; a Wine-side fix would be
showing the window earlier or pinning the state field.

### Three fixes, each visible in one run

1. **`dcv_AddVisual` dropped foreign children** (`dlls/dcomp/composition.c`). The vc205 code
   retained a child only when its vtable was exactly `visual_interop_vtbl`; XAML passes public
   `IVisual`/`ISpriteVisual` pointers, so the root visual was silently discarded with only a
   debug trace. It now falls back to `IUnknown_QueryInterface(IID_IInteropVisual_private)` and
   retains the resolved interface as `public_child`. Trace:
   `resolved foreign child ... -> interop`.
2. **Sub-pixel sizes defeated the size fallback** (`paint_public_visual`). XAML sets the root
   sprite's size to float noise (put_Size logs `0.000000` but the value is ~1.6e-07), so the
   `<= 0` fallback never fired and the paint was a hairline. Anything below half a pixel now
   counts as unset and falls back to parent/clip size.
3. **The CoreWindow never mapped to X11.** Screenshots during presentation were byte-identical
   to the idle desktop, and no 1280x800 X window existed. `CreateWindowInBandEx` had created
   it with `style 0x80000000` (WS_POPUP, no WS_VISIBLE), `ex 0x200000`
   (WS_EX_NOREDIRECTIONBITMAP), band 9 — on Windows the shell shows band windows after
   activation, and nothing here plays the shell. `CreateWindowInBand` now ORs in WS_VISIBLE
   (`dlls/user32/message.c`); the `App` window appears at 0,0 and stays alive.

With those, `present_desktop_hwnd` walks the recorded interop tree, resolves the WinRT root,
and paints the sprite's brush — the app's light-theme background, srgb(232,232,240), across
the full 1280x800 client. This is the first on-screen content derived from the XAML visual
tree rather than a stand-in fill: `docs/LinuxPort/calc-background-paint.png`.

`check-presentation.sh` is green again under its original env. `check-visual-content.sh` was
rewritten to gate on what this path actually produces (foreign root retained + non-hairline
sprite painted); its old `dcv_SetWidth`/`dcv_SetContent` gates cannot pass here because XAML
sets size 0/float-noise and content NULL on the interop visuals until real D2D surfaces exist.

### Next

The remaining gap to Calculator chrome is real surface content: XAML fills sprites with solid
brushes only at the root; everything else (NavigationView, buttons, digits) needs the
EnsureResources → CreateGraphicsDevice → CreateDrawingSurface/D2D path to record drawables the
fallback renderer can blit, or the full composition path (M4 Cut A) to present them properly.
Also still open from before: `probe-mrt` crashes in `CompareStringOrdinal` (exit 5); XAML
teardown still runs `FrameworkView::Uninitialize` before `CoreApplicationView::Load` (state
never reaches 4), which is what feeds the E_APPLICATION_EXITING race above.

## The navigation client, slot for slot; XAML is closing the window (2026-09-11)

### The whole INavigationClient vtable is now named

`dlls/wineuwp` hosts a stand-in for `Windows.Phone.UI.Core.ImmersiveNavigationClient` because
`CoreApplicationView::PrepareToActivateAsync` fails the activation without one. Until now every
slot past `IInspectable` was a numbered stub returning a guessed `S_OK`/`E_NOTIMPL`, and the
vtable was 74 slots long on a guess.

The real vtable is recoverable exactly. `CoreUIComponents.dll` (pinned Windows 11 build) has
`??_7NavigationClient@Core@UI@Phone@Windows@@6BINavigationClient@1234@@` at RVA `0x1af3f8`; the
next vtable symbol sits `0x450` bytes later, so the interface is **138 slots**, not 74. Reading
the 138 pointers and naming each through the public PDB gives the complete map — slots 6-20 are
splash screen/screen capture/overlay/`VisibleBounds` plus four event pairs, 21-28 orientation
and the window callbacks, 29-67 launch sizing and the whole title-bar colour-override block,
68-71 `OnNavigateBackRequest`/`SetAppAsComponent`/`DisableResponsiveAppViewPolicy`/`Uninitialize`,
73-103 the window-trait property pairs (`IsMaximizable`, `IsWin32TopLevelAppWindow`, …), and
104-137 scale, instance ids, proxying, `PresentationLoadingComplete` and the consolidation
state. Reproduce with the vtable dump recipe in `tools/wine-uwp/README.md`.

All 138 are now implemented as named methods describing the one view this host has: a single
top-level 1280x800 landscape window with no system title bar (`dlls/wineuwp/main.c`). Two
classes of latent bug went away with the numbered stubs:

- The four `add_*` event slots returned `S_OK` **without writing the out `EventRegistrationToken`**,
  so XAML kept a token from uninitialised stack and later removed it. They now write 0.
- Every getter that returned `E_NOTIMPL` left its out parameter untouched, including
  `get_VisibleBounds`, `GetViewBounds`, `GetViewInstanceId` and the preferred-size getters — the
  numbers XAML sizes its tree from.

Only one slot stays a stub: 40, `get_TitleBarInputRoutingInfo`, whose
`CoreMessageRoutingInformation` layout is not recoverable from the symbol name, and writing a
guessed struct through the caller's pointer is worse than failing.

Two layouts are now proven rather than assumed, from the implementations themselves:

- `get_VisibleBounds` (RVA `0x61850`) writes `Windows.Foundation.Rect` as `X=0, Y=0`,
  `Width=(float)[this+0x2c0]`, `Height=(float)[this+0x2c4]`.
- `GetViewBounds` (RVA `0x7dc40`) copies nine 32-bit fields from `[this+0x2c0 .. +0x2e0]`, so
  `VIEW_BOUNDS` is `0x24` bytes and its first two fields are the same width/height in pixels.
  The remaining seven (one of them a widened `BYTE` at `+0x1c`) are unidentified and read 0.

### XAML calls OnCloseWindow and OnDestroyWindow, i.e. it is tearing the view down

With the slots named, the trace names the blocker. The last thing the view thread does is:

```
try_configure_core_window ConfigureCoreWindow ... hr 0
combase:CoEndProcessEvents: semi-stub
dataexchange:core_dragdrop_manager_remove_TargetRequested   (mirrors the earlier add_)
wineuwp:nav_OnCloseWindow  core window 00007FFFFEBF5C60
wineuwp:nav_OnDestroyWindow core window 00007FFFFEBF5C60
```

Those are slots 23 and 24 — previously logged only as "INavigationClient slot 23/24: stub", which
read like missing functionality and was in fact XAML reporting that it had just closed and
destroyed the view. Everything that follows in the log is idle thread-pool noise. So the run does
not stall in composition: XAML deinitialises immediately after `ConfigureCoreWindow`, which is why
only one sprite and a 1x1 surface are ever created and why the root sprite's size stays float
noise. It matches the older observation that `FrameworkView::Uninitialize` runs before
`CoreApplicationView::Load` and the lifecycle state never reaches 4.

The host emulates `FrameworkView::SetWindow` by hand (`ConfigureCoreWindow` +
`InvokeOnWindowCreated`) but nothing plays the rest of the shell's sequence: `IFrameworkView::Load`
with the entry point, then the `CoreApplicationView` `Activated` event that makes XAML run
`Application::OnLaunched`. Without `Load`, `RunInternal` finds no activated view and unwinds. That
is the next thing to build, and it is also the `E_APPLICATION_EXITING` mechanism recorded above:
the same teardown sets the lifecycle field to 2 that later makes `AddViewForCurrentThread` fail.

### Corrections to the previous section

`srgb(232,232,240)` in `calc-background-paint.png` is `paint_public_visual`'s own fallback colour
for an unmatched brush, not the app's light-theme background. XAML's actual root brush is a
`CompositionColorBrush` created with A=255 R=255 G=255 B=255 and, in the run captured after that,
painted as `matched 1 color ff000000`. The brush colour XAML puts on the root is not yet a
trustworthy signal, and no on-screen pixel so far proves a theme lookup happened.

`check-visual-content.sh` could not pass as written: it required the word `brush` in
`paint_public_visual`'s trace, which prints `... matched %d color ...`. The gate now matches the
trace that exists. The presentation stage remains flaky run to run (roughly one red in three),
unchanged by this work.

### Verification
`dlls/wineuwp` builds clean. `check-visual-content.sh` reaches
`GREEN: XAML root visual resolved and painted` on runs where the presentation stage goes green.
`nav_OnCloseWindow`/`nav_OnDestroyWindow` reproduce on every run that creates the CoreWindow.

## The view's own startup: Load runs, Activate reaches the dispatcher (2026-09-11)

Following the OnCloseWindow/OnDestroyWindow finding above: nothing was calling the rest of the
shell's view startup, because `ApplicationActivationFactory::Activate` **never returns** — it
blocks until the app exits. Anything queued after it in `do_activate` is dead code.

### Where the missing calls live

`CoreApplicationView`'s private `ICoreApplicationViewInternal` (vtable at twinapi.appcore RVA
`0x1a4ce8`, 13 slots) carries exactly the three the shell uses:

| Slot | Method |
| --- | --- |
| 7 | `SetWindowAndGetDispatcher( ICoreWindowFactory *, ICoreWindow **, ICoreDispatcher ** )` |
| 8 | `Load( HSTRING entryPoint )` |
| 9 | `Activate( IActivationEventCallback *, IActivatedEventArgs * )` |

The interface is cloaked, so `QueryInterface` cannot reach it without an IID that is not in any
header. `dlls/wineuwp` finds the subobject by scanning the object for the vtable pointer instead,
which is a fixed address in the loaded image. (`CoreApplicationView::Activate` opens with
`lea r15,[rsi-0x28]`, so the subobject is at +0x28, but the scan does not depend on that.)

The new `drive_view_startup` runs on the thread whose `DXamlCore` accepted the CoreWindow — the
point the shell would have reached `SetWindow` — and is gated on `WINE_UWP_DRIVE_VIEW`
(`load` = stop after Load, anything else = Load then Activate). Two things had to be fixed to get
that far:

- `ICoreImmersiveApplication::get_MainView` answers `E_ILLEGAL_METHOD_CALL` (0x8000000e) here, so
  the view comes from `CoreApplication.GetCurrentView()` (`ICoreApplication` slot 12) with
  `get_MainView` only as a fallback.
- `try_configure_core_window` returned early when `FrameworkApplication::GetCurrentNoRef` was
  NULL, which is the normal case in these runs, so the startup call never ran.

**`CoreApplicationView::Load(L"App")` now returns S_OK** — the app's `IFrameworkView::Load` has
never run in this project before. Load alone changes nothing visible: the view still gets
OnCloseWindow/OnDestroyWindow, because `Activate` is what raises the Activated event that makes
XAML call `Application::OnLaunched`.

### Activate: callback recovered, then it faults in the dispatcher

`Activate(NULL, args)` access-violates: twinapi QIs the callback at RVA `0x1b87f` with no null
check. The callback interface is small — `IActivationEventCallback`
`{fefdc269-a317-4e33-9f50-c285c9c29725}`, IInspectable plus `ActivationHandled` at slot 6, read off
`??_7ActivationEventCallback@Core@ApplicationModel@Windows@@6B@` (RVA `0x1a45d0`) — so wineuwp
implements it.

With a real callback, `Activate` gets further and then faults inside **`Windows.UI.dll`
`CDispatcher::ProcessMessage`** (+0x46): `mov BYTE PTR [r9],r12b` with `r9 == 3`, i.e. a caller
passed 3 where a `bool *` out parameter belongs (`rdx=2`, `r8=0x20000`). Passing an explicit zero
fourth argument from our side does not change it, so the bad pointer is produced inside the
Windows stack, not by our call shape.

That same fault already happens **without** `WINE_UWP_DRIVE_VIEW`, on the teardown path, so
Activate is not introducing it — it is arriving at it sooner.

### Why the app tears itself down: a stashed E_NOTIMPL

The teardown is XAML deciding its graphics device is lost:

- `CXcpBrowserHost::OnTick` atomically reads-and-clears an HRESULT stashed at `[[r14+0x18]+0x88]`
  (`lock cmpxchg`). When it is a failure it calls `CCoreServices::HandleDeviceLost`, and when that
  does not clear it, records the error and unwinds.
- In the failing runs the stashed value is `0x80004001` (`E_NOTIMPL`). XAML then raises a stowed
  exception (`nested type 0x4c4d4158` = 'XAML') and calls `RoFailFastWithErrorContextInternal2`,
  which our combase deliberately ignores; the app tears down anyway.
- `CCoreServices::DetermineDeviceLost` is *not* the source: breakpoints show it entered (from
  `DeviceLostDispatcher::Execute`, which tail-jumps to it with a NULL out parameter) and returning
  before the branch that could fail. Whoever writes the stash is still unidentified.

One incidental gap turned up in the error path itself: combase logs
`class {00000352-0000-0000-c000-000000000046} not registered`. That CLSID is
**`CLSID_RestrictedErrorObject`** — `CRestrictedError::GetUnmarshalClass` and
`RpcUnmarshalRestrictedError` in the real combase are its only references, and it is not in the
Windows registry either because the real combase handles it internally. Wine has no equivalent, so
cross-apartment `IRestrictedErrorInfo` unmarshalling fails. That is a consequence of the error, not
its cause, but it makes every such failure louder than it needs to be.

### Fixed: the render walk jumped through the end of a vtable

`dcv_visual_collection_vtbl` in `dlls/dcomp` had 11 entries. The render walk calls slot 11, which
read whatever static data followed the array and called it — the recurring
`page fault on execute access to 0000000000000000`. Slot 11 is the collection's **Remove**: it
arrives with the child visual and Insert's two trailing arguments (0x20, 0x30), only while a tree
is being taken apart. It is implemented now, and the rest of the vtable is padded with numbered
probes so the next unknown slot reports itself instead of crashing.

Effect on the checks: `check-visual-content.sh` went from roughly one green in three with recurring
page faults to three greens in a row with none; the remaining reds are the old
pre-activation timing stall, and one later run still hit the `CDispatcher::ProcessMessage` fault
described above.

### Tools added
- `tools/wine-uwp/xrefs.py DLL RVA...` — direct call/jmp callers of an address, named through the
  cached PDB publics. Virtual calls do not show up, so "no direct callers" means "called through a
  vtable", which is itself the answer often enough.
- `tools/wine-uwp/disas.py DLL RVA [len]` — disassemble one function with its call targets named.
  This is what recovered `VIEW_BOUNDS`' size, `Activate`'s missing null check, and the OnTick
  device-lost logic.
- `WINE_UWP_BREAK_TWINAPI` now takes a comma separated list of twinapi.appcore RVAs instead of
  three hardcoded ones.

### Next
The two named blockers, in order:
1. Find what stashes `E_NOTIMPL` in XAML's device-lost slot. Everything else is downstream of it:
   no tick survives, so no layout, no surfaces, no `OnLaunched`.
2. `CDispatcher::ProcessMessage` being handed 3 as an out parameter. Both the teardown path and
   `Activate` land on it.

## The app stops killing itself: WaitForVBlank (2026-09-11)

The stashed `E_NOTIMPL` from the previous section is **Wine's `IDXGIOutput::WaitForVBlank`**.

The chain, every frame:

```
CompositorScheduler::RenderThreadFrame      (windows.ui.xaml)
  -> [scheduler+0x78]->slot 3               = RefreshRateInfo::WaitForRefreshInterval
       -> IDXGIOutput::WaitForVBlank        -> E_NOTIMPL          (dlls/dxgi/output.c)
  <- xchg [scheduler+0x88], hr              (RenderThreadFrame+0x9b2: stash the failure)
CXcpBrowserHost::OnTick                     (UI thread, next tick)
  -> lock cmpxchg [scheduler+0x88]          (OnTick+0xcf: read and clear)
  -> CCoreServices::HandleDeviceLost, does not clear it
  -> record error -> RoFailFastWithErrorContextInternal2
```

Found by scanning XAML's `.text` for the rare `lock cmpxchg [reg+0x88]` encoding — three sites,
one of them `OnTick` — then for the matching `xchg` store, which lands in `RenderThreadFrame`'s
error path; a breakpoint on the call that feeds it named `RefreshRateInfo` from its vtable.

`dlls/dxgi` now sleeps one refresh interval (from `wined3d_output_get_display_mode`, 16 ms if that
fails) and returns S_OK. Returning an error from a call a render loop is paced by is worse than
approximating the wait, whoever the caller is.

Result, with `WINE_UWP_DRIVE_VIEW` unset, three runs each:

| | before | after |
| --- | --- | --- |
| stowed exception / FailFast | 1 per run | **0** |
| `nav_OnCloseWindow` / `OnDestroyWindow` | 1 per run | **0** |
| page faults | intermittent | 0 |
| `CommitMainDevice` | 2 | 5-6 |
| `CreateGraphicsDevice` / sprite visuals | 1 each | 2 each |

The app no longer tears itself down, and the render thread keeps ticking.

### What that exposed: the presentation check was measuring a dying app

`check-presentation.sh` went from 3/3 green to 1/5 green. This is attribution, not noise — the old
binary put back in place scores 2/3 green again.

The explanation is in the traces: in the "green" runs the CoreWindow was created *while the app was
unwinding*. With the app alive, activation now stalls **earlier**, inside
`CoreApplicationViewAgileContainer::ActivateInternal`: breakpoints show it entered once and never
reaching `PrepareToActivateInternal` (RVA `0x285d0`), `CoreApplicationView::Load` (`0x37f80`) or
`CreateCoreWindow` (`0x2c600`). The last twinapi functions reached are `ActivateContract`,
`MakeAndInitialize<ActivationEventCallback>` and `CoreApplication::get_CurrentInternal`; the
alive run loads 64 modules against the dying run's 71, all of the missing ones (`InputHost`,
`UiaManager`, `Windows.UI.Immersive`) downstream of the CoreWindow.

So the milestone the checks were reporting was partly an artifact. That is worth knowing now rather
than after building more on top of it.

`ActivateInternal` branches on a lifecycle state at `[container+0x108]` (`== 2` takes one path,
otherwise another) and on a flag at `[container+0x71]`; state 2 is the one the dying app was in.
Identifying which branch the live app takes, and what it then blocks on, is the next step.

## Where ActivateInternal blocked: a race with the app's own teardown (2026-09-11)

`CoreApplicationViewAgileContainer::ActivateInternal` was not blocked on anything missing. It
reaches, through the container's agile reference to the view,
`ICoreApplicationViewInternal3::CreateCoreWindow` — slot 6 of
`{3e883104-76fa-4a6f-b834-cae9d3891b3c}`, named from
`___x_Windows_CApplicationModel_CCore_CICoreApplicationViewInternal3ProxyVtbl` in
`OneCoreUAPCommonProxyStub.dll`. The container and the view are in different apartments, so that is
a **marshalled** call:

```
ClientRpcChannelBuffer_SendReceive ..., iMethod 32774      (0x8006 -> method 6)
ClientRpcChannelBuffer_SendReceive Calling apartment thread 0x2b0...
CoWaitForMultipleHandles waiting for rpc completion or window message
```

Thread 0x2b0 never answers, because with `+timestamp` the order is plain:

```
489382.225  028c  UninitializeView          <- the container is already tearing the views down
489382.227  028c  running Activate
489382.231  02b0  UninitializeView
489382.231  028c  ...blocked in CreateCoreWindow
```

The activation arrived about two milliseconds too late, and the thread it has to call into is
inside `UninitializeView`. (The `E_POINTER` fail-fast seen on the way out was a symptom: XAML's
`DCompTreeHost::CloseAndReleaseInteropCompositor` calls dcomp's CreateVisual slot during shutdown
with something that is not an out pointer, and our returning `E_POINTER` made XAML fail-fast. It
now returns S_OK, as the NULL case already did.)

The two milliseconds came from our own politeness: `activation_thread` polled
`ICoreImmersiveApplication::get_MainView` every 25 ms and only queued the Activate APCs once it
saw a view — by which time the container had moved on. The fix is to stop waiting: queue the APCs
immediately and let the APC retry the `E_POINTER` that `ApplicationActivationFactory::Activate`
returns before the view exists, once a millisecond instead of once every fifty.

With that, the marshalled `CreateCoreWindow` is serviced, the CoreWindow is created, and
`check-visual-content.sh` reaches **GREEN in 2 of 3 runs** — this time in a live app:

| | teardown-era | now |
| --- | --- | --- |
| stowed exception / FailFast | 1 per run | 0 |
| page faults | intermittent | 0 |
| `presented desktop hwnd` | 1 | 3 |
| `painted public sprite` | 1 | 5 |

Also tried and rejected, so the next attempt does not repeat them: queueing the APC only to the
registering thread (it never enters an alertable wait, and the app exits while we wait for it), and
only letting a thread whose `CoreApplicationView` is not a proxy take the activation (the proxy
that matters is the container's, not ours, so this just made the activation less likely to happen
at all). Both are still in the tree, the first behind `WINE_UWP_ACTIVATE_ON_OWNER`.

## FrameworkView::SetWindow runs for the first time (2026-09-11)

With the activation landing in time, the sequence goes further than it ever has. From a run that
creates the window:

```
CreateCoreWindow            (our window factory, from CoreApplicationView::CreateCoreWindow)
published CoreWindow to the XAML mailbox
nav_PrepareToActivate       (CoreApplicationViewAgileContainer::PrepareToActivateInternal)
window factory QI ICoreWindowFactory   x2      (SetWindowAndGetDispatcher)
```

and then it fail-fasted. The stowed exception named the reason exactly:

```
windows.ui.xaml!CJupiterWindow::ConfigureCoreWindow+0x24e      hr 0x8000ffff
windows.ui.xaml!CJupiterControl::ConfigureCoreWindow+0x30
windows.ui.xaml!DXamlCore::ConfigureCoreWindow+0x4f
windows.ui.xaml!FrameworkView::SetWindow+0x1c
twinapi.appcore!CoreApplicationView::SetWindowAndGetDispatcher+0x37b
rpcrt4 / combase                                (the call is marshalled)
```

`DXamlCore::ConfigureCoreWindow` is the function **this host has been calling by hand** since M2,
as a stand-in for a `SetWindow` that never arrived. It arrives now, and configuring the window
twice is not harmless: the second call returns `E_UNEXPECTED`, XAML stows it, fail-fasts and
unwinds into a stack overflow.

Dropping the manual call (it is now off by default; `WINE_UWP_MANUAL_CONFIGURE` puts it back)
removes the stowed exception and the fail-fast entirely, and XAML gets through its own window
setup — `InputHost.dll`, `UiaManager.dll`, `Windows.UI.Immersive.dll` all load off the back of the
real `SetWindow`. `check-visual-content.sh` stays green 2 of 3, now with `stowed=0` and `pf=0`.

So M2's ConfigureCoreWindow workaround is retired: that milestone's "wire the window into every
thread that has a DXamlCore" hack was compensating for the activation never getting there, and it
became the thing standing in the way.

### The next blocker, one call further on

After `SetWindow` returns, the activation blocks again, and `+ole` names the call:

```
ClientRpcChannelBuffer_SendReceive ..., iMethod 32776      (0x8008 -> method 8)
ClientRpcChannelBuffer_SendReceive Calling apartment thread 0x598...
CoWaitForMultipleHandles waiting for rpc completion or window message
```

Method 8 of `ICoreApplicationViewInternal` is **`Load`**. So `ActivateInternal`'s order is
`CreateCoreWindow` → `SetWindowAndGetDispatcher` → `Load` → `PrepareToActivate` → `Activate`, and
it now dies on `Load`. Thread 0x598 — the view's apartment, the same thread that serviced
`CreateCoreWindow` and `SetWindow`, and the thread our window factory ran on — stops servicing:
it tears its stubs down and exits without answering.

This is the structural problem, not a race: on Windows the shell's activation arrives **on the view
thread**, so `CreateCoreWindow`/`Load`/`Activate` are direct calls. Here the activation runs on
whichever thread takes the APC, so every one of them is a cross-apartment call into a view thread
that only pumps sometimes and stops pumping before `Load`.

Tried and rejected as a discriminator for "which thread is the view thread": a non-proxy
`CoreApplicationView` from `GetCurrentView` (several threads have one), and `DXamlCore::GetCurrent`
being non-NULL (the MAINSTA thread has a DXamlCore too, and the activation still marshalled to
0x598). The activation currently prefers a thread with a DXamlCore and falls back to any thread.

## The activation completes (2026-09-11)

Option 2 above worked. The window factory runs **on the view thread** — it is the thread servicing
the marshalled `CreateCoreWindow` — so `wf_CreateCoreWindow` queues an APC to itself, and the next
time that thread waits alertably it drains its message queue for a few seconds. Wine delivers a
cross-apartment call to an STA by posting to the apartment window, so that is all it takes for the
rest of the activation to arrive.

`ApplicationActivationFactory::Activate` now **returns** instead of hanging: `S_OK` when it beats
the view teardown, `E_APPLICATION_VIEW_EXITING` (0x8000001b) when it does not. That error is
returned by `CoreApplicationView::{CreateCoreWindow, SetWindowAndGetDispatcher, Load, Activate}`
when the container's view state is 6, which `UninitializeView` sets.

Correction to an earlier section: state **2 is the initial state**, set by
`CoreApplicationViewAgileContainer::RuntimeClassInitialize`, not "exiting". Exiting is state 6
(`ViewExiting` tests `[container+0xd8] == NULL || state == 6`). The `ActivateInternal` branch on
"state == 2" is therefore the normal path, not a teardown path.

### Two more gaps this exposed

**`user32.LayoutCompleted` (ordinal 2538, `NtUserLayoutCompleted`) was a stub, so the process
aborted.** With the activation completing, XAML builds a compositor for the activated view and then
calls it — it is the other half of `EnableResizeLayoutSynchronization`, the window telling the
compositor that layout for the current size is done. Nothing here holds a frame back, so it reports
success. After that the app reaches its message loop (`CoMsgWaitInProcessEvents` every 16 ms) and
idles there instead of dying.

**`{99fc44e3-ce9a-4284-bf04-76331d1b1788}` is `IInitializeActivatedEventArgs`, not `ISplashScreen`.**
The real `ISplashScreen` is `{ca4d975c-d4d6-43f0-97c0-0833c6391c24}` (it is in the SDK headers; the
other is private and named from `OneCoreUAPCommonProxyStub`'s proxy vtable). The activation args
answered the private IID with the splash object, giving the caller a vtable of the wrong shape —
and simply failing the QI is worse: the whole activation comes back `E_NOINTERFACE`. So it is
implemented now, shaped from `CActivatedEventArgsBase`'s vtable (twinapi RVA 0x1a6568):

```
slot 6  Initialize( UINT64, HSTRING, ApplicationExecutionState, byte, byte, HSTRING, GUID )
slot 7  InitializeViewDependent( ApplicationExecutionState, int viewId, ISplashScreen *, byte )
```

`InitializeViewDependent` is how the view hands the args their splash screen. It is called on every
run now, and the splash it passes replaces our stand-in.

### Where it stands

On a successful activation the app: creates its CoreWindow, gets a real `FrameworkView::SetWindow`,
completes layout, builds a compositor for the activated view, presents, and sits in its message
loop. `check-visual-content.sh` is green 3 of 3.

What still does not happen is `Application::OnLaunched`: `Microsoft.UI.Xaml.dll` never loads, so the
app has not built its WinUI content, and the visual tree stays at one root sprite.

## OnLaunched runs. The app is stopped by M3's resource blocker (2026-09-11)

Breakpoints on a successful activation (windows.ui.xaml RVAs 0x323f7c, 0x58f9a0, 0x5fd910):

```
FrameworkView::HookCoreActivationEvents   x2      XAML subscribes to Activated
FrameworkView::SetWindow                  x1
FrameworkView::OnActivated                x1      the Activated event reaches XAML
```

And the app's own code runs after it. The `wineuwp` trace of what the activation args are asked
for lines up with `src/Calculator/App.xaml.cs` call for call:

| trace | App.xaml.cs |
| --- | --- |
| `withuser_get_User` | `NavCategoryStates.SetCurrentUser(args.User.NonRoamableId)` |
| `prelaunch_get_PrelaunchActivated` | `OnAppLaunch(args, args.Arguments, args.PrelaunchActivated)` |
| `args_get_splash`, `splash_add_Dismissed` | `args.SplashScreen.Dismissed += ...` (first line of `OnAppLaunch`) |

So **`Application::OnLaunched` runs, and the app gets as far as the first statement of
`OnAppLaunch`**. The next one is

```csharp
var minWindowWidth = Convert.ToSingle(Resources["AppMinWindowWidth"]);
```

and that is where it stops: the trace goes `Bcp47Langs.dll` loads, `RoGetErrorReportingFlags`,
`RoGetMatchingRestrictedErrorInfo 0x80004005`. `Resources[...]` is `Application.Resources`, which
means parsing App.xaml, which merges the WinUI 2.8 theme dictionaries — **the M3 blocker**, the
`ms-appx://Microsoft.UI.Xaml.2.8/...` lookup that returns E_FAIL.

The distance from here to a drawn Calculator is that one resource lookup. Nothing about activation
is in the way any more.

### What the +file trace says about the resource path

- The WinUI framework package's `resources.pri` **is** opened, from the framework package
  directory — the package graph works.
- XAML also probes `<app package>\Microsoft.UI.Xaml.Controls\Themes\generic.xaml` and does not
  find it (`c000003a`, the directory does not exist). That is the app-local fallback path, and the
  path is `Microsoft.UI.Xaml.Controls`, not the `Microsoft.UI.Xaml\Themes\21h1_themeresources.xbf`
  recorded in Findings-M3.
- `stage.sh` now puts the NuGet package's `Generic.xaml` there — it is the full 372 KB dictionary,
  not a design-time stub. **It is not sufficient on its own**: the lookup still ends in E_FAIL, so
  something earlier in XAML's `ms-appx` mapping fails before the file probe can help. The file the
  probe looks for does at least exist now.

### Two more experiments that did not work (both reverted, both noted in the source)

- Retrying `E_APPLICATION_VIEW_EXITING`: `CoreApplication` does not build a fresh view for a second
  activation, and an attempt that blocks before reaching the window factory never gets a view pump,
  so the run hangs instead of failing.
- Queueing the message pump to every thread as the activation starts, so the view thread would be
  pumping for the calls that arrive before `CreateCoreWindow`: it reaches threads that must not
  pump, and no run got a window at all.

## The E_FAIL is not the resource loader (2026-09-12)

`CCoreServices::LoadXamlResource` (RVA 0x183b00) and `TryLoadXamlResourceHelper` (0x1848a4) are
**never called** — breakpoints on both, armed on 18 threads, across runs including ones where the
activation completed. Neither is `FrameworkApplication::LoadComponent` (0x183e38, 0xa743c0) or
`CApplication::LoadComponent` (0x1841d4). So no XAML document is ever loaded, and M3's
`ms-appx://Microsoft.UI.Xaml.2.8/...` failure is **not** what stops the app. `Resources[...]`
returning nothing is consistent with App.xaml never being parsed: `Convert.ToSingle(null)` is 0,
not an exception, and the app carries on.

### Where the failure actually comes from

`WINE_UWP_ERROR_TRACE` (new; makes `RoGetMatchingRestrictedErrorInfo` print the stack behind a
failed HRESULT) puts it exactly:

```
combase!RoGetMatchingRestrictedErrorInfo                 hr 0x80004005
windows.ui.xaml!DelegateTraits<-1>::EnsureStackSnapshot+0x26
windows.ui.xaml!DelegateInvokeHelper<IAsyncActionCompletedHandler,...>::Invoke+0x27
twinapi.appcore!InvokeDelegates<ITypedEventHandler<CoreApplicationView*,IActivatedEventArgs*>>+0x66
twinapi.appcore!CoreApplicationView::Activate+0x33b
rpcrt4 / combase                                          (the Activated event is marshalled)
wineuwp!view_pump_apc                                     (our pump delivering it)
```

So the Activated **delegate** — XAML's `FrameworkView::OnActivated`, which calls the app's
`OnLaunched` — returns E_FAIL, and WRL captures a stack snapshot for it. The error is the app's,
surfaced as an HRESULT; nothing in the activation itself fails. Neither `RoOriginateError` nor
`RoOriginateLanguageException` is called for it, so there is no message to read
(`WINE_UWP_TRACE_ORIGINATE` stays silent).

### A real bug behind it: the launch args had the wrong vtable

`ILaunchActivatedEventArgs` `{fbc93e26}` derives from **IInspectable**, not from
`IActivatedEventArgs`: it has exactly `get_Arguments` (slot 6) and `get_TileId` (slot 7), per the
SDK header. `dlls/wineuwp` repeated `Kind`, `PreviousExecutionState` and `SplashScreen` in it, which
pushed `get_Arguments` to slot 9 — so an app reading its launch arguments called **`get_Kind`** and
had an `ActivationKind` written through its `HSTRING *`. Calculator does exactly that:
`OnLaunched` passes `args.Arguments` to `OnAppLaunch`, which passes it to `Frame.Navigate`.

Fixed, and the fix is visible: `launch_get_Arguments` is called on every successful activation now
and was never called before. The app still fails afterwards, so this was not the only problem.

### How far the app gets

Reading the factory activations in order against `App.xaml.cs`, after `get_Arguments`:

| trace | source |
| --- | --- |
| `ResourceLoader`, `ResourceContext` | `LocalizationSettings` / `NavCategoryStates` |
| `CoreWindow`, `UIContext`, `GetScaleFactorForWindow` | `ApplicationView.GetForCurrentView()` |
| `DesignMode` | `new Frame { ... }` |
| `windows.applicationmodel.dll` loads | `ApplicationData.Current.LocalSettings` |
| `JumpList`, `SHTaskPoolQueueTask`, `AsyncCausalityTracer` | `SetupJumpListAsync` from the splash `Dismissed` handler |

which is essentially all of `OnAppLaunch` up to
`rootFrame.Navigate(typeof(MainPage), arguments)`. `Microsoft.UI.Xaml.dll` never loads and no XAML
document is ever parsed, so `Navigate` — which constructs `MainPage` and runs its
`InitializeComponent` — is where it dies. The app's own code throws
`SystemException("6d430286-...")` when `Navigate` returns false.

### Measurement hygiene: the machine was the variable

Several "regressions" today were not real. A run killed by the harness timeout leaves this prefix's
`explorer.exe /desktop` behind spinning above 100% CPU; nine of them had accumulated, load average
was 13-19, and the activation is a race that a loaded machine loses. `check-visual-content.sh` went
3/3 green -> 0/3 red with no relevant code change, and back to 2/3 green after killing them.
`run.sh` now clears leftovers before each run. **Do not compare activation numbers across a dirty
machine.**

## Nobody loads App.xaml. Load it, and the app builds its UI (2026-09-12)

Checked properly this time, on runs where the activation completed (the earlier "never called"
measurements were taken on runs that never activated, which proves nothing):

- **No XAML document is ever loaded.** `FrameworkApplicationFactory::LoadComponent` (0xa743c0),
  `LoadComponentWithResourceLocation` (0x183c20) and its Impl (0x183d08),
  `FrameworkApplication::LoadComponent` (0x183e38), `CApplication::LoadComponent` (0x1841d4), the
  flat `Application_LoadComponent` export (0x1840d0), `CCoreServices::LoadXamlResource` (0x183b00),
  `TryLoadXamlResourceHelper` (0x1848a4) and
  `XamlNodeStreamCacheManager::GetBinaryResourceForXamlUri` (0x2059c0) — zero hits, all of them.
- **`Application.Resources` is read exactly once** (`FrameworkApplicationGenerated::get_Resources`
  and `get_ResourcesImpl` hit once each), and `get_ResourcesImpl` only reads the Resources
  dependency property; it does not load anything lazily.

So the dictionary is empty when the app asks for `Resources["AppMinWindowWidth"]`, the WinRT map
lookup fails, and the exception comes back out of `OnLaunched` as the E_FAIL traced above.

The string **"App.xaml" does not appear anywhere in `CalculatorApp.dll`** (searched UTF-16 and
ASCII), while `ms-appx:///Views/MainPage.xaml` and its siblings do, and `App.xbf` *is* in the
package's `resources.pri`. So this app's generated `InitializeComponent` does not load App.xaml
and something else is expected to — on Windows it evidently does; here nothing does.

### Doing it ourselves

`dlls/wineuwp` now loads it, with public WinRT calls only: `IApplicationStatics` {06499997}
`get_Current`, a `Windows.Foundation.Uri` for `ms-appx:///App.xaml`, then
`LoadComponentWithResourceLocation(app, uri, ComponentResourceLocation_Application)`. XAML objects
belong to the apartment that made them, so the call is offered to every thread by APC and the one
holding the Application takes it. It is triggered from the window factory, which runs on the view
thread and is still before the Activated event.

**It returns S_OK, and the app then does what it is supposed to:**

```
Microsoft.UI.Xaml.dll loads
Windows.UI.Xaml.Window, Controls.Frame, Controls.Page activated
47 XAML types in total: ResourceDictionary, SolidColorBrush, AcrylicBrush,
XamlCompositionBrushBase, BackdropMaterial, SizeHelper, AutomationPeer, MarkupExtension,
DecimalFormatter, CalendarIdentifiers, ApplicationLanguages ...
```

`OnLaunched` no longer fails: the app reaches `new Frame`, navigates to `MainPage`, and XAML parses
it and the WinUI dictionaries. No stowed exception, no page fault.

### And the app-local generic.xaml was making things worse

With App.xaml loading, one error remained: `E_XAMLPARSEFAILED` (0x802b000a). The originate stack
names it exactly — `XamlScanner::LoadQueue` -> `PullXmlNode` -> **`ReportXmlLiteError`** ->
`ParserErrorService::ReportError` -> `ErrorHelper::OriginateError` — an **XmlLite** failure, i.e.
XAML was parsing a `.xaml` *text* file and the XML parse failed. The only text XAML file in the
package is the one staged yesterday at `Microsoft.UI.Xaml.Controls\Themes\generic.xaml`.

Removing it makes the parse error disappear **and WinUI still resolves** — `XamlControlsResources`
comes out of the framework package's PRI after all. So the M3 fallback was not only unnecessary,
it was actively breaking the parse: XAML finds the app-local file first, hands it to XmlLite, and
fails. `stage.sh` no longer writes it, with the reason recorded there.

Measured, with `generic.xaml` gone: App.xaml loads hr 0, parse errors 0, WinUI loaded, Frame and
Page activated.

## The view thread's shape, and a measurement hazard (2026-09-12)

`dlls/shcore` now traces what `SHCreateThread` was handed, which settles where a UWP view lives:

| | address | what it does |
| --- | --- | --- |
| callback | twinapi.appcore+0x8ef0c | `ContinueViewActivationActivityOnCurrentThread`, then `CoreApplicationViewAgileContainer::InitializeView` — **constructs** the CoreApplicationView |
| thread proc | twinapi.appcore+0xc410 (thunk at +0x97800) | names the thread `"ApplicationView ..."`, then **if the container's flag at +0x70 is set** runs `WaitForViewReady` and `UninitializeView`; otherwise just releases and returns |

Wine's `shcore_thread_wrapper` runs the callback first and the thread procedure after, which is
the documented order. So the view thread creates the view and then waits in `WaitForViewReady` —
which `ActivateInternal` is what finally signals (`SignalViewReady`, RVA 0x29a6a). The teardown is
the *end* of that wait, not a timeout firing early; an activation that arrives after it gets
`E_APPLICATION_VIEW_EXITING`. Earlier notes in this file called the teardown "premature", which is
not the right description.

`PrepareToActivateAsync` — the handshake — is now wired behind `WINE_UWP_PREPARE_ACTIVATE`, along
with a generalised "find this interface subobject by its vtable" helper. It is **unproven**: the
thread that ends up taking the activation is usually in the MTA, where
`CoreApplication.GetCurrentView` answers `E_ILLEGAL_METHOD_CALL`, so there is no view to prepare
from there. Getting the activation onto the view's own apartment is still the prerequisite.

### Two tooling notes worth more than they look

**`run.sh`'s leftover cleanup must be scoped to the prefix.** The first version used
`pkill -f "explorer.exe /desktop"` and `pkill -f "svchost.exe -k"`. Two problems, both real:
an unanchored `-f` pattern also matches *the shell running the command* when the caller's own
command line contains the text, so it killed its own task (exit 144); and the image names are not
unique to this prefix, so it would kill another Wine prefix's processes on the same machine. It is
`wineserver -k` now, which only touches `WINEPREFIX`.

**Activation numbers are meaningless on a loaded machine.** 68 stale `svchost.exe` service hosts
had accumulated from killed runs (`run.sh` clears them now), and this machine also runs other
work — while the measurements below were being taken, load average went from 3.6 to 24 because of
an unrelated Wine workload. Runs that reach `Activate returned 0` roughly one time in three when
the box is idle go to zero in eight attempts when it is not. Check `uptime` before believing a
regression.

## M4: the drawing surfaces are real now (2026-09-12)

XAML draws every texture the same way — `DCompSurface::InitializeSurface` asks
`IDCompositionSurfaceFactory::CreateSurface(w, h, DXGI format, alpha)`, then `DCompSurface::
BeginDraw` calls `IDCompositionSurface::BeginDraw(rect, iid, &object, &offset)`, issues D2D
commands against what comes back, and `EndDraw`s. `dlls/dcomp` answered `BeginDraw` with a
recording stub, so **every drawing command the app issued went nowhere**.

Now a surface keeps the size and format it was asked for and, on first `BeginDraw`:

- creates a D3D11 texture on **the app's own rendering device** — the one passed to
  `CreateInteropDevice`, which is now kept rather than ignored;
- creates a D2D device context for it through `ID2D1Factory1::CreateDevice` and
  `CreateBitmapFromDxgiSurface`, with `D2D1_BITMAP_OPTIONS_TARGET`;
- returns that context QI'd to whatever iid the caller asked for.

`EndDraw` ends the D2D batch. Anything that fails falls back to the old recording stub, so the
change cannot be worse than before. `CompositionSurfaceBrush` now remembers its surface (slots
12/13 of `ICompositionSurfaceBrush`), and the fallback renderer paints a sprite from its surface's
texture — through a staging copy and `SetDIBitsToDevice` — instead of filling it with a guessed
colour.

### Tested without the app: `probe-surface`

The activation race makes app runs an unreliable way to test composition (and `+dcomp` tracing is
heavy enough to lose the race by itself). `tools/wine-uwp/probes/probe-surface.cpp` drives the same
sequence directly — no activation, no CoreWindow, no race:

```
ok   D3D11CreateDevice
ok   Compositor activation factory
ok   ICompositorFactoryInterop
ok   CreateInteropDevice
ok   CreateSurfaceFactory
ok   CreateSurface 64x64 B8G8R8A8
ok   BeginDraw
     update offset 0,0
ok   D2D target 00007FFFFF45C9E0
ok   EndDraw
GREEN: the surface draws
```

The `D2D target` line is the point: it calls `ID2D1DeviceContext::GetTarget` on the object
`BeginDraw` handed back, which only succeeds on a real context with a bitmap bound. `Clear` and
`EndDraw` then succeed against it. This is the first time XAML's drawing path has had somewhere to
draw in this project.

Two argument shapes were fixed while writing the probe:
`IDCompositionDevice2::CreateSurfaceFactory` takes `(renderingDevice, out)` - two parameters, not
three - and the interop device iid XAML asks for is `{d14b6158-c3fa-4bce-9c1f-b61d8665eab0}`.
`IDCompositionDevice::CreateSurface` now makes the same real surface the factory does.

### The pixels reach the window

XAML attaches drawn surfaces through `IDCompositionVisual::SetContent`
(`DCompVisual::SetContentInternal` in windows.ui.xaml), and `dcv_SetContent` stored the pointer
but the renderer only used it to choose a lighter fill colour. `paint_dcomp_visual` now blits the
surface's texture (the surface carries its own size; these visuals often have none).

`probe-surface` was extended to cover the renderer half: after drawing, it creates an interop
visual, `SetContent`s the surface, hands dcomp a window through `__wine_dcomp_set_target_window`,
and reads the pixel back with `GetPixel`:

```
ok   CreateVisual
ok   SetContent(surface)
     pixel at 10,10 = 0000ff
ok   the drawn red reached the window
GREEN: the surface draws and reaches the window
```

with `blit_surface_to_dc blitted surface ... 64x64 at 0,0` and `painted interop visual ... from
surface content` in the trace. **D2D drawing -> D3D11 texture -> interop visual -> fallback
renderer -> window pixel is proven**, deterministically and without the activation race.

### Positioning: transforms and clips

The interop visual table in `dlls/dcomp` was checked slot for slot against `dcomp.pdb`'s
`??_7Api@InteropVisual@Composition@UI@Windows@@6B@` (RVA 0x199150, 52 slots) and matches: offsets
3-6, `SetTransform` 7 (object) / **8 (`D2D_MATRIX_3X2_F`)**, `SetClip` 13 (object) /
**14 (`D2D_RECT_F`)**, `SetContent` 15, `AddVisual`/`RemoveVisual`/`RemoveAllVisuals` 16-18,
opacity 29-30, `SetVisible` 33, `SetWidth`/`SetHeight` 36-37, `GetVisualCollection` 45,
`RealClose` 46. The comments in the table were guesses; they are now confirmed.

Slots 8 and 14 were no-ops, so every surface XAML positioned through a transform landed at its
parent's origin. They are implemented now: the renderer paints with GDI, so only the matrix's
translation is honoured (a non-translation matrix is logged once), and the clip becomes a GDI
clip rectangle around the visual and its children. `probe-surface` checks both:

```
ok   SetTransform translate 100,100
     pixel at 10,10 = c06020, at 110,110 = 0000ff
ok   the surface moved with the transform
ok   SetClip 0,0-32,32
     pixel at 110,110 = 0000ff, at 150,150 = c06020
ok   the clip cut the surface
GREEN: the surface draws and reaches the window
```

Scale followed: the matrix's `_11`/`_22` are kept too, and the surface is stretched with
`StretchDIBits`, so an axis-aligned scale (what any display scale other than 100% puts on XAML's
root) is honoured. Only rotation and skew are still logged and ignored.

```
ok   SetTransform scale 2
     pixel at 100,100 = 0000ff, at 140,140 = c06020
ok   the surface scaled with the transform
```

### Next
1. **Run it in the app.** Drawing, attaching, positioning and clipping are all proven in isolation.
   What is left to see is XAML's own drawing landing in those textures - that needs a quiet
   machine and a won activation race. A batch of app runs at load average ~22 has not activated
   once; do not read that as a result.
2. Still ignored by the renderer: the object forms of `SetTransform`/`SetClip` (slots 7 and 13,
   which take `IDCompositionTransform`/`IDCompositionClip` objects), rotation and skew, the
   surface's `BeginDraw` update offset, and opacity blending.
3. Activation reliability: roughly one run in three on an idle machine.
   `PrepareToActivateAsync` (`ICoreApplicationView_Activation`, twinapi vtable RVA 0x1a4cb0,
   slot 6) would replace the race with a handshake.
4. Why the app's own `InitializeComponent` does not load App.xaml. Loading it from outside works,
   but something about how this .NET Native build expects its XAML to arrive is still not
   understood, and the same question will come up for Shapr3D.
5. `dcomp`'s `uicontext_method` returns E_NOTIMPL for every `Windows.UI.UIContext` slot past
   IInspectable, and it is called on the app's path.
6. `CDispatcher::ProcessMessage` being handed 3 as an out parameter (still open, reachable through
   the explicit `WINE_UWP_DRIVE_VIEW` Activate path).

## Clean-run checkpoint: the surface path has not started (2026-09-12)

A fresh three-run loop was performed after `run.sh` cleaned the stale `explorer.exe /desktop`
processes. One run completed activation and two missed the presentation-target race; the successful
run is `/tmp/wine-uwp-retry-2.log`.

The successful trace confirms the current boundary:

- `OnLaunched` runs and `LoadComponent(ms-appx:///App.xaml)` returns `S_OK`.
- WinUI types and the Calculator XAML dictionaries are activated.
- The compositor creates the root/container/sprite objects and commits them.
- `presented desktop hwnd` and `painted public sprite` are emitted.
- There are **no** `ICompositionGraphicsDevice::CreateDrawingSurface`, `BeginDraw`, or `EndDraw`
  calls. The only surface call is the private 1x1 `CreateSurface` probe during resource setup.

Therefore the next probe is not yet “implement D2D drawing.” It is to identify why Calculator's
post-navigation content never enters the public drawing-surface path. The likely evidence points
are the first layout tick after `rootFrame.Navigate`, `ICompositionSurfaceBrush::put_Surface`, and
the private surface-factory calls after the 1x1 resource probe. Any compositor implementation added
before one of those calls is observed would be speculative.

The first attempt to advance that boundary found a nearer compatibility gap. `CreateScalarKeyFrameAnimation`
was changed from an `E_NOTIMPL` stub to a recording object, and Private5 slot 8 was changed to return
`S_OK`. The old trace showed the factory call followed by `probe_slot_8` returning `E_NOTIMPL` and
XAML stowing `0x80004001`; the rebuilt module compiled successfully. The focused patch is
`tools/wine-uwp/wine-patches/0004-dcomp-accept-scalar-animation-registration.patch`.

The post-change run was not a valid green regression run: activation did not reach the scalar path
within the available attempts. Re-run the visual loop on a clean prefix before treating this fix as
validated. The next useful success criterion is absence of that slot-8 stowed error, followed by
the first post-navigation layout call in the trace.

## Past MainPage: IDWritePrivateTextAnalyzer is not what it looked like (2026-09-12)

An activated run reached `Windows.UI.Xaml.Controls.Page` and then crashed in Wine's dwrite:
a read of address 0x18 inside `AnalyzeScript`, whose "sink" had a NULL vtable.

The earlier guess was wrong: `{b7e6163e-…}` is not a revision of `IDWriteTextAnalyzer`. In Windows 11's
`DWrite.dll` (with `DWrite.pdb`), `??_7?$ComObject@VDWriteTextAnalyzer@@…@@6BIDWritePrivateTextAnalyzer@@@`
(RVA 0x197968) has four slots: IUnknown and then
`GetContentReadingDirection(IDWriteTextAnalysisSource*, UINT32 position, UINT32 length,
DWRITE_READING_DIRECTION*, BOOL *isAmbiguous)`. XAML's `DWriteFontAndScriptServices::Initialize`
keeps it at +0x60, separately from the public analyzer at +0x58. Because Wine answered the IID with the
public vtable, slot 3 landed on `AnalyzeScript`, and the zeroed direction out parameter was used as a sink.

What native does (from disassembly at RVA 0xccdb0): it sets `*dir = 0` and `*ambiguous = TRUE`, returns
E_INVALIDARG for a NULL source or an overflowing range, then reads the paragraph direction. The first
strongly directional character decides LTR or RTL, keeping the paragraph's vertical bits. With none, the
result is the paragraph direction, still flagged ambiguous. Wine now implements exactly that as a separate
object (commit `dwrite: Implement IDWritePrivateTextAnalyzer as its own interface`).

Tooling note: the helper scripts need `~/uwp-bin/venv/bin/python` (pefile), and the native Win11 system
DLLs for comparison live in `~/uwp-bin/win11/Windows/System32`.

Checked without the app by `probe-dwrite`, which makes the same QI and call XAML does:

```
ok   QI IDWritePrivateTextAnalyzer
ok   latin text: dir 0 ambiguous 0
ok   hebrew text: dir 1 ambiguous 0
ok   digits then arabic: dir 1 ambiguous 0
ok   neutral only, ltr paragraph: dir 0 ambiguous 1
ok   neutral only, rtl paragraph: dir 1 ambiguous 1
ok   NULL source is E_INVALIDARG
GREEN: IDWritePrivateTextAnalyzer matches Windows
```

A 20-run app batch after the fix did not activate even once (load average 48). That says nothing
about the fix; re-run on a quiet machine to see what comes after MainPage's text setup.

## First measure pass: IDWriteTextFormatDebug (2026-09-13)

On a quiet machine (load ~2) activation won on run 10 of 10. The dwrite crash is gone: the app got through
MainPage's text setup and into its first layout pass. There XAML fail-fast with E_NOINTERFACE, which
hosted mode logs and ignores, but the measure pass does not finish and nothing is drawn. The stowed
stack, resolved through `windows.ui.xaml.pdb`:

```
CTextBlock::ConfigureDWriteTextLayout+0x16d2
CTextBlock::MeasureOverride → CFrameworkElement::MeasureCore → CUIElement::Measure
CGrid::MeasureCellsGroup / CGrid::MeasureOverride … Page::MeasureOverride … CContentPresenter
```

The failing call (RVA 0x55021) QIs the default `IDWriteTextFormat` from
`DWriteFontAndScriptServices::GetDefaultDWriteTextFormat` for `{28d5197f-3940-4535-b315-e327e13d845b}`,
unless quirk 0x20056 is enabled, and on success calls slot 11 with the language list. In Windows 11's
`DWrite.dll` that IID is `IDWriteTextFormatDebug` (vtable RVA 0x195a10): `Set/GetForceSlowPath`,
`Set/GetForceReshape`, `Set/GetDisableFontFallback`, `GetLocaleNameListLength`, `GetLocaleNameList`,
`SetLocaleNameList`. Wine's text format now has it (commit
`dwrite: Add IDWriteTextFormatDebug to text formats`). The values are stored but not yet used for layout.
`probe-dwrite` checks the QI and the round trips.

Two things in the same run look unrelated to this failure but are unexplained: marshalling
`{18026348-8619-4c7b-b534-ced45d9de219}` and `{5c21c742-2ced-4fd9-ba38-7118d40e966b}` fails (the GIT then
registers the objects directly), and `{00000352-0000-0000-c000-000000000046}` is not registered as a
class (XAML refers to that GUID only as data, at RVA 0xfc99b0).

### Then a use-after-free in Wine's system font collection (unconfirmed cause)

With `IDWriteTextFormatDebug` in place, the next activated run (5 of 5) had no fail-fast but crashed on the
view thread right after `dwritefactory6_GetSystemFontCollection`: a read of address 0x7f64 at
dwrite.dll+0xb6d4. That address is `create_font_collection_from_set` → `get_filestream_from_file` →
`IDWriteFontFile_GetReferenceKey`, and the entry's font file had a garbage vtable (0x7f4c…), meaning a
freed object. The same call succeeded in two earlier activated runs, so it is intermittent.

Everything checked in the ownership path is balanced: builder entries, `fontset_builder_get_entries`,
`fontset_create_from_set`, entry → file references, font file Release. The shared factory is never freed
(its Release returns 1), and XAML creates a shared factory. One real bug was found in
`factory_get_system_collection`. Its cache is a weak pointer read without a lock. When two threads build the
collection at once, the thread that loses the compare-and-swap calls Release on the winner's collection, a
reference it never owned. That can free a collection still in use and release its font set entries twice.
The fix does the lookup and fill under `factory->cs`, only takes a reference while the refcount is non-zero,
and makes `factory_detach_fontcollection` take the same lock.

`probe-fontcollection` (8 threads × 12 `GetSystemFontCollection` on one factory, optionally with a thread
rewriting `HKLM\…\Fonts` ~1,600 times to force system font set rebuilds) stays GREEN even on the
**unfixed** dwrite. So the race is real by reading but not reproduced, and it is not yet shown to be this
crash. The EUDC cache in `GetEudcFontCollection` has the same unlocked pattern and is not changed.

The lock fix is committed (`dwrite: Lock the factory's system font collection cache`). The first batch on
it got 0 of 20 activations at load ~2. That is not a regression. The non-activated logs are identical to
the ones from before the fix (435 lines, same last line, same dwrite calls), because the race is lost
before the app touches dwrite's font collections. Earlier batches on the same setup activated 1 in 10,
1 in 5 and 1 in 10, so activation reliability is now the main thing slowing down each round.

## Activation race: the view thread does not wait for the marshalled CreateCoreWindow (2026-09-13)

Comparing a won run (`gf-act.log`) with a lost one (`gf1.log`) line by line: they match (apart from async
GLSL shader log placement) up to and after `try_activate_once running Activate`, which runs on the main
thread (0290, MAINSTA) *before* the view thread (02b4) exists. The view thread then does
`NtUserInitThreadCoreMessagingIocp2` → `Windows.UI.Xaml.FrameworkView` → `CoBeginProcessEvents`. The
two runs split there:

- won: `CoBeginProcessEvents` → `wf_CreateCoreWindow` on the view thread (the activation's marshalled call
  gets dispatched) → App.xaml, OnLaunched, …
- lost: `CoBeginProcessEvents` → `Windows.UI.Composition.Compositor` → `CoEndProcessEvents` → device
  unregister, `CancelDownload`: the view is torn down and Activate never returns.

Across every log from the day's batches: 39 of 41 lost runs where Activate started show exactly
`CoBeginProcessEvents, Compositor, CoEndProcessEvents`; 2 of 3 won runs show `wf_CreateCoreWindow`
immediately after `CoBeginProcessEvents` (the third one compositor lookup first).

`CoBeginProcessEvents` (combase ordinal 86) was a stub that returned at once, so whether the call got
serviced came down to whether it arrived a moment before or after the thread passed that point. The change
being tested: wineuwp sets a named event `Local\WineUwpActivationPending` before queueing Activate, and
`Local\WineUwpCoreWindowCreated` on entry to `wf_CreateCoreWindow`; hosted-UWP `CoBeginProcessEvents`, while
an activation is pending and no CoreWindow exists, pumps messages and APCs for up to 2 s (once per thread)
until the CoreWindow is created, and logs whether it saw it.

### Narrowing the lost view thread (2026-09-13, diagnostic builds only)

- Both lost-run view-thread `CoBeginProcessEvents`/`CoEndProcessEvents` calls come from windows.ui
  `CDispatcher::WaitAndProcessMessagesInternal` (return addresses +0x82 / +0x12b), with no
  `CoMsgWaitInProcessEvents` in between; won runs pump there and dispatch `CreateCoreWindow`.
- A wait inside hosted-UWP `CoBeginProcessEvents` (until wineuwp's `CreateCoreWindow`) did get the call
  dispatched nested, but the thread still went straight to `CoEndProcessEvents` and Activate failed
  0x8000001b. Reverted.
- Not a WM_QUIT: win32u logging of `NtUserPostQuitMessage`, `NtUserPostThreadMessage(WM_QUIT)` and
  WM_QUIT returned by `NtUserPeekMessage` shows none in three lost runs, although
  `CDispatcher::ProcessMessage` does exit on WM_QUIT (0x1fc90).
- The view thread is started by twinapi.appcore's view proc, which calls
  `CoreApplicationViewAgileContainer::WaitForViewReady` → `Event::WaitAndProcessEvents(ICoreDispatcher*)`
  → dispatcher pump, then `UpdateViewState` and `UninitializeView`.
- The only twinapi `SetEvent` before Activate is `CoreApplication::CreateMainView` setting `CoreApplication+0x258`
  (read by `ActivateApplication`) on the process main thread — present in every run.
- A zero-timeout poll from windows.ui+0x3da61 of two already-signaled handles just before the lost view thread's
  `CoEndProcessEvents` turned out to be a false lead: they are WIL feature-staging semaphores
  (`SM0:<pid>:304:WilStaging_02_p0` / `_p0h`, read by `wil::details_abi::SemaphoreValue::GetValueFromSemaphore`),
  i.e. feature-flag caching. The pump's own handle-exit check (`WaitForSingleObjectEx` at
  `WaitAndProcessMessagesInternal`+0x1b1) never fired, so why the loop ends without pumping is still open.
- Handles and the loop mode are fine: the lost view thread's `WaitAndProcessMessagesInternal` is entered from
  `CDispatcher::WaitAndProcessMessages` (windows.ui+0x1f7ae) with two real, unsignaled, unnamed events (the
  dispatcher's wake event and the view container's ready event), so it is in "pump until the ready event" mode.
  `CoMsgWaitInProcessEvents` is never reached on that thread.
- A diagnostic hook on windows.ui `CDispatcher::ProcessMessage` (its entry is a 5-byte `jmp` into the body at
  +0xd3b8c; wineuwp repoints it at a logger) shows the cause directly: on the lost view thread the **first**
  `ProcessMessage(wait=1)` returns **true** with the dispatcher's quit flag (`+0xc0`) still 0 and both out flags
  0, which sends the loop straight to `CoEndProcessEvents`. In a run where `CreateCoreWindow` was dispatched, the
  first call returned 0 and a later one returned 1.
- Logging windows.ui's own `PeekMessageW`/`GetMessageW` (delay-import slots patched by wineuwp) shows the actual
  difference. **Lost:** the view thread's first `ProcessMessage` peeks a WM_USER (0x400) for combase's apartment
  window (an incoming COM call already queued for the new STA), dispatches it, and returns true → pump ends. **Won**
  (same batch, `Activate returned 0`): the first peeks find nothing, `ProcessMessage` returns false, the loop waits,
  and `CreateCoreWindow` arrives. Next: identify that early incoming call (interface/method) and who makes it.
- combase RPC logging names the early message: it is part of the activation itself. The main thread, in
  twinapi.appcore (+0x2973b, via combase's proxy QueryInterface), makes an in-process cross-apartment
  `IRundown`/`IRemUnknown` call `{00000134-0000-0000-c000-000000000046}` method 3 (RemQueryInterface) on the view
  object, then `{3e883104-76fa-4a6f-b834-cae9d3891b3c}` method 6 (twinapi +0x29759). Wine delivers both as
  `DM_EXECUTERPC` posted to combase's apartment window on the view thread. windows.ui `CDispatcher::ProcessMessage`
  retrieves the first one, dispatches it and returns true, so the pump ends before method 6 is even posted.
  When the first peek finds nothing, the loop reaches `CoMsgWaitInProcessEvents`, whose combase implementation
  dispatches the COM messages itself, and activation proceeds.
- Hypothesis for the fix: on Windows, cross-apartment calls into an application STA (a thread with a registered
  message dispatcher) are serviced through CoreMessaging priority events (`CoMsgWaitInProcessEvents`,
  `CoHandlePriorityEventsFromMessagePump`), not as window messages that the app's `PeekMessage` sees. Queue such
  calls per apartment with a wake event instead of posting them.

### Experiment: queue incoming calls for application STAs (uncommitted)

combase change being tested: a thread that calls `CoSetMessageDispatcher` in a hosted UWP process is marked as
an application STA (`apartment->asta`). Cross-apartment calls into it are queued on the apartment with a wake
event instead of posted as `DM_EXECUTERPC`, at both the in-process shortcut and the RPC-runtime post sites. The
queue is run by `CoMsgWaitInProcessEvents` (the event is added to its wait set),
`CoHandlePriorityEventsFromMessagePump`, and combase's own `CoWaitForMultipleHandles` modal loop (polling
every 16 ms on such threads). `WINE_UWP_NO_ASTA_QUEUE` turns it off. There is no fallback yet for an application
STA that never calls any of those.

### With the queue: first launch activates, next wall is SetFontAxisValues

The first launch on the queued-call combase activated (`Activate returned 0`, CreateCoreWindow, App.xaml, MainPage's
Page, no deadlock). During MainPage's first measure XAML then fail-fast `0x80004001` from
`CTextBlock::ConfigureDWriteTextLayout`+0x14cf, right after `dwritetextlayout4_SetFontAxisValues ... 5, 0:4294967295`:
Wine's `IDWriteTextLayout4::SetFontAxisValues`, `GetFontAxisValueCount` and `GetFontAxisValues` were stubs.
They are now per-range layout properties (a new `LAYOUT_RANGE_ATTR_AXIS_VALUES` on regular ranges, inherited
from the text format's axis values, split/merged like the other range attributes); `probe-dwrite` checks a
sub-range set, the counts inside/outside it, the round trip, and the short-buffer error. The values are stored
but not yet used for font selection or shaping.

Activation with the application-STA queue: **3 of 3** launches activated at load ~2 (each: both threads marked,
`CreateCoreWindow`, App.xaml, MainPage's Page, `Activate returned 0`), against roughly 1 in 10 before on the
same setup and 0 of 20 in the batch just before the change. The batch was stopped after three runs to move on.
All the diagnostics used to find this (win32u WM_QUIT logs, kernelbase wait/SetEvent logs, combase pump/RPC
logging, the wineuwp `ProcessMessage` hook and import patching, the named activation events) have been removed
from the tree; the remaining changes are the combase queue (with queued calls failed with `RPC_E_DISCONNECTED`
if their apartment is destroyed) and the dwrite layout axis values.

### Committed, and the next wall: InMemoryRandomAccessStream

Committed in the Wine tree: `f72f08f dwrite: Implement font axis values on text layouts` and
`d20850d combase: Queue incoming calls for hosted UWP application STAs instead of posting them`.
`probe-dwrite` is GREEN with the axis checks. In three launches on the cleaned-up build all three created the
CoreWindow, loaded App.xaml and MainPage and measured past the TextBlock with no fail-fast, then aborted in the
same place: XAML activates `Windows.Storage.Streams.InMemoryRandomAccessStream`, the prefix's 64-bit WinRT
registration says `DllPath = C:\windows\system32\shcore.dll` (the WOW64 key says `windows.storage.dll`), and
Wine's shcore exports `DllGetActivationFactory` only as a stub, so the process aborts
("unimplemented function shcore.dll.DllGetActivationFactory"). Wine's windows.storage.dll has an implementation
of the class.

Fix under test: Wine's shcore `DllGetActivationFactory` (a stub) now forwards to windows.storage's, which
implements `InMemoryRandomAccessStream`. A scan of the prefix's 64-bit WinRT registrations found other classes
registered (as on Windows) to Wine builtins that have no real `DllGetActivationFactory`; activating any of them
will abort the same way, so they are candidates for the next walls:

| DLL | classes | examples |
| --- | --- | --- |
| shell32.dll | 5 | Windows.Internal.ApplicationDefaults.ApplicationDefaultCategories, Windows.Internal.Storage.ObservableItem |
| thumbcache.dll | 4 | Windows.Internal.Storage.ThumbnailCache.ThumbnailCache |
| uiautomationcore.dll | 4 | Windows.UI.UIAutomation.Core.CoreAutomationRegistrar |
| msxml6.dll | 3 | Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Xsl.XsltProcessor |
| shcore.dll | 2 | Windows.Storage.Streams.InMemoryRandomAccessStream, RandomAccessStream (forwarded now) |
| msftedit.dll | 2 | Windows.UI.Text.ContentLinkInfo, Windows.UI.Text.TextConstants |
| gamingtcui.dll, msvproc.dll, mf.dll, uxtheme.dll | 1 each | Windows.UI.Core.AnimationMetrics.AnimationDescription (uxtheme) |

### Next: DataWriter → FlushAsync on the in-memory stream

With shcore forwarding (`ca159b1 shcore: Forward DllGetActivationFactory to windows.storage`), all three launches
got past the stream activation. MainPage then creates a `Windows.Storage.Streams.DataWriter` (wintypes) over the
`InMemoryRandomAccessStream` and flushes it; `memory_stream_output_FlushAsync` was a stub returning E_NOTIMPL, the
C++/WinRT caller threw from a `noexcept` function (`check_noexcept noexcept function propagating exception`) and
the process terminated. Fix under test: windows.storage gains an `IAsyncOperation<boolean>` (the same
implementation cryptowinrt and windows.gaming.input carry) and the in-memory stream's `FlushAsync` completes with
TRUE; wintypes' `DataWriter::FlushAsync` forwards to its stream. Note that most other `DataWriter` write methods
(`WriteUInt64`, `WriteString`, `WriteBytes`, `WriteBuffer`, ...) are still stubs and are likely next.

### After FlushAsync: LoadedImageSurface and effect parameters

Committed: `d876e9f windows.storage: Implement FlushAsync on in-memory streams`. The stream read now completes and
XAML decodes a `LoadedImageSurface`. Two things follow:

- `DCompSurface::InitializeSurface` (windows.ui.xaml +0x18f396) calls slot 6 of the compositor's
  `{509eab63-6a73-4493-b7fe-80006df879c2}` part, which dcomp only answered with a probe (E_NOTIMPL fail-fast,
  ignored in hosted mode). In `dcomp.pdb` that part's vtable is `CompositorCommon::DCPrivate`
  (`ICompositorPrivate`), slots 3.. = `CreateBackEasingFunction`, `CreateBounceEasingFunction`,
  `CreateCircleEasingFunction`, **`CreateCompositionSurfaceForDCompositionSurface(IDCompositionSurface*,
  ICompositionSurface**)`**, `CreateElasticEasingFunction`, `CreateExponentialEasingFunction`,
  `CreatePowerEasingFunction`, `CreateSharedVisualReferenceController`, `OpenSharedInteractionFromHandle`. (The probe's
  "slot 8 accepted" hack for this IID is therefore `CreateExponentialEasingFunction`, not a registration call.)
  Slot 6 now returns our drawing surface itself, which already serves as the composition surface.
- The process then terminated activating `Windows.UI.Composition.CompositionEffectSourceParameter`, which dcomp did not
  provide. dcomp now has it (`ICompositionEffectSourceParameterFactory::Create(name)`, `get_Name`,
  `IGraphicsEffectSource`; IIDs from the SDK). `ICompositor::CreateEffectFactory` is still a stub and is likely next.
- The first CompositionEffectSourceParameter factory was wrong in a way worth remembering: its QueryInterface answered
  `ICompositionEffectSourceParameterFactory` with the `IActivationFactory` pointer, and the vtable carried `Create`
  after `ActivateInstance`. But `ICompositionEffectSourceParameterFactory` derives from `IInspectable`, so its
  `Create` is slot 6 - exactly where `IActivationFactory` has `ActivateInstance`. C++/WinRT called "Create", landed in
  `ActivateInstance` (E_NOTIMPL, no log), and threw `hresult_error(0x80004001)` from a `noexcept` function
  (`+seh`: `EXCEPTION_WINE_CXX_EXCEPTION`, rbx=0x80004001). A statics or factory interface on a WinRT factory needs
  its own vtable; the same shape exists in dcomp's CompositionCapabilities factory (`GetForCurrentView` behind the
  `IActivationFactory` pointer).
- With the factory fixed, `CompositionEffectSourceParameter::Create` succeeds and the next stop was
  `ICompositor::CreateEffectFactoryWithProperties` (a stub, E_NOTIMPL thrown from a `noexcept` function). Under test:
  dcomp effect factories (`CreateBrush`, `ExtendedError` = S_OK, `LoadStatus` = success) and effect brushes
  (`Get/SetSourceParameter`) on the generic composition-object layout. The effect graph itself is not rendered; the
  sprite renderer looks through an effect brush to the first source brush that has a surface, so images behind
  blur/tint effects still paint. The CompositionCapabilities factory got its own statics vtable too: its
  `GetForCurrentView` had been landing in `ActivateInstance` ("has no instance activation" in earlier logs).

### After effect brushes: a NULL compositor in WinUI

Committed: `4f7d2ac dcomp: Add effect brushes, CompositionEffectSourceParameter and a composition surface for
interop surfaces`. With it XAML gets much further (ToolTip, SplitView, KeyboardAccelerator,
`ElementCompositionPreview`) and then WinUI 2.8's `Microsoft.UI.Xaml.dll` faulted reading address 0 at +0xbda59.
All 38 callers of that function pass cubic-bezier control points (0.167, 0.85, 1.0, ...): it is
`Compositor.CreateCubicBezierEasingFunction` (ICompositor slot 10) on a compositor pointer WinUI had stored as NULL.

Cause: `ElementCompositionPreview` hands the element's interop visual (dcomp's `dcomp_visual`) to WinUI as a
Windows.UI.Composition `Visual`. `dcv_QueryInterface` answered every IID with the interop vtable, so
`ICompositionObject::get_Compositor` (slot 6) ran `dcv_SetOffsetY`, wrote nothing and returned S_OK. The `+dcomp`
trace shows the queries: `IVisual` `{117e202d-...}`, `IInspectable`, the private `{fa92376b-...}`,
`ICompositionObject` `{bcb4ad45-...}`.

First attempt, reverted: answering `IVisual`/`IContainerVisual`/`ICompositionObject` on an interop visual with a
separate public container visual broke XAML immediately (NULL read in
`HWCompTreeNodeWinRT::UpdatePrependVisual`+0x92, main thread, before MainPage). XAML queries every new interop
visual for `IVisual` right after `CreateVisual`, keeps that pointer, sets offset/opacity through it, and later
queries it for `{f2011fdf-3a34-4ec6-ae59-2ddd1bc9b6a1}` (windows.ui.xaml+0xed0e70) and dereferences the result
unchecked. In native dcomp that IID is part 8 of `InteropVisual::s_InterfaceParts`, at the same object offset (0x148)
as `IDCompositionVisual` `{4d93059d-...}` and `IDCompositionVisual2` `{e8de1639-...}` - one more interface served by the
interop visual's own `Api` vtable. Only the interop visual answers it, so `IVisual` must stay the interop visual.

Fix under test: interop visuals keep their compositor (the interop device is the `device_vtbl` member of a
`struct compositor`), and only `ICompositionObject` - the interface WinUI calls `get_Compositor` on - gets its
own small view inside `struct dcomp_visual`: `get_Compositor` returns the compositor, `get_Properties` a property
set, `get_Dispatcher` NULL, `Start/StopAnimation` accepted and ignored, QueryInterface forwarding to the interop
visual. Everything else, including `IVisual`, is still the interop visual.

### Milestone: activation returns S_OK

Committed: `dcomp: Give interop visuals a working ICompositionObject`. On that build, launch 1 activated, created the
CoreWindow, loaded App.xaml and MainPage, went through `ElementCompositionPreview` without the NULL-compositor crash,
and **`Activate returned 0`** - the first complete activation on this path. WinUI then called
`ICompositor::CreateCubicBezierEasingFunction`, a dcomp stub: the E_NOTIMPL surfaced as an unhandled language
exception (`RoReportUnhandledError`, hr 0x80004001) but did not stop the run. Easing functions are next.

Correction to the "nothing drew" reading above: the launches' `painted`/`presented`/`D2D device` counters count dcomp
TRACE messages ("presented desktop hwnd", "painted public sprite", ...), which only appear with `+dcomp`. ERR-only runs
can never show them, so those zeros say nothing about drawing. dcomp's fallback renderer presents when the target window
is set (`__wine_dcomp_set_target_window`, called from wineuwp's `wf_CreateCoreWindow`), when a root visual is set, and
on commits. Whether XAML draws after activation needs a `+dcomp` run.
(Also: a background wait loop written as `while pgrep -f "wine-uwp/run.sh"` matches its own shell command line and
never exits - the same self-match trap as the old `pkill -f`.)

### After easing functions: keyframe animations, and no commits after activation

Committed: `cd83613 dcomp: Implement cubic bezier and linear easing functions`. The `+dcomp` launch shows
`dcv_object_get_Compositor` returning the real compositor, `CreateCubicBezierEasingFunction(0,0.35 / 0.15,1)`
succeeding, and then `CreateVector3KeyFrameAnimation` - still a stub - raising the unhandled E_NOTIMPL. Activate still
returns 0.

Drawing: in that run dcomp presented the CoreWindow four times, all before `Activate returned` and all with no public
root visual, and logged no commit at all (`device_Commit` and the interop device's `CommitMainDevice` both trace and
present). So after activation XAML never committed a frame. Under test: the remaining animation stubs
(Color/Quaternion/Vector2/Vector3/Vector4 keyframe, expression animation, inset clips, `GetCommitBatch`) become
recording objects like `ScalarKeyFrameAnimation`, to see whether XAML's frame loop starts once WinUI's setup no longer
throws.

### After keyframe animations: ICompositor2 slot 11

Committed: `aebb13c dcomp: Implement the remaining keyframe animation, expression animation and inset clip creators`.
In the next `+dcomp` launch `CreateVector3KeyFrameAnimation` succeeds; WinUI queries the animation for its keyframe
interfaces (calls land in generic slots), then asks the compositor for `{735081dc-5e24-45da-a38f-e32cc349a9a0}` - dcomp
answers with a probe - and calls slot 11, whose E_NOTIMPL is the unhandled language exception. In dcomp.pdb that IID
is CompositorCommon's `ICompositor2` part.

Commits: the interop device's `CommitMainDevice` (slot 69) is called 6 times, all during startup (log lines 363-2744),
each followed by a present with no public root. None happen after `Activate returned`, so the post-activation frame
loop still does not commit.

### ICompositor2, and fake compositors from recording objects

Under test: a real `ICompositor2` (slots named from dcomp.pdb, see above). With it WinUI gets past
`CreateImplicitAnimationCollection` into its acrylic setup (`CreateEffectFactoryWithProperties`,
`CreateBackdropBrush`, effect source parameters "Backdrop" and "Noise") and then crashed reading address -1 in combase
`WindowsDeleteString` (string.c:151).

Cause, from the `+dcomp` trace: WinUI queries the effect brush for `ICompositionObject` and calls `get_Compositor`.
Recording objects answer every IID with their generic table, so the call reaches `comp_object_default_slot`, whose
out-pointer heuristic writes a *new recording object* into the "compositor" out parameter. WinUI then treats that as
a compositor, gets garbage back from it, and passes a garbage HSTRING to `WindowsDeleteString`. Same class of bug as
the interop visual's `get_Compositor` earlier. Fix in progress: recording objects (generic, effect brush/factory,
easing functions) answer `ICompositionObject` with a small view that returns the real compositor.
- First version of that fix answered `ICompositionObject` with a view on *every* recording object. That broke startup:
  XAML's `WinRTExpressionConversionContext::CreateExpression` queries its expression animation for
  `ICompositionObject`, and the view's `get_Properties` returned a real property set where the old recording object had
  returned another permissive recording object; XAML's next step failed with E_NOINTERFACE (fail-fast 0x80004002) and
  `UpdateExpression` then read NULL. Narrowed: generic recording objects answer `ICompositionObject` with themselves
  again; only effect brushes, effect factories and easing functions hand out the compositor-aware view. With that,
  startup is back, WinUI gets through `CreateBackdropBrush` twice without the combase crash, and Activate returns 0.
- The underlying mismatch: dcomp's real property set (`propset_QueryInterface`) only answered IUnknown, IInspectable,
  IAgileObject and ICompositionPropertySet, but `CompositionPropertySet` derives from `CompositionObject`. Under test:
  it also answers `ICompositionObject` with the view.

### ICompositor5 for AnimatedIcon

With the narrowed views, the next unhandled E_NOTIMPL comes from WinUI's `Microsoft.UI.Xaml.Controls.AnimatedIcon`: after
`CreatePropertySet`, `CreateExpressionAnimation`, two container visuals and an inset clip, it queries the compositor for
`{48ea31ad-7fcd-4076-a79c-90cc4b852c9b}` (answered by a probe) and calls slot 21. The SDK names that IID `ICompositor5`;
dcomp.pdb's `CompositorCommon` ICompositor5 vtable gives slots 6-25: `get/put_Comment`, `get/put_GlobalPlaybackRate`,
`CreateBounceScalar/Vector2/Vector3Animation`, `CreateContainerShape`, `CreateEllipseGeometry`, `CreateLineGeometry`,
`CreatePathGeometry`, `CreatePathGeometryWithPath`, `CreatePathKeyFrameAnimation`, `CreateRectangleGeometry`,
`CreateRoundedRectangleGeometry`, **`CreateShapeVisual` (21)**, `CreateSpriteShape`, `CreateSpriteShapeWithGeometry`,
`CreateViewBox`, `RequestCommitAsync`. Under test: a real ICompositor5 (recording objects; `CreateShapeVisual` returns a
container visual). `RequestCommitAsync` returns a recording object, which would not complete if anything awaited it.
- Property set answering `ICompositionObject`: tested, no regression (startup normal, no "property set: no interface"
  warnings, Activate returns 0, stops at the known ICompositor5 slot 21). Not committed yet: the ICompositor5 edit
  landed in the same file before the commit ran, so ICompositor2, the narrowed views, the property set and ICompositor5
  will be committed together once the ICompositor5 launch checks out.
- ICompositor5 tested: `CreateShapeVisual` succeeds and Activate returns 0. Committed with ICompositor2, the narrowed
  `ICompositionObject` views and the property-set change as
  `eb9495c dcomp: Add ICompositor2, ICompositor5 and working ICompositionObject views for recording objects`.

### Next: CompositionPath

Right after `CreateShapeVisual`, AnimatedIcon activates `Windows.UI.Composition.CompositionPath` (asking for factory
interface `{9c1e8c6a-0f33-4751-9437-eb3fb9d3ab07}`). dcomp's `DllGetActivationFactory` does not provide it, combase returns
REGDB_E_CLASSNOTREG (0x80040154), and that surfaces as the next unhandled language exception. Still no commits or presents
after activation.

### AnimatedIcon: ICompositionObject4, ShapeVisual, ICompositor6 (Wine e1b9aee + follow-up)

- Crash 1 (`read 0` in Microsoft.UI.Xaml.dll+0x9f597): AnimatedIcon QIs a CompositionSpriteShape for
  ICompositionObject4 `{0bb3784c}` and calls `TryGetAnimationController(L"RotationAngleInDegrees", &c)`,
  then `c->Pause()` (IAnimationController slot 12) with no NULL check. The default slot saw the HSTRING as
  `a` and left the out pointer alone. Recording objects now answer ICompositionObject4 with a small view
  whose slot 6 returns an `AnimationController` recording object.
- Default-slot traces always said "slot 0": MUX calls through CFG (`mov rax,[rax+disp]; call
  [__guard_dispatch_icall_fptr]`), so the decoder now reads the displacement from the `mov`. The NX VEH
  also logs jumps to non-canonical addresses (reported as read faults at the target).
- Crash 2 (`jmp rax`, rax garbage, in Microsoft.UI.Xaml.dll+0x18e9a0): XAML uses the CreateShapeVisual
  result directly as IShapeVisual. Slot 6 hit IContainerVisual::get_Children, and IVector::Append (slot 13)
  read past IVisualCollection's vtable. ShapeVisual now has its own IShapeVisual (`{f2bd13c3}`: Shapes,
  ViewBox) plus a real `IVector<CompositionShape>` (64 shapes, kept but not drawn).
- Then an unhandled E_NOTIMPL (RoReportUnhandledError, no crash) came from `{7a38b2bd}` (was
  "ICompositorPrivate3") slot 10. The native dcomp vftable identifies it as ICompositor6: 6 CreateGeometricClip,
  7 CreateGeometricClipWithGeometry, 8 CreateRedirectVisual, 9 CreateRedirectVisualWithSourceVisual,
  10 CreateBooleanKeyFrameAnimation. Implemented; redirect visuals are plain container visuals.
- Visuals must also answer ICompositionObject4 (AnimatedIcon starts an `IsVisible` BooleanKeyFrameAnimation
  on a visual, then asks it for an animation controller); `visual_qi` hands out the same view.
- State after that: Activate returns 0, no page fault, the UI thread keeps pumping for the full timeout.
  One unhandled error remains: hr 0x80070715 (ERROR_RESOURCE_TYPE_NOT_FOUND). CommitMainDevice still only
  during startup, and nothing painted from the XAML tree.
- The 0x80070715 (ERROR_RESOURCE_TYPE_NOT_FOUND) error: native windows.ui.xaml.dll calls FindResourceExW on
  itself for STRINGTABLE id 0x155 (`LdrFindResource_U module windows.ui.xaml type #6 name #0x155` → type
  not found). On Windows those strings live in `en-US\Windows.UI.Xaml.dll.mui`. The win11 dump in ~/uwp-bin
  has no `.mui` files at all, and Wine's loader does no MUI redirection. C++/WinRT then throws
  `winrt::hresult_error`, reported via RoReportUnhandledError; the process keeps running. It looks like an
  error-message path (text for some other XAML error), so the missing MUI hides the original error text.
  Next: get the en-US MUIs for windows.ui.xaml.dll (and likely others) from a Windows 11 image, then decide
  between MUI redirection in kernelbase FindResourceEx/LoadString and merging the resources into staged copies.
- Fixed (Wine a8b343a): the en-US MUIs were copied from the Windows 11 install on this machine (read-only
  mount of nvme0n1p2; 1808 files into ~/uwp-bin/win11/Windows/System32/en-US; windows.ui.xaml.dll.mui is the
  same 10.0.26100.8972 build as the dump). install-overrides.sh copies the MUIs of genuine Windows DLLs in the
  prefix into system32\en-US. kernelbase FindResourceExW now falls back to `<dir>\<locale>\<name>.mui`
  (UI language, then en-US) mapped as an image resource. LoadResource resolves such entries against the
  MUI mapping. Block 0x155 holds key names ("Home", "Insert", "Left", …) used for keyboard-accelerator text.
  Result: no more 0x80070715 and no unhandled error. Next: a hosted-UWP FailFast 0x80004001 (E_NOTIMPL).

### DirectManipulation viewports and XAML's first hardware render (Wine cce9f96 + follow-up)

- After the MUI fix, a hosted FailFast E_NOTIMPL came from `CDirectManipulationService::CreateViewport` /
  `AddViewportConfiguration` → ScrollViewer. Almost every IDirectManipulationViewport2/Content/PrimaryContent
  method in Wine was an E_NOTIMPL stub (first SetInputMode, then GetPrimaryContent for plain
  IDirectManipulationContent, then GetContentTransform from GetPrimaryContentTransform). Viewports and
  content now keep and return their state: status, tag, rects, 6-float transforms, configurations,
  modes, handler and behavior cookies, snap and zoom settings. No manipulation engine: handlers are never
  called. Committed with only these hunks; another session's uncommitted `direct_manip_Activate` change
  stays unstaged.
- With ScrollViewer working, XAML reaches `HWWalk::RenderContentAndChildren` →
  `CTextBlock::HWRenderContent` → `D2DTextDrawingContext::HWBuildGlyphRunTextures` →
  `DCompSurface::InitializeSurface`. That calls surface factory slot 7. The native CSurfaceFactory vtable
  is 3 CreateSurface, 4 CreateVirtualSurface, 5 CreateGradientSurface, 6 Flush, 7 a private
  `CreateSurface(w, h, format, alpha, bool, out)`. Wine's default slot left the out pointer NULL: an
  E_INVALIDARG FailFast, then XAML QI'd the NULL surface for {5680c979} (dcomp's surface interop IID,
  used by CompositionDrawingSurface::RecreateUnderlying) and faulted. Slots 6 and 7 are now implemented.
- Next wall in `D2DTextDrawingContext::HWBuildGlyphRunSingleTexture`: XAML QIs the surface for
  {5680c979}, which is **IDCompositionSurfaceInternal** (native `??_7CCompositionSurface@@6BIDCompositionSurfaceInternal`):
  3 BeginDraw, 4 EndDraw, 5 SuspendDraw, 6 ResumeDraw, 7 Scroll (same as IDCompositionSurface), then
  8 BeginDrawWithGutters(rect, iid, out, offset, gutters), 9 BeginDrawWithClear, 10 HasValidPixels,
  11 Resize, 12 CopySurface. XAML calls slot 8 with iid ID2D1DeviceContext {e8f7fe7a}. Wine's default slot
  took the rect pointer for an out pointer and wrote an object into it; the real out stayed NULL and
  XAML faulted. Slots 8–12 now forward to the existing BeginDraw. The gutters pointer is an int-sized
  local in XAML, so it is never written. Resize drops the texture so the next BeginDraw recreates it.
- Drawing target: XAML's `CreateInteropDevice` rendering device is not an ID3D11Device (the WARN in
  `remember_rendering_device`), so every BeginDraw fell back to a recording object. dcomp now imports
  d3d11 and creates its own hardware (or WARP) D3D11 device when it has none. D2D text drawing then works:
  35 BeginDraw/EndDraw pairs on the first frame.
- ICompositor4 is `{ae47e78a}` (public, in Windows.UI.winmd; was "ICompositorPrivate6"): 6/7 color gradient
  stops, 8 CreateLinearGradientBrush, 9–11 spring animations. `LinearGradientBrushMap::GetBrush` faulted
  on the probe's unwritten out pointer. Implemented as recording objects.
- BeginDraw with iid IDXGISurface {cafcb56c} (XAML atlas and image uploads) got the recording object,
  whose ID3D11Resource QI then went into d3d11 CopySubresource and crashed in wined3d. BeginDraw now
  hands out the surface texture for any interface the D3D11 texture supports.
- Cross-device textures (Wine 7ae6db0): with dcomp's own D3D11 device, XAML copied into BeginDraw
  textures with its own device context, and wined3d aborted on
  `cs->thread_id == GetCurrentThreadId()` from another device's command stream thread. The pointer
  dcomp treated as CreateInteropDevice's "rendering device" is really XAML's
  `DCompInteropCompositorPartnerCallback` (NotifyDirty/NotifyDeferralState; the device argument is
  NULL). XAML passes its **ID2D1Device** to `CreateSurfaceFactory`. dcomp now takes the D3D11 device
  behind it (ID2D1Device2::GetDxgiDevice) and reuses that D2D device. Result: 88 surface draws,
  53 texture handoffs, no assertion.
- ICompositor7 is `{d3483fad}` (Windows.UI.winmd): 6 get_DispatcherQueue, 7 CreateAnimationPropertyInfo,
  8 CreateRectangleClip, 9 CreateRectangleClipWithSides, 10 CreateRectangleClipWithSidesAndRadius.
  `HWCompTreeNodeWinRT::UpdateRoundedCornerClipVisual` faulted on the NULL from an unknown-interface QI.
- Still open: `CDirectManipulationService::CreateSharedContentTransformForContent` QIs Wine's
  DirectManipulation compositor for `{59f4c99d}`, an interface defined only inside
  Windows.UI.Xaml.dll (hosted FailFast E_NOINTERFACE, ignored).
- State after Wine 05c723a (ICompositor7): no page fault, no assertion; 88 surface draws, 109
  "painted public sprite" and a commit that presents again. The UI thread keeps pumping for the
  full timeout. Two ignored hosted FailFasts remain (E_NOINTERFACE: DirectManipulation `{59f4c99d}`
  and one more to identify).
- Screenshots: the host session is locked, so captures show the lock screen. `run.sh` now has
  `UWP_VDESKTOP=WxH` (runs through `wine explorer /desktop=uwp,WxH`), but Wine's explorer.exe crashes
  in `get_default_show_systray` (desktop.c:961, registry read) when creating the named desktop. The
  working setup is a separate headless X display: `Xvfb :98 -screen 0 1280x800x24`, then
  `env -u WAYLAND_DISPLAY DISPLAY=:98 tools/wine-uwp/run.sh`, and `import -display :98 -window root`.
  Display :99 belongs to an older Xvfb (since Sep 11) and is left alone.
- Xvfb only offers llvmpipe OpenGL. There wined3d's GL path hits GLSL errors ("Illegal non-directive
  after #", link failure), then crashes intermittently in ucrtbase `pf_printf_a`/`memchr` on the wined3d
  render thread while dumping shader info with garbage GL values. Not an app bug.
  `WINE_D3D_CONFIG=renderer=vulkan` (RTX 5090 Vulkan, no display needed) usually gets through; a 60 s
  run on :98 finished cleanly.
- Paint crash (read 0x1 in `paint_public_visual` → IInspectable_QueryInterface on a child): the
  IVisualCollection embedded in each visual kept its own refcount. When it dropped to 0,
  `collection_Release` released every child but left `count` alone, so the painter walked freed
  children. The collection now forwards AddRef/Release to its owning visual, and `visual_release`
  releases the children.
- Grey boxes: 88 of ~114 sprites use a CompositionMaskBrush (text and glyph icons: an A8 mask surface plus
  a colour source), still a recording object. It is now a typed brush: ICompositionMaskBrush
  {522cf09e} with 6 get_Mask, 7 put_Mask, 8 get_Source, 9 put_Source on its own vtable, and a view for
  ICompositionObject. The painter reads the mask (A8 or BGRA alpha) and colour or surface source
  back from D3D11 and composites with GdiAlphaBlend.
- Black headless screenshots: the hosted CoreWindow (hwnd 0x1006C) is a visible top-level WS_POPUP
  (style 0x94000000, exstyle 0x200000 = WS_EX_NOREDIRECTIONBITMAP), 1280x800 at 0,0, with no child windows
  and a normal clip box. win32u/winex11 do not special-case WS_EX_NOREDIRECTIONBITMAP, so a window
  surface should exist. Still diagnosing: GetPixel after paint, and the X window behind the hwnd.
- Intermittent crash in dwrite `get_filestream_from_file` (read 0x7f76, IDWriteFontFile_GetReferenceKey)
  from `create_font_collection_from_set`, right after "remote fonts are not supported" in
  dwritefactory6_GetSystemFontCollection. A font set entry's file pointer is stale. Not yet
  investigated.
- Desktop run: the CoreWindow shows up as a 1280x800 window. It held only dcomp's blue backdrop, although 109
  sprites "painted". A GetPixel readback after each FillRect gave 0 of 74 matches: every GDI fill after
  the backdrop was clipped away. Cause: IDCompositionVisual slot 14, which dcomp implemented as
  `SetClip(const D2D_RECT_F *)`, is called by XAML with an **ICompositionClip object** (the InsetClip it
  just created). MSVC lays out the SetClip overloads in the reverse of IDL order. Reading the object as
  floats produced a 0,0-0,0 clip, and `paint_dcomp_visual` applied it with IntersectClipRect to everything
  below. The first fix (treat a COM object pointer as "no clip") never triggered: the clip data reads as
  0,0 followed by pointer bytes as floats (right 1.65e-21, bottom 4.57e-41 on 31 of 104 calls; also NaN
  and -9.6e11). Slot 14 now ignores any non-finite or sub-pixel rectangle and clips only with a sane
  rectangle. TODO: check the other overload pairs (SetOffsetX/Y, SetTransform) for the same swap.
- After the clip fix the window was solid black: the last fill of every present was a 1280x800 black
  sprite. Z-order: `collection_insert` appended for InsertAtBottom/InsertBelow/InsertAbove. XAML uses
  InsertAbove 112x, InsertAtBottom 40x, InsertAtTop 10x. Children now go to their real positions.
  Presenting: `present_dcomp_visuals` painted every parentless IDCompositionVisual (about 20 roots) in
  creation order, while `CreateTargetForHwnd` returned a recording object that dropped
  `IDCompositionTarget::SetRoot` (slot 3). XAML's target root is the second tree created
  (3 children). The target now records SetRoot and the presenter paints only that root, falling back to
  all roots before SetRoot.
- Layout: every button sprite paints at 0,0 (236x82). Only 11 of 87 painted sprites got a non-zero public
  `put_Offset`. The private InteropVisual setters (native `Api@InteropVisual` vtable: 3/4 SetOffsetX
  animation/float, 5/6 SetOffsetY, 7/8 SetTransform object/matrix, 13/14 SetClip, 29/30 SetOpacity,
  36/37 SetWidth/SetHeight, 38-41 relative offset/size) are all `visual_interop_ok` no-ops in Wine
  dcomp. Next step.
- With only the target root presented, just 7 sprites painted. Two more problems. (1) Root [0] belongs to
  another view: it was created on thread 0294 (not the UI thread 02b8) and has its own full-window black
  sprite; excluding it is correct. (2) An IDCompositionVisual stored at most one public child
  (`public_child`, overwritten by every AddVisual of a WinRT visual), separately from its interop
  children, so most of the tree and all public-vs-interop z-order were lost. Children are now one ordered
  list of {interop visual | public visual}. AddVisual honours referenceVisual (above/below it); without a
  reference, insertAbove puts the child at the bottom and !insertAbove at the top. The private
  collection Insert (slots 3/8, which pass a non-pointer 0x20 "reference") appends. Remove and
  RemoveAll and the iterator handle both kinds.
- InteropVisual setters, from the native `Api@InteropVisual` vtable with full signatures: 3/4 SetOffsetX
  (animation, float), 5/6 SetOffsetY, 7/8 SetTransform (IDCompositionTransform*, const D2D_MATRIX_3X2_F&),
  13/14 SetClip (IDCompositionClip*, const D2D_RECT_F&), 15 SetContent, 29/30 SetOpacity, 31/32 SetTransform
  3D, 33 SetVisible(BOOL), 36/37 SetWidth/SetHeight, 38-41 relative offset/size, 45 GetVisualCollection.
  Wine's public-visual interop view now implements 4, 6, 8 (translation only), 30, 33, 36 and 37 on
  the visual's own offset, size, opacity and visibility, and the painter adds the translation.
- The rest of the UI hangs off separate IDCompositionVisual trees that XAML inserts into **public**
  IVisualCollections (e.g. `collection_insert_at owner 104C01C0 InsertAbove child 1046FD30`).
  `paint_public_visual` only descended into children answering the private interop IID, so those trees
  were skipped, and they were never marked as parented (so they showed up as extra roots). Public
  collections now mark inserted interop visuals as parented, and the painter paints them with
  `paint_dcomp_visual`.
- Why the window stayed black: an exact-address ancestry walk shows every "missing" sprite does reach
  the target root, all through public visual 2A2870. That visual's collection first gets XAML's black
  window-background sprite (Size 0,0, InsertAtBottom, index 0). Much later the whole content tree is
  inserted with InsertAtBottom too, so content goes below it. On Windows a size-0 sprite with no
  relative size draws nothing. The painter's "size < 0.5 → fill the parent" fallback turned it into a
  full-window black cover. Sprites of size 0 are no longer painted (containers still pass the parent size
  on). InteropVisual 38-41 (SetRelativeOffsetX/Y, SetRelativeWidth/Height) are now implemented and the
  painter scales them by the parent size.
- **Root cause of the lost layout:** XAML queries its interop visuals for `{117e202d}`, which is
  **Windows.UI.Composition.IVisual** (native: part of `VisualCommon::s_InterfaceParts`). It then drives
  them through IVisual's slots. `dcv_QueryInterface` answered every IID with the IDCompositionVisual
  vtable, so IVisual 14 `get_Clip` ran `SetClip(rect)` (the "garbage clip rects": an uninitialised out
  pointer read as floats), 15 `put_Clip` ran `SetContent` (the RectangleClip/InsetClip "content"),
  36 `put_Size` ran `SetWidth` (width 3.4e38), and `put_Offset` (21), `put_Opacity` (23),
  `put_IsVisible` (19) and `put_TransformMatrix` (38) hit silent no-ops. Every button therefore had
  no position or size. Interop visuals now answer IVisual with a dedicated view that writes offset,
  size, opacity, visibility, clip and transform into the visual. `dcv_from_any` recognises all three
  interface pointers (IDCompositionVisual, IVisual, ICompositionObject) wherever children are resolved,
  indexed, inserted into public collections or painted. `{fe93b735}` is
  IDCompositionVisualPartnerWinRTInterop (our IID_IInteropVisual_private).
- With IVisual fixed the button grid gets real positions (memory buttons 78x51 at y=237, keypad
  237x83 from 4,290). The debug fill `paint_dcomp_visual` gave sized interop visuals without surface
  content (mid blue) now covered the UI, so it is gone.
- Other IIDs XAML queries on interop visuals, identified from the XAML functions that reference them:
  `{af843985}` ILayerVisual (84x, `HWCompTreeNodeWinRT::UpdatePrimaryVisual` / `GetHandOffLayerVisual`),
  `{ef874ea1}` ICompositionObject2 (22x), `{f2011fdf}` IDCompositionVisualRestricted (private),
  `{83112a11}` a private brush/expression partner, `{af86e2e0}` IInspectable, `{fa92376b}`/`{e8de1639}`/
  `{92179cc0}` private partners. ILayerVisual now fails with E_NOINTERFACE: native interop visuals are
  not layer visuals, and XAML branches on the result. ICompositionObject2 gets a view (Comment,
  ImplicitAnimations, animation groups not run), because its get_ImplicitAnimations (slot 8) used to
  land on SetTransform(matrix).
- Button content: a 78x51 memory-button interop visual first holds a public visual with the button's
  sprites. XAML removes it (collection Remove) and inserts a leaf interop visual (through its IVisual
  view). The log shows only GetVisualCollection and put_Clip on that leaf, so its content has to arrive
  through calls that land on the silent `dcv_ok` stubs. `dcv_ok` now logs the vtable slot it was called
  through (decoded from the call site), to find them.
- `{f2011fdf}` IDCompositionVisualRestricted (native `CVisualProxyBase<IDCompositionVisualRestricted>`,
  rva 0x198c80): slots 3-44 have the same parameter types as IDCompositionVisual/InteropVisual (some
  names are folded template stubs, e.g. "SetScaleX" at 3/4 and "SetRoot" at 13), then 45/46
  get_VisualRelativePixelsToScreen...(float*), 47 EnablePixelSnapping(BOOL), 48 ProtectContents(BOOL),
  49 RedirectMouseToHwnd, 50/51 SetTransformParent2. Wine answers it with the IDCompositionVisual
  table, whose slot 45 is GetVisualCollection (writes a pointer). A slot-45 call through Restricted
  would write 8 bytes into a float out-parameter: a candidate for the intermittent heap write crash.
  {c12a8aa7} IDCompositionVisualPartner2 is used by XAML for hit-test visibility.
- **Button content path:** the collection XAML gets from an interop visual's GetVisualCollection is a WinRT
  **IVisualCollection** (3-5 IInspectable, 6 get_Count, 7 InsertAbove, 8 InsertAtBottom, 9 InsertAtTop,
  10 InsertBelow, 11 Remove, 12 RemoveAll). Slot 9 was a logging probe, so XAML's 33 InsertAtTop calls
  (button content) were dropped. Slot 8 appended where InsertAtBottom must prepend, and slot 3
  (GetIids) was treated as an insert. The collection vtable now follows IVisualCollection.
- The `dcv_ok` no-op stubs are barely used (5 calls, all at startup): slots 5, 43 SetTransparentForInput,
  47 EnablePixelSnapping (through IDCompositionVisualRestricted). XAML's call sites there are
  `mov rax,[rax+disp]; call <guard thunk>` (e8 rel32), which the decoder now handles.
- Intermittent startup crash (2 of the last 5 runs): write fault at a page boundary in kernelbase
  `PackageIdFromFullName` (version.c:2346) on the startup view thread. That file carries another session's
  uncommitted changes. Not investigated here.
- With the IVisualCollection fix, XAML's 33 InsertAtTop and 112 InsertAtBottom calls attach children: 119
  public sprites visited, 109 painted, 35 through mask brushes. The capture shows the button grid as grey
  boxes. Two remaining causes. (1) Colour fills ignored alpha: 18 of 19 solid fills were transparent
  (A=0, XAML's unset backgrounds and hover overlays) but painted opaque. They are now skipped, partial
  alpha is blended, and unresolved brushes are no longer painted as grey placeholders. (2) All 53 mask
  brushes that could not paint use a **CompositionNineGridBrush** as the mask (27 with a colour source,
  26 with another nine-grid brush as source): XAML's rounded button backgrounds.
- ICompositionNineGridBrush {f25154e4}, native `Api@CompositionNineGridBrushT` vtable (rva 0x195c18):
  6/7 BottomInset, 8/9 BottomInsetScale, 10/11 IsCenterHollow, 12/13 LeftInset, 14/15 LeftInsetScale,
  16/17 RightInset, 18/19 RightInsetScale, 20/21 Source, 22/23 TopInset, 24/25 TopInsetScale, 26 SetInsets,
  27 SetInsetsWithValues, 28 SetInsetScales, 29 SetInsetScalesWithValues. It is now a typed brush,
  returned from CreateNineGridBrush as the ICompositionNineGridBrush interface. The mask painter resolves
  a nine-grid mask to its source surface, samples it with nine-grid mapping at the sprite size, and treats a
  nine-grid colour source as that colour.
- `comp_object_caller_slot` also decodes XAML's `mov rax,[rax+disp]; call <guard thunk>` call sites (e8
  rel32). Before this every NineGridBrush call was logged as "slot 0".
- The IIDs XAML queried on (recording) nine-grid brushes: `{ab0d7608}` ICompositionBrush (107x),
  `{ad016d79}` ICompositionSurfaceBrush (53x) and `{d27174d5}` ICompositionSurfaceBrush2 (53x), both from
  `WUCBrushManager::GetSurfaceBrush`, which asks every brush. A native nine-grid brush is not a surface
  brush, so the typed nine-grid brush now refuses both with E_NOINTERFACE instead of answering with
  itself.
- Activation stalls are back at roughly 1 in 3 desktop runs (hu53, hu55, hu57): `try_activate_once` runs,
  but "Activate returned" never appears, no CoreWindow target is set, and only the startup CommitMainDevice
  calls happen, so the run times out without painting. These empty runs are the pre-existing activation
  race, not rendering regressions. Test chains now use a 240-300 s timeout and only capture a window
  whose WM_CLASS is `calculatorapp.exe` (one capture grabbed an unrelated X window that had reused a stale id).
- **Build toolchain changed under us (2026-09-13 22:33):** `make` re-ran configure (`--no-create
  --no-recursion`) because another session's uncommitted `configure`/`configure.ac` changes were newer than
  `config.status`. The regenerated build uses `x86_64_CC = x86_64-w64-mingw32-gcc` instead of clang, so raw
  MSVC-style `__try`/`__except` in dcomp stopped compiling. dcomp now uses Wine's portable
  `__TRY`/`__EXCEPT_ALL`/`__ENDTRY` macros everywhere. Other modules with raw `__try` could hit the same
  thing on their next rebuild. The other session's configure changes were left alone.

### Milestone: Calculator renders its UI (2026-09-13)

Screenshot: `docs/LinuxPort/screenshots/calculator-first-render-2026-09-13.png` (desktop run, dcomp built with
MinGW GCC). What shows: the "Calculator [Dev]" title, the "Standard" mode header, the "0" display, the History
and Memory tabs, the M+ / M− / MS memory buttons, the full keypad grid laid out correctly, and the "=" glyph.
The run painted 63 sprites, skipped 46 transparent fills, and composited 62 mask brushes (nine-grid rounded
corners included). Still wrong:
- Keypad and memory-clear buttons paint as solid white rounded rectangles with no digits. 26 mask brushes
  still fail because their source is a **CompositionLinearGradientBrush** (a recording object), which
  XAML uses for the control elevation border.
- Only the startup presents happen; there is no frame loop after activation yet.
- **White buttons:** XAML creates all 27 of its colour brushes with the no-argument
  `Compositor::CreateColorBrush()`, never `CreateColorBrushWithColor`, and sets no colour animations
  (only IsVisible and Opacity). Native `Api@Compositor::CreateColorBrush` (dcomp rva 0x71c0) allocates,
  `memset`s the object to zero, then runs the CompositionObject, CompositionBrush and CompositionColorBrush
  constructors, none of which sets a colour. So the native default is **transparent black**. Wine defaulted
  to opaque white, which turned XAML's unset fills into the solid white keypad buttons. The default now
  matches native, and `put_Color` is traced so any later colour changes show up.
- The remaining 26 failing mask brushes have a CompositionLinearGradientBrush source (6/7 EndPoint,
  8/9 StartPoint on top of CompositionGradientBrush: 10 get_ColorStops, 11/12 ExtendMode, ..., 23/24
  TransformMatrix). The gradient stops are near-transparent white (alpha 0x18 and 0x12): the control
  elevation border. Not implemented yet.
- Chasing the intermittent `PackageIdFromFullName` write fault (memset of the caller's buffer, version.c:2346):
  - With `WINEDEBUG=+ver`: 65 calls, mostly size probes with a NULL buffer. The few real calls pass
    heap buffers at odd addresses 4 bytes before a page end (e.g. `…4FFC`), with the length pointer on
    the main thread's stack. The first write then runs into the next, unmapped page (fault at page+0x4C).
    That run did not crash itself (the tracing changes the timing).
  - The loaded native importers are not it: MrmCoreR (two call sites, stack buffers of 0xF0/0x12E bytes)
    and twinapi.appcore (`ComponentActivationPrivate::GetComponentVersion`, 0xF0-byte stack buffer).
  - `+relay` with `RelayInclude=kernelbase.PackageIdFromFullName` logs nothing: native-to-builtin calls
    do not go through relay thunks. The registry value was removed again afterwards.
  - Next: dcomp's vectored exception handler temporarily unwinds write faults and logs module+offset
    frames, to name the caller.
- **Root cause of the intermittent startup crash (write fault in `PackageIdFromFullName`):** kernelbase
  `GetCurrentPackageInfo2`. MrmCoreR's `MetroAppClientProfile::CheckSystemPackage` calls
  `GetCurrentPackageInfo(0x80010, &size, NULL, &count)`, allocates exactly `size` bytes, and calls again.
  The size pass counts the PACKAGE_INFO array, each package's path, full name and family strings, and the
  PACKAGE_ID size. The fill pass also writes each package's **publisher string**
  (`get_package_publisher` from AppxManifest.xml, or "CN=Wine") with `pkg_put_string`, and that string is
  never counted. With the Calculator package plus its framework dependencies, the offset runs past the
  heap block, and the next package's `PackageIdFromFullName` memset lands on an unmapped page. The
  publisher lines are **uncommitted working-tree changes from another session** in `dlls/kernelbase/version.c`,
  so they were not edited here. Fix: in the size pass, look up each package's publisher the same way
  (`get_package_publisher(pkgs[i].path, ...)`, falling back to "CN=Wine") and add
  `(lstrlenW(publisher) + 1) * sizeof(WCHAR)` to `needed`.
- Confirmed with an unwound stack at the fault: kernelbase `PackageIdFromFullName` (+0x71a0c) ←
  kernelbase +0x72198 (GetCurrentPackageInfo2, version.c:1799) ← mrmcorer.dll +0x352fb
  (`IProfileHelpers::GetCurrentPackageInfo`) ← mrmcorer …
  ← windows.ui.xaml.dll +0x51e30a. The temporary stack-walk diagnostic was removed from dcomp again.
- **Black window after the transparent-default fix:** with the default brush colour fixed, the window
  background sprite gets its real colour, opaque black via put_Color. It was painted three times in the
  final present, the last paint covering all content. XAML inserts that sprite into the same container
  three times with `InsertAtBottom` and never removes it in between (0 failed Removes). dcomp appended a new
  entry each time, so a later `InsertAbove` of content (at 1 of 3) ended up below two stale copies. On
  Windows a visual has one parent and re-inserting moves it, so both the public IVisualCollection insert and
  the interop visual's child list now move an existing entry instead of duplicating it.
- Committed the rendering work as Wine `439083a` (dcomp IVisual views, IVisualCollection, move-on-reinsert,
  target root, mask and nine-grid brushes, alpha fills, transparent default colour, portable SEH; combase
  `__TRY`). Screenshot: `docs/LinuxPort/screenshots/calculator-render-2026-09-13-b.png` (MC/MR now dimmed;
  keypad still solid white).
- **White keypad buttons:** XAML sets the button mask sources to opaque white with put_Color, on purpose. The
  sprites carrying those masks have `put_Opacity` 0.0852 (and 0.125 for others), with all ancestors at 1.
  That opacity is what makes Windows' dark-theme button wash. `paint_mask_brush` ignored the visual's
  opacity and composited the white at full strength, which also hid the white digit glyphs. It now takes
  the sprite's opacity as GDI's constant alpha. Opacity animations (StartAnimation "Opacity") are still not
  run.

### Milestone: Calculator's full Standard UI renders (2026-09-13 23:00)

Screenshot: `docs/LinuxPort/screenshots/calculator-full-ui-2026-09-13.png`. Dark theme, "Standard" header,
"0" display, History/Memory tabs, MC/MR dimmed, M+/M−/MS, and the full keypad with glyphs (% CE C ⌫ ¹/x x² ²√x
÷ × − + ± . and digits) on correctly tinted buttons. Wine commit for the last fix: sprite opacity applied to
mask brushes. Still open:
- Startup crash in about half of runs: another session's uncommitted `GetCurrentPackageInfo2` publisher
  sizing in kernelbase version.c (see above).
- No frame loop after startup (only startup presents), animations not run, and input/interaction untested.
- LinearGradientBrush (control elevation borders), "=" button accent fill.
- The "There's no history yet." text overlaps the title bar area at the top left (history pane layout).

### After the full render: no response to clicks

- Clicking "7" then "8" in Calculator's window changed nothing: the present count stayed at 5, and dcomp
  logged no activity at all after the last startup present. Two gaps:
  1. **Frame scheduling:** `CreateInteropDevice` discarded XAML's `DCompInteropCompositorPartnerCallback`
     (3 NotifyDirty, 4 NotifyDeferralState), so the compositor never told XAML that composition was dirty. dcomp
     now keeps the callback and calls NotifyDirty after CommitMainDevice (click test pending).
  2. **No pointer input:** win32u converts mouse input to WM_POINTERDOWN/UPDATE/UP only when
     `enable_mouse_in_pointer == 1`, which only `EnableMouseInPointer(TRUE)` sets (default -1 = off). Native
     windows.ui.dll's CoreWindow reads input through `GetPointerInfo` and never calls EnableMouseInPointer:
     on Windows, mouse-in-pointer is on for UWP processes. Plan: call `EnableMouseInPointer(TRUE)` from wineuwp
     when it creates the CoreWindow (win32u carries another session's uncommitted changes and is not edited).
- Click tests with real XTEST input (not `xdotool --window`, which sends synthetic events), using
  `+win`: `EnableMouseInPointer(1)` takes effect (the wineuwp change is committed). On the UI thread (02b0)
  the clicks hit the CoreWindow (0x1006c), win32u creates pointer id 1 (type 0x4, mouse), and updates and
  finds it 7 times. windows.ui also calls `EnableMouseInPointerForWindow(hwnd, 1)` (a user32 stub that
  returns TRUE). XAML still makes no composition calls after the clicks: no commit, present or
  NotifyDirty. The 7 lookups pair one-to-one with the updates, so they are win32u's own message-retrieval
  path. Whether WM_POINTERDOWN/UP reach CoreWindow's window procedure is being checked with `+msg`.
- dcomp keeps XAML's partner callback and calls NotifyDirty after CommitMainDevice (10-11 calls per
  run). There has been no visible effect yet, so it stays uncommitted.
- `+msg` after the real clicks: the UI thread (02b4) peeks raw WM_LBUTTONDOWN/UP and 257 WM_MOUSEMOVE for the
  CoreWindow 0x1006c. win32u converts them and delivers WM_POINTER* with a synchronous `send_message`, which the
  msg channel does not trace. CoreWindow's pointer events are raised by `WindowServer::OnPointerDownEvent` /
  `OnPointerUpEvent` / `OnPointerRoutedToEvent`, whose only caller is **`WindowServer::WndProc`** (windows.ui
  rva 0xcb00, call sites +0x968/+0xa65/+0x1176). It compares the message with 0x245 (WM_POINTERUPDATE) and dispatches the
  pointer range, so this is a window-procedure path. Open question: whether the CoreWindow 0x1006c, which
  receives win32u's WM_POINTER sends, runs that window procedure. The window-server syscalls `NtUserSetCoreWindow`,
  `NtUserSetCoreWindowPartner` and `NtUserRegisterPointerInputTarget` are stubs in Wine and may matter if it does not.
- Side note: `CoMsgWaitInProcessEvents` (combase asta.c, wine-uwp hack) posts WM_NULL to every thread window on
  every wait: 4,435 posts in a few seconds after the clicks. Pure overhead; to be reduced.
- **Answered:** the CoreWindow's window procedure *is* `windows.ui!WindowServer::WndProc` (a temporary diagnostic in
  `__wine_dcomp_set_target_window` logged `wndproc = windows.ui.dll+0xcb00`). A temporary diagnostic in win32u
  `process_mouse_message` showed a real WM_POINTERDOWN (wp 0x20160001, pointer id 1) sent to the CoreWindow and
  returning 0. In WndProc's jump table, WM_POINTERDOWN/UP/ENTER/LEAVE/WHEEL (0x246/0x247/0x249/0x24a/0x24e)
  all go to the same handler at +0x88 (rva 0xcb88) as the keyboard messages 0x100-0x104. WM_POINTERUPDATE (0x245)
  has its own branch.
- ~~Debug channels hang startup~~ (wrong): the runs with `+loaddll` / `+msg` that never created the CoreWindow all
  hit wineuwp's any-thread activation fallback (see "Startup hang explained" below). Plain runs hit it too.
- WindowServer's pointer path, statically: `WndProc` sends every WM_POINTER* to a second jump table (message - 0x215, byte table
  rva 0xe0e0, offsets 0xe08c). WM_POINTERDOWN goes directly to `OnPointerDownEvent` (rva 0xb330), UP to 0xd536, ENTER to
  `OnPointerEnterEvent`. `OnPointerDownEvent` does nothing if the CoreWindow's PointerPressed event source has no
  subscribers (`GetSize` == 0), or if `CoreInputBase::PopulatePointerEventArgs` fails. windows.ui imports the
  wmpointer apiset (mapped to user32), and two of its imports were **missing from Wine's user32**:
  `GetPointerDevice` and `GetPointerInputTransform`. Added: GetPointerDevice fails with ERROR_INVALID_PARAMETER,
  as it does for a mouse on Windows; GetPointerInputTransform returns identity matrices. A temporary ERR in
  `NtUserGetPointerInfoList` shows whether the pointer data is queried after a click at all.
- **Startup hang explained:** with WINE_UWP_ACTIVATE_ON_VIEW, wineuwp waited only 750 ms for a view thread (one with a
  DXamlCore) to take the Activate APC, then let any thread have it. In every saved log that reached that fallback
  (7/7), Activate ran on an STA (apartment type 1, qualifier 1) and the CoreWindow was never created. Every run
  where the view thread (ASTA, type 3) took it created the CoreWindow unless it crashed first. The APC is used up
  on threads without a DXamlCore, so wineuwp now re-queues it every 500 ms for up to 10 s before falling back.
- XAML picks its CoreWindow input path in `CJupiterWindow::UseCoreWindowSubclassedEvents` (windows.ui.xaml rva 0x4eac8).
  It returns TRUE (subclass the CoreWindow and read WM_POINTER in `CoreWindowSubclassProc`) only if
  `XamlOneCoreTransforms` is initialised and disabled, `CQuirksMode2::m_quirksDisabledForProcess` is clear, and
  `QuirkIsEnabled(0x20046)` is nonzero. Wine's `QuirkIsEnabled` stub returns FALSE, so XAML uses the non-subclassed
  branch of `RegisterCoreWindowEvents` and subscribes to CoreWindow's typed events (KeyDown etc.).
- A click test after the activation fix: the view thread (ASTA) took the activation, the CoreWindow was created, and
  WM_POINTERDOWN/UP pairs reached it (returning 0). But `NtUserGetPointerInfoList` was never called, so
  `OnPointerDownEvent` stopped before `PopulatePointerEventArgs`. That is the `GetSize() == 0` path: no PointerPressed
  subscriber on the CoreWindow event source it checks. The run later died at a null read (0x6ffffa5b7ad8) right
  after an ignored FailFast, E_NOINTERFACE. At startup XAML had also asked Wine's directmanipulation compositor for
  {59f4c99d-084e-46b9-ad3b-c9776f453163} and its primary content for {d8042f40-a051-4fee-875e-243b7b38d66e}; both are
  unnamed GUIDs in windows.ui.xaml.pdb.
- Probe right after PrivateCreateCoreWindow (temporary, in wineuwp): the CoreWindow reports `IsInputEnabled = 1`, and
  `add_PointerPressed` with a logging handler succeeds, so the event source accepts subscribers. (The run crashed before
  the click, so it is not yet known whether the handler fires.)
- The two unknown IIDs XAML asks Wine's directmanipulation for, found through RIP-relative references in windows.ui.xaml:
  - {59f4c99d-084e-46b9-ad3b-c9776f453163} is requested from the *compositor* by `CDirectManipulationService::CreateViewportInteraction`,
    `RemoveViewportInteraction`, `CreateSharedContentTransformForContent`, `CreateSharedContentTransformsForOverpanReflex` and
    `CInputServices::CreateViewportInteractionForRootVisual`. It is a private DManip compositor interface for DComp-side
    "viewport interactions" (slot 3 takes (viewport, IDCompositionDesktopDevicePartner, out IUnknown**); slot 4 is used for
    the root visual). On E_NOINTERFACE, ForRootVisual fail-fasts (ignored under Wine) and carries on.
  - {d8042f40-a051-4fee-875e-243b7b38d66e} is requested from the primary content by `CDirectManipulationService::SetPrimaryContentTransform`.
- The recurring startup crash (read of a small address at `...b704`, two runs, same offset) follows
  `dwritefactory6_GetSystemFontCollection remote fonts are not supported`, but that FIXME is harmless: Wine still returns
  the local collection. The faulting module is being identified with `+loaddll`.
- **Crash at windows.ui.xaml `CUIElement::ForceCloseHandInAndHandOffVisuals+0x268`** (null read, reproduced with `+loaddll`):
  for each handed-in/off visual XAML queries `Windows.Foundation.IClosable` {30d5a829-7fa4-4026-83bb-d75bae4ea99e} and calls
  `Close` (slot 6). dcomp's public visuals (`visual_qi`) returned E_NOINTERFACE, so XAML fail-fasted (ignored) and called
  through NULL. dcomp's interop visuals would have answered with the IDCompositionVisual vtable (their catch-all QI),
  whose slot 6 is a setter. Both now have an IClosable view with a no-op `Close`; the visuals are refcounted, so there
  is nothing to release early.
- A native `directmanipulation.dll` (from the Windows partition, copied to ~/uwp-bin/win11) contains the private
  compositor IID {59f4c99d} and primary-content IID {d8042f40}. It is not used yet: it imports undocumented user32
  ordinals (2503-2505, 2516, 2539, 2551, 2587, 2691), ninput ordinals 2500/2505/2507 and dcomp ordinal 2503, which Wine does not provide.
- **Pointer input reaches CoreWindow events once someone subscribes.** With a logging `PointerPressed` handler added
  by wineuwp (temporary probe), every WM_POINTERDOWN → `WindowServer::OnPointerDownEvent` → `GetPointerInfo`
  (NtUserGetPointerInfoList) → handler invoked (17/17 presses in one run). `add_PointerPressed` and
  `OnPointerDownEvent` use the same event source (`WindowServer+0x4e0`). So windows.ui's path works; XAML just
  had not subscribed when the earlier clicks arrived.
- XAML's CoreWindow input wiring (`CJupiterWindow::RegisterCoreWindowEvents`): pointer handlers
  (`OnCoreWindowPointerPressed` etc., CoreWindow slots 38-49) are registered only when
  `XamlOneCoreTransforms::IsEnabled()`, which caches user32 `IsOneCoreTransformMode()` (Wine returns TRUE for
  packaged apps, needed for the composition island). Otherwise XAML subclasses the CoreWindow only if
  `QuirkIsEnabled(0x20046)`; that quirk is never queried in our runs.
- **The view tears itself down about 10 s after the UI renders** (24 s after launch in a timed run). It happens with
  no crash, no click and no WM_QUIT. Teardown starts ~800 log lines after `Activate returned`
  (`direct_manip_Deactivate`, `PowerSettingUnregisterNotification`, `RegisterTouchHitTestingWindow(hwnd, 0)`) and ends
  in `CJupiterWindow::~CJupiterWindow` → `nav_OnCloseWindow`. The last presented frame stays on screen meanwhile.
  twinapi.appcore's view thread (fn around rva 0xc410): `WaitForViewReady`; if view state == 4 it sets 5 and calls
  the view's run (slot 13 on `[container+0xd8]`), then `CoReleaseServerProcess` and `UninitializeView`.
  XAML's `FrameworkView::Run` → `DXamlCore::RunMessageLoop` → `CJupiterControl::RunMessageLoop`. For window type 2 that
  goes to `CJupiterWindow::RunCoreWindowMessageLoop` → `ICoreDispatcher::ProcessEvents(ProcessUntilQuit)`; otherwise it
  runs a plain `GetMessageW` loop that exits on 0/-1. In windows.ui, `ProcessEvents(UntilQuit)` returns only once
  `CDispatcher+0xc0` is set. The only writer besides the ctor/reset is `ProcessMessage`'s WM_QUIT check after a
  `GetMessageW`, which is taken only when msctf `HasDeferredInputForCoreDispatcher` is TRUE (Wine: FALSE). Neither
  WM_QUIT diagnostic (win32u PostQuitMessage/PostMessage/PostThreadMessage, combase asta dispatch) fired. A
  `NtUserGetMessage` -1/WM_QUIT diagnostic is the next check.
- **Recurring startup crash located:** Wine's dwrite `create_font_collection_from_set` (font.c:4830) →
  `get_filestream_from_file` → `IDWriteFontFile_GetReferenceKey` on a fontset entry whose `file` has a garbage vtable
  (reads 0x7fc0/0x7fce). Entry refcounts are atomic, so this looks like the IDWriteFontFile being released while a
  system fontset entry still references it (use-after-free), not a torn entry. Not fixed yet.
- Begin/End process-events diagnostic (windows.ui base 0x6ffff9340000): the view thread's only pumps are
  `CDispatcher::WaitAndProcessMessagesInternal` (+0x1f8b2 / +0x1f95b) — once until ~line 3050 and once until
  immediately after `Activate returned` (69 waits) — i.e. twinapi's `WaitForViewReady`. **No ProcessEvents pump
  starts afterwards**: XAML's `FrameworkView::Run` returns (or is skipped) before `ProcessEvents(ProcessUntilQuit)`,
  and teardown follows. No GetMessage -1/WM_QUIT either.
- **Teardown cause narrowed, and a working experiment.** vtable hooks (temporary, wineuwp): XAML's
  `FrameworkView::Run` is entered early (before activation completes) and calls `ProcessEvents(ProcessUntilQuit)`,
  which returns S_OK ~30 log lines after `Activate returned`. Run then returns and twinapi uninitializes the view.
  In `WaitAndProcessMessagesInternal`, the only exit is `ProcessMessage` returning TRUE, which only happens when
  `CDispatcher+0xc0` is set; no direct writer besides ctor/reset and the WM_QUIT path was found, so the setter is
  still unknown. Re-entering `ProcessEvents(2)` when it returns S_OK (`WINE_UWP_REPUMP=1`, 5 re-entries in a run)
  keeps the view alive: no teardown, the UI stayed on screen past +30 s, and a real click produced PointerPressed
  events. The calculator display did not change after clicking "7", though.
- **Clicks throw in the app:** with the view kept alive (re-pump), each pointer press is followed by
  `primary_QueryInterface({d8042f40-a051-4fee-875e-243b7b38d66e}) not found` in Wine's directmanipulation and then
  `RoOriginateLanguageException 0x80004002 "Specified cast is not valid."` (58 after a few clicks, 0 before the first
  press). XAML's `CDirectManipulationService::SetPrimaryContentTransform` queries that IID on the primary content
  and calls slot 12 (`+0x60`) with a 6-element matrix and count 6, a transform setter. A `{d8042f40}` view has been
  added to directmanipulation's primary content: QI/AddRef/Release plus S_OK stubs for slots 3-15, which trace their
  slot number on `+manipulation`. Click test pending.
- With the `{d8042f40}` primary-content view, the InvalidCastExceptions after clicks are gone (0 casts, 0
  `not found`, no fault, no close, 9 PointerPressed events for two real clicks on "7" and "8"). The display still
  showed 0 afterwards: either XAML does not route the press to the button, or the change is not recomposed.
- **Redraw is the blocker, not proven input loss.** Counting dcomp presents (temporary ERR): 8 at startup, still 8
  after two real clicks, and still 8 after a 1-px resize nudge of the Calculator window. XAML never presents again
  after startup, so nothing that happens after the first frames (clicks, resizes) can become visible. Whether the
  clicks change the calculator state is unobservable until XAML's frame loop runs.
- **XAML's frame tick path (why presents stop at 8):** `UIThreadScheduler::RequestAdditionalFrame` → `QueueTick`,
  which queues only when its state `[this+0x30]` is 0 (1 = queued, 2 = in `BeginTick`, reset in `EndTick`) and calls
  `CXcpDispatcher::QueueTick` (slot 13 on `[this+0x10]`). That allocates a node with message 0x402, appends it to a
  queue at `CXcpDispatcher+0x38/+0x40` under an SRW lock, and only if the queue was empty calls slot 10 on
  `[CXcpDispatcher+0x98]` with (0, 0) to wake the dispatcher. If that wake is lost or the queue is never drained,
  later ticks just append and nothing renders again; this matches presents freezing after startup. Next step: find
  the wake object and the drain function, and check at runtime which half stops.
- The dispatcher wake is a CoreMessaging timer. `CXcpDispatcher::Init` creates the object at `+0x98` with
  `MessageTimerCallbackStatic` as its callback. That callback takes one queued node under the SRW lock, dispatches it
  (message 0x402 → tick), and re-arms the timer (slot 10 with due time 0) if more nodes are queued. A due-now
  CoreMessaging timer that never fires therefore freezes XAML's frame loop for good. combase's
  CoMsgWaitInProcessEvents already notes that CoreUI's own wake does nothing under windows.ui's wait, and adds a
  proxy wake event (`Local\WineUwpCoreMsgWake`) for due-now timers.
- Proxy-wake count (temporary ERR in combase CoMsgWaitInProcessEvents): **0 proxy wakes in the whole run**. Presents
  stayed at 9 before and after a click. So the kernelbase SetWaitableTimerEx due-now pulse never fires in the
  frame-loop phase: either XAML never re-arms its tick timer after startup, or CoreMessaging arms timers through
  another API.
- XAML's tick timer, concretely: `CXcpDispatcher::Init` calls `IMessageSession::CreateTimer` (CoreUI session at
  `+0x90`, vtable slot 18) with `MessageTimerCallbackStatic` and stores the `IMessageTimer` at `+0x98`, then
  calls slot 5 with 0. `QueueTick` / the callback arm it with slot 10 = `IExportMessageTimer::SetRelative(0, 0)`.
  CoreMessaging does not import `SetWaitableTimerEx` at all (it uses SetWaitableTimer, SetThreadpoolTimer, user32
  SetTimer and wait completion packets). So the kernelbase SetWaitableTimerEx proxy-wake hack cannot cover this
  timer, which fits 0 proxy wakes.
- **Likely root cause of the frame-loop freeze, in the wait-completion-packet code** (dlls/ntdll/sync.c, another
  session's uncommitted change, not edited here). CoreMessaging waits on its timers through
  `NtCreateWaitCompletionPacket` / `NtAssociateWaitCompletionPacket` / `NtCancelWaitCompletionPacket`, and arms them
  with `SetWaitableTimer` (`TimeoutManager::SetTimerDelay`; due 0 for SetRelative(0,0)). Wine's
  `NtAssociateWaitCompletionPacket` returns STATUS_INVALID_PARAMETER when `packet->wait` is set ("already associated").
  `packet_wait_callback` (a WT_EXECUTEONLYONCE RtlRegisterWait callback) posts the completion and clears `pending`,
  but **never clears `packet->wait`**. After a packet has fired once, re-associating it therefore fails unless the
  caller cancels first. On Windows a delivered packet is unassociated and can be associated again directly. So
  every CoreMessaging timer can fire only once, which would stall XAML's tick timer after startup.
  Suggested fix (for the owner of that code): when the callback delivers the completion, also clear the registration,
  e.g. `wait = InterlockedExchangePointer( &packet->wait, NULL )` and deregister it after the callback returns (not
  with a blocking RtlDeregisterWaitEx from inside the callback). Then a new association after delivery succeeds.
- CoreMessaging re-associates wait completion packets on every wait cycle: `NtAssociateWaitCompletionPacket` is
  called from `WaitController::StartWaitCycle` and `Win32EventLoopBridge::StartWaitCycle`, while
  `NtCancelWaitCompletionPacket` is only used by `WaitCompletionPacket::TryCancel`. So the missing
  `packet->wait` reset in Wine hits every repeating CoreMessaging wait. First timer hook run: hooks were installed
  after XAML had created its tick timer, so its callback was not wrapped; 30 SetRelative(0,0) re-arms of one timer
  showed up in a burst just before activation returned, then none after a click (the log is rate-limited, so this is
  inconclusive). Next: a wineuwp IAT hook on CoreMessaging's `NtAssociateWaitCompletionPacket` that cancels and
  retries on STATUS_INVALID_PARAMETER.
- **Wait-completion-packet test inconclusive.** The run with the wineuwp IAT hook on CoreMessaging's
  `NtAssociateWaitCompletionPacket` never got past the splash screen (2 presents, blue screenshot), so its
  "0 failed associations" says nothing about the frame loop. The hook only logs failures, so it is also unknown
  whether any association happened after it was installed. To repeat: log every call, and check the UI rendered first.  Cause of that failed run: with the IAT hook installed early (log line 536), activation "returned" at line ~3000
  instead of ~14000 and XAML never loaded its content (the log stopped at 3088 lines, with the view pumping). The hook
  itself probably disturbed CoreMessaging's wait cycle, so the experiment needs a pass-through-only variant before
  its results can be trusted.
- **Wait-completion-packet hypothesis ruled out (clean run).** Pass-through IAT hook, installed at CoreWindow
  creation: startup rendered normally (activation at line 14181, 7 presents), and CoreMessaging made exactly **one**
  `NtAssociateWaitCompletionPacket` call (success) across startup plus a click. It does not re-associate packets per
  wait cycle in this configuration, so the missing `packet->wait` reset cannot explain the freeze. (The earlier stalled
  run was a random startup failure, not the hook.) Presents stayed at 7 after the click. Next lead: install the
  CoreMessaging timer hooks as soon as coremessaging.dll loads (before XAML's `CreateTimer`). Then count XAML's
  tick timer re-arms (`SetRelative(0,0)`) against its callback firings, to see whether the timer fires but the
  callback is not dispatched, or is never re-armed.
- **The CoreMessaging tick timer works.** With the timer hooks installed at coremessaging.dll load
  (LdrRegisterDllNotification), XAML's `CreateTimer` was wrapped: every `SetRelative(0,0)` re-arm was followed by
  exactly one callback fire (30/30, in lockstep, through activation). So CoreMessaging timers, wait completion
  packets and the dispatcher wake are all fine. The freeze is upstream: after the last startup tick, **XAML stops
  re-arming** (no further `RequestAdditionalFrame` → `QueueTick`). That means either the click never dirties the
  tree (input not routed to the button), or the dirtying never requests a frame. Unrate-limited re-run pending.
- **Input IS routed; the stall is in composition, not XAML.** Unrate-limited timer trace: 30 tick arms/fires through
  startup, then after one real click on "7" another 104 arms and 104 fires (XAML animating the button press), yet dcomp
  presents stayed at 8. So XAML ticks and renders after the click; either it never commits to dcomp, or our dcomp
  commits without presenting. Next: trace dcomp Commit / CommitMainDevice / present after the click.
- `+dcomp` trace across a click: **not a single dcomp call after the click** (0 of any kind), while XAML ticked
  104 times. So the render walk bails inside XAML before touching composition. Candidate gate: `DCompTreeHost::
  CommitMainDevice` returns early when `[this+0x188]` (device) is NULL, and when a pending-commit flag at
  `[this+0x280]` (guarded by a sync object at `+0x278`) is set unless the `force` argument is TRUE — i.e. XAML waits
  for the previous commit to complete and we may never signal completion.
- To locate where the post-click ticks die without more static reading, wineuwp now has a DIAG entry-hook facility
  (`WINE_UWP_DIAG_XAML=1`): 5-byte jmp patches on `CCoreServices::Tick` (rva 0x1c6b28), `NWDrawTree` (0x2ade0),
  `DCompTreeHost::CommitMainDevice` (0x2bc07c), `HandleDeviceLost` (0x196b30) and `RecoverFromDeviceLost`
  (0x1c7ff0), each re-running its copied prologue from a trampoline. Log-only; temporary.
- **Entry-hook run:** startup = Tick 6, NWDrawTree 6, CommitMainDevice 2, presents 7, **HandleDeviceLost 2**.
  After the click: Tick 0, NWDrawTree 0, CommitMainDevice 0 — even though CoreMessaging fires XAML's tick timer
  every time. So the post-click ticks are dropped between `CXcpDispatcher::MessageTimerCallbackStatic` (which calls
  `CXcpDispatcher::WindowProc` with message 0x402) and `CCoreServices::Tick`. Two device-lost events during startup
  are the prime suspect for a "rendering disabled / waiting for device" state; logging the HRESULT next.
- **Found the layer.** `CXcpDispatcher::WindowProc` hook (hwnd 0x20062, GWLP_USERDATA intact): startup delivers six
  `0x402` tick messages (→ `OnReentrancyProtectedWindowMessage` → `Tick`), after the click only `0x403` (26) and
  `0x418` (24) — never `0x402`. So input is processed (deferred invokes run) but **`UIThreadScheduler` never queues
  a tick again**: `QueueTick` only queues when its state `[this+0x30]` is 0 (1 = queued, 2 = in BeginTick, cleared in
  EndTick). A tick that unwound without `EndTick` (e.g. an ignored FailFast inside the render walk) leaves it stuck.
  Hooking the scheduler to read that state.
- **ROOT CAUSE of the frozen frame loop.** Scheduler hooks: ticks 1-6 cycle Queue→Begin→Tick→End normally; tick #7
  (the first after activation) gets `BeginTick` but never `EndTick`, and every later `RequestAdditionalFrame`
  (252 at startup, all after the click) arrives in state 2 ("in tick") and is parked until an EndTick that never
  comes. Inside tick #7 the stowed E_NOTIMPL stack is: `CXcpBrowserHost::OnTick` → `CInputServices::
  ProcessUIThreadTick` → `InitializeDirectManipulationContainers` → `ScrollViewer` manipulation handler →
  `CInputServices::NotifySecondaryContentAdded` → `CDirectManipulationService::AddSecondaryContent+0x2f7` →
  `IDirectManipulationManager::CreateContent` (riid IDirectManipulationContent) — **Wine's stub returns E_NOTIMPL**,
  XAML fail-fasts (ignored by our combase) and the tick unwinds without `EndTick`. Fix: implement `CreateContent`
  in dlls/directmanipulation (return a content object; XAML only needs the IDirectManipulationContent methods to
  succeed).
- Fix applied (dlls/directmanipulation, uncommitted): `IDirectManipulationManager::CreateContent` now returns a
  content object (the primary-content object with a NULL viewport; `content_GetViewport` already guards that), via a
  shared `create_content()` also used by `viewport_GetPrimaryContent`.
- Testing moved off the user's desktop (their request) to the existing headless `Xvfb :98` (1280x800):
  `env -u WAYLAND_DISPLAY DISPLAY=:98 WINE_D3D_CONFIG=renderer=vulkan tools/wine-uwp/run.sh`; clicks with
  `DISPLAY=:98 xdotool`, captures with `import -display :98 -window root`.
- On `Xvfb :98` (vulkan renderer) the startup crash became deterministic (2/2): `0xc0000409` in
  windows.ui `__GSHandlerCheck` on the UI thread, right after `dwritefactory6_GetSystemFontCollection`, during tick #4.
  That is a stack-cookie check failing while an exception is dispatched through windows.ui frames — the same
  dwrite fault as before (garbage `entry->file` in `create_font_collection_from_set`), surfacing differently.
- With `CreateContent` implemented (run on :98, no entry hooks): startup renders (6 presents), but after two clicks
  still 0 presents. XAML now gets one step further in the same tick and asks the new content for
  `{820c0b62-45be-41dd-a2dc-6ccaf6ae0f04}` (Wine: not found), then tries to create class
  `{00000352-0000-0000-c000-000000000046}` (not registered) and fail-fasts with E_NOINTERFACE (#3). Same failure
  mode: the tick that hosts `AddSecondaryContent` never ends. Next: implement that private content interface.
- `{820c0b62-45be-41dd-a2dc-6ccaf6ae0f04}` is queried on DM contents by XAML's `SetupSecondaryContent` (both
  overloads), `ApplyPrimary/SecondaryReflexCurves` and passed as riid by `CreateParametricBehavior`. Observed use:
  slot 3 `(REFIID {cca6498e-f33f-4bf5-a344-c08067a863e4}, void **out)` returns a per-motion curve object, slot 4
  `(int motion 1|2, int, IUnknown *curve, int 1)`; the curve object's slot 6 takes floats, and the reflex-curve
  helpers call more of its slots. directmanipulation now answers with an accept-and-ignore view (`content_curves_vtbl`)
  whose slot 3 hands out a refcounted stub curve (`dm_curve_vtbl`, 16 S_OK slots).
- With `{820c0b62}` stubbed, the tick got further and hit (a) `E_UNEXPECTED` from
  `CDirectManipulationService::GetCompositorContentTransform` in the render walk — Wine's
  `content_GetViewport` returns E_UNEXPECTED for viewport-less contents, and `viewport_AddContent` never recorded the
  viewport; AddContent/RemoveContent now set/clear `primary->viewport` — and (b) repeated
  `compositor_QueryInterface({59f4c99d})` misses from `CreateViewportInteraction` (per manipulatable element, from
  `HWCompTreeNodeWinRT::UpdatePrimaryVisual`), after which XAML's fail-fast is fatal (`0xc0000409`). The compositor
  now has a `{59f4c99d}` view: slots 3/4 return a refcounted stub interaction object, the rest are S_OK.
- Test hygiene: launching a new run while a previous background run's retry loop is still alive lets the two
  `wineserver -k` each other; wait for the earlier task to finish first.
- Correction to the `{59f4c99d}` view: slot 4 (`CInputServices::CreateViewportInteractionForRootVisual`) is called as
  `(this, visual, device partner, IUnknown **out)`; the first stub took `(this, device, out)` and so wrote the stub
  pointer over the device partner's vtable, which produced the immediate `0xc0000409` fast-fails (6/6, with no
  "ignoring FailFast" line). Signature fixed.
- `+seh` on the deterministic `0xc0000409`: the dispatched exception is the known dwrite fault —
  `c0000005` at `dwrite.dll+0xb704` (`create_font_collection_from_set` → `get_filestream_from_file` →
  `IDWriteFontFile_GetReferenceKey`), rcx = the entry's `file` = 0x7f6c2130d230 whose first qword reads 0x7f6c: a
  freed `IDWriteFontFile`. The handler walk goes xaml → CoreMessaging (__CxxFrameHandler4 frames) → the first
  windows.ui frame, whose `__GSHandlerCheck` then fails the cookie and fast-fails. So every "GS failure" here is the
  dwrite use-after-free; it is deterministic on :98 (vulkan renderer) at the first `GetSystemFontCollection` on the
  UI thread after the CoreWindow exists.
- Commit 6d1ca33 (2026-09-13) had already locked dwrite's system-collection cache for this crash without a confirmed
  cause. Since it is now deterministic on :98, dwrite carries temporary ERR tracing (font file freed + caller, fontset
  entry freed, and each entry/file `create_font_collection_from_set` touches) to catch the free-before-use directly.
- **Heap validation (`WINEDEBUG=+heap`) finds the real bug: a write into a freed 8-byte block** ("free block
  overwritten", process heap), on the thread that builds the system font collection (also the D3D/vulkan thread),
  well before dwrite's `create_font_collection_from_set` reads a corrupted `entry->file`. The 8-byte
  HEAP_ZERO_MEMORY block is allocated in a repeating pattern "string(0x22-0x2c), 8, 8, string, free" and freed
  in a batch shortly after. The dwrite `entry->file` corruption (first dword = high half of a pointer) is the
  collateral damage. Tracing `+heap,+dwrite` together to name the code that owns those 8-byte blocks.
- **ROOT CAUSE of the startup crashes (dwrite "use-after-free", GS fast-fails): a heap overrun in
  `GetCurrentPackageInfo2`** (dlls/kernelbase/version.c, the other session's uncommitted package-identity code).
  `+heap,+dwrite` tracing: the overwritten freed block was the `AppxManifest.xml` read buffer (0x24f5 = 9460+1
  bytes), which sits directly above the caller's 0xbb2-byte package-info buffer. The size pass of
  `GetCurrentPackageInfo2` did not count the publisher strings, but the fill pass advanced `offset` by them anyway
  (`pkg_put_string` bumps `offset` even when the string does not fit), so for the last package
  `ids_size = *buffer_size - offset` underflowed and `PackageIdFromFullName` wrote a PACKAGE_ID plus strings past
  the end of the buffer — into whatever block followed: the freed manifest buffer here, live dwrite font-file
  objects in the earlier runs (hence the garbage `entry->file` vtable). Fixed with the user's explicit OK, in
  version.c: the size pass now counts the publisher, and `ids_size` is clamped (no PackageIdFromFullName call
  when there is no room). Left uncommitted for the owning session.
- After the kernelbase fix the runs still fast-failed, but `+seh` now shows a different first exception: an access
  violation in XAML `HWCompTreeNodeWinRT::UpdatePrimaryVisualTransformMatrix` (+0x2c0152) releasing a garbage
  pointer that came out of `CDirectManipulationService::EnsureSharedContentTransform`. `DManipDataWinRT::
  SetSharedContentTransforms(IDCompositionTransform *, IDCompositionTransform *, ICompositor *)` shows what the
  `{59f4c99d}` slot 4 must return: a real **IDCompositionTransform** of the device partner passed in. The opaque
  stub (any-IID QI, out-params never written) was being probed for other interfaces and left garbage behind.
  Slot 4 now QIs the partner for IDCompositionDevice and returns `CreateMatrixTransform()` — one of dcomp's
  generic comp objects. (Both `CreateSharedContentTransformForContent` and `CreateViewportInteractionForRootVisual`
  call slot 4; slot 3 is the per-viewport interaction and keeps the stub.)
- Returning a real dcomp matrix transform from `{59f4c99d}` slot 4 did not change the fault: XAML's
  `UpdatePrimaryVisualTransformMatrix` still releases a garbage first output of `EnsureSharedContentTransform`
  (values like 0x1f4339a2a2d, not a Wine heap address). `EnsureSharedContentTransform` zeroes both outputs and its
  two locals, creates one transform via `CreateSharedContentTransformForContent` and caches a `SharedTransformParts`
  pair; the garbage must come from what XAML does with the transform afterwards. Tracing every call on the
  handed-out transform objects with `+dcomp`.
- **Found the transform corrupter, in our own dcomp.** XAML asks the WinRT Compositor for `{e7894c70-af56-4f52-b382-
  4b3cd263dc6f}` and calls slot 5 with `(transform, REFIID {c12779d8-...}, out)`. dcomp answered that IID with the
  interop *device* vtable (it was assumed to be a second IDCompositionDesktopDevicePartner revision), so slot 5 was
  `GetFrameStatistics`, which wrote a DCOMPOSITION_FRAME_STATISTICS over the transform object — the "garbage vtable"
  was a QPC timestamp. Native dcomp's `InteropCompositor::s_InterfaceParts` maps `{e7894c70}` to its own part
  (+0x518), whose vtable (dcomp.pdb, `Api@InteropCompositor` at 0x19d6b8) is: 3 MarkDirty, 4 ClearCallback,
  5 CreateManipulationTransform(IUnknown *, REFIID, void **), 6 RealClose. dcomp now hands out a dedicated
  `partner_vtbl_impl` for it (CreateManipulationTransform returns a generic comp object). This also explains the old
  "RealClose lands on CreateVisual with no out-pointer" hack (partner slot 6 hitting device slot 6).

## Clicks work: "78" on the display (2026-09-14)

- The `{e7894c70}` interop-compositor part fix holds: with it, rendering no longer stops after the first
  click (presents keep flowing, no GS fast-fail). Three "crashes" seen right after that change were a
  stale-log race in the test script (it grepped the previous run's log before `run.sh` truncated it),
  not real crashes.
- Why XAML never saw pointer input, even though WM_POINTERDOWN/UP reached the CoreWindow and a probe
  handler on `CoreWindow.PointerPressed` fired: `CJupiterWindow::RegisterCoreWindowEvents` only subscribes
  the CoreWindow pointer events (vtable slots 38-49, `OnCoreWindowPointer*`) when
  `XamlOneCoreTransforms::IsEnabled()` is TRUE, and that function is **binary-patched to `xor eax,eax; ret`**
  in our Windows.UI.Xaml.dll (island147, RVA 0x80b60). Wine's `IsOneCoreTransformMode` returning TRUE only
  set `s_enabled`, which the patched `IsEnabled` never reads. A detour on RegisterCoreWindowEvents confirmed
  it returns S_OK with the pointer/DManip tokens (+0x160..+0x1c0) untouched.
- Restoring the real `IsEnabled` (bytes `48 83 ec`) is not the way: in OneCore mode XAML builds a
  `CoreWindowIslandAdapter` during DXamlCore init (main thread, before activation) and needs a CoreWindow
  that does not exist yet; the ctor's null path hits a stale island-era patch (`je 0xc6ccc0` at 0xa00906
  lands inside a rewritten cave, illegal instruction at +0xc6ccc3). Two runs also died in windows.ui
  `CDispatcher::ProcessMessage+0x46` writing through r9 == 3 — see the cave fix below. Source DLL restored
  byte-for-byte (verified against a backup, which was then removed as identical).
- **The classic (desktop) path instead:** `CJupiterWindow::UseCoreWindowSubclassedEvents` (0x4eac8) returns
  TRUE when XamlOneCoreTransforms is initialised and *disabled*, `CQuirksMode2::m_quirksDisabledForProcess`
  is clear, no XamlPresenter is associated, and kernelbase `QuirkIsEnabled(0x20046)` is nonzero. XAML then
  subclasses the CoreWindow HWND (`CJupiterWindow::CoreWindowSubclassProc`) and reads WM_POINTER* itself
  via `CJupiterControl::HandlePointerMessage`. Two Wine changes:
  - user32 `IsOneCoreTransformMode` now returns FALSE (opt back in with `WINE_UWP_ONECORE=1`). Nothing in
    the current rendering path depends on it: every caller goes through the patched `IsEnabled()`, which is
    FALSE anyway.
  - kernelbase `QuirkIsEnabled` returns TRUE for quirk 0x20046 only.
  Result: WM_POINTERDOWN → `CoreWindowSubclassProc` → `HandlePointerMessage(0x246)` →
  `PointerInputProcessor::ProcessPointerInput` → `ButtonBase::OnPointerPressed`, then the UP →
  `OnPointerReleased` → **`ButtonBase::OnClick`**, for both test clicks.
- The first click run then faulted in `CInputServices::ProcessUIThreadTick+0xde` (null in the pointer-state
  list) after two exceptions had been swallowed inside the window procedure: Wine's ninput.dll lacked
  `ResetInteractionContext` (unimplemented-function exception 80000100) and `ProcessPointerFramesInteractionContext`
  / `AddPointerInteractionContext` / `RemovePointerInteractionContext` / `StopInteractionContext`. All five are
  now S_OK stubs in dlls/ninput. With them the run is clean: no fault, no ignored exception, and the display
  reads **78** after clicking 7 and 8 (the number is partly hidden by the oversized tile image that still
  sits over the display; that and the left-aligned display text are rendering issues, not input).
- windows.ui `CDispatcher::ProcessMessage` cave (RVA 0xd3b8c, the ProcessMessage → DrainCoreMessaging
  trampoline from island108) called `DrainCoreMessaging` without preserving rdx/r8/r9, so whenever the drain
  ran anything that clobbered r9 the function faulted at +0x46 writing the fourth argument. The cave now
  pushes rdx/r8/r9/rcx around the call (`cmp` shortened to the imm8 form to make room); source copy
  patched, `Windows.UI.dll.bak-cave-clobber` is the previous state.
- Test-harness note: `run.sh` runs `install-overrides.sh`, which replaces any prefix system32 copy that
  differs from `~/uwp-bin/win11/Windows/System32`. Binary patches therefore have to be made in the win11
  source tree; editing the prefix copy is silently undone on the next launch.
- Diagnostic hooks used for this (all `WINE_UWP_DIAG_XAML`, removed before commit): entry hooks on the XAML
  pointer path, a return-logging detour on RegisterCoreWindowEvents, hooks in windows.ui
  (`WindowServer::add_PointerPressed`), and a #UD/stack dump in dcomp's vectored handler. Trampolines must
  be allocated within rel32 reach of the module (probe downwards from the module base), and copied prologue
  bytes containing a rel32 call need the displacement retargeted.

## After the first click: what is left on screen (2026-09-14)

- The square image over the display was the title bar's 16x16 app icon: `paint_public_visual` copied a
  sprite's surface brush at the texture's size (256x256 asset) with SRCCOPY, so it was huge and its
  transparent area was black. dcomp now scales the premultiplied BGRA texture to the sprite's size and
  alpha-blends it (`blit_surface_rect`). Only that one surface goes through this path in Calculator;
  all text is drawn as alpha masks through `paint_mask_brush`.
- The result/expression text sits at the left edge because `CalculationResult` right-aligns by
  scrolling its ScrollViewer to the end (`ChangeView(ExtentWidth - ViewportWidth)`). XAML drives that
  through DirectManipulation (`ZoomToRect`, a no-op here) and moves the visual through the shared
  content transform / `CreateManipulationTransform` object bound with a composition expression
  animation; dcomp accepts but never runs animations, and `viewport_GetPrimaryContent` still creates a
  fresh content object per call. Fixing this means real DManip offsets plus animated transforms in
  dcomp; deferred.
- Pressed buttons lose their fill: XAML's pressed/pointer-over visual states swap the button
  background brush, and the replacement (a theme resource brush) is not resolved by the painter.
  Not yet looked at.
- **Intermittent tick loss (about 2 in 10 runs) explained.** The click is processed but no tick
  follows and `UIThreadScheduler` stays in state 1 (queued) forever, so every later
  `RequestAdditionalFrame` is a no-op. The reproduced log shows the last `QueueTick` → `SetRelative(0,0)`
  coming from a worker thread (02c0, the DManip/compositor thread; about a fifth of all QueueTicks come
  from it) right after the UI thread's `EndTick`. The island108 CoreMessaging patch hands due-now
  timeouts to the UI thread through one BSS slot (0x10d000): the enqueue cave does `xchg`, but the
  dequeue cave (0xd49e0) did `mov rbx,[slot]; test; and [slot],0`, so a timeout stored between the
  read and the clear was wiped without ever reaching `OnTimerSignaled`. The dequeue now takes the slot
  with `xor ebx,ebx; xchg [slot],rbx` (`CoreMessaging.dll.bak-drain-race` is the previous state).
  Note the enqueue still overwrites an undelivered entry; that only costs a tick when two different
  timers are due at once.
- That was not the whole story: with the atomic dequeue the stall still hit 2 of 8 runs, and hooks on
  the OnTimerSignaled thunk showed the real loss. The slot is global, but every dispatcher's
  `DrainSignaledWaits` drained it, and the main thread (028c, twinapi's view host) runs one too. When
  it got there first, `TimeoutManager::OnTimerSignaled` ran on the main thread, activated the dispatch
  item on the wrong dispatcher, and XAML's `MessageTimerCallbackStatic` never ran; the scheduler stayed
  in state 1 for good. The dequeue routine now lives in free space at CoreMessaging+0xd4a60
  (`0xd49e0` jumps there) and only takes the slot when `gs:[0x48]` (the current thread id) equals the
  id wineuwp stores at +0x10d010 from `wf_CreateCoreWindow` (the XAML UI thread); zero means any
  thread, for the pre-CoreWindow startup ticks.
- With the thread-bound dequeue, 9 of 9 surviving runs in a batch of 10 ticked after the click. The
  one loss was a startup crash on tick #3: `CTransform::MakeWinRTExpressionWithOrigin` reads the
  property set it asked an expression animation for (`ICompositionObject::get_Properties`, slot 8 of
  dcomp's generic recording object) and got nothing. `is_out_pointer` refused to fill the caller's
  out-pointer because the stack address (0x7f98c018dfc0-style) matched its "low dword looks like a
  float" heuristic; thread stack placement varies per run, hence the intermittence. The packed-float
  case that heuristic was for (a Size in a register) is already non-canonical and rejected earlier,
  so the float test is gone.
- "Vanishing buttons" were not a rendering-state problem: the fallback presenter painted straight
  into the window DC, backdrop first, so screenshots sampled mid-frame showed half-painted frames
  (a stepwise pointer experiment caught one with only MC/MR/M+ drawn). The presenter now paints into
  a compatible bitmap and BitBlts once. What remains after that: buttons that were hovered or pressed
  keep the faint pressed fill, because XAML animates the state change and dcomp does not run
  composition animations.
- Button hover/pressed fills now return to normal: XAML animates the state change with
  ColorKeyFrameAnimations on the button background's colour brush (also ScalarKeyFrameAnimation
  "Opacity" and BooleanKeyFrameAnimation "IsVisible" on visuals). dcomp's key-frame animations are
  typed objects now (generic recording vtable plus the IColor/IScalar/IBooleanKeyFrameAnimation view)
  that keep the last key frame, and StartAnimation jumps the target property to that end value. The
  stepwise pointer experiment shows the hovered button darker and the rest restored.

## The result display is right-aligned (2026-09-14)

- `CalculationResult`'s right alignment is a DirectManipulation content alignment
  (`primary_SetHorizontalAlignment(RIGHT)` on a 936x117 viewport holding 39x87 of text), not a layout
  one. Two pieces were missing:
  - directmanipulation: one primary content per viewport (XAML configured the first object and read
    transforms from later ones), and `GetContentTransform`/`GetOutputTransform` return the alignment
    translation (viewport width minus content width) when the content is smaller than the viewport.
    The same matrix is pushed into the shared content transform (`CreateSharedContentTransform`),
    which now comes from dcomp's `__wine_dcomp_create_matrix_transform` because the "device" XAML
    passes is the flattened desktop-partner table, where the IDCompositionDevice slot for
    CreateMatrixTransform lands on CreateTranslateTransform3D.
  - dcomp: the position reaches the visual only through composition expressions:
    `InteropCompositor::CreateManipulationTransform(shared)` → `PS.DManip` animated by
    `"transform*manipTransform.Matrix"` → the visual's TransformMatrix animated by
    `"Matrix4x4(LOCAL.Render) * ... * LOCAL.DManip * ..."`. The matrix transform, manipulation
    transform and expression animation are typed objects now (expression animations record
    `SetReferenceParameter`; StartAnimation receives the ICompositionAnimation view, references the
    generic pointer or a property set's ICompositionObject view), and the painter adds the linked
    matrix's translation. Two traps on the way: the expression's ICompositionObject must stay the
    generic table (XAML walks Properties and a private partner {83112a11} from it and fail-fasts on
    E_NOINTERFACE), and the generic default slot clones objects by vtable, which must route typed
    vtables through their own constructors or the clone is a truncated struct.
- Still wrong on that screen: the expression line at the top left is drawn large and unaligned, and
  the memory buttons' row is fine. History/Memory panel and the title bar are as before.
- The second "78" at the top left was the same digit sprites painted twice: the tree-walk trace
  (now part of the painter's TRACE output, "child N of ... is ...") showed sprite X as child 0 of the
  DManip content container and still as child 8 of its previous parent. `collection_Remove`
  compared children by interface pointer, so XAML's Remove on the old collection did not match.
  Children are now compared by object (QI for the private InteropVisual view), each public visual
  remembers its parent collection, and an insert into another collection detaches it from the old
  one. With that the Standard view renders as on Windows except for the hamburger glyph (top left,
  next to the app icon), which is missing.

## Polish: startup time, log noise, the hamburger (2026-09-14)

- Startup: 10 s to a full UI with run.sh's old default tracing (+loaddll,+combase,+winrt), 4 s with
  Wine's normal channels. run.sh now defaults to `fixme+all,err+all`; set WINEDEBUG to trace again.
  The per-tick FIXMEs (compositor probe answers, QuirkIsEnabled for 75 distinct ids, the dwrite
  download queue) are reported once each.
- The hamburger is WinUI's AnimatedIcon: a ShapeVisual (48x48, under a container scaled 0.333)
  holding container shapes of sprite shapes over path geometries (IGeometrySource2DInterop gives the
  ID2D1Geometry), filled with a colour brush whose Color is the expression
  `ColorRGB(_theme.Foreground.W,_theme.Foreground.X,_theme.Foreground.Y,_theme.Foreground.Z)` over a
  Vector4 in the theme property set. dcomp now has typed shape objects, rasterises the shapes with
  D2D into a texture of the visual's size (Wine's ID2D1DCRenderTarget was tried first: its present
  is a whole-rect SRCCOPY from a DXGI surface whose GetDC fails here), keeps property-set values and
  expression text, resolves those two colour expression forms at StartAnimation, applies a visual's
  Scale about its CenterPoint to its content and children, and skips invisible subtrees (the back
  button's AnimatedIcon, also 48x48 at the same spot, is IsVisible=false). The icon's other
  animation states stay hidden without evaluating the Progress-driven animations, since their
  containers have no offset and land outside the 48x48 texture.
- One-off crash seen on the user's desktop (GL renderer, once):
  `WUCBrushManager::GetNineGridBrush+0xdd` reading a NULL nine-grid brush vtable; not reproduced on
  :98 with the Vulkan renderer.
- Keyboard input works (click 9, type `12+3=` → "912 + 3 = 915", expression line included). The first
  key press had WinUI's ScrollPresenter create an InteractionTracker: with the class unregistered the
  app raised an unhandled language exception; with a recording tracker WinUI overwrote the object
  (expressions like `tracker.Position` and private interfaces); with statics that fail (E_NOTIMPL)
  WinUI reports one unhandled error and carries on. Found on the way: recording objects must answer
  IInspectable's three methods properly (the default slot wrote a fake object over GetIids' count),
  and WinUI asks composition animations for IDependencyObject, which must be refused.
- The desktop-only crash (`WUCBrushManager::GetNineGridBrush+0xdd`) reproduces headlessly with a
  stream of pointer moves over the keypad (the user's real mouse; 17 moves were enough). XAML called
  a nine-grid brush through its recording table and the default slot answered the out-pointer with
  a "clone" carrying the nine-grid vtable over a plain recording struct, which the next use read as
  a brush with a NULL vtable. The default slot now clones only the two recording tables and answers
  every other typed object with a plain recording object. 600 pointer moves plus a click are clean.

## Window frame: move and resize (2026-09-14)

- wineuwp now gives the CoreWindow WS_OVERLAPPEDWINDOW when it shows it, and the navigation
  client's view bounds (`GetViewBounds`, `VisibleBounds`) follow the client rect, so a resize
  reaches XAML through windows.ui's WM_SIZE → `SendWindowSizeEvent` → CoreWindow.SizeChanged and the
  page re-lays out (History panel beside the keypad at 800x560). Two testing traps: without a
  window manager Wine ignores external ConfigureNotify (unmanaged window), and win32u marks a window
  that covers the whole monitor non-resizable (min = max size hints), so on the 1280x800 test
  display the tests run under kwin_x11 with WINE_UWP_WINDOW_SIZE=1000x680. run.sh defaults the frame
  title to "Calculator".

## The focus rectangle and the equals button (2026-09-14)

- The white rectangle around the display was XAML's keyboard focus visual: Calculator focuses the
  result programmatically, and XAML converts a programmatic focus into a keyboard one when its "last
  input device" is the keyboard. That value is set by `CContentRoot::UpdateLastInputDeviceTypeFromSystem`
  during `FrameworkView::SetWindow`, which sends WM_CHANGEUISTATE(UIS_INITIALIZE) and WM_QUERYUISTATE
  to `GetOwnerWindow()`. At that moment the CJupiterWindow is not attached yet, so the hwnd is NULL,
  the query returns 0 (no UISF_HIDEFOCUS) and XAML records "keyboard". Windows gets the right answer
  later: win32k initialises a top-level window's UI state on its first activation and DefWindowProc
  broadcasts WM_UPDATEUISTATE(UIS_SET, HIDEFOCUS|HIDEACCEL) down the tree, which XAML's subclass
  procedure (`CJupiterControl::HandleWindowMessage`) turns back into "no input device". Wine's
  DefWindowProc ignored all three messages (traced with winedbg breakpoints: `SetLastInputDeviceType`
  was never called, the "keyboard" write at xaml+0x3bd2f9 was, with rbx = hwnd = 0).
- win32u now keeps a per-window `ui_state`: WM_CHANGEUISTATE forwards to the top-level window,
  UIS_INITIALIZE hides cues unless SPI_GETKEYBOARDCUES is on (like Windows, minus the "last input
  event was a key" refinement, which Wine does not track), a change sends WM_UPDATEUISTATE to the
  window and its descendants, and WM_QUERYUISTATE answers it. `set_active_window` initialises a
  window on its first activation. Because wineuwp shows the CoreWindow before XAML subclasses it,
  that first activation is too early; the navigation client's `OnActivateWindow` (the app's
  `Window.Activate`, when the shell would activate on Windows) re-initialises the state. Keyboard
  input then still works (typed 912+3= and got 915), and focus visuals will appear after real key
  presses as they do on Windows.
- The equals button was the base accent #0078D4 instead of Windows 11's #4CC2FF: XAML's
  `SystemThemingInterop::GetSystemVariantAccentColor` maps its seven variants to uxtheme
  `GetColorFromPreference` colour types 1..7 (Light3, Light2, Light1, accent, Dark1, Dark2, Dark3)
  and Wine's semi-stub returned the base colour for every type. It now returns the Windows 11 default
  palette (99EBFF 4CC2FF 0091F8 0078D4 005EB7 003D92 001A68), with alpha, which XAML byte-swaps
  itself. AccentButtonBackground in the dark theme is Light2, hence the match.

## The hamburger menu: selection, scrolling, the pane fill (2026-09-14)

- **Nothing could be selected.** WinUI's NavigationView invokes items from the Tapped event, and XAML
  raises Tapped from ninput's InteractionContext output callback, which Wine's stub never called
  (hover works because it is plain pointer input). ninput now recognises a tap: pointer down and up
  within 10 px and 500 ms, reported as INTERACTION_ID_TAP in the context's units (HIMETRIC by
  default), with double-tap counting; GetStateInteractionContext reports IN_INTERACTION between the
  two. Manipulations, holds, drags and cross-slides are still not recognised.
- **The wheel did nothing.** XAML's ScrollViewer hands wheel input to DirectManipulation only when
  the viewport reports READY; Wine's viewport stayed ENABLED after Enable, so
  IDirectManipulationManager::ProcessInput was never called (and was E_NOTIMPL anyway). An enabled
  idle viewport now reports READY, and ProcessInput moves the primary content of the innermost READY
  viewport under the pointer by 48 px per notch, clamped to the content, then raises
  OnViewportStatusChanged (RUNNING, READY), OnContentUpdated and OnViewportUpdated; XAML reads the
  content transform back and its visuals follow the shared content transform. Traced with
  WINEDEBUG=+manipulation (the channel is "manipulation", not "directmanipulation").
- **Scrolled content spilled out of the pane.** XAML clips ScrollViewer content with an InsetClip on
  its interop visual; dcomp recorded the clip object but never applied it, and CreateInsetClip
  returned a generic recording object whose inset setters were swallowed. InsetClip is now typed (its
  default interface is IInsetClip, which XAML uses without a QI) and the painter clips the subtree of
  public and interop visuals to it.
- **The pane had no background.** It is an acrylic brush: CompositionEffectBrush over XAML's effect
  graph (blurred Backdrop, tint flood, Noise). XAML creates it even though CompositionCapabilities
  says effects are neither supported nor fast. The effect factory now keeps the IGraphicsEffect and
  the painter walks it through IGraphicsEffectD2D1Interop for the most opaque D2D1Flood colour and
  paints that, opaque. Two traps: the Backdrop/Noise sources are our own recording objects that
  answer every interface (walking them wrote fake values into the source count), and XAML's flood
  effect faults inside Windows.UI.Xaml.Controls.dll when GetProperty is called without a
  GetPropertyCount check; the walk checks the count and runs under __TRY.
- **Not fixed.** The Settings gear (an AnimatedIcon) is drawn at its 48 px design size: its root
  visual never gets the Scale(1/3) the hamburger's root gets, cause not found. Making
  BooleanKeyFrameAnimation typed (so IsVisible key frames apply) changed the hamburger into a
  different Lottie layer, since which layer shows depends on the icon's Progress state, so boolean
  key frames stay ignored. The pane does not close after picking a mode. Graphing mode fails on
  dxgi's CreateSwapChainForComposition stub.

## Picking a mode from the menu (2026-09-15)

- After picking "Scientific" the pane stayed open and nothing responded. Under it, in order:
  composition batches never raised Completed (XAML finishes SplitView's pane-closing storyboard on
  it; scoped and commit batches are typed now and complete from a timer on the ending thread);
  the visual collections were fixed arrays of 32/64 children and the Scientific keypad overflowed
  them, so XAML fail-fasted on every tick (15,000 stowed E_OUTOFMEMORY exceptions per run, ignored
  by wineuwp, leaving a half-built tree); a parent-link write into interop visuals (which answer
  the same private interface as public visuals) that the old inline arrays had hidden; and a
  painter that copied every sprite's texture GPU-to-CPU every frame, which with the keypad and the
  pane up took seconds per frame and starved input (each surface now caches its pixels until XAML
  redraws it). Tracing note: `echo >> $UWP_LOG` marks get overwritten because wine writes at its
  own offset; record `wc -l` instead. Alt+2 (mode shortcuts) hits a separate AccessKeys fail-fast
  (`MakeAccessKeyUppercase` E_FAIL), not fixed.

## Every menu entry, and the Settings gear (2026-09-15)

- A sweep script (`/tmp/links.sh`: open the pane, pick each entry, screenshot, count crashes,
  aborts, fail-fasts) found four killers, each one page deeper than the last:
  Programmer aborted (XAML's reveal lights need Windows.UI.Composition.Effects.SceneLightingEffect,
  a missing class throws in a noexcept function; then the light's ICompositionObject view had no
  compositor so the light dereferenced an animation it never built); Programmer then Currency crashed
  (ThemeShadowScene wants ICompositorWithProjectedShadow); leaving Settings fail-fasted on
  DirectManipulation's RemoveContent; and any page that trims memory aborted on a failed delay load
  of ext-ms-win-sysmain-pfsapi-l1-1-0 (XAML's OfferableHeap), now served by wineuwp as a plain heap.
  The Settings page itself came up blank with input dead because its variable-font text goes through
  IDWriteFontFallback1::MapCharacters, a Wine stub, and XAML fail-fasts on the layout error.
- The gear: XAML's Viewbox scales the AnimatedIcon by putting a 1/3 scale into the interop visual's
  TransformMatrix (XAML evaluates the "Matrix3x2.CreateFromScale(vector2(PS.ScaleX, PS.ScaleY))"
  expression itself through its animator partner); the painter only used the matrix's translation.
  Interop visuals now propagate matrix scale, and a small evaluator handles the TransformMatrix
  expressions XAML does hand to dcomp.
- Result: all 19 entries open without a crash, abort or fail-fast, apart from the Programmer page's
  ignored E_NOINTERFACE from ElementCompositionPreview::GetPointerPositionPropertySet (the reveal
  light's pointer tracking, cosmetic). Not working: Date calculation needs ILanguageStatics from
  windows.globalization (E_NOINTERFACE); every converter needs
  Windows.Globalization.NumberFormatting.CurrencyFormatter (class not found); both are in another
  session's uncommitted windows.globalization work. Graphing needs dxgi's
  CreateSwapChainForComposition. Cosmetic: the page title overlaps the previous one after a
  navigation (the old title's fade-out is a composition animation), expanding a Settings section
  leaves stale copies of the text below it, and the Alt+number shortcuts hit an AccessKeys
  fail-fast.
- Changing the app theme (Settings, App theme, Light/Dark) crashed: XAML starts the theme's colour
  animations through the brushes' private composition object partner ({83112a11}), which the colour
  brush refused; it now answers with a partner view that applies the animation. Light and Dark both
  render. The old radio selection and the previous page title still linger (fade-outs are composition
  animations the painter does not run).

## Settings page render issues (2026-09-15)

- Every radio button looked selected: WinUI scales the check glyph to 0 with a Vector3 key-frame
  animation, which dcomp only recorded. Vector2/Vector3 key frames are typed now and their end value
  lands on Offset/Scale/CenterPoint/Size. That broke the hamburger (its Lottie layers use the same
  animations, driven by Progress), fixed by undoing the end value when XAML fetches an animation
  controller for the property, which is what an AnimatedIcon does.
- Still open, with what is known: the previous page title shows through the Settings page, and
  "There's no history yet" stays after a calculation. In both cases the app collapses an element
  (x:Bind with a converter) and XAML treats it as collapsed (a click on the covered keypad does not
  register), yet no composition call ever hides or removes its sprites; the mechanism XAML uses for
  Visible -> Collapsed of an existing node was not found in the traces (no IsVisible, Opacity, Clip,
  Remove, or Size change). For the history text there is also an unhandled exception from
  SwipeControl's VisualInteractionSource (the deliberately failing Interactions statics) in the same
  update; recording objects for them crash later on a released view. Expanding a Settings section
  leaves stale copies of the text below it, presumably the same class of problem.

## Collapsed elements and AnimatedIcon states (2026-09-15)

- Visible -> Collapsed reaches the compositor through CUIElement::ClearPCRenderData, which asks each
  of the element's sprites for IVisual::get_Parent and removes it from that parent's children. dcomp's
  get_Parent answered NULL, so sprites of collapsed text stayed: the old page title under Settings,
  "There's no history yet" after a calculation, stale copies of text an expanding section moved.
  get_Parent now answers the collection link; all three are gone.
- AnimatedIcons (hamburger, Settings gear, Expander chevrons) are Lottie compositions whose key-frame
  animations are driven by a controller whose Progress follows "_.Progress" on the icon's property
  set. dcomp now keeps every key frame, hands out real controllers bound to the animated property, and
  samples the animations at the icon's Progress when painting (nothing before the first key, last key
  for booleans and colours, interpolation otherwise). Two traps: animations whose key frames XAML
  inserted as expressions have no keys (sampling them shrank the hamburger to dots), and a value
  before the first key must leave the static property alone (otherwise hidden layers show).
- Theme switching left the chevrons white on white: a brush's colour expression over the theme
  property set was evaluated once; it is re-read at every use now. Left: the About section's app icon
  is a blank box in the light theme (theme-qualified image asset), and the Programmer page's reveal
  lights still log 16 ignored E_NOINTERFACE fail-fasts for pointer tracking.
- Tried and reverted: answering the compositor's {36af3eb2} pointer-source interface (XAML's
  PointerSourceWrapper) with a probe removed the Programmer page's 16 ignored fail-fasts but made
  XAML's reveal light code crash the process; the refusal stays. Light theme verified end to end
  (Settings, back to the Standard page: dark-blue accent equals button as on Windows).

### Native Windows.Globalization: Date calculation and every converter (Wine 8d2d3db)

- Re-evaluated the remaining gaps against genuine Windows DLLs. Date calculation and the converters
  only needed `Windows.Globalization` (ILanguageStatics, CurrencyFormatter, Calendar,
  DateTimeFormatter); the genuine DLL is in the dump, so it is now overridden native (`run.sh`
  default list, `install-overrides.sh`). Findings along the way:
  - `Language.CurrentInputMethodLanguageTag` returned E_NOTIMPL because the DLL calls
    `ApiSetQueryApiSetPresence("ext-ms-win-globalization-input-l1-1-0")` first; the apiset is now
    mapped to `globinputhost.dll` (copied from the partition). Only the version-less name may be
    listed in apisetschema.spec, the loader hashes it without the version suffix.
  - `icu.dll` (Windows' combined ICU) returned E_FAIL from Calendar creation until
    `C:\windows\globalization\icu\icudtl.dat` and the `.res` time-zone files existed. The prefix
    already had a lowercase `globalization` directory (Sorting), Wine matches case exactly when both
    spellings exist, so the data goes under the lowercase name.
  - `WinLangdb.dll` is imported but is not in the live System32 any more; the WinSxS copy of the
    matching build works.
  - The converter pages then crashed in Microsoft.UI.Xaml (WinUI 2.8 AnimatedVisualPlayer behind
    the currency ProgressRing): it asks its progress CompositionPropertySet for ICompositionObject4
    and its root SpriteVisual for IContainerVisual (SpriteVisual derives from ContainerVisual),
    and dereferences the NULL out pointers. dcomp answers both now.
- Result: all 19 entries open cleanly (Programmer's 16 ignored fail-fasts unchanged); Date shows
  today's dates and "Same dates"; Temperature converts 73 °C to 163.4 °F and 346.1 K; Currency
  renders with the offline "Couldn't get new rates" message. The native DLL replaces the other
  session's uncommitted builtin work for this app; Wine's builtin stays the fallback if the override
  is removed.
- Still native-DLL-proof: Graphing (dxgi CreateSwapChainForComposition needs Wine's own swap chain
  code; the genuine dxgi.dll needs the kernel-mode driver interface), the reveal-light pointer
  tracking (dcomp), the light-theme About icon (MRT asset qualifiers), Alt+number AccessKeys.

### Graphing mode: composition swap chains (Wine, see `git log` for "Composition swap chains")

- `CreateSwapChainForComposition` is now an offscreen chain in dxgi (`compswapchain.c`): buffers are
  ordinary D3D11 render-target textures on the app's device, Present flushes and counts. The app's
  DeviceResources needed `ID3D11Texture2D1` from GetBuffer plus `CreateTexture2D1` and
  `CreateRenderTargetView1`, all stubs before; the texture/view vtables grew the GetDesc1 slot.
- XAML's `SwapChainPanel::SetSwapChain` queries IUnknown, IDXGISwapChain1 and an undocumented
  `{b02d7a1a-05e0-4a71-8ef4-5c5bbf475086}`; a missing third answer is XAML error 0x88001b00
  (disassembled at Windows.UI.Xaml+0x654e8f). It only releases the result, so the chain answers
  it with itself. XAML then hands the chain to `IDCompositionVisual::SetContent`, not to
  `ICompositorInterop::CreateCompositionSurfaceForSwapChain`; dcomp wraps either in a drawing
  surface that pulls buffer 0 through a staging copy on the texture's own device when the present
  count changes, and a 33 ms timer repaints the window because the app presents without commits.
- `DisplayInformation::DisplayContentsInvalidated` returned E_NOTIMPL from Wine's stub and the
  C++/CX DeviceResources treated that as fatal; it now accepts the handler. Note the other
  session's windows.graphics tree does not link (duplicate runtime-class strings between main.c
  and the untracked displayinformation.c); for testing the DLL was linked by hand with
  `-Wl,--allow-multiple-definition` and copied over the prefix's stale Sep 9 system32 copy, which
  the loader had been picking instead of the build tree.
- Result: the Graphing page opens with its toolbar, zoom buttons and mode toggle, and the graph
  area shows the swap chain: white, which is right for this build (IsMatchAppTheme defaults to
  the always-light graph theme, and the open-source GraphingImpl is a mock renderer that draws
  nothing over the cleared background). Sweep of all 19 entries still clean.

### Keyboard: Enter, Backspace, Escape, Ctrl+E and the Alt fail-fast (Wine 547195f, Windows.UI.dll patch)

- Alt+2 fail-fasted in XAML's `AccessKeys::AKAccessKey::MakeAccessKeyUppercase`: it upper-cases the
  key in place with `LCMapStringEx(locale, LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, key, 6, key, 6,
  ...)`, and Wine refused `src == dst` with any flag beyond plain LOWER/UPPERCASE (ERROR_INVALID_FLAGS).
  Windows accepts in-place linguistic casing; kernelbase now does too.
- With that out of the way none of the app's `CoreWindow.KeyDown` shortcuts worked (Enter did not
  compute, Backspace and Escape did nothing, Ctrl+E did not switch to Date), while typed characters
  did. Breakpoints (winedbg attach, `x /16b $rcx+0x20`, note `\$` in scripts) showed the app's handler
  reading `args.Handled == TRUE` and returning at once. On this WndProc input path
  (`WindowServer::OnKeyDownEvent`, used because there is no InputHost site, see the pointer notes)
  windows.ui creates the args with `Make<CKeyEventArgs>` and `CKeyEventArgs::Initialize(vk, status)`,
  and Initialize (RVA 0x97480) ends with `mov byte [this+0x38], 1`; `ICoreWindowEventArgs::get_Handled`
  (secondary vtable at this+0x10, reads +0x28 = this+0x38) hands that byte out, and nothing clears it
  before `InvokeAll`. Byte patch in the win11 source copy: RVA 0x97493 `c6 41 38 01` → `c6 41 38 00`
  (`Windows.UI.dll.bak-keyargs-handled` is the previous state). Windows apps do not see this because
  their input arrives through InputHost's KeyboardInputWinRT, not this path.
- Result: `1 2 + 3 Enter` shows 815, Backspace and Escape edit/clear, Ctrl+E opens Date calculation.
  Alt+number still does not switch modes: the accelerator event fires (the handler's
  `GetKeyState(Control)`/`(Shift)` pair shows up in a `RelayInclude=user32.GetKeyState` trace) and the
  Ctrl+E path proves `NavigateModeByShortcut` works, so the difference is inside the Alt branch
  (XAML's own AccessKeys handling of `AccessKey="2"` on the NavigationViewItem is the likely
  competitor); not fixed.
- Tooling: `tools/wine-uwp/pdbsyms.py --rva` and `xrefs.py` (run with `~/uwp-bin/venv/bin/python`)
  name Windows.UI / Windows.UI.Xaml addresses from the cached PDBs; a `RelayInclude` of one function
  in `HKCU\Software\Wine\Debug` is a much cheaper trace than `+relay`.

### Light theme: the NavigationView pane stayed dark (Wine 102413c)

- The pane is an acrylic (CompositionEffectBrush) and dcomp painted it from the flood colour in the
  effect description, which is the tint the brush was created with in the dark theme. XAML's acrylic
  keeps the live colours in `brush.Properties` ("TintColor.Color", "LuminosityColor.Color",
  "FallbackColor.Color", `propset_InsertColor` in a `+dcomp` trace) and rewrites them on a theme
  change, reaching the property set through a fresh ICompositionObject view every time, so each view's
  own set was a different, throw-away object. Effect brushes now own one property set that all their
  views share, and the painter takes "TintColor.Color" (then "FallbackColor.Color") from it before
  falling back to the description's flood. Light pane verified open and scrolled; dark unchanged.

### Date calculation: clicking a date killed the app (Wine, "ICompositor3 with CreateHostBackdropBrush")

- The CalendarDatePicker flyout aborted the process with `err:seh:check_noexcept noexcept function
  propagating exception`: a C++ exception (`winrt::hresult_no_interface`) thrown inside
  Windows.UI.Xaml.Controls' `AcrylicBrush::OnElementConnected`, which is noexcept. The raising stack
  (a temporary `RtlCaptureStackBackTrace` dump in ntdll's `dispatch_exception`, named with
  `pdbsyms.py --rva` after fetching Windows.UI.Xaml.Controls.pdb) was
  `CreateAcrylicBrushWorker` → `consume_ICompositor3::CreateHostBackdropBrush` → `check_hresult`:
  dcomp's compositor had no ICompositor3 `{c9dd8ef0-6eb1-4e3c-a658-675d9c64d4ab}`. It now answers it
  and records a backdrop brush. Before that the same code path threw on the DisplayInformation
  factory refusing IAgileObject (windows.graphics main.c, HEAD code, staged as its own hunk).
- winedbg was no help this time: `break *addr` in windows.ui.xaml / kernelbase reported "Invalid
  address" and the process died, and first-chance C++ exceptions do not stop it. The `+seh` channel
  only names the last (noexcept) frame through its ipmap line. A one-off stack dump in
  `dispatch_exception` gated by an env var was the reliable tool; it is not committed.
- Tried and reverted: patching XAML's `GetIslandFromElementImpl` (RVA 0x355c68) to skip the island
  peer lookup; that was a wrong guess about the thrower.
- Result: the flyout opens (month header, weekday row) and closes; the day grid is empty, a separate
  rendering issue. The other session's windows.graphics tree still needs `-Wl,--allow-multiple-definition`
  to link and the DLL copied over the prefix's system32 copy.

### CalendarView: the empty day grid (Wine, "Programmatic content offsets")

- The day cells were drawn (a `+dcomp` trace shows the 9x12..17x12 text surfaces and mask brushes)
  but at Y ≈ 932,900: the month panel is virtualized from year 1601 and XAML expects the scroll
  offset to come from DirectManipulation, through the visual's TransformMatrix expression
  `Translation(V.Offset) * LOCAL.DManip * Translation(-V.Offset)`, LOCAL.DManip being bound to
  `PS.Matrix`, itself `transform*manipTransform.Matrix` over the DManip shared transform.
- XAML's ChangeView path (winedbg breakpoints on `ScrollViewer::BringIntoViewportInternal`,
  `CInputServices::BringIntoViewport`, `CDirectManipulationService::*`) never reached
  `ZoomToRect`; it calls `CDirectManipulationService::SetPrimaryContentTransform(x, y, zoom)`,
  which hands (zoom,0,0,zoom,x,y) to slot 12 of the private primary-content interface
  `{d8042f40-a051-4fee-875e-243b7b38d66e}` (args: unknown pointer, float matrix[6], count 6).
  Wine's stub ignored it; it now behaves like SyncContentTransform and updates the shared
  transform, so the compositor-side DManip matrix carries -932820 and the days land in the flyout.
- Two more things were needed on the way: XAML tracks viewport status from
  `OnViewportStatusChanged`, and `CDirectManipulationService::EnableViewport` re-enters
  (Disable, Enable) when the change arrives synchronously inside `Enable`; the notifications are now
  posted through a timer, as the engine delivers them. And dcomp's expression evaluator learned
  Set*Parameter values (bare identifiers such as `transform`), `manipTransform.Matrix`, and to
  evaluate a bound LOCAL.DManip before falling back to the manipulation transform.
- Result: the picker shows the month with today highlighted; picking the 22nd sets From to
  9/22/2026 and the difference reads "1 week / 7 days". Sweep of all 19 entries clean.
- Debugging notes: `winedbg` `break *addr` works when the module base comes from the right
  process (the log can contain another process's load line); memory dumps need `\$rcx` inside
  scripts; first-chance C++ exceptions do not stop it; a temporary
  `RtlCaptureStackBackTrace` dump in ntdll's `dispatch_exception` named the AcrylicBrush thrower.

### Blue window on the desktop (:0): the OpenGL renderer

- A plain `run.sh` launch on the NVIDIA desktop showed only the blue placeholder: wined3d took its
  OpenGL path (Mesa EGL "could not get DRI3 device", `context_choose_pixel_format` fallback, GLSL
  shader logs on the CS thread) and XAML's first frame never came; the same build on Xvfb :98
  rendered. The test scripts had always passed `WINE_D3D_CONFIG=renderer=vulkan`; `run.sh` now
  defaults to it.

## Performance pass (2026-09-16)

Measured with a temporary frame timer in `present_desktop_hwnd` (not committed) on the 700x800
Xvfb window, and `date` stamps around the launch:

| | before | after |
|---|---|---|
| idle | 62 repaints/s, 3.3 ms each | none (XAML stops ticking too) |
| Standard page frame | 3.3 ms | 1.4 ms |
| pane open | 7.3 ms | 2.2 ms |
| launch to first frame | 9.8 s | 3.0 s |

- **Idle loop.** dcomp called XAML's partner `NotifyDirty` after every commit, and XAML schedules
  its next frame on that, so the two ticked each other forever. Every state-changing dcomp entry
  point (131 setters, inserts, EndDraw, StartAnimation, ...) now bumps a scene generation; a
  commit repaints only when the generation moved or moved within the last two seconds
  (time-driven animations and expressions change without any call), and NotifyDirty is only
  raised in that window. `matrix_transform_SetMatrix` is called from the DManip read path every
  frame, which is why the generation has to come from real setters and not from "any call".
- **Mask brushes.** Every glyph run and button fill is a mask brush; each paint copied the mask
  pixels, allocated a DIB, composed it and freed it. The composed bitmap is now kept on the brush,
  keyed on the mask surface (a generation counter bumped when its CPU copy is refreshed), the
  colour/source and the nine-grid values.
- **Presenter.** The back buffer and the blend scratch DC persist across frames; the back buffer is
  a DIB and unscaled blends/fills under a rectangular clip are done by dcomp itself (`soft_blend`,
  `soft_fill`, exact `DIV255`); GDI is the fallback for scaling and complex clips. The first
  version without opaque fast paths was slower than GDI (5 ms); the fast paths matter.
- **Startup.** `install-overrides.sh` compared ~1800 files on every launch (3.5 s); it now keeps a
  stamp of the source tree, the Wine build, the script and the prefix's system32 listing and exits
  in 6 ms when nothing changed (`WINE_UWP_FORCE_OVERRIDES=1` forces it). wineuwp's view pump
  after CreateCoreWindow always ran its full 5 s; it ends once the queue has been idle for 0.5 s.
- Verified: 19-entry sweep, keyboard (812+3 Enter), date picker, light-theme pane, Scientific.

### Graphing at desktop width: RichEditBox (Wine 4c17c6a, 05f27bc; native msftedit)

- On the 1280-wide desktop window the Graphing page shows its equation panel, and creating
  `CalculatorApp.Controls.MathRichEditBox` failed with 0x802b000a ("Cannot create instance").
  The 700-wide Xvfb tests never hit it because the narrow layout hides the panel. After the
  failed navigation the app sat on a blank page with an idle UI thread, which is the "locks up
  when I click it" symptom.
- `CTextBoxBase::InitializeTextServices` loads msftedit dynamically, resolves
  `CreateTextServices` *and* `ShutdownTextServices` (Wine's msftedit lacked the export), queries
  `ITextServices2` {8d33f741-...} and then a private `{6bda2cdc-d9b3-4310-8fcb-5690308c2e7c}`
  (fatal when missing; it is in native msftedit's IID table, not in any SDK header). Wine's
  riched20 now exports ShutdownTextServices and answers ITextServices2 (`TxGetNaturalSize2`,
  `TxDrawD2D` via the GDI path), which gets past the first two, but the third is unknown, so
  the native msftedit.dll is used instead (override list + install-overrides.sh). It imports
  `ntdll.SbSelectProcedure` (switchback quirks; NULL = no quirk), now a stub.
- With native msftedit the failure moved into Wine's `windows.ui.core.textinput`:
  `CoreTextServicesManager.CreateEditContext` was E_NOTIMPL. The new `CoreTextEditContext`
  (editcontext.c) stores the properties and accepts all events without ever raising them.
  `CTextInputPrivateSettings::GetPrivateEditContext` (Windows.UI.Xaml+0x3cee4) then sends
  msg 0x557 to the text services to get the edit context back and queries
  `{2f292ff4-823c-4a17-9fd5-188c56e252f8}` = `IPrivateEditContext` (unnamed GUID in both
  binaries; identified from Windows.UI.Core.TextInput's `CEditContext` vtable order:
  put_InputSettings(DWORD), get/put_IsEmbeddedHandwritingViewEnabled, ...Visible,
  get/put_IsMultiLine, FocusLeaveCompleted, ReplaceTextEx, SetSmartLink). Finally the app read
  `CoreTextServicesManager.InputLanguage` (E_NOTIMPL → .NET fail-fast); it now returns a
  `Windows.Globalization.Language` for the user locale.
- Result: Graphing opens at 1280x800 with the equation panel, typing `y=x^2` + Enter adds the
  function entry (plot area stays white: mock renderer, see above). Remaining noise: XAML also
  queries `{398aac15-c99c-4025-8015-ce834633c4fc}` on the edit context and activates
  `CoreTextServicesConstants` / `CoreFrameworkInputView`; all tolerated.

### Graphing: the real engine, pointer input, tracing (Wine 7ab4386, 36f3730, 078cfa9, c588dfd)

- **Engine.** The repo's GraphingImpl is a mock; the Store package ships the real engine as
  `GraphingImpl.dll` + `omsautimmss.dll` (see Overrides.md). With it, every equation errored
  (error type Syntax, code 12: read `Grapher::m_errorType/m_errorCode` at this+512/516 with a
  breakpoint on `SetEquationErrors`). The MathML request was fine; the engine parses through
  msxml6's SAX reader and the app's `Windows.Data.Xml.Dom.XmlDocument` also resolves to
  msxml6.dll, which Wine's builtin cannot activate (`DllGetActivationFactory` missing). Native
  msxml6 needs combase ordinals 2-25 (`ObjectStublessClient3..26`, now forwarded to rpcrt4's
  thunks), `ntdll.NtQuerySecurityPolicy` (stub) and its resource DLL `msxml6r.dll`. Result: lines,
  parabolas, trig, inequalities (shaded), variable sliders all plot.
- **Wheel zoom did nothing.** `WM_POINTERWHEEL` carries the delta in the high word of wParam;
  win32u stored it as pointer flags and left `InputData` zero, so `PointerPoint.Properties.
  MouseWheelDelta` (which windows.ui copies from `POINTER_INFO.InputData` when the WHEEL flag is
  set) was 0. Note win32u's `input.c` is in the unix part (`win32u.so`), not the PE dll.
- **Drag pan did nothing.** XAML's ElementGestureTracker feeds manipulations through ninput's
  Interaction Context, which only reported taps. It now reports single-pointer MANIPULATION
  begin/delta/end once the pointer moves past the tap slop. Deltas are in pixels: XAML never
  sets the measurement-unit property and used the (HIMETRIC) values as DIPs, so a 160 px drag
  panned by 26 screens.
- **Trace toggle fail-fasted.** `AddTracePointerShadow` builds a DropShadow with
  `CursorPath.GetAlphaMask()` and hangs it on a sprite. Four dcomp problems: the typed brush
  tables answered `GetRuntimeClassName` with a recording-object pointer (.NET Native picks the
  RCW type from it); sprites lacked `ISpriteVisual2` (put_Shadow, "Unable to cast"); the
  recording DropShadow wrote 8-byte pointers into 4-byte float out-parameters when XAML read the
  shadow back (a sprite's Size.X became -1e37); and every generic object answered
  `IReferenceTracker` {11d3b13a-...}, so .NET Native drove the tracker protocol through slots
  that now held IInspectable methods (writes through callback pointers, finalizer-thread
  fail-fast E_FAIL). All fixed; the trace pointer glyph, the nearest-point dot and the coordinate
  popup render and the arrow keys move it.
- **Pacing.** A drag repainted the window ~100 times a second (XAML commits per input event plus
  the app's presents). Presents closer than 15 ms are deferred to a one-shot timer, giving a
  steady 16 ms cadence. Per-frame cost on the graph page (frame trace, `WINEDEBUG=+dcomp`):
  hover 3.5 ms, wheel 4.4 ms, drag 7.7 ms of which 4.7 ms is the swap chain readback
  (CopyResource + Map wait for the app's GPU work; double-buffering the staging copy did not
  help because wined3d's Map waits for everything queued before it).
- Verified: 19-entry sweep clean, Graphing at 1280x800: plot, trace, wheel, pan, zoom buttons,
  reset, KGF toggle, sliders, inequality, trig, settings flyout.

### Graphing: the wheel-then-drag stall (Wine 2846796, de774ec) and the readback thread (Wine f0aa3c5)

- **Symptom.** After a wheel zoom the graph stopped updating and drags delivered only pointer
  down/up; the log showed `RtlpWaitForCriticalSection ... loader_section wait timed out in
  thread <UI>, blocked by <new thread>` a minute later. Timing-dependent: adding relay traces
  usually hid it; the reproduction was `/tmp/ghang.sh` (screenshot, three notches, drag).
- **Cause.** A zoom makes GraphControl's DeviceResources rebuild its D3D11 device, XAML then
  starts a `CompositorScheduler` render thread, and that thread's DLL_THREAD_ATTACH callouts
  (under the loader lock) reach native msftedit's DllMain, whose `UseRichEditCore()` WIL
  feature check does a once-initialisation. The UI thread was inside the same initialisation
  and went to the loader (feature-staging API set lookup) for the lock the new thread held.
  ptrace is off (yama scope 1) and winedbg cannot attach during a loader-lock deadlock (its
  break-in thread needs the same lock), so the identification came from `+relay` with
  `RelayInclude=ntdll.NtCreateThreadEx`: relay also logs every `Call PE DLL (... THREAD_ATTACH)`
  per thread, and the blocker's last unreturned one was msftedit's.
- **Fixes.** wined3d creates its command-stream and budget-notification threads with
  `THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH` (they run wined3d code only); the wineuwp host
  preloads msftedit and calls `DisableThreadLibraryCalls` on it, so XAML's thread never runs
  that handler (it only manages a lazily created TLS block). Two reproductions then ran clean;
  wheel, drag and reset verified by screenshot diffs.
- **Readback off the UI thread.** The swap chain readback (CopyResource + Map, which in wined3d
  waits for everything queued before it, so double-buffering the staging copy did not help)
  now runs as a thread-pool job into a second cache buffer that the UI thread swaps in at its
  next sync. Drag frames: 7.7 ms -> 3.8 ms; hover/wheel/drag all ~3.7 ms at a steady 16 ms
  cadence; idle stays at 0 presents. Not a new thread: its DLL_THREAD_ATTACH needed the loader
  lock and deadlocked the same way. Remaining per-frame cost is the full-window repaint of the
  keypad and panels (mask-brush sprites); a dirty-region painter is the next step if needed.
- Cosmetic: after a drag the trace popup sits at the top-left reading (0, 0.0) until the pointer
  moves again.
- **Second variant (Wine 5650753).** On the desktop the window froze after focusing the
  equation box: same `loader_section` timeout, blocker a silent new thread created by Wine's
  shcore `SHTaskPoolQueueTask` (one thread per task). The UI thread was inside the RichEdit
  language setup (Windows.Globalization.Language activations) when it needed the loader. Tasks
  now go to the thread pool instead of a fresh thread. Six focus/type/zoom rounds and the
  19-entry sweep ran clean. The underlying difference from Windows remains: Wine's loader takes
  the DllMain-serialising lock for module and export lookups too, so any native DllMain that
  once-initialises through the loader on DLL_THREAD_ATTACH can deadlock a UI thread doing the
  same lookup; avoiding fresh threads while such DLLs are loaded is the workaround.
- **Third and fourth variants, and the general switch (Wine 971c5d6, 3aec3a0).** The new
  loader-lock timeout report (Wine 5bdb0fd: the owner's current DllMain is printed) showed the
  idle desktop freeze was still msftedit's thread attach: `DisableThreadLibraryCalls` refuses
  modules with static TLS (msftedit has a .tls directory; Windows behaves the same), so the host's
  call had failed silently. The host now sets `LDR_NO_DLL_CALLS` on the loader entry directly.
  The next freeze was `Windows.Energy.dll`'s thread attach, the same WIL pattern. Rather than a
  list, ntdll now honours `WINE_SKIP_NATIVE_THREAD_CALLS=1` (set by run.sh): no
  DLL_THREAD_ATTACH/DETACH callouts into native modules at all (TLS slots and callbacks still
  run). Focus rounds, graph wheel/drag/trace and the 19-entry sweep are clean with it.
  Trigger for the desktop freezes: regaining focus by clicking inside the window makes XAML
  restart its render thread.
- Note: an edit script of mine truncated `dlls/ntdll/loader.c` on 2026-09-16 (evaluating
  `open(p,'w')` before reading). The committed version was restored and the one edit the other
  session had recorded there (the `uwp_rearm_breaks()` call in LdrLoadDll) re-applied from its
  transcript; any other uncommitted loader.c work from that session is lost.
- **Black window after focus games (Wine aea22e9).** Not a hang: the process was idle. Two
  parts. (1) XAML paints on its own commits only; an expose (minimize/restore, another window
  dragged across, `xdotool windowminimize`+`windowactivate` on :98) left the CoreWindow black until
  the next commit. wineuwp now subclasses the CoreWindow and has dcomp re-present its back buffer
  on WM_PAINT (`__wine_dcomp_repaint_window`). (2) The first restore then drew the whole UI at
  640,389: while minimized XAML sets the root ScrollViewer's DManip content rect to 0x0, the
  centring offset computed against that leaked into the transform XAML syncs back, and content
  that fits its viewport kept the stale pan. The resting transform now gives such content the
  alignment offset only. The freezes with `WINE_SKIP_NATIVE_THREAD_CALLS` had already stopped;
  the black window was the leftover symptom.
- **History/memory pane missing, resize crash (Wine 71dd57f).** The pane is a Pivot whose
  SwipeControl items need `Windows.UI.Composition.Interactions`; the failing statics aborted the
  list, and the recording fallback crashed WinUI on resize (a visual answered XAML's
  `IDependencyObject` {5c526665} with itself, then `dcv_SetClipRect` ran on garbage). Four layers:
  1. dcomp: typed InteractionTracker (owner ValuesChanged/IdleStateEntered callbacks),
     VisualInteractionSource, InertiaRestingValue, `ICompositionPropertySet2` booleans; the XAML
     identity interfaces are refused.
  2. The Pivot scrolls its 320000 px extent to 160000 through a programmatic content transform.
     XAML adopts it only from `OnContentUpdated`, now raised from the message loop.
  3. The header panel is a DManip *secondary content*: XAML builds curves through the private
     {820c0b62} interface (slot 3 GetCurve, curve slot 6 adds `{start in, start out, c1, c2, c3}`
     segments, slot 8 empties the curve, content slot 4 applies it to X or Y). The header uses
     slope -1 (it stays put), the items slope +1.
  4. The items panel sits at 160000 through its RenderTransform: `Render = PS.Expr0 * PS.Expr1`,
     `Expr0 = translate(PS.X, PS.Y)`, where PS is the expression's own Properties and XAML passes
     `LOCAL` as an explicit reference. The evaluator used the visual's own property set for LOCAL,
     so Render was never read. With Render evaluated, storyboard values started to matter: the
     NavigationView pane stayed at its closed X and dialogs at scale 0.92. XAML starts those through
     the property set's {83112a11} partner, slot 3 `CreateAnimatorPartner(name, key-frame
     animation, out)`, which now settles scalar animations at their end value.
  Headless resizes need `SetWindowPos` from inside Wine (a small helper): Xvfb has no window
  manager, and Wine ignores external resizes of unmanaged windows, so earlier `xdotool
  windowsize` "resize" checks never reached the app.
- **Graphing resize left a light layer over the keypad (Wine b984b0a).** dcomp kept a reference to the
  graph swap chain's back buffer; DXGI refuses `ResizeBuffers` while one is outstanding, so the
  graph kept rendering at its old width, past the new canvas edge and under the translucent keypad
  buttons. Swap chain surfaces now keep no back buffer reference between frames.

## Performance rework (2026-09-16)

Measured with `tools/wine-uwp/perf` (below) on the RTX 5090 / Xvfb setup, 1280x800 window. CPU is the share of
one core used by the app's processes during each scripted phase.

| phase | before | after |
|---|---|---|
| typing (a digit every 150 ms) | 22% | 6% |
| hovering across the keypad | 21% | 3-4% |
| graph drag | 43% at ~15 graph frames/s shown | 15-21% with every frame the app presents shown |
| graph wheel zoom | 27% | 4-5% |
| resize | 18% | 4% |
| idle | 1-2% | 1% |

Key press to digit on screen: 7 ms median. Menu pick to the graphing page drawn: ~250 ms (from ~307 ms).
Launch to a drawn keypad: ~1.8 s. Standard <-> Graphing rounds keep RSS flat after the first graphing visit.

What changed (Wine commits 25484f0, ce6e242, cb5636c, c205c53, 5424dcf, 27db824f):
- **No work without change.** Repainting an unchanged scene reproduced the previous frame bit for bit (hashed
  over every scenario), yet dcomp repainted for two seconds after any change and told XAML the composition was
  dirty for as long, so XAML committed ~120 times a second. Only a new scene generation or window size paints now,
  and only a commit that changed the scene asks XAML for the next frame.
- **Display list + damage.** The painter records fills and blends (software clip, content generations unique
  across objects) and diffs against the previous frame; only the union of what changed operations cover is
  rasterized and copied to the window. `WINE_DCOMP_VERIFY_DAMAGE=1` rasterizes every frame in full as well and
  reports differing pixels (none over the history, mode, graph and menu scripts; screenshots of those states match
  the old painter pixel for pixel). Opaque sources copy rows, and an area is rasterized from the last opaque
  operation that covers it.
- **Readback.** Surfaces XAML drew since the last frame are copied into one staging atlas per format and mapped
  once (one wined3d round trip instead of 20-30 per typed digit). wined3d gives CPU-read textures cached host
  memory: NVIDIA's first host-visible type is write-combined and reading a mapped 860x716 graph frame from it took
  17 ms (0.1 ms now). Composition swap chains report presents to dcomp instead of a 33 ms poll that showed about
  half the graph's frames.
- **Shape icons** are redrawn only when their resolved drawing inputs change (was ~1 ms every frame).
- **D3D device creation** no longer creates a Vulkan swapchain for DXGI's hidden implicit swapchain (~20 ms of
  every graphing page visit).
- Uncommitted, inside the other session's uncommitted `dlls/ntdll/rtl.c` work: `RtlQueryFeatureConfiguration` and
  `arm_uwp_breaks` looked up environment variables on every call (WIL feature checks run constantly; 14% of the UI
  thread during a graph drag). Both cache the lookup now; the change is also saved as a patch outside the tree.

## More performance (2026-09-18)

- `RoGetActivationFactory` resolved the class and loaded its module on every call: four registry lookups, a
  LoadLibrary and a DllGetActivationFactory. XAML activates Windows.UI.Input.PointerPoint on every pointer
  message, so a two second graph drag did 616 reads of the `DllPath` value, ~3% of the UI thread. combase now
  keeps the factory for a class, the way Windows does (Wine 24df22b); the drag's busy UI-thread samples went
  from 224 to 206 and the registry reads disappeared.
- Wine's dwrite bypassed its glyph cache whenever a transform was set, and XAML draws all its text through one
  (the window's DPI scale), so every glyph of every frame was re-rasterized through the unix side (~8% of the
  UI thread on the graphing page). The 2x2 matrix is part of the cache key now, and bitmaps are keyed by their
  rendering mode as well - two rendering modes of one glyph used to share an entry, so the second got the
  first's bitmap (Wine e4a555a). A graph drag costs 4.5 ms of CPU per frame instead of 5.4 and shows 273 frames
  where it managed 209; page screenshots compare equal pixel for pixel.

### Programmer page froze on the desktop (Wine 742d523)

`IDirectManipulationManager::Deactivate` returned E_NOTIMPL while `Activate` succeeded. XAML tears a page's
manipulation down when navigating away and throws on that failure: the desktop session's log has
`unhandled error details: hr 0x80004001` one line after `direct_manip_Deactivate`, and the window never painted
again - the same fail-fast-inside-the-tick that `CreateContent` was already fixed for. Deactivate now undoes what
Activate recorded and returns S_OK; `RegisterHitTestTarget`, which has nothing to register either, does the same.

The call never happens on the headless display (no window manager, so the window is never deactivated), which is
why 16 page switches, focus cycling and narrow-window runs did not reproduce it. For the next hang there is
`tools/wine-uwp/perf/stacks.sh`: with the app launched under `WINE_UWP_PROFILE=<prefix>` it asks the in-process
sampler for a dump and symbolizes it, which works where eu-stack and gdb do not (ptrace is limited to
descendants unless `kernel.yama.ptrace_scope` is 0).

Found, not changed (other layers):
- Idle, the UI and CoreMessaging threads wake ~2000 times a second: `ScheduleCoreMessagingDispatch` (user32) posts
  a WM_NULL on every request, ~270 requests a second. Coalescing requests the way Windows' queue does would fix it;
  CPU cost is ~1%.
- Each D3D11 device creates a Vulkan instance and device (`vkCreateDevice` ~50 ms of a graphing page visit).
- wined3d maps host-visible Vulkan memory in 64 MB chunks per memory type and device; the graphing page's second
  device accounts for most of its +430 MB RSS.

### tools/wine-uwp/perf

All scripts drive the app on `$PERF_DISPLAY` (:98) and write to `$PERF_OUT` (/tmp/wine-uwp-perf).
- `bench.sh [tag]`: CPU per phase and painted frames (above table).
- `latency.sh`: key, hover and page switch latency, polling the screen through XTEST/XGetImage (`latency.py`).
- `startup.sh`: launch to first keypad pixel and to a settled window.
- `profile.sh [tag]`: sampling profile of typing, switching to Graphing and dragging the graph. wineuwp's
  `WINE_UWP_PROFILE=<prefix>` sampler suspends threads every `WINE_UWP_PROFILE_MS` ms; `<prefix>.reset` and
  `<prefix>.dump` clear and write the counts; `profsym.py` symbolizes with `$UWP_PDB_DIR/*.pdb.syms` for Windows
  DLLs and `nm` for Wine builtins, listing each busy thread's top functions with their likely callers.
- `leak.sh [rounds]`: RSS, threads and handles over Standard <-> Graphing rounds.
- `winresize.c`: resizes the window from inside Wine (`SetWindowPos`); Xvfb has no window manager and Wine ignores
  external resizes of its unmanaged windows, so `xdotool windowsize` never reaches the app.
- `WINEDEBUG=+dcompperf` prints one line per second: commits, swap chain presents/readbacks, paints, walk and raster
  time, damaged pixels, batched readbacks.

## ComboBox dropdowns: hover offset by the title bar row (2026-09-18, Wine b48f393)

On the Currency page the dropdown highlighted the wrong row: with the pointer on "Mercury - Sun Notes" XAML
highlighted "Mars - The Martian Dollar", one row (35 px) up, and a click selected that wrong row.

The presenter painted exactly what XAML asked for. A one-shot visual tree dump (a temporary `+dcompanim` dump,
triggered by a marker file so it could be taken while the popup was open) showed the popup chain hanging next to
the page content under the island root container, not inside it:

```
container <island root>                      at 0,0
  dcomp   <page content>          1280x800   at 0,0
    ...
      dcomp <content root>        1280x768   at 0,32   (offset 0,32: the app's title bar row)
        ...     sprite  161x44 at 12,286               <- the ComboBox
  dcomp   <popup root>            0x0        at 0,0
    dcomp <popup host>            129x44     at 12,254 (TransformMatrix translate 12,254)
      dcomp <popup>               349x354    at 12,154 (Offset 0,-100)
        sprite 1280x800 at -12,-286                    <- the light-dismiss layer
```

The popup host's y (254) is the ComboBox's position *relative to the content root* (it paints at 286), and the
popup's own full-window light-dismiss sprite asks for a parent at 286 too. So XAML computes a popup's position in
the XamlRoot content root's space but parents the popup's visuals where that offset does not apply. Probing the
hit-test region with the pointer (hover the row, dump the tree, read which row got the hover sprite) put XAML's
row A at client 222..254 against the painted 192..223: a constant +32, the content root's offset, not a scale.

The presenter now carries the content root's offset over to popup roots: the content root is recognized while
painting (a non-zero offset, covering the rest of the window), the branch of the island root that leads to it is
remembered, and the island root's other dcomp children are painted with that offset. Hovering and clicking now
land on the row under the pointer; the visual suite (history, modes, graph resize, hover/press, menu) shows no
change with `WINE_DCOMP_VERIFY_DAMAGE=1`.

The CalendarDatePicker flyout is the other kind: XAML places it in window coordinates (its own offset, 12,128,
is absolute), so the same shift moved it a title bar row down and its days hit-tested a row above where they were
painted. The two are told apart by the tree XAML builds: a flyout placed in window coordinates is hosted in its own
window-sized visual (the layer that dims the window behind it), a ComboBox popup's subtree has nothing window sized
in it, so the presenter walks the popup's subtree before applying the offset (Wine 946c1b5). Checked: the Currency
and "Difference between dates" ComboBox popups and the From/To calendars all highlight and select what is under the
pointer.

`WINEDEBUG=+dcompanim` (committed) traces every animation started on a visual or a property set and the value it
settles at - full `+dcomp` tracing now stalls startup, so the narrow channel is the way to read the popup's
transform chain (the popup opens with `Y -> -53.5`, `ScaleY 0.5@0 -> 1.30398@1` and `Opacity <- "PS.POpacity *
PS.TTOpacity"`; none of it was the cause).

## A second app: Media Player (2026-09-19)

Calculator is one data point, so the question is what an app we have never looked at hits. The Windows install on
`/dev/nvme0n1p2` carries 55 installed packages; `Microsoft.ZuneMusic` (Media Player) is the closest match to the
calculator's stack - the same `Microsoft.UI.Xaml.2.8` + `Microsoft.NET.Native.2.2` + `VCLibs` dependency set, no
WebView2 - while being a much larger XAML app. `tools/wine-uwp/stage-app.sh` stages any installed package the way
`stage.sh` stages the calculator: it reads the executable, the application id and the display name out of
`AppxManifest.xml` and writes an `identity.env` that `run.sh` sources, so nothing app-specific is hard-coded any
more (`UWP_STAGE=media-player tools/wine-uwp/run.sh`).

Three gaps, none of them calculator-shaped:

1. `ParseApplicationUserModelId` was `# @ stub` in kernelbase. `twinapi.appcore` calls it while the app publishes
   its window, the delay-load hook aborts the process, and the fail-fast unwind ends in `ploptin.dll` - nothing in
   the stack says "AUMID parsing". It is a string split on the single `!` separating the package family name from
   the PRAID; implemented it and `FormatApplicationUserModelId` next to
   `PackageNameAndPublisherIdFromFamilyName`, which has exactly the same two-output shape.

2. `Windows.Storage.AppDataPaths` was not implemented. The app's startup path asks for it, the activation fails,
   and the whole thing comes back as .NET Native's "a type initializer threw an exception" with no hint of what
   threw. The class is stateless - nine known folders of the calling user - so the statics, the activation factory
   and the instance are one object in `dlls/windows.storage/appdatapaths.c`, with the properties going to
   `SHGetKnownFolderPath`. Two things the projection needs that a bare getter implementation does not obviously
   imply: the instance must be its own COM identity (delegating `QueryInterface` to the factory hands managed code
   an object whose `IUnknown` is the factory), and `GetRuntimeClassName` must return the class name rather than
   `E_NOTIMPL`, or the managed wrapper cannot be built. The exact interface definitions came out of the Windows SDK
   headers on the same partition (`Include/10.0.26100.0/winrt/windows.storage.h`).

3. `ApplicationData.LocalFolder` was a stub, and a stub is enough to abort the same type initializer. Implemented
   `StorageFolder` for an existing directory - `IStorageFolder` + `IStorageItem`, with `Name`/`Path`/`Attributes`/
   `DateCreated`/`IsOfType` real and the async methods still `E_NOTIMPL` - and pointed `LocalFolder`,
   `RoamingFolder` and `TemporaryFolder` at `%LOCALAPPDATA%\Packages\<family>\{Local,Roaming,Temp}State`,
   creating the directory on first use. It lives in `windows.storage.applicationdata` rather than
   `windows.storage`, where the class belongs, because Wine's build cannot make an import library for a module
   with a dotted name, so the two cannot share one implementation.

With those three, Media Player starts, lays out and renders its own UI. It then puts up its own error page -
"Media Player does not work due to a lack of media framework" - which is the app reacting to
`Windows.Media.Playback.MediaPlaybackList` missing from Wine's `windows.media.dll`. That is the app's own
diagnostic rather than a crash: everything up to and including its XAML, text and app logic works.

Worth noting for the Shapr3D target: the two fixed gaps were both in the app's first seconds, both fatal, and
neither was visible from the calculator. The failure modes are the expensive part - an aborted process whose stack
points at a privacy DLL, and a managed type-initializer exception that names nothing.

### Where five more packages land

With staging generic, running another package costs a minute, so here is the spread (all on :98, 45 s each):

| package | how far it gets |
| --- | --- |
| Calculator | fully working |
| Media Player | renders its own UI, stops at `Windows.Media.Playback.MediaPlaybackList` |
| Windows Security | window up, content still blank, no error |
| Camera | XAML parse error: "The property 'Title' was not found in type 'Microsoft.CameraApp.TitleBar'" |
| Feedback Hub | `ApplicationLanguages.PrimaryLanguageOverride` setter returns `0x80070032` (see below) |
| Solitaire | needs `Windows.System.UserPicker` and shell32's `CreateStorageItemFromShellItem_FullTrustCaller_ForPackage` |

Windows Security only needed `wincorlib.dll` - the C++/CX language projection - staged into the prefix; without it
the loader cannot even resolve the exe's imports (`c0000135`), which reads as a total failure and is a one-line
fix. It is in the override list now.

The Camera failure looked like the interesting one for Shapr3D - XAML cannot find a property on one of the app's
*own* types, which would suggest the app's `IXamlMetadataProvider` table is not consulted the way it is on Windows,
and would then hit any app with custom controls. **That generalisation turned out to be wrong; see the correction
below.**

### Feedback Hub: the PrimaryLanguageOverride setter

The app dies on an unhandled `0x80070032` right after taking the `Windows.Globalization.ApplicationLanguages`
factory, and `+seh` shows a single C++ throw, out of `Bcp47Langs.dll`. `probes/probe-langs.cpp` walks the class
member by member and pins it down: the three getters are fine (`Languages` -> en-US, `ManifestLanguages` -> en-US,
en), and it is the **`put_PrimaryLanguageOverride`** setter that returns `0x80070032` - for any value, including
an empty string, so it is not about the language being unavailable.

Under `+reg` the setter makes no registry write at all before failing, so it bails out early; the last thing it
does is a run of failed opens of
`HKCU\...\AppContainer\Storage\Microsoft.WindowsFeedbackHub_8wekyb3d8bbwe\...`, the per-package hive Wine
does not create. Not proven to be the cause yet. What is ruled out: `GetPersistedRegistryLocationW`, an
undocumented `api-ms-win-stateseparation-helpers` export Wine lacks and `Bcp47Langs` imports - a FIXME stub added
at the right export never printed, so it is not on this path.

## Media Player: fourteen layers deep, still short of the UI (2026-09-19)

The "lack of media framework" page was one missing class; past it the app walks a long chain, and each
link is fatal. In order, with what each needed:

| # | what stopped it | what it took |
| --- | --- | --- |
| 1 | `Windows.Media.Playback.MediaPlaybackList` missing | the class, with a real `IObservableVector<MediaPlaybackItem>` behind `Items`; `MediaPlaybackItem`, the three event-arg classes and the `Windows.Media.Core` track types declared for their shape |
| 2 | `Windows.Storage.StorageFile` missing | `StorageFile` (`IStorageFile` + `IStorageItem` + the two stream references), `IStorageFileStatics.GetFileFromPathAsync`, and an `IAsyncOperation<StorageFile>` |
| 3 | `Windows.Storage.StorageFolder` missing | `IStorageFolderStatics.GetFolderFromPathAsync` plus `IAsyncOperation<StorageFolder>`; the folder implementation moved from `windows.storage.applicationdata` into `windows.storage` where the class belongs, and applicationdata now resolves `__wine_storage_folder_create` with `GetProcAddress` (see the dotted-import-library problem above) |
| 4 | `IApplicationData2` not answered | `LocalCacheFolder`, next to the other three folders |
| 5 | `MediaPlayer.AudioCategory` setter `E_NOTIMPL` | it and `AudioDeviceType` round-trip now |
| 6 | `IMediaPlayer5` not answered | the video-frame-server interface: the flag round-trips, the copies are stubs |
| 7 | `IMediaPlayer3` not answered | implemented; the first attempt had a **vtable one-slot-short bug** - I missed `TimelineControllerPositionOffset` and added a `SetSurfaceSize` that belongs to a later revision, so the app's `get_PlaybackSession` landed on `StepBackwardOneFrame`. A wrong method order does not fail to compile and does not look like a vtable bug in the log |
| 8 | `MediaPlayer` event registration `E_NOTIMPL` | every `add_*`/`remove_*` returns S_OK with a zero token; a handler that can never be registered is fatal, one that never fires is not |
| 9 | `MediaPlayer` scalar properties `E_NOTIMPL` | Volume, IsMuted, AutoPlay, IsLoopingEnabled, PlaybackRate, Position now stored; CanSeek/CanPause/CurrentState answer for a stopped player |
| 10 | `get_PlaybackSession` stub | `MediaPlaybackSession`, 38 members |
| 11 | `IMediaPlaybackSession2` not answered | events, `IsMirroring`; the time-range vectors typed as `IInspectable` since Wine has no `MediaTimeRange` |
| 12 | `get_SphericalVideoProjection` stub | `MediaPlaybackSphericalVideoProjection`, all ten properties |
| 13 | `IMediaPlayerSource2` not answered | `Source` round-trips |
| 14 | `Items` not enumerable | `IIterable`/`IIterator<MediaPlaybackItem>`. The app QI'd for `IIterable` and **fail-fasted** (code 0x1001) rather than taking an error: a WinRT collection that cannot be enumerated kills the process |
| 15 | `Windows.Storage.KnownFolders` missing | `IKnownFoldersStatics`: Music/Pictures/Videos/Documents from `SHGetKnownFolderPath`, and HomeGroup/RemovableDevices/MediaServerDevices as empty folders - returning ERROR_NOT_FOUND for RemovableDevices, which is the truthful answer, is fatal |

It now stops at `IStorageFolderQueryOperations` on the Music library folder - the folder-query stack
(`QueryOptions`, `StorageFileQueryResult` and friends). That is the subsystem a media library app is built
on, so it is the last big piece rather than another small gap, and it is where this run stops.

Two lessons that generalize past Media Player:

- **The fatal/non-fatal split is not where you would guess.** `E_NOTIMPL` from an event registration, from a
  scalar property setter, or from a collection's `IIterable` kills a .NET Native app outright. Answering
  those with S_OK and a do-nothing implementation is what buys forward progress; being honest about a
  missing capability (`ERROR_NOT_FOUND` from `RemovableDevices`) is what stops it.
- **Getting a vtable one slot wrong is invisible.** Item 7 cost the most time of anything here: the build is
  clean, the log shows a plausible method being called, and only counting the SDK's method list against the
  IDL finds it. Worth diffing method-by-method whenever an interface is transcribed by hand.

The calculator is byte-identical through all of it.

### The folder-query stack, and where it lands

`IStorageFolderQueryOperations` turned out to be the last of the data plumbing. Implemented in
`dlls/windows.storage`:

- `Windows.Storage.Search` in the IDL: the `CommonFileQuery`/`CommonFolderQuery`/`IndexedState` enums,
  `IStorageFolderQueryOperations`, `IStorageQueryResultBase`, `IStorageItemQueryResult`, and `IQueryOptions`
  plus the two other result interfaces as shapes. It has to be *defined* in `windows.storage.idl` rather than
  `windows.storage.search.idl`: search cannot import storage (storage already imports search), so the
  search types cannot name `StorageFile`. The definitions go after the `Windows.Storage` namespace closes,
  with forward declarations before it.
- Three read-only `IVectorView` implementations (StorageFile, StorageFolder, IStorageItem), three matching
  `IAsyncOperation<IVectorView<T>>`, plus `IAsyncOperation<UINT32>` and `IAsyncAction`, all cloned from the
  existing `async_bool` pattern.
- A query **is** a directory listing: `GetFilesAsync`/`GetFoldersAsync`/`GetItemsAsync` walk the directory
  with `FindFirstFileW` and hand back a snapshot. The `CommonFileQuery` orderings are ignored and
  `IsCommonFileQuerySupported` admits to only `DefaultQuery` and `OrderByName`; there is no indexer, so
  `StorageItemQueryResult` re-lists on every call and `ContentsChanged` never fires.
- `StorageFile.DeleteAsync` is real now (it was the first thing Media Player asked a file to do).

With that, Media Player gets out of the data layer entirely and starts building its UI. It now fails
constructing `Windows.UI.Xaml.Controls.AutoSuggestBox` - an `E_XAMLPARSEFAILED` from inside
windows.ui.xaml.dll, with no failed activation in front of it, so the reason is internal to XAML rather
than another missing class.

One wrong turn worth recording: the two `dcomp:uicontext_method stub` FIXMEs immediately before the
AutoSuggestBox failure look exactly like the cause. They are not - the calculator hits the same stub three
times per run and works. Making the slot return S_OK with a null out parameter did get past AutoSuggestBox
but crashed the activation with `0xc0000005`, and returning the context itself put the parse error back.
**Check whether a suspicious FIXME also fires in the app that works before chasing it.**

## Feedback Hub works (2026-09-19)

A second app is up and interactive: the NavigationView renders with its icons, clicking **Feedback**
moves the selection and navigates to a real page (title, body text, accent button). No unhandled errors
in the run.

What it took, after the `PrimaryLanguageOverride` registry key and the five DLL/apiset fixes above, was
the `LsaLookup*` family that `sechost` exports for name resolution - undocumented, and all `@ stub`:

- `LsaLookupOpenLocalPolicy` is `LsaOpenPolicy` against the local machine with `POLICY_LOOKUP_NAMES`;
  implemented in `advapi32/lsa.c` and forwarded from sechost the way `LsaOpenPolicy` already is.
- `LsaLookupGetDomainInfo`, `LsaLookupTranslateNames`, `LsaLookupTranslateSids`,
  `LsaLookupUserAccountType`, `LsaLookupSids2`, `LsaLookupFreeMemory` and `LsaLookupClose` return
  `STATUS_NOT_SUPPORTED` (or succeed, for the two that free and close).

The point is not that the implementations do anything - most of them refuse - it is that **a spec stub
aborts the process and a failing call does not**. `@ stub` in a .spec means Wine's loader prints
"Call from ... to unimplemented function" and kills the app; the same function returning
`STATUS_NOT_SUPPORTED` is something the caller handles and walks past. Seven one-line refusals were the
difference between no UI at all and a working app.

### Where Media Player's AutoSuggestBox failure actually is

Not solved, but pinned precisely. `WINE_UWP_TRACE_ORIGINATE=802b000a` (the existing hook in
`combase/roerror.c`) plus `windows.ui.xaml.pdb.syms` gives the chain:

```
CParser::LoadXamlCore -> BinaryFormatObjectWriter::WriteNode -> CreateInstanceFromType
  -> ObjectWriterRuntime::CreateTypeImpl +0x60a  <- reports parser error 0x9e7, returns E_FAIL
```

In the disassembly, `CreateTypeImpl` fails at `+0x135` on an indirect call through vtable slot 1 of the
XamlType (`0x30b0a5`), stores the HRESULT in `%edi`, and branches to the error report at `0x30b546`.
Nothing originates the inner HRESULT, so it never reaches the log. `Windows.UI.Xaml.Controls.AutoSuggestBox`
*is* registered as an activatable class pointing at windows.ui.xaml.dll, so this is not the registry.
Finding the inner HRESULT needs a breakpoint at `0x30b0a5` or a shim on that vtable slot.

### Two more aborts removed, and the delay-load trap

Re-running the whole set after the global fixes surfaced two more of the same kind, both found by
grepping every log for `DelayLoadFailureHook`:

- **Windows Security** aborted on `RMCLIENT.dll.HamQueryCallerTerminateReason`. The staged
  `rmclient.dll` exports it perfectly well - the DLL simply was not in the prefix's system32, so the
  bare-name delay load never found it. Added to the override list.
- **Solitaire** aborted on `ext-ms-win-shell-shell32-l1-2-0.dll.CreateStorageItemFromShellItem_FullTrustCaller_ForPackage`.
  The apiset maps to shell32 and shell32 does not export it; implemented as a refusal in `shellord.c`.

Worth knowing: **Wine's `DelayLoadFailureHook` aborts the process.** A delay-load import is the one
kind of missing export that an app has no chance to handle, so `grep DelayLoadFailureHook` over a log is
the cheapest way to find the next fatal gap - it fires before the "unimplemented function" message.

Neither app has an unhandled error or an abort any more. Neither draws content yet:

| app | state |
| --- | --- |
| Calculator | working |
| Feedback Hub | working, navigates |
| Windows Security | window and title, content blank; waiting on COM class `{e041c90b-68ba-42c9-991e-477b73a75c90}`, which is a Windows service we do not have |
| Solitaire | `Activate failed, hr 0xc0000005` - an access violation, needs its own investigation |
| Media Player | data layer complete, dies creating `AutoSuggestBox` |
| Camera | XAML cannot find `Title` on the app's own `Microsoft.CameraApp.TitleBar` |

### AutoSuggestBox: what it is not

Following the disassembly down from `ObjectWriterRuntime::CreateTypeImpl`: the indirect call is
`XamlNativeRuntime::CreateInstance`, which looks up a per-type activator in a 16-byte-stride table at
RVA `0xc904e0`. Reading that table out of the DLL image and symbolizing every entry gives
`OnCoreCreateAutoSuggestBox` at index 811, which is nothing but a RuntimeProfiler call and a tail-jump
to `CAutoSuggestBox::Create`.

`CAutoSuggestBox::Create` has exactly one early-out - it returns `E_NOTIMPL` when the CREATEPARAMETERS
kind is `0xe` - so I byte-patched the `cmp $0xe,%al` to `cmp $0xff,%al` in our own copy of the DLL to
rule it out. The parse error was unchanged, so the failure is in `CDependencyObject::ValidateAndInit`,
i.e. inside AutoSuggestBox's own initialization. (The patch is reverted; the staged DLL is byte-identical
to the original again.)

### Solitaire's access violation: one missing interface revision

`Activate failed, hr 0xc0000005` came three lines after
`coreinputview:edit_context_QueryInterface {b1867dbb-083b-49e1-b281-2b35d62bf466}` returning
E_NOINTERFACE. That IID is `ICoreTextEditContext2`, which adds exactly one event -
`NotifyFocusLeaveCompleted`. Solitaire QI's for it while building its first page and faults on the null
it gets back rather than checking the HRESULT.

Declared it in `windows.ui.text.core.idl` and implemented it on the existing edit context in
`windows.ui.core.textinput`: registration succeeds with a zero token and the event never fires, because
Wine's edit context has no asynchronous focus handshake to complete. The crash is gone and the app now
runs steadily and gets as far as initialising D3D and a `Windows.UI.Composition.Compositor`; it still
draws nothing, which is a separate problem (it is a game that renders through the compositor rather than
XAML, and it wants to be signed in).

This is the third variant of the same theme: **the fatal thing is usually the refusal, not the missing
feature.** `E_NOTIMPL` from an event registration, a spec stub instead of a failing function, a delay-load
that cannot resolve, and an interface revision answered with E_NOINTERFACE all kill an app that would
have run happily with a do-nothing implementation.

It did not help Media Player's AutoSuggestBox, which was worth checking - AutoSuggestBox contains a
TextBox and so reaches the same edit context.

### Camera's failure is template property resolution, not type resolution

Same originate trace, symbolized:

```
CFrameworkElement::InvokeApplyTemplate -> CContentControl::ApplyTemplate -> CFrameworkElement::ApplyTemplate
  -> CControlTemplate::LoadContent -> CTemplateContent::LoadXbfVersion2
  -> BinaryFormatObjectWriter::WriteNode -> ObjectWriterRuntime::TypeConvertValueImpl
```

So it is not the app's type that XAML cannot find - it found `Microsoft.CameraApp.TitleBar` fine - it is
a *member* of that type, while **type-converting a value inside a ControlTemplate** (the shape of
`<Setter Property="Title"/>` or a `TemplateBinding`, where the property name is a string that has to be
resolved against the target type). For a managed type that resolution goes out to the app's generated
`IXamlMetadataProvider` and asks `IXamlType::GetMember`.

That narrows the Shapr3D-relevant question usefully: **types from an app's own metadata provider resolve,
and so do their properties in ordinary markup** - Calculator's custom controls with custom properties
work - so what fails is specifically the string-keyed member lookup done while loading a template. Worth
checking next whether the member lookup works for a custom property on a custom type *outside* a
template, which would separate "provider member lookup is broken" from "template-time resolution takes a
different path".

### Windows Security's blank content is a missing service, not a missing API

The unregistered COM class `{e041c90b-68ba-42c9-991e-477b73a75c90}` lives in `SecurityHealthAgent.dll` /
`SecurityHealthCore.dll` (found by searching the partition's System32 for the GUID bytes) - the Defender
Security Health service. There is no API gap to close here; the app is correct to show nothing.

### tools/wine-uwp/next-gap.sh

The triage I kept doing by hand, in one script: it reads a run log and reports the fatal gaps ordered by
*how* they kill the app - delay-load aborts first, then .spec stub aborts, then missing modules, then
unresolved WinRT classes, and the app's own unhandled error last, because that one is usually a
consequence of the others. `tools/wine-uwp/next-gap.sh /tmp/*.log` summarises a whole sweep.


### Correction: the custom-type metadata path is not broken

I wrote earlier that Camera's "property 'Title' was not found in type 'Microsoft.CameraApp.TitleBar'" implied the
app-provided XAML metadata path was broken, and that any app with custom controls would hit it. Two checks say
otherwise:

- **Calculator** resolves custom property setters against its own type through exactly this path. `App.xaml` has
  `<Style TargetType="local:TitleBar">` with eight setters for custom dependency properties
  (`ButtonBackground`, `ButtonHoverForeground`, ...), and `CalculatorApp.TitleBar` is not in any winmd in the
  package - it is resolved through the app's generated metadata provider, same as Camera's.
- **Feedback Hub** is a .NET Native app (`mrt100_app.dll`, the `Microsoft.NET.Native.*.2.2` dependencies) and
  renders a full custom XAML UI that navigates between pages. So the *managed* provider works too, not just the
  C++/CX one.

So Camera's failure is specific to that type or that property, not a class of failure that Shapr3D would inherit.
Good news for the target, and it redirects the effort: there is no general metadata gap to close.

## Feedback Hub is properly interactive, and two font findings (2026-09-19)

Exercising the working app: clicking **Settings** renders a full page - radio group with the selection
drawn, a checkbox, a button, four hyperlinks with their external-link glyphs, version text - and
clicking **Light** repaints the entire app from dark to light theme. Nothing about it is a static
screenshot; the app is live.

The three WinRT classes still unresolved for it (`Windows.System.User`,
`Microsoft.UI.Private.Controls.CBSPackageDetector`) cost nothing visible, which is worth knowing before
spending effort on them. `Windows.System.User` is not fixable by registering a DLL anyway: in the real
Windows registry it has **no `DllPath`** - it is `ActivationType=1, Server=UserManager`, an out-of-process
class hosted by a Windows service, as are `Windows.System.UserPicker` and
`Windows.Internal.StateRepository.Application`. (Read out of the partition's SOFTWARE hive with
`tools/wine-uwp/fasthive.py`.) `CBSPackageDetector` is not in the Windows registry at all.

One cosmetic defect is visible: the copyright sign in "(c) 2026 Microsoft Corp." draws as a
missing-glyph box. `probes/probe-glyph.cpp` asks DWrite what it can resolve, and found two real gaps
while failing to confirm that one:

- **Segoe UI Variable's optical-size families are missing.** Wine's font collection has 380 families
  including `Segoe UI` and `Segoe UI Variable`, both of which have U+00A9 and U+2122 with real glyph
  ids. But `Segoe UI Variable Text`, `Segoe UI Variable Small` and `Segoe UI Variable Display` - three
  separate families on Windows 11, and the default UI font for Windows 11-era XAML - are **not found**.
  Wine's freetype backend exposes `SegUIVar.ttf` as the single family `Segoe UI Variable` rather than its
  named optical-size instances. Copying the real font files from the partition into the prefix changes
  nothing, because fontconfig was already serving them; the gap is in how Wine enumerates a variable
  font's named instances.
- **`MapCharacters` can return S_OK with a NULL font.** For an unknown base family the whole run falls
  back to Tahoma, and for U+2122 it returns `hr 0, mappedLength 1, font NULL` - a caller that does not
  treat a NULL font as "cannot render" will draw nothing or a box.

Neither is confirmed to be the cause of the copyright box - the fonts that do resolve have the glyph, and
the page's text visibly renders in a Segoe face rather than Tahoma, so the run is not falling back the
way the probe's unknown-family case does. Recorded as measured facts, not as the diagnosis.

## I broke the calculator with wineboot, and what it taught us (2026-09-19)

While testing the font staging I ran `wine wineboot -u` to force a font rescan. **That rewrote 719
DLLs in the prefix's system32**, replacing the set the prefix had been running since 09-09 with
freshly built ones. `install-overrides.sh` only restores the ~30 native modules on its list, so the
rest stayed at their new versions. The calculator has failed ever since with
`Cannot create instance of type 'Windows.UI.Xaml.Controls.Pivot'`.

**Do not run `wineboot` against this prefix.** There is nothing in the tree that restores it.

### What the regression actually is

`windows.graphics.dll` is the DLL behind `Windows.Graphics.Display.DisplayInformation`. Before
wineboot the prefix had a copy that worked; wineboot replaced it with a build of the current tree, in
which `DisplayInformation` is not compiled at all - `displayinformation.c` exists but is not in
`Makefile.in`'s `SOURCES`, so `GetForCurrentView` is the `E_NOTIMPL` stub in `main.c`. The comment at
the top of `displayinformation.c` says exactly what follows: *"XAML asks for it while it lays out (a
Pivot cannot be created without it)"*.

The evidence, from `WINE_UWP_TRACE_ORIGINATE=802b000a` plus the XAML symbols:

- The failing stack is identical to Media Player's AutoSuggestBox: `ObjectWriterRuntime::CreateTypeImpl`.
- In the working log `Windows.Graphics.Display.DisplayInformation` is activated **once** and the run
  continues; in the broken log it is activated **nine times**, XAML retrying as each one fails.

So the AutoSuggestBox failure is very likely the same shape - a control whose initialisation needs
something Wine answers with `E_NOTIMPL` - which is a much better lead than "something inside XAML".

### The fix, and the false trail that delayed it

Fixed by compiling the DisplayInformation implementation that was already sitting in the tree:
`displayinformation.c` and `weakref.c` added to `Makefile.in`'s `SOURCES`. The calculator is
byte-identical to its baseline screenshot again and Feedback Hub is unchanged.

The link needs `EXTRADLLFLAGS = -Wl,--allow-multiple-definition`: `main.c` and
`displayinformation.c` both emit the selectany `RuntimeClass_Windows_Foundation_*` strings from
windows.foundation.h and mingw ld does not merge them here. Adding `initguid.h` to the second file
only trades those for duplicate `IID_*`, and removing it from `main.c` leaves the IIDs undefined
because libuuid has no WinRT IIDs. Worth a proper fix; the flag is marked as temporary in the file.
(The two .c files are `git add -N`'d, which `make_makefiles` requires - it builds `SOURCES` from
`git ls-files`.)

**What cost the most time was a second self-inflicted fault on top of the first.** While testing, I
briefly added `windows.graphics` to `install-overrides.sh`'s override list. That script writes
`HKCU\Software\Wine\DllOverrides\<name> = native` for everything in the list and **never removes
entries**, so taking the name back out of the list left the registry forcing *native-only* - with no
native file installed. Every subsequent load failed with `c0000135`, which made a correctly built
builtin look unloadable and sent me off testing the native DLL instead. `WINEDEBUG=trace+module`
says it in one line: `get_load_order_value got standard key n for L"windows.graphics"`. **A stale
DllOverrides value outlives the list it came from; check the registry, not the script.**

Also fixed on the way: `shcore.RegisterWindowMonitorChangeListener` and its Unregister were spec
stubs, so the native windows.graphics aborted the process on them. They are refusals now.

### AutoSuggestBox and the UIContext slot both callers want differently

With DisplayInformation working, Media Player's AutoSuggestBox failure turned out to hinge on the same
place as before: the second private method of `Windows.UI.UIContext`, vtable slot 7, in
`dlls/dcomp/device.c`. The three behaviours are now measured, not guessed:

| slot 7 returns | result |
| --- | --- |
| `E_NOTIMPL` | AutoSuggestBox creation fails; MRM handles the error fine |
| `S_OK` with a NULL out | AutoSuggestBox is **created**, then MRM faults |
| `S_OK` with the context itself | AutoSuggestBox creation fails again |

The fault is `Microsoft::Resources::CoreWindowContainer::Initialize+0x106` in MrmCoreR.dll
(symbolized from `MrmCoreR.pdb.syms`). Disassembling it: MRM takes the CoreWindow, QIs it for
`ICoreWindowWithContext` (`9ac40241-3575-4c3b-af66-e8c529d4d06c`), walks from there to the UIContext,
calls slot 7 for an object and then QIs *that* for `84acbbfe-649c-5ac7-8130-7b6b76db463c` - a
parameterized IID that is in no SDK header - without null-checking. Every other call in that function
is `test eax,eax; js <error>`, so a failing HRESULT is handled and a successful-but-empty one is not.

So the slot has to hand back a real object of a type we cannot yet name. Left returning `E_NOTIMPL`,
which is the behaviour the calculator and Feedback Hub need; the alternative trades a working
reference app for one more control in one app, which is not a trade worth making. The three
measurements and the MRM call chain are the concrete handle for next time.

## End-to-end check of the reference app, and CoreTextServicesConstants (2026-09-19)

After a day of DLL churn, a first-frame screenshot is not enough evidence, so the calculator was
driven rather than just launched: 1234 + 876 = **2,110** with the history panel populated, the
navigation menu opens with all sixteen entries and icons, Programmer and Graphing both switch and
render, and typing `x` into the Graphing expression and committing it with the keypad's enter key
**plots the line**, with the axes rescaling to ±8 and a second equation row appearing. The graphing
engine, the D2D path and the history/memory panes all still work.

One thing worth remembering about driving this app: `xdotool key Return` does **not** commit a
graphing expression, and a typed `*` does not register. Only the keypad's enter button commits. I
briefly took "no curve" as a regression from the day's changes when it was just an uncommitted
expression.

That run's log showed two classes registered to `windows.ui.core.textinput.dll` but not implemented.
The first is now done:

**`Windows.UI.Text.Core.CoreTextServicesConstants`** is a statics-only class with a single property,
`HiddenCharacter`. Rather than guess the constant, it came out of the shipping
`Windows.UI.Core.TextInput.dll`: scanning for `mov word ptr [rdx], imm16` finds exactly one
plausible getter, at file offset 0x61d20, and its whole body is
`66 c7 02 fd f8 / 33 c0 / c3` - `*value = 0xF8FD; return S_OK;`, surrounded by int3 padding. So the
answer is U+F8FD, read off the real implementation rather than inferred.

`Windows.UI.ViewManagement.Core.CoreFrameworkInputView` is still missing; it needs the
`ICoreFrameworkInputView` surface (occluded rects and their changed event), which is more than a
constant.

### The UIContext slot, read off the shipping dcomp.dll

The interface our dcomp hands out as "UIContext" is not the empty public `IUIContext` - it is
`IUIContextPartner`. Read out of the shipping `dcomp.dll` with `dcomp.pdb.syms`: `UIContext` is a WRL
`RuntimeClass<IUIContext, CloakedIid<IUIContextPartner>, CloakedIid<IUIContextInternal>>`, and the
partner vtable (`??_7UIContext@UI@Windows@@6B<partner>`) is IInspectable followed by exactly two
methods - `get_UIContentRoot(IUIContentRoot **)` and `get_WindowContext(IWindowContextPartner **)`.
So the slot everything was calling, vtable index 7, is **`get_WindowContext`**.

It now returns a real (empty) object instead of `E_NOTIMPL`. MrmCoreR's `CoreWindowContainer::Initialize`
QIs it for `84acbbfe-649c-5ac7-8130-7b6b76db463c` and **does** check the result - the disassembly at
`0x1800239e1` is `test eax,eax; jns <success>` with a logging call on the failure path - so refusing
that IID is handled. The calculator and Feedback Hub are unchanged by it.

**A correction to the earlier entry:** I recorded that returning `S_OK` with a null out parameter
"created the AutoSuggestBox and then faulted MRM". That was wrong. MRM's `CoreWindowContainer::Initialize`
runs *early*, so the null faulted it before XAML ever reached the AutoSuggestBox page - the control was
never created, the app just died sooner. The three-row table in that entry should be read as "what
happened to the run", not "what happened to AutoSuggestBox".

With the slot implemented, AutoSuggestBox still fails, and the UIContext work turns out to be
unrelated to it. What is now established about that failure, from the vtables rather than by guessing:
`ObjectWriterRuntime::CreateTypeImpl` calls vtable slot 1 of the XAML runtime, and slot 1 of both
`??_7XamlNativeRuntime@@6B@` and `??_7XamlManagedRuntime@@6B@` is `CreateInstance` - so the failing
call is `XamlNativeRuntime::CreateInstance`. Inside it, the per-type activator is found in a table at
RVA `0xc904e0` (16-byte stride), AutoSuggestBox's entry is index 811 `OnCoreCreateAutoSuggestBox`,
which tail-jumps to `CAutoSuggestBox::Create` -> `CDependencyObject::ValidateAndInit` -> vtable slot 24.
Slot 24 of `CAutoSuggestBox`, `CItemsControl` and `CControl` is the *same* trivial `xor eax,eax; ret`,
so initialisation succeeds and the failure is in what `CreateInstance` does **after** the activator
returns - the type-info lookup and flag checks at `0x18020a03b` onwards.

### AutoSuggestBox: the failure is the provider-kind dispatch, proved by patching

Tracing `ObjectWriterRuntime::CreateTypeImpl` properly (`0x30af70`) rather than guessing:

```
mov (%r8),%r14                  ; r14 = the XamlType
mov $0x80004005,%edi            ; default hr = E_FAIL
lea 0xb0(%r14),%r15             ; r15 = the XamlTypeToken inside it
mov (%r15),%ecx ; shr $0x1c,%ecx ; sub $1,%ecx
jne 0x18030b218                 ; kind != 1 (native)
  0x18030b218: cmp $1,%ecx ; jne 0x18030b42b   ; kind != 2 (managed)
    0x18030b42b: <record edi> -> returns the default E_FAIL
```

Both the native and the managed branch fetch a cached runtime out of the schema context (slots +0x30/+0x38
and +0x40/+0x48) and converge on the *same* call site, whose slot 1 is `CreateInstance` - confirmed by
dumping `??_7XamlNativeRuntime@@6B@` and `??_7XamlManagedRuntime@@6B@`. So a token whose provider kind
is neither 1 nor 2 never calls anything: it falls straight out with the E_FAIL that was loaded before the
dispatch, and that becomes parser error 0x9e7, "Cannot create instance of type".

Confirmed by patching our copy of the DLL, one branch at a time:

| patch | result |
| --- | --- |
| force the **native** path (`jne` at `0x30b00b` -> nops) | the parse error is **gone**; the app then throws a managed `ArgumentOutOfRangeException` (0x80131502, "Parameter name: index") |
| force the **managed** path (`jne` at `0x30b21b` -> nops) | same "Cannot create instance" error - the app's managed activation genuinely fails for this type |
| unpatched | E_FAIL from the dispatch itself |

So: the token for `Windows.UI.Xaml.Controls.AutoSuggestBox` in this app's XBF carries a provider kind
that is neither native nor managed, the native activator *can* build the control (index 811 in the table
at `0xc904e0`), and the app's own managed activation cannot. The remaining question is what sets that
token - it is type *resolution*, not type *creation*, that is wrong, which is the same conclusion the
Camera trace reached from the other end ("the property was not found in type" while resolving a name
against a target type).

Both patches are reverted; the staged `Windows.UI.Xaml.dll` is byte-identical to the original (verified
against two separate copies taken earlier in the day) and the calculator baseline is intact.

## AutoSuggestBox: the failure is InputPane, not type resolution (correcting the entry above)

The previous entry concluded that "the token for `Windows.UI.Xaml.Controls.AutoSuggestBox` in this
app's XBF carries a provider kind that is neither native nor managed ... it is type *resolution*, not
type *creation*, that is wrong". **That is wrong**, and the way it was wrong is worth keeping: it was
inferred from two byte-patch experiments (force-native removed the parse error, force-managed
reproduced it) that are equally consistent with the token simply *being* managed. An inference from
two data points that a third reading also explains is not a result.

Re-done with a probe that answers yes/no per basic block instead: overwrite the first two bytes of a
candidate block with `0f 0b` (`ud2`) in our copy of `Windows.UI.Xaml.dll` and see whether the run
dies. A hit shows up as `err:seh:NtRaiseException Unhandled exception code 1007`; the address in that
line is the Wine-side rethrow and is the same for every probe, so it carries no information - only
the presence of the line does. Always run a *control* probe (`ud2` at the entry of a function that
must be called) before trusting a negative; three negatives in a row are much more likely to mean the
patched file is not the file being loaded.

Probe results, in order (`Windows.UI.Xaml.dll` RVAs):

| RVA | meaning | fired |
| --- | --- | --- |
| `0x30af70` | `ObjectWriterRuntime::CreateTypeImpl` entry (control) | yes |
| `0x30b42b` | provider kind neither native nor managed -> preloaded `E_FAIL` | **no** |
| `0x30b2a2` | `XamlType`'s runtime shared_ptr is null -> preloaded `E_FAIL` | **no** |
| `0x30b4e3` | the runtime's `CreateInstance` returned a failure | yes |
| `0x23b450` | `XamlManagedRuntime::CreateInstance` entry | yes |
| `0x23b53c` | `XamlManagedRuntimeRPInvokes_CreateInstance` returned a failure | yes |
| `0x23ac0d` | token handle below `0x10000000` | no |
| `0x23ac52` | the app's own `IXamlType::ActivateInstance` failed | no |
| `0x23ac72` | `DXamlCore::GetPeerPrivate` failed | yes |
| `0x14e313/31f/8cc/974/9fa` | all five of `GetPeerPrivate`'s own refusals | no |
| `0x14e6eb` | the peer's vtable slot 13 returned a failure | yes |

So the provider kind is fine, the type resolves, the native activator
(`OnCoreCreateAutoSuggestBox`, index 811 in the table at `0xc904e0`) runs, and the DirectUI peer is
allocated (`ctl::CreateComObjectInstanceNoInit<DirectUI::AutoSuggestBox>`, index 811 in the *second*
table at `0xc904e8` - the two tables are the same 16-byte entries, activator at +0 and peer creator
at +8). The peer's vtable lives at object+0x10 (`0xdf3748`); slot 13 of it is
**`DirectUI::AutoSuggestBox::PrepareState`**, and that is what fails.

`PrepareState` calls `ItemsControl::PrepareState`, then gets the activation factory for
`Windows.UI.ViewManagement.InputPane`, then `VisualTree::GetContentRootForElement` ->
`CContentRoot::GetUIContext` -> `IInputPaneStatics2::GetForUIContext`, and registers Showing/Hiding
handlers. A control that fails to construct because it could not subscribe to the on-screen keyboard
is the same shape as every other blocker in this file: the refusal is fatal, the missing feature is
not.

Two separate gaps there, one ours and one Wine's:

- **Wine's** `dlls/windows.ui/inputpane.c` answered `add_Showing`/`add_Hiding` with `E_NOTIMPL` and
  `OccludedRect` likewise. Implemented for real (handlers stored in the instance under a critical
  section, real tokens, `OccludedRect` returns an empty rect). This does *not* fix Media Player,
  because `run.sh` sets `windows.ui=n` and the class is registered to the extracted
  `twinapi.dll` - but it is a real fix for any app that does reach Wine's copy.
- **Ours**: the line immediately after the InputPane factory in the log is
  `fixme:dcomp:window_context_QueryInterface window context: no interface
  {84acbbfe-649c-5ac7-8130-7b6b76db463c}`. That is the empty `window_context` added for the UIContext
  slot (`get_WindowContext`, see the entry above) being asked by twinapi for a private interface it
  does not have. The IID is in `twinapi.dll`, `Windows.UI.dll`, `Windows.Graphics.dll`, `MrmCoreR.dll`
  and six others, and in **none** of the public `.winmd` files, so it is internal and has to be
  reversed rather than looked up.

Next: find the QI site in `twinapi.dll` and work out what `GetForUIContext` wants from the UIContext.

### The UIContext walk is only looking for an HWND

`twinapi.pdb` off the public symbol server names the caller:
`CInputPaneEventSourceFactory::GetForUIContext(IUIContext *, IInputPane **)` at RVA `0x18da0`. It
does not read a single field of the UIContext. It walks:

```
QI(uicontext, {f21e14c1-...} IUIContext)      -> V1
V1->slot7(&V2)                                 -> V2   (get_WindowContext - already implemented)
QI(V2, {84acbbfe-...})                         -> V3   <- returned E_NOINTERFACE, ending the walk
V3->slot7(&V4)                                 -> V4
V4->slot7(&hwnd)                               -> an HWND, not an interface
CInputPaneEventSource_GetInstance(hwnd, {640ada70-06f3-4c87-a678-9829c9127c28}, out)
```

Every hop is IInspectable plus one unused method with the getter at slot 7, and the whole chain
exists to turn a UIContext into the window handle that the public
`IInputPaneInterop::GetForWindow` takes directly. So `dlls/dcomp/device.c` now carries an HWND down
the chain: `create_uicontext` takes the view and asks it for `ICoreWindowInterop::get_WindowHandle`
(`{45d64a29-a63e-4cb6-b498-5781d298cb4f}`), the window context keeps it, and the two `window_host`
hops hand it over. Failing that, `GetActiveWindow()`.

**Media Player now gets past XAML entirely.** No parse error, no `Cannot create instance of type`;
the log shows it building its real UI - `NavigationViewItemSeparator`, `ToggleButton`,
`SolidColorBrush`, `ElementCompositionPreview`, `Windows.UI.Colors`.

It now fail-fasts (WIL, code `0x1007`) further in: `CCollection::Add` ->
`DependencyObjectCollection::Append` -> `RaiseVectorChanged` -> `TrackerEventSource<VectorChangedEventHandler>::Raise`
-> the app's own handler in `Microsoft.Media.Player.dll`. Immediately before it,
`iertutil:uri_QueryInterface` refuses `{659a8956-ef29-4f50-8724-7e3207d23076}` and
`{4bd682dd-7554-40e9-9a9b-82654ede7e62}` repeatedly - the next thing to look at, and the same shape
of bug as this one.

### Regression check after the dcomp change

`Calculator` screenshot byte-identical to the pre-change baseline; Feedback Hub still starts clean
(no app error, no fail-fast); Camera still fails on its own unrelated
`The property 'Title' was not found in type 'Microsoft.CameraApp.TitleBar'`.

Media Player's remaining abort is in *its own* managed code: the package is .NET Native
(the PDB path in `Microsoft.Media.Player.dll` is `...\ilc\Microsoft.Media.Player.pdb`, and
`SharedLibrary.dll` is the ProjectN shared runtime), and neither PDB is on the public symbol server,
so the unwind past `TrackerEventSource<VectorChangedEventHandler>::Raise` cannot be named. The
`IReferenceTracker` (`{11d3b13a-...}`) and `IPropertyValue` (`{4bd682dd-...}`) QIs refused around it
are normal - .NET Native's RCW asks every WinRT object for them and handles the refusal.

For Camera: `DirectUI::XBFXamlTypeWrapper::GetMember` (`0x8f45c0`) just forwards to the wrapped
`IXamlType`'s slot 20, so the app's own generated XamlTypeInfo is what fails to find `Title`. That
points at an earlier lookup returning the wrong `IXamlType`, not at a missing API.

## Media Player's fail-fast: a fake object written into an HSTRING out-parameter

The abort after the UIContext fix was not managed code throwing. `mrt100_app.pdb` (public symbol
server) names the frames: `RhpPInvokeExceptionGuard` -> `RhpFailFastForPInvokeExceptionPreemp`. That
guard is the SEH handler on a CoreRT P/Invoke transition frame - **any** native exception during a
P/Invoke is a fail-fast, because it cannot be unwound through managed frames.

`WINEDEBUG=+seh` gives the exception: `EXCEPTION_ACCESS_VIOLATION`, `info[0]=0 info[1]=1`, i.e. a read
of address `1`, at `VCRUNTIME140_APP.dll+0x10c4b`, which disassembles to `rep movsb`. Resolving the
P/Invoke's IAT slot in `SharedLibrary.dll` (`0x55c190`) names the callee: `mrt100_app.dll!memmove`.
At the fault `rdx` (the source) is `1` and `rcx` (the count) is `0xdffe`. So managed code called
`memmove(dst, (void *)1, 57342)`.

Note the two apps are the same stack - Feedback Hub is also WinUI 2.8 on .NET Native 2.2 and works -
so this was Media Player specific, and `WINEDEBUG=+dcomp` says what it does differently: implicit
animations. The trace before the abort is

```
compositor_CreateVector3KeyFrameAnimation
kf_animation_QueryInterface ... {c8039daa-...}   IVector3KeyFrameAnimation
kf_animation_QueryInterface ... {bcb4ad45-...}   ICompositionObject
kf_animation_QueryInterface ... {845bf0b4-...}   (IKeyFrameAnimation / ICompositionAnimation)
comp_object_default_slot Vector3KeyFrameAnimation ... a 0x1 b 0x0: ignored
comp_object_default_slot Vector3KeyFrameAnimation ... a 0x0 b <stack>: ignored
```

Only the typed interface has a real vtable (`kf_vector3_vtbl`); every other interface gets the
generic `kf_animation_vtbl`, whose slots 6+ are all `comp_object_default_slot`. That helper probes
each argument with `is_out_pointer()` - which is what the 23 caught access violations inside
`dcomp!is_out_pointer` in an `+seh` log are, they are deliberate - and writes **a pointer to a fake
recording object** into the first one it finds.

For a key frame animation those out-parameters are not objects. They are the `IKeyFrameAnimation`
accessors: `HSTRING *` (`get_Target`), `TimeSpan *` (`get_Duration`, and note the `0x2dc6c0` = 3 000 000
= 0.3 s passed to the setter), `UINT32 *` (`get_KeyFrameCount`). Writing a fake object pointer into
an `HSTRING *` hands .NET Native a bogus HSTRING; marshalling it to a `string` reads a garbage buffer
pointer and length out of it and `memmove`s them - `(1, 57342)`.

`NULL` is a valid empty HSTRING, a zero TimeSpan and a zero count, so `comp_object_default_slot` now
writes NULL rather than a fake object when the receiver is a key frame animation. The
`comp_object_vtbl` / `expression_animation_vtbl` clone path that XAML depends on is untouched.

**Media Player no longer fail-fasts.** It now reports a plain XAML error instead:
`Cannot add instance of type 'Microsoft.Toolkit.Uwp.UI.Animations.Vector3Animation' to a collection
of type 'Microsoft.Toolkit.Uwp.UI.Animations.ImplicitAnimationSet'` - the Windows Community Toolkit's
implicit-animation types, and an app-metadata type check rather than a missing API. That is the same
shape as Camera's `The property 'Title' was not found in type 'Microsoft.CameraApp.TitleBar'`: both
are lookups in the app's own generated XamlTypeInfo.

Regression checks after the change: Calculator screenshot byte-identical to baseline, Feedback Hub
clean, Solitaire unchanged (no fail-fast, no app error).

Also taught `comp_object_caller_slot` the `call <reg>` form so non-MSVC callers can be decoded; the
callers here still do not match any pattern and report slot 0, so the method is identified from the
QI that precedes it rather than from the slot.

### Naming the slots: one thunk per slot

`comp_object_caller_slot()` decodes the caller's `call [reg+disp]` to work out which method was
called. That cannot work for .NET Native: its interop stub takes the slot index as an *argument*.
The call site in `Microsoft.Media.Player.dll` is

```
movsxd rdi, r8d              ; r8d = slot index, passed in
mov    rdx, [rax]            ; vtable
mov    rdi, [rdx+rdi*8]      ; method
...                          ; P/Invoke transition frame + GC poll
call   rax
```

so every Composition call from the app shares one return address (one of them accounts for 175 of
the calls in a run) and decodes as slot 0. `dlls/dcomp/composition.c` now generates 256 one-line
thunks, `comp_object_slot_00` .. `comp_object_slot_ff`, and the vtable fills take
`comp_object_slot_fn( i )`; slots past 255 keep the old decoder. Calculator stays byte-identical and
Feedback Hub stays clean with the thunks in.

With real slot numbers the tail of the Media Player run reads as `IMap<HSTRING, ICompositionAnimationBase>`
on the `ImplicitAnimationCollection` (slot 6 `Lookup`, 7 `get_Size`, 8 `HasKey`, 10 `Insert`):

```
slot 8  a 0x0            b <stack>     HasKey(NULL, &found)
slot 10 a 0x0            b <animation> Insert(NULL, animation, &replaced)
slot 6  a 0x0            b <stack>     Lookup(NULL, &value)
```

**The key is NULL.** The Community Toolkit keys the implicit-animation map by the Composition
animation's `Target`, and we never stored the `Target` the app set - `put_Target` is swallowed by the
generic slot handler and the matching getter now answers NULL (before the fix it answered a fake
object pointer, which is what produced the `memmove` fail-fast). Hence
`Cannot add instance of type 'Vector3Animation' to a collection of type 'ImplicitAnimationSet'`.

The underlying design problem is visible now: `kf_animation_QueryInterface` hands the *same* generic
vtable back for every interface it is asked for, so slot 7 in the trace is a different method
depending on which IID the app most recently queried - the app asks this one animation for
`{c8039daa}` (IVector3KeyFrameAnimation), `{bcb4ad45}` (ICompositionObject), `{126e7f22}`,
`{845bf0b4}` and `{369b603e}`, and none of the last three are in Wine's
`include/windows.ui.composition.idl` or in the public `.winmd` set. Until QI returns a distinct
vtable per IID, a slot number alone cannot be mapped to a method, and the handler cannot know whether
an out-parameter is an `HSTRING *`, a `boolean *` or an `IInspectable **`.

A concrete latent bug of the same kind, now visible in the trace: `HasKey(key, boolean *found)` on a
non-animation object still takes the `create_comp_object` path, which writes **eight bytes** - a
pointer - through a `boolean *`.

Next step for Media Player: give the composition objects a per-IID vtable, starting with storing and
returning `CompositionAnimation.Target` so the Toolkit's map gets a real key.

## Reading the metadata: tools/wine-uwp/winmd.py

Naming an interface from its IID, and getting its method order, is what turns a dcomp slot number
into something actionable, and nothing in the tree could read a `.winmd`. `tools/wine-uwp/winmd.py`
is a minimal ECMA-335 reader (no dependencies) that does exactly that:

```
winmd.py Windows.UI.winmd --guid 369b603e-a80f-4948-93e3-ed23fb38c6cb
winmd.py Windows.UI.winmd --type Windows.UI.Composition.IKeyFrameAnimation
winmd.py Windows.UI.winmd --grep Animation
```

It prints slots starting at 6, i.e. already offset past IUnknown and IInspectable. Checked against
`IVector3KeyFrameAnimation`, which Wine's `include/windows.ui.composition.idl` also declares: both
say `InsertKeyFrame`, `InsertKeyFrameWithEasingFunction`.

The three interfaces Media Player asks a key frame animation for, which are in the metadata but not
in Wine's IDL:

| IID | interface | slots |
| --- | --- | --- |
| `126e7f22-...` | `IKeyFrameAnimation` | 6 get_DelayTime, 7 put_DelayTime, 8 get_Duration, 9 put_Duration, 10/11 IterationBehavior, 12/13 IterationCount, 14 get_KeyFrameCount, 15/16 StopBehavior, 17/18 InsertExpressionKeyFrame |
| `845bf0b4-...` | `IKeyFrameAnimation3` | 6 get_DelayBehavior, 7 put_DelayBehavior |
| `369b603e-...` | `ICompositionAnimation2` | 6 SetBooleanParameter, 7 get_Target, 8 put_Target |

That confirms the reading of the trace: `slot 9 a 0x2dc6c0` is `put_Duration(3 000 000)` = 0.3 s, and
it also shows why one vtable cannot serve every interface - slot 7 is `put_DelayTime` on
`IKeyFrameAnimation` and `get_Target` on `ICompositionAnimation2`.

## Media Player builds its UI

Two implementations, both per-IID views rather than the generic slot handler:

- **`ICompositionAnimation2` on key frame animations**, storing `Target`. The Community Toolkit keys
  its implicit-animation map by it. `put_Target` now records `"Opacity"`, `"Translation"`, `"Offset"`.
- **`IMap<HSTRING, ICompositionAnimationBase>` (`4ba9aa1d-...`) on composition objects**, with real
  storage: an `ImplicitAnimationCollection` is a map and the Toolkit round-trips through it
  (`HasKey`, `Insert`, `Lookup`). Under the generic handler `Insert` recorded nothing and `Lookup`
  returned `S_OK` without writing an animation.

**A first attempt at the map put its storage in `struct comp_object` and broke Calculator**, which
is worth recording: those three fields (`vtbl`, `kind`, `ref`) are the shared prefix of every
recording object in the file - `kf_animation`, `comp_shape` and the typed brushes all begin with them
and reuse `comp_object_AddRef` / `comp_object_Release`. The new `entry_count` landed on
`kf_animation`'s `target[]`, so releasing an animation iterated `0x664f` entries of garbage; the
fault report shows `rdx 007300660066004f`, which is `"Offs"` from `"Offset"`. The map now lives in
its own allocation, found by owner through a small table.

With both in, Media Player gets through the whole visual tree - the log shows it activating
`Slider`, `ToggleSwitch`, `AppBarToggleButton`, `ToggleMenuFlyoutItem`, `RangeBase` and
`Microsoft.UI.Xaml.Controls.ProgressRing` - and **creates a 1280x800 `ZuneMusic` window**. It paints
nothing yet and ends on `REGDB_E_CLASSNOTREG` for `Windows.Internal.StateRepository.User`.

That class is `ActivationType=1, Server=StateRepository` in the Windows hive - service-activated,
no `DllPath`, which is why `gen-classes-reg.py` skips it. Registering it in-process does not work:
`Windows.StateRepositoryClient.dll` and `Windows.StateRepositoryBroker.dll` both export
`DllGetActivationFactory` and both contain the class name, but both answer
`CLASS_E_CLASSNOTAVAILABLE` for it (and the client DLL's failure path fail-fasts the app 24 times).
The name is only a reference in those binaries; the implementation is in the service. Registration
backed out.

Regression checks with all of the above in: Calculator byte-identical, Feedback Hub clean, Solitaire
clean (no fail-fast, no page fault, no app error).

## Serving a service-activated class: Windows.Internal.StateRepository

`Windows.Internal.StateRepository.User` is `ActivationType=1, Server=StateRepository` in the Windows
hive, so `gen-classes-reg.py` skips it and `RoGetActivationFactory` answers `REGDB_E_CLASSNOTREG`.
It cannot be registered in-process: `Windows.StateRepository.dll` exports only `ServiceMain` and the
`StateRepository_*` service entry points, and the client and broker DLLs answer
`CLASS_E_CLASSNOTAVAILABLE` for the class even though they contain its name.

It can, however, be **reimplemented**, because the layouts are all recoverable:

- `HKCR\Interface\{iid}` in the Windows hive maps IID to interface name.
  `{84103ccb-2fd7-4d6c-962e-5d8582b4c720}` is
  `__x_Windows_CInternal_CStateRepository_CIUserStatics`; `{5232f8ea-...}` is `...CIUser`. A scan of
  that key found 173 interfaces in this family.
- `Windows.StateRepository.pdb` from the public symbol server
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/microsoft-public-symbols)
  names the WRL server classes, and their **vtables** give the exact method order:
  `??_7UserFactoryServer@...` at RVA `0x290730` is `IUserStatics`, `??_7UserServer@...` at
  `0x28bfe0` is `IUser`. The mangled names give the signatures - `?Exists@...@UEAAJ_JPEAE@Z` is
  `HRESULT Exists(INT64, boolean *)`.

`tools/wine-uwp/shims/staterepository/` implements both interfaces from those vtables, answering for
the one user a Wine prefix has (real SID from the process token). `install-overrides.sh` copies it
into the prefix and registers the classes. The trace confirms the layout was right: the app calls
`IUserStatics::GetCurrent`, which is slot 13.

Anything else in the family gets a **generic traced table** - 64 one-per-slot thunks, the same
technique as the composition slots - so a run names the method instead of guessing at it. That
immediately paid off: Media Player asks `Windows.Internal.StateRepository.Application` for
`IApplicationStatics` `{07adcc9a-...}` and calls slot 55, which the PDB names
`FindByUserAndPackageFamilyNameAndAppListEntry`. Its out-parameter is an `IVectorView`, and
answering `S_OK` with null fail-fasted the app 46 times; an empty view is the honest answer and is
what the shim returns.

### is_out_pointer must not fault

With the state repository served, the app's *managed* code started calling composition methods
directly, and `dcomp`'s `is_out_pointer()` began killing the process. Its probe was a dereference
inside `__TRY`, which raises a real access violation for every argument that is not a pointer -
about twenty per run, and the `+seh` logs above show them. That was survivable while only XAML
called in. It is not survivable underneath .NET Native: `RhpPInvokeExceptionGuard` sits on every
P/Invoke transition frame and turns **any** native exception taken below it into a fail-fast, which
is the same mechanism as the `memmove` abort earlier in this file.

It now asks `VirtualQuery` instead - committed, readable, and the whole pointer inside one region -
and reads only then. A run went from 23 access violations to **zero**.

Media Player still ends on a fail-fast, but a different one: `RaiseFailFastException` called
directly from its own managed code (`Microsoft.Media.Player.dll` frames through `SharedLibrary.dll`,
not through the P/Invoke guard), i.e. an unhandled managed exception rather than anything faulting
underneath it. `Windows.Internal.CapabilityAccess.CapabilityAccess` is the next unregistered class
in the log and is the obvious thing to serve next.

Regression checks: Calculator byte-identical, Feedback Hub clean, Solitaire clean.

### Driving the stand-in from the real signatures

Serving one class was not enough: the app walks the model. `User.GetCurrent` leads to
`Application.FindByUserAndPackageFamilyNameAndAppListEntry`, then `get__ApplicationID` straight back
into `GetByUserAnd_ApplicationId`, then `MrtApplication.GetByApplication`, `get_DisplayNameReference`,
`Application.get_ApplicationUserModelId`, `GetPackage`, and on into `Package`. Two rules fell out of
doing it by hand, both of which had to be got right before anything progressed:

- **An empty result is fatal.** The caller is the app looking *itself* up, and a null row or an
  empty `IVectorView` is a state it treats as unrecoverable - the empty collection cost 46 fail-fasts,
  a null `IMrtApplication` another 52. One row is the truthful answer: this package is installed and
  it is the one running. Its `_ApplicationID` has to be non-zero too, because the app reads it and
  immediately looks it up again.
- **Guessing which argument is the out-parameter corrupts things**, exactly as it did in the
  composition default slot. `get_DescriptionReference(HSTRING **)` takes its out-parameter *first*;
  the hand-written rule wrote to the last writable-looking register instead, left the real
  out-parameter holding whatever the caller's stack had, and the app fail-fasted on the garbage
  HSTRING.

So the shim no longer guesses. `tools/wine-uwp/gen-staterepository.py` generates
`staterepository_slots.h` from the server vtables plus the public PDB, demangled with
`llvm-undname`: for every slot of every class it records the arity, what the last parameter is, and
for interface and collection out-parameters which class the row must be. In this family the
out-parameter is always the last one, so arity is enough to write to the right place. 416 slots
across six classes, all demangled.

Two bugs in the generator worth recording, because both fail *silently* and produce a table that is
confidently wrong:

- `llvm-undname` prints three lines per symbol (echo, result, blank) and only two for a symbol it
  cannot demangle. Batching and pairing the lines up shifts every prototype after the first failure
  onto the wrong slot - i.e. the wrong arity, i.e. writing through the wrong argument. It now
  demangles one name per call.
- Walking a vtable until a slot "looks like" the end does not work: identical-code folding makes a
  slot resolve to a same-shaped method of an unrelated class (`ApplicationServer` slot 14 resolves
  to `SecondaryTileViewServer::put_VisibleInAppList`). That truncated three of the four tables, and
  silently dropped the one slot the app actually called. Vtables are bounded by the next `??_7`
  symbol instead. Picking the statics vtable also needs care - several selector vtables mention
  `I<Class>Statics` in their template arguments, and the FtmBase one is `IMarshal`.

With six classes served (`User`, `Application`, `MrtApplication`, `Package`, `PackageFamily`,
`MrtPackage`), Media Player runs the whole state-repository walk with **no fail-fasts and nothing
off-table**, and its window stays up for the length of the run. The error is no longer a missing
registration: it is `0x8001010e`, `RPC_E_WRONG_THREAD`. Still unregistered and worth serving next:
`Windows.Internal.CapabilityAccess.CapabilityAccess` and
`Microsoft.UI.Private.Controls.CBSPackageDetector`.

The window is still blank. Regression checks: Calculator byte-identical, Feedback Hub unchanged
(822 colours, no fail-fast, no app error).

## RPC_E_WRONG_THREAD: the XAML core and the CoreWindow are on different threads

Media Player's remaining error, `0x8001010e`, is not a missing API. Finding it took three steps,
each of which is reusable:

**1. Get the managed stack.** `trace_language_exception` in `dlls/combase/roerror.c` prints the
exception's `IStringable` text, but `debugstr_wn` truncates and the interesting part is the stack
below the message. Printing it in 160-character chunks gives:

```
System.Exception: Exception from HRESULT: 0x8001010E
   at System.Runtime.InteropServices.McgMarshal.ThrowOnExternalCallFailed(Int32, RuntimeTypeHandle)
   at Microsoft.Media.Player.App.AppChrome..ctor(IAppChromeViewModel, INotificationService,
       IConfigurationService, INavigationService, NavigationHost, IOverlayProgressRingService)
```

`ThrowOnExternalCallFailed` means a *native* call returned it and the runtime rethrew, so the
originate stack (which showed .NET Native frames) was the rethrow, not the source.

**2. Find which native call.** Neither of Wine's two `RPC_E_WRONG_THREAD` sites logs its
"called from wrong apartment" ERR, so it is not Wine's. Rather than guess, patch **every** site that
loads the constant in a candidate DLL to load a *unique* value instead - `0x8001e000 + index` - and
read the index straight out of the exception message. One run per DLL identifies the exact function.
`Windows.UI.dll` (96 sites) was a clean negative; `Windows.UI.Xaml.dll` (231 sites) answered
`0x8001e030`, i.e. site 48:
`DirectUI::DependencyObject::SetValueCore(const CDependencyProperty *, IInspectable *, bool, BaseValueSource)`.
Bypassing that one guard moves the failure to `GetValueByKnownIndex`, so it is systematic rather
than one bad call.

**3. Read the guard.** At `0x2382fc`:

```
mov  rax, [rbx+0x90]      ; the object's core
mov  rcx, [rax+0x10]
mov  rax, [rcx]
mov  r15d, [rax+0xbc]     ; the thread the XAML core belongs to
call [__imp_GetCurrentThreadId]
cmp  r15d, eax
jne  -> RPC_E_WRONG_THREAD
```

Patching the site to return each of the two values in turn gives them directly: the core belongs to
thread **0x52c**, the call comes from **0x550**.

And the log says what those threads are. `052c` is the real view thread: it activates
`Windows.UI.Core.CoreDispatcher`, calls `NtUserInitThreadCoreMessagingIocp2` and
`CoSetMessageDispatcher`, activates `Windows.UI.Xaml.FrameworkView`, and ends up pumping
`CoMsgWaitInProcessEvents`. `0550` is a *second* thread that does its own core-messaging init and
then:

```
0550:err:wineuwp:wf_CreateCoreWindow CreateCoreWindow L"" on thread 1360
0550:err:wineuwp:publish_core_window_mailbox published CoreWindow ... to XAML mailbox
0550:fixme:combase:RoGetActivationFactory (L"Windows.UI.Xaml.Window", ...)
```

So **the host creates the CoreWindow, and the app builds its XAML tree, on a different thread from
the one XAML's `DXamlCore` was initialized on**. On Windows a view's `Initialize`, `SetWindow` and
`Run` all happen on one thread; here they are split, and every `DependencyObject` the app touches
from the second thread is correctly rejected as cross-thread.

This is ours - `dlls/wineuwp/main.c` - not a missing Windows API, and the mailbox and
APC-broadcast machinery around `try_configure_from_get_for_current_thread` exists to paper over the
same split. Calculator and Feedback Hub do not hit it because they do their work on the thread that
ends up owning the core. Fixing it means running the FrameworkView's whole lifecycle on the thread
that creates the CoreWindow, which is host surgery rather than another stand-in, so it is written up
rather than attempted at the end of this pass.

Also fixed along the way: `RoGetApartmentIdentifier` answered `0xdeadbeef` for every apartment,
which makes every thread in the process look like the same one. .NET Native's RCW records the
identifier an object belongs to and compares it per call, so a constant is not a harmless stub. It
now returns the thread id for a single-threaded apartment and a shared value for the MTA. It is not
what caused this error - the error predates the change - but it was wrong.

Binaries restored: `Windows.UI.dll` and `Windows.UI.Xaml.dll` both byte-identical to the originals.
Regression checks: Calculator byte-identical, Feedback Hub unchanged (822 colours, no fail-fast).

### Where the second view thread comes from

The thread split behind the `RPC_E_WRONG_THREAD` above is not a race or a marshalling accident. The
log's ordering says what happens, and it is the same with and without `WINE_UWP_ACTIVATE_ON_VIEW`:

| line | thread | what |
| --- | --- | --- |
| 37  | `01b8` | activates `CoreDispatcher`, then `Windows.UI.Xaml.FrameworkView` - the **first** view thread |
| 133 | `01b8` | activates `Windows.UI.Xaml.Controls.UserControl` - the app is already building its tree here |
| 207 | `01b8` | enters `CoMsgWaitInProcessEvents` - `FrameworkView::Run`, its dispatcher loop |
| 209 | `01b4` | the activation thread: `com_get_class_object ... in-process server for {3480a401-bde9-4407-bc02-798a866ac051}` |
| 214 | `01dc` | a **second** view thread appears: `NtUserInitThreadCoreMessagingIocp2`, `CoSetMessageDispatcher` |
| 244 | `01dc` | `wf_CreateCoreWindow` - and `DXamlCore::GetCurrent()` is non-null here too, so there are two cores |
| 528+ | `01dc` | the rest of the tree: `MediaPlayerElement`, `MediaTransportControls`, `ContentControl`, ... |

So the app starts on the first view thread, that thread enters its dispatcher loop, and only *then*
does the activation run - and it brings up a second view, with its own thread, its own `DXamlCore`
and the `CoreWindow`. The app's remaining UI work happens on the second one while the objects it
made at line 133 belong to the first. XAML rejecting that is correct.

This is why Calculator and Feedback Hub are unaffected: they do not create anything before the
activation lands, so everything they own belongs to whichever core ends up with the window.

The fix is for the activation to reuse the existing view rather than cause a second one, which
means changing when the host activates relative to `FrameworkView::Run` - the ordering the comments
around `queue_activate_apcs` and `activate_apc` are already fighting ("Waiting for a dispatcher used
to lose the race with the UI thread crashing before SetWindow"). That is a rework of the host's
activation sequencing, not a stand-in, and it wants its own pass with the existing
`WINE_UWP_ACTIVATE_ON_VIEW` / `WINE_UWP_NO_VIEW_PUMP` experiments redone against it.

The diagnostic used to get here (an `ERR` in `wf_CreateCoreWindow` printing
`DXamlCore::GetCurrent()`) has been removed. Calculator byte-identical afterwards.

### Fixing half of it: the view startup now runs on the view thread

Three defects in `dlls/wineuwp/main.c` kept the startup sequence off the view thread. All three are
fixed; the remaining split is narrower and better understood.

1. **`drive_view_startup` claimed its one-shot before it knew it could do the work.** The
   `InterlockedCompareExchange( &view_startup_done, 1, 0 )` was the second line of the function, so
   the first caller won even when it was on the wrong thread and could do nothing. The guard now
   fires only once a usable view has been found, and the early check is a plain read.

2. **Off the view thread, `CoreApplication` hands back a marshalled proxy**, and
   `find_view_subobject` scanned its memory anyway looking for twinapi vtables that are not in it.
   The log line is `no ICoreApplicationViewInternal vtable ... within 0x100 of view`; dumping the
   object shows slot 0 is a heap address, not a twinapi one. It now recognises a proxy - the test
   is whether the vtable lies inside twinapi's `SizeOfImage`, resolved from the module's own PE
   header - and returns NULL so the caller can retry where the real object lives.

   The first attempt at this compared the vtable against `CoreApplicationView`'s *primary* vtable
   and bailed on any mismatch. **That broke Calculator** (blank window): a different twinapi vtable
   is the ordinary case and must keep scanning. Only a vtable outside the image means a proxy.

3. **Nothing drove the startup from the view thread at all.** The `drive_view_startup( NULL )` call
   that the comment describes as "the view thread with a live DXamlCore and CoreWindow" sits at the
   end of the manual `ConfigureCoreWindow` path, which is off by default (doing it twice fail-fasts
   XAML). `wf_CreateCoreWindow` now queues the startup as an APC on its own thread, behind the
   existing view pump - APCs run in order, so the pump delivers `SetWindowAndGetDispatcher` first
   and `Load`/`Activate` follow.

With `WINE_UWP_DRIVE_VIEW=run` the sequence now completes on the view thread:

```
01dc:err:wineuwp:drive_view_startup Load(L"Microsoft.ZuneMusic") on view ..., hr 0
01dc:err:wineuwp:drive_view_startup view Activate(callback ..., args ...), hr 0
```

and the app builds 32 XAML controls on the view thread where it built 15 before.

**It is not enough on its own.** `0x8001010e` is still the reported error, because the app's `App`
object is constructed even earlier, on the *first* view thread (`01b8`, which activates
`CoreDispatcher` and `FrameworkView` and creates a `UserControl` at log line 133 - before any
CoreWindow exists), while the container puts its `CoreApplicationView` and the CoreWindow on `01dc`.
Driving Load/Activate on `01dc` is right, but `OnLaunched` then runs there against objects the App
constructor already made on `01b8`.

So the remaining question is narrower than before: not "why is the activation on the wrong thread"
but **why the container creates a `CoreApplicationView` on a new thread when XAML's `FrameworkView`
already exists on the app's main thread**. `WINE_UWP_ACTIVATE_ON_VIEW=1` does not change it - even
with `Activate` running on `01b8`, `CreateCoreWindow` still lands on `01dc` - which points at the
view never having been associated with a `CoreApplicationView` in the first place, rather than at
where the activation is delivered.

`WINE_UWP_DRIVE_VIEW` stays opt-in: it is right for this app but wants the other apps re-tested
against it before it becomes the default. Regression checks with the three fixes in: Calculator
byte-identical, Feedback Hub unchanged (822 colours, no fail-fast).

### Two views, and why driving the first one does not work yet

The remaining split is between two `CoreApplicationView`s, and the log names who makes each:

- `0180` (the process main thread) CoCreates `{dea794e0-1c1d-4363-b171-98d0b1703586}` -
  `ActivatableApplicationRegistrar`, which is ours. The **first** view thread (`01b8`) comes up
  right after: `CoreDispatcher`, then `Windows.UI.Xaml.FrameworkView`. The app's `App` object is
  constructed here - it creates a `UserControl` at log line 133, well before any CoreWindow.
- `01b4` (our activation thread) CoCreates `{3480a401-...}`, our `CoreWindowBroker`, and the
  **second** view thread (`01dc`) appears immediately after and takes `CreateCoreWindow`.
  (`{4b7a83d5-cf71-40ad-ab0f-bf6830e95330}`, `UiaManager`, fails to create on that thread as well;
  it is unregistered and appears harmless.)

So the container stands up a second view for the activation rather than using the one the app is
already running on. Two experiments to close that, both **backed out**:

1. **Broadcast the view startup to every thread** rather than only queueing it from
   `wf_CreateCoreWindow`. This is safe in principle - off a view thread `GetCurrentView` returns a
   proxy, `find_view_subobject` now says so, and the one-shot stays unclaimed - and it did reach an
   *earlier* view thread than before. But with no CoreWindow on that view, `Load`/`Activate`
   fail-fast inside XAML: 39 of them.
2. **Call `SetWindowAndGetDispatcher` from the view thread first**, with our own window factory, so
   that view makes its own CoreWindow (the factory has no thread affinity by design, precisely so
   the window lands on the caller's thread). That removed the fail-fasts and the app error, but the
   app then hangs: the startup is claimed, nothing after it logs, and only four XAML activations
   ever happen. It deadlocks somewhere inside that call - which is the same class of hang the
   comments around `pump_view_thread_later` and `broadcast_configure_core_window` describe.

Both are reverted, leaving the state that measurably works: the startup is driven from
`wf_CreateCoreWindow`'s thread, `Load` and `Activate` both return `hr 0` there, and the app builds
about twice as much of its tree as before. The error is still `0x8001010e` because the `App` object
remains on the first view thread.

What this rules out: it is not about *where the activation is delivered*. Delivering it on the first
view thread (`WINE_UWP_ACTIVATE_ON_VIEW`) still puts `CreateCoreWindow` on the second, and driving
the startup on the first view thread directly deadlocks rather than producing a window. The next
thing to establish is why the container creates a second view at all - i.e. what the first view is
missing that makes it ineligible - rather than trying to redirect work onto it after the fact.

Regression checks: Calculator byte-identical, Feedback Hub unchanged (822 colours, no fail-fast).

### Correction: two views is normal here, and that is not what is wrong

`WINE_UWP_REPORT_VIEWS=1` queues a read-only APC onto every thread; each reports whether
`CoreApplication` hands it a real `CoreApplicationView` (vtable inside twinapi) or a marshalled
proxy, plus its `DXamlCore`. Media Player:

```
tid 444  view 0x...8481E0  REAL   core 0x...890020
tid 480  view 0x...ECBDA0  REAL   core 0x...DD5A30
others   view 0x...83F278  proxy  core 0
```

Two real views, each with its own core - as expected. **But Calculator reports exactly the same
shape**: two real views, on tids 408 and 444, each with a core. Calculator works.

So the entry above, and the commit message with it, put the emphasis in the wrong place: the second
view is not the defect. The defect is that Media Player *uses both* - its `App` object and the
`UserControl` it makes are on the first view, and `OnLaunched` and the rest of the UI are on the
second. Calculator has the same two views but does all its work on the one that ends up owning the
CoreWindow, so nothing of its is ever split.

That changes what the fix has to do. It does not have to stop the second view existing; it has to
make the activation - `Load`/`Activate`, and therefore `OnLaunched` - land on the *same* view the
app's `App` object was constructed on. That is what the two backed-out experiments were reaching
for, and the one that got there (broadcasting the startup to every thread, so the first view can
claim it) is the right shape; what defeated it is that the first view has no CoreWindow, and giving
it one through `SetWindowAndGetDispatcher` deadlocks. Understanding that deadlock is the next step,
not avoiding it.

`report_views` is kept as a deliberate debugging aid rather than removed: it is off unless
`WINE_UWP_REPORT_VIEWS` is set, it only reads, and counting real views per thread is the
measurement this whole area kept needing. Calculator is byte-identical with it in and unset.

### The first view cannot be given a window: SetWindowAndGetDispatcher never returns

With the diagnosis corrected - the job is to land the activation on the view the app's `App` object
is on, not to stop the second view existing - the obvious move is to let that view claim the startup
and give it a CoreWindow of its own. `WINE_UWP_DRIVE_FIRST_VIEW=1` does exactly that: broadcast the
startup APC so any real view can take it, and call `ICoreApplicationViewInternal::SetWindowAndGetDispatcher`
with our own window factory before `Load`. The factory has no thread affinity by design, so the
window would be created on the claiming thread.

It hangs, every time, and the instrumentation says precisely where:

```
01b8:err:wineuwp:drive_view_startup STEP tid 440: creating the window factory
01b8:err:wineuwp:drive_view_startup STEP tid 440: factory 0x... hr 0
01b8:err:wineuwp:drive_view_startup STEP tid 440: SetWindowAndGetDispatcher on view 0x...
        <nothing further from tid 440>
```

Our `wf_CreateCoreWindow` is never entered, so it blocks *before* asking the factory for anything.

The first hypothesis was contention: the startup was being driven from `wf_CreateCoreWindow`, i.e.
asking the container for a window on one view while it was mid-way through creating another. Moving
the broadcast into `activation_thread`, before the activation factory is called at all, makes the
first view claim it with **no `CreateCoreWindow` anywhere in the run** - and it still hangs. So it is
not contention with the second view's creation; it is unconditional.

What that leaves is that the first view is not in a state where it can accept the call - which is
the same thing the container is apparently deciding when it declines to use that view and stands up
its own. Two candidates worth testing next, in order:

- The call is made from inside a `QueueUserAPC` callback, so the view thread is blocked in our frame
  and cannot pump. If `SetWindowAndGetDispatcher` posts to that view's own dispatcher and waits, it
  can never complete. Testing this means driving the call from the thread's normal message loop
  rather than from an APC.
- The view is genuinely not ready: `ICoreApplicationView_Activation::PrepareToActivateAsync` (slot 6
  of the activation vtable the host already knows how to find, `TWINAPI_VIEW_ACTIVATION_VTBL`) is
  "how the shell tells a view that an activation is on its way before sending it". Nothing calls it
  here. That is a concrete, cheap thing to try.

Both experiments stay behind `WINE_UWP_DRIVE_FIRST_VIEW` and are off by default. Defaults verified
after: Calculator byte-identical, and with `WINE_UWP_DRIVE_VIEW=run` Media Player still gets `Load`
and `Activate` on the window's view thread with no fail-fasts.

### Correction: it crashes, it does not deadlock - and where

The previous entry called the `SetWindowAndGetDispatcher` failure a hang. It is not. The thread
stops logging, `ps` finds no process a minute later, and the tail of the log is
`fixme:dbghelp_dwarf:compute_location` - Wine's own debugger running. The process **crashes**:

```
wine: Unhandled page fault on read access to 0000000000000000 at address 0x...9434 (thread 01b8)
```

`0x...9434` is `twinapi.appcore.dll+0x29434`, which its public PDB names
`CoreApplicationView::RegisterForCoreWindowActivatedEvent +0x54`:

```
mov rcx, [rdi+0xe8]     ; the view's CoreWindow slot
mov rax, [rcx]          ; <- reads through NULL
```

So `SetWindowAndGetDispatcher` gets as far as hooking the window's Activated event, and the view's
window field is still NULL - consistent with our `wf_CreateCoreWindow` never being entered on that
path. The call is not waiting for anything; it runs into a field that something earlier in the
view's lifecycle was supposed to fill.

Two candidates from the previous entry are now settled:

- **`PrepareToActivateAsync` is not the missing precondition.** The host already has
  `prepare_view_for_activation()`; calling it on this view first returns **`E_NOTIMPL`** (`0x80004001`)
  and changes nothing.
- **It is not an APC/pumping problem.** That theory was for a blocking wait, and there is no wait.

A note on tooling, because it cost time: `gdb` cannot attach here (`/proc/sys/kernel/yama/ptrace_scope`
is 1, and changing it is a system-wide setting, not something to do unasked), `eu-stack` returns
nothing useful for Wine threads, and `winedbg` crashes on startup in this build. Mapping the fault
address from the log against a `+loaddll` run and resolving it with the public PDB is the way to
locate a crash in a native DLL here. Also worth knowing: `pgrep -f "<exe name>"` matches the
agent's own shell commands containing that string, which is how a stale background poller ended up
being reported as the app's pid.

`WINE_UWP_DRIVE_FIRST_VIEW` stays off by default; Calculator byte-identical with it unset.

## RPC_E_WRONG_THREAD is fixed: the window has to be made through the view, not through SetWindowAndGetDispatcher

The null read in `RegisterForCoreWindowActivatedEvent` is explained by one branch in
`CoreApplicationView::SetWindowAndGetDispatcher` (twinapi.appcore `0x288a0`):

```
call IsApplicationActivationWatcherEnabled   ; 0x29320, a free function - process-wide
test al,al
je   +0x3f5                                  ; false -> call the factory's CreateCoreWindow
...                                          ; true  -> straight on to
call RegisterForCoreWindowActivatedEvent     ; which reads the view's window slot, +0xc0
```

`IsApplicationActivationWatcherEnabled()` calls `RtlGetDeviceFamilyInfoEnum` and returns **true for
`DEVICEFAMILYINFOENUM_DESKTOP` (3)**, which is what Wine's stub reports. So on this device family
`SetWindowAndGetDispatcher` never makes a window - it assumes the activation watcher already
did - and faults on the empty slot. That is exactly what the comment in `dlls/ntdll/rtl.c` warns
about: "the desktop family makes the app model expect shell services (an activation watcher, a view
manager) that a Wine prefix has no counterpart for".

Reporting a different family is not the answer: `WINE_UWP_DEVICE_FAMILY=7` avoids the branch but
takes the app model somewhere far worse - three XAML activations instead of thirty-eight.

The answer is to make the window the way the container does on the working path:
`CoreApplicationView::CreateCoreWindow( ICoreWindowFactory * )`, which is slot 6 of the interface
whose vtable is at twinapi RVA `0x1a4e68` (found by scanning the view's vtables for the method's
address, `0x2c600`). With that called first, from the view's own thread, our `wf_CreateCoreWindow`
runs **on that thread** and the view's window slot is filled. `SetWindowAndGetDispatcher` is then
not needed at all - it only hands back a dispatcher, and it does not return when called from here -
so the startup goes straight to `Load` and `Activate`.

With `WINE_UWP_DRIVE_VIEW=run WINE_UWP_DRIVE_FIRST_VIEW=1`:

```
STEP tid 448: CreateCoreWindow hr 0
drive_view_startup Load(L"Microsoft.ZuneMusic") on view ..., hr 0
drive_view_startup view Activate(callback ..., args ...), hr 0
```

no fail-fasts, 38 XAML activations, the window stays up for the run - and **`0x8001010e` is gone**.
The app is no longer split across two views. The error is now
`Delegate to an instance method cannot have null 'this'.`, a managed failure inside the app, which
is a different problem entirely.

Still blank. Both switches stay opt-in until the other apps are tried against them; defaults
verified after: Calculator byte-identical, Feedback Hub unchanged (822 colours, no fail-fast).

### After the wrong-thread fix: the window context's other getter

With the app on one view, the next error is a managed one, and the chunked exception trace names it:

```
System.Exception: Call failed. (Exception from HRESULT: 0x80004005)
   at McgMarshal.ThrowOnExternalCallFailed(...)
   ...
   at System.Lazy`1.ViaFactory(...)
   at Microsoft.Media.Player.Controls.Uwp.Utilities.ViewportStateTrigger..ctor() + 0x168
```

followed by `System.ArgumentException: Delegate to an instance method cannot have null 'this'.`,
which is the consequence - the `Lazy` never produced its value.

Tracing the `dcomp` channel found one of ours in that path and it is now fixed:
`MrmCoreR`'s `CoreWindowContainer::Initialize` (RVA `0x23aa8`, named from its public PDB) calls
**slot 6** of the window-context partner interface with a single out-parameter, stores what comes
back and releases it later, and treats a failure as fatal. That slot was the `window_host_method`
stub added with the InputPane chain, returning `E_NOTIMPL`. It now hands back another hop carrying
the same window handle, the way the QI and slot 7 already do, and the stub no longer appears in a
run.

The `E_FAIL` itself is still there, and these are ruled out:

- Not `windows.graphics`: the last traced call before the exception is
  `display_info_add_OrientationChanged`, which returns `S_OK`, and nothing else in that DLL is
  called.
- Not `twinapi.appcore`: all 21 sites that load `0x80004005` as a literal were tagged with unique
  values and none of them fired. (That technique only covers literal loads - an `E_FAIL` returned
  from a helper or propagated in a register would not be caught, so this is not conclusive, just
  the cheap half.)
- The originate hook does not help here: `RoOriginateError` is called by .NET Native's marshalling
  layer when it converts the failed call, so the stack is the re-origination, not the producer.
- The app's interop stub takes its vtable slot in `r8d`, and the caller loads it from `eax`
  (computed), so the slot cannot be read statically the way it can for MSVC callers.

The next step is to find the producer at runtime rather than statically - the same unique-HRESULT
tagging, but covering non-literal returns, or a breakpoint-equivalent on the interop stub that
records the interface pointer it is called with.

Regression checks: Calculator byte-identical, Feedback Hub unchanged (822 colours, no fail-fast).

### Window::get_BoundsImpl, and why skipping SetWindowAndGetDispatcher was only half a fix

The `E_FAIL` in `ViewportStateTrigger..ctor()` was found by tagging: all 1662 sites in
`Windows.UI.Xaml.dll` that load `0x80004005` as a literal were given unique values
(`0x8001c000 + index`), and one run reported `0x8001c32a` -
**`DirectUI::Window::get_BoundsImpl( Rect * )`**, i.e. the app reading `Window.Current.Bounds`.
Its failure path is reached from

```
0x5a0820: cmp QWORD PTR [rdi+0x1e8], 0   ; the HWND
0x5a0828: je  -> test al,al -> E_FAIL
```

with the CoreWindow branch above it already skipped because `[rdi+0x148]` is null too. So XAML's
`Window` had neither a CoreWindow nor an HWND - because `CreateCoreWindow` gives the window to the
*view*, and what tells XAML about it is `FrameworkView::SetWindow`, which only runs as part of the
`SetWindowAndGetDispatcher` this path deliberately skips.

The host already has the missing step (`try_configure_core_window` ->
`DXamlCore::ConfigureCoreWindow`). It is off by default for a good reason - running it *as well as*
XAML's own `SetWindow` fail-fasts - but on this path XAML's never runs, so it is the only one. With
`WINE_UWP_MANUAL_CONFIGURE=1` alongside the other two switches:

```
try_configure_core_window ConfigureCoreWindow core ... window ... hr 0
drive_view_startup Load(L"Microsoft.ZuneMusic") hr 0
```

XAML activations go from 38 to **46**, and the error changes again - `get_BoundsImpl` no longer
fails. Media Player is now far enough in to be running its own x:Bind bindings:

```
System.NotImplementedException: Not implemented. (0x80004001)
   at ...AppChrome_obj1_Bindings.BindingsTracking.PropertyChanged_NavigationViewSelectedItem(...)
   at Microsoft.Media.Player.Observables.ComputedObservable`1.set_Value(T)
```

A caution worth writing down: an intermediate run looked like the `E_FAIL` had gone, because
`grep "Call failed"` found nothing - but that text only appears when `WINE_UWP_TRACE_ORIGINATE` is
set, and that run did not set it. Re-tagging showed `get_BoundsImpl` still failing. Absence of a
log line is only evidence when the line would have been printed.

Regression checks: Calculator byte-identical, Feedback Hub unchanged.

### Into the app's own bindings

With the window configured, the next two failures were in the media stack and both were ours:

- `media_player_add_CurrentStateChanged` returned `E_NOTIMPL` while its siblings returned `S_OK`.
  Every `add_*`/`remove_*` in `dlls/windows.media.playback.mediaplayer/main.c` now succeeds with a
  zero token - registering has to work, the events simply never fire here. (Four of them were still
  refusing.)
- That moved the error to `E_NOINTERFACE` (`0x80004002`), raised as an `InvalidCastException`
  inside `AppChrome_obj1_Bindings.BindingsTracking.PropertyChanged_NavigationViewSelectedItem` -
  the app's own generated x:Bind code. XAML QIs our `MediaPlayer` for `{5c526665-...}`
  (`Windows.UI.Xaml.IDependencyObject` - correctly refused, a MediaPlayer is not one) and then for
  `{02dd3ad0-...}`, `{64bd43f8-...}` and `{9d47a108-...}`, none of which are in any public `.winmd`
  or named in `HKCR\Interface`. The first two appear only inside `Windows.UI.Xaml.dll`; the third is
  also in `Windows.Media.Playback.ProxyStub.dll`.

**A tagging caveat learned the hard way:** the unique-HRESULT trick is safe for `E_FAIL` but *not*
for `E_NOTIMPL`. Tagging all 629 `E_NOTIMPL` sites in `Windows.UI.Xaml.dll` took the run from 46
XAML activations down to 6 - callers test for `E_NOTIMPL` specifically and take a different path
when they get something else. Tag `E_FAIL`, and find `E_NOTIMPL` from the FIXME lines our own stubs
already print.

Where this leaves Media Player: window up for the whole run, 46 XAML activations, no fail-fasts, and
every remaining failure inside the app's own binding code rather than in the host. It still paints
nothing, and the honest reading is that it aborts its startup bindings before any content reaches
the tree - so "painting" is gated on those internal XAML interfaces, not on the presentation path,
which demonstrably works for Calculator and Feedback Hub.

All byte-patched binaries verified restored: `Windows.UI.Xaml.dll`, `Windows.UI.dll`,
`twinapi.appcore.dll`, `Microsoft.UI.Xaml.dll`. Regression checks: Calculator byte-identical,
Feedback Hub unchanged.

### The three private interfaces, and why two of them must stay refused

Attempting to satisfy the `InvalidCastException` by answering the three unknown IIDs with a traced
view produced three clear results, two of them negative and worth keeping:

- **`{64bd43f8-bfee-4ec4-b7eb-2935158dae21}` is XAML's `IReferenceTrackerInternal.`** Its users in
  `Windows.UI.Xaml.dll` are `TrackerTargetReference::Set`,
  `ComposingTrackerTargetWrapper::Ensure`, `ComposingTrackerExtensionWrapper::IsComposed` and
  `interface_forwarder<IReferenceTrackerInternal>::ClearReferenceTrackerPeg`. Claiming it made XAML
  start calling tracking methods (slots 14 and 19) on an object that cannot honour them: **51
  fail-fasts**. A plain WinRT object is *supposed* to answer `E_NOINTERFACE` here.
  `{02dd3ad0-...}` appears only in `Windows.UI.Xaml.dll` too and is almost certainly the same kind.
- **`{9d47a108-81a6-43f9-b40d-6403552df734}` is the one the app's cast wants.** Answering the QI
  removes the `InvalidCastException`; the app then calls **slot 6**. Returning `E_NOTIMPL` there
  gives a clean `NotImplementedException`, i.e. no better than refusing the QI. Returning `S_OK`
  with a null out-parameter is **worse**: the caller uses the result without a null check and the
  process dies on a read of `0x10`.
- What the interface looks like, from the only caller that can be read:
  `CMediaPlayerSubtitleManager::AttachMediaPlayer` in `Windows.Media.Playback.MediaPlayer.dll` QIs
  for it and calls **slot 14** with a single out-parameter, then checks the result for null and
  carries on. So its methods are object getters - but slot 6, the one Media Player calls, needs a
  real object, and there is nothing to build one from: the IID is in no `.winmd`, unnamed in
  `HKCR\Interface`, and the only implementations are inside shipped binaries.

All of it is reverted; the tree is back to the state that does not crash (46 XAML activations, no
fail-fasts, `InvalidCastException`). The lesson is the same one this file keeps recording from the
other direction: claiming an interface you cannot implement is not a free way past a cast. Both
`E_NOINTERFACE` and `E_NOTIMPL` are honest answers that the caller handles; a plausible-looking
null is not.

Regression checks: Calculator byte-identical, Feedback Hub unchanged (822 colours).

### Why Media Player's window is blank, settled

Two more things were tried and both are negative results worth keeping.

**Serving the class from the native implementation does not work.** `Windows.Media.Playback.MediaPlayer.dll`
from the Windows install exports `DllGetActivationFactory` and contains the class name, and it does
implement `{9d47a108}` properly - but Wine's builtin wins by module name, so it takes both a
`DllPath` change *and* a `windows.media.playback.mediaplayer=n` override plus the file in the
prefix. With the native one actually loaded the run is far worse: **64 fail-fasts and 12 XAML
activations** against 0 and 46. It wants media infrastructure this prefix does not have. Reverted.

**The blank window is not a sizing or presentation bug.** The `+dcomp` paint trace for Media Player
ends with

```
paint_public_visual visit public container ... size 0x0 ... children 1
present_desktop_hwnd frame: 0 ops, 1 damage rects
```

which looked like the root visual never getting a size. It is not: Calculator, on the same code,
reports real sizes and **68 ops** per frame. Media Player's tree is empty because the app aborts its
startup before anything is set as `Window.Content` - the 0x0 is the consequence, not the cause.

So the chain is now fully accounted for. Media Player gets a window, builds 46 XAML types on one
view with no fail-fasts, and then stops in its own generated binding code on a cast to a private
MediaPlayer interface that cannot be implemented from anything public. Painting is gated on that,
not on the host, the view plumbing or the renderer - all of which work, as Calculator and Feedback
Hub demonstrate every run.

Prefix left clean: the MediaPlayer class points back at `C:\windows\system32\` and the builtin is
restored. Calculator byte-identical.

## Camera paints

Retesting the other staged apps against this session's fixes:
`The property 'Title' was not found in type 'Microsoft.CameraApp.TitleBar'` - Camera's blocker for
most of this file - **is gone**, and Camera now renders: 223 colours, stable for the whole run, no
fail-fasts, no app error reported to the host.

What it renders is its own crash page: a .NET Native exception screen reading
`System.TypeInitializationException ... ---> System.NotImplementedException: The method or
operation is not implemented`, thrown from a static constructor
(`ClassConstructorRunner.EnsureClassConstructorRun`). That is still a real result - the text is laid
out and drawn by XAML through DWrite, so Camera's rendering path works end to end.

Immediately before the error page, repeatedly:

```
fixme:wineuwp:nav_QueryInterface navigation client has no interface {ba5e7927-7cc2-4ee3-bc78-781c98ca88ce}
```

That IID is in `twinapi.appcore.dll` and `CoreUIComponents.dll`, in no `.winmd` and unregistered,
but twinapi's public PDB names its user:
`CoreApplicationViewTitleBar::_TryGetTitleBarClientAdapter( ITitleBarClientAdapter ** )`. So the
interface is **`ITitleBarClientAdapter`**, our own `wineuwp` navigation client is what is being
asked for it, and Camera's static constructor is title-bar code - which is exactly what its old
`Microsoft.CameraApp.TitleBar` error was about. `CoreApplicationViewTitleBar` has the methods that
would use it: `get_Height`, `put_ExtendViewIntoTitleBar`, `SetTitleBarVisual`, `SetInputSinkWindow`,
`OnLayoutMetricsChanged`. That is the next concrete target for Camera, and unlike Media Player's
blocker this one has a name.

### Reading method counts out of a MIDL proxy/stub

For Media Player's `{9d47a108}` there is one more authoritative source: `Windows.Media.Playback.ProxyStub.dll`.
Walking it gives the exact size of the interface:

- find the IID in `.rdata`, then a pointer to it - that pointer is the `piid` of a
  `CInterfaceStubHeader`, whose `DispatchTableCount` sits at +0x10;
- a pointer to that header is an entry in the `pStubVtblList` array, and a pointer to the array is
  the `ProxyFileInfo`'s second field, giving `TableSize` and `pNamesArray`.

`{9d47a108}` has **19 vtable entries, i.e. 13 methods**, and is interface 0 of 12 in that proxy
file. The names array is NULL in this build, so it stays unnamed. Decoding the `MIDL_SERVER_INFO`
proc strings for the parameter lists did not work - the output repeats implausibly, which says this
is NDR64 rather than the -Oicf layout assumed - and guessing further was not worth it.

Also tried and rejected: serving `Windows.Media.Playback.MediaPlayer` from the native Windows DLL.

Regression checks: Calculator byte-identical, Feedback Hub unchanged (822 colours).

## Media Player's private interface, named at last

The blocker written up above as unreachable was reachable, from the shipping implementation rather
than from metadata. `Windows.Media.Playback.MediaPlayer.dll` has 24 `MediaPlayerImpl` vtables; the
one with the 19 entries the proxy/stub reported is at RVA `0xbe830`, and its public PDB names every
slot:

| slot | method |
| --- | --- |
| 6/7 | add/remove_CastingRenderLocationChanged |
| 8/9 | add/remove_TimelineControllerChanged |
| 10/11 | add/remove_IsLoopingEnabledChanged |
| 12 | get_MediaPlayerCastingRenderLocation |
| 13 | get_VideoSwapchainHandle |
| 14 | get_CurrentMediaPlaybackItem |
| 15 | get_AudioSessionId |
| 16/17 | get/put_TelemetryCorrelationId |
| 18 | DisableVisibilityHandling |

**Slot 6 is an event registration, not a getter**, which is exactly why the earlier guess killed the
process: writing NULL through its first argument overwrote the caller's *handler*, not an
out-parameter, and the caller then dereferenced it. Implemented from this table, the
`InvalidCastException` out of `PropertyChanged_NavigationViewSelectedItem` is gone.

`get_CommandManager` was next, and `IMediaPlaybackCommandManager` and
`IMediaPlaybackCommandManagerCommandBehavior` *are* public - their slot lists come straight from
`Windows.Media.winmd` - so both are implemented properly: disabled behaviours, events that register
and never fire, and a real behaviour object from every getter, because the app dereferences what it
gets. Wine's IDL declares `IMediaPlaybackCommandManager` with no members, so these use raw vtables
rather than the header's empty one.

`get_BreakManager` is next and stays refused. A `MediaBreakManager` built the same way from the
public interface list made the run **unstable** - 66 fail-fasts in one run, six XAML activations in
the next, against a steady 46 and none with it refusing - so the app does more with it than
register events. That is written down rather than shipped: `E_NOTIMPL` is the honest answer until
it is understood, and the experiment is recorded in a comment at the call site.

State: 46 XAML activations, no fail-fasts, no page faults, repeatable across runs, stopping on
`NotImplementedException` from `get_BreakManager`.

Regression checks: Calculator byte-identical, Feedback Hub unchanged.

## Media Player renders

`MediaBreakManager::get_PlaybackSession` was the last blocker, and the fix was the opposite of the
previous entry's conclusion. Instrumenting every entry point of the break manager - rather than
guessing again - showed exactly what the app does with it:

```
get_BreakManager -> get_CurrentBreak      (null is fine)
get_BreakManager -> get_CurrentBreak
get_BreakManager -> get_PlaybackSession   <- returned null
                 -> add_event x3
```

It reads the playback session straight back out of the break manager and uses it. Returning null
there is what made the run unstable, not handing back a break manager at all. The player already
has a session (`media_player3_get_PlaybackSession` creates one on demand), so the break manager now
carries it and hands it back.

**With that, Media Player renders its real UI**: the navigation pane (Home, Music library, Video
library, Play queue, Playlists, Settings), the search box, the "Meet the new Media Player" hero
panel with artwork and an Open file(s) button, the seek bar, and the full transport control strip.
2205 colours, stable for the length of the run, **72 XAML activations, no fail-fasts, no app
error**, repeatable across runs.

It needs all three switches: `WINE_UWP_DRIVE_VIEW=run WINE_UWP_DRIVE_FIRST_VIEW=1
WINE_UWP_MANUAL_CONFIGURE=1`. They are opt-in because the other apps have not been tested against
them; that is the obvious next job, and if it holds they should become the default.

The general lesson from the last three fixes, all of which were the same mistake in different
clothes: **when a stand-in has to answer a getter, find out what the caller does with the answer
before choosing one.** Null is not a neutral value. It crashed the process from
`{9d47a108}` slot 6 (which turned out to be an event registration, not a getter), and it
destabilised the run from `get_PlaybackSession`. Both were solved by reading the shipping
implementation's vtable or by instrumenting the call and looking.

Regression checks: Calculator byte-identical, Feedback Hub unchanged (822 colours).

## The msftedit "DllMain deadlock" was a wedged Xvfb (2026-09-20)

Media Player stopped starting: no `ZuneMusic` window, `first-view startup not taken after 600
tries`, and Wine reporting `loader_section ... blocked by <tid> (in DllMain of L"msftedit.dll")`.
I produced three explanations before the right one, and wrote the first two into `AGENTS.md`:

1. *"The other session rebuilt ntdll and kernelbase."* Wrong. `ls -lt --time-style=+%H:%M` prints
   no date; the `03:43` I read was **09-19**. Nothing shared had been rebuilt.
2. *"Every app including Calculator is broken."* Wrong. That came from counting log lines
   (`deadlock=4 xaml=2`), which appear in healthy Calculator runs too. A screenshot showed
   Calculator byte-identical at 249 colours.
3. *"msftedit's DllMain is intermittently slow."* Also wrong.

The actual chain, found by adding load timing to `probes/probe-load.cpp` and reducing the failure
to `wine probe-load.exe msftedit.dll`:

- msftedit's `DllMain` calls `user32.RegisterClassW` (visible as the last relay line with no
  matching `Ret`, under `RelayFromInclude=msftedit`).
- Wine's first `RegisterClass` in a process runs `win32u/driver.c:load_desktop_driver`, whose
  "wait for graphics driver to be ready" is `send_message( hwnd, WM_NULL, 0, 0 )` to the desktop
  window - **with no timeout**. That is the `NtWaitForMultipleObjects type 1, handles {0x74},
  timeout (infinite)` in a `+sync` trace.
- The loader lock is held across all of it, so every other thread stalls and the host's first-view
  poll expires.

With `:98` wedged the load never returns; on a healthy display it takes **153 ms**. The wedge
survives `wineserver -k` - it is the X server, not the prefix - so the display has to be restarted.
Killing a run mid-X-grab is what wedges it.

### WAYLAND_DISPLAY is not enough

Runs kept printing `err:waylanddrv:wayland_process_init ... compositor doesn't support
zwlr_data_control_manager_v1`, which only a real compositor answers: Wine was talking to the
desktop session while `DISPLAY=:98` was set. `unset WAYLAND_DISPLAY` does not fix it, because
libwayland falls back to `$XDG_RUNTIME_DIR/wayland-0`. Wine chooses its driver from
`HKCU\Software\Wine\Drivers\Graphics` and has no environment override, so `run.sh` now writes
`Graphics=x11`. A run then logs zero `waylanddrv` lines.

### Measurement traps hit while chasing this

- Screenshotting on a fixed timer produced several false "blank window"/"never started" results.
  Media Player needs ~45 s to paint untraced and ~105 s with `+loaddll`/`+seh`. Gate on
  `identify -format %k > 100`, not on a `sleep`.
- `addr2mod.py` answers with the nearest *lower* module base, so it happily reports
  `mrt100_app.dll+0x1f8440` for an address past the end of a 0x6CDF0-byte DLL. Check the offset
  against `SizeOfImage` before believing it. Wine builtins are relocated off their preferred
  `ImageBase`, so scanning the build tree for a covering base finds nothing either; read
  `/proc/<pid>/maps` of the live faulting process instead.
- `grep`/`pgrep`/`pkill` with `-f` match your own command line. This cost three separate
  incidents in one session: `pkill -f ZuneMusic` killed the session shell, two
  `pgrep -f runmp-relay.sh` pollers span forever and loaded the machine during timing runs, and a
  crash-catcher watching for `[w]inedbg` fired on itself after one second. Use `pgrep -x` on the
  image name.

## Media Player: the music library needs AppCapability and StorageLibrary (2026-09-20)

The Music and Video library pages painted *"Media Player can't access your music library"*. The app
asks `Windows.Security.Authorization.AppCapabilityAccess.AppCapability`, registered to the native
`CapabilityAccessManagerClient.dll`, which RPCs to a CapabilityAccessManager service the prefix does
not have - so `CheckAccess` is always denied.

`tools/wine-uwp/shims/appcapability` answers it from the package's `AppxManifest.xml`: declared
capability -> `Allowed` (4), otherwise `NotDeclaredByApp` (1). Verified: the manifest's ten
capabilities are parsed and `CheckAccess(musicLibrary)`/`CheckAccess(videosLibrary)` both return 4.
`RequestAccessAsync` is deliberately `E_NOTIMPL` - it only ever prompts on Windows, and an app that
respects `CheckAccess` never reaches it; a wrong answer there would be invisible.

With the capability allowed the app gets further and hits the real gap:

```
err:combase:RoGetActivationFactory Class L"Windows.Storage.StorageLibrary" not found in
    L"C:\windows\system32\windows.storage.dll", hr 0x80040111
err:combase:RoGetActivationFactory Class L"Windows.UI.ViewManagement.Core.CoreFrameworkInputView"
    not found in L"C:\windows\system32\windows.ui.core.textinput.dll", hr 0x80040111
```

`StorageLibrary` is small and `dlls/windows.storage` already has everything needed:

| Interface | Slots |
| --- | --- |
| `IStorageLibraryStatics` `{4208a6db}` | 6 `GetLibraryAsync` |
| `IStorageLibrary` `{1edd7103}` | 6 `RequestAddFolderAsync` 7 `RequestRemoveFolderAsync` 8 `get_Folders` 9 `get_SaveFolder` 10/11 `add`/`remove_DefinitionChanged` |
| `IStorageLibrary2` `{5b0ce348}` | 6 `get_ChangeTracker` |
| `IStorageLibrary3` `{8a281291}` | 6 `AreFolderSuggestionsAvailableAsync` |

`async_operation_*_create` and `path_param` in `storagefile.c` are the pattern for the async
operation; `known_folders_get_MusicLibrary` already resolves to the real profile directory, so
`get_SaveFolder` is a one-liner. The one genuinely new piece is `get_Folders`, which returns
`IObservableVector<StorageFolder>` rather than the `IVectorView` the existing helpers build.

Also seen on that path, not yet chased: `StdMarshalImpl_MarshalInterface` failing `0x80004002` for
`{5c21c742-2ced-4fd9-ba38-7118d40e966b}` and `{18026348-8619-4c7b-b534-ced45d9de219}`, and a
reproducible read fault at a fixed Wine-builtin IP `0x6FFFFECF8440` (target address always just
past a page start) on the UI thread. The fault does not happen in every run.

### StorageLibrary shim: the library page reaches the empty state (2026-09-20)

`shims/storagelibrary` implements the class. Verified by trace in two runs: the app calls
`GetLibraryAsync(0)` on the Music page and `GetLibraryAsync(2)` on the Video page - which
corroborates `KnownLibraryId` Music=0, Videos=2 - then `GetResults`, `get_Folders` (one folder) and
`get_SaveFolder` twice. `RoGetActivationFactory`'s class-not-found error is gone.

The page consequently changes from the permission wall to the **normal empty-library page**:
"We couldn't find any music / Your music library doesn't contain any music content" with an
"Add folder" button (380 colours, vs 394 for the old denial page and 2205 for Home). So the
capability and library layers are both satisfied and the app is past them.

It still finds no files, and the reason is *not* the folder path: `KnownFolders.MusicLibrary`
resolves through `SHGetKnownFolderPath(FOLDERID_Music)` to `C:\users\bobby\Music`, which in this
prefix is a symlink to `/home/bobby/Music`, and the two test files are there.

What the log shows instead is that **the app never enumerates the folder**. Wine's
`storage_folder_QueryInterface` is asked for exactly one interface, thirteen times:
`{11d3b13a-180e-4789-a8be-7712882893e6}`, which is `IReferenceTracker` - .NET Native's RCW layer
asks every WinRT object and must be refused (claiming it caused 51 fail-fasts, see above). It is
never asked for `IStorageFolderQueryOperations` `{cb43ccc9-446b-4a4f-be97-757771be5203}`, which
Wine *does* implement, and no `Windows.Storage.Search` or indexer class is ever activated.

So Media Player is not listing the folder through StorageFolder at all. It ships `e_sqlite3.dll`
and builds its library in its own database from a background scan; the next thing to find is what
starts that scan. The most likely candidate is `IStorageLibrary2::get_ChangeTracker`
`{5b0ce348}` - a `StorageLibraryChangeTracker`, which the shim currently answers `E_NOTIMPL` - since
the scan is driven by change-tracking on Windows. Implementing the tracker and its
`StorageLibraryChangeReader` is the next step.

Unrelated observation from the same runs: the page fault at `0x6FFFFECF8440` is **intermittent**,
not deterministic - two runs completed without it. An earlier note here implying the Music library
click triggers it was wrong: with the x11 driver pinned it has also been seen during startup,
before any click.

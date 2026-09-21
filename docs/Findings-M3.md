# Findings M3 — from CoreDispatcher to the XAML parser

Continues Findings-M2.md. Same pinned Windows build (10.0.26200.9168), same prefix `~/.wine-uwp`,
same launcher `tools/wine-uwp/run.sh`.

## Where the app gets to now

.NET Native init -> real `Windows.UI.Xaml` Application -> `CoreApplication` -> our in-process activation
registrar -> `CoreDispatcher` over the genuine `CoreMessagingRegistrar` service -> `FrameworkView` ->
**`CoreWindow` created** -> MRT resource manager initialised against the real package graph ->
XAML type system fully initialised (system, WinUI and app-defined types all resolve) -> binary XAML
(XBF) parser running over the app's own `App.xbf`.

The theme-dictionary blocker described below is now resolved; see Findings-M4.md for the fix and current stopping point.

## What was actually wrong (each of these was a real defect, not a stub)

**1. `GetCurrentPackageInfo2` reported a zero count on size queries.**
`MrmCoreR!DynamicPackageProfile::InitPackagesInfo` sizes its package array from `*count` on the
`ERROR_INSUFFICIENT_BUFFER` pass and returns `E_OUTOFMEMORY` when the count is zero. That surfaced
several layers up as a .NET Native `OutOfMemoryException` ("Arg_OutOfMemoryException") during XAML
parse, which looked nothing like a package API bug. The count has to be filled on the size pass.

**2. `PackageIdFromFullName` never set `PACKAGE_ID.publisher`.**
A package full name does not encode the publisher, so the field stayed NULL.
`Windows.ApplicationModel!PackageServer::Initialize` rejects that with `E_POINTER`, so
`Package.Current` failed for every packaged app. `kernelbase` now reads the `Publisher` attribute out
of the package's `AppxManifest.xml` (`get_package_publisher`) and falls back to `CN=Wine`.
`Package.Current`, `.Id.Name` and `.Id.FamilyName` work after this.

**3. The five package *context* APIs had the wrong shape.**
`GetCurrentPackage{,Application,Resources,ApplicationResources,Globalization}Context` all take the
output pointer as the **third** argument (`reserved, reserved2, context`), verified against
`KernelBase.dll` (`test r8, r8` at every entry point). Declared with two parameters they received a
NULL output and returned `E_INVALIDARG`. `Package.Current` fails outright if the resources context
cannot be created, so this blocked the whole app model surface.

**4. `PackageInfoType3` 0x10 is a package-graph generation counter,** not a package list: buffer is a
single `UINT32`. MrmCoreR compares it against a cached value to decide whether to re-read the graph.

**5. Modern MIDL proxy tables (`TableVersion >= 0x10`) are a different, compact format.**
Entries are `{ MIDL_STUBLESS_PROXY_INFO*, const IID*, vtbl-or-marker }` for proxies and
`{ CInterfaceStubHeader, marker }` for stubs; per-method tables are absent, every method is stubless,
inherited methods carry format-string offset `0xffff`, and `pDispatchTable == (void*)-2` means the stub
must forward inherited methods to the base interface. `rpcrt4` now synthesises the vtables and dispatch
tables for these (`get_proxy_vtbl`/`get_stub_vtbl` in `dlls/rpcrt4/cpsf.c`). Without it, marshalling
`Windows.ApplicationModel.Core.CoreApplication` through `OneCoreUAPCommonProxyStub.dll` jumped through a
null slot.

**6. WinRT error origination was a stub, which hid every failure.**
`RoOriginateError`/`RoOriginateLanguageException`/`GetRestrictedErrorInfo` now share one object exposing
`IRestrictedErrorInfo`, `IErrorInfo` and `ILanguageExceptionErrorInfo2`, stored in the thread's error slot
exactly like Windows (`dlls/combase/roerror.c`). `RoFailFastWithErrorContextInternal2` also dumps the
stowed exception chain, which is how most of the above was diagnosed at all.

**7. `.winmd` files declare a `.text` VirtualSize past `SizeOfImage`.** Wine's image mapper rejected them;
Windows maps what fits. Metadata resolution (`ApiInformation`, XAML type lookup) failed until the mapper
truncated instead of refusing.

**8. AVL generic tables were stubs.** `Windows.ApplicationModel` uses `RtlInsertElementGenericTableAvl`
and friends. Implemented properly in `dlls/ntdll/rtl.c` with `probes/probe-avl.cpp` as the test
(20k randomised insert/delete rounds checking order, balance factors and parent links).

## New Wine module

`dlls/windows.staterepositorycore` implements the
`ext-ms-onecore-appmodel-staterepository-cache-l1-1-0` API set. There is no state repository service
here, so `SRCacheManager_Open` reports "unavailable" and callers fall back to reading manifests
directly, which is what `MrmCoreR!StateRepositoryHelper::ArePackageMrtResourcesRedirected` already does
on failure. Also mapped `ext-ms-win-resources-deployment-l1-1-0` to the genuine `MrmDeploy.dll`.

## Package graph, done properly

`run.sh` now stages framework packages as real package directories under `~/uwp-bin/stage/frameworks`,
publishes them through `WINE_UWP_PACKAGE_DEPENDENCIES`, and puts every framework package root on the
process DLL search path via `WINEPATH` — which is what Windows does. `stage.sh` no longer copies
`Microsoft.UI.Xaml.dll` into the app directory: WinUI must load from its framework package or it
mis-detects its own deployment mode.

`combase` resolves activatable classes from the manifests of every package in the graph before falling
back to the registry (`uwp_scan_manifest` in `dlls/combase/roapi.c`), which is how app-local classes
(`CalcManager.Interop.*`, `GraphControl.Grapher`) and WinUI's classes resolve.

## Former blocker (resolved in Findings-M4.md): framework theme lookup

`XamlControlsResources` loads WinUI's theme dictionary. The XBF is **embedded in the framework
package's `resources.pri`** (that package has ten files on disk and no `Themes` directory), so it can
only be served as an MRT embedded-data candidate.

Four things were verified separately, and each one is correct:

1. **WinUI builds the right URI.** It has three forms compiled in and picks the framework-qualified one:
   `ms-appx://Microsoft.UI.Xaml.2.8/Microsoft.UI.Xaml/Themes/21h1_themeresources.xbf`.
2. **Wine parses it correctly.** `probe-mrt` reports
   `scheme=ms-appx host=[Microsoft.UI.Xaml.2.8] path=/Microsoft.UI.Xaml/Themes/21h1_themeresources.xbf`.
3. **MRT can resolve it.** `AllResourceMaps` contains `Microsoft.UI.Xaml.2.8` (190 entries) and the
   lookup returns an embedded-data candidate, the same shape that already works for the app's own
   `App.xbf`.
4. **The API sets it might need resolve.** `ext-ms-win-resources-deployment-l1-1-0` (to `MrmDeploy.dll`),
   `ext-ms-onecore-appmodel-staterepository-cache-l1-1-0` and `ext-ms-win-appmodel-state-ext-l1-2-0` all
   load and export their functions.

Despite that, XAML resolves the URI against the **current** package root and probes
`<app package>\Microsoft.UI.Xaml\Themes\21h1_themeresources.xbf` (then `.xaml`), finds nothing and
returns `E_FAIL` from `CCoreServices::LoadXamlResource`. So the authority is dropped inside XAML's own
`ms-appx` mapping (`CCoreServices::TryLoadXamlResourceHelper` ->
`XamlNodeStreamCacheManager::GetBinaryResourceForXamlUri` -> the `IPALResourceManager` implementation),
not in the URI, not in MRT, and not in the package graph.

Two known-imperfect inputs remain, either of which XAML's resource manager may be consulting to map an
authority to a package:

- `Package.Current.Dependencies` returns 0. `Windows.ApplicationModel` resolves dependencies through the
  WinRT class `Windows.Internal.StateRepository.Package`, which is not in the Windows
  `ActivatableClassId` map at all, so it cannot simply be registered.
- `MrmDeploy.dll` is reachable but `MrmCoreR` never calls it, so no merged/related PRI file is built.

Next step is to disassemble XAML's `IPALResourceManager` implementation and find which call it makes for
a non-empty `ms-appx` authority; that names the missing API precisely instead of guessing. Fallback
routes, in order of cost: make `Package.Dependencies` work without the state repository, or deploy WinUI
app-local by extracting the theme XBFs from the framework PRI through MrmCoreR's private interfaces.

## Tools added
- `probes/probe-mrt.cpp` — dumps MRT resource maps, resolves named resources, and prints
  `Package.Current` and its dependencies. The fastest way to separate "MRM is wrong" from
  "the caller asked the wrong question".
- `probes/probe-avl.cpp` — randomised AVL generic table test.
- `pdbsyms.py` now strips Windows path separators out of the PDB name, so PDBs for modules built with
  full source paths resolve.
- `install-overrides.sh` removes case-duplicate builtins left by `wineboot`, installs resource-only
  modules and the genuine `.winmd` set, and syncs the native override list into the registry so
  svchost-hosted services (the CoreMessaging registrar) load the same DLLs.

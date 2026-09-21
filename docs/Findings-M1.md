# M1 findings: real Windows binaries in the prefix (2026-09-09)

## Source
Windows 11 IoT Enterprise 25H2, build 26200.9168, read from `/dev/nvme0n1p2` with 7z inside a container
(no kernel NTFS driver on the running kernel). Extracted into `~/uwp-bin/win11/`:
- `Windows/System32/`: 455 DLLs = every DLL referenced by `WindowsRuntime\ActivatableClassId` plus the
  DirectX/composition set. Includes the real `mrt100.dll`, which replaces the stand-in (stand-in kept as the
  record of the delay-load gap).
- `Windows/System32/config/SOFTWARE`: registry hive. Parsed with `regipy`; 4754 activatable classes.
- `Program Files/WindowsApps/`: NET.Native Runtime 2.2.28604, Framework 2.2.29512, VCLibs 14.0.33519 (real
  `*_app.dll`), UWPDesktop 14.0.33728, UI.Xaml 2.8 8.2511.26001, and the shipping Calculator 11.2607 package
  (second test subject).

## Registry
`tools/wine-uwp/gen-classes-reg.py` writes `HKLM\Software\Microsoft\WindowsRuntime\ActivatableClassId\*` for
every in-process class whose DLL we have, with absolute `Z:` paths. 4040 registered, 334 server-activated
skipped (no broker), 380 pointing at DLLs outside System32 (SystemApps) skipped.

## Override rule learned
Wine loaded `Windows.UI.Xaml.dll` as its own *builtin* stub even though the registry gave an absolute path
to the real file. DLLs that Wine also ships need `WINEDLLOVERRIDES=name=n`. 55 of the extracted DLLs collide
with Wine builtins. Must stay builtin: `combase`, `d3d11`, `dxgi`, `d2d1`, `dwrite`, `shell32`, `msxml6`,
`iertutil`, `mf`. Force native so far: `windows.ui.xaml`. To decide as they come up: `windows.ui`,
`twinapi.appcore`, `coremessaging`, `windows.applicationmodel`, `windows.globalization`, `windows.storage`,
`wintypes`, `dcomp`, `uiautomationcore`, `dataexchange`.

## Launch progression with real DLLs
1. `Windows.System.MemoryManager` -> `rmclient.dll` native: OK.
2. `Windows.UI.Xaml.Application` -> real `Windows.UI.Xaml.dll` (18 MB) loads natively, factory created.
3. `Windows.ApplicationModel.Resources.Core.ResourceContext` -> `MrmCoreR.dll`: factory created.
4. Dies on unimplemented Wine exports, in this order depending on override:
   - `ntdll.RtlQueryPackageClaims` (called by the XAML engine during Application init)
   - `kernelbase.CheckTokenMembershipEx` (called by MrmCoreR via `api-ms-win-security-base-l1-2-0`)
   - `kernelbase.GetCurrentPackageFullName` is a stub (`fixme:ver`), `AppPolicyGetWindowingModel` fixme.

These are all package identity / AppContainer token queries: M1 proper. They need a Wine source build,
which starts now (`~/src/wine`, tag wine-11.17, PE side built with clang/lld since mingw is not installed).

## Wine gaps found and fixed so far (patches in `tools/wine-uwp/wine-patches/`)
Each was found by launching, reading the trace, mapping the faulting address with `addr2mod.py`, and where
needed disassembling the Microsoft binary around the call site.

| # | Gap | What the app did | Fix |
|---|---|---|---|
| 1 | delay-load failure raises `EXCEPTION_WINE_STUB` | `mrt100_app.dll` filters on `ERROR_DELAY_LOAD_FAILED` | real `mrt100.dll` from Windows, stand-in retired; Wine patch still worth doing |
| 2 | `ntdll.RtlQueryPackageClaims` missing | XAML `Application` init fail-fasts unless it succeeds with the PACKAGED flag | implemented from `WINE_UWP_*` env |
| 3 | `kernelbase.CheckTokenMembershipEx` missing | `MrmCoreR.dll` resource loading | implemented via `CheckTokenMembership` |
| 4 | `GetCurrentPackage{FullName,FamilyName,Path,Id}`, `GetCurrentApplicationUserModelId`, `GetPackagePathByFullName` stubs | XAML and app model everywhere | implemented from env |
| 5 | `AppPolicyGetWindowingModel` always ClassicDesktop | XAML picks the UWP (CoreWindow) path only for Universal | Universal when packaged |
| 6 | `NtQueryInformationToken(TokenSecurityAttributes)` unhandled | `twinapi.appcore` (CoreApplication) reads `WIN://SYSAPPID` from the token | implemented, kernel `TOKEN_SECURITY_ATTRIBUTE_V1` layout, values `[full name, PRAID, family name]` |
| 7 | `GetCurrentPackageInfo2` missing | XAML `DllGetActivationFactory` | implemented with head package plus `WINE_UWP_PACKAGE_DEPENDENCIES` |
| 8 | `combase.CoGetSystemSecurityPermissions` missing | .NET Native error path | stub |
| 9 | native override DLLs not found by bare name | delay-loads of `iertutil.dll` etc. | `install-overrides.sh` copies them into the prefix's system32 |

Lesson on #6: the first attempt used a plausible value order and produced a heap over-read deep inside
`twinapi.appcore`; the actual order was recovered from the disassembly (it splits value[0] at `_`, takes the
last 13 chars as publisher id, appends `!` + value[1]).

## Private ordinal imports (the big hidden dependency)
Windows' own UI DLLs import many combase functions **by ordinal only**; Wine numbers its exports
sequentially, so `twinapi.appcore` calling combase ordinal 122 landed on an unrelated Wine function.
`tools/wine-uwp/ordinal-audit.py` lists every by-ordinal import from the extracted DLLs into Wine builtins,
and `tools/wine-uwp/pdbsyms.py` names them by downloading the matching PDB from Microsoft's public symbol
server (`msdl.microsoft.com`). Names recovered and now pinned in Wine specs:

| DLL | Ordinals | Names |
|---|---|---|
| combase | 32-35, 69, 86-88, 90, 95, 110, 111, 120, 122, 133, 134, 140, 147, 148, 157, 167, 168 | NdrProxyForwardingFunction3-6, CoRegisterServerShutdownDelay, CoBeginProcessEvents, CoMsgWaitInProcessEvents, CoEndProcessEvents, RoGetDesignMode, CoSignalPendingGitRegistrationWaits, CoSetMessageDispatcher, CoHandlePriorityEventsFromMessagePump, CoRegisterForApartmentShutdown, CoGetApartmentIdentifier, RoIsASTAWorkScheduledAtPriorityOrHigher, RoInitializeASTA, CoAllowSetForegroundWindow, RoActivateInstanceAsUser, RoGetActivationFactoryAsUser, RoGetDesignModeV2, CoGetCallContextOfObject, CoImpersonateClientOfObject |
| iertutil | 811 | SetWebPlatformSecurityManagerFactoryCallback |
| shcore | 265 | GetScaleFactorForCoreWindow (240/245/246/249/270 were already pinned) |
| urlmon | 505, 507, 517 | CreateAppxProtocolClassFactory, CreateAppxUrlHelper, CreateAppxSecurityManager |
| ninput | 2508 | BufferPointerPacketsInteractionContext2 |
| propsys | 435 | PropVariantToWinRTPropertyValue |
| wldp | 8, 20 | WldpQueryWindowsLockdownMode, WldpQueryPolicySettingEnabled2 |
| dcomp | 1019, 1038-1045, 2006, 2502 | unknown, dcomp.pdb is not on the public server |

Implementations live in `dlls/combase/asta.c` (new file). Prototypes came from the SDK where public
(`MessageDispatcherApi.h`) and from call sites otherwise; the message-pump trio is a best guess until CoreMessaging
exercises it.

## The activation handshake (first piece of the "shell" we had to build)
`CoreApplication::RegisterActivatableApplication` (twinapi.appcore) does
`CoCreateInstance(CLSID_ActivatableApplicationRegistrar {dea794e0-...}, CLSCTX_LOCAL_SERVER, IID {036c57fc-...})`
and calls slot 3 `RegisterActivatableApplication(IUnknown *app)`. Windows' registrar lives in the shell; it later
calls back `IActivatableApplication::Activate(aumid, contract, activationId, IActivatedEventArgs*, u64, u64)` on
the app (IID {af86e2e0-...}, slot 6, vtable recovered from the PDB), which is what makes `OnLaunched` happen.

wine-uwp provides this in-process: new Wine module `dlls/wineuwp` (registered via `tools/wine-uwp/wineuwp.reg`)
whose registrar posts a window message to itself and delivers `Activate` with a `LaunchActivatedEventArgs`
implementation (IActivatedEventArgs, ILaunchActivatedEventArgs(2), IApplicationViewActivatedEventArgs,
IPrelaunchActivatedEventArgs, IViewSwitcherProvider, IActivatedEventArgsWithUser; IIDs read out of
`WinMetadata\*.winmd` with `dnfile`). combase was taught to let a `CLSCTX_LOCAL_SERVER` request fall back to an
in-process server when `WINE_UWP_PACKAGE_FULL_NAME` is set, since none of the shell brokers exist.

## Where M1 ends: CoreMessaging needs ALPC
With identity, ordinals and the registrar in place the app reaches `Windows.UI.Core.CoreDispatcher` creation
(real `Windows.UI.dll`), which pulls in `CoreMessaging.dll`. Its first act is
`NtAlpcConnectPortEx` to `\BaseNamedObjects\CoreMessagingRegistrar`: every CoreMessaging endpoint registers with
the CoreMessagingRegistrar service, which is `CoreMessaging.dll` itself hosted by `svchost -k LocalServiceNoNetwork`
(`ServiceMain` export, `Parameters\ServiceDll`). Even its in-process test mode
(`CoreUIInitializeTestService`, `CoreUIRouteToTestRegistrar`) runs an `AlpcServerHost`, so ALPC is unavoidable.

Wine 11.17 has ALPC scaffolding only: `NtAlpcCreatePort` creates a named object; connect, accept,
send/receive and disconnect are `STATUS_NOT_IMPLEMENTED` stubs (`dlls/ntdll/unix/alpc.c`, `server/alpc.c`).
`NtAlpcConnectPortEx` and `NtAlpcQueryInformation` did not exist at all (added).

So M2 starts with a wineserver ALPC implementation: connection ports, communication ports, accept, synchronous
send/wait/receive with message attributes (handles, context, view/section attributes as CoreMessaging uses them),
disconnect. Then either run the genuine registrar as a Wine service or call `CoreUIInitializeTestService` in-process.

Evidence of the boundary (`tools/wine-uwp/run.sh`, patched Wine):
```
trace:wineuwp:registrar_RegisterActivatableApplication ...
trace:combase:RoGetActivationFactory Found library L"Windows.UI.dll" for class L"Windows.UI.Core.CoreDispatcher"
fixme:alpc:NtAlpcConnectPort ..., L"\\BaseNamedObjects\\CoreMessagingRegistrar", ... stub!
err:seh:RaiseFailFastException fail-fast: code 0xe0464645 (WIL) param[0] = 0x80070424 (ERROR_SERVICE_DOES_NOT_EXIST)
```

# M2 findings: ALPC, CoreMessaging, and the UI thread's message pump (2026-09-09/10)

## ALPC in wineserver (new)
`server/alpc.c` + `dlls/ntdll/unix/alpc.c`, validated by `tools/wine-uwp/probes/probe-alpc` (connect, accept
with port context, sync request/reply, datagram with transferred handle, disconnect notification),
`probe-wcp` (wait completion packet on a port) and `probe-wait` (single and multi-object waits).

Facts about ALPC learned from CoreMessaging.dll that differ from what Wine's header said:
- `ALPC_MSGFLG_REPLY_MESSAGE` is 0x1, `ALPC_MSGFLG_RELEASE_MESSAGE` is 0x10000, `SYNC_REQUEST` 0x20000.
  Wine's winternl.h had REPLY as 0x10000. A reply is recognised by the header's MessageId matching a request
  still outstanding; the flag is not what makes it a reply.
- Servers receive on the connection port: messages sent to any accepted client's server-side port land in
  the connection port's queue, and the server-side port handle must wait on the same sync.
- The registrar sends a RELEASE with a zeroed header that refers to no held message; Windows accepts it.
- Waitable objects in Wine 11 need an in-process-capable sync (`create_internal_sync`) or multi-object
  waits (thread pool waits) silently never fire.
- CoreMessaging's registrar refuses a connection unless `NtAlpcImpersonateClientOfPort` leaves a real
  impersonation token on the thread; impersonating our own process token satisfies it.

## Running the genuine CoreMessagingRegistrar
Registered as a Wine service (`tools/wine-uwp/coremessaging-service.reg` values written with `reg add`,
group `LocalServiceNoNetwork` for Wine's svchost). Prefix-wide `HKCU\Software\Wine\DllOverrides` make the five
native DLLs win in every process, which svchost needs. Start it with `wine net start CoreMessagingRegistrar`
before the app. It needed: wait completion packets (`NtCreate/Associate/CancelWaitCompletionPacket`, built on
`RtlRegisterWait`), `WaitServiceState`, `QueryTransientObjectSecurityDescriptor`/`FreeTransientObjectSecurityDescriptor`.

## The UI thread pump
After the registrar handshake, `Windows.UI.dll` creates the CoreDispatcher and calls user32 ordinals 2541/2669/2670
(`NtUserGetQueueStatusReadonly`, `NtUserInitThreadCoreMessagingIocp2`, `NtUserDrainThreadCoreMessagingCompletions2`)
plus `GetQueueStatusForCoreMessaging` and `ScheduleCoreMessagingDispatch` from an apiset. Names via user32.pdb.
`InitThreadCoreMessagingIocp2(hwnd, params)` creates and returns an I/O completion port tied to the thread's
message queue; wineserver now posts a completion (ApcContext bit 31 set, which is how CoreMessaging tells it from
its wait packets) whenever the queue becomes signaled, and Drain2 re-arms it.
Then the private combase pump family gets exercised: `CoBeginProcessEvents(&ctx)`,
`CoMsgWaitInProcessEvents(ctx, count, handles, timeout, qs_mask)` (returns the wait result),
`CoEndProcessEvents(ctx)`, `CoHandlePriorityEventsFromMessagePump()`; first guess at the wait prototype was wrong
and faulted writing to the mask argument.

## Current boundary
The pump runs a full cycle. Remaining blockers seen in one run:
- `CoMarshalInterface(ICoreApplicationView)` fails with E_NOINTERFACE: Wine has no proxy for WinRT interfaces.
  Windows uses the interface registrations in HKCR\Interface plus `OneCoreUAPCommonProxyStub.dll` /
  `execmodelproxy.dll`. `tools/wine-uwp/gen-interfaces-reg.py` imports them from the hive.
- `Windows.Foundation.Collections.Detail.Vector` is a private class in `WinTypes.dll`; the native WinTypes
  works as an override once wintypes is installed into the prefix.
- CLSID {96c7a5ef-...} is a threadpoolwinrt.dll COM class Wine's builtin does not provide; pointed at the native DLL.
- `GetCurrentPackageInfo3`, `AppXGetOSMaxVersionTested` (MrmCoreR), `CoGetStdMarshalEx`, `RpcBindingCreateW`,
  `RpcBindingBind`, msctf `HasDeferredInputForCoreDispatcher` added.

## Milestone: registrar handshake, queue completions, activation delivered, XAML view created
- Genuine CoreMessagingRegistrar accepted the app once `NtAlpcImpersonateClientOfPort` produced a real token.
- Wine ALPC facts corrected in the code and header (flag values, connection-port queue sharing, zero-id release).
- `RegisterActivatableApplication` must be answered by calling `IActivatableApplication::Activate` from a
  *different thread*: the main thread blocks on an event inside `CoreApplication::Run` and pumps nothing.
  `dlls/wineuwp` now spawns an MTA thread for it. `Activate` returned S_OK; the UI thread then created the
  `CoreDispatcher` and the XAML `FrameworkView`.
- WinRT objects marshal through `CoGetStdMarshalEx`; handing back the free-threaded marshaler avoids needing
  proxies for in-process use. Real proxy DLLs still got wired for cross-apartment cases: HKCR\Interface and the
  proxy CLSIDs come from the hive (`gen-interfaces-reg.py`, now on a 0.2 s hive parser `fasthive.py`), and
  Wine's rpcrt4 learned modern MIDL proxy tables: read-only vtables (shadow copies), `-1` "delegated" markers,
  `pIIDLookupRtn == -1` (scan by IID), flags in the high word of `TableSize`, missing name tables, and an
  ole32 `IInspectable` proxy so delegating WinRT proxies have a base.
- **msvcp_win.dll**: Windows UI components (Windows.UI.Xaml, CoreMessaging, ...) link the OS-internal STL
  `msvcp_win.dll`, not msvcp140. Wine forwards it to its msvcp140, whose `_Mtx_t` layout (owner/count at
  64/68 or 72/76) does not match the modern 32-byte `_Mtx_internal_imp_t` those DLLs use, so the XAML
  DependencyLocator mutex looked owned by a garbage thread and the UI thread deadlocked on it. The genuine
  `msvcp_win.dll` as a native override fixed it outright (it only needs ucrt api-sets). A proper Wine fix is a
  layout-correct msvcp_win.

// api-ms-win-appmodel-advertisingid-l1-1-0.dll: the three functions twinapi.appcore imports.
//
// twinapi.appcore.dll (installed native) imports CreateNewAdvertisingId, QueryAdvertisingId and
// QueryAdvertisingIdStatusForUser from this apiset. Wine has no implementation, so its loader
// points the unresolved thunks at allocate_stub()/stub_entry_point(), which raises
// EXCEPTION_WINE_STUB when called. In Solitaire that fires during startup:
//
//     [   1.276] AdIdProvider -- Initialize -- UseAppInstallID
//     [   1.276] *** CRITICAL ERROR ***
//     Unhandled Exception At 0x...  -- Unknown exception: 0x80000100
//     wine: Unimplemented function api-ms-win-appmodel-advertisingid-l1-1-0.dll.
//           QueryAdvertisingIdStatusForUser called at address ...
//
// 0x80000100 is EXCEPTION_WINE_STUB, which is how an unresolved *import* differs from a Wine
// `@ stub` in a .spec: nothing declares this function anywhere, the loader invented the stub.
//
// An advertising ID is exactly the kind of thing that should not exist here, and the caller has a
// documented "no advertising ID" path - the app logs "UseAppInstallID", its own fallback. So these
// report failure rather than inventing an ID. They deliberately do **not** write through any
// argument: the real signatures are not in any header or metadata, and writing eight bytes through
// a guessed out-parameter is the mistake AGENTS.md warns about. A caller that checks the HRESULT
// first - which is the whole point of returning one - never reads the out-parameter.
#include <windows.h>
#include <cstdio>
#include <cstdlib>

static void trace(const char *name)
{
    if (!getenv("WINE_UWP_ADVERTISINGID_TRACE")) return;
    fprintf(stderr, "[advertisingid] %s -> E_NOTIMPL (no advertising ID in this environment)\n", name);
}

/* E_NOTIMPL, not E_FAIL: callers test for it and take their documented fallback. */
extern "C" __declspec(dllexport) HRESULT WINAPI CreateNewAdvertisingId(void)
{
    trace("CreateNewAdvertisingId");
    return E_NOTIMPL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI QueryAdvertisingId(void)
{
    trace("QueryAdvertisingId");
    return E_NOTIMPL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI QueryAdvertisingIdStatusForUser(void)
{
    trace("QueryAdvertisingIdStatusForUser");
    return E_NOTIMPL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

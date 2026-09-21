// mrt100.dll stand-in for Wine.
// On Windows, mrt100.dll (OS component, "Microsoft .NET Native Runtime") exports
// GetManagedRuntimeService(REFGUID service, IUnknown** out). mrt100_app.dll delay-loads it inside
// DllMain and wraps the call in __try: if the delay-load throws, it falls back to a built-in
// in-DLL implementation. Reverse engineered from mrt100_app.dll 2.2.27328 at RVA 0x433ec / 0x80570.
// Until Wine's DelayLoadFailureHook raises the same VcppException Windows does, this stand-in
// raises it from inside the call so the app's own fallback path is taken.
#include <windows.h>
#include <delayimp.h>
#include <cstdio>
#define VcppException(sev, err) ((sev) | (FACILITY_VISUALCPP << 16) | (err))
extern "C" __declspec(dllexport) HRESULT WINAPI GetManagedRuntimeService(const GUID* service, void** out) {
    if (out) *out = nullptr;
    if (service)
        fprintf(stderr, "[mrt100 stand-in] GetManagedRuntimeService({%08lx-%04x-%04x-...}) -> raising delay-load failure\n",
                service->Data1, service->Data2, service->Data3);
    static DelayLoadInfo dli = {};
    dli.cb = sizeof(dli);
    dli.szDll = "mrt100.dll";
    dli.dlp.fImportByName = TRUE;
    dli.dlp.szProcName = "GetManagedRuntimeService";
    dli.dwLastError = ERROR_MOD_NOT_FOUND;
    ULONG_PTR args[1] = { (ULONG_PTR)&dli };
    // mrt100_app.dll's __except filter accepts exactly ERROR_DELAY_LOAD_FAILED (0x505), which is what
    // ntdll!LdrResolveDelayLoadedAPI raises on Windows 8+. Wine raises EXCEPTION_WINE_STUB instead.
    RaiseException(ERROR_DELAY_LOAD_FAILED, 0, 1, args);
    return E_NOINTERFACE;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

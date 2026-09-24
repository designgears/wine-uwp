// ext-ms-win-devmgmt-policy-l1-1-1: the MDM policy API set (policymanager.dll on Windows). Wine has
// neither the DLL nor a schema entry, so this file sits in system32 under the apiset's own name and
// the delay-load fallback finds it (see AGENTS.md on apisets that are not in Wine's schema).
//
// Windows.UI.Input.Inking.dll delay-loads PolicyManager_GetPolicy and _FreeGetPolicyData; XAML
// brings the ink stack up as a viewport starts taking pointer input, and the unresolved import's
// stub raised on XAML's UI thread - a fail-fast the first time Shapr3D's sketch viewport got a
// click. No device-management policy is configured on this machine, so every lookup answers "not
// found", which is what an unmanaged Windows machine answers; callers take their defaults. Nothing
// is written through the output, since there is no data to hand back and callers only read it on
// success.
#include <windows.h>

extern "C" __declspec(dllexport) HRESULT WINAPI PolicyManager_GetPolicy(const WCHAR *area, const WCHAR *name, void *value)
{
    (void)area; (void)name; (void)value;
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

extern "C" __declspec(dllexport) HRESULT WINAPI PolicyManager_FreeGetPolicyData(void *data)
{
    (void)data;   /* nothing is ever handed out */
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

// api-ms-win-net-isolation-l1-1-1: the network-isolation (AppContainer firewall) API set, which
// maps to firewallapi.dll on Windows. Wine has neither the DLL nor a schema entry for the set, so
// this file is dropped into system32 under the apiset's own name and found by the delay-load
// fallback (see AGENTS.md on apisets that are not in Wine's schema).
//
// Windows.Web.Http's SendRequestOperation::CreateInternalRequest calls
// NetworkIsolationDiagnoseConnectFailure(serverName) for every request, before a connection
// exists, and fails the request on a non-zero result. With the import unresolved the delay-load
// stub raised and took the whole app down - Shapr3D's first HttpClient request after startup.
// There is no AppContainer isolation here to diagnose: report success.
#include <windows.h>
#include <cstdio>

extern "C" __declspec(dllexport) DWORD WINAPI NetworkIsolationDiagnoseConnectFailure(LPCWSTR server)
{
    (void)server;
    return ERROR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

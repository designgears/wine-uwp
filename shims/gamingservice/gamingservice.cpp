// GamingServices / GamingServicesNet: stand-in NT services.
//
// Gaming Services is a platform of two auto-start services (GamingServices, LocalSystem, and
// GamingServicesNet, LocalService) plus the kernel drivers xvdd.sys and gameflt.sys, none of
// which can run under Wine. Its client API is answered by shims/gameplatform. The Xbox app also
// checks the services themselves: its full-trust server starts "GamingServices" through the
// service control manager (ServiceMonitor::StartAsync) and fails with
// ERROR_SERVICE_DOES_NOT_EXIST without it.
//
// So the services exist, start and stop, and do nothing else: there is no game platform behind
// them, which is what the client stand-in reports too. One executable serves both names.
#include <windows.h>
#include <cstdio>

static SERVICE_STATUS_HANDLE status_handle;
static SERVICE_STATUS status;
static HANDLE stop_event;

static void set_state(DWORD state)
{
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = NO_ERROR;
    SetServiceStatus(status_handle, &status);
}

static DWORD WINAPI handler(DWORD control, DWORD, void *, void *)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
    {
        set_state(SERVICE_STOP_PENDING);
        SetEvent(stop_event);
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_INTERROGATE) return NO_ERROR;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI service_main(DWORD argc, WCHAR **argv)
{
    const WCHAR *name = argc ? argv[0] : L"GamingServices";

    if (!(status_handle = RegisterServiceCtrlHandlerExW(name, handler, nullptr))) return;
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    set_state(SERVICE_RUNNING);
    fprintf(stderr, "[gamingservice] %ls running (stand-in)\n", name);
    WaitForSingleObject(stop_event, INFINITE);
    set_state(SERVICE_STOPPED);
}

int wmain(void)
{
    SERVICE_TABLE_ENTRYW table[] = { { (WCHAR *)L"", service_main }, { nullptr, nullptr } };
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}

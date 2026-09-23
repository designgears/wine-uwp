// Send one message to every window of class Windows.UI.Core.CoreWindow (top-level or child)
// owned by another process, and report the result.
//
//   probe-sendmsg.exe <msg> [wparam] [lparam]     (numbers accept 0x)
//
// Written to test whether the undocumented 0x270 that Windows.UI's WindowServer::WndProc maps to
// OnVisibilityChangedEvent reaches that handler when sent from outside the app.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static UINT g_msg; static WPARAM g_wp; static LPARAM g_lp;

static void try_window(HWND hwnd)
{
    char cls[128] = "";
    DWORD pid = 0;
    GetClassNameA(hwnd, cls, sizeof(cls));
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() || strcmp(cls, "Windows.UI.Core.CoreWindow")) return;
    DWORD_PTR res = 0;
    LRESULT ok = SendMessageTimeoutA(hwnd, g_msg, g_wp, g_lp, SMTO_NORMAL, 5000, &res);
    printf("hwnd %p pid %lu class '%s' visible %d -> ok %Id result %Ix\n", hwnd, pid, cls,
           IsWindowVisible(hwnd), ok, res);
    fflush(stdout);
}

static BOOL CALLBACK child(HWND h, LPARAM) { try_window(h); return TRUE; }
static BOOL CALLBACK top(HWND h, LPARAM) { try_window(h); EnumChildWindows(h, child, 0); return TRUE; }

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: probe-sendmsg <msg> [wparam] [lparam]\n"); return 1; }
    g_msg = strtoul(argv[1], nullptr, 0);
    g_wp = argc > 2 ? strtoull(argv[2], nullptr, 0) : 0;
    g_lp = argc > 3 ? strtoll(argv[3], nullptr, 0) : 0;
    EnumWindows(top, 0);
    return 0;
}

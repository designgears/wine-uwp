// Resize a running UWP app's windows through Wine's own API, and report what happened.
//
// `xdotool windowsize` resizes the X window directly and never reaches Wine: a +dcomp run shows
// every visual still 1280x800 after the X window was shrunk to 1000x640. So an X-side resize
// cannot be used to ask "does a size change propagate into XAML?" - it tests nothing.
//
// This goes through SetWindowPos instead, which is where WM_SIZE (and therefore CoreWindow's
// SizeChanged, and therefore XAML's layout pass) actually comes from.
//
//   probe-resize.exe <width> <height> [title-substring]
//
// It walks every top-level window in the prefix, prints the ones belonging to another process
// along with their class and current rect, and resizes those whose title or class matches. UWP
// hosts a CoreWindow as a child of a frame window, so it resizes the child too - resizing only the
// frame leaves the CoreWindow at its old size and XAML never notices.
#include <windows.h>
#include <cstdio>
#include <cstring>

static int g_w, g_h;
static const char *g_match;
static int g_resized;

static void describe(HWND hwnd, const char *tag)
{
    char cls[128] = "", title[256] = "";
    RECT r = {};
    GetClassNameA(hwnd, cls, sizeof(cls));
    GetWindowTextA(hwnd, title, sizeof(title));
    GetWindowRect(hwnd, &r);
    printf("  %s hwnd %p class '%s' title '%s' rect %ldx%ld at %ld,%ld visible %d\n",
           tag, hwnd, cls, title, r.right - r.left, r.bottom - r.top, r.left, r.top,
           IsWindowVisible(hwnd));
    fflush(stdout);
}

static BOOL CALLBACK resize_child(HWND child, LPARAM)
{
    describe(child, "child");
    /* The CoreWindow child has to be resized as well: XAML reads its size, not the frame's. */
    if (!SetWindowPos(child, nullptr, 0, 0, g_w, g_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
        printf("    child SetWindowPos failed %lu\n", GetLastError());
    else
        printf("    child resized to %dx%d\n", g_w, g_h);
    fflush(stdout);
    return TRUE;
}

static BOOL CALLBACK enum_top(HWND hwnd, LPARAM)
{
    DWORD pid = 0;
    char cls[128] = "", title[256] = "";
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    GetClassNameA(hwnd, cls, sizeof(cls));
    GetWindowTextA(hwnd, title, sizeof(title));
    if (!*cls && !*title) return TRUE;

    describe(hwnd, "top");
    if (g_match && *g_match && !strstr(title, g_match) && !strstr(cls, g_match)) return TRUE;

    if (!SetWindowPos(hwnd, nullptr, 0, 0, g_w, g_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
        printf("    SetWindowPos failed %lu\n", GetLastError());
    else
    {
        printf("    resized to %dx%d\n", g_w, g_h);
        g_resized++;
    }
    fflush(stdout);
    EnumChildWindows(hwnd, resize_child, 0);
    return TRUE;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: probe-resize <width> <height> [title-substring]\n"); return 2; }
    g_w = atoi(argv[1]);
    g_h = atoi(argv[2]);
    g_match = argc > 3 ? argv[3] : "";
    printf("resizing windows matching '%s' to %dx%d\n", g_match, g_w, g_h);
    EnumWindows(enum_top, 0);
    printf("resized %d top-level window(s)\n", g_resized);
    return g_resized ? 0 : 1;
}

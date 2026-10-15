/* probe-xchild: is a child window owned by another process, painted with GDI, visible under Wine?
 *   probe-xchild.exe parent [seconds]        a 600x400 top-level; prints its HWND
 *   probe-xchild.exe child <hwnd> [seconds]  a red 300x200 child of that window, painted on WM_PAINT and by a timer */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT && GetParent(hwnd))
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        HBRUSH b = CreateSolidBrush(RGB(220, 30, 30));
        FillRect(dc, &r, b);
        DeleteObject(b);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_TIMER)
    {
        HDC dc = GetDC(hwnd);
        RECT r = { 10, 10, 100, 60 };
        HBRUSH b = CreateSolidBrush(RGB(30, 30, 220));
        FillRect(dc, &r, b);
        DeleteObject(b);
        ReleaseDC(hwnd, dc);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(int argc, wchar_t **argv)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"probe-xchild";
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassW(&wc);
    bool child = argc > 2 && !wcscmp(argv[1], L"child");
    DWORD seconds = _wtoi(argv[child ? 3 : 2 < argc ? 2 : 1]);
    if (!seconds) seconds = 30;
    HWND hwnd;
    if (child)
    {
        HWND parent = (HWND)(ULONG_PTR)wcstoull(argv[2], nullptr, 16);
        wc.lpszClassName = L"probe-xchild-c";
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        hwnd = CreateWindowW(L"probe-xchild-c", L"", WS_CHILD | WS_VISIBLE, 50, 50, 300, 200, parent, nullptr, wc.hInstance, nullptr);
        SetTimer(hwnd, 1, 200, nullptr);
    }
    else
        hwnd = CreateWindowW(L"probe-xchild", L"probe-xchild", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 600, 400, nullptr, nullptr, wc.hInstance, nullptr);
    printf("%p\n", hwnd);
    fflush(stdout);
    DWORD end = GetTickCount() + seconds * 1000;
    MSG msg;
    while (GetTickCount() < end)
        if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT) == WAIT_OBJECT_0)
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    return 0;
}

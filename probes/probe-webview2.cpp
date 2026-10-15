/* probe-webview2: does a WebView2 runtime run here at all, in a plain window (windowed hosting)?
 *
 *   probe-webview2.exe <browser executable folder or ""> <url> [seconds]
 *
 * Loads WebView2Loader.dll (from the WebView2 SDK, next to the exe), creates an environment for the given runtime
 * folder ("" = the installed runtime), a windowed controller on a 1000x700 window and navigates. Prints each step's
 * HRESULT, NavigationCompleted, the document title, and the result of a script, then keeps the window up for the
 * given time so it can be captured. Built against the SDK's WebView2.h, which lives in uwp-bin (not in git):
 * cmake -DWEBVIEW2_SDK=<dir with WebView2.h>. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlwapi.h>
#include "WebView2.h"

static HWND window;
static ICoreWebView2Controller *controller;
static ICoreWebView2 *webview;
static const wchar_t *url;

template <class I> struct Handler : I
{
    LONG ref = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(I)) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
};

struct ScriptDone : Handler<ICoreWebView2ExecuteScriptCompletedHandler>
{
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, LPCWSTR json) override
    {
        printf("script -> %#lx %ls\n", (unsigned long)hr, json ? json : L"(null)");
        return S_OK;
    }
};

struct CaptureDone : Handler<ICoreWebView2CapturePreviewCompletedHandler>
{
    IStream *stream;
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr) override
    {
        printf("CapturePreview -> %#lx\n", (unsigned long)hr);
        fflush(stdout);
        stream->Release();
        return S_OK;
    }
};

struct NavDone : Handler<ICoreWebView2NavigationCompletedEventHandler>
{
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) override
    {
        BOOL ok = FALSE;
        COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
        LPWSTR title = nullptr;
        args->get_IsSuccess(&ok);
        args->get_WebErrorStatus(&status);
        sender->get_DocumentTitle(&title);
        printf("NavigationCompleted success %d status %d title \"%ls\"\n", ok, status, title ? title : L"");
        CoTaskMemFree(title);
        sender->ExecuteScript(L"navigator.userAgent + ' | ' + document.body.innerText.length", new ScriptDone);
        /* the page as an image, whether or not the window shows it */
        CaptureDone *cap = new CaptureDone;
        if (SUCCEEDED(SHCreateStreamOnFileW(L"capture.png", STGM_CREATE | STGM_WRITE, &cap->stream)))
            sender->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, cap->stream, cap);
        fflush(stdout);
        return S_OK;
    }
};

struct NavStart : Handler<ICoreWebView2NavigationStartingEventHandler>
{
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2NavigationStartingEventArgs *args) override
    {
        LPWSTR uri = nullptr;
        args->get_Uri(&uri);
        printf("NavigationStarting %ls\n", uri ? uri : L"");
        CoTaskMemFree(uri);
        fflush(stdout);
        return S_OK;
    }
};

static BOOL CALLBACK list_child(HWND hwnd, LPARAM)
{
    wchar_t cls[128];
    RECT r;
    DWORD pid = 0;
    GetClassNameW(hwnd, cls, 128);
    GetWindowRect(hwnd, &r);
    GetWindowThreadProcessId(hwnd, &pid);
    printf("child %p class %ls pid %lu rect %ld,%ld-%ld,%ld parent %p\n", hwnd, cls, pid, r.left, r.top, r.right, r.bottom, GetParent(hwnd));
    return TRUE;
}

struct click_ctx { int x, y; const char *only; };
/* PROBE_CLICK=x,y[,class]: post a left click at client coordinates to every child window (or those of a class) */
static void CALLBACK click_timer(HWND, UINT, UINT_PTR id, DWORD)
{
    KillTimer(window, id);
    EnumChildWindows(window, list_child, 0);
    char *c = getenv("PROBE_CLICK");
    if (!c) return;
    int x = atoi(c), y = atoi(strchr(c, ',') + 1);
    const char *only = strchr(strchr(c, ',') + 1, ',');
    static click_ctx ctx;
    ctx = { x, y, only ? only + 1 : nullptr };
    EnumChildWindows(window, [](HWND hwnd, LPARAM lp) -> BOOL {
        click_ctx *k = (click_ctx *)lp;
        char cls[128];
        GetClassNameA(hwnd, cls, 128);
        if (k->only && strcmp(cls, k->only)) return TRUE;
        POINT pt = { k->x, k->y };
        MapWindowPoints(window, hwnd, &pt, 1);
        LPARAM l = MAKELPARAM(pt.x, pt.y);
        PostMessageW(hwnd, WM_MOUSEMOVE, 0, l);
        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, l);
        PostMessageW(hwnd, WM_LBUTTONUP, 0, l);
        printf("clicked %s %p at %ld,%ld\n", cls, hwnd, pt.x, pt.y);
        return TRUE;
    }, (LPARAM)&ctx);
    fflush(stdout);
}

struct ControllerDone : Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>
{
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller *c) override
    {
        printf("controller -> %#lx %p\n", (unsigned long)hr, c);
        fflush(stdout);
        if (FAILED(hr) || !c) return S_OK;
        (controller = c)->AddRef();
        RECT r;
        GetClientRect(window, &r);
        controller->put_Bounds(r);
        controller->get_CoreWebView2(&webview);
        EventRegistrationToken t;
        webview->add_NavigationCompleted(new NavDone, &t);
        webview->add_NavigationStarting(new NavStart, &t);
        SetTimer(window, 7, 12000, click_timer);
        hr = webview->Navigate(url);
        printf("Navigate -> %#lx\n", (unsigned long)hr);
        fflush(stdout);
        return S_OK;
    }
};

struct EnvDone : Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
{
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment *env) override
    {
        printf("environment -> %#lx %p\n", (unsigned long)hr, env);
        fflush(stdout);
        if (FAILED(hr) || !env) return S_OK;
        LPWSTR ver = nullptr;
        env->get_BrowserVersionString(&ver);
        printf("browser version %ls\n", ver ? ver : L"?");
        CoTaskMemFree(ver);
        hr = env->CreateCoreWebView2Controller(window, new ControllerDone);
        printf("CreateCoreWebView2Controller -> %#lx\n", (unsigned long)hr);
        fflush(stdout);
        return S_OK;
    }
};

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE && controller) { RECT r; GetClientRect(hwnd, &r); controller->put_Bounds(r); }
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(int argc, wchar_t **argv)
{
    typedef HRESULT (STDAPICALLTYPE *create_fn)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
                                                ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
    if (argc < 3) { printf("usage: probe-webview2 <runtime folder or \"\"> <url> [seconds]\n"); return 2; }
    url = argv[2];
    DWORD seconds = argc > 3 ? _wtoi(argv[3]) : 60;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    create_fn create = loader ? (create_fn)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions") : nullptr;
    if (!create) { printf("no WebView2Loader.dll (%lu)\n", GetLastError()); return 1; }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"probe-webview2";
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    window = CreateWindowW(L"probe-webview2", L"probe-webview2", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           0, 0, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);

    wchar_t data[MAX_PATH];
    GetTempPathW(MAX_PATH, data);
    lstrcatW(data, L"probe-webview2");
    HRESULT hr = create(argv[1][0] ? argv[1] : nullptr, data, nullptr, new EnvDone);
    printf("CreateCoreWebView2EnvironmentWithOptions -> %#lx\n", (unsigned long)hr);
    fflush(stdout);

    DWORD end = GetTickCount() + seconds * 1000;
    MSG msg;
    while (GetTickCount() < end)
    {
        if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT) == WAIT_OBJECT_0)
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    printf("done\n");
    return 0;
}

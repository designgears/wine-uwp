/* EmbeddedBrowserWebView.dll stand-in: the real WebView2 runtime, hosted so that Wine can show it.
 *
 * Microsoft's WebView2 runtime (msedgewebview2.exe, from the Windows image) runs under Wine: pages load, scripts run,
 * web messages flow. What Wine cannot do is show it where apps put it:
 *   - XAML's WebView2 control (WinUI 2, used by React Native's WebView) asks for a *composition* controller, which
 *     hands the browser's output to the app's compositor across processes (DirectComposition surface handles);
 *   - a windowed controller draws into a child window of the app's window from the browser's processes, and Wine
 *     shows no GDI or swap chain output of a child window that belongs to another process.
 * So this DLL sits where the runtime's EmbeddedBrowserWebView.dll is looked up (the real one is renamed
 * EmbeddedBrowserWebView.real.dll in the same folder, so the runtime still finds itself) and answers a request for a
 * composition controller with a windowed controller on a popup window of its own, owned by the app's window and
 * placed over the control's bounds. The page reaches that popup as CoreWebView2.CapturePreview images (a few per
 * second), and the popup's mouse and keyboard input is posted to the browser's window, whose own handling is the
 * real one. Everything else - the CoreWebView2 itself, navigation, scripts, web messages - is the real runtime's.
 *
 * Chromium is started with software compositing (--disable-gpu --disable-gpu-compositing): its GPU process needs
 * more of DirectComposition than Wine's dcomp has, and its software output presents through dcomp's
 * IDCompositionDevice, which is what makes CapturePreview produce frames.
 *
 * Built against the WebView2 SDK's WebView2.h (BSD-licensed, Microsoft's, not kept in this repository; build.sh passes
 * -DWEBVIEW2_SDK). */
#include <windows.h>
#include <wincodec.h>
#include <objbase.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include "WebView2.h"

static HMODULE real_module;
static HINSTANCE self_instance;

static void tracef(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    fprintf(stderr, "[webview2] %s\n", buf);
}

/* ---- the popup that shows the page ---- */

struct Host;
static const wchar_t POPUP_CLASS[] = L"wineuwp_webview2_host";
static LRESULT CALLBACK popup_proc(HWND, UINT, WPARAM, LPARAM);

static void register_popup_class(void)
{
    static bool done;
    if (done) return;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = popup_proc;
    wc.hInstance = self_instance;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = POPUP_CLASS;
    RegisterClassW(&wc);
    done = true;
}

template <class I> struct Callback : I
{
    LONG ref = 1;
    virtual ~Callback() {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(I)) { *out = static_cast<I *>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
};

/* The wrapper XAML gets as its composition controller: ICoreWebView2CompositionController(1-4) over a windowed
 * ICoreWebView2Controller(1-4) whose parent is the popup. */
struct Host : ICoreWebView2CompositionController4, ICoreWebView2Controller4
{
    LONG ref = 1;
    ICoreWebView2Controller *real = nullptr;
    ICoreWebView2Controller4 *real4 = nullptr;
    ICoreWebView2 *webview = nullptr;
    IUnknown *root_visual = nullptr;
    HWND owner = nullptr, popup = nullptr;
    RECT bounds = {};
    BOOL visible = TRUE;
    HBITMAP frame = nullptr;
    int frame_w = 0, frame_h = 0;
    bool capturing = false, closed = false;
    IStream *capture_stream = nullptr;
    EventRegistrationToken cursor_tokens = {};

    virtual ~Host() {}

    void place()
    {
        if (!popup || closed) return;
        POINT pt = { bounds.left, bounds.top };
        ClientToScreen(owner, &pt);
        int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
        bool show = visible && w > 0 && h > 0 && IsWindowVisible(owner);
        if (!show)
        {
            if (IsWindowVisible(popup)) { tracef("popup hidden"); ShowWindow(popup, SW_HIDE); }
            return;
        }
        if (!IsWindowVisible(popup)) tracef("popup shown at %ld,%ld %dx%d", pt.x, pt.y, w, h);
        SetWindowPos(popup, HWND_TOP, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    HWND browser_window()
    {
        struct find { HWND found; } f = { nullptr };
        EnumChildWindows(popup, [](HWND hwnd, LPARAM lp) -> BOOL {
            wchar_t cls[64];
            GetClassNameW(hwnd, cls, 64);
            if (!wcscmp(cls, L"Chrome_WidgetWin_1")) { ((find *)lp)->found = hwnd; return FALSE; }
            return TRUE;
        }, (LPARAM)&f);
        return f.found;
    }

    void capture();
    void show_frame(IStream *stream);

    /* IUnknown, for both faces */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ICoreWebView2CompositionController) ||
            iid == __uuidof(ICoreWebView2CompositionController2) || iid == __uuidof(ICoreWebView2CompositionController3) ||
            iid == __uuidof(ICoreWebView2CompositionController4))
        {
            *out = static_cast<ICoreWebView2CompositionController4 *>(this);
            AddRef();
            return S_OK;
        }
        if (iid == __uuidof(ICoreWebView2Controller) || iid == __uuidof(ICoreWebView2Controller2) ||
            iid == __uuidof(ICoreWebView2Controller3) || iid == __uuidof(ICoreWebView2Controller4))
        {
            *out = static_cast<ICoreWebView2Controller4 *>(this);
            AddRef();
            return S_OK;
        }
        wchar_t s[40];
        StringFromGUID2(iid, s, 40);
        HRESULT hr = real->QueryInterface(iid, out);
        tracef("controller QueryInterface %ls -> real %#lx", s, (unsigned long)hr);
        return hr;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = InterlockedDecrement(&ref);
        if (!r)
        {
            if (popup) DestroyWindow(popup);
            if (frame) DeleteObject(frame);
            if (root_visual) root_visual->Release();
            if (webview) webview->Release();
            if (real4) real4->Release();
            if (real) real->Release();
            delete this;
        }
        return r;
    }

    /* ICoreWebView2CompositionController */
    HRESULT STDMETHODCALLTYPE get_RootVisualTarget(IUnknown **target) override
    {
        if (!target) return E_POINTER;
        if ((*target = root_visual)) root_visual->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_RootVisualTarget(IUnknown *target) override
    {
        /* kept, not drawn into: the page is shown by the popup */
        if (target) target->AddRef();
        if (root_visual) root_visual->Release();
        root_visual = target;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND kind, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS keys,
                                             UINT32 data, POINT point) override
    {
        /* the popup covers the control and takes the input itself */
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SendPointerInput(COREWEBVIEW2_POINTER_EVENT_KIND, ICoreWebView2PointerInfo *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Cursor(HCURSOR *cursor) override
    {
        if (!cursor) return E_POINTER;
        *cursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SystemCursorId(UINT32 *id) override
    {
        if (!id) return E_POINTER;
        *id = 32512; /* IDC_ARROW */
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE add_CursorChanged(ICoreWebView2CursorChangedEventHandler *, EventRegistrationToken *token) override
    {
        if (token) token->value = ++cursor_tokens.value;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE remove_CursorChanged(EventRegistrationToken) override { return S_OK; }
    /* ICoreWebView2CompositionController2 */
    HRESULT STDMETHODCALLTYPE get_AutomationProvider(IUnknown **provider) override
    {
        if (!provider) return E_POINTER;
        *provider = nullptr;
        return E_NOTIMPL;
    }
    /* ICoreWebView2CompositionController3 */
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *, DWORD, POINT, DWORD *effect) override { if (effect) *effect = DROPEFFECT_NONE; return S_OK; }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINT, DWORD *effect) override { if (effect) *effect = DROPEFFECT_NONE; return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINT, DWORD *effect) override { if (effect) *effect = DROPEFFECT_NONE; return S_OK; }
    /* ICoreWebView2CompositionController4 */
    HRESULT STDMETHODCALLTYPE GetNonClientRegionAtPoint(POINT, COREWEBVIEW2_NON_CLIENT_REGION_KIND *kind) override
    {
        if (kind) *kind = COREWEBVIEW2_NON_CLIENT_REGION_KIND_CLIENT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryNonClientRegion(COREWEBVIEW2_NON_CLIENT_REGION_KIND, ICoreWebView2RegionRectCollectionView **rects) override
    {
        if (rects) *rects = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE add_NonClientRegionChanged(ICoreWebView2NonClientRegionChangedEventHandler *, EventRegistrationToken *token) override
    {
        if (token) token->value = ++cursor_tokens.value;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE remove_NonClientRegionChanged(EventRegistrationToken) override { return S_OK; }

    /* ICoreWebView2Controller: forwarded, with the bounds and visibility applied to the popup */
    HRESULT STDMETHODCALLTYPE get_IsVisible(BOOL *v) override { if (!v) return E_POINTER; *v = visible; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_IsVisible(BOOL v) override
    {
        tracef("IsVisible %d", v);
        visible = v;
        place();
        return real->put_IsVisible(v);
    }
    HRESULT STDMETHODCALLTYPE get_Bounds(RECT *r) override { if (!r) return E_POINTER; *r = bounds; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_Bounds(RECT r) override
    {
        tracef("Bounds %ld,%ld-%ld,%ld", r.left, r.top, r.right, r.bottom);
        bounds = r;
        place();
        RECT inner = { 0, 0, r.right - r.left, r.bottom - r.top };
        return real->put_Bounds(inner);
    }
    HRESULT STDMETHODCALLTYPE get_ZoomFactor(double *z) override { return real->get_ZoomFactor(z); }
    HRESULT STDMETHODCALLTYPE put_ZoomFactor(double z) override { return real->put_ZoomFactor(z); }
    HRESULT STDMETHODCALLTYPE add_ZoomFactorChanged(ICoreWebView2ZoomFactorChangedEventHandler *h, EventRegistrationToken *t) override { return real->add_ZoomFactorChanged(h, t); }
    HRESULT STDMETHODCALLTYPE remove_ZoomFactorChanged(EventRegistrationToken t) override { return real->remove_ZoomFactorChanged(t); }
    HRESULT STDMETHODCALLTYPE SetBoundsAndZoomFactor(RECT r, double z) override
    {
        put_Bounds(r);
        return real->put_ZoomFactor(z);
    }
    HRESULT STDMETHODCALLTYPE MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON reason) override
    {
        if (popup && visible) SetForegroundWindow(popup);
        return real->MoveFocus(reason);
    }
    HRESULT STDMETHODCALLTYPE add_MoveFocusRequested(ICoreWebView2MoveFocusRequestedEventHandler *h, EventRegistrationToken *t) override { return real->add_MoveFocusRequested(h, t); }
    HRESULT STDMETHODCALLTYPE remove_MoveFocusRequested(EventRegistrationToken t) override { return real->remove_MoveFocusRequested(t); }
    HRESULT STDMETHODCALLTYPE add_GotFocus(ICoreWebView2FocusChangedEventHandler *h, EventRegistrationToken *t) override { return real->add_GotFocus(h, t); }
    HRESULT STDMETHODCALLTYPE remove_GotFocus(EventRegistrationToken t) override { return real->remove_GotFocus(t); }
    HRESULT STDMETHODCALLTYPE add_LostFocus(ICoreWebView2FocusChangedEventHandler *h, EventRegistrationToken *t) override { return real->add_LostFocus(h, t); }
    HRESULT STDMETHODCALLTYPE remove_LostFocus(EventRegistrationToken t) override { return real->remove_LostFocus(t); }
    HRESULT STDMETHODCALLTYPE add_AcceleratorKeyPressed(ICoreWebView2AcceleratorKeyPressedEventHandler *h, EventRegistrationToken *t) override { return real->add_AcceleratorKeyPressed(h, t); }
    HRESULT STDMETHODCALLTYPE remove_AcceleratorKeyPressed(EventRegistrationToken t) override { return real->remove_AcceleratorKeyPressed(t); }
    HRESULT STDMETHODCALLTYPE get_ParentWindow(HWND *w) override { if (!w) return E_POINTER; *w = owner; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_ParentWindow(HWND w) override
    {
        tracef("ParentWindow %p", w);
        if (w && w != owner) { owner = w; SetWindowLongPtrW(popup, GWLP_HWNDPARENT, (LONG_PTR)w); place(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE NotifyParentWindowPositionChanged() override { place(); return real->NotifyParentWindowPositionChanged(); }
    HRESULT STDMETHODCALLTYPE Close() override
    {
        tracef("Close");
        closed = true;
        if (popup) ShowWindow(popup, SW_HIDE);
        return real->Close();
    }
    HRESULT STDMETHODCALLTYPE get_CoreWebView2(ICoreWebView2 **w) override { return real->get_CoreWebView2(w); }
    /* ICoreWebView2Controller2-4 */
    HRESULT STDMETHODCALLTYPE get_DefaultBackgroundColor(COREWEBVIEW2_COLOR *c) override { return real4 ? real4->get_DefaultBackgroundColor(c) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_DefaultBackgroundColor(COREWEBVIEW2_COLOR c) override { return real4 ? real4->put_DefaultBackgroundColor(c) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_RasterizationScale(double *s) override { return real4 ? real4->get_RasterizationScale(s) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_RasterizationScale(double s) override { return real4 ? real4->put_RasterizationScale(s) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_ShouldDetectMonitorScaleChanges(BOOL *v) override { return real4 ? real4->get_ShouldDetectMonitorScaleChanges(v) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_ShouldDetectMonitorScaleChanges(BOOL v) override { return real4 ? real4->put_ShouldDetectMonitorScaleChanges(v) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE add_RasterizationScaleChanged(ICoreWebView2RasterizationScaleChangedEventHandler *h, EventRegistrationToken *t) override { return real4 ? real4->add_RasterizationScaleChanged(h, t) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE remove_RasterizationScaleChanged(EventRegistrationToken t) override { return real4 ? real4->remove_RasterizationScaleChanged(t) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_BoundsMode(COREWEBVIEW2_BOUNDS_MODE *m) override { return real4 ? real4->get_BoundsMode(m) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE m) override { return real4 ? real4->put_BoundsMode(m) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_AllowExternalDrop(BOOL *v) override { return real4 ? real4->get_AllowExternalDrop(v) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_AllowExternalDrop(BOOL v) override { return real4 ? real4->put_AllowExternalDrop(v) : E_NOTIMPL; }
};

struct CaptureDone : Callback<ICoreWebView2CapturePreviewCompletedHandler>
{
    Host *host;
    IStream *stream;
    CaptureDone(Host *h, IStream *s) : host(h), stream(s) { host->AddRef(); }
    ~CaptureDone() { stream->Release(); host->Release(); }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr) override
    {
        static int traced;
        if (traced < 5 || FAILED(hr)) { traced++; tracef("CapturePreview -> %#lx", (unsigned long)hr); }
        host->capturing = false;
        if (SUCCEEDED(hr) && !host->closed) host->show_frame(stream);
        return S_OK;
    }
};

void Host::capture()
{
    if (capturing || closed || !visible || !IsWindowVisible(popup)) return;
    if (!webview && FAILED(real->get_CoreWebView2(&webview))) return;
    IStream *stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return;
    capturing = true;
    CaptureDone *done = new CaptureDone(this, stream);
    HRESULT hr = webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream, done);
    static int traced;
    if (traced++ < 3 || FAILED(hr)) tracef("CapturePreview requested -> %#lx", (unsigned long)hr);
    if (FAILED(hr)) capturing = false;
    done->Release();
}

void Host::show_frame(IStream *stream)
{
    IWICImagingFactory *wic = nullptr;
    IWICBitmapDecoder *decoder = nullptr;
    IWICBitmapFrameDecode *source = nullptr;
    IWICFormatConverter *conv = nullptr;
    LARGE_INTEGER zero = {};
    if (GetEnvironmentVariableA("WINE_UWP_WEBVIEW2_DUMP", nullptr, 0))
    {
        /* the last frame as the runtime delivered it, for checking what the popup should show */
        HGLOBAL mem;
        STATSTG st;
        if (SUCCEEDED(GetHGlobalFromStream(stream, &mem)) && SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME)))
        {
            HANDLE f = CreateFileW(L"C:\\users\\Public\\webview2-frame.png", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD written;
            if (f != INVALID_HANDLE_VALUE)
            {
                WriteFile(f, GlobalLock(mem), (DWORD)st.cbSize.QuadPart, &written, nullptr);
                GlobalUnlock(mem);
                CloseHandle(f);
            }
        }
    }
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &source)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
    {
        UINT w, h;
        conv->GetSize(&w, &h);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -(LONG)h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void *bits = nullptr;
        HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && SUCCEEDED(conv->CopyPixels(nullptr, w * 4, w * 4 * h, (BYTE *)bits)))
        {
            if (frame) DeleteObject(frame);
            frame = bmp;
            frame_w = w;
            frame_h = h;
            InvalidateRect(popup, nullptr, FALSE);
            static int traced;
            if (traced++ < 3) tracef("frame %ux%u", w, h);
        }
        else if (bmp) DeleteObject(bmp);
    }
    if (conv) conv->Release();
    if (source) source->Release();
    if (decoder) decoder->Release();
    if (wic) wic->Release();
}

static LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Host *host = (Host *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_TIMER:
        if (wp == 2) KillTimer(hwnd, 2);
        if (host) { host->place(); host->capture(); }
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        if (host && host->frame)
        {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, host->frame);
            BitBlt(dc, 0, 0, host->frame_w, host->frame_h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        else FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW));
        return TRUE;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        if (host)
            if (HWND browser = host->browser_window())
            {
                if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) SetCapture(hwnd);
                if (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) ReleaseCapture();
                PostMessageW(browser, msg, wp, lp);
                SetTimer(hwnd, 2, 30, nullptr);  /* a quick frame after input */
            }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the environment ---- */

struct ControllerCreated : Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>
{
    Host *host;
    ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler *app;
    ControllerCreated(Host *h, ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler *a) : host(h), app(a) { app->AddRef(); }
    ~ControllerCreated() { app->Release(); host->Release(); }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller *controller) override
    {
        tracef("windowed controller -> %#lx %p", (unsigned long)hr, controller);
        if (FAILED(hr) || !controller) return app->Invoke(FAILED(hr) ? hr : E_FAIL, nullptr);
        (host->real = controller)->AddRef();
        controller->QueryInterface(IID_PPV_ARGS(&host->real4));
        controller->put_IsVisible(TRUE);
        SetTimer(host->popup, 1, 120, nullptr);
        host->AddRef();
        hr = app->Invoke(S_OK, static_cast<ICoreWebView2CompositionController4 *>(host));
        host->Release();
        return hr;
    }
};

struct Environment : ICoreWebView2Environment10
{
    LONG ref = 1;
    ICoreWebView2Environment *real;
    ICoreWebView2Environment10 *real10 = nullptr;
    virtual ~Environment() {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        static const IID *mine[] = { &__uuidof(IUnknown), &__uuidof(ICoreWebView2Environment), &__uuidof(ICoreWebView2Environment2),
            &__uuidof(ICoreWebView2Environment3), &__uuidof(ICoreWebView2Environment4), &__uuidof(ICoreWebView2Environment5),
            &__uuidof(ICoreWebView2Environment6), &__uuidof(ICoreWebView2Environment7), &__uuidof(ICoreWebView2Environment8),
            &__uuidof(ICoreWebView2Environment9), &__uuidof(ICoreWebView2Environment10) };
        for (auto *m : mine)
            if (iid == *m) { *out = static_cast<ICoreWebView2Environment10 *>(this); AddRef(); return S_OK; }
        wchar_t s[40];
        StringFromGUID2(iid, s, 40);
        HRESULT hr = real->QueryInterface(iid, out);
        tracef("environment QueryInterface %ls -> real %#lx", s, (unsigned long)hr);
        return hr;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = InterlockedDecrement(&ref);
        if (!r) { if (real10) real10->Release(); real->Release(); delete this; }
        return r;
    }
    HRESULT STDMETHODCALLTYPE CreateCoreWebView2Controller(HWND parent, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *h) override
    {
        tracef("CreateCoreWebView2Controller %p", parent);
        return real->CreateCoreWebView2Controller(parent, h);
    }
    HRESULT STDMETHODCALLTYPE CreateWebResourceResponse(IStream *content, int status, LPCWSTR reason, LPCWSTR headers,
                                                        ICoreWebView2WebResourceResponse **out) override
    {
        return real->CreateWebResourceResponse(content, status, reason, headers, out);
    }
    HRESULT STDMETHODCALLTYPE get_BrowserVersionString(LPWSTR *v) override { return real->get_BrowserVersionString(v); }
    HRESULT STDMETHODCALLTYPE add_NewBrowserVersionAvailable(ICoreWebView2NewBrowserVersionAvailableEventHandler *h, EventRegistrationToken *t) override { return real->add_NewBrowserVersionAvailable(h, t); }
    HRESULT STDMETHODCALLTYPE remove_NewBrowserVersionAvailable(EventRegistrationToken t) override { return real->remove_NewBrowserVersionAvailable(t); }
    HRESULT STDMETHODCALLTYPE CreateWebResourceRequest(LPCWSTR uri, LPCWSTR method, IStream *post, LPCWSTR headers, ICoreWebView2WebResourceRequest **out) override
    {
        return real10->CreateWebResourceRequest(uri, method, post, headers, out);
    }
    HRESULT create_composition(HWND parent, ICoreWebView2ControllerOptions *options,
                               ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler *handler)
    {
        tracef("composition controller for %p: a windowed controller on a popup", parent);
        register_popup_class();
        Host *host = new Host;
        host->owner = parent;
        host->popup = CreateWindowExW(WS_EX_TOOLWINDOW, POPUP_CLASS, L"", WS_POPUP, 0, 0, 1, 1,
                                      parent, nullptr, self_instance, nullptr);
        SetWindowLongPtrW(host->popup, GWLP_USERDATA, (LONG_PTR)host);
        ControllerCreated *created = new ControllerCreated(host, handler);   /* takes the reference */
        HRESULT hr = options ? real10->CreateCoreWebView2ControllerWithOptions(host->popup, options, created)
                             : real->CreateCoreWebView2Controller(host->popup, created);
        created->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateCoreWebView2CompositionController(HWND parent, ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler *h) override
    {
        return create_composition(parent, nullptr, h);
    }
    HRESULT STDMETHODCALLTYPE CreateCoreWebView2PointerInfo(ICoreWebView2PointerInfo **out) override { return real10->CreateCoreWebView2PointerInfo(out); }
    HRESULT STDMETHODCALLTYPE GetAutomationProviderForWindow(HWND hwnd, IUnknown **out) override { return real10->GetAutomationProviderForWindow(hwnd, out); }
    HRESULT STDMETHODCALLTYPE add_BrowserProcessExited(ICoreWebView2BrowserProcessExitedEventHandler *h, EventRegistrationToken *t) override { return real10->add_BrowserProcessExited(h, t); }
    HRESULT STDMETHODCALLTYPE remove_BrowserProcessExited(EventRegistrationToken t) override { return real10->remove_BrowserProcessExited(t); }
    HRESULT STDMETHODCALLTYPE CreatePrintSettings(ICoreWebView2PrintSettings **out) override { return real10->CreatePrintSettings(out); }
    HRESULT STDMETHODCALLTYPE get_UserDataFolder(LPWSTR *v) override { return real10->get_UserDataFolder(v); }
    HRESULT STDMETHODCALLTYPE add_ProcessInfosChanged(ICoreWebView2ProcessInfosChangedEventHandler *h, EventRegistrationToken *t) override { return real10->add_ProcessInfosChanged(h, t); }
    HRESULT STDMETHODCALLTYPE remove_ProcessInfosChanged(EventRegistrationToken t) override { return real10->remove_ProcessInfosChanged(t); }
    HRESULT STDMETHODCALLTYPE GetProcessInfos(ICoreWebView2ProcessInfoCollection **out) override { return real10->GetProcessInfos(out); }
    HRESULT STDMETHODCALLTYPE CreateContextMenuItem(LPCWSTR label, IStream *icon, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND kind, ICoreWebView2ContextMenuItem **out) override
    {
        return real10->CreateContextMenuItem(label, icon, kind, out);
    }
    HRESULT STDMETHODCALLTYPE CreateCoreWebView2ControllerOptions(ICoreWebView2ControllerOptions **out) override { return real10->CreateCoreWebView2ControllerOptions(out); }
    HRESULT STDMETHODCALLTYPE CreateCoreWebView2ControllerWithOptions(HWND parent, ICoreWebView2ControllerOptions *o, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *h) override
    {
        return real10->CreateCoreWebView2ControllerWithOptions(parent, o, h);
    }
    HRESULT STDMETHODCALLTYPE CreateCoreWebView2CompositionControllerWithOptions(HWND parent, ICoreWebView2ControllerOptions *o,
            ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler *h) override
    {
        return create_composition(parent, o, h);
    }
};

static bool is_wrapper(ICoreWebView2Environment *env)
{
    Environment probe;
    bool same = *(void ***)env == *(void ***)static_cast<ICoreWebView2Environment10 *>(&probe);
    probe.real = nullptr;
    return same;
}

struct EnvironmentCreated : Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
{
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *app;
    EnvironmentCreated(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *a) : app(a) { app->AddRef(); }
    ~EnvironmentCreated() { app->Release(); }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment *env) override
    {
        tracef("environment -> %#lx %p", (unsigned long)hr, env);
        if (FAILED(hr) || !env) return app->Invoke(hr, env);
        if (is_wrapper(env)) return app->Invoke(hr, env);
        Environment *wrap = new Environment;
        (wrap->real = env)->AddRef();
        if (FAILED(env->QueryInterface(IID_PPV_ARGS(&wrap->real10))))
        {
            tracef("runtime has no ICoreWebView2Environment10; passing it through");
            wrap->Release();
            return app->Invoke(hr, env);
        }
        tracef("environment %p wrapped as %p (handler %p)", env, wrap, app);
        hr = app->Invoke(S_OK, wrap);
        tracef("app's environment handler -> %#lx", (unsigned long)hr);
        wrap->Release();
        return hr;
    }
};

static const wchar_t EXTRA_ARGS[] = L"--disable-gpu --disable-gpu-compositing";

static void add_browser_arguments(IUnknown *options)
{
    ICoreWebView2EnvironmentOptions *o = nullptr;
    LPWSTR args = nullptr;
    if (!options || FAILED(options->QueryInterface(IID_PPV_ARGS(&o)))) return;
    std::wstring all;
    if (SUCCEEDED(o->get_AdditionalBrowserArguments(&args)) && args) { all = args; CoTaskMemFree(args); }
    if (all.find(L"--disable-gpu") == std::wstring::npos)
    {
        if (!all.empty()) all += L" ";
        all += EXTRA_ARGS;
        o->put_AdditionalBrowserArguments(all.c_str());
    }
    tracef("browser arguments: %ls", all.c_str());
    o->Release();
}

typedef HRESULT (STDAPICALLTYPE *create_fn)(UINT_PTR, UINT_PTR, PCWSTR, IUnknown *, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE CreateWebViewEnvironmentWithOptionsInternal(
        UINT_PTR check_running, UINT_PTR runtime_type, PCWSTR user_data, IUnknown *options,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler)
{
    if (!real_module)
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(self_instance, path, MAX_PATH);
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) wcscpy(slash + 1, L"EmbeddedBrowserWebView.real.dll");
        real_module = LoadLibraryW(path);
        tracef("real runtime %ls -> %p", path, real_module);
    }
    create_fn create = real_module ? (create_fn)GetProcAddress(real_module, "CreateWebViewEnvironmentWithOptionsInternal") : nullptr;
    if (!create) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    add_browser_arguments(options);
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", EXTRA_ARGS);
    EnvironmentCreated *created = new EnvironmentCreated(handler);
    HRESULT hr = create(check_running, runtime_type, user_data, options, created);
    created->Release();
    tracef("CreateWebViewEnvironmentWithOptionsInternal -> %#lx", (unsigned long)hr);
    return hr;
}

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        HMODULE pinned;
        self_instance = instance;
        /* the loader in Microsoft.Web.WebView2.Core.dll frees the runtime DLL once the environment exists; the
         * objects handed out live here */
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)DllMain, &pinned);
    }
    return TRUE;
}

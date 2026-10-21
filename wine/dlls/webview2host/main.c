/*
 * WebView2 runtime host: shows Microsoft's WebView2 runtime where Wine cannot
 *
 * Copyright 2026 wine-uwp contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * The WebView2 runtime (msedgewebview2.exe, from a Windows image) runs under Wine: pages load, scripts run, web
 * messages flow. What Wine cannot do is show it where apps put it:
 *   - XAML's WebView2 control (WinUI 2, used by React Native's WebView) asks for a *composition* controller, which
 *     hands the browser's output to the app's compositor across processes;
 *   - a windowed controller draws into a child window of the app's window from the browser's processes, and Wine
 *     shows no output of a child window that belongs to another process.
 *
 * So this DLL is installed as the runtime's EmbeddedBrowserWebView.dll (the real one renamed
 * EmbeddedBrowserWebView.real.dll in the same folder) and answers a request for a composition controller with a
 * windowed controller on a popup of its own, owned by the app's window and placed over the control's bounds. The
 * page reaches the popup as CapturePreview PNGs, a few a second, and the popup's mouse and keyboard input is posted
 * to the browser's window. Everything else is the real runtime's.
 *
 * Chromium is started with software compositing: its GPU process needs more of DirectComposition than Wine's dcomp
 * has, and its software output is what makes CapturePreview produce frames.
 *
 * WebView2's interfaces are reached by vtable slot. The environment and the windowed controller are wrapped with
 * forwarding thunks, so only the methods that change behaviour are written out; the slot numbers below are the
 * published interface layouts.
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "wingdi.h"
#include "objbase.h"
#include "oleidl.h"
#include "wincodec.h"

#include "wine/asm.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(webview2);

static HMODULE real_module;
static HINSTANCE self_instance;

typedef struct { INT64 value; } token;

/* the interfaces this module implements or recognises */
static const IID IID_Controller =   {0x4d00c0d1,0x9434,0x4eb6,{0x80,0x78,0x86,0x97,0xa5,0x60,0x33,0x4f}};
static const IID IID_Controller2 =  {0xc979903e,0xd4ca,0x4228,{0x92,0xeb,0x47,0xee,0x3f,0xa9,0x6e,0xab}};
static const IID IID_Controller3 =  {0xf9614724,0x5d2b,0x41dc,{0xae,0xf7,0x73,0xd6,0x2b,0x51,0x54,0x3b}};
static const IID IID_Controller4 =  {0x97d418d5,0xa426,0x4e49,{0xa1,0x51,0xe1,0xa1,0x0f,0x32,0x7d,0x9e}};
static const IID IID_Composition =  {0x3df9b733,0xb9ae,0x4a15,{0x86,0xb4,0xeb,0x9e,0xe9,0x82,0x64,0x69}};
static const IID IID_Composition2 = {0x0b6a3d24,0x49cb,0x4806,{0xba,0x20,0xb5,0xe0,0x73,0x4a,0x7b,0x26}};
static const IID IID_Composition3 = {0x9570570e,0x4d76,0x4361,{0x9e,0xe1,0xf0,0x4d,0x0d,0xbd,0xfb,0x1e}};
static const IID IID_Composition4 = {0x7c367b9b,0x3d2b,0x450f,{0x9e,0x58,0xd6,0x1a,0x20,0xf4,0x86,0xaa}};
static const IID IID_Environment[10] =
{
    {0xb96d755e,0x0319,0x4e92,{0xa2,0x96,0x23,0x43,0x6f,0x46,0xa1,0xfc}},
    {0x41f3632b,0x5ef4,0x404f,{0xad,0x82,0x2d,0x60,0x6c,0x5a,0x9a,0x21}},
    {0x80a22ae3,0xbe7c,0x4ce2,{0xaf,0xe1,0x5a,0x50,0x05,0x6c,0xde,0xeb}},
    {0x20944379,0x6dcf,0x41d6,{0xa0,0xa0,0xab,0xc0,0xfc,0x50,0xde,0x0d}},
    {0x319e423d,0xe0d7,0x4b8d,{0x92,0x54,0xae,0x94,0x75,0xde,0x9b,0x17}},
    {0xe59ee362,0xacbd,0x4857,{0x9a,0x8e,0xd3,0x64,0x4d,0x94,0x59,0xa9}},
    {0x43c22296,0x3bbd,0x43a4,{0x9c,0x00,0x5c,0x0d,0xf6,0xdd,0x29,0xa2}},
    {0xd6eb91dd,0xc3d2,0x45e5,{0xbd,0x29,0x6d,0xc2,0xbc,0x4d,0xe9,0xcf}},
    {0xf06f41bf,0x4b5a,0x49d8,{0xb9,0xf6,0xfa,0x16,0xcd,0x29,0xf2,0x74}},
    {0xee0eb9df,0x6f12,0x46ce,{0xb5,0x3f,0x3f,0x47,0xb9,0xc9,0x28,0xe0}},
};
static const IID IID_EnvironmentOptions = {0x2fde08a8,0x1e9a,0x4766,{0x8c,0x05,0x95,0xa9,0xce,0xb9,0xd1,0xc5}};
static const IID IID_EnvironmentCompleted = {0x4e8a3389,0xc9d8,0x4bd2,{0xb6,0xb5,0x12,0x4f,0xee,0x6c,0xc1,0x4d}};
static const IID IID_ControllerCompleted = {0x6c4819f3,0xc9b7,0x4260,{0x81,0x27,0xc9,0xf5,0xbd,0xe7,0xf6,0x8c}};
static const IID IID_CaptureCompleted = {0x697e05e9,0x3d8f,0x45fa,{0x96,0xf4,0x8f,0xfe,0x1e,0xde,0xda,0xf5}};

/* slots used on the runtime's objects */
enum
{
    CONTROLLER_put_IsVisible = 4,
    CONTROLLER_put_Bounds = 6,
    CONTROLLER_put_ZoomFactor = 8,
    CONTROLLER_MoveFocus = 12,
    CONTROLLER_NotifyParentWindowPositionChanged = 23,
    CONTROLLER_Close = 24,
    CONTROLLER_get_CoreWebView2 = 25,
    CONTROLLER_SLOTS = 38,          /* ICoreWebView2Controller4 */
    COMPOSITION_SLOTS = 20,         /* ICoreWebView2CompositionController4 */
    ENVIRONMENT_CreateController = 3,
    ENVIRONMENT_CreateCompositionController = 9,
    ENVIRONMENT_CreateControllerWithOptions = 21,
    ENVIRONMENT_CreateCompositionControllerWithOptions = 22,
    ENVIRONMENT_SLOTS = 23,         /* ICoreWebView2Environment10 */
    WEBVIEW_CapturePreview = 30,
    OPTIONS_get_AdditionalBrowserArguments = 3,
    OPTIONS_put_AdditionalBrowserArguments = 4,
    HANDLER_Invoke = 3,
};

#define VTBL(obj) (*(void ***)(obj))
#define CALL(type, obj, slot, ...) ((type)VTBL(obj)[slot])( (obj), ##__VA_ARGS__ )

typedef HRESULT (WINAPI *fn_bool)( IUnknown *, BOOL );
typedef HRESULT (WINAPI *fn_rect)( IUnknown *, RECT );
typedef HRESULT (WINAPI *fn_double)( IUnknown *, double );
typedef HRESULT (WINAPI *fn_int)( IUnknown *, int );
typedef HRESULT (WINAPI *fn_void)( IUnknown * );
typedef HRESULT (WINAPI *fn_out)( IUnknown *, void ** );
typedef HRESULT (WINAPI *fn_wstr)( IUnknown *, const WCHAR * );
typedef HRESULT (WINAPI *fn_create)( IUnknown *, HWND, IUnknown * );
typedef HRESULT (WINAPI *fn_create_opts)( IUnknown *, HWND, IUnknown *, IUnknown * );
typedef HRESULT (WINAPI *fn_capture)( IUnknown *, int, IStream *, IUnknown * );
typedef HRESULT (WINAPI *fn_invoke)( IUnknown *, HRESULT, IUnknown * );

/*
 * A forwarding face: a vtable pointer followed by the object every unwritten slot forwards to. thunk_N replaces
 * `this` with that object and jumps to its slot N, so the call reaches the runtime with its own arguments untouched.
 */
struct face
{
    void **vtbl;
    IUnknown *target;
};

#ifdef __x86_64__
#define FORWARD(n) \
    extern void forward_##n(void); \
    __ASM_GLOBAL_FUNC( forward_##n, \
                       "movq 8(%rcx),%rcx\n\t" \
                       "movq (%rcx),%rax\n\t" \
                       "jmp *(8*" #n ")(%rax)" )
#elif defined(__i386__)
#define FORWARD(n) \
    extern void forward_##n(void); \
    __ASM_GLOBAL_FUNC( forward_##n, \
                       "movl 4(%esp),%eax\n\t" \
                       "movl 4(%eax),%eax\n\t" \
                       "movl %eax,4(%esp)\n\t" \
                       "movl (%eax),%eax\n\t" \
                       "jmp *(4*" #n ")(%eax)" )
#else
#define FORWARD(n) static void forward_##n(void) { }
#endif

FORWARD(3)  FORWARD(4)  FORWARD(5)  FORWARD(6)  FORWARD(7)  FORWARD(8)  FORWARD(9)  FORWARD(10) FORWARD(11)
FORWARD(12) FORWARD(13) FORWARD(14) FORWARD(15) FORWARD(16) FORWARD(17) FORWARD(18) FORWARD(19) FORWARD(20)
FORWARD(21) FORWARD(22) FORWARD(23) FORWARD(24) FORWARD(25) FORWARD(26) FORWARD(27) FORWARD(28) FORWARD(29)
FORWARD(30) FORWARD(31) FORWARD(32) FORWARD(33) FORWARD(34) FORWARD(35) FORWARD(36) FORWARD(37)

static void *const forwards[] =
{
    NULL, NULL, NULL, forward_3, forward_4, forward_5, forward_6, forward_7, forward_8, forward_9, forward_10,
    forward_11, forward_12, forward_13, forward_14, forward_15, forward_16, forward_17, forward_18, forward_19,
    forward_20, forward_21, forward_22, forward_23, forward_24, forward_25, forward_26, forward_27, forward_28,
    forward_29, forward_30, forward_31, forward_32, forward_33, forward_34, forward_35, forward_36, forward_37,
};

static void trace_iid( const char *what, REFIID iid, HRESULT hr )
{
    TRACE( "%s QueryInterface %s -> real %#lx\n", what, debugstr_guid( iid ), hr );
}

/* ---- the popup that shows the page ---- */

struct host
{
    struct face composition;        /* ICoreWebView2CompositionController4, all written out */
    struct face controller;         /* ICoreWebView2Controller4 over the runtime's windowed controller */
    LONG ref;
    IUnknown *real;                 /* the runtime's ICoreWebView2Controller4 */
    IUnknown *webview;
    IUnknown *root_visual;
    HWND owner, popup;
    RECT bounds;
    BOOL visible, capturing, closed;
    HBITMAP frame;
    int frame_w, frame_h;
    LONG tokens;
};

static const WCHAR popup_class[] = L"wineuwp_webview2_host";

static struct host *host_from_composition( IUnknown *iface ) { return CONTAINING_RECORD( iface, struct host, composition ); }
static struct host *host_from_controller( IUnknown *iface ) { return CONTAINING_RECORD( iface, struct host, controller ); }

static void host_place( struct host *host )
{
    POINT pt = { host->bounds.left, host->bounds.top };
    int w = host->bounds.right - host->bounds.left, h = host->bounds.bottom - host->bounds.top;

    if (!host->popup || host->closed) return;
    ClientToScreen( host->owner, &pt );
    if (!host->visible || w <= 0 || h <= 0 || !IsWindowVisible( host->owner ))
    {
        if (IsWindowVisible( host->popup ))
        {
            TRACE( "popup hidden\n" );
            ShowWindow( host->popup, SW_HIDE );
        }
        return;
    }
    if (!IsWindowVisible( host->popup )) TRACE( "popup shown at %ld,%ld %dx%d\n", pt.x, pt.y, w, h );
    SetWindowPos( host->popup, HWND_TOP, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW );
}

static BOOL CALLBACK find_browser( HWND hwnd, LPARAM lp )
{
    WCHAR cls[64];

    GetClassNameW( hwnd, cls, ARRAY_SIZE(cls) );
    if (wcscmp( cls, L"Chrome_WidgetWin_1" )) return TRUE;
    *(HWND *)lp = hwnd;
    return FALSE;
}

static HWND host_browser_window( struct host *host )
{
    HWND found = NULL;
    EnumChildWindows( host->popup, find_browser, (LPARAM)&found );
    return found;
}

static void host_destroy( struct host *host )
{
    if (host->popup) DestroyWindow( host->popup );
    if (host->frame) DeleteObject( host->frame );
    if (host->root_visual) IUnknown_Release( host->root_visual );
    if (host->webview) IUnknown_Release( host->webview );
    if (host->real) IUnknown_Release( host->real );
    free( host );
}

static ULONG host_addref( struct host *host ) { return InterlockedIncrement( &host->ref ); }

static ULONG host_release( struct host *host )
{
    ULONG ref = InterlockedDecrement( &host->ref );
    if (!ref) host_destroy( host );
    return ref;
}

static HRESULT host_query( struct host *host, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_Composition ) || IsEqualGUID( iid, &IID_Composition2 ) ||
        IsEqualGUID( iid, &IID_Composition3 ) || IsEqualGUID( iid, &IID_Composition4 ))
        *out = &host->composition;
    else if (IsEqualGUID( iid, &IID_Controller ) || IsEqualGUID( iid, &IID_Controller2 ) ||
             IsEqualGUID( iid, &IID_Controller3 ) || IsEqualGUID( iid, &IID_Controller4 ))
        *out = &host->controller;
    else
    {
        HRESULT hr = IUnknown_QueryInterface( host->real, iid, out );
        trace_iid( "controller", iid, hr );
        return hr;
    }
    host_addref( host );
    return S_OK;
}

/* ICoreWebView2CompositionController(1-4): the popup shows the page and takes the input itself */

static HRESULT WINAPI comp_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{ return host_query( host_from_composition( iface ), iid, out ); }
static ULONG WINAPI comp_AddRef( IUnknown *iface ) { return host_addref( host_from_composition( iface ) ); }
static ULONG WINAPI comp_Release( IUnknown *iface ) { return host_release( host_from_composition( iface ) ); }

static HRESULT WINAPI comp_get_RootVisualTarget( IUnknown *iface, IUnknown **target )
{
    struct host *host = host_from_composition( iface );
    if (!target) return E_POINTER;
    if ((*target = host->root_visual)) IUnknown_AddRef( *target );
    return S_OK;
}

static HRESULT WINAPI comp_put_RootVisualTarget( IUnknown *iface, IUnknown *target )
{
    struct host *host = host_from_composition( iface );
    /* kept, not drawn into: the page is shown by the popup */
    if (target) IUnknown_AddRef( target );
    if (host->root_visual) IUnknown_Release( host->root_visual );
    host->root_visual = target;
    return S_OK;
}

static HRESULT WINAPI comp_SendMouseInput( IUnknown *iface, int kind, int keys, UINT32 data, POINT point ) { return S_OK; }
static HRESULT WINAPI comp_SendPointerInput( IUnknown *iface, int kind, IUnknown *info ) { return S_OK; }

static HRESULT WINAPI comp_get_Cursor( IUnknown *iface, HCURSOR *cursor )
{
    if (!cursor) return E_POINTER;
    *cursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    return S_OK;
}

static HRESULT WINAPI comp_get_SystemCursorId( IUnknown *iface, UINT32 *id )
{
    if (!id) return E_POINTER;
    *id = 32512; /* IDC_ARROW */
    return S_OK;
}

static HRESULT WINAPI comp_add_event( IUnknown *iface, IUnknown *handler, token *out )
{
    struct host *host = host_from_composition( iface );
    if (out) out->value = InterlockedIncrement( &host->tokens );
    return S_OK;
}

static HRESULT WINAPI comp_remove_event( IUnknown *iface, token value ) { return S_OK; }

static HRESULT WINAPI comp_get_AutomationProvider( IUnknown *iface, IUnknown **provider )
{
    if (!provider) return E_POINTER;
    *provider = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI comp_DragEnter( IUnknown *iface, IDataObject *data, DWORD keys, POINT point, DWORD *effect )
{
    if (effect) *effect = DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT WINAPI comp_DragLeave( IUnknown *iface ) { return S_OK; }

static HRESULT WINAPI comp_DragOver( IUnknown *iface, DWORD keys, POINT point, DWORD *effect )
{
    if (effect) *effect = DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT WINAPI comp_Drop( IUnknown *iface, IDataObject *data, DWORD keys, POINT point, DWORD *effect )
{
    if (effect) *effect = DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT WINAPI comp_GetNonClientRegionAtPoint( IUnknown *iface, POINT point, int *kind )
{
    if (kind) *kind = 1; /* COREWEBVIEW2_NON_CLIENT_REGION_KIND_CLIENT */
    return S_OK;
}

static HRESULT WINAPI comp_QueryNonClientRegion( IUnknown *iface, int kind, IUnknown **rects )
{
    if (rects) *rects = NULL;
    return E_NOTIMPL;
}

static void *composition_vtbl[COMPOSITION_SLOTS] =
{
    comp_QueryInterface, comp_AddRef, comp_Release,
    comp_get_RootVisualTarget, comp_put_RootVisualTarget, comp_SendMouseInput, comp_SendPointerInput,
    comp_get_Cursor, comp_get_SystemCursorId, comp_add_event, comp_remove_event,       /* CursorChanged */
    comp_get_AutomationProvider,
    comp_DragEnter, comp_DragLeave, comp_DragOver, comp_Drop,
    comp_GetNonClientRegionAtPoint, comp_QueryNonClientRegion, comp_add_event, comp_remove_event, /* NonClientRegionChanged */
};

/* ICoreWebView2Controller(1-4): forwarded, with the bounds and visibility applied to the popup */

static HRESULT WINAPI ctrl_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{ return host_query( host_from_controller( iface ), iid, out ); }
static ULONG WINAPI ctrl_AddRef( IUnknown *iface ) { return host_addref( host_from_controller( iface ) ); }
static ULONG WINAPI ctrl_Release( IUnknown *iface ) { return host_release( host_from_controller( iface ) ); }

static HRESULT WINAPI ctrl_get_IsVisible( IUnknown *iface, BOOL *visible )
{
    if (!visible) return E_POINTER;
    *visible = host_from_controller( iface )->visible;
    return S_OK;
}

static HRESULT WINAPI ctrl_put_IsVisible( IUnknown *iface, BOOL visible )
{
    struct host *host = host_from_controller( iface );
    TRACE( "IsVisible %d\n", visible );
    host->visible = visible;
    host_place( host );
    return CALL( fn_bool, host->real, CONTROLLER_put_IsVisible, visible );
}

static HRESULT WINAPI ctrl_get_Bounds( IUnknown *iface, RECT *bounds )
{
    if (!bounds) return E_POINTER;
    *bounds = host_from_controller( iface )->bounds;
    return S_OK;
}

static HRESULT WINAPI ctrl_put_Bounds( IUnknown *iface, RECT bounds )
{
    struct host *host = host_from_controller( iface );
    RECT inner = { 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top };

    TRACE( "Bounds %s\n", wine_dbgstr_rect( &bounds ) );
    host->bounds = bounds;
    host_place( host );
    return CALL( fn_rect, host->real, CONTROLLER_put_Bounds, inner );
}

static HRESULT WINAPI ctrl_SetBoundsAndZoomFactor( IUnknown *iface, RECT bounds, double zoom )
{
    struct host *host = host_from_controller( iface );
    ctrl_put_Bounds( iface, bounds );
    return CALL( fn_double, host->real, CONTROLLER_put_ZoomFactor, zoom );
}

static HRESULT WINAPI ctrl_MoveFocus( IUnknown *iface, int reason )
{
    struct host *host = host_from_controller( iface );
    if (host->popup && host->visible) SetForegroundWindow( host->popup );
    return CALL( fn_int, host->real, CONTROLLER_MoveFocus, reason );
}

static HRESULT WINAPI ctrl_get_ParentWindow( IUnknown *iface, HWND *parent )
{
    if (!parent) return E_POINTER;
    *parent = host_from_controller( iface )->owner;
    return S_OK;
}

static HRESULT WINAPI ctrl_put_ParentWindow( IUnknown *iface, HWND parent )
{
    struct host *host = host_from_controller( iface );
    TRACE( "ParentWindow %p\n", parent );
    if (parent && parent != host->owner)
    {
        host->owner = parent;
        SetWindowLongPtrW( host->popup, GWLP_HWNDPARENT, (LONG_PTR)parent );
        host_place( host );
    }
    return S_OK;
}

static HRESULT WINAPI ctrl_NotifyParentWindowPositionChanged( IUnknown *iface )
{
    struct host *host = host_from_controller( iface );
    host_place( host );
    return CALL( fn_void, host->real, CONTROLLER_NotifyParentWindowPositionChanged );
}

static HRESULT WINAPI ctrl_Close( IUnknown *iface )
{
    struct host *host = host_from_controller( iface );
    TRACE( "Close\n" );
    host->closed = TRUE;
    if (host->popup) ShowWindow( host->popup, SW_HIDE );
    return CALL( fn_void, host->real, CONTROLLER_Close );
}

static void *controller_vtbl[CONTROLLER_SLOTS];

static void init_controller_vtbl(void)
{
    memcpy( controller_vtbl, forwards, sizeof(controller_vtbl) );
    controller_vtbl[0] = ctrl_QueryInterface;
    controller_vtbl[1] = ctrl_AddRef;
    controller_vtbl[2] = ctrl_Release;
    controller_vtbl[3] = ctrl_get_IsVisible;
    controller_vtbl[4] = ctrl_put_IsVisible;
    controller_vtbl[5] = ctrl_get_Bounds;
    controller_vtbl[6] = ctrl_put_Bounds;
    controller_vtbl[11] = ctrl_SetBoundsAndZoomFactor;
    controller_vtbl[12] = ctrl_MoveFocus;
    controller_vtbl[21] = ctrl_get_ParentWindow;
    controller_vtbl[22] = ctrl_put_ParentWindow;
    controller_vtbl[23] = ctrl_NotifyParentWindowPositionChanged;
    controller_vtbl[24] = ctrl_Close;
}

/* ---- the frames ---- */

/* A completion handler of ours: three IUnknown slots and Invoke. */
struct handler
{
    void **vtbl;
    LONG ref;
    void (*destroy)( struct handler * );
};

static HRESULT WINAPI handler_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    /* the runtime asks only for IUnknown and the handler's own interface */
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_EnvironmentCompleted ) ||
        IsEqualGUID( iid, &IID_ControllerCompleted ) || IsEqualGUID( iid, &IID_CaptureCompleted ))
    {
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI handler_AddRef( IUnknown *iface )
{
    struct handler *handler = (struct handler *)iface;
    return InterlockedIncrement( &handler->ref );
}

static ULONG WINAPI handler_Release( IUnknown *iface )
{
    struct handler *handler = (struct handler *)iface;
    ULONG ref = InterlockedDecrement( &handler->ref );
    if (!ref)
    {
        handler->destroy( handler );
        free( handler );
    }
    return ref;
}

struct capture_done
{
    struct handler handler;
    struct host *host;
    IStream *stream;
};

static void host_show_frame( struct host *host, IStream *stream )
{
    IWICImagingFactory *wic = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *source = NULL;
    IWICFormatConverter *conv = NULL;
    LARGE_INTEGER zero = {{0}};
    static int traced;

    if (GetEnvironmentVariableA( "WINE_UWP_WEBVIEW2_DUMP", NULL, 0 ))
    {
        /* the last frame as the runtime delivered it, for checking what the popup should show */
        HGLOBAL mem;
        STATSTG st;
        HANDLE file;
        DWORD written;

        if (SUCCEEDED(GetHGlobalFromStream( stream, &mem )) && SUCCEEDED(IStream_Stat( stream, &st, STATFLAG_NONAME )) &&
            (file = CreateFileW( L"C:\\users\\Public\\webview2-frame.png", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL )) != INVALID_HANDLE_VALUE)
        {
            WriteFile( file, GlobalLock( mem ), st.cbSize.LowPart, &written, NULL );
            GlobalUnlock( mem );
            CloseHandle( file );
        }
    }
    IStream_Seek( stream, zero, STREAM_SEEK_SET, NULL );
    if (SUCCEEDED(CoCreateInstance( &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic )) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream( wic, stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder )) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame( decoder, 0, &source )) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter( wic, &conv )) &&
        SUCCEEDED(IWICFormatConverter_Initialize( conv, (IWICBitmapSource *)source, &GUID_WICPixelFormat32bppBGRA,
                                                  WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom )))
    {
        BITMAPINFO bi = {{ sizeof(bi.bmiHeader) }};
        void *bits = NULL;
        HBITMAP bmp;
        UINT w, h;

        IWICFormatConverter_GetSize( conv, &w, &h );
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -(LONG)h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection( NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0 );
        if (bmp && SUCCEEDED(IWICFormatConverter_CopyPixels( conv, NULL, w * 4, w * 4 * h, bits )))
        {
            if (host->frame) DeleteObject( host->frame );
            host->frame = bmp;
            host->frame_w = w;
            host->frame_h = h;
            InvalidateRect( host->popup, NULL, FALSE );
            if (traced++ < 3) TRACE( "frame %ux%u\n", w, h );
        }
        else if (bmp) DeleteObject( bmp );
    }
    if (conv) IWICFormatConverter_Release( conv );
    if (source) IWICBitmapFrameDecode_Release( source );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    if (wic) IWICImagingFactory_Release( wic );
}

static HRESULT WINAPI capture_done_Invoke( IUnknown *iface, HRESULT hr )
{
    struct capture_done *done = (struct capture_done *)iface;
    static int traced;

    if (traced < 5 || FAILED(hr))
    {
        traced++;
        TRACE( "CapturePreview -> %#lx\n", hr );
    }
    done->host->capturing = FALSE;
    if (SUCCEEDED(hr) && !done->host->closed) host_show_frame( done->host, done->stream );
    return S_OK;
}

static void capture_done_destroy( struct handler *handler )
{
    struct capture_done *done = CONTAINING_RECORD( handler, struct capture_done, handler );
    IStream_Release( done->stream );
    host_release( done->host );
}

static void *capture_done_vtbl[] = { handler_QueryInterface, handler_AddRef, handler_Release, capture_done_Invoke };

static void host_capture( struct host *host )
{
    struct capture_done *done;
    IStream *stream;
    HRESULT hr;
    static int traced;

    if (host->capturing || host->closed || !host->visible || !IsWindowVisible( host->popup )) return;
    if (!host->webview && FAILED(CALL( fn_out, host->real, CONTROLLER_get_CoreWebView2, (void **)&host->webview ))) return;
    if (FAILED(CreateStreamOnHGlobal( NULL, TRUE, &stream ))) return;
    if (!(done = calloc( 1, sizeof(*done) )))
    {
        IStream_Release( stream );
        return;
    }
    done->handler.vtbl = capture_done_vtbl;
    done->handler.ref = 1;
    done->handler.destroy = capture_done_destroy;
    done->host = host;
    done->stream = stream;
    host_addref( host );
    host->capturing = TRUE;
    hr = CALL( fn_capture, host->webview, WEBVIEW_CapturePreview, 0 /* PNG */, stream, (IUnknown *)&done->handler );
    if (traced++ < 3 || FAILED(hr)) TRACE( "CapturePreview requested -> %#lx\n", hr );
    if (FAILED(hr)) host->capturing = FALSE;
    handler_Release( (IUnknown *)&done->handler );
}

static LRESULT CALLBACK popup_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct host *host = (struct host *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
    HWND browser;

    switch (msg)
    {
    case WM_TIMER:
        if (wp == 2) KillTimer( hwnd, 2 );
        if (host)
        {
            host_place( host );
            host_capture( host );
        }
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint( hwnd, &ps );
        RECT rect;

        GetClientRect( hwnd, &rect );
        if (host && host->frame)
        {
            HDC mem = CreateCompatibleDC( dc );
            HGDIOBJ old = SelectObject( mem, host->frame );
            BitBlt( dc, 0, 0, host->frame_w, host->frame_h, mem, 0, 0, SRCCOPY );
            SelectObject( mem, old );
            DeleteDC( mem );
        }
        else FillRect( dc, &rect, GetStockObject( WHITE_BRUSH ) );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_SETCURSOR:
        SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_ARROW ) );
        return TRUE;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        if (host && (browser = host_browser_window( host )))
        {
            if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) SetCapture( hwnd );
            if (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) ReleaseCapture();
            PostMessageW( browser, msg, wp, lp );
            SetTimer( hwnd, 2, 30, NULL );  /* a quick frame after input */
        }
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static void register_popup_class(void)
{
    static BOOL done;
    WNDCLASSW wc = {0};

    if (done) return;
    wc.lpfnWndProc = popup_proc;
    wc.hInstance = self_instance;
    wc.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    wc.lpszClassName = popup_class;
    RegisterClassW( &wc );
    done = TRUE;
}

/* ---- the environment ---- */

struct controller_created
{
    struct handler handler;
    struct host *host;
    IUnknown *app;  /* ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler */
};

static HRESULT WINAPI controller_created_Invoke( IUnknown *iface, HRESULT hr, IUnknown *controller )
{
    struct controller_created *created = (struct controller_created *)iface;
    struct host *host = created->host;

    TRACE( "windowed controller -> %#lx %p\n", hr, controller );
    if (FAILED(hr) || !controller) return CALL( fn_invoke, created->app, HANDLER_Invoke, FAILED(hr) ? hr : E_FAIL, NULL );
    /* every slot of the controller face forwards to it, so it has to be the newest revision we describe */
    if (FAILED(IUnknown_QueryInterface( controller, &IID_Controller4, (void **)&host->real )))
    {
        ERR( "runtime has no ICoreWebView2Controller4\n" );
        return CALL( fn_invoke, created->app, HANDLER_Invoke, E_NOINTERFACE, NULL );
    }
    host->controller.target = host->real;
    CALL( fn_bool, host->real, CONTROLLER_put_IsVisible, TRUE );
    SetTimer( host->popup, 1, 120, NULL );
    host_addref( host );
    hr = CALL( fn_invoke, created->app, HANDLER_Invoke, S_OK, (IUnknown *)&host->composition );
    host_release( host );
    return hr;
}

static void controller_created_destroy( struct handler *handler )
{
    struct controller_created *created = CONTAINING_RECORD( handler, struct controller_created, handler );
    IUnknown_Release( created->app );
    host_release( created->host );
}

static void *controller_created_vtbl[] = { handler_QueryInterface, handler_AddRef, handler_Release, controller_created_Invoke };

struct environment
{
    struct face face;   /* ICoreWebView2Environment10 over the runtime's */
    LONG ref;
    IUnknown *real;
};

static struct environment *environment_from_iface( IUnknown *iface ) { return CONTAINING_RECORD( iface, struct environment, face ); }

static HRESULT WINAPI env_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    struct environment *env = environment_from_iface( iface );
    HRESULT hr;
    int i;

    if (IsEqualGUID( iid, &IID_IUnknown ))
    {
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    for (i = 0; i < ARRAY_SIZE(IID_Environment); i++)
    {
        if (!IsEqualGUID( iid, &IID_Environment[i] )) continue;
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    hr = IUnknown_QueryInterface( env->real, iid, out );
    trace_iid( "environment", iid, hr );
    return hr;
}

static ULONG WINAPI env_AddRef( IUnknown *iface ) { return InterlockedIncrement( &environment_from_iface( iface )->ref ); }

static ULONG WINAPI env_Release( IUnknown *iface )
{
    struct environment *env = environment_from_iface( iface );
    ULONG ref = InterlockedDecrement( &env->ref );

    if (!ref)
    {
        IUnknown_Release( env->face.target );
        IUnknown_Release( env->real );
        free( env );
    }
    return ref;
}

static HRESULT WINAPI env_CreateCoreWebView2Controller( IUnknown *iface, HWND parent, IUnknown *handler )
{
    struct environment *env = environment_from_iface( iface );
    TRACE( "CreateCoreWebView2Controller %p\n", parent );
    return CALL( fn_create, env->face.target, ENVIRONMENT_CreateController, parent, handler );
}

static HRESULT create_composition( struct environment *env, HWND parent, IUnknown *options, IUnknown *handler )
{
    struct controller_created *created;
    struct host *host;
    HRESULT hr;

    TRACE( "composition controller for %p: a windowed controller on a popup\n", parent );
    if (!handler) return E_POINTER;
    register_popup_class();
    if (!(host = calloc( 1, sizeof(*host) ))) return E_OUTOFMEMORY;
    if (!(created = calloc( 1, sizeof(*created) )))
    {
        free( host );
        return E_OUTOFMEMORY;
    }
    host->composition.vtbl = composition_vtbl;
    host->controller.vtbl = controller_vtbl;
    host->ref = 1;
    host->visible = TRUE;
    host->owner = parent;
    host->popup = CreateWindowExW( WS_EX_TOOLWINDOW, popup_class, L"", WS_POPUP, 0, 0, 1, 1, parent, NULL, self_instance, NULL );
    SetWindowLongPtrW( host->popup, GWLP_USERDATA, (LONG_PTR)host );

    created->handler.vtbl = controller_created_vtbl;
    created->handler.ref = 1;
    created->handler.destroy = controller_created_destroy;
    created->host = host;   /* takes the reference */
    created->app = handler;
    IUnknown_AddRef( handler );

    if (options) hr = CALL( fn_create_opts, env->face.target, ENVIRONMENT_CreateControllerWithOptions, host->popup, options,
                            (IUnknown *)&created->handler );
    else hr = CALL( fn_create, env->face.target, ENVIRONMENT_CreateController, host->popup, (IUnknown *)&created->handler );
    handler_Release( (IUnknown *)&created->handler );
    return hr;
}

static HRESULT WINAPI env_CreateCoreWebView2CompositionController( IUnknown *iface, HWND parent, IUnknown *handler )
{
    return create_composition( environment_from_iface( iface ), parent, NULL, handler );
}

static HRESULT WINAPI env_CreateCoreWebView2CompositionControllerWithOptions( IUnknown *iface, HWND parent, IUnknown *options,
                                                                               IUnknown *handler )
{
    return create_composition( environment_from_iface( iface ), parent, options, handler );
}

static void *environment_vtbl[ENVIRONMENT_SLOTS];

static void init_environment_vtbl(void)
{
    memcpy( environment_vtbl, forwards, sizeof(environment_vtbl) );
    environment_vtbl[0] = env_QueryInterface;
    environment_vtbl[1] = env_AddRef;
    environment_vtbl[2] = env_Release;
    environment_vtbl[ENVIRONMENT_CreateController] = env_CreateCoreWebView2Controller;
    environment_vtbl[ENVIRONMENT_CreateCompositionController] = env_CreateCoreWebView2CompositionController;
    environment_vtbl[ENVIRONMENT_CreateCompositionControllerWithOptions] = env_CreateCoreWebView2CompositionControllerWithOptions;
}

struct environment_created
{
    struct handler handler;
    IUnknown *app;  /* ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler */
};

static HRESULT WINAPI environment_created_Invoke( IUnknown *iface, HRESULT hr, IUnknown *real )
{
    struct environment_created *created = (struct environment_created *)iface;
    struct environment *env;
    IUnknown *real10;

    TRACE( "environment -> %#lx %p\n", hr, real );
    if (FAILED(hr) || !real || VTBL(real) == environment_vtbl) return CALL( fn_invoke, created->app, HANDLER_Invoke, hr, real );
    if (FAILED(IUnknown_QueryInterface( real, &IID_Environment[9], (void **)&real10 )))
    {
        TRACE( "runtime has no ICoreWebView2Environment10; passing it through\n" );
        return CALL( fn_invoke, created->app, HANDLER_Invoke, hr, real );
    }
    if (!(env = calloc( 1, sizeof(*env) )))
    {
        IUnknown_Release( real10 );
        return CALL( fn_invoke, created->app, HANDLER_Invoke, E_OUTOFMEMORY, NULL );
    }
    env->face.vtbl = environment_vtbl;
    env->face.target = real10;
    env->ref = 1;
    env->real = real;
    IUnknown_AddRef( real );
    TRACE( "environment %p wrapped as %p (handler %p)\n", real, env, created->app );
    hr = CALL( fn_invoke, created->app, HANDLER_Invoke, S_OK, (IUnknown *)&env->face );
    TRACE( "app's environment handler -> %#lx\n", hr );
    env_Release( (IUnknown *)&env->face );
    return hr;
}

static void environment_created_destroy( struct handler *handler )
{
    struct environment_created *created = CONTAINING_RECORD( handler, struct environment_created, handler );
    IUnknown_Release( created->app );
}

static void *environment_created_vtbl[] = { handler_QueryInterface, handler_AddRef, handler_Release, environment_created_Invoke };

static const WCHAR extra_args[] = L"--disable-gpu --disable-gpu-compositing";

static void add_browser_arguments( IUnknown *options )
{
    IUnknown *opts;
    WCHAR *args = NULL, *all;
    size_t len;

    if (!options || FAILED(IUnknown_QueryInterface( options, &IID_EnvironmentOptions, (void **)&opts ))) return;
    if (FAILED(CALL( fn_out, opts, OPTIONS_get_AdditionalBrowserArguments, (void **)&args ))) args = NULL;
    if (args && wcsstr( args, L"--disable-gpu" ))
    {
        TRACE( "browser arguments: %s\n", debugstr_w( args ) );
        CoTaskMemFree( args );
        IUnknown_Release( opts );
        return;
    }
    len = (args ? wcslen( args ) + 1 : 0) + ARRAY_SIZE(extra_args);
    if ((all = malloc( len * sizeof(WCHAR) )))
    {
        all[0] = 0;
        if (args && *args)
        {
            wcscpy( all, args );
            wcscat( all, L" " );
        }
        wcscat( all, extra_args );
        CALL( fn_wstr, opts, OPTIONS_put_AdditionalBrowserArguments, all );
        TRACE( "browser arguments: %s\n", debugstr_w( all ) );
        free( all );
    }
    CoTaskMemFree( args );
    IUnknown_Release( opts );
}

typedef HRESULT (WINAPI *create_environment_fn)( UINT_PTR, UINT_PTR, const WCHAR *, IUnknown *, IUnknown * );

HRESULT WINAPI CreateWebViewEnvironmentWithOptionsInternal( UINT_PTR check_running, UINT_PTR runtime_type, const WCHAR *user_data,
                                                            IUnknown *options, IUnknown *handler )
{
    struct environment_created *created;
    create_environment_fn create;
    HRESULT hr;

    if (!real_module)
    {
        WCHAR path[MAX_PATH], *slash;

        GetModuleFileNameW( self_instance, path, MAX_PATH );
        if ((slash = wcsrchr( path, '\\' ))) wcscpy( slash + 1, L"EmbeddedBrowserWebView.real.dll" );
        real_module = LoadLibraryW( path );
        TRACE( "real runtime %s -> %p\n", debugstr_w( path ), real_module );
    }
    if (!real_module || !(create = (void *)GetProcAddress( real_module, "CreateWebViewEnvironmentWithOptionsInternal" )))
        return HRESULT_FROM_WIN32( ERROR_MOD_NOT_FOUND );
    if (!handler) return E_POINTER;
    if (!(created = calloc( 1, sizeof(*created) ))) return E_OUTOFMEMORY;

    init_controller_vtbl();
    init_environment_vtbl();
    add_browser_arguments( options );
    SetEnvironmentVariableW( L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", extra_args );
    created->handler.vtbl = environment_created_vtbl;
    created->handler.ref = 1;
    created->handler.destroy = environment_created_destroy;
    created->app = handler;
    IUnknown_AddRef( handler );
    hr = create( check_running, runtime_type, user_data, options, (IUnknown *)&created->handler );
    handler_Release( (IUnknown *)&created->handler );
    TRACE( "CreateWebViewEnvironmentWithOptionsInternal -> %#lx\n", hr );
    return hr;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        HMODULE pinned;

        self_instance = instance;
        DisableThreadLibraryCalls( instance );
        /* the loader in Microsoft.Web.WebView2.Core.dll frees the runtime DLL once the environment exists; the
         * objects handed out live here */
        GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (const WCHAR *)DllMain, &pinned );
    }
    return TRUE;
}

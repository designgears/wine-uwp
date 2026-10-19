/*
 * Window client adapters for hosted CoreWindows
 *
 * Copyright 2026 the Wine project authors
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

/* On Windows the shell gives every CoreWindow a window client adapter (CoreUIComponents'
 * NavigationClientWindowClientAdapter), installed with ICoreWindowAdapterInterop::SetWindowClientAdapter.
 * Windows.UI's WindowServer hands its sub-adapters to twinapi.appcore: without one, everything in
 * ApplicationView that asks the shell fails with ERROR_NOT_FOUND. The Microsoft Store awaits
 * ApplicationView.TryEnterViewModeAsync(Default) at start-up and died on the null operation that failure
 * left it.
 *
 * Slot order is CoreUIComponents' shipping vtables, named from its public PDB:
 *   IWindowClientAdapter {6d015416} (??_7NavigationClientWindowClientAdapter, 73 slots, IUnknown-based)
 *   IApplicationViewClientAdapter {f02ee4c8} (??_7NavigationClientApplicationViewClientAdapter, 34 slots)
 *   ICoreApplicationViewClientAdapter {c86d297c} (??_7NavigationClientCoreApplicationViewClientAdapter, 8 slots)
 * WindowServer itself only reads the sub-adapter getters (3, 4, 6, 7, 9, 27, 32, 35), registers five events
 * (50, 62, 66, 68, 70) and calls AddSetWindowPosFlags (39). Events are accepted and never raised, so the
 * window's sizes and visibility keep coming from where they come from now. Getters for adapters we do not
 * have answer NULL, which is what WindowServer saw before there was any adapter. Everything else is logged
 * and refused, so the next caller says what it needs. */

#include <stdarg.h>
#include <stdlib.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "objbase.h"
#include "inspectable.h"
#include "eventtoken.h"
#include "initguid.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wineuwp);

DEFINE_GUID(IID_IWindowClientAdapter_, 0x6d015416, 0x85e6, 0x48c2, 0x8b, 0x21, 0x66, 0xb2, 0xbe, 0x16, 0x6a, 0xeb);
DEFINE_GUID(IID_IApplicationViewClientAdapter_, 0xf02ee4c8, 0x92b3, 0x48c7, 0x9b, 0x48, 0x3d, 0x0a, 0x0d, 0x2f, 0x1b, 0xf4);
DEFINE_GUID(IID_ICoreApplicationViewClientAdapter_, 0xc86d297c, 0x6302, 0x41aa, 0x9d, 0x05, 0x72, 0x41, 0x29, 0x61, 0x1e, 0x32);
DEFINE_GUID(IID_ICoreWindowAdapterInterop_, 0x7a5b6fd1, 0xcd73, 0x4b6c, 0x9c, 0xf4, 0x2e, 0x86, 0x9e, 0xaf, 0x47, 0x0a);
DEFINE_GUID(IID_IAsyncOperation_boolean_, 0xcdb5efb3, 0x5788, 0x509d, 0x9b, 0xe1, 0x71, 0xcc, 0xb8, 0xa3, 0x36, 0x2a);
DEFINE_GUID(IID_IAsyncInfo_, 0x00000036, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(IID_IAgileObject_, 0x94ea2b94, 0xe9cc, 0x49e0, 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90);

static LONG next_token;

/* ---- an IAsyncOperation<bool> that is born completed ---- */
struct done_bool
{
    const void **vtbl_op;
    const void **vtbl_info;
    LONG refcount;
    boolean result;
    IUnknown *handler;
};

static struct done_bool *bool_from_op( void *iface ) { return iface; }
static struct done_bool *bool_from_info( void *iface ) { return (struct done_bool *)((char *)iface - sizeof(void *)); }

static HRESULT done_bool_qi( struct done_bool *op, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject_ ) || IsEqualGUID( iid, &IID_IAsyncOperation_boolean_ ))
        *out = &op->vtbl_op;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo_ )) *out = &op->vtbl_info;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement( &op->refcount );
    return S_OK;
}
static ULONG done_bool_release( struct done_bool *op )
{
    ULONG ref = InterlockedDecrement( &op->refcount );
    if (!ref)
    {
        if (op->handler) IUnknown_Release( op->handler );
        free( op );
    }
    return ref;
}
static HRESULT WINAPI op_qi( void *i, REFIID iid, void **out ) { return done_bool_qi( bool_from_op( i ), iid, out ); }
static ULONG WINAPI op_addref( void *i ) { return InterlockedIncrement( &bool_from_op( i )->refcount ); }
static ULONG WINAPI op_release( void *i ) { return done_bool_release( bool_from_op( i ) ); }
static HRESULT WINAPI op_get_iids( void *i, ULONG *count, IID **iids ) { *count = 0; *iids = NULL; return S_OK; }
static HRESULT WINAPI op_get_name( void *i, HSTRING *name )
{
    static const WCHAR str[] = L"Windows.Foundation.IAsyncOperation`1<Boolean>";
    return WindowsCreateString( str, ARRAY_SIZE(str) - 1, name );
}
static HRESULT WINAPI op_get_trust( void *i, TrustLevel *level ) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI op_put_completed( void *iface, IUnknown *handler )
{
    struct done_bool *op = bool_from_op( iface );
    HRESULT (WINAPI *invoke)( IUnknown *, void *, int );

    if (op->handler) IUnknown_Release( op->handler );
    if (!(op->handler = handler)) return S_OK;
    IUnknown_AddRef( handler );
    /* a delegate: Invoke is slot 3; AsyncStatus::Completed is 1 */
    invoke = (void *)(*(void ***)handler)[3];
    return invoke( handler, &op->vtbl_op, 1 );
}
static HRESULT WINAPI op_get_completed( void *iface, IUnknown **handler )
{
    struct done_bool *op = bool_from_op( iface );
    if ((*handler = op->handler)) IUnknown_AddRef( op->handler );
    return S_OK;
}
static HRESULT WINAPI op_get_results( void *iface, boolean *result ) { *result = bool_from_op( iface )->result; return S_OK; }
static const void *done_bool_op_vtbl[] =
{
    op_qi, op_addref, op_release, op_get_iids, op_get_name, op_get_trust,
    op_put_completed, op_get_completed, op_get_results,
};

static HRESULT WINAPI info_qi( void *i, REFIID iid, void **out ) { return done_bool_qi( bool_from_info( i ), iid, out ); }
static ULONG WINAPI info_addref( void *i ) { return InterlockedIncrement( &bool_from_info( i )->refcount ); }
static ULONG WINAPI info_release( void *i ) { return done_bool_release( bool_from_info( i ) ); }
static HRESULT WINAPI info_get_id( void *i, UINT32 *id ) { *id = 1; return S_OK; }
static HRESULT WINAPI info_get_status( void *i, int *status ) { *status = 1; return S_OK; }
static HRESULT WINAPI info_get_error( void *i, HRESULT *error ) { *error = S_OK; return S_OK; }
static HRESULT WINAPI info_cancel( void *i ) { return S_OK; }
static HRESULT WINAPI info_close( void *i ) { return S_OK; }
static const void *done_bool_info_vtbl[] =
{
    info_qi, info_addref, info_release, op_get_iids, op_get_name, op_get_trust,
    info_get_id, info_get_status, info_get_error, info_cancel, info_close,
};

static HRESULT done_bool_create( boolean result, void **out )
{
    struct done_bool *op;

    if (!(op = calloc( 1, sizeof(*op) ))) return E_OUTOFMEMORY;
    op->vtbl_op = done_bool_op_vtbl;
    op->vtbl_info = done_bool_info_vtbl;
    op->refcount = 1;
    op->result = result;
    *out = &op->vtbl_op;
    return S_OK;
}

/* ---- the adapters ---- */
#define WINDOW_SLOTS 73
#define VIEW_SLOTS   33
#define CORE_SLOTS   7

struct window_adapter
{
    const void **vtbl_window;            /* IWindowClientAdapter */
    const void **vtbl_view;              /* IApplicationViewClientAdapter */
    const void **vtbl_core;              /* ICoreApplicationViewClientAdapter */
    LONG refcount;
    IUnknown *ftm;

    HWND hwnd;
    HSTRING title;
    boolean suppress_overlays, multiview_policy;
    INT32 launch_windowing_mode;
    struct view_size { float width, height; } launch_size, min_size;
};

static struct window_adapter *from_window( void *iface ) { return CONTAINING_RECORD( iface, struct window_adapter, vtbl_window ); }
static struct window_adapter *from_view( void *iface ) { return CONTAINING_RECORD( iface, struct window_adapter, vtbl_view ); }
static struct window_adapter *from_core( void *iface ) { return CONTAINING_RECORD( iface, struct window_adapter, vtbl_core ); }

static HRESULT adapter_qi( struct window_adapter *adapter, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IWindowClientAdapter_ ))
        *out = &adapter->vtbl_window;
    else if (IsEqualGUID( iid, &IID_IApplicationViewClientAdapter_ )) *out = &adapter->vtbl_view;
    else if (IsEqualGUID( iid, &IID_ICoreApplicationViewClientAdapter_ )) *out = &adapter->vtbl_core;
    else if (IsEqualGUID( iid, &IID_IMarshal ) || IsEqualGUID( iid, &IID_IAgileObject_ ))
        return IUnknown_QueryInterface( adapter->ftm, iid, out );
    else
    {
        TRACE( "window client adapter: no %s\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement( &adapter->refcount );
    return S_OK;
}
static ULONG adapter_release( struct window_adapter *adapter )
{
    ULONG ref = InterlockedDecrement( &adapter->refcount );
    if (!ref)
    {
        WindowsDeleteString( adapter->title );
        IUnknown_Release( adapter->ftm );
        free( adapter );
    }
    return ref;
}

/* every slot not implemented below: logged with its CoreUIComponents name and refused */
#define REFUSED(prefix, n, name) \
    static HRESULT WINAPI prefix##_slot##n( void *iface ) { FIXME( #name ": not implemented\n" ); return E_NOTIMPL; }

/* IWindowClientAdapter */
static HRESULT WINAPI wca_qi( void *i, REFIID iid, void **out ) { return adapter_qi( from_window( i ), iid, out ); }
static ULONG WINAPI wca_addref( void *i ) { return InterlockedIncrement( &from_window( i )->refcount ); }
static ULONG WINAPI wca_release( void *i ) { return adapter_release( from_window( i ) ); }
static HRESULT WINAPI wca_null( void *iface, IUnknown **out ) { *out = NULL; return S_OK; }
static HRESULT WINAPI wca_get_ApplicationViewClientAdapter( void *iface, IUnknown **out )
{
    struct window_adapter *adapter = from_window( iface );
    *out = (IUnknown *)&adapter->vtbl_view;
    InterlockedIncrement( &adapter->refcount );
    return S_OK;
}
static HRESULT WINAPI wca_get_CoreApplicationViewClientAdapter( void *iface, IUnknown **out )
{
    struct window_adapter *adapter = from_window( iface );
    *out = (IUnknown *)&adapter->vtbl_core;
    InterlockedIncrement( &adapter->refcount );
    return S_OK;
}
static HRESULT WINAPI wca_add_event( void *iface, IUnknown *handler, EventRegistrationToken *token )
{
    token->value = InterlockedIncrement( &next_token );
    return S_OK;
}
static HRESULT WINAPI wca_remove_event( void *iface, EventRegistrationToken token ) { return S_OK; }
static HRESULT WINAPI wca_window_pos_flags( void *iface, UINT32 flags ) { return S_OK; }

/* the window's client area in screen DIPs, which is what CoreWindow.Bounds and ApplicationView.VisibleBounds
 * report on Windows: no system overlays cover a desktop window here, so visible bounds are the whole client area */
struct view_rect { float x, y, width, height; };
static BOOL client_dips( struct window_adapter *adapter, struct view_rect *rect )
{
    POINT origin = { 0, 0 };
    float scale;
    RECT rc;

    if (!adapter->hwnd || !GetClientRect( adapter->hwnd, &rc )) return FALSE;
    ClientToScreen( adapter->hwnd, &origin );
    scale = GetDpiForWindow( adapter->hwnd ) / 96.0f;
    if (scale <= 0) scale = 1.0f;
    rect->x = origin.x / scale;
    rect->y = origin.y / scale;
    rect->width = (rc.right - rc.left) / scale;
    rect->height = (rc.bottom - rc.top) / scale;
    return TRUE;
}
static HRESULT WINAPI wca_get_VisibleBounds( void *iface, struct view_rect *value )
{
    if (!client_dips( from_window( iface ), value )) return E_FAIL;
    TRACE( "%.1f,%.1f %.1fx%.1f\n", value->x, value->y, value->width, value->height );
    return S_OK;
}
static HRESULT WINAPI wca_get_LogicalSize( void *iface, struct view_size *value )
{
    struct view_rect rect;
    if (!client_dips( from_window( iface ), &rect )) return E_FAIL;
    value->width = rect.width;
    value->height = rect.height;
    return S_OK;
}
static HRESULT WINAPI wca_get_IsVisible( void *iface, boolean *value )
{
    struct window_adapter *adapter = from_window( iface );
    *value = adapter->hwnd && IsWindowVisible( adapter->hwnd );
    return S_OK;
}
static HRESULT WINAPI wca_get_IsMinimized( void *iface, boolean *value )
{
    struct window_adapter *adapter = from_window( iface );
    *value = adapter->hwnd && IsIconic( adapter->hwnd );
    return S_OK;
}
static HRESULT WINAPI wca_false( void *iface, boolean *value ) { *value = FALSE; return S_OK; }
/* notifications from the owner and from layout: nothing to report back */
static HRESULT WINAPI wca_notified( void *iface ) { return S_OK; }

REFUSED(wca, 5, get_AppViewId) REFUSED(wca, 10, get_Id) 
REFUSED(wca, 12, get_IsScreenCaptureEnabled) REFUSED(wca, 13, put_IsScreenCaptureEnabled) 
  
REFUSED(wca, 18, get_Occlusion) REFUSED(wca, 19, get_Placement) REFUSED(wca, 20, get_PreferredMaxSize)
REFUSED(wca, 21, put_PreferredMaxSize) REFUSED(wca, 22, get_PreferredMinSize) REFUSED(wca, 23, put_PreferredMinSize)
REFUSED(wca, 24, get_PreferredScale) REFUSED(wca, 25, put_PreferredScale) REFUSED(wca, 26, get_PhysicalBounds)
REFUSED(wca, 29, get_SetWindowPosFlags) REFUSED(wca, 30, get_ShouldWaitForLayoutCompletion)
REFUSED(wca, 31, put_ShouldWaitForLayoutCompletion) REFUSED(wca, 33, get_Title) REFUSED(wca, 34, put_Title)
 REFUSED(wca, 37, get_PrimaryDisplayRegion) REFUSED(wca, 41, Consolidate)
REFUSED(wca, 42, SetOwner)  
  REFUSED(wca, 47, TryShowAsync)
REFUSED(wca, 72, Hide)

static const void *window_vtbl[WINDOW_SLOTS] =
{
    wca_qi, wca_addref, wca_release,
    /*  3 */ wca_null,                                  /* get_AppActivationClientAdapter */
    /*  4 */ wca_get_ApplicationViewClientAdapter,
    /*  5 */ wca_slot5,
    /*  6 */ wca_get_CoreApplicationViewClientAdapter,
    /*  7 */ wca_null,                                  /* get_DisplayClientAdapter */
    /*  8 */ wca_null,                                  /* get_FrameClientAdapter */
    /*  9 */ wca_null,                                  /* get_HoloViewClientAdapter */
    /* 10 */ wca_slot10, wca_get_IsMinimized, wca_slot12, wca_slot13, wca_get_IsVisible, wca_false, wca_false, wca_get_LogicalSize,
    /* 18 */ wca_slot18, wca_slot19, wca_slot20, wca_slot21, wca_slot22, wca_slot23, wca_slot24, wca_slot25,
    /* 26 */ wca_slot26,
    /* 27 */ wca_null,                                  /* get_PositionerClientAdapter */
    /* 28 */ wca_null,                                  /* get_PresenterClientAdapter */
    /* 29 */ wca_slot29, wca_slot30, wca_slot31,
    /* 32 */ wca_null,                                  /* get_SystemNavigationClientAdapter */
    /* 33 */ wca_slot33, wca_slot34,
    /* 35 */ wca_null,                                  /* get_TitleBarClientAdapter */
    /* 36 */ wca_get_VisibleBounds, wca_slot37,
    /* 38 */ wca_null,                                  /* get_AppWindowProxiedIdentityClientAdapter */
    /* 39 */ wca_window_pos_flags, wca_window_pos_flags, /* Add/RemoveSetWindowPosFlags */
    /* 41 */ wca_slot41, wca_slot42, wca_notified, wca_notified, wca_notified, wca_notified, wca_slot47,
    /* 48 */ wca_add_event, wca_remove_event,            /* CloakChanged */
    /* 50 */ wca_add_event, wca_remove_event,            /* Closed */
    /* 52 */ wca_add_event, wca_remove_event,            /* Consolidated */
    /* 54 */ wca_add_event, wca_remove_event,            /* FrameSizeChanged */
    /* 56 */ wca_add_event, wca_remove_event,            /* TopMostChanged */
    /* 58 */ wca_add_event, wca_remove_event,            /* FrameOffsetChanged */
    /* 60 */ wca_add_event, wca_remove_event,            /* IsMinimizedChanged */
    /* 62 */ wca_add_event, wca_remove_event,            /* LogicalSizeChanged */
    /* 64 */ wca_add_event, wca_remove_event,            /* OcclusionChanged */
    /* 66 */ wca_add_event, wca_remove_event,            /* PhysicalBoundsChanged */
    /* 68 */ wca_add_event, wca_remove_event,            /* VisibleBoundsChanged */
    /* 70 */ wca_add_event, wca_remove_event,            /* VisibilityChanged */
    /* 72 */ wca_slot72,
};

/* IApplicationViewClientAdapter */
static HRESULT WINAPI avca_qi( void *i, REFIID iid, void **out ) { return adapter_qi( from_view( i ), iid, out ); }
static ULONG WINAPI avca_addref( void *i ) { return InterlockedIncrement( &from_view( i )->refcount ); }
static ULONG WINAPI avca_release( void *i ) { return adapter_release( from_view( i ) ); }
static HRESULT WINAPI avca_zero( void *iface, INT32 *value ) { *value = 0; return S_OK; }
static HRESULT WINAPI avca_false( void *iface, boolean *value ) { *value = FALSE; return S_OK; }
static HRESULT WINAPI avca_get_multiview( void *iface, boolean *value ) { *value = from_view( iface )->multiview_policy; return S_OK; }
static HRESULT WINAPI avca_put_multiview( void *iface, boolean value ) { from_view( iface )->multiview_policy = value; return S_OK; }
static HRESULT WINAPI avca_get_is_full_screen( void *iface, boolean *value )
{
    struct window_adapter *adapter = from_view( iface );
    *value = adapter->hwnd && !(GetWindowLongW( adapter->hwnd, GWL_STYLE ) & WS_CAPTION);
    return S_OK;
}
static HRESULT WINAPI avca_get_launch_mode( void *iface, INT32 *value ) { *value = from_view( iface )->launch_windowing_mode; return S_OK; }
static HRESULT WINAPI avca_put_launch_mode( void *iface, INT32 value ) { from_view( iface )->launch_windowing_mode = value; return S_OK; }
static HRESULT WINAPI avca_get_launch_size( void *iface, void *size ) { memcpy( size, &from_view( iface )->launch_size, sizeof(from_view( iface )->launch_size) ); return S_OK; }
/* Windows.Foundation.Size is eight bytes, passed by value */
static HRESULT WINAPI avca_put_launch_size( void *iface, struct view_size size ) { from_view( iface )->launch_size = size; return S_OK; }
static HRESULT WINAPI avca_get_min_size( void *iface, void *size ) { memcpy( size, &from_view( iface )->min_size, sizeof(from_view( iface )->min_size) ); return S_OK; }
static HRESULT WINAPI avca_put_min_size( void *iface, struct view_size size ) { from_view( iface )->min_size = size; return S_OK; }
static HRESULT WINAPI avca_get_suppress( void *iface, boolean *value ) { *value = from_view( iface )->suppress_overlays; return S_OK; }
static HRESULT WINAPI avca_put_suppress( void *iface, boolean value ) { from_view( iface )->suppress_overlays = value; return S_OK; }
static HRESULT WINAPI avca_get_title( void *iface, HSTRING *value ) { return WindowsDuplicateString( from_view( iface )->title, value ); }
static HRESULT WINAPI avca_put_title( void *iface, HSTRING value )
{
    struct window_adapter *adapter = from_view( iface );
    WindowsDeleteString( adapter->title );
    return WindowsDuplicateString( value, &adapter->title );
}
static HRESULT WINAPI avca_ok( void *iface ) { return S_OK; }
/* ApplicationViewMode: Default 0, CompactOverlay 1. A plain window is always in Default. */
static HRESULT WINAPI avca_is_view_mode_supported( void *iface, INT32 mode, boolean *value ) { *value = !mode; return S_OK; }
/* twinapi.appcore's CApplicationView::_TryEnterViewModeHelperAsync passes ( ~view id, mode, preferences, result ) */
static HRESULT WINAPI avca_try_show_as_view_mode( void *iface, INT32 view, INT32 mode, IUnknown *preferences, void **operation )
{
    TRACE( "view %d, mode %d\n", ~view, mode );
    return done_bool_create( !mode, operation );
}

REFUSED(avca, 17, get_SpanningRects) REFUSED(avca, 25, ShowAsStandaloneViewAsync) REFUSED(avca, 27, SwitchViewAsync)
REFUSED(avca, 28, TryConsolidateAsync) REFUSED(avca, 29, TryShowAsStandaloneViewAsync)

static const void *view_vtbl[VIEW_SLOTS] =
{
    avca_qi, avca_addref, avca_release,
    /*  3 */ avca_zero,                 /* get_ApplicationViewMode: Default */
    /*  4 */ avca_zero,                 /* get_ApplicationViewState */
    /*  5 */ avca_false, avca_false,    /* get_AdjacentToLeft/RightDisplayEdge */
    /*  7 */ avca_get_multiview, avca_put_multiview,
    /*  9 */ avca_false,                /* get_IsOnLockScreen */
    /* 10 */ avca_get_is_full_screen,
    /* 11 */ avca_get_launch_mode, avca_put_launch_mode,
    /* 13 */ avca_get_launch_size, avca_put_launch_size,
    /* 15 */ avca_get_min_size, avca_put_min_size,
    /* 17 */ avca_slot17,
    /* 18 */ avca_get_suppress, avca_put_suppress,
    /* 20 */ avca_get_title, avca_put_title,
    /* 22 */ avca_zero,                 /* get_UserInteractionMode: Mouse */
    /* 23 */ avca_ok,                   /* DisableResponsiveAppViewPolicy */
    /* 24 */ avca_is_view_mode_supported,
    /* 25 */ avca_slot25,
    /* 26 */ avca_ok,                   /* ShowStandardSystemOverlays */
    /* 27 */ avca_slot27, avca_slot28, avca_slot29,
    /* 30 */ avca_try_show_as_view_mode,
    /* 31 */ wca_add_event, wca_remove_event,   /* StateChanged */
};

/* ICoreApplicationViewClientAdapter: PresentationLoading / PresentationUnloaded */
static HRESULT WINAPI cavca_qi( void *i, REFIID iid, void **out ) { return adapter_qi( from_core( i ), iid, out ); }
static ULONG WINAPI cavca_addref( void *i ) { return InterlockedIncrement( &from_core( i )->refcount ); }
static ULONG WINAPI cavca_release( void *i ) { return adapter_release( from_core( i ) ); }
static const void *core_vtbl[CORE_SLOTS] =
{
    cavca_qi, cavca_addref, cavca_release,
    wca_add_event, wca_remove_event, wca_add_event, wca_remove_event,
};

/* Give a new CoreWindow its client adapter, as the shell's navigation client does on Windows. */
HRESULT attach_window_client_adapter( IUnknown *window, HWND hwnd )
{
    struct interop_vtbl
    {
        void *unknown_inspectable[6];
        void *getters[7];
        HRESULT (WINAPI *SetWindowClientAdapter)( void *iface, IUnknown *adapter );
    };
    struct interop { const struct interop_vtbl *vtbl; } *interop;
    struct window_adapter *adapter;
    HRESULT hr;

    if (FAILED( hr = IUnknown_QueryInterface( window, &IID_ICoreWindowAdapterInterop_, (void **)&interop ) ))
    {
        WARN( "CoreWindow %p has no ICoreWindowAdapterInterop, hr %#lx\n", window, hr );
        return hr;
    }
    if (!(adapter = calloc( 1, sizeof(*adapter) )))
    {
        IUnknown_Release( (IUnknown *)interop );
        return E_OUTOFMEMORY;
    }
    adapter->vtbl_window = window_vtbl;
    adapter->vtbl_view = view_vtbl;
    adapter->vtbl_core = core_vtbl;
    adapter->refcount = 1;
    adapter->hwnd = hwnd;
    if (FAILED( hr = CoCreateFreeThreadedMarshaler( (IUnknown *)&adapter->vtbl_window, &adapter->ftm ) ))
    {
        free( adapter );
        IUnknown_Release( (IUnknown *)interop );
        return hr;
    }
    hr = interop->vtbl->SetWindowClientAdapter( interop, (IUnknown *)&adapter->vtbl_window );
    TRACE( "window client adapter %p on %p, hr %#lx\n", adapter, window, hr );
    IUnknown_Release( (IUnknown *)&adapter->vtbl_window );
    IUnknown_Release( (IUnknown *)interop );
    return hr;
}

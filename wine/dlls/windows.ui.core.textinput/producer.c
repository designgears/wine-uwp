/* Windows.UI.Core.TextInput's text input producer (ordinal 1500, PrivateCreateTextInputProducer)
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

/* A CoreWindow hands its keys to the producer it was given through ITextInputConsumer::put_TextInputProducer
 * (Windows.UI keeps it at WindowServer+0x168). With one, WindowServer::OnKeyDownEvent/OnKeyUpEvent only ask the
 * producer whether the key was handled (get_MessageHandled): the producer has already seen the message and raised
 * KeyDown/KeyUp through the consumer, with args of its own. Without one Windows.UI raises the events itself with
 * a CKeyEventArgs whose Handled starts TRUE, so every CoreWindow.KeyDown handler ignored Enter, Backspace and
 * Escape. Windows' producer sits on TSF; this one watches the window's key messages with a WH_GETMESSAGE hook, as
 * they are taken off the queue and before they are dispatched, which is when TSF sees them too. Characters are not
 * the producer's business: WindowServer::OnCharacterEvent raises CharacterReceived from WM_CHAR either way. */

#define WIDL_using_Windows_System
#include "private.h"
#include "winuser.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(textinput);

/* private interfaces, slots from the shipping vtables (Windows.UI.pdb, Windows.UI.Core.TextInput.pdb) */
typedef struct text_input_consumer text_input_consumer;
struct text_input_consumer_vtbl
{
    HRESULT (WINAPI *QueryInterface)( text_input_consumer *, REFIID, void ** );
    ULONG (WINAPI *AddRef)( text_input_consumer * );
    ULONG (WINAPI *Release)( text_input_consumer * );
    void *GetIids, *GetRuntimeClassName, *GetTrustLevel;
    void *get_TextInputProducer, *put_TextInputProducer;
    HRESULT (WINAPI *InvokeAcceleratorKeyEventHandlers)( text_input_consumer *, IInspectable * );
    HRESULT (WINAPI *InvokeKeyDownEventHandlers)( text_input_consumer *, IKeyEventArgs * );
    HRESULT (WINAPI *InvokeKeyUpEventHandlers)( text_input_consumer *, IKeyEventArgs * );
    HRESULT (WINAPI *InvokeCharacterReceivedEventHandlers)( text_input_consumer *, IInspectable * );
    HRESULT (WINAPI *InvokeSystemKeyDownEventHandlers)( text_input_consumer *, IKeyEventArgs * );
    HRESULT (WINAPI *InvokeSystemKeyUpEventHandlers)( text_input_consumer *, IKeyEventArgs * );
};
struct text_input_consumer { const struct text_input_consumer_vtbl *lpVtbl; };

static const GUID IID_ITextInputConsumer_local = {0xa9d00ab3, 0x2fef, 0x41a0, {0xb0, 0xad, 0x4b, 0x21, 0x29, 0xea, 0x26, 0x63}};
static const GUID IID_ITextInputProducer_local = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
/* put_TextInputProducer also takes this one, and drops the producer again if it is refused; every method of it
 * is E_NOTIMPL in Windows' own producer on the desktop */
static const GUID IID_ITextInputProducerPhone_local = {0x400ae3c0, 0xbf20, 0x44ce, {0xa4, 0x2f, 0x09, 0xb7, 0x44, 0xd9, 0xfe, 0x88}};
static const GUID IID_ICoreWindowInterop_local = {0x45d64a29, 0xa63e, 0x4cb6, {0xb4, 0x98, 0x57, 0x81, 0xd2, 0x98, 0xcb, 0x4f}};

typedef struct core_window_interop core_window_interop;
struct core_window_interop_vtbl
{
    HRESULT (WINAPI *QueryInterface)( core_window_interop *, REFIID, void ** );
    ULONG (WINAPI *AddRef)( core_window_interop * );
    ULONG (WINAPI *Release)( core_window_interop * );
    HRESULT (WINAPI *get_WindowHandle)( core_window_interop *, HWND * );
};
struct core_window_interop { const struct core_window_interop_vtbl *lpVtbl; };

/* the key's KeyEventArgs: IKeyEventArgs, IKeyEventArgs2 and ICoreWindowEventArgs */
struct key_args
{
    IKeyEventArgs IKeyEventArgs_iface;
    IKeyEventArgs2 IKeyEventArgs2_iface;
    ICoreWindowEventArgs ICoreWindowEventArgs_iface;
    LONG ref;
    VirtualKey key;
    CorePhysicalKeyStatus status;
    boolean handled;
};

static inline struct key_args *key_args_from_IKeyEventArgs( IKeyEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct key_args, IKeyEventArgs_iface );
}

static HRESULT WINAPI key_args_QueryInterface( IKeyEventArgs *iface, REFIID iid, void **out )
{
    struct key_args *impl = key_args_from_IKeyEventArgs( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IKeyEventArgs ))
        *out = &impl->IKeyEventArgs_iface;
    else if (IsEqualGUID( iid, &IID_IKeyEventArgs2 ))
        *out = &impl->IKeyEventArgs2_iface;
    else if (IsEqualGUID( iid, &IID_ICoreWindowEventArgs ))
        *out = &impl->ICoreWindowEventArgs_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI key_args_AddRef( IKeyEventArgs *iface )
{
    struct key_args *impl = key_args_from_IKeyEventArgs( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI key_args_Release( IKeyEventArgs *iface )
{
    struct key_args *impl = key_args_from_IKeyEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI key_args_GetIids( IKeyEventArgs *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI key_args_GetRuntimeClassName( IKeyEventArgs *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.UI.Core.KeyEventArgs";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI key_args_GetTrustLevel( IKeyEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI key_args_get_VirtualKey( IKeyEventArgs *iface, VirtualKey *value )
{
    *value = key_args_from_IKeyEventArgs( iface )->key;
    return S_OK;
}

static HRESULT WINAPI key_args_get_KeyStatus( IKeyEventArgs *iface, CorePhysicalKeyStatus *value )
{
    *value = key_args_from_IKeyEventArgs( iface )->status;
    return S_OK;
}

static const struct IKeyEventArgsVtbl key_args_vtbl =
{
    key_args_QueryInterface,
    key_args_AddRef,
    key_args_Release,
    key_args_GetIids,
    key_args_GetRuntimeClassName,
    key_args_GetTrustLevel,
    key_args_get_VirtualKey,
    key_args_get_KeyStatus,
};

#define KEY_ARGS_FORWARD( type, name ) \
static inline struct key_args *key_args_from_##type( type *iface ) \
{ \
    return CONTAINING_RECORD( iface, struct key_args, type##_iface ); \
} \
static HRESULT WINAPI name##_QueryInterface( type *iface, REFIID iid, void **out ) \
{ \
    return key_args_QueryInterface( &key_args_from_##type( iface )->IKeyEventArgs_iface, iid, out ); \
} \
static ULONG WINAPI name##_AddRef( type *iface ) \
{ \
    return key_args_AddRef( &key_args_from_##type( iface )->IKeyEventArgs_iface ); \
} \
static ULONG WINAPI name##_Release( type *iface ) \
{ \
    return key_args_Release( &key_args_from_##type( iface )->IKeyEventArgs_iface ); \
} \
static HRESULT WINAPI name##_GetIids( type *iface, ULONG *count, IID **iids ) \
{ \
    return E_NOTIMPL; \
} \
static HRESULT WINAPI name##_GetRuntimeClassName( type *iface, HSTRING *class_name ) \
{ \
    return key_args_GetRuntimeClassName( &key_args_from_##type( iface )->IKeyEventArgs_iface, class_name ); \
} \
static HRESULT WINAPI name##_GetTrustLevel( type *iface, TrustLevel *level ) \
{ \
    *level = BaseTrust; \
    return S_OK; \
}

KEY_ARGS_FORWARD( IKeyEventArgs2, key_args2 )

static HRESULT WINAPI key_args2_get_DeviceId( IKeyEventArgs2 *iface, HSTRING *value )
{
    *value = NULL;
    return S_OK;
}

static const struct IKeyEventArgs2Vtbl key_args2_vtbl =
{
    key_args2_QueryInterface,
    key_args2_AddRef,
    key_args2_Release,
    key_args2_GetIids,
    key_args2_GetRuntimeClassName,
    key_args2_GetTrustLevel,
    key_args2_get_DeviceId,
};

KEY_ARGS_FORWARD( ICoreWindowEventArgs, key_window_args )

static HRESULT WINAPI key_window_args_get_Handled( ICoreWindowEventArgs *iface, boolean *value )
{
    *value = key_args_from_ICoreWindowEventArgs( iface )->handled;
    return S_OK;
}

static HRESULT WINAPI key_window_args_put_Handled( ICoreWindowEventArgs *iface, boolean value )
{
    key_args_from_ICoreWindowEventArgs( iface )->handled = value;
    return S_OK;
}

static const struct ICoreWindowEventArgsVtbl key_window_args_vtbl =
{
    key_window_args_QueryInterface,
    key_window_args_AddRef,
    key_window_args_Release,
    key_window_args_GetIids,
    key_window_args_GetRuntimeClassName,
    key_window_args_GetTrustLevel,
    key_window_args_get_Handled,
    key_window_args_put_Handled,
};

/* the producer: ITextInputProducer, slots 6-14 */
typedef struct text_input_producer text_input_producer;
struct text_input_producer_vtbl
{
    HRESULT (WINAPI *QueryInterface)( text_input_producer *, REFIID, void ** );
    ULONG (WINAPI *AddRef)( text_input_producer * );
    ULONG (WINAPI *Release)( text_input_producer * );
    HRESULT (WINAPI *GetIids)( text_input_producer *, ULONG *, IID ** );
    HRESULT (WINAPI *GetRuntimeClassName)( text_input_producer *, HSTRING * );
    HRESULT (WINAPI *GetTrustLevel)( text_input_producer *, TrustLevel * );
    HRESULT (WINAPI *get_IsInputEnabled)( text_input_producer *, boolean * );
    HRESULT (WINAPI *put_IsInputEnabled)( text_input_producer *, boolean );
    HRESULT (WINAPI *get_HasFocus)( text_input_producer *, boolean * );
    HRESULT (WINAPI *put_HasFocus)( text_input_producer *, boolean );
    HRESULT (WINAPI *get_MessageHandled)( text_input_producer *, boolean * );
    HRESULT (WINAPI *get_CurrentKeyEventType)( text_input_producer *, INT32 * );
    HRESULT (WINAPI *GetAsyncKeyState)( text_input_producer *, VirtualKey, CoreVirtualKeyStates * );
    HRESULT (WINAPI *GetKeyState)( text_input_producer *, VirtualKey, CoreVirtualKeyStates * );
    HRESULT (WINAPI *GetCurrentKeyEventDeviceId)( text_input_producer *, HSTRING * );
};
struct text_input_producer { const struct text_input_producer_vtbl *lpVtbl; };

struct producer
{
    text_input_producer iface;
    IInspectable phone_iface;
    LONG ref;
    struct list entry;
    text_input_consumer *consumer;   /* not held: the consumer owns us */
    HWND hwnd;
    DWORD thread;
    HHOOK hook;
    boolean input_enabled, has_focus, handled;
};

static struct list producers = LIST_INIT( producers );
static CRITICAL_SECTION producers_cs;
static CRITICAL_SECTION_DEBUG producers_cs_debug =
{
    0, 0, &producers_cs,
    { &producers_cs_debug.ProcessLocksList, &producers_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": producers_cs") }
};
static CRITICAL_SECTION producers_cs = { &producers_cs_debug, -1, 0, 0, 0, 0 };

static inline struct producer *impl_from_producer( text_input_producer *iface )
{
    return CONTAINING_RECORD( iface, struct producer, iface );
}

static HRESULT WINAPI producer_QueryInterface( text_input_producer *iface, REFIID iid, void **out )
{
    struct producer *impl = impl_from_producer( iface );

    TRACE( "%p %s\n", iface, debugstr_guid( iid ) );
    /* Windows.UI takes ITextInputProducer from put_TextInputProducer as it is; anything else it asks for
     * (the input-enable interface {4514e4be}, weak references) is optional and refused. */
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ITextInputProducer_local ))
    {
        *out = &impl->iface;
        impl->iface.lpVtbl->AddRef( &impl->iface );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_ITextInputProducerPhone_local ))
    {
        *out = &impl->phone_iface;
        impl->iface.lpVtbl->AddRef( &impl->iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI producer_AddRef( text_input_producer *iface )
{
    return InterlockedIncrement( &impl_from_producer( iface )->ref );
}

static ULONG WINAPI producer_Release( text_input_producer *iface )
{
    struct producer *impl = impl_from_producer( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        EnterCriticalSection( &producers_cs );
        list_remove( &impl->entry );
        LeaveCriticalSection( &producers_cs );
        if (impl->hook) UnhookWindowsHookEx( impl->hook );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI producer_GetIids( text_input_producer *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI producer_GetRuntimeClassName( text_input_producer *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.UI.Core.TextInputProducer";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI producer_GetTrustLevel( text_input_producer *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI producer_get_IsInputEnabled( text_input_producer *iface, boolean *value )
{
    *value = impl_from_producer( iface )->input_enabled;
    return S_OK;
}

static HRESULT WINAPI producer_put_IsInputEnabled( text_input_producer *iface, boolean value )
{
    TRACE( "%p %d\n", iface, value );
    impl_from_producer( iface )->input_enabled = value;
    return S_OK;
}

static HRESULT WINAPI producer_get_HasFocus( text_input_producer *iface, boolean *value )
{
    *value = impl_from_producer( iface )->has_focus;
    return S_OK;
}

static HRESULT WINAPI producer_put_HasFocus( text_input_producer *iface, boolean value )
{
    TRACE( "%p %d\n", iface, value );
    impl_from_producer( iface )->has_focus = value;
    return S_OK;
}

/* asked by WindowServer::OnKeyDownEvent/OnKeyUpEvent while it dispatches the key the hook has already raised */
static HRESULT WINAPI producer_get_MessageHandled( text_input_producer *iface, boolean *value )
{
    struct producer *impl = impl_from_producer( iface );
    *value = impl->handled;
    TRACE( "%p -> %d\n", iface, impl->handled );
    return S_OK;
}

static HRESULT WINAPI producer_get_CurrentKeyEventType( text_input_producer *iface, INT32 *value )
{
    *value = 0;
    return S_OK;
}

static CoreVirtualKeyStates key_states( SHORT state )
{
    CoreVirtualKeyStates ret = CoreVirtualKeyStates_None;
    if (state & 0x8000) ret |= CoreVirtualKeyStates_Down;
    if (state & 1) ret |= CoreVirtualKeyStates_Locked;
    return ret;
}

static HRESULT WINAPI producer_GetAsyncKeyState( text_input_producer *iface, VirtualKey key, CoreVirtualKeyStates *value )
{
    *value = key_states( GetAsyncKeyState( key ) );
    return S_OK;
}

static HRESULT WINAPI producer_GetKeyState( text_input_producer *iface, VirtualKey key, CoreVirtualKeyStates *value )
{
    *value = key_states( GetKeyState( key ) );
    return S_OK;
}

static HRESULT WINAPI producer_GetCurrentKeyEventDeviceId( text_input_producer *iface, HSTRING *value )
{
    *value = NULL;
    return S_OK;
}

static const struct text_input_producer_vtbl producer_vtbl =
{
    producer_QueryInterface,
    producer_AddRef,
    producer_Release,
    producer_GetIids,
    producer_GetRuntimeClassName,
    producer_GetTrustLevel,
    producer_get_IsInputEnabled,
    producer_put_IsInputEnabled,
    producer_get_HasFocus,
    producer_put_HasFocus,
    producer_get_MessageHandled,
    producer_get_CurrentKeyEventType,
    producer_GetAsyncKeyState,
    producer_GetKeyState,
    producer_GetCurrentKeyEventDeviceId,
};

/* ITextInputProducerPhone: IInspectable plus four methods, all E_NOTIMPL as in Windows' producer */
static inline struct producer *impl_from_phone( IInspectable *iface )
{
    return CONTAINING_RECORD( iface, struct producer, phone_iface );
}

static HRESULT WINAPI phone_QueryInterface( IInspectable *iface, REFIID iid, void **out )
{
    return producer_QueryInterface( &impl_from_phone( iface )->iface, iid, out );
}

static ULONG WINAPI phone_AddRef( IInspectable *iface )
{
    return producer_AddRef( &impl_from_phone( iface )->iface );
}

static ULONG WINAPI phone_Release( IInspectable *iface )
{
    return producer_Release( &impl_from_phone( iface )->iface );
}

static HRESULT WINAPI phone_GetIids( IInspectable *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI phone_GetRuntimeClassName( IInspectable *iface, HSTRING *name )
{
    return producer_GetRuntimeClassName( &impl_from_phone( iface )->iface, name );
}

static HRESULT WINAPI phone_GetTrustLevel( IInspectable *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI phone_notimpl( IInspectable *iface )
{
    FIXME( "%p: ITextInputProducerPhone method called\n", iface );
    return E_NOTIMPL;
}

static const void *phone_vtbl[] =
{
    phone_QueryInterface,
    phone_AddRef,
    phone_Release,
    phone_GetIids,
    phone_GetRuntimeClassName,
    phone_GetTrustLevel,
    phone_notimpl,
    phone_notimpl,
    phone_notimpl,
    phone_notimpl,
};

/* raise KeyDown/KeyUp (or the System variants) for one key message; the same status Windows.UI's own
 * OnKeyDownEvent builds from lParam */
static void raise_key_event( struct producer *impl, const MSG *msg )
{
    BOOL up = msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP;
    BOOL sys = msg->message == WM_SYSKEYDOWN || msg->message == WM_SYSKEYUP;
    text_input_consumer *consumer = impl->consumer;
    struct key_args *args;
    HRESULT hr;

    impl->handled = FALSE;
    if (!(args = calloc( 1, sizeof(*args) ))) return;
    args->IKeyEventArgs_iface.lpVtbl = &key_args_vtbl;
    args->IKeyEventArgs2_iface.lpVtbl = &key_args2_vtbl;
    args->ICoreWindowEventArgs_iface.lpVtbl = &key_window_args_vtbl;
    args->ref = 1;
    args->key = msg->wParam;
    args->status.RepeatCount = LOWORD( msg->lParam );
    args->status.ScanCode = (msg->lParam >> 16) & 0xff;
    args->status.IsExtendedKey = (msg->lParam >> 24) & 1;
    args->status.IsMenuKeyDown = sys ? (msg->lParam >> 29) & 1 : 0;
    args->status.WasKeyDown = (msg->lParam >> 30) & 1;
    args->status.IsKeyReleased = up;

    consumer->lpVtbl->AddRef( consumer );
    if (sys) hr = up ? consumer->lpVtbl->InvokeSystemKeyUpEventHandlers( consumer, &args->IKeyEventArgs_iface )
                     : consumer->lpVtbl->InvokeSystemKeyDownEventHandlers( consumer, &args->IKeyEventArgs_iface );
    else hr = up ? consumer->lpVtbl->InvokeKeyUpEventHandlers( consumer, &args->IKeyEventArgs_iface )
                 : consumer->lpVtbl->InvokeKeyDownEventHandlers( consumer, &args->IKeyEventArgs_iface );
    consumer->lpVtbl->Release( consumer );
    impl->handled = args->handled;
    TRACE( "%p msg %#x vk %#Ix -> hr %#lx handled %d\n", impl, msg->message, msg->wParam, hr, args->handled );
    key_args_Release( &args->IKeyEventArgs_iface );
}

static LRESULT CALLBACK producer_hook( int code, WPARAM wparam, LPARAM lparam )
{
    MSG *msg = (MSG *)lparam;
    struct producer *impl, *found = NULL;
    DWORD thread = GetCurrentThreadId();
    HHOOK hook = NULL;

    EnterCriticalSection( &producers_cs );
    LIST_FOR_EACH_ENTRY( impl, &producers, struct producer, entry )
    {
        if (impl->thread != thread) continue;
        if (!hook) hook = impl->hook;
        if (msg->hwnd == impl->hwnd) found = impl;
    }
    if (found) found->iface.lpVtbl->AddRef( &found->iface );
    LeaveCriticalSection( &producers_cs );

    if (found)
    {
        if (code == HC_ACTION && wparam == PM_REMOVE &&
            (msg->message == WM_KEYDOWN || msg->message == WM_KEYUP ||
             msg->message == WM_SYSKEYDOWN || msg->message == WM_SYSKEYUP))
            raise_key_event( found, msg );
        found->iface.lpVtbl->Release( &found->iface );
    }
    return CallNextHookEx( hook, code, wparam, lparam );
}

/***********************************************************************
 *           PrivateCreateTextInputProducer (windows.ui.core.textinput.1500)
 *
 * Only for a CoreWindow: Windows.UI's component input sources (CoreIndependentInputSource) call this too, and
 * failed it before this existed, which they tolerate; they keep their own key path.
 */
HRESULT WINAPI PrivateCreateTextInputProducer( IUnknown *consumer_unk, IUnknown **out )
{
    core_window_interop *interop = NULL;
    text_input_consumer *consumer = NULL;
    struct producer *impl;
    HWND hwnd = NULL;
    HRESULT hr;

    TRACE( "%p %p\n", consumer_unk, out );
    *out = NULL;
    if (FAILED( hr = IUnknown_QueryInterface( consumer_unk, &IID_ICoreWindowInterop_local, (void **)&interop ) ))
        return E_FAIL;
    hr = interop->lpVtbl->get_WindowHandle( interop, &hwnd );
    interop->lpVtbl->Release( interop );
    if (FAILED( hr ) || !hwnd) return E_FAIL;
    if (FAILED( hr = IUnknown_QueryInterface( consumer_unk, &IID_ITextInputConsumer_local, (void **)&consumer ) ))
        return E_FAIL;
    consumer->lpVtbl->Release( consumer );   /* not held: the consumer owns the producer */

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->iface.lpVtbl = &producer_vtbl;
    impl->phone_iface.lpVtbl = (const IInspectableVtbl *)phone_vtbl;
    impl->ref = 1;
    impl->consumer = consumer;
    impl->hwnd = hwnd;
    impl->thread = GetWindowThreadProcessId( hwnd, NULL );
    impl->input_enabled = TRUE;
    if (!(impl->hook = SetWindowsHookExW( WH_GETMESSAGE, producer_hook, NULL, impl->thread )))
    {
        WARN( "could not hook thread %04lx, error %lu\n", impl->thread, GetLastError() );
        free( impl );
        return E_FAIL;
    }
    EnterCriticalSection( &producers_cs );
    list_add_tail( &producers, &impl->entry );
    LeaveCriticalSection( &producers_cs );
    TRACE( "producer %p for window %p, thread %04lx\n", impl, hwnd, impl->thread );
    *out = (IUnknown *)&impl->iface;
    return S_OK;
}

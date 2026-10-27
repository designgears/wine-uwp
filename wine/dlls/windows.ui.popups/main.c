/*
 * Windows.UI.Popups: MessageDialog and UICommand
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
 * Windows' MessageDialog and UICommand live in Windows.UI.Immersive.dll, whose thread attach initialises the shell's
 * DirectUI (DUI70.dll, not in a prefix): the delay-load failure raised 0x505 and ended Gears of War 4 the moment it
 * reached for a dialog, which also hid what the dialog was going to say. Here the dialog is a plain message box. Its
 * text is logged as well (ERR, so it is in every log): a dialog an app shows at start-up is usually an error.
 *
 * There is no IDL for Windows.UI.Popups in Wine, so the interfaces are written out by slot, from the metadata:
 * IMessageDialog {33f59b01}, IMessageDialogFactory {2d161777}, IUICommand {4ff93a75}, IUICommandFactory {a21a8189};
 * IVector<IUICommand>, IVectorView<IUICommand> and IAsyncOperation<IUICommand> by piid.py.
 *
 * The dialog is a task dialog with the app's own button labels, its DefaultCommandIndex the default button and its
 * CancelCommandIndex what closing it chooses. With no commands Windows adds Close, which is what the operation then
 * returns.
 */

#include <stdarg.h>
#include <stdlib.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "commctrl.h"
#include "winstring.h"
#include "objbase.h"
#include "initguid.h"
#include "activation.h"
#include "roapi.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(popups);

static const GUID IID_IMessageDialog_ = { 0x33f59b01, 0x5325, 0x43ab, { 0x9a, 0xb3, 0xbd, 0xae, 0x44, 0x0e, 0x41, 0x21 } };
static const GUID IID_IMessageDialogFactory_ = { 0x2d161777, 0xa66f, 0x4ea5, { 0xbb, 0x87, 0x79, 0x3f, 0xfa, 0x49, 0x41, 0xf2 } };
static const GUID IID_IUICommand_ = { 0x4ff93a75, 0x4145, 0x47ff, { 0xac, 0x7f, 0xdf, 0xf1, 0xc1, 0xfa, 0x5b, 0x0f } };
static const GUID IID_IUICommandFactory_ = { 0xa21a8189, 0x26b0, 0x4676, { 0xae, 0x94, 0x54, 0x04, 0x1b, 0xc1, 0x25, 0xe8 } };
static const GUID IID_IVector_IUICommand = { 0x105139a1, 0xdcb8, 0x5f65, { 0x97, 0xef, 0xcb, 0x1b, 0xf0, 0xb7, 0x5f, 0x9d } };
static const GUID IID_IVectorView_IUICommand = { 0xed1165e6, 0xf377, 0x5b00, { 0x81, 0x72, 0x93, 0xc1, 0xbd, 0x04, 0xde, 0xb4 } };
static const GUID IID_IAsyncOperation_IUICommand = { 0xb8770535, 0x6a4b, 0x52b1, { 0xb5, 0x78, 0xf3, 0xcd, 0xc5, 0x00, 0x7a, 0x1f } };
static const GUID IID_IAsyncInfo_ = { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID IID_IAgileObject_ = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

static const WCHAR dialog_class[] = L"Windows.UI.Popups.MessageDialog";
static HINSTANCE instance;

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        instance = inst;
        DisableThreadLibraryCalls( inst );
    }
    return TRUE;
}
static const WCHAR command_class[] = L"Windows.UI.Popups.UICommand";

static HRESULT WINAPI any_GetIids( void *iface, ULONG *count, IID **iids )
{
    if (count) *count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}
static HRESULT WINAPI any_GetTrustLevel( void *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}
static HRESULT class_name( const WCHAR *name, HSTRING *out )
{
    return WindowsCreateString( name, wcslen( name ), out );
}

/* ---- UICommand ---- */

struct command
{
    const void **vtbl;
    LONG refs;
    HSTRING label;
    IUnknown *invoked;              /* UICommandInvokedHandler */
    IUnknown *id;
};

static const void *command_vtbl[12];

static struct command *command_from( void *iface )
{
    struct command *c = iface;
    return c && c->vtbl == command_vtbl ? c : NULL;
}

static HRESULT WINAPI command_QueryInterface( struct command *c, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject_ ) || IsEqualGUID( iid, &IID_IUICommand_ ))
    {
        InterlockedIncrement( &c->refs );
        *out = c;
        return S_OK;
    }
    FIXME( "UICommand: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI command_AddRef( struct command *c ) { return InterlockedIncrement( &c->refs ); }
static ULONG WINAPI command_Release( struct command *c )
{
    ULONG refs = InterlockedDecrement( &c->refs );
    if (!refs)
    {
        WindowsDeleteString( c->label );
        if (c->invoked) IUnknown_Release( c->invoked );
        if (c->id) IUnknown_Release( c->id );
        free( c );
    }
    return refs;
}
static HRESULT WINAPI command_GetRuntimeClassName( struct command *c, HSTRING *name ) { return class_name( command_class, name ); }
static HRESULT WINAPI command_get_Label( struct command *c, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( c->label, value );
}
static HRESULT WINAPI command_put_Label( struct command *c, HSTRING value )
{
    HSTRING copy;
    HRESULT hr;
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( c->label );
    c->label = copy;
    return S_OK;
}
static HRESULT get_object( IUnknown *object, IUnknown **value )
{
    if (!value) return E_POINTER;
    if ((*value = object)) IUnknown_AddRef( object );
    return S_OK;
}
static void put_object( IUnknown **slot, IUnknown *value )
{
    if (value) IUnknown_AddRef( value );
    if (*slot) IUnknown_Release( *slot );
    *slot = value;
}
static HRESULT WINAPI command_get_Invoked( struct command *c, IUnknown **value ) { return get_object( c->invoked, value ); }
static HRESULT WINAPI command_put_Invoked( struct command *c, IUnknown *value )
{
    put_object( &c->invoked, value );
    return S_OK;
}
static HRESULT WINAPI command_get_Id( struct command *c, IUnknown **value ) { return get_object( c->id, value ); }
static HRESULT WINAPI command_put_Id( struct command *c, IUnknown *value )
{
    put_object( &c->id, value );
    return S_OK;
}

static struct command *command_create( HSTRING label, IUnknown *invoked, IUnknown *id )
{
    struct command *c;

    if (!(c = calloc( 1, sizeof(*c) ))) return NULL;
    c->vtbl = command_vtbl;
    c->refs = 1;
    if (label) WindowsDuplicateString( label, &c->label );
    put_object( &c->invoked, invoked );
    put_object( &c->id, id );
    return c;
}

/* ---- the dialog's Commands: IVector<IUICommand>, and its view ---- */

struct commands
{
    const void **vtbl;
    const void **view_vtbl;
    LONG refs;
    IUnknown **items;
    UINT32 count, capacity;
};

static const void *commands_vtbl[18], *commands_view_vtbl[10];

static struct commands *commands_from_view( void *iface ) { return CONTAINING_RECORD( iface, struct commands, view_vtbl ); }

static HRESULT WINAPI commands_QueryInterface( struct commands *v, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject_ ) || IsEqualGUID( iid, &IID_IVector_IUICommand ))
        *out = &v->vtbl;
    else if (IsEqualGUID( iid, &IID_IVectorView_IUICommand )) *out = &v->view_vtbl;
    else
    {
        FIXME( "Commands: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement( &v->refs );
    return S_OK;
}
static ULONG WINAPI commands_AddRef( struct commands *v ) { return InterlockedIncrement( &v->refs ); }
static ULONG WINAPI commands_Release( struct commands *v )
{
    ULONG refs = InterlockedDecrement( &v->refs );
    UINT32 i;
    if (!refs)
    {
        for (i = 0; i < v->count; i++) IUnknown_Release( v->items[i] );
        free( v->items );
        free( v );
    }
    return refs;
}
static HRESULT WINAPI commands_GetRuntimeClassName( struct commands *v, HSTRING *name )
{
    return class_name( L"Windows.Foundation.Collections.IVector`1<Windows.UI.Popups.IUICommand>", name );
}
static HRESULT WINAPI commands_GetAt( struct commands *v, UINT32 index, IUnknown **item )
{
    if (!item) return E_POINTER;
    *item = NULL;
    if (index >= v->count) return E_BOUNDS;
    *item = v->items[index];
    IUnknown_AddRef( *item );
    return S_OK;
}
static HRESULT WINAPI commands_get_Size( struct commands *v, UINT32 *size )
{
    if (!size) return E_POINTER;
    *size = v->count;
    return S_OK;
}
static HRESULT WINAPI commands_GetView( struct commands *v, void **view )
{
    if (!view) return E_POINTER;
    InterlockedIncrement( &v->refs );
    *view = &v->view_vtbl;
    return S_OK;
}
static HRESULT WINAPI commands_IndexOf( struct commands *v, IUnknown *item, UINT32 *index, boolean *found )
{
    UINT32 i;
    if (!index || !found) return E_POINTER;
    for (i = 0; i < v->count; i++) if (v->items[i] == item) break;
    *found = i < v->count;
    *index = *found ? i : 0;
    return S_OK;
}
static HRESULT WINAPI commands_InsertAt( struct commands *v, UINT32 index, IUnknown *item )
{
    if (!item) return E_INVALIDARG;
    if (index > v->count) return E_BOUNDS;
    if (v->count == v->capacity)
    {
        UINT32 cap = v->capacity ? v->capacity * 2 : 4;
        IUnknown **items = realloc( v->items, cap * sizeof(*items) );
        if (!items) return E_OUTOFMEMORY;
        v->items = items;
        v->capacity = cap;
    }
    memmove( v->items + index + 1, v->items + index, (v->count - index) * sizeof(*v->items) );
    v->items[index] = item;
    IUnknown_AddRef( item );
    v->count++;
    return S_OK;
}
static HRESULT WINAPI commands_SetAt( struct commands *v, UINT32 index, IUnknown *item )
{
    if (!item) return E_INVALIDARG;
    if (index >= v->count) return E_BOUNDS;
    put_object( &v->items[index], item );
    return S_OK;
}
static HRESULT WINAPI commands_RemoveAt( struct commands *v, UINT32 index )
{
    if (index >= v->count) return E_BOUNDS;
    IUnknown_Release( v->items[index] );
    memmove( v->items + index, v->items + index + 1, (v->count - index - 1) * sizeof(*v->items) );
    v->count--;
    return S_OK;
}
static HRESULT WINAPI commands_Append( struct commands *v, IUnknown *item ) { return commands_InsertAt( v, v->count, item ); }
static HRESULT WINAPI commands_RemoveAtEnd( struct commands *v )
{
    return v->count ? commands_RemoveAt( v, v->count - 1 ) : E_BOUNDS;
}
static HRESULT WINAPI commands_Clear( struct commands *v )
{
    while (v->count) commands_RemoveAt( v, v->count - 1 );
    return S_OK;
}
static HRESULT WINAPI commands_GetMany( struct commands *v, UINT32 start, UINT32 size, IUnknown **items, UINT32 *actual )
{
    UINT32 i;
    if (!actual) return E_POINTER;
    for (i = 0; start + i < v->count && i < size; i++)
    {
        items[i] = v->items[start + i];
        IUnknown_AddRef( items[i] );
    }
    *actual = i;
    return S_OK;
}
static HRESULT WINAPI commands_ReplaceAll( struct commands *v, UINT32 count, IUnknown **items )
{
    UINT32 i;
    HRESULT hr = S_OK;
    commands_Clear( v );
    for (i = 0; i < count && SUCCEEDED(hr); i++) hr = commands_Append( v, items[i] );
    return hr;
}

static HRESULT WINAPI view_QueryInterface( void *iface, REFIID iid, void **out ) { return commands_QueryInterface( commands_from_view( iface ), iid, out ); }
static ULONG WINAPI view_AddRef( void *iface ) { return commands_AddRef( commands_from_view( iface ) ); }
static ULONG WINAPI view_Release( void *iface ) { return commands_Release( commands_from_view( iface ) ); }
static HRESULT WINAPI view_GetRuntimeClassName( void *iface, HSTRING *name ) { return commands_GetRuntimeClassName( NULL, name ); }
static HRESULT WINAPI view_GetAt( void *iface, UINT32 index, IUnknown **item ) { return commands_GetAt( commands_from_view( iface ), index, item ); }
static HRESULT WINAPI view_get_Size( void *iface, UINT32 *size ) { return commands_get_Size( commands_from_view( iface ), size ); }
static HRESULT WINAPI view_IndexOf( void *iface, IUnknown *item, UINT32 *index, boolean *found )
{
    return commands_IndexOf( commands_from_view( iface ), item, index, found );
}
static HRESULT WINAPI view_GetMany( void *iface, UINT32 start, UINT32 size, IUnknown **items, UINT32 *actual )
{
    return commands_GetMany( commands_from_view( iface ), start, size, items, actual );
}

static struct commands *commands_create( void )
{
    struct commands *v;
    if (!(v = calloc( 1, sizeof(*v) ))) return NULL;
    v->vtbl = commands_vtbl;
    v->view_vtbl = commands_view_vtbl;
    v->refs = 1;
    return v;
}

/* ---- ShowAsync's operation ---- */

struct show
{
    const void **vtbl;
    const void **info_vtbl;
    LONG refs;
    CRITICAL_SECTION cs;
    BOOL done;
    IUnknown *result;               /* the command chosen */
    IUnknown *handler;              /* AsyncOperationCompletedHandler<IUICommand> */
};

static const void *show_vtbl[9], *show_info_vtbl[11];

static struct show *show_from_info( void *iface ) { return CONTAINING_RECORD( iface, struct show, info_vtbl ); }

static HRESULT WINAPI show_QueryInterface( struct show *op, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject_ ) || IsEqualGUID( iid, &IID_IAsyncOperation_IUICommand ))
        *out = &op->vtbl;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo_ )) *out = &op->info_vtbl;
    else
    {
        FIXME( "ShowAsync operation: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement( &op->refs );
    return S_OK;
}
static ULONG WINAPI show_AddRef( struct show *op ) { return InterlockedIncrement( &op->refs ); }
static ULONG WINAPI show_Release( struct show *op )
{
    ULONG refs = InterlockedDecrement( &op->refs );
    if (!refs)
    {
        if (op->result) IUnknown_Release( op->result );
        if (op->handler) IUnknown_Release( op->handler );
        DeleteCriticalSection( &op->cs );
        free( op );
    }
    return refs;
}
static HRESULT WINAPI show_GetRuntimeClassName( struct show *op, HSTRING *name )
{
    return class_name( L"Windows.Foundation.IAsyncOperation`1<Windows.UI.Popups.IUICommand>", name );
}
static void invoke_completed( struct show *op, IUnknown *handler )
{
    HRESULT (WINAPI *invoke)( IUnknown *, void *, INT32 ) = (*(void ***)handler)[3];   /* a delegate's Invoke */
    invoke( handler, &op->vtbl, 1 /* Completed */ );
}
static HRESULT WINAPI show_put_Completed( struct show *op, IUnknown *handler )
{
    BOOL done;

    if (!handler) return E_POINTER;
    EnterCriticalSection( &op->cs );
    if (!(done = op->done)) put_object( &op->handler, handler );
    LeaveCriticalSection( &op->cs );
    if (done) invoke_completed( op, handler );
    return S_OK;
}
static HRESULT WINAPI show_get_Completed( struct show *op, IUnknown **handler )
{
    HRESULT hr;
    EnterCriticalSection( &op->cs );
    hr = get_object( op->handler, handler );
    LeaveCriticalSection( &op->cs );
    return hr;
}
static HRESULT WINAPI show_GetResults( struct show *op, IUnknown **result )
{
    HRESULT hr;
    EnterCriticalSection( &op->cs );
    hr = op->done ? get_object( op->result, result ) : E_ILLEGAL_METHOD_CALL;
    LeaveCriticalSection( &op->cs );
    return hr;
}
static HRESULT WINAPI info_QueryInterface( void *iface, REFIID iid, void **out ) { return show_QueryInterface( show_from_info( iface ), iid, out ); }
static ULONG WINAPI info_AddRef( void *iface ) { return show_AddRef( show_from_info( iface ) ); }
static ULONG WINAPI info_Release( void *iface ) { return show_Release( show_from_info( iface ) ); }
static HRESULT WINAPI info_GetRuntimeClassName( void *iface, HSTRING *name ) { return show_GetRuntimeClassName( NULL, name ); }
static HRESULT WINAPI info_get_Id( void *iface, UINT32 *id )
{
    if (!id) return E_POINTER;
    *id = 1;
    return S_OK;
}
static HRESULT WINAPI info_get_Status( void *iface, INT32 *status )
{
    struct show *op = show_from_info( iface );
    if (!status) return E_POINTER;
    EnterCriticalSection( &op->cs );
    *status = op->done ? 1 /* Completed */ : 0 /* Started */;
    LeaveCriticalSection( &op->cs );
    return S_OK;
}
static HRESULT WINAPI info_get_ErrorCode( void *iface, HRESULT *code )
{
    if (!code) return E_POINTER;
    *code = S_OK;
    return S_OK;
}
static HRESULT WINAPI info_Cancel( void *iface ) { return S_OK; }
static HRESULT WINAPI info_Close( void *iface ) { return S_OK; }

/* ---- MessageDialog ---- */

struct dialog
{
    const void **vtbl;
    LONG refs;
    HSTRING title, content;
    struct commands *commands;
    UINT32 default_index, cancel_index;
    INT32 options;
};

static const void *dialog_vtbl[18];

struct show_params
{
    struct dialog *dialog;
    struct show *op;
};

/* TaskDialogIndirect is comctl32 6's, loaded through this module's manifest */
static HRESULT task_dialog( const TASKDIALOGCONFIG *config, int *button )
{
    static HRESULT (WINAPI *task_dialog_indirect)( const TASKDIALOGCONFIG *, int *, int *, BOOL * );
    ACTCTXW ctx = { sizeof(ctx) };
    ULONG_PTR cookie;
    HANDLE actctx;
    HMODULE module;

    if (!task_dialog_indirect)
    {
        ctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        ctx.hModule = instance;
        ctx.lpResourceName = MAKEINTRESOURCEW( ISOLATIONAWARE_MANIFEST_RESOURCE_ID );
        if ((actctx = CreateActCtxW( &ctx )) == INVALID_HANDLE_VALUE)
        {
            ERR( "no activation context for comctl32 6: error %lu\n", GetLastError() );
            return HRESULT_FROM_WIN32( GetLastError() );
        }
        ActivateActCtx( actctx, &cookie );
        if ((module = LoadLibraryW( L"comctl32.dll" )))
            task_dialog_indirect = (void *)GetProcAddress( module, "TaskDialogIndirect" );
        DeactivateActCtx( 0, cookie );
        ReleaseActCtx( actctx );
        if (!task_dialog_indirect)
        {
            ERR( "no TaskDialogIndirect in comctl32 (%p)\n", module );
            return E_NOTIMPL;
        }
    }
    return task_dialog_indirect( config, button, NULL, NULL );
}

static DWORD WINAPI show_thread( void *param )
{
    struct show_params *p = param;
    struct dialog *d = p->dialog;
    struct show *op = p->op;
    struct commands *commands = d->commands;
    UINT32 count = commands->count, chosen = 0, i;
    IUnknown *result = NULL, *handler;

    for (i = 0; i < count; i++)
    {
        struct command *c = command_from( commands->items[i] );
        ERR( "MessageDialog command %u: %s\n", i, c ? debugstr_hstring( c->label ) : "(not ours)" );
    }
    /* a task dialog carries the app's own labels: a message box's OK ran Gears of War 4's "More Information" when the
     * user meant its "Continue" */
    if (count)
    {
        TASKDIALOG_BUTTON *buttons = calloc( count, sizeof(*buttons) );
        TASKDIALOGCONFIG config = { sizeof(config) };
        int button = 0;
        HRESULT hr = S_OK;

        for (i = 0; buttons && i < count; i++)
        {
            struct command *c = command_from( commands->items[i] );
            buttons[i].nButtonID = 100 + i;
            buttons[i].pszButtonText = c ? WindowsGetStringRawBuffer( c->label, NULL ) : L"";
        }
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        config.pszWindowTitle = WindowsGetStringRawBuffer( d->title, NULL );
        config.pszContent = WindowsGetStringRawBuffer( d->content, NULL );
        config.pButtons = buttons;
        config.cButtons = buttons ? count : 0;
        config.nDefaultButton = 100 + min( d->default_index, count - 1 );
        if (buttons && SUCCEEDED(hr = task_dialog( &config, &button )) && button >= 100 && button < 100 + (int)count)
            chosen = button - 100;
        else if (FAILED(hr)) ERR( "task dialog failed: %#lx\n", hr );
        else chosen = min( d->cancel_index, count - 1 );   /* closed, or Escape: the cancel command */
        free( buttons );
    }
    else MessageBoxW( NULL, WindowsGetStringRawBuffer( d->content, NULL ), WindowsGetStringRawBuffer( d->title, NULL ),
                      MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND );
    if (count)
    {
        struct command *c;

        result = commands->items[min( chosen, count - 1 )];
        IUnknown_AddRef( result );
        /* the chosen command's own handler runs first, as on Windows */
        if ((c = command_from( result )) && c->invoked)
        {
            HRESULT (WINAPI *invoke)( IUnknown *, IUnknown * ) = (*(void ***)c->invoked)[3];
            invoke( c->invoked, result );
        }
    }
    else
    {
        HSTRING close;
        WindowsCreateString( L"Close", 5, &close );
        result = (IUnknown *)command_create( close, NULL, NULL );
        WindowsDeleteString( close );
    }
    TRACE( "dialog %p: command %u chosen.\n", d, chosen );

    EnterCriticalSection( &op->cs );
    op->result = result;
    op->done = TRUE;
    handler = op->handler;
    op->handler = NULL;
    LeaveCriticalSection( &op->cs );
    if (handler)
    {
        invoke_completed( op, handler );
        IUnknown_Release( handler );
    }
    IUnknown_Release( (IUnknown *)&op->vtbl );
    IUnknown_Release( (IUnknown *)d );
    free( p );
    return 0;
}

static HRESULT WINAPI dialog_QueryInterface( struct dialog *d, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject_ ) || IsEqualGUID( iid, &IID_IMessageDialog_ ))
    {
        InterlockedIncrement( &d->refs );
        *out = d;
        return S_OK;
    }
    FIXME( "MessageDialog: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI dialog_AddRef( struct dialog *d ) { return InterlockedIncrement( &d->refs ); }
static ULONG WINAPI dialog_Release( struct dialog *d )
{
    ULONG refs = InterlockedDecrement( &d->refs );
    if (!refs)
    {
        WindowsDeleteString( d->title );
        WindowsDeleteString( d->content );
        commands_Release( d->commands );
        free( d );
    }
    return refs;
}
static HRESULT WINAPI dialog_GetRuntimeClassName( struct dialog *d, HSTRING *name ) { return class_name( dialog_class, name ); }
static HRESULT put_string( HSTRING *slot, HSTRING value )
{
    HSTRING copy;
    HRESULT hr;
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( *slot );
    *slot = copy;
    return S_OK;
}
static HRESULT WINAPI dialog_get_Title( struct dialog *d, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( d->title, value );
}
static HRESULT WINAPI dialog_put_Title( struct dialog *d, HSTRING value ) { return put_string( &d->title, value ); }
static HRESULT WINAPI dialog_get_Commands( struct dialog *d, void **value )
{
    if (!value) return E_POINTER;
    commands_AddRef( d->commands );
    *value = &d->commands->vtbl;
    return S_OK;
}
static HRESULT WINAPI dialog_get_DefaultCommandIndex( struct dialog *d, UINT32 *value )
{
    if (!value) return E_POINTER;
    *value = d->default_index;
    return S_OK;
}
static HRESULT WINAPI dialog_put_DefaultCommandIndex( struct dialog *d, UINT32 value )
{
    d->default_index = value;
    return S_OK;
}
static HRESULT WINAPI dialog_get_CancelCommandIndex( struct dialog *d, UINT32 *value )
{
    if (!value) return E_POINTER;
    *value = d->cancel_index;
    return S_OK;
}
static HRESULT WINAPI dialog_put_CancelCommandIndex( struct dialog *d, UINT32 value )
{
    d->cancel_index = value;
    return S_OK;
}
static HRESULT WINAPI dialog_get_Content( struct dialog *d, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( d->content, value );
}
static HRESULT WINAPI dialog_put_Content( struct dialog *d, HSTRING value ) { return put_string( &d->content, value ); }
static HRESULT WINAPI dialog_ShowAsync( struct dialog *d, void **operation )
{
    struct show_params *p;
    struct show *op;
    HANDLE thread;

    if (!operation) return E_POINTER;
    ERR( "MessageDialog %s: %s\n", debugstr_hstring( d->title ), debugstr_hstring( d->content ) );
    if (!(op = calloc( 1, sizeof(*op) )) || !(p = malloc( sizeof(*p) )))
    {
        free( op );
        return E_OUTOFMEMORY;
    }
    op->vtbl = show_vtbl;
    op->info_vtbl = show_info_vtbl;
    op->refs = 2;                   /* the caller's and the dialog thread's */
    InitializeCriticalSection( &op->cs );
    dialog_AddRef( d );
    p->dialog = d;
    p->op = op;
    if (!(thread = CreateThread( NULL, 0, show_thread, p, 0, NULL )))
    {
        dialog_Release( d );
        free( p );
        DeleteCriticalSection( &op->cs );
        free( op );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    CloseHandle( thread );
    *operation = &op->vtbl;
    return S_OK;
}
static HRESULT WINAPI dialog_get_Options( struct dialog *d, INT32 *value )
{
    if (!value) return E_POINTER;
    *value = d->options;
    return S_OK;
}
static HRESULT WINAPI dialog_put_Options( struct dialog *d, INT32 value )
{
    d->options = value;
    return S_OK;
}

static HRESULT dialog_create( HSTRING content, HSTRING title, void **out )
{
    struct dialog *d;

    if (!out) return E_POINTER;
    if (!(d = calloc( 1, sizeof(*d) )) || !(d->commands = commands_create()))
    {
        free( d );
        return E_OUTOFMEMORY;
    }
    d->vtbl = dialog_vtbl;
    d->refs = 1;
    WindowsDuplicateString( content, &d->content );
    WindowsDuplicateString( title, &d->title );
    *out = d;
    return S_OK;
}

/* ---- the factories ---- */

struct factory { const void **vtbl; const WCHAR *name; const GUID *iid; };

static const void *dialog_factory_vtbl[8], *command_factory_vtbl[9];
static struct factory dialog_factory = { dialog_factory_vtbl, dialog_class, &IID_IMessageDialogFactory_ };
static struct factory command_factory = { command_factory_vtbl, command_class, &IID_IUICommandFactory_ };

static HRESULT WINAPI factory_QueryInterface( struct factory *f, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject_ ) || IsEqualGUID( iid, &IID_IActivationFactory ) || IsEqualGUID( iid, f->iid ))
    {
        *out = f;
        return S_OK;
    }
    FIXME( "%s factory: %s not implemented, returning E_NOINTERFACE.\n", debugstr_w( f->name ), debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI factory_AddRef( struct factory *f ) { return 2; }
static ULONG WINAPI factory_Release( struct factory *f ) { return 1; }
static HRESULT WINAPI factory_GetRuntimeClassName( struct factory *f, HSTRING *name ) { return class_name( f->name, name ); }

/* IActivationFactory::ActivateInstance and the factory interface's first method would share slot 6; neither class is
 * default-constructible on Windows, so the slot is the factory's */
static HRESULT WINAPI dialog_factory_Create( struct factory *f, HSTRING content, void **out )
{
    return dialog_create( content, NULL, out );
}
static HRESULT WINAPI dialog_factory_CreateWithTitle( struct factory *f, HSTRING content, HSTRING title, void **out )
{
    return dialog_create( content, title, out );
}
static HRESULT WINAPI command_factory_Create( struct factory *f, HSTRING label, void **out )
{
    if (!out) return E_POINTER;
    return (*out = command_create( label, NULL, NULL )) ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI command_factory_CreateWithHandler( struct factory *f, HSTRING label, IUnknown *handler, void **out )
{
    if (!out) return E_POINTER;
    return (*out = command_create( label, handler, NULL )) ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI command_factory_CreateWithHandlerAndId( struct factory *f, HSTRING label, IUnknown *handler,
                                                              IUnknown *id, void **out )
{
    if (!out) return E_POINTER;
    return (*out = command_create( label, handler, id )) ? S_OK : E_OUTOFMEMORY;
}

static BOOL WINAPI init_vtables( INIT_ONCE *once, void *param, void **context )
{
    const void **v;

    v = command_vtbl;
    v[0] = command_QueryInterface; v[1] = command_AddRef; v[2] = command_Release; v[3] = any_GetIids;
    v[4] = command_GetRuntimeClassName; v[5] = any_GetTrustLevel; v[6] = command_get_Label; v[7] = command_put_Label;
    v[8] = command_get_Invoked; v[9] = command_put_Invoked; v[10] = command_get_Id; v[11] = command_put_Id;

    v = commands_vtbl;
    v[0] = commands_QueryInterface; v[1] = commands_AddRef; v[2] = commands_Release; v[3] = any_GetIids;
    v[4] = commands_GetRuntimeClassName; v[5] = any_GetTrustLevel; v[6] = commands_GetAt; v[7] = commands_get_Size;
    v[8] = commands_GetView; v[9] = commands_IndexOf; v[10] = commands_SetAt; v[11] = commands_InsertAt;
    v[12] = commands_RemoveAt; v[13] = commands_Append; v[14] = commands_RemoveAtEnd; v[15] = commands_Clear;
    v[16] = commands_GetMany; v[17] = commands_ReplaceAll;

    v = commands_view_vtbl;
    v[0] = view_QueryInterface; v[1] = view_AddRef; v[2] = view_Release; v[3] = any_GetIids;
    v[4] = view_GetRuntimeClassName; v[5] = any_GetTrustLevel; v[6] = view_GetAt; v[7] = view_get_Size;
    v[8] = view_IndexOf; v[9] = view_GetMany;

    v = show_vtbl;
    v[0] = show_QueryInterface; v[1] = show_AddRef; v[2] = show_Release; v[3] = any_GetIids;
    v[4] = show_GetRuntimeClassName; v[5] = any_GetTrustLevel; v[6] = show_put_Completed; v[7] = show_get_Completed;
    v[8] = show_GetResults;

    v = show_info_vtbl;
    v[0] = info_QueryInterface; v[1] = info_AddRef; v[2] = info_Release; v[3] = any_GetIids;
    v[4] = info_GetRuntimeClassName; v[5] = any_GetTrustLevel; v[6] = info_get_Id; v[7] = info_get_Status;
    v[8] = info_get_ErrorCode; v[9] = info_Cancel; v[10] = info_Close;

    v = dialog_vtbl;
    v[0] = dialog_QueryInterface; v[1] = dialog_AddRef; v[2] = dialog_Release; v[3] = any_GetIids;
    v[4] = dialog_GetRuntimeClassName; v[5] = any_GetTrustLevel; v[6] = dialog_get_Title; v[7] = dialog_put_Title;
    v[8] = dialog_get_Commands; v[9] = dialog_get_DefaultCommandIndex; v[10] = dialog_put_DefaultCommandIndex;
    v[11] = dialog_get_CancelCommandIndex; v[12] = dialog_put_CancelCommandIndex; v[13] = dialog_get_Content;
    v[14] = dialog_put_Content; v[15] = dialog_ShowAsync; v[16] = dialog_get_Options; v[17] = dialog_put_Options;

    v = dialog_factory_vtbl;
    v[0] = factory_QueryInterface; v[1] = factory_AddRef; v[2] = factory_Release; v[3] = any_GetIids;
    v[4] = factory_GetRuntimeClassName; v[5] = any_GetTrustLevel; v[6] = dialog_factory_Create;
    v[7] = dialog_factory_CreateWithTitle;

    v = command_factory_vtbl;
    v[0] = factory_QueryInterface; v[1] = factory_AddRef; v[2] = factory_Release; v[3] = any_GetIids;
    v[4] = factory_GetRuntimeClassName; v[5] = any_GetTrustLevel; v[6] = command_factory_Create;
    v[7] = command_factory_CreateWithHandler; v[8] = command_factory_CreateWithHandlerAndId;
    return TRUE;
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );
    InitOnceExecuteOnce( &once, init_vtables, NULL, NULL );
    *factory = NULL;
    if (!wcscmp( name, dialog_class )) *factory = (IActivationFactory *)&dialog_factory;
    else if (!wcscmp( name, command_class )) *factory = (IActivationFactory *)&command_factory;
    else return CLASS_E_CLASSNOTAVAILABLE;
    return S_OK;
}

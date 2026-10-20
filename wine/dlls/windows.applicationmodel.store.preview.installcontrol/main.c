/* WinRT Windows.ApplicationModel.Store.Preview.InstallControl Implementation
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
 * AppInstallManager is how the Microsoft Store installs and updates apps. Windows implements it in
 * InstallService.dll, a client of the InstallService *service*, a broker that downloads and deploys
 * packages; neither exists in a prefix, and the Store fail-fasts during start-up when
 * `new AppInstallManager()` throws.
 *
 * Here it answers honestly: no installs in progress, automatic updates as configured, nothing
 * blocked by policy, and updates for the running package. A search looks the package's family up in
 * the Store's catalogue and delivery service (appxinstall's appx_store_*), and if the Store has a
 * newer version, installing it stages that version over the running package's own stage directory
 * with appx_install, the way a package is installed in the first place. Only the running package is
 * looked at: the Store's Library lists nothing else here. Installing other products is refused.
 *
 * The package comes from WINE_UWP_PACKAGE_FULL_NAME and WINE_UWP_PACKAGE_PATH, the Windows tree for
 * the frameworks from WINE_UWP_WINDOWS_TREE, and launch defaults from WINE_UWP_ROOT, all set by
 * wine-uwp's launcher.
 */

#include <stdio.h>

#include "initguid.h"
#include "private.h"
#include "wine/appxinstall.h"
#include "wine/json.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(installcontrol);

#define CLASS_PREFIX L"Windows.ApplicationModel.Store.Preview.InstallControl."

static HRESULT get_iids( ULONG *iid_count, IID **iids )
{
    *iid_count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT class_name( const WCHAR *name, HSTRING *out )
{
    return WindowsCreateString( name, wcslen( name ), out );
}

/* Event handlers. A manager's handlers are invoked with that manager as the sender. */

struct handler
{
    IUnknown *handler;          /* TypedEventHandler<S, A>: Invoke(sender, args) is slot 3 */
    IInspectable *sender;
    INT64 token;
};

struct handlers
{
    struct handler *items;
    unsigned int count;
};

static SRWLOCK events_lock = SRWLOCK_INIT;
static LONG next_token;

static HRESULT add_handler( struct handlers *list, IUnknown *handler, IInspectable *sender, EventRegistrationToken *token )
{
    struct handler *items;

    if (!handler) return E_INVALIDARG;
    AcquireSRWLockExclusive( &events_lock );
    if (!(items = realloc( list->items, (list->count + 1) * sizeof(*items) )))
    {
        ReleaseSRWLockExclusive( &events_lock );
        return E_OUTOFMEMORY;
    }
    list->items = items;
    items[list->count].handler = handler;
    items[list->count].sender = sender;
    items[list->count].token = token->value = InterlockedIncrement( &next_token );
    list->count++;
    IUnknown_AddRef( handler );
    if (sender) IInspectable_AddRef( sender );
    ReleaseSRWLockExclusive( &events_lock );
    return S_OK;
}

static HRESULT remove_handler( struct handlers *list, EventRegistrationToken token )
{
    struct handler gone = {0};
    unsigned int i;

    AcquireSRWLockExclusive( &events_lock );
    for (i = 0; i < list->count; i++)
    {
        if (list->items[i].token != token.value) continue;
        gone = list->items[i];
        list->items[i] = list->items[--list->count];
        break;
    }
    ReleaseSRWLockExclusive( &events_lock );
    if (gone.handler) IUnknown_Release( gone.handler );
    if (gone.sender) IInspectable_Release( gone.sender );
    return S_OK;
}

static void raise_event( struct handlers *list, IInspectable *sender, IInspectable *args )
{
    typedef HRESULT (WINAPI *invoke_func)( IUnknown *handler, IInspectable *sender, IInspectable *args );
    struct handler *copy;
    unsigned int count, i;

    AcquireSRWLockShared( &events_lock );
    count = list->count;
    if ((copy = malloc( max( count, 1 ) * sizeof(*copy) )))
    {
        memcpy( copy, list->items, count * sizeof(*copy) );
        for (i = 0; i < count; i++)
        {
            IUnknown_AddRef( copy[i].handler );
            if (copy[i].sender) IInspectable_AddRef( copy[i].sender );
        }
    }
    ReleaseSRWLockShared( &events_lock );
    if (!copy) return;
    for (i = 0; i < count; i++)
    {
        ((invoke_func)(*(void ***)copy[i].handler)[3])( copy[i].handler, copy[i].sender ? copy[i].sender : sender, args );
        IUnknown_Release( copy[i].handler );
        if (copy[i].sender) IInspectable_Release( copy[i].sender );
    }
    free( copy );
}

static struct handlers manager_completed, manager_status_changed;

/* IVectorView<AppInstallItem>, with its IIterable */

struct item_list
{
    IVectorView_AppInstallItem IVectorView_AppInstallItem_iface;
    IIterable_AppInstallItem IIterable_AppInstallItem_iface;
    LONG ref;
    IAppInstallItem **items;
    unsigned int count;
};

struct item_iterator
{
    IIterator_AppInstallItem IIterator_AppInstallItem_iface;
    LONG ref;
    struct item_list *list;
    unsigned int index;
};

static inline struct item_list *impl_from_IVectorView_AppInstallItem( IVectorView_AppInstallItem *iface )
{
    return CONTAINING_RECORD( iface, struct item_list, IVectorView_AppInstallItem_iface );
}

static HRESULT WINAPI item_list_QueryInterface( IVectorView_AppInstallItem *iface, REFIID iid, void **out )
{
    struct item_list *impl = impl_from_IVectorView_AppInstallItem( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IVectorView_AppInstallItem ))
        *out = &impl->IVectorView_AppInstallItem_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_AppInstallItem ))
        *out = &impl->IIterable_AppInstallItem_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI item_list_AddRef( IVectorView_AppInstallItem *iface )
{
    return InterlockedIncrement( &impl_from_IVectorView_AppInstallItem( iface )->ref );
}

static ULONG WINAPI item_list_Release( IVectorView_AppInstallItem *iface )
{
    struct item_list *impl = impl_from_IVectorView_AppInstallItem( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    unsigned int i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IAppInstallItem_Release( impl->items[i] );
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI item_list_GetIids( IVectorView_AppInstallItem *iface, ULONG *iid_count, IID **iids )
{
    return get_iids( iid_count, iids );
}

static HRESULT WINAPI item_list_GetRuntimeClassName( IVectorView_AppInstallItem *iface, HSTRING *name )
{
    return class_name( L"Windows.Foundation.Collections.IVectorView`1<" CLASS_PREFIX L"AppInstallItem>", name );
}

static HRESULT WINAPI item_list_GetTrustLevel( IVectorView_AppInstallItem *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI item_list_GetAt( IVectorView_AppInstallItem *iface, UINT32 index, IAppInstallItem **value )
{
    struct item_list *impl = impl_from_IVectorView_AppInstallItem( iface );

    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    IAppInstallItem_AddRef( (*value = impl->items[index]) );
    return S_OK;
}

static HRESULT WINAPI item_list_get_Size( IVectorView_AppInstallItem *iface, UINT32 *value )
{
    *value = impl_from_IVectorView_AppInstallItem( iface )->count;
    return S_OK;
}

static HRESULT WINAPI item_list_IndexOf( IVectorView_AppInstallItem *iface, IAppInstallItem *element, UINT32 *index,
                                         boolean *found )
{
    struct item_list *impl = impl_from_IVectorView_AppInstallItem( iface );
    unsigned int i;

    *index = 0;
    *found = FALSE;
    for (i = 0; i < impl->count; i++)
    {
        if (impl->items[i] != element) continue;
        *index = i;
        *found = TRUE;
        break;
    }
    return S_OK;
}

static HRESULT WINAPI item_list_GetMany( IVectorView_AppInstallItem *iface, UINT32 start, UINT32 count,
                                         IAppInstallItem **items, UINT32 *written )
{
    struct item_list *impl = impl_from_IVectorView_AppInstallItem( iface );

    *written = 0;
    if (start > impl->count) return E_BOUNDS;
    while (*written < count && start + *written < impl->count)
    {
        IAppInstallItem_AddRef( (items[*written] = impl->items[start + *written]) );
        (*written)++;
    }
    return S_OK;
}

static const struct IVectorView_AppInstallItemVtbl item_list_vtbl =
{
    item_list_QueryInterface,
    item_list_AddRef,
    item_list_Release,
    /* IInspectable methods */
    item_list_GetIids,
    item_list_GetRuntimeClassName,
    item_list_GetTrustLevel,
    /* IVectorView<AppInstallItem> methods */
    item_list_GetAt,
    item_list_get_Size,
    item_list_IndexOf,
    item_list_GetMany,
};

static inline struct item_iterator *impl_from_IIterator_AppInstallItem( IIterator_AppInstallItem *iface )
{
    return CONTAINING_RECORD( iface, struct item_iterator, IIterator_AppInstallItem_iface );
}

static HRESULT WINAPI item_iterator_QueryInterface( IIterator_AppInstallItem *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_AppInstallItem ))
    {
        *out = iface;
        IInspectable_AddRef( (IInspectable *)iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI item_iterator_AddRef( IIterator_AppInstallItem *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_AppInstallItem( iface )->ref );
}

static ULONG WINAPI item_iterator_Release( IIterator_AppInstallItem *iface )
{
    struct item_iterator *impl = impl_from_IIterator_AppInstallItem( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVectorView_AppInstallItem_Release( &impl->list->IVectorView_AppInstallItem_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI item_iterator_GetIids( IIterator_AppInstallItem *iface, ULONG *iid_count, IID **iids )
{
    return get_iids( iid_count, iids );
}

static HRESULT WINAPI item_iterator_GetRuntimeClassName( IIterator_AppInstallItem *iface, HSTRING *name )
{
    return class_name( L"Windows.Foundation.Collections.IIterator`1<" CLASS_PREFIX L"AppInstallItem>", name );
}

static HRESULT WINAPI item_iterator_GetTrustLevel( IIterator_AppInstallItem *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI item_iterator_get_Current( IIterator_AppInstallItem *iface, IAppInstallItem **value )
{
    struct item_iterator *impl = impl_from_IIterator_AppInstallItem( iface );
    return item_list_GetAt( &impl->list->IVectorView_AppInstallItem_iface, impl->index, value );
}

static HRESULT WINAPI item_iterator_get_HasCurrent( IIterator_AppInstallItem *iface, boolean *value )
{
    struct item_iterator *impl = impl_from_IIterator_AppInstallItem( iface );

    *value = impl->index < impl->list->count;
    return S_OK;
}

static HRESULT WINAPI item_iterator_MoveNext( IIterator_AppInstallItem *iface, boolean *value )
{
    struct item_iterator *impl = impl_from_IIterator_AppInstallItem( iface );

    if (impl->index < impl->list->count) impl->index++;
    return item_iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI item_iterator_GetMany( IIterator_AppInstallItem *iface, UINT32 count, IAppInstallItem **items,
                                             UINT32 *written )
{
    struct item_iterator *impl = impl_from_IIterator_AppInstallItem( iface );
    HRESULT hr = item_list_GetMany( &impl->list->IVectorView_AppInstallItem_iface, impl->index, count, items, written );

    if (SUCCEEDED(hr)) impl->index += *written;
    return hr;
}

static const struct IIterator_AppInstallItemVtbl item_iterator_vtbl =
{
    item_iterator_QueryInterface,
    item_iterator_AddRef,
    item_iterator_Release,
    /* IInspectable methods */
    item_iterator_GetIids,
    item_iterator_GetRuntimeClassName,
    item_iterator_GetTrustLevel,
    /* IIterator<AppInstallItem> methods */
    item_iterator_get_Current,
    item_iterator_get_HasCurrent,
    item_iterator_MoveNext,
    item_iterator_GetMany,
};

DEFINE_IINSPECTABLE( item_iterable, IIterable_AppInstallItem, struct item_list, IVectorView_AppInstallItem_iface )

static HRESULT WINAPI item_iterable_First( IIterable_AppInstallItem *iface, IIterator_AppInstallItem **value )
{
    struct item_list *impl = impl_from_IIterable_AppInstallItem( iface );
    struct item_iterator *iter;

    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_AppInstallItem_iface.lpVtbl = &item_iterator_vtbl;
    iter->ref = 1;
    iter->list = impl;
    IVectorView_AppInstallItem_AddRef( &impl->IVectorView_AppInstallItem_iface );
    *value = &iter->IIterator_AppInstallItem_iface;
    return S_OK;
}

static const struct IIterable_AppInstallItemVtbl item_iterable_vtbl =
{
    item_iterable_QueryInterface,
    item_iterable_AddRef,
    item_iterable_Release,
    /* IInspectable methods */
    item_iterable_GetIids,
    item_iterable_GetRuntimeClassName,
    item_iterable_GetTrustLevel,
    /* IIterable<AppInstallItem> methods */
    item_iterable_First,
};

/* Takes the references in items, and the array. */
static HRESULT item_list_create( IAppInstallItem **items, unsigned int count, IVectorView_AppInstallItem **out )
{
    struct item_list *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_AppInstallItem_iface.lpVtbl = &item_list_vtbl;
    impl->IIterable_AppInstallItem_iface.lpVtbl = &item_iterable_vtbl;
    impl->ref = 1;
    impl->items = items;
    impl->count = count;
    *out = &impl->IVectorView_AppInstallItem_iface;
    return S_OK;
}

/* AppInstallStatus: a snapshot, as Windows' GetCurrentStatus hands out */

struct install_status
{
    IAppInstallStatus IAppInstallStatus_iface;
    IAppInstallStatus2 IAppInstallStatus2_iface;
    IAppInstallStatus3 IAppInstallStatus3_iface;
    LONG ref;
    AppInstallState state;
    UINT64 total, done;
    HRESULT error;
};

static inline struct install_status *impl_from_IAppInstallStatus( IAppInstallStatus *iface )
{
    return CONTAINING_RECORD( iface, struct install_status, IAppInstallStatus_iface );
}

static HRESULT WINAPI status_QueryInterface( IAppInstallStatus *iface, REFIID iid, void **out )
{
    struct install_status *impl = impl_from_IAppInstallStatus( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAppInstallStatus ))
        *out = &impl->IAppInstallStatus_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallStatus2 ))
        *out = &impl->IAppInstallStatus2_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallStatus3 ))
        *out = &impl->IAppInstallStatus3_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI status_AddRef( IAppInstallStatus *iface )
{
    return InterlockedIncrement( &impl_from_IAppInstallStatus( iface )->ref );
}

static ULONG WINAPI status_Release( IAppInstallStatus *iface )
{
    struct install_status *impl = impl_from_IAppInstallStatus( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI status_GetIids( IAppInstallStatus *iface, ULONG *iid_count, IID **iids )
{
    return get_iids( iid_count, iids );
}

static HRESULT WINAPI status_GetRuntimeClassName( IAppInstallStatus *iface, HSTRING *name )
{
    return class_name( RuntimeClass_Windows_ApplicationModel_Store_Preview_InstallControl_AppInstallStatus, name );
}

static HRESULT WINAPI status_GetTrustLevel( IAppInstallStatus *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI status_get_InstallState( IAppInstallStatus *iface, AppInstallState *value )
{
    *value = impl_from_IAppInstallStatus( iface )->state;
    return S_OK;
}

static HRESULT WINAPI status_get_DownloadSizeInBytes( IAppInstallStatus *iface, UINT64 *value )
{
    *value = impl_from_IAppInstallStatus( iface )->total;
    return S_OK;
}

static HRESULT WINAPI status_get_BytesDownloaded( IAppInstallStatus *iface, UINT64 *value )
{
    *value = impl_from_IAppInstallStatus( iface )->done;
    return S_OK;
}

static HRESULT WINAPI status_get_PercentComplete( IAppInstallStatus *iface, DOUBLE *value )
{
    struct install_status *impl = impl_from_IAppInstallStatus( iface );

    if (impl->state == AppInstallState_Completed) *value = 100.0;
    else *value = impl->total ? 100.0 * impl->done / impl->total : 0.0;
    return S_OK;
}

static HRESULT WINAPI status_get_ErrorCode( IAppInstallStatus *iface, HRESULT *value )
{
    *value = impl_from_IAppInstallStatus( iface )->error;
    return S_OK;
}

static const struct IAppInstallStatusVtbl status_vtbl =
{
    status_QueryInterface,
    status_AddRef,
    status_Release,
    /* IInspectable methods */
    status_GetIids,
    status_GetRuntimeClassName,
    status_GetTrustLevel,
    /* IAppInstallStatus methods */
    status_get_InstallState,
    status_get_DownloadSizeInBytes,
    status_get_BytesDownloaded,
    status_get_PercentComplete,
    status_get_ErrorCode,
};

DEFINE_IINSPECTABLE( status2, IAppInstallStatus2, struct install_status, IAppInstallStatus_iface )

static HRESULT WINAPI status2_get_User( IAppInstallStatus2 *iface, __x_ABI_CWindows_CSystem_CIUser **value )
{
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI status2_get_ReadyForLaunch( IAppInstallStatus2 *iface, boolean *value )
{
    *value = impl_from_IAppInstallStatus2( iface )->state == AppInstallState_Completed;
    return S_OK;
}

static const struct IAppInstallStatus2Vtbl status2_vtbl =
{
    status2_QueryInterface,
    status2_AddRef,
    status2_Release,
    /* IInspectable methods */
    status2_GetIids,
    status2_GetRuntimeClassName,
    status2_GetTrustLevel,
    /* IAppInstallStatus2 methods */
    status2_get_User,
    status2_get_ReadyForLaunch,
};

DEFINE_IINSPECTABLE( status3, IAppInstallStatus3, struct install_status, IAppInstallStatus_iface )

static HRESULT WINAPI status3_get_IsStaged( IAppInstallStatus3 *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static const struct IAppInstallStatus3Vtbl status3_vtbl =
{
    status3_QueryInterface,
    status3_AddRef,
    status3_Release,
    /* IInspectable methods */
    status3_GetIids,
    status3_GetRuntimeClassName,
    status3_GetTrustLevel,
    /* IAppInstallStatus3 methods */
    status3_get_IsStaged,
};

/* AppInstallItem: an update of the running package, and its install */

struct install_item
{
    IAppInstallItem IAppInstallItem_iface;
    IAppInstallItem2 IAppInstallItem2_iface;
    IAppInstallItem3 IAppInstallItem3_iface;
    IAppInstallItem4 IAppInstallItem4_iface;
    IAppInstallItem5 IAppInstallItem5_iface;
    LONG ref;
    WCHAR *product_id;
    WCHAR *family;
    WCHAR *stage;               /* the running package's stage directory, a Unix path */
    char *url;
    char version[32];
    struct handlers completed, status_changed;

    SRWLOCK lock;               /* guards the rest */
    AppInstallState state;
    UINT64 total, done;
    HRESULT error;
    BOOL started, canceled, launch_after;
    LONG cancel;                /* what appx_install polls */
};

static SRWLOCK items_lock = SRWLOCK_INIT;
static struct install_item **items;
static unsigned int items_count;

static inline struct install_item *impl_from_IAppInstallItem( IAppInstallItem *iface )
{
    return CONTAINING_RECORD( iface, struct install_item, IAppInstallItem_iface );
}

static HRESULT WINAPI item_QueryInterface( IAppInstallItem *iface, REFIID iid, void **out )
{
    struct install_item *impl = impl_from_IAppInstallItem( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAppInstallItem ))
        *out = &impl->IAppInstallItem_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallItem2 )) *out = &impl->IAppInstallItem2_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallItem3 )) *out = &impl->IAppInstallItem3_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallItem4 )) *out = &impl->IAppInstallItem4_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallItem5 )) *out = &impl->IAppInstallItem5_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI item_AddRef( IAppInstallItem *iface )
{
    return InterlockedIncrement( &impl_from_IAppInstallItem( iface )->ref );
}

static ULONG WINAPI item_Release( IAppInstallItem *iface )
{
    struct install_item *impl = impl_from_IAppInstallItem( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    /* the item list holds a reference for the life of the process */
    if (!ref)
    {
        free( impl->product_id );
        free( impl->family );
        free( impl->stage );
        free( impl->url );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI item_GetIids( IAppInstallItem *iface, ULONG *iid_count, IID **iids )
{
    return get_iids( iid_count, iids );
}

static HRESULT WINAPI item_GetRuntimeClassName( IAppInstallItem *iface, HSTRING *name )
{
    return class_name( RuntimeClass_Windows_ApplicationModel_Store_Preview_InstallControl_AppInstallItem, name );
}

static HRESULT WINAPI item_GetTrustLevel( IAppInstallItem *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI item_get_ProductId( IAppInstallItem *iface, HSTRING *value )
{
    return class_name( impl_from_IAppInstallItem( iface )->product_id, value );
}

static HRESULT WINAPI item_get_PackageFamilyName( IAppInstallItem *iface, HSTRING *value )
{
    return class_name( impl_from_IAppInstallItem( iface )->family, value );
}

static HRESULT WINAPI item_get_InstallType( IAppInstallItem *iface, AppInstallType *value )
{
    *value = AppInstallType_Update;
    return S_OK;
}

static HRESULT WINAPI item_get_IsUserInitiated( IAppInstallItem *iface, boolean *value )
{
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI item_GetCurrentStatus( IAppInstallItem *iface, IAppInstallStatus **value )
{
    struct install_item *impl = impl_from_IAppInstallItem( iface );
    struct install_status *status;

    if (!(status = calloc( 1, sizeof(*status) ))) return E_OUTOFMEMORY;
    status->IAppInstallStatus_iface.lpVtbl = &status_vtbl;
    status->IAppInstallStatus2_iface.lpVtbl = &status2_vtbl;
    status->IAppInstallStatus3_iface.lpVtbl = &status3_vtbl;
    status->ref = 1;
    AcquireSRWLockShared( &impl->lock );
    status->state = impl->state;
    status->total = impl->total;
    status->done = impl->done;
    status->error = impl->error;
    ReleaseSRWLockShared( &impl->lock );
    *value = &status->IAppInstallStatus_iface;
    return S_OK;
}

/* AppInstallManagerItemEventArgs */

struct item_event_args
{
    IAppInstallManagerItemEventArgs IAppInstallManagerItemEventArgs_iface;
    LONG ref;
    IAppInstallItem *item;
};

static inline struct item_event_args *impl_from_IAppInstallManagerItemEventArgs( IAppInstallManagerItemEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct item_event_args, IAppInstallManagerItemEventArgs_iface );
}

static HRESULT WINAPI event_args_QueryInterface( IAppInstallManagerItemEventArgs *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAppInstallManagerItemEventArgs ))
    {
        *out = iface;
        IInspectable_AddRef( (IInspectable *)iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI event_args_AddRef( IAppInstallManagerItemEventArgs *iface )
{
    return InterlockedIncrement( &impl_from_IAppInstallManagerItemEventArgs( iface )->ref );
}

static ULONG WINAPI event_args_Release( IAppInstallManagerItemEventArgs *iface )
{
    struct item_event_args *impl = impl_from_IAppInstallManagerItemEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IAppInstallItem_Release( impl->item );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI event_args_GetIids( IAppInstallManagerItemEventArgs *iface, ULONG *iid_count, IID **iids )
{
    return get_iids( iid_count, iids );
}

static HRESULT WINAPI event_args_GetRuntimeClassName( IAppInstallManagerItemEventArgs *iface, HSTRING *name )
{
    return class_name( RuntimeClass_Windows_ApplicationModel_Store_Preview_InstallControl_AppInstallManagerItemEventArgs,
                       name );
}

static HRESULT WINAPI event_args_GetTrustLevel( IAppInstallManagerItemEventArgs *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI event_args_get_Item( IAppInstallManagerItemEventArgs *iface, IAppInstallItem **value )
{
    IAppInstallItem_AddRef( (*value = impl_from_IAppInstallManagerItemEventArgs( iface )->item) );
    return S_OK;
}

static const struct IAppInstallManagerItemEventArgsVtbl event_args_vtbl =
{
    event_args_QueryInterface,
    event_args_AddRef,
    event_args_Release,
    /* IInspectable methods */
    event_args_GetIids,
    event_args_GetRuntimeClassName,
    event_args_GetTrustLevel,
    /* IAppInstallManagerItemEventArgs methods */
    event_args_get_Item,
};

/* Sets the state and tells the item's handlers and every manager's. */
static void item_set_state( struct install_item *impl, AppInstallState state, HRESULT error )
{
    struct item_event_args *args;
    IInspectable *self = (IInspectable *)&impl->IAppInstallItem_iface;

    AcquireSRWLockExclusive( &impl->lock );
    impl->state = state;
    if (FAILED(error)) impl->error = error;
    ReleaseSRWLockExclusive( &impl->lock );

    if (!(args = calloc( 1, sizeof(*args) ))) return;
    args->IAppInstallManagerItemEventArgs_iface.lpVtbl = &event_args_vtbl;
    args->ref = 1;
    IAppInstallItem_AddRef( (args->item = &impl->IAppInstallItem_iface) );

    raise_event( &impl->status_changed, self, NULL );
    raise_event( &manager_status_changed, NULL, (IInspectable *)&args->IAppInstallManagerItemEventArgs_iface );
    if (state == AppInstallState_Completed || state == AppInstallState_Canceled || state == AppInstallState_Error)
    {
        raise_event( &impl->completed, self, NULL );
        raise_event( &manager_completed, NULL, (IInspectable *)&args->IAppInstallManagerItemEventArgs_iface );
    }
    IAppInstallManagerItemEventArgs_Release( &args->IAppInstallManagerItemEventArgs_iface );
}

/* Installing: appx_install stages the update over the running package's stage directory, which is
 * DATADIR/stage/NAME. */

struct install_progress
{
    struct install_item *item;
    AppInstallState reported;
    ULONGLONG reported_time;
    BOOL installed;
};

static void CALLBACK install_event( void *context, enum appx_event event, UINT64 done, UINT64 total, const WCHAR *text )
{
    struct install_progress *progress = context;
    struct install_item *impl = progress->item;
    ULONGLONG now;

    switch (event)
    {
    case APPX_EVENT_STAGE:
        TRACE( "%s.\n", debugstr_w( text ) );
        if (progress->reported == AppInstallState_Downloading && wcsstr( text, L"framework" ))
            item_set_state( impl, (progress->reported = AppInstallState_Installing), S_OK );
        break;
    case APPX_EVENT_MESSAGE:
        MESSAGE( "wine: store update: %s\n", debugstr_w( text ) );
        break;
    case APPX_EVENT_INSTALLED:
        progress->installed = TRUE;
        break;
    case APPX_EVENT_PROGRESS:
        if (!total) break;
        AcquireSRWLockExclusive( &impl->lock );
        impl->total = total;
        impl->done = min( done, total );
        ReleaseSRWLockExclusive( &impl->lock );
        now = GetTickCount64();
        if (progress->reported != AppInstallState_Downloading || now - progress->reported_time >= 500 || done >= total)
        {
            progress->reported_time = now;
            item_set_state( impl, (progress->reported = AppInstallState_Downloading), S_OK );
        }
        break;
    }
}

/* A Unix path as a Windows one, allocated. */
static WCHAR *dos_path( const WCHAR *unix_path, const WCHAR *suffix )
{
    size_t len = wcslen( unix_path ) + wcslen( suffix ) + 3;
    WCHAR *path, *p;

    if (!(path = malloc( len * sizeof(WCHAR) ))) return NULL;
    swprintf( path, len, L"Z:%s%s", unix_path, suffix );
    for (p = path; *p; p++) if (*p == '/') *p = '\\';
    return path;
}

static WCHAR *get_env( const WCHAR *name )
{
    DWORD len = GetEnvironmentVariableW( name, NULL, 0 );
    WCHAR *value;

    if (!len || !(value = malloc( len * sizeof(WCHAR) ))) return NULL;
    GetEnvironmentVariableW( name, value, len );
    return value;
}

static DWORD WINAPI install_thread( void *param )
{
    struct install_item *impl = param;
    struct appx_install_params params = {0};
    struct install_progress progress = {impl, AppInstallState_Starting};
    WCHAR *tree = get_env( L"WINE_UWP_WINDOWS_TREE" ), *root = get_env( L"WINE_UWP_ROOT" ), *slash, *stage_root;
    WCHAR *package = NULL, *data = NULL, *tree_dos = NULL, *defaults = NULL;
    HRESULT hr = HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    BOOL canceled;
    int len;

    /* DATADIR/stage/NAME */
    stage_root = wcsdup( impl->stage );
    if (tree && stage_root && (slash = wcsrchr( stage_root, '/' )))
    {
        params.stage = slash + 1;
        *slash = 0;
        if ((slash = wcsrchr( stage_root, '/' )) && !wcscmp( slash, L"/stage" ))
        {
            *slash = 0;
            data = dos_path( stage_root, L"" );
            tree_dos = dos_path( tree, L"" );
            if (root) defaults = dos_path( root, L"/launch-defaults.env" );
            len = MultiByteToWideChar( CP_UTF8, 0, impl->url, -1, NULL, 0 );
            if ((package = malloc( len * sizeof(WCHAR) ))) MultiByteToWideChar( CP_UTF8, 0, impl->url, -1, package, len );
        }
    }
    if (package && data && tree_dos)
    {
        params.package = package;
        params.data_dir = data;
        params.windows_tree = tree_dos;
        params.launch_defaults = defaults;
        params.cancel = &impl->cancel;
        params.callback = install_event;
        params.context = &progress;
        item_set_state( impl, AppInstallState_Starting, S_OK );
        MESSAGE( "wine: store update: installing %s %s into stage %s\n", debugstr_w( impl->family ),
                 debugstr_a( impl->version ), debugstr_w( params.stage ) );
        hr = appx_install( &params );
    }
    else ERR( "no Windows tree or stage directory (%s).\n", debugstr_w( impl->stage ) );

    AcquireSRWLockShared( &impl->lock );
    canceled = impl->canceled;
    ReleaseSRWLockShared( &impl->lock );
    TRACE( "installer -> %#lx%s.\n", hr, progress.installed ? ", installed" : "" );
    if (SUCCEEDED(hr) && progress.installed) item_set_state( impl, AppInstallState_Completed, S_OK );
    else if (canceled) item_set_state( impl, AppInstallState_Canceled, S_OK );
    else item_set_state( impl, AppInstallState_Error, FAILED(hr) ? hr : E_FAIL );

    free( package );
    free( data );
    free( tree_dos );
    free( defaults );
    free( stage_root );
    free( tree );
    free( root );
    IAppInstallItem_Release( &impl->IAppInstallItem_iface );
    return 0;
}

static void item_start( struct install_item *impl )
{
    HANDLE thread;
    BOOL go;

    AcquireSRWLockExclusive( &impl->lock );
    if ((go = !impl->started || impl->state == AppInstallState_Error || impl->state == AppInstallState_Canceled))
    {
        impl->started = TRUE;
        impl->canceled = FALSE;
        impl->cancel = 0;
        impl->state = AppInstallState_Pending;
        impl->done = 0;
        impl->error = S_OK;
    }
    ReleaseSRWLockExclusive( &impl->lock );
    if (!go) return;

    IAppInstallItem_AddRef( &impl->IAppInstallItem_iface );
    if ((thread = CreateThread( NULL, 0, install_thread, impl, 0, NULL ))) CloseHandle( thread );
    else
    {
        IAppInstallItem_Release( &impl->IAppInstallItem_iface );
        item_set_state( impl, AppInstallState_Error, HRESULT_FROM_WIN32( GetLastError() ) );
    }
}

static HRESULT WINAPI item_Cancel( IAppInstallItem *iface )
{
    struct install_item *impl = impl_from_IAppInstallItem( iface );
    BOOL running;

    TRACE( "iface %p.\n", iface );

    AcquireSRWLockExclusive( &impl->lock );
    impl->canceled = TRUE;
    InterlockedExchange( &impl->cancel, 1 );
    running = impl->started && impl->state != AppInstallState_Completed && impl->state != AppInstallState_Error &&
              impl->state != AppInstallState_Canceled;
    ReleaseSRWLockExclusive( &impl->lock );
    if (!running) item_set_state( impl, AppInstallState_Canceled, S_OK );
    return S_OK;
}

static HRESULT WINAPI item_Pause( IAppInstallItem *iface )
{
    FIXME( "iface %p: pausing is not supported, the install goes on.\n", iface );
    return S_OK;
}

static HRESULT WINAPI item_Restart( IAppInstallItem *iface )
{
    TRACE( "iface %p.\n", iface );
    item_start( impl_from_IAppInstallItem( iface ) );
    return S_OK;
}

static HRESULT WINAPI item_add_Completed( IAppInstallItem *iface, ITypedEventHandler_AppInstallItem_IInspectable *handler,
                                          EventRegistrationToken *token )
{
    return add_handler( &impl_from_IAppInstallItem( iface )->completed, (IUnknown *)handler, NULL, token );
}

static HRESULT WINAPI item_remove_Completed( IAppInstallItem *iface, EventRegistrationToken token )
{
    return remove_handler( &impl_from_IAppInstallItem( iface )->completed, token );
}

static HRESULT WINAPI item_add_StatusChanged( IAppInstallItem *iface, ITypedEventHandler_AppInstallItem_IInspectable *handler,
                                              EventRegistrationToken *token )
{
    return add_handler( &impl_from_IAppInstallItem( iface )->status_changed, (IUnknown *)handler, NULL, token );
}

static HRESULT WINAPI item_remove_StatusChanged( IAppInstallItem *iface, EventRegistrationToken token )
{
    return remove_handler( &impl_from_IAppInstallItem( iface )->status_changed, token );
}

static const struct IAppInstallItemVtbl item_vtbl =
{
    item_QueryInterface,
    item_AddRef,
    item_Release,
    /* IInspectable methods */
    item_GetIids,
    item_GetRuntimeClassName,
    item_GetTrustLevel,
    /* IAppInstallItem methods */
    item_get_ProductId,
    item_get_PackageFamilyName,
    item_get_InstallType,
    item_get_IsUserInitiated,
    item_GetCurrentStatus,
    item_Cancel,
    item_Pause,
    item_Restart,
    item_add_Completed,
    item_remove_Completed,
    item_add_StatusChanged,
    item_remove_StatusChanged,
};

DEFINE_IINSPECTABLE( item2, IAppInstallItem2, struct install_item, IAppInstallItem_iface )

static HRESULT WINAPI item2_CancelWithTelemetry( IAppInstallItem2 *iface, HSTRING correlation_vector )
{
    return item_Cancel( &impl_from_IAppInstallItem2( iface )->IAppInstallItem_iface );
}

static HRESULT WINAPI item2_PauseWithTelemetry( IAppInstallItem2 *iface, HSTRING correlation_vector )
{
    return item_Pause( &impl_from_IAppInstallItem2( iface )->IAppInstallItem_iface );
}

static HRESULT WINAPI item2_RestartWithTelemetry( IAppInstallItem2 *iface, HSTRING correlation_vector )
{
    return item_Restart( &impl_from_IAppInstallItem2( iface )->IAppInstallItem_iface );
}

static const struct IAppInstallItem2Vtbl item2_vtbl =
{
    item2_QueryInterface,
    item2_AddRef,
    item2_Release,
    /* IInspectable methods */
    item2_GetIids,
    item2_GetRuntimeClassName,
    item2_GetTrustLevel,
    /* IAppInstallItem2 methods */
    item2_CancelWithTelemetry,
    item2_PauseWithTelemetry,
    item2_RestartWithTelemetry,
};

DEFINE_IINSPECTABLE( item3, IAppInstallItem3, struct install_item, IAppInstallItem_iface )

static HRESULT WINAPI item3_get_Children( IAppInstallItem3 *iface, IVectorView_AppInstallItem **value )
{
    return item_list_create( NULL, 0, value );
}

static HRESULT WINAPI item3_get_ItemOperationsMightAffectOtherItems( IAppInstallItem3 *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static const struct IAppInstallItem3Vtbl item3_vtbl =
{
    item3_QueryInterface,
    item3_AddRef,
    item3_Release,
    /* IInspectable methods */
    item3_GetIids,
    item3_GetRuntimeClassName,
    item3_GetTrustLevel,
    /* IAppInstallItem3 methods */
    item3_get_Children,
    item3_get_ItemOperationsMightAffectOtherItems,
};

DEFINE_IINSPECTABLE( item4, IAppInstallItem4, struct install_item, IAppInstallItem_iface )

static HRESULT WINAPI item4_get_LaunchAfterInstall( IAppInstallItem4 *iface, boolean *value )
{
    *value = impl_from_IAppInstallItem4( iface )->launch_after;
    return S_OK;
}

static HRESULT WINAPI item4_put_LaunchAfterInstall( IAppInstallItem4 *iface, boolean value )
{
    impl_from_IAppInstallItem4( iface )->launch_after = value;
    return S_OK;
}

static const struct IAppInstallItem4Vtbl item4_vtbl =
{
    item4_QueryInterface,
    item4_AddRef,
    item4_Release,
    /* IInspectable methods */
    item4_GetIids,
    item4_GetRuntimeClassName,
    item4_GetTrustLevel,
    /* IAppInstallItem4 methods */
    item4_get_LaunchAfterInstall,
    item4_put_LaunchAfterInstall,
};

DEFINE_IINSPECTABLE( item5, IAppInstallItem5, struct install_item, IAppInstallItem_iface )

/* There is no Start menu, taskbar or desktop to pin to, nor install toasts. */
static HRESULT WINAPI item5_get_bool( IAppInstallItem5 *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI item5_put_bool( IAppInstallItem5 *iface, boolean value )
{
    return S_OK;
}

static HRESULT WINAPI item5_get_mode( IAppInstallItem5 *iface, AppInstallationToastNotificationMode *value )
{
    *value = AppInstallationToastNotificationMode_Default;
    return S_OK;
}

static HRESULT WINAPI item5_put_mode( IAppInstallItem5 *iface, AppInstallationToastNotificationMode value )
{
    return S_OK;
}

static const struct IAppInstallItem5Vtbl item5_vtbl =
{
    item5_QueryInterface,
    item5_AddRef,
    item5_Release,
    /* IInspectable methods */
    item5_GetIids,
    item5_GetRuntimeClassName,
    item5_GetTrustLevel,
    /* IAppInstallItem5 methods */
    item5_get_bool,     /* PinToDesktopAfterInstall */
    item5_put_bool,
    item5_get_bool,     /* PinToStartAfterInstall */
    item5_put_bool,
    item5_get_bool,     /* PinToTaskbarAfterInstall */
    item5_put_bool,
    item5_get_mode,     /* CompletedInstallToastNotificationMode */
    item5_put_mode,
    item5_get_mode,     /* InstallInProgressToastNotificationMode */
    item5_put_mode,
};

/* Searching */

/* The running package's family, version and stage directory (a Unix path). */
static BOOL own_package( WCHAR **family, char *version, size_t version_size, WCHAR **stage )
{
    WCHAR *full = get_env( L"WINE_UWP_PACKAGE_FULL_NAME" ), *path = get_env( L"WINE_UWP_PACKAGE_PATH" );
    WCHAR *first, *second, *last, *p;
    BOOL ret = FALSE;

    *family = *stage = NULL;
    /* Name_Version_Arch_ResourceId_PublisherId */
    if (full && path && !wcsncmp( path, L"Z:", 2 ) && (first = wcschr( full, '_' )) &&
        (second = wcschr( first + 1, '_' )) && (last = wcsrchr( full, '_' )) && second - first - 1 < version_size)
    {
        WideCharToMultiByte( CP_UTF8, 0, first + 1, second - first - 1, version, version_size, NULL, NULL );
        version[second - first - 1] = 0;
        if ((*family = malloc( (first - full + wcslen( last ) + 1) * sizeof(WCHAR) )))
        {
            memcpy( *family, full, (first - full) * sizeof(WCHAR) );
            wcscpy( *family + (first - full), last );
        }
        if ((*stage = wcsdup( path + 2 )))
        {
            for (p = *stage; *p; p++) if (*p == '\\') *p = '/';
            while (p > *stage && p[-1] == '/') *--p = 0;
        }
        ret = *family && *stage;
    }
    if (!ret)
    {
        free( *family );
        free( *stage );
        *family = *stage = NULL;
    }
    free( full );
    free( path );
    return ret;
}

static ULONGLONG version_number( const char *version )
{
    unsigned int v[4] = {0};
    sscanf( version, "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3] );
    return ((ULONGLONG)v[0] << 48) | ((ULONGLONG)v[1] << 32) | ((ULONGLONG)v[2] << 16) | v[3];
}

/* The catalogue: a package family's product id and Windows Update category. */
static BOOL catalog_lookup( const WCHAR *family, WCHAR **product_id, char *category, size_t size )
{
    const struct json *product, *skus;
    struct json *root = NULL;
    WCHAR path[512];
    DWORD status;
    char *body;
    unsigned int i;
    int len;

    *product_id = NULL;
    category[0] = 0;
    swprintf( path, ARRAY_SIZE(path), L"/v7.0/products/lookup?alternateId=PackageFamilyName&value=%s"
              L"&market=US&languages=en-US&fieldsTemplate=details", family );
    if (FAILED(appx_store_get( L"displaycatalog.mp.microsoft.com", path, &body, &status ))) return FALSE;
    if (status == 200) root = json_parse( body, strlen( body ) );
    appx_free( body );

    product = json_at( json_get( root, "Products" ), 0 );
    if (product->type != JSON_OBJECT)
    {
        WARN( "catalogue: HTTP %lu, no product for %s.\n", status, debugstr_w( family ) );
        json_free( root );
        return FALSE;
    }
    len = MultiByteToWideChar( CP_UTF8, 0, json_str( json_get( product, "ProductId" ) ), -1, NULL, 0 );
    if ((*product_id = malloc( len * sizeof(WCHAR) )))
        MultiByteToWideChar( CP_UTF8, 0, json_str( json_get( product, "ProductId" ) ), -1, *product_id, len );
    skus = json_get( product, "DisplaySkuAvailabilities" );
    for (i = 0; i < json_count( skus ) && !category[0]; i++)
        lstrcpynA( category, json_str( json_get( json_get( json_get( json_get( json_at( skus, i ), "Sku" ), "Properties" ),
                                                           "FulfillmentData" ), "WuCategoryId" ) ), size );
    json_free( root );
    return *product_id && category[0];
}

static struct install_item *find_item( const WCHAR *family )
{
    struct install_item *found = NULL;
    unsigned int i;

    AcquireSRWLockShared( &items_lock );
    for (i = 0; i < items_count; i++)
    {
        if (wcscmp( items[i]->family, family )) continue;
        IAppInstallItem_AddRef( &(found = items[i])->IAppInstallItem_iface );
        break;
    }
    ReleaseSRWLockShared( &items_lock );
    return found;
}

static HRESULT item_create( WCHAR *product_id, WCHAR *family, WCHAR *stage, char *url, const char *version,
                            struct install_item **out )
{
    struct install_item *impl, **grown;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAppInstallItem_iface.lpVtbl = &item_vtbl;
    impl->IAppInstallItem2_iface.lpVtbl = &item2_vtbl;
    impl->IAppInstallItem3_iface.lpVtbl = &item3_vtbl;
    impl->IAppInstallItem4_iface.lpVtbl = &item4_vtbl;
    impl->IAppInstallItem5_iface.lpVtbl = &item5_vtbl;
    impl->ref = 2;              /* the caller's and the item list's */
    impl->product_id = product_id;
    impl->family = family;
    impl->stage = stage;
    impl->url = url;
    lstrcpynA( impl->version, version, sizeof(impl->version) );
    InitializeSRWLock( &impl->lock );
    impl->state = AppInstallState_ReadyToDownload;

    AcquireSRWLockExclusive( &items_lock );
    if (!(grown = realloc( items, (items_count + 1) * sizeof(*items) )))
    {
        ReleaseSRWLockExclusive( &items_lock );
        free( impl );
        return E_OUTOFMEMORY;
    }
    items = grown;
    items[items_count++] = impl;
    ReleaseSRWLockExclusive( &items_lock );
    *out = impl;
    return S_OK;
}

/* An item for an update of the running package newer than what is staged, or NULL. */
static struct install_item *search_update( BOOL auto_install )
{
    WCHAR *family, *stage, *product_id = NULL;
    struct appx_store_package package;
    char installed[32], category[64], *url = NULL, *newest, *end;
    struct install_item *item = NULL;
    HRESULT hr;

    if (!own_package( &family, installed, sizeof(installed), &stage ))
    {
        WARN( "no package identity.\n" );
        return NULL;
    }
    if ((item = find_item( family )))
    {
        if (auto_install) item_start( item );
        free( family );
        free( stage );
        return item;
    }

    if (!catalog_lookup( family, &product_id, category, sizeof(category) )) goto done;
    if (FAILED(hr = appx_store_find_package( category, family, &package )))
    {
        WARN( "%s: the Store has no package, hr %#lx.\n", debugstr_w( family ), hr );
        goto done;
    }
    newest = strchr( package.moniker, '_' ) + 1;
    if ((end = strchr( newest, '_' ))) *end = 0;
    MESSAGE( "wine: store update: %s has %s, the Store %s\n", debugstr_w( family ), installed, newest );
    if (version_number( newest ) <= version_number( installed )) goto done;
    if (end) *end = '_';
    if (FAILED(appx_store_package_url( &package, &url ))) goto done;
    if (end) *end = 0;
    if (SUCCEEDED(item_create( product_id, family, stage, url, newest, &item )))
    {
        product_id = NULL;
        family = stage = NULL;
        url = NULL;
        if (auto_install) item_start( item );
    }

done:
    free( product_id );
    free( family );
    free( stage );
    appx_free( url );
    return item;
}

/* What a search operation is asked to do; its async parameter. */
struct search
{
    IUnknown IUnknown_iface;
    LONG ref;
    BOOL all;
    BOOL auto_install;
};

static inline struct search *impl_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct search, IUnknown_iface );
}

static HRESULT WINAPI search_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ))
    {
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI search_AddRef( IUnknown *iface )
{
    return InterlockedIncrement( &impl_from_IUnknown( iface )->ref );
}

static ULONG WINAPI search_Release( IUnknown *iface )
{
    struct search *impl = impl_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static const IUnknownVtbl search_vtbl =
{
    search_QueryInterface,
    search_AddRef,
    search_Release,
};

static HRESULT search_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct search *search;
    struct install_item *item;
    IVectorView_AppInstallItem *list;
    IAppInstallItem **array = NULL;
    HRESULT hr;

    /* the network: on the thread pool, not the caller's thread */
    if (!called_async) return STATUS_PENDING;
    if (!param)
    {
        result->vt = VT_UNKNOWN;
        result->punkVal = NULL;
        return S_OK;
    }
    search = impl_from_IUnknown( param );
    item = search_update( search->auto_install );
    TRACE( "-> %s.\n", item ? "an update" : "no update" );
    result->vt = VT_UNKNOWN;
    if (!search->all)
    {
        result->punkVal = item ? (IUnknown *)&item->IAppInstallItem_iface : NULL;
        return S_OK;
    }
    if (item && !(array = malloc( sizeof(*array) )))
    {
        IAppInstallItem_Release( &item->IAppInstallItem_iface );
        return E_OUTOFMEMORY;
    }
    if (item) array[0] = &item->IAppInstallItem_iface;
    if (FAILED(hr = item_list_create( array, item ? 1 : 0, &list )))
    {
        if (item) IAppInstallItem_Release( &item->IAppInstallItem_iface );
        free( array );
        return hr;
    }
    result->punkVal = (IUnknown *)list;
    return S_OK;
}

static HRESULT search_create( BOOL all, BOOL auto_install, IUnknown **out )
{
    struct search *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUnknown_iface.lpVtbl = &search_vtbl;
    impl->ref = 1;
    impl->all = all;
    impl->auto_install = auto_install;
    *out = &impl->IUnknown_iface;
    return S_OK;
}

static BOOL auto_install_of( IAppUpdateOptions *options )
{
    IAppUpdateOptions2 *options2;
    boolean value = FALSE;

    if (options && SUCCEEDED(IAppUpdateOptions_QueryInterface( options, &IID_IAppUpdateOptions2, (void **)&options2 )))
    {
        IAppUpdateOptions2_get_AutomaticallyDownloadAndInstallUpdateIfFound( options2, &value );
        IAppUpdateOptions2_Release( options2 );
    }
    return value;
}

static HRESULT search_one_async( IAppUpdateOptions *options, IAsyncOperation_AppInstallItem **operation )
{
    IUnknown *search;
    HRESULT hr;

    *operation = NULL;
    if (FAILED(hr = search_create( FALSE, auto_install_of( options ), &search ))) return hr;
    hr = async_operation_item_create( NULL, search, search_callback, operation );
    IUnknown_Release( search );
    return hr;
}

static HRESULT search_all_async( IAppUpdateOptions *options, IAsyncOperation_IVectorView_AppInstallItem **operation )
{
    IUnknown *search;
    HRESULT hr;

    *operation = NULL;
    if (FAILED(hr = search_create( TRUE, auto_install_of( options ), &search ))) return hr;
    hr = async_operation_items_create( NULL, search, search_callback, operation );
    IUnknown_Release( search );
    return hr;
}

/* Only the running package is updated; for any other family there is no update. */
static HRESULT update_family_async( HSTRING family, IAsyncOperation_AppInstallItem **operation )
{
    WCHAR *own, *stage;
    char version[32];
    BOOL ours;

    ours = own_package( &own, version, sizeof(version), &stage ) && !wcscmp( own, WindowsGetStringRawBuffer( family, NULL ) );
    free( own );
    free( stage );
    if (ours) return search_one_async( NULL, operation );
    TRACE( "%s is not the running package.\n", debugstr_hstring( family ) );
    return async_operation_item_create( NULL, NULL, search_callback, operation );
}

static HRESULT return_bool( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_BOOL;
    result->boolVal = param ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

/* The parameter is only a flag: any object makes the result TRUE. */
static HRESULT bool_async( BOOL value, IAsyncOperation_boolean **operation )
{
    IUnknown *flag = NULL;
    HRESULT hr;

    *operation = NULL;
    if (value && FAILED(hr = search_create( FALSE, FALSE, &flag ))) return hr;
    hr = async_operation_boolean_create( NULL, flag, return_bool, operation );
    if (flag) IUnknown_Release( flag );
    return hr;
}

static HRESULT current_items( IVectorView_AppInstallItem **value )
{
    IAppInstallItem **array;
    unsigned int i, count = 0;
    BOOL active;
    HRESULT hr;

    AcquireSRWLockShared( &items_lock );
    if (!(array = malloc( max( items_count, 1 ) * sizeof(*array) )))
    {
        ReleaseSRWLockShared( &items_lock );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < items_count; i++)
    {
        AcquireSRWLockShared( &items[i]->lock );
        active = items[i]->started && items[i]->state != AppInstallState_Completed &&
                 items[i]->state != AppInstallState_Canceled && items[i]->state != AppInstallState_Error;
        ReleaseSRWLockShared( &items[i]->lock );
        if (active) IAppInstallItem_AddRef( (array[count++] = &items[i]->IAppInstallItem_iface) );
    }
    ReleaseSRWLockShared( &items_lock );
    TRACE( "%u in progress.\n", count );
    if (FAILED(hr = item_list_create( array, count, value )))
    {
        while (count) IAppInstallItem_Release( array[--count] );
        free( array );
    }
    return hr;
}

/* The manager's Cancel, Pause and Restart name an item by product id. */
enum item_operation { ITEM_CANCEL, ITEM_PAUSE, ITEM_RESTART };

static HRESULT by_product( HSTRING product_id, enum item_operation operation )
{
    const WCHAR *id = WindowsGetStringRawBuffer( product_id, NULL );
    struct install_item *found = NULL;
    unsigned int i;

    AcquireSRWLockShared( &items_lock );
    for (i = 0; i < items_count; i++)
    {
        if (wcsicmp( items[i]->product_id, id )) continue;
        IAppInstallItem_AddRef( &(found = items[i])->IAppInstallItem_iface );
        break;
    }
    ReleaseSRWLockShared( &items_lock );
    TRACE( "%s, operation %d%s.\n", debugstr_w( id ), operation, found ? "" : ": no such item" );
    if (!found) return S_OK;
    switch (operation)
    {
    case ITEM_CANCEL: item_Cancel( &found->IAppInstallItem_iface ); break;
    case ITEM_PAUSE: item_Pause( &found->IAppInstallItem_iface ); break;
    case ITEM_RESTART: item_Restart( &found->IAppInstallItem_iface ); break;
    }
    IAppInstallItem_Release( &found->IAppInstallItem_iface );
    return S_OK;
}

/* AppUpdateOptions: a property bag */

struct update_options
{
    IAppUpdateOptions IAppUpdateOptions_iface;
    IAppUpdateOptions2 IAppUpdateOptions2_iface;
    LONG ref;
    HSTRING catalog_id;
    boolean forced_restart;
    boolean auto_install;
};

static inline struct update_options *impl_from_IAppUpdateOptions( IAppUpdateOptions *iface )
{
    return CONTAINING_RECORD( iface, struct update_options, IAppUpdateOptions_iface );
}

static HRESULT WINAPI options_QueryInterface( IAppUpdateOptions *iface, REFIID iid, void **out )
{
    struct update_options *impl = impl_from_IAppUpdateOptions( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAppUpdateOptions ))
        *out = &impl->IAppUpdateOptions_iface;
    else if (IsEqualGUID( iid, &IID_IAppUpdateOptions2 ))
        *out = &impl->IAppUpdateOptions2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI options_AddRef( IAppUpdateOptions *iface )
{
    return InterlockedIncrement( &impl_from_IAppUpdateOptions( iface )->ref );
}

static ULONG WINAPI options_Release( IAppUpdateOptions *iface )
{
    struct update_options *impl = impl_from_IAppUpdateOptions( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->catalog_id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI options_GetIids( IAppUpdateOptions *iface, ULONG *iid_count, IID **iids )
{
    return get_iids( iid_count, iids );
}

static HRESULT WINAPI options_GetRuntimeClassName( IAppUpdateOptions *iface, HSTRING *name )
{
    return class_name( RuntimeClass_Windows_ApplicationModel_Store_Preview_InstallControl_AppUpdateOptions, name );
}

static HRESULT WINAPI options_GetTrustLevel( IAppUpdateOptions *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI options_get_CatalogId( IAppUpdateOptions *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IAppUpdateOptions( iface )->catalog_id, value );
}

static HRESULT WINAPI options_put_CatalogId( IAppUpdateOptions *iface, HSTRING value )
{
    struct update_options *impl = impl_from_IAppUpdateOptions( iface );
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->catalog_id );
    impl->catalog_id = copy;
    return S_OK;
}

static HRESULT WINAPI options_get_AllowForcedAppRestart( IAppUpdateOptions *iface, boolean *value )
{
    *value = impl_from_IAppUpdateOptions( iface )->forced_restart;
    return S_OK;
}

static HRESULT WINAPI options_put_AllowForcedAppRestart( IAppUpdateOptions *iface, boolean value )
{
    impl_from_IAppUpdateOptions( iface )->forced_restart = value;
    return S_OK;
}

static const struct IAppUpdateOptionsVtbl options_vtbl =
{
    options_QueryInterface,
    options_AddRef,
    options_Release,
    /* IInspectable methods */
    options_GetIids,
    options_GetRuntimeClassName,
    options_GetTrustLevel,
    /* IAppUpdateOptions methods */
    options_get_CatalogId,
    options_put_CatalogId,
    options_get_AllowForcedAppRestart,
    options_put_AllowForcedAppRestart,
};

DEFINE_IINSPECTABLE( options2, IAppUpdateOptions2, struct update_options, IAppUpdateOptions_iface )

static HRESULT WINAPI options2_get_AutomaticallyDownloadAndInstallUpdateIfFound( IAppUpdateOptions2 *iface, boolean *value )
{
    *value = impl_from_IAppUpdateOptions2( iface )->auto_install;
    return S_OK;
}

static HRESULT WINAPI options2_put_AutomaticallyDownloadAndInstallUpdateIfFound( IAppUpdateOptions2 *iface, boolean value )
{
    impl_from_IAppUpdateOptions2( iface )->auto_install = value;
    return S_OK;
}

static const struct IAppUpdateOptions2Vtbl options2_vtbl =
{
    options2_QueryInterface,
    options2_AddRef,
    options2_Release,
    /* IInspectable methods */
    options2_GetIids,
    options2_GetRuntimeClassName,
    options2_GetTrustLevel,
    /* IAppUpdateOptions2 methods */
    options2_get_AutomaticallyDownloadAndInstallUpdateIfFound,
    options2_put_AutomaticallyDownloadAndInstallUpdateIfFound,
};

/* AppInstallManager */

static AutoUpdateSetting auto_update_setting = AutoUpdateSetting_Enabled;

struct manager
{
    IAppInstallManager IAppInstallManager_iface;
    IAppInstallManager2 IAppInstallManager2_iface;
    IAppInstallManager3 IAppInstallManager3_iface;
    IAppInstallManager4 IAppInstallManager4_iface;
    IAppInstallManager5 IAppInstallManager5_iface;
    IAppInstallManager6 IAppInstallManager6_iface;
    IAppInstallManager7 IAppInstallManager7_iface;
    LONG ref;
    HSTRING acquisition_identity;
};

static inline struct manager *impl_from_IAppInstallManager( IAppInstallManager *iface )
{
    return CONTAINING_RECORD( iface, struct manager, IAppInstallManager_iface );
}

static HRESULT WINAPI manager_QueryInterface( IAppInstallManager *iface, REFIID iid, void **out )
{
    struct manager *impl = impl_from_IAppInstallManager( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAppInstallManager ))
        *out = &impl->IAppInstallManager_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallManager2 )) *out = &impl->IAppInstallManager2_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallManager3 )) *out = &impl->IAppInstallManager3_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallManager4 )) *out = &impl->IAppInstallManager4_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallManager5 )) *out = &impl->IAppInstallManager5_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallManager6 )) *out = &impl->IAppInstallManager6_iface;
    else if (IsEqualGUID( iid, &IID_IAppInstallManager7 )) *out = &impl->IAppInstallManager7_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI manager_AddRef( IAppInstallManager *iface )
{
    return InterlockedIncrement( &impl_from_IAppInstallManager( iface )->ref );
}

static ULONG WINAPI manager_Release( IAppInstallManager *iface )
{
    struct manager *impl = impl_from_IAppInstallManager( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->acquisition_identity );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI manager_GetIids( IAppInstallManager *iface, ULONG *iid_count, IID **iids )
{
    return get_iids( iid_count, iids );
}

static HRESULT WINAPI manager_GetRuntimeClassName( IAppInstallManager *iface, HSTRING *name )
{
    return class_name( RuntimeClass_Windows_ApplicationModel_Store_Preview_InstallControl_AppInstallManager, name );
}

static HRESULT WINAPI manager_GetTrustLevel( IAppInstallManager *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI manager_get_AppInstallItems( IAppInstallManager *iface, IVectorView_AppInstallItem **value )
{
    return current_items( value );
}

static HRESULT WINAPI manager_Cancel( IAppInstallManager *iface, HSTRING product_id )
{
    return by_product( product_id, ITEM_CANCEL );
}

static HRESULT WINAPI manager_Pause( IAppInstallManager *iface, HSTRING product_id )
{
    return by_product( product_id, ITEM_PAUSE );
}

static HRESULT WINAPI manager_Restart( IAppInstallManager *iface, HSTRING product_id )
{
    return by_product( product_id, ITEM_RESTART );
}

static HRESULT WINAPI manager_add_ItemCompleted( IAppInstallManager *iface,
        ITypedEventHandler_AppInstallManager_AppInstallManagerItemEventArgs *handler, EventRegistrationToken *token )
{
    return add_handler( &manager_completed, (IUnknown *)handler, (IInspectable *)iface, token );
}

static HRESULT WINAPI manager_remove_ItemCompleted( IAppInstallManager *iface, EventRegistrationToken token )
{
    return remove_handler( &manager_completed, token );
}

static HRESULT WINAPI manager_add_ItemStatusChanged( IAppInstallManager *iface,
        ITypedEventHandler_AppInstallManager_AppInstallManagerItemEventArgs *handler, EventRegistrationToken *token )
{
    return add_handler( &manager_status_changed, (IUnknown *)handler, (IInspectable *)iface, token );
}

static HRESULT WINAPI manager_remove_ItemStatusChanged( IAppInstallManager *iface, EventRegistrationToken token )
{
    return remove_handler( &manager_status_changed, token );
}

static HRESULT WINAPI manager_get_AutoUpdateSetting( IAppInstallManager *iface, AutoUpdateSetting *value )
{
    *value = auto_update_setting;
    return S_OK;
}

static HRESULT WINAPI manager_put_AutoUpdateSetting( IAppInstallManager *iface, AutoUpdateSetting value )
{
    auto_update_setting = value;
    return S_OK;
}

static HRESULT WINAPI manager_get_AcquisitionIdentity( IAppInstallManager *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IAppInstallManager( iface )->acquisition_identity, value );
}

static HRESULT WINAPI manager_put_AcquisitionIdentity( IAppInstallManager *iface, HSTRING value )
{
    struct manager *impl = impl_from_IAppInstallManager( iface );
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->acquisition_identity );
    impl->acquisition_identity = copy;
    return S_OK;
}

static HRESULT WINAPI manager_GetIsApplicableAsync( IAppInstallManager *iface, HSTRING product_id, HSTRING sku_id,
                                                    IAsyncOperation_boolean **operation )
{
    return bool_async( TRUE, operation );
}

/* Installing other products needs the Store's licences and installer service. */
static HRESULT WINAPI manager_StartAppInstallAsync( IAppInstallManager *iface, HSTRING product_id, HSTRING sku_id,
                                                    boolean repair, boolean force, IAsyncOperation_AppInstallItem **operation )
{
    FIXME( "iface %p, product %s stub!\n", iface, debugstr_hstring( product_id ) );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI manager_UpdateAppByPackageFamilyNameAsync( IAppInstallManager *iface, HSTRING family,
                                                                 IAsyncOperation_AppInstallItem **operation )
{
    TRACE( "iface %p, family %s.\n", iface, debugstr_hstring( family ) );
    return update_family_async( family, operation );
}

static HRESULT WINAPI manager_SearchForUpdatesAsync( IAppInstallManager *iface, HSTRING product_id, HSTRING sku_id,
                                                     IAsyncOperation_AppInstallItem **operation )
{
    TRACE( "iface %p, product %s.\n", iface, debugstr_hstring( product_id ) );
    return search_one_async( NULL, operation );
}

static HRESULT WINAPI manager_SearchForAllUpdatesAsync( IAppInstallManager *iface,
                                                        IAsyncOperation_IVectorView_AppInstallItem **operation )
{
    TRACE( "iface %p.\n", iface );
    return search_all_async( NULL, operation );
}

static HRESULT WINAPI manager_IsStoreBlockedByPolicyAsync( IAppInstallManager *iface, HSTRING name, HSTRING publisher,
                                                           IAsyncOperation_boolean **operation )
{
    return bool_async( FALSE, operation );
}

static HRESULT WINAPI manager_GetIsAppAllowedToInstallAsync( IAppInstallManager *iface, HSTRING product_id,
                                                             IAsyncOperation_boolean **operation )
{
    return bool_async( TRUE, operation );
}

static const struct IAppInstallManagerVtbl manager_vtbl =
{
    manager_QueryInterface,
    manager_AddRef,
    manager_Release,
    /* IInspectable methods */
    manager_GetIids,
    manager_GetRuntimeClassName,
    manager_GetTrustLevel,
    /* IAppInstallManager methods */
    manager_get_AppInstallItems,
    manager_Cancel,
    manager_Pause,
    manager_Restart,
    manager_add_ItemCompleted,
    manager_remove_ItemCompleted,
    manager_add_ItemStatusChanged,
    manager_remove_ItemStatusChanged,
    manager_get_AutoUpdateSetting,
    manager_put_AutoUpdateSetting,
    manager_get_AcquisitionIdentity,
    manager_put_AcquisitionIdentity,
    manager_GetIsApplicableAsync,
    manager_StartAppInstallAsync,
    manager_UpdateAppByPackageFamilyNameAsync,
    manager_SearchForUpdatesAsync,
    manager_SearchForAllUpdatesAsync,
    manager_IsStoreBlockedByPolicyAsync,
    manager_GetIsAppAllowedToInstallAsync,
};

DEFINE_IINSPECTABLE( manager2, IAppInstallManager2, struct manager, IAppInstallManager_iface )

static HRESULT WINAPI manager2_StartAppInstallWithTelemetryAsync( IAppInstallManager2 *iface, HSTRING product_id, HSTRING sku_id,
        boolean repair, boolean force, HSTRING catalog_id, HSTRING bundle_id, HSTRING correlation_vector,
        IAsyncOperation_AppInstallItem **operation )
{
    return manager_StartAppInstallAsync( &impl_from_IAppInstallManager2( iface )->IAppInstallManager_iface, product_id,
                                         sku_id, repair, force, operation );
}

static HRESULT WINAPI manager2_UpdateAppByPackageFamilyNameWithTelemetryAsync( IAppInstallManager2 *iface, HSTRING family,
        HSTRING correlation_vector, IAsyncOperation_AppInstallItem **operation )
{
    return update_family_async( family, operation );
}

static HRESULT WINAPI manager2_SearchForUpdatesWithTelemetryAsync( IAppInstallManager2 *iface, HSTRING product_id,
        HSTRING sku_id, HSTRING catalog_id, HSTRING correlation_vector, IAsyncOperation_AppInstallItem **operation )
{
    return search_one_async( NULL, operation );
}

static HRESULT WINAPI manager2_SearchForAllUpdatesWithTelemetryAsync( IAppInstallManager2 *iface, HSTRING correlation_vector,
        IAsyncOperation_IVectorView_AppInstallItem **operation )
{
    return search_all_async( NULL, operation );
}

static HRESULT WINAPI manager2_GetIsAppAllowedToInstallWithTelemetryAsync( IAppInstallManager2 *iface, HSTRING product_id,
        HSTRING sku_id, HSTRING catalog_id, HSTRING correlation_vector, IAsyncOperation_boolean **operation )
{
    return bool_async( TRUE, operation );
}

static HRESULT WINAPI manager2_CancelWithTelemetry( IAppInstallManager2 *iface, HSTRING product_id, HSTRING correlation_vector )
{
    return by_product( product_id, ITEM_CANCEL );
}

static HRESULT WINAPI manager2_PauseWithTelemetry( IAppInstallManager2 *iface, HSTRING product_id, HSTRING correlation_vector )
{
    return by_product( product_id, ITEM_PAUSE );
}

static HRESULT WINAPI manager2_RestartWithTelemetry( IAppInstallManager2 *iface, HSTRING product_id, HSTRING correlation_vector )
{
    return by_product( product_id, ITEM_RESTART );
}

static const struct IAppInstallManager2Vtbl manager2_vtbl =
{
    manager2_QueryInterface,
    manager2_AddRef,
    manager2_Release,
    /* IInspectable methods */
    manager2_GetIids,
    manager2_GetRuntimeClassName,
    manager2_GetTrustLevel,
    /* IAppInstallManager2 methods */
    manager2_StartAppInstallWithTelemetryAsync,
    manager2_UpdateAppByPackageFamilyNameWithTelemetryAsync,
    manager2_SearchForUpdatesWithTelemetryAsync,
    manager2_SearchForAllUpdatesWithTelemetryAsync,
    manager2_GetIsAppAllowedToInstallWithTelemetryAsync,
    manager2_CancelWithTelemetry,
    manager2_PauseWithTelemetry,
    manager2_RestartWithTelemetry,
};

DEFINE_IINSPECTABLE( manager3, IAppInstallManager3, struct manager, IAppInstallManager_iface )

static HRESULT WINAPI manager3_StartProductInstallAsync( IAppInstallManager3 *iface, HSTRING product_id, HSTRING catalog_id,
        HSTRING flight_id, HSTRING client_id, boolean repair, boolean force, HSTRING correlation_vector,
        IInspectable *target_volume, IAsyncOperation_IVectorView_AppInstallItem **operation )
{
    FIXME( "iface %p, product %s stub!\n", iface, debugstr_hstring( product_id ) );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI manager3_StartProductInstallForUserAsync( IAppInstallManager3 *iface, __x_ABI_CWindows_CSystem_CIUser *user,
        HSTRING product_id, HSTRING catalog_id, HSTRING flight_id, HSTRING client_id, boolean repair, boolean force,
        HSTRING correlation_vector, IInspectable *target_volume, IAsyncOperation_IVectorView_AppInstallItem **operation )
{
    FIXME( "iface %p, product %s stub!\n", iface, debugstr_hstring( product_id ) );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI manager3_UpdateAppByPackageFamilyNameForUserAsync( IAppInstallManager3 *iface,
        __x_ABI_CWindows_CSystem_CIUser *user, HSTRING family, HSTRING correlation_vector,
        IAsyncOperation_AppInstallItem **operation )
{
    return update_family_async( family, operation );
}

static HRESULT WINAPI manager3_SearchForUpdatesForUserAsync( IAppInstallManager3 *iface, __x_ABI_CWindows_CSystem_CIUser *user,
        HSTRING product_id, HSTRING sku_id, HSTRING catalog_id, HSTRING correlation_vector,
        IAsyncOperation_AppInstallItem **operation )
{
    return search_one_async( NULL, operation );
}

static HRESULT WINAPI manager3_SearchForAllUpdatesForUserAsync( IAppInstallManager3 *iface, __x_ABI_CWindows_CSystem_CIUser *user,
        HSTRING correlation_vector, IAsyncOperation_IVectorView_AppInstallItem **operation )
{
    return search_all_async( NULL, operation );
}

static HRESULT WINAPI manager3_GetIsAppAllowedToInstallForUserAsync( IAppInstallManager3 *iface,
        __x_ABI_CWindows_CSystem_CIUser *user, HSTRING product_id, HSTRING sku_id, HSTRING catalog_id,
        HSTRING correlation_vector, IAsyncOperation_boolean **operation )
{
    return bool_async( TRUE, operation );
}

static HRESULT WINAPI manager3_GetIsApplicableForUserAsync( IAppInstallManager3 *iface, __x_ABI_CWindows_CSystem_CIUser *user,
        HSTRING product_id, HSTRING sku_id, IAsyncOperation_boolean **operation )
{
    return bool_async( TRUE, operation );
}

static HRESULT WINAPI manager3_MoveToFrontOfDownloadQueue( IAppInstallManager3 *iface, HSTRING product_id,
                                                           HSTRING correlation_vector )
{
    FIXME( "iface %p, product %s stub!\n", iface, debugstr_hstring( product_id ) );
    return E_NOTIMPL;
}

static const struct IAppInstallManager3Vtbl manager3_vtbl =
{
    manager3_QueryInterface,
    manager3_AddRef,
    manager3_Release,
    /* IInspectable methods */
    manager3_GetIids,
    manager3_GetRuntimeClassName,
    manager3_GetTrustLevel,
    /* IAppInstallManager3 methods */
    manager3_StartProductInstallAsync,
    manager3_StartProductInstallForUserAsync,
    manager3_UpdateAppByPackageFamilyNameForUserAsync,
    manager3_SearchForUpdatesForUserAsync,
    manager3_SearchForAllUpdatesForUserAsync,
    manager3_GetIsAppAllowedToInstallForUserAsync,
    manager3_GetIsApplicableForUserAsync,
    manager3_MoveToFrontOfDownloadQueue,
};

DEFINE_IINSPECTABLE( manager4, IAppInstallManager4, struct manager, IAppInstallManager_iface )

/* Entitlements need an account with the Store. */
static HRESULT WINAPI manager4_GetFreeUserEntitlementAsync( IAppInstallManager4 *iface, HSTRING store_id, HSTRING campaign_id,
        HSTRING correlation_vector, IInspectable **operation )
{
    FIXME( "iface %p, store id %s stub!\n", iface, debugstr_hstring( store_id ) );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI manager4_GetFreeUserEntitlementForUserAsync( IAppInstallManager4 *iface,
        __x_ABI_CWindows_CSystem_CIUser *user, HSTRING store_id, HSTRING campaign_id, HSTRING correlation_vector,
        IInspectable **operation )
{
    FIXME( "iface %p, store id %s stub!\n", iface, debugstr_hstring( store_id ) );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI manager4_GetFreeDeviceEntitlementAsync( IAppInstallManager4 *iface, HSTRING store_id,
        HSTRING campaign_id, HSTRING correlation_vector, IInspectable **operation )
{
    FIXME( "iface %p, store id %s stub!\n", iface, debugstr_hstring( store_id ) );
    *operation = NULL;
    return E_NOTIMPL;
}

static const struct IAppInstallManager4Vtbl manager4_vtbl =
{
    manager4_QueryInterface,
    manager4_AddRef,
    manager4_Release,
    /* IInspectable methods */
    manager4_GetIids,
    manager4_GetRuntimeClassName,
    manager4_GetTrustLevel,
    /* IAppInstallManager4 methods */
    manager4_GetFreeUserEntitlementAsync,
    manager4_GetFreeUserEntitlementForUserAsync,
    manager4_GetFreeDeviceEntitlementAsync,
};

DEFINE_IINSPECTABLE( manager5, IAppInstallManager5, struct manager, IAppInstallManager_iface )

static HRESULT WINAPI manager5_get_AppInstallItemsWithGroupSupport( IAppInstallManager5 *iface,
                                                                    IVectorView_AppInstallItem **value )
{
    return current_items( value );
}

static const struct IAppInstallManager5Vtbl manager5_vtbl =
{
    manager5_QueryInterface,
    manager5_AddRef,
    manager5_Release,
    /* IInspectable methods */
    manager5_GetIids,
    manager5_GetRuntimeClassName,
    manager5_GetTrustLevel,
    /* IAppInstallManager5 methods */
    manager5_get_AppInstallItemsWithGroupSupport,
};

DEFINE_IINSPECTABLE( manager6, IAppInstallManager6, struct manager, IAppInstallManager_iface )

static HRESULT WINAPI manager6_SearchForAllUpdatesWithUpdateOptionsAsync( IAppInstallManager6 *iface,
        HSTRING correlation_vector, HSTRING client_id, IAppUpdateOptions *options,
        IAsyncOperation_IVectorView_AppInstallItem **operation )
{
    return search_all_async( options, operation );
}

static HRESULT WINAPI manager6_SearchForAllUpdatesWithUpdateOptionsForUserAsync( IAppInstallManager6 *iface,
        __x_ABI_CWindows_CSystem_CIUser *user, HSTRING correlation_vector, HSTRING client_id, IAppUpdateOptions *options,
        IAsyncOperation_IVectorView_AppInstallItem **operation )
{
    return search_all_async( options, operation );
}

static HRESULT WINAPI manager6_SearchForUpdatesWithUpdateOptionsAsync( IAppInstallManager6 *iface, HSTRING product_id,
        HSTRING sku_id, HSTRING correlation_vector, HSTRING client_id, IAppUpdateOptions *options,
        IAsyncOperation_AppInstallItem **operation )
{
    return search_one_async( options, operation );
}

static HRESULT WINAPI manager6_SearchForUpdatesWithUpdateOptionsForUserAsync( IAppInstallManager6 *iface,
        __x_ABI_CWindows_CSystem_CIUser *user, HSTRING product_id, HSTRING sku_id, HSTRING correlation_vector,
        HSTRING client_id, IAppUpdateOptions *options, IAsyncOperation_AppInstallItem **operation )
{
    return search_one_async( options, operation );
}

static HRESULT WINAPI manager6_StartProductInstallWithOptionsAsync( IAppInstallManager6 *iface, HSTRING product_id,
        HSTRING flight_id, HSTRING client_id, HSTRING correlation_vector,
        __x_ABI_CWindows_CApplicationModel_CStore_CPreview_CInstallControl_CIAppInstallOptions *options,
        IAsyncOperation_IVectorView_AppInstallItem **operation )
{
    FIXME( "iface %p, product %s stub!\n", iface, debugstr_hstring( product_id ) );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI manager6_StartProductInstallWithOptionsForUserAsync( IAppInstallManager6 *iface,
        __x_ABI_CWindows_CSystem_CIUser *user, HSTRING product_id, HSTRING flight_id, HSTRING client_id,
        HSTRING correlation_vector,
        __x_ABI_CWindows_CApplicationModel_CStore_CPreview_CInstallControl_CIAppInstallOptions *options,
        IAsyncOperation_IVectorView_AppInstallItem **operation )
{
    FIXME( "iface %p, product %s stub!\n", iface, debugstr_hstring( product_id ) );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI manager6_GetIsPackageIdentityAllowedToInstallAsync( IAppInstallManager6 *iface,
        HSTRING correlation_vector, HSTRING identity_name, HSTRING publisher, IAsyncOperation_boolean **operation )
{
    return bool_async( TRUE, operation );
}

static HRESULT WINAPI manager6_GetIsPackageIdentityAllowedToInstallForUserAsync( IAppInstallManager6 *iface,
        __x_ABI_CWindows_CSystem_CIUser *user, HSTRING correlation_vector, HSTRING identity_name, HSTRING publisher,
        IAsyncOperation_boolean **operation )
{
    return bool_async( TRUE, operation );
}

static const struct IAppInstallManager6Vtbl manager6_vtbl =
{
    manager6_QueryInterface,
    manager6_AddRef,
    manager6_Release,
    /* IInspectable methods */
    manager6_GetIids,
    manager6_GetRuntimeClassName,
    manager6_GetTrustLevel,
    /* IAppInstallManager6 methods */
    manager6_SearchForAllUpdatesWithUpdateOptionsAsync,
    manager6_SearchForAllUpdatesWithUpdateOptionsForUserAsync,
    manager6_SearchForUpdatesWithUpdateOptionsAsync,
    manager6_SearchForUpdatesWithUpdateOptionsForUserAsync,
    manager6_StartProductInstallWithOptionsAsync,
    manager6_StartProductInstallWithOptionsForUserAsync,
    manager6_GetIsPackageIdentityAllowedToInstallAsync,
    manager6_GetIsPackageIdentityAllowedToInstallForUserAsync,
};

DEFINE_IINSPECTABLE( manager7, IAppInstallManager7, struct manager, IAppInstallManager_iface )

static HRESULT WINAPI manager7_get_CanInstallForAllUsers( IAppInstallManager7 *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static const struct IAppInstallManager7Vtbl manager7_vtbl =
{
    manager7_QueryInterface,
    manager7_AddRef,
    manager7_Release,
    /* IInspectable methods */
    manager7_GetIids,
    manager7_GetRuntimeClassName,
    manager7_GetTrustLevel,
    /* IAppInstallManager7 methods */
    manager7_get_CanInstallForAllUsers,
};

/* The two activatable classes */

struct factory
{
    IActivationFactory IActivationFactory_iface;
    const WCHAR *name;
};

static inline struct factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = iface;
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return 2;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return 1;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    return get_iids( iid_count, iids );
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return class_name( impl_from_IActivationFactory( iface )->name, name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    const WCHAR *name = impl_from_IActivationFactory( iface )->name;

    TRACE( "%s.\n", debugstr_w( name ) );

    if (!wcscmp( name, RuntimeClass_Windows_ApplicationModel_Store_Preview_InstallControl_AppUpdateOptions ))
    {
        struct update_options *impl;

        if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
        impl->IAppUpdateOptions_iface.lpVtbl = &options_vtbl;
        impl->IAppUpdateOptions2_iface.lpVtbl = &options2_vtbl;
        impl->ref = 1;
        *instance = (IInspectable *)&impl->IAppUpdateOptions_iface;
    }
    else
    {
        struct manager *impl;

        if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
        impl->IAppInstallManager_iface.lpVtbl = &manager_vtbl;
        impl->IAppInstallManager2_iface.lpVtbl = &manager2_vtbl;
        impl->IAppInstallManager3_iface.lpVtbl = &manager3_vtbl;
        impl->IAppInstallManager4_iface.lpVtbl = &manager4_vtbl;
        impl->IAppInstallManager5_iface.lpVtbl = &manager5_vtbl;
        impl->IAppInstallManager6_iface.lpVtbl = &manager6_vtbl;
        impl->IAppInstallManager7_iface.lpVtbl = &manager7_vtbl;
        impl->ref = 1;
        *instance = (IInspectable *)&impl->IAppInstallManager_iface;
    }
    return S_OK;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

static struct factory factories[] =
{
    {{&factory_vtbl}, RuntimeClass_Windows_ApplicationModel_Store_Preview_InstallControl_AppInstallManager},
    {{&factory_vtbl}, RuntimeClass_Windows_ApplicationModel_Store_Preview_InstallControl_AppUpdateOptions},
};

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );
    unsigned int i;

    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;
    for (i = 0; i < ARRAY_SIZE(factories); i++)
    {
        if (wcscmp( name, factories[i].name )) continue;
        *factory = &factories[i].IActivationFactory_iface;
        return S_OK;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

/* WinRT Windows.Storage.AccessCache Implementation
 *
 * Copyright 2026 wine-uwp
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

/* After a user picks a file, an app adds it to the future-access list so that its access outlives
 * the pick; Media Player does this right after "Open file(s)". A Wine prefix has no sandbox, so the
 * lists grant nothing: they remember items for the life of the process, hand out tokens for them
 * and give the items back. They are not persisted, so an app that relies on that finds an empty
 * list, as it would after the user cleared its history. */

#include "private.h"

#define WIDL_using_Windows_Storage_AccessCache
#include "windows.storage.accesscache.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

struct access_entry
{
    WCHAR token[48];
    HSTRING metadata;
    IStorageItem *item;
};

struct access_list
{
    IStorageItemAccessList IStorageItemAccessList_iface;
    IStorageItemMostRecentlyUsedList IStorageItemMostRecentlyUsedList_iface;
    IStorageItemMostRecentlyUsedList2 IStorageItemMostRecentlyUsedList2_iface;
    BOOL mru;
    const WCHAR *name;
    UINT32 max;
    CRITICAL_SECTION cs;
    struct access_entry *entries;
    UINT32 count;
    LONG next_token;
};

/* AccessListEntryView: a snapshot of the list's tokens and metadata */

struct entry_view
{
    IVectorView_AccessListEntry IVectorView_AccessListEntry_iface;
    IIterable_AccessListEntry IIterable_AccessListEntry_iface;
    LONG ref;
    AccessListEntry *entries;
    UINT32 count;
};

struct entry_iterator
{
    IIterator_AccessListEntry IIterator_AccessListEntry_iface;
    LONG ref;
    struct entry_view *view;
    UINT32 index;
};

static HRESULT copy_entry( const AccessListEntry *src, AccessListEntry *dst )
{
    HRESULT hr;

    dst->Token = dst->Metadata = NULL;
    if (FAILED(hr = WindowsDuplicateString( src->Token, &dst->Token ))) return hr;
    if (FAILED(hr = WindowsDuplicateString( src->Metadata, &dst->Metadata )))
    {
        WindowsDeleteString( dst->Token );
        dst->Token = NULL;
    }
    return hr;
}

static inline struct entry_iterator *impl_from_IIterator_AccessListEntry( IIterator_AccessListEntry *iface )
{
    return CONTAINING_RECORD( iface, struct entry_iterator, IIterator_AccessListEntry_iface );
}

static HRESULT WINAPI entry_iterator_QueryInterface( IIterator_AccessListEntry *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_AccessListEntry ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI entry_iterator_AddRef( IIterator_AccessListEntry *iface )
{
    struct entry_iterator *impl = impl_from_IIterator_AccessListEntry( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI entry_iterator_Release( IIterator_AccessListEntry *iface )
{
    struct entry_iterator *impl = impl_from_IIterator_AccessListEntry( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVectorView_AccessListEntry_Release( &impl->view->IVectorView_AccessListEntry_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI entry_iterator_GetIids( IIterator_AccessListEntry *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI entry_iterator_GetRuntimeClassName( IIterator_AccessListEntry *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Foundation.Collections.IIterator`1<Windows.Storage.AccessCache.AccessListEntry>";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI entry_iterator_GetTrustLevel( IIterator_AccessListEntry *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI entry_iterator_get_Current( IIterator_AccessListEntry *iface, AccessListEntry *value )
{
    struct entry_iterator *impl = impl_from_IIterator_AccessListEntry( iface );

    if (impl->index >= impl->view->count) return E_BOUNDS;
    return copy_entry( &impl->view->entries[impl->index], value );
}

static HRESULT WINAPI entry_iterator_get_HasCurrent( IIterator_AccessListEntry *iface, boolean *value )
{
    struct entry_iterator *impl = impl_from_IIterator_AccessListEntry( iface );

    *value = impl->index < impl->view->count;
    return S_OK;
}

static HRESULT WINAPI entry_iterator_MoveNext( IIterator_AccessListEntry *iface, boolean *value )
{
    struct entry_iterator *impl = impl_from_IIterator_AccessListEntry( iface );

    if (impl->index < impl->view->count) impl->index++;
    return entry_iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI entry_iterator_GetMany( IIterator_AccessListEntry *iface, UINT32 count,
                                              AccessListEntry *items, UINT32 *written )
{
    struct entry_iterator *impl = impl_from_IIterator_AccessListEntry( iface );
    HRESULT hr = S_OK;

    *written = 0;
    while (*written < count && impl->index < impl->view->count)
    {
        if (FAILED(hr = copy_entry( &impl->view->entries[impl->index], &items[*written] ))) break;
        impl->index++;
        (*written)++;
    }
    return hr;
}

static const struct IIterator_AccessListEntryVtbl entry_iterator_vtbl =
{
    entry_iterator_QueryInterface,
    entry_iterator_AddRef,
    entry_iterator_Release,
    /* IInspectable methods */
    entry_iterator_GetIids,
    entry_iterator_GetRuntimeClassName,
    entry_iterator_GetTrustLevel,
    /* IIterator<AccessListEntry> methods */
    entry_iterator_get_Current,
    entry_iterator_get_HasCurrent,
    entry_iterator_MoveNext,
    entry_iterator_GetMany,
};

static inline struct entry_view *impl_from_IVectorView_AccessListEntry( IVectorView_AccessListEntry *iface )
{
    return CONTAINING_RECORD( iface, struct entry_view, IVectorView_AccessListEntry_iface );
}

static HRESULT WINAPI entry_view_QueryInterface( IVectorView_AccessListEntry *iface, REFIID iid, void **out )
{
    struct entry_view *impl = impl_from_IVectorView_AccessListEntry( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IVectorView_AccessListEntry ))
        *out = &impl->IVectorView_AccessListEntry_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_AccessListEntry ))
        *out = &impl->IIterable_AccessListEntry_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI entry_view_AddRef( IVectorView_AccessListEntry *iface )
{
    struct entry_view *impl = impl_from_IVectorView_AccessListEntry( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI entry_view_Release( IVectorView_AccessListEntry *iface )
{
    struct entry_view *impl = impl_from_IVectorView_AccessListEntry( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++)
        {
            WindowsDeleteString( impl->entries[i].Token );
            WindowsDeleteString( impl->entries[i].Metadata );
        }
        free( impl->entries );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI entry_view_GetIids( IVectorView_AccessListEntry *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI entry_view_GetRuntimeClassName( IVectorView_AccessListEntry *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Storage_AccessCache_AccessListEntryView,
                                wcslen( RuntimeClass_Windows_Storage_AccessCache_AccessListEntryView ), class_name );
}

static HRESULT WINAPI entry_view_GetTrustLevel( IVectorView_AccessListEntry *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI entry_view_GetAt( IVectorView_AccessListEntry *iface, UINT32 index, AccessListEntry *value )
{
    struct entry_view *impl = impl_from_IVectorView_AccessListEntry( iface );

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (index >= impl->count) return E_BOUNDS;
    return copy_entry( &impl->entries[index], value );
}

static HRESULT WINAPI entry_view_get_Size( IVectorView_AccessListEntry *iface, UINT32 *value )
{
    struct entry_view *impl = impl_from_IVectorView_AccessListEntry( iface );

    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI entry_view_IndexOf( IVectorView_AccessListEntry *iface, AccessListEntry element,
                                          UINT32 *index, boolean *found )
{
    struct entry_view *impl = impl_from_IVectorView_AccessListEntry( iface );
    INT32 order;
    UINT32 i;

    *index = 0;
    *found = FALSE;
    for (i = 0; i < impl->count; i++)
    {
        if (FAILED(WindowsCompareStringOrdinal( impl->entries[i].Token, element.Token, &order )) || order) continue;
        *index = i;
        *found = TRUE;
        break;
    }
    return S_OK;
}

static HRESULT WINAPI entry_view_GetMany( IVectorView_AccessListEntry *iface, UINT32 start, UINT32 count,
                                          AccessListEntry *items, UINT32 *written )
{
    struct entry_view *impl = impl_from_IVectorView_AccessListEntry( iface );
    HRESULT hr = S_OK;

    *written = 0;
    if (start > impl->count) return E_BOUNDS;
    while (*written < count && start + *written < impl->count)
    {
        if (FAILED(hr = copy_entry( &impl->entries[start + *written], &items[*written] ))) break;
        (*written)++;
    }
    return hr;
}

static const struct IVectorView_AccessListEntryVtbl entry_view_vtbl =
{
    entry_view_QueryInterface,
    entry_view_AddRef,
    entry_view_Release,
    /* IInspectable methods */
    entry_view_GetIids,
    entry_view_GetRuntimeClassName,
    entry_view_GetTrustLevel,
    /* IVectorView<AccessListEntry> methods */
    entry_view_GetAt,
    entry_view_get_Size,
    entry_view_IndexOf,
    entry_view_GetMany,
};

DEFINE_IINSPECTABLE( entry_iterable, IIterable_AccessListEntry, struct entry_view, IVectorView_AccessListEntry_iface )

static HRESULT WINAPI entry_iterable_First( IIterable_AccessListEntry *iface, IIterator_AccessListEntry **value )
{
    struct entry_view *impl = impl_from_IIterable_AccessListEntry( iface );
    struct entry_iterator *iter;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_AccessListEntry_iface.lpVtbl = &entry_iterator_vtbl;
    iter->ref = 1;
    iter->view = impl;
    IVectorView_AccessListEntry_AddRef( &impl->IVectorView_AccessListEntry_iface );
    *value = &iter->IIterator_AccessListEntry_iface;
    return S_OK;
}

static const struct IIterable_AccessListEntryVtbl entry_iterable_vtbl =
{
    entry_iterable_QueryInterface,
    entry_iterable_AddRef,
    entry_iterable_Release,
    /* IInspectable methods */
    entry_iterable_GetIids,
    entry_iterable_GetRuntimeClassName,
    entry_iterable_GetTrustLevel,
    /* IIterable<AccessListEntry> methods */
    entry_iterable_First,
};

/* The two lists. They live as long as the process, so their references are not counted. */

static inline struct access_list *impl_from_IStorageItemAccessList( IStorageItemAccessList *iface )
{
    return CONTAINING_RECORD( iface, struct access_list, IStorageItemAccessList_iface );
}

static HRESULT WINAPI access_list_QueryInterface( IStorageItemAccessList *iface, REFIID iid, void **out )
{
    struct access_list *impl = impl_from_IStorageItemAccessList( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ))
        *out = impl->mru ? (void *)&impl->IStorageItemMostRecentlyUsedList_iface : (void *)&impl->IStorageItemAccessList_iface;
    else if (IsEqualGUID( iid, &IID_IStorageItemAccessList ))
        *out = &impl->IStorageItemAccessList_iface;
    else if (impl->mru && IsEqualGUID( iid, &IID_IStorageItemMostRecentlyUsedList ))
        *out = &impl->IStorageItemMostRecentlyUsedList_iface;
    else if (impl->mru && IsEqualGUID( iid, &IID_IStorageItemMostRecentlyUsedList2 ))
        *out = &impl->IStorageItemMostRecentlyUsedList2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static ULONG WINAPI access_list_AddRef( IStorageItemAccessList *iface )
{
    return 2;
}

static ULONG WINAPI access_list_Release( IStorageItemAccessList *iface )
{
    return 1;
}

static HRESULT WINAPI access_list_GetIids( IStorageItemAccessList *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

/* .NET types its wrapper from this name: calling the MRU list a plain StorageItemAccessList makes
 * the managed cast to StorageItemMostRecentlyUsedList fail. */
static HRESULT WINAPI access_list_GetRuntimeClassName( IStorageItemAccessList *iface, HSTRING *class_name )
{
    struct access_list *impl = impl_from_IStorageItemAccessList( iface );
    const WCHAR *name = impl->mru ? RuntimeClass_Windows_Storage_AccessCache_StorageItemMostRecentlyUsedList
                                  : RuntimeClass_Windows_Storage_AccessCache_StorageItemAccessList;
    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI access_list_GetTrustLevel( IStorageItemAccessList *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static void remove_entry( struct access_list *impl, UINT32 index )
{
    IStorageItem_Release( impl->entries[index].item );
    WindowsDeleteString( impl->entries[index].metadata );
    memmove( impl->entries + index, impl->entries + index + 1, (impl->count - index - 1) * sizeof(*impl->entries) );
    impl->count--;
}

/* called with the list's lock held */
static struct access_entry *find_entry( struct access_list *impl, HSTRING token )
{
    const WCHAR *str = WindowsGetStringRawBuffer( token, NULL );
    UINT32 i;

    for (i = 0; i < impl->count; i++)
        if (!wcscmp( impl->entries[i].token, str )) return &impl->entries[i];
    return NULL;
}

/* Adds the item under a new token, or replaces the item and metadata of an existing one. */
static HRESULT add_entry( struct access_list *impl, HSTRING token, IStorageItem *item, HSTRING metadata, HSTRING *new_token )
{
    struct access_entry *entry = NULL, *entries;
    HSTRING copy = NULL;
    HRESULT hr;

    if (!item) return E_INVALIDARG;
    if (token && WindowsGetStringLen( token ) >= ARRAY_SIZE(entry->token)) return E_INVALIDARG;
    if (FAILED(hr = WindowsDuplicateString( metadata, &copy ))) return hr;

    EnterCriticalSection( &impl->cs );
    if (token && (entry = find_entry( impl, token )))
    {
        IStorageItem_Release( entry->item );
        WindowsDeleteString( entry->metadata );
    }
    else
    {
        /* Windows evicts the oldest entry of a full list. */
        if (impl->count >= impl->max) remove_entry( impl, 0 );
        if (!(entries = realloc( impl->entries, (impl->count + 1) * sizeof(*entries) )))
        {
            LeaveCriticalSection( &impl->cs );
            WindowsDeleteString( copy );
            return E_OUTOFMEMORY;
        }
        impl->entries = entries;
        entry = &impl->entries[impl->count++];
        if (token) wcscpy( entry->token, WindowsGetStringRawBuffer( token, NULL ) );
        else swprintf( entry->token, ARRAY_SIZE(entry->token), L"wine-%s-%ld", impl->name,
                       InterlockedIncrement( &impl->next_token ) );
    }
    IStorageItem_AddRef( (entry->item = item) );
    entry->metadata = copy;
    TRACE( "%s list: %p as %s, %u entries.\n", debugstr_w( impl->name ), item, debugstr_w( entry->token ), impl->count );
    if (new_token) hr = WindowsCreateString( entry->token, wcslen( entry->token ), new_token );
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI access_list_AddOverloadDefaultMetadata( IStorageItemAccessList *iface, IStorageItem *file, HSTRING *token )
{
    TRACE( "iface %p, file %p, token %p.\n", iface, file, token );
    return add_entry( impl_from_IStorageItemAccessList( iface ), NULL, file, NULL, token );
}

static HRESULT WINAPI access_list_Add( IStorageItemAccessList *iface, IStorageItem *file, HSTRING metadata, HSTRING *token )
{
    TRACE( "iface %p, file %p, metadata %s, token %p.\n", iface, file, debugstr_hstring( metadata ), token );
    return add_entry( impl_from_IStorageItemAccessList( iface ), NULL, file, metadata, token );
}

static HRESULT WINAPI access_list_AddOrReplaceOverloadDefaultMetadata( IStorageItemAccessList *iface, HSTRING token, IStorageItem *file )
{
    TRACE( "iface %p, token %s, file %p.\n", iface, debugstr_hstring( token ), file );
    return add_entry( impl_from_IStorageItemAccessList( iface ), token, file, NULL, NULL );
}

static HRESULT WINAPI access_list_AddOrReplace( IStorageItemAccessList *iface, HSTRING token, IStorageItem *file, HSTRING metadata )
{
    TRACE( "iface %p, token %s, file %p, metadata %s.\n", iface, debugstr_hstring( token ), file, debugstr_hstring( metadata ) );
    return add_entry( impl_from_IStorageItemAccessList( iface ), token, file, metadata, NULL );
}

/* The item is looked up when the operation is made, so the callbacks only hand it over, as the
 * interface the operation returns. */
static HRESULT return_item( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_UNKNOWN;
    return IUnknown_QueryInterface( param, &IID_IStorageItem, (void **)&result->punkVal );
}

static HRESULT return_file( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_UNKNOWN;
    return IUnknown_QueryInterface( param, &IID_IStorageFile, (void **)&result->punkVal );
}

static HRESULT return_folder( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_UNKNOWN;
    return IUnknown_QueryInterface( param, &IID_IStorageFolder, (void **)&result->punkVal );
}

/* An unknown token is an invalid argument, as on Windows. */
static HRESULT lookup_item( struct access_list *impl, HSTRING token, IStorageItem **item )
{
    struct access_entry *entry;

    EnterCriticalSection( &impl->cs );
    if ((entry = find_entry( impl, token ))) IStorageItem_AddRef( (*item = entry->item) );
    LeaveCriticalSection( &impl->cs );
    if (!entry) WARN( "%s list: no token %s.\n", debugstr_w( impl->name ), debugstr_hstring( token ) );
    return entry ? S_OK : E_INVALIDARG;
}

static HRESULT WINAPI access_list_GetItemAsync( IStorageItemAccessList *iface, HSTRING token,
                                                IAsyncOperation_IStorageItem **operation )
{
    IStorageItem *item;
    HRESULT hr;

    TRACE( "iface %p, token %s, operation %p.\n", iface, debugstr_hstring( token ), operation );

    *operation = NULL;
    if (FAILED(hr = lookup_item( impl_from_IStorageItemAccessList( iface ), token, &item ))) return hr;
    hr = async_operation_storage_item_create( (IUnknown *)iface, (IUnknown *)item, return_item, operation );
    IStorageItem_Release( item );
    return hr;
}

static HRESULT WINAPI access_list_GetFileAsync( IStorageItemAccessList *iface, HSTRING token,
                                                IAsyncOperation_StorageFile **operation )
{
    IStorageItem *item;
    HRESULT hr;

    TRACE( "iface %p, token %s, operation %p.\n", iface, debugstr_hstring( token ), operation );

    *operation = NULL;
    if (FAILED(hr = lookup_item( impl_from_IStorageItemAccessList( iface ), token, &item ))) return hr;
    hr = async_operation_storage_file_create( (IUnknown *)iface, (IUnknown *)item, return_file, operation );
    IStorageItem_Release( item );
    return hr;
}

static HRESULT WINAPI access_list_GetFolderAsync( IStorageItemAccessList *iface, HSTRING token,
                                                  IAsyncOperation_StorageFolder **operation )
{
    IStorageItem *item;
    HRESULT hr;

    TRACE( "iface %p, token %s, operation %p.\n", iface, debugstr_hstring( token ), operation );

    *operation = NULL;
    if (FAILED(hr = lookup_item( impl_from_IStorageItemAccessList( iface ), token, &item ))) return hr;
    hr = async_operation_storage_folder_create( (IUnknown *)iface, (IUnknown *)item, return_folder, operation );
    IStorageItem_Release( item );
    return hr;
}

/* The options are about prompting the user and offline copies, neither of which applies. */
static HRESULT WINAPI access_list_GetItemWithOptionsAsync( IStorageItemAccessList *iface, HSTRING token, AccessCacheOptions options,
                                                           IAsyncOperation_IStorageItem **operation )
{
    TRACE( "options %#x.\n", options );
    return access_list_GetItemAsync( iface, token, operation );
}

static HRESULT WINAPI access_list_GetFileWithOptionsAsync( IStorageItemAccessList *iface, HSTRING token, AccessCacheOptions options,
                                                           IAsyncOperation_StorageFile **operation )
{
    TRACE( "options %#x.\n", options );
    return access_list_GetFileAsync( iface, token, operation );
}

static HRESULT WINAPI access_list_GetFolderWithOptionsAsync( IStorageItemAccessList *iface, HSTRING token, AccessCacheOptions options,
                                                             IAsyncOperation_StorageFolder **operation )
{
    TRACE( "options %#x.\n", options );
    return access_list_GetFolderAsync( iface, token, operation );
}

static HRESULT WINAPI access_list_Remove( IStorageItemAccessList *iface, HSTRING token )
{
    struct access_list *impl = impl_from_IStorageItemAccessList( iface );
    struct access_entry *entry;

    TRACE( "iface %p, token %s.\n", iface, debugstr_hstring( token ) );

    EnterCriticalSection( &impl->cs );
    if ((entry = find_entry( impl, token ))) remove_entry( impl, entry - impl->entries );
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI access_list_ContainsItem( IStorageItemAccessList *iface, HSTRING token, boolean *value )
{
    struct access_list *impl = impl_from_IStorageItemAccessList( iface );

    TRACE( "iface %p, token %s, value %p.\n", iface, debugstr_hstring( token ), value );

    EnterCriticalSection( &impl->cs );
    *value = !!find_entry( impl, token );
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI access_list_Clear( IStorageItemAccessList *iface )
{
    struct access_list *impl = impl_from_IStorageItemAccessList( iface );

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->cs );
    while (impl->count) remove_entry( impl, impl->count - 1 );
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

/* Whether the app may still open an item. With no sandbox, it can. */
static HRESULT WINAPI access_list_CheckAccess( IStorageItemAccessList *iface, IStorageItem *file, boolean *value )
{
    TRACE( "iface %p, file %p, value %p.\n", iface, file, value );
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI access_list_get_Entries( IStorageItemAccessList *iface, IVectorView_AccessListEntry **value )
{
    struct access_list *impl = impl_from_IStorageItemAccessList( iface );
    struct entry_view *view;
    HRESULT hr = S_OK;
    UINT32 i;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!(view = calloc( 1, sizeof(*view) ))) return E_OUTOFMEMORY;
    view->IVectorView_AccessListEntry_iface.lpVtbl = &entry_view_vtbl;
    view->IIterable_AccessListEntry_iface.lpVtbl = &entry_iterable_vtbl;
    view->ref = 1;

    EnterCriticalSection( &impl->cs );
    if (impl->count && !(view->entries = calloc( impl->count, sizeof(*view->entries) ))) hr = E_OUTOFMEMORY;
    for (i = 0; SUCCEEDED(hr) && i < impl->count; i++, view->count++)
    {
        if (FAILED(hr = WindowsCreateString( impl->entries[i].token, wcslen( impl->entries[i].token ), &view->entries[i].Token )))
            break;
        if (FAILED(hr = WindowsDuplicateString( impl->entries[i].metadata, &view->entries[i].Metadata )))
        {
            WindowsDeleteString( view->entries[i].Token );
            break;
        }
    }
    LeaveCriticalSection( &impl->cs );

    if (FAILED(hr))
    {
        IVectorView_AccessListEntry_Release( &view->IVectorView_AccessListEntry_iface );
        return hr;
    }
    *value = &view->IVectorView_AccessListEntry_iface;
    return S_OK;
}

static HRESULT WINAPI access_list_get_MaximumItemsAllowed( IStorageItemAccessList *iface, UINT32 *value )
{
    struct access_list *impl = impl_from_IStorageItemAccessList( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->max;
    return S_OK;
}

static const struct IStorageItemAccessListVtbl access_list_vtbl =
{
    access_list_QueryInterface,
    access_list_AddRef,
    access_list_Release,
    /* IInspectable methods */
    access_list_GetIids,
    access_list_GetRuntimeClassName,
    access_list_GetTrustLevel,
    /* IStorageItemAccessList methods */
    access_list_AddOverloadDefaultMetadata,
    access_list_Add,
    access_list_AddOrReplaceOverloadDefaultMetadata,
    access_list_AddOrReplace,
    access_list_GetItemAsync,
    access_list_GetFileAsync,
    access_list_GetFolderAsync,
    access_list_GetItemWithOptionsAsync,
    access_list_GetFileWithOptionsAsync,
    access_list_GetFolderWithOptionsAsync,
    access_list_Remove,
    access_list_ContainsItem,
    access_list_Clear,
    access_list_CheckAccess,
    access_list_get_Entries,
    access_list_get_MaximumItemsAllowed,
};

DEFINE_IINSPECTABLE( mru_list, IStorageItemMostRecentlyUsedList, struct access_list, IStorageItemAccessList_iface )

/* Nothing removes an item behind the app's back except a full list, so this never fires; the
 * registration has to succeed all the same. */
static HRESULT WINAPI mru_list_add_ItemRemoved( IStorageItemMostRecentlyUsedList *iface,
                                                ITypedEventHandler_StorageItemMostRecentlyUsedList_ItemRemovedEventArgs *handler,
                                                EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    token->value = 1;
    return S_OK;
}

static HRESULT WINAPI mru_list_remove_ItemRemoved( IStorageItemMostRecentlyUsedList *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static const struct IStorageItemMostRecentlyUsedListVtbl mru_list_vtbl =
{
    mru_list_QueryInterface,
    mru_list_AddRef,
    mru_list_Release,
    /* IInspectable methods */
    mru_list_GetIids,
    mru_list_GetRuntimeClassName,
    mru_list_GetTrustLevel,
    /* IStorageItemMostRecentlyUsedList methods */
    mru_list_add_ItemRemoved,
    mru_list_remove_ItemRemoved,
};

DEFINE_IINSPECTABLE( mru_list2, IStorageItemMostRecentlyUsedList2, struct access_list, IStorageItemAccessList_iface )

/* Visibility decides whether the system's own recent list shows the item too; there is none. */
static HRESULT WINAPI mru_list2_AddWithMetadataAndVisibility( IStorageItemMostRecentlyUsedList2 *iface, IStorageItem *file,
                                                              HSTRING metadata, RecentStorageItemVisibility visibility,
                                                              HSTRING *token )
{
    TRACE( "iface %p, file %p, metadata %s, visibility %d, token %p.\n", iface, file, debugstr_hstring( metadata ), visibility, token );
    return add_entry( impl_from_IStorageItemMostRecentlyUsedList2( iface ), NULL, file, metadata, token );
}

static HRESULT WINAPI mru_list2_AddOrReplaceWithMetadataAndVisibility( IStorageItemMostRecentlyUsedList2 *iface, HSTRING token,
                                                                       IStorageItem *file, HSTRING metadata,
                                                                       RecentStorageItemVisibility visibility )
{
    TRACE( "iface %p, token %s, file %p, metadata %s, visibility %d.\n", iface, debugstr_hstring( token ), file,
           debugstr_hstring( metadata ), visibility );
    return add_entry( impl_from_IStorageItemMostRecentlyUsedList2( iface ), token, file, metadata, NULL );
}

static const struct IStorageItemMostRecentlyUsedList2Vtbl mru_list2_vtbl =
{
    mru_list2_QueryInterface,
    mru_list2_AddRef,
    mru_list2_Release,
    /* IInspectable methods */
    mru_list2_GetIids,
    mru_list2_GetRuntimeClassName,
    mru_list2_GetTrustLevel,
    /* IStorageItemMostRecentlyUsedList2 methods */
    mru_list2_AddWithMetadataAndVisibility,
    mru_list2_AddOrReplaceWithMetadataAndVisibility,
};

static struct access_list future_list, mru_list;

static CRITICAL_SECTION_DEBUG future_list_cs_debug =
{
    0, 0, &future_list.cs,
    { &future_list_cs_debug.ProcessLocksList, &future_list_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": future_list.cs") }
};

static CRITICAL_SECTION_DEBUG mru_list_cs_debug =
{
    0, 0, &mru_list.cs,
    { &mru_list_cs_debug.ProcessLocksList, &mru_list_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": mru_list.cs") }
};

/* Windows' limits: 1000 future-access entries, 25 recent ones. */
static struct access_list future_list =
{
    .IStorageItemAccessList_iface = {&access_list_vtbl},
    .IStorageItemMostRecentlyUsedList_iface = {&mru_list_vtbl},
    .IStorageItemMostRecentlyUsedList2_iface = {&mru_list2_vtbl},
    .name = L"future",
    .max = 1000,
    .cs = { &future_list_cs_debug, -1, 0, 0, 0, 0 },
};

static struct access_list mru_list =
{
    .IStorageItemAccessList_iface = {&access_list_vtbl},
    .IStorageItemMostRecentlyUsedList_iface = {&mru_list_vtbl},
    .IStorageItemMostRecentlyUsedList2_iface = {&mru_list2_vtbl},
    .mru = TRUE,
    .name = L"mru",
    .max = 25,
    .cs = { &mru_list_cs_debug, -1, 0, 0, 0, 0 },
};

/* StorageApplicationPermissions statics */

struct permissions_statics
{
    IActivationFactory IActivationFactory_iface;
    IStorageApplicationPermissionsStatics IStorageApplicationPermissionsStatics_iface;
    IStorageApplicationPermissionsStatics2 IStorageApplicationPermissionsStatics2_iface;
};

static inline struct permissions_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct permissions_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct permissions_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IStorageApplicationPermissionsStatics ))
        *out = &impl->IStorageApplicationPermissionsStatics_iface;
    else if (IsEqualGUID( iid, &IID_IStorageApplicationPermissionsStatics2 ))
        *out = &impl->IStorageApplicationPermissionsStatics2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
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
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p: static class.\n", iface, instance );
    return E_NOTIMPL;
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

DEFINE_IINSPECTABLE( permissions, IStorageApplicationPermissionsStatics, struct permissions_statics, IActivationFactory_iface )

static HRESULT WINAPI permissions_get_FutureAccessList( IStorageApplicationPermissionsStatics *iface, IStorageItemAccessList **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = &future_list.IStorageItemAccessList_iface;
    return S_OK;
}

static HRESULT WINAPI permissions_get_MostRecentlyUsedList( IStorageApplicationPermissionsStatics *iface,
                                                            IStorageItemMostRecentlyUsedList **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = &mru_list.IStorageItemMostRecentlyUsedList_iface;
    return S_OK;
}

static const struct IStorageApplicationPermissionsStaticsVtbl permissions_vtbl =
{
    permissions_QueryInterface,
    permissions_AddRef,
    permissions_Release,
    /* IInspectable methods */
    permissions_GetIids,
    permissions_GetRuntimeClassName,
    permissions_GetTrustLevel,
    /* IStorageApplicationPermissionsStatics methods */
    permissions_get_FutureAccessList,
    permissions_get_MostRecentlyUsedList,
};

DEFINE_IINSPECTABLE( permissions2, IStorageApplicationPermissionsStatics2, struct permissions_statics, IActivationFactory_iface )

/* A prefix has one user, so every user's lists are the same ones. */
static HRESULT WINAPI permissions2_GetFutureAccessListForUser( IStorageApplicationPermissionsStatics2 *iface, IUser *user,
                                                               IStorageItemAccessList **value )
{
    TRACE( "iface %p, user %p, value %p.\n", iface, user, value );
    *value = &future_list.IStorageItemAccessList_iface;
    return S_OK;
}

static HRESULT WINAPI permissions2_GetMostRecentlyUsedListForUser( IStorageApplicationPermissionsStatics2 *iface, IUser *user,
                                                                   IStorageItemMostRecentlyUsedList **value )
{
    TRACE( "iface %p, user %p, value %p.\n", iface, user, value );
    *value = &mru_list.IStorageItemMostRecentlyUsedList_iface;
    return S_OK;
}

static const struct IStorageApplicationPermissionsStatics2Vtbl permissions2_vtbl =
{
    permissions2_QueryInterface,
    permissions2_AddRef,
    permissions2_Release,
    /* IInspectable methods */
    permissions2_GetIids,
    permissions2_GetRuntimeClassName,
    permissions2_GetTrustLevel,
    /* IStorageApplicationPermissionsStatics2 methods */
    permissions2_GetFutureAccessListForUser,
    permissions2_GetMostRecentlyUsedListForUser,
};

static struct permissions_statics permissions_statics =
{
    {&factory_vtbl},
    {&permissions_vtbl},
    {&permissions2_vtbl},
};

IActivationFactory *storage_permissions_factory = &permissions_statics.IActivationFactory_iface;

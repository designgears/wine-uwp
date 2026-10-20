/* WinRT Windows.Storage.StorageLibrary Implementation
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

/* A library is a list of folders plus a save folder. A Wine prefix has no library database, so the
 * library for a KnownLibraryId is exactly the one known folder KnownFolders already maps to the
 * user's profile directory. Media Player's library pages ask for it once AppCapability reports
 * musicLibrary as Allowed. */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

/* IIterator<StorageFolder> over the one folder */

struct folder_iterator
{
    IIterator_StorageFolder IIterator_StorageFolder_iface;
    LONG ref;
    IStorageFolder *folder;
    BOOL done;
};

static inline struct folder_iterator *impl_from_IIterator_StorageFolder( IIterator_StorageFolder *iface )
{
    return CONTAINING_RECORD( iface, struct folder_iterator, IIterator_StorageFolder_iface );
}

static HRESULT WINAPI folder_iterator_QueryInterface( IIterator_StorageFolder *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_StorageFolder ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI folder_iterator_AddRef( IIterator_StorageFolder *iface )
{
    struct folder_iterator *impl = impl_from_IIterator_StorageFolder( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI folder_iterator_Release( IIterator_StorageFolder *iface )
{
    struct folder_iterator *impl = impl_from_IIterator_StorageFolder( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IStorageFolder_Release( impl->folder );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI folder_iterator_GetIids( IIterator_StorageFolder *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_iterator_GetRuntimeClassName( IIterator_StorageFolder *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Foundation.Collections.IIterator`1<Windows.Storage.StorageFolder>";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI folder_iterator_GetTrustLevel( IIterator_StorageFolder *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI folder_iterator_get_Current( IIterator_StorageFolder *iface, IStorageFolder **value )
{
    struct folder_iterator *impl = impl_from_IIterator_StorageFolder( iface );

    if (!value) return E_POINTER;
    if (impl->done)
    {
        *value = NULL;
        return E_BOUNDS;
    }
    IStorageFolder_AddRef( (*value = impl->folder) );
    return S_OK;
}

static HRESULT WINAPI folder_iterator_get_HasCurrent( IIterator_StorageFolder *iface, boolean *value )
{
    struct folder_iterator *impl = impl_from_IIterator_StorageFolder( iface );

    if (!value) return E_POINTER;
    *value = !impl->done;
    return S_OK;
}

static HRESULT WINAPI folder_iterator_MoveNext( IIterator_StorageFolder *iface, boolean *value )
{
    struct folder_iterator *impl = impl_from_IIterator_StorageFolder( iface );

    impl->done = TRUE;
    if (value) *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI folder_iterator_GetMany( IIterator_StorageFolder *iface, UINT32 count,
                                               IStorageFolder **items, UINT32 *written )
{
    struct folder_iterator *impl = impl_from_IIterator_StorageFolder( iface );

    if (!written) return E_POINTER;
    *written = 0;
    if (count && !impl->done)
    {
        IStorageFolder_AddRef( (items[0] = impl->folder) );
        impl->done = TRUE;
        *written = 1;
    }
    return S_OK;
}

static const struct IIterator_StorageFolderVtbl folder_iterator_vtbl =
{
    folder_iterator_QueryInterface,
    folder_iterator_AddRef,
    folder_iterator_Release,
    /* IInspectable methods */
    folder_iterator_GetIids,
    folder_iterator_GetRuntimeClassName,
    folder_iterator_GetTrustLevel,
    /* IIterator<StorageFolder> methods */
    folder_iterator_get_Current,
    folder_iterator_get_HasCurrent,
    folder_iterator_MoveNext,
    folder_iterator_GetMany,
};

/* The library's folder list. One object answers IObservableVector, IVector, IVectorView and
 * IIterable of StorageFolder: the contents are one fixed folder, so the shapes do not differ.
 * Mutation is refused, since there is no library database to write to, and VectorChanged
 * registrations succeed but never fire. */

struct folder_list
{
    IObservableVector_StorageFolder IObservableVector_StorageFolder_iface;
    IVector_StorageFolder IVector_StorageFolder_iface;
    IVectorView_StorageFolder IVectorView_StorageFolder_iface;
    IIterable_StorageFolder IIterable_StorageFolder_iface;
    LONG ref;
    IStorageFolder *folder;
};

static inline struct folder_list *impl_from_IObservableVector_StorageFolder( IObservableVector_StorageFolder *iface )
{
    return CONTAINING_RECORD( iface, struct folder_list, IObservableVector_StorageFolder_iface );
}

static HRESULT WINAPI folder_list_QueryInterface( IObservableVector_StorageFolder *iface, REFIID iid, void **out )
{
    struct folder_list *impl = impl_from_IObservableVector_StorageFolder( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IObservableVector_StorageFolder ))
        *out = &impl->IObservableVector_StorageFolder_iface;
    else if (IsEqualGUID( iid, &IID_IVector_StorageFolder ))
        *out = &impl->IVector_StorageFolder_iface;
    else if (IsEqualGUID( iid, &IID_IVectorView_StorageFolder ))
        *out = &impl->IVectorView_StorageFolder_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_StorageFolder ))
        *out = &impl->IIterable_StorageFolder_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI folder_list_AddRef( IObservableVector_StorageFolder *iface )
{
    struct folder_list *impl = impl_from_IObservableVector_StorageFolder( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI folder_list_Release( IObservableVector_StorageFolder *iface )
{
    struct folder_list *impl = impl_from_IObservableVector_StorageFolder( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IStorageFolder_Release( impl->folder );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI folder_list_GetIids( IObservableVector_StorageFolder *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_list_GetRuntimeClassName( IObservableVector_StorageFolder *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Foundation.Collections.IObservableVector`1<Windows.Storage.StorageFolder>";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI folder_list_GetTrustLevel( IObservableVector_StorageFolder *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI folder_list_add_VectorChanged( IObservableVector_StorageFolder *iface,
                                                     IVectorChangedEventHandler_StorageFolder *handler,
                                                     EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    token->value = 1;
    return S_OK;
}

static HRESULT WINAPI folder_list_remove_VectorChanged( IObservableVector_StorageFolder *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static const struct IObservableVector_StorageFolderVtbl folder_list_vtbl =
{
    folder_list_QueryInterface,
    folder_list_AddRef,
    folder_list_Release,
    /* IInspectable methods */
    folder_list_GetIids,
    folder_list_GetRuntimeClassName,
    folder_list_GetTrustLevel,
    /* IObservableVector<StorageFolder> methods */
    folder_list_add_VectorChanged,
    folder_list_remove_VectorChanged,
};

static HRESULT folder_list_get_at( struct folder_list *impl, UINT32 index, IStorageFolder **value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    if (index) return E_BOUNDS;
    IStorageFolder_AddRef( (*value = impl->folder) );
    return S_OK;
}

static HRESULT folder_list_index_of( struct folder_list *impl, IStorageFolder *item, UINT32 *index, boolean *found )
{
    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = item == impl->folder;
    return S_OK;
}

static HRESULT folder_list_get_many( struct folder_list *impl, UINT32 start, UINT32 count,
                                     IStorageFolder **items, UINT32 *written )
{
    if (!written) return E_POINTER;
    *written = 0;
    if (start > 1) return E_BOUNDS;
    if (!start && count)
    {
        IStorageFolder_AddRef( (items[0] = impl->folder) );
        *written = 1;
    }
    return S_OK;
}

DEFINE_IINSPECTABLE( folder_vector, IVector_StorageFolder, struct folder_list, IObservableVector_StorageFolder_iface )

static HRESULT WINAPI folder_vector_GetAt( IVector_StorageFolder *iface, UINT32 index, IStorageFolder **value )
{
    return folder_list_get_at( impl_from_IVector_StorageFolder( iface ), index, value );
}

static HRESULT WINAPI folder_vector_get_Size( IVector_StorageFolder *iface, UINT32 *value )
{
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI folder_vector_GetView( IVector_StorageFolder *iface, IVectorView_StorageFolder **value )
{
    struct folder_list *impl = impl_from_IVector_StorageFolder( iface );
    IVectorView_StorageFolder_AddRef( (*value = &impl->IVectorView_StorageFolder_iface) );
    return S_OK;
}

static HRESULT WINAPI folder_vector_IndexOf( IVector_StorageFolder *iface, IStorageFolder *item, UINT32 *index, boolean *found )
{
    return folder_list_index_of( impl_from_IVector_StorageFolder( iface ), item, index, found );
}

static HRESULT WINAPI folder_vector_SetAt( IVector_StorageFolder *iface, UINT32 index, IStorageFolder *item )
{
    FIXME( "iface %p, index %u, item %p: library folders are fixed.\n", iface, index, item );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_vector_InsertAt( IVector_StorageFolder *iface, UINT32 index, IStorageFolder *item )
{
    FIXME( "iface %p, index %u, item %p: library folders are fixed.\n", iface, index, item );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_vector_RemoveAt( IVector_StorageFolder *iface, UINT32 index )
{
    FIXME( "iface %p, index %u: library folders are fixed.\n", iface, index );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_vector_Append( IVector_StorageFolder *iface, IStorageFolder *item )
{
    FIXME( "iface %p, item %p: library folders are fixed.\n", iface, item );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_vector_RemoveAtEnd( IVector_StorageFolder *iface )
{
    FIXME( "iface %p: library folders are fixed.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_vector_Clear( IVector_StorageFolder *iface )
{
    FIXME( "iface %p: library folders are fixed.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_vector_GetMany( IVector_StorageFolder *iface, UINT32 start, UINT32 count,
                                             IStorageFolder **items, UINT32 *written )
{
    return folder_list_get_many( impl_from_IVector_StorageFolder( iface ), start, count, items, written );
}

static HRESULT WINAPI folder_vector_ReplaceAll( IVector_StorageFolder *iface, UINT32 count, IStorageFolder **items )
{
    FIXME( "iface %p, count %u, items %p: library folders are fixed.\n", iface, count, items );
    return E_NOTIMPL;
}

static const struct IVector_StorageFolderVtbl folder_vector_vtbl =
{
    folder_vector_QueryInterface,
    folder_vector_AddRef,
    folder_vector_Release,
    /* IInspectable methods */
    folder_vector_GetIids,
    folder_vector_GetRuntimeClassName,
    folder_vector_GetTrustLevel,
    /* IVector<StorageFolder> methods */
    folder_vector_GetAt,
    folder_vector_get_Size,
    folder_vector_GetView,
    folder_vector_IndexOf,
    folder_vector_SetAt,
    folder_vector_InsertAt,
    folder_vector_RemoveAt,
    folder_vector_Append,
    folder_vector_RemoveAtEnd,
    folder_vector_Clear,
    folder_vector_GetMany,
    folder_vector_ReplaceAll,
};

DEFINE_IINSPECTABLE( folder_view, IVectorView_StorageFolder, struct folder_list, IObservableVector_StorageFolder_iface )

static HRESULT WINAPI folder_view_GetAt( IVectorView_StorageFolder *iface, UINT32 index, IStorageFolder **value )
{
    return folder_list_get_at( impl_from_IVectorView_StorageFolder( iface ), index, value );
}

static HRESULT WINAPI folder_view_get_Size( IVectorView_StorageFolder *iface, UINT32 *value )
{
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI folder_view_IndexOf( IVectorView_StorageFolder *iface, IStorageFolder *item, UINT32 *index, boolean *found )
{
    return folder_list_index_of( impl_from_IVectorView_StorageFolder( iface ), item, index, found );
}

static HRESULT WINAPI folder_view_GetMany( IVectorView_StorageFolder *iface, UINT32 start, UINT32 count,
                                           IStorageFolder **items, UINT32 *written )
{
    return folder_list_get_many( impl_from_IVectorView_StorageFolder( iface ), start, count, items, written );
}

static const struct IVectorView_StorageFolderVtbl folder_view_vtbl =
{
    folder_view_QueryInterface,
    folder_view_AddRef,
    folder_view_Release,
    /* IInspectable methods */
    folder_view_GetIids,
    folder_view_GetRuntimeClassName,
    folder_view_GetTrustLevel,
    /* IVectorView<StorageFolder> methods */
    folder_view_GetAt,
    folder_view_get_Size,
    folder_view_IndexOf,
    folder_view_GetMany,
};

DEFINE_IINSPECTABLE( folder_iterable, IIterable_StorageFolder, struct folder_list, IObservableVector_StorageFolder_iface )

static HRESULT WINAPI folder_iterable_First( IIterable_StorageFolder *iface, IIterator_StorageFolder **value )
{
    struct folder_list *impl = impl_from_IIterable_StorageFolder( iface );
    struct folder_iterator *iter;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_StorageFolder_iface.lpVtbl = &folder_iterator_vtbl;
    iter->ref = 1;
    IStorageFolder_AddRef( (iter->folder = impl->folder) );
    *value = &iter->IIterator_StorageFolder_iface;
    return S_OK;
}

static const struct IIterable_StorageFolderVtbl folder_iterable_vtbl =
{
    folder_iterable_QueryInterface,
    folder_iterable_AddRef,
    folder_iterable_Release,
    /* IInspectable methods */
    folder_iterable_GetIids,
    folder_iterable_GetRuntimeClassName,
    folder_iterable_GetTrustLevel,
    /* IIterable<StorageFolder> methods */
    folder_iterable_First,
};

/* StorageLibrary */

struct storage_library
{
    IStorageLibrary IStorageLibrary_iface;
    IStorageLibrary2 IStorageLibrary2_iface;
    IStorageLibrary3 IStorageLibrary3_iface;
    LONG ref;
    IStorageFolder *folder;
    struct folder_list *folders;
};

static inline struct storage_library *impl_from_IStorageLibrary( IStorageLibrary *iface )
{
    return CONTAINING_RECORD( iface, struct storage_library, IStorageLibrary_iface );
}

static HRESULT WINAPI storage_library_QueryInterface( IStorageLibrary *iface, REFIID iid, void **out )
{
    struct storage_library *impl = impl_from_IStorageLibrary( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IStorageLibrary ))
        *out = &impl->IStorageLibrary_iface;
    else if (IsEqualGUID( iid, &IID_IStorageLibrary2 ))
        *out = &impl->IStorageLibrary2_iface;
    else if (IsEqualGUID( iid, &IID_IStorageLibrary3 ))
        *out = &impl->IStorageLibrary3_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI storage_library_AddRef( IStorageLibrary *iface )
{
    struct storage_library *impl = impl_from_IStorageLibrary( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI storage_library_Release( IStorageLibrary *iface )
{
    struct storage_library *impl = impl_from_IStorageLibrary( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IObservableVector_StorageFolder_Release( &impl->folders->IObservableVector_StorageFolder_iface );
        IStorageFolder_Release( impl->folder );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI storage_library_GetIids( IStorageLibrary *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_library_GetRuntimeClassName( IStorageLibrary *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Storage.StorageLibrary";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI storage_library_GetTrustLevel( IStorageLibrary *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

/* Adding or removing a library folder is a shell dialog on Windows; there is nothing behind it
 * here. E_NOTIMPL is an answer callers handle, a NULL operation is not. */
static HRESULT WINAPI storage_library_RequestAddFolderAsync( IStorageLibrary *iface, IAsyncOperation_StorageFolder **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_library_RequestRemoveFolderAsync( IStorageLibrary *iface, IStorageFolder *folder,
                                                                IAsyncOperation_boolean **operation )
{
    FIXME( "iface %p, folder %p, operation %p stub!\n", iface, folder, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_library_get_Folders( IStorageLibrary *iface, IObservableVector_StorageFolder **value )
{
    struct storage_library *impl = impl_from_IStorageLibrary( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    IObservableVector_StorageFolder_AddRef( (*value = &impl->folders->IObservableVector_StorageFolder_iface) );
    return S_OK;
}

static HRESULT WINAPI storage_library_get_SaveFolder( IStorageLibrary *iface, IStorageFolder **value )
{
    struct storage_library *impl = impl_from_IStorageLibrary( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    IStorageFolder_AddRef( (*value = impl->folder) );
    return S_OK;
}

/* The definition cannot change, so the handler never fires; the registration must succeed. */
static HRESULT WINAPI storage_library_add_DefinitionChanged( IStorageLibrary *iface,
                                                             ITypedEventHandler_StorageLibrary_IInspectable *handler,
                                                             EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    token->value = 1;
    return S_OK;
}

static HRESULT WINAPI storage_library_remove_DefinitionChanged( IStorageLibrary *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static const struct IStorageLibraryVtbl storage_library_vtbl =
{
    storage_library_QueryInterface,
    storage_library_AddRef,
    storage_library_Release,
    /* IInspectable methods */
    storage_library_GetIids,
    storage_library_GetRuntimeClassName,
    storage_library_GetTrustLevel,
    /* IStorageLibrary methods */
    storage_library_RequestAddFolderAsync,
    storage_library_RequestRemoveFolderAsync,
    storage_library_get_Folders,
    storage_library_get_SaveFolder,
    storage_library_add_DefinitionChanged,
    storage_library_remove_DefinitionChanged,
};

DEFINE_IINSPECTABLE( storage_library2, IStorageLibrary2, struct storage_library, IStorageLibrary_iface )

/* A StorageLibraryChangeTracker would be a second class whose methods we cannot honour. */
static HRESULT WINAPI storage_library2_get_ChangeTracker( IStorageLibrary2 *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static const struct IStorageLibrary2Vtbl storage_library2_vtbl =
{
    storage_library2_QueryInterface,
    storage_library2_AddRef,
    storage_library2_Release,
    /* IInspectable methods */
    storage_library2_GetIids,
    storage_library2_GetRuntimeClassName,
    storage_library2_GetTrustLevel,
    /* IStorageLibrary2 methods */
    storage_library2_get_ChangeTracker,
};

DEFINE_IINSPECTABLE( storage_library3, IStorageLibrary3, struct storage_library, IStorageLibrary_iface )

static HRESULT no_folder_suggestions( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_BOOL;
    result->boolVal = VARIANT_FALSE;
    return S_OK;
}

static HRESULT WINAPI storage_library3_AreFolderSuggestionsAvailableAsync( IStorageLibrary3 *iface,
                                                                          IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return async_operation_boolean_create( (IUnknown *)iface, NULL, no_folder_suggestions, operation );
}

static const struct IStorageLibrary3Vtbl storage_library3_vtbl =
{
    storage_library3_QueryInterface,
    storage_library3_AddRef,
    storage_library3_Release,
    /* IInspectable methods */
    storage_library3_GetIids,
    storage_library3_GetRuntimeClassName,
    storage_library3_GetTrustLevel,
    /* IStorageLibrary3 methods */
    storage_library3_AreFolderSuggestionsAvailableAsync,
};

static HRESULT library_folder( KnownLibraryId id, IStorageFolder **folder )
{
    IKnownFoldersStatics *statics;
    HRESULT hr;

    if (FAILED(hr = IActivationFactory_QueryInterface( known_folders_factory, &IID_IKnownFoldersStatics, (void **)&statics )))
        return hr;
    switch (id)
    {
    case KnownLibraryId_Music:     hr = IKnownFoldersStatics_get_MusicLibrary( statics, folder ); break;
    case KnownLibraryId_Pictures:  hr = IKnownFoldersStatics_get_PicturesLibrary( statics, folder ); break;
    case KnownLibraryId_Videos:    hr = IKnownFoldersStatics_get_VideosLibrary( statics, folder ); break;
    case KnownLibraryId_Documents: hr = IKnownFoldersStatics_get_DocumentsLibrary( statics, folder ); break;
    default:
        WARN( "unknown library %d.\n", id );
        hr = E_INVALIDARG;
        break;
    }
    IKnownFoldersStatics_Release( statics );
    return hr;
}

static HRESULT storage_library_create( KnownLibraryId id, IStorageLibrary **out )
{
    struct storage_library *impl;
    struct folder_list *folders;
    IStorageFolder *folder;
    HRESULT hr;

    if (FAILED(hr = library_folder( id, &folder ))) return hr;
    if (!(impl = calloc( 1, sizeof(*impl) )) || !(folders = calloc( 1, sizeof(*folders) )))
    {
        free( impl );
        IStorageFolder_Release( folder );
        return E_OUTOFMEMORY;
    }
    folders->IObservableVector_StorageFolder_iface.lpVtbl = &folder_list_vtbl;
    folders->IVector_StorageFolder_iface.lpVtbl = &folder_vector_vtbl;
    folders->IVectorView_StorageFolder_iface.lpVtbl = &folder_view_vtbl;
    folders->IIterable_StorageFolder_iface.lpVtbl = &folder_iterable_vtbl;
    folders->ref = 1;
    IStorageFolder_AddRef( (folders->folder = folder) );

    impl->IStorageLibrary_iface.lpVtbl = &storage_library_vtbl;
    impl->IStorageLibrary2_iface.lpVtbl = &storage_library2_vtbl;
    impl->IStorageLibrary3_iface.lpVtbl = &storage_library3_vtbl;
    impl->ref = 1;
    impl->folder = folder;
    impl->folders = folders;

    TRACE( "library %d -> %p, folder %p.\n", id, impl, folder );
    *out = &impl->IStorageLibrary_iface;
    return S_OK;
}

/* StorageLibrary statics */

struct storage_library_statics
{
    IActivationFactory IActivationFactory_iface;
    IStorageLibraryStatics IStorageLibraryStatics_iface;
    IStorageLibraryStatics2 IStorageLibraryStatics2_iface;
    LONG ref;
};

static inline struct storage_library_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct storage_library_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct storage_library_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IStorageLibraryStatics ))
        *out = &impl->IStorageLibraryStatics_iface;
    else if (IsEqualGUID( iid, &IID_IStorageLibraryStatics2 ))
        *out = &impl->IStorageLibraryStatics2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct storage_library_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct storage_library_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
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
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
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

/* The library is made before the operation, so the async callback only hands it over. */
static HRESULT return_library( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_UNKNOWN;
    result->punkVal = param;
    IUnknown_AddRef( param );
    return S_OK;
}

static HRESULT get_library_async( IUnknown *invoker, KnownLibraryId id, IAsyncOperation_StorageLibrary **operation )
{
    IStorageLibrary *library;
    HRESULT hr;

    *operation = NULL;
    if (FAILED(hr = storage_library_create( id, &library ))) return hr;
    hr = async_operation_storage_library_create( invoker, (IUnknown *)library, return_library, operation );
    IStorageLibrary_Release( library );
    return hr;
}

DEFINE_IINSPECTABLE( statics, IStorageLibraryStatics, struct storage_library_statics, IActivationFactory_iface )

static HRESULT WINAPI statics_GetLibraryAsync( IStorageLibraryStatics *iface, KnownLibraryId id,
                                               IAsyncOperation_StorageLibrary **operation )
{
    TRACE( "iface %p, id %d, operation %p.\n", iface, id, operation );
    return get_library_async( (IUnknown *)iface, id, operation );
}

static const struct IStorageLibraryStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IStorageLibraryStatics methods */
    statics_GetLibraryAsync,
};

DEFINE_IINSPECTABLE( statics2, IStorageLibraryStatics2, struct storage_library_statics, IActivationFactory_iface )

/* A prefix has one user, so every user's library is the same one. */
static HRESULT WINAPI statics2_GetLibraryForUserAsync( IStorageLibraryStatics2 *iface, IUser *user, KnownLibraryId id,
                                                       IAsyncOperation_StorageLibrary **operation )
{
    TRACE( "iface %p, user %p, id %d, operation %p.\n", iface, user, id, operation );
    return get_library_async( (IUnknown *)iface, id, operation );
}

static const struct IStorageLibraryStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* IStorageLibraryStatics2 methods */
    statics2_GetLibraryForUserAsync,
};

static struct storage_library_statics storage_library_statics =
{
    {&factory_vtbl},
    {&statics_vtbl},
    {&statics2_vtbl},
    1,
};

IActivationFactory *storage_library_factory = &storage_library_statics.IActivationFactory_iface;

/* WinRT Windows.Storage.StorageFolder Implementation
 *
 * Copyright (C) 2026 Bobby Kircher
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

#include "private.h"
#include "shlobj.h"
#include "knownfolders.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

struct storage_folder
{
    IStorageFolder IStorageFolder_iface;
    IStorageItem IStorageItem_iface;
    IStorageFolderQueryOperations IStorageFolderQueryOperations_iface;
    IStorageFolder2 IStorageFolder2_iface;
    LONG ref;
    WCHAR *path;
};

static inline struct storage_folder *impl_from_IStorageFolder( IStorageFolder *iface )
{
    return CONTAINING_RECORD( iface, struct storage_folder, IStorageFolder_iface );
}

static HRESULT WINAPI storage_folder_QueryInterface( IStorageFolder *iface, REFIID iid, void **out )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IStorageFolder ))
    {
        *out = &impl->IStorageFolder_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IStorageItem ))
    {
        *out = &impl->IStorageItem_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    /* Refusing this made .NET's cast of the publisher cache folder throw InvalidCastException
     * (Solitaire's AdConsent "PublisherStorageAccessFailure"). See Findings-M4. */
    if (IsEqualGUID( iid, &IID_IStorageFolder2 ))
    {
        *out = &impl->IStorageFolder2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IStorageFolderQueryOperations ))
    {
        *out = &impl->IStorageFolderQueryOperations_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI storage_folder_AddRef( IStorageFolder *iface )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI storage_folder_Release( IStorageFolder *iface )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        free( impl->path );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI storage_folder_GetIids( IStorageFolder *iface, ULONG *iid_count, IID **iids )
{
    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );

    if (!iid_count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( 2 * sizeof(IID) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_IStorageFolder;
    (*iids)[1] = IID_IStorageItem;
    *iid_count = 2;
    return S_OK;
}

static HRESULT WINAPI storage_folder_GetRuntimeClassName( IStorageFolder *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    if (!class_name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Storage_StorageFolder,
                                ARRAY_SIZE(RuntimeClass_Windows_Storage_StorageFolder) - 1, class_name );
}

static HRESULT WINAPI storage_folder_GetTrustLevel( IStorageFolder *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );

    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

/* "<folder path>\<name>", as an async parameter for the callbacks below. */
static HRESULT folder_child_param( struct storage_folder *impl, HSTRING name, IUnknown **param )
{
    const WCHAR *buffer;
    WCHAR *path;
    HRESULT hr;
    SIZE_T len;

    if (!(buffer = WindowsGetStringRawBuffer( name, NULL ))) return E_INVALIDARG;
    len = wcslen( impl->path ) + 1 + wcslen( buffer ) + 1;
    if (!(path = malloc( len * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    swprintf( path, len, L"%s\\%s", impl->path, buffer );
    hr = storage_path_param_create( path, param );
    free( path );
    return hr;
}

static HRESULT child_as_item( const WCHAR *path, DWORD attributes, IStorageItem **item )
{
    HRESULT hr;

    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        IStorageFolder *folder;
        if (FAILED(hr = __wine_storage_folder_create( path, &folder ))) return hr;
        hr = IStorageFolder_QueryInterface( folder, &IID_IStorageItem, (void **)item );
        IStorageFolder_Release( folder );
        return hr;
    }
    else
    {
        IStorageFile *file;
        if (FAILED(hr = storage_file_create( path, &file ))) return hr;
        hr = IStorageFile_QueryInterface( file, &IID_IStorageItem, (void **)item );
        IStorageFile_Release( file );
        return hr;
    }
}

static HRESULT get_child_file( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    const WCHAR *path = storage_path_param_buffer( param );
    IStorageFile *file;
    HRESULT hr;

    if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    if (FAILED(hr = storage_file_create( path, &file ))) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)file;
    return S_OK;
}

static HRESULT get_child_folder( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    const WCHAR *path = storage_path_param_buffer( param );
    DWORD attributes = GetFileAttributesW( path );
    IStorageFolder *folder;
    HRESULT hr;

    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return HRESULT_FROM_WIN32( ERROR_PATH_NOT_FOUND );
    if (FAILED(hr = __wine_storage_folder_create( path, &folder ))) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)folder;
    return S_OK;
}

static HRESULT get_child_item( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    const WCHAR *path = storage_path_param_buffer( param );
    DWORD attributes = GetFileAttributesW( path );
    IStorageItem *item;
    HRESULT hr;

    if (attributes == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    if (FAILED(hr = child_as_item( path, attributes, &item ))) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)item;
    return S_OK;
}

/* TryGetItemAsync differs from GetItemAsync in exactly one way: a missing item is not an error,
 * it completes with NULL. Callers test the result for null instead of catching. */
static HRESULT try_get_child_item( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    const WCHAR *path = storage_path_param_buffer( param );
    DWORD attributes = GetFileAttributesW( path );
    IStorageItem *item = NULL;
    HRESULT hr;

    result->vt = VT_UNKNOWN;
    result->punkVal = NULL;
    if (attributes == INVALID_FILE_ATTRIBUTES) return S_OK;
    if (FAILED(hr = child_as_item( path, attributes, &item ))) return hr;
    result->punkVal = (IUnknown *)item;
    return S_OK;
}

/* Create every missing component of a (possibly multi-segment) relative path. The app asks for
 * things like "user_data\Guest\launch_data" in one call, which CreateDirectoryW cannot do. */
static BOOL create_directory_tree( const WCHAR *path )
{
    WCHAR *copy, *p;
    BOOL ret = TRUE;

    if (!(copy = wcsdup( path ))) return FALSE;
    for (p = copy; *p; p++)
    {
        if (*p != '\\' && *p != '/') continue;
        *p = 0;
        if (wcslen( copy ) > 2) CreateDirectoryW( copy, NULL );
        *p = '\\';
    }
    if (!CreateDirectoryW( copy, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS) ret = FALSE;
    free( copy );
    return ret;
}

/* The async callbacks below only receive the path, so the collision option is applied here, before
 * the operation is created. If the create fails the callback's open fails too, which surfaces as a
 * faulted operation - the same thing the caller would see from Windows. */
static HRESULT folder_create_child( struct storage_folder *impl, HSTRING name, BOOL directory,
                                    CreationCollisionOption options, IUnknown **param )
{
    const WCHAR *path;
    HRESULT hr;

    if (FAILED(hr = folder_child_param( impl, name, param ))) return hr;
    path = storage_path_param_buffer( *param );

    if (directory)
    {
        if (options == CreationCollisionOption_ReplaceExisting) RemoveDirectoryW( path );
        create_directory_tree( path );
    }
    else
    {
        DWORD disposition = OPEN_ALWAYS;
        HANDLE file;

        switch (options)
        {
        case CreationCollisionOption_ReplaceExisting: disposition = CREATE_ALWAYS; break;
        case CreationCollisionOption_FailIfExists:    disposition = CREATE_NEW; break;
        /* GenerateUniqueName would need a "name (2)" search; opening the existing file is closer to
         * useful than failing, and no caller here relies on the rename. */
        default:                                      disposition = OPEN_ALWAYS; break;
        }
        file = CreateFileW( path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL );
        if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
    }
    return S_OK;
}

static HRESULT WINAPI storage_folder_CreateFileAsync( IStorageFolder *iface, HSTRING name, CreationCollisionOption options,
                                                      IAsyncOperation_StorageFile **operation )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, name %s, options %d, operation %p.\n", iface, debugstr_hstring( name ), options, operation );

    if (!operation) return E_POINTER;
    if (FAILED(hr = folder_create_child( impl, name, FALSE, options, &param ))) return hr;
    hr = async_operation_storage_file_create( (IUnknown *)iface, param, get_child_file, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI storage_folder_CreateFileAsyncOverloadDefaultOptions( IStorageFolder *iface, HSTRING name,
                                                                            IAsyncOperation_StorageFile **operation )
{
    return storage_folder_CreateFileAsync( iface, name, CreationCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_folder_CreateFolderAsync( IStorageFolder *iface, HSTRING name, CreationCollisionOption options,
                                                        IAsyncOperation_StorageFolder **operation )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, name %s, options %d, operation %p.\n", iface, debugstr_hstring( name ), options, operation );

    if (!operation) return E_POINTER;
    if (FAILED(hr = folder_create_child( impl, name, TRUE, options, &param ))) return hr;
    hr = async_operation_storage_folder_create( (IUnknown *)iface, param, get_child_folder, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI storage_folder_CreateFolderAsyncOverloadDefaultOptions( IStorageFolder *iface, HSTRING name,
                                                                              IAsyncOperation_StorageFolder **operation )
{
    return storage_folder_CreateFolderAsync( iface, name, CreationCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_folder_GetFileAsync( IStorageFolder *iface, HSTRING name, IAsyncOperation_StorageFile **operation )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, name %s, operation %p.\n", iface, debugstr_hstring( name ), operation );

    if (!operation) return E_POINTER;
    if (FAILED(hr = folder_child_param( impl, name, &param ))) return hr;
    hr = async_operation_storage_file_create( (IUnknown *)iface, param, get_child_file, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI storage_folder_GetFolderAsync( IStorageFolder *iface, HSTRING name, IAsyncOperation_StorageFolder **operation )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, name %s, operation %p.\n", iface, debugstr_hstring( name ), operation );

    if (!operation) return E_POINTER;
    if (FAILED(hr = folder_child_param( impl, name, &param ))) return hr;
    hr = async_operation_storage_folder_create( (IUnknown *)iface, param, get_child_folder, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI storage_folder_GetItemAsync( IStorageFolder *iface, HSTRING name, IAsyncOperation_IStorageItem **operation )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, name %s, operation %p.\n", iface, debugstr_hstring( name ), operation );

    if (!operation) return E_POINTER;
    if (FAILED(hr = folder_child_param( impl, name, &param ))) return hr;
    hr = async_operation_storage_item_create( (IUnknown *)iface, param, get_child_item, operation );
    IUnknown_Release( param );
    return hr;
}

/* IStorageFolder's GetFilesAsync(), GetFoldersAsync() and GetItemsAsync() are the default queries of
 * IStorageFolderQueryOperations on the same folder (the Microsoft Store lists its account cache with the first). */
static HRESULT WINAPI folder_query_GetFilesAsyncOverloadDefaultStartAndCount( IStorageFolderQueryOperations *iface,
        CommonFileQuery query, IAsyncOperation_IVectorView_StorageFile **operation );
static HRESULT WINAPI folder_query_GetFoldersAsyncOverloadDefaultStartAndCount( IStorageFolderQueryOperations *iface,
        CommonFolderQuery query, IAsyncOperation_IVectorView_StorageFolder **operation );
static HRESULT WINAPI folder_query_GetItemsAsync( IStorageFolderQueryOperations *iface, UINT32 start_index,
        UINT32 max_items_to_retrieve, IAsyncOperation_IVectorView_IStorageItem **operation );

static HRESULT WINAPI storage_folder_GetFilesAsyncOverloadDefaultOptionsStartAndCount( IStorageFolder *iface,
                                                                                       IAsyncOperation_IVectorView_StorageFile **operation )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return folder_query_GetFilesAsyncOverloadDefaultStartAndCount( &impl->IStorageFolderQueryOperations_iface,
                                                                   CommonFileQuery_DefaultQuery, operation );
}

static HRESULT WINAPI storage_folder_GetFoldersAsyncOverloadDefaultOptionsStartAndCount( IStorageFolder *iface,
                                                                                         IAsyncOperation_IVectorView_StorageFolder **operation )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return folder_query_GetFoldersAsyncOverloadDefaultStartAndCount( &impl->IStorageFolderQueryOperations_iface,
                                                                     CommonFolderQuery_DefaultQuery, operation );
}

static HRESULT WINAPI storage_folder_GetItemsAsyncOverloadDefaultStartAndCount( IStorageFolder *iface,
                                                                                IAsyncOperation_IVectorView_IStorageItem **operation )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return folder_query_GetItemsAsync( &impl->IStorageFolderQueryOperations_iface, 0, ~0u, operation );
}

static const struct IStorageFolderVtbl storage_folder_vtbl =
{
    storage_folder_QueryInterface,
    storage_folder_AddRef,
    storage_folder_Release,
    /* IInspectable methods */
    storage_folder_GetIids,
    storage_folder_GetRuntimeClassName,
    storage_folder_GetTrustLevel,
    /* IStorageFolder methods */
    storage_folder_CreateFileAsyncOverloadDefaultOptions,
    storage_folder_CreateFileAsync,
    storage_folder_CreateFolderAsyncOverloadDefaultOptions,
    storage_folder_CreateFolderAsync,
    storage_folder_GetFileAsync,
    storage_folder_GetFolderAsync,
    storage_folder_GetItemAsync,
    storage_folder_GetFilesAsyncOverloadDefaultOptionsStartAndCount,
    storage_folder_GetFoldersAsyncOverloadDefaultOptionsStartAndCount,
    storage_folder_GetItemsAsyncOverloadDefaultStartAndCount,
};

DEFINE_IINSPECTABLE( storage_item, IStorageItem, struct storage_folder, IStorageFolder_iface )

static HRESULT WINAPI storage_item_RenameAsyncOverloadDefaultOptions( IStorageItem *iface, HSTRING name, IAsyncAction **operation )
{
    FIXME( "iface %p, name %s, operation %p stub!\n", iface, debugstr_hstring( name ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_RenameAsync( IStorageItem *iface, HSTRING name, NameCollisionOption option, IAsyncAction **operation )
{
    FIXME( "iface %p, name %s, option %d, operation %p stub!\n", iface, debugstr_hstring( name ), option, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_DeleteAsyncOverloadDefaultOptions( IStorageItem *iface, IAsyncAction **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_DeleteAsync( IStorageItem *iface, StorageDeleteOption option, IAsyncAction **operation )
{
    FIXME( "iface %p, option %d, operation %p stub!\n", iface, option, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_GetBasicPropertiesAsync( IStorageItem *iface, IAsyncOperation_BasicProperties **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_get_Name( IStorageItem *iface, HSTRING *value )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    const WCHAR *name;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    /* the last component, without the trailing separator a root path would have */
    if ((name = wcsrchr( impl->path, '\\' )) && name[1]) name++;
    else name = impl->path;
    return WindowsCreateString( name, wcslen( name ), value );
}

static HRESULT WINAPI storage_item_get_Path( IStorageItem *iface, HSTRING *value )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsCreateString( impl->path, wcslen( impl->path ), value );
}

static HRESULT WINAPI storage_item_get_Attributes( IStorageItem *iface, FileAttributes *value )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    DWORD attrs;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if ((attrs = GetFileAttributesW( impl->path )) == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32( GetLastError() );
    /* FileAttributes mirrors the Win32 attribute bits this far */
    *value = attrs & (FileAttributes_ReadOnly | FileAttributes_Directory | FileAttributes_Archive |
                      FileAttributes_Temporary);
    return S_OK;
}

static HRESULT WINAPI storage_item_get_DateCreated( IStorageItem *iface, DateTime *value )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    WIN32_FILE_ATTRIBUTE_DATA data;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (!GetFileAttributesExW( impl->path, GetFileExInfoStandard, &data ))
        return HRESULT_FROM_WIN32( GetLastError() );
    value->UniversalTime = ((UINT64)data.ftCreationTime.dwHighDateTime << 32) | data.ftCreationTime.dwLowDateTime;
    return S_OK;
}

static HRESULT WINAPI storage_item_IsOfType( IStorageItem *iface, StorageItemTypes type, boolean *value )
{
    TRACE( "iface %p, type %d, value %p.\n", iface, type, value );

    if (!value) return E_POINTER;
    *value = !!(type & StorageItemTypes_Folder);
    return S_OK;
}

static const struct IStorageItemVtbl storage_item_vtbl =
{
    storage_item_QueryInterface,
    storage_item_AddRef,
    storage_item_Release,
    /* IInspectable methods */
    storage_item_GetIids,
    storage_item_GetRuntimeClassName,
    storage_item_GetTrustLevel,
    /* IStorageItem methods */
    storage_item_RenameAsyncOverloadDefaultOptions,
    storage_item_RenameAsync,
    storage_item_DeleteAsyncOverloadDefaultOptions,
    storage_item_DeleteAsync,
    storage_item_GetBasicPropertiesAsync,
    storage_item_get_Name,
    storage_item_get_Path,
    storage_item_get_Attributes,
    storage_item_get_DateCreated,
    storage_item_IsOfType,
};


/*
 * Read-only vector views over a snapshot of directory entries: what the folder query operations
 * below hand back. One struct per element type, all the same shape.
 */

struct file_view
{
    IVectorView_StorageFile IVectorView_StorageFile_iface;
    IIterableStorageFileInternal IIterableStorageFileInternal_iface;
    LONG ref;

    IStorageFile **items;
    UINT32 count;
};

/* A cursor over a file_view, so the collection can be enumerated and not just counted. */
struct file_iterator
{
    IIteratorStorageFileInternal IIteratorStorageFileInternal_iface;
    LONG ref;

    IVectorView_StorageFile *view;
    UINT32 index;
};

static inline struct file_view *impl_from_IVectorView_StorageFile( IVectorView_StorageFile *iface )
{
    return CONTAINING_RECORD( iface, struct file_view, IVectorView_StorageFile_iface );
}

static HRESULT WINAPI file_view_QueryInterface( IVectorView_StorageFile *iface, REFIID iid, void **out )
{
    struct file_view *impl = impl_from_IVectorView_StorageFile( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IVectorView_StorageFile ))
    {
        *out = &impl->IVectorView_StorageFile_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    /* .NET Native projects IVectorView<T> as IReadOnlyList<T>, but enumerating it goes through
     * IIterable<T>. Refusing that leaves a caller able to read the count and nothing else. */
    if (IsEqualGUID( iid, &IID_IIterableStorageFileInternal ))
    {
        *out = &impl->IIterableStorageFileInternal_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI file_view_AddRef( IVectorView_StorageFile *iface )
{
    return InterlockedIncrement( &impl_from_IVectorView_StorageFile( iface )->ref );
}

static ULONG WINAPI file_view_Release( IVectorView_StorageFile *iface )
{
    struct file_view *impl = impl_from_IVectorView_StorageFile( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IStorageFile_Release( impl->items[i] );
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI file_view_GetIids( IVectorView_StorageFile *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_view_GetRuntimeClassName( IVectorView_StorageFile *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_view_GetTrustLevel( IVectorView_StorageFile *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI file_view_GetAt( IVectorView_StorageFile *iface, UINT32 index, IStorageFile **value )
{
    struct file_view *impl = impl_from_IVectorView_StorageFile( iface );

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    IStorageFile_AddRef( (*value = impl->items[index]) );
    return S_OK;
}

static HRESULT WINAPI file_view_get_Size( IVectorView_StorageFile *iface, UINT32 *value )
{
    struct file_view *impl = impl_from_IVectorView_StorageFile( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI file_view_IndexOf( IVectorView_StorageFile *iface, IStorageFile *element, UINT32 *index, BOOLEAN *found )
{
    struct file_view *impl = impl_from_IVectorView_StorageFile( iface );
    UINT32 i;

    TRACE( "iface %p, element %p, index %p, found %p.\n", iface, element, index, found );

    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = FALSE;
    for (i = 0; i < impl->count; i++) if (impl->items[i] == element)
    {
        *index = i;
        *found = TRUE;
        break;
    }
    return S_OK;
}

static HRESULT WINAPI file_view_GetMany( IVectorView_StorageFile *iface, UINT32 start_index, UINT32 items_size,
                                       IStorageFile **items, UINT32 *value )
{
    struct file_view *impl = impl_from_IVectorView_StorageFile( iface );
    UINT32 i;

    TRACE( "iface %p, start_index %u, items_size %u, items %p, value %p.\n", iface, start_index,
           items_size, items, value );

    if (!value) return E_POINTER;
    *value = 0;
    for (i = 0; i < items_size && start_index + i < impl->count; i++)
        IStorageFile_AddRef( (items[i] = impl->items[start_index + i]) );
    *value = i;
    return S_OK;
}

static const struct IVectorView_StorageFileVtbl file_view_vtbl =
{
    file_view_QueryInterface,
    file_view_AddRef,
    file_view_Release,
    /* IInspectable methods */
    file_view_GetIids,
    file_view_GetRuntimeClassName,
    file_view_GetTrustLevel,
    /* IVectorView<T> methods */
    file_view_GetAt,
    file_view_get_Size,
    file_view_IndexOf,
    file_view_GetMany,
};

static inline struct file_iterator *impl_from_IIteratorStorageFileInternal( IIteratorStorageFileInternal *iface )
{
    return CONTAINING_RECORD( iface, struct file_iterator, IIteratorStorageFileInternal_iface );
}

static HRESULT WINAPI file_iterator_QueryInterface( IIteratorStorageFileInternal *iface, REFIID iid, void **out )
{
    struct file_iterator *impl = impl_from_IIteratorStorageFileInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IIteratorStorageFileInternal ))
    {
        *out = &impl->IIteratorStorageFileInternal_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI file_iterator_AddRef( IIteratorStorageFileInternal *iface )
{
    return InterlockedIncrement( &impl_from_IIteratorStorageFileInternal( iface )->ref );
}

static ULONG WINAPI file_iterator_Release( IIteratorStorageFileInternal *iface )
{
    struct file_iterator *impl = impl_from_IIteratorStorageFileInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVectorView_StorageFile_Release( impl->view );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI file_iterator_GetIids( IIteratorStorageFileInternal *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_iterator_GetRuntimeClassName( IIteratorStorageFileInternal *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.Collections.IIterator`1<Windows.Storage.StorageFile>",
                                ARRAY_SIZE(L"Windows.Foundation.Collections.IIterator`1<Windows.Storage.StorageFile>") - 1,
                                class_name );
}

static HRESULT WINAPI file_iterator_GetTrustLevel( IIteratorStorageFileInternal *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI file_iterator_get_Current( IIteratorStorageFileInternal *iface, IStorageFile **value )
{
    struct file_iterator *impl = impl_from_IIteratorStorageFileInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    return IVectorView_StorageFile_GetAt( impl->view, impl->index, value );
}

static HRESULT WINAPI file_iterator_get_HasCurrent( IIteratorStorageFileInternal *iface, boolean *value )
{
    struct file_iterator *impl = impl_from_IIteratorStorageFileInternal( iface );
    UINT32 size;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (FAILED(hr = IVectorView_StorageFile_get_Size( impl->view, &size ))) return hr;
    *value = impl->index < size;
    return S_OK;
}

static HRESULT WINAPI file_iterator_MoveNext( IIteratorStorageFileInternal *iface, boolean *value )
{
    struct file_iterator *impl = impl_from_IIteratorStorageFileInternal( iface );
    UINT32 size;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (FAILED(hr = IVectorView_StorageFile_get_Size( impl->view, &size ))) return hr;
    if (impl->index < size) impl->index++;
    return file_iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI file_iterator_GetMany( IIteratorStorageFileInternal *iface, UINT32 items_size,
                                             IStorageFile **items, UINT32 *value )
{
    struct file_iterator *impl = impl_from_IIteratorStorageFileInternal( iface );
    HRESULT hr;

    TRACE( "iface %p, items_size %u, items %p, value %p.\n", iface, items_size, items, value );

    if (FAILED(hr = IVectorView_StorageFile_GetMany( impl->view, impl->index, items_size, items, value )))
        return hr;
    impl->index += *value;
    return S_OK;
}

static const struct IIteratorStorageFileInternalVtbl file_iterator_vtbl =
{
    /* IUnknown methods */
    file_iterator_QueryInterface,
    file_iterator_AddRef,
    file_iterator_Release,
    /* IInspectable methods */
    file_iterator_GetIids,
    file_iterator_GetRuntimeClassName,
    file_iterator_GetTrustLevel,
    /* IIterator<StorageFile> methods */
    file_iterator_get_Current,
    file_iterator_get_HasCurrent,
    file_iterator_MoveNext,
    file_iterator_GetMany,
};

DEFINE_IINSPECTABLE_( file_iterable, IIterableStorageFileInternal, struct file_view,
                      impl_from_IIterableStorageFileInternal, IIterableStorageFileInternal_iface,
                      &impl->IVectorView_StorageFile_iface )

static HRESULT WINAPI file_iterable_First( IIterableStorageFileInternal *iface,
                                           IIteratorStorageFileInternal **value )
{
    struct file_view *impl = impl_from_IIterableStorageFileInternal( iface );
    struct file_iterator *iter;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIteratorStorageFileInternal_iface.lpVtbl = &file_iterator_vtbl;
    iter->ref = 1;
    IVectorView_StorageFile_AddRef( (iter->view = &impl->IVectorView_StorageFile_iface) );

    *value = &iter->IIteratorStorageFileInternal_iface;
    return S_OK;
}

static const struct IIterableStorageFileInternalVtbl file_iterable_vtbl =
{
    /* IUnknown methods */
    file_iterable_QueryInterface,
    file_iterable_AddRef,
    file_iterable_Release,
    /* IInspectable methods */
    file_iterable_GetIids,
    file_iterable_GetRuntimeClassName,
    file_iterable_GetTrustLevel,
    /* IIterable<StorageFile> methods */
    file_iterable_First,
};

static HRESULT file_view_create( IStorageFile **items, UINT32 count, IVectorView_StorageFile **out )
{
    struct file_view *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_StorageFile_iface.lpVtbl = &file_view_vtbl;
    impl->IIterableStorageFileInternal_iface.lpVtbl = &file_iterable_vtbl;
    impl->ref = 1;
    impl->items = items;
    impl->count = count;

    *out = &impl->IVectorView_StorageFile_iface;
    return S_OK;
}

struct folder_view
{
    IVectorView_StorageFolder IVectorView_StorageFolder_iface;
    LONG ref;

    IStorageFolder **items;
    UINT32 count;
};

static inline struct folder_view *impl_from_IVectorView_StorageFolder( IVectorView_StorageFolder *iface )
{
    return CONTAINING_RECORD( iface, struct folder_view, IVectorView_StorageFolder_iface );
}

static HRESULT WINAPI folder_view_QueryInterface( IVectorView_StorageFolder *iface, REFIID iid, void **out )
{
    struct folder_view *impl = impl_from_IVectorView_StorageFolder( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IVectorView_StorageFolder ))
    {
        *out = &impl->IVectorView_StorageFolder_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI folder_view_AddRef( IVectorView_StorageFolder *iface )
{
    return InterlockedIncrement( &impl_from_IVectorView_StorageFolder( iface )->ref );
}

static ULONG WINAPI folder_view_Release( IVectorView_StorageFolder *iface )
{
    struct folder_view *impl = impl_from_IVectorView_StorageFolder( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IStorageFolder_Release( impl->items[i] );
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI folder_view_GetIids( IVectorView_StorageFolder *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_view_GetRuntimeClassName( IVectorView_StorageFolder *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_view_GetTrustLevel( IVectorView_StorageFolder *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI folder_view_GetAt( IVectorView_StorageFolder *iface, UINT32 index, IStorageFolder **value )
{
    struct folder_view *impl = impl_from_IVectorView_StorageFolder( iface );

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    IStorageFolder_AddRef( (*value = impl->items[index]) );
    return S_OK;
}

static HRESULT WINAPI folder_view_get_Size( IVectorView_StorageFolder *iface, UINT32 *value )
{
    struct folder_view *impl = impl_from_IVectorView_StorageFolder( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI folder_view_IndexOf( IVectorView_StorageFolder *iface, IStorageFolder *element, UINT32 *index, BOOLEAN *found )
{
    struct folder_view *impl = impl_from_IVectorView_StorageFolder( iface );
    UINT32 i;

    TRACE( "iface %p, element %p, index %p, found %p.\n", iface, element, index, found );

    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = FALSE;
    for (i = 0; i < impl->count; i++) if (impl->items[i] == element)
    {
        *index = i;
        *found = TRUE;
        break;
    }
    return S_OK;
}

static HRESULT WINAPI folder_view_GetMany( IVectorView_StorageFolder *iface, UINT32 start_index, UINT32 items_size,
                                       IStorageFolder **items, UINT32 *value )
{
    struct folder_view *impl = impl_from_IVectorView_StorageFolder( iface );
    UINT32 i;

    TRACE( "iface %p, start_index %u, items_size %u, items %p, value %p.\n", iface, start_index,
           items_size, items, value );

    if (!value) return E_POINTER;
    *value = 0;
    for (i = 0; i < items_size && start_index + i < impl->count; i++)
        IStorageFolder_AddRef( (items[i] = impl->items[start_index + i]) );
    *value = i;
    return S_OK;
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
    /* IVectorView<T> methods */
    folder_view_GetAt,
    folder_view_get_Size,
    folder_view_IndexOf,
    folder_view_GetMany,
};

static HRESULT folder_view_create( IStorageFolder **items, UINT32 count, IVectorView_StorageFolder **out )
{
    struct folder_view *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_StorageFolder_iface.lpVtbl = &folder_view_vtbl;
    impl->ref = 1;
    impl->items = items;
    impl->count = count;

    *out = &impl->IVectorView_StorageFolder_iface;
    return S_OK;
}

struct item_view
{
    IVectorView_IStorageItem IVectorView_IStorageItem_iface;
    LONG ref;

    IStorageItem **items;
    UINT32 count;
};

static inline struct item_view *impl_from_IVectorView_IStorageItem( IVectorView_IStorageItem *iface )
{
    return CONTAINING_RECORD( iface, struct item_view, IVectorView_IStorageItem_iface );
}

static HRESULT WINAPI item_view_QueryInterface( IVectorView_IStorageItem *iface, REFIID iid, void **out )
{
    struct item_view *impl = impl_from_IVectorView_IStorageItem( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IVectorView_IStorageItem ))
    {
        *out = &impl->IVectorView_IStorageItem_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI item_view_AddRef( IVectorView_IStorageItem *iface )
{
    return InterlockedIncrement( &impl_from_IVectorView_IStorageItem( iface )->ref );
}

static ULONG WINAPI item_view_Release( IVectorView_IStorageItem *iface )
{
    struct item_view *impl = impl_from_IVectorView_IStorageItem( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IStorageItem_Release( impl->items[i] );
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI item_view_GetIids( IVectorView_IStorageItem *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI item_view_GetRuntimeClassName( IVectorView_IStorageItem *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI item_view_GetTrustLevel( IVectorView_IStorageItem *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI item_view_GetAt( IVectorView_IStorageItem *iface, UINT32 index, IStorageItem **value )
{
    struct item_view *impl = impl_from_IVectorView_IStorageItem( iface );

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    IStorageItem_AddRef( (*value = impl->items[index]) );
    return S_OK;
}

static HRESULT WINAPI item_view_get_Size( IVectorView_IStorageItem *iface, UINT32 *value )
{
    struct item_view *impl = impl_from_IVectorView_IStorageItem( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI item_view_IndexOf( IVectorView_IStorageItem *iface, IStorageItem *element, UINT32 *index, BOOLEAN *found )
{
    struct item_view *impl = impl_from_IVectorView_IStorageItem( iface );
    UINT32 i;

    TRACE( "iface %p, element %p, index %p, found %p.\n", iface, element, index, found );

    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = FALSE;
    for (i = 0; i < impl->count; i++) if (impl->items[i] == element)
    {
        *index = i;
        *found = TRUE;
        break;
    }
    return S_OK;
}

static HRESULT WINAPI item_view_GetMany( IVectorView_IStorageItem *iface, UINT32 start_index, UINT32 items_size,
                                       IStorageItem **items, UINT32 *value )
{
    struct item_view *impl = impl_from_IVectorView_IStorageItem( iface );
    UINT32 i;

    TRACE( "iface %p, start_index %u, items_size %u, items %p, value %p.\n", iface, start_index,
           items_size, items, value );

    if (!value) return E_POINTER;
    *value = 0;
    for (i = 0; i < items_size && start_index + i < impl->count; i++)
        IStorageItem_AddRef( (items[i] = impl->items[start_index + i]) );
    *value = i;
    return S_OK;
}

static const struct IVectorView_IStorageItemVtbl item_view_vtbl =
{
    item_view_QueryInterface,
    item_view_AddRef,
    item_view_Release,
    /* IInspectable methods */
    item_view_GetIids,
    item_view_GetRuntimeClassName,
    item_view_GetTrustLevel,
    /* IVectorView<T> methods */
    item_view_GetAt,
    item_view_get_Size,
    item_view_IndexOf,
    item_view_GetMany,
};

static HRESULT item_view_create( IStorageItem **items, UINT32 count, IVectorView_IStorageItem **out )
{
    struct item_view *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_IStorageItem_iface.lpVtbl = &item_view_vtbl;
    impl->ref = 1;
    impl->items = items;
    impl->count = count;

    *out = &impl->IVectorView_IStorageItem_iface;
    return S_OK;
}

/*
 * IStorageFolderQueryOperations - a media library app reaches its content through this. Wine has no
 * indexer, so a query is a directory listing: the CommonFileQuery orderings are ignored (the shell
 * does not sort by music metadata here) and the query-result objects, which are live and raise
 * ContentsChanged, are not created at all.
 */

DEFINE_IINSPECTABLE( folder_query, IStorageFolderQueryOperations, struct storage_folder, IStorageFolder_iface )

/* one directory listing, split into files and directories */
static HRESULT list_directory( const WCHAR *path, BOOL want_files, BOOL want_folders,
                               IStorageItem ***out_items, UINT32 *out_count )
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    IStorageItem **items = NULL;
    UINT32 count = 0, capacity = 0;
    HANDLE find;
    HRESULT hr = S_OK;

    if (wcslen( path ) + 3 > ARRAY_SIZE(pattern)) return E_FAIL;
    wcscpy( pattern, path );
    wcscat( pattern, L"\\*" );

    if ((find = FindFirstFileW( pattern, &data )) == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32( GetLastError() );

    do
    {
        WCHAR child[MAX_PATH];
        BOOL is_dir = !!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        IStorageItem *item = NULL;

        if (!wcscmp( data.cFileName, L"." ) || !wcscmp( data.cFileName, L".." )) continue;
        if (is_dir ? !want_folders : !want_files) continue;
        if (wcslen( path ) + 1 + wcslen( data.cFileName ) + 1 > ARRAY_SIZE(child)) continue;
        swprintf( child, ARRAY_SIZE(child), L"%s\\%s", path, data.cFileName );

        if (is_dir)
        {
            IStorageFolder *folder;
            if (FAILED(__wine_storage_folder_create( child, &folder ))) continue;
            IStorageFolder_QueryInterface( folder, &IID_IStorageItem, (void **)&item );
            IStorageFolder_Release( folder );
        }
        else
        {
            IStorageFile *file;
            if (FAILED(storage_file_create( child, &file ))) continue;
            IStorageFile_QueryInterface( file, &IID_IStorageItem, (void **)&item );
            IStorageFile_Release( file );
        }
        if (!item) continue;

        if (count == capacity)
        {
            IStorageItem **new_items;
            UINT32 new_capacity = capacity ? capacity * 2 : 16;
            if (!(new_items = realloc( items, new_capacity * sizeof(*items) )))
            {
                IStorageItem_Release( item );
                hr = E_OUTOFMEMORY;
                break;
            }
            items = new_items;
            capacity = new_capacity;
        }
        items[count++] = item;
    } while (FindNextFileW( find, &data ));
    FindClose( find );

    if (FAILED(hr))
    {
        while (count--) IStorageItem_Release( items[count] );
        free( items );
        return hr;
    }

    *out_items = items;
    *out_count = count;
    return S_OK;
}

/* the async callbacks below all run over the folder's path, carried as the invoker */
static const WCHAR *invoker_path( IUnknown *invoker )
{
    return CONTAINING_RECORD( (IStorageFolderQueryOperations *)invoker, struct storage_folder,
                              IStorageFolderQueryOperations_iface )->path;
}

static HRESULT query_files( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    IVectorView_StorageFile *view;
    IStorageItem **items;
    IStorageFile **files;
    UINT32 count, i;
    HRESULT hr;

    if (FAILED(hr = list_directory( invoker_path( invoker ), TRUE, FALSE, &items, &count ))) return hr;
    if (!(files = malloc( max( count, 1 ) * sizeof(*files) )))
    {
        while (count--) IStorageItem_Release( items[count] );
        free( items );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < count; i++)
    {
        IStorageItem_QueryInterface( items[i], &IID_IStorageFile, (void **)&files[i] );
        IStorageItem_Release( items[i] );
    }
    free( items );

    if (FAILED(hr = file_view_create( files, count, &view )))
    {
        while (count--) IStorageFile_Release( files[count] );
        free( files );
        return hr;
    }
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)view;
    return S_OK;
}

static HRESULT query_folders( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    IVectorView_StorageFolder *view;
    IStorageItem **items;
    IStorageFolder **folders;
    UINT32 count, i;
    HRESULT hr;

    if (FAILED(hr = list_directory( invoker_path( invoker ), FALSE, TRUE, &items, &count ))) return hr;
    if (!(folders = malloc( max( count, 1 ) * sizeof(*folders) )))
    {
        while (count--) IStorageItem_Release( items[count] );
        free( items );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < count; i++)
    {
        IStorageItem_QueryInterface( items[i], &IID_IStorageFolder, (void **)&folders[i] );
        IStorageItem_Release( items[i] );
    }
    free( items );

    if (FAILED(hr = folder_view_create( folders, count, &view )))
    {
        while (count--) IStorageFolder_Release( folders[count] );
        free( folders );
        return hr;
    }
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)view;
    return S_OK;
}

static HRESULT query_items( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    IVectorView_IStorageItem *view;
    IStorageItem **items;
    UINT32 count;
    HRESULT hr;

    if (FAILED(hr = list_directory( invoker_path( invoker ), TRUE, TRUE, &items, &count ))) return hr;
    if (FAILED(hr = item_view_create( items, count, &view )))
    {
        while (count--) IStorageItem_Release( items[count] );
        free( items );
        return hr;
    }
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)view;
    return S_OK;
}


/*
 * StorageItemQueryResult - the live view of a folder query. Without an indexer there is nothing to
 * watch, so ContentsChanged never fires and every call re-lists the directory.
 */

struct item_query_result
{
    IStorageItemQueryResult IStorageItemQueryResult_iface;
    IStorageQueryResultBase IStorageQueryResultBase_iface;
    LONG ref;

    IStorageFolder *folder;
    WCHAR *path;
};

static inline struct item_query_result *impl_from_IStorageItemQueryResult( IStorageItemQueryResult *iface )
{
    return CONTAINING_RECORD( iface, struct item_query_result, IStorageItemQueryResult_iface );
}

static HRESULT WINAPI query_result_QueryInterface( IStorageItemQueryResult *iface, REFIID iid, void **out )
{
    struct item_query_result *impl = impl_from_IStorageItemQueryResult( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IStorageItemQueryResult ))
    {
        *out = &impl->IStorageItemQueryResult_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IStorageQueryResultBase ))
    {
        *out = &impl->IStorageQueryResultBase_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI query_result_AddRef( IStorageItemQueryResult *iface )
{
    return InterlockedIncrement( &impl_from_IStorageItemQueryResult( iface )->ref );
}

static ULONG WINAPI query_result_Release( IStorageItemQueryResult *iface )
{
    struct item_query_result *impl = impl_from_IStorageItemQueryResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IStorageFolder_Release( impl->folder );
        free( impl->path );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI query_result_GetIids( IStorageItemQueryResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI query_result_GetRuntimeClassName( IStorageItemQueryResult *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    if (!class_name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Storage_Search_StorageItemQueryResult,
            ARRAY_SIZE(RuntimeClass_Windows_Storage_Search_StorageItemQueryResult) - 1, class_name );
}

static HRESULT WINAPI query_result_GetTrustLevel( IStorageItemQueryResult *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static const WCHAR *query_result_path( IUnknown *invoker )
{
    return CONTAINING_RECORD( (IStorageItemQueryResult *)invoker, struct item_query_result,
                              IStorageItemQueryResult_iface )->path;
}

static HRESULT query_result_items( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    IVectorView_IStorageItem *view;
    IStorageItem **items;
    UINT32 count;
    HRESULT hr;

    if (FAILED(hr = list_directory( query_result_path( invoker ), TRUE, TRUE, &items, &count ))) return hr;
    if (FAILED(hr = item_view_create( items, count, &view )))
    {
        while (count--) IStorageItem_Release( items[count] );
        free( items );
        return hr;
    }
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)view;
    return S_OK;
}

static HRESULT WINAPI query_result_GetItemsAsync( IStorageItemQueryResult *iface, UINT32 start_index,
        UINT32 max_number_of_items, IAsyncOperation_IVectorView_IStorageItem **operation )
{
    TRACE( "iface %p, start_index %u, max_number_of_items %u, operation %p.\n", iface, start_index,
           max_number_of_items, operation );

    if (!operation) return E_POINTER;
    return async_operation_items_create( (IUnknown *)iface, NULL, query_result_items, operation );
}

static HRESULT WINAPI query_result_GetItemsAsyncDefaultStartAndCount( IStorageItemQueryResult *iface,
        IAsyncOperation_IVectorView_IStorageItem **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    return async_operation_items_create( (IUnknown *)iface, NULL, query_result_items, operation );
}

static const struct IStorageItemQueryResultVtbl query_result_vtbl =
{
    query_result_QueryInterface,
    query_result_AddRef,
    query_result_Release,
    /* IInspectable methods */
    query_result_GetIids,
    query_result_GetRuntimeClassName,
    query_result_GetTrustLevel,
    /* IStorageItemQueryResult methods */
    query_result_GetItemsAsync,
    query_result_GetItemsAsyncDefaultStartAndCount,
};

DEFINE_IINSPECTABLE( query_base, IStorageQueryResultBase, struct item_query_result, IStorageItemQueryResult_iface )

static HRESULT query_result_count( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    IStorageItem **items;
    UINT32 count, i;
    HRESULT hr;

    if (FAILED(hr = list_directory( CONTAINING_RECORD( (IStorageQueryResultBase *)invoker,
            struct item_query_result, IStorageQueryResultBase_iface )->path, TRUE, TRUE, &items, &count )))
        return hr;
    for (i = 0; i < count; i++) IStorageItem_Release( items[i] );
    free( items );

    result->vt = VT_UI4;
    result->ulVal = count;
    return S_OK;
}

static HRESULT WINAPI query_base_GetItemCountAsync( IStorageQueryResultBase *iface, IAsyncOperation_UINT32 **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    return async_operation_uint32_create( (IUnknown *)iface, NULL, query_result_count, operation );
}

static HRESULT WINAPI query_base_get_Folder( IStorageQueryResultBase *iface, IStorageFolder **container )
{
    struct item_query_result *impl = impl_from_IStorageQueryResultBase( iface );

    TRACE( "iface %p, container %p.\n", iface, container );

    if (!container) return E_POINTER;
    IStorageFolder_AddRef( (*container = impl->folder) );
    return S_OK;
}

static HRESULT WINAPI query_base_add_ContentsChanged( IStorageQueryResultBase *iface,
        ITypedEventHandler_IStorageQueryResultBase_IInspectable *handler, EventRegistrationToken *event_cookie )
{
    FIXME( "iface %p, handler %p, event_cookie %p stub!\n", iface, handler, event_cookie );
    if (!event_cookie) return E_POINTER;
    event_cookie->value = 0;
    return S_OK;
}

static HRESULT WINAPI query_base_remove_ContentsChanged( IStorageQueryResultBase *iface, EventRegistrationToken event_cookie )
{
    FIXME( "iface %p, event_cookie %#I64x stub!\n", iface, event_cookie.value );
    return S_OK;
}

static HRESULT WINAPI query_base_add_OptionsChanged( IStorageQueryResultBase *iface,
        ITypedEventHandler_IStorageQueryResultBase_IInspectable *changed_handler, EventRegistrationToken *event_cookie )
{
    FIXME( "iface %p, changed_handler %p, event_cookie %p stub!\n", iface, changed_handler, event_cookie );
    if (!event_cookie) return E_POINTER;
    event_cookie->value = 0;
    return S_OK;
}

static HRESULT WINAPI query_base_remove_OptionsChanged( IStorageQueryResultBase *iface, EventRegistrationToken event_cookie )
{
    FIXME( "iface %p, event_cookie %#I64x stub!\n", iface, event_cookie.value );
    return S_OK;
}

static HRESULT WINAPI query_base_FindStartIndexAsync( IStorageQueryResultBase *iface, IInspectable *value,
                                                      IAsyncOperation_UINT32 **operation )
{
    FIXME( "iface %p, value %p, operation %p stub!\n", iface, value, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI query_base_GetCurrentQueryOptions( IStorageQueryResultBase *iface, IQueryOptions **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI query_base_ApplyNewQueryOptions( IStorageQueryResultBase *iface, IQueryOptions *new_query_options )
{
    /* there are no options to apply to a plain directory listing */
    FIXME( "iface %p, new_query_options %p: ignored.\n", iface, new_query_options );
    return S_OK;
}

static const struct IStorageQueryResultBaseVtbl query_base_vtbl =
{
    query_base_QueryInterface,
    query_base_AddRef,
    query_base_Release,
    /* IInspectable methods */
    query_base_GetIids,
    query_base_GetRuntimeClassName,
    query_base_GetTrustLevel,
    /* IStorageQueryResultBase methods */
    query_base_GetItemCountAsync,
    query_base_get_Folder,
    query_base_add_ContentsChanged,
    query_base_remove_ContentsChanged,
    query_base_add_OptionsChanged,
    query_base_remove_OptionsChanged,
    query_base_FindStartIndexAsync,
    query_base_GetCurrentQueryOptions,
    query_base_ApplyNewQueryOptions,
};

static HRESULT item_query_result_create( IStorageFolder *folder, const WCHAR *path, IStorageItemQueryResult **out )
{
    struct item_query_result *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (!(impl->path = wcsdup( path )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    impl->IStorageItemQueryResult_iface.lpVtbl = &query_result_vtbl;
    impl->IStorageQueryResultBase_iface.lpVtbl = &query_base_vtbl;
    impl->ref = 1;
    IStorageFolder_AddRef( (impl->folder = folder) );

    *out = &impl->IStorageItemQueryResult_iface;
    return S_OK;
}

/* Windows Search does not index anything in a Wine prefix. Saying so is not a cosmetic detail:
 * apps call this first to choose between handing a query to the indexer and walking the folder
 * themselves, and returning E_NOTIMPL instead of an answer makes them take neither path. Media
 * Player asks once per library and, without an answer, paints "We couldn't find any music" over a
 * music folder it never reads. NotIndexed sends it down the enumeration path, which
 * GetFilesAsync below actually implements. */
static HRESULT indexed_state_not_indexed( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_UI4;
    result->ulVal = IndexedState_NotIndexed;
    return S_OK;
}

static HRESULT WINAPI folder_query_GetIndexedStateAsync( IStorageFolderQueryOperations *iface, IInspectable **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    return async_operation_indexed_state_create( (IUnknown *)iface, NULL, indexed_state_not_indexed,
                                                 (IAsyncOperation_UINT32 **)operation );
}

static HRESULT WINAPI folder_query_CreateFileQueryOverloadDefault( IStorageFolderQueryOperations *iface,
                                                                   IStorageFileQueryResult **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_query_CreateFileQuery( IStorageFolderQueryOperations *iface, CommonFileQuery query,
                                                    IStorageFileQueryResult **value )
{
    FIXME( "iface %p, query %d, value %p stub!\n", iface, query, value );
    return E_NOTIMPL;
}

/*
 * StorageFileQueryResult - a file query over a folder, filtered by the options' FileTypeFilter.
 * Like the item query above there is no indexer behind it, so the listing is done afresh on every
 * GetFilesAsync and ContentsChanged never fires.
 */

struct file_query_result
{
    IStorageFileQueryResultInternal IStorageFileQueryResultInternal_iface;
    IStorageQueryResultBase IStorageQueryResultBase_iface;
    LONG ref;

    IStorageFolder *folder;
    WCHAR *path;
    IQueryOptionsInternal *options;
};

static inline struct file_query_result *impl_from_IStorageFileQueryResultInternal( IStorageFileQueryResultInternal *iface )
{
    return CONTAINING_RECORD( iface, struct file_query_result, IStorageFileQueryResultInternal_iface );
}

static HRESULT WINAPI file_query_QueryInterface( IStorageFileQueryResultInternal *iface, REFIID iid, void **out )
{
    struct file_query_result *impl = impl_from_IStorageFileQueryResultInternal( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IStorageFileQueryResultInternal ))
    {
        IInspectable_AddRef( (*out = &impl->IStorageFileQueryResultInternal_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IStorageQueryResultBase ))
    {
        IInspectable_AddRef( (*out = &impl->IStorageQueryResultBase_iface) );
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI file_query_AddRef( IStorageFileQueryResultInternal *iface )
{
    return InterlockedIncrement( &impl_from_IStorageFileQueryResultInternal( iface )->ref );
}

static ULONG WINAPI file_query_Release( IStorageFileQueryResultInternal *iface )
{
    struct file_query_result *impl = impl_from_IStorageFileQueryResultInternal( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->options) IQueryOptionsInternal_Release( impl->options );
        if (impl->folder) IStorageFolder_Release( impl->folder );
        free( impl->path );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI file_query_GetIids( IStorageFileQueryResultInternal *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_query_GetRuntimeClassName( IStorageFileQueryResultInternal *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Storage.Search.StorageFileQueryResult",
                                ARRAY_SIZE(L"Windows.Storage.Search.StorageFileQueryResult") - 1,
                                class_name );
}

static HRESULT WINAPI file_query_GetTrustLevel( IStorageFileQueryResultInternal *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

/* The filter is a list of extensions including the dot, as in ".mp3". An empty list means every
 * file, which is what the default query asks for. */
static BOOL file_matches_filter( const WCHAR *name, IVector_HSTRING *filter )
{
    const WCHAR *ext;
    UINT32 count, i;

    if (!filter || FAILED(IVector_HSTRING_get_Size( filter, &count )) || !count) return TRUE;
    if (!(ext = wcsrchr( name, '.' ))) return FALSE;

    for (i = 0; i < count; i++)
    {
        HSTRING entry;
        const WCHAR *buffer;
        BOOL match;

        if (FAILED(IVector_HSTRING_GetAt( filter, i, &entry ))) continue;
        buffer = WindowsGetStringRawBuffer( entry, NULL );
        /* "*" is the documented way to ask for everything. */
        match = buffer && (!wcscmp( buffer, L"*" ) || !wcsicmp( buffer, ext ));
        WindowsDeleteString( entry );
        if (match) return TRUE;
    }
    return FALSE;
}

static HRESULT query_result_files( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct file_query_result *impl = CONTAINING_RECORD( (IStorageFileQueryResultInternal *)invoker,
                                                        struct file_query_result,
                                                        IStorageFileQueryResultInternal_iface );
    IVector_HSTRING *filter = NULL;
    IVectorView_StorageFile *view;
    IStorageItem **items;
    IStorageFile **files;
    UINT32 count, kept = 0, i;
    HRESULT hr;

    if (impl->options) IQueryOptionsInternal_get_FileTypeFilter( impl->options, &filter );

    if (FAILED(hr = list_directory( impl->path, TRUE, FALSE, &items, &count )))
    {
        if (filter) IVector_HSTRING_Release( filter );
        return hr;
    }

    if (!(files = malloc( max( count, 1 ) * sizeof(*files) )))
    {
        while (count--) IStorageItem_Release( items[count] );
        free( items );
        if (filter) IVector_HSTRING_Release( filter );
        return E_OUTOFMEMORY;
    }

    for (i = 0; i < count; i++)
    {
        HSTRING name = NULL;
        BOOL keep = FALSE;

        if (SUCCEEDED(IStorageItem_get_Name( items[i], &name )))
        {
            keep = file_matches_filter( WindowsGetStringRawBuffer( name, NULL ), filter );
            WindowsDeleteString( name );
        }

        if (keep && SUCCEEDED(IStorageItem_QueryInterface( items[i], &IID_IStorageFile, (void **)&files[kept] )))
            kept++;
        IStorageItem_Release( items[i] );
    }
    free( items );
    if (filter) IVector_HSTRING_Release( filter );

    TRACE( "%s: %u of %u entries matched.\n", debugstr_w(impl->path), kept, count );

    if (FAILED(hr = file_view_create( files, kept, &view )))
    {
        while (kept--) IStorageFile_Release( files[kept] );
        free( files );
        return hr;
    }

    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)view;
    return S_OK;
}

static HRESULT WINAPI file_query_GetFilesAsync( IStorageFileQueryResultInternal *iface, UINT32 start_index,
        UINT32 max_number_of_items, IAsyncOperation_IVectorView_StorageFile **operation )
{
    TRACE( "iface %p, start_index %u, max_number_of_items %u, operation %p.\n", iface, start_index,
           max_number_of_items, operation );

    if (!operation) return E_POINTER;
    return async_operation_files_create( (IUnknown *)iface, NULL, query_result_files, operation );
}

static HRESULT WINAPI file_query_GetFilesAsyncDefaultStartAndCount( IStorageFileQueryResultInternal *iface,
        IAsyncOperation_IVectorView_StorageFile **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    return async_operation_files_create( (IUnknown *)iface, NULL, query_result_files, operation );
}

static const struct IStorageFileQueryResultInternalVtbl file_query_vtbl =
{
    /* IUnknown methods */
    file_query_QueryInterface,
    file_query_AddRef,
    file_query_Release,
    /* IInspectable methods */
    file_query_GetIids,
    file_query_GetRuntimeClassName,
    file_query_GetTrustLevel,
    /* IStorageFileQueryResult methods */
    file_query_GetFilesAsync,
    file_query_GetFilesAsyncDefaultStartAndCount,
};

DEFINE_IINSPECTABLE_( file_query_base, IStorageQueryResultBase, struct file_query_result,
                      impl_from_IStorageQueryResultBase_file, IStorageQueryResultBase_iface,
                      &impl->IStorageFileQueryResultInternal_iface )

static HRESULT file_query_count( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    PROPVARIANT files = {0};
    IVectorView_StorageFile *view;
    UINT32 count = 0;
    HRESULT hr;

    if (FAILED(hr = query_result_files( invoker, param, &files, called_async ))) return hr;
    view = (IVectorView_StorageFile *)files.punkVal;
    hr = IVectorView_StorageFile_get_Size( view, &count );
    IVectorView_StorageFile_Release( view );
    result->vt = VT_UI4;
    result->ulVal = count;
    return hr;
}

static HRESULT WINAPI file_query_base_GetItemCountAsync( IStorageQueryResultBase *iface,
        IAsyncOperation_UINT32 **operation )
{
    struct file_query_result *impl = impl_from_IStorageQueryResultBase_file( iface );

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    return async_operation_uint32_create( (IUnknown *)&impl->IStorageFileQueryResultInternal_iface, NULL,
                                          file_query_count, operation );
}

static HRESULT WINAPI file_query_base_get_Folder( IStorageQueryResultBase *iface, IStorageFolder **value )
{
    struct file_query_result *impl = impl_from_IStorageQueryResultBase_file( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    IStorageFolder_AddRef( (*value = impl->folder) );
    return S_OK;
}

/* Nothing watches the directory, so these never fire - but the registration has to succeed, or a
 * caller that treats a failed subscription as fatal never asks for the files. */
static HRESULT WINAPI file_query_base_add_ContentsChanged( IStorageQueryResultBase *iface,
        __FITypedEventHandler_2_Windows__CStorage__CSearch__CIStorageQueryResultBase_IInspectable *handler,
        EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p: contents are never reported as changed.\n", iface, handler, token );
    if (token) token->value = 1;
    return S_OK;
}

static HRESULT WINAPI file_query_base_remove_ContentsChanged( IStorageQueryResultBase *iface,
        EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI file_query_base_add_OptionsChanged( IStorageQueryResultBase *iface,
        __FITypedEventHandler_2_Windows__CStorage__CSearch__CIStorageQueryResultBase_IInspectable *handler,
        EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p: options changes are never reported.\n", iface, handler, token );
    if (token) token->value = 1;
    return S_OK;
}

static HRESULT WINAPI file_query_base_remove_OptionsChanged( IStorageQueryResultBase *iface,
        EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI file_query_base_FindStartIndexAsync( IStorageQueryResultBase *iface, IInspectable *value,
        IAsyncOperation_UINT32 **operation )
{
    FIXME( "iface %p, value %p, operation %p stub!\n", iface, value, operation );
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_query_base_GetCurrentQueryOptions( IStorageQueryResultBase *iface, IQueryOptions **value )
{
    struct file_query_result *impl = impl_from_IStorageQueryResultBase_file( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->options) IQueryOptionsInternal_AddRef( impl->options );
    *value = (IQueryOptions *)impl->options;
    return S_OK;
}

static HRESULT WINAPI file_query_base_ApplyNewQueryOptions( IStorageQueryResultBase *iface, IQueryOptions *value )
{
    struct file_query_result *impl = impl_from_IStorageQueryResultBase_file( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (impl->options) IQueryOptionsInternal_Release( impl->options );
    impl->options = (IQueryOptionsInternal *)value;
    if (impl->options) IQueryOptionsInternal_AddRef( impl->options );
    return S_OK;
}

static const struct IStorageQueryResultBaseVtbl file_query_base_vtbl =
{
    /* IUnknown methods */
    file_query_base_QueryInterface,
    file_query_base_AddRef,
    file_query_base_Release,
    /* IInspectable methods */
    file_query_base_GetIids,
    file_query_base_GetRuntimeClassName,
    file_query_base_GetTrustLevel,
    /* IStorageQueryResultBase methods */
    file_query_base_GetItemCountAsync,
    file_query_base_get_Folder,
    file_query_base_add_ContentsChanged,
    file_query_base_remove_ContentsChanged,
    file_query_base_add_OptionsChanged,
    file_query_base_remove_OptionsChanged,
    file_query_base_FindStartIndexAsync,
    file_query_base_GetCurrentQueryOptions,
    file_query_base_ApplyNewQueryOptions,
};

static HRESULT file_query_result_create( IStorageFolder *folder, const WCHAR *path, IQueryOptionsInternal *options,
                                         IStorageFileQueryResult **out )
{
    struct file_query_result *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IStorageFileQueryResultInternal_iface.lpVtbl = &file_query_vtbl;
    impl->IStorageQueryResultBase_iface.lpVtbl = &file_query_base_vtbl;
    impl->ref = 1;
    if (!(impl->path = wcsdup( path )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    IStorageFolder_AddRef( (impl->folder = folder) );
    if ((impl->options = options)) IQueryOptionsInternal_AddRef( options );

    *out = (IStorageFileQueryResult *)&impl->IStorageFileQueryResultInternal_iface;
    return S_OK;
}

static HRESULT WINAPI folder_query_CreateFileQueryWithOptions( IStorageFolderQueryOperations *iface,
        IQueryOptions *query_options, IStorageFileQueryResult **value )
{
    struct storage_folder *impl = impl_from_IStorageFolderQueryOperations( iface );

    TRACE( "iface %p, query_options %p, value %p.\n", iface, query_options, value );

    if (!value) return E_POINTER;
    return file_query_result_create( &impl->IStorageFolder_iface, impl->path,
                                     (IQueryOptionsInternal *)query_options, value );
}

static HRESULT WINAPI folder_query_CreateFolderQueryOverloadDefault( IStorageFolderQueryOperations *iface,
                                                                     IStorageFolderQueryResult **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_query_CreateFolderQuery( IStorageFolderQueryOperations *iface, CommonFolderQuery query,
                                                      IStorageFolderQueryResult **value )
{
    FIXME( "iface %p, query %d, value %p stub!\n", iface, query, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_query_CreateFolderQueryWithOptions( IStorageFolderQueryOperations *iface,
        IQueryOptions *query_options, IStorageFolderQueryResult **value )
{
    FIXME( "iface %p, query_options %p, value %p stub!\n", iface, query_options, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_query_CreateItemQuery( IStorageFolderQueryOperations *iface,
                                                    IStorageItemQueryResult **value )
{
    struct storage_folder *impl = impl_from_IStorageFolderQueryOperations( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return item_query_result_create( &impl->IStorageFolder_iface, impl->path, value );
}

static HRESULT WINAPI folder_query_CreateItemQueryWithOptions( IStorageFolderQueryOperations *iface,
        IQueryOptions *query_options, IStorageItemQueryResult **value )
{
    FIXME( "iface %p, query_options %p, value %p stub!\n", iface, query_options, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_query_GetFilesAsync( IStorageFolderQueryOperations *iface, CommonFileQuery query,
        UINT32 start_index, UINT32 max_items_to_retrieve, IAsyncOperation_IVectorView_StorageFile **operation )
{
    TRACE( "iface %p, query %d, start_index %u, max_items_to_retrieve %u, operation %p.\n", iface, query,
           start_index, max_items_to_retrieve, operation );

    if (!operation) return E_POINTER;
    if (start_index || max_items_to_retrieve != ~0u)
        FIXME( "ignoring start_index %u and max_items_to_retrieve %u.\n", start_index, max_items_to_retrieve );
    return async_operation_files_create( (IUnknown *)iface, NULL, query_files, operation );
}

static HRESULT WINAPI folder_query_GetFilesAsyncOverloadDefaultStartAndCount( IStorageFolderQueryOperations *iface,
        CommonFileQuery query, IAsyncOperation_IVectorView_StorageFile **operation )
{
    TRACE( "iface %p, query %d, operation %p.\n", iface, query, operation );

    if (!operation) return E_POINTER;
    return async_operation_files_create( (IUnknown *)iface, NULL, query_files, operation );
}

static HRESULT WINAPI folder_query_GetFoldersAsync( IStorageFolderQueryOperations *iface, CommonFolderQuery query,
        UINT32 start_index, UINT32 max_items_to_retrieve, IAsyncOperation_IVectorView_StorageFolder **operation )
{
    TRACE( "iface %p, query %d, start_index %u, max_items_to_retrieve %u, operation %p.\n", iface, query,
           start_index, max_items_to_retrieve, operation );

    if (!operation) return E_POINTER;
    return async_operation_folders_create( (IUnknown *)iface, NULL, query_folders, operation );
}

static HRESULT WINAPI folder_query_GetFoldersAsyncOverloadDefaultStartAndCount( IStorageFolderQueryOperations *iface,
        CommonFolderQuery query, IAsyncOperation_IVectorView_StorageFolder **operation )
{
    TRACE( "iface %p, query %d, operation %p.\n", iface, query, operation );

    if (!operation) return E_POINTER;
    return async_operation_folders_create( (IUnknown *)iface, NULL, query_folders, operation );
}

static HRESULT WINAPI folder_query_GetItemsAsync( IStorageFolderQueryOperations *iface, UINT32 start_index,
        UINT32 max_items_to_retrieve, IAsyncOperation_IVectorView_IStorageItem **operation )
{
    TRACE( "iface %p, start_index %u, max_items_to_retrieve %u, operation %p.\n", iface, start_index,
           max_items_to_retrieve, operation );

    if (!operation) return E_POINTER;
    return async_operation_items_create( (IUnknown *)iface, NULL, query_items, operation );
}

static HRESULT WINAPI folder_query_AreQueryOptionsSupported( IStorageFolderQueryOperations *iface,
        IQueryOptions *query_options, boolean *value )
{
    FIXME( "iface %p, query_options %p, value %p: reporting unsupported.\n", iface, query_options, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI folder_query_IsCommonFolderQuerySupported( IStorageFolderQueryOperations *iface,
        CommonFolderQuery query, boolean *value )
{
    TRACE( "iface %p, query %d, value %p.\n", iface, query, value );

    if (!value) return E_POINTER;
    /* the grouping queries all need metadata Wine does not read */
    *value = (query == CommonFolderQuery_DefaultQuery);
    return S_OK;
}

static HRESULT WINAPI folder_query_IsCommonFileQuerySupported( IStorageFolderQueryOperations *iface,
        CommonFileQuery query, boolean *value )
{
    TRACE( "iface %p, query %d, value %p.\n", iface, query, value );

    if (!value) return E_POINTER;
    *value = (query == CommonFileQuery_DefaultQuery || query == CommonFileQuery_OrderByName);
    return S_OK;
}

static const struct IStorageFolderQueryOperationsVtbl folder_query_vtbl =
{
    folder_query_QueryInterface,
    folder_query_AddRef,
    folder_query_Release,
    /* IInspectable methods */
    folder_query_GetIids,
    folder_query_GetRuntimeClassName,
    folder_query_GetTrustLevel,
    /* IStorageFolderQueryOperations methods */
    folder_query_GetIndexedStateAsync,
    folder_query_CreateFileQueryOverloadDefault,
    folder_query_CreateFileQuery,
    folder_query_CreateFileQueryWithOptions,
    folder_query_CreateFolderQueryOverloadDefault,
    folder_query_CreateFolderQuery,
    folder_query_CreateFolderQueryWithOptions,
    folder_query_CreateItemQuery,
    folder_query_CreateItemQueryWithOptions,
    folder_query_GetFilesAsync,
    folder_query_GetFilesAsyncOverloadDefaultStartAndCount,
    folder_query_GetFoldersAsync,
    folder_query_GetFoldersAsyncOverloadDefaultStartAndCount,
    folder_query_GetItemsAsync,
    folder_query_AreQueryOptionsSupported,
    folder_query_IsCommonFolderQuerySupported,
    folder_query_IsCommonFileQuerySupported,
};

/***********************************************************************
 *      __wine_storage_folder_create   (windows.storage.@)
 *
 * A StorageFolder for an existing directory. windows.storage.applicationdata.dll resolves this at
 * runtime for its ApplicationData folders: Wine's build cannot make an import library for a module
 * with a dotted name, so the two cannot be linked together.
 */
/*
 * IStorageFolder2 - one method, TryGetItemAsync. Windows exposes it on every StorageFolder, and
 * .NET casts to it unconditionally, so answering it matters more than the method does.
 */
DEFINE_IINSPECTABLE( folder2, IStorageFolder2, struct storage_folder, IStorageFolder_iface )

static HRESULT WINAPI folder2_TryGetItemAsync( IStorageFolder2 *iface, HSTRING name,
                                               IAsyncOperation_IStorageItem **operation )
{
    struct storage_folder *impl = impl_from_IStorageFolder2( iface );
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, name %s, operation %p.\n", iface, debugstr_hstring( name ), operation );

    if (!operation) return E_POINTER;
    if (FAILED(hr = folder_child_param( impl, name, &param ))) return hr;
    hr = async_operation_storage_item_create( (IUnknown *)iface, param, try_get_child_item, operation );
    IUnknown_Release( param );
    return hr;
}

static const struct IStorageFolder2Vtbl folder2_vtbl =
{
    folder2_QueryInterface,
    folder2_AddRef,
    folder2_Release,
    /* IInspectable methods */
    folder2_GetIids,
    folder2_GetRuntimeClassName,
    folder2_GetTrustLevel,
    /* IStorageFolder2 methods */
    folder2_TryGetItemAsync,
};

HRESULT WINAPI __wine_storage_folder_create( const WCHAR *path, IStorageFolder **out )
{
    struct storage_folder *impl;
    DWORD attrs;

    TRACE( "path %s, out %p.\n", debugstr_w( path ), out );

    if (!path || !out) return E_POINTER;
    *out = NULL;
    if ((attrs = GetFileAttributesW( path )) == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32( GetLastError() );
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return HRESULT_FROM_WIN32( ERROR_DIRECTORY );

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (!(impl->path = wcsdup( path )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    impl->IStorageFolder_iface.lpVtbl = &storage_folder_vtbl;
    impl->IStorageItem_iface.lpVtbl = &storage_item_vtbl;
    impl->IStorageFolderQueryOperations_iface.lpVtbl = &folder_query_vtbl;
    impl->IStorageFolder2_iface.lpVtbl = &folder2_vtbl;
    impl->ref = 1;

    *out = &impl->IStorageFolder_iface;
    return S_OK;
}

/*
 * StorageFolder statics
 */

struct storage_folder_statics
{
    IActivationFactory IActivationFactory_iface;
    IStorageFolderStatics IStorageFolderStatics_iface;
    LONG ref;
};

static inline struct storage_folder_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct storage_folder_statics, IActivationFactory_iface );
}

static HRESULT WINAPI folder_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct storage_folder_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IStorageFolderStatics ))
    {
        *out = &impl->IStorageFolderStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI folder_factory_AddRef( IActivationFactory *iface )
{
    struct storage_folder_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI folder_factory_Release( IActivationFactory *iface )
{
    struct storage_folder_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI folder_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* StorageFolder is not activatable: it only comes out of the statics. */
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl folder_factory_vtbl =
{
    folder_factory_QueryInterface,
    folder_factory_AddRef,
    folder_factory_Release,
    /* IInspectable methods */
    folder_factory_GetIids,
    folder_factory_GetRuntimeClassName,
    folder_factory_GetTrustLevel,
    /* IActivationFactory methods */
    folder_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( folder_statics, IStorageFolderStatics, struct storage_folder_statics, IActivationFactory_iface )

static HRESULT get_folder_from_path( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    IStorageFolder *folder;
    HRESULT hr;

    if (FAILED(hr = __wine_storage_folder_create( storage_path_param_buffer( param ), &folder ))) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)folder;
    return S_OK;
}

static HRESULT WINAPI folder_statics_GetFolderFromPathAsync( IStorageFolderStatics *iface, HSTRING path,
                                                             IAsyncOperation_StorageFolder **operation )
{
    const WCHAR *buffer;
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, path %s, operation %p.\n", iface, debugstr_hstring( path ), operation );

    if (!operation) return E_POINTER;
    if (!(buffer = WindowsGetStringRawBuffer( path, NULL ))) return E_INVALIDARG;
    if (FAILED(hr = storage_path_param_create( buffer, &param ))) return hr;
    hr = async_operation_storage_folder_create( (IUnknown *)iface, param, get_folder_from_path, operation );
    IUnknown_Release( param );
    return hr;
}

static const struct IStorageFolderStaticsVtbl folder_statics_vtbl =
{
    folder_statics_QueryInterface,
    folder_statics_AddRef,
    folder_statics_Release,
    /* IInspectable methods */
    folder_statics_GetIids,
    folder_statics_GetRuntimeClassName,
    folder_statics_GetTrustLevel,
    /* IStorageFolderStatics methods */
    folder_statics_GetFolderFromPathAsync,
};

static struct storage_folder_statics storage_folder_statics =
{
    {&folder_factory_vtbl},
    {&folder_statics_vtbl},
    0,
};

IActivationFactory *storage_folder_factory = &storage_folder_statics.IActivationFactory_iface;


/*
 * KnownFolders - the user's libraries as StorageFolders. Media Player asks for MusicLibrary at
 * startup; the folders that have no Wine equivalent report ERROR_NOT_FOUND rather than failing
 * the whole class.
 */

struct known_folders_statics
{
    IActivationFactory IActivationFactory_iface;
    IKnownFoldersStatics IKnownFoldersStatics_iface;
    LONG ref;
};

static inline struct known_folders_statics *impl_from_IActivationFactory_kf( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct known_folders_statics, IActivationFactory_iface );
}

static HRESULT WINAPI known_folders_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct known_folders_statics *impl = impl_from_IActivationFactory_kf( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IKnownFoldersStatics ))
    {
        *out = &impl->IKnownFoldersStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI known_folders_factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory_kf( iface )->ref );
}

static ULONG WINAPI known_folders_factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory_kf( iface )->ref );
}

static HRESULT WINAPI known_folders_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI known_folders_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI known_folders_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI known_folders_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl known_folders_factory_vtbl =
{
    known_folders_factory_QueryInterface,
    known_folders_factory_AddRef,
    known_folders_factory_Release,
    /* IInspectable methods */
    known_folders_factory_GetIids,
    known_folders_factory_GetRuntimeClassName,
    known_folders_factory_GetTrustLevel,
    /* IActivationFactory methods */
    known_folders_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( known_folders, IKnownFoldersStatics, struct known_folders_statics, IActivationFactory_iface )

static HRESULT known_folder_storage_folder( const KNOWNFOLDERID *id, IStorageFolder **value )
{
    WCHAR *path;
    HRESULT hr;

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = SHGetKnownFolderPath( id, KF_FLAG_CREATE, NULL, &path ))) return hr;
    hr = __wine_storage_folder_create( path, value );
    CoTaskMemFree( path );
    return hr;
}

#define DEFINE_KNOWN_FOLDER_GETTER( name, folder )                                                 \
    static HRESULT WINAPI known_folders_get_##name( IKnownFoldersStatics *iface, IStorageFolder **value ) \
    {                                                                                              \
        TRACE( "iface %p, value %p.\n", iface, value );                                            \
        return known_folder_storage_folder( &folder, value );                                      \
    }

DEFINE_KNOWN_FOLDER_GETTER( MusicLibrary, FOLDERID_Music )
DEFINE_KNOWN_FOLDER_GETTER( PicturesLibrary, FOLDERID_Pictures )
DEFINE_KNOWN_FOLDER_GETTER( VideosLibrary, FOLDERID_Videos )
DEFINE_KNOWN_FOLDER_GETTER( DocumentsLibrary, FOLDERID_Documents )

/* HomeGroup, removable devices and DLNA servers are virtual folders whose children are the devices
 * of that kind. Wine enumerates none of them, and an app that asks is better served by an empty
 * folder than by an error - Media Player treats a failure here as fatal. */
static HRESULT empty_virtual_folder( const WCHAR *name, IStorageFolder **value )
{
    WCHAR path[MAX_PATH];
    DWORD len;

    if (!value) return E_POINTER;
    *value = NULL;

    if (!(len = GetTempPathW( ARRAY_SIZE(path), path ))) return HRESULT_FROM_WIN32( GetLastError() );
    if (len + wcslen( name ) + 1 > ARRAY_SIZE(path)) return E_FAIL;
    wcscat( path, name );
    if (!CreateDirectoryW( path, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32( GetLastError() );
    return __wine_storage_folder_create( path, value );
}

static HRESULT WINAPI known_folders_get_HomeGroup( IKnownFoldersStatics *iface, IStorageFolder **value )
{
    FIXME( "iface %p, value %p: empty folder.\n", iface, value );
    return empty_virtual_folder( L"wine-homegroup", value );
}

static HRESULT WINAPI known_folders_get_RemovableDevices( IKnownFoldersStatics *iface, IStorageFolder **value )
{
    FIXME( "iface %p, value %p: empty folder.\n", iface, value );
    return empty_virtual_folder( L"wine-removable-devices", value );
}

static HRESULT WINAPI known_folders_get_MediaServerDevices( IKnownFoldersStatics *iface, IStorageFolder **value )
{
    FIXME( "iface %p, value %p: empty folder.\n", iface, value );
    return empty_virtual_folder( L"wine-media-servers", value );
}

static const struct IKnownFoldersStaticsVtbl known_folders_vtbl =
{
    known_folders_QueryInterface,
    known_folders_AddRef,
    known_folders_Release,
    /* IInspectable methods */
    known_folders_GetIids,
    known_folders_GetRuntimeClassName,
    known_folders_GetTrustLevel,
    /* IKnownFoldersStatics methods */
    known_folders_get_MusicLibrary,
    known_folders_get_PicturesLibrary,
    known_folders_get_VideosLibrary,
    known_folders_get_DocumentsLibrary,
    known_folders_get_HomeGroup,
    known_folders_get_RemovableDevices,
    known_folders_get_MediaServerDevices,
};

static struct known_folders_statics known_folders_statics =
{
    {&known_folders_factory_vtbl},
    {&known_folders_vtbl},
    0,
};

IActivationFactory *known_folders_factory = &known_folders_statics.IActivationFactory_iface;

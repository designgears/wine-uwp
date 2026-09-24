/* WinRT Windows.Storage.StorageFile Implementation
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
#include "shlwapi.h"
#include "appmodel.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

struct storage_file
{
    IStorageFile IStorageFile_iface;
    IStorageItem IStorageItem_iface;
    IRandomAccessStreamReference IRandomAccessStreamReference_iface;
    IInputStreamReference IInputStreamReference_iface;
    IStorageItemPropertiesInternal IStorageItemPropertiesInternal_iface;
    LONG ref;
    WCHAR *path;
};

static inline struct storage_file *impl_from_IStorageFile( IStorageFile *iface )
{
    return CONTAINING_RECORD( iface, struct storage_file, IStorageFile_iface );
}

static HRESULT WINAPI storage_file_QueryInterface( IStorageFile *iface, REFIID iid, void **out )
{
    struct storage_file *impl = impl_from_IStorageFile( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IStorageFile ))
    {
        *out = &impl->IStorageFile_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IStorageItem ))
    {
        *out = &impl->IStorageItem_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    /* A file that cannot answer this has no display name and no properties, so a library view
     * drops it even after enumerating it. */
    if (IsEqualGUID( iid, &IID_IStorageItemPropertiesInternal ))
    {
        *out = &impl->IStorageItemPropertiesInternal_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IRandomAccessStreamReference ))
    {
        *out = &impl->IRandomAccessStreamReference_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IInputStreamReference ))
    {
        *out = &impl->IInputStreamReference_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI storage_file_AddRef( IStorageFile *iface )
{
    struct storage_file *impl = impl_from_IStorageFile( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI storage_file_Release( IStorageFile *iface )
{
    struct storage_file *impl = impl_from_IStorageFile( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        free( impl->path );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI storage_file_GetIids( IStorageFile *iface, ULONG *iid_count, IID **iids )
{
    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );

    if (!iid_count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( 4 * sizeof(IID) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_IStorageFile;
    (*iids)[1] = IID_IStorageItem;
    (*iids)[2] = IID_IRandomAccessStreamReference;
    (*iids)[3] = IID_IInputStreamReference;
    *iid_count = 4;
    return S_OK;
}

static HRESULT WINAPI storage_file_GetRuntimeClassName( IStorageFile *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    if (!class_name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Storage_StorageFile,
                                ARRAY_SIZE(RuntimeClass_Windows_Storage_StorageFile) - 1, class_name );
}

static HRESULT WINAPI storage_file_GetTrustLevel( IStorageFile *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );

    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI storage_file_get_FileType( IStorageFile *iface, HSTRING *value )
{
    struct storage_file *impl = impl_from_IStorageFile( iface );
    const WCHAR *ext;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    ext = PathFindExtensionW( impl->path );
    return WindowsCreateString( ext, wcslen( ext ), value );
}

static HRESULT WINAPI storage_file_get_ContentType( IStorageFile *iface, HSTRING *value )
{
    struct storage_file *impl = impl_from_IStorageFile( iface );
    WCHAR type[256];

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    file_content_type( impl->path, type, ARRAY_SIZE(type) );
    return WindowsCreateString( type, wcslen( type ), value );
}

enum open_kind
{
    OPEN_RANDOM_ACCESS,     /* IStorageFile::OpenAsync */
    OPEN_WITH_CONTENT_TYPE, /* IRandomAccessStreamReference::OpenReadAsync */
    OPEN_SEQUENTIAL,        /* IInputStreamReference::OpenSequentialReadAsync */
};

struct open_params
{
    IUnknown IUnknown_iface;
    LONG ref;

    WCHAR *path;
    FileAccessMode mode;
    enum open_kind kind;
};

static void open_params_destroy( struct open_params *params )
{
    free( params->path );
    free( params );
}

DEFINE_ASYNC_PARAMS( open_params )

/* The file is opened by the operation, not by the call, so a missing or locked file fails the
 * operation the way Windows does rather than the call that started it. */
static HRESULT open_file_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct open_params *params = open_params_from_IUnknown( param );
    IRandomAccessStreamWithContentType *stream;
    HRESULT hr;

    if (FAILED(hr = file_stream_open( params->path, params->mode, params->kind == OPEN_SEQUENTIAL, &stream )))
        return hr;

    /* the operation's GetResults hands back punkVal as-is, so it must be the exact interface */
    result->vt = VT_UNKNOWN;
    switch (params->kind)
    {
    case OPEN_RANDOM_ACCESS:
        hr = IRandomAccessStreamWithContentType_QueryInterface( stream, &IID_IRandomAccessStream, (void **)&result->punkVal );
        break;
    case OPEN_WITH_CONTENT_TYPE:
        IRandomAccessStreamWithContentType_AddRef( stream );
        result->punkVal = (IUnknown *)stream;
        break;
    case OPEN_SEQUENTIAL:
        hr = IRandomAccessStreamWithContentType_QueryInterface( stream, &IID_IInputStream, (void **)&result->punkVal );
        break;
    }
    IRandomAccessStreamWithContentType_Release( stream );
    return hr;
}

static HRESULT open_params_create( struct storage_file *impl, FileAccessMode mode, enum open_kind kind, IUnknown **out )
{
    struct open_params *params;

    if (!(params = open_params_alloc())) return E_OUTOFMEMORY;
    if (!(params->path = wcsdup( impl->path )))
    {
        free( params );
        return E_OUTOFMEMORY;
    }
    params->mode = mode;
    params->kind = kind;
    *out = &params->IUnknown_iface;
    return S_OK;
}

static HRESULT WINAPI storage_file_OpenAsync( IStorageFile *iface, FileAccessMode mode, IAsyncOperation_IRandomAccessStream **operation )
{
    struct storage_file *impl = impl_from_IStorageFile( iface );
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, mode %d, operation %p.\n", iface, (int)mode, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (FAILED(hr = open_params_create( impl, mode, OPEN_RANDOM_ACCESS, &param ))) return hr;
    hr = async_operation_random_access_stream_create( (IUnknown *)iface, param, open_file_async, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI storage_file_OpenTransactedWriteAsync( IStorageFile *iface, IAsyncOperation_StorageStreamTransaction **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

/* Copy and move. The work is packed into one path parameter, "<op><option>|<source>|<target>", op
 * being C or M and the target either a file path or "<folder>\<name>". Shapr3D's exporter writes to a
 * temporary file and moves it onto the file the save picker returned. */
static HRESULT item_path( IUnknown *item, WCHAR *buffer, SIZE_T size )
{
    IStorageItem *storage_item;
    HSTRING path;
    HRESULT hr;

    if (!item) return E_INVALIDARG;
    if (FAILED(hr = IUnknown_QueryInterface( item, &IID_IStorageItem, (void **)&storage_item ))) return hr;
    hr = IStorageItem_get_Path( storage_item, &path );
    IStorageItem_Release( storage_item );
    if (FAILED(hr)) return hr;
    if (WindowsGetStringLen( path ) >= size) hr = HRESULT_FROM_WIN32( ERROR_FILENAME_EXCED_RANGE );
    else lstrcpyW( buffer, WindowsGetStringRawBuffer( path, NULL ) );
    WindowsDeleteString( path );
    return hr;
}

/* "name (2).ext", "name (3).ext", ... for NameCollisionOption_GenerateUniqueName */
static BOOL unique_target( WCHAR *target, SIZE_T size )
{
    WCHAR base[MAX_PATH], *dot, *slash;
    unsigned int i;

    if (GetFileAttributesW( target ) == INVALID_FILE_ATTRIBUTES) return TRUE;
    lstrcpynW( base, target, ARRAY_SIZE(base) );
    slash = wcsrchr( base, '\\' );
    if ((dot = wcsrchr( base, '.' )) && (!slash || dot > slash)) *dot++ = 0;
    else dot = NULL;
    for (i = 2; i < 1000; i++)
    {
        swprintf( target, size, dot ? L"%s (%u).%s" : L"%s (%u)", base, i, dot );
        if (GetFileAttributesW( target ) == INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    return FALSE;
}

static HRESULT transfer_file( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    const WCHAR *spec = storage_path_param_buffer( param ), *source, *sep;
    WCHAR source_path[MAX_PATH], target[MAX_PATH];
    NameCollisionOption option = spec[1] - '0';
    BOOL move = spec[0] == 'M', ok;
    IStorageFile *file;
    HRESULT hr;

    if (!called_async) return STATUS_PENDING;

    source = spec + 3;
    if (!(sep = wcschr( source, '|' )) || sep - source >= MAX_PATH) return E_INVALIDARG;
    memcpy( source_path, source, (sep - source) * sizeof(WCHAR) );
    source_path[sep - source] = 0;
    lstrcpynW( target, sep + 1, ARRAY_SIZE(target) );

    if (option == NameCollisionOption_GenerateUniqueName && !unique_target( target, ARRAY_SIZE(target) ))
        return HRESULT_FROM_WIN32( ERROR_ALREADY_EXISTS );
    if (option == NameCollisionOption_FailIfExists && GetFileAttributesW( target ) != INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32( ERROR_ALREADY_EXISTS );

    TRACE( "%s %s -> %s.\n", move ? "moving" : "copying", debugstr_w( source_path ), debugstr_w( target ) );
    if (move) ok = MoveFileExW( source_path, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED );
    else ok = CopyFileW( source_path, target, FALSE );
    if (!ok) return HRESULT_FROM_WIN32( GetLastError() );

    if (move) return S_OK;
    if (FAILED(hr = storage_file_create( target, &file ))) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)file;
    return S_OK;
}

static HRESULT transfer_param( struct storage_file *impl, WCHAR op, NameCollisionOption option, const WCHAR *target,
                               IUnknown **param )
{
    WCHAR *spec;
    SIZE_T len = 4 + wcslen( impl->path ) + wcslen( target );
    HRESULT hr;

    if (!(spec = malloc( (len + 1) * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    swprintf( spec, len + 1, L"%c%c|%s|%s", op, '0' + option, impl->path, target );
    hr = storage_path_param_create( spec, param );
    free( spec );
    return hr;
}

static HRESULT folder_target( IStorageFolder *folder, HSTRING name, const WCHAR *default_name, WCHAR *target, SIZE_T size )
{
    const WCHAR *leaf = name && WindowsGetStringLen( name ) ? WindowsGetStringRawBuffer( name, NULL ) : default_name;
    HRESULT hr;

    if (FAILED(hr = item_path( (IUnknown *)folder, target, size ))) return hr;
    if (wcslen( target ) + 1 + wcslen( leaf ) >= size) return HRESULT_FROM_WIN32( ERROR_FILENAME_EXCED_RANGE );
    if (target[0] && target[wcslen( target ) - 1] != '\\') wcscat( target, L"\\" );
    wcscat( target, leaf );
    return S_OK;
}

static const WCHAR *file_leaf( const struct storage_file *impl )
{
    const WCHAR *leaf = wcsrchr( impl->path, '\\' );
    return leaf ? leaf + 1 : impl->path;
}

static HRESULT WINAPI storage_file_CopyOverload( IStorageFile *iface, IStorageFolder *folder, HSTRING name, NameCollisionOption option, IAsyncOperation_StorageFile **operation )
{
    struct storage_file *impl = impl_from_IStorageFile( iface );
    WCHAR target[MAX_PATH];
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, folder %p, name %s, option %d, operation %p.\n", iface, folder, debugstr_hstring( name ), (int)option, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (FAILED(hr = folder_target( folder, name, file_leaf( impl ), target, ARRAY_SIZE(target) ))) return hr;
    if (FAILED(hr = transfer_param( impl, 'C', option, target, &param ))) return hr;
    hr = async_operation_storage_file_create( (IUnknown *)iface, param, transfer_file, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI storage_file_CopyOverloadDefaultNameAndOptions( IStorageFile *iface, IStorageFolder *folder, IAsyncOperation_StorageFile **operation )
{
    return storage_file_CopyOverload( iface, folder, NULL, NameCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_file_CopyOverloadDefaultOptions( IStorageFile *iface, IStorageFolder *folder, HSTRING name, IAsyncOperation_StorageFile **operation )
{
    return storage_file_CopyOverload( iface, folder, name, NameCollisionOption_FailIfExists, operation );
}

static HRESULT replace_file( IStorageFile *iface, IStorageFile *file, WCHAR op, IAsyncAction **operation )
{
    struct storage_file *impl = impl_from_IStorageFile( iface );
    WCHAR target[MAX_PATH];
    IUnknown *param;
    HRESULT hr;

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (FAILED(hr = item_path( (IUnknown *)file, target, ARRAY_SIZE(target) ))) return hr;
    if (FAILED(hr = transfer_param( impl, op, NameCollisionOption_ReplaceExisting, target, &param ))) return hr;
    hr = async_action_create( (IUnknown *)iface, param, transfer_file, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI storage_file_CopyAndReplaceAsync( IStorageFile *iface, IStorageFile *file, IAsyncAction **operation )
{
    TRACE( "iface %p, file %p, operation %p.\n", iface, file, operation );
    return replace_file( iface, file, 'C', operation );
}

static HRESULT WINAPI storage_file_MoveOverload( IStorageFile *iface, IStorageFolder *folder, HSTRING name, NameCollisionOption option, IAsyncAction **operation )
{
    struct storage_file *impl = impl_from_IStorageFile( iface );
    WCHAR target[MAX_PATH];
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, folder %p, name %s, option %d, operation %p.\n", iface, folder, debugstr_hstring( name ), (int)option, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (FAILED(hr = folder_target( folder, name, file_leaf( impl ), target, ARRAY_SIZE(target) ))) return hr;
    if (FAILED(hr = transfer_param( impl, 'M', option, target, &param ))) return hr;
    hr = async_action_create( (IUnknown *)iface, param, transfer_file, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI storage_file_MoveOverloadDefaultNameAndOptions( IStorageFile *iface, IStorageFolder *folder, IAsyncAction **operation )
{
    return storage_file_MoveOverload( iface, folder, NULL, NameCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_file_MoveOverloadDefaultOptions( IStorageFile *iface, IStorageFolder *folder, HSTRING name, IAsyncAction **operation )
{
    return storage_file_MoveOverload( iface, folder, name, NameCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_file_MoveAndReplaceAsync( IStorageFile *iface, IStorageFile *file, IAsyncAction **operation )
{
    TRACE( "iface %p, file %p, operation %p.\n", iface, file, operation );
    return replace_file( iface, file, 'M', operation );
}

static const struct IStorageFileVtbl storage_file_vtbl =
{
    storage_file_QueryInterface,
    storage_file_AddRef,
    storage_file_Release,
    /* IInspectable methods */
    storage_file_GetIids,
    storage_file_GetRuntimeClassName,
    storage_file_GetTrustLevel,
    /* IStorageFile methods */
    storage_file_get_FileType,
    storage_file_get_ContentType,
    storage_file_OpenAsync,
    storage_file_OpenTransactedWriteAsync,
    storage_file_CopyOverloadDefaultNameAndOptions,
    storage_file_CopyOverloadDefaultOptions,
    storage_file_CopyOverload,
    storage_file_CopyAndReplaceAsync,
    storage_file_MoveOverloadDefaultNameAndOptions,
    storage_file_MoveOverloadDefaultOptions,
    storage_file_MoveOverload,
    storage_file_MoveAndReplaceAsync,
};

DEFINE_IINSPECTABLE( file_item, IStorageItem, struct storage_file, IStorageFile_iface )

static HRESULT WINAPI file_item_RenameAsyncOverloadDefaultOptions( IStorageItem *iface, HSTRING name, IAsyncAction **operation )
{
    FIXME( "iface %p, name %s, operation %p stub!\n", iface, debugstr_hstring( name ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_item_RenameAsync( IStorageItem *iface, HSTRING name, NameCollisionOption option, IAsyncAction **operation )
{
    FIXME( "iface %p, name %s, option %d, operation %p stub!\n", iface, debugstr_hstring( name ), (int)option, operation );
    return E_NOTIMPL;
}

static HRESULT delete_file( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    if (!DeleteFileW( storage_path_param_buffer( param ) )) return HRESULT_FROM_WIN32( GetLastError() );
    return S_OK;
}

static HRESULT file_item_delete( IStorageItem *iface, IAsyncAction **operation )
{
    struct storage_file *impl = impl_from_IStorageItem( iface );
    IUnknown *param;
    HRESULT hr;

    if (!operation) return E_POINTER;
    if (FAILED(hr = storage_path_param_create( impl->path, &param ))) return hr;
    hr = async_action_create( (IUnknown *)iface, param, delete_file, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI file_item_DeleteAsyncOverloadDefaultOptions( IStorageItem *iface, IAsyncAction **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return file_item_delete( iface, operation );
}

/* StorageDeleteOption only chooses between the recycle bin and a permanent delete; Wine has no
 * recycle bin for a packaged app's files, so both do the same thing. */
static HRESULT WINAPI file_item_DeleteAsync( IStorageItem *iface, StorageDeleteOption option, IAsyncAction **operation )
{
    TRACE( "iface %p, option %d, operation %p.\n", iface, option, operation );
    return file_item_delete( iface, operation );
}

static HRESULT WINAPI file_item_GetBasicPropertiesAsync( IStorageItem *iface, IAsyncOperation_BasicProperties **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_item_get_Name( IStorageItem *iface, HSTRING *value )
{
    struct storage_file *impl = impl_from_IStorageItem( iface );
    const WCHAR *name;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if ((name = wcsrchr( impl->path, '\\' ))) name++;
    else name = impl->path;
    return WindowsCreateString( name, wcslen( name ), value );
}

static HRESULT WINAPI file_item_get_Path( IStorageItem *iface, HSTRING *value )
{
    struct storage_file *impl = impl_from_IStorageItem( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsCreateString( impl->path, wcslen( impl->path ), value );
}

static HRESULT WINAPI file_item_get_Attributes( IStorageItem *iface, FileAttributes *value )
{
    struct storage_file *impl = impl_from_IStorageItem( iface );
    DWORD attrs;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if ((attrs = GetFileAttributesW( impl->path )) == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32( GetLastError() );
    *value = attrs & (FileAttributes_ReadOnly | FileAttributes_Directory | FileAttributes_Archive |
                      FileAttributes_Temporary);
    return S_OK;
}

static HRESULT WINAPI file_item_get_DateCreated( IStorageItem *iface, DateTime *value )
{
    struct storage_file *impl = impl_from_IStorageItem( iface );
    WIN32_FILE_ATTRIBUTE_DATA data;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (!GetFileAttributesExW( impl->path, GetFileExInfoStandard, &data ))
        return HRESULT_FROM_WIN32( GetLastError() );
    value->UniversalTime = ((UINT64)data.ftCreationTime.dwHighDateTime << 32) | data.ftCreationTime.dwLowDateTime;
    return S_OK;
}

static HRESULT WINAPI file_item_IsOfType( IStorageItem *iface, StorageItemTypes type, boolean *value )
{
    TRACE( "iface %p, type %d, value %p.\n", iface, type, value );

    if (!value) return E_POINTER;
    *value = !!(type & StorageItemTypes_File);
    return S_OK;
}

static const struct IStorageItemVtbl file_item_vtbl =
{
    file_item_QueryInterface,
    file_item_AddRef,
    file_item_Release,
    /* IInspectable methods */
    file_item_GetIids,
    file_item_GetRuntimeClassName,
    file_item_GetTrustLevel,
    /* IStorageItem methods */
    file_item_RenameAsyncOverloadDefaultOptions,
    file_item_RenameAsync,
    file_item_DeleteAsyncOverloadDefaultOptions,
    file_item_DeleteAsync,
    file_item_GetBasicPropertiesAsync,
    file_item_get_Name,
    file_item_get_Path,
    file_item_get_Attributes,
    file_item_get_DateCreated,
    file_item_IsOfType,
};

DEFINE_IINSPECTABLE( file_stream_ref, IRandomAccessStreamReference, struct storage_file, IStorageFile_iface )

static HRESULT WINAPI file_stream_ref_OpenReadAsync( IRandomAccessStreamReference *iface,
                                                     IAsyncOperation_IRandomAccessStreamWithContentType **operation )
{
    struct storage_file *impl = impl_from_IRandomAccessStreamReference( iface );
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (FAILED(hr = open_params_create( impl, FileAccessMode_Read, OPEN_WITH_CONTENT_TYPE, &param ))) return hr;
    hr = async_operation_random_access_stream_ct_create( (IUnknown *)iface, param, open_file_async, operation );
    IUnknown_Release( param );
    return hr;
}

static const struct IRandomAccessStreamReferenceVtbl file_stream_ref_vtbl =
{
    file_stream_ref_QueryInterface,
    file_stream_ref_AddRef,
    file_stream_ref_Release,
    /* IInspectable methods */
    file_stream_ref_GetIids,
    file_stream_ref_GetRuntimeClassName,
    file_stream_ref_GetTrustLevel,
    /* IRandomAccessStreamReference methods */
    file_stream_ref_OpenReadAsync,
};

DEFINE_IINSPECTABLE( file_input_ref, IInputStreamReference, struct storage_file, IStorageFile_iface )

static HRESULT WINAPI file_input_ref_OpenSequentialReadAsync( IInputStreamReference *iface,
                                                              IAsyncOperation_IInputStream **operation )
{
    struct storage_file *impl = impl_from_IInputStreamReference( iface );
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (FAILED(hr = open_params_create( impl, FileAccessMode_Read, OPEN_SEQUENTIAL, &param ))) return hr;
    hr = async_operation_input_stream_create( (IUnknown *)iface, param, open_file_async, operation );
    IUnknown_Release( param );
    return hr;
}

static const struct IInputStreamReferenceVtbl file_input_ref_vtbl =
{
    file_input_ref_QueryInterface,
    file_input_ref_AddRef,
    file_input_ref_Release,
    /* IInspectable methods */
    file_input_ref_GetIids,
    file_input_ref_GetRuntimeClassName,
    file_input_ref_GetTrustLevel,
    /* IInputStreamReference methods */
    file_input_ref_OpenSequentialReadAsync,
};

DEFINE_IINSPECTABLE( file_props, IStorageItemPropertiesInternal, struct storage_file, IStorageFile_iface )

/* Thumbnails would mean decoding album art or rendering an icon; report them as unavailable
 * rather than hand back an empty stream the caller would try to decode. */
static HRESULT WINAPI file_props_GetThumbnailAsyncOverloadDefaultSizeDefaultOptions(
        IStorageItemPropertiesInternal *iface, INT32 mode, IInspectable **operation )
{
    FIXME( "iface %p, mode %d, operation %p stub!\n", iface, mode, operation );
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_props_GetThumbnailAsyncOverloadDefaultOptions(
        IStorageItemPropertiesInternal *iface, INT32 mode, UINT32 requested_size, IInspectable **operation )
{
    FIXME( "iface %p, mode %d, requested_size %u, operation %p stub!\n", iface, mode, requested_size, operation );
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_props_GetThumbnailAsync( IStorageItemPropertiesInternal *iface, INT32 mode,
        UINT32 requested_size, INT32 options, IInspectable **operation )
{
    FIXME( "iface %p, mode %d, requested_size %u, options %d, operation %p stub!\n", iface, mode,
           requested_size, options, operation );
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

/* The display name is the file name without its extension, which is what the shell shows when
 * extensions are hidden - and what an untagged track is listed as. */
static HRESULT WINAPI file_props_get_DisplayName( IStorageItemPropertiesInternal *iface, HSTRING *value )
{
    struct storage_file *impl = impl_from_IStorageItemPropertiesInternal( iface );
    const WCHAR *name, *ext;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    name = wcsrchr( impl->path, '\\' );
    name = name ? name + 1 : impl->path;
    ext = wcsrchr( name, '.' );
    return WindowsCreateString( name, ext ? (UINT32)(ext - name) : wcslen( name ), value );
}

static HRESULT WINAPI file_props_get_DisplayType( IStorageItemPropertiesInternal *iface, HSTRING *value )
{
    struct storage_file *impl = impl_from_IStorageItemPropertiesInternal( iface );
    const WCHAR *ext;
    WCHAR buffer[64];

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    /* Windows says "MP3 File" and so on; the extension is the only part we can know. */
    if ((ext = wcsrchr( impl->path, '.' )) && wcslen( ext ) < ARRAY_SIZE(buffer) - 8)
    {
        UINT32 i;
        for (i = 0; ext[i + 1]; i++) buffer[i] = towupper( ext[i + 1] );
        buffer[i] = 0;
        wcscat( buffer, L" File" );
        return WindowsCreateString( buffer, wcslen( buffer ), value );
    }
    return WindowsCreateString( L"File", 4, value );
}

/* The folder-relative id identifies an item within its query; the path is unique and stable, which
 * is all a caller uses it for. */
static HRESULT WINAPI file_props_get_FolderRelativeId( IStorageItemPropertiesInternal *iface, HSTRING *value )
{
    struct storage_file *impl = impl_from_IStorageItemPropertiesInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsCreateString( impl->path, wcslen( impl->path ), value );
}

static HRESULT WINAPI file_props_get_Properties( IStorageItemPropertiesInternal *iface,
        IStorageItemContentPropertiesInternal **value )
{
    struct storage_file *impl = impl_from_IStorageItemPropertiesInternal( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return content_properties_create( impl->path, value );
}

static const struct IStorageItemPropertiesInternalVtbl file_props_vtbl =
{
    /* IUnknown methods */
    file_props_QueryInterface,
    file_props_AddRef,
    file_props_Release,
    /* IInspectable methods */
    file_props_GetIids,
    file_props_GetRuntimeClassName,
    file_props_GetTrustLevel,
    /* IStorageItemProperties methods */
    file_props_GetThumbnailAsyncOverloadDefaultSizeDefaultOptions,
    file_props_GetThumbnailAsyncOverloadDefaultOptions,
    file_props_GetThumbnailAsync,
    file_props_get_DisplayName,
    file_props_get_DisplayType,
    file_props_get_FolderRelativeId,
    file_props_get_Properties,
};

/* The UWP host needs a StorageFile for a file-activation argument, and the async GetFileFromPathAsync
 * is awkward to drive from there; this is the same object, made directly. */
HRESULT WINAPI __wine_storage_file_create( const WCHAR *path, IStorageFile **out )
{
    return storage_file_create( path, out );
}

HRESULT storage_file_create( const WCHAR *path, IStorageFile **out )
{
    struct storage_file *impl;
    DWORD attrs;

    TRACE( "path %s, out %p.\n", debugstr_w( path ), out );

    if (!path || !out) return E_POINTER;
    *out = NULL;
    if ((attrs = GetFileAttributesW( path )) == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32( GetLastError() );
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (!(impl->path = wcsdup( path )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    impl->IStorageFile_iface.lpVtbl = &storage_file_vtbl;
    impl->IStorageItem_iface.lpVtbl = &file_item_vtbl;
    impl->IRandomAccessStreamReference_iface.lpVtbl = &file_stream_ref_vtbl;
    impl->IInputStreamReference_iface.lpVtbl = &file_input_ref_vtbl;
    impl->IStorageItemPropertiesInternal_iface.lpVtbl = &file_props_vtbl;
    impl->ref = 1;

    *out = &impl->IStorageFile_iface;
    return S_OK;
}

/*
 * StorageFile statics
 */

struct storage_file_statics
{
    IActivationFactory IActivationFactory_iface;
    IStorageFileStatics IStorageFileStatics_iface;
    LONG ref;
};

static inline struct storage_file_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct storage_file_statics, IActivationFactory_iface );
}

static HRESULT WINAPI file_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct storage_file_statics *impl = impl_from_IActivationFactory( iface );

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

    if (IsEqualGUID( iid, &IID_IStorageFileStatics ))
    {
        *out = &impl->IStorageFileStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI file_factory_AddRef( IActivationFactory *iface )
{
    struct storage_file_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI file_factory_Release( IActivationFactory *iface )
{
    struct storage_file_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI file_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* StorageFile is not activatable: it only comes out of the statics. */
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl file_factory_vtbl =
{
    file_factory_QueryInterface,
    file_factory_AddRef,
    file_factory_Release,
    /* IInspectable methods */
    file_factory_GetIids,
    file_factory_GetRuntimeClassName,
    file_factory_GetTrustLevel,
    /* IActivationFactory methods */
    file_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( file_statics, IStorageFileStatics, struct storage_file_statics, IActivationFactory_iface )

/* async_info_create keeps a reference on the parameter, so the path has to arrive as a COM object
 * rather than a bare string - the async machinery would otherwise AddRef the characters. */
struct path_param
{
    IUnknown IUnknown_iface;
    LONG ref;
    WCHAR path[1];
};

static inline struct path_param *impl_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct path_param, IUnknown_iface );
}

static HRESULT WINAPI path_param_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (!IsEqualGUID( iid, &IID_IUnknown )) { *out = NULL; return E_NOINTERFACE; }
    IUnknown_AddRef( iface );
    *out = iface;
    return S_OK;
}

static ULONG WINAPI path_param_AddRef( IUnknown *iface )
{
    return InterlockedIncrement( &impl_from_IUnknown( iface )->ref );
}

static ULONG WINAPI path_param_Release( IUnknown *iface )
{
    struct path_param *impl = impl_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static const struct IUnknownVtbl path_param_vtbl =
{
    path_param_QueryInterface,
    path_param_AddRef,
    path_param_Release,
};

HRESULT storage_path_param_create( const WCHAR *path, IUnknown **out )
{
    struct path_param *impl;
    SIZE_T len = wcslen( path );

    if (!(impl = calloc( 1, offsetof( struct path_param, path[len + 1] ) ))) return E_OUTOFMEMORY;
    impl->IUnknown_iface.lpVtbl = &path_param_vtbl;
    impl->ref = 1;
    memcpy( impl->path, path, (len + 1) * sizeof(WCHAR) );

    *out = &impl->IUnknown_iface;
    return S_OK;
}

const WCHAR *storage_path_param_buffer( IUnknown *param )
{
    return impl_from_IUnknown( param )->path;
}

static HRESULT get_file_from_path( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct path_param *path = impl_from_IUnknown( param );
    IStorageFile *file;
    HRESULT hr;

    if (FAILED(hr = storage_file_create( path->path, &file ))) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)file;
    return S_OK;
}

static HRESULT WINAPI file_statics_GetFileFromPathAsync( IStorageFileStatics *iface, HSTRING path,
                                                         IAsyncOperation_StorageFile **operation )
{
    const WCHAR *buffer;
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, path %s, operation %p.\n", iface, debugstr_hstring( path ), operation );

    if (!operation) return E_POINTER;
    if (!(buffer = WindowsGetStringRawBuffer( path, NULL ))) return E_INVALIDARG;
    if (FAILED(hr = storage_path_param_create( buffer, &param ))) return hr;
    hr = async_operation_storage_file_create( (IUnknown *)iface, param, get_file_from_path, operation );
    IUnknown_Release( param );
    return hr;
}

LONG WINAPI GetCurrentPackagePath( UINT32 *length, WCHAR *path );
LONG WINAPI GetCurrentPackageFamilyName( UINT32 *length, WCHAR *name );

static int hex_digit( WCHAR c )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Append a URI path to a directory: percent-decoded, '/' turned into '\\'. Returns FALSE for a
 * path that would climb out of the directory, which is how Windows keeps ms-appx inside the
 * package. Only ASCII escapes are decoded; a multi-byte UTF-8 escape is refused rather than
 * mangled. */
static BOOL append_uri_path( WCHAR *buf, SIZE_T size, const WCHAR *path, const WCHAR *end )
{
    SIZE_T len = wcslen( buf ), seg_start;

    for (seg_start = len; path < end; path++)
    {
        WCHAR c = *path;

        if (c == '?' || c == '#') break;
        if (c == '%')
        {
            int hi, lo;
            if (end - path < 3 || (hi = hex_digit( path[1] )) < 0 || (lo = hex_digit( path[2] )) < 0) return FALSE;
            if ((c = hi * 16 + lo) >= 0x80 || !c) return FALSE;
            path += 2;
        }
        if (c == '/') c = '\\';
        if (c == '\\')
        {
            /* reject "." and ".." segments outright */
            if (len - seg_start && buf[seg_start] == '.' &&
                (len - seg_start == 1 || (len - seg_start == 2 && buf[seg_start + 1] == '.')))
                return FALSE;
            if (len && buf[len - 1] == '\\') continue;
        }
        if (len + 1 >= size) return FALSE;
        buf[len++] = c;
        if (c == '\\') seg_start = len;
    }
    if (len - seg_start && buf[seg_start] == '.' &&
        (len - seg_start == 1 || (len - seg_start == 2 && buf[seg_start + 1] == '.')))
        return FALSE;
    buf[len] = 0;
    return TRUE;
}

/* ms-appx:///<path> names a file in the package's install directory, and ms-appdata:///local/,
 * /roaming/ and /temp/ name files in its data folders - the same %LOCALAPPDATA%\Packages\<family>
 * layout windows.storage.applicationdata hands out for ApplicationData.LocalFolder and friends.
 * The host part may be empty or the package name; nothing else is resolvable here. */
static HRESULT application_uri_to_path( IUriRuntimeClass *uri, WCHAR *buf, SIZE_T size )
{
    static const struct { const WCHAR *prefix; const WCHAR *state; } appdata[] =
    {
        { L"local/", L"LocalState" }, { L"roaming/", L"RoamingState" }, { L"temp/", L"TempState" },
    };
    const WCHAR *str, *rest, *path, *end;
    WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    UINT32 len, family_len = ARRAY_SIZE(family);
    HSTRING absolute;
    HRESULT hr = E_INVALIDARG;
    BOOL appx;
    LONG err;
    UINT i;

    if (FAILED(hr = IUriRuntimeClass_get_AbsoluteUri( uri, &absolute ))) return hr;
    str = WindowsGetStringRawBuffer( absolute, &len );
    end = str + len;
    TRACE( "uri %s.\n", debugstr_wn( str, len ) );

    hr = E_INVALIDARG;
    if (len > 9 && !wcsnicmp( str, L"ms-appx://", 10 )) { appx = TRUE; rest = str + 10; }
    else if (len > 12 && !wcsnicmp( str, L"ms-appdata://", 13 )) { appx = FALSE; rest = str + 13; }
    else goto done;

    /* skip the authority; it names the package, and we only have the one */
    for (path = rest; path < end && *path != '/'; path++);
    if (path++ == end) goto done;

    if (appx)
    {
        UINT32 path_len = size;
        if ((err = GetCurrentPackagePath( &path_len, buf ))) { hr = HRESULT_FROM_WIN32( err ); goto done; }
    }
    else
    {
        for (i = 0; i < ARRAY_SIZE(appdata); i++)
            if (end - path >= wcslen( appdata[i].prefix ) &&
                !wcsnicmp( path, appdata[i].prefix, wcslen( appdata[i].prefix ) )) break;
        if (i == ARRAY_SIZE(appdata)) goto done;
        path += wcslen( appdata[i].prefix );
        if ((err = GetCurrentPackageFamilyName( &family_len, family ))) { hr = HRESULT_FROM_WIN32( err ); goto done; }
        if (!GetEnvironmentVariableW( L"LOCALAPPDATA", buf, size ) ||
            swprintf( buf + wcslen( buf ), size - wcslen( buf ), L"\\Packages\\%s\\%s", family, appdata[i].state ) < 0)
        {
            hr = E_FAIL;
            goto done;
        }
    }

    if (buf[0] && buf[wcslen( buf ) - 1] != '\\' && wcslen( buf ) + 1 < size) wcscat( buf, L"\\" );
    if (append_uri_path( buf, size, path, end )) hr = S_OK;

done:
    if (FAILED(hr)) WARN( "cannot resolve %s, hr %#lx.\n", debugstr_wn( str, len ), hr );
    WindowsDeleteString( absolute );
    return hr;
}

static HRESULT WINAPI file_statics_GetFileFromApplicationUriAsync( IStorageFileStatics *iface, IUriRuntimeClass *uri, IAsyncOperation_StorageFile **operation )
{
    WCHAR path[MAX_PATH];
    IUnknown *param;
    HRESULT hr;

    TRACE( "iface %p, uri %p, operation %p.\n", iface, uri, operation );

    if (!uri) return E_INVALIDARG;
    if (!operation) return E_POINTER;
    if (FAILED(hr = application_uri_to_path( uri, path, ARRAY_SIZE(path) ))) return hr;
    TRACE( "resolved to %s.\n", debugstr_w( path ) );
    /* a missing file fails the operation, not the call, as GetFileFromPathAsync does */
    if (FAILED(hr = storage_path_param_create( path, &param ))) return hr;
    hr = async_operation_storage_file_create( (IUnknown *)iface, param, get_file_from_path, operation );
    IUnknown_Release( param );
    return hr;
}

static HRESULT WINAPI file_statics_CreateStreamedFileAsync( IStorageFileStatics *iface, HSTRING display_name_with_extension, IStreamedFileDataRequestedHandler *data_requested, IRandomAccessStreamReference *thumbnail, IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, display_name_with_extension %s, data_requested %p, thumbnail %p, operation %p stub!\n", iface, debugstr_hstring( display_name_with_extension ), data_requested, thumbnail, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_statics_ReplaceWithStreamedFileAsync( IStorageFileStatics *iface, IStorageFile *file_to_replace, IStreamedFileDataRequestedHandler *data_requested, IRandomAccessStreamReference *thumbnail, IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, file_to_replace %p, data_requested %p, thumbnail %p, operation %p stub!\n", iface, file_to_replace, data_requested, thumbnail, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_statics_CreateStreamedFileFromUriAsync( IStorageFileStatics *iface, HSTRING display_name_with_extension, IUriRuntimeClass *uri, IRandomAccessStreamReference *thumbnail, IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, display_name_with_extension %s, uri %p, thumbnail %p, operation %p stub!\n", iface, debugstr_hstring( display_name_with_extension ), uri, thumbnail, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_statics_ReplaceWithStreamedFileFromUriAsync( IStorageFileStatics *iface, IStorageFile *file_to_replace, IUriRuntimeClass *uri, IRandomAccessStreamReference *thumbnail, IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, file_to_replace %p, uri %p, thumbnail %p, operation %p stub!\n", iface, file_to_replace, uri, thumbnail, operation );
    return E_NOTIMPL;
}

static const struct IStorageFileStaticsVtbl file_statics_vtbl =
{
    file_statics_QueryInterface,
    file_statics_AddRef,
    file_statics_Release,
    /* IInspectable methods */
    file_statics_GetIids,
    file_statics_GetRuntimeClassName,
    file_statics_GetTrustLevel,
    /* IStorageFileStatics methods */
    file_statics_GetFileFromPathAsync,
    file_statics_GetFileFromApplicationUriAsync,
    file_statics_CreateStreamedFileAsync,
    file_statics_ReplaceWithStreamedFileAsync,
    file_statics_CreateStreamedFileFromUriAsync,
    file_statics_ReplaceWithStreamedFileFromUriAsync,
};

static struct storage_file_statics storage_file_statics =
{
    {&file_factory_vtbl},
    {&file_statics_vtbl},
    0,
};

IActivationFactory *storage_file_factory = &storage_file_statics.IActivationFactory_iface;

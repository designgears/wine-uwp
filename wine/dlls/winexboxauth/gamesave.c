/*
 * Windows.Gaming.XboxLive.Storage: connected storage (GameSaveProvider) kept on this machine
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
 * On Windows a UWP title's saves go through the connected storage service, which syncs them with Xbox Live title
 * storage; Windows' Windows.Gaming.XboxLive.Storage.dll cannot be activated without it (Gears of War 4: "Failed to
 * create game", 0x80040154). This keeps them on disk, synced with title storage by cloudsave.c:
 *
 *   %LOCALAPPDATA%\wineuwp\gamesave\<service config id>\<xuid>\<container>\<blob>
 *
 * Container and blob names are stored hex-encoded (they may hold any character); a container's display name is in
 * its ".display" file. Writes go to a temporary file and are renamed over the blob, so a crash leaves the old one.
 * Every operation completes before it returns.
 *
 * Slots (Windows.Gaming.winmd, ./winmd.py --sig):
 *   ProviderStatics {d01d3ed0}: 6 GetForUserAsync(user, scid) 7 GetSyncOnDemandForUserAsync(user, scid)
 *   Provider {90a60394}: 6 User 7 CreateContainer(name) 8 DeleteContainerAsync(name) 9/10 CreateContainerInfoQuery([prefix])
 *                        11 GetRemainingBytesInQuotaAsync 12 ContainersChangedSinceLastSync
 *   Container {c3c08f89}: 6 Name 7 Provider 8 SubmitUpdatesAsync(writes, deletes, displayName) 9 ReadAsync(blobs)
 *                         10 GetAsync(names) 11 SubmitPropertySetUpdatesAsync(writes, deletes, displayName)
 *                         12 CreateBlobInfoQuery(prefix)
 *   ContainerInfoQuery {3c94e863} / BlobInfoQuery {9fdd74b2}: 6/7 Get...InfoAsync([start, max]) 8 GetItemCountAsync
 */

#include "private.h"
#include <shlwapi.h>

WINE_DEFAULT_DEBUG_CHANNEL(xboxauth);

static const GUID IID_IGameSaveProviderStatics = {0xd01d3ed0,0x7b03,0x449d,{0x8c,0xbd,0x34,0x02,0x84,0x2a,0x10,0x48}};
static const GUID IID_IGameSaveProvider = {0x90a60394,0x80fe,0x4211,{0x97,0xf8,0xa5,0xde,0x14,0xdd,0x95,0xd2}};
static const GUID IID_IGameSaveProviderGetResult = {0x3ab90816,0xd393,0x4d65,{0xac,0x16,0x41,0xc3,0xe6,0x7a,0xb9,0x45}};
static const GUID IID_IGameSaveContainer = {0xc3c08f89,0x563f,0x4ecd,{0x9c,0x6f,0x33,0xfd,0x0e,0x32,0x3d,0x10}};
static const GUID IID_IGameSaveOperationResult = {0xcf0f1a05,0x24a0,0x4582,{0x9a,0x55,0xb1,0xbb,0xbb,0x93,0x88,0xd8}};
static const GUID IID_IGameSaveBlobGetResult = {0x917281e0,0x7201,0x4953,{0xaa,0x2c,0x40,0x08,0xf0,0x3a,0xef,0x45}};
static const GUID IID_IGameSaveContainerInfoQuery = {0x3c94e863,0x6f80,0x4327,{0x93,0x27,0xff,0xc1,0x1a,0xfd,0x42,0xb3}};
static const GUID IID_IGameSaveContainerInfo = {0xb7e27300,0x155d,0x4bb4,{0xb2,0xba,0x93,0x03,0x06,0xf3,0x91,0xb5}};
static const GUID IID_IGameSaveContainerInfoGetResult = {0xffc50d74,0xc581,0x4f9d,{0x9e,0x39,0x30,0xa1,0x0c,0x1e,0x4c,0x50}};
static const GUID IID_IGameSaveBlobInfoQuery = {0x9fdd74b2,0xeeee,0x447b,{0xa9,0xd2,0x7f,0x96,0xc0,0xf8,0x32,0x08}};
static const GUID IID_IGameSaveBlobInfo = {0xadd38034,0xbaf0,0x4645,{0xb6,0xd0,0x46,0xed,0xaf,0xfb,0x3c,0x2b}};
static const GUID IID_IGameSaveBlobInfoGetResult = {0xc7578582,0x3697,0x42bf,{0x98,0x9c,0x66,0x5d,0x92,0x3b,0x52,0x31}};

/* parameterised IIDs: piid.py --signature, checked against IVector<String> 98b9acc1 */
static const GUID IID_AsyncOp_ProviderGetResult = {0x06a8ff56,0x5cf9,0x5b78,{0xad,0x02,0xb8,0x57,0xba,0xc9,0x47,0xd9}};
static const GUID IID_AsyncOp_OperationResult = {0xd157ceb0,0x110c,0x55bf,{0xba,0x0a,0xf1,0x07,0x1a,0x74,0x1d,0x82}};
static const GUID IID_AsyncOp_BlobGetResult = {0x4487609d,0x57aa,0x54ed,{0x82,0xa7,0x61,0x7c,0x16,0x41,0x73,0x25}};
static const GUID IID_AsyncOp_ContainerInfoGetResult = {0xdd1e843e,0xae61,0x5efa,{0x97,0x18,0x88,0xb4,0xb1,0x8e,0xf4,0x88}};
static const GUID IID_AsyncOp_BlobInfoGetResult = {0xad740238,0x7211,0x5cad,{0xa8,0x22,0x1c,0x29,0x8d,0x7a,0x48,0x22}};
static const GUID IID_AsyncOp_INT64 = {0x2932cb17,0x2e62,0x5941,{0x9a,0xcd,0x63,0x66,0xa2,0x89,0x53,0x66}};
static const GUID IID_AsyncOp_UINT32 = {0x0428eac3,0xb8f6,0x5020,{0xbc,0xdc,0xd7,0x80,0x59,0xa1,0xf2,0xb3}};
static const GUID IID_IVectorView_String = {0x2f13c006,0xa03a,0x5f69,{0xb0,0x90,0x75,0xa4,0x3e,0x33,0x42,0x3e}};
static const GUID IID_IIterable_String = {0xe2fcc7c1,0x3bfc,0x5a0b,{0xb2,0xb0,0x72,0xe7,0x69,0xd1,0xcb,0x7e}};
static const GUID IID_IIterator_String = {0x8c304ebb,0x6615,0x50a4,{0x88,0x29,0x87,0x9e,0xcd,0x44,0x32,0x36}};
static const GUID IID_IVectorView_ContainerInfo = {0x9c490594,0x0846,0x50f5,{0xb2,0xef,0xc6,0xf0,0x3e,0xe6,0x86,0x8a}};
static const GUID IID_IIterable_ContainerInfo = {0x55e4d98f,0x0889,0x5c06,{0xa8,0x57,0x7d,0xd1,0x68,0xc2,0xd8,0x52}};
static const GUID IID_IIterator_ContainerInfo = {0x0ecd9756,0x3e0d,0x523f,{0xa5,0x49,0x2b,0x65,0x04,0xdb,0x52,0x02}};
static const GUID IID_IVectorView_BlobInfo = {0xb9c466a0,0x2a3f,0x5f28,{0xa1,0xc1,0x9c,0xb1,0x92,0xf6,0xc7,0x86}};
static const GUID IID_IIterable_BlobInfo = {0xa7c456d7,0xfa9f,0x536f,{0x8e,0xd2,0x45,0x95,0x45,0x81,0x1e,0xd4}};
static const GUID IID_IIterator_BlobInfo = {0xaaaf545b,0xf5e9,0x5da6,{0xaf,0x70,0x9d,0x90,0x4c,0x7a,0x4d,0x37}};
static const GUID IID_IMapView_String_Buffer = {0x10f6b0f3,0x55a9,0x55c2,{0x9d,0x4f,0xf6,0x3e,0x32,0x07,0x11,0x79}};
static const GUID IID_IKeyValuePair_String_Buffer = {0x1709dda3,0xd77b,0x5301,{0xae,0x12,0x8d,0xb4,0x5c,0x50,0x24,0xcf}};
static const GUID IID_IIterable_KVP_String_Buffer = {0xaec4873f,0x4329,0x509a,{0xb4,0x8a,0x2b,0x94,0x2a,0x77,0xcb,0x73}};
static const GUID IID_IIterator_KVP_String_Buffer = {0x592d3059,0x50bd,0x5ce9,{0xb9,0xbc,0xe3,0x67,0x25,0xa8,0xfd,0x20}};
/* IIterable<IKeyValuePair<String, Object>>, what an IPropertySet is enumerated as */
static const GUID IID_IIterable_KVP_String_Object = {0xfe2f3d47,0x5d47,0x5499,{0x83,0x74,0x43,0x0c,0x7c,0xda,0x02,0x04}};
static const GUID IID_IBuffer_ = {0x905a0fe0,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
static const GUID IID_IBufferByteAccess_ = {0x905a0fef,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};

/* GameSaveErrorStatus */
#define GS_OK                       0
#define GS_BLOB_NOT_FOUND           ((INT32)0x80830008)
#define GS_PROVIDED_BUFFER_TOO_SMALL ((INT32)0x80830007)
#define GS_INVALID_CONTAINER_NAME   ((INT32)0x80830001)
#define GS_OUT_OF_LOCAL_STORAGE     ((INT32)0x80830003)

#define GS_QUOTA (256 * 1024 * 1024)

/* ---- names and paths ---- */

WCHAR *hex_name( const char *name )
{
    size_t i, n = strlen( name );
    WCHAR *ret = malloc( (2 * n + 1) * sizeof(WCHAR) );
    if (!ret) return NULL;
    for (i = 0; i < n; i++) swprintf( ret + 2 * i, 3, L"%02x", (unsigned char)name[i] );
    ret[2 * n] = 0;
    return ret;
}

char *unhex_name( const WCHAR *hex )
{
    size_t i, n = wcslen( hex );
    char *ret;

    if (n % 2) return NULL;
    if (!(ret = malloc( n / 2 + 1 ))) return NULL;
    for (i = 0; i < n / 2; i++)
    {
        unsigned v;
        if (swscanf( hex + 2 * i, L"%2x", &v ) != 1) { free( ret ); return NULL; }
        ret[i] = v;
    }
    ret[n / 2] = 0;
    return ret;
}

WCHAR *path_join( const WCHAR *a, const WCHAR *b )
{
    WCHAR *ret = malloc( (wcslen( a ) + wcslen( b ) + 2) * sizeof(WCHAR) );
    if (ret) swprintf( ret, wcslen( a ) + wcslen( b ) + 2, L"%s\\%s", a, b );
    return ret;
}

void make_dirs( const WCHAR *path )
{
    WCHAR *p = wcsdup( path ), *s;
    if (!p) return;
    for (s = p + 3; *s; s++)
    {
        if (*s != '\\') continue;
        *s = 0;
        CreateDirectoryW( p, NULL );
        *s = '\\';
    }
    CreateDirectoryW( p, NULL );
    free( p );
}

BOOL read_file( const WCHAR *path, BYTE **data, DWORD *size )
{
    HANDLE f = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    DWORD got = 0;
    BOOL ok;

    *data = NULL;
    *size = 0;
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    *size = GetFileSize( f, NULL );
    ok = (*data = malloc( *size ? *size : 1 )) && ReadFile( f, *data, *size, &got, NULL ) && got == *size;
    CloseHandle( f );
    if (!ok)
    {
        free( *data );
        *data = NULL;
    }
    return ok;
}

BOOL write_file( const WCHAR *path, const void *data, DWORD size )
{
    WCHAR *tmp = malloc( (wcslen( path ) + 5) * sizeof(WCHAR) );
    HANDLE f;
    DWORD done = 0;
    BOOL ok;

    if (!tmp) return FALSE;
    swprintf( tmp, wcslen( path ) + 5, L"%s.new", path );
    f = CreateFileW( tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    if (f == INVALID_HANDLE_VALUE)
    {
        free( tmp );
        return FALSE;
    }
    ok = WriteFile( f, data, size, &done, NULL ) && done == size && FlushFileBuffers( f );
    CloseHandle( f );
    ok = ok && MoveFileExW( tmp, path, MOVEFILE_REPLACE_EXISTING );
    if (!ok) DeleteFileW( tmp );
    free( tmp );
    return ok;
}

/* the bytes of an IBuffer, or NULL */
BYTE *buffer_bytes( IInspectable *buffer, UINT32 *length, UINT32 *capacity )
{
    IUnknown *access = NULL;
    BYTE *bytes = NULL;

    *length = *capacity = 0;
    if (!buffer) return NULL;
    SLOT(buffer, 6, HRESULT (WINAPI *)( void *, UINT32 * ))( buffer, capacity );
    SLOT(buffer, 7, HRESULT (WINAPI *)( void *, UINT32 * ))( buffer, length );
    if (SUCCEEDED(IInspectable_QueryInterface( buffer, &IID_IBufferByteAccess_, (void **)&access )))
    {
        SLOT(access, 3, HRESULT (WINAPI *)( void *, BYTE ** ))( access, &bytes );
        IUnknown_Release( access );
    }
    return bytes;
}

/* calls fn( key, value, ctx ) for each pair of an IIterable<IKeyValuePair<String, T>> */
static HRESULT for_each_pair( IInspectable *iterable, HRESULT (*fn)( const char *key, IInspectable *value, void *ctx ),
                              void *ctx )
{
    IInspectable *it = NULL, *kvp, *value;
    boolean has = FALSE;
    HRESULT hr;
    HSTRING h;

    if (FAILED(hr = SLOT(iterable, 6, HRESULT (WINAPI *)( void *, IInspectable ** ))( iterable, &it ))) return hr;
    SLOT(it, 7, HRESULT (WINAPI *)( void *, boolean * ))( it, &has );
    while (has && SUCCEEDED(hr))
    {
        kvp = value = NULL;
        h = NULL;
        if (SUCCEEDED(hr = SLOT(it, 6, HRESULT (WINAPI *)( void *, IInspectable ** ))( it, &kvp )))
        {
            char *key;
            SLOT(kvp, 6, HRESULT (WINAPI *)( void *, HSTRING * ))( kvp, &h );
            SLOT(kvp, 7, HRESULT (WINAPI *)( void *, IInspectable ** ))( kvp, &value );
            key = from_hstr( h );
            hr = fn( S(key), value, ctx );
            free( key );
            WindowsDeleteString( h );
            if (value) IInspectable_Release( value );
            IInspectable_Release( kvp );
        }
        if (SUCCEEDED(hr)) hr = SLOT(it, 8, HRESULT (WINAPI *)( void *, boolean * ))( it, &has );
    }
    IInspectable_Release( it );
    return hr;
}

/* calls fn( name, ctx ) for each string of an IIterable<String> */
static HRESULT for_each_string( IInspectable *iterable, HRESULT (*fn)( const char *name, void *ctx ), void *ctx )
{
    IInspectable *it = NULL;
    boolean has = FALSE;
    HRESULT hr;
    HSTRING h;

    if (!iterable) return S_OK;
    if (FAILED(hr = SLOT(iterable, 6, HRESULT (WINAPI *)( void *, IInspectable ** ))( iterable, &it ))) return hr;
    SLOT(it, 7, HRESULT (WINAPI *)( void *, boolean * ))( it, &has );
    while (has && SUCCEEDED(hr))
    {
        h = NULL;
        if (SUCCEEDED(hr = SLOT(it, 6, HRESULT (WINAPI *)( void *, HSTRING * ))( it, &h )))
        {
            char *name = from_hstr( h );
            hr = fn( S(name), ctx );
            free( name );
            WindowsDeleteString( h );
        }
        if (SUCCEEDED(hr)) hr = SLOT(it, 8, HRESULT (WINAPI *)( void *, boolean * ))( it, &has );
    }
    IInspectable_Release( it );
    return hr;
}

/* ---- result objects: a status, and for some a value ---- */

struct result
{
    struct object obj;
    INT32 status;
    IInspectable *value;
};

static struct result *result_from( void *iface ) { return CONTAINING_RECORD( OBJ_AT(iface, 0), struct result, obj ); }
static void result_destroy( struct object *obj )
{
    struct result *r = CONTAINING_RECORD( obj, struct result, obj );
    if (r->value) IInspectable_Release( r->value );
}
static HRESULT WINAPI result_get_Status( void *iface, INT32 *v )
{
    if (!v) return E_POINTER;
    *v = result_from( iface )->status;
    return S_OK;
}
static HRESULT WINAPI result_get_Value( void *iface, IInspectable **v )
{
    struct result *r = result_from( iface );
    if (!v) return E_POINTER;
    if ((*v = r->value)) IInspectable_AddRef( r->value );
    return S_OK;
}
static const void *result_vtbl[] = { OBJ_METHODS(0), result_get_Status, result_get_Value };

/* completes an IAsyncOperation<cls> with a result holding status and value (whose reference it takes) */
static HRESULT complete_result( const GUID *async_iid, const WCHAR *cls, const GUID *iid, const char *name, INT32 status,
                                IInspectable *value, void **out )
{
    struct result *r = object_create( sizeof(*r), cls, name, result_destroy );
    HRESULT hr;

    if (!r)
    {
        if (value) IInspectable_Release( value );
        return E_OUTOFMEMORY;
    }
    object_add( &r->obj, result_vtbl, iid );
    r->status = status;
    r->value = value;
    TRACE( "%s -> status %#x\n", name, status );
    hr = completed_iface( async_iid, name, (IUnknown *)&r->obj.vtbl[0], out );
    obj_Release( &r->obj );
    return hr;
}

static HRESULT complete_operation( const char *name, INT32 status, void **out )
{
    return complete_result( &IID_AsyncOp_OperationResult, L"Windows.Gaming.XboxLive.Storage.GameSaveOperationResult",
                            &IID_IGameSaveOperationResult, name, status, NULL, out );
}

static HRESULT complete_number( const GUID *iid, enum result_kind kind, const char *name, INT64 value, void **out )
{
    struct async *op = async_create( iid, kind, name );
    if (!op) return E_OUTOFMEMORY;
    op->i4 = (INT32)value;
    op->i4b = (INT32)(value >> 32);
    async_finish( op, S_OK );
    *out = &op->obj.vtbl[0];
    return S_OK;
}

/* ---- an IBuffer of our own, and IMapView<String, IBuffer> ---- */

struct buffer
{
    struct object obj;
    BYTE *bytes;
    UINT32 capacity, length;
};

static struct buffer *buffer_from( void *iface, int n ) { return CONTAINING_RECORD( OBJ_AT(iface, n), struct buffer, obj ); }
static void buffer_destroy( struct object *obj ) { free( CONTAINING_RECORD( obj, struct buffer, obj )->bytes ); }
static HRESULT WINAPI buffer_get_Capacity( void *iface, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = buffer_from( iface, 0 )->capacity;
    return S_OK;
}
static HRESULT WINAPI buffer_get_Length( void *iface, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = buffer_from( iface, 0 )->length;
    return S_OK;
}
static HRESULT WINAPI buffer_put_Length( void *iface, UINT32 v )
{
    struct buffer *b = buffer_from( iface, 0 );
    if (v > b->capacity) return E_INVALIDARG;
    b->length = v;
    return S_OK;
}
static HRESULT WINAPI buffer_Buffer( void *iface, BYTE **v )
{
    if (!v) return E_POINTER;
    *v = buffer_from( iface, 1 )->bytes;
    return S_OK;
}
static const void *buffer_vtbl[] = { OBJ_METHODS(0), buffer_get_Capacity, buffer_get_Length, buffer_put_Length };
static const void *buffer_access_vtbl[] = { UNK_METHODS(1), buffer_Buffer };

/* takes bytes */
IInspectable *buffer_create( BYTE *bytes, UINT32 size )
{
    struct buffer *b = object_create( sizeof(*b), L"Windows.Storage.Streams.Buffer", "Buffer", buffer_destroy );
    if (!b)
    {
        free( bytes );
        return NULL;
    }
    object_add( &b->obj, buffer_vtbl, &IID_IBuffer_ );
    object_add( &b->obj, buffer_access_vtbl, &IID_IBufferByteAccess_ );
    b->bytes = bytes;
    b->capacity = b->length = size;
    return (IInspectable *)&b->obj.vtbl[0];
}

struct blob_map
{
    struct object obj;
    char **names;
    IInspectable **buffers;
    UINT32 count;
};

struct kvp
{
    struct object obj;
    char *key;
    IInspectable *value;
};

struct map_iterator
{
    struct object obj;
    struct blob_map *map;
    UINT32 pos;
};

static struct blob_map *blob_map_from( void *iface, int n ) { return CONTAINING_RECORD( OBJ_AT(iface, n), struct blob_map, obj ); }

static void blob_map_destroy( struct object *obj )
{
    struct blob_map *m = CONTAINING_RECORD( obj, struct blob_map, obj );
    UINT32 i;
    for (i = 0; i < m->count; i++)
    {
        free( m->names[i] );
        IInspectable_Release( m->buffers[i] );
    }
    free( m->names );
    free( m->buffers );
}

static int blob_map_find( struct blob_map *m, HSTRING key )
{
    char *k = from_hstr( key );
    UINT32 i;
    int ret = -1;
    for (i = 0; k && i < m->count; i++) if (!strcmp( m->names[i], k )) ret = i;
    free( k );
    return ret;
}

static HRESULT WINAPI blob_map_Lookup( void *iface, HSTRING key, IInspectable **v )
{
    struct blob_map *m = blob_map_from( iface, 0 );
    int i = blob_map_find( m, key );
    if (!v) return E_POINTER;
    *v = NULL;
    if (i < 0) return E_BOUNDS;
    IInspectable_AddRef( (*v = m->buffers[i]) );
    return S_OK;
}
static HRESULT WINAPI blob_map_get_Size( void *iface, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = blob_map_from( iface, 0 )->count;
    return S_OK;
}
static HRESULT WINAPI blob_map_HasKey( void *iface, HSTRING key, boolean *v )
{
    if (!v) return E_POINTER;
    *v = blob_map_find( blob_map_from( iface, 0 ), key ) >= 0;
    return S_OK;
}
static HRESULT WINAPI blob_map_Split( void *iface, IInspectable **first, IInspectable **second )
{
    if (!first || !second) return E_POINTER;
    *first = *second = NULL;
    return S_OK;
}

static void kvp_destroy( struct object *obj )
{
    struct kvp *p = CONTAINING_RECORD( obj, struct kvp, obj );
    free( p->key );
    IInspectable_Release( p->value );
}
static HRESULT WINAPI kvp_get_Key( void *iface, HSTRING *v )
{
    return to_hstr( CONTAINING_RECORD( OBJ_AT(iface, 0), struct kvp, obj )->key, v );
}
static HRESULT WINAPI kvp_get_Value( void *iface, IInspectable **v )
{
    struct kvp *p = CONTAINING_RECORD( OBJ_AT(iface, 0), struct kvp, obj );
    if (!v) return E_POINTER;
    IInspectable_AddRef( (*v = p->value) );
    return S_OK;
}
static const void *kvp_vtbl[] = { OBJ_METHODS(0), kvp_get_Key, kvp_get_Value };

static struct map_iterator *map_iterator_from( void *iface ) { return CONTAINING_RECORD( OBJ_AT(iface, 0), struct map_iterator, obj ); }
static void map_iterator_destroy( struct object *obj )
{
    obj_Release( &CONTAINING_RECORD( obj, struct map_iterator, obj )->map->obj );
}
static HRESULT WINAPI map_iterator_get_Current( void *iface, IInspectable **v )
{
    struct map_iterator *it = map_iterator_from( iface );
    struct kvp *p;

    if (!v) return E_POINTER;
    *v = NULL;
    if (it->pos >= it->map->count) return E_BOUNDS;
    if (!(p = object_create( sizeof(*p), L"Windows.Foundation.Collections.IKeyValuePair`2<String, Windows.Storage.Streams.IBuffer>",
                             "KeyValuePair", kvp_destroy )))
        return E_OUTOFMEMORY;
    object_add( &p->obj, kvp_vtbl, &IID_IKeyValuePair_String_Buffer );
    p->key = str_dup( it->map->names[it->pos] );
    IInspectable_AddRef( (p->value = it->map->buffers[it->pos]) );
    *v = (IInspectable *)&p->obj.vtbl[0];
    return S_OK;
}
static HRESULT WINAPI map_iterator_get_HasCurrent( void *iface, boolean *v )
{
    struct map_iterator *it = map_iterator_from( iface );
    if (!v) return E_POINTER;
    *v = it->pos < it->map->count;
    return S_OK;
}
static HRESULT WINAPI map_iterator_MoveNext( void *iface, boolean *v )
{
    struct map_iterator *it = map_iterator_from( iface );
    if (it->pos < it->map->count) it->pos++;
    return map_iterator_get_HasCurrent( iface, v );
}
static const void *map_iterator_vtbl[] = { OBJ_METHODS(0), map_iterator_get_Current, map_iterator_get_HasCurrent, map_iterator_MoveNext };

static HRESULT WINAPI blob_map_First( void *iface, IInspectable **v )
{
    struct blob_map *m = blob_map_from( iface, 1 );
    struct map_iterator *it;

    if (!v) return E_POINTER;
    if (!(it = object_create( sizeof(*it), L"Windows.Foundation.Collections.IIterator`1<Windows.Foundation.Collections.IKeyValuePair`2<String, Windows.Storage.Streams.IBuffer>>",
                              "MapIterator", map_iterator_destroy )))
        return E_OUTOFMEMORY;
    object_add( &it->obj, map_iterator_vtbl, &IID_IIterator_KVP_String_Buffer );
    obj_AddRef( &m->obj );
    it->map = m;
    *v = (IInspectable *)&it->obj.vtbl[0];
    return S_OK;
}

static const void *blob_map_vtbl[] = { OBJ_METHODS(0), blob_map_Lookup, blob_map_get_Size, blob_map_HasKey, blob_map_Split };
static const void *blob_map_iterable_vtbl[] = { OBJ_METHODS(1), blob_map_First };

static struct blob_map *blob_map_create( void )
{
    struct blob_map *m = object_create( sizeof(*m), L"Windows.Foundation.Collections.IMapView`2<String, Windows.Storage.Streams.IBuffer>",
                                        "BlobMap", blob_map_destroy );
    if (!m) return NULL;
    object_add( &m->obj, blob_map_vtbl, &IID_IMapView_String_Buffer );
    object_add( &m->obj, blob_map_iterable_vtbl, &IID_IIterable_KVP_String_Buffer );
    return m;
}

/* takes buffer */
static BOOL blob_map_add( struct blob_map *m, const char *name, IInspectable *buffer )
{
    char **names = realloc( m->names, (m->count + 1) * sizeof(*names) );
    IInspectable **buffers;

    if (names) m->names = names;
    if (!names || !(buffers = realloc( m->buffers, (m->count + 1) * sizeof(*buffers) )))
    {
        IInspectable_Release( buffer );
        return FALSE;
    }
    m->buffers = buffers;
    m->names[m->count] = str_dup( name );
    m->buffers[m->count++] = buffer;
    return TRUE;
}

/* ---- the provider ---- */

struct provider
{
    struct object obj;
    IInspectable *user;
    WCHAR *dir;                 /* ...\gamesave\<scid>\<xuid> */
    char *cloud;                /* its title storage URL, NULL when not signed in */
};

static struct provider *provider_from( void *iface ) { return CONTAINING_RECORD( OBJ_AT(iface, 0), struct provider, obj ); }

static void provider_destroy( struct object *obj )
{
    struct provider *p = CONTAINING_RECORD( obj, struct provider, obj );
    if (p->user) IInspectable_Release( p->user );
    free( p->dir );
    free( p->cloud );
}

struct container
{
    struct object obj;
    struct provider *provider;
    char *name;
    WCHAR *dir;
};

static struct container *container_from( void *iface ) { return CONTAINING_RECORD( OBJ_AT(iface, 0), struct container, obj ); }

static void container_destroy( struct object *obj )
{
    struct container *c = CONTAINING_RECORD( obj, struct container, obj );
    obj_Release( &c->provider->obj );
    free( c->name );
    free( c->dir );
}

static WCHAR *container_dir( struct provider *p, const char *name )
{
    WCHAR *hex = hex_name( name ), *ret = hex ? path_join( p->dir, hex ) : NULL;
    free( hex );
    return ret;
}

static WCHAR *blob_path( struct container *c, const char *name )
{
    WCHAR *hex = hex_name( name ), *ret = hex ? path_join( c->dir, hex ) : NULL;
    free( hex );
    return ret;
}

/* bytes in every blob of a directory */
static UINT64 dir_size( const WCHAR *dir, FILETIME *newest )
{
    WCHAR *pattern = path_join( dir, L"*" );
    WIN32_FIND_DATAW fd;
    UINT64 total = 0;
    HANDLE h;

    if (newest) newest->dwLowDateTime = newest->dwHighDateTime = 0;
    if (!pattern) return 0;
    if ((h = FindFirstFileW( pattern, &fd )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (fd.cFileName[0] == '.') continue;
            total += ((UINT64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (newest && CompareFileTime( &fd.ftLastWriteTime, newest ) > 0) *newest = fd.ftLastWriteTime;
        } while (FindNextFileW( h, &fd ));
        FindClose( h );
    }
    free( pattern );
    return total;
}

/* ---- SubmitUpdatesAsync / SubmitPropertySetUpdatesAsync ---- */

struct submit
{
    struct container *c;
    INT32 status;
    unsigned written, deleted;
};

static HRESULT submit_write( const char *name, IInspectable *value, void *ctx )
{
    struct submit *s = ctx;
    IInspectable *buffer = NULL;
    UINT32 length, capacity;
    WCHAR *path;
    BYTE *bytes;

    if (!*name || s->status) return S_OK;
    if (value) IInspectable_QueryInterface( value, &IID_IBuffer_, (void **)&buffer );
    bytes = buffer_bytes( buffer, &length, &capacity );
    if ((path = blob_path( s->c, name )))
    {
        if (!write_file( path, bytes ? bytes : (BYTE *)"", bytes ? length : 0 )) s->status = GS_OUT_OF_LOCAL_STORAGE;
        else
        {
            cloud_blob_written( s->c->dir, name );
            s->written++;
        }
        free( path );
    }
    if (buffer) IInspectable_Release( buffer );
    return S_OK;
}

static HRESULT submit_delete( const char *name, void *ctx )
{
    struct submit *s = ctx;
    WCHAR *path = blob_path( s->c, name );
    if (path && DeleteFileW( path ))
    {
        cloud_blob_written( s->c->dir, name );
        s->deleted++;
    }
    free( path );
    return S_OK;
}

static HRESULT submit_updates( struct container *c, IInspectable *writes, const GUID *iterable_iid, IInspectable *deletes,
                               HSTRING display, const char *name, void **out )
{
    struct submit s = { c, GS_OK };
    IInspectable *iterable = NULL;
    char *shown = from_hstr( display );
    WCHAR *path;

    if (!out) return E_POINTER;
    make_dirs( c->dir );
    if (writes && SUCCEEDED(IInspectable_QueryInterface( writes, iterable_iid, (void **)&iterable )))
    {
        for_each_pair( iterable, submit_write, &s );
        IInspectable_Release( iterable );
    }
    if (!s.status) for_each_string( deletes, submit_delete, &s );
    if (!s.status && shown && *shown && (path = path_join( c->dir, L".display" )))
    {
        write_file( path, shown, strlen( shown ) );
        free( path );
    }
    TRACE( "GameSaveContainer(%s).%s: %u written, %u deleted, display name %s\n", debugstr_a(c->name), name, s.written,
           s.deleted, debugstr_a(shown) );
    if (!s.status) cloud_container_changed( c->provider->cloud, c->dir, c->name );
    free( shown );
    return complete_operation( name, s.status, out );
}

/* ---- ReadAsync: into the caller's buffers ---- */

static HRESULT read_into( const char *name, IInspectable *value, void *ctx )
{
    struct submit *s = ctx;
    IInspectable *buffer = NULL;
    UINT32 length, capacity;
    BYTE *bytes, *data;
    DWORD size;
    WCHAR *path;

    if (s->status) return S_OK;
    if (!(path = blob_path( s->c, name )) || !read_file( path, &data, &size ))
    {
        s->status = GS_BLOB_NOT_FOUND;
        free( path );
        return S_OK;
    }
    free( path );
    if (value) IInspectable_QueryInterface( value, &IID_IBuffer_, (void **)&buffer );
    bytes = buffer_bytes( buffer, &length, &capacity );
    if (!bytes || capacity < size) s->status = GS_PROVIDED_BUFFER_TOO_SMALL;
    else
    {
        memcpy( bytes, data, size );
        SLOT(buffer, 8, HRESULT (WINAPI *)( void *, UINT32 ))( buffer, size );
        s->written++;
    }
    if (buffer) IInspectable_Release( buffer );
    free( data );
    return S_OK;
}

/* ---- GetAsync: new buffers ---- */

struct get
{
    struct container *c;
    struct blob_map *map;
    INT32 status;
};

static HRESULT get_blob( const char *name, void *ctx )
{
    struct get *g = ctx;
    IInspectable *buffer;
    BYTE *data;
    DWORD size;
    WCHAR *path;

    if (g->status) return S_OK;
    if (!(path = blob_path( g->c, name )) || !read_file( path, &data, &size ))
        g->status = GS_BLOB_NOT_FOUND;
    else if ((buffer = buffer_create( data, size ))) blob_map_add( g->map, name, buffer );
    free( path );
    return S_OK;
}

/* ---- info queries ---- */

struct info
{
    struct object obj;
    char *name, *display;
    UINT64 size;
    FILETIME modified;
};

static struct info *info_from( void *iface ) { return CONTAINING_RECORD( OBJ_AT(iface, 0), struct info, obj ); }
static void info_destroy( struct object *obj )
{
    struct info *i = CONTAINING_RECORD( obj, struct info, obj );
    free( i->name );
    free( i->display );
}
static HRESULT WINAPI info_get_Name( void *iface, HSTRING *v ) { return to_hstr( info_from( iface )->name, v ); }
static HRESULT WINAPI info_get_TotalSize( void *iface, UINT64 *v )
{
    if (!v) return E_POINTER;
    *v = info_from( iface )->size;
    return S_OK;
}
static HRESULT WINAPI info_get_Size( void *iface, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = info_from( iface )->size;
    return S_OK;
}
static HRESULT WINAPI info_get_DisplayName( void *iface, HSTRING *v ) { return to_hstr( S(info_from( iface )->display), v ); }
static HRESULT WINAPI info_get_LastModifiedTime( void *iface, INT64 *v )
{
    FILETIME *ft = &info_from( iface )->modified;
    if (!v) return E_POINTER;
    *v = ((INT64)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    return S_OK;
}
static HRESULT WINAPI info_get_NeedsSync( void *iface, boolean *v )
{
    if (!v) return E_POINTER;
    *v = FALSE;
    return S_OK;
}
static const void *container_info_vtbl[] =
{
    OBJ_METHODS(0), info_get_Name, info_get_TotalSize, info_get_DisplayName, info_get_LastModifiedTime, info_get_NeedsSync
};
static const void *blob_info_vtbl[] = { OBJ_METHODS(0), info_get_Name, info_get_Size };

struct query
{
    struct object obj;
    BOOL blobs;
    WCHAR *dir;
    char *prefix;
};

static struct query *query_from( void *iface ) { return CONTAINING_RECORD( OBJ_AT(iface, 0), struct query, obj ); }
static void query_destroy( struct object *obj )
{
    struct query *q = CONTAINING_RECORD( obj, struct query, obj );
    free( q->dir );
    free( q->prefix );
}

/* the containers (subdirectories) or blobs (files) of the query's directory whose names start with its prefix */
static struct vector_view *query_run( struct query *q, UINT32 start, UINT32 max )
{
    struct vector_view *view;
    WCHAR *pattern = path_join( q->dir, L"*" );
    WIN32_FIND_DATAW fd;
    UINT32 index = 0;
    HANDLE h;

    if (q->blobs)
        view = vector_view_create( &IID_IVectorView_BlobInfo, &IID_IIterable_BlobInfo, &IID_IIterator_BlobInfo,
                L"Windows.Foundation.Collections.IVectorView`1<Windows.Gaming.XboxLive.Storage.GameSaveBlobInfo>" );
    else
        view = vector_view_create( &IID_IVectorView_ContainerInfo, &IID_IIterable_ContainerInfo, &IID_IIterator_ContainerInfo,
                L"Windows.Foundation.Collections.IVectorView`1<Windows.Gaming.XboxLive.Storage.GameSaveContainerInfo>" );
    if (!view || !pattern || (h = FindFirstFileW( pattern, &fd )) == INVALID_HANDLE_VALUE)
    {
        free( pattern );
        return view;
    }
    do
    {
        BOOL is_dir = !!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        struct info *info;
        char *name;
        size_t len = wcslen( fd.cFileName );

        if (fd.cFileName[0] == '.' || is_dir != !q->blobs) continue;
        if (len > 4 && !wcscmp( fd.cFileName + len - 4, L".new" )) continue;
        if (!(name = unhex_name( fd.cFileName ))) continue;
        if (strncmp( name, S(q->prefix), strlen( S(q->prefix) ) ) || index++ < start || (max && view->count >= max))
        {
            free( name );
            continue;
        }
        if (!(info = object_create( sizeof(*info), q->blobs ? L"Windows.Gaming.XboxLive.Storage.GameSaveBlobInfo"
                                    : L"Windows.Gaming.XboxLive.Storage.GameSaveContainerInfo",
                                    q->blobs ? "GameSaveBlobInfo" : "GameSaveContainerInfo", info_destroy )))
        {
            free( name );
            break;
        }
        if (q->blobs)
        {
            object_add( &info->obj, blob_info_vtbl, &IID_IGameSaveBlobInfo );
            info->size = fd.nFileSizeLow;
        }
        else
        {
            WCHAR *sub = path_join( q->dir, fd.cFileName ), *display = sub ? path_join( sub, L".display" ) : NULL;
            BYTE *text;
            DWORD n;

            object_add( &info->obj, container_info_vtbl, &IID_IGameSaveContainerInfo );
            if (sub) info->size = dir_size( sub, &info->modified );
            if (!info->modified.dwLowDateTime && !info->modified.dwHighDateTime) info->modified = fd.ftLastWriteTime;
            if (display && read_file( display, &text, &n ))
            {
                info->display = str_dupn( (char *)text, n );
                free( text );
            }
            free( display );
            free( sub );
        }
        info->name = name;
        vector_view_add( view, (IInspectable *)&info->obj.vtbl[0] );
    } while (FindNextFileW( h, &fd ));
    FindClose( h );
    free( pattern );
    return view;
}

static HRESULT query_get( void *iface, UINT32 start, UINT32 max, void **out )
{
    struct query *q = query_from( iface );
    struct vector_view *view = query_run( q, start, max );

    if (!out) return E_POINTER;
    if (!view) return E_OUTOFMEMORY;
    TRACE( "%s(prefix %s, %u, %u) -> %u\n", q->blobs ? "GetBlobInfoAsync" : "GetContainerInfoAsync",
           debugstr_a(q->prefix), start, max, view->count );
    if (q->blobs)
        return complete_result( &IID_AsyncOp_BlobInfoGetResult, L"Windows.Gaming.XboxLive.Storage.GameSaveBlobInfoGetResult",
                                &IID_IGameSaveBlobInfoGetResult, "GetBlobInfoAsync", GS_OK,
                                (IInspectable *)&view->obj.vtbl[0], out );
    return complete_result( &IID_AsyncOp_ContainerInfoGetResult,
                            L"Windows.Gaming.XboxLive.Storage.GameSaveContainerInfoGetResult",
                            &IID_IGameSaveContainerInfoGetResult, "GetContainerInfoAsync", GS_OK,
                            (IInspectable *)&view->obj.vtbl[0], out );
}
static HRESULT WINAPI query_GetInfoAsync( void *iface, void **out ) { return query_get( iface, 0, 0, out ); }
static HRESULT WINAPI query_GetInfoRangeAsync( void *iface, UINT32 start, UINT32 max, void **out )
{
    return query_get( iface, start, max, out );
}
static HRESULT WINAPI query_GetItemCountAsync( void *iface, void **out )
{
    struct vector_view *view = query_run( query_from( iface ), 0, 0 );
    UINT32 count = view ? view->count : 0;

    if (!out) return E_POINTER;
    if (view) obj_Release( &view->obj );
    TRACE( "GetItemCountAsync -> %u\n", count );
    return complete_number( &IID_AsyncOp_UINT32, R_I4, "GetItemCountAsync", count, out );
}
static const void *query_vtbl[] = { OBJ_METHODS(0), query_GetInfoAsync, query_GetInfoRangeAsync, query_GetItemCountAsync };

static HRESULT query_create( BOOL blobs, const WCHAR *dir, HSTRING prefix, void **out )
{
    struct query *q;

    if (!out) return E_POINTER;
    if (!(q = object_create( sizeof(*q), blobs ? L"Windows.Gaming.XboxLive.Storage.GameSaveBlobInfoQuery"
                             : L"Windows.Gaming.XboxLive.Storage.GameSaveContainerInfoQuery",
                             blobs ? "GameSaveBlobInfoQuery" : "GameSaveContainerInfoQuery", query_destroy )))
        return E_OUTOFMEMORY;
    object_add( &q->obj, query_vtbl, blobs ? &IID_IGameSaveBlobInfoQuery : &IID_IGameSaveContainerInfoQuery );
    q->blobs = blobs;
    q->dir = wcsdup( dir );
    q->prefix = prefix ? from_hstr( prefix ) : str_dup( "" );
    *out = &q->obj.vtbl[0];
    return S_OK;
}

/* ---- GameSaveContainer ---- */

static HRESULT WINAPI container_get_Name( void *iface, HSTRING *v ) { return to_hstr( container_from( iface )->name, v ); }
static HRESULT WINAPI container_get_Provider( void *iface, void **v )
{
    struct container *c = container_from( iface );
    if (!v) return E_POINTER;
    obj_AddRef( &c->provider->obj );
    *v = &c->provider->obj.vtbl[0];
    return S_OK;
}
static HRESULT WINAPI container_SubmitUpdatesAsync( void *iface, IInspectable *writes, IInspectable *deletes,
                                                    HSTRING display, void **out )
{
    return submit_updates( container_from( iface ), writes, &IID_IIterable_KVP_String_Buffer, deletes, display,
                           "SubmitUpdatesAsync", out );
}
static HRESULT WINAPI container_SubmitPropertySetUpdatesAsync( void *iface, IInspectable *writes, IInspectable *deletes,
                                                               HSTRING display, void **out )
{
    return submit_updates( container_from( iface ), writes, &IID_IIterable_KVP_String_Object, deletes, display,
                           "SubmitPropertySetUpdatesAsync", out );
}
static HRESULT WINAPI container_ReadAsync( void *iface, IInspectable *blobs, void **out )
{
    struct container *c = container_from( iface );
    struct submit s = { c, GS_OK };
    IInspectable *iterable = NULL;

    if (!out) return E_POINTER;
    if (blobs && SUCCEEDED(IInspectable_QueryInterface( blobs, &IID_IIterable_KVP_String_Buffer, (void **)&iterable )))
    {
        for_each_pair( iterable, read_into, &s );
        IInspectable_Release( iterable );
    }
    TRACE( "GameSaveContainer(%s).ReadAsync: %u read\n", debugstr_a(c->name), s.written );
    return complete_operation( "ReadAsync", s.status, out );
}
static HRESULT WINAPI container_GetAsync( void *iface, IInspectable *names, void **out )
{
    struct container *c = container_from( iface );
    struct get g = { c, blob_map_create(), GS_OK };

    if (!out) return E_POINTER;
    if (!g.map) return E_OUTOFMEMORY;
    for_each_string( names, get_blob, &g );
    TRACE( "GameSaveContainer(%s).GetAsync: %u blobs, status %#x\n", debugstr_a(c->name), g.map->count, g.status );
    if (g.status)
    {
        obj_Release( &g.map->obj );
        g.map = NULL;
    }
    return complete_result( &IID_AsyncOp_BlobGetResult, L"Windows.Gaming.XboxLive.Storage.GameSaveBlobGetResult",
                            &IID_IGameSaveBlobGetResult, "GetAsync", g.status,
                            g.map ? (IInspectable *)&g.map->obj.vtbl[0] : NULL, out );
}
static HRESULT WINAPI container_CreateBlobInfoQuery( void *iface, HSTRING prefix, void **out )
{
    return query_create( TRUE, container_from( iface )->dir, prefix, out );
}

static const void *container_vtbl[] =
{
    OBJ_METHODS(0), container_get_Name, container_get_Provider, container_SubmitUpdatesAsync, container_ReadAsync,
    container_GetAsync, container_SubmitPropertySetUpdatesAsync, container_CreateBlobInfoQuery
};

/* ---- GameSaveProvider ---- */

static HRESULT WINAPI provider_get_User( void *iface, IInspectable **v )
{
    struct provider *p = provider_from( iface );
    if (!v) return E_POINTER;
    if ((*v = p->user)) IInspectable_AddRef( p->user );
    return S_OK;
}
static HRESULT WINAPI provider_CreateContainer( void *iface, HSTRING name, void **out )
{
    struct provider *p = provider_from( iface );
    struct container *c;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!WindowsGetStringLen( name )) return E_INVALIDARG;
    if (!(c = object_create( sizeof(*c), L"Windows.Gaming.XboxLive.Storage.GameSaveContainer", "GameSaveContainer",
                             container_destroy )))
        return E_OUTOFMEMORY;
    object_add( &c->obj, container_vtbl, &IID_IGameSaveContainer );
    obj_AddRef( &p->obj );
    c->provider = p;
    c->name = from_hstr( name );
    c->dir = container_dir( p, c->name );
    TRACE( "GameSaveProvider.CreateContainer(%s)\n", debugstr_a(c->name) );
    *out = &c->obj.vtbl[0];
    return S_OK;
}
static HRESULT WINAPI provider_DeleteContainerAsync( void *iface, HSTRING name, void **out )
{
    struct provider *p = provider_from( iface );
    char *n = from_hstr( name );
    WCHAR *dir = n ? container_dir( p, n ) : NULL, *pattern = dir ? path_join( dir, L"*" ) : NULL;
    WIN32_FIND_DATAW fd;
    INT32 status = GS_OK;
    HANDLE h;

    if (!out) return E_POINTER;
    if (pattern && (h = FindFirstFileW( pattern, &fd )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            WCHAR *file;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if ((file = path_join( dir, fd.cFileName ))) DeleteFileW( file );
            free( file );
        } while (FindNextFileW( h, &fd ));
        FindClose( h );
    }
    if (!dir || !RemoveDirectoryW( dir )) status = GS_INVALID_CONTAINER_NAME;
    TRACE( "GameSaveProvider.DeleteContainerAsync(%s)\n", debugstr_a(n) );
    if (!status) cloud_container_deleted( p->cloud, n );
    free( pattern );
    free( dir );
    free( n );
    return complete_operation( "DeleteContainerAsync", status, out );
}
static HRESULT WINAPI provider_CreateContainerInfoQuery( void *iface, void **out )
{
    return query_create( FALSE, provider_from( iface )->dir, NULL, out );
}
static HRESULT WINAPI provider_CreateContainerInfoQueryWithName( void *iface, HSTRING prefix, void **out )
{
    return query_create( FALSE, provider_from( iface )->dir, prefix, out );
}
static HRESULT WINAPI provider_GetRemainingBytesInQuotaAsync( void *iface, void **out )
{
    struct provider *p = provider_from( iface );
    WCHAR *pattern = path_join( p->dir, L"*" );
    WIN32_FIND_DATAW fd;
    INT64 used = 0;
    HANDLE h;

    if (!out) return E_POINTER;
    if (pattern && (h = FindFirstFileW( pattern, &fd )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            WCHAR *sub;
            if (fd.cFileName[0] == '.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if ((sub = path_join( p->dir, fd.cFileName ))) used += dir_size( sub, NULL );
            free( sub );
        } while (FindNextFileW( h, &fd ));
        FindClose( h );
    }
    free( pattern );
    TRACE( "GetRemainingBytesInQuotaAsync -> %I64d\n", GS_QUOTA - used );
    return complete_number( &IID_AsyncOp_INT64, R_I8, "GetRemainingBytesInQuotaAsync", GS_QUOTA - used, out );
}
static HRESULT WINAPI provider_get_ContainersChangedSinceLastSync( void *iface, void **v )
{
    struct vector_view *view;

    if (!v) return E_POINTER;
    /* nothing is synced from anywhere else */
    if (!(view = vector_view_create( &IID_IVectorView_String, &IID_IIterable_String, &IID_IIterator_String,
                                     L"Windows.Foundation.Collections.IVectorView`1<String>" )))
        return E_OUTOFMEMORY;
    *v = &view->obj.vtbl[0];
    return S_OK;
}

static const void *provider_vtbl[] =
{
    OBJ_METHODS(0), provider_get_User, provider_CreateContainer, provider_DeleteContainerAsync,
    provider_CreateContainerInfoQuery, provider_CreateContainerInfoQueryWithName,
    provider_GetRemainingBytesInQuotaAsync, provider_get_ContainersChangedSinceLastSync
};

/* ---- the factory ---- */

static HRESULT get_for_user( IInspectable *user, HSTRING scid, const char *name, void **out )
{
    char *id = from_hstr( scid ), *xid;
    WCHAR base[MAX_PATH], *sub;
    struct provider *p;
    INT32 status = GS_OK;
    DWORD n;

    if (!out) return E_POINTER;
    account_init();
    sign_in_silently();
    xid = account_field( &account.xid );
    n = GetEnvironmentVariableW( L"LOCALAPPDATA", base, MAX_PATH );
    if (!n || n >= MAX_PATH - 64) wcscpy( base, L"C:\\users\\Public" );
    wcscat( base, L"\\wineuwp\\gamesave" );
    if (!(p = object_create( sizeof(*p), L"Windows.Gaming.XboxLive.Storage.GameSaveProvider", "GameSaveProvider",
                             provider_destroy )))
    {
        free( id );
        free( xid );
        return E_OUTOFMEMORY;
    }
    object_add( &p->obj, provider_vtbl, &IID_IGameSaveProvider );
    if ((p->user = user)) IInspectable_AddRef( user );
    n = wcslen( base ) + strlen( S(id) ) + strlen( *S(xid) ? xid : "offline" ) + 3;
    if ((sub = malloc( n * sizeof(WCHAR) )))
    {
        swprintf( sub, n, L"%s\\%S\\%S", base, S(id), *S(xid) ? xid : "offline" );
        make_dirs( sub );
        p->dir = sub;
        /* down before the title reads anything; uploads of what is newer here */
        if ((p->cloud = cloud_base( id, xid ))) cloud_sync( p->cloud, p->dir );
    }
    TRACE( "GameSaveProvider.%s(scid %s) -> %s\n", name, debugstr_a(id), debugstr_w(p->dir) );
    free( id );
    free( xid );
    return complete_result( &IID_AsyncOp_ProviderGetResult, L"Windows.Gaming.XboxLive.Storage.GameSaveProviderGetResult",
                            &IID_IGameSaveProviderGetResult, name, status, (IInspectable *)&p->obj.vtbl[0], out );
}

static HRESULT WINAPI statics_GetForUserAsync( void *iface, IInspectable *user, HSTRING scid, void **out )
{
    return get_for_user( user, scid, "GetForUserAsync", out );
}
static HRESULT WINAPI statics_GetSyncOnDemandForUserAsync( void *iface, IInspectable *user, HSTRING scid, void **out )
{
    return get_for_user( user, scid, "GetSyncOnDemandForUserAsync", out );
}
static HRESULT WINAPI gs_ActivateInstance( void *iface, IInspectable **out )
{
    if (out) *out = NULL;
    return E_NOTIMPL;
}

static const void *gs_activation_vtbl[] = { OBJ_METHODS(0), gs_ActivateInstance };
static const void *gs_statics_vtbl[] = { OBJ_METHODS(1), statics_GetForUserAsync, statics_GetSyncOnDemandForUserAsync };

static struct object gs_factory;

IActivationFactory *game_save_provider_factory( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;

    if (InitOnceBeginInitialize( &once, 0, &pending, NULL ) && pending)
    {
        object_init_static( &gs_factory, L"Windows.Gaming.XboxLive.Storage.GameSaveProvider", "GameSaveProvider" );
        object_add( &gs_factory, gs_activation_vtbl, &IID_IActivationFactory );
        object_add( &gs_factory, gs_statics_vtbl, &IID_IGameSaveProviderStatics );
        InitOnceComplete( &once, 0, NULL );
    }
    return (IActivationFactory *)&gs_factory.vtbl[0];
}

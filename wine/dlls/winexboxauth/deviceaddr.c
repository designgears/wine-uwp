/*
 * Windows.Networking.XboxLive.XboxLiveDeviceAddress: this device's secure device address
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
 * On Windows the snapshot is an opaque blob from the Xbox Live networking stack (Teredo and IPsec security
 * associations), which titles put into their multiplayer session documents. Gears of War 4 asks for the local
 * address before it creates any game, campaign included ("Failed to create game", 0x80040154). There is no such
 * stack here, so the local address is a random blob kept for the life of the process: titles only store it, compare
 * it and hand it back. Peer-to-peer connections between two such addresses will not work.
 *
 * Slots (Windows.Networking.winmd, ./winmd.py --sig):
 *   IXboxLiveDeviceAddress {f5bbd279}: 6/7 SnapshotChanged 8 GetSnapshotAsBase64 9 GetSnapshotAsBuffer
 *     10 GetSnapshotAsBytes(fill array, out written) 11 Compare 12 IsValid 13 IsLocal 14 NetworkAccessKind
 *   IXboxLiveDeviceAddressStatics {5954a819}: 6 CreateFromSnapshotBase64 7 CreateFromSnapshotBuffer
 *     8 CreateFromSnapshotBytes 9 GetLocal 10 MaxSnapshotBytesSize
 */

#include "private.h"
#include <wincrypt.h>

WINE_DEFAULT_DEBUG_CHANNEL(xboxauth);

static const GUID IID_IXboxLiveDeviceAddress = {0xf5bbd279,0x3c86,0x4b57,{0xa3,0x1a,0xb9,0x46,0x24,0x08,0xfd,0x01}};
static const GUID IID_IXboxLiveDeviceAddressStatics = {0x5954a819,0x4a79,0x4931,{0x82,0x7c,0x7f,0x50,0x3e,0x96,0x32,0x63}};

#define SNAPSHOT_SIZE 64
#define MAX_SNAPSHOT 1024

static const WCHAR class_name[] = L"Windows.Networking.XboxLive.XboxLiveDeviceAddress";

struct address
{
    struct object obj;
    BYTE *bytes;
    UINT32 size;
};

static BYTE local_snapshot[SNAPSHOT_SIZE];

static struct address *address_from( void *iface ) { return CONTAINING_RECORD( OBJ_AT(iface, 0), struct address, obj ); }
static void address_destroy( struct object *obj ) { free( CONTAINING_RECORD( obj, struct address, obj )->bytes ); }

static BOOL is_local( struct address *a )
{
    return a->size == SNAPSHOT_SIZE && !memcmp( a->bytes, local_snapshot, SNAPSHOT_SIZE );
}

static HRESULT WINAPI address_add_SnapshotChanged( void *iface, IUnknown *handler, INT64 *token )
{
    if (!token) return E_POINTER;
    *token = 1; /* the address never changes */
    return S_OK;
}
static HRESULT WINAPI address_remove_SnapshotChanged( void *iface, INT64 token )
{
    return S_OK;
}
static HRESULT WINAPI address_GetSnapshotAsBase64( void *iface, HSTRING *v )
{
    struct address *a = address_from( iface );
    DWORD n = 0;
    char *text;
    HRESULT hr;

    if (!v) return E_POINTER;
    CryptBinaryToStringA( a->bytes, a->size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &n );
    if (!(text = malloc( n ))) return E_OUTOFMEMORY;
    CryptBinaryToStringA( a->bytes, a->size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, text, &n );
    hr = to_hstr( text, v );
    free( text );
    return hr;
}
static HRESULT WINAPI address_GetSnapshotAsBuffer( void *iface, IInspectable **v )
{
    struct address *a = address_from( iface );
    BYTE *copy;

    if (!v) return E_POINTER;
    if (!(copy = malloc( a->size ))) return E_OUTOFMEMORY;
    memcpy( copy, a->bytes, a->size );
    return (*v = buffer_create( copy, a->size )) ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI address_GetSnapshotAsBytes( void *iface, UINT32 length, BYTE *buffer, UINT32 *written )
{
    struct address *a = address_from( iface );

    if (!written || (length && !buffer)) return E_POINTER;
    *written = 0;
    if (length < a->size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    memcpy( buffer, a->bytes, a->size );
    *written = a->size;
    return S_OK;
}
static HRESULT WINAPI address_Compare( void *iface, IInspectable *other, INT32 *v )
{
    struct address *a = address_from( iface ), *b;
    int c;

    if (!v || !other) return E_POINTER;
    b = address_from( other );
    if (!(c = memcmp( a->bytes, b->bytes, min( a->size, b->size ) ))) c = (int)a->size - (int)b->size;
    *v = c < 0 ? -1 : c > 0;
    return S_OK;
}
static HRESULT WINAPI address_get_IsValid( void *iface, boolean *v )
{
    if (!v) return E_POINTER;
    *v = address_from( iface )->size != 0;
    return S_OK;
}
static HRESULT WINAPI address_get_IsLocal( void *iface, boolean *v )
{
    if (!v) return E_POINTER;
    *v = is_local( address_from( iface ) );
    return S_OK;
}
static HRESULT WINAPI address_get_NetworkAccessKind( void *iface, INT32 *v )
{
    if (!v) return E_POINTER;
    *v = 0; /* XboxLiveNetworkAccessKind_Open */
    return S_OK;
}

static const void *address_vtbl[] =
{
    OBJ_METHODS(0), address_add_SnapshotChanged, address_remove_SnapshotChanged, address_GetSnapshotAsBase64,
    address_GetSnapshotAsBuffer, address_GetSnapshotAsBytes, address_Compare, address_get_IsValid,
    address_get_IsLocal, address_get_NetworkAccessKind
};

/* copies bytes */
static HRESULT address_create( const BYTE *bytes, UINT32 size, const char *how, IInspectable **out )
{
    struct address *a;

    if (!out) return E_POINTER;
    *out = NULL;
    if (size > MAX_SNAPSHOT) return E_INVALIDARG;
    if (!(a = object_create( sizeof(*a), class_name, "XboxLiveDeviceAddress", address_destroy ))) return E_OUTOFMEMORY;
    object_add( &a->obj, address_vtbl, &IID_IXboxLiveDeviceAddress );
    if (size && !(a->bytes = malloc( size )))
    {
        obj_Release( &a->obj );
        return E_OUTOFMEMORY;
    }
    if (size) memcpy( a->bytes, bytes, size );
    a->size = size;
    TRACE( "XboxLiveDeviceAddress.%s: %u bytes%s\n", how, size, is_local( a ) ? " (local)" : "" );
    *out = (IInspectable *)&a->obj.vtbl[0];
    return S_OK;
}

static HRESULT WINAPI statics_CreateFromSnapshotBase64( void *iface, HSTRING base64, IInspectable **out )
{
    char *text = from_hstr( base64 );
    DWORD n = 0;
    BYTE *bytes = NULL;
    HRESULT hr = E_INVALIDARG;

    if (text && CryptStringToBinaryA( text, 0, CRYPT_STRING_BASE64, NULL, &n, NULL, NULL ) && (bytes = malloc( n ))
        && CryptStringToBinaryA( text, 0, CRYPT_STRING_BASE64, bytes, &n, NULL, NULL ))
        hr = address_create( bytes, n, "CreateFromSnapshotBase64", out );
    free( bytes );
    free( text );
    return hr;
}
static HRESULT WINAPI statics_CreateFromSnapshotBuffer( void *iface, IInspectable *buffer, IInspectable **out )
{
    UINT32 length, capacity;
    BYTE *bytes = buffer_bytes( buffer, &length, &capacity );

    if (!bytes && length) return E_INVALIDARG;
    return address_create( bytes, length, "CreateFromSnapshotBuffer", out );
}
static HRESULT WINAPI statics_CreateFromSnapshotBytes( void *iface, UINT32 length, BYTE *bytes, IInspectable **out )
{
    if (length && !bytes) return E_POINTER;
    return address_create( bytes, length, "CreateFromSnapshotBytes", out );
}
static HRESULT WINAPI statics_GetLocal( void *iface, IInspectable **out )
{
    return address_create( local_snapshot, SNAPSHOT_SIZE, "GetLocal", out );
}
static HRESULT WINAPI statics_get_MaxSnapshotBytesSize( void *iface, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = MAX_SNAPSHOT;
    return S_OK;
}

static HRESULT WINAPI da_ActivateInstance( void *iface, IInspectable **out )
{
    if (out) *out = NULL;
    return E_NOTIMPL;
}

static const void *da_activation_vtbl[] = { OBJ_METHODS(0), da_ActivateInstance };
static const void *da_statics_vtbl[] =
{
    OBJ_METHODS(1), statics_CreateFromSnapshotBase64, statics_CreateFromSnapshotBuffer, statics_CreateFromSnapshotBytes,
    statics_GetLocal, statics_get_MaxSnapshotBytesSize
};

static struct object da_factory;

IActivationFactory *device_address_factory( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;

    if (InitOnceBeginInitialize( &once, 0, &pending, NULL ) && pending)
    {
        BCryptGenRandom( NULL, local_snapshot, sizeof(local_snapshot), BCRYPT_USE_SYSTEM_PREFERRED_RNG );
        object_init_static( &da_factory, class_name, "XboxLiveDeviceAddress" );
        object_add( &da_factory, da_activation_vtbl, &IID_IActivationFactory );
        object_add( &da_factory, da_statics_vtbl, &IID_IXboxLiveDeviceAddressStatics );
        InitOnceComplete( &once, 0, NULL );
    }
    return (IActivationFactory *)&da_factory.vtbl[0];
}

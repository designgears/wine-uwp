/*
 * appxinstall: SHA-256 and AES-128-XTS decryption
 *
 * Wine's bcrypt has no XTS (its XTS_AES entry is empty), so XTS is built here on AES-ECB: each 16-byte block of a
 * data unit is decrypted as D_k1(C ^ T) ^ T, T starting at E_k2(tweak) and multiplied by x in GF(2^128) per block.
 * Data units are whole blocks (512 bytes for EAppx, 4096 for XVD pages), so no ciphertext stealing is needed.
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

#include <stdlib.h>
#include <string.h>

#include "appxinstall_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxinstall);

HRESULT sha256( const void *data, ULONG size, BYTE digest[32] )
{
    NTSTATUS status = BCryptHash( BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (UCHAR *)data, size, digest, 32 );
    return status ? HRESULT_FROM_NT( status ) : S_OK;
}

struct xts
{
    BCRYPT_KEY_HANDLE data_key;
    BCRYPT_KEY_HANDLE tweak_key;
};

static HRESULT ecb_key( const BYTE *secret, BCRYPT_KEY_HANDLE *key )
{
    NTSTATUS status;

    if ((status = BCryptGenerateSymmetricKey( BCRYPT_AES_ECB_ALG_HANDLE, key, NULL, 0, (UCHAR *)secret, 16, 0 )))
        return HRESULT_FROM_NT( status );
    return S_OK;
}

/* key: 32 bytes, the data key then the tweak key, as IEEE 1619 and OpenSSL's EVP_aes_128_xts take them */
HRESULT xts_create( const BYTE key[32], struct xts **out )
{
    struct xts *xts;
    HRESULT hr;

    if (!(xts = calloc( 1, sizeof(*xts) ))) return E_OUTOFMEMORY;
    if (FAILED(hr = ecb_key( key, &xts->data_key )) || FAILED(hr = ecb_key( key + 16, &xts->tweak_key )))
    {
        xts_destroy( xts );
        return hr;
    }
    *out = xts;
    return S_OK;
}

void xts_destroy( struct xts *xts )
{
    if (!xts) return;
    if (xts->data_key) BCryptDestroyKey( xts->data_key );
    if (xts->tweak_key) BCryptDestroyKey( xts->tweak_key );
    free( xts );
}

/* T * x in GF(2^128), little-endian bytes as IEEE 1619 orders them */
static void gf_double( BYTE t[16] )
{
    BYTE carry = 0, next;
    unsigned int i;

    for (i = 0; i < 16; i++)
    {
        next = t[i] >> 7;
        t[i] = (t[i] << 1) | carry;
        carry = next;
    }
    if (carry) t[0] ^= 0x87;
}

/* decrypts one data unit of size bytes (a multiple of 16) in place, with the 16-byte tweak given */
HRESULT xts_decrypt_unit( struct xts *xts, BYTE *data, ULONG size, const BYTE tweak[16] )
{
    BYTE t[16], *ts;
    ULONG done, i, j;
    NTSTATUS status;

    if (size % 16) return E_INVALIDARG;
    if (!(ts = malloc( size ))) return E_OUTOFMEMORY;
    if ((status = BCryptEncrypt( xts->tweak_key, (UCHAR *)tweak, 16, NULL, NULL, 0, t, 16, &done, 0 )))
    {
        free( ts );
        return HRESULT_FROM_NT( status );
    }
    for (i = 0; i < size; i += 16)
    {
        memcpy( ts + i, t, 16 );
        for (j = 0; j < 16; j++) data[i + j] ^= t[j];
        gf_double( t );
    }
    status = BCryptDecrypt( xts->data_key, data, size, NULL, NULL, 0, data, size, &done, 0 );
    if (!status)
        for (i = 0; i < size; i++) data[i] ^= ts[i];
    free( ts );
    return status ? HRESULT_FROM_NT( status ) : S_OK;
}

/* The IEEE 1619 XTS-AES-128 vectors 1 and 2, decrypted; run once when the first key is made. */
HRESULT xts_self_test(void)
{
    static const BYTE key1[32] = { 0 };
    static const BYTE ct1[32] =
    {
        0x91,0x7c,0xf6,0x9e,0xbd,0x68,0xb2,0xec,0x9b,0x9f,0xe9,0xa3,0xea,0xdd,0xa6,0x92,
        0xcd,0x43,0xd2,0xf5,0x95,0x98,0xed,0x85,0x8c,0x02,0xc2,0x65,0x2f,0xbf,0x92,0x2e,
    };
    static const BYTE key2[32] =
    {
        0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
        0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,
    };
    static const BYTE tweak2[16] = { 0x33,0x33,0x33,0x33,0x33 };
    static const BYTE ct2[32] =
    {
        0xc4,0x54,0x18,0x5e,0x6a,0x16,0x93,0x6e,0x39,0x33,0x40,0x38,0xac,0xef,0x83,0x8b,
        0xfb,0x18,0x6f,0xff,0x74,0x80,0xad,0xc4,0x28,0x93,0x82,0xec,0xd6,0xd3,0x94,0xf0,
    };
    static const BYTE tweak1[16] = { 0 };
    BYTE buf[32], pt2[32];
    struct xts *xts;
    HRESULT hr;

    memset( pt2, 0x44, sizeof(pt2) );
    if (FAILED(hr = xts_create( key1, &xts ))) return hr;
    memcpy( buf, ct1, 32 );
    hr = xts_decrypt_unit( xts, buf, 32, tweak1 );
    xts_destroy( xts );
    if (FAILED(hr)) return hr;
    if (memcmp( buf, key1, 32 )) { ERR( "XTS vector 1 failed\n" ); return E_FAIL; }

    if (FAILED(hr = xts_create( key2, &xts ))) return hr;
    memcpy( buf, ct2, 32 );
    hr = xts_decrypt_unit( xts, buf, 32, tweak2 );
    xts_destroy( xts );
    if (FAILED(hr)) return hr;
    if (memcmp( buf, pt2, 32 )) { ERR( "XTS vector 2 failed\n" ); return E_FAIL; }
    return S_OK;
}

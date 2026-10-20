/*
 * Windows.Security.Cryptography.Certificates.Certificate
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
 * Windows.Web.Http wraps every server certificate of every HTTPS response in a Certificate, made with
 * ICertificateFactory::CreateCertificate when the request is sent. Windows registers the class in certenroll.dll, which
 * a harvested Windows image does not have: activation failed and the client silently closed the connection, so every
 * HTTPS request through HttpClient died with no error anywhere (Shapr3D's sign-in code exchange stopped there).
 * The certificate is real: crypt32 parses the DER, and subject, issuer, validity, serial, hashes and the blob come from
 * it. Built from a bare DER blob, it never has a private key. Chain building and key usages are not implemented.
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"
#include "initguid.h"
#include "winstring.h"
#include "objbase.h"
#include "activation.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_Storage_Streams
#define WIDL_using_Windows_Security_Cryptography
#define WIDL_using_Windows_Security_Cryptography_Certificates
#include "windows.foundation.h"
#include "windows.storage.streams.h"
#include "robuffer.h"
#include "windows.security.cryptography.h"
#include "windows.security.cryptography.certificates.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(certificates);

static HRESULT get_class_name( HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Security_Cryptography_Certificates_Certificate,
                                wcslen( RuntimeClass_Windows_Security_Cryptography_Certificates_Certificate ), name );
}

static HRESULT copy_bytes( const BYTE *data, DWORD size, UINT32 *value_size, BYTE **value )
{
    if (!value_size || !value) return E_POINTER;
    if (!(*value = CoTaskMemAlloc( size ? size : 1 ))) return E_OUTOFMEMORY;
    memcpy( *value, data, size );
    *value_size = size;
    return S_OK;
}

/* ---- Certificate ---- */

struct certificate
{
    ICertificate ICertificate_iface;
    LONG ref;
    const CERT_CONTEXT *context;
    HSTRING friendly_name;
};

static inline struct certificate *impl_from_ICertificate( ICertificate *iface )
{
    return CONTAINING_RECORD( iface, struct certificate, ICertificate_iface );
}

static HRESULT WINAPI certificate_QueryInterface( ICertificate *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ICertificate ))
    {
        *out = iface;
        ICertificate_AddRef( iface );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI certificate_AddRef( ICertificate *iface )
{
    return InterlockedIncrement( &impl_from_ICertificate( iface )->ref );
}

static ULONG WINAPI certificate_Release( ICertificate *iface )
{
    struct certificate *impl = impl_from_ICertificate( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        CertFreeCertificateContext( impl->context );
        WindowsDeleteString( impl->friendly_name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI certificate_GetIids( ICertificate *iface, ULONG *iid_count, IID **iids )
{
    if (!(*iids = CoTaskMemAlloc( sizeof(IID) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_ICertificate;
    *iid_count = 1;
    return S_OK;
}

static HRESULT WINAPI certificate_GetRuntimeClassName( ICertificate *iface, HSTRING *name ) { return get_class_name( name ); }
static HRESULT WINAPI certificate_GetTrustLevel( ICertificate *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI certificate_BuildChainAsync( ICertificate *iface, IIterable_Certificate *certificates, IInspectable **operation )
{
    FIXME( "iface %p, certificates %p, operation %p: not implemented.\n", iface, certificates, operation );
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI certificate_BuildChainWithParametersAsync( ICertificate *iface, IIterable_Certificate *certificates,
                                                                 IInspectable *parameters, IInspectable **operation )
{
    FIXME( "iface %p, certificates %p, parameters %p, operation %p: not implemented.\n", iface, certificates, parameters, operation );
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

/* CERT_INFO keeps the serial little-endian; the property has the certificate's own byte order */
static HRESULT WINAPI certificate_get_SerialNumber( ICertificate *iface, UINT32 *value_size, BYTE **value )
{
    const CRYPT_INTEGER_BLOB *serial = &impl_from_ICertificate( iface )->context->pCertInfo->SerialNumber;
    HRESULT hr;
    DWORD i;

    if (FAILED(hr = copy_bytes( serial->pbData, serial->cbData, value_size, value ))) return hr;
    for (i = 0; i < serial->cbData / 2; i++)
    {
        BYTE b = (*value)[i];
        (*value)[i] = (*value)[serial->cbData - 1 - i];
        (*value)[serial->cbData - 1 - i] = b;
    }
    return S_OK;
}

static HRESULT hash_value( struct certificate *impl, DWORD property, UINT32 *value_size, BYTE **value )
{
    BYTE hash[64];
    DWORD size = sizeof(hash);

    if (!CertGetCertificateContextProperty( impl->context, property, hash, &size )) return HRESULT_FROM_WIN32( GetLastError() );
    return copy_bytes( hash, size, value_size, value );
}

static HRESULT WINAPI certificate_GetHashValue( ICertificate *iface, UINT32 *value_size, BYTE **value )
{
    return hash_value( impl_from_ICertificate( iface ), CERT_SHA1_HASH_PROP_ID, value_size, value );
}

static HRESULT WINAPI certificate_GetHashValueWithAlgorithm( ICertificate *iface, HSTRING algorithm, UINT32 *value_size, BYTE **value )
{
    struct certificate *impl = impl_from_ICertificate( iface );
    const WCHAR *name = WindowsGetStringRawBuffer( algorithm, NULL );

    if (!wcsicmp( name, L"SHA1" )) return hash_value( impl, CERT_SHA1_HASH_PROP_ID, value_size, value );
    if (!wcsicmp( name, L"SHA256" ))
    {
        BYTE hash[32];
        DWORD size = sizeof(hash);

        if (!CryptHashCertificate( 0, CALG_SHA_256, 0, impl->context->pbCertEncoded, impl->context->cbCertEncoded, hash, &size ))
            return HRESULT_FROM_WIN32( GetLastError() );
        return copy_bytes( hash, size, value_size, value );
    }
    if (!wcsicmp( name, L"MD5" )) return hash_value( impl, CERT_MD5_HASH_PROP_ID, value_size, value );
    FIXME( "unsupported hash algorithm %s\n", debugstr_w(name) );
    return E_INVALIDARG;
}

/* a new IBuffer holding a copy of the encoded certificate, from CryptographicBuffer */
static HRESULT WINAPI certificate_GetCertificateBlob( ICertificate *iface, IBuffer **value )
{
    struct certificate *impl = impl_from_ICertificate( iface );
    ICryptographicBufferStatics *statics;
    HSTRING class;
    HRESULT hr;

    if (FAILED(hr = WindowsCreateString( RuntimeClass_Windows_Security_Cryptography_CryptographicBuffer,
                                         wcslen( RuntimeClass_Windows_Security_Cryptography_CryptographicBuffer ), &class )))
        return hr;
    hr = RoGetActivationFactory( class, &IID_ICryptographicBufferStatics, (void **)&statics );
    WindowsDeleteString( class );
    if (FAILED(hr)) return hr;
    hr = ICryptographicBufferStatics_CreateFromByteArray( statics, impl->context->cbCertEncoded,
                                                          impl->context->pbCertEncoded, value );
    ICryptographicBufferStatics_Release( statics );
    return hr;
}

static HRESULT certificate_name( struct certificate *impl, DWORD flags, HSTRING *value )
{
    WCHAR buffer[512];
    DWORD len = CertGetNameStringW( impl->context, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, NULL, buffer, ARRAY_SIZE(buffer) );

    return WindowsCreateString( buffer, len ? len - 1 : 0, value );
}

static HRESULT WINAPI certificate_get_Subject( ICertificate *iface, HSTRING *value )
{
    return certificate_name( impl_from_ICertificate( iface ), 0, value );
}

static HRESULT WINAPI certificate_get_Issuer( ICertificate *iface, HSTRING *value )
{
    return certificate_name( impl_from_ICertificate( iface ), CERT_NAME_ISSUER_FLAG, value );
}

static HRESULT WINAPI certificate_get_HasPrivateKey( ICertificate *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI certificate_get_IsStronglyProtected( ICertificate *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

/* a DateTime is a FILETIME */
static HRESULT WINAPI certificate_get_ValidFrom( ICertificate *iface, DateTime *value )
{
    const FILETIME *time = &impl_from_ICertificate( iface )->context->pCertInfo->NotBefore;
    value->UniversalTime = ((INT64)time->dwHighDateTime << 32) | time->dwLowDateTime;
    return S_OK;
}

static HRESULT WINAPI certificate_get_ValidTo( ICertificate *iface, DateTime *value )
{
    const FILETIME *time = &impl_from_ICertificate( iface )->context->pCertInfo->NotAfter;
    value->UniversalTime = ((INT64)time->dwHighDateTime << 32) | time->dwLowDateTime;
    return S_OK;
}

static HRESULT WINAPI certificate_get_EnhancedKeyUsages( ICertificate *iface, IVectorView_HSTRING **value )
{
    FIXME( "iface %p, value %p: not implemented.\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI certificate_put_FriendlyName( ICertificate *iface, HSTRING value )
{
    struct certificate *impl = impl_from_ICertificate( iface );
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->friendly_name );
    impl->friendly_name = copy;
    return S_OK;
}

static HRESULT WINAPI certificate_get_FriendlyName( ICertificate *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_ICertificate( iface )->friendly_name, value );
}

static const struct ICertificateVtbl certificate_vtbl =
{
    certificate_QueryInterface,
    certificate_AddRef,
    certificate_Release,
    certificate_GetIids,
    certificate_GetRuntimeClassName,
    certificate_GetTrustLevel,
    certificate_BuildChainAsync,
    certificate_BuildChainWithParametersAsync,
    certificate_get_SerialNumber,
    certificate_GetHashValue,
    certificate_GetHashValueWithAlgorithm,
    certificate_GetCertificateBlob,
    certificate_get_Subject,
    certificate_get_Issuer,
    certificate_get_HasPrivateKey,
    certificate_get_IsStronglyProtected,
    certificate_get_ValidFrom,
    certificate_get_ValidTo,
    certificate_get_EnhancedKeyUsages,
    certificate_put_FriendlyName,
    certificate_get_FriendlyName,
};

/* ---- the factory ---- */

static struct
{
    IActivationFactory IActivationFactory_iface;
    ICertificateFactory ICertificateFactory_iface;
} factory;

static HRESULT factory_query( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &factory.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ICertificateFactory )) *out = &factory.ICertificateFactory_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static HRESULT get_iids( ULONG *iid_count, IID **iids )
{
    if (!(*iids = CoTaskMemAlloc( sizeof(IID) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_ICertificateFactory;
    *iid_count = 1;
    return S_OK;
}

static HRESULT WINAPI activation_QueryInterface( IActivationFactory *iface, REFIID iid, void **out ) { return factory_query( iid, out ); }
static ULONG WINAPI activation_AddRef( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI activation_Release( IActivationFactory *iface ) { return 1; }
static HRESULT WINAPI activation_GetIids( IActivationFactory *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI activation_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name ) { return get_class_name( name ); }
static HRESULT WINAPI activation_GetTrustLevel( IActivationFactory *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI activation_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;   /* no default constructor */
}

static const struct IActivationFactoryVtbl activation_vtbl =
{
    activation_QueryInterface,
    activation_AddRef,
    activation_Release,
    activation_GetIids,
    activation_GetRuntimeClassName,
    activation_GetTrustLevel,
    activation_ActivateInstance,
};

static HRESULT WINAPI certificate_factory_QueryInterface( ICertificateFactory *iface, REFIID iid, void **out ) { return factory_query( iid, out ); }
static ULONG WINAPI certificate_factory_AddRef( ICertificateFactory *iface ) { return 2; }
static ULONG WINAPI certificate_factory_Release( ICertificateFactory *iface ) { return 1; }
static HRESULT WINAPI certificate_factory_GetIids( ICertificateFactory *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI certificate_factory_GetRuntimeClassName( ICertificateFactory *iface, HSTRING *name ) { return get_class_name( name ); }
static HRESULT WINAPI certificate_factory_GetTrustLevel( ICertificateFactory *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI certificate_factory_CreateCertificate( ICertificateFactory *iface, IBuffer *buffer, ICertificate **value )
{
    IBufferByteAccess *access;
    struct certificate *impl;
    const CERT_CONTEXT *context;
    UINT32 size;
    BYTE *data;
    HRESULT hr;

    TRACE( "iface %p, buffer %p, value %p.\n", iface, buffer, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!buffer) return E_INVALIDARG;
    if (FAILED(hr = IBuffer_get_Length( buffer, &size ))) return hr;
    if (FAILED(hr = IBuffer_QueryInterface( buffer, &IID_IBufferByteAccess, (void **)&access ))) return hr;
    hr = IBufferByteAccess_Buffer( access, &data );
    IBufferByteAccess_Release( access );
    if (FAILED(hr)) return hr;
    if (!(context = CertCreateCertificateContext( X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, data, size )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        WARN( "%u bytes do not parse as a certificate: %#lx\n", size, hr );
        return hr;
    }
    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        CertFreeCertificateContext( context );
        return E_OUTOFMEMORY;
    }
    impl->ICertificate_iface.lpVtbl = &certificate_vtbl;
    impl->ref = 1;
    impl->context = context;
    *value = &impl->ICertificate_iface;
    return S_OK;
}

static const struct ICertificateFactoryVtbl certificate_factory_vtbl =
{
    certificate_factory_QueryInterface,
    certificate_factory_AddRef,
    certificate_factory_Release,
    certificate_factory_GetIids,
    certificate_factory_GetRuntimeClassName,
    certificate_factory_GetTrustLevel,
    certificate_factory_CreateCertificate,
};

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **out )
{
    TRACE( "classid %s, out %p.\n", debugstr_hstring( classid ), out );

    factory.IActivationFactory_iface.lpVtbl = &activation_vtbl;
    factory.ICertificateFactory_iface.lpVtbl = &certificate_factory_vtbl;

    *out = NULL;
    if (wcscmp( WindowsGetStringRawBuffer( classid, NULL ), RuntimeClass_Windows_Security_Cryptography_Certificates_Certificate ))
        return CLASS_E_CLASSNOTAVAILABLE;
    *out = &factory.IActivationFactory_iface;
    return S_OK;
}

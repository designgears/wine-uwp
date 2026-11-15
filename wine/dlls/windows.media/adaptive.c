/* WinRT Windows.Media.Streaming.Adaptive.AdaptiveMediaSource: no adaptive streaming here
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
#include "mferror.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

/* The player plays files through a Media Foundation source reader; it cannot play DASH or HLS manifests. Windows
 * reports a stream it cannot use through the creation result's status, and apps fall back on that. The class was
 * missing entirely, and the Microsoft Store's game pages, whose header plays a DASH trailer, threw while building the
 * header and lost their Get button with it. So: no content type is supported, and every creation completes at once
 * with UnsupportedManifestContentType. */

/* Windows.Media.winmd */
static const GUID IID_IAdaptiveMediaSourceStatics_ =
    {0x50a6bd5d,0x66ef,0x4cd3,{0x95,0x79,0x9e,0x66,0x05,0x07,0xdc,0x3f}};
static const GUID IID_IAdaptiveMediaSourceCreationResult_ =
    {0x4686b6b2,0x800f,0x4e31,{0x90,0x93,0x76,0xd4,0x78,0x20,0x13,0xe7}};
static const GUID IID_IAdaptiveMediaSourceCreationResult2_ =
    {0x1c3243bf,0x1c44,0x404b,{0xa2,0x01,0xdf,0x45,0xac,0x78,0x98,0xe8}};
/* IAsyncOperation<AdaptiveMediaSourceCreationResult> and its Completed handler (piid.py --signature) */
static const GUID IID_IAsyncOperation_CreationResult =
    {0xbe0bcc1d,0xd606,0x59d2,{0xb2,0xf9,0xff,0x20,0x45,0x43,0xda,0x12}};

enum { UnsupportedManifestContentType = 3 };   /* AdaptiveMediaSourceCreationStatus */

/* ---------------------------------------------------------------- the creation result */

struct creation_result
{
    const void *vtbl;           /* IAdaptiveMediaSourceCreationResult */
    const void *result2_vtbl;   /* IAdaptiveMediaSourceCreationResult2 */
    LONG ref;
    INT32 status;
};

static struct creation_result *result_from( void *iface ) { return CONTAINING_RECORD( iface, struct creation_result, vtbl ); }
static struct creation_result *result_from2( void *iface ) { return CONTAINING_RECORD( iface, struct creation_result, result2_vtbl ); }

static HRESULT WINAPI result_QueryInterface( void *iface, REFIID iid, void **out )
{
    struct creation_result *impl = result_from( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAdaptiveMediaSourceCreationResult_ ))
    {
        InterlockedIncrement( &impl->ref );
        *out = &impl->vtbl;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IAdaptiveMediaSourceCreationResult2_ ))
    {
        InterlockedIncrement( &impl->ref );
        *out = &impl->result2_vtbl;
        return S_OK;
    }
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI result_AddRef( void *iface )
{
    return InterlockedIncrement( &result_from( iface )->ref );
}
static ULONG WINAPI result_Release( void *iface )
{
    struct creation_result *impl = result_from( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}
static HRESULT WINAPI result_GetIids( void *iface, ULONG *count, IID **iids )
{
    if (count) *count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}
static HRESULT WINAPI result_GetRuntimeClassName( void *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Media.Streaming.Adaptive.AdaptiveMediaSourceCreationResult";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}
static HRESULT WINAPI result_GetTrustLevel( void *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI result_get_Status( void *iface, INT32 *value )
{
    if (!value) return E_POINTER;
    *value = result_from( iface )->status;
    return S_OK;
}
static HRESULT WINAPI result_get_null( void *iface, IInspectable **value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}
static const void *result_vtbl[] =
{
    result_QueryInterface, result_AddRef, result_Release, result_GetIids, result_GetRuntimeClassName,
    result_GetTrustLevel, result_get_Status, result_get_null /* MediaSource */, result_get_null /* HttpResponseMessage */,
};

static HRESULT WINAPI result2_QueryInterface( void *iface, REFIID iid, void **out )
{
    return result_QueryInterface( &result_from2( iface )->vtbl, iid, out );
}
static ULONG WINAPI result2_AddRef( void *iface ) { return result_AddRef( &result_from2( iface )->vtbl ); }
static ULONG WINAPI result2_Release( void *iface ) { return result_Release( &result_from2( iface )->vtbl ); }
static HRESULT WINAPI result2_get_ExtendedError( void *iface, HRESULT *value )
{
    if (!value) return E_POINTER;
    *value = MF_E_UNSUPPORTED_FORMAT;
    return S_OK;
}
static const void *result2_vtbl[] =
{
    result2_QueryInterface, result2_AddRef, result2_Release, result_GetIids, result_GetRuntimeClassName,
    result_GetTrustLevel, result2_get_ExtendedError,
};

/* ---------------------------------------------------------------- the operation, complete from the start */

struct creation_operation
{
    const void *vtbl;           /* IAsyncOperation<AdaptiveMediaSourceCreationResult> */
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    void *result;
    IUnknown *handler;          /* IAsyncOperationCompletedHandler<AdaptiveMediaSourceCreationResult> */
};

static struct creation_operation *operation_from( void *iface ) { return CONTAINING_RECORD( iface, struct creation_operation, vtbl ); }

static HRESULT WINAPI operation_QueryInterface( void *iface, REFIID iid, void **out )
{
    struct creation_operation *impl = operation_from( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncOperation_CreationResult ))
    {
        InterlockedIncrement( &impl->ref );
        *out = &impl->vtbl;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        InterlockedIncrement( &impl->ref );
        *out = &impl->IAsyncInfo_iface;
        return S_OK;
    }
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI operation_AddRef( void *iface )
{
    return InterlockedIncrement( &operation_from( iface )->ref );
}
static ULONG WINAPI operation_Release( void *iface )
{
    struct creation_operation *impl = operation_from( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->handler) IUnknown_Release( impl->handler );
        result_Release( impl->result );
        free( impl );
    }
    return ref;
}
static HRESULT WINAPI operation_GetRuntimeClassName( void *iface, HSTRING *name )
{
    static const WCHAR class_name[] =
        L"Windows.Foundation.IAsyncOperation`1<Windows.Media.Streaming.Adaptive.AdaptiveMediaSourceCreationResult>";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}
/* a delegate is IUnknown and Invoke: slot 3 */
static HRESULT WINAPI operation_put_Completed( void *iface, IUnknown *handler )
{
    struct creation_operation *impl = operation_from( iface );

    TRACE( "iface %p, handler %p.\n", iface, handler );
    if (!handler) return E_POINTER;
    if (impl->handler) return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    impl->handler = handler;
    IUnknown_AddRef( handler );
    return ((HRESULT (WINAPI *)( IUnknown *, void *, AsyncStatus ))(*(void ***)handler)[3])( handler, iface, Completed );
}
static HRESULT WINAPI operation_get_Completed( void *iface, IUnknown **handler )
{
    struct creation_operation *impl = operation_from( iface );

    if (!handler) return E_POINTER;
    if ((*handler = impl->handler)) IUnknown_AddRef( *handler );
    return S_OK;
}
static HRESULT WINAPI operation_GetResults( void *iface, void **results )
{
    struct creation_operation *impl = operation_from( iface );

    if (!results) return E_POINTER;
    result_AddRef( impl->result );
    *results = impl->result;
    return S_OK;
}
static const void *operation_vtbl[] =
{
    operation_QueryInterface, operation_AddRef, operation_Release, result_GetIids, operation_GetRuntimeClassName,
    result_GetTrustLevel, operation_put_Completed, operation_get_Completed, operation_GetResults,
};

static struct creation_operation *impl_from_IAsyncInfo( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct creation_operation, IAsyncInfo_iface );
}
static HRESULT WINAPI info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    return operation_QueryInterface( &impl_from_IAsyncInfo( iface )->vtbl, iid, out );
}
static ULONG WINAPI info_AddRef( IAsyncInfo *iface ) { return operation_AddRef( &impl_from_IAsyncInfo( iface )->vtbl ); }
static ULONG WINAPI info_Release( IAsyncInfo *iface ) { return operation_Release( &impl_from_IAsyncInfo( iface )->vtbl ); }
static HRESULT WINAPI info_GetIids( IAsyncInfo *iface, ULONG *count, IID **iids ) { return result_GetIids( iface, count, iids ); }
static HRESULT WINAPI info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *name )
{
    return operation_GetRuntimeClassName( &impl_from_IAsyncInfo( iface )->vtbl, name );
}
static HRESULT WINAPI info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level ) { return result_GetTrustLevel( iface, level ); }
static HRESULT WINAPI info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    if (!id) return E_POINTER;
    *id = 1;
    return S_OK;
}
static HRESULT WINAPI info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    if (!status) return E_POINTER;
    *status = Completed;
    return S_OK;
}
static HRESULT WINAPI info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error )
{
    if (!error) return E_POINTER;
    *error = S_OK;
    return S_OK;
}
static HRESULT WINAPI info_Cancel( IAsyncInfo *iface ) { return S_OK; }
static HRESULT WINAPI info_Close( IAsyncInfo *iface ) { return S_OK; }
static const IAsyncInfoVtbl info_vtbl =
{
    info_QueryInterface, info_AddRef, info_Release, info_GetIids, info_GetRuntimeClassName, info_GetTrustLevel,
    info_get_Id, info_get_Status, info_get_ErrorCode, info_Cancel, info_Close,
};

static HRESULT unsupported_creation( void **operation )
{
    struct creation_operation *impl;
    struct creation_result *result;

    if (!operation) return E_POINTER;
    if (!(result = calloc( 1, sizeof(*result) ))) return E_OUTOFMEMORY;
    result->vtbl = result_vtbl;
    result->result2_vtbl = result2_vtbl;
    result->ref = 1;
    result->status = UnsupportedManifestContentType;
    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        free( result );
        return E_OUTOFMEMORY;
    }
    impl->vtbl = operation_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &info_vtbl;
    impl->ref = 1;
    impl->result = &result->vtbl;
    *operation = &impl->vtbl;
    return S_OK;
}

/* ---------------------------------------------------------------- the statics */

struct adaptive_statics
{
    IActivationFactory IActivationFactory_iface;
    const void *statics_vtbl;   /* IAdaptiveMediaSourceStatics */
    LONG ref;
};

static struct adaptive_statics *statics_from_factory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct adaptive_statics, IActivationFactory_iface );
}
static struct adaptive_statics *statics_from( void *iface )
{
    return CONTAINING_RECORD( iface, struct adaptive_statics, statics_vtbl );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct adaptive_statics *impl = statics_from_factory( iface );

    TRACE( "iface %p, iid %s.\n", iface, debugstr_guid( iid ) );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef( (*out = &impl->IActivationFactory_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IAdaptiveMediaSourceStatics_ ))
    {
        IActivationFactory_AddRef( &impl->IActivationFactory_iface );
        *out = &impl->statics_vtbl;
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &statics_from_factory( iface )->ref );
}
static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &statics_from_factory( iface )->ref );
}
static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    return result_GetIids( iface, count, iids );
}
static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Media.Streaming.Adaptive.AdaptiveMediaSource";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}
static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    return result_GetTrustLevel( iface, level );
}
static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    return E_NOTIMPL;
}
static const IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface, factory_AddRef, factory_Release, factory_GetIids, factory_GetRuntimeClassName,
    factory_GetTrustLevel, factory_ActivateInstance,
};

static HRESULT WINAPI statics_QueryInterface( void *iface, REFIID iid, void **out )
{
    return factory_QueryInterface( &statics_from( iface )->IActivationFactory_iface, iid, out );
}
static ULONG WINAPI statics_AddRef( void *iface ) { return factory_AddRef( &statics_from( iface )->IActivationFactory_iface ); }
static ULONG WINAPI statics_Release( void *iface ) { return factory_Release( &statics_from( iface )->IActivationFactory_iface ); }
static HRESULT WINAPI statics_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return factory_GetRuntimeClassName( &statics_from( iface )->IActivationFactory_iface, name );
}
static HRESULT WINAPI statics_IsContentTypeSupported( void *iface, HSTRING type, boolean *value )
{
    TRACE( "iface %p, type %s: no.\n", iface, debugstr_hstring( type ) );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}
static HRESULT WINAPI statics_CreateFromUriAsync( void *iface, IInspectable *uri, void **operation )
{
    TRACE( "iface %p, uri %p: unsupported.\n", iface, uri );
    return unsupported_creation( operation );
}
static HRESULT WINAPI statics_CreateFromUriWithDownloaderAsync( void *iface, IInspectable *uri, IInspectable *client,
                                                                void **operation )
{
    TRACE( "iface %p, uri %p, client %p: unsupported.\n", iface, uri, client );
    return unsupported_creation( operation );
}
static HRESULT WINAPI statics_CreateFromStreamAsync( void *iface, IInspectable *stream, IInspectable *uri, HSTRING type,
                                                     void **operation )
{
    TRACE( "iface %p, stream %p, type %s: unsupported.\n", iface, stream, debugstr_hstring( type ) );
    return unsupported_creation( operation );
}
static HRESULT WINAPI statics_CreateFromStreamWithDownloaderAsync( void *iface, IInspectable *stream, IInspectable *uri,
                                                                   HSTRING type, IInspectable *client, void **operation )
{
    TRACE( "iface %p, stream %p, type %s, client %p: unsupported.\n", iface, stream, debugstr_hstring( type ), client );
    return unsupported_creation( operation );
}
static const void *statics_vtbl[] =
{
    statics_QueryInterface, statics_AddRef, statics_Release, result_GetIids, statics_GetRuntimeClassName,
    result_GetTrustLevel, statics_IsContentTypeSupported, statics_CreateFromUriAsync,
    statics_CreateFromUriWithDownloaderAsync, statics_CreateFromStreamAsync, statics_CreateFromStreamWithDownloaderAsync,
};

static struct adaptive_statics adaptive_statics =
{
    {&factory_vtbl},
    statics_vtbl,
    1,
};

IActivationFactory *adaptive_media_source_factory = &adaptive_statics.IActivationFactory_iface;

/*
 * Windows.Media.Audio spatial audio configuration
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
 * Windows registers SpatialAudioDeviceConfiguration and SpatialAudioFormatSubtype in Windows.Media.Devices.dll. Apps
 * written for the platform ask for them with C++/WinRT's .as<>(), which throws on E_NOINTERFACE: Wwise in Solitaire
 * did, on its own audio thread, and the process died. There is no spatial audio here, which these say plainly:
 * IsSpatialAudioSupported is false, no format is supported, the active and default formats are empty strings (a valid
 * WinRT empty string, not an object anyone dereferences), and setting one fails. ConfigurationChanged never fires but
 * registering is accepted.
 *
 * The SpatialAudioFormatSubtype strings appear in no shipping Windows DLL to copy them from, so they are our own: an
 * app only compares them with each other and with ActiveSpatialAudioFormat, which is empty here, so being distinct and
 * stable is all that matters. Anything passing one to a real audio API would break that reasoning.
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "activation.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Media_Audio
#include "windows.foundation.h"
/* main.c defines the Foundation IIDs; the spatial-audio ones are defined here */
#include "initguid.h"
#include "windows.media.audio.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mmdevapi);

static HRESULT get_iids( ULONG *iid_count, IID **iids )
{
    if (iid_count) *iid_count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT make_string( const WCHAR *text, HSTRING *out )
{
    if (!out) return E_POINTER;
    return WindowsCreateString( text, wcslen( text ), out );
}

static BOOL is_base_iid( REFIID iid )
{
    return IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject );
}

/* ---- SpatialAudioDeviceConfiguration ---- */

struct configuration
{
    ISpatialAudioDeviceConfiguration ISpatialAudioDeviceConfiguration_iface;
    LONG ref;
    HSTRING device_id;
};

static inline struct configuration *impl_from_ISpatialAudioDeviceConfiguration( ISpatialAudioDeviceConfiguration *iface )
{
    return CONTAINING_RECORD( iface, struct configuration, ISpatialAudioDeviceConfiguration_iface );
}

static HRESULT WINAPI configuration_QueryInterface( ISpatialAudioDeviceConfiguration *iface, REFIID iid, void **out )
{
    if (!is_base_iid( iid ) && !IsEqualGUID( iid, &IID_ISpatialAudioDeviceConfiguration ))
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    *out = iface;
    ISpatialAudioDeviceConfiguration_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI configuration_AddRef( ISpatialAudioDeviceConfiguration *iface )
{
    return InterlockedIncrement( &impl_from_ISpatialAudioDeviceConfiguration( iface )->ref );
}

static ULONG WINAPI configuration_Release( ISpatialAudioDeviceConfiguration *iface )
{
    struct configuration *impl = impl_from_ISpatialAudioDeviceConfiguration( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->device_id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI configuration_GetIids( ISpatialAudioDeviceConfiguration *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); }
static HRESULT WINAPI configuration_GetRuntimeClassName( ISpatialAudioDeviceConfiguration *iface, HSTRING *name )
{
    return make_string( RuntimeClass_Windows_Media_Audio_SpatialAudioDeviceConfiguration, name );
}
static HRESULT WINAPI configuration_GetTrustLevel( ISpatialAudioDeviceConfiguration *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI configuration_get_DeviceId( ISpatialAudioDeviceConfiguration *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_ISpatialAudioDeviceConfiguration( iface )->device_id, value );
}

static HRESULT WINAPI configuration_get_IsSpatialAudioSupported( ISpatialAudioDeviceConfiguration *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI configuration_IsSpatialAudioFormatSupported( ISpatialAudioDeviceConfiguration *iface, HSTRING subtype, boolean *result )
{
    TRACE( "iface %p, subtype %s, result %p.\n", iface, debugstr_hstring( subtype ), result );
    if (!result) return E_POINTER;
    *result = FALSE;
    return S_OK;
}

static HRESULT WINAPI configuration_get_ActiveSpatialAudioFormat( ISpatialAudioDeviceConfiguration *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI configuration_get_DefaultSpatialAudioFormat( ISpatialAudioDeviceConfiguration *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI configuration_SetDefaultSpatialAudioFormatAsync( ISpatialAudioDeviceConfiguration *iface, HSTRING subtype,
                                                                      IAsyncOperation_SetDefaultSpatialAudioFormatResult **operation )
{
    FIXME( "iface %p, subtype %s, operation %p: no spatial audio to configure.\n", iface, debugstr_hstring( subtype ), operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI configuration_add_ConfigurationChanged( ISpatialAudioDeviceConfiguration *iface,
        ITypedEventHandler_SpatialAudioDeviceConfiguration_IInspectable *handler, EventRegistrationToken *token )
{
    static LONG64 next_token;

    TRACE( "iface %p, handler %p, token %p: never raised.\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = InterlockedIncrement64( &next_token );
    return S_OK;
}

static HRESULT WINAPI configuration_remove_ConfigurationChanged( ISpatialAudioDeviceConfiguration *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static const struct ISpatialAudioDeviceConfigurationVtbl configuration_vtbl =
{
    configuration_QueryInterface,
    configuration_AddRef,
    configuration_Release,
    /* IInspectable methods */
    configuration_GetIids,
    configuration_GetRuntimeClassName,
    configuration_GetTrustLevel,
    /* ISpatialAudioDeviceConfiguration methods */
    configuration_get_DeviceId,
    configuration_get_IsSpatialAudioSupported,
    configuration_IsSpatialAudioFormatSupported,
    configuration_get_ActiveSpatialAudioFormat,
    configuration_get_DefaultSpatialAudioFormat,
    configuration_SetDefaultSpatialAudioFormatAsync,
    configuration_add_ConfigurationChanged,
    configuration_remove_ConfigurationChanged,
};

/* ---- the two static factories ---- */

static struct
{
    IActivationFactory IActivationFactory_iface;
    ISpatialAudioDeviceConfigurationStatics ISpatialAudioDeviceConfigurationStatics_iface;
} configuration_factory;

static struct
{
    IActivationFactory IActivationFactory_iface;
    ISpatialAudioFormatSubtypeStatics ISpatialAudioFormatSubtypeStatics_iface;
    ISpatialAudioFormatSubtypeStatics2 ISpatialAudioFormatSubtypeStatics2_iface;
} subtype_factory;

static HRESULT no_interface( REFIID iid, void **out )
{
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT configuration_factory_query( REFIID iid, void **out )
{
    if (is_base_iid( iid ) || IsEqualGUID( iid, &IID_IActivationFactory )) *out = &configuration_factory.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ISpatialAudioDeviceConfigurationStatics ))
        *out = &configuration_factory.ISpatialAudioDeviceConfigurationStatics_iface;
    else return no_interface( iid, out );
    return S_OK;
}

static HRESULT subtype_factory_query( REFIID iid, void **out )
{
    if (is_base_iid( iid ) || IsEqualGUID( iid, &IID_IActivationFactory )) *out = &subtype_factory.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ISpatialAudioFormatSubtypeStatics )) *out = &subtype_factory.ISpatialAudioFormatSubtypeStatics_iface;
    else if (IsEqualGUID( iid, &IID_ISpatialAudioFormatSubtypeStatics2 )) *out = &subtype_factory.ISpatialAudioFormatSubtypeStatics2_iface;
    else return no_interface( iid, out );
    return S_OK;
}

/* the IUnknown/IInspectable part of a static object, forwarding QueryInterface to query */
#define STATIC_IINSPECTABLE( prefix, type, query, class ) \
    static HRESULT WINAPI prefix##_QueryInterface( type *iface, REFIID iid, void **out ) { return query( iid, out ); } \
    static ULONG WINAPI prefix##_AddRef( type *iface ) { return 2; } \
    static ULONG WINAPI prefix##_Release( type *iface ) { return 1; } \
    static HRESULT WINAPI prefix##_GetIids( type *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); } \
    static HRESULT WINAPI prefix##_GetRuntimeClassName( type *iface, HSTRING *name ) { return make_string( class, name ); } \
    static HRESULT WINAPI prefix##_GetTrustLevel( type *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

STATIC_IINSPECTABLE( configuration_factory, IActivationFactory, configuration_factory_query,
                     RuntimeClass_Windows_Media_Audio_SpatialAudioDeviceConfiguration )
STATIC_IINSPECTABLE( configuration_statics, ISpatialAudioDeviceConfigurationStatics, configuration_factory_query,
                     RuntimeClass_Windows_Media_Audio_SpatialAudioDeviceConfiguration )
STATIC_IINSPECTABLE( subtype_factory, IActivationFactory, subtype_factory_query, RuntimeClass_Windows_Media_Audio_SpatialAudioFormatSubtype )
STATIC_IINSPECTABLE( subtype_statics, ISpatialAudioFormatSubtypeStatics, subtype_factory_query,
                     RuntimeClass_Windows_Media_Audio_SpatialAudioFormatSubtype )
STATIC_IINSPECTABLE( subtype_statics2, ISpatialAudioFormatSubtypeStatics2, subtype_factory_query,
                     RuntimeClass_Windows_Media_Audio_SpatialAudioFormatSubtype )

static HRESULT WINAPI static_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;   /* static classes */
}

static const struct IActivationFactoryVtbl configuration_factory_vtbl =
{
    configuration_factory_QueryInterface,
    configuration_factory_AddRef,
    configuration_factory_Release,
    configuration_factory_GetIids,
    configuration_factory_GetRuntimeClassName,
    configuration_factory_GetTrustLevel,
    static_ActivateInstance,
};

static HRESULT WINAPI configuration_statics_GetForDeviceId( ISpatialAudioDeviceConfigurationStatics *iface, HSTRING device_id,
                                                            ISpatialAudioDeviceConfiguration **result )
{
    struct configuration *impl;
    HRESULT hr;

    TRACE( "iface %p, device_id %s, result %p.\n", iface, debugstr_hstring( device_id ), result );

    if (!result) return E_POINTER;
    *result = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ISpatialAudioDeviceConfiguration_iface.lpVtbl = &configuration_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( device_id, &impl->device_id )))
    {
        free( impl );
        return hr;
    }
    *result = &impl->ISpatialAudioDeviceConfiguration_iface;
    return S_OK;
}

static const struct ISpatialAudioDeviceConfigurationStaticsVtbl configuration_statics_vtbl =
{
    configuration_statics_QueryInterface,
    configuration_statics_AddRef,
    configuration_statics_Release,
    configuration_statics_GetIids,
    configuration_statics_GetRuntimeClassName,
    configuration_statics_GetTrustLevel,
    configuration_statics_GetForDeviceId,
};

static const struct IActivationFactoryVtbl subtype_factory_vtbl =
{
    subtype_factory_QueryInterface,
    subtype_factory_AddRef,
    subtype_factory_Release,
    subtype_factory_GetIids,
    subtype_factory_GetRuntimeClassName,
    subtype_factory_GetTrustLevel,
    static_ActivateInstance,
};

#define SUBTYPE( name, statics ) \
    static HRESULT WINAPI subtype_get_##name( statics *iface, HSTRING *value ) \
    { \
        return make_string( L"Windows.Spatial.Sound." #name, value ); \
    }

SUBTYPE( WindowsSonic, ISpatialAudioFormatSubtypeStatics )
SUBTYPE( DolbyAtmosForHeadphones, ISpatialAudioFormatSubtypeStatics )
SUBTYPE( DolbyAtmosForHomeTheater, ISpatialAudioFormatSubtypeStatics )
SUBTYPE( DolbyAtmosForSpeakers, ISpatialAudioFormatSubtypeStatics )
SUBTYPE( DTSHeadphoneX, ISpatialAudioFormatSubtypeStatics )
SUBTYPE( DTSXUltra, ISpatialAudioFormatSubtypeStatics )
SUBTYPE( DTSXForHomeTheater, ISpatialAudioFormatSubtypeStatics2 )

static const struct ISpatialAudioFormatSubtypeStaticsVtbl subtype_statics_vtbl =
{
    subtype_statics_QueryInterface,
    subtype_statics_AddRef,
    subtype_statics_Release,
    subtype_statics_GetIids,
    subtype_statics_GetRuntimeClassName,
    subtype_statics_GetTrustLevel,
    subtype_get_WindowsSonic,
    subtype_get_DolbyAtmosForHeadphones,
    subtype_get_DolbyAtmosForHomeTheater,
    subtype_get_DolbyAtmosForSpeakers,
    subtype_get_DTSHeadphoneX,
    subtype_get_DTSXUltra,
};

static const struct ISpatialAudioFormatSubtypeStatics2Vtbl subtype_statics2_vtbl =
{
    subtype_statics2_QueryInterface,
    subtype_statics2_AddRef,
    subtype_statics2_Release,
    subtype_statics2_GetIids,
    subtype_statics2_GetRuntimeClassName,
    subtype_statics2_GetTrustLevel,
    subtype_get_DTSXForHomeTheater,
};

/* the factory for one of the spatial-audio classes, or NULL */
IActivationFactory *spatial_audio_factory( const WCHAR *name )
{
    configuration_factory.IActivationFactory_iface.lpVtbl = &configuration_factory_vtbl;
    configuration_factory.ISpatialAudioDeviceConfigurationStatics_iface.lpVtbl = &configuration_statics_vtbl;
    subtype_factory.IActivationFactory_iface.lpVtbl = &subtype_factory_vtbl;
    subtype_factory.ISpatialAudioFormatSubtypeStatics_iface.lpVtbl = &subtype_statics_vtbl;
    subtype_factory.ISpatialAudioFormatSubtypeStatics2_iface.lpVtbl = &subtype_statics2_vtbl;

    if (!wcscmp( name, RuntimeClass_Windows_Media_Audio_SpatialAudioDeviceConfiguration ))
        return &configuration_factory.IActivationFactory_iface;
    if (!wcscmp( name, RuntimeClass_Windows_Media_Audio_SpatialAudioFormatSubtype ))
        return &subtype_factory.IActivationFactory_iface;
    return NULL;
}

/* WinRT device classes for radios this machine does not have: ProximityDevice (NFC) and MobileBroadbandModem
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
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(winsock);

/* Each class's statics answer "no such device", as Windows does on a PC without the radio. The genuine DLLs need
 * driver stacks Wine does not have: Windows.Networking.Proximity.dll fails to load (ProximityRtapiPal.dll,
 * ProximityCommon.dll), and Windows.Networking.NetworkOperators.dll's MobileBroadbandModem.GetDefault delay-loads
 * the WWAN API and takes the process down when that fails. The Microsoft Store probes both at start-up (with the
 * camera selector and smart-card emulation) and rethrew the first failure on every click on a product, so no product
 * page opened. */

struct absent_device
{
    IActivationFactory IActivationFactory_iface;
    const void *statics_vtbl;
    LONG ref;
    const WCHAR *class_name;
    GUID statics_iid;
    const WCHAR *selector;
};

static struct absent_device *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct absent_device, IActivationFactory_iface );
}
static struct absent_device *impl_from_statics( void *iface )
{
    return CONTAINING_RECORD( iface, struct absent_device, statics_vtbl );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct absent_device *impl = impl_from_IActivationFactory( iface );

    TRACE( "%s, iid %s, out %p.\n", debugstr_w( impl->class_name ), debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef( (*out = &impl->IActivationFactory_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &impl->statics_iid ))
    {
        IActivationFactory_AddRef( &impl->IActivationFactory_iface );
        *out = &impl->statics_vtbl;
        return S_OK;
    }
    FIXME( "%s: %s not implemented, returning E_NOINTERFACE.\n", debugstr_w( impl->class_name ), debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory( iface )->ref );
}
static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory( iface )->ref );
}
static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}
static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    const WCHAR *class_name = impl_from_IActivationFactory( iface )->class_name;
    return WindowsCreateString( class_name, wcslen( class_name ), name );
}
static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* not activatable on Windows either: instances come from GetDefault and FromId */
    TRACE( "iface %p, instance %p.\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface, factory_AddRef, factory_Release,
    factory_GetIids, factory_GetRuntimeClassName, factory_GetTrustLevel,
    factory_ActivateInstance,
};

static HRESULT WINAPI statics_QueryInterface( void *iface, REFIID iid, void **out )
{
    return factory_QueryInterface( &impl_from_statics( iface )->IActivationFactory_iface, iid, out );
}
static ULONG WINAPI statics_AddRef( void *iface )
{
    return factory_AddRef( &impl_from_statics( iface )->IActivationFactory_iface );
}
static ULONG WINAPI statics_Release( void *iface )
{
    return factory_Release( &impl_from_statics( iface )->IActivationFactory_iface );
}
static HRESULT WINAPI statics_GetIids( void *iface, ULONG *count, IID **iids )
{
    return factory_GetIids( &impl_from_statics( iface )->IActivationFactory_iface, count, iids );
}
static HRESULT WINAPI statics_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return factory_GetRuntimeClassName( &impl_from_statics( iface )->IActivationFactory_iface, name );
}
static HRESULT WINAPI statics_GetTrustLevel( void *iface, TrustLevel *level )
{
    return factory_GetTrustLevel( &impl_from_statics( iface )->IActivationFactory_iface, level );
}
static HRESULT WINAPI statics_GetDeviceSelector( void *iface, HSTRING *value )
{
    const WCHAR *selector = impl_from_statics( iface )->selector;

    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsCreateString( selector, wcslen( selector ), value );
}
static HRESULT WINAPI statics_GetDefault( void *iface, IInspectable **value )
{
    TRACE( "%s: no device.\n", debugstr_w( impl_from_statics( iface )->class_name ) );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}
static HRESULT WINAPI statics_FromId( void *iface, HSTRING id, IInspectable **value )
{
    TRACE( "%s: no device %s.\n", debugstr_w( impl_from_statics( iface )->class_name ), debugstr_hstring( id ) );
    if (!value) return E_POINTER;
    *value = NULL;
    return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
}

/* IProximityDeviceStatics {914ba01d-f6e1-47c4-a14c-148a1903d0c6}: GetDeviceSelector, GetDefault, FromId */
static const void *proximity_statics_vtbl[] =
{
    statics_QueryInterface, statics_AddRef, statics_Release,
    statics_GetIids, statics_GetRuntimeClassName, statics_GetTrustLevel,
    statics_GetDeviceSelector, statics_GetDefault, statics_FromId,
};

/* IMobileBroadbandModemStatics {f99ed637-d6f1-4a78-8cbc-6421a65063c8}: GetDeviceSelector, FromId, GetDefault */
static const void *modem_statics_vtbl[] =
{
    statics_QueryInterface, statics_AddRef, statics_Release,
    statics_GetIids, statics_GetRuntimeClassName, statics_GetTrustLevel,
    statics_GetDeviceSelector, statics_FromId, statics_GetDefault,
};

static struct absent_device proximity_device =
{
    {&factory_vtbl}, proximity_statics_vtbl, 1,
    L"Windows.Networking.Proximity.ProximityDevice",
    { 0x914ba01d, 0xf6e1, 0x47c4, { 0xa1, 0x4c, 0x14, 0x8a, 0x19, 0x03, 0xd0, 0xc6 } },
    /* GUID_DEVINTERFACE_NFP */
    L"System.Devices.InterfaceClassGuid:=\"{FB3842CD-9E2A-4F83-8FCC-4B0761139AE9}\""
    L" AND System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True",
};

static struct absent_device mobile_broadband_modem =
{
    {&factory_vtbl}, modem_statics_vtbl, 1,
    L"Windows.Networking.NetworkOperators.MobileBroadbandModem",
    { 0xf99ed637, 0xd6f1, 0x4a78, { 0x8c, 0xbc, 0x64, 0x21, 0xa6, 0x50, 0x63, 0xc8 } },
    /* GUID_DEVINTERFACE_NET: whatever the query, it finds no modem here */
    L"System.Devices.InterfaceClassGuid:=\"{CAC88484-7515-4C03-82E6-71A87ABAC361}\""
    L" AND System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True",
};

IActivationFactory *proximity_device_factory = &proximity_device.IActivationFactory_iface;
IActivationFactory *mobile_broadband_modem_factory = &mobile_broadband_modem.IActivationFactory_iface;

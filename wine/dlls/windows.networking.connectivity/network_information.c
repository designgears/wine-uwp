/* WinRT Windows.Networking.Connectivity.NetworkInformation Implementation
 *
 * Copyright (C) 2024 Mohamad Al-Jaf
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
#include "initguid.h"
#include "netlistmgr.h"
#include "winsock2.h"
#include "ws2ipdef.h"
#include "iphlpapi.h"
#include "roapi.h"
#include "netioapi.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(connectivity);


struct network_information_statics
{
    IActivationFactory IActivationFactory_iface;
    INetworkInformationStatics INetworkInformationStatics_iface;
    LONG ref;
};

static inline struct network_information_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct network_information_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct network_information_statics *impl = impl_from_IActivationFactory( iface );

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

    if (IsEqualGUID( iid, &IID_INetworkInformationStatics ))
    {
        *out = &impl->INetworkInformationStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct network_information_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct network_information_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
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

struct connection_profile
{
    IConnectionProfile IConnectionProfile_iface;
    IConnectionProfile2 IConnectionProfile2_iface;
    LONG ref;

    INetworkListManager *network_list_manager;
};

static inline struct connection_profile *impl_from_IConnectionProfile( IConnectionProfile *iface )
{
    return CONTAINING_RECORD( iface, struct connection_profile, IConnectionProfile_iface );
}

static HRESULT WINAPI connection_profile_QueryInterface( IConnectionProfile *iface, REFIID iid, void **out )
{
    struct connection_profile *impl = impl_from_IConnectionProfile( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IConnectionProfile ))
    {
        *out = &impl->IConnectionProfile_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IConnectionProfile2 ))
    {
        *out = &impl->IConnectionProfile2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI connection_profile_AddRef( IConnectionProfile *iface )
{
    struct connection_profile *impl = impl_from_IConnectionProfile( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI connection_profile_Release( IConnectionProfile *iface )
{
    struct connection_profile *impl = impl_from_IConnectionProfile( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        INetworkListManager_Release( impl->network_list_manager );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI connection_profile_GetIids( IConnectionProfile *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI connection_profile_GetRuntimeClassName( IConnectionProfile *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI connection_profile_GetTrustLevel( IConnectionProfile *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI connection_profile_get_ProfileName( IConnectionProfile *iface, HSTRING *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI connection_profile_GetNetworkConnectivityLevel( IConnectionProfile *iface, NetworkConnectivityLevel *value )
{
    struct connection_profile *impl = impl_from_IConnectionProfile( iface );
    NetworkConnectivityLevel network_connectivity_level;
    NLM_CONNECTIVITY connectivity = 0xdeadbeef;
    HRESULT hr;

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_POINTER;
    if (FAILED(hr = INetworkListManager_GetConnectivity( impl->network_list_manager, &connectivity ))) return hr;

    if (connectivity == NLM_CONNECTIVITY_DISCONNECTED)
        network_connectivity_level = NetworkConnectivityLevel_None;
    else if (connectivity & ( NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET ))
        network_connectivity_level = NetworkConnectivityLevel_InternetAccess;
    else if (connectivity & ( NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_LOCALNETWORK | NLM_CONNECTIVITY_IPV4_NOTRAFFIC | NLM_CONNECTIVITY_IPV6_NOTRAFFIC ))
        network_connectivity_level = NetworkConnectivityLevel_LocalAccess;
    else
        network_connectivity_level = NetworkConnectivityLevel_ConstrainedInternetAccess;

    *value = network_connectivity_level;
    return S_OK;
}

static HRESULT WINAPI connection_profile_GetNetworkNames( IConnectionProfile *iface, IVectorView_HSTRING **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

/* Wine has no metering information: report an unrestricted, unmetered connection. Calculator's currency
 * converter asks for the cost before loading rates and treated the failure as no connection. */
static IConnectionCost2 connection_cost2;

static HRESULT WINAPI connection_cost_QueryInterface( IConnectionCost *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IConnectionCost ))
    {
        *out = iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IConnectionCost2 ))
    {
        *out = &connection_cost2;
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI connection_cost_AddRef( IConnectionCost *iface ) { return 2; }
static ULONG WINAPI connection_cost_Release( IConnectionCost *iface ) { return 1; }
static HRESULT WINAPI connection_cost_GetIids( IConnectionCost *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}
static HRESULT WINAPI connection_cost_GetRuntimeClassName( IConnectionCost *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Networking.Connectivity.ConnectionCost";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}
static HRESULT WINAPI connection_cost_GetTrustLevel( IConnectionCost *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI connection_cost_get_NetworkCostType( IConnectionCost *iface, NetworkCostType *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NetworkCostType_Unrestricted;
    return S_OK;
}
static HRESULT WINAPI connection_cost_get_Roaming( IConnectionCost *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}
static HRESULT WINAPI connection_cost_get_OverDataLimit( IConnectionCost *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}
static HRESULT WINAPI connection_cost_get_ApproachingDataLimit( IConnectionCost *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}
static const struct IConnectionCostVtbl connection_cost_vtbl =
{
    connection_cost_QueryInterface, connection_cost_AddRef, connection_cost_Release,
    connection_cost_GetIids, connection_cost_GetRuntimeClassName, connection_cost_GetTrustLevel,
    connection_cost_get_NetworkCostType, connection_cost_get_Roaming, connection_cost_get_OverDataLimit,
    connection_cost_get_ApproachingDataLimit,
};
static IConnectionCost connection_cost = { &connection_cost_vtbl };

static HRESULT WINAPI connection_cost2_QueryInterface( IConnectionCost2 *iface, REFIID iid, void **out )
{
    return connection_cost_QueryInterface( &connection_cost, iid, out );
}
static ULONG WINAPI connection_cost2_AddRef( IConnectionCost2 *iface ) { return 2; }
static ULONG WINAPI connection_cost2_Release( IConnectionCost2 *iface ) { return 1; }
static HRESULT WINAPI connection_cost2_GetIids( IConnectionCost2 *iface, ULONG *iid_count, IID **iids )
{
    return connection_cost_GetIids( &connection_cost, iid_count, iids );
}
static HRESULT WINAPI connection_cost2_GetRuntimeClassName( IConnectionCost2 *iface, HSTRING *class_name )
{
    return connection_cost_GetRuntimeClassName( &connection_cost, class_name );
}
static HRESULT WINAPI connection_cost2_GetTrustLevel( IConnectionCost2 *iface, TrustLevel *trust_level )
{
    return connection_cost_GetTrustLevel( &connection_cost, trust_level );
}
/* no Data Saver here: background data is never restricted */
static HRESULT WINAPI connection_cost2_get_BackgroundDataUsageRestricted( IConnectionCost2 *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}
static const struct IConnectionCost2Vtbl connection_cost2_vtbl =
{
    connection_cost2_QueryInterface, connection_cost2_AddRef, connection_cost2_Release,
    connection_cost2_GetIids, connection_cost2_GetRuntimeClassName, connection_cost2_GetTrustLevel,
    connection_cost2_get_BackgroundDataUsageRestricted,
};
static IConnectionCost2 connection_cost2 = { &connection_cost2_vtbl };

static HRESULT WINAPI connection_profile_GetConnectionCost( IConnectionProfile *iface, IConnectionCost **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = &connection_cost;
    return S_OK;
}

static HRESULT WINAPI connection_profile_GetDataPlanStatus( IConnectionProfile *iface, IDataPlanStatus **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

struct network_adapter
{
    INetworkAdapter INetworkAdapter_iface;
    LONG ref;

    MIB_IF_ROW2 row;
};

static inline struct network_adapter *impl_from_INetworkAdapter( INetworkAdapter *iface )
{
    return CONTAINING_RECORD( iface, struct network_adapter, INetworkAdapter_iface );
}

static HRESULT WINAPI network_adapter_QueryInterface( INetworkAdapter *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_INetworkAdapter ))
    {
        *out = iface;
        INetworkAdapter_AddRef( iface );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI network_adapter_AddRef( INetworkAdapter *iface )
{
    return InterlockedIncrement( &impl_from_INetworkAdapter( iface )->ref );
}

static ULONG WINAPI network_adapter_Release( INetworkAdapter *iface )
{
    struct network_adapter *impl = impl_from_INetworkAdapter( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI network_adapter_GetIids( INetworkAdapter *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI network_adapter_GetRuntimeClassName( INetworkAdapter *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Networking_Connectivity_NetworkAdapter,
                                wcslen( RuntimeClass_Windows_Networking_Connectivity_NetworkAdapter ), class_name );
}

static HRESULT WINAPI network_adapter_GetTrustLevel( INetworkAdapter *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI network_adapter_get_OutboundMaxBitsPerSecond( INetworkAdapter *iface, UINT64 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl_from_INetworkAdapter( iface )->row.TransmitLinkSpeed;
    return S_OK;
}

static HRESULT WINAPI network_adapter_get_InboundMaxBitsPerSecond( INetworkAdapter *iface, UINT64 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl_from_INetworkAdapter( iface )->row.ReceiveLinkSpeed;
    return S_OK;
}

static HRESULT WINAPI network_adapter_get_IanaInterfaceType( INetworkAdapter *iface, UINT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl_from_INetworkAdapter( iface )->row.Type;
    return S_OK;
}

static HRESULT WINAPI network_adapter_get_NetworkItem( INetworkAdapter *iface, INetworkItem **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI network_adapter_get_NetworkAdapterId( INetworkAdapter *iface, GUID *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl_from_INetworkAdapter( iface )->row.InterfaceGuid;
    return S_OK;
}

static HRESULT WINAPI network_adapter_GetConnectedProfileAsync( INetworkAdapter *iface, IAsyncOperation_ConnectionProfile **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static const struct INetworkAdapterVtbl network_adapter_vtbl =
{
    network_adapter_QueryInterface,
    network_adapter_AddRef,
    network_adapter_Release,
    /* IInspectable methods */
    network_adapter_GetIids,
    network_adapter_GetRuntimeClassName,
    network_adapter_GetTrustLevel,
    /* INetworkAdapter methods */
    network_adapter_get_OutboundMaxBitsPerSecond,
    network_adapter_get_InboundMaxBitsPerSecond,
    network_adapter_get_IanaInterfaceType,
    network_adapter_get_NetworkItem,
    network_adapter_get_NetworkAdapterId,
    network_adapter_GetConnectedProfileAsync,
};

/* The adapter the Internet is reached through: the interface of the best route to a public address. Apps read its
 * type and speed to tell a wired link from a metered one (Solitaire's reachability handler gave up on E_NOTIMPL here,
 * and every component waiting on reachability waited for ever). */
static HRESULT network_adapter_create( DWORD index, INetworkAdapter **value )
{
    struct network_adapter *impl;
    DWORD err;

    *value = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->INetworkAdapter_iface.lpVtbl = &network_adapter_vtbl;
    impl->ref = 1;
    impl->row.InterfaceIndex = index;
    if ((err = GetIfEntry2( &impl->row )))
    {
        free( impl );
        return HRESULT_FROM_WIN32( err );
    }
    *value = &impl->INetworkAdapter_iface;
    return S_OK;
}

static HRESULT WINAPI connection_profile_get_NetworkAdapter( IConnectionProfile *iface, INetworkAdapter **value )
{
    DWORD index, err;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;
    /* 8.8.8.8: a public address, and the same in either byte order */
    if ((err = GetBestInterface( 0x08080808, &index ))) return HRESULT_FROM_WIN32( err );
    return network_adapter_create( index, value );
}

static HRESULT WINAPI connection_profile_GetLocalUsage( IConnectionProfile *iface, DateTime start, DateTime end, IDataUsage **value )
{
    FIXME( "iface %p, start %I64d, end %I64d, value %p stub!\n", iface, start.UniversalTime, end.UniversalTime, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI connection_profile_GetLocalUsagePerRoamingStates( IConnectionProfile *iface, DateTime start, DateTime end, RoamingStates states, IDataUsage **value )
{
    FIXME( "iface %p, start %I64d, end %I64d, states %u, value %p stub!\n", iface, start.UniversalTime, end.UniversalTime, states, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI connection_profile_get_NetworkSecuritySettings( IConnectionProfile *iface, INetworkSecuritySettings **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static const struct IConnectionProfileVtbl connection_profile_vtbl =
{
    connection_profile_QueryInterface,
    connection_profile_AddRef,
    connection_profile_Release,
    /* IInspectable methods */
    connection_profile_GetIids,
    connection_profile_GetRuntimeClassName,
    connection_profile_GetTrustLevel,
    /* IConnectionProfile methods */
    connection_profile_get_ProfileName,
    connection_profile_GetNetworkConnectivityLevel,
    connection_profile_GetNetworkNames,
    connection_profile_GetConnectionCost,
    connection_profile_GetDataPlanStatus,
    connection_profile_get_NetworkAdapter,
    connection_profile_GetLocalUsage,
    connection_profile_GetLocalUsagePerRoamingStates,
    connection_profile_get_NetworkSecuritySettings,
};

static inline struct connection_profile *impl_from_IConnectionProfile2( IConnectionProfile2 *iface )
{
    return CONTAINING_RECORD( iface, struct connection_profile, IConnectionProfile2_iface );
}

static HRESULT WINAPI connection_profile2_QueryInterface( IConnectionProfile2 *iface, REFIID iid, void **out )
{
    struct connection_profile *impl = impl_from_IConnectionProfile2( iface );
    return IConnectionProfile_QueryInterface(&impl->IConnectionProfile_iface, iid, out);
}

static ULONG WINAPI connection_profile2_AddRef( IConnectionProfile2 *iface )
{
    struct connection_profile *impl = impl_from_IConnectionProfile2( iface );
    return IConnectionProfile_AddRef(&impl->IConnectionProfile_iface);
}

static ULONG WINAPI connection_profile2_Release( IConnectionProfile2 *iface )
{
    struct connection_profile *impl = impl_from_IConnectionProfile2( iface );
    return IConnectionProfile_Release(&impl->IConnectionProfile_iface);
}

static HRESULT WINAPI connection_profile2_GetIids( IConnectionProfile2 *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI connection_profile2_GetRuntimeClassName( IConnectionProfile2 *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI connection_profile2_GetTrustLevel( IConnectionProfile2 *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

HRESULT WINAPI connection_profile2_get_IsWwanConnectionProfile( IConnectionProfile2 *iface, boolean *value )
{
    TRACE( "iface %p, value %p: wired.\n", iface, value );
    *value = 0;
    return S_OK;
}

HRESULT WINAPI connection_profile2_get_IsWlanConnectionProfile( IConnectionProfile2 *iface, boolean *value )
{
    TRACE( "iface %p, value %p: wired.\n", iface, value );
    *value = 0;
    return S_OK;
}

HRESULT WINAPI connection_profile2_get_WwanConnectionProfileDetails( IConnectionProfile2 *iface, IWwanConnectionProfileDetails **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

HRESULT WINAPI connection_profile2_get_WlanConnectionProfileDetails( IConnectionProfile2 *iface, IWlanConnectionProfileDetails **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

HRESULT WINAPI connection_profile2_get_ServiceProviderGuid( IConnectionProfile2 *iface, IReference_GUID **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

/* a wired connection has no signal bars: Windows answers NULL. E_NOTIMPL made the Xbox app's network
 * check throw and show its offline page. */
HRESULT WINAPI connection_profile2_GetSignalBars( IConnectionProfile2 *iface, IReference_BYTE **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = NULL;
    return S_OK;
}

HRESULT WINAPI connection_profile2_GetDomainConnectivityLevel( IConnectionProfile2 *iface, DomainConnectivityLevel *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

HRESULT WINAPI connection_profile2_GetNetworkUsageAsync( IConnectionProfile2 *iface, DateTime time_start, DateTime time_end,
        DataUsageGranularity granularity, NetworkUsageStates states, IAsyncOperation_IVectorView_NetworkUsage **value )
{
    FIXME( "iface %p, time_start %I64d, time_end %I64d, granularity %d, states %d-%d, value %p stub!\n",
            iface, time_start.UniversalTime, time_end.UniversalTime, granularity, states.Roaming, states.Shared, value );
    return E_NOTIMPL;
}

HRESULT WINAPI connection_profile2_GetConnectivityIntervalsAsync( IConnectionProfile2 *iface, DateTime time_start, DateTime time_end,
        NetworkUsageStates states, IAsyncOperation_IVectorView_ConnectivityInterval **value )
{
    FIXME( "iface %p, time_start %I64d, time_end %I64d, states %d-%d, value %p stub!\n",
            iface, time_start.UniversalTime, time_end.UniversalTime, states.Roaming, states.Shared, value );
    return E_NOTIMPL;
}

static const struct IConnectionProfile2Vtbl connection_profile2_vtbl =
{
    connection_profile2_QueryInterface,
    connection_profile2_AddRef,
    connection_profile2_Release,
    /* IInspectable methods */
    connection_profile2_GetIids,
    connection_profile2_GetRuntimeClassName,
    connection_profile2_GetTrustLevel,
    /* IConnectionProfile2 methods */
    connection_profile2_get_IsWwanConnectionProfile,
    connection_profile2_get_IsWlanConnectionProfile,
    connection_profile2_get_WwanConnectionProfileDetails,
    connection_profile2_get_WlanConnectionProfileDetails,
    connection_profile2_get_ServiceProviderGuid,
    connection_profile2_GetSignalBars,
    connection_profile2_GetDomainConnectivityLevel,
    connection_profile2_GetNetworkUsageAsync,
    connection_profile2_GetConnectivityIntervalsAsync,
};

DEFINE_IINSPECTABLE( network_information_statics, INetworkInformationStatics, struct network_information_statics, IActivationFactory_iface )

static HRESULT WINAPI network_information_statics_GetConnectionProfiles( INetworkInformationStatics *iface, IVectorView_ConnectionProfile **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI network_information_statics_GetInternetConnectionProfile( INetworkInformationStatics *iface, IConnectionProfile **value )
{
    NetworkConnectivityLevel network_connectivity_level = 0xdeadbeef;
    INetworkListManager *network_list_manager = NULL;
    struct connection_profile *impl;
    HRESULT hr;

    TRACE( "iface %p, value %p\n", iface, value );

    *value = NULL;
    if (FAILED(hr = CoCreateInstance( &CLSID_NetworkListManager, NULL, CLSCTX_INPROC_SERVER, &IID_INetworkListManager, (void **)&network_list_manager ))) return hr;
    if (!(impl = calloc( 1, sizeof( *impl ) )))
    {
        INetworkListManager_Release( network_list_manager );
        return E_OUTOFMEMORY;
    }

    impl->IConnectionProfile_iface.lpVtbl = &connection_profile_vtbl;
    impl->IConnectionProfile2_iface.lpVtbl = &connection_profile2_vtbl;
    impl->ref = 1;
    impl->network_list_manager = network_list_manager;

    hr = IConnectionProfile_GetNetworkConnectivityLevel( &impl->IConnectionProfile_iface, &network_connectivity_level );
    if (FAILED(hr) || network_connectivity_level == NetworkConnectivityLevel_None)
    {
        free( impl );
        INetworkListManager_Release( network_list_manager );
        return hr;
    }

    *value = &impl->IConnectionProfile_iface;
    TRACE( "created IConnectionProfile %p.\n", *value );
    return hr;
}

static HRESULT WINAPI network_information_statics_GetLanIdentifiers( INetworkInformationStatics *iface, IVectorView_LanIdentifier **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

/* The local host names NetworkInformation.GetHostNames lists: the computer's DNS name, and every unicast address of an
 * adapter that is up, with IPInformation naming that adapter (and so matching ConnectionProfile.NetworkAdapter). WhatsApp
 * decides whether it is on IPv4 and IPv6 from exactly that match, and retried E_NOTIMPL here every 1.5 s. */
struct local_host_name
{
    IHostName IHostName_iface;
    IIPInformation IIPInformation_iface;
    LONG ref;
    HSTRING name;
    HostNameType type;
    DWORD if_index;
    BYTE prefix;
};

static inline struct local_host_name *impl_from_IHostName( IHostName *iface )
{
    return CONTAINING_RECORD( iface, struct local_host_name, IHostName_iface );
}

static HRESULT WINAPI local_host_name_QueryInterface( IHostName *iface, REFIID iid, void **out )
{
    struct local_host_name *impl = impl_from_IHostName( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IHostName ))
    {
        IInspectable_AddRef( (*out = &impl->IHostName_iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI local_host_name_AddRef( IHostName *iface )
{
    return InterlockedIncrement( &impl_from_IHostName( iface )->ref );
}

static ULONG WINAPI local_host_name_Release( IHostName *iface )
{
    struct local_host_name *impl = impl_from_IHostName( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        WindowsDeleteString( impl->name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI local_host_name_GetIids( IHostName *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI local_host_name_GetRuntimeClassName( IHostName *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Networking_HostName, wcslen( RuntimeClass_Windows_Networking_HostName ),
                                class_name );
}

static HRESULT WINAPI local_host_name_GetTrustLevel( IHostName *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI local_host_name_get_IPInformation( IHostName *iface, IIPInformation **value )
{
    struct local_host_name *impl = impl_from_IHostName( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    /* a domain name has none */
    if ((*value = impl->type == HostNameType_DomainName ? NULL : &impl->IIPInformation_iface))
        IHostName_AddRef( iface );
    return S_OK;
}

static HRESULT WINAPI local_host_name_get_RawName( IHostName *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IHostName( iface )->name, value );
}

static HRESULT WINAPI local_host_name_get_Type( IHostName *iface, HostNameType *value )
{
    *value = impl_from_IHostName( iface )->type;
    return S_OK;
}

static HRESULT WINAPI local_host_name_IsEqual( IHostName *iface, IHostName *name, boolean *equal )
{
    HSTRING other;
    INT32 order = 1;
    HRESULT hr;

    if (!name || !equal) return E_POINTER;
    if (FAILED(hr = IHostName_get_CanonicalName( name, &other ))) return hr;
    WindowsCompareStringOrdinal( impl_from_IHostName( iface )->name, other, &order );
    WindowsDeleteString( other );
    *equal = !order;
    return S_OK;
}

static const struct IHostNameVtbl local_host_name_vtbl =
{
    local_host_name_QueryInterface,
    local_host_name_AddRef,
    local_host_name_Release,
    /* IInspectable methods */
    local_host_name_GetIids,
    local_host_name_GetRuntimeClassName,
    local_host_name_GetTrustLevel,
    /* IHostName methods */
    local_host_name_get_IPInformation,
    local_host_name_get_RawName,
    local_host_name_get_RawName,     /* DisplayName */
    local_host_name_get_RawName,     /* CanonicalName: addresses and DNS names as the system formats them */
    local_host_name_get_Type,
    local_host_name_IsEqual,
};

static inline struct local_host_name *impl_from_IIPInformation( IIPInformation *iface )
{
    return CONTAINING_RECORD( iface, struct local_host_name, IIPInformation_iface );
}

static HRESULT WINAPI ip_information_QueryInterface( IIPInformation *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIPInformation ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ip_information_AddRef( IIPInformation *iface )
{
    return local_host_name_AddRef( &impl_from_IIPInformation( iface )->IHostName_iface );
}

static ULONG WINAPI ip_information_Release( IIPInformation *iface )
{
    return local_host_name_Release( &impl_from_IIPInformation( iface )->IHostName_iface );
}

static HRESULT WINAPI ip_information_GetIids( IIPInformation *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI ip_information_GetRuntimeClassName( IIPInformation *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Networking.Connectivity.IPInformation";
    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI ip_information_GetTrustLevel( IIPInformation *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI ip_information_get_NetworkAdapter( IIPInformation *iface, INetworkAdapter **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return network_adapter_create( impl_from_IIPInformation( iface )->if_index, value );
}

static HRESULT WINAPI ip_information_get_PrefixLength( IIPInformation *iface, IReference_BYTE **value )
{
    static const WCHAR class[] = L"Windows.Foundation.PropertyValue";
    IPropertyValueStatics *statics;
    IInspectable *boxed;
    HSTRING name;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = WindowsCreateString( class, wcslen( class ), &name ))) return hr;
    hr = RoGetActivationFactory( name, &IID_IPropertyValueStatics, (void **)&statics );
    WindowsDeleteString( name );
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = IPropertyValueStatics_CreateUInt8( statics, impl_from_IIPInformation( iface )->prefix, &boxed )))
    {
        hr = IInspectable_QueryInterface( boxed, &IID_IReference_BYTE, (void **)value );
        IInspectable_Release( boxed );
    }
    IPropertyValueStatics_Release( statics );
    return hr;
}

static const struct IIPInformationVtbl ip_information_vtbl =
{
    ip_information_QueryInterface,
    ip_information_AddRef,
    ip_information_Release,
    /* IInspectable methods */
    ip_information_GetIids,
    ip_information_GetRuntimeClassName,
    ip_information_GetTrustLevel,
    /* IIPInformation methods */
    ip_information_get_NetworkAdapter,
    ip_information_get_PrefixLength,
};

static HRESULT local_host_name_create( const WCHAR *name, HostNameType type, DWORD if_index, BYTE prefix,
                                       IHostName **out )
{
    struct local_host_name *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IHostName_iface.lpVtbl = &local_host_name_vtbl;
    impl->IIPInformation_iface.lpVtbl = &ip_information_vtbl;
    impl->ref = 1;
    impl->type = type;
    impl->if_index = if_index;
    impl->prefix = prefix;
    if (FAILED(hr = WindowsCreateString( name, wcslen( name ), &impl->name )))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IHostName_iface;
    return S_OK;
}

static HRESULT append_host_name( IVector_HostName *vector, const WCHAR *name, HostNameType type, DWORD if_index,
                                 BYTE prefix )
{
    IHostName *host;
    HRESULT hr;

    if (FAILED(hr = local_host_name_create( name, type, if_index, prefix, &host ))) return hr;
    hr = IVector_HostName_Append( vector, (IHostName *)host );
    IHostName_Release( host );
    return hr;
}

static HRESULT get_local_host_names( IVectorView_HostName **value )
{
    static const struct vector_iids iids =
    {
        .vector = &IID_IVector_HostName,
        .view = &IID_IVectorView_HostName,
        .iterable = &IID_IIterable_HostName,
        .iterator = &IID_IIterator_HostName,
    };
    IP_ADAPTER_ADDRESSES *addresses = NULL, *adapter;
    IP_ADAPTER_UNICAST_ADDRESS *unicast;
    ULONG size = 16384, err;
    WCHAR name[256];
    DWORD len = ARRAY_SIZE(name);
    IVector_HostName *vector;
    HRESULT hr;

    if (FAILED(hr = vector_create( &iids, (void **)&vector ))) return hr;
    if (GetComputerNameExW( ComputerNameDnsHostname, name, &len ) && name[0])
        append_host_name( vector, name, HostNameType_DomainName, 0, 0 );

    do
    {
        free( addresses );
        if (!(addresses = malloc( size ))) break;
        err = GetAdaptersAddresses( AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    NULL, addresses, &size );
    } while (err == ERROR_BUFFER_OVERFLOW);

    for (adapter = addresses; addresses && !err && adapter; adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
        {
            const SOCKADDR *sa = unicast->Address.lpSockaddr;
            DWORD if_index = sa->sa_family == AF_INET6 ? adapter->Ipv6IfIndex : adapter->IfIndex;
            WCHAR text[64];
            DWORD text_len = ARRAY_SIZE(text);

            if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) continue;
            if (WSAAddressToStringW( (SOCKADDR *)sa, unicast->Address.iSockaddrLength, NULL, text, &text_len )) continue;
            append_host_name( vector, text, sa->sa_family == AF_INET6 ? HostNameType_Ipv6 : HostNameType_Ipv4,
                              if_index ? if_index : adapter->IfIndex, unicast->OnLinkPrefixLength );
        }
    }
    free( addresses );

    hr = IVector_HostName_GetView( vector, value );
    IVector_HostName_Release( vector );
    return hr;
}

static HRESULT WINAPI network_information_statics_GetHostNames( INetworkInformationStatics *iface, IVectorView_HostName **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return get_local_host_names( value );
}

static HRESULT WINAPI network_information_statics_GetProxyConfigurationAsync( INetworkInformationStatics *iface, IUriRuntimeClass *uri, IAsyncOperation_ProxyConfiguration **value )
{
    FIXME( "iface %p, uri %p, value %p stub!\n", iface, uri, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI network_information_statics_GetSortedEndpointPairs( INetworkInformationStatics *iface, IIterable_EndpointPair *endpoint,
                                                                          HostNameSortOptions options, IVectorView_EndpointPair **value )
{
    FIXME( "iface %p, endpoint %p, options %d, value %p stub!\n", iface, endpoint, options, value );
    return E_NOTIMPL;
}

/* NetworkStatusChanged. Nothing here watches the network for changes, but apps take their initial reachability from
 * the first notification (the handler re-reads the status itself; the sender is NULL on Windows too): Solitaire waited
 * on its splash screen for ever with a registration that never fired. So each handler is notified after it registers,
 * and again for a while: an app often fans one handler out to components that only start listening seconds later,
 * and Windows raises the event whenever the network moves, so repeating it keeps to the contract. */
#define STATUS_NOTIFY_COUNT  20
#define STATUS_NOTIFY_PERIOD 1500

struct status_registration
{
    struct list entry;
    LONG ref;
    BOOL removed;
    EventRegistrationToken token;
    INetworkStatusChangedEventHandler *handler;
};

static CRITICAL_SECTION status_cs;
static CRITICAL_SECTION_DEBUG status_cs_debug =
{
    0, 0, &status_cs,
    { &status_cs_debug.ProcessLocksList, &status_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": status_cs") }
};
static CRITICAL_SECTION status_cs = { &status_cs_debug, -1, 0, 0, 0, 0 };
static struct list status_registrations = LIST_INIT( status_registrations );
static INT64 status_next_token;

static void status_registration_release( struct status_registration *reg )
{
    if (InterlockedDecrement( &reg->ref )) return;
    INetworkStatusChangedEventHandler_Release( reg->handler );
    free( reg );
}

static DWORD WINAPI status_notify_thread( void *arg )
{
    struct status_registration *reg = arg;
    BOOL removed;
    int i;

    Sleep( 250 );
    for (i = 0; i < STATUS_NOTIFY_COUNT; i++)
    {
        EnterCriticalSection( &status_cs );
        removed = reg->removed;
        LeaveCriticalSection( &status_cs );
        if (removed) break;
        TRACE( "notifying handler %p (%d)\n", reg->handler, i + 1 );
        INetworkStatusChangedEventHandler_Invoke( reg->handler, NULL );
        Sleep( STATUS_NOTIFY_PERIOD );
    }
    status_registration_release( reg );
    return 0;
}

static HRESULT WINAPI network_information_statics_add_NetworkStatusChanged( INetworkInformationStatics *iface, INetworkStatusChangedEventHandler *handler,
                                                                            EventRegistrationToken *cookie )
{
    struct status_registration *reg;
    HANDLE thread;

    TRACE( "iface %p, handler %p, cookie %p.\n", iface, handler, cookie );

    if (!cookie) return E_POINTER;
    if (!handler) return E_INVALIDARG;
    if (!(reg = calloc( 1, sizeof(*reg) ))) return E_OUTOFMEMORY;
    reg->ref = 1;
    reg->handler = handler;
    INetworkStatusChangedEventHandler_AddRef( handler );
    EnterCriticalSection( &status_cs );
    reg->token.value = ++status_next_token;
    list_add_tail( &status_registrations, &reg->entry );
    LeaveCriticalSection( &status_cs );
    *cookie = reg->token;

    InterlockedIncrement( &reg->ref );
    if ((thread = CreateThread( NULL, 0, status_notify_thread, reg, 0, NULL ))) CloseHandle( thread );
    else status_registration_release( reg );
    return S_OK;
}

static HRESULT WINAPI network_information_statics_remove_NetworkStatusChanged( INetworkInformationStatics *iface, EventRegistrationToken cookie )
{
    struct status_registration *reg, *found = NULL;

    TRACE( "iface %p, cookie %#I64x.\n", iface, cookie.value );

    EnterCriticalSection( &status_cs );
    LIST_FOR_EACH_ENTRY( reg, &status_registrations, struct status_registration, entry )
    {
        if (reg->token.value != cookie.value) continue;
        list_remove( &reg->entry );
        reg->removed = TRUE;
        found = reg;
        break;
    }
    LeaveCriticalSection( &status_cs );
    if (found) status_registration_release( found );
    return S_OK;
}

static const struct INetworkInformationStaticsVtbl network_information_statics_vtbl =
{
    network_information_statics_QueryInterface,
    network_information_statics_AddRef,
    network_information_statics_Release,
    /* IInspectable methods */
    network_information_statics_GetIids,
    network_information_statics_GetRuntimeClassName,
    network_information_statics_GetTrustLevel,
    /* INetworkInformationStatics methods */
    network_information_statics_GetConnectionProfiles,
    network_information_statics_GetInternetConnectionProfile,
    network_information_statics_GetLanIdentifiers,
    network_information_statics_GetHostNames,
    network_information_statics_GetProxyConfigurationAsync,
    network_information_statics_GetSortedEndpointPairs,
    network_information_statics_add_NetworkStatusChanged,
    network_information_statics_remove_NetworkStatusChanged,
};

static struct network_information_statics network_information_statics =
{
    {&factory_vtbl},
    {&network_information_statics_vtbl},
    1,
};

IActivationFactory *network_information_factory = &network_information_statics.IActivationFactory_iface;

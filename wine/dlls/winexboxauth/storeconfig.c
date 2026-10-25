/*
 * Windows.ApplicationModel.Store.Preview.StoreConfiguration: which WebAccount is the Store's
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
 * Windows' own (Windows.ApplicationModel.Store.dll) asks an out-of-process StoreConfigurationServer, which is not
 * registered here (REGDB_E_CLASSNOTREG at IStoreConfigurationStatics4). The Xbox app reads the Store's web account id
 * with GetStoreWebAccountId[ForUser], finds it with WebAuthenticationCoreManager.FindAccountAsync and compares it with
 * the Xbox user's WebAccountId ("Account mismatch"), and sets it with SetStoreWebAccountId[ForUser] ("trySetAccount").
 * The only account here is the Microsoft account of the Xbox sign-in, so that is the Store's account unless the app
 * sets another id, which is kept in xboxauth.bin ("store_account_id").
 *
 * Slots (Windows.ApplicationModel.winmd): Statics {728f7fc0} 6 SetSystemConfiguration
 *   7 SetMobileOperatorConfiguration 8 SetStoreWebAccountId 9 IsStoreWebAccountId
 *   10 get_HardwareManufacturerInfo 11 FilterUnsupportedSystemFeaturesAsync; Statics2 {657c4595}
 *   6/7 PurchasePromptingPolicy; Statics3 {6d45f57c} 6 HasStoreWebAccount 7 ..ForUser 8 GetStoreLogDataAsync
 *   9 SetStoreWebAccountIdForUser 10 IsStoreWebAccountIdForUser 11/12 ..PurchasePromptingPolicyForUser;
 *   Statics4 {20ff56d2} 6 GetStoreWebAccountId 7 ..ForUser 8-11 enterprise ids 12/13 ShouldRestrictToEnterpriseStoreOnly
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(xboxauth);

static const GUID IID_IStoreConfigurationStatics = {0x728f7fc0,0x8628,0x42ec,{0x84,0xa2,0x07,0x78,0x0e,0xb4,0x4d,0x8b}};
static const GUID IID_IStoreConfigurationStatics2 = {0x657c4595,0xc8b7,0x4fe9,{0x9f,0x4c,0x4d,0x71,0x02,0x7d,0x34,0x7e}};
static const GUID IID_IStoreConfigurationStatics3 = {0x6d45f57c,0xf144,0x4cb5,{0x9d,0x3f,0x4e,0xb0,0x5e,0x30,0xb6,0xd3}};
static const GUID IID_IStoreConfigurationStatics4 = {0x20ff56d2,0x4ee3,0x4cf0,{0x9b,0x12,0x55,0x2c,0x03,0x31,0x0f,0x75}};
static const GUID IID_IStoreHardwareManufacturerInfo = {0xf292dc08,0xc654,0x43ac,{0xa2,0x1f,0x34,0x80,0x1c,0x9d,0x33,0x88}};

/* NULL when nobody is signed in */
static char *store_web_account_id( void )
{
    char *id = stored_msa_account(), *set;

    if (!id) return NULL;
    if ((set = store_get( "store_account_id" )) && *set)
    {
        free( id );
        return set;
    }
    free( set );
    return id;
}

static HRESULT set_store_web_account_id( HSTRING h )
{
    char *id = from_hstr( h ), *msa = stored_msa_account();
    BOOL ours = id && msa && !_stricmp( id, msa );

    TRACE( "StoreConfiguration.SetStoreWebAccountId(%s)\n", !id || !*id ? "\"\"" : ours ? "the Microsoft account" : "another id" );
    store_update( "store_account_id", ours ? "" : S(id), NULL );
    free( id );
    free( msa );
    return S_OK;
}

static HRESULT is_store_web_account_id( HSTRING h, boolean *v )
{
    char *id, *have;

    if (!v) return E_POINTER;
    id = from_hstr( h );
    have = store_web_account_id();
    *v = have && id && !_stricmp( id, have );
    TRACE( "StoreConfiguration.IsStoreWebAccountId -> %d\n", *v );
    free( id );
    free( have );
    return S_OK;
}

/* ---- StoreHardwareManufacturerInfo: the PC's maker and model, from the BIOS key Wine fills from SMBIOS ---- */

/* The two OEM Store ids are empty, as on any PC without an OEM store configuration. The Microsoft Store reads this at
 * start-up; a refusal there ended in an ArgumentNullException on its UI thread. */
static HRESULT bios_value( const WCHAR *name, HSTRING *v )
{
    WCHAR buf[256];
    DWORD size = sizeof(buf);

    if (RegGetValueW( HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", name, RRF_RT_REG_SZ, NULL, buf, &size ))
        buf[0] = 0;
    return to_hstrw( buf, v );
}

static HRESULT WINAPI hw_get_HardwareManufacturerId( void *iface, HSTRING *v ) { return to_hstrw( L"", v ); }
static HRESULT WINAPI hw_get_StoreContentModifierId( void *iface, HSTRING *v ) { return to_hstrw( L"", v ); }
static HRESULT WINAPI hw_get_ModelName( void *iface, HSTRING *v ) { return bios_value( L"SystemProductName", v ); }
static HRESULT WINAPI hw_get_ManufacturerName( void *iface, HSTRING *v ) { return bios_value( L"SystemManufacturer", v ); }

static const void *hw_vtbl[] =
{
    OBJ_METHODS(0), hw_get_HardwareManufacturerId, hw_get_StoreContentModifierId, hw_get_ModelName, hw_get_ManufacturerName
};

/* ---- the factory ---- */

static HRESULT WINAPI sc_ActivateInstance( void *iface, IInspectable **out )
{
    if (out) *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI sc_SetSystemConfiguration( void *iface, HSTRING a, HSTRING b, INT64 c, HSTRING d )
{
    TRACE( "StoreConfiguration.SetSystemConfiguration: ignored\n" );
    return S_OK;
}
static HRESULT WINAPI sc_SetMobileOperatorConfiguration( void *iface, HSTRING a, UINT32 b, UINT32 c )
{
    TRACE( "StoreConfiguration.SetMobileOperatorConfiguration: ignored\n" );
    return S_OK;
}
static HRESULT WINAPI sc_SetStoreWebAccountId( void *iface, HSTRING id ) { return set_store_web_account_id( id ); }
static HRESULT WINAPI sc_IsStoreWebAccountId( void *iface, HSTRING id, boolean *v ) { return is_store_web_account_id( id, v ); }
static HRESULT WINAPI sc_get_HardwareManufacturerInfo( void *iface, void **v )
{
    struct object *obj;

    if (!v) return E_POINTER;
    TRACE( "StoreConfiguration.HardwareManufacturerInfo\n" );
    if (!(obj = object_create( sizeof(*obj), L"Windows.ApplicationModel.Store.Preview.StoreHardwareManufacturerInfo",
                               "StoreHardwareManufacturerInfo", NULL )))
        return E_OUTOFMEMORY;
    object_add( obj, hw_vtbl, &IID_IStoreHardwareManufacturerInfo );
    *v = &obj->vtbl[0];
    return S_OK;
}
/* IAsyncOperation<IVectorView<StoreSystemFeature>> and the view's interfaces; StoreSystemFeature is an enum
 * (enum(...;i4)), so these are derived from explicit signatures: piid.py's nested form gives a wrong IID */
static const GUID IID_AsyncOp_VectorView_StoreSystemFeature = {0xb1713163,0xee1b,0x5290,{0x83,0x16,0xf7,0xeb,0xb9,0xd5,0x31,0x63}};
static const GUID IID_IVectorView_StoreSystemFeature = {0xc8f90757,0xebcf,0x5f2c,{0xb9,0x18,0x64,0x87,0x10,0x5b,0x0c,0xa1}};
static const GUID IID_IIterable_StoreSystemFeature = {0x98a8577a,0xb128,0x5400,{0x8d,0x3d,0x58,0x65,0x4e,0xaa,0xf9,0x57}};
static const GUID IID_IIterator_StoreSystemFeature = {0xd0455b2d,0xd8aa,0x557e,{0x89,0xa3,0x63,0xc3,0x3e,0x8c,0xee,0x99}};

/* The features of the list this machine lacks: none (an x64 desktop; the Store only offers what passes). Refusing
 * the call left the Microsoft Store's Apps page on its loading skeletons for good. */
static HRESULT WINAPI sc_FilterUnsupportedSystemFeaturesAsync( void *iface, IInspectable *features, void **v )
{
    struct vector_view *view;

    TRACE( "StoreConfiguration.FilterUnsupportedSystemFeaturesAsync -> none unsupported\n" );
    if (!v) return E_POINTER;
    if (!(view = vector_view_create( &IID_IVectorView_StoreSystemFeature, &IID_IIterable_StoreSystemFeature,
            &IID_IIterator_StoreSystemFeature,
            L"Windows.Foundation.Collections.IVectorView`1<Windows.ApplicationModel.Store.Preview.StoreSystemFeature>" )))
        return E_OUTOFMEMORY;
    return completed_iface( &IID_AsyncOp_VectorView_StoreSystemFeature, "FilterUnsupportedSystemFeaturesAsync",
                            (IUnknown *)&view->obj.vtbl[0], v );
}

/* null is the documented "no policy set" */
static HRESULT WINAPI sc_get_PurchasePromptingPolicy( void *iface, IInspectable **v )
{
    if (!v) return E_POINTER;
    *v = NULL;
    return S_OK;
}
static HRESULT WINAPI sc_put_PurchasePromptingPolicy( void *iface, IInspectable *p )
{
    TRACE( "StoreConfiguration.PurchasePromptingPolicy: ignored\n" );
    return S_OK;
}

static HRESULT WINAPI sc_HasStoreWebAccount( void *iface, boolean *v )
{
    char *id;

    if (!v) return E_POINTER;
    id = store_web_account_id();
    *v = id != NULL;
    free( id );
    TRACE( "StoreConfiguration.HasStoreWebAccount -> %d\n", *v );
    return S_OK;
}
static HRESULT WINAPI sc_HasStoreWebAccountForUser( void *iface, IInspectable *user, boolean *v ) { return sc_HasStoreWebAccount( iface, v ); }
static HRESULT WINAPI sc_GetStoreLogDataAsync( void *iface, UINT32 options, void **v )
{
    TRACE( "StoreConfiguration.GetStoreLogDataAsync -> E_NOTIMPL\n" );
    if (v) *v = NULL;
    return E_NOTIMPL;
}
static HRESULT WINAPI sc_SetStoreWebAccountIdForUser( void *iface, IInspectable *user, HSTRING id ) { return set_store_web_account_id( id ); }
static HRESULT WINAPI sc_IsStoreWebAccountIdForUser( void *iface, IInspectable *user, HSTRING id, boolean *v )
{ return is_store_web_account_id( id, v ); }
static HRESULT WINAPI sc_GetPurchasePromptingPolicyForUser( void *iface, IInspectable *user, IInspectable **v )
{ return sc_get_PurchasePromptingPolicy( iface, v ); }
static HRESULT WINAPI sc_SetPurchasePromptingPolicyForUser( void *iface, IInspectable *user, IInspectable *p )
{ return sc_put_PurchasePromptingPolicy( iface, p ); }

static HRESULT WINAPI sc_GetStoreWebAccountId( void *iface, HSTRING *v )
{
    char *id = store_web_account_id();
    HRESULT hr;

    TRACE( "StoreConfiguration.GetStoreWebAccountId -> %s\n", id ? "the Microsoft account" : "none" );
    hr = to_hstr( S(id), v );
    free( id );
    return hr;
}
static HRESULT WINAPI sc_GetStoreWebAccountIdForUser( void *iface, IInspectable *user, HSTRING *v ) { return sc_GetStoreWebAccountId( iface, v ); }
static HRESULT WINAPI sc_SetEnterpriseStoreWebAccountId( void *iface, HSTRING id ) { return S_OK; }
static HRESULT WINAPI sc_SetEnterpriseStoreWebAccountIdForUser( void *iface, IInspectable *user, HSTRING id ) { return S_OK; }
static HRESULT WINAPI sc_GetEnterpriseStoreWebAccountId( void *iface, HSTRING *v )
{
    if (!v) return E_POINTER;
    *v = NULL;
    return S_OK;
}
static HRESULT WINAPI sc_GetEnterpriseStoreWebAccountIdForUser( void *iface, IInspectable *user, HSTRING *v )
{ return sc_GetEnterpriseStoreWebAccountId( iface, v ); }
static HRESULT WINAPI sc_ShouldRestrictToEnterpriseStoreOnly( void *iface, boolean *v )
{
    if (!v) return E_POINTER;
    *v = FALSE;
    return S_OK;
}
static HRESULT WINAPI sc_ShouldRestrictToEnterpriseStoreOnlyForUser( void *iface, IInspectable *user, boolean *v )
{ return sc_ShouldRestrictToEnterpriseStoreOnly( iface, v ); }

static const void *sc_activation_vtbl[] = { OBJ_METHODS(0), sc_ActivateInstance };
static const void *sc_statics_vtbl[] =
{
    OBJ_METHODS(1), sc_SetSystemConfiguration, sc_SetMobileOperatorConfiguration, sc_SetStoreWebAccountId,
    sc_IsStoreWebAccountId, sc_get_HardwareManufacturerInfo, sc_FilterUnsupportedSystemFeaturesAsync
};
static const void *sc_statics2_vtbl[] = { OBJ_METHODS(2), sc_get_PurchasePromptingPolicy, sc_put_PurchasePromptingPolicy };
static const void *sc_statics3_vtbl[] =
{
    OBJ_METHODS(3), sc_HasStoreWebAccount, sc_HasStoreWebAccountForUser, sc_GetStoreLogDataAsync,
    sc_SetStoreWebAccountIdForUser, sc_IsStoreWebAccountIdForUser, sc_GetPurchasePromptingPolicyForUser,
    sc_SetPurchasePromptingPolicyForUser
};
static const void *sc_statics4_vtbl[] =
{
    OBJ_METHODS(4), sc_GetStoreWebAccountId, sc_GetStoreWebAccountIdForUser, sc_SetEnterpriseStoreWebAccountId,
    sc_SetEnterpriseStoreWebAccountIdForUser, sc_GetEnterpriseStoreWebAccountId, sc_GetEnterpriseStoreWebAccountIdForUser,
    sc_ShouldRestrictToEnterpriseStoreOnly, sc_ShouldRestrictToEnterpriseStoreOnlyForUser
};

static struct object sc_factory;

IActivationFactory *store_configuration_factory( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending = FALSE;

    if (InitOnceBeginInitialize( &once, 0, &pending, NULL ) && pending)
    {
        object_init_static( &sc_factory, L"Windows.ApplicationModel.Store.Preview.StoreConfiguration", "StoreConfiguration" );
        object_add( &sc_factory, sc_activation_vtbl, &IID_IActivationFactory );
        object_add( &sc_factory, sc_statics_vtbl, &IID_IStoreConfigurationStatics );
        object_add( &sc_factory, sc_statics2_vtbl, &IID_IStoreConfigurationStatics2 );
        object_add( &sc_factory, sc_statics3_vtbl, &IID_IStoreConfigurationStatics3 );
        object_add( &sc_factory, sc_statics4_vtbl, &IID_IStoreConfigurationStatics4 );
        InitOnceComplete( &once, 0, NULL );
    }
    return (IActivationFactory *)&sc_factory.vtbl[0];
}

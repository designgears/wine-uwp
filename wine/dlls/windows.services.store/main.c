/*
 * Windows.Services.Store.StoreContext
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
 * Windows' StoreContext needs an out-of-process server (ContainerHostActivation, {22f5b1df-...}) that a prefix does not
 * have, and its operations then never complete: Solitaire's start-up waited on one forever. A stand-in whose every
 * IStoreContext method was traced showed the app calls GetDefault and nothing else, and that being able to get a
 * context was the whole fix. So this answers GetDefault and GetForUser, accepts the OfflineLicensesChanged registration
 * (refusing an event registration is fatal to callers), and answers every other method E_NOTIMPL, with a FIXME naming
 * its slot, writing nothing: the out-parameter's position differs per method. Typing the 27 methods would need most of
 * Windows.Services.Store in the IDL for calls nothing makes; the vtable is padded well past the interface's end, since a
 * shorter vtable would be a wild jump rather than an error.
 *
 * GetAppLicenseAsync is answered: Gears of War 4 asks for its licence at start-up, and an E_NOTIMPL there became an
 * unobserved task exception that ended the game. The licence is the one this prefix has (the user owns what the Xbox
 * app installed): active, not a trial, with no add-ons. The operation is already complete when handed out.
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "initguid.h"
#include "winstring.h"
#include "activation.h"
#include "roapi.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(storecontext);

DEFINE_GUID( IID_IStoreContextStatics, 0x9c06ee5f, 0x15c0, 0x4e72, 0x93, 0x30, 0xd6, 0x19, 0x1c, 0xeb, 0xd1, 0x9c );
DEFINE_GUID( IID_IStoreContext, 0xac98b6be, 0xf4fd, 0x4912, 0xba, 0xbd, 0x50, 0x35, 0xe5, 0xe8, 0xbc, 0xab );
DEFINE_GUID( IID_IStoreAppLicense, 0xf389f9de, 0x73c0, 0x45ce, 0x9b, 0xab, 0xb2, 0xfe, 0x3e, 0x5e, 0xaf, 0xd3 );
DEFINE_GUID( IID_IStoreAppLicense2, 0xb4666e91, 0x4443, 0x40b3, 0x99, 0x3f, 0x28, 0x90, 0x44, 0x35, 0xbd, 0xc6 );
/* IAsyncOperation<StoreAppLicense>, IMapView<String, StoreLicense> (piid.py) */
DEFINE_GUID( IID_IAsyncOperation_StoreAppLicense, 0x3866370b, 0xafc6, 0x5d01, 0x84, 0xc2, 0x45, 0x74, 0x62, 0x8d, 0xe5, 0x39 );
DEFINE_GUID( IID_IMapView_StoreLicense, 0x7dbaefd8, 0xc83a, 0x5ce3, 0xaf, 0x87, 0xac, 0xde, 0x89, 0x51, 0x97, 0xab );
DEFINE_GUID( IID_IAsyncInfo_, 0x00000036, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 );

static const WCHAR class_name[] = L"Windows.Services.Store.StoreContext";

/* extexec.c */
extern const WCHAR extexec_class_name[];
IActivationFactory *extexec_factory( void );

#define CONTEXT_SLOTS 48

struct object
{
    const void **vtbl;
};

static const void *context_vtbl[CONTEXT_SLOTS];
static const void *statics_vtbl[8];
static struct object context = { context_vtbl };
static struct object statics = { statics_vtbl };
static LONG64 next_token;

static HRESULT WINAPI context_QueryInterface( struct object *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IStoreContext ))
    {
        *out = &context;
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

/* both objects are static: no reference counting */
static ULONG WINAPI object_AddRef( struct object *iface ) { return 2; }
static ULONG WINAPI object_Release( struct object *iface ) { return 1; }

static HRESULT WINAPI object_GetIids( struct object *iface, ULONG *iid_count, IID **iids )
{
    if (iid_count) *iid_count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI object_GetRuntimeClassName( struct object *iface, HSTRING *name )
{
    return WindowsCreateString( class_name, wcslen( class_name ), name );
}

static HRESULT WINAPI object_GetTrustLevel( struct object *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI context_add_OfflineLicensesChanged( struct object *iface, IUnknown *handler, INT64 *token )
{
    TRACE( "handler %p, token %p: never raised.\n", handler, token );
    if (!token) return E_POINTER;
    *token = InterlockedIncrement64( &next_token );
    return S_OK;
}

static HRESULT WINAPI context_remove_OfflineLicensesChanged( struct object *iface, INT64 token )
{
    TRACE( "token %#I64x.\n", token );
    return S_OK;
}

#define CONTEXT_SLOT(n) \
    static HRESULT WINAPI context_slot_##n( struct object *iface, void *a, void *b, void *c, void *d, void *e ) \
    { \
        FIXME( "IStoreContext slot %u: not implemented.\n", n ); \
        return E_NOTIMPL; \
    }

CONTEXT_SLOT(6)  CONTEXT_SLOT(9)  CONTEXT_SLOT(10) CONTEXT_SLOT(11) CONTEXT_SLOT(12) CONTEXT_SLOT(13)
CONTEXT_SLOT(14) CONTEXT_SLOT(15) CONTEXT_SLOT(16) CONTEXT_SLOT(17) CONTEXT_SLOT(18) CONTEXT_SLOT(19)
CONTEXT_SLOT(20) CONTEXT_SLOT(21) CONTEXT_SLOT(22) CONTEXT_SLOT(23) CONTEXT_SLOT(24) CONTEXT_SLOT(25)
CONTEXT_SLOT(26) CONTEXT_SLOT(27) CONTEXT_SLOT(28) CONTEXT_SLOT(29) CONTEXT_SLOT(30) CONTEXT_SLOT(31)
CONTEXT_SLOT(32) CONTEXT_SLOT(33) CONTEXT_SLOT(34) CONTEXT_SLOT(35) CONTEXT_SLOT(36) CONTEXT_SLOT(37)
CONTEXT_SLOT(38) CONTEXT_SLOT(39) CONTEXT_SLOT(40) CONTEXT_SLOT(41) CONTEXT_SLOT(42) CONTEXT_SLOT(43)
CONTEXT_SLOT(44) CONTEXT_SLOT(45) CONTEXT_SLOT(46) CONTEXT_SLOT(47)

/* ---- the app's licence ---- */

static const void *license_vtbl[15], *license2_vtbl[7], *addons_vtbl[10], *license_op_vtbl[9], *license_info_vtbl[11];
static struct object license = { license_vtbl }, license2 = { license2_vtbl }, addons = { addons_vtbl };
static struct object license_op = { license_op_vtbl }, license_info = { license_info_vtbl };

static HRESULT WINAPI license_QueryInterface( struct object *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IStoreAppLicense ))
        *out = &license;
    else if (IsEqualGUID( iid, &IID_IStoreAppLicense2 )) *out = &license2;
    else
    {
        FIXME( "StoreAppLicense: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}
static HRESULT WINAPI license_GetRuntimeClassName( struct object *iface, HSTRING *name )
{
    static const WCHAR str[] = L"Windows.Services.Store.StoreAppLicense";
    return WindowsCreateString( str, wcslen( str ), name );
}
static HRESULT WINAPI license_empty_string( struct object *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}
static HRESULT WINAPI license_true( struct object *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}
static HRESULT WINAPI license_false( struct object *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}
static HRESULT WINAPI license_get_ExpirationDate( struct object *iface, INT64 *value )
{
    if (!value) return E_POINTER;
    *value = 0x7fffffffffffffff;   /* never: a full licence has no end date */
    return S_OK;
}
static HRESULT WINAPI license_get_AddOnLicenses( struct object *iface, struct object **value )
{
    if (!value) return E_POINTER;
    *value = &addons;
    return S_OK;
}
static HRESULT WINAPI license_get_TrialTimeRemaining( struct object *iface, INT64 *value )
{
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

/* AddOnLicenses: an empty IMapView<String, StoreLicense> */
static HRESULT WINAPI addons_QueryInterface( struct object *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IMapView_StoreLicense ))
    {
        *out = &addons;
        return S_OK;
    }
    FIXME( "AddOnLicenses: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}
static HRESULT WINAPI addons_GetRuntimeClassName( struct object *iface, HSTRING *name )
{
    static const WCHAR str[] = L"Windows.Foundation.Collections.IMapView`2<String, Windows.Services.Store.StoreLicense>";
    return WindowsCreateString( str, wcslen( str ), name );
}
static HRESULT WINAPI addons_Lookup( struct object *iface, HSTRING key, IInspectable **value )
{
    if (value) *value = NULL;
    return E_BOUNDS;
}
static HRESULT WINAPI addons_get_Size( struct object *iface, UINT32 *size )
{
    if (!size) return E_POINTER;
    *size = 0;
    return S_OK;
}
static HRESULT WINAPI addons_HasKey( struct object *iface, HSTRING key, boolean *found )
{
    if (!found) return E_POINTER;
    *found = FALSE;
    return S_OK;
}
static HRESULT WINAPI addons_Split( struct object *iface, IInspectable **first, IInspectable **second )
{
    if (first) *first = NULL;
    if (second) *second = NULL;
    return S_OK;
}

/* the operation: complete when handed out, so .get() skips its wait and a Completed handler runs at once */
static HRESULT WINAPI license_op_QueryInterface( struct object *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncOperation_StoreAppLicense ))
        *out = &license_op;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo_ )) *out = &license_info;
    else
    {
        FIXME( "GetAppLicenseAsync operation: %s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}
static HRESULT WINAPI license_op_GetRuntimeClassName( struct object *iface, HSTRING *name )
{
    static const WCHAR str[] = L"Windows.Foundation.IAsyncOperation`1<Windows.Services.Store.StoreAppLicense>";
    return WindowsCreateString( str, wcslen( str ), name );
}
static HRESULT WINAPI license_op_put_Completed( struct object *iface, IUnknown *handler )
{
    HRESULT (WINAPI *invoke)( IUnknown *, struct object *, INT32 );

    if (!handler) return E_POINTER;
    invoke = (*(void ***)handler)[3];   /* a delegate: IUnknown, then Invoke */
    invoke( handler, &license_op, 1 /* Completed */ );
    return S_OK;
}
static HRESULT WINAPI license_op_get_Completed( struct object *iface, IUnknown **handler )
{
    if (!handler) return E_POINTER;
    *handler = NULL;
    return S_OK;
}
static HRESULT WINAPI license_op_GetResults( struct object *iface, struct object **result )
{
    TRACE( "-> active, not a trial.\n" );
    if (!result) return E_POINTER;
    *result = &license;
    return S_OK;
}
static HRESULT WINAPI license_info_get_Id( struct object *iface, UINT32 *id )
{
    if (!id) return E_POINTER;
    *id = 1;
    return S_OK;
}
static HRESULT WINAPI license_info_get_Status( struct object *iface, INT32 *status )
{
    if (!status) return E_POINTER;
    *status = 1;   /* Completed */
    return S_OK;
}
static HRESULT WINAPI license_info_get_ErrorCode( struct object *iface, HRESULT *code )
{
    if (!code) return E_POINTER;
    *code = S_OK;
    return S_OK;
}
static HRESULT WINAPI license_info_Cancel( struct object *iface ) { return S_OK; }
static HRESULT WINAPI license_info_Close( struct object *iface ) { return S_OK; }

static HRESULT WINAPI context_GetAppLicenseAsync( struct object *iface, struct object **operation )
{
    TRACE( "operation %p.\n", operation );
    if (!operation) return E_POINTER;
    *operation = &license_op;
    return S_OK;
}

static void init_license_vtables(void)
{
    const void **v;

    v = license_vtbl;
    v[0] = license_QueryInterface; v[1] = object_AddRef; v[2] = object_Release; v[3] = object_GetIids;
    v[4] = license_GetRuntimeClassName; v[5] = object_GetTrustLevel;
    v[6] = license_empty_string;        /* SkuStoreId */
    v[7] = license_true;                /* IsActive */
    v[8] = license_false;               /* IsTrial */
    v[9] = license_get_ExpirationDate;
    v[10] = license_empty_string;       /* ExtendedJsonData */
    v[11] = license_get_AddOnLicenses;
    v[12] = license_get_TrialTimeRemaining;
    v[13] = license_false;              /* IsTrialOwnedByThisUser */
    v[14] = license_empty_string;       /* TrialUniqueId */

    v = license2_vtbl;
    v[0] = license_QueryInterface; v[1] = object_AddRef; v[2] = object_Release; v[3] = object_GetIids;
    v[4] = license_GetRuntimeClassName; v[5] = object_GetTrustLevel;
    v[6] = license_false;               /* IsDiscLicense */

    v = addons_vtbl;
    v[0] = addons_QueryInterface; v[1] = object_AddRef; v[2] = object_Release; v[3] = object_GetIids;
    v[4] = addons_GetRuntimeClassName; v[5] = object_GetTrustLevel;
    v[6] = addons_Lookup; v[7] = addons_get_Size; v[8] = addons_HasKey; v[9] = addons_Split;

    v = license_op_vtbl;
    v[0] = license_op_QueryInterface; v[1] = object_AddRef; v[2] = object_Release; v[3] = object_GetIids;
    v[4] = license_op_GetRuntimeClassName; v[5] = object_GetTrustLevel;
    v[6] = license_op_put_Completed; v[7] = license_op_get_Completed; v[8] = license_op_GetResults;

    v = license_info_vtbl;
    v[0] = license_op_QueryInterface; v[1] = object_AddRef; v[2] = object_Release; v[3] = object_GetIids;
    v[4] = license_op_GetRuntimeClassName; v[5] = object_GetTrustLevel;
    v[6] = license_info_get_Id; v[7] = license_info_get_Status; v[8] = license_info_get_ErrorCode;
    v[9] = license_info_Cancel; v[10] = license_info_Close;
}

static HRESULT WINAPI statics_QueryInterface( struct object *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ) || IsEqualGUID( iid, &IID_IStoreContextStatics ))
    {
        *out = &statics;
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

/* IActivationFactory::ActivateInstance and IStoreContextStatics::GetDefault share slot 6; a static class is never
 * activated, so the slot answers GetDefault */
static HRESULT WINAPI statics_GetDefault( struct object *iface, struct object **result )
{
    TRACE( "result %p.\n", result );
    if (!result) return E_POINTER;
    *result = &context;
    return S_OK;
}

static HRESULT WINAPI statics_GetForUser( struct object *iface, IUnknown *user, struct object **result )
{
    TRACE( "user %p, result %p.\n", user, result );
    return statics_GetDefault( iface, result );
}

static void init_vtables(void)
{
    static const void *slots[] =
    {
        context_slot_6, NULL, NULL, context_slot_9, context_slot_10, context_slot_11, context_slot_12, context_slot_13,
        context_slot_14, context_slot_15, context_slot_16, context_slot_17, context_slot_18, context_slot_19,
        context_slot_20, context_slot_21, context_slot_22, context_slot_23, context_slot_24, context_slot_25,
        context_slot_26, context_slot_27, context_slot_28, context_slot_29, context_slot_30, context_slot_31,
        context_slot_32, context_slot_33, context_slot_34, context_slot_35, context_slot_36, context_slot_37,
        context_slot_38, context_slot_39, context_slot_40, context_slot_41, context_slot_42, context_slot_43,
        context_slot_44, context_slot_45, context_slot_46, context_slot_47,
    };
    unsigned int i;

    context_vtbl[0] = context_QueryInterface;
    context_vtbl[1] = object_AddRef;
    context_vtbl[2] = object_Release;
    context_vtbl[3] = object_GetIids;
    context_vtbl[4] = object_GetRuntimeClassName;
    context_vtbl[5] = object_GetTrustLevel;
    for (i = 0; i < ARRAY_SIZE(slots); i++) context_vtbl[6 + i] = slots[i];
    context_vtbl[7] = context_add_OfflineLicensesChanged;
    context_vtbl[8] = context_remove_OfflineLicensesChanged;
    context_vtbl[11] = context_GetAppLicenseAsync;
    init_license_vtables();

    statics_vtbl[0] = statics_QueryInterface;
    statics_vtbl[1] = object_AddRef;
    statics_vtbl[2] = object_Release;
    statics_vtbl[3] = object_GetIids;
    statics_vtbl[4] = object_GetRuntimeClassName;
    statics_vtbl[5] = object_GetTrustLevel;
    statics_vtbl[6] = statics_GetDefault;
    statics_vtbl[7] = statics_GetForUser;
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;
    if (!wcscmp( WindowsGetStringRawBuffer( classid, NULL ), extexec_class_name ))
    {
        *factory = extexec_factory();
        return S_OK;
    }
    if (wcscmp( WindowsGetStringRawBuffer( classid, NULL ), class_name )) return CLASS_E_CLASSNOTAVAILABLE;
    if (!context_vtbl[0]) init_vtables();
    *factory = (IActivationFactory *)&statics;
    return S_OK;
}

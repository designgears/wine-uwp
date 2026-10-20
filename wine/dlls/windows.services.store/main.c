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

static const WCHAR class_name[] = L"Windows.Services.Store.StoreContext";

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
    if (wcscmp( WindowsGetStringRawBuffer( classid, NULL ), class_name )) return CLASS_E_CLASSNOTAVAILABLE;
    if (!context_vtbl[0]) init_vtables();
    *factory = (IActivationFactory *)&statics;
    return S_OK;
}

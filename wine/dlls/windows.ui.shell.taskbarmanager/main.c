/*
 * Windows.UI.Shell.TaskbarManager
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
 * Windows asks the shell (ImmersiveShell, {c2f03a33-...}) whether an app is pinned. A prefix has no such shell, and
 * Windows' operations then never complete, neither succeeding nor failing: Solitaire's start-up waited on one for ever
 * and never started its render loop. There is no taskbar to pin to here: IsSupported and IsPinningAllowed are false,
 * and every operation completes with false. Windows registers the class in wpnapps.dll; this module has its own name
 * so as not to take over wpnapps' other classes.
 */

#include "initguid.h"
#include "private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(taskbar);

static struct
{
    IActivationFactory IActivationFactory_iface;
    ITaskbarManagerStatics ITaskbarManagerStatics_iface;
    ITaskbarManager ITaskbarManager_iface;
    ITaskbarManager2 ITaskbarManager2_iface;
} impl;

static HRESULT get_iids( ULONG *iid_count, IID **iids )
{
    if (iid_count) *iid_count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT get_class_name( HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_UI_Shell_TaskbarManager,
                                wcslen( RuntimeClass_Windows_UI_Shell_TaskbarManager ), name );
}

/* the factory and its statics answer for the class; the manager answers for itself */
static HRESULT query_interface( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ITaskbarManagerStatics )) *out = &impl.ITaskbarManagerStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static HRESULT manager_query( REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ITaskbarManager ))
        *out = &impl.ITaskbarManager_iface;
    else if (IsEqualGUID( iid, &IID_ITaskbarManager2 )) *out = &impl.ITaskbarManager2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

#define STATIC_IINSPECTABLE( prefix, type, query ) \
    static HRESULT WINAPI prefix##_QueryInterface( type *iface, REFIID iid, void **out ) { return query( iid, out ); } \
    static ULONG WINAPI prefix##_AddRef( type *iface ) { return 2; } \
    static ULONG WINAPI prefix##_Release( type *iface ) { return 1; } \
    static HRESULT WINAPI prefix##_GetIids( type *iface, ULONG *count, IID **iids ) { return get_iids( count, iids ); } \
    static HRESULT WINAPI prefix##_GetRuntimeClassName( type *iface, HSTRING *name ) { return get_class_name( name ); } \
    static HRESULT WINAPI prefix##_GetTrustLevel( type *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

STATIC_IINSPECTABLE( factory, IActivationFactory, query_interface )
STATIC_IINSPECTABLE( statics, ITaskbarManagerStatics, query_interface )
STATIC_IINSPECTABLE( manager, ITaskbarManager, manager_query )
STATIC_IINSPECTABLE( manager2, ITaskbarManager2, manager_query )

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;   /* the manager comes from GetDefault */
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

static HRESULT WINAPI statics_GetDefault( ITaskbarManagerStatics *iface, ITaskbarManager **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    if (!result) return E_POINTER;
    *result = &impl.ITaskbarManager_iface;
    return S_OK;
}

static const struct ITaskbarManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    statics_GetDefault,
};

static HRESULT not_pinned( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_BOOL;
    result->boolVal = VARIANT_FALSE;
    return S_OK;
}

static HRESULT completed_false( void *invoker, IAsyncOperation_boolean **operation )
{
    if (!operation) return E_POINTER;
    return async_operation_boolean_create( (IUnknown *)invoker, NULL, not_pinned, operation );
}

static HRESULT WINAPI manager_get_IsSupported( ITaskbarManager *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI manager_get_IsPinningAllowed( ITaskbarManager *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI manager_IsCurrentAppPinnedAsync( ITaskbarManager *iface, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return completed_false( iface, operation );
}

static HRESULT WINAPI manager_IsAppListEntryPinnedAsync( ITaskbarManager *iface, IAppListEntry *entry, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, entry %p, operation %p.\n", iface, entry, operation );
    return completed_false( iface, operation );
}

static HRESULT WINAPI manager_RequestPinCurrentAppAsync( ITaskbarManager *iface, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return completed_false( iface, operation );
}

static HRESULT WINAPI manager_RequestPinAppListEntryAsync( ITaskbarManager *iface, IAppListEntry *entry, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, entry %p, operation %p.\n", iface, entry, operation );
    return completed_false( iface, operation );
}

static const struct ITaskbarManagerVtbl manager_vtbl =
{
    manager_QueryInterface,
    manager_AddRef,
    manager_Release,
    manager_GetIids,
    manager_GetRuntimeClassName,
    manager_GetTrustLevel,
    manager_get_IsSupported,
    manager_get_IsPinningAllowed,
    manager_IsCurrentAppPinnedAsync,
    manager_IsAppListEntryPinnedAsync,
    manager_RequestPinCurrentAppAsync,
    manager_RequestPinAppListEntryAsync,
};

static HRESULT WINAPI manager2_IsSecondaryTilePinnedAsync( ITaskbarManager2 *iface, HSTRING id, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, id %s, operation %p.\n", iface, debugstr_hstring( id ), operation );
    return completed_false( iface, operation );
}

static HRESULT WINAPI manager2_RequestPinSecondaryTileAsync( ITaskbarManager2 *iface, __x_ABI_CWindows_CUI_CStartScreen_CISecondaryTile *tile,
                                                             IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, tile %p, operation %p.\n", iface, tile, operation );
    return completed_false( iface, operation );
}

static HRESULT WINAPI manager2_TryUnpinSecondaryTileAsync( ITaskbarManager2 *iface, HSTRING id, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, id %s, operation %p.\n", iface, debugstr_hstring( id ), operation );
    return completed_false( iface, operation );
}

static const struct ITaskbarManager2Vtbl manager2_vtbl =
{
    manager2_QueryInterface,
    manager2_AddRef,
    manager2_Release,
    manager2_GetIids,
    manager2_GetRuntimeClassName,
    manager2_GetTrustLevel,
    manager2_IsSecondaryTilePinnedAsync,
    manager2_RequestPinSecondaryTileAsync,
    manager2_TryUnpinSecondaryTileAsync,
};

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    impl.IActivationFactory_iface.lpVtbl = &factory_vtbl;
    impl.ITaskbarManagerStatics_iface.lpVtbl = &statics_vtbl;
    impl.ITaskbarManager_iface.lpVtbl = &manager_vtbl;
    impl.ITaskbarManager2_iface.lpVtbl = &manager2_vtbl;

    *factory = NULL;
    if (wcscmp( WindowsGetStringRawBuffer( classid, NULL ), RuntimeClass_Windows_UI_Shell_TaskbarManager ))
        return CLASS_E_CLASSNOTAVAILABLE;
    *factory = &impl.IActivationFactory_iface;
    return S_OK;
}

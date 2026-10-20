/*
 * Windows.System.Profile.WindowsIntegrityPolicy: is this PC in S mode?
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
 * Windows asks wldp.dll, which makes an RPC call to the lockdown-policy service; with no such service the call
 * raises EPT_S_NOT_REGISTERED through the caller, and the Microsoft Store fail-fasts reading IsEnabled at start-up.
 * Wine enforces no code-integrity policy, so every property is false and PolicyChanged never fires (registering is
 * still accepted: refusing an event registration is fatal to callers).
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "initguid.h"
#include "winstring.h"
#include "activation.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_System_Profile
#include "windows.foundation.h"
#include "windows.system.profile.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(integrity);

struct policy_statics
{
    IActivationFactory IActivationFactory_iface;
    IWindowsIntegrityPolicyStatics IWindowsIntegrityPolicyStatics_iface;
};

static struct policy_statics policy_statics;

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &policy_statics.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IWindowsIntegrityPolicyStatics ))
        *out = &policy_statics.IWindowsIntegrityPolicyStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

/* a static object: no reference counting */
static ULONG WINAPI factory_AddRef( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI factory_Release( IActivationFactory *iface ) { return 1; }

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    if (iid_count) *iid_count = 0;
    if (iids) *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_System_Profile_WindowsIntegrityPolicy,
                                wcslen( RuntimeClass_Windows_System_Profile_WindowsIntegrityPolicy ), class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;   /* a static class */
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

static HRESULT WINAPI statics_QueryInterface( IWindowsIntegrityPolicyStatics *iface, REFIID iid, void **out )
{
    return factory_QueryInterface( &policy_statics.IActivationFactory_iface, iid, out );
}

static ULONG WINAPI statics_AddRef( IWindowsIntegrityPolicyStatics *iface ) { return 2; }
static ULONG WINAPI statics_Release( IWindowsIntegrityPolicyStatics *iface ) { return 1; }

static HRESULT WINAPI statics_GetIids( IWindowsIntegrityPolicyStatics *iface, ULONG *iid_count, IID **iids )
{
    return factory_GetIids( &policy_statics.IActivationFactory_iface, iid_count, iids );
}

static HRESULT WINAPI statics_GetRuntimeClassName( IWindowsIntegrityPolicyStatics *iface, HSTRING *class_name )
{
    return factory_GetRuntimeClassName( &policy_statics.IActivationFactory_iface, class_name );
}

static HRESULT WINAPI statics_GetTrustLevel( IWindowsIntegrityPolicyStatics *iface, TrustLevel *trust_level )
{
    return factory_GetTrustLevel( &policy_statics.IActivationFactory_iface, trust_level );
}

static HRESULT no( boolean *value )
{
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI statics_get_IsEnabled( IWindowsIntegrityPolicyStatics *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return no( value );
}

static HRESULT WINAPI statics_get_IsEnabledForTrial( IWindowsIntegrityPolicyStatics *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return no( value );
}

static HRESULT WINAPI statics_get_CanDisable( IWindowsIntegrityPolicyStatics *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return no( value );
}

static HRESULT WINAPI statics_get_IsDisableSupported( IWindowsIntegrityPolicyStatics *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return no( value );
}

static HRESULT WINAPI statics_add_PolicyChanged( IWindowsIntegrityPolicyStatics *iface, IEventHandler_IInspectable *handler,
                                                 EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = 1;   /* never raised */
    return S_OK;
}

static HRESULT WINAPI statics_remove_PolicyChanged( IWindowsIntegrityPolicyStatics *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static const struct IWindowsIntegrityPolicyStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IWindowsIntegrityPolicyStatics methods */
    statics_get_IsEnabled,
    statics_get_IsEnabledForTrial,
    statics_get_CanDisable,
    statics_get_IsDisableSupported,
    statics_add_PolicyChanged,
    statics_remove_PolicyChanged,
};

static struct policy_statics policy_statics =
{
    {&factory_vtbl},
    {&statics_vtbl},
};

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;
    if (wcscmp( name, RuntimeClass_Windows_System_Profile_WindowsIntegrityPolicy )) return CLASS_E_CLASSNOTAVAILABLE;
    *factory = &policy_statics.IActivationFactory_iface;
    return S_OK;
}

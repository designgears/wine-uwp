/*
 * Windows.Internal.StateRepository classes
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
 * On Windows these classes are service-activated (ActivationType=1, Server=StateRepository) and
 * implemented inside Windows.StateRepository.dll, which exports only ServiceMain; the client and
 * broker DLLs answer CLASS_E_CLASSNOTAVAILABLE for them. Without them Media Player ends on
 * REGDB_E_CLASSNOTREG after its whole UI is built.
 *
 * The interfaces are private and in no metadata. The layouts are read from the server vtables in
 * Windows.StateRepository.dll, named with its public PDB:
 *   {84103ccb-2fd7-4d6c-962e-5d8582b4c720} IUserStatics (UserFactoryServer)
 *   {5232f8ea-49c7-4840-bfbb-66e785689e88} IUser (UserServer)
 *
 * The repository describes the packages installed for each user. Everything here answers for the
 * one user the process runs as, which is the only one a prefix has, and for the running package.
 * User is implemented. The other classes are served by one table of slots, each of which knows
 * from staterepository_slots.h (./gen-staterepository.py in wine-uwp) its method's name, arity and
 * the type of its out-parameter, which is always the last: it traces itself and writes a zero, an
 * empty string, a row or a collection of one row there. Guessing which argument is the
 * out-parameter instead is how a caller's HSTRING ends up as garbage.
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "appmodel.h"
#include "sddl.h"
#include "objbase.h"
#include "initguid.h"
#include "activation.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(staterepo);

static const GUID IID_IUserStatics = {0x84103ccb, 0x2fd7, 0x4d6c, {0x96, 0x2e, 0x5d, 0x85, 0x82, 0xb4, 0xc7, 0x20}};
static const GUID IID_IUser = {0x5232f8ea, 0x49c7, 0x4840, {0xbf, 0xbb, 0x66, 0xe7, 0x85, 0x68, 0x9e, 0x88}};
static const GUID IID_IAgileObject_ = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

static const WCHAR user_class[] = L"Windows.Internal.StateRepository.User";

/* The process's user SID, in both forms the interface hands out. */
static BYTE user_sid[SECURITY_MAX_SID_SIZE];
static DWORD user_sid_len;
static WCHAR user_sid_text[256];

static BOOL WINAPI load_user_sid( INIT_ONCE *once, void *param, void **context )
{
    BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    HANDLE token;
    WCHAR *text;
    DWORD len;

    if (OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token ))
    {
        if (GetTokenInformation( token, TokenUser, buffer, sizeof(buffer), &len ))
        {
            PSID sid = ((TOKEN_USER *)buffer)->User.Sid;
            user_sid_len = min( GetLengthSid( sid ), sizeof(user_sid) );
            memcpy( user_sid, sid, user_sid_len );
            if (ConvertSidToStringSidW( sid, &text ))
            {
                lstrcpynW( user_sid_text, text, ARRAY_SIZE(user_sid_text) );
                LocalFree( text );
            }
        }
        CloseHandle( token );
    }
    if (!user_sid_len)  /* no token: the account a service would run as */
    {
        SID_IDENTIFIER_AUTHORITY nt = {SECURITY_NT_AUTHORITY};
        PSID sid;
        if (AllocateAndInitializeSid( &nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &sid ))
        {
            user_sid_len = GetLengthSid( sid );
            memcpy( user_sid, sid, user_sid_len );
            FreeSid( sid );
        }
        lstrcpyW( user_sid_text, L"S-1-5-18" );
    }
    return TRUE;
}

static void get_user_sid(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce( &once, load_user_sid, NULL, NULL );
}

/* IUser: one row, id 1, the current user. */

struct user
{
    const void **vtbl;
    LONG ref;
};

static HRESULT WINAPI user_QueryInterface( struct user *impl, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IUser ) || IsEqualGUID( iid, &IID_IAgileObject_ ))
    {
        InterlockedIncrement( &impl->ref );
        *out = impl;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI user_AddRef( struct user *impl )
{
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI user_Release( struct user *impl )
{
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI get_iids( void *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI user_GetRuntimeClassName( struct user *impl, HSTRING *name )
{
    return WindowsCreateString( user_class, ARRAY_SIZE(user_class) - 1, name );
}

static HRESULT WINAPI get_trust_level( void *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI user_get_UserID( struct user *impl, INT64 *value )
{
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI user_get_Revision( struct user *impl, UINT64 *value )
{
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI user_get_UserSid( struct user *impl, HSTRING *value )
{
    get_user_sid();
    TRACE( "-> %s.\n", debugstr_w( user_sid_text ) );
    return WindowsCreateString( user_sid_text, wcslen( user_sid_text ), value );
}

static HRESULT WINAPI user_get_Is_DictionaryNull( struct user *impl, boolean *value )
{
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI user_Get_Dictionary( struct user *impl, UINT32 *size, BYTE **value )
{
    *size = 0;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI user_GetUserSidAsSecurityIdentifier( struct user *impl, UINT32 *size, BYTE **value )
{
    get_user_sid();
    if (!(*value = CoTaskMemAlloc( user_sid_len ))) return E_OUTOFMEMORY;
    memcpy( *value, user_sid, user_sid_len );
    *size = user_sid_len;
    return S_OK;
}

static HRESULT WINAPI user_SetUserSidAsSecurityIdentifier( struct user *impl, UINT32 size, BYTE *value )
{
    return S_OK;
}

static const void *user_vtbl[] =
{
    user_QueryInterface,
    user_AddRef,
    user_Release,
    /* IInspectable methods */
    get_iids,
    user_GetRuntimeClassName,
    get_trust_level,
    /* IUser methods */
    user_get_UserID,
    user_get_Revision,
    user_get_UserSid,
    user_get_Is_DictionaryNull,
    user_Get_Dictionary,
    user_GetUserSidAsSecurityIdentifier,
    user_SetUserSidAsSecurityIdentifier,
};

static HRESULT user_create( void **out )
{
    struct user *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->vtbl = user_vtbl;
    impl->ref = 1;
    *out = impl;
    return S_OK;
}

/* IUserStatics, in the order UserFactoryServer declares it. Every lookup finds the one user. */

struct statics
{
    const void **vtbl;
};

static struct statics user_statics, user_factory;

static HRESULT WINAPI statics_QueryInterface( struct statics *iface, REFIID iid, void **out )
{
    /* the factory is a different layout: IInspectable and ActivateInstance */
    if (IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &user_factory;
    else if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
             IsEqualGUID( iid, &IID_IUserStatics ) || IsEqualGUID( iid, &IID_IAgileObject_ ))
        *out = &user_statics;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static ULONG WINAPI statics_AddRef( struct statics *iface )
{
    return 2;
}

static ULONG WINAPI statics_Release( struct statics *iface )
{
    return 1;
}

static HRESULT WINAPI statics_GetRuntimeClassName( struct statics *iface, HSTRING *name )
{
    return WindowsCreateString( user_class, ARRAY_SIZE(user_class) - 1, name );
}

static HRESULT WINAPI statics_Exists( struct statics *iface, INT64 id, boolean *value )
{
    *value = id == 1;
    return S_OK;
}

static HRESULT WINAPI statics_ExistsByUserSid( struct statics *iface, HSTRING sid, boolean *value )
{
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI statics_Get( struct statics *iface, INT64 id, void **value )
{
    return user_create( value );
}

static HRESULT WINAPI statics_GetByUserSid( struct statics *iface, HSTRING sid, void **value )
{
    return user_create( value );
}

/* a collection of users, whose element type is unknown and which nothing has asked for */
static HRESULT WINAPI statics_Find( struct statics *iface, void **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_GetCurrent( struct statics *iface, void **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return user_create( value );
}

static HRESULT WINAPI statics_ExistsByUserSecurityIdentifier( struct statics *iface, UINT32 size, BYTE *sid, boolean *value )
{
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI statics_GetByUserSecurityIdentifier( struct statics *iface, UINT32 size, BYTE *sid, void **value )
{
    return user_create( value );
}

static HRESULT WINAPI statics_GetByWindowsSystemUser( struct statics *iface, void *user, void **value )
{
    return user_create( value );
}

static HRESULT WINAPI statics_ActivateInstance( struct statics *iface, IInspectable **instance )
{
    return user_create( (void **)instance );
}

static const void *statics_vtbl[] =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    get_iids,
    statics_GetRuntimeClassName,
    get_trust_level,
    /* IUserStatics methods */
    statics_Exists,
    statics_ExistsByUserSid,
    statics_Get,                                /* Get */
    statics_Get,                                /* TryGet */
    statics_GetByUserSid,                       /* GetByUserSid */
    statics_GetByUserSid,                       /* TryGetByUserSid */
    statics_Find,
    statics_GetCurrent,
    statics_GetCurrent,                         /* GetEveryone */
    statics_GetCurrent,                         /* GetDefaultAccount */
    statics_GetCurrent,                         /* TryGetDefaultAccount */
    statics_ExistsByUserSecurityIdentifier,
    statics_GetByUserSecurityIdentifier,        /* GetByUserSecurityIdentifier */
    statics_GetByUserSecurityIdentifier,        /* TryGetByUserSecurityIdentifier */
    statics_GetByWindowsSystemUser,             /* TryGetByWindowsSystemUser */
    statics_GetByWindowsSystemUser,             /* GetByWindowsSystemUser */
};

static const void *factory_vtbl[] =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    get_iids,
    statics_GetRuntimeClassName,
    get_trust_level,
    /* IActivationFactory methods */
    statics_ActivateInstance,
};

/* The other classes */

enum
{
    SR_OUT_NONE, SR_OUT_ZERO1, SR_OUT_ZERO4, SR_OUT_ZERO8,
    SR_OUT_HSTRING, SR_OUT_IFACE, SR_OUT_COLLECTION,
};

#include "staterepository_slots.h"

#define GENERIC_SLOTS 160

struct generic
{
    const void **vtbl;
    LONG ref;
    BOOL row;           /* a row of the class rather than its statics */
    WCHAR cls[128];
};

static HRESULT generic_create( const WCHAR *cls, BOOL row, void **out );

/* A collection of one row. Several Find statics return one, and a NULL one is fatal: Media Player
 * fail-fasts forty-odd times on IApplicationStatics::FindByUserAndPackageFamilyNameAndAppListEntry.
 * The caller is the app looking itself up, and an empty result is a state it treats as fatal too.
 * It is handed back for whatever collection interface is asked for - IVectorView, IIterable,
 * IVector - whose tables differ in length, so the table is padded well past the longest. */
struct collection
{
    const void **vtbl;
    LONG ref;
    WCHAR element[128];
};

static HRESULT WINAPI collection_QueryInterface( struct collection *impl, REFIID iid, void **out )
{
    InterlockedIncrement( &impl->ref );
    *out = impl;
    return S_OK;
}

static ULONG WINAPI collection_AddRef( struct collection *impl )
{
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI collection_Release( struct collection *impl )
{
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI collection_GetRuntimeClassName( struct collection *impl, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Foundation.Collections.IVectorView`1<Object>";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI collection_GetAt( struct collection *impl, UINT32 index, void **item )
{
    *item = NULL;
    if (!impl->element[0] || index) return E_BOUNDS;
    return generic_create( impl->element, TRUE, item );
}

static HRESULT WINAPI collection_get_Size( struct collection *impl, UINT32 *size )
{
    *size = impl->element[0] ? 1 : 0;
    return S_OK;
}

static HRESULT WINAPI collection_IndexOf( struct collection *impl, void *item, UINT32 *index, boolean *found )
{
    *index = 0;
    *found = FALSE;
    return S_OK;
}

static HRESULT WINAPI collection_GetMany( struct collection *impl, UINT32 start, UINT32 capacity, void **items,
                                          UINT32 *written )
{
    *written = 0;
    if (impl->element[0] && !start && capacity && SUCCEEDED(generic_create( impl->element, TRUE, &items[0] )))
        *written = 1;
    return S_OK;
}

/* the methods of the other collection interfaces: clears the last writable pointer argument */
static HRESULT WINAPI collection_other( struct collection *impl, void *a, void *b, void *c, void *d )
{
    void *args[4] = {a, b, c, d};
    int i;

    for (i = 3; i >= 0; i--)
    {
        if (!args[i] || IsBadWritePtr( args[i], sizeof(void *) )) continue;
        *(void **)args[i] = NULL;
        break;
    }
    return S_OK;
}

#define OTHER4 collection_other, collection_other, collection_other, collection_other

static const void *collection_vtbl[32] =
{
    collection_QueryInterface,
    collection_AddRef,
    collection_Release,
    /* IInspectable methods */
    get_iids,
    collection_GetRuntimeClassName,
    get_trust_level,
    /* IVectorView<T> methods */
    collection_GetAt,
    collection_get_Size,
    collection_IndexOf,
    collection_GetMany,
    collection_other, collection_other,
    OTHER4, OTHER4, OTHER4, OTHER4, OTHER4,
};

static HRESULT collection_create( const WCHAR *element, void **out )
{
    struct collection *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->vtbl = collection_vtbl;
    impl->ref = 1;
    if (element) lstrcpynW( impl->element, element, ARRAY_SIZE(impl->element) );
    *out = impl;
    return S_OK;
}

static HRESULT WINAPI generic_QueryInterface( struct generic *impl, REFIID iid, void **out )
{
    /* Every interface is served by the same table: which one the caller holds decides what a slot
     * means, and the trace names the slot either way. */
    TRACE( "%s: %s.\n", debugstr_w( impl->cls ), debugstr_guid( iid ) );
    InterlockedIncrement( &impl->ref );
    *out = impl;
    return S_OK;
}

static ULONG WINAPI generic_AddRef( struct generic *impl )
{
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI generic_Release( struct generic *impl )
{
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI generic_GetRuntimeClassName( struct generic *impl, HSTRING *name )
{
    return WindowsCreateString( impl->cls, wcslen( impl->cls ), name );
}

static const struct sr_slot *find_slot( const WCHAR *cls, BOOL statics, unsigned int slot )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(sr_slots); i++)
    {
        if (sr_slots[i].slot != slot || sr_slots[i].statics != !!statics) continue;
        if (!wcscmp( sr_slots[i].cls, cls )) return &sr_slots[i];
    }
    return NULL;
}

static HRESULT generic_slot( unsigned int slot, struct generic *impl, void *args[6] )
{
    const struct sr_slot *desc = find_slot( impl->cls, !impl->row, slot );
    WCHAR name[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
    UINT32 len;
    void *out;

    TRACE( "%s %s slot %u %s, %d arguments.\n", debugstr_w( impl->cls ), impl->row ? "row" : "statics", slot,
           desc ? desc->name : "(unknown)", desc ? desc->argc : -1 );
    if (!desc || !desc->argc) return S_OK;
    out = args[desc->argc - 1];
    if (!out || IsBadWritePtr( out, sizeof(void *) )) return S_OK;

    if (impl->row)
    {
        /* The row needs an identity: the app reads _ApplicationID straight back into
         * IApplicationStatics::GetByUserAnd_ApplicationId, and zero finds nothing. */
        if (desc->kind == SR_OUT_ZERO8 && !strcmp( desc->name, "get__ApplicationID" ))
        {
            *(INT64 *)out = 1;
            return S_OK;
        }
        /* An empty display name is not survivable either. run.sh knows the package's; resources
         * hand back anything that is not an ms-resource URI unchanged. */
        if (desc->kind == SR_OUT_HSTRING && !strcmp( desc->name, "get_DisplayNameReference" ))
        {
            if (!GetEnvironmentVariableW( L"WINE_UWP_WINDOW_TITLE", name, ARRAY_SIZE(name) )) lstrcpyW( name, L"App" );
            return WindowsCreateString( name, wcslen( name ), out );
        }
        /* The Package row is the running package. Windows' AppInfo.Package looks it up by this
         * name; an empty one found nothing, and Solitaire threw out of the step after its bootflow. */
        if (desc->kind == SR_OUT_HSTRING && !strcmp( desc->name, "get_PackageFullName" ) &&
            !wcscmp( impl->cls, L"Windows.Internal.StateRepository.Package" ))
        {
            len = ARRAY_SIZE(name);
            if (!GetCurrentPackageFullName( &len, name )) return WindowsCreateString( name, wcslen( name ), out );
        }
    }

    switch (desc->kind)
    {
    case SR_OUT_ZERO1: *(BYTE *)out = 0; break;
    case SR_OUT_ZERO4: *(UINT32 *)out = 0; break;
    case SR_OUT_ZERO8: *(INT64 *)out = 0; break;
    case SR_OUT_HSTRING: *(HSTRING *)out = NULL; break;
    case SR_OUT_IFACE: return generic_create( desc->element ? desc->element : impl->cls, TRUE, out );
    case SR_OUT_COLLECTION: return collection_create( desc->element, out );
    default: break;
    }
    return S_OK;
}

#define SLOT(n) \
    static HRESULT WINAPI generic_slot_##n( struct generic *impl, void *a, void *b, void *c, void *d, void *e, void *f ) \
    { void *args[6] = {a, b, c, d, e, f}; return generic_slot( 0x##n, impl, args ); }
#define SLOTS(p) SLOT(p##0) SLOT(p##1) SLOT(p##2) SLOT(p##3) SLOT(p##4) SLOT(p##5) SLOT(p##6) SLOT(p##7) \
                 SLOT(p##8) SLOT(p##9) SLOT(p##a) SLOT(p##b) SLOT(p##c) SLOT(p##d) SLOT(p##e) SLOT(p##f)
SLOT(06) SLOT(07) SLOT(08) SLOT(09) SLOT(0a) SLOT(0b) SLOT(0c) SLOT(0d) SLOT(0e) SLOT(0f)
SLOTS(1) SLOTS(2) SLOTS(3) SLOTS(4) SLOTS(5) SLOTS(6) SLOTS(7) SLOTS(8) SLOTS(9)
#define ENTRIES(p) generic_slot_##p##0, generic_slot_##p##1, generic_slot_##p##2, generic_slot_##p##3, \
                   generic_slot_##p##4, generic_slot_##p##5, generic_slot_##p##6, generic_slot_##p##7, \
                   generic_slot_##p##8, generic_slot_##p##9, generic_slot_##p##a, generic_slot_##p##b, \
                   generic_slot_##p##c, generic_slot_##p##d, generic_slot_##p##e, generic_slot_##p##f

static const void *generic_vtbl[GENERIC_SLOTS] =
{
    generic_QueryInterface,
    generic_AddRef,
    generic_Release,
    /* IInspectable methods */
    get_iids,
    generic_GetRuntimeClassName,
    get_trust_level,
    /* the class's methods */
    generic_slot_06, generic_slot_07, generic_slot_08, generic_slot_09, generic_slot_0a, generic_slot_0b,
    generic_slot_0c, generic_slot_0d, generic_slot_0e, generic_slot_0f,
    ENTRIES(1), ENTRIES(2), ENTRIES(3), ENTRIES(4), ENTRIES(5), ENTRIES(6), ENTRIES(7), ENTRIES(8), ENTRIES(9)
};

static HRESULT generic_create( const WCHAR *cls, BOOL row, void **out )
{
    struct generic *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->vtbl = generic_vtbl;
    impl->ref = 1;
    impl->row = row;
    lstrcpynW( impl->cls, cls, ARRAY_SIZE(impl->cls) );
    *out = impl;
    return S_OK;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    static const WCHAR prefix[] = L"Windows.Internal.StateRepository.";
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;
    if (wcsncmp( name, prefix, ARRAY_SIZE(prefix) - 1 )) return CLASS_E_CLASSNOTAVAILABLE;
    if (wcscmp( name, user_class )) return generic_create( name, FALSE, (void **)factory );

    user_statics.vtbl = statics_vtbl;
    user_factory.vtbl = factory_vtbl;
    /* RoGetActivationFactory then asks for the interface the caller wants */
    *factory = (IActivationFactory *)&user_statics;
    return S_OK;
}

/*
 * Xbox sign-in: XboxAuthManager and its User
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

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(xboxauth);

static const GUID IID_IXboxAuthManager = {0xfebeda28,0x6412,0x558d,{0x97,0x81,0xc3,0xb0,0xd3,0x57,0x22,0xc5}};
static const GUID IID_IXboxAuthManagerStatics = {0xa4d7c0e6,0xc053,0x5b25,{0xa3,0xdf,0x14,0xae,0xd1,0x49,0xf7,0x36}};
static const GUID IID_IXboxAuthManagerFactory = {0x0fe0a92a,0x8479,0x509b,{0x98,0xaf,0x99,0xfb,0xce,0xa8,0x5a,0x36}};
static const GUID IID_IXboxAuthInitializationArgs = {0x61be67fb,0xd2c6,0x5494,{0x93,0x31,0xc6,0x2c,0x9e,0x0b,0xea,0x5b}};
static const GUID IID_IXboxAuthInitializationArgsFactory = {0x67116537,0xd929,0x58da,{0xad,0x5b,0x4a,0x04,0xaa,0xd2,0xb5,0x71}};
static const GUID IID_ITokenRequest = {0x6ba19172,0xafe5,0x579d,{0xbf,0x0f,0x86,0x26,0xe7,0xc7,0x1f,0xa5}};
static const GUID IID_IUser = {0x703e831e,0xe673,0x5e9d,{0x93,0x07,0x5a,0xf2,0x84,0x71,0xd4,0x17}};
static const GUID IID_IUserStateChangedEventArgs = {0x80d9a355,0x4425,0x5663,{0xbd,0x24,0xe7,0xa2,0xe2,0xfb,0x5a,0x64}};
static const GUID IID_IUserEventArgs = {0xfd4b985a,0xf082,0x58b3,{0xba,0xf1,0x49,0x90,0x7d,0x2f,0x14,0x01}};
static const GUID IID_AsyncOp_User = {0x33ba6bff,0xf4cd,0x5235,{0x8e,0x39,0xb6,0xa0,0x3b,0xf6,0x5e,0x9d}};
static const GUID IID_AsyncOp_TokenRequestResult = {0xfeaa4deb,0xd613,0x5fe2,{0x97,0xcd,0xa2,0x33,0x9a,0xa5,0x7c,0x11}};
static const GUID IID_AsyncOp_MsaTicketRequestResult = {0x9fd1fea4,0x02f6,0x5339,{0xae,0x61,0x26,0x22,0xa0,0xc3,0xf4,0x6d}};
static const GUID IID_AsyncOp_TransferTokenRequestResult = {0x3f3b1941,0x2ef2,0x5905,{0x9d,0x88,0x03,0x77,0x8a,0x1d,0xe7,0x3d}};
static const GUID IID_AsyncOp_ConsentState = {0xc92086c7,0xb593,0x50c4,{0xb9,0x31,0x8a,0x9c,0x06,0xd5,0x4e,0xd2}};
static const GUID IID_IBufferByteAccess_ = {0x905a0fef,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};

/* ---- UserStateChangedEventArgs: IUserStateChangedEventArgs, IUserEventArgs ---- */

struct state_args
{
    struct object obj;
    INT32 type;
    IUnknown *user;
};

static void state_args_destroy( struct object *obj )
{
    IUnknown_Release( CONTAINING_RECORD( obj, struct state_args, obj )->user );
}

static HRESULT WINAPI state_args_get_Type( void *iface, INT32 *v )
{
    if (!v) return E_POINTER;
    *v = CONTAINING_RECORD( OBJ_AT(iface, 0), struct state_args, obj )->type;
    return S_OK;
}

static HRESULT WINAPI state_args_TryGetDeferral( void *iface, IInspectable **deferral, boolean *ok )
{
    if (!deferral || !ok) return E_POINTER;
    *deferral = NULL;
    *ok = FALSE;
    return S_OK;
}

static HRESULT WINAPI state_args_get_User( void *iface, IUnknown **v )
{
    struct state_args *args = CONTAINING_RECORD( OBJ_AT(iface, 1), struct state_args, obj );
    if (!v) return E_POINTER;
    *v = args->user;
    IUnknown_AddRef( args->user );
    return S_OK;
}

static const void *state_args_vtbl[] = { OBJ_METHODS(0), state_args_get_Type, state_args_TryGetDeferral };
static const void *state_args_user_vtbl[] = { OBJ_METHODS(1), state_args_get_User };

/* ---- User ---- */

struct modern_gamertag { HSTRING modern, suffix, unique; };
struct privilege_result { boolean has; INT32 deny; };

struct user
{
    struct object obj;
    INT32 state;    /* SignedIn */
    struct events state_changed, properties_changed, picture_changed;
};

static struct user *current_user;  /* the signed-in user of this process, guarded by account.cs */

static struct user *user_from( void *iface )
{
    return CONTAINING_RECORD( OBJ_AT(iface, 0), struct user, obj );
}

static void user_destroy( struct object *obj )
{
    struct user *user = CONTAINING_RECORD( obj, struct user, obj );
    events_free( &user->state_changed );
    events_free( &user->properties_changed );
    events_free( &user->picture_changed );
}

static HRESULT field_hstr( char **field, HSTRING *v )
{
    char *s = account_field( field );
    HRESULT hr = to_hstr( s, v );
    free( s );
    return hr;
}

static HRESULT WINAPI user_get_Gamertag( void *iface, HSTRING *v )
{
    return field_hstr( &account.gtg, v );
}

static HRESULT WINAPI user_get_ModernGamertagProperties( void *iface, struct modern_gamertag *v )
{
    char *gtg, *mgt, *mgs, *umg;

    if (!v) return E_POINTER;
    gtg = account_field( &account.gtg );
    mgt = account_field( &account.mgt );
    mgs = account_field( &account.mgs );
    umg = account_field( &account.umg );
    if (!*S(mgt)) str_set( &mgt, gtg );
    if (!*S(umg))
    {
        free( umg );
        umg = *S(mgs) ? strf( "%s#%s", S(mgt), mgs ) : str_dup( mgt );
    }
    v->modern = v->suffix = v->unique = NULL;
    to_hstr( mgt, &v->modern );
    to_hstr( mgs, &v->suffix );
    to_hstr( umg, &v->unique );
    TRACE( "User.ModernGamertagProperties\n" );
    free( gtg );
    free( mgt );
    free( mgs );
    free( umg );
    return S_OK;
}

static HRESULT WINAPI user_get_Xuid( void *iface, UINT64 *v )
{
    char *xid;
    if (!v) return E_POINTER;
    xid = account_field( &account.xid );
    *v = _strtoui64( S(xid), NULL, 10 );
    free( xid );
    return S_OK;
}

static HRESULT WINAPI user_get_XuidStr( void *iface, HSTRING *v )
{
    return field_hstr( &account.xid, v );
}

static HRESULT WINAPI user_get_AgeGroup( void *iface, INT32 *v )
{
    char *a;
    if (!v) return E_POINTER;
    a = account_field( &account.agg );
    *v = !strcmp( S(a), "Adult" ) ? 3 : !strcmp( S(a), "Teen" ) ? 2 : !strcmp( S(a), "Child" ) ? 1 : 0;
    free( a );
    return S_OK;
}

static HRESULT WINAPI user_get_State( void *iface, INT32 *v )
{
    if (!v) return E_POINTER;
    *v = user_from( iface )->state;
    return S_OK;
}

static HRESULT WINAPI user_get_IsStoreUser( void *iface, boolean *v )
{
    /* the Store account is the Microsoft account WebAccount (webaccount.c), which is this user's own account */
    if (!v) return E_POINTER;
    *v = TRUE;
    TRACE( "User.IsStoreUser -> true\n" );
    return S_OK;
}

static HRESULT WINAPI user_get_WebAccountId( void *iface, HSTRING *v )
{
    char *id = stored_msa_account();
    HRESULT hr = to_hstr( id, v );
    TRACE( "User.WebAccountId\n" );
    free( id );
    return hr;
}

static HRESULT WINAPI user_CheckPrivilege( void *iface, INT32 privilege, struct privilege_result *v )
{
    /* the XSTS token's "prv" claim lists the privileges the account holds */
    char *prv, *spaced, needle[16];

    if (!v) return E_POINTER;
    prv = account_field( &account.prv );
    spaced = strf( " %s ", S(prv) );
    snprintf( needle, sizeof(needle), " %d ", privilege );
    v->has = strstr( spaced, needle ) != NULL;
    v->deny = v->has ? 0 : 2;  /* Restricted */
    TRACE( "User.CheckPrivilege(%d) -> %s\n", privilege, v->has ? "yes" : "restricted" );
    free( prv );
    free( spaced );
    return S_OK;
}

struct token_request
{
    char *url, *method;
    struct buf body;
    boolean force, ignore_title;
};

static void token_request_free( void *ctx )
{
    struct token_request *r = ctx;
    free( r->url );
    free( r->method );
    buf_free( &r->body );
    free( r );
}

static HRESULT token_request_work( struct async *op, void *ctx )
{
    struct token_request *r = ctx;
    struct signing_policy policy;
    char *header = NULL;
    HRESULT hr;

    if (FAILED(hr = get_token_for_url( r->url, r->force, &header, !r->ignore_title ))) return hr;
    op->s1 = header ? header : str_dup( "" );
    op->s2 = NULL;
    if (find_policy( r->url, &policy ))
    {
        struct url_parts u;
        split_url( r->url, &u );
        op->s2 = sign_request( policy.version, policy.max_body_bytes, r->method, u.path_and_query, op->s1, r->body.p,
                               r->body.n, 0 );
        TRACE( "signed %s %.*s (policy %d, body %Iu bytes)\n", r->method, (int)strcspn( r->url, "?" ), r->url,
               policy.version, r->body.n );
        url_parts_free( &u );
    }
    return S_OK;
}

static HRESULT WINAPI user_GetTokenAndSignatureAsync( void *iface, IInspectable *request, void **out )
{
    struct token_request *r;
    IInspectable *tr = NULL, *buffer = NULL;
    HSTRING h = NULL;

    if (!out) return E_POINTER;
    if (!request || FAILED(IInspectable_QueryInterface( request, &IID_ITokenRequest, (void **)&tr ))) return E_INVALIDARG;
    if (!(r = calloc( 1, sizeof(*r) )))
    {
        IInspectable_Release( tr );
        return E_OUTOFMEMORY;
    }
    r->method = str_dup( "GET" );
    /* ITokenRequest: 6 get_Method 8 get_Url 10 get_Headers 12 get_Body 14 get_ForceRefresh 16 get_IgnoreTitleIdentity */
    if (SUCCEEDED(SLOT(tr, 6, HRESULT (WINAPI *)( IInspectable *, HSTRING * ))( tr, &h )))
    {
        if (h)
        {
            free( r->method );
            r->method = from_hstr( h );
        }
        WindowsDeleteString( h );
        h = NULL;
    }
    if (SUCCEEDED(SLOT(tr, 8, HRESULT (WINAPI *)( IInspectable *, HSTRING * ))( tr, &h )))
    {
        r->url = from_hstr( h );
        WindowsDeleteString( h );
    }
    if (!r->url) r->url = str_dup( "" );
    SLOT(tr, 14, HRESULT (WINAPI *)( IInspectable *, boolean * ))( tr, &r->force );
    SLOT(tr, 16, HRESULT (WINAPI *)( IInspectable *, boolean * ))( tr, &r->ignore_title );
    if (SUCCEEDED(SLOT(tr, 12, HRESULT (WINAPI *)( IInspectable *, IInspectable ** ))( tr, &buffer )) && buffer)
    {
        /* IBuffer: 7 get_Length; IBufferByteAccess: 3 Buffer */
        IUnknown *access = NULL;
        UINT32 len = 0;
        BYTE *bytes = NULL;

        if (SUCCEEDED(SLOT(buffer, 7, HRESULT (WINAPI *)( IInspectable *, UINT32 * ))( buffer, &len )) && len &&
            SUCCEEDED(IInspectable_QueryInterface( buffer, &IID_IBufferByteAccess_, (void **)&access )))
        {
            if (SUCCEEDED(SLOT(access, 3, HRESULT (WINAPI *)( IUnknown *, BYTE ** ))( access, &bytes )) && bytes)
                buf_addn( &r->body, bytes, len );
            IUnknown_Release( access );
        }
        IInspectable_Release( buffer );
    }
    IInspectable_Release( tr );
    *out = async_start( &IID_AsyncOp_TokenRequestResult, R_STRINGS2, "GetTokenAndSignatureAsync", token_request_work,
                        r, token_request_free );
    if (!*out) return E_OUTOFMEMORY;
    *out = &((struct async *)*out)->obj.vtbl[0];
    return S_OK;
}

static HRESULT msa_ticket_work( struct async *op, void *ctx )
{
    char *token = NULL;
    HRESULT hr;

    EnterCriticalSection( &account.signin_cs );
    hr = msa_refresh( ctx, &token );
    LeaveCriticalSection( &account.signin_cs );
    if (SUCCEEDED(hr)) op->s1 = token;
    else str_wipe_free( token );
    return hr;
}

static HRESULT msa_ticket( HSTRING scope, BOOL force, void **out )
{
    struct async *op;
    char *s;

    if (!out) return E_POINTER;
    s = from_hstr( scope );
    TRACE( "User.GetMsaTicketAsync(\"%s\", force %d)\n", S(s), force );
    if (!(op = async_start( &IID_AsyncOp_MsaTicketRequestResult, R_STRINGS1, "GetMsaTicketAsync", msa_ticket_work,
                            s, free )))
        return E_OUTOFMEMORY;
    *out = &op->obj.vtbl[0];
    return S_OK;
}

static HRESULT WINAPI user_GetMsaTicketAsync( void *iface, HSTRING scope, void **out )
{
    return msa_ticket( scope, FALSE, out );
}

static HRESULT WINAPI user_GetMsaTicketAsync2( void *iface, HSTRING scope, boolean force, void **out )
{
    return msa_ticket( scope, force, out );
}

static HRESULT WINAPI user_GetGamerPictureAsync( void *iface, INT32 size, void **out )
{
    TRACE( "User.GetGamerPictureAsync(%d) -> E_NOTIMPL\n", size );
    if (out) *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI user_ResolveUserIssueWithUiAsync( void *iface, HSTRING url, void **out )
{
    /* the issue is resolved on a web page: opens it, as the Windows UI would show it */
    struct async *op;
    char *u;

    if (!out) return E_POINTER;
    u = from_hstr( url );
    TRACE( "User.ResolveUserIssueWithUiAsync\n" );
    if (u && *u) open_browser( u );
    free( u );
    if (!(op = async_create( &IID_IAsyncAction_, R_ACTION, "ResolveUserIssueWithUiAsync" ))) return E_OUTOFMEMORY;
    async_finish( op, S_OK );
    *out = &op->obj.vtbl[0];
    return S_OK;
}

static HRESULT WINAPI user_CheckConsent( void *iface, HSTRING name, INT32 *v )
{
    /* no consent service here: the consent does not apply */
    if (!v) return E_POINTER;
    *v = 1;  /* NotApplicable */
    TRACE( "User.CheckConsent(%s) -> NotApplicable\n", debugstr_hstring( name ) );
    return S_OK;
}

static HRESULT WINAPI user_ManageConsentWithUi( void *iface, HSTRING name, void **out )
{
    struct async *op;

    if (!out) return E_POINTER;
    TRACE( "User.ManageConsentWithUi(%s) -> NotApplicable\n", debugstr_hstring( name ) );
    if (!(op = async_create( &IID_AsyncOp_ConsentState, R_I4, "ManageConsentWithUi" ))) return E_OUTOFMEMORY;
    op->i4 = 1;
    async_finish( op, S_OK );
    *out = &op->obj.vtbl[0];
    return S_OK;
}

static void user_raise_state( struct user *user, INT32 type )
{
    struct state_args *args = object_create( sizeof(*args), L"Microsoft.GamePlatform.Services.Identity.UserStateChangedEventArgs",
                                             "UserStateChangedEventArgs", state_args_destroy );
    if (!args) return;
    object_add( &args->obj, state_args_vtbl, &IID_IUserStateChangedEventArgs );
    object_add( &args->obj, state_args_user_vtbl, &IID_IUserEventArgs );
    args->type = type;
    args->user = (IUnknown *)&user->obj.vtbl[0];
    obj_AddRef( &user->obj );
    events_raise( &user->state_changed, &user->obj.vtbl[0], &args->obj.vtbl[0] );
    obj_Release( &args->obj );
}

static HRESULT sign_out_work( struct async *op, void *ctx )
{
    struct user *user = ctx;

    user->state = 1;  /* SigningOut */
    user_raise_state( user, 1 );
    sign_out();
    EnterCriticalSection( &account.cs );
    if (current_user == user)
    {
        current_user = NULL;
        obj_Release( &user->obj );
    }
    LeaveCriticalSection( &account.cs );
    user->state = 2;  /* SignedOut */
    user_raise_state( user, 2 );
    return S_OK;
}

static void release_user( void *ctx )
{
    obj_Release( &((struct user *)ctx)->obj );
}

static HRESULT WINAPI user_SignOutAsync( void *iface, void **out )
{
    struct user *user = user_from( iface );
    struct async *op;

    if (!out) return E_POINTER;
    obj_AddRef( &user->obj );
    if (!(op = async_start( &IID_IAsyncAction_, R_ACTION, "SignOutAsync", sign_out_work, user, release_user )))
        return E_OUTOFMEMORY;
    *out = &op->obj.vtbl[0];
    return S_OK;
}

static HRESULT WINAPI user_add_UserStateChanged( void *iface, IUnknown *h, INT64 *t )
{
    return events_add( &user_from( iface )->state_changed, h, t );
}

static HRESULT WINAPI user_remove_UserStateChanged( void *iface, INT64 t )
{
    return events_remove( &user_from( iface )->state_changed, t );
}

static HRESULT WINAPI user_add_UserPropertiesChanged( void *iface, IUnknown *h, INT64 *t )
{
    return events_add( &user_from( iface )->properties_changed, h, t );
}

static HRESULT WINAPI user_remove_UserPropertiesChanged( void *iface, INT64 t )
{
    return events_remove( &user_from( iface )->properties_changed, t );
}

static HRESULT WINAPI user_add_UserGamerPictureChanged( void *iface, IUnknown *h, INT64 *t )
{
    return events_add( &user_from( iface )->picture_changed, h, t );
}

static HRESULT WINAPI user_remove_UserGamerPictureChanged( void *iface, INT64 t )
{
    return events_remove( &user_from( iface )->picture_changed, t );
}

static const void *user_vtbl[] =
{
    OBJ_METHODS(0),
    user_get_Gamertag,
    user_get_ModernGamertagProperties,
    user_get_Xuid,
    user_get_XuidStr,
    user_get_AgeGroup,
    user_get_State,
    user_get_IsStoreUser,
    user_get_WebAccountId,
    user_CheckPrivilege,
    user_GetTokenAndSignatureAsync,
    user_GetMsaTicketAsync,
    user_GetMsaTicketAsync2,
    user_GetGamerPictureAsync,
    user_ResolveUserIssueWithUiAsync,
    user_CheckConsent,
    user_ManageConsentWithUi,
    user_SignOutAsync,
    user_add_UserStateChanged,
    user_remove_UserStateChanged,
    user_add_UserPropertiesChanged,
    user_remove_UserPropertiesChanged,
    user_add_UserGamerPictureChanged,
    user_remove_UserGamerPictureChanged,
};
static const void *user_closable_vtbl[] = { OBJ_METHODS(1), closable_Close };

/* the current user, with a reference; NULL when nobody is signed in */
static struct user *get_current_user( void )
{
    struct user *user;

    EnterCriticalSection( &account.cs );
    if (!current_user && account.signed_in &&
        (current_user = object_create( sizeof(*current_user), L"Microsoft.GamePlatform.Services.Identity.User", "User",
                                       user_destroy )))
    {
        object_add( &current_user->obj, user_vtbl, &IID_IUser );
        object_add( &current_user->obj, user_closable_vtbl, &IID_IClosable_ );
        events_init( &current_user->state_changed );
        events_init( &current_user->properties_changed );
        events_init( &current_user->picture_changed );
    }
    if ((user = current_user)) obj_AddRef( &user->obj );
    LeaveCriticalSection( &account.cs );
    return user;
}

/* ---- XboxAuthInitializationArgs ---- */

struct init_args
{
    struct object obj;
    char *msa_app_id;
    UINT32 title_id;
    boolean presence, spop;
    char **consents;
    UINT32 consent_count;
};

static struct init_args *init_args_from( void *iface )
{
    return CONTAINING_RECORD( OBJ_AT(iface, 0), struct init_args, obj );
}

static void init_args_clear_consents( struct init_args *args )
{
    UINT32 i;
    for (i = 0; i < args->consent_count; i++) free( args->consents[i] );
    free( args->consents );
    args->consents = NULL;
    args->consent_count = 0;
}

static void init_args_destroy( struct object *obj )
{
    struct init_args *args = CONTAINING_RECORD( obj, struct init_args, obj );
    free( args->msa_app_id );
    init_args_clear_consents( args );
}

static HRESULT WINAPI init_args_get_MsaAppId( void *iface, HSTRING *v )
{
    return to_hstr( init_args_from( iface )->msa_app_id, v );
}

static HRESULT WINAPI init_args_put_MsaAppId( void *iface, HSTRING v )
{
    struct init_args *args = init_args_from( iface );
    free( args->msa_app_id );
    args->msa_app_id = from_hstr( v );
    return S_OK;
}

static HRESULT WINAPI init_args_get_TitleId( void *iface, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = init_args_from( iface )->title_id;
    return S_OK;
}

static HRESULT WINAPI init_args_put_TitleId( void *iface, UINT32 v )
{
    init_args_from( iface )->title_id = v;
    return S_OK;
}

static HRESULT WINAPI init_args_get_EnablePresence( void *iface, boolean *v )
{
    if (!v) return E_POINTER;
    *v = init_args_from( iface )->presence;
    return S_OK;
}

static HRESULT WINAPI init_args_put_EnablePresence( void *iface, boolean v )
{
    init_args_from( iface )->presence = v;
    return S_OK;
}

static HRESULT WINAPI init_args_get_EnableSpop( void *iface, boolean *v )
{
    if (!v) return E_POINTER;
    *v = init_args_from( iface )->spop;
    return S_OK;
}

static HRESULT WINAPI init_args_put_EnableSpop( void *iface, boolean v )
{
    init_args_from( iface )->spop = v;
    return S_OK;
}

static HRESULT WINAPI init_args_get_RequiredUcsConsents( void *iface, UINT32 *n, HSTRING **v )
{
    struct init_args *args = init_args_from( iface );
    HSTRING *arr;
    UINT32 i;

    if (!n || !v) return E_POINTER;
    *n = 0;
    *v = NULL;
    if (!args->consent_count) return S_OK;
    if (!(arr = CoTaskMemAlloc( args->consent_count * sizeof(HSTRING) ))) return E_OUTOFMEMORY;
    for (i = 0; i < args->consent_count; i++)
    {
        arr[i] = NULL;
        to_hstr( args->consents[i], &arr[i] );
    }
    *n = args->consent_count;
    *v = arr;
    return S_OK;
}

static HRESULT WINAPI init_args_put_RequiredUcsConsents( void *iface, UINT32 n, HSTRING *v )
{
    struct init_args *args = init_args_from( iface );
    UINT32 i;

    init_args_clear_consents( args );
    if (!v || !n) return S_OK;
    if (!(args->consents = calloc( n, sizeof(char *) ))) return E_OUTOFMEMORY;
    for (i = 0; i < n; i++) args->consents[i] = from_hstr( v[i] );
    args->consent_count = n;
    return S_OK;
}

static const void *init_args_vtbl[] =
{
    OBJ_METHODS(0),
    init_args_get_MsaAppId,
    init_args_put_MsaAppId,
    init_args_get_TitleId,
    init_args_put_TitleId,
    init_args_get_EnablePresence,
    init_args_put_EnablePresence,
    init_args_get_EnableSpop,
    init_args_put_EnableSpop,
    init_args_get_RequiredUcsConsents,
    init_args_put_RequiredUcsConsents,
};

static struct init_args *init_args_create( void )
{
    struct init_args *args = object_create( sizeof(*args), L"Microsoft.GamePlatform.Services.Identity.XboxAuthInitializationArgs",
                                            "XboxAuthInitializationArgs", init_args_destroy );
    if (args) object_add( &args->obj, init_args_vtbl, &IID_IXboxAuthInitializationArgs );
    return args;
}

static struct object init_args_factory;

static HRESULT WINAPI init_args_factory_ActivateInstance( void *iface, IInspectable **out )
{
    struct init_args *args;
    if (!out) return E_POINTER;
    if (!(args = init_args_create())) return E_OUTOFMEMORY;
    *out = (IInspectable *)&args->obj.vtbl[0];
    return S_OK;
}

static HRESULT WINAPI init_args_factory_CreateInstance( void *iface, HSTRING app, UINT32 title, void **out )
{
    struct init_args *args;
    if (!out) return E_POINTER;
    if (!(args = init_args_create())) return E_OUTOFMEMORY;
    args->msa_app_id = from_hstr( app );
    args->title_id = title;
    TRACE( "XboxAuthInitializationArgs(msa app %s, title %#x)\n", S(args->msa_app_id), title );
    *out = &args->obj.vtbl[0];
    return S_OK;
}

static HRESULT WINAPI init_args_factory_CreateInstance2( void *iface, HSTRING app, UINT32 title, boolean presence,
                                                         boolean spop, UINT32 n, HSTRING *consents, void **out )
{
    struct init_args *args;
    if (!out) return E_POINTER;
    if (!(args = init_args_create())) return E_OUTOFMEMORY;
    args->msa_app_id = from_hstr( app );
    args->title_id = title;
    args->presence = presence;
    args->spop = spop;
    init_args_put_RequiredUcsConsents( &args->obj.vtbl[0], n, consents );
    TRACE( "XboxAuthInitializationArgs(msa app %s, title %#x, presence %d, spop %d, %u consents)\n",
           S(args->msa_app_id), title, presence, spop, n );
    *out = &args->obj.vtbl[0];
    return S_OK;
}

static const void *init_args_activation_vtbl[] = { OBJ_METHODS(0), init_args_factory_ActivateInstance };
static const void *init_args_factory_vtbl[] = { OBJ_METHODS(1), init_args_factory_CreateInstance,
                                                init_args_factory_CreateInstance2 };

IActivationFactory *xboxauth_init_args_factory( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending = FALSE;

    if (InitOnceBeginInitialize( &once, 0, &pending, NULL ) && pending)
    {
        object_init_static( &init_args_factory, L"Microsoft.GamePlatform.Services.Identity.XboxAuthInitializationArgs",
                            "XboxAuthInitializationArgs factory" );
        object_add( &init_args_factory, init_args_activation_vtbl, &IID_IActivationFactory );
        object_add( &init_args_factory, init_args_factory_vtbl, &IID_IXboxAuthInitializationArgsFactory );
        InitOnceComplete( &once, 0, NULL );
    }
    return (IActivationFactory *)&init_args_factory.vtbl[0];
}

/* ---- XboxAuthManager ---- */

static HRESULT complete_with_user( struct async *op, HRESULT hr )
{
    struct user *user;
    if (FAILED(hr)) return hr;
    if ((user = get_current_user())) op->iface = (IUnknown *)&user->obj.vtbl[0];
    return S_OK;
}

static HRESULT WINAPI manager_get_Sandbox( void *iface, HSTRING *v )
{
    return to_hstrw( L"RETAIL", v );
}

static HRESULT add_silently_work( struct async *op, void *ctx )
{
    HRESULT hr = sign_in_silently();
    if (hr == XA_E_NO_ACCOUNT)
    {
        TRACE( "no stored sign-in: no default user\n" );
        return S_OK;
    }
    return complete_with_user( op, hr );
}

static HRESULT add_optional_ui_work( struct async *op, void *ctx )
{
    HRESULT hr = sign_in_silently();
    if (FAILED(hr) && hr != XA_E_NETWORK) hr = sign_in_with_device_code( &op->cancelled );
    return complete_with_user( op, hr );
}

static HRESULT add_ui_work( struct async *op, void *ctx )
{
    BOOL in;
    EnterCriticalSection( &account.cs );
    in = account.signed_in;
    LeaveCriticalSection( &account.cs );
    return complete_with_user( op, in ? S_OK : sign_in_with_device_code( &op->cancelled ) );
}

static HRESULT start_user_op( const char *name, HRESULT (*work)( struct async *, void * ), void **out )
{
    struct async *op;
    if (!out) return E_POINTER;
    TRACE( "XboxAuthManager.%s\n", name );
    if (!(op = async_start( &IID_AsyncOp_User, R_IFACE, name, work, NULL, NULL ))) return E_OUTOFMEMORY;
    *out = &op->obj.vtbl[0];
    return S_OK;
}

static HRESULT WINAPI manager_TryAddDefaultUserSilentlyAsync( void *iface, void **out )
{
    return start_user_op( "TryAddDefaultUserSilentlyAsync", add_silently_work, out );
}

static HRESULT WINAPI manager_AddUserWithOptionalUiAsync( void *iface, void **out )
{
    return start_user_op( "AddUserWithOptionalUiAsync", add_optional_ui_work, out );
}

static HRESULT WINAPI manager_AddUserWithUiAsync( void *iface, void **out )
{
    return start_user_op( "AddUserWithUiAsync", add_ui_work, out );
}

static HRESULT WINAPI manager_GetDeviceUserAsync( void *iface, void **out )
{
    /* a device identity needs an Xbox device token and a proof key of its own; this has neither */
    TRACE( "XboxAuthManager.GetDeviceUserAsync -> E_NOTIMPL\n" );
    if (out) *out = NULL;
    return E_NOTIMPL;
}

static const void *manager_vtbl[] =
{
    OBJ_METHODS(0),
    manager_get_Sandbox,
    manager_TryAddDefaultUserSilentlyAsync,
    manager_AddUserWithOptionalUiAsync,
    manager_AddUserWithUiAsync,
    manager_GetDeviceUserAsync,
};
static const void *manager_closable_vtbl[] = { OBJ_METHODS(1), closable_Close };

/* A GDK title ships MicrosoftGame.Config at its package root; packaged apps such as the Xbox app do not. */
static BOOL is_gdk_title_process( void )
{
    WCHAR path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_PATH", path, MAX_PATH );

    if (!n || n >= MAX_PATH - 32) return FALSE;
    if (path[n - 1] != '\\') wcscat( path, L"\\" );
    wcscat( path, L"MicrosoftGame.Config" );
    return GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
}

static struct object manager_factory;

static HRESULT WINAPI manager_factory_ActivateInstance( void *iface, IInspectable **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI manager_factory_CreateInstance( void *iface, IInspectable *args, void **out )
{
    struct object *manager;
    IInspectable *a = NULL;
    char *app = NULL;
    UINT32 title = 0;

    if (!out) return E_POINTER;
    if (args && SUCCEEDED(IInspectable_QueryInterface( args, &IID_IXboxAuthInitializationArgs, (void **)&a )))
    {
        HSTRING h = NULL;
        if (SUCCEEDED(SLOT(a, 6, HRESULT (WINAPI *)( IInspectable *, HSTRING * ))( a, &h )))
        {
            app = from_hstr( h );
            WindowsDeleteString( h );
        }
        SLOT(a, 8, HRESULT (WINAPI *)( IInspectable *, UINT32 * ))( a, &title );
        IInspectable_Release( a );
    }
    TRACE( "XboxAuthManager(msa app %s, title %#x)\n", S(app), title );
    /* Keeps the title identity for GDK GetToken in this process when the manager is a game's. The Xbox app creates
     * one for its own client too; that is not a GDK title, and pinning a Win32 device made the app's SISU 401 and
     * left it signed out. */
    if (app && *app && title && strcmp( app, MSA_CLIENT_ID ) && is_gdk_title_process())
        XboxAuthSetTitleContext( app, title );
    free( app );
    if (!(manager = object_create( sizeof(*manager), L"Microsoft.GamePlatform.Services.Identity.XboxAuthManager",
                                   "XboxAuthManager", NULL )))
        return E_OUTOFMEMORY;
    object_add( manager, manager_vtbl, &IID_IXboxAuthManager );
    object_add( manager, manager_closable_vtbl, &IID_IClosable_ );
    *out = &manager->vtbl[0];
    return S_OK;
}

static HRESULT WINAPI manager_factory_AddUserWithTransferTokenAsync( void *iface, HSTRING token, void **out )
{
    struct async *op;

    if (!out) return E_POINTER;
    TRACE( "XboxAuthManager.AddUserWithTransferTokenAsync -> NotSupported\n" );
    if (!(op = async_create( &IID_AsyncOp_TransferTokenRequestResult, R_TRANSFER, "AddUserWithTransferTokenAsync" )))
        return E_OUTOFMEMORY;
    op->i4 = 1;  /* NotSupported */
    op->i4b = S_OK;
    async_finish( op, S_OK );
    *out = &op->obj.vtbl[0];
    return S_OK;
}

static const void *manager_activation_vtbl[] = { OBJ_METHODS(0), manager_factory_ActivateInstance };
static const void *manager_factory_vtbl[] = { OBJ_METHODS(1), manager_factory_CreateInstance };
static const void *manager_statics_vtbl[] = { OBJ_METHODS(2), manager_factory_AddUserWithTransferTokenAsync };

IActivationFactory *xboxauth_manager_factory( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending = FALSE;

    if (InitOnceBeginInitialize( &once, 0, &pending, NULL ) && pending)
    {
        object_init_static( &manager_factory, L"Microsoft.GamePlatform.Services.Identity.XboxAuthManager",
                            "XboxAuthManager factory" );
        object_add( &manager_factory, manager_activation_vtbl, &IID_IActivationFactory );
        object_add( &manager_factory, manager_factory_vtbl, &IID_IXboxAuthManagerFactory );
        object_add( &manager_factory, manager_statics_vtbl, &IID_IXboxAuthManagerStatics );
        InitOnceComplete( &once, 0, NULL );
    }
    return (IActivationFactory *)&manager_factory.vtbl[0];
}

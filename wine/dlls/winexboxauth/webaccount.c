/*
 * Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager for the Microsoft account
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
 * On Windows the Store's account is the Microsoft account WebAccount that the Web Account Manager (the token broker,
 * out of process) holds for provider https://login.microsoft.com, authority "consumers". The Xbox app's
 * StoreAccountManager asks for that provider with FindAccountProviderAsync(id, authority, user) (Statics2), finds the
 * account by its id and asks GetTokenSilentlyAsync for tokens. There is no broker here, so the manager answers from
 * the same Microsoft account the Xbox sign-in holds: WebAccount.Id is the account's PUID (its user id until that is
 * known), which is also what User.WebAccountId reports, and tokens are refresh-token grants for the request's scope.
 * Other providers are not found (null).
 *
 * Slots (Windows.Security.winmd, ./winmd.py --sig):
 *   Statics {6aca7c92}: 6/7 GetTokenSilentlyAsync(request[, account]) 8/9 RequestTokenAsync
 *                       10 FindAccountAsync(provider, id) 11/12 FindAccountProviderAsync(id[, authority])
 *   Statics2 {f584184a}: 6 FindAccountProviderAsync(id, authority, user)
 *   Statics4 {54e633fe}: 6/7 FindAllAccountsAsync(provider[, clientId]) 8-10 FindSystemAccountProviderAsync
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(xboxauth);

static const GUID IID_IWACMStatics = {0x6aca7c92,0xa581,0x4479,{0x9c,0x10,0x75,0x2e,0xff,0x44,0xfd,0x34}};
static const GUID IID_IWACMStatics2 = {0xf584184a,0x8b57,0x4820,{0xb6,0xa4,0x70,0xa5,0xb6,0xfc,0xf4,0x4a}};
static const GUID IID_IWACMStatics4 = {0x54e633fe,0x96e0,0x41e8,{0x98,0x32,0x12,0x98,0x89,0x7c,0x2a,0xaf}};
static const GUID IID_IWebAccountProvider = {0x29dcc8c3,0x7ab9,0x4a7c,{0xa3,0x36,0xb9,0x42,0xf9,0xdb,0xf7,0xc7}};
static const GUID IID_IWebAccountProvider2 = {0x4a01eb05,0x4e42,0x41d4,{0xb5,0x18,0xe0,0x08,0xa5,0x16,0x36,0x14}};
static const GUID IID_IWebAccountProvider3 = {0xda1c518b,0x970d,0x4d49,{0x82,0x5c,0xf2,0x70,0x6f,0x8c,0xa7,0xfe}};
static const GUID IID_IWebAccountProvider4 = {0x718fd8db,0xe796,0x4210,{0xb7,0x4e,0x84,0xd2,0x98,0x94,0xb0,0x80}};
static const GUID IID_IWebAccount = {0x69473eb2,0x8031,0x49be,{0x80,0xbb,0x96,0xcb,0x46,0xd9,0x9a,0xba}};
static const GUID IID_IWebAccount2 = {0x7b56d6f8,0x990b,0x4eb5,{0x94,0xa7,0x56,0x21,0xf3,0xa8,0xb8,0x24}};
static const GUID IID_IWebTokenRequest = {0xb77b4d68,0xadcb,0x4673,{0xb3,0x64,0x0c,0xf7,0xb3,0x5c,0xaf,0x97}};
static const GUID IID_IWebTokenRequestResult = {0xc12a8305,0xd1f8,0x4483,{0x8d,0x54,0x38,0xfe,0x29,0x27,0x84,0xff}};
static const GUID IID_IWebTokenResponse = {0x67a7c5ca,0x83f6,0x44c6,{0xa3,0xb1,0x0e,0xb6,0x9e,0x41,0xfa,0x8a}};
static const GUID IID_IWebProviderError = {0xdb191bb1,0x50c5,0x4809,{0x8d,0xca,0x09,0xc9,0x94,0x10,0x24,0x5c}};
static const GUID IID_IFindAllAccountsResult = {0xa5812b5d,0xb72e,0x420c,{0x86,0xab,0xaa,0xc0,0xd7,0xb7,0x26,0x1f}};
static const GUID IID_AsyncOp_WebAccountProvider = {0x88c66009,0x12f7,0x58e2,{0x8d,0xbe,0x6e,0xfc,0x62,0x0c,0x85,0xba}};
static const GUID IID_AsyncOp_WebAccount = {0xacd76b54,0x297f,0x5a18,{0x91,0x43,0x20,0xa3,0x09,0xe2,0xdf,0xd3}};
static const GUID IID_AsyncOp_WebTokenRequestResult = {0x0a815852,0x7c44,0x5674,{0xb3,0xd2,0xfa,0x2e,0x4c,0x1e,0x46,0xc9}};
static const GUID IID_AsyncOp_FindAllAccountsResult = {0x9affb572,0x58c3,0x5c6c,{0x93,0x97,0x2b,0x77,0x04,0xaa,0x35,0xc3}};
static const GUID IID_IVectorView_WebTokenResponse = {0x199e065c,0x8195,0x55da,{0x9c,0x10,0x8a,0xea,0xf9,0xac,0x10,0x62}};
static const GUID IID_IVectorView_WebAccount = {0xe0798d3d,0x2b4a,0x589a,{0xab,0x12,0x02,0xdc,0xcc,0x15,0x8a,0xfc}};
static const GUID IID_IIterable_WebTokenResponse = {0x7e5bb7ec,0xbbd7,0x5575,{0x9a,0x61,0xf5,0x81,0x5f,0xa2,0x2a,0x0e}};
static const GUID IID_IIterator_WebTokenResponse = {0xf080b0c9,0xa095,0x5b3a,{0xa1,0xdc,0xd1,0x7e,0x7d,0x29,0x82,0xc7}};
static const GUID IID_IIterable_WebAccount = {0xcb15d439,0xa910,0x542a,{0x89,0xed,0x7c,0xfe,0x67,0x84,0x8a,0x83}};
static const GUID IID_IIterator_WebAccount = {0xbfb82cca,0xaebc,0x567c,{0x95,0xd9,0xeb,0xa2,0x5c,0x36,0x5f,0xaa}};
static const GUID IID_AsyncOp_RandomAccessStream = {0x430ecece,0x1418,0x5d19,{0x81,0xb2,0x5d,0xdb,0x38,0x16,0x03,0xcc}};
static const GUID IID_IRandomAccessStream_ = {0x905a0fe1,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
static const GUID IID_IBuffer_ = {0x905a0fe0,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
static const GUID IID_IBufferByteAccess_ = {0x905a0fef,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};
static const GUID IID_IOutputStream_ = {0x905a0fe6,0xbc53,0x11df,{0x8c,0x49,0x00,0x1e,0x4f,0xc6,0x86,0xda}};

#define MSA_PROVIDER_ID "https://login.microsoft.com"
/* the Xbox Live token provider a UWP title's XSAPI asks for: its requests name a Url and get an XSTS token */
#define XSTS_PROVIDER_ID "https://xsts.auth.xboxlive.com"

/* ---- the stored Microsoft account ---- */

/* The id of the Microsoft account's WebAccount (loading the stored account if this process has not signed in yet):
 * its PUID once that is known (fetch_account_puid), until then its user id; NULL when nobody is signed in. */
char *stored_msa_account( void )
{
    struct kv kv = {0};
    char *id = NULL;

    EnterCriticalSection( &account.cs );
    if (!account.refresh_token || !*account.refresh_token)
    {
        store_lock();
        store_read( &kv );
        store_unlock();
        str_set( &account.refresh_token, kv_get( &kv, "refresh_token" ) );
        if (!account.msa_user_id || !*account.msa_user_id) str_set( &account.msa_user_id, kv_get( &kv, "user_id" ) );
        kv_free( &kv );
    }
    if ((!account.puid || !*account.puid) && account.refresh_token && *account.refresh_token)
    {
        store_lock();
        store_read( &kv );
        store_unlock();
        str_set( &account.puid, kv_get( &kv, "puid" ) );
        kv_free( &kv );
    }
    if (account.refresh_token && *account.refresh_token)
        id = str_dup( account.puid && *account.puid ? account.puid : S(account.msa_user_id) );
    LeaveCriticalSection( &account.cs );
    if (id && !*id)
    {
        free( id );
        id = NULL;
    }
    return id;
}

/* ---- tokens for any scope, cached until a minute before they expire ---- */

struct scoped_token
{
    char *key, *token, *form;
    ULONGLONG expires;
};

static struct scoped_token *scoped_tokens;    /* guarded by account.signin_cs */
static unsigned scoped_token_count;

static struct scoped_token *find_scoped( const char *key )
{
    unsigned i;
    for (i = 0; i < scoped_token_count; i++) if (!strcmp( scoped_tokens[i].key, key )) return &scoped_tokens[i];
    return NULL;
}

void scoped_token_forget( const char *key )
{
    struct scoped_token *t;

    EnterCriticalSection( &account.signin_cs );
    if ((t = find_scoped( key )))
    {
        free( t->key );
        str_wipe_free( t->token );
        str_wipe_free( t->form );
        *t = scoped_tokens[--scoped_token_count];
    }
    LeaveCriticalSection( &account.signin_cs );
}

/* With oauth2_response=1 among a request's properties, WAM answers the whole OAuth response as form data (the Xbox
 * app's age verification reads an id_token out of it: "Invalid token form data" otherwise): the string and number
 * fields of the token endpoint's JSON answer, url-encoded. */
static char *json_to_form( const struct json *j )
{
    struct buf out = {0};
    unsigned i;

    for (i = 0; i < json_count( j ); i++)
    {
        char *k, *v;
        if (j->items[i].type != JSON_STRING && j->items[i].type != JSON_NUMBER) continue;
        k = urlencode( j->keys[i] );
        v = urlencode( json_str( &j->items[i] ) );
        if (out.n) buf_addc( &out, '&' );
        buf_printf( &out, "%s=%s", k, v );
        free( k );
        str_wipe_free( v );
    }
    return out.p ? out.p : str_dup( "" );
}

/* A refresh-token grant for scope (extra: more form fields, "&name=value"). form_out, if given, gets the answer as
 * form data; error, if given, the service's error code. */
HRESULT msa_token_for_scope( const char *scope, BOOL force, char **token, char **error, char **form_out, const char *extra )
{
    char *key = strf( "%s%s", scope, S(extra) ), *refresh = NULL, *enc_scope, *enc_refresh, *form;
    struct scoped_token *t;
    struct buf resp = {0};
    struct json *j;
    DWORD status;
    HRESULT hr = S_OK;

    *token = NULL;
    if (error) *error = NULL;
    if (form_out) *form_out = NULL;
    EnterCriticalSection( &account.signin_cs );
    if (!force && (t = find_scoped( key )) && t->expires > now_s())
    {
        *token = str_dup( t->token );
        if (form_out) *form_out = str_dup( t->form );
        LeaveCriticalSection( &account.signin_cs );
        free( key );
        return S_OK;
    }
    refresh = account_field( &account.refresh_token );
    if (!refresh || !*refresh)
    {
        LeaveCriticalSection( &account.signin_cs );
        str_wipe_free( refresh );
        free( key );
        return XA_E_NO_ACCOUNT;
    }

    enc_scope = urlencode( scope );
    enc_refresh = urlencode( refresh );
    form = strf( "grant_type=refresh_token&client_id=%s&scope=%s&refresh_token=%s%s", MSA_CLIENT_ID, enc_scope,
                 enc_refresh, S(extra) );
    status = http_form( "https://login.live.com/oauth20_token.srf", form, &resp );
    str_wipe_free( form );
    str_wipe_free( enc_refresh );
    str_wipe_free( refresh );
    free( enc_scope );
    j = json_parse( resp.p, resp.n );
    if (!status) hr = XA_E_NETWORK;
    else if (status != 200 || !*json_str( json_get( j, "access_token" ) ))
    {
        if (error) *error = str_dup( json_str( json_get( j, "error" ) ) );
        TRACE( "WebAuthenticationCoreManager: scope \"%s\": HTTP %lu, error \"%s\"\n", scope, status,
               json_str( json_get( j, "error" ) ) );
        hr = XA_E_MSA;
    }
    else
    {
        double expires_in = json_get( j, "expires_in" )->number;
        const char *new_refresh = json_str( json_get( j, "refresh_token" ) );
        struct buf fields = {0};
        unsigned i;

        if (!(t = find_scoped( key )))
        {
            struct scoped_token *grown = realloc( scoped_tokens, (scoped_token_count + 1) * sizeof(*grown) );
            if (grown)
            {
                scoped_tokens = grown;
                t = &scoped_tokens[scoped_token_count++];
                memset( t, 0, sizeof(*t) );
                t->key = str_dup( key );
            }
        }
        *token = str_dup( json_str( json_get( j, "access_token" ) ) );
        if (t)
        {
            str_set( &t->token, *token );
            str_wipe_free( t->form );
            t->form = json_to_form( j );
            t->expires = now_s() + (ULONGLONG)(expires_in > 120 ? expires_in - 60 : 60);
            if (form_out) *form_out = str_dup( t->form );
        }
        else if (form_out) *form_out = json_to_form( j );
        if (*new_refresh)
        {
            char *user_id;
            EnterCriticalSection( &account.cs );
            str_set( &account.refresh_token, new_refresh );
            user_id = str_dup( S(account.msa_user_id) );
            LeaveCriticalSection( &account.cs );
            store_update( "refresh_token", new_refresh, "user_id", user_id, NULL );
            free( user_id );
        }
        for (i = 0; i < json_count( j ); i++) buf_printf( &fields, "%s%s", i ? " " : "", j->keys[i] );
        TRACE( "WebAuthenticationCoreManager: scope \"%s\": token (%Iu chars, expires in %.0f s; answer fields: %s)\n",
               scope, strlen( *token ), expires_in, buf_str( &fields ) );
        buf_free( &fields );
    }
    json_free( j );
    buf_free( &resp );
    LeaveCriticalSection( &account.signin_cs );
    free( key );
    return hr;
}

/* The Microsoft account's e-mail address, for WebAccount.UserName (what Windows shows for a Microsoft account): the
 * preferred_username claim of an "openid profile" id_token, fetched once and kept in xboxauth.bin. */
void fetch_account_email( void )
{
    char *have = store_get( "email" ), *token = NULL, *form = NULL, *at, *jwt, *d1, *d2;
    struct buf claims_text = {0};
    struct json *claims;
    const char *email;
    HRESULT hr;

    if (have && *have)
    {
        free( have );
        return;
    }
    free( have );
    hr = msa_token_for_scope( "openid profile", FALSE, &token, NULL, &form, NULL );
    str_wipe_free( token );
    if (FAILED(hr) || !form || !(at = strstr( form, "id_token=" )))
    {
        TRACE( "Microsoft account e-mail address: none (hr %#lx)\n", hr );
        str_wipe_free( form );
        return;
    }
    jwt = at + 9;
    if ((at = strchr( jwt, '&' ))) *at = 0;
    if (!(d1 = strchr( jwt, '.' )) || !(d2 = strchr( d1 + 1, '.' )))
    {
        str_wipe_free( form );
        return;
    }
    *d2 = 0;
    unb64( d1 + 1, &claims_text );
    str_wipe_free( form );
    claims = json_parse( claims_text.p, claims_text.n );
    buf_free( &claims_text );
    email = json_str( json_get( claims, "preferred_username" ) );
    if (!*email) email = json_str( json_get( claims, "email" ) );
    if (*email)
    {
        store_update( "email", email, NULL );
        TRACE( "Microsoft account e-mail address stored (%Iu chars)\n", strlen( email ) );
    }
    json_free( claims );
}

/* The account's PUID. A Microsoft account WebAccount's Id is the PUID in hex, and the Xbox app checks the Store
 * account against the account its content access answer is for ("ContentAccessWrongPuid"). No Microsoft account
 * token carries it (the id_token's oid is the CID, like user_id), but the content access answer does: protobuf
 * field 3 (the user), its field 1, in decimal. Fetched once, with the user's Xbox Live token, and kept. */
static BOOL pb_varint( const unsigned char **p, const unsigned char *end, ULONGLONG *v )
{
    int shift;

    *v = 0;
    for (shift = 0; *p < end && shift < 64; shift += 7)
    {
        unsigned char c = *(*p)++;
        *v |= (ULONGLONG)(c & 0x7f) << shift;
        if (!(c & 0x80)) return TRUE;
    }
    return FALSE;
}

/* the first length-delimited field `field` of a message */
static BOOL pb_field( const unsigned char *p, const unsigned char *end, unsigned field, const unsigned char **out,
                      size_t *len )
{
    while (p < end)
    {
        ULONGLONG key, v;
        if (!pb_varint( &p, end, &key )) return FALSE;
        switch (key & 7)
        {
        case 0: if (!pb_varint( &p, end, &v )) return FALSE; break;
        case 1: p += 8; break;
        case 5: p += 4; break;
        case 2:
            if (!pb_varint( &p, end, &v ) || v > (ULONGLONG)(end - p)) return FALSE;
            if ((key >> 3) == field)
            {
                *out = p;
                *len = (size_t)v;
                return TRUE;
            }
            p += v;
            break;
        default: return FALSE;
        }
    }
    return FALSE;
}

void fetch_account_puid( void )
{
    const unsigned char *user, *id;
    unsigned char *body = NULL;
    char *have, dec[32], hex[17];
    size_t ulen, idlen;
    ULONGLONG puid;
    UINT32 len = 0;
    DWORD status = 0;

    EnterCriticalSection( &account.cs );
    have = str_dup( S(account.puid) );
    LeaveCriticalSection( &account.cs );
    if (*have)
    {
        free( have );
        return;
    }
    free( have );
    if ((have = store_get( "puid" )) && *have)
    {
        EnterCriticalSection( &account.cs );
        str_set( &account.puid, have );
        LeaveCriticalSection( &account.cs );
        free( have );
        return;
    }
    free( have );
    if (FAILED(XboxAuthHttpGet( "https://contentaccess.exp.xboxservices.com/all/v1?market=US&offering=XGPUPCAPP",
                                "MS-CV: wineuwpPuidFetch.0", "1", &body, &len, &status )) || status != 200)
    {
        TRACE( "account PUID: content access HTTP %lu\n", status );
        if (body) CoTaskMemFree( body );
        return;
    }
    dec[0] = 0;
    if (pb_field( body, body + len, 3, &user, &ulen ) && pb_field( user, user + ulen, 1, &id, &idlen ) &&
        idlen < sizeof(dec))
    {
        memcpy( dec, id, idlen );
        dec[idlen] = 0;
    }
    CoTaskMemFree( body );
    if (!(puid = _strtoui64( dec, NULL, 10 )))
    {
        TRACE( "account PUID: not in the content access answer\n" );
        return;
    }
    snprintf( hex, sizeof(hex), "%016I64X", puid );
    EnterCriticalSection( &account.cs );
    str_set( &account.puid, hex );
    LeaveCriticalSection( &account.cs );
    store_update( "puid", hex, NULL );
    TRACE( "account PUID stored\n" );
}

/* The user name a Microsoft account WebAccount shows: the account's e-mail address (fetch_account_email), or until
 * that is known the Xbox gamertag of the same account. */
static char *web_account_user_name( void )
{
    struct kv kv = {0};
    char *name;

    store_lock();
    store_read( &kv );
    store_unlock();
    if (*kv_get( &kv, "email" )) name = str_dup( kv_get( &kv, "email" ) );
    else
    {
        EnterCriticalSection( &account.cs );
        name = str_dup( S(account.gtg) );
        LeaveCriticalSection( &account.cs );
        if (!*name) str_set( &name, kv_get( &kv, "gamertag" ) );
        if (!*name) str_set( &name, "Microsoft account" );
    }
    kv_free( &kv );
    return name;
}

/* ---- WebAccountProvider ---- */

struct provider
{
    struct object obj;
    BOOL xsts;
    char *authority;
    IInspectable *user;
};

static struct provider *provider_from( void *iface, int n ) { return CONTAINING_RECORD( OBJ_AT(iface, n), struct provider, obj ); }

static void provider_destroy( struct object *obj )
{
    struct provider *p = CONTAINING_RECORD( obj, struct provider, obj );
    if (p->user) IInspectable_Release( p->user );
    free( p->authority );
}

static HRESULT WINAPI provider_get_Id( void *iface, HSTRING *v )
{
    return to_hstr( provider_from( iface, 0 )->xsts ? XSTS_PROVIDER_ID : MSA_PROVIDER_ID, v );
}
static HRESULT WINAPI provider_get_DisplayName( void *iface, HSTRING *v )
{
    return to_hstr( provider_from( iface, 0 )->xsts ? "Xbox Live" : "Microsoft account", v );
}
static HRESULT WINAPI provider_get_IconUri( void *iface, IInspectable **v )
{
    if (!v) return E_POINTER;
    *v = NULL;
    TRACE( "WebAccountProvider.IconUri -> null\n" );
    return S_OK;
}
static HRESULT WINAPI provider_get_DisplayPurpose( void *iface, HSTRING *v )
{
    if (!v) return E_POINTER;
    *v = NULL;
    return S_OK;
}
static HRESULT WINAPI provider_get_Authority( void *iface, HSTRING *v ) { return to_hstr( provider_from( iface, 1 )->authority, v ); }
static HRESULT WINAPI provider_get_User( void *iface, IInspectable **v )
{
    struct provider *p = provider_from( iface, 2 );
    if (!v) return E_POINTER;
    if ((*v = p->user)) IInspectable_AddRef( p->user );
    return S_OK;
}
static HRESULT WINAPI provider_get_IsSystemProvider( void *iface, boolean *v )
{
    if (!v) return E_POINTER;
    *v = FALSE;
    return S_OK;
}

static const void *provider_vtbl[] = { OBJ_METHODS(0), provider_get_Id, provider_get_DisplayName, provider_get_IconUri };
static const void *provider2_vtbl[] = { OBJ_METHODS(1), provider_get_DisplayPurpose, provider_get_Authority };
static const void *provider3_vtbl[] = { OBJ_METHODS(2), provider_get_User };
static const void *provider4_vtbl[] = { OBJ_METHODS(3), provider_get_IsSystemProvider };

static struct provider *provider_create( const char *authority, IInspectable *user )
{
    struct provider *p = object_create( sizeof(*p), L"Windows.Security.Credentials.WebAccountProvider",
                                        "WebAccountProvider", provider_destroy );
    if (!p) return NULL;
    object_add( &p->obj, provider_vtbl, &IID_IWebAccountProvider );
    object_add( &p->obj, provider2_vtbl, &IID_IWebAccountProvider2 );
    object_add( &p->obj, provider3_vtbl, &IID_IWebAccountProvider3 );
    object_add( &p->obj, provider4_vtbl, &IID_IWebAccountProvider4 );
    p->authority = str_dup( authority );
    if ((p->user = user)) IInspectable_AddRef( user );
    return p;
}

/* ---- WebAccount ---- */

struct web_account
{
    struct object obj;
    struct provider *provider;
    char *id, *user_name;
};

static struct web_account *web_account_from( void *iface, int n )
{
    return CONTAINING_RECORD( OBJ_AT(iface, n), struct web_account, obj );
}

static void web_account_destroy( struct object *obj )
{
    struct web_account *wa = CONTAINING_RECORD( obj, struct web_account, obj );
    obj_Release( &wa->provider->obj );
    free( wa->id );
    free( wa->user_name );
}

static HRESULT WINAPI web_account_get_WebAccountProvider( void *iface, void **v )
{
    struct web_account *wa = web_account_from( iface, 0 );
    if (!v) return E_POINTER;
    obj_AddRef( &wa->provider->obj );
    *v = &wa->provider->obj.vtbl[0];
    return S_OK;
}
static HRESULT WINAPI web_account_get_UserName( void *iface, HSTRING *v )
{
    TRACE( "WebAccount.UserName\n" );
    return to_hstr( web_account_from( iface, 0 )->user_name, v );
}
static HRESULT WINAPI web_account_get_State( void *iface, INT32 *v )
{
    if (!v) return E_POINTER;
    *v = 1;  /* Connected */
    return S_OK;
}
static HRESULT WINAPI web_account_get_Id( void *iface, HSTRING *v ) { return to_hstr( web_account_from( iface, 1 )->id, v ); }
static HRESULT WINAPI web_account_get_Properties( void *iface, IInspectable **v )
{
    struct web_account *wa = web_account_from( iface, 1 );
    IInspectable *map;
    HRESULT hr;

    if (!v) return E_POINTER;
    *v = NULL;
    TRACE( "WebAccount.Properties\n" );
    if (!(map = make_string_map( "Authority", wa->provider->authority, "UserName", wa->user_name, "CID", wa->id, NULL )))
        return E_OUTOFMEMORY;
    hr = SLOT(map, 9, HRESULT (WINAPI *)( void *, IInspectable ** ))( map, v );   /* GetView */
    IInspectable_Release( map );
    return hr;
}

/* A Windows.Storage.Streams.Buffer over bytes, for writing them into a stream */
struct byte_buffer
{
    struct object obj;
    BYTE *bytes;
    UINT32 size, length;
};

static struct byte_buffer *byte_buffer_from( void *iface, int n )
{
    return CONTAINING_RECORD( OBJ_AT(iface, n), struct byte_buffer, obj );
}
static void byte_buffer_destroy( struct object *obj ) { free( CONTAINING_RECORD( obj, struct byte_buffer, obj )->bytes ); }
static HRESULT WINAPI byte_buffer_get_Capacity( void *iface, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = byte_buffer_from( iface, 0 )->size;
    return S_OK;
}
static HRESULT WINAPI byte_buffer_get_Length( void *iface, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = byte_buffer_from( iface, 0 )->length;
    return S_OK;
}
static HRESULT WINAPI byte_buffer_put_Length( void *iface, UINT32 v )
{
    struct byte_buffer *b = byte_buffer_from( iface, 0 );
    if (v > b->size) return E_INVALIDARG;
    b->length = v;
    return S_OK;
}
static HRESULT WINAPI byte_buffer_Buffer( void *iface, BYTE **v )
{
    if (!v) return E_POINTER;
    *v = byte_buffer_from( iface, 1 )->bytes;
    return S_OK;
}

static const void *byte_buffer_vtbl[] = { OBJ_METHODS(0), byte_buffer_get_Capacity, byte_buffer_get_Length, byte_buffer_put_Length };
static const void *byte_buffer_access_vtbl[] = { UNK_METHODS(1), byte_buffer_Buffer };

/* A Windows.Storage.Streams.InMemoryRandomAccessStream holding bytes, positioned at 0. Wine's completes WriteAsync inline. */
HRESULT make_memory_stream( const void *bytes, size_t len, IInspectable **out )
{
    IInspectable *obj = NULL, *output = NULL, *ras = NULL, *write = NULL, *info = NULL;
    struct byte_buffer *buf;
    HSTRING cls = NULL;
    INT32 st = 0;
    HRESULT hr;
    int i;

    *out = NULL;
    to_hstrw( L"Windows.Storage.Streams.InMemoryRandomAccessStream", &cls );
    hr = RoActivateInstance( cls, &obj );
    WindowsDeleteString( cls );
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = IInspectable_QueryInterface( obj, &IID_IOutputStream_, (void **)&output )))
    {
        if (!(buf = object_create( sizeof(*buf), L"Windows.Storage.Streams.Buffer", NULL, byte_buffer_destroy )) ||
            !(buf->bytes = malloc( len ? len : 1 )))
        {
            free( buf );
            IInspectable_Release( output );
            IInspectable_Release( obj );
            return E_OUTOFMEMORY;
        }
        object_add( &buf->obj, byte_buffer_vtbl, &IID_IBuffer_ );
        object_add( &buf->obj, byte_buffer_access_vtbl, &IID_IBufferByteAccess_ );
        memcpy( buf->bytes, bytes, len );
        buf->size = buf->length = len;
        if (SUCCEEDED(hr = SLOT(output, 6, HRESULT (WINAPI *)( void *, void *, IInspectable ** ))( output,
                                                                     &buf->obj.vtbl[0], &write )))
        {
            if (SUCCEEDED(IInspectable_QueryInterface( write, &IID_IAsyncInfo_, (void **)&info )))
            {
                for (i = 0; i < 500; i++)
                {
                    if (FAILED(SLOT(info, 7, HRESULT (WINAPI *)( void *, INT32 * ))( info, &st )) || st) break;
                    Sleep( 10 );
                }
                IInspectable_Release( info );
            }
            IInspectable_Release( write );
            if (st != 1) hr = E_FAIL;
        }
        obj_Release( &buf->obj );
        IInspectable_Release( output );
    }
    if (SUCCEEDED(hr) && SUCCEEDED(hr = IInspectable_QueryInterface( obj, &IID_IRandomAccessStream_, (void **)&ras )))
    {
        SLOT(ras, 11, HRESULT (WINAPI *)( void *, UINT64 ))( ras, 0 );   /* Seek */
        *out = ras;
    }
    IInspectable_Release( obj );
    return hr;
}

/* The account picture: the Xbox gamer picture of the same account (profile setting GameDisplayPicRaw), the PNG
 * downloaded. The app's StoreAccountModule.GetProfile reads it, and without it reports no Store account at all. */
static BOOL gamer_picture( int size, struct buf *png )
{
    static const char url[] = "https://profile.xboxlive.com/users/me/profile/settings?settings=GameDisplayPicRaw";
    struct buf headers = {0}, resp = {0};
    char *header = NULL, *pic_url;
    struct json *j;
    const char *pic;
    DWORD status;

    if (FAILED(sign_in_silently()) || FAILED(get_token_for_url( url, FALSE, &header, TRUE ))) return FALSE;
    hdr_add( &headers, "Authorization: %s", header );
    hdr_add( &headers, "x-xbl-contract-version: 2" );
    hdr_add( &headers, "Accept: application/json" );
    add_signature( &headers, "GET", url, header, NULL, 0 );
    str_wipe_free( header );
    status = http_request( L"GET", url, buf_str( &headers ), NULL, 0, &resp );
    buf_free( &headers );
    j = json_parse( resp.p, resp.n );
    pic = json_str( json_get( json_at( json_get( json_at( json_get( j, "profileUsers" ), 0 ), "settings" ), 0 ), "value" ) );
    if (status != 200 || !*pic)
    {
        TRACE( "gamer picture: profile HTTP %lu, no picture\n", status );
        json_free( j );
        buf_free( &resp );
        return FALSE;
    }
    pic_url = strf( "%s%sw=%d&h=%d", pic, strchr( pic, '?' ) ? "&" : "?", size, size );
    json_free( j );
    buf_free( &resp );
    status = http_request( L"GET", pic_url, "Accept: image/png\r\n", NULL, 0, png );
    free( pic_url );
    TRACE( "gamer picture: HTTP %lu, %Iu bytes\n", status, png->n );
    if (status == 200) return TRUE;
    buf_free( png );
    return FALSE;
}

/* The gamer picture for the GDK's XUserGetGamerPictureAsync (xgameruntime): *png is CoTaskMemAlloc'ed. */
HRESULT WINAPI XboxAuthGamerPicture( int size, unsigned char **png, UINT32 *len )
{
    struct buf pic = {0};

    if (!png || !len) return E_POINTER;
    *png = NULL;
    *len = 0;
    account_init();
    if (!gamer_picture( size, &pic )) return XA_E_NETWORK;
    if (!(*png = CoTaskMemAlloc( pic.n ? pic.n : 1 )))
    {
        buf_free( &pic );
        return E_OUTOFMEMORY;
    }
    memcpy( *png, pic.p, pic.n );
    *len = pic.n;
    buf_free( &pic );
    return S_OK;
}

static HRESULT picture_work( struct async *op, void *ctx )
{
    /* if the picture cannot be fetched, a 1x1 transparent PNG stands in */
    static const unsigned char blank_png[] = {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,
        0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,0x00,0x00,0x00,0x0d,0x49,0x44,0x41,
        0x54,0x78,0x9c,0x63,0x00,0x01,0x00,0x00,0x05,0x00,0x01,0x0d,0x0a,0x2d,0xb4,0x00,0x00,0x00,0x00,0x49,
        0x45,0x4e,0x44,0xae,0x42,0x60,0x82 };
    int size = (int)(INT_PTR)ctx;
    struct buf png = {0};
    HRESULT hr;

    if (gamer_picture( size > 0 ? size : 208, &png ))
        hr = make_memory_stream( png.p, png.n, (IInspectable **)&op->iface );
    else
        hr = make_memory_stream( blank_png, sizeof(blank_png), (IInspectable **)&op->iface );
    buf_free( &png );
    return hr;
}

static HRESULT WINAPI web_account_GetPictureAsync( void *iface, INT32 size, void **v )
{
    struct async *op;

    if (!v) return E_POINTER;
    TRACE( "WebAccount.GetPictureAsync(%d)\n", size );
    if (!(op = async_start( &IID_AsyncOp_RandomAccessStream, R_IFACE, "WebAccount.GetPictureAsync", picture_work,
                            (void *)(INT_PTR)size, NULL )))
        return E_OUTOFMEMORY;
    *v = &op->obj.vtbl[0];
    return S_OK;
}

static HRESULT WINAPI web_account_SignOutAsync( void *iface, void **v )
{
    /* signing the Store out would sign Xbox out too: it is one account here. Accept and keep it. */
    struct async *op;

    TRACE( "WebAccount.SignOutAsync -> kept\n" );
    if (!v) return E_POINTER;
    if (!(op = async_create( &IID_IAsyncAction_, R_ACTION, "WebAccount.SignOutAsync" ))) return E_OUTOFMEMORY;
    async_finish( op, S_OK );
    *v = &op->obj.vtbl[0];
    return S_OK;
}
static HRESULT WINAPI web_account_SignOutAsync2( void *iface, HSTRING client, void **v ) { return web_account_SignOutAsync( iface, v ); }

static const void *web_account_vtbl[] =
{
    OBJ_METHODS(0), web_account_get_WebAccountProvider, web_account_get_UserName, web_account_get_State
};
static const void *web_account2_vtbl[] =
{
    OBJ_METHODS(1), web_account_get_Id, web_account_get_Properties, web_account_GetPictureAsync,
    web_account_SignOutAsync, web_account_SignOutAsync2
};

static struct web_account *web_account_create( struct provider *provider, const char *id, const char *user_name )
{
    struct web_account *wa = object_create( sizeof(*wa), L"Windows.Security.Credentials.WebAccount", "WebAccount",
                                            web_account_destroy );
    if (!wa) return NULL;
    object_add( &wa->obj, web_account_vtbl, &IID_IWebAccount );
    object_add( &wa->obj, web_account2_vtbl, &IID_IWebAccount2 );
    obj_AddRef( &provider->obj );
    wa->provider = provider;
    wa->id = str_dup( id );
    wa->user_name = str_dup( user_name );
    return wa;
}

static struct web_account *make_web_account( struct provider *provider )
{
    struct web_account *wa;
    char *id = stored_msa_account(), *name;

    if (!id) return NULL;
    name = web_account_user_name();
    wa = web_account_create( provider, id, name );
    free( name );
    free( id );
    return wa;
}

/* ---- WebProviderError ---- */

struct provider_error
{
    struct object obj;
    UINT32 code;
    char *message;
};

static struct provider_error *provider_error_from( void *iface ) { return CONTAINING_RECORD( OBJ_AT(iface, 0), struct provider_error, obj ); }
static void provider_error_destroy( struct object *obj ) { free( CONTAINING_RECORD( obj, struct provider_error, obj )->message ); }
static HRESULT WINAPI provider_error_get_ErrorCode( void *iface, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = provider_error_from( iface )->code;
    return S_OK;
}
static HRESULT WINAPI provider_error_get_ErrorMessage( void *iface, HSTRING *v ) { return to_hstr( provider_error_from( iface )->message, v ); }
static HRESULT WINAPI provider_error_get_Properties( void *iface, IInspectable **v )
{
    if (!v) return E_POINTER;
    *v = make_string_map( NULL );
    return *v ? S_OK : E_OUTOFMEMORY;
}

static const void *provider_error_vtbl[] =
{
    OBJ_METHODS(0), provider_error_get_ErrorCode, provider_error_get_ErrorMessage, provider_error_get_Properties
};

static struct provider_error *provider_error_create( UINT32 code, const char *message )
{
    struct provider_error *e = object_create( sizeof(*e), L"Windows.Security.Authentication.Web.Core.WebProviderError",
                                              "WebProviderError", provider_error_destroy );
    if (!e) return NULL;
    object_add( &e->obj, provider_error_vtbl, &IID_IWebProviderError );
    e->code = code;
    e->message = str_dup( message );
    return e;
}

/* ---- WebTokenResponse ---- */

struct token_response
{
    struct object obj;
    char *token;
    struct web_account *web_account;
    IInspectable *properties;
};

static struct token_response *token_response_from( void *iface ) { return CONTAINING_RECORD( OBJ_AT(iface, 0), struct token_response, obj ); }

static void token_response_destroy( struct object *obj )
{
    struct token_response *r = CONTAINING_RECORD( obj, struct token_response, obj );
    if (r->web_account) obj_Release( &r->web_account->obj );
    if (r->properties) IInspectable_Release( r->properties );
    str_wipe_free( r->token );
}

static HRESULT WINAPI token_response_get_Token( void *iface, HSTRING *v ) { return to_hstr( token_response_from( iface )->token, v ); }
static HRESULT WINAPI token_response_get_ProviderError( void *iface, void **v )
{
    if (!v) return E_POINTER;
    *v = NULL;
    return S_OK;
}
static HRESULT WINAPI token_response_get_WebAccount( void *iface, void **v )
{
    struct token_response *r = token_response_from( iface );
    if (!v) return E_POINTER;
    *v = NULL;
    if (r->web_account)
    {
        obj_AddRef( &r->web_account->obj );
        *v = &r->web_account->obj.vtbl[0];
    }
    return S_OK;
}
static HRESULT WINAPI token_response_get_Properties( void *iface, IInspectable **v )
{
    struct token_response *r = token_response_from( iface );
    if (!v) return E_POINTER;
    if ((*v = r->properties)) IInspectable_AddRef( r->properties );
    return r->properties ? S_OK : E_OUTOFMEMORY;
}

static const void *token_response_vtbl[] =
{
    OBJ_METHODS(0), token_response_get_Token, token_response_get_ProviderError, token_response_get_WebAccount,
    token_response_get_Properties
};

static struct token_response *token_response_create( const char *token, struct web_account *wa )
{
    struct token_response *r = object_create( sizeof(*r), L"Windows.Security.Authentication.Web.Core.WebTokenResponse",
                                              "WebTokenResponse", token_response_destroy );
    if (!r) return NULL;
    object_add( &r->obj, token_response_vtbl, &IID_IWebTokenResponse );
    r->token = str_dup( token );
    if ((r->web_account = wa)) obj_AddRef( &wa->obj );
    r->properties = make_string_map( "Authority", wa ? wa->provider->authority : "consumers", NULL );
    return r;
}

/* ---- WebTokenRequestResult ---- */

/* WebTokenRequestStatus */
enum { WTRS_SUCCESS = 0, WTRS_USER_CANCEL = 1, WTRS_USER_INTERACTION_REQUIRED = 3, WTRS_PROVIDER_NOT_AVAILABLE = 4,
       WTRS_PROVIDER_ERROR = 5 };

struct request_result
{
    struct object obj;
    INT32 status;
    struct vector_view *data;
    struct provider_error *error;
    char *scope;
};

static struct request_result *request_result_from( void *iface ) { return CONTAINING_RECORD( OBJ_AT(iface, 0), struct request_result, obj ); }

static void request_result_destroy( struct object *obj )
{
    struct request_result *r = CONTAINING_RECORD( obj, struct request_result, obj );
    if (r->data) obj_Release( &r->data->obj );
    if (r->error) obj_Release( &r->error->obj );
    free( r->scope );
}

static HRESULT WINAPI request_result_get_ResponseData( void *iface, void **v )
{
    struct request_result *r = request_result_from( iface );
    if (!v) return E_POINTER;
    obj_AddRef( &r->data->obj );
    *v = &r->data->obj.vtbl[0];
    return S_OK;
}
static HRESULT WINAPI request_result_get_ResponseStatus( void *iface, INT32 *v )
{
    if (!v) return E_POINTER;
    *v = request_result_from( iface )->status;
    return S_OK;
}
static HRESULT WINAPI request_result_get_ResponseError( void *iface, void **v )
{
    struct request_result *r = request_result_from( iface );
    if (!v) return E_POINTER;
    *v = NULL;
    if (r->error)
    {
        obj_AddRef( &r->error->obj );
        *v = &r->error->obj.vtbl[0];
    }
    return S_OK;
}
static HRESULT WINAPI request_result_InvalidateCacheAsync( void *iface, void **v )
{
    struct request_result *r = request_result_from( iface );
    struct async *op;

    if (!v) return E_POINTER;
    scoped_token_forget( r->scope );
    TRACE( "WebTokenRequestResult.InvalidateCacheAsync(\"%s\")\n", r->scope );
    if (!(op = async_create( &IID_IAsyncAction_, R_ACTION, "InvalidateCacheAsync" ))) return E_OUTOFMEMORY;
    async_finish( op, S_OK );
    *v = &op->obj.vtbl[0];
    return S_OK;
}

static const void *request_result_vtbl[] =
{
    OBJ_METHODS(0), request_result_get_ResponseData, request_result_get_ResponseStatus,
    request_result_get_ResponseError, request_result_InvalidateCacheAsync
};

static struct request_result *request_result_create( INT32 status, const char *scope )
{
    struct request_result *r = object_create( sizeof(*r), L"Windows.Security.Authentication.Web.Core.WebTokenRequestResult",
                                              "WebTokenRequestResult", request_result_destroy );
    if (!r) return NULL;
    object_add( &r->obj, request_result_vtbl, &IID_IWebTokenRequestResult );
    r->status = status;
    r->scope = str_dup( scope );
    if (!(r->data = vector_view_create( &IID_IVectorView_WebTokenResponse, &IID_IIterable_WebTokenResponse,
            &IID_IIterator_WebTokenResponse,
            L"Windows.Foundation.Collections.IVectorView`1<Windows.Security.Authentication.Web.Core.WebTokenResponse>" )))
    {
        obj_Release( &r->obj );
        return NULL;
    }
    return r;
}

/* ---- GetTokenSilentlyAsync / RequestTokenAsync: a token for the request's scope from the stored account ---- */

struct token_work
{
    char *scope, *extra;
    char *url, *method, *body;   /* an Xbox Live request's Url, HttpMethod and RequestBody */
    BOOL msa, xsts, force, ui, oauth2_response;
    IInspectable *account_in;
};

static void token_work_free( void *ctx )
{
    struct token_work *w = ctx;
    if (w->account_in) IInspectable_Release( w->account_in );
    free( w->scope );
    free( w->url );
    free( w->method );
    str_wipe_free( w->body );
    str_wipe_free( w->extra );
    free( w );
}

/* An Xbox Live request: the XSTS Authorization header for its Url as the token, and the user's claims and the request's
 * Signature as properties, which is what XSAPI reads (XboxUserId, Gamertag, AgeGroup, Privileges, Signature). */
static struct request_result *xsts_token_result( struct token_work *w )
{
    /* a Url with no host (XSAPI building one from a service endpoint it lacks) gets the default xboxlive.com token */
    const char *url = w->url && !_strnicmp( w->url, "http", 4 ) ? w->url : "https://xboxlive.com";
    char *header = NULL, *sig = NULL, *xid, *gtg, *agg, *prv;
    struct signing_policy policy;
    struct request_result *result;
    struct provider *provider;
    struct web_account *wa;
    struct token_response *resp;
    IInspectable *props;
    HRESULT hr;

    account_init();
    if (FAILED(hr = sign_in_silently()))
    {
        TRACE( "  Xbox Live: not signed in (%#lx)\n", hr );
        return request_result_create( w->ui ? WTRS_USER_CANCEL : WTRS_USER_INTERACTION_REQUIRED, w->scope );
    }
    if (FAILED(hr = get_token_for_url( url, w->force, &header, XSTS_WINDOWS_DEVICE )) || !header)
    {
        TRACE( "  Xbox Live: no token for %s (%#lx)\n", debugstr_an(url, strcspn( url, "?" )), hr );
        if (!(result = request_result_create( WTRS_PROVIDER_ERROR, w->scope ))) return NULL;
        result->error = provider_error_create( (UINT32)(FAILED(hr) ? hr : E_FAIL), "no Xbox Live token for this URL" );
        return result;
    }
    if (find_policy( url, &policy ))
    {
        struct url_parts u;
        split_url( url, &u );
        sig = sign_request( policy.version, policy.max_body_bytes, w->method && *w->method ? w->method : "GET",
                            u.path_and_query, header, w->body, w->body ? strlen( w->body ) : 0, 0 );
        url_parts_free( &u );
    }
    xid = account_field( &account.xid );
    gtg = account_field( &account.gtg );
    agg = account_field( &account.agg );
    prv = account_field( &account.prv );
    props = make_string_map( "XboxUserId", S(xid), "Gamertag", S(gtg), "AgeGroup", S(agg), "Privileges", S(prv),
                             "Signature", S(sig), "Environment", "prod", "Sandbox", "RETAIL", NULL );
    result = request_result_create( WTRS_SUCCESS, w->scope );
    provider = provider_create( "consumers", NULL );
    if (provider) provider->xsts = TRUE;
    wa = provider ? web_account_create( provider, S(xid), S(gtg) ) : NULL;
    if (result && props && (resp = token_response_create( header, wa )))
    {
        IInspectable_Release( resp->properties );
        resp->properties = props;
        props = NULL;
        vector_view_add( result->data, (IInspectable *)&resp->obj.vtbl[0] );
    }
    TRACE( "  Xbox Live token for %s (xuid %s, signature %s)\n", debugstr_an(url, strcspn( url, "?" )), S(xid),
           sig ? "yes" : "no" );
    if (props) IInspectable_Release( props );
    if (wa) obj_Release( &wa->obj );
    if (provider) obj_Release( &provider->obj );
    free( xid );
    free( gtg );
    free( agg );
    free( prv );
    free( sig );
    str_wipe_free( header );
    return result;
}

static HRESULT token_work( struct async *op, void *ctx )
{
    struct token_work *w = ctx;
    struct request_result *result;
    char *id = stored_msa_account();

    if (w->xsts) result = xsts_token_result( w );
    else if (!w->msa) result = request_result_create( WTRS_PROVIDER_NOT_AVAILABLE, w->scope );
    else if (!id) result = request_result_create( w->ui ? WTRS_USER_CANCEL : WTRS_USER_INTERACTION_REQUIRED, w->scope );
    else
    {
        char *token = NULL, *error = NULL, *form = NULL;
        HRESULT hr = msa_token_for_scope( w->scope, FALSE, &token, &error, &form, w->extra );

        if (SUCCEEDED(hr) && w->oauth2_response)
        {
            char *t = token;
            token = form;
            form = t;
        }
        str_wipe_free( form );
        if (SUCCEEDED(hr))
        {
            struct provider *provider = provider_create( "consumers", NULL );
            char *name = web_account_user_name();
            struct web_account *wa = provider ? web_account_create( provider, id, name ) : NULL;
            struct token_response *resp;

            free( name );
            if (provider) obj_Release( &provider->obj );
            result = request_result_create( WTRS_SUCCESS, w->scope );
            if (result && (resp = token_response_create( token, wa )))
                vector_view_add( result->data, (IInspectable *)&resp->obj.vtbl[0] );
            if (wa) obj_Release( &wa->obj );
        }
        else if (error && (!strcmp( error, "invalid_grant" ) || !strcmp( error, "interaction_required" )))
            result = request_result_create( WTRS_USER_INTERACTION_REQUIRED, w->scope );
        else
        {
            result = request_result_create( WTRS_PROVIDER_ERROR, w->scope );
            if (result)
                result->error = provider_error_create( (UINT32)hr, error && *error ? error
                                                       : "the Microsoft account service refused" );
        }
        str_wipe_free( token );
        free( error );
    }
    free( id );
    if (!result) return E_OUTOFMEMORY;
    TRACE( "  -> status %d\n", result->status );
    op->iface = (IUnknown *)&result->obj.vtbl[0];
    return S_OK;
}

static HRESULT get_web_token( IInspectable *request, IInspectable *account_in, BOOL ui, void **out )
{
    IInspectable *req = NULL, *pv = NULL, *p = NULL, *map = NULL;
    char *scope = NULL, *client = NULL, *provider_id = NULL, *props = NULL, *claims, *wrapped;
    struct token_work *w;
    struct async *op;
    HSTRING h = NULL;
    INT32 prompt = 0;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!request || FAILED(IInspectable_QueryInterface( request, &IID_IWebTokenRequest, (void **)&req ))) return E_INVALIDARG;
    if (SUCCEEDED(SLOT(req, 7, HRESULT (WINAPI *)( void *, HSTRING * ))( req, &h )))
    {
        scope = from_hstr( h );
        WindowsDeleteString( h );
        h = NULL;
    }
    if (SUCCEEDED(SLOT(req, 8, HRESULT (WINAPI *)( void *, HSTRING * ))( req, &h )))
    {
        client = from_hstr( h );
        WindowsDeleteString( h );
        h = NULL;
    }
    SLOT(req, 9, HRESULT (WINAPI *)( void *, INT32 * ))( req, &prompt );
    if (SUCCEEDED(SLOT(req, 6, HRESULT (WINAPI *)( void *, IInspectable ** ))( req, &pv )) && pv)
    {
        if (SUCCEEDED(IInspectable_QueryInterface( pv, &IID_IWebAccountProvider, (void **)&p )))
        {
            if (SUCCEEDED(SLOT(p, 6, HRESULT (WINAPI *)( void *, HSTRING * ))( p, &h )))
            {
                provider_id = from_hstr( h );
                WindowsDeleteString( h );
                h = NULL;
            }
            IInspectable_Release( p );
        }
        IInspectable_Release( pv );
    }
    if (!(w = calloc( 1, sizeof(*w) ))) goto oom;
    if (SUCCEEDED(SLOT(req, 10, HRESULT (WINAPI *)( void *, IInspectable ** ))( req, &map )) && map)
    {
        props = describe_map( map );
        /* "claims" (an id_token claims request, e.g. the age verification's xms_agvs) goes to the token endpoint too */
        if ((claims = map_lookup( map, L"claims" )))
        {
            char *enc = urlencode( claims );
            w->extra = strf( "&claims=%s", enc );
            free( enc );
            free( claims );
        }
        w->url = map_lookup( map, L"Url" );
        w->method = map_lookup( map, L"HttpMethod" );
        w->body = map_lookup( map, L"RequestBody" );
        if ((wrapped = map_lookup( map, L"ForceRefresh" )))
        {
            w->force = !_stricmp( wrapped, "true" ) || !strcmp( wrapped, "1" );
            free( wrapped );
        }
        IInspectable_Release( map );
    }
    IInspectable_Release( req );
    TRACE( "WebAuthenticationCoreManager.%s(provider \"%s\", scope \"%s\", client \"%s\", prompt %d, account %s, "
           "properties {%s})\n", ui ? "RequestTokenAsync" : "GetTokenSilentlyAsync", S(provider_id), S(scope), S(client),
           prompt, account_in ? "given" : "none", S(props) );

    w->scope = str_dup( S(scope) );
    w->msa = provider_id && !_stricmp( provider_id, MSA_PROVIDER_ID );
    w->xsts = provider_id && !_stricmp( provider_id, XSTS_PROVIDER_ID );
    w->ui = ui;
    wrapped = strf( ", %s,", S(props) );
    w->oauth2_response = wrapped && strstr( wrapped, ", oauth2_response=1," ) != NULL;
    free( wrapped );
    if ((w->account_in = account_in)) IInspectable_AddRef( account_in );
    free( scope );
    free( client );
    free( provider_id );
    free( props );
    if (!(op = async_start( &IID_AsyncOp_WebTokenRequestResult, R_IFACE, ui ? "RequestTokenAsync" : "GetTokenSilentlyAsync",
                            token_work, w, token_work_free )))
        return E_OUTOFMEMORY;
    *out = &op->obj.vtbl[0];
    return S_OK;

oom:
    IInspectable_Release( req );
    free( scope );
    free( client );
    free( provider_id );
    return E_OUTOFMEMORY;
}

static HRESULT find_provider( HSTRING id, HSTRING authority, IInspectable *user, void **out )
{
    char *pid = from_hstr( id ), *auth = authority ? from_hstr( authority ) : str_dup( "consumers" );
    BOOL msa = pid && !_stricmp( pid, MSA_PROVIDER_ID ) && (!auth || !*auth || !_stricmp( auth, "consumers" ));
    BOOL xsts = pid && !_stricmp( pid, XSTS_PROVIDER_ID );
    struct provider *p = NULL;
    HRESULT hr;

    TRACE( "WebAuthenticationCoreManager.FindAccountProviderAsync(\"%s\", \"%s\", user %s) -> %s\n", S(pid), S(auth),
           user ? "given" : "none", msa ? "Microsoft account" : xsts ? "Xbox Live" : "none" );
    if (msa || xsts) p = provider_create( auth && *auth ? auth : "consumers", user );
    if (p) p->xsts = xsts;
    hr = completed_iface( &IID_AsyncOp_WebAccountProvider, "FindAccountProviderAsync",
                          p ? (IUnknown *)&p->obj.vtbl[0] : NULL, out );
    free( pid );
    free( auth );
    return hr;
}

/* a provider object of the app's making (WebAccountProvider's own constructor) is answered as ours */
static struct provider *our_provider( IInspectable *provider )
{
    IInspectable *p = NULL;
    HSTRING h = NULL;
    char *id = NULL;
    BOOL ours, xsts;

    if (!provider || FAILED(IInspectable_QueryInterface( provider, &IID_IWebAccountProvider, (void **)&p ))) return NULL;
    if (SUCCEEDED(SLOT(p, 6, HRESULT (WINAPI *)( void *, HSTRING * ))( p, &h )))
    {
        id = from_hstr( h );
        WindowsDeleteString( h );
    }
    IInspectable_Release( p );
    ours = id && !_stricmp( id, MSA_PROVIDER_ID );
    xsts = id && !_stricmp( id, XSTS_PROVIDER_ID );
    free( id );
    if (!ours && !xsts) return NULL;
    if ((p = (IInspectable *)provider_create( "consumers", NULL ))) ((struct provider *)p)->xsts = xsts;
    return (struct provider *)p;
}

/* ---- FindAllAccountsResult ---- */

struct all_accounts
{
    struct object obj;
    struct vector_view *accounts;
};

static struct all_accounts *all_accounts_from( void *iface ) { return CONTAINING_RECORD( OBJ_AT(iface, 0), struct all_accounts, obj ); }
static void all_accounts_destroy( struct object *obj )
{
    struct all_accounts *r = CONTAINING_RECORD( obj, struct all_accounts, obj );
    if (r->accounts) obj_Release( &r->accounts->obj );
}
static HRESULT WINAPI all_accounts_get_Accounts( void *iface, void **v )
{
    struct all_accounts *r = all_accounts_from( iface );
    if (!v) return E_POINTER;
    obj_AddRef( &r->accounts->obj );
    *v = &r->accounts->obj.vtbl[0];
    return S_OK;
}
static HRESULT WINAPI all_accounts_get_Status( void *iface, INT32 *v )
{
    if (!v) return E_POINTER;
    *v = 0;  /* Success */
    return S_OK;
}
static HRESULT WINAPI all_accounts_get_ProviderError( void *iface, void **v )
{
    if (!v) return E_POINTER;
    *v = NULL;
    return S_OK;
}

static const void *all_accounts_vtbl[] =
{
    OBJ_METHODS(0), all_accounts_get_Accounts, all_accounts_get_Status, all_accounts_get_ProviderError
};

/* ---- the factory: IWebAuthenticationCoreManagerStatics, Statics2, Statics4 ---- */

static HRESULT WINAPI wacm_ActivateInstance( void *iface, IInspectable **out )
{
    if (out) *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI wacm_GetTokenSilentlyAsync( void *iface, IInspectable *r, void **o ) { return get_web_token( r, NULL, FALSE, o ); }
static HRESULT WINAPI wacm_GetTokenSilentlyWithAccountAsync( void *iface, IInspectable *r, IInspectable *a, void **o )
{ return get_web_token( r, a, FALSE, o ); }
static HRESULT WINAPI wacm_RequestTokenAsync( void *iface, IInspectable *r, void **o ) { return get_web_token( r, NULL, TRUE, o ); }
static HRESULT WINAPI wacm_RequestTokenWithAccountAsync( void *iface, IInspectable *r, IInspectable *a, void **o )
{ return get_web_token( r, a, TRUE, o ); }

static HRESULT WINAPI wacm_FindAccountAsync( void *iface, IInspectable *provider, HSTRING id, void **out )
{
    char *want = from_hstr( id ), *have = stored_msa_account(), *cid;
    struct provider *p = our_provider( provider );
    struct web_account *wa = NULL;
    HRESULT hr;

    EnterCriticalSection( &account.cs );
    cid = str_dup( S(account.msa_user_id) );
    LeaveCriticalSection( &account.cs );
    if (p && p->xsts)
    {
        /* XSAPI keeps the Xbox Live account by its id, the xuid, and takes "not found" for a sign-out */
        char *xid = account_field( &account.xid ), *gtg = account_field( &account.gtg );
        if (want && *S(xid) && !strcmp( want, xid )) wa = web_account_create( p, xid, S(gtg) );
        free( xid );
        free( gtg );
    }
    else if (p && have && want && (!_stricmp( want, have ) || !_stricmp( want, cid ))) wa = make_web_account( p );
    TRACE( "WebAuthenticationCoreManager.FindAccountAsync(%s) -> %s\n", want && *want ? "an id" : "\"\"",
           wa ? "the Microsoft account" : !have ? "none (not signed in)" : p ? "none (another account)" : "none (provider)" );
    if (p) obj_Release( &p->obj );
    free( want );
    free( have );
    free( cid );
    hr = completed_iface( &IID_AsyncOp_WebAccount, "FindAccountAsync", wa ? (IUnknown *)&wa->obj.vtbl[0] : NULL, out );
    return hr;
}
static HRESULT WINAPI wacm_FindAccountProviderAsync( void *iface, HSTRING id, void **out ) { return find_provider( id, NULL, NULL, out ); }
static HRESULT WINAPI wacm_FindAccountProviderWithAuthorityAsync( void *iface, HSTRING id, HSTRING a, void **out )
{ return find_provider( id, a, NULL, out ); }
static HRESULT WINAPI wacm_FindAccountProviderWithAuthorityForUserAsync( void *iface, HSTRING id, HSTRING a,
                                                                          IInspectable *u, void **out )
{ return find_provider( id, a, u, out ); }

static HRESULT WINAPI wacm_FindAllAccountsAsync( void *iface, IInspectable *provider, void **out )
{
    struct all_accounts *r = object_create( sizeof(*r), L"Windows.Security.Authentication.Web.Core.FindAllAccountsResult",
                                            "FindAllAccountsResult", all_accounts_destroy );
    struct provider *p;
    struct web_account *wa;

    if (!r) return E_OUTOFMEMORY;
    object_add( &r->obj, all_accounts_vtbl, &IID_IFindAllAccountsResult );
    if (!(r->accounts = vector_view_create( &IID_IVectorView_WebAccount, &IID_IIterable_WebAccount, &IID_IIterator_WebAccount,
            L"Windows.Foundation.Collections.IVectorView`1<Windows.Security.Credentials.WebAccount>" )))
    {
        obj_Release( &r->obj );
        return E_OUTOFMEMORY;
    }
    p = our_provider( provider );
    if (p && (wa = make_web_account( p ))) vector_view_add( r->accounts, (IInspectable *)&wa->obj.vtbl[0] );
    if (p) obj_Release( &p->obj );
    TRACE( "WebAuthenticationCoreManager.FindAllAccountsAsync -> %u\n", r->accounts->count );
    return completed_iface( &IID_AsyncOp_FindAllAccountsResult, "FindAllAccountsAsync", (IUnknown *)&r->obj.vtbl[0], out );
}
static HRESULT WINAPI wacm_FindAllAccountsWithClientIdAsync( void *iface, IInspectable *provider, HSTRING client, void **out )
{ return wacm_FindAllAccountsAsync( iface, provider, out ); }
static HRESULT WINAPI wacm_FindSystemAccountProviderAsync( void *iface, HSTRING id, void **out ) { return find_provider( id, NULL, NULL, out ); }
static HRESULT WINAPI wacm_FindSystemAccountProviderWithAuthorityAsync( void *iface, HSTRING id, HSTRING a, void **out )
{ return find_provider( id, a, NULL, out ); }
static HRESULT WINAPI wacm_FindSystemAccountProviderWithAuthorityForUserAsync( void *iface, HSTRING id, HSTRING a,
                                                                                IInspectable *u, void **out )
{ return find_provider( id, a, u, out ); }

static const void *wacm_activation_vtbl[] = { OBJ_METHODS(0), wacm_ActivateInstance };
static const void *wacm_statics_vtbl[] =
{
    OBJ_METHODS(1), wacm_GetTokenSilentlyAsync, wacm_GetTokenSilentlyWithAccountAsync, wacm_RequestTokenAsync,
    wacm_RequestTokenWithAccountAsync, wacm_FindAccountAsync, wacm_FindAccountProviderAsync,
    wacm_FindAccountProviderWithAuthorityAsync
};
static const void *wacm_statics2_vtbl[] = { OBJ_METHODS(2), wacm_FindAccountProviderWithAuthorityForUserAsync };
static const void *wacm_statics4_vtbl[] =
{
    OBJ_METHODS(3), wacm_FindAllAccountsAsync, wacm_FindAllAccountsWithClientIdAsync, wacm_FindSystemAccountProviderAsync,
    wacm_FindSystemAccountProviderWithAuthorityAsync, wacm_FindSystemAccountProviderWithAuthorityForUserAsync
};

static struct object wacm_factory;

IActivationFactory *web_auth_manager_factory( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending = FALSE;

    if (InitOnceBeginInitialize( &once, 0, &pending, NULL ) && pending)
    {
        object_init_static( &wacm_factory, L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager",
                            "WebAuthenticationCoreManager" );
        object_add( &wacm_factory, wacm_activation_vtbl, &IID_IActivationFactory );
        object_add( &wacm_factory, wacm_statics_vtbl, &IID_IWACMStatics );
        object_add( &wacm_factory, wacm_statics2_vtbl, &IID_IWACMStatics2 );
        object_add( &wacm_factory, wacm_statics4_vtbl, &IID_IWACMStatics4 );
        InitOnceComplete( &once, 0, NULL );
    }
    return (IActivationFactory *)&wacm_factory.vtbl[0];
}

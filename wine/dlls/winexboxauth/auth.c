/*
 * Xbox sign-in: the Microsoft account, the device and Xbox Live tokens
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
 * On Windows the Xbox app signs in through Gaming Services, whose NT service holds the user's Microsoft account and
 * Xbox Live tokens; that service cannot run under Wine. This signs in itself, with no embedded browser:
 *   1. Microsoft account, OAuth device-code flow: login.live.com/oauth20_connect.srf hands out a user code, the
 *      user's own browser is opened at https://www.microsoft.com/link?otc=<code> (WINE_UWP_LAUNCHER=log only logs
 *      it), and oauth20_token.srf is polled until the user has approved. The refresh token is kept, DPAPI-protected,
 *      in %LOCALAPPDATA%\wineuwp\xboxauth.bin, so the next sign-in is silent, in every process of the package.
 *   2. Xbox Live: SISU authorization (sisu.xboxlive.com/authorize) gives user, title and XSTS tokens, or
 *      user.auth.xboxlive.com and xsts.auth.xboxlive.com when SISU refuses. The xui claims give the user hash,
 *      XUID, gamertag, age group and privileges.
 *   3. A token for a URL takes the relying party from the title endpoints table
 *      (title.mgt.xboxlive.com/titles/default/endpoints?type=1, fetched once): "XBL3.0 x=<uhs>;<xsts>", and a
 *      Signature whenever the endpoint has a signature policy.
 *   4. The device: an ECDSA P-256 proof key made once per prefix (kept in xboxauth.bin), a device token from
 *      device.auth.xboxlive.com (ProofOfPossession), and the Xbox "Signature" header on every signed request.
 *
 * The device part is ported from xal-rs (https://github.com/OpenXbox/xal-rs, fork xodus-gaming/xal-rs; MIT License,
 * Copyright (c) 2023 Team OpenXbox): src/request_signer.rs (RequestSigner::prehash_message_data, sign_raw,
 * SignaturePolicyCache::find_policy_for_url), src/models.rs (ProofKey) and src/authenticator.rs (get_device_token,
 * sisu_authorize, get_xsts_token). A signature is
 *   base64( policy version (4 bytes BE) | FILETIME (8 bytes BE) | ECDSA r|s (64 bytes) )
 * over SHA-256 of  version\0 filetime\0 METHOD\0 path?query\0 authorization\0 body[:max]\0.
 *
 * Tokens are never written to the log: traces carry HTTP statuses, error codes and lengths only.
 */

#include "private.h"
#include "shellapi.h"

WINE_DEFAULT_DEBUG_CHANNEL(xboxauth);

struct account account;
struct device device;

static CRITICAL_SECTION title_ctx_cs;
static char *title_ctx_msa_app;     /* GDK titles pass MSAAppId+TitleId; presence/XSTS need a TitleToken with that tid */
static UINT32 title_ctx_title_id;

static BOOL WINAPI init_locks( INIT_ONCE *once, void *param, void **context )
{
    InitializeCriticalSection( &account.cs );
    InitializeCriticalSection( &account.signin_cs );
    InitializeCriticalSection( &device.cs );
    InitializeCriticalSection( &title_ctx_cs );
    return TRUE;
}

void account_init( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce( &once, init_locks, NULL, NULL );
}

char *account_field( char **field )
{
    char *ret;
    EnterCriticalSection( &account.cs );
    ret = str_dup( *field );
    LeaveCriticalSection( &account.cs );
    return ret;
}

void title_context( char **msa_app, UINT32 *title_id )
{
    account_init();
    EnterCriticalSection( &title_ctx_cs );
    *msa_app = str_dup( title_ctx_msa_app );
    *title_id = title_ctx_title_id;
    LeaveCriticalSection( &title_ctx_cs );
}

/* Background: binds the title identity as soon as SetTitleContext runs, so Party/ListPartyQos does not start under
 * a cached title-less XSTS while the warm GetToken is still in flight. */
static DWORD WINAPI title_ctx_bind_thunk( void *param )
{
    char *app;
    UINT32 tid;
    HRESULT hr;

    title_context( &app, &tid );
    if (app && *app && tid && strcmp( app, MSA_CLIENT_ID ))
    {
        hr = ensure_title_identity( app, tid );
        TRACE( "title context early bind: app=%s tid=%u -> %#lx\n", app, tid, hr );
    }
    free( app );
    return 0;
}

/* GDK / xgameruntime: binds later GetToken XSTS TitleTokens to this MSAAppId+TitleId. An empty msa_app_id clears
 * the binding (the Xbox app's default title). */
HRESULT WINAPI XboxAuthSetTitleContext( const char *msa_app_id, UINT32 title_id )
{
    BOOL gdk;

    account_init();
    EnterCriticalSection( &title_ctx_cs );
    str_set( &title_ctx_msa_app, S(msa_app_id) );
    title_ctx_title_id = title_id;
    LeaveCriticalSection( &title_ctx_cs );
    gdk = msa_app_id && *msa_app_id && strcmp( msa_app_id, MSA_CLIENT_ID );
    /* GDK PC titles: pin Win32 so SISU's TitleToken and later XSTS share one device identity */
    SetEnvironmentVariableA( "WINE_UWP_XBOX_DEVICE_TYPE", gdk ? "Win32" : "Android" );
    /* drop any title-less XSTS cached before the title context existed; the next one carries the TitleToken */
    if (gdk && title_id)
    {
        EnterCriticalSection( &account.cs );
        xsts_clear();
        LeaveCriticalSection( &account.cs );
        QueueUserWorkItem( title_ctx_bind_thunk, NULL, WT_EXECUTELONGFUNCTION );
    }
    TRACE( "title context set: msa app %s, title %#x\n", S(msa_app_id), title_id );
    return S_OK;
}

/* ---- the XSTS token cache, guarded by account.cs ---- */

void xsts_clear( void )
{
    unsigned i;
    for (i = 0; i < account.xsts_count; i++)
    {
        free( account.xsts[i].key );
        str_wipe_free( account.xsts[i].token );
    }
    free( account.xsts );
    account.xsts = NULL;
    account.xsts_count = 0;
}

static struct xsts *xsts_find( const char *key )
{
    unsigned i;
    for (i = 0; i < account.xsts_count; i++) if (!strcmp( account.xsts[i].key, key )) return &account.xsts[i];
    return NULL;
}

static void xsts_put( const char *key, const char *token, ULONGLONG expires )
{
    struct xsts *x = xsts_find( key ), *grown;

    if (!x)
    {
        if (!(grown = realloc( account.xsts, (account.xsts_count + 1) * sizeof(*grown) ))) return;
        account.xsts = grown;
        x = &account.xsts[account.xsts_count++];
        x->key = str_dup( key );
        x->token = NULL;
    }
    str_set( &x->token, token );
    x->expires = expires;
}

/* ---- the device: proof key, request signing and the Xbox Live device token ---- */

/* RequestSigner::prehash_message_data */
void sign_prehash_input( struct buf *m, const unsigned char version[4], const unsigned char filetime[8], const char *method,
                         const char *path_and_query, const char *authorization, const void *body, size_t body_len,
                         ULONGLONG max_body_bytes )
{
    const char *p;

    buf_addn( m, version, 4 );
    buf_addc( m, 0 );
    buf_addn( m, filetime, 8 );
    buf_addc( m, 0 );
    for (p = S(method); *p; p++) buf_addc( m, toupper( (unsigned char)*p ) );
    buf_addc( m, 0 );
    buf_add( m, path_and_query );
    buf_addc( m, 0 );
    buf_add( m, authorization );
    buf_addc( m, 0 );
    buf_addn( m, body, body_len < max_body_bytes ? body_len : (size_t)max_body_bytes );
    buf_addc( m, 0 );
}

static BOOL device_take_key( BCRYPT_KEY_HANDLE key )
{
    unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    ULONG len = 0;

    if (!BCRYPT_SUCCESS( BCryptExportKey( key, NULL, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &len, 0 ) ) ||
        len != sizeof(blob))
        return FALSE;
    device.key = key;
    free( device.x );
    free( device.y );
    device.x = b64( blob + sizeof(BCRYPT_ECCKEY_BLOB), 32, TRUE );
    device.y = b64( blob + sizeof(BCRYPT_ECCKEY_BLOB) + 32, 32, TRUE );
    return TRUE;
}

/* Loads the proof key, device id and device token from the store, or makes and stores a new key. */
BOOL device_load( void )
{
    BCRYPT_KEY_HANDLE key = NULL;
    struct buf priv = {0};
    struct kv kv;

    account_init();
    EnterCriticalSection( &device.cs );
    if (device.key)
    {
        LeaveCriticalSection( &device.cs );
        return TRUE;
    }
    store_lock();
    store_read( &kv );
    unb64( kv_get( &kv, "proof_key" ), &priv );
    if (priv.n == sizeof(BCRYPT_ECCKEY_BLOB) + 96 &&
        BCRYPT_SUCCESS( BCryptImportKeyPair( BCRYPT_ECDSA_P256_ALG_HANDLE, NULL, BCRYPT_ECCPRIVATE_BLOB, &key,
                                             (UCHAR *)priv.p, priv.n, 0 ) ) &&
        device_take_key( key ))
    {
        str_set( &device.id, kv_get( &kv, "device_id" ) );
        str_set( &device.token, kv_get( &kv, "device_token" ) );
        device.token_expires = _strtoui64( kv_get( &kv, "device_expires" ), NULL, 10 );
        TRACE( "proof key loaded\n" );
    }
    else
    {
        unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96];
        ULONG len = 0;
        char id[40], *enc;
        GUID g;

        if (key) BCryptDestroyKey( key );
        key = NULL;
        if (!BCRYPT_SUCCESS( BCryptGenerateKeyPair( BCRYPT_ECDSA_P256_ALG_HANDLE, &key, 256, 0 ) ) ||
            !BCRYPT_SUCCESS( BCryptFinalizeKeyPair( key, 0 ) ) ||
            !BCRYPT_SUCCESS( BCryptExportKey( key, NULL, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof(blob), &len, 0 ) ) ||
            len != sizeof(blob) || !device_take_key( key ) || FAILED(CoCreateGuid( &g )))
        {
            WARN( "no proof key: ECDSA P-256 key generation failed\n" );
            if (key) BCryptDestroyKey( key );
            kv_free( &kv );
            buf_free( &priv );
            store_unlock();
            LeaveCriticalSection( &device.cs );
            return FALSE;
        }
        snprintf( id, sizeof(id), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7] );
        str_set( &device.id, id );
        str_set( &device.token, "" );
        device.token_expires = 0;
        enc = b64( blob, sizeof(blob), FALSE );
        kv_set( &kv, "proof_key", enc );
        kv_set( &kv, "device_id", device.id );
        kv_set( &kv, "device_token", "" );
        kv_set( &kv, "device_expires", "" );
        store_write( &kv );
        str_wipe_free( enc );
        SecureZeroMemory( blob, sizeof(blob) );
        TRACE( "proof key created\n" );
    }
    kv_free( &kv );
    buf_free( &priv );
    store_unlock();
    LeaveCriticalSection( &device.cs );
    return TRUE;
}

/* ProofKey, as JSON (xal-rs field order) */
char *proof_key_json( void )
{
    return strf( "{\"alg\":\"ES256\",\"crv\":\"P-256\",\"kty\":\"EC\",\"use\":\"sig\",\"x\":\"%s\",\"y\":\"%s\"}",
                 S(device.x), S(device.y) );
}

/* RequestSigner::sign_raw. filetime 0: now. Returns the Signature header value, NULL on failure. */
char *sign_request( int policy_version, ULONGLONG max_body_bytes, const char *method, const char *path_and_query,
                    const char *authorization, const void *body, size_t body_len, ULONGLONG filetime )
{
    unsigned char out[4 + 8 + 64], hash[32];
    struct buf m = {0};
    ULONG len = 0;
    int i;
    BOOL ok;

    if (!device_load()) return NULL;
    if (!filetime)
    {
        FILETIME ft;
        GetSystemTimeAsFileTime( &ft );
        filetime = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }
    for (i = 0; i < 4; i++) out[i] = (unsigned)policy_version >> (24 - 8 * i);
    for (i = 0; i < 8; i++) out[4 + i] = filetime >> (56 - 8 * i);
    sign_prehash_input( &m, out, out + 4, method, path_and_query, authorization, body, body_len, max_body_bytes );
    ok = sha256( m.p, m.n, hash ) &&
         BCRYPT_SUCCESS( BCryptSignHash( device.key, NULL, hash, 32, out + 12, 64, &len, 0 ) ) && len == 64;
    buf_free( &m );
    if (!ok)
    {
        WARN( "request signing failed\n" );
        return NULL;
    }
    return b64( out, sizeof(out), FALSE );
}

/* scheme, host (lower case), path and path?query of a URL */
void split_url( const char *url, struct url_parts *u )
{
    const char *p = strstr( S(url), "://" ), *h, *slash, *rest;
    char *host, *at, *colon, *hash;

    memset( u, 0, sizeof(*u) );
    if (!p)
    {
        u->scheme = str_dup( "" );
        u->host = str_dup( "" );
        u->path = str_dup( "" );
        u->path_and_query = str_dup( "" );
        return;
    }
    u->scheme = str_lower( str_dupn( url, p - url ) );
    h = p + 3;
    slash = h + strcspn( h, "/?#" );
    host = str_dupn( h, slash - h );
    if ((at = strrchr( host, '@' ))) memmove( host, at + 1, strlen( at + 1 ) + 1 );
    if ((colon = strchr( host, ':' ))) *colon = 0;
    u->host = str_lower( host );
    if (!strcmp( u->scheme, "wss" )) str_set( &u->scheme, "https" );
    else if (!strcmp( u->scheme, "ws" )) str_set( &u->scheme, "http" );
    rest = slash;
    if (*rest == '/') u->path_and_query = str_dup( rest );
    else u->path_and_query = strf( "/%s", rest );
    if ((hash = strchr( u->path_and_query, '#' ))) *hash = 0;
    u->path = str_dupn( u->path_and_query, strcspn( u->path_and_query, "?" ) );
}

void url_parts_free( struct url_parts *u )
{
    free( u->scheme );
    free( u->host );
    free( u->path );
    free( u->path_and_query );
}

/* adds a Signature header for a request when its endpoint has a signing policy */
void add_signature( struct buf *h, const char *method, const char *url, const char *authorization, const void *body,
                    size_t body_len )
{
    struct signing_policy policy;
    struct url_parts u;
    char *sig;

    if (!find_policy( url, &policy )) return;
    split_url( url, &u );
    if ((sig = sign_request( policy.version, policy.max_body_bytes, method, u.path_and_query, authorization, body,
                             body_len, 0 )))
        hdr_add( h, "Signature: %s", sig );
    free( sig );
    url_parts_free( &u );
}

/* signs one of our own requests to *.auth.xboxlive.com / sisu, with the endpoint's policy */
void signed_headers( struct buf *h, const char *url, const char *body, BOOL contract )
{
    struct signing_policy policy = { 1, 8192, TRUE };  /* the table's; xal-rs' default without it */
    struct url_parts u;
    char *sig;

    hdr_add( h, "Content-Type: application/json" );
    hdr_add( h, "Accept: application/json" );
    if (contract) hdr_add( h, "x-xbl-contract-version: 1" );
    if (!find_policy( url, &policy ))
    {
        policy.version = 1;
        policy.max_body_bytes = 8192;
    }
    split_url( url, &u );
    if ((sig = sign_request( policy.version, policy.max_body_bytes, "POST", u.path_and_query, "", body, strlen( body ), 0 )))
        hdr_add( h, "Signature: %s", sig );
    free( sig );
    url_parts_free( &u );
}

void log_xbl_error( const char *what, DWORD status, const struct buf *resp )
{
    struct json *j = json_parse( resp->p, resp->n );
    const char *x = json_str( json_get( j, "XErr" ) );

    WARN( "%s: HTTP %lu, XErr %s %s\n", what, status, x, json_str( json_get( j, "Message" ) ) );
    if (!strcmp( x, "2148916233" )) WARN( "  this Microsoft account has no Xbox profile yet: create one at https://www.xbox.com/live\n" );
    else if (!strcmp( x, "2148916235" )) WARN( "  Xbox Live is not available in this account's country\n" );
    else if (!strcmp( x, "2148916236" ) || !strcmp( x, "2148916237" )) WARN( "  the account needs adult verification (South Korea)\n" );
    else if (!strcmp( x, "2148916238" )) WARN( "  a child account: an adult has to add it to a Microsoft family first\n" );
    json_free( j );
}

static void log_xbl_xerr( const char *what, DWORD status, const char *xerr )
{
    struct buf b = {0};
    buf_printf( &b, "{\"XErr\":%s}", xerr );
    log_xbl_error( what, status, &b );
    buf_free( &b );
}


static const char *jget( const struct json *j, const char *key )
{
    return json_str( json_get( j, key ) );
}

static double jnum( const struct json *j, const char *key )
{
    const struct json *v = json_get( j, key );
    return v->type == JSON_NUMBER ? v->number : 0;
}

static struct json *parse_buf( const struct buf *b )
{
    return json_parse( b->p ? b->p : "", b->n );
}

static char *device_token_type;  /* the DeviceType the cached token was issued for, NULL = loaded from the store */

/* XalAuthenticator::get_device_token: device.auth.xboxlive.com, AuthMethod ProofOfPossession */
HRESULT xbl_device_token( char **token, BOOL force, struct json **answer )
{
    /* Android: SISU refuses this MSA client (xal-rs' APP_XBOX) with a Win32 device (401), and without SISU's title
     * token peoplehub hands back an empty "people" for the signed-in user, which the Xbox app reads as "no profile"
     * and drops back to its sign-in gate. xal-rs pairs this client with Android/iOS too. */
    char type[32] = "Android", version[64] = "13", exp[24], *id, *key, *body, *p;
    const char *url = "https://device.auth.xboxlive.com/device/authenticate";
    struct buf h = {0}, resp = {0};
    struct json *j;
    DWORD status;
    BOOL usable;

    *token = NULL;
    if (answer) *answer = NULL;
    GetEnvironmentVariableA( "WINE_UWP_XBOX_DEVICE_TYPE", type, sizeof(type) );
    if (!device_load()) return XA_E_XBL;
    EnterCriticalSection( &device.cs );
    /* a stored token's type is not recorded; a non-default type asked for in this process always gets a fresh one */
    usable = device_token_type ? !strcmp( device_token_type, type ) : !strcmp( type, "Android" );
    if (!force && device.token && *device.token && device.token_expires > now_s() + 3600 && usable)
    {
        *token = str_dup( device.token );
        LeaveCriticalSection( &device.cs );
        return S_OK;
    }
    GetEnvironmentVariableA( "WINE_UWP_XBOX_DEVICE_VERSION", version, sizeof(version) );
    /* xal-rs: Android and Nintendo want "{uuid}", iOS upper case, everything else plain */
    if (!strcmp( type, "Android" ) || !strcmp( type, "Nintendo" )) id = strf( "{%s}", S(device.id) );
    else
    {
        id = str_dup( device.id );
        if (!strcmp( type, "iOS" )) for (p = id; *p; p++) *p = toupper( (unsigned char)*p );
    }
    key = proof_key_json();
    body = strf( "{\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"AuthMethod\":"
                 "\"ProofOfPossession\",\"Id\":\"%s\",\"DeviceType\":\"%s\",\"Version\":\"%s\",\"ProofKey\":%s}}",
                 id, type, version, key );
    free( id );
    free( key );
    signed_headers( &h, url, body, TRUE );
    status = http_request( L"POST", url, buf_str( &h ), body, strlen( body ), &resp );
    free( body );
    buf_free( &h );
    j = parse_buf( &resp );
    if (!status)
    {
        LeaveCriticalSection( &device.cs );
        buf_free( &resp );
        if (answer) *answer = j; else json_free( j );
        return XA_E_NETWORK;
    }
    if (status != 200 || !*jget( j, "Token" ))
    {
        log_xbl_error( "device/authenticate", status, &resp );
        LeaveCriticalSection( &device.cs );
        buf_free( &resp );
        if (answer) *answer = j; else json_free( j );
        return XA_E_XBL;
    }
    str_set( &device.token, jget( j, "Token" ) );
    str_set( &device_token_type, type );
    device.token_expires = parse_iso( jget( j, "NotAfter" ) );
    if (!device.token_expires) device.token_expires = now_s() + 86400;
    *token = str_dup( device.token );
    snprintf( exp, sizeof(exp), "%I64u", device.token_expires );
    TRACE( "Xbox Live device token received (%s, %Iu chars, valid %I64u h)\n", type, strlen( *token ),
           (device.token_expires - now_s()) / 3600 );
    LeaveCriticalSection( &device.cs );
    store_update( "device_token", *token, "device_expires", exp, NULL );
    buf_free( &resp );
    if (answer) *answer = j; else json_free( j );
    return S_OK;
}

char *refresh_store_key( const char *client_id )
{
    if (!client_id || !*client_id || !strcmp( client_id, MSA_CLIENT_ID )) return str_dup( "refresh_token" );
    return strf( "refresh_token_%s", client_id );
}

/* Takes an oauth20_token.srf success response. Caller holds signin_cs. */
static BOOL take_msa_tokens( const struct json *j )
{
    const char *access = jget( j, "access_token" ), *refresh = jget( j, "refresh_token" ), *user = jget( j, "user_id" );
    double expires_in = jnum( j, "expires_in" );
    char *refresh_copy, *user_copy;
    BOOL other;

    if (!*access) return FALSE;
    EnterCriticalSection( &account.cs );
    str_set( &account.msa_token, access );
    account.msa_expires = now_s() + (ULONGLONG)(expires_in > 60 ? expires_in - 60 : 0);
    if (*refresh) str_set( &account.refresh_token, refresh );
    other = *user && account.msa_user_id && *account.msa_user_id && strcmp( user, account.msa_user_id );
    if (*user) str_set( &account.msa_user_id, user );
    if (other) str_set( &account.puid, "" );   /* another account: forget the last one's */
    refresh_copy = str_dup( account.refresh_token );
    user_copy = str_dup( account.msa_user_id );
    LeaveCriticalSection( &account.cs );
    if (other)
        store_update( "refresh_token", refresh_copy, "user_id", user_copy, "email", "", "store_account_id", "",
                      "puid", "", NULL );
    else
        store_update( "refresh_token", refresh_copy, "user_id", user_copy, NULL );
    str_wipe_free( refresh_copy );
    free( user_copy );
    TRACE( "Microsoft account tokens received (access %Iu chars, refresh %s, expires in %.0f s)\n", strlen( access ),
           *refresh ? "renewed" : "unchanged", expires_in );
    return TRUE;
}

static void log_oauth_error( const char *what, DWORD status, const struct buf *resp )
{
    struct json *j = parse_buf( resp );
    WARN( "%s: HTTP %lu, error \"%s\"\n", what, status, jget( j, "error" ) );
    json_free( j );
}

/* POSTs a refresh_token grant; returns the parsed answer */
static DWORD refresh_grant( const char *client_id, const char *scope, const char *refresh, struct buf *resp,
                            struct json **j )
{
    char *scope_enc = urlencode( scope ), *refresh_enc = urlencode( refresh ), *form;
    DWORD status;

    form = strf( "grant_type=refresh_token&client_id=%s&scope=%s&refresh_token=%s", client_id, scope_enc, refresh_enc );
    status = http_form( "https://login.live.com/oauth20_token.srf", form, resp );
    str_wipe_free( form );
    free( scope_enc );
    str_wipe_free( refresh_enc );
    *j = parse_buf( resp );
    return status;
}

/* grant_type=refresh_token for a scope. Caller holds signin_cs. */
HRESULT msa_refresh( const char *scope, char **token_out )
{
    struct buf resp = {0};
    struct json *j;
    char *refresh;
    DWORD status;

    if (token_out) *token_out = NULL;
    refresh = account_field( &account.refresh_token );
    if (!refresh || !*refresh)
    {
        free( refresh );
        return XA_E_NO_ACCOUNT;
    }
    status = refresh_grant( MSA_CLIENT_ID, scope, refresh, &resp, &j );
    str_wipe_free( refresh );
    if (!status)
    {
        json_free( j );
        buf_free( &resp );
        return XA_E_NETWORK;
    }
    if (status != 200 || !*jget( j, "access_token" ))
    {
        log_oauth_error( "refresh", status, &resp );
        /* (only for the sign-in scope: another scope's invalid_grant means that scope is not granted) */
        if (!strcmp( jget( j, "error" ), "invalid_grant" ) && !strcmp( scope, MSA_SCOPE ))
        {
            /* the stored sign-in is gone for good (password change, revoked): forget it */
            EnterCriticalSection( &account.cs );
            str_set( &account.refresh_token, "" );
            LeaveCriticalSection( &account.cs );
            store_update( "refresh_token", "", "user_id", "", NULL );
            json_free( j );
            buf_free( &resp );
            return XA_E_NO_ACCOUNT;
        }
        json_free( j );
        buf_free( &resp );
        return XA_E_MSA;
    }
    if (!strcmp( scope, MSA_SCOPE )) take_msa_tokens( j );
    else if (*jget( j, "refresh_token" ))
    {
        char *r, *u;
        EnterCriticalSection( &account.cs );
        str_set( &account.refresh_token, jget( j, "refresh_token" ) );
        r = str_dup( account.refresh_token );
        u = str_dup( account.msa_user_id );
        LeaveCriticalSection( &account.cs );
        store_update( "refresh_token", r, "user_id", u, NULL );
        str_wipe_free( r );
        free( u );
    }
    if (token_out) *token_out = str_dup( jget( j, "access_token" ) );
    json_free( j );
    buf_free( &resp );
    return S_OK;
}

/* user.auth.xboxlive.com: Microsoft account token -> Xbox Live user token */
HRESULT xbl_user_authenticate( const char *msa_token, char **user_token, ULONGLONG *expires )
{
    const char *url = "https://user.auth.xboxlive.com/user/authenticate";
    struct buf h = {0}, resp = {0};
    char *esc = json_escape( msa_token ), *body;
    struct json *j;
    DWORD status;

    *user_token = NULL;
    body = strf( "{\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"AuthMethod\":"
                 "\"RPS\",\"SiteName\":\"user.auth.xboxlive.com\",\"RpsTicket\":\"t=%s\"}}", esc );
    str_wipe_free( esc );
    signed_headers( &h, url, body, TRUE );
    status = http_request( L"POST", url, buf_str( &h ), body, strlen( body ), &resp );
    str_wipe_free( body );
    buf_free( &h );
    if (!status)
    {
        buf_free( &resp );
        return XA_E_NETWORK;
    }
    j = parse_buf( &resp );
    if (status != 200 || !*jget( j, "Token" ))
    {
        log_xbl_error( "user/authenticate", status, &resp );
        json_free( j );
        buf_free( &resp );
        return XA_E_XBL;
    }
    *user_token = str_dup( jget( j, "Token" ) );
    *expires = parse_iso( jget( j, "NotAfter" ) );
    if (!*expires) *expires = now_s() + 3600;
    TRACE( "Xbox Live user token received (%Iu chars)\n", strlen( *user_token ) );
    json_free( j );
    buf_free( &resp );
    return S_OK;
}

/* xsts.auth.xboxlive.com: user token -> XSTS token for one relying party. XalAuthenticator::get_xsts_token: the device
 * token (and the title token, when SISU gave one) bind the XSTS token to our proof key, which is what makes signed
 * requests acceptable. *answer gets the whole reply (DisplayClaims.xui[0] has the claims). */
/* An Xbox Live device token for this prefix's Windows device: device/authenticate with the device's own Microsoft
 * account ticket (RPS), as Windows sends it. Title storage refuses writes from XSTS tokens of any other device type
 * ("Access is denied due to platform restriction policies", 403 code 2016), and so presumably do other PC-only title
 * services; the Xbox app's client has to stay on Android for SISU. Kept for the process. */
static CRITICAL_SECTION windows_device_cs;
static CRITICAL_SECTION_DEBUG windows_device_cs_debug =
{
    0, 0, &windows_device_cs,
    { &windows_device_cs_debug.ProcessLocksList, &windows_device_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": windows_device_cs") }
};
static CRITICAL_SECTION windows_device_cs = { &windows_device_cs_debug, -1, 0, 0, 0, 0 };

static HRESULT windows_device_token( char **token )
{
    static char *cached;
    static ULONGLONG cached_expires;
    const char *url = "https://device.auth.xboxlive.com/device/authenticate";
    char *ticket = NULL, *e, *key, *body;
    struct buf h = {0}, resp = {0};
    struct json *j;
    DWORD status;
    HRESULT hr;

    *token = NULL;
    EnterCriticalSection( &windows_device_cs );
    if (cached && cached_expires > now_s() + 3600)
    {
        *token = str_dup( cached );
        LeaveCriticalSection( &windows_device_cs );
        return S_OK;
    }
    if (FAILED(hr = windev_service_ticket( "user.auth.xboxlive.com", "mbi_ssl", &ticket )))
    {
        LeaveCriticalSection( &windows_device_cs );
        WARN( "no Windows device ticket for Xbox Live (%#lx)\n", hr );
        return hr;
    }
    e = json_escape( ticket );
    str_wipe_free( ticket );
    key = proof_key_json();
    body = strf( "{\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"AuthMethod\":"
                 "\"RPS\",\"SiteName\":\"user.auth.xboxlive.com\",\"RpsTicket\":\"%s\",\"ProofKey\":%s}}", e, key );
    str_wipe_free( e );
    free( key );
    signed_headers( &h, url, body, TRUE );
    status = http_request( L"POST", url, buf_str( &h ), body, strlen( body ), &resp );
    str_wipe_free( body );
    buf_free( &h );
    j = parse_buf( &resp );
    if (status != 200 || !*jget( j, "Token" ))
    {
        log_xbl_error( "device/authenticate (Windows)", status, &resp );
        hr = status ? XA_E_XBL : XA_E_NETWORK;
    }
    else
    {
        str_set( &cached, jget( j, "Token" ) );
        cached_expires = parse_iso( jget( j, "NotAfter" ) );
        if (!cached_expires) cached_expires = now_s() + 86400;
        *token = str_dup( cached );
        TRACE( "Xbox Live device token for the Windows device received (valid %I64u h)\n",
               (cached_expires - now_s()) / 3600 );
        hr = S_OK;
    }
    LeaveCriticalSection( &windows_device_cs );
    json_free( j );
    buf_free( &resp );
    return hr;
}

HRESULT xbl_xsts( const char *user_token, const char *rp, char **token, ULONGLONG *expires, struct json **answer,
                  BOOL with_title )
{
    const char *url = "https://xsts.auth.xboxlive.com/xsts/authorize";
    char *device_token = NULL, *title_token = NULL, *esc;
    struct buf h = {0}, resp = {0}, body = {0};
    struct json *j;
    DWORD status;

    *token = NULL;
    if (answer) *answer = NULL;
    if (with_title == XSTS_WINDOWS_DEVICE)
    {
        if (FAILED(windows_device_token( &device_token ))) str_set( &device_token, "" );
    }
    else if (FAILED(xbl_device_token( &device_token, FALSE, NULL ))) str_set( &device_token, "" );
    if (with_title == TRUE)
    {
        EnterCriticalSection( &account.cs );
        if (account.title_expires > now_s() + 60) title_token = str_dup( account.title_token );
        LeaveCriticalSection( &account.cs );
    }
    esc = json_escape( rp );
    buf_printf( &body, "{\"RelyingParty\":\"%s\",\"TokenType\":\"JWT\",\"Properties\":{\"SandboxId\":\"RETAIL\"", esc );
    free( esc );
    if (device_token && *device_token)
    {
        esc = json_escape( device_token );
        buf_printf( &body, ",\"DeviceToken\":\"%s\"", esc );
        free( esc );
    }
    if (title_token && *title_token)
    {
        esc = json_escape( title_token );
        buf_printf( &body, ",\"TitleToken\":\"%s\"", esc );
        free( esc );
    }
    esc = json_escape( user_token );
    buf_printf( &body, ",\"UserTokens\":[\"%s\"]}}", esc );
    str_wipe_free( esc );
    signed_headers( &h, url, buf_str( &body ), TRUE );
    status = http_request( L"POST", url, buf_str( &h ), body.p, body.n, &resp );
    buf_free( &body );
    buf_free( &h );
    if (!status)
    {
        buf_free( &resp );
        str_wipe_free( device_token );
        str_wipe_free( title_token );
        return XA_E_NETWORK;
    }
    j = parse_buf( &resp );
    if (status != 200 || !*jget( j, "Token" ))
    {
        WARN( "xsts for %s:\n", rp );
        log_xbl_error( "xsts/authorize", status, &resp );
        json_free( j );
        buf_free( &resp );
        str_wipe_free( device_token );
        str_wipe_free( title_token );
        return XA_E_XBL;
    }
    *token = str_dup( jget( j, "Token" ) );
    *expires = parse_iso( jget( j, "NotAfter" ) );
    if (!*expires) *expires = now_s() + 3600;
    *expires = *expires > 300 ? *expires - 300 : 0;
    TRACE( "XSTS token for %s received (%Iu chars; device %s, title %s)\n", rp, strlen( *token ),
           device_token && *device_token ? "yes" : "no", title_token && *title_token ? "yes" : "no" );
    if (answer) *answer = j; else json_free( j );
    buf_free( &resp );
    str_wipe_free( device_token );
    str_wipe_free( title_token );
    return S_OK;
}

/* XalAuthenticator::sisu_authorize, without a session id (xal-rs' xbox_live_sisu_authorization_flow after a
 * device-code login): one signed call gives user, title and device-bound XSTS tokens. AppId must be the MSA client
 * the access token was issued to: TitleToken.tid follows that client, not a TitleId field. *answer gets the reply
 * (AuthorizationToken.DisplayClaims.xui[0] has the claims). */
static HRESULT xbl_sisu_authorize_app( const char *msa_token, const char *app_id, char **user_token,
                                       ULONGLONG *user_expires, char **xsts, ULONGLONG *xsts_expires,
                                       struct json **answer )
{
    const char *url = "https://sisu.xboxlive.com/authorize", *app = app_id && *app_id ? app_id : MSA_CLIENT_ID;
    char *device_token, *e1, *e2, *e3, *key, *body;
    const struct json *u, *t, *a;
    struct buf h = {0}, resp = {0};
    struct json *j;
    HRESULT hr;
    DWORD status;

    *user_token = *xsts = NULL;
    *answer = NULL;
    if (FAILED(hr = xbl_device_token( &device_token, FALSE, NULL ))) return hr;
    e1 = json_escape( msa_token );
    e2 = json_escape( app );
    e3 = json_escape( device_token );
    key = proof_key_json();
    body = strf( "{\"AccessToken\":\"t=%s\",\"AppId\":\"%s\",\"DeviceToken\":\"%s\",\"Sandbox\":\"RETAIL\",\"SiteName\":"
                 "\"user.auth.xboxlive.com\",\"ProofKey\":%s}", e1, e2, e3, key );
    str_wipe_free( e1 );
    free( e2 );
    str_wipe_free( e3 );
    free( key );
    str_wipe_free( device_token );
    /* xal-rs sends no contract version header here */
    signed_headers( &h, url, body, FALSE );
    status = http_request( L"POST", url, buf_str( &h ), body, strlen( body ), &resp );
    str_wipe_free( body );
    buf_free( &h );
    if (!status)
    {
        buf_free( &resp );
        return XA_E_NETWORK;
    }
    j = parse_buf( &resp );
    u = json_get( j, "UserToken" );
    t = json_get( j, "TitleToken" );
    a = json_get( j, "AuthorizationToken" );
    if (status != 200 || !*jget( u, "Token" ) || !*jget( a, "Token" ))
    {
        log_xbl_error( "sisu/authorize", status, &resp );
        if (*jget( a, "XErr" )) log_xbl_xerr( "sisu/authorize (authorization)", status, jget( a, "XErr" ) );
        json_free( j );
        buf_free( &resp );
        return XA_E_XBL;
    }
    *user_token = str_dup( jget( u, "Token" ) );
    *user_expires = parse_iso( jget( u, "NotAfter" ) );
    if (!*user_expires) *user_expires = now_s() + 3600;
    *xsts = str_dup( jget( a, "Token" ) );
    *xsts_expires = parse_iso( jget( a, "NotAfter" ) );
    if (!*xsts_expires) *xsts_expires = now_s() + 3600;
    *xsts_expires = *xsts_expires > 300 ? *xsts_expires - 300 : 0;
    EnterCriticalSection( &account.cs );
    str_set( &account.title_token, jget( t, "Token" ) );
    account.title_expires = parse_iso( jget( t, "NotAfter" ) );
    if (!account.title_expires) account.title_expires = now_s() + 3600;
    str_set( &account.title_tid, jget( json_get( json_get( t, "DisplayClaims" ), "xti" ), "tid" ) );
    LeaveCriticalSection( &account.cs );
    TRACE( "SISU authorization: user, title (%s tid=%s app=%s) and XSTS tokens received\n",
           *jget( t, "Token" ) ? "yes" : "none", jget( json_get( json_get( t, "DisplayClaims" ), "xti" ), "tid" ), app );
    *answer = j;
    buf_free( &resp );
    return S_OK;
}

HRESULT xbl_sisu_authorize( const char *msa_token, char **user_token, ULONGLONG *user_expires, char **xsts,
                            ULONGLONG *xsts_expires, struct json **answer )
{
    return xbl_sisu_authorize_app( msa_token, MSA_CLIENT_ID, user_token, user_expires, xsts, xsts_expires, answer );
}

/* the claims of an XSTS answer: xui[0] of DisplayClaims, under AuthorizationToken for SISU */
static const struct json *xui_claims( const struct json *answer )
{
    const struct json *a = json_get( answer, "AuthorizationToken" );
    if (a->type == JSON_OBJECT) answer = a;
    return json_at( json_get( json_get( answer, "DisplayClaims" ), "xui" ), 0 );
}

/* Refresh-token grant for an arbitrary MSA client. Per-client refresh lives in xboxauth.bin as
 * refresh_token_<clientId> (the Xbox-app client keeps the bare refresh_token key). */
static HRESULT msa_refresh_for_client( const char *client_id, const char *scope, char **token_out )
{
    char *key = refresh_store_key( client_id ), *refresh = NULL;
    BOOL primary = !strcmp( key, "refresh_token" );
    struct buf resp = {0};
    struct json *j;
    DWORD status;

    if (token_out) *token_out = NULL;
    if (primary)
    {
        refresh = account_field( &account.refresh_token );
        if (!refresh || !*refresh)
        {
            str_wipe_free( refresh );
            refresh = store_get( "refresh_token" );
            if (*refresh)
            {
                EnterCriticalSection( &account.cs );
                str_set( &account.refresh_token, refresh );
                LeaveCriticalSection( &account.cs );
            }
        }
    }
    else refresh = store_get( key );
    if (!refresh || !*refresh)
    {
        str_wipe_free( refresh );
        free( key );
        return XA_E_NO_ACCOUNT;
    }
    status = refresh_grant( client_id, scope, refresh, &resp, &j );
    str_wipe_free( refresh );
    if (!status)
    {
        json_free( j );
        buf_free( &resp );
        free( key );
        return XA_E_NETWORK;
    }
    if (status != 200 || !*jget( j, "access_token" ))
    {
        char what[64];
        snprintf( what, sizeof(what), "refresh %s", client_id );
        log_oauth_error( what, status, &resp );
        if (!strcmp( jget( j, "error" ), "invalid_grant" ) && !strcmp( scope, MSA_SCOPE ))
            store_update( key, "", NULL );
        json_free( j );
        buf_free( &resp );
        free( key );
        return XA_E_MSA;
    }
    if (*jget( j, "refresh_token" ))
    {
        store_update( key, jget( j, "refresh_token" ), NULL );
        if (primary)
        {
            EnterCriticalSection( &account.cs );
            str_set( &account.refresh_token, jget( j, "refresh_token" ) );
            LeaveCriticalSection( &account.cs );
        }
    }
    if (token_out) *token_out = str_dup( jget( j, "access_token" ) );
    if (primary && !strcmp( scope, MSA_SCOPE )) take_msa_tokens( j );
    else TRACE( "Microsoft account token for client %s received (%Iu chars)\n", client_id,
                strlen( jget( j, "access_token" ) ) );
    json_free( j );
    buf_free( &resp );
    free( key );
    return S_OK;
}

static BOOL have_title_token( const char *want )
{
    BOOL have;
    EnterCriticalSection( &account.cs );
    have = account.title_token && *account.title_token && account.title_expires > now_s() + 60 &&
           !strcmp( S(account.title_tid), want );
    LeaveCriticalSection( &account.cs );
    return have;
}

/* Binds account.title_token to msa_app/title_id so XSTS carries that tid. Needs a refresh token issued to msa_app
 * (a device-code sign-in once per title client). */
HRESULT ensure_title_identity( const char *msa_app, UINT32 title_id )
{
    char want[32], *msa = NULL, *user_token = NULL, *xsts = NULL, *got;
    ULONGLONG user_expires, xsts_expires;
    struct json *answer = NULL;
    const char *uhs;
    HRESULT hr;

    if (!msa_app || !*msa_app || !title_id) return S_OK;
    snprintf( want, sizeof(want), "%u", title_id );
    if (have_title_token( want )) return S_OK;

    EnterCriticalSection( &account.signin_cs );
    if (have_title_token( want ))
    {
        LeaveCriticalSection( &account.signin_cs );
        return S_OK;
    }
    if (FAILED(hr = msa_refresh_for_client( msa_app, MSA_SCOPE, &msa )))
    {
        LeaveCriticalSection( &account.signin_cs );
        WARN( "title identity %s/%u: no MSA for client (%#lx); device-code that client once\n", msa_app, title_id, hr );
        return hr;
    }
    /* PC GDK titles need Win32 device tokens for SISU and the matching XSTS; Android (the Xbox app's default) 401s
     * here, and a TitleToken minted under Win32 is refused by XSTS with an Android device. */
    if (strcmp( msa_app, MSA_CLIENT_ID )) SetEnvironmentVariableA( "WINE_UWP_XBOX_DEVICE_TYPE", "Win32" );
    hr = xbl_sisu_authorize_app( msa, msa_app, &user_token, &user_expires, &xsts, &xsts_expires, &answer );
    if (FAILED(hr) && strcmp( msa_app, MSA_CLIENT_ID ))
    {
        SetEnvironmentVariableA( "WINE_UWP_XBOX_DEVICE_TYPE", "Android" );
        hr = xbl_sisu_authorize_app( msa, msa_app, &user_token, &user_expires, &xsts, &xsts_expires, &answer );
    }
    str_wipe_free( msa );
    if (FAILED(hr))
    {
        LeaveCriticalSection( &account.signin_cs );
        return hr;
    }

    EnterCriticalSection( &account.cs );
    if (strcmp( S(account.title_tid), want ))
    {
        got = str_dup( account.title_tid );
        LeaveCriticalSection( &account.cs );
        LeaveCriticalSection( &account.signin_cs );
        WARN( "title identity: SISU tid=%s want=%s (the MSA client does not bind this title)\n", S(got), want );
        free( got );
        str_wipe_free( user_token );
        str_wipe_free( xsts );
        json_free( answer );
        return XA_E_XBL;
    }
    /* keeps the user claims from title SISU; drops cached XSTS so the next one carries the new TitleToken */
    str_set( &account.user_token, user_token );
    account.user_expires = user_expires;
    xsts_clear();
    uhs = jget( xui_claims( answer ), "uhs" );
    if (*uhs) str_set( &account.uhs, uhs );
    LeaveCriticalSection( &account.cs );
    LeaveCriticalSection( &account.signin_cs );
    TRACE( "title identity bound: app=%s tid=%s\n", msa_app, want );
    str_wipe_free( user_token );
    str_wipe_free( xsts );
    json_free( answer );
    return S_OK;
}

/* title.auth.xboxlive.com/title/authenticate: the title token SISU would have returned, from the device token and the
 * Microsoft account ticket. Some services (peoplehub's "people" for the signed-in user) answer an empty list to XSTS
 * tokens that carry no title claims. Leaves account.title_token empty when the service refuses. */
static HRESULT xbl_title_token( const char *msa_token )
{
    const char *url = "https://title.auth.xboxlive.com/title/authenticate";
    char *device_token, *e1, *e2, *key, *body;
    struct buf h = {0}, resp = {0};
    struct json *j;
    HRESULT hr;
    DWORD status;

    if (FAILED(hr = xbl_device_token( &device_token, FALSE, NULL ))) return hr;
    e1 = json_escape( device_token );
    e2 = json_escape( msa_token );
    key = proof_key_json();
    body = strf( "{\"Properties\":{\"AuthMethod\":\"RPS\",\"DeviceToken\":\"%s\",\"RpsTicket\":\"t=%s\",\"SiteName\":"
                 "\"user.auth.xboxlive.com\",\"ProofKey\":%s},\"RelyingParty\":\"http://auth.xboxlive.com\","
                 "\"TokenType\":\"JWT\"}", e1, e2, key );
    str_wipe_free( e1 );
    str_wipe_free( e2 );
    free( key );
    str_wipe_free( device_token );
    signed_headers( &h, url, body, TRUE );
    status = http_request( L"POST", url, buf_str( &h ), body, strlen( body ), &resp );
    str_wipe_free( body );
    buf_free( &h );
    if (!status)
    {
        buf_free( &resp );
        return XA_E_NETWORK;
    }
    j = parse_buf( &resp );
    if (status != 200 || !*jget( j, "Token" ))
    {
        log_xbl_error( "title/authenticate", status, &resp );
        json_free( j );
        buf_free( &resp );
        return XA_E_XBL;
    }
    EnterCriticalSection( &account.cs );
    str_set( &account.title_token, jget( j, "Token" ) );
    account.title_expires = parse_iso( jget( j, "NotAfter" ) );
    if (!account.title_expires) account.title_expires = now_s() + 3600;
    LeaveCriticalSection( &account.cs );
    TRACE( "Xbox Live title token received (%Iu chars)\n", strlen( jget( j, "Token" ) ) );
    json_free( j );
    buf_free( &resp );
    return S_OK;
}

/* What Windows' Xbox sign-in and the Microsoft Store leave in HKCU for games to read. Halo Infinite compares
 * XboxLive\Xuid with its user and XboxLive\AccountId with Store\PrimaryWebAccountId, and shows "The Microsoft Store
 * user is not the same as the logged in Xbox Live user" when any is missing or different. Our Store account is the
 * Xbox sign-in's Microsoft account, so both ids are its WebAccount.Id (the PUID). */
static void set_reg( const char *key, const char *name, const char *value )
{
    if (!value || !*value) return;
    RegSetKeyValueA( HKEY_CURRENT_USER, key, name, REG_SZ, value, strlen( value ) + 1 );
}

static void publish_signed_in_account( const char *xuid )
{
    char *id;

    EnterCriticalSection( &account.cs );
    id = str_dup( account.puid && *account.puid ? account.puid : account.msa_user_id );
    LeaveCriticalSection( &account.cs );
    set_reg( "SOFTWARE\\Microsoft\\XboxLive", "Xuid", xuid );
    set_reg( "SOFTWARE\\Microsoft\\XboxLive", "AccountId", id );
    set_reg( "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Store", "PrimaryWebAccountId", id );
    TRACE( "published the signed-in account for games (xuid %s)\n", xuid );
    free( id );
}

static HRESULT xbl_sign_in_via_title_sisu( const char *title_app, UINT32 title_id, char **user_token,
                                           ULONGLONG *user_expires, char **xsts, ULONGLONG *xsts_expires,
                                           struct json **answer )
{
    char want[32], *msa;
    HRESULT hr;
    BOOL ok;

    if (!title_app || !*title_app || !title_id || !strcmp( title_app, MSA_CLIENT_ID )) return E_FAIL;
    snprintf( want, sizeof(want), "%u", title_id );
    SetEnvironmentVariableA( "WINE_UWP_XBOX_DEVICE_TYPE", "Win32" );
    if (FAILED(hr = msa_refresh_for_client( title_app, MSA_SCOPE, &msa ))) return hr;
    hr = xbl_sisu_authorize_app( msa, title_app, user_token, user_expires, xsts, xsts_expires, answer );
    str_wipe_free( msa );
    if (FAILED(hr)) return hr;
    EnterCriticalSection( &account.cs );
    ok = !strcmp( S(account.title_tid), want ) && account.title_token && *account.title_token;
    LeaveCriticalSection( &account.cs );
    if (!ok)
    {
        WARN( "title SISU tid mismatch during sign-in (want %s)\n", want );
        return XA_E_XBL;
    }
    TRACE( "sign-in via title SISU (app=%s tid=%u): no title-less XSTS window\n", title_app, title_id );
    return S_OK;
}

static void free_tokens( char **user_token, char **xsts, struct json **answer )
{
    str_wipe_free( *user_token );
    str_wipe_free( *xsts );
    json_free( *answer );
    *user_token = *xsts = NULL;
    *answer = NULL;
}

void fetch_account_email( void );
void fetch_account_puid( void );

/* With a Microsoft account token in hand (account.msa_token), gets the Xbox identity. Caller holds signin_cs.
 * Prefers title-bound SISU when a GDK SetTitleContext is set, so the first XSTS is title-bound (Party/ListPartyQos
 * otherwise race a long title-less window after default SISU 401 -> user/authenticate -> title/authenticate 403). */
static HRESULT xbl_sign_in( void )
{
    char *msa, *title_app, *user_token = NULL, *xsts = NULL;
    ULONGLONG user_expires = 0, xsts_expires = 0;
    struct json *answer = NULL;
    const struct json *xui;
    BOOL signed_via_title = FALSE, title_ctx;
    UINT32 title_id;
    char want[32];
    HRESULT hr = S_OK;

    msa = account_field( &account.msa_token );
    title_context( &title_app, &title_id );
    title_ctx = title_app && *title_app && title_id && strcmp( title_app, MSA_CLIENT_ID );

    if (title_ctx)
    {
        BOOL have_title;

        snprintf( want, sizeof(want), "%u", title_id );
        /* reuses the title binding from SetTitleContext's early bind when that won the race */
        EnterCriticalSection( &account.cs );
        have_title = account.title_token && *account.title_token && !strcmp( S(account.title_tid), want ) &&
                     account.user_token && *account.user_token && account.user_expires > now_s() + 60;
        if (have_title)
        {
            user_token = str_dup( account.user_token );
            user_expires = account.user_expires;
        }
        LeaveCriticalSection( &account.cs );
        if (have_title)
        {
            char *ut = user_token;
            user_token = NULL;
            hr = xbl_xsts( ut, "http://xboxlive.com", &xsts, &xsts_expires, &answer, TRUE );
            user_token = ut;
            if (SUCCEEDED(hr))
            {
                signed_via_title = TRUE;
                TRACE( "sign-in reused the title-bound user token (tid=%s)\n", want );
            }
            else
            {
                str_wipe_free( user_token );
                user_token = NULL;
            }
        }
        if (!signed_via_title)
        {
            hr = xbl_sign_in_via_title_sisu( title_app, title_id, &user_token, &user_expires, &xsts, &xsts_expires,
                                             &answer );
            if (SUCCEEDED(hr)) signed_via_title = TRUE;
            else if (hr == XA_E_NETWORK) goto done;
            else
            {
                free_tokens( &user_token, &xsts, &answer );
                WARN( "title SISU during sign-in failed (%#lx); trying default SISU\n", hr );
            }
        }
    }

    if (!signed_via_title &&
        FAILED(hr = xbl_sisu_authorize( msa, &user_token, &user_expires, &xsts, &xsts_expires, &answer )))
    {
        if (hr == XA_E_NETWORK) goto done;
        free_tokens( &user_token, &xsts, &answer );
        EnterCriticalSection( &account.cs );
        str_set( &account.title_token, "" );
        account.title_expires = 0;
        str_set( &account.title_tid, "" );
        LeaveCriticalSection( &account.cs );
        WARN( "SISU refused; falling back to user/authenticate + XSTS\n" );
        if (FAILED(hr = xbl_user_authenticate( msa, &user_token, &user_expires ))) goto done;
        /* When a title context is set, binds the title before the first XSTS so Party never sees a title-less
         * token. Inline: the caller already holds signin_cs. */
        if (title_ctx)
        {
            char *ut2 = NULL;
            ULONGLONG ue2;
            HRESULT thr = xbl_sign_in_via_title_sisu( title_app, title_id, &ut2, &ue2, &xsts, &xsts_expires,
                                                      &answer );
            if (SUCCEEDED(thr))
            {
                str_wipe_free( user_token );
                user_token = ut2;
                user_expires = ue2;
                signed_via_title = TRUE;
            }
            else if (thr == XA_E_NETWORK)
            {
                hr = thr;
                goto done;
            }
            else
            {
                str_wipe_free( ut2 );
                str_wipe_free( xsts );
                xsts = NULL;
                json_free( answer );
                answer = NULL;
                WARN( "title SISU after user/authenticate failed (%#lx); last resort title-less XSTS\n", thr );
            }
        }
        if (!signed_via_title)
        {
            if (xbl_title_token( msa ) == XA_E_NETWORK)
            {
                hr = XA_E_NETWORK;
                goto done;
            }
            if (FAILED(hr = xbl_xsts( user_token, "http://xboxlive.com", &xsts, &xsts_expires, &answer, TRUE )))
                goto done;
        }
    }
    xui = xui_claims( answer );
    if (!*jget( xui, "uhs" ) || !*jget( xui, "xid" ))
    {
        WARN( "XSTS answer carries no xid/uhs claims\n" );
        hr = XA_E_XBL;
        goto done;
    }
    EnterCriticalSection( &account.cs );
    str_set( &account.user_token, user_token );
    account.user_expires = user_expires;
    xsts_clear();
    xsts_put( "http://xboxlive.com", xsts, xsts_expires );
    str_set( &account.uhs, jget( xui, "uhs" ) );
    str_set( &account.xid, jget( xui, "xid" ) );
    str_set( &account.gtg, jget( xui, "gtg" ) );
    str_set( &account.agg, jget( xui, "agg" ) );
    str_set( &account.prv, jget( xui, "prv" ) );
    str_set( &account.mgt, jget( xui, "mgt" ) );
    str_set( &account.mgs, jget( xui, "mgs" ) );
    str_set( &account.umg, jget( xui, "umg" ) );
    account.signed_in = TRUE;
    LeaveCriticalSection( &account.cs );
    TRACE( "signed in to Xbox Live as \"%s\" (age group %s)\n", jget( xui, "gtg" ), jget( xui, "agg" ) );
    /* the XUID and claims, so GDK XUser in other processes (Halo) can answer without SISU again */
    store_update( "gamertag", jget( xui, "gtg" ), "xid", jget( xui, "xid" ), "uhs", jget( xui, "uhs" ),
                  "agg", jget( xui, "agg" ), "prv", jget( xui, "prv" ), "mgt", jget( xui, "mgt" ),
                  "mgs", jget( xui, "mgs" ), "umg", jget( xui, "umg" ), NULL );
    fetch_account_email();
    fetch_account_puid();
    publish_signed_in_account( jget( xui, "xid" ) );
    hr = S_OK;
done:
    free_tokens( &user_token, &xsts, &answer );
    str_wipe_free( msa );
    free( title_app );
    return hr;
}

HRESULT sign_in_silently( void )
{
    BOOL done, have;
    HRESULT hr;

    account_init();
    EnterCriticalSection( &account.signin_cs );
    EnterCriticalSection( &account.cs );
    done = account.signed_in;
    if (!account.refresh_token || !*account.refresh_token)
    {
        struct kv kv;
        store_lock();
        store_read( &kv );
        store_unlock();
        str_set( &account.refresh_token, kv_get( &kv, "refresh_token" ) );
        str_set( &account.msa_user_id, kv_get( &kv, "user_id" ) );
        kv_free( &kv );
    }
    have = account.refresh_token && *account.refresh_token;
    LeaveCriticalSection( &account.cs );
    if (done) hr = S_OK;
    else if (!have) hr = XA_E_NO_ACCOUNT;
    else if (SUCCEEDED(hr = msa_refresh( MSA_SCOPE, NULL ))) hr = xbl_sign_in();
    LeaveCriticalSection( &account.signin_cs );
    return hr;
}

void open_browser( const char *url )
{
    char mode[16] = "";
    WCHAR *w;
    BOOL ok;

    if (GetEnvironmentVariableA( "WINE_UWP_LAUNCHER", mode, sizeof(mode) ) && !strcmp( mode, "log" ))
    {
        HANDLE file = CreateFileW( L"C:\\users\\Public\\wineuwp-launched.uri", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL );
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written;
            WriteFile( file, url, strlen( url ), &written, NULL );
            CloseHandle( file );
        }
        else WARN( "C:\\users\\Public\\wineuwp-launched.uri not written, error %lu\n", GetLastError() );
        MESSAGE( "sign-in page %s (dry run, WINE_UWP_LAUNCHER=log)\n", url );
        return;
    }
    w = widen( url );
    ok = (INT_PTR)ShellExecuteW( NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL ) > 32;
    free( w );
    MESSAGE( "sign-in page %s -> %s\n", url, ok ? "opened in the browser" : "no browser" );
}

/* The device-code flow for an MSA client. For the Xbox app's client this also runs xbl_sign_in and replaces the primary
 * refresh_token; for other clients (Halo's MSAAppId) it only stores refresh_token_<client>. */
HRESULT sign_in_with_device_code_client( const char *client_id, volatile LONG *cancelled, BOOL full_signin )
{
    const char *client = client_id && *client_id ? client_id : MSA_CLIENT_ID, *verify;
    char *scope = urlencode( MSA_SCOPE ), *form, *user_code, *device_code, *link;
    struct buf resp = {0};
    ULONGLONG deadline;
    double expires_in;
    struct json *j;
    DWORD status;
    int interval, i;
    HRESULT hr;

    form = strf( "client_id=%s&scope=%s&response_type=device_code", client, scope );
    free( scope );
    status = http_form( "https://login.live.com/oauth20_connect.srf", form, &resp );
    free( form );
    if (!status)
    {
        buf_free( &resp );
        return XA_E_NETWORK;
    }
    j = parse_buf( &resp );
    if (status != 200 || !*jget( j, "user_code" ) || !*jget( j, "device_code" ))
    {
        log_oauth_error( "device code", status, &resp );
        json_free( j );
        buf_free( &resp );
        return XA_E_MSA;
    }
    user_code = str_dup( jget( j, "user_code" ) );
    device_code = str_dup( jget( j, "device_code" ) );
    interval = jnum( j, "interval" ) >= 1 ? (int)jnum( j, "interval" ) : 5;
    expires_in = jnum( j, "expires_in" );
    deadline = now_s() + (expires_in > 0 ? (ULONGLONG)expires_in : 900);
    verify = *jget( j, "verification_uri" ) ? jget( j, "verification_uri" ) : "https://www.microsoft.com/link";
    printf( "\nSign in with your Microsoft account:\n"
            "  1. Open %s (your browser should open it by itself)\n"
            "  2. Enter the code  %s\n"
            "  3. Sign in and allow access.\n"
            "Waiting for you to finish (the code is valid for %.0f minutes)...\n\n",
            verify, user_code, (expires_in > 0 ? expires_in : 900) / 60 );
    fflush( stdout );
    MESSAGE( "device code obtained: client %s, user code %s, valid %.0f s, verification %s\n", client, user_code,
             expires_in, verify );
    json_free( j );
    link = strf( "https://www.microsoft.com/link?otc=%s", user_code );
    open_browser( link );
    free( link );
    free( user_code );

    hr = XA_E_TIMEOUT;
    while (now_s() < deadline)
    {
        char *code_enc, *err;

        for (i = 0; i < interval * 10; i++)
        {
            if (cancelled && *cancelled)
            {
                TRACE( "device-code sign-in cancelled\n" );
                hr = E_ABORT;
                goto done;
            }
            Sleep( 100 );
        }
        code_enc = urlencode( device_code );
        form = strf( "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=%s&device_code=%s", client,
                     code_enc );
        free( code_enc );
        buf_free( &resp );
        status = http_form( "https://login.live.com/oauth20_token.srf", form, &resp );
        free( form );
        if (!status) continue;  /* transient; keeps polling */
        j = parse_buf( &resp );
        if (status == 200 && *jget( j, "access_token" ))
        {
            char *key = refresh_store_key( client );
            if (*jget( j, "refresh_token" )) store_update( key, jget( j, "refresh_token" ), NULL );
            if (full_signin && !strcmp( key, "refresh_token" ))
            {
                take_msa_tokens( j );
                json_free( j );
                free( key );
                hr = xbl_sign_in();
                goto done;
            }
            TRACE( "device-code for client %s: refresh stored under %s\n", client, key );
            printf( "DEVICE_CODE_OK client=%s refresh_key=%s\n", client, key );
            fflush( stdout );
            json_free( j );
            free( key );
            hr = S_OK;
            goto done;
        }
        err = str_dup( jget( j, "error" ) );
        json_free( j );
        if (!strcmp( err, "authorization_pending" ))
        {
            free( err );
            continue;
        }
        if (!strcmp( err, "slow_down" ))
        {
            free( err );
            interval += 5;
            continue;
        }
        log_oauth_error( "device-code poll", status, &resp );
        hr = !strcmp( err, "expired_token" ) ? XA_E_TIMEOUT : XA_E_MSA;
        free( err );
        goto done;
    }
    WARN( "device code expired before the sign-in was finished\n" );
done:
    str_wipe_free( device_code );
    buf_free( &resp );
    return hr;
}

/* The device-code flow for the Xbox app's client. Blocks until the user approves, refuses or the code expires. */
HRESULT sign_in_with_device_code( volatile LONG *cancelled )
{
    return sign_in_with_device_code_client( MSA_CLIENT_ID, cancelled, TRUE );
}

void sign_out( void )
{
    account_init();
    EnterCriticalSection( &account.signin_cs );
    EnterCriticalSection( &account.cs );
    account.signed_in = FALSE;
    str_set( &account.refresh_token, "" );
    str_set( &account.puid, "" );
    str_set( &account.msa_token, "" );
    str_set( &account.user_token, "" );
    xsts_clear();
    LeaveCriticalSection( &account.cs );
    /* the device's proof key and device token stay: they belong to the machine, not the user */
    store_update( "refresh_token", "", "user_id", "", "gamertag", "", "email", "", "store_account_id", "", "puid", "",
                  "xid", "", "uhs", "", "agg", "", "prv", "", "mgt", "", "mgs", "", "umg", "", NULL );
    LeaveCriticalSection( &account.signin_cs );
    TRACE( "signed out; the stored sign-in is deleted\n" );
}

/* ---- relying parties: the title endpoints table ---- */

struct endpoint
{
    char *protocol, *host, *path, *rp;
    BOOL wildcard;
    int policy;
};

static struct endpoint *endpoints;      /* guarded by account.cs */
static unsigned endpoint_count;
static struct signing_policy *policies;
static unsigned policy_count;
static BOOL endpoints_loaded, title_endpoints_loaded;

static unsigned read_endpoints( const struct json *j, struct endpoint **out, BOOL with_policy )
{
    const struct json *list = json_get( j, "EndPoints" );
    unsigned i, count = json_count( list );
    struct endpoint *eps = calloc( count ? count : 1, sizeof(*eps) );

    for (i = 0; i < count; i++)
    {
        const struct json *e = json_at( list, i ), *pol = json_get( e, "SignaturePolicyIndex" );
        eps[i].protocol = str_lower( str_dup( jget( e, "Protocol" ) ) );
        eps[i].host = str_lower( str_dup( jget( e, "Host" ) ) );
        eps[i].path = str_dup( jget( e, "Path" ) );
        eps[i].rp = str_dup( jget( e, "RelyingParty" ) );
        eps[i].wildcard = !strcmp( jget( e, "HostType" ), "wildcard" );
        eps[i].policy = with_policy && pol->type == JSON_NUMBER ? (int)pol->number : -1;
    }
    *out = eps;
    return count;
}

static void free_endpoints( struct endpoint *eps, unsigned count )
{
    unsigned i;
    for (i = 0; i < count; i++)
    {
        free( eps[i].protocol );
        free( eps[i].host );
        free( eps[i].path );
        free( eps[i].rp );
    }
    free( eps );
}

static void load_endpoints( void )
{
    struct signing_policy *pols;
    struct endpoint *list;
    unsigned count, npol, i, k;
    struct buf resp = {0};
    const struct json *pl;
    struct json *j;
    DWORD status;
    BOOL loaded;

    account_init();
    EnterCriticalSection( &account.cs );
    loaded = endpoints_loaded;
    LeaveCriticalSection( &account.cs );
    if (loaded) return;

    status = http_request( L"GET", "https://title.mgt.xboxlive.com/titles/default/endpoints?type=1",
                           "Accept: application/json\r\nx-xbl-contract-version: 1\r\n", "", 0, &resp );
    j = parse_buf( &resp );
    count = read_endpoints( j, &list, TRUE );
    pl = json_get( j, "SignaturePolicies" );
    npol = json_count( pl );
    pols = calloc( npol ? npol : 1, sizeof(*pols) );
    for (i = 0; i < npol; i++)
    {
        const struct json *p = json_at( pl, i ), *algs = json_get( p, "SupportedAlgorithms" );
        pols[i].version = (int)jnum( p, "Version" );
        pols[i].max_body_bytes = (ULONGLONG)jnum( p, "MaxBodyBytes" );
        pols[i].es256 = FALSE;
        for (k = 0; k < json_count( algs ); k++) if (!strcmp( json_str( json_at( algs, k ) ), "ES256" )) pols[i].es256 = TRUE;
    }
    json_free( j );
    buf_free( &resp );
    TRACE( "title endpoints: HTTP %lu, %u entries, %u signature policies\n", status, count, npol );
    EnterCriticalSection( &account.cs );
    if (!endpoints_loaded && (status == 200 || !status))
    {
        free_endpoints( endpoints, endpoint_count );
        free( policies );
        endpoints = list;
        endpoint_count = count;
        policies = pols;
        policy_count = npol;
        list = NULL;
        pols = NULL;
        /* a failed fetch leaves the table empty and the fallback below in force; retried next time */
        endpoints_loaded = count != 0;
    }
    LeaveCriticalSection( &account.cs );
    if (list) free_endpoints( list, count );
    free( pols );
}

/* XAL also reads the title's own endpoint table: titles/<title id>/endpoints (no query; "?type=1" answers 414), which
 * needs an Xbox Live token. It names the title's partner services and their relying parties; Halo's EOS anti-cheat
 * login asks for a token for https://api.epicgames.dev, which only that table maps, and without it EOS Connect gets a
 * null token, anti-cheat never logs in and matchmaking reports a server problem. Its entries go before the default
 * table's, which still covers Xbox Live itself. */
static void load_title_endpoints( void )
{
    struct signing_policy policy;
    struct endpoint *list, *merged;
    struct buf h = {0}, resp = {0};
    struct url_parts u;
    char url[160], *auth = NULL, *sig;
    unsigned count, i;
    struct json *j;
    DWORD status;
    UINT32 tid;
    BOOL skip;

    EnterCriticalSection( &title_ctx_cs );
    tid = title_ctx_title_id;
    LeaveCriticalSection( &title_ctx_cs );
    EnterCriticalSection( &account.cs );
    skip = title_endpoints_loaded || !endpoints_loaded || !tid || !account.signed_in;
    if (!skip) title_endpoints_loaded = TRUE;  /* one attempt per process; get_token_for_url comes back here */
    LeaveCriticalSection( &account.cs );
    if (skip) return;

    snprintf( url, sizeof(url), "https://title.mgt.xboxlive.com/titles/%u/endpoints", tid );
    if (FAILED(get_token_for_url( url, FALSE, &auth, TRUE )) || !auth || !*auth)
    {
        WARN( "title %u endpoints: no token\n", tid );
        str_wipe_free( auth );
        return;
    }
    hdr_add( &h, "Accept: application/json" );
    hdr_add( &h, "x-xbl-contract-version: 1" );
    hdr_add( &h, "Authorization: %s", auth );
    if (!find_policy( url, &policy ))
    {
        policy.version = 1;
        policy.max_body_bytes = 8192;
    }
    split_url( url, &u );
    if ((sig = sign_request( policy.version, policy.max_body_bytes, "GET", u.path_and_query, auth, "", 0, 0 )))
        hdr_add( &h, "Signature: %s", sig );
    free( sig );
    url_parts_free( &u );
    str_wipe_free( auth );
    status = http_request( L"GET", url, buf_str( &h ), "", 0, &resp );
    buf_free( &h );
    j = parse_buf( &resp );
    count = read_endpoints( j, &list, FALSE );
    json_free( j );
    buf_free( &resp );
    TRACE( "title %u endpoints: HTTP %lu, %u entries\n", tid, status, count );
    for (i = 0; i < count; i++)
        TRACE( "  title endpoint %s://%s%s -> %s\n", list[i].protocol, list[i].host, list[i].path, list[i].rp );
    if (!count)
    {
        free_endpoints( list, count );
        return;
    }
    EnterCriticalSection( &account.cs );
    if ((merged = malloc( (count + endpoint_count) * sizeof(*merged) )))
    {
        memcpy( merged, list, count * sizeof(*merged) );
        memcpy( merged + count, endpoints, endpoint_count * sizeof(*merged) );
        free( endpoints );
        endpoints = merged;
        endpoint_count += count;
        free( list );
        list = NULL;
    }
    LeaveCriticalSection( &account.cs );
    if (list) free_endpoints( list, count );
}

static BOOL host_has_suffix( const char *host, const char *suffix )
{
    size_t h = strlen( host ), s = strlen( suffix );
    return h >= s && !strcmp( host + h - s, suffix );
}

/* The relying party for a URL, NULL for none (the request needs no token). */
char *relying_party( const char *url )
{
    const struct endpoint *best = NULL;
    struct url_parts u;
    char *rp = NULL;
    const char *path;
    int best_score = -1;
    unsigned i;

    if (!strstr( S(url), "://" )) return NULL;
    split_url( url, &u );
    /* (wss/ws are kept as written here; split_url maps them) */
    free( u.scheme );
    u.scheme = str_lower( str_dupn( url, strstr( url, "://" ) - url ) );
    path = *u.path ? u.path : "/";

    load_endpoints();
    load_title_endpoints();
    EnterCriticalSection( &account.cs );
    for (i = 0; i < endpoint_count; i++)
    {
        const struct endpoint *e = &endpoints[i];
        BOOL match;
        int score;

        if (*e->protocol && strcmp( e->protocol, u.scheme )) continue;
        if (e->wildcard) match = host_has_suffix( u.host, e->host[0] == '*' && e->host[1] ? e->host + 1 : e->host );
        else match = !strcmp( u.host, e->host );
        if (!match) continue;
        if (*e->path && strncmp( path, e->path, strlen( e->path ) )) continue;
        score = (e->wildcard ? 0 : 1 << 20) + (int)strlen( e->path ) * 2 + (int)strlen( e->host );
        if (score > best_score)
        {
            best_score = score;
            best = e;
        }
    }
    if (best) rp = str_dup( best->rp );
    LeaveCriticalSection( &account.cs );
    if (!best)
    {
        /* no table (offline?): every Xbox Live service takes the default relying party */
        if (strlen( u.host ) > strlen( ".xboxlive.com" ) && host_has_suffix( u.host, ".xboxlive.com" ))
            rp = str_dup( "http://xboxlive.com" );
        /* Halo Infinite's title services (dummy-xsts-host probe, settings, economy...) are not in title.mgt's
         * default table; the Partner Center relying party is the Halo Waypoint audience */
        else if (strlen( u.host ) > strlen( ".halowaypoint.com" ) && host_has_suffix( u.host, ".halowaypoint.com" ))
            rp = str_dup( "https://prod.xsts.halowaypoint.com/" );
    }
    url_parts_free( &u );
    if (rp && !*rp)
    {
        free( rp );
        rp = NULL;
    }
    return rp;
}

/* SignaturePolicyCache::find_policy_for_url: among endpoints with a signature policy, the one whose protocol and host
 * match (fqdn exactly, wildcard by suffix) and whose path, if it has one, equals the URL's; the longest host wins.
 * FALSE: the request is not signed. */
BOOL find_policy( const char *url, struct signing_policy *out )
{
    const struct endpoint *best = NULL;
    struct url_parts u;
    unsigned i;
    BOOL found;

    split_url( url, &u );
    if (strcmp( u.scheme, "http" ) && strcmp( u.scheme, "https" ))
    {
        url_parts_free( &u );
        return FALSE;
    }
    load_endpoints();
    EnterCriticalSection( &account.cs );
    for (i = 0; i < endpoint_count; i++)
    {
        const struct endpoint *e = &endpoints[i];
        const char *suffix = e->host;
        BOOL match;

        if (e->policy < 0 || strcmp( e->protocol, u.scheme )) continue;
        if (e->wildcard)
        {
            while (*suffix == '*') suffix++;
            match = host_has_suffix( u.host, suffix );
        }
        else match = !strcmp( u.host, e->host );
        if (!match || (*e->path && strcmp( u.path, e->path ))) continue;
        if (!best || strlen( e->host ) > strlen( best->host )) best = e;
    }
    found = best && (unsigned)best->policy < policy_count;
    if (found) *out = policies[best->policy];
    LeaveCriticalSection( &account.cs );
    /* the same Halo Waypoint hosts as relying_party(): signed with the default policy, so GetTokenAndSignature
     * returns a Signature along with the Halo-audience XSTS */
    if (!found && strlen( u.host ) > strlen( ".halowaypoint.com" ) && host_has_suffix( u.host, ".halowaypoint.com" ))
    {
        out->version = 1;
        out->max_body_bytes = 8192;
        out->es256 = TRUE;
        found = TRUE;
    }
    url_parts_free( &u );
    return found && out->es256;
}

/* An "XBL3.0 x=<uhs>;<xsts>" Authorization header for a URL; *header NULL when the URL needs no token. */
HRESULT get_token_for_url( const char *url, BOOL force, char **header, BOOL with_title )
{
    char *rp, *uhs = NULL, *user_token = NULL, *xsts = NULL, *key, *shown;
    ULONGLONG user_expires, xsts_expires;
    struct xsts *cached_x;
    BOOL signed_in, cached;
    HRESULT hr = S_OK;

    *header = NULL;
    shown = str_dupn( url, strcspn( url, "?" ) );
    if (!(rp = relying_party( url )))
    {
        TRACE( "token for %s: no relying party, no token\n", shown );
        free( shown );
        return S_OK;
    }
    key = with_title == XSTS_WINDOWS_DEVICE ? strf( "%s (Windows device)", rp )
                                            : with_title ? str_dup( rp ) : strf( "%s (no title)", rp );
    EnterCriticalSection( &account.cs );
    signed_in = account.signed_in;
    uhs = str_dup( account.uhs );
    user_token = str_dup( account.user_token );
    user_expires = account.user_expires;
    cached_x = xsts_find( key );
    cached = !force && cached_x && cached_x->expires > now_s();
    if (cached) xsts = str_dup( cached_x->token );
    LeaveCriticalSection( &account.cs );
    if (!signed_in)
    {
        hr = XA_E_NO_ACCOUNT;
        goto done;
    }

    if (!cached)
    {
        /* A full MSA/SISU renewal only when the user token has (nearly) expired. ForceRefresh with a still valid user
         * token mints XSTS again and nothing else; otherwise SISU 401 -> user/authenticate replaces the title-bound
         * user token, and XSTS(title token + new user token) is refused by RTA while older cached tokens still work
         * for presence and profile. */
        if (user_expires <= now_s() + 60)
        {
            EnterCriticalSection( &account.signin_cs );
            hr = msa_refresh( MSA_SCOPE, NULL );
            if (SUCCEEDED(hr))
            {
                char *msa = account_field( &account.msa_token ), *ax = NULL;
                ULONGLONG ax_expires;
                struct json *answer = NULL;

                str_wipe_free( user_token );
                user_token = NULL;
                hr = xbl_sisu_authorize( msa, &user_token, &user_expires, &ax, &ax_expires, &answer );
                if (FAILED(hr) && hr != XA_E_NETWORK)
                {
                    str_wipe_free( user_token );
                    user_token = NULL;
                    hr = xbl_user_authenticate( msa, &user_token, &user_expires );
                }
                else if (SUCCEEDED(hr))
                {
                    EnterCriticalSection( &account.cs );
                    xsts_put( "http://xboxlive.com", ax, ax_expires );
                    LeaveCriticalSection( &account.cs );
                }
                if (SUCCEEDED(hr))
                {
                    const char *new_uhs = jget( xui_claims( answer ), "uhs" );
                    EnterCriticalSection( &account.cs );
                    str_set( &account.user_token, user_token );
                    account.user_expires = user_expires;
                    if (*new_uhs) str_set( &account.uhs, new_uhs );
                    str_set( &uhs, account.uhs );
                    LeaveCriticalSection( &account.cs );
                }
                str_wipe_free( ax );
                json_free( answer );
                str_wipe_free( msa );
            }
            LeaveCriticalSection( &account.signin_cs );
            if (FAILED(hr)) goto done;
        }
        else if (force)
        {
            EnterCriticalSection( &account.cs );
            str_set( &user_token, account.user_token );
            str_set( &uhs, account.uhs );
            LeaveCriticalSection( &account.cs );
            TRACE( "ForceRefresh: XSTS again only (the user token is still valid) for %s\n", shown );
        }
        if (FAILED(hr = xbl_xsts( user_token, rp, &xsts, &xsts_expires, NULL, with_title ))) goto done;
        EnterCriticalSection( &account.cs );
        xsts_put( key, xsts, xsts_expires );
        LeaveCriticalSection( &account.cs );
    }
    *header = strf( "XBL3.0 x=%s;%s", S(uhs), S(xsts) );
    TRACE( "token for %s: relying party %s%s%s\n", shown, rp, cached ? " (cached)" : "",
           force && !cached ? " (forced)" : "" );
done:
    free( shown );
    free( rp );
    free( key );
    free( uhs );
    str_wipe_free( user_token );
    str_wipe_free( xsts );
    return hr;
}

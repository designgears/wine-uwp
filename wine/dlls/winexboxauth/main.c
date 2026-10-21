/*
 * winexboxauth: the factories and the exports other modules call
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

#include "initguid.h"
#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(xboxauth);

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );

    account_init();
    if (!wcscmp( name, L"Microsoft.GamePlatform.Services.Identity.XboxAuthManager" ))
        *factory = xboxauth_manager_factory();
    else if (!wcscmp( name, L"Microsoft.GamePlatform.Services.Identity.XboxAuthInitializationArgs" ))
        *factory = xboxauth_init_args_factory();
    else if (!wcscmp( name, L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager" ))
        *factory = web_auth_manager_factory();
    else if (!wcscmp( name, L"Windows.ApplicationModel.Store.Preview.StoreConfiguration" ))
        *factory = store_configuration_factory();
    else
    {
        TRACE( "%s: no stand-in\n", debugstr_w(name) );
        *factory = NULL;
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    TRACE( "%s: factory\n", debugstr_w(name) );
    IActivationFactory_AddRef( *factory );
    return S_OK;
}

HRESULT WINAPI DllCanUnloadNow( void )
{
    return S_FALSE;
}

/* the refresh token from the store, if this process has not signed in yet */
static void load_refresh_token( void )
{
    char *stored;

    EnterCriticalSection( &account.cs );
    if (account.refresh_token && *account.refresh_token)
    {
        LeaveCriticalSection( &account.cs );
        return;
    }
    LeaveCriticalSection( &account.cs );
    stored = store_get( "refresh_token" );
    EnterCriticalSection( &account.cs );
    if (!account.refresh_token || !*account.refresh_token) str_set( &account.refresh_token, stored );
    LeaveCriticalSection( &account.cs );
    str_wipe_free( stored );
}

/* An authenticated GET for the other modules (winegameplatform answers Gaming Services' content access with it):
 * the Xbox Live token for the URL's relying party, its Signature when the endpoint has a policy, and extra headers
 * ('|'-separated). *body is CoTaskMemAlloc'ed. Returns S_OK when a response arrived, whatever its status. */
HRESULT WINAPI XboxAuthHttpGet( const char *url, const char *extra, const char *contract, unsigned char **body,
                                UINT32 *len, DWORD *status )
{
    struct buf h = {0}, resp = {0};
    char *header = NULL;
    const char *a, *b;
    HRESULT hr;

    account_init();
    *body = NULL;
    *len = 0;
    *status = 0;
    if (FAILED(hr = sign_in_silently())) return hr;
    if (FAILED(hr = get_token_for_url( url, FALSE, &header, TRUE ))) return hr;
    hdr_add( &h, "Authorization: %s", header );
    hdr_add( &h, "x-xbl-contract-version: %s", contract ? contract : "1" );
    hdr_add( &h, "Accept-Language: en-US" );
    for (a = extra; a && *a; a = *b ? b + 1 : b)
    {
        b = a + strcspn( a, "|" );
        if (b > a)
        {
            buf_addn( &h, a, b - a );
            buf_add( &h, "\r\n" );
        }
    }
    add_signature( &h, "GET", url, header, "", 0 );
    *status = http_request( L"GET", url, buf_str( &h ), NULL, 0, &resp );
    str_wipe_free( header );
    buf_free( &h );
    if (!*status)
    {
        buf_free( &resp );
        return XA_E_NETWORK;
    }
    if (!(*body = CoTaskMemAlloc( resp.n ? resp.n : 1 )))
    {
        buf_free( &resp );
        return E_OUTOFMEMORY;
    }
    memcpy( *body, resp.p, resp.n );
    *len = resp.n;
    TRACE( "GET %s: HTTP %lu, %u bytes\n", debugstr_a(url), *status, (unsigned)resp.n );
    buf_free( &resp );
    return S_OK;
}

/* GDK XUserGetTokenAndSignature: Authorization header + optional Signature for a URL.
 * token/signature are written NUL-terminated; the lengths include the NUL. */
HRESULT WINAPI XboxAuthGetTokenAndSignature( const char *method, const char *url, int force, const void *body,
                                             UINT32 body_size, char *token, UINT32 token_cap, UINT32 *token_len,
                                             char *signature, UINT32 sig_cap, UINT32 *sig_len )
{
    char *app = NULL, *header = NULL, *sig = NULL;
    struct signing_policy policy;
    UINT32 title_id = 0, need;
    HRESULT hr;

    if (!url || !token_len) return E_INVALIDARG;
    *token_len = 0;
    if (sig_len) *sig_len = 0;
    if (token && token_cap) token[0] = 0;
    if (signature && sig_cap) signature[0] = 0;

    account_init();
    if (FAILED(hr = sign_in_silently())) return hr;
    title_context( &app, &title_id );
    if (app && *app && title_id) hr = ensure_title_identity( app, title_id );
    free( app );
    if (FAILED(hr)) return hr;
    if (FAILED(hr = get_token_for_url( url, force != 0, &header, TRUE ))) return hr;

    need = strlen( header ) + 1;
    *token_len = need;
    if (token)
    {
        if (token_cap < need)
        {
            hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            goto done;
        }
        memcpy( token, header, need );
    }
    if (find_policy( url, &policy ))
    {
        struct url_parts u;

        split_url( url, &u );
        sig = sign_request( policy.version, policy.max_body_bytes, method && *method ? method : "GET", u.path_and_query,
                            header, body, body ? body_size : 0, 0 );
        url_parts_free( &u );
        if (sig) TRACE( "GDK-token signed %s %s (policy %d, body %u)\n", method ? method : "GET",
                        debugstr_an(url, strcspn( url, "?" )), policy.version, body_size );
    }
    if (sig_len)
    {
        UINT32 sneed = sig ? strlen( sig ) + 1 : 0;
        *sig_len = sneed;
        if (sneed && signature)
        {
            if (sig_cap < sneed) hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            else memcpy( signature, sig, sneed );
        }
    }
done:
    free( sig );
    str_wipe_free( header );
    return hr;
}

/* Does the signed-in account own Store product ARG outright? collections.mp.microsoft.com's v7.0 query with the
 * account as beneficiary: S_OK owned, S_FALSE not (subscription access is not ownership; gameplatform checks that
 * from the content access data). */
HRESULT collections_owns( const char *product )
{
    char *ticket = NULL, *puid, *t, *p, *pr, *body, *headers;
    struct buf resp = {0};
    struct json *j;
    const struct json *items;
    DWORD status;
    HRESULT hr;
    BOOL owned;

    account_init();
    load_refresh_token();
    puid = account_field( &account.puid );
    if (!*puid)
    {
        free( puid );
        puid = store_get( "puid" );
    }
    EnterCriticalSection( &account.signin_cs );
    hr = msa_refresh( "service::collections.mp.microsoft.com::MBI_SSL", &ticket );
    LeaveCriticalSection( &account.signin_cs );
    if (FAILED(hr))
    {
        free( puid );
        return hr;
    }
    t = json_escape( ticket );
    p = json_escape( S(puid) );
    pr = json_escape( product );
    body = strf( "{\"beneficiaries\":[{\"identityType\":\"msa\",\"identityValue\":\"%s\",\"localTicketReference\":\"%s\"}],"
                 "\"maxPageSize\":25,\"market\":\"US\",\"productSkuIds\":[{\"productId\":\"%s\"}],\"validityType\":\"Valid\"}",
                 t, p, pr );
    headers = strf( "Content-Type: application/json\r\nAuthorization: WLID1.0=\"t=%s\"\r\n", ticket );
    status = http_request( L"POST", "https://collections.mp.microsoft.com/v7.0/collections/query", headers, body,
                           strlen( body ), &resp );
    str_wipe_free( headers );
    str_wipe_free( body );
    str_wipe_free( t );
    free( p );
    free( pr );
    free( puid );
    str_wipe_free( ticket );
    if (status != 200)
    {
        TRACE( "collections query for %s: HTTP %lu\n", debugstr_a(product), status );
        buf_free( &resp );
        return XA_E_NETWORK;
    }
    j = json_parse( resp.p, resp.n );
    items = json_get( j, "items" );
    owned = items->type == JSON_ARRAY && items->count;
    json_free( j );
    buf_free( &resp );
    TRACE( "collections: %s %s\n", debugstr_a(product), owned ? "owned" : "not owned" );
    return owned ? S_OK : S_FALSE;
}

HRESULT WINAPI XboxAuthOwnsProduct( const char *product )
{
    return collections_owns( product ? product : "" );
}

/* The content keys of an encrypted Store package (content id from the catalogue), as "keyid:key;..." in hex for the
 * installer. winegameplatform calls it when the Xbox app installs an EAppx/EMsix or MSIXVC package. */
HRESULT WINAPI XboxAuthContentKeys( const char *content_id, const char *market, char *out, UINT32 size )
{
    struct content_key *keys = NULL;
    unsigned count = 0, i;
    struct buf s = {0};
    size_t k;
    HRESULT hr;

    account_init();
    load_refresh_token();
    hr = content_keys( content_id ? content_id : "", market && *market ? market : "US", &keys, &count, NULL );
    if (FAILED(hr)) return hr;
    buf_addn( &s, "", 0 );
    for (i = 0; i < count; i++)
    {
        for (k = 0; k < 16; k++) buf_printf( &s, "%02x", keys[i].id[k] );
        buf_addc( &s, ':' );
        for (k = 0; k < keys[i].key_len; k++) buf_printf( &s, "%02x", keys[i].key[k] );
        buf_addc( &s, ';' );
    }
    SecureZeroMemory( keys, count * sizeof(*keys) );
    free( keys );
    if (!out || s.n + 1 > size) hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    else memcpy( out, s.p, s.n + 1 );
    buf_free( &s );
    return hr;
}

/* ---- XboxAuthSelfTest, for probes/probe-xboxauth.cpp: the HTTP paths without an app. It prints shapes and
 * statuses, never a token or a key. ---- */

static void hex_to_ft( ULONGLONG t, unsigned char ft[8] )
{
    int i;
    for (i = 0; i < 8; i++) ft[i] = (unsigned char)(t >> (56 - 8 * i));
}

static int selftest_sigtest( void )
{
    static const unsigned char want[32] = { 0xf7,0xd6,0x1b,0x6f,0x8d,0x4d,0xcd,0x86,0xda,0x1a,0xa8,0x55,0x3f,0x0e,0xe7,0xc1,
                                            0x54,0x50,0x81,0x1e,0x7c,0xd2,0x75,0x93,0x64,0xe2,0x2f,0x67,0xd8,0x53,0xff,0x50 };
    /* xal-rs src/request_signer.rs tests: prehashed_data and verify_real_request */
    static const char body[] = "{\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"AuthMethod\":\"ProofOfPossession\",\"Id\":\"{e51d4344-196a-4550-9e27-f6c5006a9949}\",\"DeviceType\":\"Android\",\"Version\":\"8.0.0\",\"ProofKey\":{\"kty\":\"EC\",\"alg\":\"ES256\",\"crv\":\"P-256\",\"x\":\"GJS1AAhiPYw0ZSQJDCF8kcZkKAc2tRWXAN6Yw-o_hMQ\",\"y\":\"UAQJHUc_yVIhUQgIvoED3kt0JDz_G8jgXys_XR2s-Xc\",\"use\":\"sig\"}}}";
    unsigned char ver[4] = { 0, 0, 0, 1 }, ft[8], hash[32], pub[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    BCRYPT_ECCKEY_BLOB *hdr = (BCRYPT_ECCKEY_BLOB *)pub;
    struct buf m = {0}, sig = {0}, x = {0}, y = {0}, mine = {0};
    BCRYPT_KEY_HANDLE k = NULL;
    char *own;
    int fails = 0;
    BOOL ok;

    hex_to_ft( (1586999965ULL + 11644473600ULL) * 10000000ULL, ft );
    sign_prehash_input( &m, ver, ft, "POST", "/path?query=1", "XBL3.0 x=userid;jsonwebtoken", "thebodygoeshere", 15, 8192 );
    sha256( m.p, m.n, hash );
    ok = !memcmp( hash, want, 32 );
    fails += !ok;
    printf( "sigtest prehash vector: %s\n", ok ? "match" : "MISMATCH" );

    unb64( "AAAAAQHY4xgs5DyIujFG5E5MZ4D1xjd9Up+H4AKLoyBHd95MAUZcabUN//Y/gijed4vvKtlfp4Cd4dJzVhpK0m+sYZcYRqQjBEKAZw==", &sig );
    unb64( "GJS1AAhiPYw0ZSQJDCF8kcZkKAc2tRWXAN6Yw-o_hMQ", &x );
    unb64( "UAQJHUc_yVIhUQgIvoED3kt0JDz_G8jgXys_XR2s-Xc", &y );
    hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    ok = sig.n == 76 && x.n == 32 && y.n == 32;
    if (ok)
    {
        memcpy( pub + sizeof(*hdr), x.p, 32 );
        memcpy( pub + sizeof(*hdr) + 32, y.p, 32 );
        ok = BCRYPT_SUCCESS( BCryptImportKeyPair( BCRYPT_ECDSA_P256_ALG_HANDLE, NULL, BCRYPT_ECCPUBLIC_BLOB, &k, pub,
                                                  sizeof(pub), 0 ) );
    }
    if (ok)
    {
        m.n = 0;
        sign_prehash_input( &m, (unsigned char *)sig.p, (unsigned char *)sig.p + 4, "POST", "/device/authenticate", "",
                            body, strlen( body ), 8192 );
        sha256( m.p, m.n, hash );
        ok = BCRYPT_SUCCESS( BCryptVerifySignature( k, NULL, hash, 32, (UCHAR *)sig.p + 12, 64, 0 ) );
        BCryptDestroyKey( k );
    }
    fails += !ok;
    printf( "sigtest real device/authenticate signature from xal-rs: %s\n", ok ? "verifies" : "DOES NOT VERIFY" );

    /* our own key: sign, then verify through the device key */
    device_load();
    own = sign_request( 1, 8192, "POST", "/path?query=1", "XBL3.0 x=userid;jsonwebtoken", "thebodygoeshere", 15, 0 );
    unb64( own, &mine );
    ok = mine.n == 76 && !memcmp( mine.p, ver, 4 );
    if (ok)
    {
        m.n = 0;
        sign_prehash_input( &m, (unsigned char *)mine.p, (unsigned char *)mine.p + 4, "POST", "/path?query=1",
                            "XBL3.0 x=userid;jsonwebtoken", "thebodygoeshere", 15, 8192 );
        sha256( m.p, m.n, hash );
        ok = BCRYPT_SUCCESS( BCryptVerifySignature( device.key, NULL, hash, 32, (UCHAR *)mine.p + 12, 64, 0 ) );
    }
    fails += !ok;
    printf( "sigtest own proof key round trip: %s\n", ok ? "verifies" : "FAILS" );
    free( own );
    buf_free( &m );
    buf_free( &sig );
    buf_free( &x );
    buf_free( &y );
    buf_free( &mine );
    return fails;
}

/* replaces one character of the Signature header's value, as a control the service must refuse */
static void damage_signature( struct buf *h )
{
    char *s = strstr( h->p, "Signature: " ), *end;
    if (!s || !(end = strstr( s, "\r\n" )) || end - s < 20) return;
    end[-5] = end[-5] == 'A' ? 'B' : 'A';
}

static void drop_signature( struct buf *h )
{
    char *s = strstr( h->p, "Signature: " ), *end;
    if (!s || !(end = strstr( s, "\r\n" ))) return;
    memmove( s, end + 2, strlen( end + 2 ) + 1 );
    h->n = strlen( h->p );
}

static int selftest_devicetoken( const char *type )
{
    struct json *answer = NULL, *r;
    char *token = NULL, *body, *dbody, *pk;
    struct buf h = {0}, resp = {0};
    DWORD status;
    HRESULT hr;

    if (type && *type) SetEnvironmentVariableA( "WINE_UWP_XBOX_DEVICE_TYPE", type );
    hr = xbl_device_token( &token, TRUE, &answer );
    printf( "device token -> %#lx: %u chars, NotAfter %s, did %s\n", hr, token ? (unsigned)strlen( token ) : 0,
            json_str( json_get( answer, "NotAfter" ) ),
            *json_str( json_get( json_get( json_get( answer, "DisplayClaims" ), "xdi" ), "did" ) ) ? "present" : "absent" );
    json_free( answer );
    if (FAILED(hr)) return 1;

    /* an XSTS request with the device token alone, and the same with a damaged signature */
    body = strf( "{\"RelyingParty\":\"http://xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"SandboxId\":\"RETAIL\","
                 "\"DeviceToken\":\"%s\"}}", token );
    signed_headers( &h, "https://xsts.auth.xboxlive.com/xsts/authorize", body, FALSE );
    status = http_request( L"POST", "https://xsts.auth.xboxlive.com/xsts/authorize", buf_str( &h ), body, strlen( body ), &resp );
    r = json_parse( resp.p, resp.n );
    printf( "xsts with the device token alone: HTTP %lu, token %u chars\n", status,
            (unsigned)strlen( json_str( json_get( r, "Token" ) ) ) );
    json_free( r );
    damage_signature( &h );
    status = http_request( L"POST", "https://xsts.auth.xboxlive.com/xsts/authorize", buf_str( &h ), body, strlen( body ), &resp );
    printf( "control, xsts with a damaged signature: HTTP %lu\n", status );
    str_wipe_free( body );

    /* device/authenticate itself: signed, unsigned, and with one signature bit flipped */
    pk = proof_key_json();
    dbody = strf( "{\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"AuthMethod\":"
                  "\"ProofOfPossession\",\"Id\":\"%s\",\"DeviceType\":\"Win32\",\"Version\":\"10.0.26100\",\"ProofKey\":%s}}",
                  S(device.id), pk );
    free( pk );
    h.n = 0;
    signed_headers( &h, "https://device.auth.xboxlive.com/device/authenticate", dbody, FALSE );
    status = http_request( L"POST", "https://device.auth.xboxlive.com/device/authenticate", buf_str( &h ), dbody,
                           strlen( dbody ), &resp );
    printf( "device/authenticate, signed: HTTP %lu\n", status );
    {
        struct buf u = {0};
        buf_add( &u, buf_str( &h ) );
        drop_signature( &u );
        status = http_request( L"POST", "https://device.auth.xboxlive.com/device/authenticate", buf_str( &u ), dbody,
                               strlen( dbody ), &resp );
        printf( "device/authenticate, no Signature: HTTP %lu\n", status );
        buf_free( &u );
    }
    damage_signature( &h );
    status = http_request( L"POST", "https://device.auth.xboxlive.com/device/authenticate", buf_str( &h ), dbody,
                           strlen( dbody ), &resp );
    printf( "device/authenticate, damaged signature: HTTP %lu\n", status );
    free( dbody );
    buf_free( &h );
    buf_free( &resp );
    str_wipe_free( token );
    return 0;
}

static int selftest_license( const char *content_id )
{
    struct content_key *keys = NULL;
    char *pfn = NULL;
    unsigned count = 0, i;
    HRESULT hr;

    load_refresh_token();
    hr = content_keys( content_id ? content_id : "", "US", &keys, &count, &pfn );
    printf( "content_keys -> %#lx, %u keys, package %s\n", hr, count, S(pfn) );
    for (i = 0; i < count; i++)
    {
        GUID g;
        memcpy( &g, keys[i].id, 16 );
        printf( "  key {%08lx-%04x-%04x-...} %u bytes\n", g.Data1, g.Data2, g.Data3, (unsigned)keys[i].key_len );
    }
    SecureZeroMemory( keys, count * sizeof(*keys) );
    free( keys );
    free( pfn );
    return SUCCEEDED(hr) ? 0 : 1;
}

int WINAPI XboxAuthSelfTest( const char *what, const char *arg )
{
    account_init();
    if (!strcmp( what, "sigtest" )) return selftest_sigtest();
    if (!strcmp( what, "devicetoken" )) return selftest_devicetoken( arg );
    if (!strcmp( what, "license" )) return selftest_license( arg );
    if (!strcmp( what, "windev" ))
    {
        /* this prefix's Windows device (provisioned once) and a ticket with it; shapes only */
        char *ticket = NULL;
        HRESULT hr = windev_device_ticket( &ticket );
        printf( "device ticket for www.microsoft.com -> %#lx: %u chars\n", hr, ticket ? (unsigned)strlen( ticket ) : 0 );
        str_wipe_free( ticket );
        return SUCCEEDED(hr) ? 0 : 1;
    }
    if (!strcmp( what, "owns" ))
    {
        HRESULT hr = collections_owns( arg ? arg : "" );
        printf( "owns %s -> %#lx\n", arg ? arg : "", hr );
        return 0;
    }
    if (!strcmp( what, "msaticket" ))
    {
        char *token = NULL;
        HRESULT hr;
        load_refresh_token();
        EnterCriticalSection( &account.signin_cs );
        hr = msa_refresh( arg ? arg : "", &token );
        LeaveCriticalSection( &account.signin_cs );
        printf( "msa ticket for \"%s\" -> %#lx: %u chars\n", arg ? arg : "", hr, token ? (unsigned)strlen( token ) : 0 );
        str_wipe_free( token );
        return SUCCEEDED(hr) ? 0 : 1;
    }
    if (!strcmp( what, "policy" ))
    {
        struct signing_policy sp = {0};
        BOOL found = find_policy( arg ? arg : "", &sp );
        printf( "signature policy for %s: %s version %d max body %I64u\n", arg ? arg : "", found ? "yes" : "none", sp.version,
                sp.max_body_bytes );
        return 0;
    }
    if (!strcmp( what, "endpoints" ))
    {
        char *rp = relying_party( arg ? arg : "" );
        printf( "relying party for %s: \"%s\"\n", arg ? arg : "", S(rp) );
        free( rp );
        return 0;
    }
    if (!strcmp( what, "xbl" ))
    {
        char *t = NULL;
        ULONGLONG e;
        HRESULT hr = xbl_user_authenticate( arg ? arg : "invalid", &t, &e );
        printf( "user/authenticate -> %#lx\n", hr );
        free( t );
        t = NULL;
        hr = xbl_xsts( "invalid", "http://xboxlive.com", &t, &e, NULL, FALSE );
        printf( "xsts -> %#lx\n", hr );
        free( t );
        return 0;
    }
    if (!strcmp( what, "sisu" ))
    {
        char *u = NULL, *x = NULL;
        ULONGLONG ue, xe;
        struct json *answer = NULL;
        HRESULT hr = xbl_sisu_authorize( "invalid", &u, &ue, &x, &xe, &answer );
        printf( "sisu/authorize with an invalid ticket -> %#lx\n", hr );
        json_free( answer );
        free( u );
        free( x );
        return 0;
    }
    if (!strcmp( what, "silent" ))
    {
        HRESULT hr = sign_in_silently();
        printf( "silent sign-in -> %#lx\n", hr );
        if (SUCCEEDED(hr))
        {
            char *h = NULL;
            hr = get_token_for_url( arg && *arg ? arg : "https://profile.xboxlive.com/users/me/profile/settings", FALSE, &h, TRUE );
            printf( "token -> %#lx, header %u chars\n", hr, h ? (unsigned)strlen( h ) : 0 );
            str_wipe_free( h );
        }
        return SUCCEEDED(hr) ? 0 : 1;
    }
    if (!strcmp( what, "get" ) && arg)
    {
        unsigned char *body = NULL;
        UINT32 len = 0;
        DWORD status = 0;
        const char *contract = getenv( "XBL_CONTRACT" ), *out = getenv( "XBL_OUT" );
        HRESULT hr = XboxAuthHttpGet( arg, getenv( "XBL_EXTRA" ), contract ? contract : "3", &body, &len, &status );
        printf( "get -> %#lx, HTTP %lu, %u bytes\n%.600s\n", hr, status, len, body ? (char *)body : "" );
        if (out && body)
        {
            FILE *f = fopen( out, "wb" );
            if (f)
            {
                fwrite( body, 1, len, f );
                fclose( f );
            }
        }
        CoTaskMemFree( body );
        return status == 200 ? 0 : 1;
    }
    if (!strcmp( what, "devicecode" ))
    {
        struct buf resp = {0};
        struct json *j;
        char *scope = urlencode( MSA_SCOPE ), *form = strf( "client_id=%s&scope=%s&response_type=device_code", MSA_CLIENT_ID, scope );
        DWORD status = http_form( "https://login.live.com/oauth20_connect.srf", form, &resp );
        j = json_parse( resp.p, resp.n );
        printf( "device code: HTTP %lu, verification %s, device code %u chars\n", status,
                json_str( json_get( j, "verification_uri" ) ), (unsigned)strlen( json_str( json_get( j, "device_code" ) ) ) );
        json_free( j );
        buf_free( &resp );
        free( form );
        free( scope );
        return status == 200 ? 0 : 1;
    }
    if (!strcmp( what, "browser" ))
    {
        open_browser( arg ? arg : "https://www.microsoft.com/link" );
        return 0;
    }
    if (!strcmp( what, "halodevice" ))
    {
        /* one-time device-code sign-in for Halo's MSAAppId */
        HRESULT hr = sign_in_with_device_code_client( HALO_MSA_APP_ID, NULL, FALSE );
        printf( "halodevice -> %#lx\n", hr );
        return SUCCEEDED(hr) ? 0 : 1;
    }
    return 2;
}

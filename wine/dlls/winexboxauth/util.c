/*
 * Xbox sign-in: strings, HTTP, the store, and the COM plumbing
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

const GUID IID_IAgileObject_ = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
const GUID IID_IAsyncInfo_ = {0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
const GUID IID_IAsyncAction_ = {0x5a648006,0x843a,0x4da9,{0x86,0x5b,0x9d,0x26,0xe5,0xdf,0xad,0x7b}};
const GUID IID_IClosable_ = {0x30d5a829,0x7fa4,0x4026,{0x83,0xbb,0xd7,0x5b,0xae,0x4e,0xa9,0x9e}};
const GUID IID_IMap_String_String = {0xf6d1f700,0x49c2,0x52ae,{0x81,0x54,0x82,0x6f,0x99,0x08,0x77,0x3c}};
const GUID IID_IIterable_KVP_String_String = {0xe9bdaaf0,0xcbf6,0x5c72,{0xbe,0x90,0x29,0xcb,0xf3,0xa1,0x31,0x9b}};

/* ---- strings ---- */

static BOOL buf_reserve( struct buf *b, size_t len )
{
    size_t cap = b->cap ? b->cap : 64;
    char *grown;

    if (b->n + len + 1 <= b->cap) return TRUE;
    while (cap < b->n + len + 1) cap *= 2;
    if (!(grown = realloc( b->p, cap ))) return FALSE;
    b->p = grown;
    b->cap = cap;
    return TRUE;
}

void buf_addn( struct buf *b, const void *data, size_t len )
{
    if (!buf_reserve( b, len )) return;
    if (len) memcpy( b->p + b->n, data, len );
    b->n += len;
    b->p[b->n] = 0;
}

void buf_add( struct buf *b, const char *str )
{
    buf_addn( b, S(str), strlen( S(str) ) );
}

void buf_addc( struct buf *b, char c )
{
    buf_addn( b, &c, 1 );
}

static void buf_vprintf( struct buf *b, const char *fmt, va_list args )
{
    va_list copy;
    int len;

    va_copy( copy, args );
    len = vsnprintf( NULL, 0, fmt, copy );
    va_end( copy );
    if (len < 0 || !buf_reserve( b, len )) return;
    vsnprintf( b->p + b->n, len + 1, fmt, args );
    b->n += len;
}

void WINAPIV buf_printf( struct buf *b, const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    buf_vprintf( b, fmt, args );
    va_end( args );
}

const char *buf_str( const struct buf *b )
{
    return b->p ? b->p : "";
}

void buf_free( struct buf *b )
{
    if (b->p) SecureZeroMemory( b->p, b->cap );
    free( b->p );
    memset( b, 0, sizeof(*b) );
}

char *WINAPIV strf( const char *fmt, ... )
{
    struct buf b = {0};
    va_list args;
    va_start( args, fmt );
    buf_vprintf( &b, fmt, args );
    va_end( args );
    return b.p ? b.p : str_dup( "" );
}

char *str_dupn( const char *str, size_t len )
{
    char *ret = malloc( len + 1 );
    if (!ret) return NULL;
    if (len) memcpy( ret, str, len );
    ret[len] = 0;
    return ret;
}

char *str_dup( const char *str )
{
    return str_dupn( S(str), strlen( S(str) ) );
}

void str_wipe_free( char *str )
{
    if (str) SecureZeroMemory( str, strlen( str ) );
    free( str );
}

void str_set( char **dst, const char *src )
{
    char *old = *dst;
    *dst = str_dup( src );
    str_wipe_free( old );
}

char *str_lower( char *str )
{
    char *p;
    for (p = str; p && *p; p++) *p = tolower( (unsigned char)*p );
    return str;
}

BOOL str_ends_with( const char *str, const char *suffix )
{
    size_t a = strlen( S(str) ), b = strlen( S(suffix) );
    return a > b && !strcmp( str + a - b, suffix );
}

char *urlencode( const char *str )
{
    static const char hex[] = "0123456789ABCDEF";
    struct buf o = {0};
    const unsigned char *p;

    for (p = (const unsigned char *)S(str); *p; p++)
    {
        if (isalnum( *p ) || *p == '-' || *p == '_' || *p == '.' || *p == '~') buf_addc( &o, *p );
        else
        {
            buf_addc( &o, '%' );
            buf_addc( &o, hex[*p >> 4] );
            buf_addc( &o, hex[*p & 15] );
        }
    }
    return o.p ? o.p : str_dup( "" );
}

char *json_escape( const char *str )
{
    struct buf o = {0};
    const unsigned char *p;

    for (p = (const unsigned char *)S(str); *p; p++)
    {
        if (*p == '"' || *p == '\\')
        {
            buf_addc( &o, '\\' );
            buf_addc( &o, *p );
        }
        else if (*p < 0x20) buf_printf( &o, "\\u%04x", *p );
        else buf_addc( &o, *p );
    }
    return o.p ? o.p : str_dup( "" );
}

char *narrow( const WCHAR *str, size_t len )
{
    int n;
    char *ret;

    if (!str || !len) return str_dup( "" );
    n = WideCharToMultiByte( CP_UTF8, 0, str, len, NULL, 0, NULL, NULL );
    if (!(ret = malloc( n + 1 ))) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, str, len, ret, n, NULL, NULL );
    ret[n] = 0;
    return ret;
}

WCHAR *widen( const char *str )
{
    int n = MultiByteToWideChar( CP_UTF8, 0, S(str), -1, NULL, 0 );
    WCHAR *ret = malloc( n * sizeof(WCHAR) );
    if (ret) MultiByteToWideChar( CP_UTF8, 0, S(str), -1, ret, n );
    return ret;
}

char *from_hstr( HSTRING str )
{
    UINT32 len = 0;
    const WCHAR *p = WindowsGetStringRawBuffer( str, &len );
    return narrow( p, len );
}

HRESULT to_hstr( const char *str, HSTRING *out )
{
    WCHAR *w;
    HRESULT hr;

    if (!out) return E_POINTER;
    if (!(w = widen( str ))) return E_OUTOFMEMORY;
    hr = WindowsCreateString( w, wcslen( w ), out );
    free( w );
    return hr;
}

HRESULT to_hstrw( const WCHAR *str, HSTRING *out )
{
    if (!out) return E_POINTER;
    if (!str) str = L"";
    return WindowsCreateString( str, wcslen( str ), out );
}

ULONGLONG now_s( void )
{
    return (ULONGLONG)time( NULL );
}

/* ISO 8601 "2026-10-15T10:11:12.1234567Z" -> unix seconds; 0 if unparseable */
ULONGLONG parse_iso( const char *str )
{
    SYSTEMTIME st = {0};
    FILETIME ft;
    int y, mo, d, h, mi, se;
    ULONGLONG t;

    if (sscanf( S(str), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se ) != 6) return 0;
    st.wYear = y; st.wMonth = mo; st.wDay = d; st.wHour = h; st.wMinute = mi; st.wSecond = se;
    if (!SystemTimeToFileTime( &st, &ft )) return 0;
    t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10000000 - 11644473600ull;
}

char *b64( const unsigned char *p, size_t n, BOOL url )
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    struct buf o = {0};
    size_t i;

    for (i = 0; i < n; i += 3)
    {
        unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        buf_addc( &o, t[v >> 18 & 63] );
        buf_addc( &o, t[v >> 12 & 63] );
        buf_addc( &o, i + 1 < n ? t[v >> 6 & 63] : '=' );
        buf_addc( &o, i + 2 < n ? t[v & 63] : '=' );
    }
    if (!o.p) return str_dup( "" );
    if (url)
    {
        char *c;
        while (o.n && o.p[o.n - 1] == '=') o.p[--o.n] = 0;
        for (c = o.p; *c; c++) *c = *c == '+' ? '-' : *c == '/' ? '_' : *c;
    }
    return o.p;
}

void unb64( const char *s, struct buf *out )
{
    unsigned v = 0;
    int bits = 0;

    buf_addn( out, "", 0 );
    for (; s && *s; s++)
    {
        char c = *s;
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else continue;
        v = v << 6 | d;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            buf_addc( out, (char)(v >> bits & 0xff) );
        }
    }
}

BOOL sha256( const void *data, size_t len, unsigned char out[32] )
{
    return BCRYPT_SUCCESS( BCryptHash( BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (UCHAR *)data, len, out, 32 ) );
}

void hdr_add( struct buf *headers, const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    buf_vprintf( headers, fmt, args );
    va_end( args );
    buf_add( headers, "\r\n" );
}

/* ---- HTTP, with WinHTTP ---- */

static HINTERNET session;

static BOOL WINAPI open_session( INIT_ONCE *once, void *param, void **context )
{
    session = WinHttpOpen( L"XboxAuthManager/1.0 (wine-uwp)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
    if (!session)
        session = WinHttpOpen( L"XboxAuthManager/1.0 (wine-uwp)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
    return TRUE;
}

/* Returns the HTTP status, or 0 if the request did not complete. */
DWORD http_request( const WCHAR *method, const char *url, const char *headers, const void *body, size_t body_len,
                    struct buf *response )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    URL_COMPONENTS uc = { sizeof(uc) };
    WCHAR host[256], path[2048], *wurl, *object = NULL, *whdrs = NULL;
    HINTERNET connect = NULL, request = NULL;
    DWORD status = 0, size = sizeof(status);
    size_t len;

    response->n = 0;
    buf_addn( response, "", 0 );
    InitOnceExecuteOnce( &once, open_session, NULL, NULL );
    if (!(wurl = widen( url ))) return 0;
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAY_SIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAY_SIZE(path);
    uc.dwExtraInfoLength = ~0u;
    if (!WinHttpCrackUrl( wurl, 0, 0, &uc )) goto done;
    len = wcslen( path ) + (uc.lpszExtraInfo ? uc.dwExtraInfoLength : 0) + 1;
    if (!(object = malloc( len * sizeof(WCHAR) ))) goto done;
    wcscpy( object, path );
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength)
    {
        memcpy( object + wcslen( path ), uc.lpszExtraInfo, uc.dwExtraInfoLength * sizeof(WCHAR) );
        object[len - 1] = 0;
    }
    if (headers && *headers) whdrs = widen( headers );

    if (!session) goto done;
    if (!(connect = WinHttpConnect( session, host, uc.nPort, 0 ))) goto done;
    if (!(request = WinHttpOpenRequest( connect, method, object, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0 )))
        goto done;
    WinHttpSetTimeouts( request, 15000, 15000, 30000, 30000 );
    if (!WinHttpSendRequest( request, whdrs ? whdrs : WINHTTP_NO_ADDITIONAL_HEADERS, ~0u, (void *)body, body_len,
                             body_len, 0 ))
        goto done;
    if (!WinHttpReceiveResponse( request, NULL )) goto done;
    if (!WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL ))
        status = 0;
    for (;;)
    {
        DWORD avail = 0, read = 0;
        char *chunk;
        if (!WinHttpQueryDataAvailable( request, &avail ) || !avail) break;
        if (!(chunk = malloc( avail ))) break;
        if (!WinHttpReadData( request, chunk, avail, &read ))
        {
            free( chunk );
            break;
        }
        buf_addn( response, chunk, read );
        free( chunk );
        if (response->n > (16u << 20)) break;
    }
done:
    if (!status)
    {
        char *shown = str_dup( url );
        if (shown && strchr( shown, '?' )) *strchr( shown, '?' ) = 0;
        WARN( "%s %s: request failed, error %lu\n", debugstr_w(method), S(shown), GetLastError() );
        free( shown );
    }
    if (request) WinHttpCloseHandle( request );
    if (connect) WinHttpCloseHandle( connect );
    free( whdrs );
    free( object );
    free( wurl );
    return status;
}

DWORD http_form( const char *url, const char *form, struct buf *response )
{
    return http_request( L"POST", url, "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n",
                         form, strlen( form ), response );
}

/* ---- the store ---- */

const char *kv_get( const struct kv *kv, const char *key )
{
    unsigned i;
    for (i = 0; i < kv->count; i++) if (!strcmp( kv->keys[i], key )) return kv->vals[i];
    return "";
}

void kv_set( struct kv *kv, const char *key, const char *val )
{
    unsigned i;

    for (i = 0; i < kv->count; i++)
    {
        if (strcmp( kv->keys[i], key )) continue;
        if (val && *val)
        {
            str_set( &kv->vals[i], val );
            return;
        }
        free( kv->keys[i] );
        str_wipe_free( kv->vals[i] );
        kv->count--;
        memmove( kv->keys + i, kv->keys + i + 1, (kv->count - i) * sizeof(char *) );
        memmove( kv->vals + i, kv->vals + i + 1, (kv->count - i) * sizeof(char *) );
        return;
    }
    if (!val || !*val) return;
    kv->keys = realloc( kv->keys, (kv->count + 1) * sizeof(char *) );
    kv->vals = realloc( kv->vals, (kv->count + 1) * sizeof(char *) );
    kv->keys[kv->count] = str_dup( key );
    kv->vals[kv->count] = str_dup( val );
    kv->count++;
}

void kv_free( struct kv *kv )
{
    unsigned i;
    for (i = 0; i < kv->count; i++)
    {
        free( kv->keys[i] );
        str_wipe_free( kv->vals[i] );
    }
    free( kv->keys );
    free( kv->vals );
    memset( kv, 0, sizeof(*kv) );
}

static WCHAR *store_path( BOOL create_dir )
{
    WCHAR base[MAX_PATH], *path;
    DWORD n = GetEnvironmentVariableW( L"WINE_UWP_XBOXAUTH_STORE", base, MAX_PATH );

    if (n && n < MAX_PATH) return wcsdup( base );
    n = GetEnvironmentVariableW( L"LOCALAPPDATA", base, MAX_PATH );
    if (!n || n >= MAX_PATH) wcscpy( base, L"C:\\users\\Public" );
    wcscat( base, L"\\wineuwp" );
    if (create_dir) CreateDirectoryW( base, NULL );
    if (!(path = malloc( (wcslen( base ) + 16) * sizeof(WCHAR) ))) return NULL;
    wcscpy( path, base );
    wcscat( path, L"\\xboxauth.bin" );
    return path;
}

void store_read( struct kv *kv )
{
    WCHAR *path = store_path( FALSE );
    HANDLE file;
    DWORD size, read = 0;
    char *blob = NULL, *p, *nl, *eq;
    DATA_BLOB in, out = {0};

    memset( kv, 0, sizeof(*kv) );
    if (!path) return;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, 0, NULL );
    free( path );
    if (file == INVALID_HANDLE_VALUE) return;
    size = GetFileSize( file, NULL );
    if (size != INVALID_FILE_SIZE && size < (1u << 20) && (blob = malloc( size + 1 )))
        if (!ReadFile( file, blob, size, &read, NULL )) read = 0;
    CloseHandle( file );
    in.cbData = read;
    in.pbData = (BYTE *)blob;
    if (!read || !CryptUnprotectData( &in, NULL, NULL, NULL, NULL, 0, &out ))
    {
        WARN( "stored sign-in could not be read (error %lu)\n", GetLastError() );
        free( blob );
        return;
    }
    free( blob );
    if (!(blob = str_dupn( (char *)out.pbData, out.cbData )))
    {
        LocalFree( out.pbData );
        return;
    }
    SecureZeroMemory( out.pbData, out.cbData );
    LocalFree( out.pbData );
    for (p = blob; *p; p = nl + 1)
    {
        if (!(nl = strchr( p, '\n' ))) nl = p + strlen( p );
        if ((eq = memchr( p, '=', nl - p )))
        {
            char *key = str_dupn( p, eq - p ), *val = str_dupn( eq + 1, nl - eq - 1 );
            kv_set( kv, key, val );
            free( key );
            str_wipe_free( val );
        }
        if (!*nl) break;
    }
    str_wipe_free( blob );
}

static int __cdecl cmp_keys( const void *a, const void *b )
{
    return strcmp( **(char ***)a, **(char ***)b );
}

BOOL store_write( const struct kv *kv )
{
    WCHAR *path = store_path( TRUE ), *tmp;
    struct buf text = {0};
    DATA_BLOB in, out = {0};
    char ***order;
    HANDLE file;
    DWORD written = 0;
    unsigned i;
    BOOL ok;

    if (!path) return FALSE;
    if (!kv->count)
    {
        DeleteFileW( path );
        free( path );
        return TRUE;
    }
    /* in key order, as the stand-in this replaces wrote it */
    order = malloc( kv->count * sizeof(*order) );
    for (i = 0; i < kv->count; i++) order[i] = &kv->keys[i];
    qsort( order, kv->count, sizeof(*order), cmp_keys );
    for (i = 0; i < kv->count; i++)
    {
        unsigned idx = order[i] - kv->keys;
        buf_printf( &text, "%s=%s\n", kv->keys[idx], kv->vals[idx] );
    }
    free( order );
    in.cbData = text.n;
    in.pbData = (BYTE *)text.p;
    if (!CryptProtectData( &in, L"wine-uwp Xbox sign-in", NULL, NULL, NULL, 0, &out ))
    {
        WARN( "sign-in not saved: CryptProtectData failed, error %lu\n", GetLastError() );
        buf_free( &text );
        free( path );
        return FALSE;
    }
    buf_free( &text );
    tmp = malloc( (wcslen( path ) + 8) * sizeof(WCHAR) );
    wcscpy( tmp, path );
    wcscat( tmp, L".tmp" );
    file = CreateFileW( tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    ok = file != INVALID_HANDLE_VALUE && WriteFile( file, out.pbData, out.cbData, &written, NULL ) && written == out.cbData;
    if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
    LocalFree( out.pbData );
    if (ok) ok = MoveFileExW( tmp, path, MOVEFILE_REPLACE_EXISTING );
    if (!ok) WARN( "sign-in not saved, error %lu\n", GetLastError() );
    free( tmp );
    free( path );
    return ok;
}

/* Read-modify-write of the store, under a prefix-wide mutex: the app and its FT server share the file, and each
 * keeps only some of its keys in memory. */
static HANDLE store_mutex;

static BOOL WINAPI create_store_mutex( INIT_ONCE *once, void *param, void **context )
{
    store_mutex = CreateMutexW( NULL, FALSE, L"Global\\wineuwp-xboxauth-store" );
    if (!store_mutex) store_mutex = CreateMutexW( NULL, FALSE, L"Local\\wineuwp-xboxauth-store" );
    return TRUE;
}

static HANDLE get_store_mutex( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce( &once, create_store_mutex, NULL, NULL );
    return store_mutex;
}

void store_lock( void )
{
    if (get_store_mutex()) WaitForSingleObject( get_store_mutex(), 10000 );
}

void store_unlock( void )
{
    if (get_store_mutex()) ReleaseMutex( get_store_mutex() );
}

BOOL WINAPIV store_update( const char *key, ... )
{
    struct kv kv;
    va_list args;
    BOOL ok;

    store_lock();
    store_read( &kv );
    va_start( args, key );
    while (key)
    {
        const char *val = va_arg( args, const char * );
        kv_set( &kv, key, val );
        key = va_arg( args, const char * );
    }
    va_end( args );
    ok = store_write( &kv );
    kv_free( &kv );
    store_unlock();
    return ok;
}

char *store_get( const char *key )
{
    struct kv kv;
    char *ret;

    store_lock();
    store_read( &kv );
    store_unlock();
    ret = str_dup( kv_get( &kv, key ) );
    kv_free( &kv );
    return ret;
}

/* ---- the object base ---- */

BOOL base_iid( REFIID iid )
{
    return IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject_ );
}

void *object_create( size_t size, const WCHAR *name, const char *trace_name, void (*destroy)( struct object * ) )
{
    struct object *obj = calloc( 1, size );
    if (!obj) return NULL;
    obj->ref = 1;
    obj->name = name;
    obj->trace_name = trace_name;
    obj->destroy = destroy;
    return obj;
}

void object_init_static( struct object *obj, const WCHAR *name, const char *trace_name )
{
    obj->ref = 1;
    obj->is_static = TRUE;
    obj->name = name;
    obj->trace_name = trace_name;
}

void object_add( struct object *obj, const void **vtbl, const GUID *iid )
{
    obj->vtbl[obj->count] = vtbl;
    obj->iids[obj->count] = *iid;
    obj->count++;
}

HRESULT obj_QueryInterface( struct object *obj, REFIID iid, void **out )
{
    unsigned i;

    if (!out) return E_POINTER;
    if (base_iid( iid ))
    {
        *out = &obj->vtbl[0];
        obj_AddRef( obj );
        return S_OK;
    }
    for (i = 0; i < obj->count; i++)
    {
        if (!IsEqualGUID( iid, &obj->iids[i] )) continue;
        *out = &obj->vtbl[i];
        obj_AddRef( obj );
        return S_OK;
    }
    if (obj->trace_name) TRACE( "%s: QueryInterface refused %s\n", obj->trace_name, debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

ULONG obj_AddRef( struct object *obj )
{
    if (obj->is_static) return 2;
    return InterlockedIncrement( &obj->ref );
}

ULONG obj_Release( struct object *obj )
{
    ULONG ref;

    if (obj->is_static) return 1;
    if (!(ref = InterlockedDecrement( &obj->ref )))
    {
        if (obj->destroy) obj->destroy( obj );
        free( obj );
    }
    return ref;
}

#define DEFINE_OBJ_METHODS(n) \
    HRESULT WINAPI obj##n##_QueryInterface( void *iface, REFIID iid, void **out ) \
    { return obj_QueryInterface( OBJ_AT(iface, n), iid, out ); } \
    ULONG WINAPI obj##n##_AddRef( void *iface ) { return obj_AddRef( OBJ_AT(iface, n) ); } \
    ULONG WINAPI obj##n##_Release( void *iface ) { return obj_Release( OBJ_AT(iface, n) ); } \
    HRESULT WINAPI obj##n##_GetIids( void *iface, ULONG *count, IID **ids ) \
    { if (count) *count = 0; if (ids) *ids = NULL; return S_OK; } \
    HRESULT WINAPI obj##n##_GetRuntimeClassName( void *iface, HSTRING *name ) \
    { return to_hstrw( OBJ_AT(iface, n)->name, name ); } \
    HRESULT WINAPI obj##n##_GetTrustLevel( void *iface, TrustLevel *level ) \
    { if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }

DEFINE_OBJ_METHODS(0)
DEFINE_OBJ_METHODS(1)
DEFINE_OBJ_METHODS(2)
DEFINE_OBJ_METHODS(3)
DEFINE_OBJ_METHODS(4)

HRESULT WINAPI closable_Close( void *iface )
{
    return S_OK;
}

/* ---- IAsyncOperation<T> / IAsyncAction, completed from any thread ---- */

static void async_destroy( struct object *obj )
{
    struct async *op = CONTAINING_RECORD( obj, struct async, obj );
    if (op->handler) IUnknown_Release( op->handler );
    if (op->iface) IUnknown_Release( op->iface );
    str_wipe_free( op->s1 );
    str_wipe_free( op->s2 );
    DeleteCriticalSection( &op->cs );
}

static struct async *async_from( void *iface, int n )
{
    return CONTAINING_RECORD( OBJ_AT(iface, n), struct async, obj );
}

static void async_invoke( struct async *op, IUnknown *handler, INT32 status )
{
    SLOT(handler, 3, HRESULT (WINAPI *)( IUnknown *, void *, INT32 ))( handler, &op->obj.vtbl[0], status );
}

static HRESULT WINAPI async_put_Completed( void *iface, IUnknown *handler )
{
    struct async *op = async_from( iface, 0 );
    INT32 status;

    EnterCriticalSection( &op->cs );
    if (op->handler)
    {
        LeaveCriticalSection( &op->cs );
        return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    }
    if (handler) IUnknown_AddRef( handler );
    op->handler = handler;
    status = op->status;
    LeaveCriticalSection( &op->cs );
    if (handler && status) async_invoke( op, handler, status );   /* already done: call it now */
    return S_OK;
}

static HRESULT WINAPI async_get_Completed( void *iface, IUnknown **handler )
{
    struct async *op = async_from( iface, 0 );
    if (!handler) return E_POINTER;
    EnterCriticalSection( &op->cs );
    if ((*handler = op->handler)) IUnknown_AddRef( op->handler );
    LeaveCriticalSection( &op->cs );
    return S_OK;
}

static HRESULT WINAPI async_GetResults( void *iface, void *result )
{
    struct async *op = async_from( iface, 0 );
    HRESULT hr = S_OK, error;
    INT32 status;

    EnterCriticalSection( &op->cs );
    status = op->status;
    error = op->error;
    LeaveCriticalSection( &op->cs );
    if (status == 3) return error;
    if (status != 1) return E_ILLEGAL_METHOD_CALL;
    switch (op->kind)
    {
    case R_ACTION: break;
    case R_IFACE:
        if (!result) return E_POINTER;
        if ((*(IUnknown **)result = op->iface)) IUnknown_AddRef( op->iface );
        break;
    case R_STRINGS1:
        if (!result) return E_POINTER;
        hr = to_hstr( op->s1, result );
        break;
    case R_STRINGS2:
    {
        HSTRING *pair = result;
        if (!result) return E_POINTER;
        pair[0] = pair[1] = NULL;
        if (SUCCEEDED(hr = to_hstr( op->s1, &pair[0] )) && FAILED(hr = to_hstr( op->s2, &pair[1] )))
        {
            WindowsDeleteString( pair[0] );
            pair[0] = NULL;
        }
        break;
    }
    case R_I4:
        if (!result) return E_POINTER;
        *(INT32 *)result = op->i4;
        break;
    case R_TRANSFER:
        if (!result) return E_POINTER;
        ((INT32 *)result)[0] = op->i4;
        ((INT32 *)result)[1] = op->i4b;
        break;
    }
    return hr;
}

static HRESULT WINAPI async_get_Id( void *iface, UINT32 *id )
{
    if (!id) return E_POINTER;
    *id = async_from( iface, 1 )->id;
    return S_OK;
}

static HRESULT WINAPI async_get_Status( void *iface, INT32 *status )
{
    struct async *op = async_from( iface, 1 );
    if (!status) return E_POINTER;
    EnterCriticalSection( &op->cs );
    *status = op->status;
    LeaveCriticalSection( &op->cs );
    return S_OK;
}

static HRESULT WINAPI async_get_ErrorCode( void *iface, HRESULT *code )
{
    struct async *op = async_from( iface, 1 );
    if (!code) return E_POINTER;
    EnterCriticalSection( &op->cs );
    *code = op->status == 3 ? op->error : S_OK;
    LeaveCriticalSection( &op->cs );
    return S_OK;
}

static HRESULT WINAPI async_Cancel( void *iface )
{
    InterlockedExchange( &async_from( iface, 1 )->cancelled, 1 );
    return S_OK;
}

static const void *async_vtbl[] = { OBJ_METHODS(0), async_put_Completed, async_get_Completed, async_GetResults };
static const void *async_info_vtbl[] = { OBJ_METHODS(1), async_get_Id, async_get_Status, async_get_ErrorCode,
                                         async_Cancel, closable_Close };

struct async *async_create( const GUID *iid, enum result_kind kind, const char *name )
{
    static LONG next_id;
    struct async *op = object_create( sizeof(*op), kind == R_ACTION ? L"Windows.Foundation.IAsyncAction" :
                                      L"Windows.Foundation.IAsyncOperation`1", NULL, async_destroy );
    if (!op) return NULL;
    object_add( &op->obj, async_vtbl, iid );
    object_add( &op->obj, async_info_vtbl, &IID_IAsyncInfo_ );
    op->kind = kind;
    op->op_name = name;
    op->id = InterlockedIncrement( &next_id );
    InitializeCriticalSection( &op->cs );
    return op;
}

/* hr fails -> Error, E_ABORT after Cancel -> Canceled, else Completed */
void async_finish( struct async *op, HRESULT hr )
{
    IUnknown *handler;
    INT32 status;

    EnterCriticalSection( &op->cs );
    if (op->status)
    {
        LeaveCriticalSection( &op->cs );
        return;
    }
    if (hr == E_ABORT && op->cancelled) op->status = 2;
    else if (FAILED(hr))
    {
        op->status = 3;
        op->error = hr;
    }
    else op->status = 1;
    status = op->status;
    if ((handler = op->handler)) IUnknown_AddRef( handler );
    LeaveCriticalSection( &op->cs );
    TRACE( "%s #%u -> %s (hr %#lx)\n", op->op_name, op->id, status == 1 ? "Completed" : status == 2 ? "Canceled" : "Error", hr );
    if (handler)
    {
        async_invoke( op, handler, status );
        IUnknown_Release( handler );
    }
}

struct async_thread
{
    struct async *op;
    HRESULT (*work)( struct async *op, void *ctx );
    void *ctx;
    void (*free_ctx)( void *ctx );
};

static DWORD WINAPI async_thread_proc( void *param )
{
    struct async_thread *t = param;
    HRESULT hr = t->work( t->op, t->ctx );
    async_finish( t->op, hr );
    obj_Release( &t->op->obj );
    if (t->free_ctx) t->free_ctx( t->ctx );
    free( t );
    return 0;
}

struct async *async_start( const GUID *iid, enum result_kind kind, const char *name,
                           HRESULT (*work)( struct async *op, void *ctx ), void *ctx, void (*free_ctx)( void *ctx ) )
{
    struct async *op = async_create( iid, kind, name );
    struct async_thread *t = malloc( sizeof(*t) );
    HANDLE thread;

    if (!op || !t)
    {
        if (op) obj_Release( &op->obj );
        if (free_ctx) free_ctx( ctx );
        free( t );
        return NULL;
    }
    t->op = op;
    t->work = work;
    t->ctx = ctx;
    t->free_ctx = free_ctx;
    obj_AddRef( &op->obj ); /* for the thread */
    if ((thread = CreateThread( NULL, 0, async_thread_proc, t, 0, NULL ))) CloseHandle( thread );
    else
    {
        async_finish( op, E_OUTOFMEMORY );
        obj_Release( &op->obj );
        if (free_ctx) free_ctx( ctx );
        free( t );
    }
    return op;
}

/* an operation that is already complete, with result (whose reference it takes) */
HRESULT completed_iface( const GUID *iid, const char *name, IUnknown *result, void **out )
{
    struct async *op;

    if (!out)
    {
        if (result) IUnknown_Release( result );
        return E_POINTER;
    }
    if (!(op = async_create( iid, R_IFACE, name )))
    {
        if (result) IUnknown_Release( result );
        return E_OUTOFMEMORY;
    }
    op->iface = result;
    async_finish( op, S_OK );
    *out = &op->obj.vtbl[0];
    return S_OK;
}

/* ---- event lists ---- */

void events_init( struct events *e )
{
    memset( e, 0, sizeof(*e) );
    InitializeCriticalSection( &e->cs );
}

void events_free( struct events *e )
{
    unsigned i;
    for (i = 0; i < e->count; i++) IUnknown_Release( e->items[i].handler );
    free( e->items );
    DeleteCriticalSection( &e->cs );
}

HRESULT events_add( struct events *e, IUnknown *handler, INT64 *token )
{
    static LONG64 next;
    void *items;

    if (!handler || !token) return E_POINTER;
    EnterCriticalSection( &e->cs );
    if (!(items = realloc( e->items, (e->count + 1) * sizeof(*e->items) )))
    {
        LeaveCriticalSection( &e->cs );
        return E_OUTOFMEMORY;
    }
    e->items = items;
    IUnknown_AddRef( handler );
    *token = InterlockedIncrement64( &next );
    e->items[e->count].token = *token;
    e->items[e->count].handler = handler;
    e->count++;
    LeaveCriticalSection( &e->cs );
    return S_OK;
}

HRESULT events_remove( struct events *e, INT64 token )
{
    IUnknown *handler = NULL;
    unsigned i;

    EnterCriticalSection( &e->cs );
    for (i = 0; i < e->count; i++)
    {
        if (e->items[i].token != token) continue;
        handler = e->items[i].handler;
        e->count--;
        memmove( e->items + i, e->items + i + 1, (e->count - i) * sizeof(*e->items) );
        break;
    }
    LeaveCriticalSection( &e->cs );
    if (handler) IUnknown_Release( handler );
    return S_OK;
}

/* EventHandler<T>::Invoke(sender, args) is slot 3 */
void events_raise( struct events *e, void *sender, void *args )
{
    IUnknown **copy;
    unsigned i, count;

    EnterCriticalSection( &e->cs );
    count = e->count;
    if ((copy = malloc( (count + 1) * sizeof(*copy) )))
        for (i = 0; i < count; i++)
        {
            copy[i] = e->items[i].handler;
            IUnknown_AddRef( copy[i] );
        }
    LeaveCriticalSection( &e->cs );
    if (!copy) return;
    for (i = 0; i < count; i++)
    {
        SLOT(copy[i], 3, HRESULT (WINAPI *)( IUnknown *, void *, void * ))( copy[i], sender, args );
        IUnknown_Release( copy[i] );
    }
    free( copy );
}

/* ---- IVectorView<T> of interfaces, with IIterable<T> ---- */

static struct vector_view *view_from( void *iface, int n )
{
    return CONTAINING_RECORD( OBJ_AT(iface, n), struct vector_view, obj );
}

static void view_destroy( struct object *obj )
{
    struct vector_view *view = CONTAINING_RECORD( obj, struct vector_view, obj );
    UINT32 i;
    for (i = 0; i < view->count; i++) IInspectable_Release( view->items[i] );
    free( view->items );
}

static HRESULT view_get_at( struct vector_view *view, UINT32 i, IInspectable **v )
{
    if (!v) return E_POINTER;
    if (i >= view->count)
    {
        *v = NULL;
        return E_BOUNDS;
    }
    IInspectable_AddRef( (*v = view->items[i]) );
    return S_OK;
}

static HRESULT WINAPI view_GetAt( void *iface, UINT32 i, IInspectable **v )
{
    return view_get_at( view_from( iface, 0 ), i, v );
}

static HRESULT WINAPI view_get_Size( void *iface, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = view_from( iface, 0 )->count;
    return S_OK;
}

static HRESULT WINAPI view_IndexOf( void *iface, IInspectable *x, UINT32 *index, boolean *found )
{
    struct vector_view *view = view_from( iface, 0 );
    UINT32 i;

    if (!index || !found) return E_POINTER;
    *found = FALSE;
    *index = 0;
    for (i = 0; i < view->count; i++)
        if (view->items[i] == x)
        {
            *found = TRUE;
            *index = i;
            break;
        }
    return S_OK;
}

static HRESULT WINAPI view_GetMany( void *iface, UINT32 start, UINT32 n, IInspectable **out, UINT32 *got )
{
    struct vector_view *view = view_from( iface, 0 );
    UINT32 i;

    if (!got || (n && !out)) return E_POINTER;
    *got = 0;
    for (i = start; i < view->count && *got < n; i++) IInspectable_AddRef( (out[(*got)++] = view->items[i]) );
    return S_OK;
}

struct view_iterator
{
    struct object obj;
    struct vector_view *view;
    UINT32 pos;
};

static struct view_iterator *iterator_from( void *iface )
{
    return CONTAINING_RECORD( OBJ_AT(iface, 0), struct view_iterator, obj );
}

static void iterator_destroy( struct object *obj )
{
    obj_Release( &CONTAINING_RECORD( obj, struct view_iterator, obj )->view->obj );
}

static HRESULT WINAPI iterator_get_Current( void *iface, IInspectable **v )
{
    struct view_iterator *it = iterator_from( iface );
    return view_get_at( it->view, it->pos, v );
}

static HRESULT WINAPI iterator_get_HasCurrent( void *iface, boolean *v )
{
    struct view_iterator *it = iterator_from( iface );
    if (!v) return E_POINTER;
    *v = it->pos < it->view->count;
    return S_OK;
}

static HRESULT WINAPI iterator_MoveNext( void *iface, boolean *v )
{
    struct view_iterator *it = iterator_from( iface );
    if (it->pos < it->view->count) it->pos++;
    return iterator_get_HasCurrent( iface, v );
}

static const void *iterator_vtbl[] = { OBJ_METHODS(0), iterator_get_Current, iterator_get_HasCurrent, iterator_MoveNext };

static HRESULT WINAPI view_First( void *iface, IInspectable **v )
{
    struct vector_view *view = view_from( iface, 1 );
    struct view_iterator *it;

    if (!v) return E_POINTER;
    if (!(it = object_create( sizeof(*it), L"Windows.Foundation.Collections.IIterator`1", NULL, iterator_destroy )))
        return E_OUTOFMEMORY;
    object_add( &it->obj, iterator_vtbl, &view->iterator_iid );
    it->view = view;
    obj_AddRef( &view->obj );
    *v = (IInspectable *)&it->obj.vtbl[0];
    return S_OK;
}

static const void *view_vtbl[] = { OBJ_METHODS(0), view_GetAt, view_get_Size, view_IndexOf, view_GetMany };
static const void *view_iterable_vtbl[] = { OBJ_METHODS(1), view_First };

struct vector_view *vector_view_create( const GUID *view_iid, const GUID *iterable, const GUID *iterator, const WCHAR *name )
{
    struct vector_view *view = object_create( sizeof(*view), name, "VectorView", view_destroy );
    if (!view) return NULL;
    object_add( &view->obj, view_vtbl, view_iid );
    object_add( &view->obj, view_iterable_vtbl, iterable );
    view->iterator_iid = *iterator;
    return view;
}

void vector_view_add( struct vector_view *view, IInspectable *item )
{
    IInspectable **items = realloc( view->items, (view->count + 1) * sizeof(*items) );
    if (!items)
    {
        IInspectable_Release( item );
        return;
    }
    view->items = items;
    view->items[view->count++] = item;
}

/* ---- Windows.Foundation.Collections.StringMap ---- */

/* IMap<String,String>: 6 Lookup 7 get_Size 8 HasKey 9 GetView 10 Insert */
IInspectable *WINAPIV make_string_map( const char *key, ... )
{
    HSTRING cls = NULL;
    IInspectable *obj = NULL, *map = NULL;
    va_list args;
    HRESULT hr;

    to_hstrw( L"Windows.Foundation.Collections.StringMap", &cls );
    hr = RoActivateInstance( cls, &obj );
    WindowsDeleteString( cls );
    if (FAILED(hr))
    {
        WARN( "StringMap: activation failed, hr %#lx\n", hr );
        return NULL;
    }
    hr = IInspectable_QueryInterface( obj, &IID_IMap_String_String, (void **)&map );
    IInspectable_Release( obj );
    if (FAILED(hr)) return NULL;
    va_start( args, key );
    while (key)
    {
        const char *val = va_arg( args, const char * );
        HSTRING k = NULL, v = NULL;
        boolean replaced;
        to_hstr( key, &k );
        to_hstr( val, &v );
        SLOT(map, 10, HRESULT (WINAPI *)( void *, HSTRING, HSTRING, boolean * ))( map, k, v, &replaced );
        WindowsDeleteString( k );
        WindowsDeleteString( v );
        key = va_arg( args, const char * );
    }
    va_end( args );
    return map;
}

/* key=value of an app's IMap<String,String>, values cut short: they are request options, but be safe */
char *describe_map( IInspectable *map )
{
    IInspectable *iterable = NULL, *iter = NULL;
    struct buf out = {0};
    boolean has = FALSE;

    if (!map || FAILED(IInspectable_QueryInterface( map, &IID_IIterable_KVP_String_String, (void **)&iterable )))
        return str_dup( "?" );
    if (SUCCEEDED(SLOT(iterable, 6, HRESULT (WINAPI *)( void *, IInspectable ** ))( iterable, &iter )) && iter)
    {
        SLOT(iter, 7, HRESULT (WINAPI *)( void *, boolean * ))( iter, &has );
        while (has)
        {
            IInspectable *cur = NULL;
            if (SUCCEEDED(SLOT(iter, 6, HRESULT (WINAPI *)( void *, IInspectable ** ))( iter, &cur )) && cur)
            {
                HSTRING k = NULL, v = NULL;
                char *key, *val;
                SLOT(cur, 6, HRESULT (WINAPI *)( void *, HSTRING * ))( cur, &k );
                SLOT(cur, 7, HRESULT (WINAPI *)( void *, HSTRING * ))( cur, &v );
                key = from_hstr( k );
                val = from_hstr( v );
                if (out.n) buf_add( &out, ", " );
                buf_printf( &out, "%s=%.48s%s", S(key), S(val), strlen( S(val) ) > 48 ? "..." : "" );
                free( key );
                str_wipe_free( val );
                WindowsDeleteString( k );
                WindowsDeleteString( v );
                IInspectable_Release( cur );
            }
            if (FAILED(SLOT(iter, 8, HRESULT (WINAPI *)( void *, boolean * ))( iter, &has ))) break;
        }
        IInspectable_Release( iter );
    }
    IInspectable_Release( iterable );
    return out.p ? out.p : str_dup( "" );
}

/* the value of key in an IMap<String,String>, NULL when it has none */
char *map_lookup( IInspectable *map, const WCHAR *key )
{
    IInspectable *m = NULL;
    HSTRING k = NULL, v = NULL;
    boolean has = FALSE;
    char *ret = NULL;

    if (!map || FAILED(IInspectable_QueryInterface( map, &IID_IMap_String_String, (void **)&m ))) return NULL;
    to_hstrw( key, &k );
    if (SUCCEEDED(SLOT(m, 8, HRESULT (WINAPI *)( void *, HSTRING, boolean * ))( m, k, &has )) && has &&
        SUCCEEDED(SLOT(m, 6, HRESULT (WINAPI *)( void *, HSTRING, HSTRING * ))( m, k, &v )))
    {
        ret = from_hstr( v );
        WindowsDeleteString( v );
    }
    WindowsDeleteString( k );
    IInspectable_Release( m );
    return ret;
}

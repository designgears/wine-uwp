/*
 * XUser, from the Xbox sign-in kept by winexboxauth.dll
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "private.h"
#include "wincrypt.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdk);

/*
 * The user is the one signed in to the Xbox app. winexboxauth.dll keeps that sign-in in
 * %LOCALAPPDATA%\wineuwp\xboxauth.bin (DPAPI-protected "key=value" lines; WINE_UWP_XBOXAUTH_STORE overrides the path)
 * and signs the title's requests for it.
 */

enum { XUserAgeGroup_Unknown, XUserAgeGroup_Child, XUserAgeGroup_Teen, XUserAgeGroup_Adult };
enum { XUserGamertagComponent_Classic, XUserGamertagComponent_Modern, XUserGamertagComponent_ModernSuffix,
       XUserGamertagComponent_UniqueModern };
enum { XUserState_SignedIn, XUserState_SigningOut, XUserState_SignedOut };
enum { XUserPrivilegeDenyReason_None, XUserPrivilegeDenyReason_PurchaseRequired, XUserPrivilegeDenyReason_Restricted };
#define XUserGetTokenAndSignatureOptions_ForceRefresh 1
#define XUserAddOptions_AddDefaultUserAllowingUI 4

typedef struct { UINT64 value; } XUserLocalId;

typedef struct
{
    SIZE_T tokenSize;
    SIZE_T signatureSize;
    const char *token;
    const char *signature;
} XUserGetTokenAndSignatureData;

struct identity
{
    UINT64 xuid;
    char gamertag[64];
    char modern[64];
    char modern_suffix[16];
    char unique_modern[80];
    char privileges[1024];   /* the XSTS token's prv claim: numbers separated by spaces */
    UINT32 age;
};

struct user
{
    LONG refs;
    struct identity id;
    XUserLocalId local;
};

static WCHAR *store_path(void)
{
    static WCHAR path[MAX_PATH];
    WCHAR base[MAX_PATH];
    DWORD n;

    n = GetEnvironmentVariableW( L"WINE_UWP_XBOXAUTH_STORE", path, MAX_PATH );
    if (n && n < MAX_PATH) return path;
    n = GetEnvironmentVariableW( L"LOCALAPPDATA", base, MAX_PATH );
    if (!n || n >= MAX_PATH) wcscpy( base, L"C:\\users\\Public" );
    swprintf( path, MAX_PATH, L"%s\\wineuwp\\xboxauth.bin", base );
    return path;
}

/* the decrypted store, NUL-terminated */
static char *store_read(void)
{
    DATA_BLOB in, out = {0};
    DWORD size, got = 0;
    char *blob, *text = NULL;
    HANDLE file;

    file = CreateFileW( store_path(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize( file, NULL );
    if (size == INVALID_FILE_SIZE || !size || size >= (1u << 20) || !(blob = malloc( size )))
    {
        CloseHandle( file );
        return NULL;
    }
    if (!ReadFile( file, blob, size, &got, NULL )) got = 0;
    CloseHandle( file );
    in.cbData = got;
    in.pbData = (BYTE *)blob;
    if (got && CryptUnprotectData( &in, NULL, NULL, NULL, NULL, 0, &out ))
    {
        if ((text = malloc( out.cbData + 1 )))
        {
            memcpy( text, out.pbData, out.cbData );
            text[out.cbData] = 0;
        }
        SecureZeroMemory( out.pbData, out.cbData );
        LocalFree( out.pbData );
    }
    free( blob );
    return text;
}

static void store_value( const char *text, const char *key, char *value, size_t size )
{
    size_t key_len = strlen( key );
    const char *line = text, *end;

    *value = 0;
    while (line && *line)
    {
        end = strchr( line, '\n' );
        if (!strncmp( line, key, key_len ) && line[key_len] == '=')
        {
            const char *v = line + key_len + 1;
            size_t len = end ? end - v : strlen( v );
            if (len >= size) len = size - 1;
            memcpy( value, v, len );
            value[len] = 0;
        }
        line = end ? end + 1 : NULL;
    }
}

static BOOL load_identity( struct identity *id )
{
    char *text = store_read(), value[64];

    memset( id, 0, sizeof(*id) );
    id->age = XUserAgeGroup_Adult;
    if (!text) return FALSE;
    store_value( text, "gamertag", id->gamertag, sizeof(id->gamertag) );
    if (!*id->gamertag) store_value( text, "gtg", id->gamertag, sizeof(id->gamertag) );
    store_value( text, "mgt", id->modern, sizeof(id->modern) );
    if (!*id->modern) strcpy( id->modern, id->gamertag );
    store_value( text, "mgs", id->modern_suffix, sizeof(id->modern_suffix) );
    store_value( text, "umg", id->unique_modern, sizeof(id->unique_modern) );
    if (!*id->unique_modern) strcpy( id->unique_modern, id->gamertag );
    store_value( text, "prv", id->privileges, sizeof(id->privileges) );
    store_value( text, "xid", value, sizeof(value) );
    if (*value) id->xuid = _strtoui64( value, NULL, 10 );
    store_value( text, "agg", value, sizeof(value) );
    if (!strcmp( value, "Adult" ) || !strcmp( value, "3" )) id->age = XUserAgeGroup_Adult;
    else if (!strcmp( value, "Teen" ) || !strcmp( value, "2" )) id->age = XUserAgeGroup_Teen;
    else if (!strcmp( value, "Child" ) || !strcmp( value, "1" )) id->age = XUserAgeGroup_Child;
    SecureZeroMemory( text, strlen( text ) );
    free( text );

    if (!*id->gamertag) return FALSE;
    if (!id->xuid)
    {
        /* older stores kept only the gamertag; a stable local xuid still lets the title add the user */
        id->xuid = 0x1000000000000001ull;
        WARN( "gamertag %s but no xuid; using a local placeholder\n", debugstr_a(id->gamertag) );
    }
    return TRUE;
}

static struct user *default_user;
static CRITICAL_SECTION user_cs;
static CRITICAL_SECTION_DEBUG user_cs_debug =
{
    0, 0, &user_cs,
    { &user_cs_debug.ProcessLocksList, &user_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": user_cs") }
};
static CRITICAL_SECTION user_cs = { &user_cs_debug, -1, 0, 0, 0, 0 };

static struct user *user_addref( struct user *u )
{
    if (u) InterlockedIncrement( &u->refs );
    return u;
}

static void user_release( struct user *u )
{
    if (u && !InterlockedDecrement( &u->refs )) free( u );
}

static struct user *get_default_user(void)
{
    struct identity id;
    struct user *u = NULL;

    EnterCriticalSection( &user_cs );
    if (!default_user && load_identity( &id ) && (default_user = calloc( 1, sizeof(*default_user) )))
    {
        default_user->refs = 1;
        default_user->id = id;
        default_user->local.value = 1;
        TRACE( "default user %s, xuid %I64u\n", debugstr_a(id.gamertag), id.xuid );
    }
    u = user_addref( default_user );
    LeaveCriticalSection( &user_cs );
    return u;
}

/* --- the title's identity and the xboxauth bridge --- */

typedef HRESULT (WINAPI *get_token_fn)( const char *method, const char *url, int force, const void *body,
                                        UINT32 body_size, char *token, UINT32 token_cap, UINT32 *token_len,
                                        char *signature, UINT32 sig_cap, UINT32 *sig_len );
typedef HRESULT (WINAPI *gamer_picture_fn)( int size, unsigned char **png, UINT32 *len );
typedef HRESULT (WINAPI *set_title_context_fn)( const char *msa_app_id, UINT32 title_id );

static BOOL xml_tag_value( const char *xml, const char *tag, char *value, size_t size )
{
    char open[64], close[64];
    const char *a, *b;
    size_t len;

    snprintf( open, sizeof(open), "<%s>", tag );
    snprintf( close, sizeof(close), "</%s>", tag );
    if (!(a = strstr( xml, open ))) return FALSE;
    a += strlen( open );
    if (!(b = strstr( a, close ))) return FALSE;
    while (a < b && (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n')) a++;
    while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) b--;
    if (a == b || (len = b - a) >= size) return FALSE;
    memcpy( value, a, len );
    value[len] = 0;
    return TRUE;
}

static BOOL load_game_config( const WCHAR *dir, void *context )
{
    struct title_identity *id = context;
    WCHAR path[MAX_PATH * 4];
    char *xml, value[64];
    DWORD size, got = 0;
    HANDLE file;
    BOOL found;

    swprintf( path, ARRAY_SIZE(path), L"%s\\MicrosoftGame.Config", dir );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize( file, NULL );
    if (size == INVALID_FILE_SIZE || size > (4u << 20) || !(xml = malloc( size + 1 )))
    {
        CloseHandle( file );
        return FALSE;
    }
    found = ReadFile( file, xml, size, &got, NULL ) && got == size;
    CloseHandle( file );
    xml[found ? got : 0] = 0;

    if (xml_tag_value( xml, "TitleId", value, sizeof(value) ) && (id->title_id = strtoul( value, NULL, 16 )))
        id->have_title = TRUE;
    if (xml_tag_value( xml, "MSAAppId", id->msa_app_id, sizeof(id->msa_app_id) )) id->have_msa = TRUE;
    free( xml );
    if (!id->have_title && !id->have_msa) return FALSE;
    TRACE( "MicrosoftGame.Config in %s: title %08X, MSA app %s\n", debugstr_w(dir), id->title_id,
           id->have_msa ? id->msa_app_id : "(none)" );
    /* keep looking only while either is missing */
    return id->have_title && id->have_msa;
}

/* WINE_UWP_XBOX_TITLE_ID and WINE_UWP_MSA_APP_ID, else MicrosoftGame.Config in the package */
void load_title_identity( struct title_identity *id )
{
    struct title_identity config;
    char env[64];
    DWORD n;

    memset( id, 0, sizeof(*id) );
    n = GetEnvironmentVariableA( "WINE_UWP_XBOX_TITLE_ID", env, sizeof(env) );
    if (n && n < sizeof(env))
    {
        id->title_id = strtoul( env, NULL, 0 );
        id->have_title = TRUE;
    }
    n = GetEnvironmentVariableA( "WINE_UWP_MSA_APP_ID", env, sizeof(env) );
    if (n && n < sizeof(env))
    {
        memcpy( id->msa_app_id, env, n + 1 );
        id->have_msa = TRUE;
    }
    if (id->have_title && id->have_msa) return;

    memset( &config, 0, sizeof(config) );
    for_stage_dirs( load_game_config, &config );
    if (!id->have_title && config.have_title)
    {
        id->title_id = config.title_id;
        id->have_title = TRUE;
    }
    if (!id->have_msa && config.have_msa)
    {
        memcpy( id->msa_app_id, config.msa_app_id, sizeof(id->msa_app_id) );
        id->have_msa = TRUE;
    }
}

static get_token_fn get_token;
static set_title_context_fn set_title_context;
static gamer_picture_fn gamer_picture;
static INIT_ONCE xboxauth_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_xboxauth( INIT_ONCE *once, void *param, void **context )
{
    HMODULE module = LoadLibraryW( L"winexboxauth.dll" );

    if (!module)
    {
        ERR( "winexboxauth.dll not loaded, error %lu\n", GetLastError() );
        return TRUE;
    }
    if (!(get_token = (get_token_fn)GetProcAddress( module, "XboxAuthGetTokenAndSignature" )))
        ERR( "no XboxAuthGetTokenAndSignature\n" );
    if (!(set_title_context = (set_title_context_fn)GetProcAddress( module, "XboxAuthSetTitleContext" )))
        ERR( "no XboxAuthSetTitleContext\n" );
    if (!(gamer_picture = (gamer_picture_fn)GetProcAddress( module, "XboxAuthGamerPicture" )))
        ERR( "no XboxAuthGamerPicture\n" );
    return TRUE;
}

static void init_xboxauth(void)
{
    InitOnceExecuteOnce( &xboxauth_once, load_xboxauth, NULL, NULL );
}

/* The first token request does a silent MSA refresh and the title's SISU/XSTS: many seconds. */
static DWORD WINAPI warm_title_token( void *param )
{
    char token[8192], signature[4096];
    UINT32 token_len = 0, sig_len = 0;
    HRESULT hr;

    if (!get_token) return 0;
    hr = get_token( "GET", "https://xboxlive.com/", 0, NULL, 0, token, sizeof(token), &token_len,
                    signature, sizeof(signature), &sig_len );
    TRACE( "title token warmed, hr %#lx, %u bytes\n", hr, token_len );
    return 0;
}

static BOOL CALLBACK set_title( INIT_ONCE *once, void *param, void **context )
{
    struct title_identity id;
    HRESULT hr;

    load_title_identity( &id );
    if (!id.have_title || !id.have_msa)
    {
        WARN( "no title id or MSA app id (environment or MicrosoftGame.Config)\n" );
        return TRUE;
    }
    init_xboxauth();
    if (!set_title_context) return TRUE;
    hr = set_title_context( id.msa_app_id, id.title_id );
    TRACE( "title context %s, %08X -> %#lx\n", id.msa_app_id, id.title_id, hr );
    if (SUCCEEDED(hr)) QueueUserWorkItem( warm_title_token, NULL, WT_EXECUTELONGFUNCTION );
    return TRUE;
}

/* Bind the title's TitleId and MSAAppId, and get its token in the background, so the title's first request (presence,
 * on entering the menu) is a cache hit. */
void warm_xboxauth_title_context(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce( &once, set_title, NULL, NULL );
}

/* --- XUserAddAsync --- */

/* an async call's identity is compared by address */
static const char add_identity[] = "XUserAddAsync";
static const char token_identity[] = "XUserGetTokenAndSignatureAsync";

struct add_context
{
    struct user *user;
};

static HRESULT WINAPI add_user_provider( UINT32 op, const XAsyncProviderData *data )
{
    struct add_context *ctx = data->context;

    switch (op)
    {
    case XAsyncOp_DoWork:
        if (!(ctx->user = get_default_user()))
        {
            async_complete( data->async, HRESULT_FROM_WIN32( ERROR_NO_SUCH_USER ), 0 );
            return HRESULT_FROM_WIN32( ERROR_NO_SUCH_USER );
        }
        async_complete( data->async, S_OK, sizeof(struct user *) );
        return S_OK;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < sizeof(struct user *)) return E_INVALIDARG;
        *(struct user **)data->buffer = user_addref( ctx->user );
        return S_OK;
    case XAsyncOp_Cleanup:
        user_release( ctx->user );
        free( ctx );
        return S_OK;
    }
    return S_OK;
}

/* --- XUserGetTokenAndSignatureAsync --- */

struct token_context
{
    char *method;
    char *url;
    BOOL force;
    void *body;
    UINT32 body_size;
    char *token;
    char *signature;
    XAsyncBlock *async;
};

static SIZE_T token_result_size( const struct token_context *ctx )
{
    SIZE_T n = sizeof(XUserGetTokenAndSignatureData) + strlen( ctx->token ) + 1;
    if (*ctx->signature) n += strlen( ctx->signature ) + 1;
    return n;
}

static void token_do_work( struct token_context *ctx )
{
    char token[8192], signature[4096];
    UINT32 token_len = 0, sig_len = 0;
    HRESULT hr;

    warm_xboxauth_title_context();
    init_xboxauth();
    if (!get_token)
    {
        async_complete( ctx->async, E_NOTIMPL, 0 );
        return;
    }
    hr = get_token( ctx->method, ctx->url, ctx->force, ctx->body, ctx->body_size, token, sizeof(token), &token_len,
                    signature, sizeof(signature), &sig_len );
    if (FAILED(hr))
    {
        WARN( "token for %s failed, hr %#lx\n", debugstr_a(ctx->url), hr );
        async_complete( ctx->async, hr, 0 );
        return;
    }
    /* the lengths count the terminator */
    if (token_len > sizeof(token)) token_len = sizeof(token);
    if (sig_len > sizeof(signature)) sig_len = sizeof(signature);
    ctx->token = malloc( token_len ? token_len : 1 );
    ctx->signature = malloc( sig_len ? sig_len : 1 );
    memcpy( ctx->token, token, token_len ? token_len - 1 : 0 );
    ctx->token[token_len ? token_len - 1 : 0] = 0;
    memcpy( ctx->signature, signature, sig_len ? sig_len - 1 : 0 );
    ctx->signature[sig_len ? sig_len - 1 : 0] = 0;
    TRACE( "token %Iu, signature %Iu bytes for %s\n", strlen( ctx->token ), strlen( ctx->signature ),
           debugstr_a(ctx->url) );
    async_complete( ctx->async, S_OK, token_result_size( ctx ) );
}

static DWORD WINAPI token_work( void *param )
{
    token_do_work( param );
    return 0;
}

static HRESULT WINAPI token_provider( UINT32 op, const XAsyncProviderData *data )
{
    struct token_context *ctx = data->context;
    XUserGetTokenAndSignatureData *out;
    char *cursor;

    switch (op)
    {
    case XAsyncOp_DoWork:
        /* Not on the title's Live/UI thread: the silent MSA refresh and the title's SISU/XSTS can take tens of
         * seconds, and race its connection timeout into the offline dialog. */
        ctx->async = data->async;
        if (!QueueUserWorkItem( token_work, ctx, WT_EXECUTELONGFUNCTION ))
        {
            token_do_work( ctx );
            return S_OK;
        }
        return E_PENDING;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < token_result_size( ctx )) return E_INVALIDARG;
        out = data->buffer;
        cursor = (char *)(out + 1);
        out->tokenSize = strlen( ctx->token ) + 1;
        memcpy( cursor, ctx->token, out->tokenSize );
        out->token = cursor;
        cursor += out->tokenSize;
        if (*ctx->signature)
        {
            out->signatureSize = strlen( ctx->signature ) + 1;
            memcpy( cursor, ctx->signature, out->signatureSize );
            out->signature = cursor;
        }
        else
        {
            out->signatureSize = 0;
            out->signature = NULL;
        }
        return S_OK;
    case XAsyncOp_Cleanup:
        free( ctx->method );
        free( ctx->url );
        free( ctx->body );
        if (ctx->token) SecureZeroMemory( ctx->token, strlen( ctx->token ) );
        free( ctx->token );
        free( ctx->signature );
        free( ctx );
        return S_OK;
    }
    return S_OK;
}

/* --- IXUserImpl --- */

static HRESULT WINAPI user_QueryInterface( gdk_object *iface, REFIID iid, void **out );
static ULONG WINAPI user_AddRef( gdk_object *iface ) { return 2; }
static ULONG WINAPI user_Release( gdk_object *iface ) { return 1; }

static HRESULT WINAPI user_DuplicateHandle( gdk_object *iface, struct user *handle, struct user **duplicated )
{
    if (!duplicated) return E_POINTER;
    *duplicated = NULL;
    if (!handle) return E_INVALIDARG;
    *duplicated = user_addref( handle );
    return S_OK;
}

static void WINAPI user_CloseHandle( gdk_object *iface, struct user *handle )
{
    user_release( handle );
}

static INT32 WINAPI user_Compare( gdk_object *iface, struct user *a, struct user *b )
{
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return (a->id.xuid > b->id.xuid) - (a->id.xuid < b->id.xuid);
}

static HRESULT WINAPI user_GetMaxUsers( gdk_object *iface, UINT32 *count )
{
    if (!count) return E_POINTER;
    *count = 1;
    return S_OK;
}

static HRESULT WINAPI user_AddAsync( gdk_object *iface, UINT32 options, XAsyncBlock *block )
{
    struct add_context *ctx;

    TRACE( "options %#x\n", options );
    if (!(ctx = calloc( 1, sizeof(*ctx) ))) return E_OUTOFMEMORY;
    return async_begin( block, ctx, add_identity, add_identity, add_user_provider );
}

static HRESULT WINAPI user_AddResult( gdk_object *iface, XAsyncBlock *block, struct user **user )
{
    if (!user) return E_POINTER;
    *user = NULL;
    return async_get_result( block, add_identity, sizeof(*user), user, NULL );
}

static HRESULT WINAPI user_GetLocalId( gdk_object *iface, struct user *user, XUserLocalId *id )
{
    if (!user || !id) return E_POINTER;
    *id = user->local;
    return S_OK;
}

static HRESULT WINAPI user_FindUserByLocalId( gdk_object *iface, XUserLocalId id, struct user **handle )
{
    struct user *u;

    if (!handle) return E_POINTER;
    *handle = NULL;
    if (!(u = get_default_user())) return HRESULT_FROM_WIN32( ERROR_NO_SUCH_USER );
    if (u->local.value != id.value)
    {
        user_release( u );
        return HRESULT_FROM_WIN32( ERROR_NO_SUCH_USER );
    }
    *handle = u;
    return S_OK;
}

static HRESULT WINAPI user_GetId( gdk_object *iface, struct user *user, UINT64 *id )
{
    if (!user || !id) return E_POINTER;
    *id = user->id.xuid;
    return S_OK;
}

static HRESULT WINAPI user_FindUserById( gdk_object *iface, UINT64 id, struct user **handle )
{
    struct user *u;

    if (!handle) return E_POINTER;
    *handle = NULL;
    if (!(u = get_default_user())) return HRESULT_FROM_WIN32( ERROR_NO_SUCH_USER );
    if (u->id.xuid != id)
    {
        user_release( u );
        return HRESULT_FROM_WIN32( ERROR_NO_SUCH_USER );
    }
    *handle = u;
    return S_OK;
}

static HRESULT WINAPI user_GetIsGuest( gdk_object *iface, struct user *user, BOOLEAN *guest )
{
    if (!user || !guest) return E_POINTER;
    *guest = FALSE;
    return S_OK;
}

static HRESULT WINAPI user_GetState( gdk_object *iface, struct user *user, UINT32 *state )
{
    if (!user || !state) return E_POINTER;
    *state = XUserState_SignedIn;
    return S_OK;
}

static HRESULT WINAPI user_notimpl( gdk_object *iface )
{
    return E_NOTIMPL;
}

/* --- XUserGetGamerPictureAsync --- */

static const char picture_identity[] = "XUserGetGamerPictureAsync";

struct picture_context
{
    int size;
    unsigned char *png;
    UINT32 len;
    XAsyncBlock *async;
};

static DWORD WINAPI picture_work( void *param )
{
    struct picture_context *ctx = param;
    HRESULT hr = E_NOTIMPL;

    init_xboxauth();
    if (gamer_picture) hr = gamer_picture( ctx->size, &ctx->png, &ctx->len );
    TRACE( "gamer picture %dpx: hr %#lx, %u bytes\n", ctx->size, hr, ctx->len );
    async_complete( ctx->async, hr, SUCCEEDED(hr) ? ctx->len : 0 );
    return 0;
}

static HRESULT WINAPI picture_provider( UINT32 op, const XAsyncProviderData *data )
{
    struct picture_context *ctx = data->context;

    switch (op)
    {
    case XAsyncOp_DoWork:
        /* two HTTP requests: not on the title's thread */
        ctx->async = data->async;
        if (!QueueUserWorkItem( picture_work, ctx, WT_EXECUTELONGFUNCTION ))
        {
            picture_work( ctx );
            return S_OK;
        }
        return E_PENDING;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < ctx->len) return E_INVALIDARG;
        memcpy( data->buffer, ctx->png, ctx->len );
        return S_OK;
    case XAsyncOp_Cleanup:
        CoTaskMemFree( ctx->png );
        free( ctx );
        return S_OK;
    }
    return S_OK;
}

static HRESULT WINAPI user_GetGamerPictureAsync( gdk_object *iface, struct user *user, UINT32 size, XAsyncBlock *block )
{
    /* XUserGamerPictureSize: Small, Medium, Large, ExtraLarge */
    static const int pixels[] = { 64, 208, 424, 1080 };
    struct picture_context *ctx;
    HRESULT hr;

    if (!user || !block) return E_INVALIDARG;
    if (size >= ARRAY_SIZE(pixels)) return E_INVALIDARG;
    if (!(ctx = calloc( 1, sizeof(*ctx) ))) return E_OUTOFMEMORY;
    ctx->size = pixels[size];
    if (FAILED(hr = async_begin( block, ctx, picture_identity, picture_identity, picture_provider ))) free( ctx );
    return hr;
}

static HRESULT WINAPI user_GetGamerPictureResultSize( gdk_object *iface, XAsyncBlock *block, SIZE_T *size )
{
    return async_get_result_size( block, size );
}

static HRESULT WINAPI user_GetGamerPictureResult( gdk_object *iface, XAsyncBlock *block, SIZE_T size, void *buffer,
                                                  SIZE_T *used )
{
    return async_get_result( block, picture_identity, size, buffer, used );
}

static HRESULT WINAPI user_GetAgeGroup( gdk_object *iface, struct user *user, UINT32 *age )
{
    if (!user || !age) return E_POINTER;
    *age = user->id.age;
    return S_OK;
}

static HRESULT WINAPI user_CheckPrivilege( gdk_object *iface, struct user *user, UINT32 options, UINT32 privilege,
                                           BOOLEAN *has_privilege, UINT32 *reason )
{
    char needle[16], *hay, *p;
    BOOL has = TRUE;

    if (!user || !has_privilege) return E_POINTER;
    if (*user->id.privileges && (hay = malloc( strlen( user->id.privileges ) + 3 )))
    {
        sprintf( hay, " %s ", user->id.privileges );
        for (p = hay; *p; p++) if (*p == ',') *p = ' ';
        sprintf( needle, " %u ", privilege );
        has = strstr( hay, needle ) != NULL;
        free( hay );
        /* This adult account's XSTS prv lists Multiplayer (254) but not Sessions (189) or Broadcast (190). Halo
         * checks Sessions while connecting to Live, and a refusal makes it offline although presence and RTA work. */
        if (!has && user->id.age == XUserAgeGroup_Adult && (privilege == 189 || privilege == 190)) has = TRUE;
    }
    *has_privilege = has;
    if (reason) *reason = has ? XUserPrivilegeDenyReason_None : XUserPrivilegeDenyReason_Restricted;
    TRACE( "privilege %u -> %u\n", privilege, has );
    return S_OK;
}

static HRESULT WINAPI user_ResolvePrivilegeWithUiAsync( gdk_object *iface, struct user *user, UINT32 options,
                                                        UINT32 privilege, XAsyncBlock *block )
{
    return async_begin_empty( block, "XUserResolvePrivilegeWithUiAsync" );
}

static HRESULT WINAPI user_status_result( gdk_object *iface, XAsyncBlock *block )
{
    return async_get_status( block, TRUE );
}

static HRESULT WINAPI user_GetTokenAndSignatureAsync( gdk_object *iface, struct user *user, UINT32 options,
                                                      const char *method, const char *url, SIZE_T header_count,
                                                      const void *headers, SIZE_T body_size, const void *body,
                                                      XAsyncBlock *block )
{
    struct token_context *ctx;

    if (!(ctx = calloc( 1, sizeof(*ctx) ))) return E_OUTOFMEMORY;
    ctx->method = strdup( method ? method : "GET" );
    ctx->url = strdup( url ? url : "" );
    ctx->force = !!(options & XUserGetTokenAndSignatureOptions_ForceRefresh);
    if (body && body_size && (ctx->body = malloc( body_size )))
    {
        memcpy( ctx->body, body, body_size );
        ctx->body_size = body_size;
    }
    TRACE( "%s %s%s\n", debugstr_a(ctx->method), debugstr_a(ctx->url), ctx->force ? " ForceRefresh" : "" );
    return async_begin( block, ctx, token_identity, token_identity, token_provider );
}

static HRESULT WINAPI user_GetTokenAndSignatureResultSize( gdk_object *iface, XAsyncBlock *block, SIZE_T *size )
{
    return async_get_result_size( block, size );
}

static HRESULT WINAPI user_GetTokenAndSignatureResult( gdk_object *iface, XAsyncBlock *block, SIZE_T size,
                                                       void *buffer, void **ptr, SIZE_T *used )
{
    HRESULT hr = async_get_result( block, token_identity, size, buffer, used );
    if (SUCCEEDED(hr) && ptr) *ptr = buffer;
    return hr;
}

static HRESULT WINAPI user_ResolveIssueWithUiAsync( gdk_object *iface, struct user *user, const void *url,
                                                    XAsyncBlock *block )
{
    return async_begin_empty( block, "XUserResolveIssueWithUiAsync" );
}

static HRESULT WINAPI user_RegisterForChangeEvent( gdk_object *iface, XTaskQueueHandle queue, void *context,
                                                   void *callback, XTaskQueueRegistrationToken *token )
{
    if (token) token->token = 1;
    return S_OK;
}

static BOOLEAN WINAPI user_UnregisterForChangeEvent( gdk_object *iface, XTaskQueueRegistrationToken token,
                                                     BOOLEAN wait )
{
    return TRUE;
}

static HRESULT WINAPI user_GetSignOutDeferral( gdk_object *iface, void **deferral )
{
    if (!deferral) return E_POINTER;
    *deferral = (void *)1;
    return S_OK;
}

static void WINAPI user_CloseSignOutDeferralHandle( gdk_object *iface, void *deferral )
{
}

static HRESULT WINAPI user_AddByIdWithUiAsync( gdk_object *iface, UINT64 id, XAsyncBlock *block )
{
    return user_AddAsync( iface, XUserAddOptions_AddDefaultUserAllowingUI, block );
}

static BOOLEAN WINAPI user_IsStoreUser( gdk_object *iface, struct user *user )
{
    return TRUE;
}

/* IXUserImpl4 */
static const void *const user_vtbl[] =
{
    user_QueryInterface,
    user_AddRef,
    user_Release,
    user_DuplicateHandle,
    user_CloseHandle,
    user_Compare,
    user_GetMaxUsers,
    user_AddAsync,
    user_AddResult,
    user_GetLocalId,
    user_FindUserByLocalId,
    user_GetId,
    user_FindUserById,
    user_GetIsGuest,
    user_GetState,
    user_notimpl,                           /* padding */
    user_GetGamerPictureAsync,
    user_GetGamerPictureResultSize,
    user_GetGamerPictureResult,
    user_GetAgeGroup,
    user_CheckPrivilege,
    user_ResolvePrivilegeWithUiAsync,
    user_status_result,                     /* ResolvePrivilegeWithUiResult */
    user_GetTokenAndSignatureAsync,
    user_GetTokenAndSignatureResultSize,
    user_GetTokenAndSignatureResult,
    user_notimpl,                           /* GetTokenAndSignatureUtf16Async */
    user_notimpl,                           /* GetTokenAndSignatureUtf16ResultSize */
    user_notimpl,                           /* GetTokenAndSignatureUtf16Result */
    user_ResolveIssueWithUiAsync,
    user_status_result,                     /* ResolveIssueWithUiResult */
    user_ResolveIssueWithUiAsync,           /* ResolveIssueWithUiUtf16Async */
    user_status_result,                     /* ResolveIssueWithUiUtf16Result */
    user_RegisterForChangeEvent,
    user_UnregisterForChangeEvent,
    user_GetSignOutDeferral,
    user_CloseSignOutDeferralHandle,
    user_AddByIdWithUiAsync,
    user_AddResult,                         /* AddByIdWithUiResult */
    user_notimpl,                           /* GetMsaTokenSilentlyAsync */
    user_notimpl,                           /* GetMsaTokenSilentlyResult */
    user_notimpl,                           /* GetMsaTokenSilentlyResultSize */
    user_IsStoreUser,
};

static gdk_object user_service = { user_vtbl };

/* IXUserGamertagImpl, answered by the same service */

static HRESULT WINAPI gamertag_QueryInterface( gdk_object *iface, REFIID iid, void **out )
{
    return user_QueryInterface( &user_service, iid, out );
}

static HRESULT WINAPI gamertag_GetGamertag( gdk_object *iface, struct user *user, UINT32 component, SIZE_T size,
                                            char *buffer, SIZE_T *used )
{
    const char *s;
    size_t len;

    if (!user || !buffer) return E_POINTER;
    switch (component)
    {
    case XUserGamertagComponent_Modern: s = user->id.modern; break;
    case XUserGamertagComponent_ModernSuffix: s = user->id.modern_suffix; break;
    case XUserGamertagComponent_UniqueModern: s = user->id.unique_modern; break;
    default: s = user->id.gamertag; break;
    }
    len = strlen( s );
    if (size <= len) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    memcpy( buffer, s, len + 1 );
    if (used) *used = len + 1;
    return S_OK;
}

static const void *const gamertag_vtbl[] =
{
    gamertag_QueryInterface,
    user_AddRef,
    user_Release,
    gamertag_GetGamertag,
};

static gdk_object gamertag_service = { gamertag_vtbl };

static const GUID IID_IXUserImpl = {0x01acd177,0x91f9,0x4763,{0xa3,0x8e,0xcc,0xbb,0x55,0xce,0x32,0xe0}};
static const GUID IID_IXUserImpl2 = {0xeb9bf948,0x18dc,0x4d82,{0xbb,0xcc,0x40,0xe0,0xa8,0x09,0xc4,0xc0}};
static const GUID IID_IXUserImpl3 = {0x1bf2f8c5,0xd507,0x4e52,{0xbb,0x05,0xf7,0x26,0xd0,0xe7,0x11,0x61}};
static const GUID IID_IXUserImpl4 = {0x079415e3,0x6727,0x437f,{0x8e,0x9d,0x8f,0x8f,0x9b,0x24,0x39,0xf7}};
static const GUID IID_IXUserImpl5 = {0x26f3c674,0xa2fe,0x44fa,{0xb6,0xc4,0xa3,0x23,0xbc,0x94,0xff,0x53}};
static const GUID IID_IXUserImpl6 = {0x5131d685,0x4394,0x4ee6,{0x8c,0x18,0xbf,0xb5,0xd4,0xae,0xf1,0xff}};
static const GUID IID_IXUserGamertagImpl = {0xcef4fac0,0x7676,0x4a94,{0xa1,0x19,0x4c,0x43,0xf9,0xeb,0x5b,0x74}};

static HRESULT WINAPI user_QueryInterface( gdk_object *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IXUserImpl ) ||
        IsEqualGUID( iid, &IID_IXUserImpl2 ) || IsEqualGUID( iid, &IID_IXUserImpl3 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl4 ) || IsEqualGUID( iid, &IID_IXUserImpl5 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl6 ))
        *out = &user_service;
    else if (IsEqualGUID( iid, &IID_IXUserGamertagImpl ))
        *out = &gamertag_service;
    else
        return E_NOINTERFACE;
    return S_OK;
}

HRESULT query_xuser( REFIID iid, void **out )
{
    return user_QueryInterface( &user_service, iid, out );
}

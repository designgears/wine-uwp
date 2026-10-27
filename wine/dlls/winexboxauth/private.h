/*
 * Xbox sign-in, the Microsoft account WebAccount and the Store's account
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

#ifndef __WINE_WINEXBOXAUTH_PRIVATE_H
#define __WINE_WINEXBOXAUTH_PRIVATE_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winstring.h"
#include "objbase.h"
#include "activation.h"
#include "roapi.h"
#include "winhttp.h"
#include "bcrypt.h"
#include "wincrypt.h"

#include "wine/debug.h"
#include "wine/json.h"

#define MSA_CLIENT_ID "000000004C12AE6F"
#define MSA_SCOPE "service::user.auth.xboxlive.com::MBI_SSL"
/* Halo Infinite's MicrosoftGame.Config MSAAppId and TitleId (0x79C6D2A0 = 2043073184) */
#define HALO_MSA_APP_ID "000000004C25467F"
#define HALO_TITLE_ID 0x79C6D2A0u

/* errors the asyncs report; FACILITY_ITF so they are recognisably ours */
#define XA_E_NO_ACCOUNT     MAKE_HRESULT(1, FACILITY_ITF, 0x7801)  /* nothing stored, silent sign-in impossible */
#define XA_E_MSA            MAKE_HRESULT(1, FACILITY_ITF, 0x7802)  /* login.live.com refused */
#define XA_E_XBL            MAKE_HRESULT(1, FACILITY_ITF, 0x7803)  /* user.auth / xsts refused */
#define XA_E_NETWORK        MAKE_HRESULT(1, FACILITY_ITF, 0x7804)
#define XA_E_TIMEOUT        MAKE_HRESULT(1, FACILITY_ITF, 0x7805)  /* the device code expired */

static inline const char *S( const char *s ) { return s ? s : ""; }

/* ---- strings ---- */

/* a growing byte string, always NUL-terminated */
struct buf
{
    char *p;
    size_t n, cap;
};

void buf_addn( struct buf *b, const void *data, size_t len );
void buf_add( struct buf *b, const char *str );
void buf_addc( struct buf *b, char c );
void WINAPIV buf_printf( struct buf *b, const char *fmt, ... );
const char *buf_str( const struct buf *b );
void buf_free( struct buf *b );     /* wipes it first */
char *WINAPIV strf( const char *fmt, ... );
char *str_dup( const char *str );
char *str_dupn( const char *str, size_t len );
void str_set( char **dst, const char *src );    /* replaces, wiping the old value */
void str_wipe_free( char *str );
char *str_lower( char *str );
BOOL str_ends_with( const char *str, const char *suffix );
char *urlencode( const char *str );
char *json_escape( const char *str );
char *from_hstr( HSTRING str );
HRESULT to_hstr( const char *str, HSTRING *out );
HRESULT to_hstrw( const WCHAR *str, HSTRING *out );
WCHAR *widen( const char *str );
char *narrow( const WCHAR *str, size_t len );
ULONGLONG now_s( void );
ULONGLONG parse_iso( const char *str );

char *b64( const unsigned char *data, size_t len, BOOL url );
void unb64( const char *str, struct buf *out );
BOOL sha256( const void *data, size_t len, unsigned char out[32] );

/* a "Name: value\r\n" list */
void hdr_add( struct buf *headers, const char *fmt, ... );

/* ---- HTTP ---- */

DWORD http_request( const WCHAR *method, const char *url, const char *headers, const void *body, size_t body_len,
                    struct buf *response );
DWORD http_form( const char *url, const char *form, struct buf *response );

/* ---- the store: %LOCALAPPDATA%\wineuwp\xboxauth.bin, key=value lines, DPAPI-protected ---- */

struct kv
{
    unsigned count;
    char **keys, **vals;
};

const char *kv_get( const struct kv *kv, const char *key );
void kv_set( struct kv *kv, const char *key, const char *val );    /* "" removes */
void kv_free( struct kv *kv );
void store_read( struct kv *kv );
BOOL store_write( const struct kv *kv );
void store_lock( void );
void store_unlock( void );
/* key, value pairs, NULL-terminated; an empty value removes the key */
BOOL WINAPIV store_update( const char *key, ... );
char *store_get( const char *key );

/* ---- the device and the account (auth.c) ---- */

struct signing_policy
{
    int version;
    ULONGLONG max_body_bytes;
    BOOL es256;
};

struct url_parts
{
    char *scheme, *host, *path, *path_and_query;
};

void split_url( const char *url, struct url_parts *u );
void url_parts_free( struct url_parts *u );
char *sign_request( int version, ULONGLONG max_body_bytes, const char *method, const char *path_and_query,
                    const char *authorization, const void *body, size_t body_len, ULONGLONG filetime );
void sign_prehash_input( struct buf *m, const unsigned char version[4], const unsigned char filetime[8], const char *method,
                         const char *path_and_query, const char *authorization, const void *body, size_t body_len,
                         ULONGLONG max_body_bytes );
BOOL find_policy( const char *url, struct signing_policy *out );
char *relying_party( const char *url );
void signed_headers( struct buf *h, const char *url, const char *body, BOOL contract );
void add_signature( struct buf *h, const char *method, const char *url, const char *authorization, const void *body,
                    size_t body_len );

struct xsts
{
    char *key, *token;
    ULONGLONG expires;
};

struct account
{
    CRITICAL_SECTION cs;          /* guards everything below */
    CRITICAL_SECTION signin_cs;   /* one sign-in (refresh or device code) at a time */
    char *refresh_token, *msa_token, *msa_user_id;
    char *puid;                   /* the account's PUID, 16 hex digits: what a Microsoft account WebAccount.Id is */
    ULONGLONG msa_expires;
    char *user_token;
    ULONGLONG user_expires;
    char *title_token;            /* from SISU; empty when it was refused (then XSTS carries no title claims) */
    ULONGLONG title_expires;
    char *title_tid;              /* DisplayClaims.xti.tid of title_token (decimal) */
    struct xsts *xsts;
    unsigned xsts_count;
    /* xui claims of the http://xboxlive.com XSTS token */
    char *uhs, *xid, *gtg, *agg, *prv, *mgt, *mgs, *umg;
    BOOL signed_in;
};

extern struct account account;

struct device
{
    CRITICAL_SECTION cs;
    BCRYPT_KEY_HANDLE key;
    char *x, *y;                  /* public point, base64url */
    char *id;                     /* device id, a GUID */
    char *token;                  /* device token */
    ULONGLONG token_expires;
};

extern struct device device;

void account_init( void );
char *account_field( char **field );   /* a copy, under the lock */
void xsts_clear( void );
BOOL device_load( void );
char *proof_key_json( void );
HRESULT xbl_device_token( char **token, BOOL force, struct json **answer );
HRESULT xbl_user_authenticate( const char *msa_token, char **user_token, ULONGLONG *expires );
HRESULT xbl_xsts( const char *user_token, const char *rp, char **token, ULONGLONG *expires, struct json **answer,
                  BOOL with_title );
HRESULT xbl_sisu_authorize( const char *msa_token, char **user_token, ULONGLONG *user_expires, char **xsts,
                            ULONGLONG *xsts_expires, struct json **answer );
HRESULT msa_refresh( const char *scope, char **token_out );
HRESULT ensure_title_identity( const char *msa_app, UINT32 title_id );
HRESULT sign_in_silently( void );
HRESULT sign_in_with_device_code_client( const char *client_id, volatile LONG *cancelled, BOOL full_signin );
HRESULT sign_in_with_device_code( volatile LONG *cancelled );
void sign_out( void );
void open_browser( const char *url );
/* with_title: FALSE, TRUE (the title token when there is one), or XSTS_WINDOWS_DEVICE: no title token, and the
 * device is this prefix's Windows device (windevice.c) rather than the Xbox app's Android one */
#define XSTS_WINDOWS_DEVICE 2
HRESULT get_token_for_url( const char *url, BOOL force, char **header, BOOL with_title );
char *stored_msa_account( void );
char *refresh_store_key( const char *client_id );
HRESULT msa_token_for_scope( const char *scope, BOOL force, char **token, char **error, char **form_out, const char *extra );
void scoped_token_forget( const char *key );
char *scoped_token_form( const char *key );
HRESULT collections_owns( const char *product );
void title_context( char **msa_app, UINT32 *title_id );
void log_xbl_error( const char *what, DWORD status, const struct buf *resp );
HRESULT WINAPI XboxAuthHttpGet( const char *url, const char *extra, const char *contract, unsigned char **body,
                                UINT32 *len, DWORD *status );
HRESULT WINAPI XboxAuthSetTitleContext( const char *msa_app_id, UINT32 title_id );

/* ---- windevice.c ---- */

struct content_key
{
    unsigned char id[16];
    unsigned char key[64];
    size_t key_len;
};

HRESULT windev_ready( void );
HRESULT windev_device_ticket( char **ticket );
HRESULT windev_service_ticket( const char *scope, const char *policy, char **ticket );
HRESULT content_keys( const char *content_id, const char *market, struct content_key **keys, unsigned *count,
                      char **package_full_name );

/* ---- COM objects: up to five untyped vtables ---- */

#define MAX_IFACES 5

struct object
{
    const void **vtbl[MAX_IFACES];
    GUID iids[MAX_IFACES];
    unsigned count;
    LONG ref;
    BOOL is_static;             /* a factory: never freed */
    const WCHAR *name;          /* the runtime class name */
    const char *trace_name;     /* for refused QueryInterface calls */
    void (*destroy)( struct object * );
};

#define OBJ_AT(iface, n) ((struct object *)((const void ***)(iface) - (n)))

void *object_create( size_t size, const WCHAR *name, const char *trace_name, void (*destroy)( struct object * ) );
void object_add( struct object *obj, const void **vtbl, const GUID *iid );
void object_init_static( struct object *obj, const WCHAR *name, const char *trace_name );
BOOL base_iid( REFIID iid );
HRESULT obj_QueryInterface( struct object *obj, REFIID iid, void **out );
ULONG obj_AddRef( struct object *obj );
ULONG obj_Release( struct object *obj );

#define DECLARE_OBJ_METHODS(n) \
    HRESULT WINAPI obj##n##_QueryInterface( void *iface, REFIID iid, void **out ); \
    ULONG WINAPI obj##n##_AddRef( void *iface ); \
    ULONG WINAPI obj##n##_Release( void *iface ); \
    HRESULT WINAPI obj##n##_GetIids( void *iface, ULONG *count, IID **ids ); \
    HRESULT WINAPI obj##n##_GetRuntimeClassName( void *iface, HSTRING *name ); \
    HRESULT WINAPI obj##n##_GetTrustLevel( void *iface, TrustLevel *level );
DECLARE_OBJ_METHODS(0)
DECLARE_OBJ_METHODS(1)
DECLARE_OBJ_METHODS(2)
DECLARE_OBJ_METHODS(3)
DECLARE_OBJ_METHODS(4)

/* the six IInspectable slots of interface n */
#define OBJ_METHODS(n) (void *)obj##n##_QueryInterface, (void *)obj##n##_AddRef, (void *)obj##n##_Release, \
                       (void *)obj##n##_GetIids, (void *)obj##n##_GetRuntimeClassName, (void *)obj##n##_GetTrustLevel
/* the three IUnknown slots of interface n */
#define UNK_METHODS(n) (void *)obj##n##_QueryInterface, (void *)obj##n##_AddRef, (void *)obj##n##_Release

/* calls slot n of a foreign object */
#define SLOT(obj, n, type) ((type)(*(void ***)(obj))[n])

extern const GUID IID_IAgileObject_, IID_IAsyncInfo_, IID_IAsyncAction_, IID_IClosable_;
extern const GUID IID_IMap_String_String, IID_IIterable_KVP_String_String;

HRESULT WINAPI closable_Close( void *iface );

/* ---- IAsyncOperation<T> / IAsyncAction ---- */

enum result_kind { R_ACTION, R_IFACE, R_STRINGS1, R_STRINGS2, R_I4, R_TRANSFER, R_I8 /* i4 low, i4b high */ };

struct async
{
    struct object obj;
    enum result_kind kind;
    const char *op_name;
    CRITICAL_SECTION cs;
    INT32 status;               /* 0 Started, 1 Completed, 2 Canceled, 3 Error */
    HRESULT error;
    IUnknown *handler;
    IUnknown *iface;            /* R_IFACE: owned */
    char *s1, *s2;
    INT32 i4, i4b;
    volatile LONG cancelled;
    UINT32 id;
};

struct async *async_create( const GUID *iid, enum result_kind kind, const char *name );
void async_finish( struct async *op, HRESULT hr );
/* runs work( op, ctx ) on its own thread, then finishes op with its result; free_ctx( ctx ) afterwards */
struct async *async_start( const GUID *iid, enum result_kind kind, const char *name,
                           HRESULT (*work)( struct async *op, void *ctx ), void *ctx, void (*free_ctx)( void *ctx ) );
HRESULT completed_iface( const GUID *iid, const char *name, IUnknown *result, void **out );

/* ---- events ---- */

struct events
{
    CRITICAL_SECTION cs;
    struct { INT64 token; IUnknown *handler; } *items;
    unsigned count;
};

void events_init( struct events *e );
void events_free( struct events *e );
HRESULT events_add( struct events *e, IUnknown *handler, INT64 *token );
HRESULT events_remove( struct events *e, INT64 token );
void events_raise( struct events *e, void *sender, void *args );

/* ---- collections ---- */

struct vector_view
{
    struct object obj;
    GUID iterator_iid;
    IInspectable **items;
    UINT32 count;
};

struct vector_view *vector_view_create( const GUID *view, const GUID *iterable, const GUID *iterator, const WCHAR *name );
void vector_view_add( struct vector_view *view, IInspectable *item );   /* takes the reference */

/* a Windows.Foundation.Collections.StringMap (wintypes) with NULL-terminated key, value pairs */
IInspectable *WINAPIV make_string_map( const char *key, ... );
char *describe_map( IInspectable *map );
char *map_lookup( IInspectable *map, const WCHAR *key );

/* ---- classes ---- */

IActivationFactory *xboxauth_manager_factory( void );
IActivationFactory *xboxauth_init_args_factory( void );
IActivationFactory *web_auth_manager_factory( void );
IActivationFactory *store_configuration_factory( void );
IActivationFactory *game_save_provider_factory( void );
IActivationFactory *device_address_factory( void );

BYTE *buffer_bytes( IInspectable *buffer, UINT32 *length, UINT32 *capacity );
IInspectable *buffer_create( BYTE *bytes, UINT32 size );

DWORD xbl_call( const char *method, const char *url, const char *extra, const void *body, size_t len,
                struct buf *resp );

/* gamesave.c's files: names hex-encoded, writes atomic */
WCHAR *hex_name( const char *name );
char *unhex_name( const WCHAR *hex );
WCHAR *path_join( const WCHAR *a, const WCHAR *b );
void make_dirs( const WCHAR *path );
BOOL read_file( const WCHAR *path, BYTE **data, DWORD *size );
BOOL write_file( const WCHAR *path, const void *data, DWORD size );

/* cloudsave.c: connected storage synced with Xbox Live title storage */
char *cloud_base( const char *scid, const char *xuid );
void cloud_sync( const char *base, const WCHAR *dir );
void cloud_blob_written( const WCHAR *container_dir, const char *blob );
void cloud_container_changed( const char *base, const WCHAR *dir, const char *container );
void cloud_container_deleted( const char *base, const char *container );
HRESULT make_memory_stream( const void *bytes, size_t len, IInspectable **out );

#endif /* __WINE_WINEXBOXAUTH_PRIVATE_H */

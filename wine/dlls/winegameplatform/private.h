/*
 * Microsoft.GamePlatform.Services classes
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

#ifndef __WINE_WINEGAMEPLATFORM_PRIVATE_H
#define __WINE_WINEGAMEPLATFORM_PRIVATE_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winstring.h"
#include "objbase.h"
#include "activation.h"
#include "roapi.h"

#include "wine/debug.h"
#include "wine/appxinstall.h"
#include "wine/json.h"

/* what the package's own client answers when the Gaming Services service is not there */
#define GP_E_NO_SERVICE ((HRESULT)0x87E00196)

extern const GUID IID_IAgileObject_, IID_IWeakReferenceSource_, IID_IWeakReference_, IID_IAsyncInfo_;

/* ---- objects: an untyped vtable, and optionally a second one (an IIterable, IAsyncInfo) ---- */

struct object
{
    const void **vtbl;
    const void **vtbl2;
    LONG ref;
    GUID iid, iid2;
    const WCHAR *name;          /* the runtime class name */
    void (*destroy)( struct object * );
};

HRESULT WINAPI obj_QueryInterface( struct object *obj, REFIID iid, void **out );
ULONG WINAPI obj_AddRef( struct object *obj );
ULONG WINAPI obj_Release( struct object *obj );
HRESULT WINAPI obj_GetIids( struct object *obj, ULONG *count, IID **ids );
HRESULT WINAPI obj_GetRuntimeClassName( struct object *obj, HSTRING *name );
HRESULT WINAPI obj_GetTrustLevel( struct object *obj, TrustLevel *level );
HRESULT WINAPI obj2_QueryInterface( const void ***iface, REFIID iid, void **out );
ULONG WINAPI obj2_AddRef( const void ***iface );
ULONG WINAPI obj2_Release( const void ***iface );
HRESULT WINAPI obj2_GetIids( const void ***iface, ULONG *count, IID **ids );
HRESULT WINAPI obj2_GetRuntimeClassName( const void ***iface, HSTRING *name );
HRESULT WINAPI obj2_GetTrustLevel( const void ***iface, TrustLevel *level );
#define OBJ_METHODS (void *)obj_QueryInterface, (void *)obj_AddRef, (void *)obj_Release, (void *)obj_GetIids, \
                    (void *)obj_GetRuntimeClassName, (void *)obj_GetTrustLevel
#define OBJ2_METHODS (void *)obj2_QueryInterface, (void *)obj2_AddRef, (void *)obj2_Release, (void *)obj2_GetIids, \
                     (void *)obj2_GetRuntimeClassName, (void *)obj2_GetTrustLevel
#define OBJ2(iface) CONTAINING_RECORD( iface, struct object, vtbl2 )

/* zeroed, one reference; the struct starts with a struct object */
void *object_create( size_t size, const void **vtbl, const GUID *iid, const WCHAR *name, void (*destroy)( struct object * ) );
HRESULT weak_source( IUnknown *target, void **out );
BOOL base_iid( REFIID iid );
HRESULT hstr( const WCHAR *str, HSTRING *out );
void wide_copy( WCHAR *dst, size_t size, const char *src );
char *utf8_dup( const WCHAR *str );

/* a getter of a field */
#define GETTER(type, prefix, name, vtype, expr) \
    static HRESULT WINAPI prefix##_get_##name( type *This, vtype *v ) { if (!v) return E_POINTER; *v = (expr); return S_OK; }
#define HSTR_GETTER(type, prefix, name, expr) \
    static HRESULT WINAPI prefix##_get_##name( type *This, HSTRING *v ) { return hstr( (expr), v ); }

/* invokes a delegate: slot 3 */
static inline HRESULT invoke_delegate( IUnknown *handler, void *sender, void *args )
{
    return ((HRESULT (WINAPI *)( IUnknown *, void *, void * ))(*(void ***)handler)[3])( handler, sender, args );
}

/* ---- the class factories, and the objects handed out with the same traced slots ---- */

struct gp_slot { int slot; const char *name; int kind; /* 0 a method, 1 add_ event, 2 remove_ event */ };
struct gp_class { const WCHAR *name; GUID statics; unsigned count; const struct gp_slot *slots; };

struct factory
{
    const void **vtbl;
    LONG ref;
    int kind;
    LONGLONG next_token;
    BOOL heap;          /* an object handed out (a "...Instance" entry), freed on its last Release */
    WCHAR source[64];   /* for a Package: the source it was created from */
};

int kind_of( const WCHAR *name );
const char *class_name( int kind );
const void **factory_vtbl_of( int kind );
struct factory *factory_of( int kind );
ULONG WINAPI factory_AddRef( struct factory *f );
HRESULT generic_slot( struct factory *f, int n, void *a2, void *ret );
void log_guid( const char *what, REFIID iid );

/* ---- the Store's catalogue ---- */

struct product
{
    WCHAR id[64];
    GUID content_id;
    WCHAR family[256], store_id[64], wu_category[64];
    UINT64 size;
    INT32 type;
    BOOL uwp, encrypted, found;
};

void query_product( const WCHAR *id, struct product *d );
BOOL content_access_has( const char *needle, size_t len );

/* ---- IAsyncOperation<T> ---- */

struct async;
struct async *async_create( const GUID *iid, BOOL guid_result );
void async_finish( struct async *op, HRESULT hr, IUnknown *result );
void async_finish_guid( struct async *op, HRESULT hr, const GUID *result );
IInspectable *async_iface( struct async *op );
IInspectable *async_start( const GUID *iid, HRESULT (*work)( void *ctx, IUnknown **result ), void *ctx );

/* ---- collections ---- */

struct hstring_view;
struct hstring_view *hstring_view_create( void );
void hstring_view_add( struct hstring_view *view, const WCHAR *str );
UINT32 hstring_view_size( struct hstring_view *view );

struct obj_view;
struct obj_view *obj_view_create( const GUID *view, const GUID *iterable, const GUID *iterator, const WCHAR *name );
void obj_view_add( struct obj_view *view, IInspectable *item );   /* takes the reference */
IInspectable *obj_iterator_create( struct obj_view *view );

struct guid_view;
struct guid_view *guid_view_create( void );
void guid_view_add( struct guid_view *view, const GUID *guid );

/* ---- string lists: features, tags, languages ---- */

struct strlist { char **items; unsigned count; };
void strlist_add( struct strlist *list, const char *str );
void strlist_add_n( struct strlist *list, const char *str, size_t len );
void strlist_add_unique( struct strlist *list, const char *str );
BOOL strlist_has( const struct strlist *list, const char *str );
void strlist_split( struct strlist *list, const char *str, char sep );
void strlist_copy( struct strlist *dst, const struct strlist *src );
void strlist_free( struct strlist *list );
char *strlist_join( const struct strlist *list, char sep );
void strlist_sort( struct strlist *list );

/* ---- install.c ---- */

struct installed_game
{
    WCHAR instance[40], store_id[64], family[256], full_name[256], stage_dir[MAX_PATH], title[256], aumid[256];
    struct strlist features;
    UINT64 size;
    BOOL uwp, features_known;
};

void games_data_unix( WCHAR *buf, size_t size );
void unix_to_windows( WCHAR *buf, size_t size, const WCHAR *unix_path );
void data_game_folder( WCHAR *buf, size_t size );
BOOL find_installed( const WCHAR *key, struct installed_game *game );
void installed_game_clear( struct installed_game *game );
BOOL is_package_instance( const GUID *id );
BOOL package_game( struct factory *f, struct installed_game *game );
BOOL queued_game( struct factory *f, struct installed_game *game );
BOOL queued_store_id( const WCHAR *key, const GUID *id, WCHAR *store_id, size_t size );
HRESULT msixvc_url( const GUID *content_id, char **url );
void read_features( IInspectable *iterable, struct strlist *features );
IInspectable *request_size_create( UINT64 size, BOOL upgrade );

struct package_request
{
    struct object obj;
    WCHAR source[256], store_id[64], sku[64], avail[64], destination[MAX_PATH], destination_name[256], alias[256];
    INT32 source_type, type;
    UINT32 options;
    IInspectable *metadata;
    struct spec_vector *specs;
    struct strlist features;
    BOOL features_chosen;
};

/* ---- gdk.c ---- */

struct spec_vector;
struct spec_vector *spec_vector_create( void );
UINT64 gdk_request_size( const WCHAR *store_id, const struct strlist *features, UINT64 fallback );
BOOL gdk_installer_choice( const WCHAR *store_id, const struct strlist *features, WCHAR **tags, WCHAR **languages );
void request_features_from_specifiers( struct package_request *r, struct strlist *features, BOOL *chosen );

/* the special slots */
HRESULT WINAPI st_EnumerateContent( struct factory *f, const GUID *device, IInspectable **out );
HRESULT WINAPI store_EnumerateMigrationBlockedProducts( struct factory *f, IInspectable **out );
HRESULT WINAPI pu_QueryUpdate( struct factory *f, HSTRING source, INT32 type, boolean related, IInspectable **out );
HRESULT WINAPI pu_QueryUpdateAsync( struct factory *f, HSTRING source, INT32 type, boolean related, IInspectable **out );
HRESULT WINAPI pu_QueryUpdatesAsync( struct factory *f, IInspectable **out );
HRESULT WINAPI pu_QueryUpdatesAsyncFiltered( struct factory *f, INT32 filter, IInspectable **out );
HRESULT WINAPI rq_CreateForSource( struct factory *f, HSTRING source, INT32 type, IInspectable **out );
HRESULT WINAPI rq_CreateForSourceDevice( struct factory *f, HSTRING source, INT32 type, GUID target, IInspectable **out );
HRESULT WINAPI rq_CreateForStoreId( struct factory *f, HSTRING store, HSTRING sku, HSTRING avail, IInspectable **out );
HRESULT WINAPI pq_SubmitRequestAsync( struct factory *f, IInspectable *request, INT32 priority, HSTRING cv, IInspectable **out );
HRESULT WINAPI pq_PauseInstance( struct factory *f, HSTRING instance );
HRESULT WINAPI pq_ResumeInstance( struct factory *f, HSTRING instance );
HRESULT WINAPI pq_CancelInstance( struct factory *f, HSTRING instance, boolean nondestructive, boolean *out );
HRESULT WINAPI pq_PauseId( struct factory *f, GUID id );
HRESULT WINAPI pq_ResumeId( struct factory *f, GUID id );
HRESULT WINAPI pq_CancelId( struct factory *f, GUID id, boolean nondestructive, boolean *out );
HRESULT WINAPI pq_PauseAll( struct factory *f );
HRESULT WINAPI pq_ResumeAll( struct factory *f );
HRESULT WINAPI pq_CancelAll( struct factory *f, boolean nondestructive, boolean *out );
HRESULT WINAPI pq_Enumerate( struct factory *f, IInspectable **out );
HRESULT WINAPI pq_EnumerateId( struct factory *f, GUID id, IInspectable **out );
HRESULT WINAPI pq_QueryInstallationInfo( struct factory *f, HSTRING instance, IInspectable **out );
HRESULT WINAPI pq_QueryInstallationSummary( struct factory *f, GUID id, IInspectable **out );
HRESULT WINAPI pq_CreateViewInstance( struct factory *f, HSTRING instance, IInspectable **out );
HRESULT WINAPI pq_CreateViewId( struct factory *f, GUID id, IInspectable **out );
HRESULT WINAPI pq_add_InstallationChanged( struct factory *f, IUnknown *h, LONGLONG *t );
HRESULT WINAPI pq_remove_InstallationChanged( struct factory *f, LONGLONG t );
HRESULT WINAPI pq_add_QueueChanged( struct factory *f, IUnknown *h, LONGLONG *t );
HRESULT WINAPI pq_remove_QueueChanged( struct factory *f, LONGLONG t );
HRESULT WINAPI pq_add_ItemAdded( struct factory *f, IUnknown *h, LONGLONG *t );
HRESULT WINAPI pq_remove_ItemAdded( struct factory *f, LONGLONG t );
HRESULT WINAPI pq_add_ItemRemoved( struct factory *f, IUnknown *h, LONGLONG *t );
HRESULT WINAPI pq_remove_ItemRemoved( struct factory *f, LONGLONG t );
HRESULT WINAPI pi_QueryInstallPath( struct factory *f, HSTRING *out );
HRESULT WINAPI pi_QueryInstanceId( struct factory *f, HSTRING *out );
HRESULT WINAPI pi_QueryIdentity( struct factory *f, INT32 type, HSTRING *out );
HRESULT WINAPI pi_QuerySize( struct factory *f, UINT64 *out );
HRESULT WINAPI pi_QueryStorageCategory( struct factory *f, INT32 *out );
HRESULT WINAPI pi_QueryHeader( struct factory *f, IInspectable **out );
HRESULT WINAPI pi_QueryDetails( struct factory *f, IInspectable **out );
HRESULT WINAPI pi_QueryFeatures( struct factory *f, IInspectable **out );
HRESULT WINAPI pi_QueryRecipes( struct factory *f, IInspectable **out );
HRESULT WINAPI pi_IsFlatStreamed( struct factory *f, boolean *out );
HRESULT WINAPI pi_IsLaunchable( struct factory *f, boolean *out );
HRESULT WINAPI pi_GetRunningProcessId( struct factory *f, HSTRING exe, UINT32 *out );
HRESULT WINAPI pi_Activate( struct factory *f, HSTRING exe, HSTRING params, HSTRING caller, HSTRING cv, UINT32 timeout );
HRESULT WINAPI pi_ActivateAumid( struct factory *f, HSTRING exe, HSTRING params, HSTRING caller, HSTRING cv );
HRESULT WINAPI pi_add_InstallationChanged( struct factory *f, IUnknown *handler, LONGLONG *token );
HRESULT WINAPI pi_remove_InstallationChanged( struct factory *f, LONGLONG token );
HRESULT WINAPI pv_Create( struct factory *f, HSTRING source, INT32 type, IInspectable **out );
HRESULT WINAPI request_get_Specifiers( struct package_request *r, IInspectable **out );
HRESULT WINAPI request_QuerySize( struct package_request *r, IInspectable **out );

#endif /* __WINE_WINEGAMEPLATFORM_PRIVATE_H */

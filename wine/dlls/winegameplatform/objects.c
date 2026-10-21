/*
 * Microsoft.GamePlatform.Services: objects, collections and asynchronous operations
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

const GUID IID_IAgileObject_ = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
const GUID IID_IWeakReferenceSource_ = {0x00000038,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
const GUID IID_IWeakReference_ = {0x00000037,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
const GUID IID_IAsyncInfo_ = {0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

BOOL base_iid( REFIID iid )
{
    return IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject_ );
}

HRESULT hstr( const WCHAR *str, HSTRING *out )
{
    if (!out) return E_POINTER;
    if (!str) str = L"";
    return WindowsCreateString( str, wcslen( str ), out );
}

void wide_copy( WCHAR *dst, size_t size, const char *src )
{
    if (!size) return;
    if (!MultiByteToWideChar( CP_UTF8, 0, src ? src : "", -1, dst, size )) dst[0] = 0;
    dst[size - 1] = 0;
}

char *utf8_dup( const WCHAR *str )
{
    int len = WideCharToMultiByte( CP_UTF8, 0, str ? str : L"", -1, NULL, 0, NULL, NULL );
    char *out = malloc( len );
    if (out) WideCharToMultiByte( CP_UTF8, 0, str ? str : L"", -1, out, len, NULL, NULL );
    return out;
}

/* ---- the object base ---- */

void *object_create( size_t size, const void **vtbl, const GUID *iid, const WCHAR *name, void (*destroy)( struct object * ) )
{
    struct object *obj = calloc( 1, size );
    if (!obj) return NULL;
    obj->vtbl = vtbl;
    obj->ref = 1;
    if (iid) obj->iid = *iid;
    obj->name = name;
    obj->destroy = destroy;
    return obj;
}

HRESULT WINAPI obj_QueryInterface( struct object *obj, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    if (base_iid( iid ) || IsEqualGUID( iid, &obj->iid )) *out = &obj->vtbl;
    else if (obj->vtbl2 && IsEqualGUID( iid, &obj->iid2 )) *out = &obj->vtbl2;
    else if (IsEqualGUID( iid, &IID_IWeakReferenceSource_ )) return weak_source( (IUnknown *)&obj->vtbl, out );
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement( &obj->ref );
    return S_OK;
}

ULONG WINAPI obj_AddRef( struct object *obj )
{
    return InterlockedIncrement( &obj->ref );
}

ULONG WINAPI obj_Release( struct object *obj )
{
    ULONG ref = InterlockedDecrement( &obj->ref );
    if (!ref)
    {
        if (obj->destroy) obj->destroy( obj );
        free( obj );
    }
    return ref;
}

HRESULT WINAPI obj_GetIids( struct object *obj, ULONG *count, IID **ids )
{
    if (count) *count = 0;
    if (ids) *ids = NULL;
    return S_OK;
}

HRESULT WINAPI obj_GetRuntimeClassName( struct object *obj, HSTRING *name )
{
    return hstr( obj->name, name );
}

HRESULT WINAPI obj_GetTrustLevel( struct object *obj, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

HRESULT WINAPI obj2_QueryInterface( const void ***iface, REFIID iid, void **out ) { return obj_QueryInterface( OBJ2(iface), iid, out ); }
ULONG WINAPI obj2_AddRef( const void ***iface ) { return obj_AddRef( OBJ2(iface) ); }
ULONG WINAPI obj2_Release( const void ***iface ) { return obj_Release( OBJ2(iface) ); }
HRESULT WINAPI obj2_GetIids( const void ***iface, ULONG *count, IID **ids ) { return obj_GetIids( OBJ2(iface), count, ids ); }
HRESULT WINAPI obj2_GetRuntimeClassName( const void ***iface, HSTRING *name ) { return obj_GetRuntimeClassName( OBJ2(iface), name ); }
HRESULT WINAPI obj2_GetTrustLevel( const void ***iface, TrustLevel *level ) { return obj_GetTrustLevel( OBJ2(iface), level ); }

/* ---- IWeakReferenceSource / IWeakReference ----
 * C++/WinRT keeps weak references to objects it gets (the app's GameCatalog holds a weak_ref to every IPackage and
 * dereferences the QueryInterface without checking). The reference here holds the object: these objects are small
 * and few. */

struct weak
{
    const void **vtbl;
    LONG ref;
    IUnknown *target;
};

static ULONG WINAPI weak_AddRef( struct weak *w )
{
    return InterlockedIncrement( &w->ref );
}

static ULONG WINAPI weak_Release( struct weak *w )
{
    ULONG ref = InterlockedDecrement( &w->ref );
    if (!ref)
    {
        IUnknown_Release( w->target );
        free( w );
    }
    return ref;
}

static HRESULT WINAPI weakref_QueryInterface( struct weak *w, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IWeakReference_ ))
    {
        *out = w;
        weak_AddRef( w );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI weakref_Resolve( struct weak *w, REFIID iid, IInspectable **out )
{
    return IUnknown_QueryInterface( w->target, iid, (void **)out );
}

static const void *weakref_vtbl[] = { weakref_QueryInterface, weak_AddRef, weak_Release, weakref_Resolve };

static struct weak *weak_create( const void **vtbl, IUnknown *target )
{
    struct weak *w = malloc( sizeof(*w) );
    if (!w) return NULL;
    w->vtbl = vtbl;
    w->ref = 1;
    w->target = target;
    IUnknown_AddRef( w->target );
    return w;
}

static HRESULT WINAPI weaksource_QueryInterface( struct weak *w, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IWeakReferenceSource_ ))
    {
        *out = w;
        weak_AddRef( w );
        return S_OK;
    }
    return IUnknown_QueryInterface( w->target, iid, out );
}

static HRESULT WINAPI weaksource_GetWeakReference( struct weak *w, IUnknown **out )
{
    if (!out) return E_POINTER;
    return (*out = (IUnknown *)weak_create( weakref_vtbl, w->target )) ? S_OK : E_OUTOFMEMORY;
}

static const void *weaksource_vtbl[] = { weaksource_QueryInterface, weak_AddRef, weak_Release, weaksource_GetWeakReference };

HRESULT weak_source( IUnknown *target, void **out )
{
    return (*out = weak_create( weaksource_vtbl, target )) ? S_OK : E_OUTOFMEMORY;
}

/* ---- string lists ---- */

void strlist_add_n( struct strlist *list, const char *str, size_t len )
{
    char **items = realloc( list->items, (list->count + 1) * sizeof(*items) );
    char *copy = malloc( len + 1 );

    if (!items || !copy)
    {
        if (items) list->items = items;
        free( copy );
        return;
    }
    memcpy( copy, str, len );
    copy[len] = 0;
    list->items = items;
    list->items[list->count++] = copy;
}

void strlist_add( struct strlist *list, const char *str )
{
    strlist_add_n( list, str, strlen( str ) );
}

BOOL strlist_has( const struct strlist *list, const char *str )
{
    unsigned i;
    for (i = 0; i < list->count; i++) if (!strcmp( list->items[i], str )) return TRUE;
    return FALSE;
}

void strlist_add_unique( struct strlist *list, const char *str )
{
    if (!strlist_has( list, str )) strlist_add( list, str );
}

/* the non-empty parts of str between separators */
void strlist_split( struct strlist *list, const char *str, char sep )
{
    const char *end;

    for (;;)
    {
        end = strchr( str, sep );
        if (!end) end = str + strlen( str );
        if (end > str) strlist_add_n( list, str, end - str );
        if (!*end) break;
        str = end + 1;
    }
}

void strlist_copy( struct strlist *dst, const struct strlist *src )
{
    unsigned i;
    strlist_free( dst );
    for (i = 0; i < src->count; i++) strlist_add( dst, src->items[i] );
}

void strlist_free( struct strlist *list )
{
    unsigned i;
    for (i = 0; i < list->count; i++) free( list->items[i] );
    free( list->items );
    list->items = NULL;
    list->count = 0;
}

char *strlist_join( const struct strlist *list, char sep )
{
    size_t len = 1;
    unsigned i;
    char *out, *p;

    for (i = 0; i < list->count; i++) len += strlen( list->items[i] ) + 1;
    if (!(out = p = malloc( len ))) return NULL;
    *p = 0;
    for (i = 0; i < list->count; i++)
    {
        if (i) *p++ = sep;
        strcpy( p, list->items[i] );
        p += strlen( p );
    }
    return out;
}

static int __cdecl compare_str( const void *a, const void *b )
{
    return strcmp( *(char *const *)a, *(char *const *)b );
}

void strlist_sort( struct strlist *list )
{
    if (list->count) qsort( list->items, list->count, sizeof(*list->items), compare_str );
}

/* ---- IVectorView<String> ---- */

static const GUID IID_IVectorView_HString = {0x2f13c006,0xa03a,0x5f69,{0xb0,0x90,0x75,0xa4,0x3e,0x33,0x42,0x3e}};
static const GUID IID_IIterable_HString = {0xe2fcc7c1,0x3bfc,0x5a0b,{0xb2,0xb0,0x72,0xe7,0x69,0xd1,0xcb,0x7e}};
static const GUID IID_IIterator_HString = {0x8c304ebb,0x6615,0x50a4,{0x88,0x29,0x87,0x9e,0xcd,0x44,0x32,0x36}};

struct hstring_view
{
    struct object obj;
    WCHAR **items;
    UINT32 count;
};

struct hstring_iterator
{
    struct object obj;
    struct hstring_view *view;
    UINT32 pos;
};

static void hstring_view_destroy( struct object *obj )
{
    struct hstring_view *view = (struct hstring_view *)obj;
    UINT32 i;
    for (i = 0; i < view->count; i++) free( view->items[i] );
    free( view->items );
}

static HRESULT WINAPI hsv_GetAt( struct hstring_view *view, UINT32 i, HSTRING *v )
{
    if (i >= view->count) return E_BOUNDS;
    return hstr( view->items[i], v );
}

static HRESULT WINAPI hsv_get_Size( struct hstring_view *view, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = view->count;
    return S_OK;
}

static HRESULT WINAPI hsv_IndexOf( struct hstring_view *view, HSTRING s, UINT32 *i, boolean *found )
{
    const WCHAR *w = WindowsGetStringRawBuffer( s, NULL );
    UINT32 k;

    for (k = 0; k < view->count; k++)
        if (!wcscmp( view->items[k], w ))
        {
            *i = k;
            *found = TRUE;
            return S_OK;
        }
    *i = 0;
    *found = FALSE;
    return S_OK;
}

static HRESULT WINAPI hsv_GetMany( struct hstring_view *view, UINT32 start, UINT32 n, HSTRING *out, UINT32 *got )
{
    UINT32 k = 0;
    while (k < n && start + k < view->count)
    {
        hstr( view->items[start + k], &out[k] );
        k++;
    }
    if (got) *got = k;
    return S_OK;
}

static void hstring_iterator_destroy( struct object *obj )
{
    obj_Release( &((struct hstring_iterator *)obj)->view->obj );
}

static HRESULT WINAPI hsi_get_Current( struct hstring_iterator *it, HSTRING *v )
{
    if (it->pos >= it->view->count) return E_BOUNDS;
    return hstr( it->view->items[it->pos], v );
}

static HRESULT WINAPI hsi_get_HasCurrent( struct hstring_iterator *it, boolean *v )
{
    if (!v) return E_POINTER;
    *v = it->pos < it->view->count;
    return S_OK;
}

static HRESULT WINAPI hsi_MoveNext( struct hstring_iterator *it, boolean *v )
{
    if (it->pos < it->view->count) it->pos++;
    if (v) *v = it->pos < it->view->count;
    return S_OK;
}

static HRESULT WINAPI hsi_GetMany( struct hstring_iterator *it, UINT32 n, HSTRING *out, UINT32 *got )
{
    UINT32 k = 0;
    while (k < n && it->pos < it->view->count) hstr( it->view->items[it->pos++], &out[k++] );
    if (got) *got = k;
    return S_OK;
}

static const void *hstring_iterator_vtbl[] = { OBJ_METHODS, hsi_get_Current, hsi_get_HasCurrent, hsi_MoveNext, hsi_GetMany };

static HRESULT WINAPI hsv_First( const void ***iface, IInspectable **out )
{
    struct hstring_view *view = (struct hstring_view *)OBJ2(iface);
    struct hstring_iterator *it;

    if (!out) return E_POINTER;
    if (!(it = object_create( sizeof(*it), hstring_iterator_vtbl, &IID_IIterator_HString,
                              L"Windows.Foundation.Collections.IIterator`1<String>", hstring_iterator_destroy )))
        return E_OUTOFMEMORY;
    obj_AddRef( &(it->view = view)->obj );
    *out = (IInspectable *)it;
    return S_OK;
}

static const void *hstring_view_vtbl[] = { OBJ_METHODS, hsv_GetAt, hsv_get_Size, hsv_IndexOf, hsv_GetMany };
static const void *hstring_iterable_vtbl[] = { OBJ2_METHODS, hsv_First };

struct hstring_view *hstring_view_create( void )
{
    struct hstring_view *view = object_create( sizeof(*view), hstring_view_vtbl, &IID_IVectorView_HString,
                                               L"Windows.Foundation.Collections.IVectorView`1<String>", hstring_view_destroy );
    if (!view) return NULL;
    view->obj.vtbl2 = hstring_iterable_vtbl;
    view->obj.iid2 = IID_IIterable_HString;
    return view;
}

void hstring_view_add( struct hstring_view *view, const WCHAR *str )
{
    WCHAR **items = realloc( view->items, (view->count + 1) * sizeof(*items) );
    if (!items) return;
    view->items = items;
    if ((items[view->count] = wcsdup( str ))) view->count++;
}

UINT32 hstring_view_size( struct hstring_view *view )
{
    return view->count;
}

/* ---- IVectorView<Guid> {9520e64b}, IIterable<Guid> {f4ca3045}, IIterator<Guid> {d3d64048} ---- */

static const GUID IID_IVectorView_Guid = {0x9520e64b,0x15b2,0x52a6,{0x98,0xed,0x31,0x91,0xfa,0x6c,0xf6,0x8a}};
static const GUID IID_IIterable_Guid = {0xf4ca3045,0x5dd7,0x54be,{0x98,0x2e,0xd8,0x8d,0x8c,0xa0,0x87,0x6e}};
static const GUID IID_IIterator_Guid = {0xd3d64048,0x82b3,0x53c7,{0x92,0x85,0xb0,0xbe,0x18,0x36,0x84,0x82}};

struct guid_view
{
    struct object obj;
    GUID items[8];
    UINT32 count;
};

struct guid_iterator
{
    struct object obj;
    struct guid_view *view;
    UINT32 pos;
};

static HRESULT WINAPI gv_GetAt( struct guid_view *view, UINT32 i, GUID *v )
{
    if (!v) return E_POINTER;
    if (i >= view->count) return E_BOUNDS;
    *v = view->items[i];
    return S_OK;
}

static HRESULT WINAPI gv_get_Size( struct guid_view *view, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = view->count;
    return S_OK;
}

static HRESULT WINAPI gv_IndexOf( struct guid_view *view, const GUID *g, UINT32 *idx, boolean *found )
{
    UINT32 i;
    if (!g || !idx || !found) return E_POINTER;
    *found = FALSE;
    *idx = 0;
    for (i = 0; i < view->count; i++)
        if (IsEqualGUID( &view->items[i], g ))
        {
            *idx = i;
            *found = TRUE;
            break;
        }
    return S_OK;
}

static HRESULT WINAPI gv_GetMany( struct guid_view *view, UINT32 start, UINT32 n, GUID *out, UINT32 *got )
{
    UINT32 i;
    if (!got || (n && !out)) return E_POINTER;
    *got = 0;
    for (i = start; i < view->count && *got < n; i++) out[(*got)++] = view->items[i];
    return S_OK;
}

static void guid_iterator_destroy( struct object *obj )
{
    obj_Release( &((struct guid_iterator *)obj)->view->obj );
}

static HRESULT WINAPI gi_get_Current( struct guid_iterator *it, GUID *v )
{
    return gv_GetAt( it->view, it->pos, v );
}

static HRESULT WINAPI gi_get_HasCurrent( struct guid_iterator *it, boolean *v )
{
    if (!v) return E_POINTER;
    *v = it->pos < it->view->count;
    return S_OK;
}

static HRESULT WINAPI gi_MoveNext( struct guid_iterator *it, boolean *v )
{
    if (it->pos < it->view->count) it->pos++;
    return gi_get_HasCurrent( it, v );
}

static HRESULT WINAPI gi_GetMany( struct guid_iterator *it, UINT32 n, GUID *out, UINT32 *got )
{
    HRESULT hr = gv_GetMany( it->view, it->pos, n, out, got );
    if (SUCCEEDED(hr)) it->pos += *got;
    return hr;
}

static const void *guid_iterator_vtbl[] = { OBJ_METHODS, gi_get_Current, gi_get_HasCurrent, gi_MoveNext, gi_GetMany };

static HRESULT WINAPI gv_First( const void ***iface, IInspectable **out )
{
    struct guid_view *view = (struct guid_view *)OBJ2(iface);
    struct guid_iterator *it;

    if (!out) return E_POINTER;
    if (!(it = object_create( sizeof(*it), guid_iterator_vtbl, &IID_IIterator_Guid,
                              L"Windows.Foundation.Collections.IIterator`1<Guid>", guid_iterator_destroy )))
        return E_OUTOFMEMORY;
    obj_AddRef( &(it->view = view)->obj );
    *out = (IInspectable *)it;
    return S_OK;
}

static const void *guid_view_vtbl[] = { OBJ_METHODS, gv_GetAt, gv_get_Size, gv_IndexOf, gv_GetMany };
static const void *guid_iterable_vtbl[] = { OBJ2_METHODS, gv_First };

struct guid_view *guid_view_create( void )
{
    struct guid_view *view = object_create( sizeof(*view), guid_view_vtbl, &IID_IVectorView_Guid,
                                            L"Windows.Foundation.Collections.IVectorView`1<Guid>", NULL );
    if (!view) return NULL;
    view->obj.vtbl2 = guid_iterable_vtbl;
    view->obj.iid2 = IID_IIterable_Guid;
    return view;
}

void guid_view_add( struct guid_view *view, const GUID *guid )
{
    if (view->count < ARRAY_SIZE(view->items)) view->items[view->count++] = *guid;
}

/* ---- IVectorView<T> of interfaces, with IIterable<T> and its iterators ---- */

struct obj_view
{
    struct object obj;
    GUID iterator_iid;
    IInspectable **items;
    UINT32 count;
};

struct obj_iterator
{
    struct object obj;
    struct obj_view *view;
    UINT32 pos;
};

static void obj_view_destroy( struct object *obj )
{
    struct obj_view *view = (struct obj_view *)obj;
    UINT32 i;
    for (i = 0; i < view->count; i++) IInspectable_Release( view->items[i] );
    free( view->items );
}

static HRESULT WINAPI ov_GetAt( struct obj_view *view, UINT32 i, IInspectable **v )
{
    if (!v) return E_POINTER;
    if (i >= view->count)
    {
        *v = NULL;
        return E_BOUNDS;
    }
    *v = view->items[i];
    IInspectable_AddRef( *v );
    return S_OK;
}

static HRESULT WINAPI ov_get_Size( struct obj_view *view, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = view->count;
    return S_OK;
}

static HRESULT WINAPI ov_IndexOf( struct obj_view *view, IInspectable *x, UINT32 *idx, boolean *found )
{
    UINT32 i;
    if (!idx || !found) return E_POINTER;
    *idx = 0;
    *found = FALSE;
    for (i = 0; i < view->count; i++)
        if (view->items[i] == x)
        {
            *idx = i;
            *found = TRUE;
            break;
        }
    return S_OK;
}

static HRESULT WINAPI ov_GetMany( struct obj_view *view, UINT32 start, UINT32 n, IInspectable **out, UINT32 *got )
{
    UINT32 i;
    if (!got) return E_POINTER;
    *got = 0;
    for (i = start; i < view->count && *got < n; i++)
    {
        out[*got] = view->items[i];
        IInspectable_AddRef( out[*got] );
        ++*got;
    }
    return S_OK;
}

static void obj_iterator_destroy( struct object *obj )
{
    obj_Release( &((struct obj_iterator *)obj)->view->obj );
}

static HRESULT WINAPI oi_get_Current( struct obj_iterator *it, IInspectable **v )
{
    return ov_GetAt( it->view, it->pos, v );
}

static HRESULT WINAPI oi_get_HasCurrent( struct obj_iterator *it, boolean *v )
{
    if (!v) return E_POINTER;
    *v = it->pos < it->view->count;
    return S_OK;
}

static HRESULT WINAPI oi_MoveNext( struct obj_iterator *it, boolean *v )
{
    if (it->pos < it->view->count) it->pos++;
    return oi_get_HasCurrent( it, v );
}

static HRESULT WINAPI oi_GetMany( struct obj_iterator *it, UINT32 n, IInspectable **out, UINT32 *got )
{
    HRESULT hr = ov_GetMany( it->view, it->pos, n, out, got );
    if (SUCCEEDED(hr)) it->pos += *got;
    return hr;
}

static const void *obj_iterator_vtbl[] = { OBJ_METHODS, oi_get_Current, oi_get_HasCurrent, oi_MoveNext, oi_GetMany };

IInspectable *obj_iterator_create( struct obj_view *view )
{
    struct obj_iterator *it = object_create( sizeof(*it), obj_iterator_vtbl, &view->iterator_iid,
                                             L"Windows.Foundation.Collections.IIterator`1", obj_iterator_destroy );
    if (!it) return NULL;
    obj_AddRef( &(it->view = view)->obj );
    return (IInspectable *)it;
}

static HRESULT WINAPI ov_First( const void ***iface, IInspectable **out )
{
    if (!out) return E_POINTER;
    return (*out = obj_iterator_create( (struct obj_view *)OBJ2(iface) )) ? S_OK : E_OUTOFMEMORY;
}

static const void *obj_view_vtbl[] = { OBJ_METHODS, ov_GetAt, ov_get_Size, ov_IndexOf, ov_GetMany };
static const void *obj_iterable_vtbl[] = { OBJ2_METHODS, ov_First };

struct obj_view *obj_view_create( const GUID *view_iid, const GUID *iterable, const GUID *iterator, const WCHAR *name )
{
    struct obj_view *view = object_create( sizeof(*view), obj_view_vtbl, view_iid, name, obj_view_destroy );
    if (!view) return NULL;
    view->obj.vtbl2 = obj_iterable_vtbl;
    view->obj.iid2 = *iterable;
    if (iterator) view->iterator_iid = *iterator;
    return view;
}

void obj_view_add( struct obj_view *view, IInspectable *item )
{
    IInspectable **items;

    if (!item) return;
    if (!(items = realloc( view->items, (view->count + 1) * sizeof(*items) )))
    {
        IInspectable_Release( item );
        return;
    }
    view->items = items;
    items[view->count++] = item;
}

/* ---- IAsyncOperation<interface> and IAsyncOperation<Guid> ---- */

struct async
{
    struct object obj;          /* vtbl: IAsyncOperation<T>, vtbl2: IAsyncInfo */
    SRWLOCK lock;
    INT32 status;               /* 0 started, 1 completed, 3 error */
    HRESULT error;
    IUnknown *handler, *result;
    GUID guid;
};

static void async_destroy( struct object *obj )
{
    struct async *op = (struct async *)obj;
    if (op->handler) IUnknown_Release( op->handler );
    if (op->result) IUnknown_Release( op->result );
}

static HRESULT WINAPI async_put_Completed( struct async *op, IUnknown *handler )
{
    INT32 status;

    AcquireSRWLockExclusive( &op->lock );
    if (op->handler)
    {
        ReleaseSRWLockExclusive( &op->lock );
        return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    }
    if (handler) IUnknown_AddRef( handler );
    op->handler = handler;
    status = op->status;
    ReleaseSRWLockExclusive( &op->lock );
    if (handler && status) invoke_delegate( handler, op, (void *)(INT_PTR)status );
    return S_OK;
}

static HRESULT WINAPI async_get_Completed( struct async *op, IUnknown **handler )
{
    if (!handler) return E_POINTER;
    AcquireSRWLockShared( &op->lock );
    if ((*handler = op->handler)) IUnknown_AddRef( op->handler );
    ReleaseSRWLockShared( &op->lock );
    return S_OK;
}

static HRESULT WINAPI async_GetResults( struct async *op, IUnknown **out )
{
    INT32 status;
    HRESULT error;

    if (!out) return E_POINTER;
    AcquireSRWLockShared( &op->lock );
    status = op->status;
    error = op->error;
    if (status == 1 && (*out = op->result)) IUnknown_AddRef( op->result );
    ReleaseSRWLockShared( &op->lock );
    return status == 1 ? S_OK : status == 3 ? error : E_ILLEGAL_METHOD_CALL;
}

static HRESULT WINAPI async_GetResults_guid( struct async *op, GUID *out )
{
    if (!out) return E_POINTER;
    *out = op->guid;
    return op->error;
}

static HRESULT WINAPI async_get_Id( const void ***iface, UINT32 *v )
{
    if (!v) return E_POINTER;
    *v = 1;
    return S_OK;
}

static HRESULT WINAPI async_get_Status( const void ***iface, INT32 *v )
{
    struct async *op = (struct async *)OBJ2(iface);
    if (!v) return E_POINTER;
    AcquireSRWLockShared( &op->lock );
    *v = op->status;
    ReleaseSRWLockShared( &op->lock );
    return S_OK;
}

static HRESULT WINAPI async_get_ErrorCode( const void ***iface, HRESULT *v )
{
    struct async *op = (struct async *)OBJ2(iface);
    if (!v) return E_POINTER;
    AcquireSRWLockShared( &op->lock );
    *v = op->status == 3 ? op->error : S_OK;
    ReleaseSRWLockShared( &op->lock );
    return S_OK;
}

static HRESULT WINAPI async_Cancel( const void ***iface ) { return S_OK; }
static HRESULT WINAPI async_Close( const void ***iface ) { return S_OK; }

static const void *async_vtbl[] = { OBJ_METHODS, async_put_Completed, async_get_Completed, async_GetResults };
static const void *async_guid_vtbl[] = { OBJ_METHODS, async_put_Completed, async_get_Completed, async_GetResults_guid };
static const void *async_info_vtbl[] = { OBJ2_METHODS, async_get_Id, async_get_Status, async_get_ErrorCode, async_Cancel, async_Close };

struct async *async_create( const GUID *iid, BOOL guid_result )
{
    struct async *op = object_create( sizeof(*op), guid_result ? async_guid_vtbl : async_vtbl, iid,
                                      guid_result ? L"Windows.Foundation.IAsyncOperation`1<Guid>"
                                                  : L"Windows.Foundation.IAsyncOperation`1", async_destroy );
    if (!op) return NULL;
    op->obj.vtbl2 = async_info_vtbl;
    op->obj.iid2 = IID_IAsyncInfo_;
    InitializeSRWLock( &op->lock );
    return op;
}

IInspectable *async_iface( struct async *op )
{
    return (IInspectable *)&op->obj.vtbl;
}

/* completes the operation; takes the result's reference */
void async_finish( struct async *op, HRESULT hr, IUnknown *result )
{
    IUnknown *handler;
    INT32 status;

    AcquireSRWLockExclusive( &op->lock );
    op->status = status = FAILED(hr) ? 3 : 1;
    op->error = hr;
    op->result = result;
    if ((handler = op->handler)) IUnknown_AddRef( handler );
    ReleaseSRWLockExclusive( &op->lock );
    if (handler)
    {
        invoke_delegate( handler, op, (void *)(INT_PTR)status );
        IUnknown_Release( handler );
    }
}

void async_finish_guid( struct async *op, HRESULT hr, const GUID *result )
{
    op->guid = *result;
    async_finish( op, hr, NULL );
}

struct async_work
{
    struct async *op;
    HRESULT (*work)( void *ctx, IUnknown **result );
    void *ctx;
};

static DWORD WINAPI async_thread( void *param )
{
    struct async_work *w = param;
    IUnknown *result = NULL;
    HRESULT hr = w->work( w->ctx, &result );

    async_finish( w->op, hr, result );
    obj_Release( &w->op->obj );
    free( w );
    return 0;
}

/* runs work on a thread of its own */
IInspectable *async_start( const GUID *iid, HRESULT (*work)( void *ctx, IUnknown **result ), void *ctx )
{
    struct async *op = async_create( iid, FALSE );
    struct async_work *w = malloc( sizeof(*w) );
    HANDLE thread;

    if (!op || !w)
    {
        if (op) obj_Release( &op->obj );
        free( w );
        return NULL;
    }
    obj_AddRef( &op->obj );
    w->op = op;
    w->work = work;
    w->ctx = ctx;
    if ((thread = CreateThread( NULL, 0, async_thread, w, 0, NULL ))) CloseHandle( thread );
    else
    {
        async_finish( op, E_OUTOFMEMORY, NULL );
        obj_Release( &op->obj );
        free( w );
    }
    return async_iface( op );
}

/* IIterable<Windows.ApplicationModel.Package> for PackageManager's Find* queries
 *
 * Copyright (C) 2026 Bobby Kircher
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
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appx);

/* The only package a hosted process can see is its own, so a result holds at most one element.
 * An empty result is a real answer ("nothing else is installed"), not a refusal. */
struct package_list
{
    IIterable_Package IIterable_Package_iface;
    LONG ref;
    IPackage *package; /* may be NULL */
};

struct package_iterator
{
    IIterator_Package IIterator_Package_iface;
    LONG ref;
    struct package_list *list;
    UINT32 index;
};

static inline struct package_iterator *impl_from_IIterator_Package( IIterator_Package *iface )
{
    return CONTAINING_RECORD( iface, struct package_iterator, IIterator_Package_iface );
}

static HRESULT WINAPI iterator_QueryInterface( IIterator_Package *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_Package ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI iterator_AddRef( IIterator_Package *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_Package( iface )->ref );
}

static ULONG WINAPI iterator_Release( IIterator_Package *iface )
{
    struct package_iterator *impl = impl_from_IIterator_Package( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IIterable_Package_Release( &impl->list->IIterable_Package_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI iterator_GetIids( IIterator_Package *iface, ULONG *iid_count, IID **iids )
{
    if (!iid_count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( sizeof(IID) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_IIterator_Package;
    *iid_count = 1;
    return S_OK;
}

static HRESULT WINAPI iterator_GetRuntimeClassName( IIterator_Package *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Foundation.Collections.IIterator`1<Windows.ApplicationModel.Package>";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI iterator_GetTrustLevel( IIterator_Package *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static UINT32 list_size( struct package_list *list )
{
    return list->package ? 1 : 0;
}

static HRESULT WINAPI iterator_get_Current( IIterator_Package *iface, IPackage **value )
{
    struct package_iterator *impl = impl_from_IIterator_Package( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (impl->index >= list_size( impl->list )) return E_BOUNDS;
    IPackage_AddRef( (*value = impl->list->package) );
    return S_OK;
}

static HRESULT WINAPI iterator_get_HasCurrent( IIterator_Package *iface, boolean *value )
{
    struct package_iterator *impl = impl_from_IIterator_Package( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->index < list_size( impl->list );
    return S_OK;
}

static HRESULT WINAPI iterator_MoveNext( IIterator_Package *iface, boolean *value )
{
    struct package_iterator *impl = impl_from_IIterator_Package( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (impl->index < list_size( impl->list )) impl->index++;
    return iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI iterator_GetMany( IIterator_Package *iface, UINT32 items_size, IPackage **items, UINT *count )
{
    struct package_iterator *impl = impl_from_IIterator_Package( iface );
    UINT32 n = 0;

    TRACE( "iface %p, items_size %u, items %p, count %p.\n", iface, items_size, items, count );

    while (n < items_size && impl->index < list_size( impl->list ))
    {
        IPackage_AddRef( (items[n++] = impl->list->package) );
        impl->index++;
    }
    *count = n;
    return S_OK;
}

static const struct IIterator_PackageVtbl iterator_vtbl =
{
    iterator_QueryInterface,
    iterator_AddRef,
    iterator_Release,
    /* IInspectable methods */
    iterator_GetIids,
    iterator_GetRuntimeClassName,
    iterator_GetTrustLevel,
    /* IIterator<Package> methods */
    iterator_get_Current,
    iterator_get_HasCurrent,
    iterator_MoveNext,
    iterator_GetMany,
};

static inline struct package_list *impl_from_IIterable_Package( IIterable_Package *iface )
{
    return CONTAINING_RECORD( iface, struct package_list, IIterable_Package_iface );
}

static HRESULT WINAPI list_QueryInterface( IIterable_Package *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterable_Package ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI list_AddRef( IIterable_Package *iface )
{
    return InterlockedIncrement( &impl_from_IIterable_Package( iface )->ref );
}

static ULONG WINAPI list_Release( IIterable_Package *iface )
{
    struct package_list *impl = impl_from_IIterable_Package( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->package) IPackage_Release( impl->package );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI list_GetIids( IIterable_Package *iface, ULONG *iid_count, IID **iids )
{
    if (!iid_count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( sizeof(IID) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_IIterable_Package;
    *iid_count = 1;
    return S_OK;
}

static HRESULT WINAPI list_GetRuntimeClassName( IIterable_Package *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Foundation.Collections.IIterable`1<Windows.ApplicationModel.Package>";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI list_GetTrustLevel( IIterable_Package *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI list_First( IIterable_Package *iface, IIterator_Package **value )
{
    struct package_list *impl = impl_from_IIterable_Package( iface );
    struct package_iterator *iter;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_Package_iface.lpVtbl = &iterator_vtbl;
    iter->ref = 1;
    IIterable_Package_AddRef( (iter->list = impl, iface) );
    *value = &iter->IIterator_Package_iface;
    return S_OK;
}

static const struct IIterable_PackageVtbl list_vtbl =
{
    list_QueryInterface,
    list_AddRef,
    list_Release,
    /* IInspectable methods */
    list_GetIids,
    list_GetRuntimeClassName,
    list_GetTrustLevel,
    /* IIterable<Package> methods */
    list_First,
};

HRESULT package_list_create( IPackage *package, IIterable_Package **out )
{
    struct package_list *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IIterable_Package_iface.lpVtbl = &list_vtbl;
    impl->ref = 1;
    if ((impl->package = package)) IPackage_AddRef( package );
    *out = &impl->IIterable_Package_iface;
    return S_OK;
}

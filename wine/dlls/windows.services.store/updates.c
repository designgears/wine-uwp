/*
 * Store package update enumeration for locally installed packages.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(storecontext);

/* IVectorView<StorePackageUpdate>, IIterable<StorePackageUpdate>,
 * IIterator<StorePackageUpdate>, IAsyncOperation<IVectorView<StorePackageUpdate>>. */
static const GUID update_view_iid = {0x971c3ea6,0x4388,0x5a38,{0xae,0x13,0x49,0x29,0xb6,0xd6,0xd7,0x80}};
static const GUID update_iterable_iid = {0x6b076c51,0x849e,0x5ec5,{0xae,0xd5,0x9b,0x05,0x85,0x59,0x19,0x02}};
static const GUID update_iterator_iid = {0xb75dd77b,0x87ca,0x5956,{0x89,0x02,0x84,0xe9,0xff,0xc9,0x7d,0x83}};
static const GUID update_operation_iid = {0x0ac66c33,0x45b8,0x546b,{0xaa,0xaf,0xd5,0x8d,0x62,0xa4,0xc5,0xc5}};

static HRESULT get_updates( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    static const struct vector_iids iids =
    {
        &IID_IVector_IInspectable, &update_view_iid, &update_iterable_iid, &update_iterator_iid,
    };
    IVectorView_IInspectable *view;
    IVector_IInspectable *vector;
    HRESULT hr;

    /* There is no Store update service for this local installation. */
    if (FAILED(hr = vector_create( &iids, (void **)&vector ))) return hr;
    hr = IVector_IInspectable_GetView( vector, &view );
    IVector_IInspectable_Release( vector );
    if (FAILED(hr)) return hr;

    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)view;
    return S_OK;
}

HRESULT WINAPI store_get_updates( IUnknown *iface, IInspectable **out )
{
    TRACE( "context %p, out %p: local installation, no Store updates.\n", iface, out );
    if (!out) return E_POINTER;
    return async_operation_inspectable_create( &update_operation_iid, iface, NULL, get_updates,
                                              (IAsyncOperation_IInspectable **)out );
}

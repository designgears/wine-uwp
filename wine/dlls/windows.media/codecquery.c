/* WinRT Windows.Media.Core.CodecQuery Implementation
 *
 * Copyright 2026 Bobby Kircher
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

#define WIDL_using_Windows_Media_Core
#include "private.h"
#include "mfapi.h"
#include "mftransform.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(media);

/* CodecQuery answers from the Media Foundation transforms that are registered, as Windows does:
 * a decoder is listed with the subtypes it takes, an encoder with the ones it produces. Apps use
 * it to decide whether media works at all - Shapr3D shows "You need a media feature pack" and
 * stops when nothing comes back - so the answer has to reflect what MF can really do here. */

/*
 * IVectorView<HSTRING>, for CodecInfo.Subtypes
 */

struct hstring_vector
{
    IVectorView_HSTRING IVectorView_HSTRING_iface;
    IIterable_HSTRING IIterable_HSTRING_iface;
    LONG ref;
    UINT32 count;
    HSTRING values[];
};

struct hstring_iterator
{
    IIterator_HSTRING IIterator_HSTRING_iface;
    LONG ref;
    struct hstring_vector *vector;
    UINT32 index;
};

static inline struct hstring_iterator *impl_from_IIterator_HSTRING( IIterator_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_iterator, IIterator_HSTRING_iface );
}

static HRESULT WINAPI hstring_iterator_QueryInterface( IIterator_HSTRING *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_HSTRING ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI hstring_iterator_AddRef( IIterator_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_HSTRING( iface )->ref );
}

static ULONG WINAPI hstring_iterator_Release( IIterator_HSTRING *iface )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVectorView_HSTRING_Release( &impl->vector->IVectorView_HSTRING_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI hstring_iterator_GetIids( IIterator_HSTRING *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI hstring_iterator_GetRuntimeClassName( IIterator_HSTRING *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Foundation.Collections.IIterator`1<String>";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI hstring_iterator_GetTrustLevel( IIterator_HSTRING *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI hstring_iterator_get_Current( IIterator_HSTRING *iface, HSTRING *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );

    *value = NULL;
    if (impl->index >= impl->vector->count) return E_BOUNDS;
    return WindowsDuplicateString( impl->vector->values[impl->index], value );
}

static HRESULT WINAPI hstring_iterator_get_HasCurrent( IIterator_HSTRING *iface, boolean *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );

    *value = impl->index < impl->vector->count;
    return S_OK;
}

static HRESULT WINAPI hstring_iterator_MoveNext( IIterator_HSTRING *iface, boolean *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );

    if (impl->index < impl->vector->count) impl->index++;
    return hstring_iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI hstring_iterator_GetMany( IIterator_HSTRING *iface, UINT32 size, HSTRING *items, UINT *count )
{
    struct hstring_iterator *impl = impl_from_IIterator_HSTRING( iface );
    UINT32 n = 0;
    HRESULT hr = S_OK;

    while (n < size && impl->index < impl->vector->count && SUCCEEDED(hr))
        if (SUCCEEDED(hr = WindowsDuplicateString( impl->vector->values[impl->index], &items[n] )))
        {
            n++;
            impl->index++;
        }
    *count = n;
    return hr;
}

static const struct IIterator_HSTRINGVtbl hstring_iterator_vtbl =
{
    hstring_iterator_QueryInterface,
    hstring_iterator_AddRef,
    hstring_iterator_Release,
    /* IInspectable methods */
    hstring_iterator_GetIids,
    hstring_iterator_GetRuntimeClassName,
    hstring_iterator_GetTrustLevel,
    /* IIterator<HSTRING> methods */
    hstring_iterator_get_Current,
    hstring_iterator_get_HasCurrent,
    hstring_iterator_MoveNext,
    hstring_iterator_GetMany,
};

static inline struct hstring_vector *impl_from_IVectorView_HSTRING( IVectorView_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_vector, IVectorView_HSTRING_iface );
}

static HRESULT WINAPI hstring_vector_QueryInterface( IVectorView_HSTRING *iface, REFIID iid, void **out )
{
    struct hstring_vector *impl = impl_from_IVectorView_HSTRING( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IVectorView_HSTRING ))
    {
        IInspectable_AddRef( (*out = &impl->IVectorView_HSTRING_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IIterable_HSTRING ))
    {
        IInspectable_AddRef( (*out = &impl->IIterable_HSTRING_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI hstring_vector_AddRef( IVectorView_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IVectorView_HSTRING( iface )->ref );
}

static ULONG WINAPI hstring_vector_Release( IVectorView_HSTRING *iface )
{
    struct hstring_vector *impl = impl_from_IVectorView_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) WindowsDeleteString( impl->values[i] );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI hstring_vector_GetIids( IVectorView_HSTRING *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI hstring_vector_GetRuntimeClassName( IVectorView_HSTRING *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Foundation.Collections.IVectorView`1<String>";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI hstring_vector_GetTrustLevel( IVectorView_HSTRING *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI hstring_vector_GetAt( IVectorView_HSTRING *iface, UINT32 index, HSTRING *value )
{
    struct hstring_vector *impl = impl_from_IVectorView_HSTRING( iface );

    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    return WindowsDuplicateString( impl->values[index], value );
}

static HRESULT WINAPI hstring_vector_get_Size( IVectorView_HSTRING *iface, UINT32 *value )
{
    *value = impl_from_IVectorView_HSTRING( iface )->count;
    return S_OK;
}

static HRESULT WINAPI hstring_vector_IndexOf( IVectorView_HSTRING *iface, HSTRING element, UINT32 *index, BOOLEAN *found )
{
    struct hstring_vector *impl = impl_from_IVectorView_HSTRING( iface );
    INT32 order;
    UINT32 i;

    for (i = 0; i < impl->count; i++)
    {
        if (SUCCEEDED(WindowsCompareStringOrdinal( impl->values[i], element, &order )) && !order)
        {
            *index = i;
            *found = TRUE;
            return S_OK;
        }
    }
    *index = 0;
    *found = FALSE;
    return S_OK;
}

static HRESULT WINAPI hstring_vector_GetMany( IVectorView_HSTRING *iface, UINT32 start, UINT32 size,
                                              HSTRING *items, UINT *count )
{
    struct hstring_vector *impl = impl_from_IVectorView_HSTRING( iface );
    UINT32 n = 0;
    HRESULT hr = S_OK;

    for (; start < impl->count && n < size && SUCCEEDED(hr); start++)
        if (SUCCEEDED(hr = WindowsDuplicateString( impl->values[start], &items[n] ))) n++;
    *count = n;
    return hr;
}

static const struct IVectorView_HSTRINGVtbl hstring_vector_vtbl =
{
    hstring_vector_QueryInterface,
    hstring_vector_AddRef,
    hstring_vector_Release,
    /* IInspectable methods */
    hstring_vector_GetIids,
    hstring_vector_GetRuntimeClassName,
    hstring_vector_GetTrustLevel,
    /* IVectorView<HSTRING> methods */
    hstring_vector_GetAt,
    hstring_vector_get_Size,
    hstring_vector_IndexOf,
    hstring_vector_GetMany,
};

DEFINE_IINSPECTABLE( hstring_iterable, IIterable_HSTRING, struct hstring_vector, IVectorView_HSTRING_iface )

static HRESULT WINAPI hstring_iterable_First( IIterable_HSTRING *iface, IIterator_HSTRING **value )
{
    struct hstring_vector *impl = impl_from_IIterable_HSTRING( iface );
    struct hstring_iterator *iter;

    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_HSTRING_iface.lpVtbl = &hstring_iterator_vtbl;
    iter->ref = 1;
    IVectorView_HSTRING_AddRef( &impl->IVectorView_HSTRING_iface );
    iter->vector = impl;
    *value = &iter->IIterator_HSTRING_iface;
    return S_OK;
}

static const struct IIterable_HSTRINGVtbl hstring_iterable_vtbl =
{
    hstring_iterable_QueryInterface,
    hstring_iterable_AddRef,
    hstring_iterable_Release,
    /* IInspectable methods */
    hstring_iterable_GetIids,
    hstring_iterable_GetRuntimeClassName,
    hstring_iterable_GetTrustLevel,
    /* IIterable<HSTRING> methods */
    hstring_iterable_First,
};

/*
 * CodecInfo
 */

struct codec_info
{
    ICodecInfo ICodecInfo_iface;
    LONG ref;
    CodecKind kind;
    CodecCategory category;
    struct hstring_vector *subtypes;
    HSTRING display_name;
};

static inline struct codec_info *impl_from_ICodecInfo( ICodecInfo *iface )
{
    return CONTAINING_RECORD( iface, struct codec_info, ICodecInfo_iface );
}

static HRESULT WINAPI codec_info_QueryInterface( ICodecInfo *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ICodecInfo ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI codec_info_AddRef( ICodecInfo *iface )
{
    return InterlockedIncrement( &impl_from_ICodecInfo( iface )->ref );
}

static ULONG WINAPI codec_info_Release( ICodecInfo *iface )
{
    struct codec_info *impl = impl_from_ICodecInfo( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVectorView_HSTRING_Release( &impl->subtypes->IVectorView_HSTRING_iface );
        WindowsDeleteString( impl->display_name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI codec_info_GetIids( ICodecInfo *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI codec_info_GetRuntimeClassName( ICodecInfo *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Core_CodecInfo,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Core_CodecInfo) - 1, name );
}

static HRESULT WINAPI codec_info_GetTrustLevel( ICodecInfo *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI codec_info_get_Kind( ICodecInfo *iface, CodecKind *value )
{
    *value = impl_from_ICodecInfo( iface )->kind;
    return S_OK;
}

static HRESULT WINAPI codec_info_get_Category( ICodecInfo *iface, CodecCategory *value )
{
    *value = impl_from_ICodecInfo( iface )->category;
    return S_OK;
}

static HRESULT WINAPI codec_info_get_Subtypes( ICodecInfo *iface, IVectorView_HSTRING **value )
{
    struct codec_info *impl = impl_from_ICodecInfo( iface );

    IVectorView_HSTRING_AddRef( (*value = &impl->subtypes->IVectorView_HSTRING_iface) );
    return S_OK;
}

static HRESULT WINAPI codec_info_get_DisplayName( ICodecInfo *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_ICodecInfo( iface )->display_name, value );
}

/* Windows calls a codec trusted when it is signed by Microsoft; everything here ships with Wine. */
static HRESULT WINAPI codec_info_get_IsTrusted( ICodecInfo *iface, boolean *value )
{
    *value = TRUE;
    return S_OK;
}

static const struct ICodecInfoVtbl codec_info_vtbl =
{
    codec_info_QueryInterface,
    codec_info_AddRef,
    codec_info_Release,
    /* IInspectable methods */
    codec_info_GetIids,
    codec_info_GetRuntimeClassName,
    codec_info_GetTrustLevel,
    /* ICodecInfo methods */
    codec_info_get_Kind,
    codec_info_get_Category,
    codec_info_get_Subtypes,
    codec_info_get_DisplayName,
    codec_info_get_IsTrusted,
};

/*
 * IVectorView<CodecInfo>
 */

struct codec_vector
{
    IVectorView_CodecInfo IVectorView_CodecInfo_iface;
    IIterable_CodecInfo IIterable_CodecInfo_iface;
    LONG ref;
    UINT32 count, capacity;
    ICodecInfo **items;
};

struct codec_iterator
{
    IIterator_CodecInfo IIterator_CodecInfo_iface;
    LONG ref;
    struct codec_vector *vector;
    UINT32 index;
};

static inline struct codec_iterator *impl_from_IIterator_CodecInfo( IIterator_CodecInfo *iface )
{
    return CONTAINING_RECORD( iface, struct codec_iterator, IIterator_CodecInfo_iface );
}

static HRESULT WINAPI codec_iterator_QueryInterface( IIterator_CodecInfo *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_CodecInfo ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI codec_iterator_AddRef( IIterator_CodecInfo *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_CodecInfo( iface )->ref );
}

static ULONG WINAPI codec_iterator_Release( IIterator_CodecInfo *iface )
{
    struct codec_iterator *impl = impl_from_IIterator_CodecInfo( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVectorView_CodecInfo_Release( &impl->vector->IVectorView_CodecInfo_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI codec_iterator_GetIids( IIterator_CodecInfo *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI codec_iterator_GetRuntimeClassName( IIterator_CodecInfo *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Foundation.Collections.IIterator`1<Windows.Media.Core.CodecInfo>";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI codec_iterator_GetTrustLevel( IIterator_CodecInfo *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI codec_iterator_get_Current( IIterator_CodecInfo *iface, ICodecInfo **value )
{
    struct codec_iterator *impl = impl_from_IIterator_CodecInfo( iface );

    *value = NULL;
    if (impl->index >= impl->vector->count) return E_BOUNDS;
    ICodecInfo_AddRef( (*value = impl->vector->items[impl->index]) );
    return S_OK;
}

static HRESULT WINAPI codec_iterator_get_HasCurrent( IIterator_CodecInfo *iface, boolean *value )
{
    struct codec_iterator *impl = impl_from_IIterator_CodecInfo( iface );

    *value = impl->index < impl->vector->count;
    return S_OK;
}

static HRESULT WINAPI codec_iterator_MoveNext( IIterator_CodecInfo *iface, boolean *value )
{
    struct codec_iterator *impl = impl_from_IIterator_CodecInfo( iface );

    if (impl->index < impl->vector->count) impl->index++;
    return codec_iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI codec_iterator_GetMany( IIterator_CodecInfo *iface, UINT32 size, ICodecInfo **items, UINT *count )
{
    struct codec_iterator *impl = impl_from_IIterator_CodecInfo( iface );
    UINT32 n = 0;

    while (n < size && impl->index < impl->vector->count)
        ICodecInfo_AddRef( (items[n++] = impl->vector->items[impl->index++]) );
    *count = n;
    return S_OK;
}

static const struct IIterator_CodecInfoVtbl codec_iterator_vtbl =
{
    codec_iterator_QueryInterface,
    codec_iterator_AddRef,
    codec_iterator_Release,
    /* IInspectable methods */
    codec_iterator_GetIids,
    codec_iterator_GetRuntimeClassName,
    codec_iterator_GetTrustLevel,
    /* IIterator<CodecInfo> methods */
    codec_iterator_get_Current,
    codec_iterator_get_HasCurrent,
    codec_iterator_MoveNext,
    codec_iterator_GetMany,
};

static inline struct codec_vector *impl_from_IVectorView_CodecInfo( IVectorView_CodecInfo *iface )
{
    return CONTAINING_RECORD( iface, struct codec_vector, IVectorView_CodecInfo_iface );
}

static HRESULT WINAPI codec_vector_QueryInterface( IVectorView_CodecInfo *iface, REFIID iid, void **out )
{
    struct codec_vector *impl = impl_from_IVectorView_CodecInfo( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IVectorView_CodecInfo ))
    {
        IInspectable_AddRef( (*out = &impl->IVectorView_CodecInfo_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IIterable_CodecInfo ))
    {
        IInspectable_AddRef( (*out = &impl->IIterable_CodecInfo_iface) );
        return S_OK;
    }
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI codec_vector_AddRef( IVectorView_CodecInfo *iface )
{
    return InterlockedIncrement( &impl_from_IVectorView_CodecInfo( iface )->ref );
}

static ULONG WINAPI codec_vector_Release( IVectorView_CodecInfo *iface )
{
    struct codec_vector *impl = impl_from_IVectorView_CodecInfo( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) ICodecInfo_Release( impl->items[i] );
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI codec_vector_GetIids( IVectorView_CodecInfo *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

/* .NET types the RCW from this string, so it names the collection interface, as Windows does for
 * the IVectorView an async operation hands back */
static HRESULT WINAPI codec_vector_GetRuntimeClassName( IVectorView_CodecInfo *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Foundation.Collections.IVectorView`1<Windows.Media.Core.CodecInfo>";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI codec_vector_GetTrustLevel( IVectorView_CodecInfo *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI codec_vector_GetAt( IVectorView_CodecInfo *iface, UINT32 index, ICodecInfo **value )
{
    struct codec_vector *impl = impl_from_IVectorView_CodecInfo( iface );

    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    ICodecInfo_AddRef( (*value = impl->items[index]) );
    return S_OK;
}

static HRESULT WINAPI codec_vector_get_Size( IVectorView_CodecInfo *iface, UINT32 *value )
{
    *value = impl_from_IVectorView_CodecInfo( iface )->count;
    return S_OK;
}

static HRESULT WINAPI codec_vector_IndexOf( IVectorView_CodecInfo *iface, ICodecInfo *element, UINT32 *index, BOOLEAN *found )
{
    struct codec_vector *impl = impl_from_IVectorView_CodecInfo( iface );
    UINT32 i;

    for (i = 0; i < impl->count; i++)
    {
        if (impl->items[i] == element)
        {
            *index = i;
            *found = TRUE;
            return S_OK;
        }
    }
    *index = 0;
    *found = FALSE;
    return S_OK;
}

static HRESULT WINAPI codec_vector_GetMany( IVectorView_CodecInfo *iface, UINT32 start, UINT32 size,
                                            ICodecInfo **items, UINT *count )
{
    struct codec_vector *impl = impl_from_IVectorView_CodecInfo( iface );
    UINT32 n = 0;

    for (; start < impl->count && n < size; start++)
        ICodecInfo_AddRef( (items[n++] = impl->items[start]) );
    *count = n;
    return S_OK;
}

static const struct IVectorView_CodecInfoVtbl codec_vector_vtbl =
{
    codec_vector_QueryInterface,
    codec_vector_AddRef,
    codec_vector_Release,
    /* IInspectable methods */
    codec_vector_GetIids,
    codec_vector_GetRuntimeClassName,
    codec_vector_GetTrustLevel,
    /* IVectorView<CodecInfo> methods */
    codec_vector_GetAt,
    codec_vector_get_Size,
    codec_vector_IndexOf,
    codec_vector_GetMany,
};

DEFINE_IINSPECTABLE( codec_iterable, IIterable_CodecInfo, struct codec_vector, IVectorView_CodecInfo_iface )

static HRESULT WINAPI codec_iterable_First( IIterable_CodecInfo *iface, IIterator_CodecInfo **value )
{
    struct codec_vector *impl = impl_from_IIterable_CodecInfo( iface );
    struct codec_iterator *iter;

    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_CodecInfo_iface.lpVtbl = &codec_iterator_vtbl;
    iter->ref = 1;
    IVectorView_CodecInfo_AddRef( &impl->IVectorView_CodecInfo_iface );
    iter->vector = impl;
    *value = &iter->IIterator_CodecInfo_iface;
    return S_OK;
}

static const struct IIterable_CodecInfoVtbl codec_iterable_vtbl =
{
    codec_iterable_QueryInterface,
    codec_iterable_AddRef,
    codec_iterable_Release,
    /* IInspectable methods */
    codec_iterable_GetIids,
    codec_iterable_GetRuntimeClassName,
    codec_iterable_GetTrustLevel,
    /* IIterable<CodecInfo> methods */
    codec_iterable_First,
};

/*
 * Enumeration
 */

static HRESULT subtype_string( const GUID *guid, HSTRING *out )
{
    WCHAR buffer[39];

    /* the form CodecSubtypes hands out: braces, upper case */
    swprintf( buffer, ARRAY_SIZE(buffer), L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              guid->Data1, guid->Data2, guid->Data3, guid->Data4[0], guid->Data4[1], guid->Data4[2],
              guid->Data4[3], guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7] );
    return WindowsCreateString( buffer, wcslen( buffer ), out );
}

static BOOL parse_subtype( HSTRING str, GUID *guid )
{
    const WCHAR *buffer = WindowsGetStringRawBuffer( str, NULL );
    WCHAR braced[40];

    if (!buffer || !*buffer) return FALSE;
    if (*buffer != '{')
    {
        swprintf( braced, ARRAY_SIZE(braced), L"{%s}", buffer );
        buffer = braced;
    }
    return SUCCEEDED(IIDFromString( buffer, guid ));
}

static HRESULT codec_info_create( IMFActivate *activate, CodecKind kind, CodecCategory category, ICodecInfo **out )
{
    MFT_REGISTER_TYPE_INFO *types = NULL;
    struct codec_info *impl;
    UINT32 size = 0, count, i, n = 0;
    WCHAR *name = NULL;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ICodecInfo_iface.lpVtbl = &codec_info_vtbl;
    impl->ref = 1;
    impl->kind = kind;
    impl->category = category;

    if (SUCCEEDED(IMFActivate_GetAllocatedString( activate, &MFT_FRIENDLY_NAME_Attribute, &name, &size )))
    {
        WindowsCreateString( name, size, &impl->display_name );
        CoTaskMemFree( name );
    }

    /* a decoder is described by what it takes, an encoder by what it makes */
    size = 0;
    if (FAILED(IMFActivate_GetAllocatedBlob( activate, category == CodecCategory_Decoder ? &MFT_INPUT_TYPES_Attributes
                                             : &MFT_OUTPUT_TYPES_Attributes, (UINT8 **)&types, &size )))
        size = 0;
    count = size / sizeof(*types);

    if (!(impl->subtypes = calloc( 1, offsetof(struct hstring_vector, values[count]) )))
    {
        CoTaskMemFree( types );
        WindowsDeleteString( impl->display_name );
        free( impl );
        return E_OUTOFMEMORY;
    }
    impl->subtypes->IVectorView_HSTRING_iface.lpVtbl = &hstring_vector_vtbl;
    impl->subtypes->IIterable_HSTRING_iface.lpVtbl = &hstring_iterable_vtbl;
    impl->subtypes->ref = 1;
    for (i = 0; i < count; i++)
        if (SUCCEEDED(hr = subtype_string( &types[i].guidSubtype, &impl->subtypes->values[n] ))) n++;
    impl->subtypes->count = n;
    CoTaskMemFree( types );

    TRACE( "%s: %u subtypes.\n", debugstr_hstring( impl->display_name ), n );
    *out = &impl->ICodecInfo_iface;
    return S_OK;
}

static HRESULT find_codecs( CodecKind kind, CodecCategory category, HSTRING subtype, struct codec_vector **out )
{
    static const GUID *const categories[2][2] =
    {
        /* Audio */ { &MFT_CATEGORY_AUDIO_ENCODER, &MFT_CATEGORY_AUDIO_DECODER },
        /* Video */ { &MFT_CATEGORY_VIDEO_ENCODER, &MFT_CATEGORY_VIDEO_DECODER },
    };
    MFT_REGISTER_TYPE_INFO type = {0}, *filter = NULL;
    struct codec_vector *vector;
    IMFActivate **activates = NULL;
    UINT32 count = 0, i;
    HRESULT hr;

    *out = NULL;
    if ((UINT)kind > CodecKind_Video || (UINT)category > CodecCategory_Decoder) return E_INVALIDARG;
    if (!(vector = calloc( 1, sizeof(*vector) ))) return E_OUTOFMEMORY;
    vector->IVectorView_CodecInfo_iface.lpVtbl = &codec_vector_vtbl;
    vector->IIterable_CodecInfo_iface.lpVtbl = &codec_iterable_vtbl;
    vector->ref = 1;

    if (WindowsGetStringLen( subtype ))
    {
        if (!parse_subtype( subtype, &type.guidSubtype ))
        {
            WARN( "unparsable subtype %s.\n", debugstr_hstring( subtype ) );
            *out = vector; /* nothing matches it */
            return S_OK;
        }
        type.guidMajorType = kind == CodecKind_Video ? MFMediaType_Video : MFMediaType_Audio;
        filter = &type;
    }

    MFStartup( MF_VERSION, MFSTARTUP_NOSOCKET );
    hr = MFTEnumEx( *categories[kind][category], MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
                    category == CodecCategory_Decoder ? filter : NULL, category == CodecCategory_Encoder ? filter : NULL, &activates, &count );
    if (SUCCEEDED(hr) && count && !(vector->items = calloc( count, sizeof(*vector->items) ))) hr = E_OUTOFMEMORY;
    for (i = 0; SUCCEEDED(hr) && i < count; i++)
        if (SUCCEEDED(codec_info_create( activates[i], kind, category, &vector->items[vector->count] )))
            vector->count++;
    for (i = 0; i < count; i++) IMFActivate_Release( activates[i] );
    CoTaskMemFree( activates );
    MFShutdown();

    TRACE( "kind %d, category %d, subtype %s: %u codecs, hr %#lx.\n", kind, category,
           debugstr_hstring( subtype ), vector->count, hr );
    if (FAILED(hr))
    {
        /* no MFT enumeration is "no codecs", which the caller can act on; a failed operation is not */
        WARN( "MFTEnumEx failed, hr %#lx.\n", hr );
    }
    *out = vector;
    return S_OK;
}

/*
 * IAsyncOperation<IVectorView<CodecInfo>>, handed back already completed: the query runs when it
 * is started, so .get() finds it Completed and a completion handler is invoked as it is set.
 */

struct codec_query_op
{
    IAsyncOperation_IVectorView_CodecInfo IAsyncOperation_IVectorView_CodecInfo_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    IVectorView_CodecInfo *result;
    IAsyncOperationCompletedHandler_IVectorView_CodecInfo *handler;
    BOOL handler_set;
    CRITICAL_SECTION cs;
};

static inline struct codec_query_op *impl_from_IAsyncOperation_IVectorView_CodecInfo( IAsyncOperation_IVectorView_CodecInfo *iface )
{
    return CONTAINING_RECORD( iface, struct codec_query_op, IAsyncOperation_IVectorView_CodecInfo_iface );
}

static HRESULT WINAPI codec_query_op_QueryInterface( IAsyncOperation_IVectorView_CodecInfo *iface, REFIID iid, void **out )
{
    struct codec_query_op *impl = impl_from_IAsyncOperation_IVectorView_CodecInfo( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncOperation_IVectorView_CodecInfo ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncOperation_IVectorView_CodecInfo_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IInspectable_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI codec_query_op_AddRef( IAsyncOperation_IVectorView_CodecInfo *iface )
{
    return InterlockedIncrement( &impl_from_IAsyncOperation_IVectorView_CodecInfo( iface )->ref );
}

static ULONG WINAPI codec_query_op_Release( IAsyncOperation_IVectorView_CodecInfo *iface )
{
    struct codec_query_op *impl = impl_from_IAsyncOperation_IVectorView_CodecInfo( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->result) IVectorView_CodecInfo_Release( impl->result );
        if (impl->handler) IAsyncOperationCompletedHandler_IVectorView_CodecInfo_Release( impl->handler );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI codec_query_op_GetIids( IAsyncOperation_IVectorView_CodecInfo *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI codec_query_op_GetRuntimeClassName( IAsyncOperation_IVectorView_CodecInfo *iface, HSTRING *name )
{
    static const WCHAR class_name[] =
        L"Windows.Foundation.IAsyncOperation`1<Windows.Foundation.Collections.IVectorView`1<Windows.Media.Core.CodecInfo>>";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI codec_query_op_GetTrustLevel( IAsyncOperation_IVectorView_CodecInfo *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI codec_query_op_put_Completed( IAsyncOperation_IVectorView_CodecInfo *iface,
                                                    IAsyncOperationCompletedHandler_IVectorView_CodecInfo *handler )
{
    struct codec_query_op *impl = impl_from_IAsyncOperation_IVectorView_CodecInfo( iface );

    TRACE( "iface %p, handler %p.\n", iface, handler );

    EnterCriticalSection( &impl->cs );
    if (impl->handler_set)
    {
        LeaveCriticalSection( &impl->cs );
        return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    }
    impl->handler_set = TRUE;
    if (handler) IAsyncOperationCompletedHandler_IVectorView_CodecInfo_AddRef( (impl->handler = handler) );
    LeaveCriticalSection( &impl->cs );

    /* already complete: a handler set now is invoked now */
    if (handler) IAsyncOperationCompletedHandler_IVectorView_CodecInfo_Invoke( handler, iface, Completed );
    return S_OK;
}

static HRESULT WINAPI codec_query_op_get_Completed( IAsyncOperation_IVectorView_CodecInfo *iface,
                                                    IAsyncOperationCompletedHandler_IVectorView_CodecInfo **handler )
{
    struct codec_query_op *impl = impl_from_IAsyncOperation_IVectorView_CodecInfo( iface );

    EnterCriticalSection( &impl->cs );
    if ((*handler = impl->handler)) IAsyncOperationCompletedHandler_IVectorView_CodecInfo_AddRef( *handler );
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI codec_query_op_GetResults( IAsyncOperation_IVectorView_CodecInfo *iface, IVectorView_CodecInfo **results )
{
    struct codec_query_op *impl = impl_from_IAsyncOperation_IVectorView_CodecInfo( iface );

    TRACE( "iface %p, results %p.\n", iface, results );

    IVectorView_CodecInfo_AddRef( (*results = impl->result) );
    return S_OK;
}

static const struct IAsyncOperation_IVectorView_CodecInfoVtbl codec_query_op_vtbl =
{
    codec_query_op_QueryInterface,
    codec_query_op_AddRef,
    codec_query_op_Release,
    /* IInspectable methods */
    codec_query_op_GetIids,
    codec_query_op_GetRuntimeClassName,
    codec_query_op_GetTrustLevel,
    /* IAsyncOperation<IVectorView<CodecInfo>> methods */
    codec_query_op_put_Completed,
    codec_query_op_get_Completed,
    codec_query_op_GetResults,
};

DEFINE_IINSPECTABLE( codec_query_info, IAsyncInfo, struct codec_query_op, IAsyncOperation_IVectorView_CodecInfo_iface )

static HRESULT WINAPI codec_query_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = 1;
    return S_OK;
}

static HRESULT WINAPI codec_query_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    *status = Completed;
    return S_OK;
}

static HRESULT WINAPI codec_query_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error )
{
    *error = S_OK;
    return S_OK;
}

static HRESULT WINAPI codec_query_info_Cancel( IAsyncInfo *iface )
{
    return S_OK;
}

static HRESULT WINAPI codec_query_info_Close( IAsyncInfo *iface )
{
    return S_OK;
}

static const struct IAsyncInfoVtbl codec_query_info_vtbl =
{
    codec_query_info_QueryInterface,
    codec_query_info_AddRef,
    codec_query_info_Release,
    /* IInspectable methods */
    codec_query_info_GetIids,
    codec_query_info_GetRuntimeClassName,
    codec_query_info_GetTrustLevel,
    /* IAsyncInfo methods */
    codec_query_info_get_Id,
    codec_query_info_get_Status,
    codec_query_info_get_ErrorCode,
    codec_query_info_Cancel,
    codec_query_info_Close,
};

/*
 * CodecQuery
 */

struct codec_query
{
    ICodecQuery ICodecQuery_iface;
    LONG ref;
};

static inline struct codec_query *impl_from_ICodecQuery( ICodecQuery *iface )
{
    return CONTAINING_RECORD( iface, struct codec_query, ICodecQuery_iface );
}

static HRESULT WINAPI codec_query_QueryInterface( ICodecQuery *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ICodecQuery ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI codec_query_AddRef( ICodecQuery *iface )
{
    return InterlockedIncrement( &impl_from_ICodecQuery( iface )->ref );
}

static ULONG WINAPI codec_query_Release( ICodecQuery *iface )
{
    struct codec_query *impl = impl_from_ICodecQuery( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI codec_query_GetIids( ICodecQuery *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI codec_query_GetRuntimeClassName( ICodecQuery *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Core_CodecQuery,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Core_CodecQuery) - 1, name );
}

static HRESULT WINAPI codec_query_GetTrustLevel( ICodecQuery *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI codec_query_FindAllAsync( ICodecQuery *iface, CodecKind kind, CodecCategory category,
                                                HSTRING subtype, IAsyncOperation_IVectorView_CodecInfo **value )
{
    struct codec_vector *vector;
    struct codec_query_op *op;
    HRESULT hr;

    TRACE( "iface %p, kind %d, category %d, subtype %s, value %p.\n", iface, kind, category,
           debugstr_hstring( subtype ), value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = find_codecs( kind, category, subtype, &vector ))) return hr;
    if (!(op = calloc( 1, sizeof(*op) )))
    {
        IVectorView_CodecInfo_Release( &vector->IVectorView_CodecInfo_iface );
        return E_OUTOFMEMORY;
    }
    op->IAsyncOperation_IVectorView_CodecInfo_iface.lpVtbl = &codec_query_op_vtbl;
    op->IAsyncInfo_iface.lpVtbl = &codec_query_info_vtbl;
    op->ref = 1;
    op->result = &vector->IVectorView_CodecInfo_iface;
    InitializeCriticalSectionEx( &op->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    op->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": codec_query_op.cs");
    *value = &op->IAsyncOperation_IVectorView_CodecInfo_iface;
    return S_OK;
}

static const struct ICodecQueryVtbl codec_query_vtbl =
{
    codec_query_QueryInterface,
    codec_query_AddRef,
    codec_query_Release,
    /* IInspectable methods */
    codec_query_GetIids,
    codec_query_GetRuntimeClassName,
    codec_query_GetTrustLevel,
    /* ICodecQuery methods */
    codec_query_FindAllAsync,
};

/*
 * Activation factory
 */

static HRESULT WINAPI codec_query_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI codec_query_factory_AddRef( IActivationFactory *iface )
{
    return 2;
}

static ULONG WINAPI codec_query_factory_Release( IActivationFactory *iface )
{
    return 1;
}

static HRESULT WINAPI codec_query_factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI codec_query_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Core_CodecQuery,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Core_CodecQuery) - 1, name );
}

static HRESULT WINAPI codec_query_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI codec_query_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct codec_query *impl;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        *instance = NULL;
        return E_OUTOFMEMORY;
    }
    impl->ICodecQuery_iface.lpVtbl = &codec_query_vtbl;
    impl->ref = 1;
    *instance = (IInspectable *)&impl->ICodecQuery_iface;
    return S_OK;
}

static const struct IActivationFactoryVtbl codec_query_factory_vtbl =
{
    codec_query_factory_QueryInterface,
    codec_query_factory_AddRef,
    codec_query_factory_Release,
    /* IInspectable methods */
    codec_query_factory_GetIids,
    codec_query_factory_GetRuntimeClassName,
    codec_query_factory_GetTrustLevel,
    /* IActivationFactory methods */
    codec_query_factory_ActivateInstance,
};

static IActivationFactory codec_query_factory_instance = { &codec_query_factory_vtbl };
IActivationFactory *codec_query_factory = &codec_query_factory_instance;

/*
 * CodecSubtypes: the subtype strings apps pass to FindAllAsync, formatted from the same Media
 * Foundation GUIDs the enumeration reports, so a query and its result always agree.
 */

static HRESULT WINAPI subtypes_QueryInterface( ICodecSubtypesStatics *iface, REFIID iid, void **out );
static ULONG WINAPI subtypes_AddRef( ICodecSubtypesStatics *iface )
{
    return 2;
}
static ULONG WINAPI subtypes_Release( ICodecSubtypesStatics *iface )
{
    return 1;
}
static HRESULT WINAPI subtypes_GetIids( ICodecSubtypesStatics *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}
static HRESULT WINAPI subtypes_GetRuntimeClassName( ICodecSubtypesStatics *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Media_Core_CodecSubtypes,
                                ARRAY_SIZE(RuntimeClass_Windows_Media_Core_CodecSubtypes) - 1, name );
}
static HRESULT WINAPI subtypes_GetTrustLevel( ICodecSubtypesStatics *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI subtypes_get_VideoFormatDV25( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_DV25, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatDV50( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_DV50, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatDvc( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_DVC, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatDvh1( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_DVH1, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatDvhD( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_DVHD, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatDvsd( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_DVSD, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatDvsl( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_DVSL, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatH263( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_H263, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatH264( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_H264, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatH265( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_H265, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatH264ES( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_H264_ES, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatHevc( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_HEVC, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatHevcES( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_HEVC_ES, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatM4S2( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_M4S2, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatMjpg( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_MJPG, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatMP43( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_MP43, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatMP4S( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_MP4S, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatMP4V( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_MP4V, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatMpeg2( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_MPEG2, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatVP80( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_VP80, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatVP90( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_VP90, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatMpg1( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_MPG1, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatMss1( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_MSS1, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatMss2( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_MSS2, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatWmv1( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_WMV1, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatWmv2( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_WMV2, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatWmv3( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_WMV3, value );
}

static HRESULT WINAPI subtypes_get_VideoFormatWvc1( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_WVC1, value );
}

static HRESULT WINAPI subtypes_get_VideoFormat420O( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_420O, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatAac( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_AAC, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatAdts( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_ADTS, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatAlac( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_ALAC, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatAmrNB( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_AMR_NB, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatAmrWB( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_AMR_WB, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatAmrWP( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_AMR_WP, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatDolbyAC3( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_Dolby_AC3, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatDolbyAC3Spdif( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_Dolby_AC3_SPDIF, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatDolbyDDPlus( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_Dolby_DDPlus, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatDrm( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_DRM, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatDts( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_DTS, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatFlac( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_FLAC, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatFloat( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_Float, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatMP3( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_MP3, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatMPeg( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_MPEG, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatMsp1( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_MSP1, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatOpus( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_Opus, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatPcm( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_PCM, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatWmaSpdif( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_WMASPDIF, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatWMAudioLossless( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_WMAudio_Lossless, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatWMAudioV8( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_WMAudioV8, value );
}

static HRESULT WINAPI subtypes_get_AudioFormatWMAudioV9( ICodecSubtypesStatics *iface, HSTRING *value )
{
    return subtype_string( &MFAudioFormat_WMAudioV9, value );
}

static const struct ICodecSubtypesStaticsVtbl subtypes_vtbl =
{
    subtypes_QueryInterface,
    subtypes_AddRef,
    subtypes_Release,
    /* IInspectable methods */
    subtypes_GetIids,
    subtypes_GetRuntimeClassName,
    subtypes_GetTrustLevel,
    /* ICodecSubtypesStatics methods */
    subtypes_get_VideoFormatDV25,
    subtypes_get_VideoFormatDV50,
    subtypes_get_VideoFormatDvc,
    subtypes_get_VideoFormatDvh1,
    subtypes_get_VideoFormatDvhD,
    subtypes_get_VideoFormatDvsd,
    subtypes_get_VideoFormatDvsl,
    subtypes_get_VideoFormatH263,
    subtypes_get_VideoFormatH264,
    subtypes_get_VideoFormatH265,
    subtypes_get_VideoFormatH264ES,
    subtypes_get_VideoFormatHevc,
    subtypes_get_VideoFormatHevcES,
    subtypes_get_VideoFormatM4S2,
    subtypes_get_VideoFormatMjpg,
    subtypes_get_VideoFormatMP43,
    subtypes_get_VideoFormatMP4S,
    subtypes_get_VideoFormatMP4V,
    subtypes_get_VideoFormatMpeg2,
    subtypes_get_VideoFormatVP80,
    subtypes_get_VideoFormatVP90,
    subtypes_get_VideoFormatMpg1,
    subtypes_get_VideoFormatMss1,
    subtypes_get_VideoFormatMss2,
    subtypes_get_VideoFormatWmv1,
    subtypes_get_VideoFormatWmv2,
    subtypes_get_VideoFormatWmv3,
    subtypes_get_VideoFormatWvc1,
    subtypes_get_VideoFormat420O,
    subtypes_get_AudioFormatAac,
    subtypes_get_AudioFormatAdts,
    subtypes_get_AudioFormatAlac,
    subtypes_get_AudioFormatAmrNB,
    subtypes_get_AudioFormatAmrWB,
    subtypes_get_AudioFormatAmrWP,
    subtypes_get_AudioFormatDolbyAC3,
    subtypes_get_AudioFormatDolbyAC3Spdif,
    subtypes_get_AudioFormatDolbyDDPlus,
    subtypes_get_AudioFormatDrm,
    subtypes_get_AudioFormatDts,
    subtypes_get_AudioFormatFlac,
    subtypes_get_AudioFormatFloat,
    subtypes_get_AudioFormatMP3,
    subtypes_get_AudioFormatMPeg,
    subtypes_get_AudioFormatMsp1,
    subtypes_get_AudioFormatOpus,
    subtypes_get_AudioFormatPcm,
    subtypes_get_AudioFormatWmaSpdif,
    subtypes_get_AudioFormatWMAudioLossless,
    subtypes_get_AudioFormatWMAudioV8,
    subtypes_get_AudioFormatWMAudioV9,
};

static ICodecSubtypesStatics subtypes_statics = { &subtypes_vtbl };

static HRESULT WINAPI subtypes2_QueryInterface( ICodecSubtypesStatics2 *iface, REFIID iid, void **out )
{
    return subtypes_QueryInterface( &subtypes_statics, iid, out );
}
static ULONG WINAPI subtypes2_AddRef( ICodecSubtypesStatics2 *iface )
{
    return 2;
}
static ULONG WINAPI subtypes2_Release( ICodecSubtypesStatics2 *iface )
{
    return 1;
}
static HRESULT WINAPI subtypes2_GetIids( ICodecSubtypesStatics2 *iface, ULONG *count, IID **iids )
{
    return subtypes_GetIids( &subtypes_statics, count, iids );
}
static HRESULT WINAPI subtypes2_GetRuntimeClassName( ICodecSubtypesStatics2 *iface, HSTRING *name )
{
    return subtypes_GetRuntimeClassName( &subtypes_statics, name );
}
static HRESULT WINAPI subtypes2_GetTrustLevel( ICodecSubtypesStatics2 *iface, TrustLevel *level )
{
    return subtypes_GetTrustLevel( &subtypes_statics, level );
}
static HRESULT WINAPI subtypes2_get_VideoFormatAv1( ICodecSubtypesStatics2 *iface, HSTRING *value )
{
    return subtype_string( &MFVideoFormat_AV1, value );
}

static const struct ICodecSubtypesStatics2Vtbl subtypes2_vtbl =
{
    subtypes2_QueryInterface,
    subtypes2_AddRef,
    subtypes2_Release,
    /* IInspectable methods */
    subtypes2_GetIids,
    subtypes2_GetRuntimeClassName,
    subtypes2_GetTrustLevel,
    /* ICodecSubtypesStatics2 methods */
    subtypes2_get_VideoFormatAv1,
};

static ICodecSubtypesStatics2 subtypes2_statics = { &subtypes2_vtbl };

/* the class is static only: its factory is the statics object, answering IActivationFactory too */
static HRESULT WINAPI subtypes_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    if (instance) *instance = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI subtypes_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    return subtypes_QueryInterface( &subtypes_statics, iid, out );
}
static ULONG WINAPI subtypes_factory_AddRef( IActivationFactory *iface )
{
    return 2;
}
static ULONG WINAPI subtypes_factory_Release( IActivationFactory *iface )
{
    return 1;
}
static HRESULT WINAPI subtypes_factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    return subtypes_GetIids( &subtypes_statics, count, iids );
}
static HRESULT WINAPI subtypes_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return subtypes_GetRuntimeClassName( &subtypes_statics, name );
}
static HRESULT WINAPI subtypes_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    return subtypes_GetTrustLevel( &subtypes_statics, level );
}

static const struct IActivationFactoryVtbl subtypes_factory_vtbl =
{
    subtypes_factory_QueryInterface,
    subtypes_factory_AddRef,
    subtypes_factory_Release,
    /* IInspectable methods */
    subtypes_factory_GetIids,
    subtypes_factory_GetRuntimeClassName,
    subtypes_factory_GetTrustLevel,
    /* IActivationFactory methods */
    subtypes_factory_ActivateInstance,
};

static IActivationFactory subtypes_factory_instance = { &subtypes_factory_vtbl };
IActivationFactory *codec_subtypes_factory = &subtypes_factory_instance;

static HRESULT WINAPI subtypes_QueryInterface( ICodecSubtypesStatics *iface, REFIID iid, void **out )
{
    TRACE( "iid %s, out %p.\n", debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &subtypes_factory_instance;
    else if (IsEqualGUID( iid, &IID_ICodecSubtypesStatics ))
        *out = &subtypes_statics;
    else if (IsEqualGUID( iid, &IID_ICodecSubtypesStatics2 ))
        *out = &subtypes2_statics;
    else
    {
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

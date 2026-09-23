/*
 * Windows.Foundation.WwwFormUrlDecoder
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

/* Parses an application/x-www-form-urlencoded query ("?a=1&b=two+words") into name/value
 * entries, exposed as IVectorView<IWwwFormUrlDecoderEntry> and IIterable of the same. A leading
 * '?' is skipped, '+' decodes to a space and %XX sequences are decoded as UTF-8. Apps use it to
 * read the parameters of a protocol activation URI - Shapr3D's sign-in callback among them. */

#include "private.h"
#include "wine/debug.h"

static const WCHAR *find_wchar(const WCHAR *str, WCHAR c, size_t len)
{
    for (; len; len--, str++) if (*str == c) return str;
    return NULL;
}

WINE_DEFAULT_DEBUG_CHANNEL(iertutil);

/* ---------------------------------------------------------------------------------------------
 * WwwFormUrlDecoderEntry
 * ------------------------------------------------------------------------------------------- */
struct entry
{
    IWwwFormUrlDecoderEntry IWwwFormUrlDecoderEntry_iface;
    LONG ref;
    HSTRING name;
    HSTRING value;
};

static inline struct entry *impl_from_IWwwFormUrlDecoderEntry(IWwwFormUrlDecoderEntry *iface)
{
    return CONTAINING_RECORD(iface, struct entry, IWwwFormUrlDecoderEntry_iface);
}

static HRESULT WINAPI entry_QueryInterface(IWwwFormUrlDecoderEntry *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable)
            || IsEqualGUID(iid, &IID_IAgileObject) || IsEqualGUID(iid, &IID_IWwwFormUrlDecoderEntry))
    {
        IWwwFormUrlDecoderEntry_AddRef((*out = iface));
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI entry_AddRef(IWwwFormUrlDecoderEntry *iface)
{
    struct entry *impl = impl_from_IWwwFormUrlDecoderEntry(iface);
    return InterlockedIncrement(&impl->ref);
}

static ULONG WINAPI entry_Release(IWwwFormUrlDecoderEntry *iface)
{
    struct entry *impl = impl_from_IWwwFormUrlDecoderEntry(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);

    if (!ref)
    {
        WindowsDeleteString(impl->name);
        WindowsDeleteString(impl->value);
        free(impl);
    }
    return ref;
}

static HRESULT WINAPI entry_GetIids(IWwwFormUrlDecoderEntry *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT WINAPI entry_GetRuntimeClassName(IWwwFormUrlDecoderEntry *iface, HSTRING *class_name)
{
    static const WCHAR name[] = L"Windows.Foundation.WwwFormUrlDecoderEntry";
    TRACE("iface %p, class_name %p.\n", iface, class_name);
    return WindowsCreateString(name, ARRAY_SIZE(name) - 1, class_name);
}

static HRESULT WINAPI entry_GetTrustLevel(IWwwFormUrlDecoderEntry *iface, TrustLevel *trust_level)
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI entry_get_Name(IWwwFormUrlDecoderEntry *iface, HSTRING *value)
{
    struct entry *impl = impl_from_IWwwFormUrlDecoderEntry(iface);
    TRACE("iface %p, value %p.\n", iface, value);
    return WindowsDuplicateString(impl->name, value);
}

static HRESULT WINAPI entry_get_Value(IWwwFormUrlDecoderEntry *iface, HSTRING *value)
{
    struct entry *impl = impl_from_IWwwFormUrlDecoderEntry(iface);
    TRACE("iface %p, value %p.\n", iface, value);
    return WindowsDuplicateString(impl->value, value);
}

static const IWwwFormUrlDecoderEntryVtbl entry_vtbl =
{
    entry_QueryInterface,
    entry_AddRef,
    entry_Release,
    entry_GetIids,
    entry_GetRuntimeClassName,
    entry_GetTrustLevel,
    entry_get_Name,
    entry_get_Value,
};

/* ---------------------------------------------------------------------------------------------
 * WwwFormUrlDecoder
 * ------------------------------------------------------------------------------------------- */
struct decoder
{
    IWwwFormUrlDecoderRuntimeClass IWwwFormUrlDecoderRuntimeClass_iface;
    IVectorView_IWwwFormUrlDecoderEntry IVectorView_IWwwFormUrlDecoderEntry_iface;
    IIterable_IWwwFormUrlDecoderEntry IIterable_IWwwFormUrlDecoderEntry_iface;
    LONG ref;
    UINT32 count;
    IWwwFormUrlDecoderEntry **entries;
};

static inline struct decoder *impl_from_IWwwFormUrlDecoderRuntimeClass(IWwwFormUrlDecoderRuntimeClass *iface)
{
    return CONTAINING_RECORD(iface, struct decoder, IWwwFormUrlDecoderRuntimeClass_iface);
}

static HRESULT WINAPI decoder_QueryInterface(IWwwFormUrlDecoderRuntimeClass *iface, REFIID iid, void **out)
{
    struct decoder *impl = impl_from_IWwwFormUrlDecoderRuntimeClass(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable)
            || IsEqualGUID(iid, &IID_IAgileObject) || IsEqualGUID(iid, &IID_IWwwFormUrlDecoderRuntimeClass))
        *out = &impl->IWwwFormUrlDecoderRuntimeClass_iface;
    else if (IsEqualGUID(iid, &IID_IVectorView_IWwwFormUrlDecoderEntry))
        *out = &impl->IVectorView_IWwwFormUrlDecoderEntry_iface;
    else if (IsEqualGUID(iid, &IID_IIterable_IWwwFormUrlDecoderEntry))
        *out = &impl->IIterable_IWwwFormUrlDecoderEntry_iface;
    else
    {
        FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI decoder_AddRef(IWwwFormUrlDecoderRuntimeClass *iface)
{
    struct decoder *impl = impl_from_IWwwFormUrlDecoderRuntimeClass(iface);
    return InterlockedIncrement(&impl->ref);
}

static ULONG WINAPI decoder_Release(IWwwFormUrlDecoderRuntimeClass *iface)
{
    struct decoder *impl = impl_from_IWwwFormUrlDecoderRuntimeClass(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IWwwFormUrlDecoderEntry_Release(impl->entries[i]);
        free(impl->entries);
        free(impl);
    }
    return ref;
}

static HRESULT WINAPI decoder_GetIids(IWwwFormUrlDecoderRuntimeClass *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT WINAPI decoder_GetRuntimeClassName(IWwwFormUrlDecoderRuntimeClass *iface, HSTRING *class_name)
{
    static const WCHAR name[] = L"Windows.Foundation.WwwFormUrlDecoder";
    TRACE("iface %p, class_name %p.\n", iface, class_name);
    return WindowsCreateString(name, ARRAY_SIZE(name) - 1, class_name);
}

static HRESULT WINAPI decoder_GetTrustLevel(IWwwFormUrlDecoderRuntimeClass *iface, TrustLevel *trust_level)
{
    *trust_level = BaseTrust;
    return S_OK;
}

/* Windows fails with E_INVALIDARG when no entry has the name. */
static HRESULT WINAPI decoder_GetFirstValueByName(IWwwFormUrlDecoderRuntimeClass *iface, HSTRING name, HSTRING *value)
{
    struct decoder *impl = impl_from_IWwwFormUrlDecoderRuntimeClass(iface);
    struct entry *entry;
    INT32 order;
    UINT32 i;

    TRACE("iface %p, name %s, value %p.\n", iface, debugstr_hstring(name), value);

    for (i = 0; i < impl->count; i++)
    {
        entry = impl_from_IWwwFormUrlDecoderEntry(impl->entries[i]);
        if (SUCCEEDED(WindowsCompareStringOrdinal(entry->name, name, &order)) && !order)
            return WindowsDuplicateString(entry->value, value);
    }
    *value = NULL;
    return E_INVALIDARG;
}

static const IWwwFormUrlDecoderRuntimeClassVtbl decoder_vtbl =
{
    decoder_QueryInterface,
    decoder_AddRef,
    decoder_Release,
    decoder_GetIids,
    decoder_GetRuntimeClassName,
    decoder_GetTrustLevel,
    decoder_GetFirstValueByName,
};

/* IVectorView<IWwwFormUrlDecoderEntry> */
DEFINE_IINSPECTABLE(decoder_view, IVectorView_IWwwFormUrlDecoderEntry, struct decoder, IWwwFormUrlDecoderRuntimeClass_iface)

static HRESULT WINAPI decoder_view_GetAt(IVectorView_IWwwFormUrlDecoderEntry *iface, UINT32 index,
        IWwwFormUrlDecoderEntry **value)
{
    struct decoder *impl = impl_from_IVectorView_IWwwFormUrlDecoderEntry(iface);

    TRACE("iface %p, index %u, value %p.\n", iface, index, value);

    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    IWwwFormUrlDecoderEntry_AddRef((*value = impl->entries[index]));
    return S_OK;
}

static HRESULT WINAPI decoder_view_get_Size(IVectorView_IWwwFormUrlDecoderEntry *iface, UINT32 *value)
{
    struct decoder *impl = impl_from_IVectorView_IWwwFormUrlDecoderEntry(iface);
    TRACE("iface %p, value %p.\n", iface, value);
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI decoder_view_IndexOf(IVectorView_IWwwFormUrlDecoderEntry *iface,
        IWwwFormUrlDecoderEntry *element, UINT32 *index, BOOLEAN *found)
{
    struct decoder *impl = impl_from_IVectorView_IWwwFormUrlDecoderEntry(iface);
    UINT32 i;

    TRACE("iface %p, element %p, index %p, found %p.\n", iface, element, index, found);

    for (i = 0; i < impl->count; i++)
        if (impl->entries[i] == element) break;
    *found = i < impl->count;
    *index = *found ? i : 0;
    return S_OK;
}

static HRESULT WINAPI decoder_view_GetMany(IVectorView_IWwwFormUrlDecoderEntry *iface, UINT32 start,
        UINT32 size, IWwwFormUrlDecoderEntry **items, UINT32 *count)
{
    struct decoder *impl = impl_from_IVectorView_IWwwFormUrlDecoderEntry(iface);
    UINT32 i;

    TRACE("iface %p, start %u, size %u, items %p, count %p.\n", iface, start, size, items, count);

    *count = 0;
    if (start > impl->count) return E_BOUNDS;
    for (i = start; i < impl->count && *count < size; i++)
        IWwwFormUrlDecoderEntry_AddRef((items[(*count)++] = impl->entries[i]));
    return S_OK;
}

static const IVectorView_IWwwFormUrlDecoderEntryVtbl decoder_view_vtbl =
{
    decoder_view_QueryInterface,
    decoder_view_AddRef,
    decoder_view_Release,
    decoder_view_GetIids,
    decoder_view_GetRuntimeClassName,
    decoder_view_GetTrustLevel,
    decoder_view_GetAt,
    decoder_view_get_Size,
    decoder_view_IndexOf,
    decoder_view_GetMany,
};

/* IIterator<IWwwFormUrlDecoderEntry>: holds the decoder and a position in it. */
struct iterator
{
    IIterator_IWwwFormUrlDecoderEntry IIterator_IWwwFormUrlDecoderEntry_iface;
    LONG ref;
    struct decoder *decoder;
    UINT32 index;
};

static inline struct iterator *impl_from_IIterator_IWwwFormUrlDecoderEntry(IIterator_IWwwFormUrlDecoderEntry *iface)
{
    return CONTAINING_RECORD(iface, struct iterator, IIterator_IWwwFormUrlDecoderEntry_iface);
}

static HRESULT WINAPI iterator_QueryInterface(IIterator_IWwwFormUrlDecoderEntry *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable)
            || IsEqualGUID(iid, &IID_IAgileObject) || IsEqualGUID(iid, &IID_IIterator_IWwwFormUrlDecoderEntry))
    {
        IIterator_IWwwFormUrlDecoderEntry_AddRef((*out = iface));
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI iterator_AddRef(IIterator_IWwwFormUrlDecoderEntry *iface)
{
    struct iterator *impl = impl_from_IIterator_IWwwFormUrlDecoderEntry(iface);
    return InterlockedIncrement(&impl->ref);
}

static ULONG WINAPI iterator_Release(IIterator_IWwwFormUrlDecoderEntry *iface)
{
    struct iterator *impl = impl_from_IIterator_IWwwFormUrlDecoderEntry(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);

    if (!ref)
    {
        IWwwFormUrlDecoderRuntimeClass_Release(&impl->decoder->IWwwFormUrlDecoderRuntimeClass_iface);
        free(impl);
    }
    return ref;
}

static HRESULT WINAPI iterator_GetIids(IIterator_IWwwFormUrlDecoderEntry *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT WINAPI iterator_GetRuntimeClassName(IIterator_IWwwFormUrlDecoderEntry *iface, HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT WINAPI iterator_GetTrustLevel(IIterator_IWwwFormUrlDecoderEntry *iface, TrustLevel *trust_level)
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI iterator_get_Current(IIterator_IWwwFormUrlDecoderEntry *iface, IWwwFormUrlDecoderEntry **value)
{
    struct iterator *impl = impl_from_IIterator_IWwwFormUrlDecoderEntry(iface);
    TRACE("iface %p, value %p.\n", iface, value);
    return IVectorView_IWwwFormUrlDecoderEntry_GetAt(&impl->decoder->IVectorView_IWwwFormUrlDecoderEntry_iface,
                                                     impl->index, value);
}

static HRESULT WINAPI iterator_get_HasCurrent(IIterator_IWwwFormUrlDecoderEntry *iface, BOOLEAN *value)
{
    struct iterator *impl = impl_from_IIterator_IWwwFormUrlDecoderEntry(iface);
    TRACE("iface %p, value %p.\n", iface, value);
    *value = impl->index < impl->decoder->count;
    return S_OK;
}

static HRESULT WINAPI iterator_MoveNext(IIterator_IWwwFormUrlDecoderEntry *iface, BOOLEAN *value)
{
    struct iterator *impl = impl_from_IIterator_IWwwFormUrlDecoderEntry(iface);
    TRACE("iface %p, value %p.\n", iface, value);
    if (impl->index < impl->decoder->count) impl->index++;
    return iterator_get_HasCurrent(iface, value);
}

static HRESULT WINAPI iterator_GetMany(IIterator_IWwwFormUrlDecoderEntry *iface, UINT32 size,
        IWwwFormUrlDecoderEntry **items, UINT32 *count)
{
    struct iterator *impl = impl_from_IIterator_IWwwFormUrlDecoderEntry(iface);
    TRACE("iface %p, size %u, items %p, count %p.\n", iface, size, items, count);
    return IVectorView_IWwwFormUrlDecoderEntry_GetMany(&impl->decoder->IVectorView_IWwwFormUrlDecoderEntry_iface,
                                                       impl->index, size, items, count);
}

static const IIterator_IWwwFormUrlDecoderEntryVtbl iterator_vtbl =
{
    iterator_QueryInterface,
    iterator_AddRef,
    iterator_Release,
    iterator_GetIids,
    iterator_GetRuntimeClassName,
    iterator_GetTrustLevel,
    iterator_get_Current,
    iterator_get_HasCurrent,
    iterator_MoveNext,
    iterator_GetMany,
};

/* IIterable<IWwwFormUrlDecoderEntry> */
DEFINE_IINSPECTABLE(decoder_iterable, IIterable_IWwwFormUrlDecoderEntry, struct decoder, IWwwFormUrlDecoderRuntimeClass_iface)

static HRESULT WINAPI decoder_iterable_First(IIterable_IWwwFormUrlDecoderEntry *iface,
        IIterator_IWwwFormUrlDecoderEntry **value)
{
    struct decoder *impl = impl_from_IIterable_IWwwFormUrlDecoderEntry(iface);
    struct iterator *iter;

    TRACE("iface %p, value %p.\n", iface, value);

    if (!(iter = calloc(1, sizeof(*iter)))) return E_OUTOFMEMORY;
    iter->IIterator_IWwwFormUrlDecoderEntry_iface.lpVtbl = &iterator_vtbl;
    iter->ref = 1;
    iter->decoder = impl;
    IWwwFormUrlDecoderRuntimeClass_AddRef(&impl->IWwwFormUrlDecoderRuntimeClass_iface);
    *value = &iter->IIterator_IWwwFormUrlDecoderEntry_iface;
    return S_OK;
}

static const IIterable_IWwwFormUrlDecoderEntryVtbl decoder_iterable_vtbl =
{
    decoder_iterable_QueryInterface,
    decoder_iterable_AddRef,
    decoder_iterable_Release,
    decoder_iterable_GetIids,
    decoder_iterable_GetRuntimeClassName,
    decoder_iterable_GetTrustLevel,
    decoder_iterable_First,
};

/* ---------------------------------------------------------------------------------------------
 * Parsing
 * ------------------------------------------------------------------------------------------- */
static int hex_digit(WCHAR c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode one name or value: '+' is a space, %XX is a byte, and the bytes are UTF-8. A '%' not
 * followed by two hex digits is kept as it is. */
static HRESULT decode_component(const WCHAR *str, UINT32 len, HSTRING *out)
{
    char *bytes, *p;
    WCHAR *wide;
    UINT32 i;
    int hi, lo, n, count;
    HRESULT hr;

    /* Each WCHAR is at most 3 UTF-8 bytes (a surrogate pair is 2 WCHARs for 4 bytes). */
    if (!(bytes = malloc(len * 3 + 1))) return E_OUTOFMEMORY;
    p = bytes;
    for (i = 0; i < len; i++)
    {
        if (str[i] == '+') *p++ = ' ';
        else if (str[i] == '%' && i + 2 < len
                 && (hi = hex_digit(str[i + 1])) >= 0 && (lo = hex_digit(str[i + 2])) >= 0)
        {
            *p++ = (char)(hi << 4 | lo);
            i += 2;
        }
        else
        {
            n = WideCharToMultiByte(CP_UTF8, 0, str + i, (IS_HIGH_SURROGATE(str[i]) && i + 1 < len) ? 2 : 1,
                                    p, 4, NULL, NULL);
            if (IS_HIGH_SURROGATE(str[i]) && i + 1 < len) i++;
            p += n;
        }
    }

    count = MultiByteToWideChar(CP_UTF8, 0, bytes, p - bytes, NULL, 0);
    if (!(wide = malloc((count + 1) * sizeof(WCHAR))))
    {
        free(bytes);
        return E_OUTOFMEMORY;
    }
    MultiByteToWideChar(CP_UTF8, 0, bytes, p - bytes, wide, count);
    hr = WindowsCreateString(wide, count, out);
    free(wide);
    free(bytes);
    return hr;
}

static HRESULT add_entry(struct decoder *impl, const WCHAR *pair, UINT32 len)
{
    const WCHAR *eq = find_wchar(pair, '=', len);
    UINT32 name_len = eq ? eq - pair : len;
    IWwwFormUrlDecoderEntry **entries;
    struct entry *entry;
    HRESULT hr;

    if (!(entries = realloc(impl->entries, (impl->count + 1) * sizeof(*entries)))) return E_OUTOFMEMORY;
    impl->entries = entries;
    if (!(entry = calloc(1, sizeof(*entry)))) return E_OUTOFMEMORY;
    entry->IWwwFormUrlDecoderEntry_iface.lpVtbl = &entry_vtbl;
    entry->ref = 1;

    if (FAILED(hr = decode_component(pair, name_len, &entry->name))
            || FAILED(hr = eq ? decode_component(eq + 1, len - name_len - 1, &entry->value) : S_OK))
    {
        IWwwFormUrlDecoderEntry_Release(&entry->IWwwFormUrlDecoderEntry_iface);
        return hr;
    }
    impl->entries[impl->count++] = &entry->IWwwFormUrlDecoderEntry_iface;
    return S_OK;
}

HRESULT create_www_form_url_decoder(HSTRING query, IWwwFormUrlDecoderRuntimeClass **out)
{
    const WCHAR *str, *end, *amp;
    struct decoder *impl;
    UINT32 len;
    HRESULT hr = S_OK;

    *out = NULL;
    if (!(impl = calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    impl->IWwwFormUrlDecoderRuntimeClass_iface.lpVtbl = &decoder_vtbl;
    impl->IVectorView_IWwwFormUrlDecoderEntry_iface.lpVtbl = &decoder_view_vtbl;
    impl->IIterable_IWwwFormUrlDecoderEntry_iface.lpVtbl = &decoder_iterable_vtbl;
    impl->ref = 1;

    str = WindowsGetStringRawBuffer(query, &len);
    end = str + len;
    if (str < end && *str == '?') str++;
    while (str < end && SUCCEEDED(hr))
    {
        if (!(amp = find_wchar(str, '&', end - str))) amp = end;
        if (amp > str) hr = add_entry(impl, str, amp - str);
        str = amp + 1;
    }

    if (FAILED(hr))
    {
        IWwwFormUrlDecoderRuntimeClass_Release(&impl->IWwwFormUrlDecoderRuntimeClass_iface);
        return hr;
    }
    TRACE("%s -> %u entries\n", debugstr_hstring(query), impl->count);
    *out = &impl->IWwwFormUrlDecoderRuntimeClass_iface;
    return S_OK;
}

/* ---------------------------------------------------------------------------------------------
 * The activation factory
 * ------------------------------------------------------------------------------------------- */
struct decoder_statics
{
    IActivationFactory IActivationFactory_iface;
    IWwwFormUrlDecoderRuntimeClassFactory IWwwFormUrlDecoderRuntimeClassFactory_iface;
};

static inline struct decoder_statics *impl_from_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct decoder_statics, IActivationFactory_iface);
}

static HRESULT WINAPI factory_QueryInterface(IActivationFactory *iface, REFIID iid, void **out)
{
    struct decoder_statics *impl = impl_from_IActivationFactory(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable)
            || IsEqualGUID(iid, &IID_IAgileObject) || IsEqualGUID(iid, &IID_IActivationFactory))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID(iid, &IID_IWwwFormUrlDecoderRuntimeClassFactory))
        *out = &impl->IWwwFormUrlDecoderRuntimeClassFactory_iface;
    else
    {
        FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static ULONG WINAPI factory_AddRef(IActivationFactory *iface) { return 2; }
static ULONG WINAPI factory_Release(IActivationFactory *iface) { return 1; }

static HRESULT WINAPI factory_GetIids(IActivationFactory *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName(IActivationFactory *iface, HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel(IActivationFactory *iface, TrustLevel *trust_level)
{
    *trust_level = BaseTrust;
    return S_OK;
}

/* The class has no default constructor. */
static HRESULT WINAPI factory_ActivateInstance(IActivationFactory *iface, IInspectable **instance)
{
    TRACE("iface %p, instance %p.\n", iface, instance);
    *instance = NULL;
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE(decoder_factory, IWwwFormUrlDecoderRuntimeClassFactory, struct decoder_statics, IActivationFactory_iface)

static HRESULT WINAPI decoder_factory_CreateWwwFormUrlDecoder(IWwwFormUrlDecoderRuntimeClassFactory *iface,
        HSTRING query, IWwwFormUrlDecoderRuntimeClass **instance)
{
    TRACE("iface %p, query %s, instance %p.\n", iface, debugstr_hstring(query), instance);
    return create_www_form_url_decoder(query, instance);
}

static const IWwwFormUrlDecoderRuntimeClassFactoryVtbl decoder_factory_vtbl =
{
    decoder_factory_QueryInterface,
    decoder_factory_AddRef,
    decoder_factory_Release,
    decoder_factory_GetIids,
    decoder_factory_GetRuntimeClassName,
    decoder_factory_GetTrustLevel,
    decoder_factory_CreateWwwFormUrlDecoder,
};

static struct decoder_statics decoder_statics =
{
    {&factory_vtbl},
    {&decoder_factory_vtbl},
};

IActivationFactory *www_form_url_decoder_factory = &decoder_statics.IActivationFactory_iface;

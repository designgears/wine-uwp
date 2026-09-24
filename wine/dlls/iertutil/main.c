/*
 * Copyright 2024-2026 Zhiyi Zhang for CodeWeavers
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
#include "iertutil_classes.h"

WINE_DEFAULT_DEBUG_CHANNEL(iertutil);

struct uri
{
    IUriRuntimeClass IUriRuntimeClass_iface;
    IUri *uri;
    LONG ref;
};

static inline struct uri *impl_from_IUriRuntimeClass(IUriRuntimeClass *iface)
{
    return CONTAINING_RECORD(iface, struct uri, IUriRuntimeClass_iface);
}

static HRESULT STDMETHODCALLTYPE uri_QueryInterface(IUriRuntimeClass *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p stub!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
        || IsEqualGUID(iid, &IID_IInspectable)
        || IsEqualGUID(iid, &IID_IUriRuntimeClass))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE uri_AddRef(IUriRuntimeClass *iface)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);
    ULONG ref = InterlockedIncrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE uri_Release(IUriRuntimeClass *iface)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        IUri_Release(impl->uri);
        free(impl);
    }

    return ref;
}

static HRESULT STDMETHODCALLTYPE uri_GetIids(IUriRuntimeClass *iface, ULONG *iid_count,
        IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE uri_GetRuntimeClassName(IUriRuntimeClass *iface,
                                                         HSTRING *class_name)
{
    static const WCHAR name[] = L"Windows.Foundation.Uri";

    TRACE("iface %p, class_name %p.\n", iface, class_name);
    if (!class_name) return E_POINTER;
    return WindowsCreateString(name, ARRAY_SIZE(name) - 1, class_name);
}

static HRESULT STDMETHODCALLTYPE uri_GetTrustLevel(IUriRuntimeClass *iface, TrustLevel *trust_level)
{
    TRACE("iface %p, trust_level %p.\n", iface, trust_level);
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT uri_prop_to_hstring(IUri *uri, Uri_PROPERTY prop, HSTRING *out)
{
    BSTR bstr = NULL;
    HRESULT hr;

    hr = IUri_GetPropertyBSTR(uri, prop, &bstr, 0);
    if (FAILED(hr))
    {
        *out = NULL;
        return hr;
    }

    if (hr == S_FALSE || !SysStringLen(bstr))
    {
        SysFreeString(bstr);
        *out = NULL;
        return S_OK;
    }

    hr = WindowsCreateString(bstr, SysStringLen(bstr), out);
    SysFreeString(bstr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE uri_AbsoluteUri(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_ABSOLUTE_URI, value);
}

static HRESULT STDMETHODCALLTYPE uri_DisplayUri(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_DISPLAY_URI, value);
}

static HRESULT STDMETHODCALLTYPE uri_Domain(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_DOMAIN, value);
}

static HRESULT STDMETHODCALLTYPE uri_Extension(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_EXTENSION, value);
}

static HRESULT STDMETHODCALLTYPE uri_Fragment(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_FRAGMENT, value);
}

static HRESULT STDMETHODCALLTYPE uri_Host(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_HOST, value);
}

static HRESULT STDMETHODCALLTYPE uri_Password(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_PASSWORD, value);
}

static HRESULT STDMETHODCALLTYPE uri_Path(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_PATH, value);
}

static HRESULT STDMETHODCALLTYPE uri_Query(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_QUERY, value);
}

static HRESULT STDMETHODCALLTYPE uri_QueryParsed(IUriRuntimeClass *iface,
                                                 IWwwFormUrlDecoderRuntimeClass **decoder)
{
    HSTRING query;
    HRESULT hr;

    TRACE("iface %p, decoder %p.\n", iface, decoder);

    if (FAILED(hr = IUriRuntimeClass_get_Query(iface, &query))) return hr;
    hr = create_www_form_url_decoder(query, decoder);
    WindowsDeleteString(query);
    return hr;
}

static HRESULT STDMETHODCALLTYPE uri_RawUri(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_RAW_URI, value);
}

static HRESULT STDMETHODCALLTYPE uri_SchemeName(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_SCHEME_NAME, value);
}

static HRESULT STDMETHODCALLTYPE uri_UserName(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return uri_prop_to_hstring(impl->uri, Uri_PROPERTY_USER_NAME, value);
}

static HRESULT STDMETHODCALLTYPE uri_Port(IUriRuntimeClass *iface, INT32 *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);
    DWORD port;
    HRESULT hr;

    TRACE("iface %p, value %p.\n", iface, value);

    hr = IUri_GetPropertyDWORD(impl->uri, Uri_PROPERTY_PORT, &port, 0);
    if (hr == S_OK)
    {
        *value = port;
        return S_OK;
    }

    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE uri_Suspicious(IUriRuntimeClass *iface, boolean *value)
{
    FIXME("iface %p, value %p stub!\n", iface, value);
    return E_NOTIMPL;
}

static const struct IUriRuntimeClassVtbl uri_vtbl;
HRESULT WINAPI PrivateCoInternetCombineIUri(IUri *base, IUri *relative, DWORD flags, IUri **result, DWORD_PTR reserved);

/* Wraps an IUri, taking over the caller's reference. */
static HRESULT uri_object_create(IUri *uri, IUriRuntimeClass **instance)
{
    struct uri *uri_impl;

    if (!(uri_impl = calloc(1, sizeof(*uri_impl))))
    {
        IUri_Release(uri);
        *instance = NULL;
        return E_OUTOFMEMORY;
    }
    uri_impl->IUriRuntimeClass_iface.lpVtbl = &uri_vtbl;
    uri_impl->uri = uri;
    uri_impl->ref = 1;
    *instance = &uri_impl->IUriRuntimeClass_iface;
    return S_OK;
}

/* Resolves a relative reference against a base, as Uri(base, relative) and CombineUri do. Shapr3D
 * builds its project-upload URLs this way ("?includeUpload=false&..."). */
static HRESULT combine(IUri *base, HSTRING relative_uri, IUriRuntimeClass **instance)
{
    IUri *relative, *combined;
    HRESULT hr;

    if (!instance) return E_POINTER;
    *instance = NULL;
    if (FAILED(hr = CreateUri(WindowsGetStringRawBuffer(relative_uri, NULL),
                              Uri_CREATE_ALLOW_RELATIVE | Uri_CREATE_NO_DECODE_EXTRA_INFO, 0, &relative)))
        return E_INVALIDARG;
    hr = PrivateCoInternetCombineIUri(base, relative, 0, &combined, 0);
    IUri_Release(relative);
    if (FAILED(hr)) return E_INVALIDARG;
    return uri_object_create(combined, instance);
}

static HRESULT STDMETHODCALLTYPE uri_Equals(IUriRuntimeClass *iface, IUriRuntimeClass *uri,
                                            boolean *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);
    BOOL equal = FALSE;
    HRESULT hr;

    TRACE("iface %p, uri %p, value %p.\n", iface, uri, value);

    if (!value) return E_POINTER;
    if (!uri || uri->lpVtbl != &uri_vtbl)
    {
        *value = FALSE;
        return S_OK;
    }
    hr = IUri_IsEqual(impl->uri, impl_from_IUriRuntimeClass(uri)->uri, &equal);
    *value = SUCCEEDED(hr) && equal;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE uri_CombineUri(IUriRuntimeClass *iface, HSTRING relative_uri,
                                                IUriRuntimeClass **instance)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, relative_uri %s, instance %p.\n", iface, debugstr_hstring(relative_uri), instance);
    return combine(impl->uri, relative_uri, instance);
}

static const struct IUriRuntimeClassVtbl uri_vtbl =
{
    uri_QueryInterface,
    uri_AddRef,
    uri_Release,
    /* IInspectable methods */
    uri_GetIids,
    uri_GetRuntimeClassName,
    uri_GetTrustLevel,
    /* IUriRuntimeClass methods */
    uri_AbsoluteUri,
    uri_DisplayUri,
    uri_Domain,
    uri_Extension,
    uri_Fragment,
    uri_Host,
    uri_Password,
    uri_Path,
    uri_Query,
    uri_QueryParsed,
    uri_RawUri,
    uri_SchemeName,
    uri_UserName,
    uri_Port,
    uri_Suspicious,
    uri_Equals,
    uri_CombineUri,
};

struct iertutil
{
    IActivationFactory IActivationFactory_iface;
    IUriRuntimeClassFactory IUriRuntimeClassFactory_iface;
    IUriEscapeStatics IUriEscapeStatics_iface;
    LONG ref;
};

static inline struct iertutil *impl_from_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct iertutil, IActivationFactory_iface);
}

static HRESULT STDMETHODCALLTYPE iertutil_QueryInterface(IActivationFactory *iface, REFIID iid,
                                                         void **out)
{
    struct iertutil *impl = impl_from_IActivationFactory(iface);

    TRACE("iface %p, iid %s, out %p stub!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
        || IsEqualGUID(iid, &IID_IInspectable)
        || IsEqualGUID(iid, &IID_IAgileObject)
        || IsEqualGUID(iid, &IID_IActivationFactory))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IUriRuntimeClassFactory))
    {
        IUriRuntimeClassFactory_AddRef(&impl->IUriRuntimeClassFactory_iface);
        *out = &impl->IUriRuntimeClassFactory_iface;
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IUriEscapeStatics))
    {
        IUriEscapeStatics_AddRef(&impl->IUriEscapeStatics_iface);
        *out = &impl->IUriEscapeStatics_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE iertutil_AddRef(IActivationFactory *iface)
{
    struct iertutil *impl = impl_from_IActivationFactory(iface);
    ULONG ref = InterlockedIncrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE iertutil_Release(IActivationFactory *iface)
{
    struct iertutil *impl = impl_from_IActivationFactory(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static HRESULT STDMETHODCALLTYPE iertutil_GetIids(IActivationFactory *iface, ULONG *iid_count,
                                                  IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE iertutil_GetRuntimeClassName(IActivationFactory *iface,
                                                              HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE iertutil_GetTrustLevel(IActivationFactory *iface,
                                                        TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE iertutil_ActivateInstance(IActivationFactory *iface,
                                                           IInspectable **instance)
{
    FIXME("iface %p, instance %p stub!\n", iface, instance);
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl activation_factory_vtbl =
{
    iertutil_QueryInterface,
    iertutil_AddRef,
    iertutil_Release,
    /* IInspectable methods */
    iertutil_GetIids,
    iertutil_GetRuntimeClassName,
    iertutil_GetTrustLevel,
    /* IActivationFactory methods */
    iertutil_ActivateInstance,
};

DEFINE_IINSPECTABLE(uri_factory, IUriRuntimeClassFactory, struct iertutil, IActivationFactory_iface)

static HRESULT STDMETHODCALLTYPE uri_factory_CreateUri(IUriRuntimeClassFactory *iface,
                                                       HSTRING uri_string,
                                                       IUriRuntimeClass **instance)
{
    const WCHAR *raw_buffer;
    struct uri *uri_impl;
    HRESULT hr;
    IUri *uri;

    TRACE("iface %p, uri_string %s, instance %p.\n", iface, debugstr_hstring(uri_string), instance);

    if (!uri_string)
        return E_POINTER;

    raw_buffer = WindowsGetStringRawBuffer(uri_string, NULL);
    hr = CreateUri(raw_buffer, Uri_CREATE_ALLOW_IMPLICIT_FILE_SCHEME | Uri_CREATE_NO_DECODE_EXTRA_INFO, 0, &uri);
    if (FAILED(hr))
    {
        WARN("CreateUri(%s) failed, hr %#lx.\n", debugstr_w(raw_buffer), hr);
        *instance = NULL;
        return E_INVALIDARG;
    }

    uri_impl = calloc(1, sizeof(*uri_impl));
    if (!uri_impl)
    {
        IUri_Release(uri);
        return E_OUTOFMEMORY;
    }

    uri_impl->IUriRuntimeClass_iface.lpVtbl = &uri_vtbl;
    uri_impl->uri = uri;
    uri_impl->ref = 1;

    *instance = &uri_impl->IUriRuntimeClass_iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE uri_factory_CreateWithRelativeUri(IUriRuntimeClassFactory *iface,
                                                                   HSTRING base_uri,
                                                                   HSTRING relative_uri,
                                                                   IUriRuntimeClass **instance)
{
    IUri *base;
    HRESULT hr;

    TRACE("iface %p, base_uri %s, relative_uri %s, instance %p.\n", iface,
          debugstr_hstring(base_uri), debugstr_hstring(relative_uri), instance);

    if (!instance) return E_POINTER;
    *instance = NULL;
    if (FAILED(CreateUri(WindowsGetStringRawBuffer(base_uri, NULL),
                         Uri_CREATE_ALLOW_IMPLICIT_FILE_SCHEME | Uri_CREATE_NO_DECODE_EXTRA_INFO, 0, &base)))
        return E_INVALIDARG;
    hr = combine(base, relative_uri, instance);
    IUri_Release(base);
    return hr;
}

static const struct IUriRuntimeClassFactoryVtbl uri_factory_vtbl =
{
    uri_factory_QueryInterface,
    uri_factory_AddRef,
    uri_factory_Release,
    /* IInspectable methods */
    uri_factory_GetIids,
    uri_factory_GetRuntimeClassName,
    uri_factory_GetTrustLevel,
    /* IUriRuntimeClassFactory methods */
    uri_factory_CreateUri,
    uri_factory_CreateWithRelativeUri,
};

/* IUriEscapeStatics. Shapr3D's HTTP layer escapes every query component through
 * Uri.EscapeComponent; the refusal came back from C++/WinRT's .as<>() as a throw, logged as
 * "failed to build request", and the request that creates a new project was never sent. */
DEFINE_IINSPECTABLE(uri_escape, IUriEscapeStatics, struct iertutil, IActivationFactory_iface)

/* RFC 3986's unreserved characters; everything else is percent-encoded as UTF-8. */
static BOOL is_unreserved(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

static HRESULT STDMETHODCALLTYPE uri_escape_EscapeComponent(IUriEscapeStatics *iface, HSTRING to_escape,
                                                            HSTRING *value)
{
    static const char hex[] = "0123456789ABCDEF";
    UINT32 len;
    const WCHAR *str = WindowsGetStringRawBuffer(to_escape, &len);
    int size = len ? WideCharToMultiByte(CP_UTF8, 0, str, len, NULL, 0, NULL, NULL) : 0;
    char *utf8;
    WCHAR *out, *p;
    HRESULT hr;
    int i;

    TRACE("iface %p, to_escape %s, value %p.\n", iface, debugstr_hstring(to_escape), value);

    if (!value) return E_POINTER;
    if (!(utf8 = malloc(size + 1)) || !(out = malloc((size * 3 + 1) * sizeof(WCHAR))))
    {
        free(utf8);
        return E_OUTOFMEMORY;
    }
    WideCharToMultiByte(CP_UTF8, 0, str, len, utf8, size, NULL, NULL);
    for (i = 0, p = out; i < size; i++)
    {
        unsigned char c = utf8[i];
        if (is_unreserved(c)) *p++ = c;
        else
        {
            *p++ = '%';
            *p++ = hex[c >> 4];
            *p++ = hex[c & 0xf];
        }
    }
    hr = WindowsCreateString(out, p - out, value);
    free(out);
    free(utf8);
    return hr;
}

static int hex_value(WCHAR c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* %XX sequences become bytes, read as UTF-8; anything else, '+' included, is kept as it is. */
static HRESULT STDMETHODCALLTYPE uri_escape_UnescapeComponent(IUriEscapeStatics *iface, HSTRING to_unescape,
                                                              HSTRING *value)
{
    UINT32 len, i;
    const WCHAR *str = WindowsGetStringRawBuffer(to_unescape, &len);
    char *bytes, *p;
    WCHAR *out;
    int hi, lo, count;
    HRESULT hr;

    TRACE("iface %p, to_unescape %s, value %p.\n", iface, debugstr_hstring(to_unescape), value);

    if (!value) return E_POINTER;
    if (!(bytes = malloc(len * 3 + 1))) return E_OUTOFMEMORY;
    for (i = 0, p = bytes; i < len; i++)
    {
        if (str[i] == '%' && i + 2 < len && (hi = hex_value(str[i + 1])) >= 0 && (lo = hex_value(str[i + 2])) >= 0)
        {
            *p++ = (char)(hi << 4 | lo);
            i += 2;
        }
        else
        {
            int n = IS_HIGH_SURROGATE(str[i]) && i + 1 < len ? 2 : 1;
            p += WideCharToMultiByte(CP_UTF8, 0, str + i, n, p, 4, NULL, NULL);
            i += n - 1;
        }
    }
    count = MultiByteToWideChar(CP_UTF8, 0, bytes, p - bytes, NULL, 0);
    if (!(out = malloc((count + 1) * sizeof(WCHAR))))
    {
        free(bytes);
        return E_OUTOFMEMORY;
    }
    MultiByteToWideChar(CP_UTF8, 0, bytes, p - bytes, out, count);
    hr = WindowsCreateString(out, count, value);
    free(out);
    free(bytes);
    return hr;
}

static const struct IUriEscapeStaticsVtbl uri_escape_vtbl =
{
    uri_escape_QueryInterface,
    uri_escape_AddRef,
    uri_escape_Release,
    /* IInspectable methods */
    uri_escape_GetIids,
    uri_escape_GetRuntimeClassName,
    uri_escape_GetTrustLevel,
    /* IUriEscapeStatics methods */
    uri_escape_UnescapeComponent,
    uri_escape_EscapeComponent,
};

static struct iertutil iertutil =
{
    {&activation_factory_vtbl},
    {&uri_factory_vtbl},
    {&uri_escape_vtbl},
    1
};

typedef struct
{
    IClassFactory IClassFactory_iface;
    HRESULT (*pfnCreateInstance)(IUnknown *pUnkOuter, LPVOID *ppObj);
} ClassFactory;

static inline ClassFactory *impl_from_IClassFactory(IClassFactory *iface)
{
    return CONTAINING_RECORD(iface, ClassFactory, IClassFactory_iface);
}

static HRESULT WINAPI CF_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    *ppv = NULL;

    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IClassFactory))
    {
        *ppv = iface;
        return S_OK;
    }

    WARN("(%p)->(%s %p)\n", iface, debugstr_guid(riid), ppv);
    return E_NOINTERFACE;
}

static ULONG WINAPI CF_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI CF_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI CF_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid,
                                        void **ppv)
{
    ClassFactory *This = impl_from_IClassFactory(iface);
    IUnknown *unk;
    HRESULT hr;

    TRACE("(%p)->(%p %s %p)\n", iface, outer, debugstr_guid(riid), ppv);

    *ppv = NULL;

    if (outer && !IsEqualGUID(riid, &IID_IUnknown))
        return CLASS_E_NOAGGREGATION;

    hr = This->pfnCreateInstance(outer, (void **)&unk);
    if (FAILED(hr))
        return hr;

    hr = IUnknown_QueryInterface(unk, riid, ppv);
    IUnknown_Release(unk);
    return hr;
}

static HRESULT WINAPI CF_LockServer(IClassFactory *iface, BOOL dolock)
{
    TRACE("(%d)\n", dolock);
    return S_OK;
}

static const IClassFactoryVtbl ClassFactoryVtbl =
{
    CF_QueryInterface,
    CF_AddRef,
    CF_Release,
    CF_CreateInstance,
    CF_LockServer
};

static ClassFactory CUriCF = {{&ClassFactoryVtbl}, Uri_Construct};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    TRACE("clsid %s, riid %s, out %p.\n", debugstr_guid(clsid), debugstr_guid(riid), out);

    if (IsEqualGUID(clsid, &CLSID_CUri))
        return IClassFactory_QueryInterface(&CUriCF.IClassFactory_iface, riid, out);

    FIXME("%s: no class found.\n", debugstr_guid(clsid));
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    const WCHAR *name = WindowsGetStringRawBuffer(classid, NULL);

    TRACE("classid %s, factory %p.\n", debugstr_hstring(classid), factory);
    if (!wcscmp(name, RuntimeClass_Windows_Foundation_WwwFormUrlDecoder))
    {
        *factory = www_form_url_decoder_factory;
        IActivationFactory_AddRef(*factory);
        return S_OK;
    }
    *factory = &iertutil.IActivationFactory_iface;
    IUnknown_AddRef(*factory);
    return S_OK;
}

/***********************************************************************
 *      SetWebPlatformSecurityManagerFactoryCallback (iertutil.811)
 *
 * Called by Windows.UI.Xaml.dll during Application init to hook its security manager factory.
 */
HRESULT WINAPI SetWebPlatformSecurityManagerFactoryCallback( void *callback )
{
    FIXME( "%p: stub\n", callback );
    return S_OK;
}

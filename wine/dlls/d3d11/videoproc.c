/*
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

/*
 * D3D11 video processing: the enumerator, the processor, its input and output views, and VideoProcessorBlt.
 *
 * Games that decode video through a Source Reader with a DXGI device manager get NV12 textures and convert them
 * with VideoProcessorBlt; on Windows every GPU has a video processor, so they do not check for one. Phantom Dust
 * draws its movies this way and dereferences the output texture whether or not the Blt worked.
 *
 * The Blt runs on the CPU through the immediate context: the input subresource is copied to a staging texture and
 * mapped, converted (NV12 or 8-bit RGB, nearest-neighbour scaling, BT.601 below 720 lines and BT.709 from there,
 * studio range), and uploaded to the output with UpdateSubresource. That is enough for full-motion video at game
 * resolutions. Not handled: deinterlacing, filters, alpha, the background colour, colour space and rotation state,
 * and more than one stream.
 */

#include "d3d11_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d11);

/* ---- ID3D11VideoProcessorEnumerator ---- */

static struct d3d_video_processor_enum *impl_from_ID3D11VideoProcessorEnumerator(ID3D11VideoProcessorEnumerator *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_video_processor_enum, ID3D11VideoProcessorEnumerator_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enum_QueryInterface(ID3D11VideoProcessorEnumerator *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11VideoProcessorEnumerator)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11VideoProcessorEnumerator_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_enum_AddRef(ID3D11VideoProcessorEnumerator *iface)
{
    struct d3d_video_processor_enum *e = impl_from_ID3D11VideoProcessorEnumerator(iface);
    ULONG refcount = InterlockedIncrement(&e->refcount);

    TRACE("%p increasing refcount to %lu.\n", e, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_enum_Release(ID3D11VideoProcessorEnumerator *iface)
{
    struct d3d_video_processor_enum *e = impl_from_ID3D11VideoProcessorEnumerator(iface);
    ULONG refcount = InterlockedDecrement(&e->refcount);

    TRACE("%p decreasing refcount to %lu.\n", e, refcount);

    if (!refcount)
    {
        ID3D11Device5_Release(&e->device->ID3D11Device5_iface);
        wined3d_private_store_cleanup(&e->private_store);
        free(e);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d3d11_video_processor_enum_GetDevice(ID3D11VideoProcessorEnumerator *iface,
        ID3D11Device **device)
{
    struct d3d_video_processor_enum *e = impl_from_ID3D11VideoProcessorEnumerator(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = (ID3D11Device *)e->device;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enum_GetPrivateData(ID3D11VideoProcessorEnumerator *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d_video_processor_enum *e = impl_from_ID3D11VideoProcessorEnumerator(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&e->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enum_SetPrivateData(ID3D11VideoProcessorEnumerator *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d_video_processor_enum *e = impl_from_ID3D11VideoProcessorEnumerator(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&e->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enum_SetPrivateDataInterface(
        ID3D11VideoProcessorEnumerator *iface, REFGUID guid, const IUnknown *data)
{
    struct d3d_video_processor_enum *e = impl_from_ID3D11VideoProcessorEnumerator(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&e->private_store, guid, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enum_GetVideoProcessorContentDesc(
        ID3D11VideoProcessorEnumerator *iface, D3D11_VIDEO_PROCESSOR_CONTENT_DESC *desc)
{
    struct d3d_video_processor_enum *e = impl_from_ID3D11VideoProcessorEnumerator(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    *desc = e->desc;
    return S_OK;
}

static BOOL is_rgb_format(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM
            || format == DXGI_FORMAT_R8G8B8A8_UNORM;
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enum_CheckVideoProcessorFormat(
        ID3D11VideoProcessorEnumerator *iface, DXGI_FORMAT format, UINT *flags)
{
    TRACE("iface %p, format %#x, flags %p.\n", iface, format, flags);

    if (!flags)
        return E_INVALIDARG;

    if (format == DXGI_FORMAT_NV12)
        *flags = D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT;
    else if (is_rgb_format(format))
        *flags = D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT | D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT;
    else
        *flags = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enum_GetVideoProcessorCaps(
        ID3D11VideoProcessorEnumerator *iface, D3D11_VIDEO_PROCESSOR_CAPS *caps)
{
    TRACE("iface %p, caps %p.\n", iface, caps);

    memset(caps, 0, sizeof(*caps));
    caps->RateConversionCapsCount = 1;
    caps->MaxInputStreams = 1;
    caps->MaxStreamStates = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enum_GetVideoProcessorRateConversionCaps(
        ID3D11VideoProcessorEnumerator *iface, UINT type_idx, D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS *caps)
{
    TRACE("iface %p, type_idx %u, caps %p.\n", iface, type_idx, caps);

    if (type_idx)
        return E_INVALIDARG;
    memset(caps, 0, sizeof(*caps));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enum_GetVideoProcessorCustomRate(
        ID3D11VideoProcessorEnumerator *iface, UINT type_idx, UINT rate_idx, D3D11_VIDEO_PROCESSOR_CUSTOM_RATE *rate)
{
    TRACE("iface %p, type_idx %u, rate_idx %u, rate %p: none.\n", iface, type_idx, rate_idx, rate);

    return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enum_GetVideoProcessorFilterRange(
        ID3D11VideoProcessorEnumerator *iface, D3D11_VIDEO_PROCESSOR_FILTER filter,
        D3D11_VIDEO_PROCESSOR_FILTER_RANGE *range)
{
    TRACE("iface %p, filter %#x, range %p: no filters.\n", iface, filter, range);

    return E_INVALIDARG;
}

static const struct ID3D11VideoProcessorEnumeratorVtbl d3d11_video_processor_enum_vtbl =
{
    d3d11_video_processor_enum_QueryInterface,
    d3d11_video_processor_enum_AddRef,
    d3d11_video_processor_enum_Release,
    d3d11_video_processor_enum_GetDevice,
    d3d11_video_processor_enum_GetPrivateData,
    d3d11_video_processor_enum_SetPrivateData,
    d3d11_video_processor_enum_SetPrivateDataInterface,
    d3d11_video_processor_enum_GetVideoProcessorContentDesc,
    d3d11_video_processor_enum_CheckVideoProcessorFormat,
    d3d11_video_processor_enum_GetVideoProcessorCaps,
    d3d11_video_processor_enum_GetVideoProcessorRateConversionCaps,
    d3d11_video_processor_enum_GetVideoProcessorCustomRate,
    d3d11_video_processor_enum_GetVideoProcessorFilterRange,
};

HRESULT d3d_video_processor_enum_create(struct d3d_device *device, const D3D11_VIDEO_PROCESSOR_CONTENT_DESC *desc,
        struct d3d_video_processor_enum **enumerator)
{
    struct d3d_video_processor_enum *object;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->ID3D11VideoProcessorEnumerator_iface.lpVtbl = &d3d11_video_processor_enum_vtbl;
    object->refcount = 1;
    object->desc = *desc;
    wined3d_private_store_init(&object->private_store);
    object->device = device;
    ID3D11Device5_AddRef(&device->ID3D11Device5_iface);

    TRACE("Created video processor enumerator %p, input %ux%u, output %ux%u.\n", object,
            desc->InputWidth, desc->InputHeight, desc->OutputWidth, desc->OutputHeight);
    *enumerator = object;

    return S_OK;
}

/* ---- ID3D11VideoProcessor ---- */

static struct d3d_video_processor *impl_from_ID3D11VideoProcessor(ID3D11VideoProcessor *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_video_processor, ID3D11VideoProcessor_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_QueryInterface(ID3D11VideoProcessor *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11VideoProcessor)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11VideoProcessor_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_AddRef(ID3D11VideoProcessor *iface)
{
    struct d3d_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);
    ULONG refcount = InterlockedIncrement(&processor->refcount);

    TRACE("%p increasing refcount to %lu.\n", processor, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_Release(ID3D11VideoProcessor *iface)
{
    struct d3d_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);
    ULONG refcount = InterlockedDecrement(&processor->refcount);

    TRACE("%p decreasing refcount to %lu.\n", processor, refcount);

    if (!refcount)
    {
        if (processor->staging)
            ID3D11Texture2D_Release(processor->staging);
        ID3D11Device5_Release(&processor->device->ID3D11Device5_iface);
        wined3d_private_store_cleanup(&processor->private_store);
        free(processor);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d3d11_video_processor_GetDevice(ID3D11VideoProcessor *iface, ID3D11Device **device)
{
    struct d3d_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = (ID3D11Device *)processor->device;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_GetPrivateData(ID3D11VideoProcessor *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&processor->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_SetPrivateData(ID3D11VideoProcessor *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&processor->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_SetPrivateDataInterface(ID3D11VideoProcessor *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&processor->private_store, guid, data);
}

static void STDMETHODCALLTYPE d3d11_video_processor_GetContentDesc(ID3D11VideoProcessor *iface,
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC *desc)
{
    struct d3d_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    *desc = processor->desc;
}

static void STDMETHODCALLTYPE d3d11_video_processor_GetRateConversionCaps(ID3D11VideoProcessor *iface,
        D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS *caps)
{
    TRACE("iface %p, caps %p.\n", iface, caps);

    memset(caps, 0, sizeof(*caps));
}

static const struct ID3D11VideoProcessorVtbl d3d11_video_processor_vtbl =
{
    d3d11_video_processor_QueryInterface,
    d3d11_video_processor_AddRef,
    d3d11_video_processor_Release,
    d3d11_video_processor_GetDevice,
    d3d11_video_processor_GetPrivateData,
    d3d11_video_processor_SetPrivateData,
    d3d11_video_processor_SetPrivateDataInterface,
    d3d11_video_processor_GetContentDesc,
    d3d11_video_processor_GetRateConversionCaps,
};

HRESULT d3d_video_processor_create(struct d3d_device *device, ID3D11VideoProcessorEnumerator *enumerator,
        struct d3d_video_processor **processor)
{
    struct d3d_video_processor *object;

    if (!enumerator || enumerator->lpVtbl != &d3d11_video_processor_enum_vtbl)
        return E_INVALIDARG;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->ID3D11VideoProcessor_iface.lpVtbl = &d3d11_video_processor_vtbl;
    object->refcount = 1;
    object->desc = impl_from_ID3D11VideoProcessorEnumerator(enumerator)->desc;
    wined3d_private_store_init(&object->private_store);
    object->device = device;
    ID3D11Device5_AddRef(&device->ID3D11Device5_iface);

    TRACE("Created video processor %p.\n", object);
    *processor = object;

    return S_OK;
}

static struct d3d_video_processor *unsafe_impl_from_ID3D11VideoProcessor(ID3D11VideoProcessor *iface)
{
    if (!iface || iface->lpVtbl != &d3d11_video_processor_vtbl)
        return NULL;
    return impl_from_ID3D11VideoProcessor(iface);
}

void d3d_video_processor_set_rect(ID3D11VideoProcessor *iface, enum d3d_video_processor_rect which,
        UINT stream_idx, BOOL enable, const RECT *rect)
{
    struct d3d_video_processor *processor = unsafe_impl_from_ID3D11VideoProcessor(iface);
    RECT empty = {0};

    TRACE("processor %p, which %u, stream_idx %u, enable %d, rect %s.\n",
            iface, which, stream_idx, enable, wine_dbgstr_rect(rect));

    if (!processor || stream_idx)
        return;
    if (!rect)
        rect = &empty;

    switch (which)
    {
        case D3D_VIDEO_PROCESSOR_RECT_OUTPUT:
            processor->output_rect_enabled = enable;
            processor->output_rect = *rect;
            break;
        case D3D_VIDEO_PROCESSOR_RECT_SOURCE:
            processor->src_rect_enabled = enable;
            processor->src_rect = *rect;
            break;
        case D3D_VIDEO_PROCESSOR_RECT_DEST:
            processor->dst_rect_enabled = enable;
            processor->dst_rect = *rect;
            break;
    }
}

/* ---- ID3D11VideoProcessorInputView and ID3D11VideoProcessorOutputView ---- */

static const struct ID3D11VideoProcessorInputViewVtbl d3d11_video_processor_input_view_vtbl;
static const struct ID3D11VideoProcessorOutputViewVtbl d3d11_video_processor_output_view_vtbl;

static struct d3d_video_processor_view *impl_from_view(void *iface)
{
    /* both interfaces sit at the start of the object */
    return iface;
}

static HRESULT view_QueryInterface(struct d3d_video_processor_view *view, REFIID riid, void **object)
{
    TRACE("view %p, riid %s, object %p.\n", view, debugstr_guid(riid), object);

    if ((view->output ? IsEqualGUID(riid, &IID_ID3D11VideoProcessorOutputView)
                      : IsEqualGUID(riid, &IID_ID3D11VideoProcessorInputView))
            || IsEqualGUID(riid, &IID_ID3D11View)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        InterlockedIncrement(&view->refcount);
        *object = view;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG view_AddRef(struct d3d_video_processor_view *view)
{
    ULONG refcount = InterlockedIncrement(&view->refcount);

    TRACE("%p increasing refcount to %lu.\n", view, refcount);

    return refcount;
}

static ULONG view_Release(struct d3d_video_processor_view *view)
{
    ULONG refcount = InterlockedDecrement(&view->refcount);

    TRACE("%p decreasing refcount to %lu.\n", view, refcount);

    if (!refcount)
    {
        ID3D11Resource_Release(view->resource);
        ID3D11Device5_Release(&view->device->ID3D11Device5_iface);
        wined3d_private_store_cleanup(&view->private_store);
        free(view);
    }

    return refcount;
}

static void view_GetDevice(struct d3d_video_processor_view *view, ID3D11Device **device)
{
    *device = (ID3D11Device *)view->device;
    ID3D11Device_AddRef(*device);
}

static void view_GetResource(struct d3d_video_processor_view *view, ID3D11Resource **resource)
{
    *resource = view->resource;
    ID3D11Resource_AddRef(*resource);
}

#define VIEW_METHODS(kind, iface_type) \
static HRESULT STDMETHODCALLTYPE d3d11_video_processor_##kind##_view_QueryInterface(iface_type *iface, \
        REFIID riid, void **object) \
{ \
    return view_QueryInterface(impl_from_view(iface), riid, object); \
} \
static ULONG STDMETHODCALLTYPE d3d11_video_processor_##kind##_view_AddRef(iface_type *iface) \
{ \
    return view_AddRef(impl_from_view(iface)); \
} \
static ULONG STDMETHODCALLTYPE d3d11_video_processor_##kind##_view_Release(iface_type *iface) \
{ \
    return view_Release(impl_from_view(iface)); \
} \
static void STDMETHODCALLTYPE d3d11_video_processor_##kind##_view_GetDevice(iface_type *iface, ID3D11Device **device) \
{ \
    view_GetDevice(impl_from_view(iface), device); \
} \
static HRESULT STDMETHODCALLTYPE d3d11_video_processor_##kind##_view_GetPrivateData(iface_type *iface, \
        REFGUID guid, UINT *data_size, void *data) \
{ \
    return d3d_get_private_data(&impl_from_view(iface)->private_store, guid, data_size, data); \
} \
static HRESULT STDMETHODCALLTYPE d3d11_video_processor_##kind##_view_SetPrivateData(iface_type *iface, \
        REFGUID guid, UINT data_size, const void *data) \
{ \
    return d3d_set_private_data(&impl_from_view(iface)->private_store, guid, data_size, data); \
} \
static HRESULT STDMETHODCALLTYPE d3d11_video_processor_##kind##_view_SetPrivateDataInterface(iface_type *iface, \
        REFGUID guid, const IUnknown *data) \
{ \
    return d3d_set_private_data_interface(&impl_from_view(iface)->private_store, guid, data); \
} \
static void STDMETHODCALLTYPE d3d11_video_processor_##kind##_view_GetResource(iface_type *iface, \
        ID3D11Resource **resource) \
{ \
    view_GetResource(impl_from_view(iface), resource); \
}

VIEW_METHODS(input, ID3D11VideoProcessorInputView)
VIEW_METHODS(output, ID3D11VideoProcessorOutputView)

static void STDMETHODCALLTYPE d3d11_video_processor_input_view_GetDesc(ID3D11VideoProcessorInputView *iface,
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC *desc)
{
    *desc = impl_from_view(iface)->input_desc;
}

static void STDMETHODCALLTYPE d3d11_video_processor_output_view_GetDesc(ID3D11VideoProcessorOutputView *iface,
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC *desc)
{
    *desc = impl_from_view(iface)->output_desc;
}

static const struct ID3D11VideoProcessorInputViewVtbl d3d11_video_processor_input_view_vtbl =
{
    d3d11_video_processor_input_view_QueryInterface,
    d3d11_video_processor_input_view_AddRef,
    d3d11_video_processor_input_view_Release,
    d3d11_video_processor_input_view_GetDevice,
    d3d11_video_processor_input_view_GetPrivateData,
    d3d11_video_processor_input_view_SetPrivateData,
    d3d11_video_processor_input_view_SetPrivateDataInterface,
    d3d11_video_processor_input_view_GetResource,
    d3d11_video_processor_input_view_GetDesc,
};

static const struct ID3D11VideoProcessorOutputViewVtbl d3d11_video_processor_output_view_vtbl =
{
    d3d11_video_processor_output_view_QueryInterface,
    d3d11_video_processor_output_view_AddRef,
    d3d11_video_processor_output_view_Release,
    d3d11_video_processor_output_view_GetDevice,
    d3d11_video_processor_output_view_GetPrivateData,
    d3d11_video_processor_output_view_SetPrivateData,
    d3d11_video_processor_output_view_SetPrivateDataInterface,
    d3d11_video_processor_output_view_GetResource,
    d3d11_video_processor_output_view_GetDesc,
};

HRESULT d3d_video_processor_view_create(struct d3d_device *device, ID3D11Resource *resource,
        const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC *input_desc,
        const D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC *output_desc, struct d3d_video_processor_view **view)
{
    struct d3d_video_processor_view *object;
    D3D11_RESOURCE_DIMENSION dimension;

    if (!resource || (!input_desc == !output_desc))
        return E_INVALIDARG;
    ID3D11Resource_GetType(resource, &dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        WARN("Resource dimension %u is not a 2D texture.\n", dimension);
        return E_INVALIDARG;
    }

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (input_desc)
    {
        object->ID3D11VideoProcessorInputView_iface.lpVtbl = &d3d11_video_processor_input_view_vtbl;
        object->input_desc = *input_desc;
    }
    else
    {
        object->ID3D11VideoProcessorOutputView_iface.lpVtbl = &d3d11_video_processor_output_view_vtbl;
        object->output_desc = *output_desc;
        object->output = TRUE;
    }
    object->refcount = 1;
    wined3d_private_store_init(&object->private_store);
    object->device = device;
    ID3D11Device5_AddRef(&device->ID3D11Device5_iface);
    object->resource = resource;
    ID3D11Resource_AddRef(resource);

    TRACE("Created video processor %s view %p.\n", object->output ? "output" : "input", object);
    *view = object;

    return S_OK;
}

static struct d3d_video_processor_view *unsafe_impl_from_view(void *iface, BOOL output)
{
    const void *vtbl = output ? (const void *)&d3d11_video_processor_output_view_vtbl
                              : (const void *)&d3d11_video_processor_input_view_vtbl;

    if (!iface || *(const void **)iface != vtbl)
        return NULL;
    return impl_from_view(iface);
}

/* ---- VideoProcessorBlt ---- */

static inline UINT calc_subresource(UINT mip, UINT slice, UINT mip_levels)
{
    return mip + slice * mip_levels;
}

static inline BYTE clamp_byte(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static BOOL rect_is_empty(const RECT *r)
{
    return r->right <= r->left || r->bottom <= r->top;
}

/* d3d11 does not import user32 */
static void set_rect(RECT *r, LONG left, LONG top, LONG right, LONG bottom)
{
    r->left = left;
    r->top = top;
    r->right = right;
    r->bottom = bottom;
}

static void intersect_rect(RECT *dst, const RECT *a, const RECT *b)
{
    set_rect(dst, max(a->left, b->left), max(a->top, b->top), min(a->right, b->right), min(a->bottom, b->bottom));
}

/* the input subresource, copied to a CPU-readable texture of its own size */
static HRESULT map_input(struct d3d_video_processor *processor, ID3D11DeviceContext *context,
        struct d3d_video_processor_view *input, D3D11_TEXTURE2D_DESC *desc, D3D11_MAPPED_SUBRESOURCE *map)
{
    ID3D11Device *device = (ID3D11Device *)&processor->device->ID3D11Device5_iface;
    UINT mip = input->input_desc.Texture2D.MipSlice, slice = input->input_desc.Texture2D.ArraySlice;
    D3D11_TEXTURE2D_DESC staging_desc;
    HRESULT hr;

    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)input->resource, desc);

    staging_desc = *desc;
    staging_desc.Width = max(1, desc->Width >> mip);
    staging_desc.Height = max(1, desc->Height >> mip);
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.SampleDesc.Quality = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    if (processor->staging && memcmp(&processor->staging_desc, &staging_desc, sizeof(staging_desc)))
    {
        ID3D11Texture2D_Release(processor->staging);
        processor->staging = NULL;
    }
    if (!processor->staging)
    {
        if (FAILED(hr = ID3D11Device_CreateTexture2D(device, &staging_desc, NULL, &processor->staging)))
        {
            WARN("Failed to create a staging texture, hr %#lx.\n", hr);
            return hr;
        }
        processor->staging_desc = staging_desc;
    }

    ID3D11DeviceContext_CopySubresourceRegion(context, (ID3D11Resource *)processor->staging, 0, 0, 0, 0,
            input->resource, calc_subresource(mip, slice, desc->MipLevels), NULL);
    *desc = staging_desc;
    return ID3D11DeviceContext_Map(context, (ID3D11Resource *)processor->staging, 0, D3D11_MAP_READ, 0, map);
}

HRESULT d3d_video_processor_blt(ID3D11DeviceContext *context, ID3D11VideoProcessor *iface,
        ID3D11VideoProcessorOutputView *output_iface, UINT stream_count, const D3D11_VIDEO_PROCESSOR_STREAM *streams)
{
    struct d3d_video_processor *processor = unsafe_impl_from_ID3D11VideoProcessor(iface);
    struct d3d_video_processor_view *output = unsafe_impl_from_view(output_iface, TRUE), *input;
    D3D11_TEXTURE2D_DESC in_desc, out_desc;
    RECT src, dst, target, out_bounds;
    D3D11_MAPPED_SUBRESOURCE map;
    int cr_r, cb_g, cr_g, cb_b;
    UINT x, y, out_mip, out_slice;
    BOOL bgra, nv12;
    D3D11_BOX box;
    BYTE *pixels;
    HRESULT hr;

    if (!processor || !output || !stream_count || !streams)
        return E_INVALIDARG;
    if (stream_count > 1)
        FIXME("Ignoring %u streams after the first.\n", stream_count - 1);
    if (!streams[0].Enable)
        return S_OK;
    if (!(input = unsafe_impl_from_view(streams[0].pInputSurface, FALSE)))
        return E_INVALIDARG;

    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)output->resource, &out_desc);
    if (!is_rgb_format(out_desc.Format))
    {
        FIXME("Unsupported output format %#x.\n", out_desc.Format);
        return E_NOTIMPL;
    }
    bgra = out_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM;
    if (output->output_desc.ViewDimension == D3D11_VPOV_DIMENSION_TEXTURE2DARRAY)
    {
        out_mip = output->output_desc.Texture2DArray.MipSlice;
        out_slice = output->output_desc.Texture2DArray.FirstArraySlice;
    }
    else
    {
        out_mip = output->output_desc.Texture2D.MipSlice;
        out_slice = 0;
    }
    set_rect(&out_bounds, 0, 0, max(1, out_desc.Width >> out_mip), max(1, out_desc.Height >> out_mip));

    if (FAILED(hr = map_input(processor, context, input, &in_desc, &map)))
    {
        WARN("Failed to read the input, hr %#lx.\n", hr);
        return hr;
    }
    nv12 = in_desc.Format == DXGI_FORMAT_NV12;
    if (!nv12 && !is_rgb_format(in_desc.Format))
    {
        FIXME("Unsupported input format %#x.\n", in_desc.Format);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)processor->staging, 0);
        return E_NOTIMPL;
    }

    set_rect(&src, 0, 0, in_desc.Width, in_desc.Height);
    if (processor->src_rect_enabled)
        intersect_rect(&src, &src, &processor->src_rect);
    target = processor->output_rect_enabled ? processor->output_rect : out_bounds;
    dst = processor->dst_rect_enabled ? processor->dst_rect : target;
    intersect_rect(&dst, &dst, &target);
    intersect_rect(&dst, &dst, &out_bounds);
    if (rect_is_empty(&src) || rect_is_empty(&dst))
    {
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)processor->staging, 0);
        return S_OK;
    }

    /* studio-range YCbCr to RGB, coefficients scaled by 256 */
    if (in_desc.Height >= 720)
    {
        cr_r = 459; cb_g = -55; cr_g = -136; cb_b = 541;   /* BT.709 */
    }
    else
    {
        cr_r = 409; cb_g = -100; cr_g = -208; cb_b = 516;  /* BT.601 */
    }

    if (!(pixels = malloc((dst.right - dst.left) * (dst.bottom - dst.top) * 4)))
    {
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)processor->staging, 0);
        return E_OUTOFMEMORY;
    }

    for (y = 0; y < dst.bottom - dst.top; ++y)
    {
        UINT sy = src.top + (UINT64)y * (src.bottom - src.top) / (dst.bottom - dst.top);
        const BYTE *row = (const BYTE *)map.pData + sy * map.RowPitch;
        const BYTE *uv_row = (const BYTE *)map.pData + map.RowPitch * (in_desc.Height + sy / 2);
        BYTE *out = pixels + y * (dst.right - dst.left) * 4;

        for (x = 0; x < dst.right - dst.left; ++x, out += 4)
        {
            UINT sx = src.left + (UINT64)x * (src.right - src.left) / (dst.right - dst.left);
            BYTE r, g, b;

            if (nv12)
            {
                int c = 298 * (row[sx] - 16), d = uv_row[sx & ~1u] - 128, e = uv_row[sx | 1u] - 128;

                r = clamp_byte((c + cr_r * e + 128) >> 8);
                g = clamp_byte((c + cb_g * d + cr_g * e + 128) >> 8);
                b = clamp_byte((c + cb_b * d + 128) >> 8);
            }
            else if (in_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
            {
                r = row[sx * 4]; g = row[sx * 4 + 1]; b = row[sx * 4 + 2];
            }
            else
            {
                b = row[sx * 4]; g = row[sx * 4 + 1]; r = row[sx * 4 + 2];
            }

            out[0] = bgra ? b : r;
            out[1] = g;
            out[2] = bgra ? r : b;
            out[3] = 0xff;
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)processor->staging, 0);

    box.left = dst.left;
    box.top = dst.top;
    box.right = dst.right;
    box.bottom = dst.bottom;
    box.front = 0;
    box.back = 1;
    ID3D11DeviceContext_UpdateSubresource(context, output->resource,
            calc_subresource(out_mip, out_slice, out_desc.MipLevels), &box, pixels, (dst.right - dst.left) * 4, 0);
    free(pixels);

    TRACE("Converted %s %s to %s.\n", nv12 ? "NV12" : "RGB", wine_dbgstr_rect(&src), wine_dbgstr_rect(&dst));
    return S_OK;
}

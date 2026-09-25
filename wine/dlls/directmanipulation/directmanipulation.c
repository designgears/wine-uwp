/*
 * Copyright 2019 Alistair Leslie-Hughes
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
#define COBJMACROS

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "oleidl.h"
#include "rpcproxy.h"
#include "wine/debug.h"

#include "directmanipulation.h"
#include "dcomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(manipulation);

struct directmanipulation
{
    IDirectManipulationManager2 IDirectManipulationManager2_iface;
    IDirectManipulationUpdateManager *updatemanager;
    LONG ref;
    HWND window;    /* from Activate(); ProcessInput's wheel positions are relative to it */
};

/* Every live viewport, so that ProcessInput can find the one under a wheel event. */
static struct directviewport *viewports[64];
static unsigned int viewport_count;

struct directupdatemanager
{
    IDirectManipulationUpdateManager IDirectManipulationUpdateManager_iface;
    LONG ref;
};

static inline struct directmanipulation *impl_from_IDirectManipulationManager2(IDirectManipulationManager2 *iface)
{
    return CONTAINING_RECORD(iface, struct directmanipulation, IDirectManipulationManager2_iface);
}

static inline struct directupdatemanager *impl_from_IDirectManipulationUpdateManager(IDirectManipulationUpdateManager *iface)
{
    return CONTAINING_RECORD(iface, struct directupdatemanager, IDirectManipulationUpdateManager_iface);
}

static HRESULT WINAPI update_manager_QueryInterface(IDirectManipulationUpdateManager *iface, REFIID riid,void **ppv)
{
    struct directupdatemanager *This = impl_from_IDirectManipulationUpdateManager(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IDirectManipulationUpdateManager)) {
        IUnknown_AddRef(iface);
        *ppv = iface;
        return S_OK;
    }

    FIXME("(%p)->(%s,%p), not found\n", This, debugstr_guid(riid), ppv);
    return E_NOINTERFACE;
}

ULONG WINAPI update_manager_AddRef(IDirectManipulationUpdateManager *iface)
{
    struct directupdatemanager *This = impl_from_IDirectManipulationUpdateManager(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);

    return ref;
}

ULONG WINAPI update_manager_Release(IDirectManipulationUpdateManager *iface)
{
    struct directupdatemanager *This = impl_from_IDirectManipulationUpdateManager(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);

    if (!ref)
    {
        free(This);
    }
    return ref;
}

static HRESULT WINAPI update_manager_RegisterWaitHandleCallback(IDirectManipulationUpdateManager *iface, HANDLE handle,
            IDirectManipulationUpdateHandler *handler, DWORD *cookie)
{
    struct directupdatemanager *This = impl_from_IDirectManipulationUpdateManager(iface);
    FIXME("%p, %p, %p, %p\n", This, handle, handler, cookie);
    return E_NOTIMPL;
}

static HRESULT WINAPI update_manager_UnregisterWaitHandleCallback(IDirectManipulationUpdateManager *iface, DWORD cookie)
{
    struct directupdatemanager *This = impl_from_IDirectManipulationUpdateManager(iface);
    FIXME("%p, %lx\n", This, cookie);
    return E_NOTIMPL;
}

static HRESULT WINAPI update_manager_Update(IDirectManipulationUpdateManager *iface, IDirectManipulationFrameInfoProvider *provider)
{
    struct directupdatemanager *This = impl_from_IDirectManipulationUpdateManager(iface);
    FIXME("%p, %p\n", This, provider);
    return E_NOTIMPL;
}

struct IDirectManipulationUpdateManagerVtbl updatemanagerVtbl =
{
    update_manager_QueryInterface,
    update_manager_AddRef,
    update_manager_Release,
    update_manager_RegisterWaitHandleCallback,
    update_manager_UnregisterWaitHandleCallback,
    update_manager_Update
};

static HRESULT create_update_manager(IDirectManipulationUpdateManager **obj)
{
    struct directupdatemanager *object;

    object = malloc(sizeof(*object));
    if(!object)
        return E_OUTOFMEMORY;

    object->IDirectManipulationUpdateManager_iface.lpVtbl = &updatemanagerVtbl;
    object->ref = 1;

    *obj = &object->IDirectManipulationUpdateManager_iface;

    return S_OK;
}

/* Content keeps what callers set. With no manipulation engine the content transform stays whatever was
 * last synced (identity by default), and inertia always ends where the content is now. */
struct dm_curve;
struct primarycontext
{
    const void **private_vtbl; /* {d8042f40} view, see primary_private_vtbl */
    const void **curves_vtbl;  /* {820c0b62} view, see content_curves_vtbl */
    IDirectManipulationPrimaryContent IDirectManipulationPrimaryContent_iface;
    IDirectManipulationContent IDirectManipulationContent_iface;
    LONG ref;

    IDirectManipulationViewport2 *viewport;
    RECT rect;
    float transform[6];
    IUnknown *tag_object;
    UINT32 tag_id;
    float zoom_min, zoom_max;
    DIRECTMANIPULATION_HORIZONTALALIGNMENT horizontal_alignment;
    DIRECTMANIPULATION_VERTICALALIGNMENT vertical_alignment;
    IDCompositionMatrixTransform *shared_transform; /* handed to XAML by CreateSharedContentTransform */
    struct dm_curve *curve_x, *curve_y; /* secondary content: parametric curves off the primary's offset */
    struct dm_curve *pending_curve;     /* the curve GetCurve handed out, being filled with segments */
};

static inline struct primarycontext *impl_from_IDirectManipulationPrimaryContent(IDirectManipulationPrimaryContent *iface)
{
    return CONTAINING_RECORD(iface, struct primarycontext, IDirectManipulationPrimaryContent_iface);
}

static inline struct primarycontext *impl_from_IDirectManipulationContent(IDirectManipulationContent *iface)
{
    return CONTAINING_RECORD(iface, struct primarycontext, IDirectManipulationContent_iface);
}

/* {820c0b62-45be-41dd-a2dc-6ccaf6ae0f04}: private content interface XAML's CDirectManipulationService queries on
 * secondary contents (SetupSecondaryContent, Apply*ReflexCurves). Slot 3 hands out a per-motion "curve" object
 * for a riid, slot 4 configures it (int, int, IUnknown *, int); the curve object's slots take floats. Neither
 * layout is documented; the objects accept everything and apply nothing, which keeps XAML's UI-thread tick
 * (where this runs) from fail-fasting. */
static const GUID IID_content_curves = { 0x820c0b62, 0x45be, 0x41dd, { 0xa2, 0xdc, 0x6c, 0xca, 0xf6, 0xae, 0x0f, 0x04 } };

/* A curve is a list of five-float segments (start input, start output, then three polynomial
 * coefficients over the distance from the start): XAML's SetupSecondaryContent builds them for the sticky
 * headers of a ScrollViewer and for the Pivot's panel, whose X curve is 0 -> 0, then slope -1 up to the
 * extent: the panel stays put in the viewport while the primary scrolls under it. */
struct dm_curve { const void **vtbl; LONG ref; float segments[16][5]; unsigned int count; };
static HRESULT WINAPI dm_curve_QueryInterface(void *iface, REFIID riid, void **ppv)
{
    struct dm_curve *This = iface;
    TRACE("%p, %s\n", This, debugstr_guid(riid));
    if (IsEqualGUID(riid, &IID_IUnknown)) { *ppv = iface; InterlockedIncrement(&This->ref); return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI dm_curve_AddRef(void *iface) { return InterlockedIncrement(&((struct dm_curve *)iface)->ref); }
static ULONG WINAPI dm_curve_Release(void *iface)
{
    ULONG ref = InterlockedDecrement(&((struct dm_curve *)iface)->ref);
    if (!ref) free(iface);
    return ref;
}
#define CURVE_STUB(n) static HRESULT WINAPI dm_curve_##n(void *iface) \
    { TRACE("%p: curve slot %d, not applied\n", iface, n); return S_OK; }
CURVE_STUB(3) CURVE_STUB(4) CURVE_STUB(5) CURVE_STUB(7) CURVE_STUB(9)
CURVE_STUB(10) CURVE_STUB(11) CURVE_STUB(12) CURVE_STUB(13) CURVE_STUB(14) CURVE_STUB(15)
#undef CURVE_STUB
/* Slot 6 adds a segment: the segment array and the float index of the segment within it. */
static HRESULT WINAPI dm_curve_6(void *iface, const float *segments, UINT64 index, UINT64 unused)
{
    struct dm_curve *This = iface;
    TRACE("%p, %p, index %I64u\n", This, segments, index);
    if (!segments || This->count == ARRAY_SIZE(This->segments)) return S_OK;
    memcpy(This->segments[This->count], segments + index, sizeof(This->segments[0]));
    TRACE("curve %p segment %u: from %g -> %g, coefficients %g %g %g\n", This, This->count,
          This->segments[This->count][0], This->segments[This->count][1], This->segments[This->count][2],
          This->segments[This->count][3], This->segments[This->count][4]);
    This->count++;
    return S_OK;
}
/* Slot 8 empties the curve: XAML fills the same object again for the next motion. */
static HRESULT WINAPI dm_curve_8(void *iface)
{
    struct dm_curve *This = iface;
    TRACE("%p: reset\n", This);
    This->count = 0;
    return S_OK;
}
static float dm_curve_eval(const struct dm_curve *curve, float x)
{
    const float *seg = curve->segments[0];
    unsigned int i;
    float d;
    if (!curve->count) return 0.0f;
    for (i = 1; i < curve->count; i++) if (curve->segments[i][0] <= x) seg = curve->segments[i];
    d = x - seg[0];
    return seg[1] + seg[2] * d + seg[3] * d * d + seg[4] * d * d * d;
}
static const void *dm_curve_vtbl[] =
{
    dm_curve_QueryInterface, dm_curve_AddRef, dm_curve_Release,
    dm_curve_3, dm_curve_4, dm_curve_5, dm_curve_6, dm_curve_7, dm_curve_8, dm_curve_9, dm_curve_10,
    dm_curve_11, dm_curve_12, dm_curve_13, dm_curve_14, dm_curve_15,
};

static HRESULT WINAPI primary_QueryInterface(IDirectManipulationPrimaryContent *iface, REFIID riid, void **ppv);
static ULONG WINAPI primary_AddRef(IDirectManipulationPrimaryContent *iface);
static ULONG WINAPI primary_Release(IDirectManipulationPrimaryContent *iface);
static struct primarycontext *impl_from_curves(void *iface)
{
    return CONTAINING_RECORD(iface, struct primarycontext, curves_vtbl);
}
static HRESULT WINAPI content_curves_QueryInterface(void *iface, REFIID riid, void **ppv)
{
    return primary_QueryInterface(&impl_from_curves(iface)->IDirectManipulationPrimaryContent_iface, riid, ppv);
}
static ULONG WINAPI content_curves_AddRef(void *iface)
{
    return primary_AddRef(&impl_from_curves(iface)->IDirectManipulationPrimaryContent_iface);
}
static ULONG WINAPI content_curves_Release(void *iface)
{
    return primary_Release(&impl_from_curves(iface)->IDirectManipulationPrimaryContent_iface);
}
static HRESULT WINAPI content_curves_GetCurve(void *iface, REFIID riid, void **out)
{
    struct dm_curve *curve;
    TRACE("%p, %s, %p\n", impl_from_curves(iface), debugstr_guid(riid), out);
    if (!out) return E_POINTER;
    if (!(curve = calloc(1, sizeof(*curve)))) return E_OUTOFMEMORY;
    curve->vtbl = dm_curve_vtbl;
    curve->ref = 2; /* one for the caller, one for pending_curve */
    if (impl_from_curves(iface)->pending_curve) dm_curve_Release(impl_from_curves(iface)->pending_curve);
    impl_from_curves(iface)->pending_curve = curve;
    *out = curve;
    return S_OK;
}
#define CURVES_STUB(n) static HRESULT WINAPI content_curves_##n(void *iface) \
    { TRACE("%p: content curves slot %d, not applied\n", impl_from_curves(iface), n); return S_OK; }
/* Slot 4 applies the curve GetCurve handed out (the third argument is XAML's own wrapper of it): the
 * primary's motion it reads (1 X, 2 Y) and the content's motion it drives. The segments are copied since
 * XAML empties the object (slot 8) and refills it for the next motion. */
static HRESULT WINAPI content_curves_4(void *iface, DWORD input, DWORD output, void *wrapper)
{
    struct primarycontext *This = impl_from_curves(iface);
    struct dm_curve **slot, *copy;
    TRACE("%p, input %lu, output %lu, wrapper %p, pending curve %p\n", This, input, output, wrapper, This->pending_curve);
    if (!This->pending_curve) return S_OK;
    if (output == 1) slot = &This->curve_x;
    else if (output == 2) slot = &This->curve_y;
    else return S_OK;
    if (!(copy = calloc(1, sizeof(*copy)))) return E_OUTOFMEMORY;
    *copy = *This->pending_curve;
    copy->ref = 1;
    if (*slot) dm_curve_Release(*slot);
    *slot = copy;
    return S_OK;
}
CURVES_STUB(5) CURVES_STUB(6) CURVES_STUB(7) CURVES_STUB(8) CURVES_STUB(9) CURVES_STUB(10)
CURVES_STUB(11) CURVES_STUB(12) CURVES_STUB(13) CURVES_STUB(14) CURVES_STUB(15)
#undef CURVES_STUB
static const void *content_curves_vtbl[] =
{
    content_curves_QueryInterface, content_curves_AddRef, content_curves_Release, content_curves_GetCurve,
    content_curves_4, content_curves_5, content_curves_6, content_curves_7, content_curves_8, content_curves_9,
    content_curves_10, content_curves_11, content_curves_12, content_curves_13, content_curves_14, content_curves_15,
};

/* {d8042f40-a051-4fee-875e-243b7b38d66e}: private interface XAML's CDirectManipulationService::SetPrimaryContentTransform
 * queries on the primary content, then calls slot 12 with a 6-element matrix. Without it that call fails with
 * E_NOINTERFACE, which the app sees as an InvalidCastException after every pointer press. */
static const GUID IID_primary_private = { 0xd8042f40, 0xa051, 0x4fee, { 0x87, 0x5e, 0x24, 0x3b, 0x7b, 0x38, 0xd6, 0x6e } };

static HRESULT WINAPI primary_QueryInterface(IDirectManipulationPrimaryContent *iface, REFIID riid, void **ppv)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IDirectManipulationPrimaryContent))
    {
        IDirectManipulationPrimaryContent_AddRef(&This->IDirectManipulationPrimaryContent_iface);
        *ppv = &This->IDirectManipulationPrimaryContent_iface;
        return S_OK;
    }
    else if(IsEqualGUID(riid, &IID_IDirectManipulationContent))
    {
        IUnknown_AddRef(iface);
        *ppv = &This->IDirectManipulationContent_iface;
        return S_OK;
    }

    else if (IsEqualGUID(riid, &IID_primary_private))
    {
        IUnknown_AddRef(iface);
        *ppv = &This->private_vtbl;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_content_curves))
    {
        IUnknown_AddRef(iface);
        *ppv = &This->curves_vtbl;
        return S_OK;
    }

    FIXME("(%p)->(%s,%p),not found\n", This, debugstr_guid(riid), ppv);
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI primary_AddRef(IDirectManipulationPrimaryContent *iface)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);

    return ref;
}

static void viewport_untrack_content(IDirectManipulationViewport2 *viewport, struct primarycontext *content);
static ULONG WINAPI primary_Release(IDirectManipulationPrimaryContent *iface)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);

    if (!ref)
    {
        if (This->tag_object) IUnknown_Release(This->tag_object);
        if (This->shared_transform) IDCompositionMatrixTransform_Release(This->shared_transform);
        if (This->curve_x) dm_curve_Release(This->curve_x);
        if (This->curve_y) dm_curve_Release(This->curve_y);
        if (This->pending_curve) dm_curve_Release(This->pending_curve);
        if (This->viewport)
        {
            viewport_untrack_content(This->viewport, This);
            IDirectManipulationViewport2_Release(This->viewport);
        }
        free(This);
    }
    return ref;
}

static HRESULT WINAPI primary_SetSnapInterval(IDirectManipulationPrimaryContent *iface, DIRECTMANIPULATION_MOTION_TYPES motion,
            float interval, float offset)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    TRACE("%p, %#x, %f, %f: snapping not applied\n", This, motion, interval, offset);
    return S_OK;
}

static HRESULT WINAPI primary_SetSnapPoints(IDirectManipulationPrimaryContent *iface, DIRECTMANIPULATION_MOTION_TYPES motion,
            const float *points, DWORD count)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    TRACE("%p, %#x, %p, %ld: snapping not applied\n", This, motion, points, count);
    return S_OK;
}

static HRESULT WINAPI primary_SetSnapType(IDirectManipulationPrimaryContent *iface, DIRECTMANIPULATION_MOTION_TYPES motion,
            DIRECTMANIPULATION_SNAPPOINT_TYPE type)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    TRACE("%p, %#x, %d: snapping not applied\n", This, motion, type);
    return S_OK;
}

static HRESULT WINAPI primary_SetSnapCoordinate(IDirectManipulationPrimaryContent *iface, DIRECTMANIPULATION_MOTION_TYPES motion,
            DIRECTMANIPULATION_SNAPPOINT_COORDINATE coordinate, float origin)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    TRACE("%p, %#x, %d, %f: snapping not applied\n", This, motion, coordinate, origin);
    return S_OK;
}

static HRESULT WINAPI primary_SetZoomBoundaries(IDirectManipulationPrimaryContent *iface, float minimum, float maximum)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    TRACE("%p, %f, %f\n", This, minimum, maximum);
    if (minimum <= 0.0f || maximum < minimum) return E_INVALIDARG;
    This->zoom_min = minimum;
    This->zoom_max = maximum;
    return S_OK;
}

static HRESULT WINAPI primary_SetHorizontalAlignment(IDirectManipulationPrimaryContent *iface, DIRECTMANIPULATION_HORIZONTALALIGNMENT alignment)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    TRACE("%p, %#x\n", This, alignment);
    This->horizontal_alignment = alignment;
    return S_OK;
}

static HRESULT WINAPI primary_SetVerticalAlignment(IDirectManipulationPrimaryContent *iface, DIRECTMANIPULATION_VERTICALALIGNMENT alignment)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    TRACE("%p, %#x\n", This, alignment);
    This->vertical_alignment = alignment;
    return S_OK;
}

static HRESULT WINAPI primary_GetInertiaEndTransform(IDirectManipulationPrimaryContent *iface, float *matrix, DWORD count)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    TRACE("%p, %p, %ld\n", This, matrix, count);
    if (!matrix || count != ARRAY_SIZE(This->transform)) return E_INVALIDARG;
    memcpy(matrix, This->transform, sizeof(This->transform));
    return S_OK;
}

static HRESULT WINAPI primary_GetCenterPoint(IDirectManipulationPrimaryContent *iface, float *x, float *y)
{
    struct primarycontext *This = impl_from_IDirectManipulationPrimaryContent(iface);
    TRACE("%p, %p, %p\n", This, x, y);
    if (!x || !y) return E_POINTER;
    *x = (This->rect.left + This->rect.right) / 2.0f;
    *y = (This->rect.top + This->rect.bottom) / 2.0f;
    return S_OK;
}

static const IDirectManipulationPrimaryContentVtbl primaryVtbl =
{
    primary_QueryInterface,
    primary_AddRef,
    primary_Release,
    primary_SetSnapInterval,
    primary_SetSnapPoints,
    primary_SetSnapType,
    primary_SetSnapCoordinate,
    primary_SetZoomBoundaries,
    primary_SetHorizontalAlignment,
    primary_SetVerticalAlignment,
    primary_GetInertiaEndTransform,
    primary_GetCenterPoint
};

static struct primarycontext *impl_from_private( void *iface )
{
    return CONTAINING_RECORD(iface, struct primarycontext, private_vtbl);
}
static HRESULT WINAPI primary_private_QueryInterface(void *iface, REFIID riid, void **ppv)
{
    return primary_QueryInterface(&impl_from_private(iface)->IDirectManipulationPrimaryContent_iface, riid, ppv);
}
static ULONG WINAPI primary_private_AddRef(void *iface)
{
    return primary_AddRef(&impl_from_private(iface)->IDirectManipulationPrimaryContent_iface);
}
static ULONG WINAPI primary_private_Release(void *iface)
{
    return primary_Release(&impl_from_private(iface)->IDirectManipulationPrimaryContent_iface);
}
/* The layout of this interface is unknown beyond slot 12; the slots accept and ignore their arguments. */
#define PRIVATE_STUB(n) static HRESULT WINAPI primary_private_##n(void *iface) \
    { TRACE("%p: private primary content slot %d, not applied\n", impl_from_private(iface), n); return S_OK; }
PRIVATE_STUB(3) PRIVATE_STUB(4) PRIVATE_STUB(5) PRIVATE_STUB(6) PRIVATE_STUB(7) PRIVATE_STUB(8) PRIVATE_STUB(9)
PRIVATE_STUB(10) PRIVATE_STUB(11) PRIVATE_STUB(13) PRIVATE_STUB(14) PRIVATE_STUB(15)
#undef PRIVATE_STUB
static void content_resting_transform(struct primarycontext *This, float *matrix);
static void content_queue_update(struct primarycontext *content);
/* Slot 12: XAML's CDirectManipulationService::SetPrimaryContentTransform(x, y, zoom) hands the content
 * transform it wants (zoom, 0, 0, zoom, x, y) here, the way a public SyncContentTransform would; a
 * ScrollViewer's programmatic ChangeView (a CalendarView jumping to the current month) is nothing else. */
static HRESULT WINAPI primary_private_12(void *iface, void *unknown, const float *matrix, DWORD count)
{
    struct primarycontext *This = impl_from_private(iface);
    float applied[6];

    TRACE("%p, %p, %p, %lu\n", This, unknown, matrix, count);
    if (!matrix || count != ARRAY_SIZE(This->transform)) return E_INVALIDARG;
    TRACE("content %p set to scale %g,%g offset %g,%g\n", This, matrix[0], matrix[3], matrix[4], matrix[5]);
    memcpy(This->transform, matrix, sizeof(This->transform));
    content_resting_transform(This, applied);
    content_queue_update(This);
    return S_OK;
}
static const void *primary_private_vtbl[] =
{
    primary_private_QueryInterface, primary_private_AddRef, primary_private_Release,
    primary_private_3, primary_private_4, primary_private_5, primary_private_6, primary_private_7, primary_private_8,
    primary_private_9, primary_private_10, primary_private_11, primary_private_12, primary_private_13,
    primary_private_14, primary_private_15,
};


static HRESULT WINAPI content_QueryInterface(IDirectManipulationContent *iface, REFIID riid, void **ppv)
{
    struct primarycontext *This = impl_from_IDirectManipulationContent(iface);
    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    return IDirectManipulationPrimaryContent_QueryInterface(&This->IDirectManipulationPrimaryContent_iface,
            riid, ppv);
}

static ULONG WINAPI content_AddRef(IDirectManipulationContent *iface)
{
    struct primarycontext *This = impl_from_IDirectManipulationContent(iface);
    return IDirectManipulationPrimaryContent_AddRef(&This->IDirectManipulationPrimaryContent_iface);
}

static ULONG WINAPI content_Release(IDirectManipulationContent *iface)
{
    struct primarycontext *This = impl_from_IDirectManipulationContent(iface);
    return IDirectManipulationPrimaryContent_Release(&This->IDirectManipulationPrimaryContent_iface);
}

static HRESULT WINAPI content_GetContentRect(IDirectManipulationContent *iface, RECT *size)
{
    struct primarycontext *This = impl_from_IDirectManipulationContent(iface);
    TRACE("%p, %p\n", This, size);
    if (!size) return E_POINTER;
    *size = This->rect;
    return S_OK;
}

static HRESULT WINAPI content_SetContentRect(IDirectManipulationContent *iface, const RECT *size)
{
    struct primarycontext *This = impl_from_IDirectManipulationContent(iface);
    TRACE("%p, %s\n", This, wine_dbgstr_rect(size));
    if (!size) return E_POINTER;
    This->rect = *size;
    return S_OK;
}

static HRESULT WINAPI content_GetViewport(IDirectManipulationContent *iface, REFIID riid, void **object)
{
    struct primarycontext *This = impl_from_IDirectManipulationContent(iface);
    TRACE("%p, %s, %p\n", This, debugstr_guid(riid), object);
    if (!object) return E_POINTER;
    *object = NULL;
    if (!This->viewport) return E_UNEXPECTED;
    return IDirectManipulationViewport2_QueryInterface(This->viewport, riid, object);
}

static HRESULT WINAPI content_GetTag(IDirectManipulationContent *iface, REFIID riid, void **object, UINT32 *id)
{
    struct primarycontext *This = impl_from_IDirectManipulationContent(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %s, %p, %p\n", This, debugstr_guid(riid), object, id);

    if (object)
    {
        *object = NULL;
        if (This->tag_object) hr = IUnknown_QueryInterface(This->tag_object, riid, object);
    }
    if (id) *id = This->tag_id;
    return hr;
}

static HRESULT WINAPI content_SetTag(IDirectManipulationContent *iface, IUnknown *object, UINT32 id)
{
    struct primarycontext *This = impl_from_IDirectManipulationContent(iface);
    TRACE("%p, %p, %u\n", This, object, id);

    if (object) IUnknown_AddRef(object);
    if (This->tag_object) IUnknown_Release(This->tag_object);
    This->tag_object = object;
    This->tag_id = id;
    return S_OK;
}

/* No manipulation ever runs here, so the content transform is the resting one: identity scale plus
 * the translation DirectManipulation applies for the content alignment when the content is smaller
 * than its viewport. XAML's ScrollViewer relies on this for HorizontalContentAlignment="Right"
 * (Calculator's result display) instead of arranging the content itself. */
static void primary_resting_transform(struct primarycontext *This, float *matrix)
{
    RECT vp;
    float vw, vh, cw, ch;

    memcpy(matrix, This->transform, sizeof(This->transform));
    if (!This->viewport || FAILED(IDirectManipulationViewport2_GetViewportRect(This->viewport, &vp))) return;
    vw = (float)(vp.right - vp.left);
    vh = (float)(vp.bottom - vp.top);
    cw = (float)(This->rect.right - This->rect.left) * matrix[0];
    ch = (float)(This->rect.bottom - This->rect.top) * matrix[3];
    /* Content that fits (or is empty: XAML sets a 0x0 content rect while the window is minimized)
     * cannot be scrolled: the offset is the alignment's, never a stale pan. XAML reads this
     * transform back and syncs it into This->transform, so a centring offset computed against
     * a tiny minimized viewport used to stick after the restore (the whole UI drawn at 640,389). */
    if (cw <= vw)
    {
        matrix[4] = 0.0f;
        if (cw > 0.0f && (This->horizontal_alignment & DIRECTMANIPULATION_HORIZONTALALIGNMENT_RIGHT)) matrix[4] = vw - cw;
        else if (cw > 0.0f && (This->horizontal_alignment & DIRECTMANIPULATION_HORIZONTALALIGNMENT_CENTER)) matrix[4] = (vw - cw) / 2.0f;
    }
    if (ch <= vh)
    {
        matrix[5] = 0.0f;
        if (ch > 0.0f && (This->vertical_alignment & DIRECTMANIPULATION_VERTICALALIGNMENT_BOTTOM)) matrix[5] = vh - ch;
        else if (ch > 0.0f && (This->vertical_alignment & DIRECTMANIPULATION_VERTICALALIGNMENT_CENTER)) matrix[5] = (vh - ch) / 2.0f;
    }
}

static HRESULT WINAPI content_GetOutputTransform(IDirectManipulationContent *iface,
                    float *matrix, DWORD count)
{
    struct primarycontext *This = impl_from_IDirectManipulationContent(iface);
    TRACE("%p, %p, %ld\n", This, matrix, count);
    if (!matrix || count != ARRAY_SIZE(This->transform)) return E_INVALIDARG;
    content_resting_transform(This, matrix);
    return S_OK;
}

static HRESULT WINAPI content_GetContentTransform(IDirectManipulationContent *iface,
                    float *matrix, DWORD count)
{
    struct primarycontext *This = impl_from_IDirectManipulationContent(iface);
    TRACE("%p, %p, %ld\n", This, matrix, count);
    if (!matrix || count != ARRAY_SIZE(This->transform)) return E_INVALIDARG;
    content_resting_transform(This, matrix);
    return S_OK;
}

static HRESULT WINAPI content_SyncContentTransform(IDirectManipulationContent *iface,
                    const float *matrix, DWORD count)
{
    struct primarycontext *This = impl_from_IDirectManipulationContent(iface);
    float applied[6];

    TRACE("%p, %p, %ld\n", This, matrix, count);
    if (!matrix || count != ARRAY_SIZE(This->transform)) return E_INVALIDARG;
    TRACE("content %p synced to scale %g,%g offset %g,%g\n", This, matrix[0], matrix[3], matrix[4], matrix[5]);
    memcpy(This->transform, matrix, sizeof(This->transform));
    /* XAML's ScrollViewer sets programmatic offsets (ChangeView, a CalendarView jumping to the current
     * month) this way and expects the compositor-side transform its visuals follow to move too. */
    content_resting_transform(This, applied);
    content_queue_update(This);
    return S_OK;
}

static const IDirectManipulationContentVtbl contentVtbl =
{
    content_QueryInterface,
    content_AddRef,
    content_Release,
    content_GetContentRect,
    content_SetContentRect,
    content_GetViewport,
    content_GetTag,
    content_SetTag,
    content_GetOutputTransform,
    content_GetContentTransform,
    content_SyncContentTransform
};

/* Viewports keep the state callers set and report it back. There is no manipulation engine behind them:
 * no input is ever captured, so a viewport only moves between ENABLED/READY and DISABLED, and event
 * handlers are held but never called. */
struct viewport_handler
{
    IDirectManipulationViewportEventHandler *handler;
    DWORD cookie;
};

struct viewport_behavior
{
    IUnknown *behavior;
    DWORD cookie;
};

struct directviewport
{
    IDirectManipulationViewport2 IDirectManipulationViewport2_iface;
    LONG ref;

    DIRECTMANIPULATION_STATUS status;
    IUnknown *tag_object;
    UINT32 tag_id;
    RECT rect;
    DIRECTMANIPULATION_VIEWPORT_OPTIONS options;
    DIRECTMANIPULATION_CONFIGURATION configurations;
    DIRECTMANIPULATION_CONFIGURATION active_configuration;
    DIRECTMANIPULATION_GESTURE_CONFIGURATION manual_gesture;
    DIRECTMANIPULATION_MOTION_TYPES chaining;
    DIRECTMANIPULATION_INPUT_MODE input_mode;
    DIRECTMANIPULATION_INPUT_MODE update_mode;
    float transform[6];
    BOOL mouse_focus;   /* XAML set DIRECTMANIPULATION_MOUSEFOCUS on it: the wheel belongs to this viewport */

    struct viewport_handler handlers[16];
    unsigned int handler_count;
    struct viewport_behavior behaviors[16];
    unsigned int behavior_count;
    DWORD next_cookie;
    struct primarycontext *primary;   /* created on the first GetPrimaryContent; holds no reference back */
    struct primarycontext *secondaries[16]; /* AddContent contents, no reference (they hold the viewport) */
    unsigned int secondary_count;
};

static inline struct directviewport *impl_from_IDirectManipulationViewport2(IDirectManipulationViewport2 *iface)
{
    return CONTAINING_RECORD(iface, struct directviewport, IDirectManipulationViewport2_iface);
}

static void viewport_track_secondary(struct directviewport *viewport, struct primarycontext *content, BOOL add)
{
    unsigned int i;
    for (i = 0; i < viewport->secondary_count; i++)
    {
        if (viewport->secondaries[i] != content) continue;
        if (!add) viewport->secondaries[i] = viewport->secondaries[--viewport->secondary_count];
        return;
    }
    if (add && content != viewport->primary && viewport->secondary_count < ARRAY_SIZE(viewport->secondaries))
        viewport->secondaries[viewport->secondary_count++] = content;
}

static void viewport_untrack_content(IDirectManipulationViewport2 *viewport, struct primarycontext *content)
{
    viewport_track_secondary(impl_from_IDirectManipulationViewport2(viewport), content, FALSE);
}

static void content_push_shared(struct primarycontext *This, const float *matrix)
{
    /* The compositor-side copy XAML's visuals follow. */
    if (This->shared_transform)
    {
        D2D_MATRIX_3X2_F m = { { { matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5] } } };
        IDCompositionMatrixTransform_SetMatrix(This->shared_transform, &m);
    }
}

static void content_resting_transform(struct primarycontext *This, float *matrix)
{
    struct directviewport *viewport = This->viewport ? impl_from_IDirectManipulationViewport2(This->viewport) : NULL;
    unsigned int i;

    if (viewport && viewport->primary && viewport->primary != This && (This->curve_x || This->curve_y))
    {
        /* Secondary content: its offset follows the curves of the primary's offset. XAML nests the visual
         * under the primary's, so the translation here is what cancels the primary's. */
        float primary[6];
        primary_resting_transform(viewport->primary, primary);
        memcpy(matrix, This->transform, sizeof(This->transform));
        if (This->curve_x) matrix[4] = -dm_curve_eval(This->curve_x, -primary[4]);
        if (This->curve_y) matrix[5] = -dm_curve_eval(This->curve_y, -primary[5]);
        TRACE("secondary %p offset %g,%g from primary offset %g,%g\n", This, matrix[4], matrix[5], -primary[4], -primary[5]);
        content_push_shared(This, matrix);
        return;
    }
    primary_resting_transform(This, matrix);
    content_push_shared(This, matrix);
    if (!viewport || viewport->primary != This) return;
    for (i = 0; i < viewport->secondary_count; i++)
    {
        float secondary[6];
        if (viewport->secondaries[i]->curve_x || viewport->secondaries[i]->curve_y)
            content_resting_transform(viewport->secondaries[i], secondary);
    }
}

static HRESULT WINAPI viewport_QueryInterface(IDirectManipulationViewport2 *iface, REFIID riid, void **ppv)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IDirectManipulationViewport) ||
        IsEqualGUID(riid, &IID_IDirectManipulationViewport2))
    {
        IDirectManipulationViewport2_AddRef(&This->IDirectManipulationViewport2_iface);
        *ppv = &This->IDirectManipulationViewport2_iface;
        return S_OK;
    }

    FIXME("(%p)->(%s,%p),not found\n", This, debugstr_guid(riid), ppv);
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI viewport_AddRef(IDirectManipulationViewport2 *iface)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);

    return ref;
}

static void viewport_release_handlers(struct directviewport *This)
{
    unsigned int i;

    for (i = 0; i < This->handler_count; i++)
        IDirectManipulationViewportEventHandler_Release(This->handlers[i].handler);
    This->handler_count = 0;
}

static void viewport_release_behaviors(struct directviewport *This)
{
    unsigned int i;

    for (i = 0; i < This->behavior_count; i++)
        IUnknown_Release(This->behaviors[i].behavior);
    This->behavior_count = 0;
}

static ULONG WINAPI viewport_Release(IDirectManipulationViewport2 *iface)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);

    if (!ref)
    {
        unsigned int i;
        for (i = 0; i < viewport_count; i++)
            if (viewports[i] == This) viewports[i] = viewports[--viewport_count];
        viewport_release_handlers(This);
        viewport_release_behaviors(This);
        if (This->tag_object) IUnknown_Release(This->tag_object);
        if (This->primary)
        {
            This->primary->viewport = NULL;
            IDirectManipulationPrimaryContent_Release(&This->primary->IDirectManipulationPrimaryContent_iface);
        }
        free(This);
    }
    return ref;
}

/* Enable/Disable status changes reach XAML later, from the UI thread's message loop, the way the
 * manipulation engine delivers them: XAML's CDirectManipulationService::EnableViewport re-enters
 * (Disable, Enable) when it sees the change synchronously and its ScrollViewer never gets to run the
 * BringIntoViewport it holds back until the viewport has become ready. */
static struct
{
    struct directviewport *viewport;
    DIRECTMANIPULATION_STATUS status, previous;
} pending_status[64];
static unsigned int pending_status_count;
static UINT_PTR pending_status_timer;

static void CALLBACK pending_status_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    unsigned int i, j, count = pending_status_count;
    struct { struct directviewport *viewport; DIRECTMANIPULATION_STATUS status, previous; } batch[64];

    KillTimer(NULL, id);
    pending_status_timer = 0;
    memcpy(batch, pending_status, count * sizeof(*batch));
    pending_status_count = 0;
    for (i = 0; i < count; i++)
    {
        struct directviewport *viewport = batch[i].viewport;
        TRACE("viewport %p status %d (was %d)\n", viewport, batch[i].status, batch[i].previous);
        for (j = 0; j < viewport->handler_count; j++)
            IDirectManipulationViewportEventHandler_OnViewportStatusChanged(viewport->handlers[j].handler,
                    (IDirectManipulationViewport *)&viewport->IDirectManipulationViewport2_iface,
                    batch[i].status, batch[i].previous);
        IDirectManipulationViewport2_Release(&viewport->IDirectManipulationViewport2_iface);
    }
}

static void viewport_queue_status(struct directviewport *viewport, DIRECTMANIPULATION_STATUS status)
{
    DIRECTMANIPULATION_STATUS previous = viewport->status;

    viewport->status = status;
    if (pending_status_count == ARRAY_SIZE(pending_status)) return;
    IDirectManipulationViewport2_AddRef(&viewport->IDirectManipulationViewport2_iface);
    pending_status[pending_status_count].viewport = viewport;
    pending_status[pending_status_count].status = status;
    pending_status[pending_status_count].previous = previous;
    pending_status_count++;
    if (!pending_status_timer) pending_status_timer = SetTimer(NULL, 0, 1, pending_status_timer_proc);
}

/* A programmatic content transform (XAML's SetPrimaryContentTransform / SyncContentTransform) is
 * answered the way the engine answers a finished ZoomToRect: RUNNING, OnContentUpdated and
 * OnViewportUpdated, READY, from the message loop. XAML's ScrollViewer only adopts the new offsets
 * from that OnContentUpdated; the Pivot's panel arranges its items around the adopted offset
 * (160000 into a 320000 wide extent), so without it the history/memory Pivot stayed empty. */
static struct primarycontext *pending_content[64];
static unsigned int pending_content_count;
static UINT_PTR pending_content_timer;
static void viewport_notify_status(struct directviewport *viewport, DIRECTMANIPULATION_STATUS status);

static void CALLBACK pending_content_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    unsigned int i, j, count = pending_content_count;
    struct primarycontext *batch[64];

    KillTimer(NULL, id);
    pending_content_timer = 0;
    memcpy(batch, pending_content, count * sizeof(*batch));
    pending_content_count = 0;
    for (i = 0; i < count; i++)
    {
        struct primarycontext *content = batch[i];
        struct directviewport *viewport = content->viewport ? impl_from_IDirectManipulationViewport2(content->viewport) : NULL;
        if (viewport)
        {
            TRACE("content %p of viewport %p updated to offset %g,%g\n", content, viewport, content->transform[4], content->transform[5]);
            IDirectManipulationViewport2_AddRef(&viewport->IDirectManipulationViewport2_iface);
            viewport_notify_status(viewport, DIRECTMANIPULATION_RUNNING);
            for (j = 0; j < viewport->handler_count; j++)
            {
                IDirectManipulationViewportEventHandler_OnContentUpdated(viewport->handlers[j].handler,
                        (IDirectManipulationViewport *)&viewport->IDirectManipulationViewport2_iface,
                        &content->IDirectManipulationContent_iface);
                IDirectManipulationViewportEventHandler_OnViewportUpdated(viewport->handlers[j].handler,
                        (IDirectManipulationViewport *)&viewport->IDirectManipulationViewport2_iface);
            }
            if (viewport->status == DIRECTMANIPULATION_RUNNING) viewport_notify_status(viewport, DIRECTMANIPULATION_READY);
            IDirectManipulationViewport2_Release(&viewport->IDirectManipulationViewport2_iface);
        }
        IDirectManipulationPrimaryContent_Release(&content->IDirectManipulationPrimaryContent_iface);
    }
}

static void content_queue_update(struct primarycontext *content)
{
    unsigned int i;
    for (i = 0; i < pending_content_count; i++) if (pending_content[i] == content) return;
    if (pending_content_count == ARRAY_SIZE(pending_content)) return;
    IDirectManipulationPrimaryContent_AddRef(&content->IDirectManipulationPrimaryContent_iface);
    pending_content[pending_content_count++] = content;
    if (!pending_content_timer) pending_content_timer = SetTimer(NULL, 0, 1, pending_content_timer_proc);
}

static HRESULT WINAPI viewport_Enable(IDirectManipulationViewport2 *iface)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p\n", This);
    /* An idle enabled viewport reports READY, which is what XAML checks before handing input over.
     * The status change is also raised on the handlers: XAML's CDMViewport tracks the status from
     * OnViewportStatusChanged and holds programmatic scrolls (a CalendarView jumping to the current
     * month) until it has seen the viewport become ready. */
    if (This->status == DIRECTMANIPULATION_BUILDING || This->status == DIRECTMANIPULATION_DISABLED)
        viewport_queue_status(This, DIRECTMANIPULATION_READY);
    return S_OK;
}

static HRESULT WINAPI viewport_Disable(IDirectManipulationViewport2 *iface)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p\n", This);
    if (This->status != DIRECTMANIPULATION_DISABLED) viewport_queue_status(This, DIRECTMANIPULATION_DISABLED);
    return S_OK;
}

/* DIRECTMANIPULATION_MOUSEFOCUS from directmanipulation.h, which our IDL does not define */
#define DM_MOUSEFOCUS 0xfffffffd

/* XAML hit-tests a wheel event itself, then brackets ProcessInput with SetContact(MOUSEFOCUS) and
 * ReleaseContact on the ScrollViewer's viewport under the cursor. That is the only reliable way to
 * know which viewport it means: their rects are all (0,0)-based, in each element's own space. */
static HRESULT WINAPI viewport_SetContact(IDirectManipulationViewport2 *iface, UINT32 id)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %u\n", This, id);
    if (id == DM_MOUSEFOCUS) This->mouse_focus = TRUE;
    return S_OK;
}

static HRESULT WINAPI viewport_ReleaseContact(IDirectManipulationViewport2 *iface, UINT32 id)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %u\n", This, id);
    if (id == DM_MOUSEFOCUS) This->mouse_focus = FALSE;
    return S_OK;
}

static HRESULT WINAPI viewport_ReleaseAllContacts(IDirectManipulationViewport2 *iface)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p\n", This);
    This->mouse_focus = FALSE;
    return S_OK;
}

static HRESULT WINAPI viewport_GetStatus(IDirectManipulationViewport2 *iface, DIRECTMANIPULATION_STATUS *status)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %p\n", This, status);
    if (!status) return E_POINTER;
    *status = This->status;
    return S_OK;
}

static HRESULT WINAPI viewport_GetTag(IDirectManipulationViewport2 *iface, REFIID riid, void **object, UINT32 *id)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %s, %p, %p\n", This, debugstr_guid(riid), object, id);

    if (object)
    {
        *object = NULL;
        if (This->tag_object) hr = IUnknown_QueryInterface(This->tag_object, riid, object);
    }
    if (id) *id = This->tag_id;
    return hr;
}

static HRESULT WINAPI viewport_SetTag(IDirectManipulationViewport2 *iface, IUnknown *object, UINT32 id)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %p, %u\n", This, object, id);

    if (object) IUnknown_AddRef(object);
    if (This->tag_object) IUnknown_Release(This->tag_object);
    This->tag_object = object;
    This->tag_id = id;
    return S_OK;
}

static HRESULT WINAPI viewport_GetViewportRect(IDirectManipulationViewport2 *iface, RECT *viewport)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %p\n", This, viewport);
    if (!viewport) return E_POINTER;
    *viewport = This->rect;
    return S_OK;
}

static HRESULT WINAPI viewport_SetViewportRect(IDirectManipulationViewport2 *iface, const RECT *viewport)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %s\n", This, wine_dbgstr_rect(viewport));
    if (!viewport) return E_POINTER;
    This->rect = *viewport;
    return S_OK;
}

static HRESULT WINAPI viewport_ZoomToRect(IDirectManipulationViewport2 *iface, const float left,
                    const float top, const float right, const float bottom, BOOL animate)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    FIXME("%p, %f, %f, %f, %f, %d: not zooming\n", This, left, top, right, bottom, animate);
    return S_OK;
}

static HRESULT WINAPI viewport_SetViewportTransform(IDirectManipulationViewport2 *iface,
                    const float *matrix, DWORD count)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %p, %ld\n", This, matrix, count);
    if (!matrix || count != ARRAY_SIZE(This->transform)) return E_INVALIDARG;
    memcpy(This->transform, matrix, sizeof(This->transform));
    return S_OK;
}

static HRESULT WINAPI viewport_SyncDisplayTransform(IDirectManipulationViewport2 *iface,
                    const float *matrix, DWORD count)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %p, %ld\n", This, matrix, count);
    if (!matrix || count != ARRAY_SIZE(This->transform)) return E_INVALIDARG;
    return S_OK;
}

/* Content objects are shared between the viewport's primary content and the secondary contents
 * IDirectManipulationManager::CreateContent hands out; the viewport is NULL until AddContent. */
static HRESULT create_content(IDirectManipulationViewport2 *viewport, REFIID riid, void **object)
{
    struct primarycontext *primary;

    if (!IsEqualGUID(riid, &IID_IDirectManipulationPrimaryContent) &&
        !IsEqualGUID(riid, &IID_IDirectManipulationContent))
    {
        FIXME("Unsupported interface %s\n", debugstr_guid(riid));
        return E_NOTIMPL;
    }
    if (!(primary = calloc(1, sizeof(*primary))))
        return E_OUTOFMEMORY;

    primary->IDirectManipulationPrimaryContent_iface.lpVtbl = &primaryVtbl;
    primary->private_vtbl = primary_private_vtbl;
    primary->curves_vtbl = content_curves_vtbl;
    primary->IDirectManipulationContent_iface.lpVtbl = &contentVtbl;
    primary->ref = 1;
    if ((primary->viewport = viewport)) IDirectManipulationViewport2_AddRef(viewport);
    primary->transform[0] = primary->transform[3] = 1.0f;
    primary->zoom_min = 1.0f;
    primary->zoom_max = 1.0f;

    if (IsEqualGUID(riid, &IID_IDirectManipulationContent))
        *object = &primary->IDirectManipulationContent_iface;
    else
        *object = &primary->IDirectManipulationPrimaryContent_iface;
    return S_OK;
}

/* The primary content is one object per viewport: XAML sets its alignment and content rect on the
 * object it gets first and reads the transform back through later GetPrimaryContent calls. */
static HRESULT WINAPI viewport_GetPrimaryContent(IDirectManipulationViewport2 *iface, REFIID riid, void **object)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    HRESULT hr;
    TRACE("%p, %s, %p\n", This, debugstr_guid(riid), object);
    if (!object) return E_POINTER;
    if (!This->primary)
    {
        void *content;
        if (FAILED(hr = create_content(iface, &IID_IDirectManipulationPrimaryContent, &content))) return hr;
        This->primary = impl_from_IDirectManipulationPrimaryContent(content);
        IDirectManipulationViewport2_Release(iface); /* the viewport owns the content; no cycle */
    }
    return IDirectManipulationPrimaryContent_QueryInterface(&This->primary->IDirectManipulationPrimaryContent_iface, riid, object);
}

/* Secondary contents come from IDirectManipulationManager::CreateContent without a viewport; AddContent gives
 * them one, which IDirectManipulationContent::GetViewport needs (XAML reads it from the render walk). */
static HRESULT WINAPI viewport_AddContent(IDirectManipulationViewport2 *iface, IDirectManipulationContent *content)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    struct primarycontext *primary;

    TRACE("%p, %p\n", This, content);
    if (!content) return E_POINTER;
    if (content->lpVtbl != &contentVtbl) { FIXME("%p: foreign content %p\n", This, content); return S_OK; }
    primary = impl_from_IDirectManipulationContent(content);
    if (primary->viewport != iface)
    {
        IDirectManipulationViewport2_AddRef(iface);
        if (primary->viewport) IDirectManipulationViewport2_Release(primary->viewport);
        primary->viewport = iface;
    }
    viewport_track_secondary(This, primary, TRUE);
    return S_OK;
}

static HRESULT WINAPI viewport_RemoveContent(IDirectManipulationViewport2 *iface, IDirectManipulationContent *content)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    struct primarycontext *primary;

    TRACE("%p, %p\n", This, content);
    if (!content) return E_POINTER;
    if (content->lpVtbl != &contentVtbl) return S_OK;
    primary = impl_from_IDirectManipulationContent(content);
    viewport_track_secondary(This, primary, FALSE);
    if (primary->viewport == iface)
    {
        primary->viewport = NULL;
        IDirectManipulationViewport2_Release(iface);
    }
    return S_OK;
}

static HRESULT WINAPI viewport_SetViewportOptions(IDirectManipulationViewport2 *iface, DIRECTMANIPULATION_VIEWPORT_OPTIONS options)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %#x\n", This, options);
    This->options = options;
    return S_OK;
}

static HRESULT WINAPI viewport_AddConfiguration(IDirectManipulationViewport2 *iface, DIRECTMANIPULATION_CONFIGURATION configuration)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %#x\n", This, configuration);
    This->configurations |= configuration;
    return S_OK;
}

static HRESULT WINAPI viewport_RemoveConfiguration(IDirectManipulationViewport2 *iface, DIRECTMANIPULATION_CONFIGURATION configuration)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %#x\n", This, configuration);
    This->configurations &= ~configuration;
    return S_OK;
}

static HRESULT WINAPI viewport_ActivateConfiguration(IDirectManipulationViewport2 *iface, DIRECTMANIPULATION_CONFIGURATION configuration)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %#x\n", This, configuration);
    This->active_configuration = configuration;
    return S_OK;
}

static HRESULT WINAPI viewport_SetManualGesture(IDirectManipulationViewport2 *iface, DIRECTMANIPULATION_GESTURE_CONFIGURATION configuration)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %#x\n", This, configuration);
    This->manual_gesture = configuration;
    return S_OK;
}

static HRESULT WINAPI viewport_SetChaining(IDirectManipulationViewport2 *iface, DIRECTMANIPULATION_MOTION_TYPES enabledTypes)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %#x\n", This, enabledTypes);
    This->chaining = enabledTypes;
    return S_OK;
}

static HRESULT WINAPI viewport_AddEventHandler(IDirectManipulationViewport2 *iface, HWND window,
                    IDirectManipulationViewportEventHandler *eventHandler, DWORD *cookie)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %p, %p, %p\n", This, window, eventHandler, cookie);

    if (!eventHandler || !cookie) return E_POINTER;
    if (This->handler_count == ARRAY_SIZE(This->handlers)) return E_OUTOFMEMORY;
    IDirectManipulationViewportEventHandler_AddRef(eventHandler);
    This->handlers[This->handler_count].handler = eventHandler;
    This->handlers[This->handler_count].cookie = *cookie = ++This->next_cookie;
    This->handler_count++;
    return S_OK;
}

static HRESULT WINAPI viewport_RemoveEventHandler(IDirectManipulationViewport2 *iface, DWORD cookie)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    unsigned int i;

    TRACE("%p, %ld\n", This, cookie);

    for (i = 0; i < This->handler_count; i++)
    {
        if (This->handlers[i].cookie != cookie) continue;
        IDirectManipulationViewportEventHandler_Release(This->handlers[i].handler);
        This->handlers[i] = This->handlers[--This->handler_count];
        return S_OK;
    }
    return E_INVALIDARG;
}

static HRESULT WINAPI viewport_SetInputMode(IDirectManipulationViewport2 *iface, DIRECTMANIPULATION_INPUT_MODE mode)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %d\n", This, mode);
    This->input_mode = mode;
    return S_OK;
}

static HRESULT WINAPI viewport_SetUpdateMode(IDirectManipulationViewport2 *iface, DIRECTMANIPULATION_INPUT_MODE mode)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %d\n", This, mode);
    This->update_mode = mode;
    return S_OK;
}

static HRESULT WINAPI viewport_Stop(IDirectManipulationViewport2 *iface)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p\n", This);
    if (This->status != DIRECTMANIPULATION_DISABLED && This->status != DIRECTMANIPULATION_BUILDING)
        This->status = DIRECTMANIPULATION_READY;
    return S_OK;
}

static HRESULT WINAPI viewport_Abandon(IDirectManipulationViewport2 *iface)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p\n", This);
    viewport_release_handlers(This);
    viewport_release_behaviors(This);
    This->status = DIRECTMANIPULATION_DISABLED;
    return S_OK;
}

static HRESULT WINAPI viewport_AddBehavior(IDirectManipulationViewport2 *iface, IUnknown *behavior, DWORD *cookie)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p, %p, %p\n", This, behavior, cookie);

    if (!behavior || !cookie) return E_POINTER;
    if (This->behavior_count == ARRAY_SIZE(This->behaviors)) return E_OUTOFMEMORY;
    IUnknown_AddRef(behavior);
    This->behaviors[This->behavior_count].behavior = behavior;
    This->behaviors[This->behavior_count].cookie = *cookie = ++This->next_cookie;
    This->behavior_count++;
    return S_OK;
}

static HRESULT WINAPI viewport_RemoveBehavior(IDirectManipulationViewport2 *iface, DWORD cookie)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    unsigned int i;

    TRACE("%p, %ld\n", This, cookie);

    for (i = 0; i < This->behavior_count; i++)
    {
        if (This->behaviors[i].cookie != cookie) continue;
        IUnknown_Release(This->behaviors[i].behavior);
        This->behaviors[i] = This->behaviors[--This->behavior_count];
        return S_OK;
    }
    return E_INVALIDARG;
}

static HRESULT WINAPI viewport_RemoveAllBehaviors(IDirectManipulationViewport2 *iface)
{
    struct directviewport *This = impl_from_IDirectManipulationViewport2(iface);
    TRACE("%p\n", This);
    viewport_release_behaviors(This);
    return S_OK;
}

static const IDirectManipulationViewport2Vtbl viewportVtbl =
{
    viewport_QueryInterface,
    viewport_AddRef,
    viewport_Release,
    viewport_Enable,
    viewport_Disable,
    viewport_SetContact,
    viewport_ReleaseContact,
    viewport_ReleaseAllContacts,
    viewport_GetStatus,
    viewport_GetTag,
    viewport_SetTag,
    viewport_GetViewportRect,
    viewport_SetViewportRect,
    viewport_ZoomToRect,
    viewport_SetViewportTransform,
    viewport_SyncDisplayTransform,
    viewport_GetPrimaryContent,
    viewport_AddContent,
    viewport_RemoveContent,
    viewport_SetViewportOptions,
    viewport_AddConfiguration,
    viewport_RemoveConfiguration,
    viewport_ActivateConfiguration,
    viewport_SetManualGesture,
    viewport_SetChaining,
    viewport_AddEventHandler,
    viewport_RemoveEventHandler,
    viewport_SetInputMode,
    viewport_SetUpdateMode,
    viewport_Stop,
    viewport_Abandon,
    viewport_AddBehavior,
    viewport_RemoveBehavior,
    viewport_RemoveAllBehaviors
};

static HRESULT create_viewport(IDirectManipulationViewport2 **obj)
{
    struct directviewport *object;

    object = calloc(1, sizeof(*object));
    if(!object)
        return E_OUTOFMEMORY;

    object->IDirectManipulationViewport2_iface.lpVtbl = &viewportVtbl;
    object->ref = 1;
    object->status = DIRECTMANIPULATION_BUILDING;
    object->input_mode = DIRECTMANIPULATION_INPUT_MODE_AUTOMATIC;
    object->update_mode = DIRECTMANIPULATION_INPUT_MODE_AUTOMATIC;
    object->transform[0] = object->transform[3] = 1.0f;
    if (viewport_count < ARRAY_SIZE(viewports)) viewports[viewport_count++] = object;

    *obj = &object->IDirectManipulationViewport2_iface;

    return S_OK;
}

static HRESULT WINAPI direct_manip_QueryInterface(IDirectManipulationManager2 *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IDirectManipulationManager) ||
        IsEqualGUID(riid, &IID_IDirectManipulationManager2)) {
        IUnknown_AddRef(iface);
        *ppv = iface;
        return S_OK;
    }

    FIXME("(%p)->(%s,%p),not found\n", iface, debugstr_guid(riid), ppv);
    return E_NOINTERFACE;
}

static ULONG WINAPI direct_manip_AddRef(IDirectManipulationManager2 *iface)
{
    struct directmanipulation *This = impl_from_IDirectManipulationManager2(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);

    return ref;
}

static ULONG WINAPI direct_manip_Release(IDirectManipulationManager2 *iface)
{
    struct directmanipulation *This = impl_from_IDirectManipulationManager2(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);

    if (!ref)
    {
        if(This->updatemanager)
            IDirectManipulationUpdateManager_Release(This->updatemanager);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI direct_manip_Activate(IDirectManipulationManager2 *iface, HWND window)
{
    struct directmanipulation *This = impl_from_IDirectManipulationManager2(iface);
    TRACE("%p, %p: no separate manipulation engine to activate.\n", This, window);
    This->window = window;
    return S_OK;
}

static void viewport_notify_status(struct directviewport *viewport, DIRECTMANIPULATION_STATUS status)
{
    DIRECTMANIPULATION_STATUS previous = viewport->status;
    unsigned int i;

    viewport->status = status;
    for (i = 0; i < viewport->handler_count; i++)
        IDirectManipulationViewportEventHandler_OnViewportStatusChanged(viewport->handlers[i].handler,
                (IDirectManipulationViewport *)&viewport->IDirectManipulationViewport2_iface, status, previous);
}

/* Mouse wheel over a viewport: move the primary content by whole notches, clamped to the content, and tell the
 * viewport's handlers (XAML's ScrollViewer) that the content moved, the way the manipulation engine would after
 * an instant scroll. 48 pixels per notch is what XAML's own wheel handling scrolls. */
static BOOL manager_process_wheel(struct directmanipulation *This, const MSG *msg)
{
    struct directviewport *best = NULL;
    struct primarycontext *content;
    POINT pt = { (short)LOWORD(msg->lParam), (short)HIWORD(msg->lParam) };
    int delta = (short)HIWORD(msg->wParam), notches;
    BOOL horizontal = msg->message == WM_MOUSEHWHEEL;
    float vw, vh, cw, ch, min_x, min_y, x, y;
    RECT vp;
    unsigned int i;
    BOOL have_focus = FALSE;

    if (msg->message == WM_POINTERWHEEL || msg->message == WM_POINTERHWHEEL) horizontal = msg->message == WM_POINTERHWHEEL;
    if (!delta) return FALSE;
    ScreenToClient(This->window ? This->window : msg->hwnd, &pt);
    for (i = 0; i < viewport_count; i++) if (viewports[i]->mouse_focus) have_focus = TRUE;

    for (i = 0; i < viewport_count; i++)
    {
        struct directviewport *v = viewports[i];
        if (v->status != DIRECTMANIPULATION_READY || !v->primary) continue;
        /* the viewport XAML named with SetContact(MOUSEFOCUS) wins outright */
        if (!v->mouse_focus && (have_focus || !PtInRect(&v->rect, pt))) continue;
        if (!(v->active_configuration & (horizontal ? DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_X
                                                    : DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_Y))) continue;
        vw = (float)(v->rect.right - v->rect.left);
        vh = (float)(v->rect.bottom - v->rect.top);
        cw = (float)(v->primary->rect.right - v->primary->rect.left) * v->primary->transform[0];
        ch = (float)(v->primary->rect.bottom - v->primary->rect.top) * v->primary->transform[3];
        if (horizontal ? cw <= vw + 0.5f : ch <= vh + 0.5f) continue;   /* nothing to scroll */
        if (v->mouse_focus)
        {
            best = v;
            break;
        }
        /* otherwise the innermost viewport wins */
        if (!best || (v->rect.right - v->rect.left) * (v->rect.bottom - v->rect.top) <
                     (best->rect.right - best->rect.left) * (best->rect.bottom - best->rect.top))
            best = v;
    }
    if (!best) return FALSE;

    content = best->primary;
    vp = best->rect;
    vw = (float)(vp.right - vp.left);
    vh = (float)(vp.bottom - vp.top);
    cw = (float)(content->rect.right - content->rect.left) * content->transform[0];
    ch = (float)(content->rect.bottom - content->rect.top) * content->transform[3];
    min_x = cw > vw ? vw - cw : 0.0f;
    min_y = ch > vh ? vh - ch : 0.0f;
    notches = delta / WHEEL_DELTA;
    if (!notches) notches = delta > 0 ? 1 : -1;
    x = content->transform[4];
    y = content->transform[5];
    if (horizontal) x += notches * 48.0f;
    else y += notches * 48.0f;
    if (x < min_x) x = min_x;
    if (x > 0.0f) x = 0.0f;
    if (y < min_y) y = min_y;
    if (y > 0.0f) y = 0.0f;
    if (x == content->transform[4] && y == content->transform[5]) return TRUE;
    content->transform[4] = x;
    content->transform[5] = y;
    TRACE("viewport %p scrolled to %g,%g (content %gx%g in %gx%g)\n", best, x, y, cw, ch, vw, vh);

    IDirectManipulationViewport2_AddRef(&best->IDirectManipulationViewport2_iface);
    viewport_notify_status(best, DIRECTMANIPULATION_RUNNING);
    for (i = 0; i < best->handler_count; i++)
    {
        IDirectManipulationViewportEventHandler_OnContentUpdated(best->handlers[i].handler,
                (IDirectManipulationViewport *)&best->IDirectManipulationViewport2_iface,
                &content->IDirectManipulationContent_iface);
        IDirectManipulationViewportEventHandler_OnViewportUpdated(best->handlers[i].handler,
                (IDirectManipulationViewport *)&best->IDirectManipulationViewport2_iface);
    }
    if (best->status == DIRECTMANIPULATION_RUNNING) viewport_notify_status(best, DIRECTMANIPULATION_READY);
    IDirectManipulationViewport2_Release(&best->IDirectManipulationViewport2_iface);
    return TRUE;
}

/* The counterpart of Activate: there is no separate manipulation engine to stop, but failing this
 * throws - XAML tears the manipulation of a page down when navigating away, and the E_NOTIMPL came
 * back out of Calculator as an unhandled error while switching to the Programmer page. */
static HRESULT WINAPI direct_manip_Deactivate(IDirectManipulationManager2 *iface, HWND window)
{
    struct directmanipulation *This = impl_from_IDirectManipulationManager2(iface);

    TRACE("%p, %p\n", This, window);
    if (This->window == window) This->window = NULL;
    return S_OK;
}

/* There is no separate hit-test target to register: input reaches the viewports through ProcessInput.
 * Failing this would fail-fast inside XAML's tick like the other manager methods. */
static HRESULT WINAPI direct_manip_RegisterHitTestTarget(IDirectManipulationManager2 *iface, HWND window,
                HWND hittest, DIRECTMANIPULATION_HITTEST_TYPE type)
{
    struct directmanipulation *This = impl_from_IDirectManipulationManager2(iface);

    TRACE("%p, %p, %p, %d: ignored\n", This, window, hittest, type);
    return S_OK;
}

static HRESULT WINAPI direct_manip_ProcessInput(IDirectManipulationManager2 *iface, const MSG *msg, BOOL *handled)
{
    struct directmanipulation *This = impl_from_IDirectManipulationManager2(iface);
    BOOL done = FALSE;

    TRACE("%p, %p (message %#x), %p\n", This, msg, msg ? msg->message : 0, handled);

    if (!msg) return E_POINTER;
    switch (msg->message)
    {
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_POINTERWHEEL:
    case WM_POINTERHWHEEL:
        done = manager_process_wheel(This, msg);
        break;
    default:
        break;
    }
    if (handled) *handled = done;
    return S_OK;
}

static HRESULT WINAPI direct_manip_GetUpdateManager(IDirectManipulationManager2 *iface, REFIID riid, void **obj)
{
    struct directmanipulation *This = impl_from_IDirectManipulationManager2(iface);
    HRESULT hr = E_FAIL;

    TRACE("%p, %s, %p\n", This, debugstr_guid(riid), obj);

    *obj = NULL;
    if(IsEqualGUID(riid, &IID_IDirectManipulationUpdateManager))
    {
        if(!This->updatemanager)
        {
            hr = create_update_manager(&This->updatemanager);
        }
        else
        {
            hr = S_OK;
        }

        if(hr == S_OK)
        {
            IDirectManipulationUpdateManager_AddRef(This->updatemanager);
            *obj = This->updatemanager;
        }
    }
    else
        FIXME("Interface %s currently not supported.\n", debugstr_guid(riid));

    return hr;
}

static HRESULT WINAPI direct_manip_CreateViewport(IDirectManipulationManager2 *iface, IDirectManipulationFrameInfoProvider *frame,
                HWND window, REFIID riid, void **obj)
{
    HRESULT hr = E_NOTIMPL;
    struct directmanipulation *This = impl_from_IDirectManipulationManager2(iface);
    TRACE("%p, %p, %p, %s, %p\n", This, frame, window, debugstr_guid(riid), obj);

    if(IsEqualGUID(riid, &IID_IDirectManipulationViewport) ||
       IsEqualGUID(riid, &IID_IDirectManipulationViewport2) )
    {
        hr = create_viewport( (IDirectManipulationViewport2**)obj);
    }
    else
        FIXME("Unsupported interface %s\n", debugstr_guid(riid));
    return hr;
}

static HRESULT WINAPI direct_manip_CreateContent(IDirectManipulationManager2 *iface, IDirectManipulationFrameInfoProvider *frame,
                REFCLSID clsid, REFIID riid, void **obj)
{
    struct directmanipulation *This = impl_from_IDirectManipulationManager2(iface);

    /* XAML creates secondary contents (scroll indicators, headers) for every ScrollViewer from the UI thread
     * tick. E_NOTIMPL here fail-fasts inside that tick, which then never ends and no frame is rendered again. */
    TRACE("%p, %p, %s, %s, %p: content class not distinguished\n", This, frame, debugstr_guid(clsid), debugstr_guid(riid), obj);
    return create_content(NULL, riid, obj);
}

static HRESULT WINAPI direct_manip_CreateBehavior(IDirectManipulationManager2 *iface, REFCLSID clsid, REFIID riid, void **obj)
{
    struct directmanipulation *This = impl_from_IDirectManipulationManager2(iface);
    FIXME("%p, %s,  %s, %p\n", This, debugstr_guid(clsid), debugstr_guid(riid), obj);
    return E_NOTIMPL;
}

static const struct IDirectManipulationManager2Vtbl directmanipVtbl =
{
    direct_manip_QueryInterface,
    direct_manip_AddRef,
    direct_manip_Release,
    direct_manip_Activate,
    direct_manip_Deactivate,
    direct_manip_RegisterHitTestTarget,
    direct_manip_ProcessInput,
    direct_manip_GetUpdateManager,
    direct_manip_CreateViewport,
    direct_manip_CreateContent,
    direct_manip_CreateBehavior
};

static HRESULT WINAPI DirectManipulation_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    struct directmanipulation *object;
    HRESULT ret;

    TRACE("(%p, %s, %p)\n", outer, debugstr_guid(riid), ppv);

    *ppv = NULL;

    object = calloc(1, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;

    object->IDirectManipulationManager2_iface.lpVtbl = &directmanipVtbl;
    object->ref = 1;

    ret = direct_manip_QueryInterface(&object->IDirectManipulationManager2_iface, riid, ppv);
    direct_manip_Release(&object->IDirectManipulationManager2_iface);

    return ret;
}

struct directcompositor
{
    IDirectManipulationCompositor2 IDirectManipulationCompositor2_iface;
    IDirectManipulationFrameInfoProvider IDirectManipulationFrameInfoProvider_iface;
    const void **interactions_vtbl; /* {59f4c99d} view, see compositor_interactions_vtbl */
    IDirectManipulationUpdateManager *manager;
    LONG ref;
};

static inline struct directcompositor *impl_from_IDirectManipulationCompositor2(IDirectManipulationCompositor2 *iface)
{
    return CONTAINING_RECORD(iface, struct directcompositor, IDirectManipulationCompositor2_iface);
}

static inline struct directcompositor *impl_from_IDirectManipulationFrameInfoProvider(IDirectManipulationFrameInfoProvider *iface)
{
    return CONTAINING_RECORD(iface, struct directcompositor, IDirectManipulationFrameInfoProvider_iface);
}

/* {59f4c99d-084e-46b9-ad3b-c9776f453163}: private compositor interface XAML uses for DComp-side "viewport
 * interactions": slot 3 CreateViewportInteraction(viewport, IDCompositionDesktopDevicePartner, IUnknown **),
 * slot 4 CreateViewportInteractionForRootVisual(visual, device partner, IUnknown **), slot 5 RemoveViewportInteraction. The interaction object only travels
 * back into dcomp's interop visuals, which ignore it; a refcounted stub is enough. XAML fail-fasts on
 * E_NOINTERFACE here from the render walk, and the tick that hosts it never ends. */
/* IID_IDCompositionDevice; no import library carries it. */
static const GUID IID_dcomp_device = { 0xc37ea93a, 0xe7aa, 0x450d, { 0xb1, 0x6f, 0x97, 0x46, 0xcb, 0x04, 0x07, 0xf3 } };
static const GUID IID_compositor_interactions = { 0x59f4c99d, 0x084e, 0x46b9, { 0xad, 0x3b, 0xc9, 0x77, 0x6f, 0x45, 0x31, 0x63 } };

static ULONG WINAPI compositor_AddRef(IDirectManipulationCompositor2 *iface);
static ULONG WINAPI compositor_Release(IDirectManipulationCompositor2 *iface);
static struct directcompositor *impl_from_interactions(void *iface)
{
    return CONTAINING_RECORD(iface, struct directcompositor, interactions_vtbl);
}
static HRESULT WINAPI interactions_QueryInterface(void *iface, REFIID riid, void **ppv)
{
    return IDirectManipulationCompositor2_QueryInterface(&impl_from_interactions(iface)->IDirectManipulationCompositor2_iface, riid, ppv);
}
static ULONG WINAPI interactions_AddRef(void *iface)
{
    return compositor_AddRef(&impl_from_interactions(iface)->IDirectManipulationCompositor2_iface);
}
static ULONG WINAPI interactions_Release(void *iface)
{
    return compositor_Release(&impl_from_interactions(iface)->IDirectManipulationCompositor2_iface);
}
static HRESULT create_stub_interaction(void **out)
{
    struct dm_curve *obj;
    if (!out) return E_POINTER;
    if (!(obj = calloc(1, sizeof(*obj)))) return E_OUTOFMEMORY;
    obj->vtbl = dm_curve_vtbl;
    obj->ref = 1;
    *out = obj;
    return S_OK;
}
static HRESULT WINAPI interactions_CreateViewportInteraction(void *iface, void *viewport, void *device, void **out)
{
    TRACE("%p, %p, %p, %p: stub interaction\n", impl_from_interactions(iface), viewport, device, out);
    return create_stub_interaction(out);
}
/* Slot 4 hands XAML an IDCompositionTransform for a content (CreateSharedContentTransformForContent,
 * CreateViewportInteractionForRootVisual); XAML feeds it to DManipDataWinRT::SetSharedContentTransforms and
 * dcomp visuals, so it has to be a real dcomp transform of the device partner passed in. */
static HRESULT WINAPI interactions_CreateSharedContentTransform(void *iface, void *content, IUnknown *device, void **out)
{
    IDCompositionDevice *dcomp_device;
    IDCompositionMatrixTransform *transform;
    HRESULT hr;

    TRACE("%p, %p, %p, %p\n", impl_from_interactions(iface), content, device, out);
    if (!out) return E_POINTER;
    *out = NULL;
    if (!device) return E_INVALIDARG;
    {
        /* Wine's dcomp: ask for its own matrix transform, which the fallback painter can read back.
         * The device XAML passes is the desktop device partner, whose table is not IDCompositionDevice's. */
        HRESULT (WINAPI *create)(void **) = (void *)GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "__wine_dcomp_create_matrix_transform");
        if (create) hr = create((void **)&transform);
        else if (SUCCEEDED(hr = IUnknown_QueryInterface(device, &IID_dcomp_device, (void **)&dcomp_device)))
        {
            hr = IDCompositionDevice_CreateMatrixTransform(dcomp_device, &transform);
            IDCompositionDevice_Release(dcomp_device);
        }
        else
        {
            FIXME("device %p is not an IDCompositionDevice, hr %#lx; handing out a stub\n", device, hr);
            return create_stub_interaction(out);
        }
    }
    if (FAILED(hr)) return hr;
    {
        IDirectManipulationContent *ours = NULL;
        /* XAML passes whichever view of the content it holds; find our object through QI. */
        if (content && SUCCEEDED(IUnknown_QueryInterface((IUnknown *)content, &IID_IDirectManipulationContent, (void **)&ours)))
        {
            if (ours->lpVtbl == &contentVtbl)
            {
                struct primarycontext *primary = impl_from_IDirectManipulationContent(ours);
                float matrix[6];
                if (primary->shared_transform) IDCompositionMatrixTransform_Release(primary->shared_transform);
                IDCompositionMatrixTransform_AddRef((primary->shared_transform = transform));
                content_resting_transform(primary, matrix);
                TRACE("content %p follows shared transform %p\n", primary, transform);
            }
            IDirectManipulationContent_Release(ours);
        }
    }
    *out = transform;
    return hr;
}
#define INTERACTIONS_STUB(n) static HRESULT WINAPI interactions_##n(void *iface) \
    { TRACE("%p: interactions slot %d, not applied\n", impl_from_interactions(iface), n); return S_OK; }
INTERACTIONS_STUB(5) INTERACTIONS_STUB(6) INTERACTIONS_STUB(7) INTERACTIONS_STUB(8) INTERACTIONS_STUB(9)
INTERACTIONS_STUB(10) INTERACTIONS_STUB(11) INTERACTIONS_STUB(12) INTERACTIONS_STUB(13) INTERACTIONS_STUB(14) INTERACTIONS_STUB(15)
#undef INTERACTIONS_STUB
static const void *compositor_interactions_vtbl[] =
{
    interactions_QueryInterface, interactions_AddRef, interactions_Release,
    interactions_CreateViewportInteraction, interactions_CreateSharedContentTransform, interactions_5, interactions_6,
    interactions_7, interactions_8, interactions_9, interactions_10, interactions_11, interactions_12,
    interactions_13, interactions_14, interactions_15,
};

static HRESULT WINAPI compositor_QueryInterface(IDirectManipulationCompositor2 *iface, REFIID riid, void **ppv)
{
    struct directcompositor *This = impl_from_IDirectManipulationCompositor2(iface);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IDirectManipulationCompositor) ||
        IsEqualGUID(riid, &IID_IDirectManipulationCompositor2))
    {
        IUnknown_AddRef(iface);
        *ppv = iface;
        return S_OK;
    }
    else if(IsEqualGUID(riid, &IID_IDirectManipulationFrameInfoProvider))
    {
        IUnknown_AddRef(iface);
        *ppv = &This->IDirectManipulationFrameInfoProvider_iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_compositor_interactions))
    {
        IUnknown_AddRef(iface);
        *ppv = &This->interactions_vtbl;
        return S_OK;
    }

    FIXME("(%p)->(%s,%p),not found\n", iface, debugstr_guid(riid), ppv);
    return E_NOINTERFACE;
}

static ULONG WINAPI compositor_AddRef(IDirectManipulationCompositor2 *iface)
{
    struct directcompositor *This = impl_from_IDirectManipulationCompositor2(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);

    return ref;
}

static ULONG WINAPI compositor_Release(IDirectManipulationCompositor2 *iface)
{
    struct directcompositor *This = impl_from_IDirectManipulationCompositor2(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);

    if (!ref)
    {
        if(This->manager)
            IDirectManipulationUpdateManager_Release(This->manager);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI compositor_AddContent(IDirectManipulationCompositor2 *iface, IDirectManipulationContent *content,
                IUnknown *device, IUnknown *parent, IUnknown *child)
{
    struct directcompositor *This = impl_from_IDirectManipulationCompositor2(iface);
    FIXME("%p, %p, %p, %p, %p\n", This, content, device, parent, child);
    return E_NOTIMPL;
}

static HRESULT WINAPI compositor_RemoveContent(IDirectManipulationCompositor2 *iface, IDirectManipulationContent *content)
{
    struct directcompositor *This = impl_from_IDirectManipulationCompositor2(iface);
    /* nothing was added to a composition tree here; XAML fail-fasts on a failure when it releases a
     * shared content transform (leaving the Settings page) */
    TRACE("%p, %p\n", This, content);
    return S_OK;
}

static HRESULT WINAPI compositor_SetUpdateManager(IDirectManipulationCompositor2 *iface, IDirectManipulationUpdateManager *manager)
{
    struct directcompositor *This = impl_from_IDirectManipulationCompositor2(iface);
    TRACE("%p, %p\n", This, manager);

    if(!manager)
        return E_INVALIDARG;

    This->manager = manager;
    IDirectManipulationUpdateManager_AddRef(This->manager);
    return S_OK;
}

static HRESULT WINAPI compositor_Flush(IDirectManipulationCompositor2 *iface)
{
    struct directcompositor *This = impl_from_IDirectManipulationCompositor2(iface);
    TRACE("%p\n", This);
    return S_OK;
}

static HRESULT WINAPI compositor_AddContentWithCrossProcessChaining(IDirectManipulationCompositor2 *iface,
                IDirectManipulationPrimaryContent *content, IUnknown *device, IUnknown *parentvisual, IUnknown *childvisual)
{
    struct directcompositor *This = impl_from_IDirectManipulationCompositor2(iface);
    FIXME("%p %p %p %p %p\n", This, content, device, parentvisual, childvisual);
    return E_NOTIMPL;
}

static const struct IDirectManipulationCompositor2Vtbl compositorVtbl =
{
    compositor_QueryInterface,
    compositor_AddRef,
    compositor_Release,
    compositor_AddContent,
    compositor_RemoveContent,
    compositor_SetUpdateManager,
    compositor_Flush,
    compositor_AddContentWithCrossProcessChaining
};

static HRESULT WINAPI provider_QueryInterface(IDirectManipulationFrameInfoProvider *iface, REFIID riid, void **ppv)
{
    struct directcompositor *This = impl_from_IDirectManipulationFrameInfoProvider(iface);
    return IDirectManipulationCompositor2_QueryInterface(&This->IDirectManipulationCompositor2_iface, riid, ppv);
}

static ULONG WINAPI provider_AddRef(IDirectManipulationFrameInfoProvider *iface)
{
    struct directcompositor *This = impl_from_IDirectManipulationFrameInfoProvider(iface);
    return IDirectManipulationCompositor2_AddRef(&This->IDirectManipulationCompositor2_iface);
}

static ULONG WINAPI provider_Release(IDirectManipulationFrameInfoProvider *iface)
{
    struct directcompositor *This = impl_from_IDirectManipulationFrameInfoProvider(iface);
    return IDirectManipulationCompositor2_Release(&This->IDirectManipulationCompositor2_iface);
}

static HRESULT WINAPI provider_GetNextFrameInfo(IDirectManipulationFrameInfoProvider *iface, ULONGLONG *time,
            ULONGLONG *process, ULONGLONG *composition)
{
    struct directcompositor *This = impl_from_IDirectManipulationFrameInfoProvider(iface);
    FIXME("%p, %p, %p, %p\n", This, time, process, composition);
    return E_NOTIMPL;
}

static const struct IDirectManipulationFrameInfoProviderVtbl providerVtbl =
{
    provider_QueryInterface,
    provider_AddRef,
    provider_Release,
    provider_GetNextFrameInfo
};

static HRESULT WINAPI DirectCompositor_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    struct directcompositor *object;
    HRESULT ret;

    TRACE("(%p, %s, %p)\n", outer, debugstr_guid(riid), ppv);

    *ppv = NULL;

    object = calloc(1, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;

    object->IDirectManipulationCompositor2_iface.lpVtbl = &compositorVtbl;
    object->IDirectManipulationFrameInfoProvider_iface.lpVtbl = &providerVtbl;
    object->interactions_vtbl = compositor_interactions_vtbl;
    object->ref = 1;

    ret = compositor_QueryInterface(&object->IDirectManipulationCompositor2_iface, riid, ppv);
    compositor_Release(&object->IDirectManipulationCompositor2_iface);

    return ret;
}

static HRESULT WINAPI ClassFactory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    *ppv = NULL;

    if(IsEqualGUID(&IID_IUnknown, riid) || IsEqualGUID(&IID_IClassFactory, riid)) {
        *ppv = iface;
    }

    if(*ppv) {
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    WARN("(%p)->(%s %p)\n", iface, debugstr_guid(riid), ppv);
    return E_NOINTERFACE;
}

static ULONG WINAPI ClassFactory_AddRef(IClassFactory *iface)
{
    TRACE("(%p)\n", iface);
    return 2;
}

static ULONG WINAPI ClassFactory_Release(IClassFactory *iface)
{
    TRACE("(%p)\n", iface);
    return 1;
}

static HRESULT WINAPI ClassFactory_LockServer(IClassFactory *iface, BOOL fLock)
{
    TRACE("(%p)->(%x)\n", iface, fLock);
    return S_OK;
}

static const IClassFactoryVtbl DirectFactoryVtbl = {
    ClassFactory_QueryInterface,
    ClassFactory_AddRef,
    ClassFactory_Release,
    DirectManipulation_CreateInstance,
    ClassFactory_LockServer
};

static const IClassFactoryVtbl DirectCompositorVtbl = {
    ClassFactory_QueryInterface,
    ClassFactory_AddRef,
    ClassFactory_Release,
    DirectCompositor_CreateInstance,
    ClassFactory_LockServer
};

static IClassFactory direct_factory = { &DirectFactoryVtbl };
static IClassFactory compositor_factory = { &DirectCompositorVtbl };

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    if(IsEqualGUID(&CLSID_DirectManipulationManager, rclsid) ||
       IsEqualGUID(&CLSID_DirectManipulationSharedManager, rclsid) ) {
        TRACE("(CLSID_DirectManipulationManager %s %p)\n", debugstr_guid(riid), ppv);
        return IClassFactory_QueryInterface(&direct_factory, riid, ppv);
    }
    else if(IsEqualGUID(&CLSID_DCompManipulationCompositor, rclsid)) {
        TRACE("(CLSID_DCompManipulationCompositor %s %p)\n", debugstr_guid(riid), ppv);
        return IClassFactory_QueryInterface(&compositor_factory, riid, ppv);
    }

    FIXME("%s %s %p\n", debugstr_guid(rclsid), debugstr_guid(riid), ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

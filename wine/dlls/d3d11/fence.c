/*
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

#include "d3d11_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d11);

/* ID3D11Fence. A fence's value only ever moves through ID3D11DeviceContext4::Signal on the
 * immediate context, and d3d11_fence_signal() is called once the GPU work submitted before the
 * Signal has completed, so the value the CPU sees never runs ahead of the GPU. Waiters registered
 * through SetEventOnCompletion are released as soon as the value reaches theirs. */

struct d3d11_fence_waiter
{
    UINT64 value;
    HANDLE event;
};

struct d3d11_fence
{
    ID3D11Fence ID3D11Fence_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    ID3D11Device5 *device;

    CRITICAL_SECTION cs;
    UINT64 value;
    struct d3d11_fence_waiter *waiters;
    SIZE_T waiter_count, waiter_capacity;
};

static const struct ID3D11FenceVtbl d3d11_fence_vtbl;

static inline struct d3d11_fence *impl_from_ID3D11Fence(ID3D11Fence *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_fence, ID3D11Fence_iface);
}

static struct d3d11_fence *unsafe_impl_from_ID3D11Fence(ID3D11Fence *iface)
{
    if (!iface)
        return NULL;
    if (iface->lpVtbl != &d3d11_fence_vtbl)
    {
        WARN("Fence %p is not ours.\n", iface);
        return NULL;
    }
    return impl_from_ID3D11Fence(iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_QueryInterface(ID3D11Fence *iface, REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11Fence)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11Fence_AddRef(*object = iface);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_fence_AddRef(ID3D11Fence *iface)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);
    ULONG refcount = InterlockedIncrement(&fence->refcount);

    TRACE("%p increasing refcount to %lu.\n", fence, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_fence_Release(ID3D11Fence *iface)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);
    ULONG refcount = InterlockedDecrement(&fence->refcount);

    TRACE("%p decreasing refcount to %lu.\n", fence, refcount);

    if (!refcount)
    {
        ID3D11Device5 *device = fence->device;

        free(fence->waiters);
        fence->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&fence->cs);
        wined3d_private_store_cleanup(&fence->private_store);
        free(fence);
        ID3D11Device5_Release(device);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d3d11_fence_GetDevice(ID3D11Fence *iface, ID3D11Device **device)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    ID3D11Device_AddRef(*device = (ID3D11Device *)fence->device);
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_GetPrivateData(ID3D11Fence *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&fence->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_SetPrivateData(ID3D11Fence *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&fence->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_SetPrivateDataInterface(ID3D11Fence *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&fence->private_store, guid, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_CreateSharedHandle(ID3D11Fence *iface,
        const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle)
{
    FIXME("iface %p, attributes %p, access %#lx, name %s, handle %p stub!\n",
            iface, attributes, access, debugstr_w(name), handle);

    return E_NOTIMPL;
}

static UINT64 STDMETHODCALLTYPE d3d11_fence_GetCompletedValue(ID3D11Fence *iface)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);
    UINT64 value;

    TRACE("iface %p.\n", iface);

    EnterCriticalSection(&fence->cs);
    value = fence->value;
    LeaveCriticalSection(&fence->cs);

    return value;
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_SetEventOnCompletion(ID3D11Fence *iface,
        UINT64 value, HANDLE event)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);
    struct d3d11_fence_waiter *waiter;

    TRACE("iface %p, value %#I64x, event %p.\n", iface, value, event);

    /* Unlike D3D12, a D3D11 fence requires an event. */
    if (!event)
        return E_INVALIDARG;

    EnterCriticalSection(&fence->cs);
    if (fence->value >= value)
    {
        LeaveCriticalSection(&fence->cs);
        SetEvent(event);
        return S_OK;
    }
    if (fence->waiter_count == fence->waiter_capacity)
    {
        SIZE_T capacity = max(4, fence->waiter_capacity * 2);
        struct d3d11_fence_waiter *waiters;

        if (!(waiters = realloc(fence->waiters, capacity * sizeof(*waiters))))
        {
            LeaveCriticalSection(&fence->cs);
            return E_OUTOFMEMORY;
        }
        fence->waiters = waiters;
        fence->waiter_capacity = capacity;
    }
    waiter = &fence->waiters[fence->waiter_count++];
    waiter->value = value;
    waiter->event = event;
    LeaveCriticalSection(&fence->cs);

    return S_OK;
}

static const struct ID3D11FenceVtbl d3d11_fence_vtbl =
{
    /* IUnknown methods */
    d3d11_fence_QueryInterface,
    d3d11_fence_AddRef,
    d3d11_fence_Release,
    /* ID3D11DeviceChild methods */
    d3d11_fence_GetDevice,
    d3d11_fence_GetPrivateData,
    d3d11_fence_SetPrivateData,
    d3d11_fence_SetPrivateDataInterface,
    /* ID3D11Fence methods */
    d3d11_fence_CreateSharedHandle,
    d3d11_fence_GetCompletedValue,
    d3d11_fence_SetEventOnCompletion,
};

HRESULT d3d11_fence_create(struct d3d_device *device, UINT64 initial_value, D3D11_FENCE_FLAG flags,
        ID3D11Fence **out)
{
    struct d3d11_fence *fence;

    if (flags & ~D3D11_FENCE_FLAG_NONE)
        FIXME("Ignoring flags %#x.\n", flags);

    if (!(fence = calloc(1, sizeof(*fence))))
        return E_OUTOFMEMORY;

    fence->ID3D11Fence_iface.lpVtbl = &d3d11_fence_vtbl;
    fence->refcount = 1;
    wined3d_private_store_init(&fence->private_store);
    ID3D11Device5_AddRef(fence->device = &device->ID3D11Device5_iface);
    InitializeCriticalSectionEx(&fence->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    fence->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": d3d11_fence.cs");
    fence->value = initial_value;

    TRACE("Created fence %p, initial value %#I64x.\n", fence, initial_value);
    *out = &fence->ID3D11Fence_iface;
    return S_OK;
}

/* The GPU has reached a Signal(fence, value): publish the value and release the waiters it
 * satisfies. Fence values are monotonic, so a smaller value never moves one back. */
HRESULT d3d11_fence_signal(ID3D11Fence *iface, UINT64 value)
{
    struct d3d11_fence *fence = unsafe_impl_from_ID3D11Fence(iface);
    SIZE_T i = 0;

    if (!fence)
        return E_INVALIDARG;

    EnterCriticalSection(&fence->cs);
    if (value > fence->value)
        fence->value = value;
    while (i < fence->waiter_count)
    {
        if (fence->waiters[i].value <= fence->value)
        {
            SetEvent(fence->waiters[i].event);
            fence->waiters[i] = fence->waiters[--fence->waiter_count];
        }
        else
        {
            ++i;
        }
    }
    LeaveCriticalSection(&fence->cs);

    return S_OK;
}

UINT64 d3d11_fence_value(ID3D11Fence *iface)
{
    struct d3d11_fence *fence = unsafe_impl_from_ID3D11Fence(iface);

    return fence ? d3d11_fence_GetCompletedValue(iface) : 0;
}

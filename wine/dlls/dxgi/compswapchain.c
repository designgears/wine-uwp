/*
 * Composition swap chains (IDXGIFactory2::CreateSwapChainForComposition)
 *
 * Copyright 2026 Wine project
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

/* A composition swap chain has no window: the application renders into its buffers and
 * Present hands the result to the compositor, which samples it as a visual's content.
 * Here the buffers are ordinary D3D11 render-target textures and Present only bumps the
 * present count; the compositor (dcomp) reads buffer 0 through a staging copy when the
 * count changes. */

#include "dxgi_private.h"
#include "d3d11.h"

WINE_DEFAULT_DEBUG_CHANNEL(dxgi);
WINE_DECLARE_DEBUG_CHANNEL(d3dperf);

/* Undocumented; XAML's SwapChainPanel::SetSwapChain queries it to tell a composition swap chain
 * from a window one (failure is its 0x88001b00 error) and only releases the result. */
/* Wine-private: an IUnknown whose fourth method is called after every Present (see comp_swapchain_present). */
static const GUID IID_wine_composition_present_notify = {0x3f6a1b2e, 0x7d44, 0x4c8a, {0x9b, 0x1e, 0x52, 0x0d, 0x6e, 0x33, 0x81, 0xa7}};
static const GUID IID_IDXGISwapChainComposition = {0xb02d7a1a, 0x05e0, 0x4a71, {0x8e, 0xf4, 0x5c, 0x5b, 0xbf, 0x47, 0x50, 0x86}};

struct comp_swapchain
{
    IDXGISwapChain4 IDXGISwapChain4_iface;
    LONG refcount;
    struct wined3d_private_store private_store;
    IUnknown *device;
    ID3D11Device *d3d_device;
    IWineDXGIFactory *factory;

    DXGI_SWAP_CHAIN_DESC1 desc;
    ID3D11Texture2D *buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    /* What the compositor reads: Present copies buffer 0 into presented[count & 1], so a frame is
     * complete and stays intact while the app draws the next one into buffer 0. */
    ID3D11Texture2D *presented[2];
    SRWLOCK presented_lock;     /* dcomp's readback worker takes presented[] while the app may resize */
    LONG present_count;
    LONGLONG vblank_slot;       /* the refresh (see dxgi_vblank_period) of the last synced present */
    DXGI_MATRIX_3X2_F transform;
    DXGI_RGBA background;
    DXGI_MODE_ROTATION rotation;
    UINT source_width, source_height;
    UINT max_latency;
    HANDLE latency_event;
    DXGI_COLOR_SPACE_TYPE color_space;
};

static inline struct comp_swapchain *impl_from_IDXGISwapChain4(IDXGISwapChain4 *iface)
{
    return CONTAINING_RECORD(iface, struct comp_swapchain, IDXGISwapChain4_iface);
}

static void comp_swapchain_release_buffers(struct comp_swapchain *swapchain)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(swapchain->buffers); i++)
    {
        if (swapchain->buffers[i]) ID3D11Texture2D_Release(swapchain->buffers[i]);
        swapchain->buffers[i] = NULL;
    }
    AcquireSRWLockExclusive(&swapchain->presented_lock);
    for (i = 0; i < ARRAY_SIZE(swapchain->presented); i++)
    {
        if (swapchain->presented[i]) ID3D11Texture2D_Release(swapchain->presented[i]);
        swapchain->presented[i] = NULL;
    }
    ReleaseSRWLockExclusive(&swapchain->presented_lock);
}

static HRESULT comp_swapchain_create_buffers(struct comp_swapchain *swapchain)
{
    D3D11_TEXTURE2D_DESC desc = {0};
    unsigned int i;
    HRESULT hr;

    desc.Width = swapchain->desc.Width;
    desc.Height = swapchain->desc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = swapchain->desc.Format;
    desc.SampleDesc = swapchain->desc.SampleDesc;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (swapchain->desc.BufferUsage & DXGI_USAGE_SHADER_INPUT) desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (swapchain->desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

    for (i = 0; i < swapchain->desc.BufferCount; i++)
    {
        if (FAILED(hr = ID3D11Device_CreateTexture2D(swapchain->d3d_device, &desc, NULL, &swapchain->buffers[i])))
        {
            WARN("Failed to create buffer %u, hr %#lx.\n", i, hr);
            comp_swapchain_release_buffers(swapchain);
            return hr;
        }
    }
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    for (i = 0; i < ARRAY_SIZE(swapchain->presented); i++)
    {
        ID3D11Texture2D *texture;

        hr = ID3D11Device_CreateTexture2D(swapchain->d3d_device, &desc, NULL, &texture);
        AcquireSRWLockExclusive(&swapchain->presented_lock);
        swapchain->presented[i] = SUCCEEDED(hr) ? texture : NULL;
        ReleaseSRWLockExclusive(&swapchain->presented_lock);
        if (FAILED(hr))
        {
            WARN("Failed to create presented copy %u, hr %#lx.\n", i, hr);
            comp_swapchain_release_buffers(swapchain);
            return hr;
        }
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_QueryInterface(IDXGISwapChain4 *iface, REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_IUnknown)
            || IsEqualGUID(riid, &IID_IDXGIObject)
            || IsEqualGUID(riid, &IID_IDXGIDeviceSubObject)
            || IsEqualGUID(riid, &IID_IDXGISwapChain)
            || IsEqualGUID(riid, &IID_IDXGISwapChain1)
            || IsEqualGUID(riid, &IID_IDXGISwapChain2)
            || IsEqualGUID(riid, &IID_IDXGISwapChain3)
            || IsEqualGUID(riid, &IID_IDXGISwapChain4)
            || IsEqualGUID(riid, &IID_IDXGISwapChainComposition))
    {
        IUnknown_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE comp_swapchain_AddRef(IDXGISwapChain4 *iface)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    ULONG refcount = InterlockedIncrement(&swapchain->refcount);

    TRACE("%p increasing refcount to %lu.\n", swapchain, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE comp_swapchain_Release(IDXGISwapChain4 *iface)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    ULONG refcount = InterlockedDecrement(&swapchain->refcount);

    TRACE("%p decreasing refcount to %lu.\n", swapchain, refcount);

    if (!refcount)
    {
        comp_swapchain_release_buffers(swapchain);
        if (swapchain->latency_event) CloseHandle(swapchain->latency_event);
        wined3d_private_store_cleanup(&swapchain->private_store);
        ID3D11Device_Release(swapchain->d3d_device);
        IUnknown_Release(swapchain->device);
        IWineDXGIFactory_Release(swapchain->factory);
        free(swapchain);
    }
    return refcount;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_SetPrivateData(IDXGISwapChain4 *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);
    return dxgi_set_private_data(&swapchain->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_SetPrivateDataInterface(IDXGISwapChain4 *iface,
        REFGUID guid, const IUnknown *object)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, guid %s, object %p.\n", iface, debugstr_guid(guid), object);
    return dxgi_set_private_data_interface(&swapchain->private_store, guid, object);
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetPrivateData(IDXGISwapChain4 *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);
    return dxgi_get_private_data(&swapchain->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetParent(IDXGISwapChain4 *iface, REFIID riid, void **parent)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, riid %s, parent %p.\n", iface, debugstr_guid(riid), parent);
    return IWineDXGIFactory_QueryInterface(swapchain->factory, riid, parent);
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetDevice(IDXGISwapChain4 *iface, REFIID riid, void **device)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, riid %s, device %p.\n", iface, debugstr_guid(riid), device);
    return IUnknown_QueryInterface(swapchain->device, riid, device);
}

/* One vblank grid for the process: refresh k happens at QPC time k * period. IDXGIOutput::WaitForVBlank and
 * synced presents both keep to it; they used to keep separate clocks (a plain 16 ms sleep, and a schedule
 * started at the first present), and Shapr3D's render loop, which waits for the vblank and then presents
 * with sync interval 1, paid for two refreshes a frame. */
LONGLONG dxgi_vblank_period(void)
{
    static LONGLONG period_ticks;
    LARGE_INTEGER freq;

    if (!period_ticks)
    {
        DEVMODEW mode = { .dmSize = sizeof(mode) };
        DWORD hz = 60;
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
                && mode.dmDisplayFrequency < 1000)
            hz = mode.dmDisplayFrequency;
        /* WINE_UWP_TEST_HZ: pace as a display of that rate would (Xvfb reports 60; the user's runs at 175) */
        if (getenv("WINE_UWP_TEST_HZ") && atoi(getenv("WINE_UWP_TEST_HZ")) > 1)
            hz = atoi(getenv("WINE_UWP_TEST_HZ"));
        QueryPerformanceFrequency(&freq);
        period_ticks = freq.QuadPart / hz;
        TRACE("vblank grid at %lu Hz.\n", hz);
    }
    return period_ticks;
}

void dxgi_wait_until(LONGLONG target)
{
    LARGE_INTEGER now, freq;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (target <= now.QuadPart) return;
    if ((target - now.QuadPart) * 1000 / freq.QuadPart > 1)
        Sleep((target - now.QuadPart) * 1000 / freq.QuadPart - 1);
    while (QueryPerformanceCounter(&now), now.QuadPart < target) Sleep(0);
}

/* A synced present shows at most one frame per refresh, as the compositor's does on Windows: with no wait
 * the Shapr3D engine presented 3,700 frames a second with sync interval 1, all of them wasted work, and the
 * compositor read back and painted 560 of them. It blocks only when this refresh already has a frame (a
 * flip-model present blocks only on a full queue), so a loop that waited for the vblank itself runs at the
 * refresh rate. */
static void comp_swapchain_wait_vblank(struct comp_swapchain *swapchain, UINT sync_interval)
{
    LONGLONG period = dxgi_vblank_period();
    LARGE_INTEGER now;
    LONGLONG slot;

    QueryPerformanceCounter(&now);
    slot = now.QuadPart / period;
    if (swapchain->vblank_slot && slot < swapchain->vblank_slot + sync_interval)
    {
        slot = swapchain->vblank_slot + sync_interval;
        dxgi_wait_until(slot * period);
    }
    swapchain->vblank_slot = slot;
}

static HRESULT comp_swapchain_present(struct comp_swapchain *swapchain, UINT sync_interval, UINT flags)
{
    if (sync_interval > 4)
    {
        WARN("Invalid sync interval %u.\n", sync_interval);
        return DXGI_ERROR_INVALID_CALL;
    }
    LARGE_INTEGER t0, t1, t2, t3;
    static LARGE_INTEGER last_present;

    if (flags & DXGI_PRESENT_TEST) return S_OK;
    QueryPerformanceCounter(&t0);
    if (sync_interval) comp_swapchain_wait_vblank(swapchain, sync_interval);
    QueryPerformanceCounter(&t1);

    /* Snapshot the frame for the compositor, in order on the app's own context, and make sure
     * it is finished before the compositor copies it. The compositor reads buffer 0 on its own
     * thread some time later; reading it directly showed frames the app was halfway through
     * drawing (Solitaire's backdrop without its cards). */
    {
        ID3D11DeviceContext *context;
        ID3D11Texture2D *dst = swapchain->presented[(swapchain->present_count + 1) & 1];
        ID3D11Device_GetImmediateContext(swapchain->d3d_device, &context);
        if (dst && swapchain->buffers[0])
        {
            if (swapchain->desc.SampleDesc.Count > 1)
                ID3D11DeviceContext_ResolveSubresource(context, (ID3D11Resource *)dst, 0,
                        (ID3D11Resource *)swapchain->buffers[0], 0, swapchain->desc.Format);
            else
                ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)dst, (ID3D11Resource *)swapchain->buffers[0]);
        }
        ID3D11DeviceContext_Flush(context);
        ID3D11DeviceContext_Release(context);
    }
    QueryPerformanceCounter(&t2);
    InterlockedIncrement(&swapchain->present_count);
    if (swapchain->latency_event) SetEvent(swapchain->latency_event);
    /* The compositor registers a notifier as private data so it can pick the frame up right away instead
     * of polling the present count (dcomp polled at 33 ms and showed half the frames of a graph drag). */
    {
        IUnknown *notify = NULL;
        UINT size = sizeof(notify);
        if (SUCCEEDED(dxgi_get_private_data(&swapchain->private_store, &IID_wine_composition_present_notify, &size, &notify))
                && size == sizeof(notify) && notify)
        {
            void (STDMETHODCALLTYPE *presented)(IUnknown *) = ((void (STDMETHODCALLTYPE **)(IUnknown *))notify->lpVtbl)[3];
            presented(notify);
            IUnknown_Release(notify);
        }
    }
    QueryPerformanceCounter(&t3);
    if (TRACE_ON(d3dperf))
    {
        LARGE_INTEGER freq;
        double gap, total;
        QueryPerformanceFrequency(&freq);
        gap = last_present.QuadPart ? (t0.QuadPart - last_present.QuadPart) * 1000.0 / freq.QuadPart : 0.0;
        total = (t3.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        /* every present: the time outside is the app's, the time inside ours */
        TRACE_(d3dperf)("present after %.1f ms outside; inside %.1f ms (vblank %.1f, copy %.1f, notify %.1f).\n",
                gap, total, (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart,
                (t2.QuadPart - t1.QuadPart) * 1000.0 / freq.QuadPart, (t3.QuadPart - t2.QuadPart) * 1000.0 / freq.QuadPart);
    }
    last_present = t3;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_Present(IDXGISwapChain4 *iface, UINT sync_interval, UINT flags)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, sync_interval %u, flags %#x.\n", iface, sync_interval, flags);
    return comp_swapchain_present(swapchain, sync_interval, flags);
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetBuffer(IDXGISwapChain4 *iface,
        UINT buffer_idx, REFIID riid, void **surface)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, buffer_idx %u, riid %s, surface %p.\n", iface, buffer_idx, debugstr_guid(riid), surface);

    /* Wine-private, for dcomp: 0x80000000 | present count asks for the snapshot of that frame. */
    if (buffer_idx & 0x80000000)
    {
        ID3D11Texture2D *texture;
        HRESULT hr;

        AcquireSRWLockShared(&swapchain->presented_lock);
        if ((texture = swapchain->presented[buffer_idx & 1]))
            hr = ID3D11Texture2D_QueryInterface(texture, riid, surface);
        else
            hr = DXGI_ERROR_INVALID_CALL;
        ReleaseSRWLockShared(&swapchain->presented_lock);
        return hr;
    }
    if (buffer_idx >= swapchain->desc.BufferCount || !swapchain->buffers[buffer_idx])
    {
        WARN("Invalid buffer index %u.\n", buffer_idx);
        return DXGI_ERROR_INVALID_CALL;
    }
    return ID3D11Texture2D_QueryInterface(swapchain->buffers[buffer_idx], riid, surface);
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_SetFullscreenState(IDXGISwapChain4 *iface,
        BOOL fullscreen, IDXGIOutput *target)
{
    TRACE("iface %p, fullscreen %#x, target %p.\n", iface, fullscreen, target);
    return fullscreen ? DXGI_ERROR_INVALID_CALL : S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetFullscreenState(IDXGISwapChain4 *iface,
        BOOL *fullscreen, IDXGIOutput **target)
{
    TRACE("iface %p, fullscreen %p, target %p.\n", iface, fullscreen, target);
    if (fullscreen) *fullscreen = FALSE;
    if (target) *target = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetDesc(IDXGISwapChain4 *iface, DXGI_SWAP_CHAIN_DESC *desc)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);
    if (!desc) return E_INVALIDARG;

    memset(desc, 0, sizeof(*desc));
    desc->BufferDesc.Width = swapchain->desc.Width;
    desc->BufferDesc.Height = swapchain->desc.Height;
    desc->BufferDesc.Format = swapchain->desc.Format;
    desc->SampleDesc = swapchain->desc.SampleDesc;
    desc->BufferUsage = swapchain->desc.BufferUsage;
    desc->BufferCount = swapchain->desc.BufferCount;
    desc->OutputWindow = NULL;
    desc->Windowed = TRUE;
    desc->SwapEffect = swapchain->desc.SwapEffect;
    desc->Flags = swapchain->desc.Flags;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_ResizeBuffers(IDXGISwapChain4 *iface,
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    DXGI_SWAP_CHAIN_DESC1 old = swapchain->desc;
    HRESULT hr;

    TRACE("iface %p, buffer_count %u, width %u, height %u, format %s, flags %#x.\n",
            iface, buffer_count, width, height, debug_dxgi_format(format), flags);

    if (buffer_count) swapchain->desc.BufferCount = buffer_count;
    if (width) swapchain->desc.Width = width;
    if (height) swapchain->desc.Height = height;
    if (format != DXGI_FORMAT_UNKNOWN) swapchain->desc.Format = format;
    swapchain->desc.Flags = flags;
    if (!dxgi_validate_swapchain_desc(&swapchain->desc))
    {
        swapchain->desc = old;
        return DXGI_ERROR_INVALID_CALL;
    }

    comp_swapchain_release_buffers(swapchain);
    if (FAILED(hr = comp_swapchain_create_buffers(swapchain)))
    {
        swapchain->desc = old;
        comp_swapchain_create_buffers(swapchain);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_ResizeTarget(IDXGISwapChain4 *iface,
        const DXGI_MODE_DESC *target_mode_desc)
{
    TRACE("iface %p, target_mode_desc %p.\n", iface, target_mode_desc);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetContainingOutput(IDXGISwapChain4 *iface, IDXGIOutput **output)
{
    TRACE("iface %p, output %p.\n", iface, output);
    *output = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetFrameStatistics(IDXGISwapChain4 *iface, DXGI_FRAME_STATISTICS *stats)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, stats %p.\n", iface, stats);
    memset(stats, 0, sizeof(*stats));
    stats->PresentCount = swapchain->present_count;
    stats->PresentRefreshCount = swapchain->present_count;
    stats->SyncRefreshCount = swapchain->present_count;
    QueryPerformanceCounter(&stats->SyncQPCTime);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetLastPresentCount(IDXGISwapChain4 *iface, UINT *last_present_count)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, last_present_count %p.\n", iface, last_present_count);
    *last_present_count = swapchain->present_count;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetDesc1(IDXGISwapChain4 *iface, DXGI_SWAP_CHAIN_DESC1 *desc)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);
    if (!desc) return E_INVALIDARG;
    *desc = swapchain->desc;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetFullscreenDesc(IDXGISwapChain4 *iface,
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc)
{
    TRACE("iface %p, desc %p.\n", iface, desc);
    if (!desc) return E_INVALIDARG;
    memset(desc, 0, sizeof(*desc));
    desc->Windowed = TRUE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetHwnd(IDXGISwapChain4 *iface, HWND *hwnd)
{
    TRACE("iface %p, hwnd %p.\n", iface, hwnd);
    *hwnd = NULL;
    return DXGI_ERROR_INVALID_CALL;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetCoreWindow(IDXGISwapChain4 *iface, REFIID iid, void **core_window)
{
    TRACE("iface %p, iid %s, core_window %p.\n", iface, debugstr_guid(iid), core_window);
    if (core_window) *core_window = NULL;
    return DXGI_ERROR_INVALID_CALL;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_Present1(IDXGISwapChain4 *iface,
        UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS *present_parameters)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, sync_interval %u, flags %#x, present_parameters %p.\n",
            iface, sync_interval, flags, present_parameters);
    return comp_swapchain_present(swapchain, sync_interval, flags);
}

static BOOL STDMETHODCALLTYPE comp_swapchain_IsTemporaryMonoSupported(IDXGISwapChain4 *iface)
{
    TRACE("iface %p.\n", iface);
    return FALSE;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetRestrictToOutput(IDXGISwapChain4 *iface, IDXGIOutput **output)
{
    TRACE("iface %p, output %p.\n", iface, output);
    *output = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_SetBackgroundColor(IDXGISwapChain4 *iface, const DXGI_RGBA *color)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, color %p.\n", iface, color);
    if (!color) return E_INVALIDARG;
    swapchain->background = *color;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetBackgroundColor(IDXGISwapChain4 *iface, DXGI_RGBA *color)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, color %p.\n", iface, color);
    if (!color) return E_INVALIDARG;
    *color = swapchain->background;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_SetRotation(IDXGISwapChain4 *iface, DXGI_MODE_ROTATION rotation)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, rotation %#x.\n", iface, rotation);
    swapchain->rotation = rotation;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetRotation(IDXGISwapChain4 *iface, DXGI_MODE_ROTATION *rotation)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, rotation %p.\n", iface, rotation);
    *rotation = swapchain->rotation;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_SetSourceSize(IDXGISwapChain4 *iface, UINT width, UINT height)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, width %u, height %u.\n", iface, width, height);
    if (!width || !height || width > swapchain->desc.Width || height > swapchain->desc.Height)
        return DXGI_ERROR_INVALID_CALL;
    swapchain->source_width = width;
    swapchain->source_height = height;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetSourceSize(IDXGISwapChain4 *iface, UINT *width, UINT *height)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, width %p, height %p.\n", iface, width, height);
    *width = swapchain->source_width ? swapchain->source_width : swapchain->desc.Width;
    *height = swapchain->source_height ? swapchain->source_height : swapchain->desc.Height;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_SetMaximumFrameLatency(IDXGISwapChain4 *iface, UINT max_latency)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, max_latency %u.\n", iface, max_latency);
    if (!(swapchain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
        return DXGI_ERROR_INVALID_CALL;
    swapchain->max_latency = max_latency;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetMaximumFrameLatency(IDXGISwapChain4 *iface, UINT *max_latency)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, max_latency %p.\n", iface, max_latency);
    if (!(swapchain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
        return DXGI_ERROR_INVALID_CALL;
    *max_latency = swapchain->max_latency;
    return S_OK;
}

static HANDLE STDMETHODCALLTYPE comp_swapchain_GetFrameLatencyWaitableObject(IDXGISwapChain4 *iface)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    HANDLE handle;

    TRACE("iface %p.\n", iface);
    if (!swapchain->latency_event) return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), swapchain->latency_event, GetCurrentProcess(), &handle,
            0, FALSE, DUPLICATE_SAME_ACCESS))
        return NULL;
    return handle;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_SetMatrixTransform(IDXGISwapChain4 *iface,
        const DXGI_MATRIX_3X2_F *matrix)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, matrix %p.\n", iface, matrix);
    if (!matrix) return E_INVALIDARG;
    swapchain->transform = *matrix;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_GetMatrixTransform(IDXGISwapChain4 *iface,
        DXGI_MATRIX_3X2_F *matrix)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, matrix %p.\n", iface, matrix);
    if (!matrix) return E_INVALIDARG;
    *matrix = swapchain->transform;
    return S_OK;
}

static UINT STDMETHODCALLTYPE comp_swapchain_GetCurrentBackBufferIndex(IDXGISwapChain4 *iface)
{
    TRACE("iface %p.\n", iface);
    return 0;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_CheckColorSpaceSupport(IDXGISwapChain4 *iface,
        DXGI_COLOR_SPACE_TYPE colour_space, UINT *colour_space_support)
{
    TRACE("iface %p, colour_space %#x, colour_space_support %p.\n", iface, colour_space, colour_space_support);
    if (!colour_space_support) return E_INVALIDARG;
    *colour_space_support = colour_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
            ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT : 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_SetColorSpace1(IDXGISwapChain4 *iface, DXGI_COLOR_SPACE_TYPE colour_space)
{
    struct comp_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, colour_space %#x.\n", iface, colour_space);
    if (colour_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) return E_INVALIDARG;
    swapchain->color_space = colour_space;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_ResizeBuffers1(IDXGISwapChain4 *iface,
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
        const UINT *node_mask, IUnknown * const *present_queue)
{
    TRACE("iface %p, buffer_count %u, width %u, height %u, format %s, flags %#x, node_mask %p, present_queue %p.\n",
            iface, buffer_count, width, height, debug_dxgi_format(format), flags, node_mask, present_queue);
    return comp_swapchain_ResizeBuffers(iface, buffer_count, width, height, format, flags);
}

static HRESULT STDMETHODCALLTYPE comp_swapchain_SetHDRMetaData(IDXGISwapChain4 *iface,
        DXGI_HDR_METADATA_TYPE type, UINT size, void *metadata)
{
    TRACE("iface %p, type %#x, size %u, metadata %p.\n", iface, type, size, metadata);
    return type == DXGI_HDR_METADATA_TYPE_NONE ? S_OK : E_NOTIMPL;
}

static const struct IDXGISwapChain4Vtbl comp_swapchain_vtbl =
{
    comp_swapchain_QueryInterface,
    comp_swapchain_AddRef,
    comp_swapchain_Release,
    comp_swapchain_SetPrivateData,
    comp_swapchain_SetPrivateDataInterface,
    comp_swapchain_GetPrivateData,
    comp_swapchain_GetParent,
    comp_swapchain_GetDevice,
    comp_swapchain_Present,
    comp_swapchain_GetBuffer,
    comp_swapchain_SetFullscreenState,
    comp_swapchain_GetFullscreenState,
    comp_swapchain_GetDesc,
    comp_swapchain_ResizeBuffers,
    comp_swapchain_ResizeTarget,
    comp_swapchain_GetContainingOutput,
    comp_swapchain_GetFrameStatistics,
    comp_swapchain_GetLastPresentCount,
    comp_swapchain_GetDesc1,
    comp_swapchain_GetFullscreenDesc,
    comp_swapchain_GetHwnd,
    comp_swapchain_GetCoreWindow,
    comp_swapchain_Present1,
    comp_swapchain_IsTemporaryMonoSupported,
    comp_swapchain_GetRestrictToOutput,
    comp_swapchain_SetBackgroundColor,
    comp_swapchain_GetBackgroundColor,
    comp_swapchain_SetRotation,
    comp_swapchain_GetRotation,
    comp_swapchain_SetSourceSize,
    comp_swapchain_GetSourceSize,
    comp_swapchain_SetMaximumFrameLatency,
    comp_swapchain_GetMaximumFrameLatency,
    comp_swapchain_GetFrameLatencyWaitableObject,
    comp_swapchain_SetMatrixTransform,
    comp_swapchain_GetMatrixTransform,
    comp_swapchain_GetCurrentBackBufferIndex,
    comp_swapchain_CheckColorSpaceSupport,
    comp_swapchain_SetColorSpace1,
    comp_swapchain_ResizeBuffers1,
    comp_swapchain_SetHDRMetaData,
};

HRESULT comp_swapchain_create(IWineDXGIFactory *factory, IUnknown *device,
        const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGISwapChain1 **swapchain)
{
    struct comp_swapchain *object;
    HRESULT hr;

    if (!device || !desc || !swapchain) return DXGI_ERROR_INVALID_CALL;
    if (desc->Stereo)
    {
        FIXME("Stereo swapchains are not supported.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (!dxgi_validate_swapchain_desc(desc)) return DXGI_ERROR_INVALID_CALL;
    if (!desc->Width || !desc->Height)
    {
        WARN("Composition swap chains need an explicit size.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!(object = calloc(1, sizeof(*object)))) return E_OUTOFMEMORY;
    object->IDXGISwapChain4_iface.lpVtbl = &comp_swapchain_vtbl;
    object->refcount = 1;
    object->desc = *desc;
    object->transform._11 = object->transform._22 = 1.0f;
    object->max_latency = 1;
    object->color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    wined3d_private_store_init(&object->private_store);

    if (FAILED(hr = IUnknown_QueryInterface(device, &IID_ID3D11Device, (void **)&object->d3d_device)))
    {
        FIXME("Only D3D11 devices are supported, hr %#lx.\n", hr);
        wined3d_private_store_cleanup(&object->private_store);
        free(object);
        return hr;
    }
    object->device = device;
    IUnknown_AddRef(device);
    object->factory = factory;
    IWineDXGIFactory_AddRef(factory);
    if (desc->Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
        object->latency_event = CreateEventW(NULL, FALSE, TRUE, NULL);

    if (FAILED(hr = comp_swapchain_create_buffers(object)))
    {
        IDXGISwapChain4_Release(&object->IDXGISwapChain4_iface);
        return hr;
    }

    TRACE("Created composition swap chain %p, %ux%u %s, %u buffers.\n", object, desc->Width, desc->Height,
            debug_dxgi_format(desc->Format), desc->BufferCount);
    *swapchain = (IDXGISwapChain1 *)&object->IDXGISwapChain4_iface;
    return S_OK;
}

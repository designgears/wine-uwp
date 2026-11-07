/*
 * CoreWindow presentation through an unmodified native DXGI implementation.
 *
 * Copyright 2026 wine-uwp contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include "windows.h"
#include "dxgi1_2.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wineuwp);

static const GUID core_window_interop_iid =
    {0x45d64a29, 0xa63e, 0x4cb6, {0xb4, 0x98, 0x57, 0x81, 0xd2, 0x98, 0xcb, 0x4f}};

static HRESULT WINAPI create_for_core_window( IDXGIFactory2 *factory, IUnknown *device,
        IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *output,
        IDXGISwapChain1 **swapchain )
{
    HRESULT (WINAPI *get_window_handle)( IUnknown *, HWND * );
    IUnknown *interop;
    HWND hwnd = NULL;
    HRESULT hr;

    if (swapchain) *swapchain = NULL;
    if (!device || !window || !desc || !swapchain) return DXGI_ERROR_INVALID_CALL;
    if (FAILED(IUnknown_QueryInterface( window, &core_window_interop_iid, (void **)&interop )))
        return DXGI_ERROR_INVALID_CALL;

    get_window_handle = (*(void ***)interop)[3];
    hr = get_window_handle( interop, &hwnd );
    IUnknown_Release( interop );
    if (FAILED(hr)) return hr;
    if (!hwnd) return DXGI_ERROR_INVALID_CALL;

    TRACE( "CoreWindow %p uses window %p through native DXGI\n", window, hwnd );
    return IDXGIFactory2_CreateSwapChainForHwnd( factory, device, hwnd, desc, NULL, output, swapchain );
}

static INIT_ONCE prepare_once = INIT_ONCE_STATIC_INIT;
static HRESULT prepare_result;

static BOOL CALLBACK prepare_dxgi( INIT_ONCE *once, void *param, void **context )
{
    HRESULT (WINAPI *create_factory)( REFIID, void ** );
    HMODULE module, owner;
    IDXGIFactory2 *factory;
    void **slot, *original;
    DWORD protect, unused;

    if (!(module = LoadLibraryW( L"dxgi.dll" )))
    {
        prepare_result = HRESULT_FROM_WIN32( GetLastError() );
        return TRUE;
    }
    if (!(create_factory = (void *)GetProcAddress( module, "CreateDXGIFactory1" )))
        prepare_result = E_NOTIMPL;
    else
        prepare_result = create_factory( &IID_IDXGIFactory2, (void **)&factory );
    if (FAILED(prepare_result)) goto release_module;

    /* DXVK's factory interfaces use one shared vtable. Redirect just the public
     * CoreWindow entry, preserving the native objects, identity and all other
     * methods, including factories returned by an adapter's GetParent. No code
     * bytes or DLL files are changed. Install before the app receives its view. */
    slot = (void **)&factory->lpVtbl->CreateSwapChainForCoreWindow;
    if (!GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (const WCHAR *)slot, &owner ) || owner != module)
    {
        prepare_result = E_NOTIMPL;
        goto release_factory;
    }
    /* The factory table must never outlive our callback. Pin only this small
     * broker; retain the LoadLibrary reference to DXGI after installation. */
    if (!GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            (const WCHAR *)create_for_core_window, &owner ))
    {
        prepare_result = HRESULT_FROM_WIN32( GetLastError() );
        goto release_factory;
    }
    if (!VirtualProtect( slot, sizeof(*slot), PAGE_READWRITE, &protect ))
    {
        prepare_result = HRESULT_FROM_WIN32( GetLastError() );
        goto release_factory;
    }
    original = InterlockedExchangePointer( slot, create_for_core_window );
    if (!VirtualProtect( slot, sizeof(*slot), protect, &unused ))
    {
        prepare_result = HRESULT_FROM_WIN32( GetLastError() );
        InterlockedExchangePointer( slot, original );
        if (!VirtualProtect( slot, sizeof(*slot), protect, &unused ))
            ERR( "could not restore DXGI factory table protection, error %lu\n", GetLastError() );
        goto release_factory;
    }
    IDXGIFactory2_Release( factory );
    TRACE( "CoreWindow adapter installed for native DXGI\n" );
    return TRUE;

release_factory:
    IDXGIFactory2_Release( factory );
release_module:
    FreeLibrary( module );
    return TRUE;
}

HRESULT WINAPI __wine_uwp_prepare_dxgi( void )
{
    WCHAR enabled[2];

    if (GetEnvironmentVariableW( L"WINE_UWP_DXVK_COREWINDOW", enabled, ARRAY_SIZE(enabled) ) != 1 ||
        enabled[0] != '1') return S_OK;
    InitOnceExecuteOnce( &prepare_once, prepare_dxgi, NULL, NULL );
    return prepare_result;
}

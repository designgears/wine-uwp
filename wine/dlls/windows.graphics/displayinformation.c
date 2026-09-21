/* WinRT Windows.Graphics.Display.DisplayInformation implementation
 *
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

#include <stdlib.h>

#include "private.h"
#include "weakref.h"

WINE_DEFAULT_DEBUG_CHANNEL(display);

/* One DisplayInformation per view. XAML asks for it while it lays out (a Pivot cannot be created
 * without it), and reads the dpi and the orientation; everything here comes from the primary
 * monitor, since a view is not tied to a monitor in Wine yet. */
struct advanced_color_info
{
    IAdvancedColorInfo IAdvancedColorInfo_iface;
    LONG ref;
};

struct display_info
{
    IDisplayInformation IDisplayInformation_iface;
    IDisplayInformation2 IDisplayInformation2_iface;
    IDisplayInformation3 IDisplayInformation3_iface;
    IDisplayInformation4 IDisplayInformation4_iface;
    IDisplayInformation5 IDisplayInformation5_iface;
    /* XAML keeps a weak reference to the view's DisplayInformation and fail-fasts without one. */
    struct weak_reference_source weak_reference_source;
};

static inline struct display_info *impl_from_IDisplayInformation( IDisplayInformation *iface )
{
    return CONTAINING_RECORD( iface, struct display_info, IDisplayInformation_iface );
}

static HRESULT WINAPI display_info_QueryInterface( IDisplayInformation *iface, REFIID iid, void **out )
{
    struct display_info *impl = impl_from_IDisplayInformation( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDisplayInformation ))
        *out = &impl->IDisplayInformation_iface;
    else if (IsEqualGUID( iid, &IID_IDisplayInformation2 ))
        *out = &impl->IDisplayInformation2_iface;
    else if (IsEqualGUID( iid, &IID_IDisplayInformation3 ))
        *out = &impl->IDisplayInformation3_iface;
    else if (IsEqualGUID( iid, &IID_IDisplayInformation4 ))
        *out = &impl->IDisplayInformation4_iface;
    else if (IsEqualGUID( iid, &IID_IDisplayInformation5 ))
        *out = &impl->IDisplayInformation5_iface;
    else if (IsEqualGUID( iid, &IID_IWeakReferenceSource ))
        *out = &impl->weak_reference_source.IWeakReferenceSource_iface;
    else
    {
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI display_info_AddRef( IDisplayInformation *iface )
{
    struct display_info *impl = impl_from_IDisplayInformation( iface );
    return weak_reference_strong_add_ref( &impl->weak_reference_source );
}

static ULONG WINAPI display_info_Release( IDisplayInformation *iface )
{
    struct display_info *impl = impl_from_IDisplayInformation( iface );
    return weak_reference_strong_release( &impl->weak_reference_source );
}

static HRESULT WINAPI display_info_GetIids( IDisplayInformation *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI display_info_GetRuntimeClassName( IDisplayInformation *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Graphics.Display.DisplayInformation";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI display_info_GetTrustLevel( IDisplayInformation *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static UINT get_primary_dpi(void)
{
    HDC hdc = GetDC( NULL );
    UINT dpi = 96;

    if (hdc)
    {
        dpi = GetDeviceCaps( hdc, LOGPIXELSX );
        ReleaseDC( NULL, hdc );
    }
    return dpi ? dpi : 96;
}

static HRESULT WINAPI display_info_get_CurrentOrientation( IDisplayInformation *iface, DisplayOrientations *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = GetSystemMetrics( SM_CXSCREEN ) >= GetSystemMetrics( SM_CYSCREEN ) ?
             DisplayOrientations_Landscape : DisplayOrientations_Portrait;
    return S_OK;
}

static HRESULT WINAPI display_info_get_NativeOrientation( IDisplayInformation *iface, DisplayOrientations *value )
{
    return display_info_get_CurrentOrientation( iface, value );
}

static HRESULT WINAPI display_info_add_OrientationChanged( IDisplayInformation *iface,
        ITypedEventHandler_DisplayInformation_IInspectable *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p: the display does not change here.\n", iface, handler );

    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI display_info_remove_OrientationChanged( IDisplayInformation *iface, EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI display_info_get_ResolutionScale( IDisplayInformation *iface, ResolutionScale *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    /* ResolutionScale is a percentage enum, and only the defined steps are valid. */
    *value = get_primary_dpi() * 100 / 96;
    return S_OK;
}

static HRESULT WINAPI display_info_get_LogicalDpi( IDisplayInformation *iface, FLOAT *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = (FLOAT)get_primary_dpi();
    return S_OK;
}

static HRESULT WINAPI display_info_get_RawDpiX( IDisplayInformation *iface, FLOAT *value )
{
    return display_info_get_LogicalDpi( iface, value );
}

static HRESULT WINAPI display_info_get_RawDpiY( IDisplayInformation *iface, FLOAT *value )
{
    return display_info_get_LogicalDpi( iface, value );
}

static HRESULT WINAPI display_info_add_DpiChanged( IDisplayInformation *iface,
        ITypedEventHandler_DisplayInformation_IInspectable *handler, EventRegistrationToken *token )
{
    return display_info_add_OrientationChanged( iface, handler, token );
}

static HRESULT WINAPI display_info_remove_DpiChanged( IDisplayInformation *iface, EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI display_info_get_StereoEnabled( IDisplayInformation *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI display_info_add_StereoEnabledChanged( IDisplayInformation *iface,
        ITypedEventHandler_DisplayInformation_IInspectable *handler, EventRegistrationToken *token )
{
    return display_info_add_OrientationChanged( iface, handler, token );
}

static HRESULT WINAPI display_info_remove_StereoEnabledChanged( IDisplayInformation *iface, EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI display_info_GetColorProfileAsync( IDisplayInformation *iface,
        IAsyncOperation_IRandomAccessStream **async_info )
{
    FIXME( "iface %p, async_info %p stub!\n", iface, async_info );
    return E_NOTIMPL;
}

static HRESULT WINAPI display_info_add_ColorProfileChanged( IDisplayInformation *iface,
        ITypedEventHandler_DisplayInformation_IInspectable *handler, EventRegistrationToken *token )
{
    return display_info_add_OrientationChanged( iface, handler, token );
}

static HRESULT WINAPI display_info_remove_ColorProfileChanged( IDisplayInformation *iface, EventRegistrationToken token )
{
    return S_OK;
}

static const struct IDisplayInformationVtbl display_info_vtbl =
{
    display_info_QueryInterface,
    display_info_AddRef,
    display_info_Release,
    display_info_GetIids,
    display_info_GetRuntimeClassName,
    display_info_GetTrustLevel,
    display_info_get_CurrentOrientation,
    display_info_get_NativeOrientation,
    display_info_add_OrientationChanged,
    display_info_remove_OrientationChanged,
    display_info_get_ResolutionScale,
    display_info_get_LogicalDpi,
    display_info_get_RawDpiX,
    display_info_get_RawDpiY,
    display_info_add_DpiChanged,
    display_info_remove_DpiChanged,
    display_info_get_StereoEnabled,
    display_info_add_StereoEnabledChanged,
    display_info_remove_StereoEnabledChanged,
    display_info_GetColorProfileAsync,
    display_info_add_ColorProfileChanged,
    display_info_remove_ColorProfileChanged,
};

DEFINE_IINSPECTABLE( display_info2, IDisplayInformation2, struct display_info, IDisplayInformation_iface )

static HRESULT WINAPI display_info2_get_RawPixelsPerViewPixel( IDisplayInformation2 *iface, DOUBLE *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = (DOUBLE)get_primary_dpi() / 96.0;
    return S_OK;
}

static const struct IDisplayInformation2Vtbl display_info2_vtbl =
{
    display_info2_QueryInterface,
    display_info2_AddRef,
    display_info2_Release,
    display_info2_GetIids,
    display_info2_GetRuntimeClassName,
    display_info2_GetTrustLevel,
    display_info2_get_RawPixelsPerViewPixel,
};

DEFINE_IINSPECTABLE( display_info3, IDisplayInformation3, struct display_info, IDisplayInformation_iface )

static HRESULT WINAPI display_info3_get_DiagonalSizeInInches( IDisplayInformation3 *iface,
        IReference_DOUBLE **value )
{
    TRACE( "iface %p, value %p: no physical size.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static const struct IDisplayInformation3Vtbl display_info3_vtbl =
{
    display_info3_QueryInterface,
    display_info3_AddRef,
    display_info3_Release,
    display_info3_GetIids,
    display_info3_GetRuntimeClassName,
    display_info3_GetTrustLevel,
    display_info3_get_DiagonalSizeInInches,
};

DEFINE_IINSPECTABLE( display_info4, IDisplayInformation4, struct display_info, IDisplayInformation_iface )

static HRESULT WINAPI display_info4_get_ScreenWidthInRawPixels( IDisplayInformation4 *iface, UINT32 *value )
{
    if (!value) return E_POINTER;
    *value = GetSystemMetrics( SM_CXSCREEN );
    return S_OK;
}

static HRESULT WINAPI display_info4_get_ScreenHeightInRawPixels( IDisplayInformation4 *iface, UINT32 *value )
{
    if (!value) return E_POINTER;
    *value = GetSystemMetrics( SM_CYSCREEN );
    return S_OK;
}

static const struct IDisplayInformation4Vtbl display_info4_vtbl =
{
    display_info4_QueryInterface,
    display_info4_AddRef,
    display_info4_Release,
    display_info4_GetIids,
    display_info4_GetRuntimeClassName,
    display_info4_GetTrustLevel,
    display_info4_get_ScreenWidthInRawPixels,
    display_info4_get_ScreenHeightInRawPixels,
};

static HRESULT WINAPI advanced_color_QueryInterface( IAdvancedColorInfo *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAdvancedColorInfo ))
    {
        *out = iface;
        IAdvancedColorInfo_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI advanced_color_AddRef( IAdvancedColorInfo *iface )
{
    struct advanced_color_info *impl = CONTAINING_RECORD( iface, struct advanced_color_info, IAdvancedColorInfo_iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI advanced_color_Release( IAdvancedColorInfo *iface )
{
    struct advanced_color_info *impl = CONTAINING_RECORD( iface, struct advanced_color_info, IAdvancedColorInfo_iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI advanced_color_GetIids( IAdvancedColorInfo *iface, ULONG *count, IID **iids )
{
    *count = 0; *iids = NULL; return S_OK;
}

static HRESULT WINAPI advanced_color_GetRuntimeClassName( IAdvancedColorInfo *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Graphics.Display.AdvancedColorInfo",
                                ARRAY_SIZE(L"Windows.Graphics.Display.AdvancedColorInfo") - 1, name );
}

static HRESULT WINAPI advanced_color_GetTrustLevel( IAdvancedColorInfo *iface, TrustLevel *level )
{
    *level = BaseTrust; return S_OK;
}

static HRESULT WINAPI advanced_color_get_CurrentAdvancedColorKind( IAdvancedColorInfo *iface, AdvancedColorKind *value )
{
    if (!value) return E_POINTER;
    *value = AdvancedColorKind_StandardDynamicRange;
    return S_OK;
}

static HRESULT WINAPI advanced_color_get_point( IAdvancedColorInfo *iface, Point *value )
{
    if (!value) return E_POINTER;
    value->X = value->Y = 0.0f;
    return S_OK;
}

static HRESULT WINAPI advanced_color_get_float( IAdvancedColorInfo *iface, FLOAT *value )
{
    if (!value) return E_POINTER;
    *value = 80.0f; /* typical SDR white level in nits */
    return S_OK;
}

static HRESULT WINAPI advanced_color_IsHdrMetadataFormatCurrentlySupported( IAdvancedColorInfo *iface,
        HdrMetadataFormat format, boolean *result )
{
    if (!result) return E_POINTER;
    *result = FALSE;
    return S_OK;
}

static HRESULT WINAPI advanced_color_IsAdvancedColorKindAvailable( IAdvancedColorInfo *iface,
        AdvancedColorKind kind, boolean *result )
{
    if (!result) return E_POINTER;
    *result = (kind == AdvancedColorKind_StandardDynamicRange);
    return S_OK;
}

static const struct IAdvancedColorInfoVtbl advanced_color_vtbl =
{
    advanced_color_QueryInterface, advanced_color_AddRef, advanced_color_Release,
    advanced_color_GetIids, advanced_color_GetRuntimeClassName, advanced_color_GetTrustLevel,
    advanced_color_get_CurrentAdvancedColorKind,
    advanced_color_get_point, /* RedPrimary */
    advanced_color_get_point, /* GreenPrimary */
    advanced_color_get_point, /* BluePrimary */
    advanced_color_get_point, /* WhitePoint */
    advanced_color_get_float, /* MaxLuminanceInNits */
    advanced_color_get_float, /* MinLuminanceInNits */
    advanced_color_get_float, /* MaxAverageFullFrameLuminanceInNits */
    advanced_color_get_float, /* SdrWhiteLevelInNits */
    advanced_color_IsHdrMetadataFormatCurrentlySupported,
    advanced_color_IsAdvancedColorKindAvailable,
};

static HRESULT create_advanced_color_info( IAdvancedColorInfo **out )
{
    struct advanced_color_info *impl;
    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAdvancedColorInfo_iface.lpVtbl = &advanced_color_vtbl;
    impl->ref = 1;
    *out = &impl->IAdvancedColorInfo_iface;
    return S_OK;
}

DEFINE_IINSPECTABLE( display_info5, IDisplayInformation5, struct display_info, IDisplayInformation_iface )

static HRESULT WINAPI display_info5_GetAdvancedColorInfo( IDisplayInformation5 *iface, IAdvancedColorInfo **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return create_advanced_color_info( value );
}

static HRESULT WINAPI display_info5_add_AdvancedColorInfoChanged( IDisplayInformation5 *iface,
        ITypedEventHandler_DisplayInformation_IInspectable *handler, EventRegistrationToken *token )
{
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI display_info5_remove_AdvancedColorInfoChanged( IDisplayInformation5 *iface,
        EventRegistrationToken token )
{
    return S_OK;
}

static const struct IDisplayInformation5Vtbl display_info5_vtbl =
{
    display_info5_QueryInterface,
    display_info5_AddRef,
    display_info5_Release,
    display_info5_GetIids,
    display_info5_GetRuntimeClassName,
    display_info5_GetTrustLevel,
    display_info5_GetAdvancedColorInfo,
    display_info5_add_AdvancedColorInfoChanged,
    display_info5_remove_AdvancedColorInfoChanged,
};

HRESULT display_information_create( IDisplayInformation **out )
{
    struct display_info *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IDisplayInformation_iface.lpVtbl = &display_info_vtbl;
    impl->IDisplayInformation2_iface.lpVtbl = &display_info2_vtbl;
    impl->IDisplayInformation3_iface.lpVtbl = &display_info3_vtbl;
    impl->IDisplayInformation4_iface.lpVtbl = &display_info4_vtbl;
    impl->IDisplayInformation5_iface.lpVtbl = &display_info5_vtbl;
    if (FAILED(hr = weak_reference_source_init( &impl->weak_reference_source,
                                                (IUnknown *)&impl->IDisplayInformation_iface )))
    {
        free( impl );
        return hr;
    }

    TRACE( "created display information %p.\n", impl );
    *out = &impl->IDisplayInformation_iface;
    return S_OK;
}

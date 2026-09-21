/* WinRT Windows.Globalization.NumberFormatting implementation
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
#include <wchar.h>
#include <math.h>

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(locale);

/* The NumberFormatting interfaces are not in Wine's IDL, so their IIDs and vtable order come from
 * Windows.Globalization.winmd. Each interface is a plain vtable on the one formatter object. */
static const GUID IID_IDecimalFormatterFactory =
    { 0x0d018c9a, 0xe393, 0x46b8, { 0xb8, 0x30, 0x7a, 0x69, 0xc8, 0xf8, 0x9f, 0xbb } };
static const GUID IID_INumberFormatter =
    { 0xa5007c49, 0x7676, 0x4db7, { 0x86, 0x31, 0x1b, 0x6f, 0xf2, 0x65, 0xca, 0xa9 } };
static const GUID IID_INumberFormatter2 =
    { 0xd4a8c1f0, 0x80d0, 0x4b0d, { 0xa8, 0x9e, 0x88, 0x2c, 0x1e, 0x8f, 0x83, 0x10 } };
static const GUID IID_INumberFormatterOptions =
    { 0x80332d21, 0xaee1, 0x4a39, { 0xba, 0xa2, 0x07, 0xed, 0x8c, 0x96, 0xda, 0xf6 } };
static const GUID IID_INumberParser =
    { 0xe6659412, 0x4a13, 0x4a53, { 0x83, 0xa1, 0x39, 0x2f, 0xbe, 0x4c, 0xff, 0x9f } };
static const GUID IID_INumberRounderOption =
    { 0x3b088433, 0x646f, 0x4efe, { 0x8d, 0x48, 0x66, 0xeb, 0x2e, 0x49, 0xe7, 0x36 } };
static const GUID IID_ISignificantDigitsOption =
    { 0x1d4dfcdd, 0x2d43, 0x4ee8, { 0xbb, 0xf1, 0xc1, 0xb2, 0x6a, 0x71, 0x1a, 0x58 } };
static const GUID IID_ISignedZeroOption =
    { 0xfd1cdd31, 0x0a3c, 0x49c4, { 0xa6, 0x42, 0x96, 0xa1, 0x56, 0x4f, 0x4f, 0x30 } };

struct formatter
{
    const void **formatter_vtbl;
    const void **formatter2_vtbl;
    const void **options_vtbl;
    const void **parser_vtbl;
    const void **rounder_vtbl;
    const void **significant_vtbl;
    const void **signed_zero_vtbl;
    LONG ref;

    WCHAR language[LOCALE_NAME_MAX_LENGTH];
    WCHAR region[16];
    INT32 integer_digits;
    INT32 fraction_digits;
    INT32 significant_digits;
    boolean is_grouped;
    boolean always_decimal_point;
    boolean signed_zero;
    HSTRING numeral_system;
};

static const void *formatter_vtbl[];
static const void *formatter2_vtbl[];
static const void *options_vtbl[];
static const void *parser_vtbl[];
static const void *rounder_vtbl[];
static const void *significant_vtbl[];
static const void *signed_zero_vtbl[];

static HRESULT formatter_query_interface( struct formatter *impl, REFIID iid, void **out )
{
    TRACE( "impl %p, iid %s, out %p.\n", impl, debugstr_guid(iid), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_INumberFormatter ))
        *out = &impl->formatter_vtbl;
    else if (IsEqualGUID( iid, &IID_INumberFormatter2 ))
        *out = &impl->formatter2_vtbl;
    else if (IsEqualGUID( iid, &IID_INumberFormatterOptions ))
        *out = &impl->options_vtbl;
    else if (IsEqualGUID( iid, &IID_INumberParser ))
        *out = &impl->parser_vtbl;
    else if (IsEqualGUID( iid, &IID_INumberRounderOption ))
        *out = &impl->rounder_vtbl;
    else if (IsEqualGUID( iid, &IID_ISignificantDigitsOption ))
        *out = &impl->significant_vtbl;
    else if (IsEqualGUID( iid, &IID_ISignedZeroOption ))
        *out = &impl->signed_zero_vtbl;
    else
    {
        WARN( "DecimalFormatter: no interface %s.\n", debugstr_guid(iid) );
        *out = NULL;
        return E_NOINTERFACE;
    }

    InterlockedIncrement( &impl->ref );
    return S_OK;
}

static ULONG formatter_release( struct formatter *impl )
{
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->numeral_system );
        free( impl );
    }
    return ref;
}

/* IUnknown is per interface: each vtable is a separate field of the one object, so the thunks have
 * to walk back from the field they were called on. */
#define FORMATTER_IUNKNOWN( pfx, field )                                                           \
    static HRESULT WINAPI pfx##_QueryInterface( void *iface, REFIID iid, void **out )              \
    {                                                                                              \
        return formatter_query_interface( CONTAINING_RECORD( iface, struct formatter, field ),     \
                                          iid, out );                                              \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( void *iface )                                                \
    {                                                                                              \
        return InterlockedIncrement( &CONTAINING_RECORD( iface, struct formatter, field )->ref );   \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( void *iface )                                               \
    {                                                                                              \
        return formatter_release( CONTAINING_RECORD( iface, struct formatter, field ) );           \
    }
FORMATTER_IUNKNOWN( formatter, formatter_vtbl )
FORMATTER_IUNKNOWN( formatter2, formatter2_vtbl )
FORMATTER_IUNKNOWN( options, options_vtbl )
FORMATTER_IUNKNOWN( parser, parser_vtbl )
FORMATTER_IUNKNOWN( rounder, rounder_vtbl )
FORMATTER_IUNKNOWN( significant, significant_vtbl )
FORMATTER_IUNKNOWN( signed_zero, signed_zero_vtbl )

static HRESULT WINAPI formatter_GetIids( void *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI formatter_GetRuntimeClassName( void *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Globalization.NumberFormatting.DecimalFormatter";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI formatter_GetTrustLevel( void *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

/* Windows resolves separators and grouping from the locale, so ask the same source. */
static void locale_info( const WCHAR *locale, LCTYPE type, WCHAR *buffer, int size, const WCHAR *fallback )
{
    if (!GetLocaleInfoEx( locale, type, buffer, size )) lstrcpynW( buffer, fallback, size );
}

static HRESULT format_number( struct formatter *impl, const WCHAR *digits, BOOL negative, HSTRING *out )
{
    WCHAR decimal_sep[8], group_sep[8], grouping[16], buffer[256], *dst = buffer;
    const WCHAR *fraction = wcschr( digits, '.' );
    int integer_len = fraction ? (int)(fraction - digits) : (int)wcslen( digits );
    int group_size, i;

    locale_info( impl->language, LOCALE_SDECIMAL, decimal_sep, ARRAY_SIZE(decimal_sep), L"." );
    locale_info( impl->language, LOCALE_STHOUSAND, group_sep, ARRAY_SIZE(group_sep), L"," );
    locale_info( impl->language, LOCALE_SGROUPING, grouping, ARRAY_SIZE(grouping), L"3;0" );
    group_size = _wtoi( grouping );
    if (group_size <= 0) group_size = 3;

    if (negative) *dst++ = '-';
    for (i = 0; i < integer_len; i++)
    {
        if (impl->is_grouped && i && !((integer_len - i) % group_size))
        {
            lstrcpyW( dst, group_sep );
            dst += lstrlenW( dst );
        }
        *dst++ = digits[i];
    }
    if (fraction && fraction[1])
    {
        lstrcpyW( dst, decimal_sep );
        dst += lstrlenW( dst );
        lstrcpyW( dst, fraction + 1 );
        dst += lstrlenW( dst );
    }
    else if (impl->always_decimal_point)
    {
        lstrcpyW( dst, decimal_sep );
        dst += lstrlenW( dst );
    }
    *dst = 0;

    TRACE( "%s -> %s.\n", debugstr_w(digits), debugstr_w(buffer) );
    return WindowsCreateString( buffer, lstrlenW( buffer ), out );
}

static HRESULT format_double( struct formatter *impl, DOUBLE value, HSTRING *out )
{
    WCHAR digits[256];
    BOOL negative = value < 0 || (value == 0 && impl->signed_zero && signbit( value ));

    if (!out) return E_POINTER;
    swprintf( digits, ARRAY_SIZE(digits), L"%0*.*f", impl->integer_digits,
              impl->fraction_digits, negative ? -value : value );
    return format_number( impl, digits, negative, out );
}

static HRESULT WINAPI formatter_Format( void *iface, INT64 value, HSTRING *out )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, formatter_vtbl );
    WCHAR digits[64];

    TRACE( "value %I64d.\n", value );

    if (!out) return E_POINTER;
    if (impl->fraction_digits > 0) return format_double( impl, (DOUBLE)value, out );
    swprintf( digits, ARRAY_SIZE(digits), L"%0*I64u", impl->integer_digits,
              value < 0 ? (UINT64)-value : (UINT64)value );
    return format_number( impl, digits, value < 0, out );
}

static HRESULT WINAPI formatter_FormatUInt( void *iface, UINT64 value, HSTRING *out )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, formatter_vtbl );
    WCHAR digits[64];

    TRACE( "value %I64u.\n", value );

    if (!out) return E_POINTER;
    if (impl->fraction_digits > 0) return format_double( impl, (DOUBLE)value, out );
    swprintf( digits, ARRAY_SIZE(digits), L"%0*I64u", impl->integer_digits, value );
    return format_number( impl, digits, FALSE, out );
}

static HRESULT WINAPI formatter_FormatDouble( void *iface, DOUBLE value, HSTRING *out )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, formatter_vtbl );

    TRACE( "value %f.\n", value );
    return format_double( impl, value, out );
}

static const void *formatter_vtbl[] =
{
    formatter_QueryInterface, formatter_AddRef, formatter_Release,
    formatter_GetIids, formatter_GetRuntimeClassName, formatter_GetTrustLevel,
    formatter_Format, formatter_FormatUInt, formatter_FormatDouble,
};

/* INumberFormatter2: the same three methods on their own vtable. */
#define FORMATTER2_THUNK(name, type, fmt)                                                          \
    static HRESULT WINAPI formatter2_##name( void *iface, type value, HSTRING *out )               \
    {                                                                                              \
        struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, formatter2_vtbl );    \
        return fmt( &impl->formatter_vtbl, value, out );                                           \
    }
FORMATTER2_THUNK( FormatInt, INT64, formatter_Format )
FORMATTER2_THUNK( FormatUInt, UINT64, formatter_FormatUInt )
FORMATTER2_THUNK( FormatDouble, DOUBLE, formatter_FormatDouble )

static const void *formatter2_vtbl[] =
{
    formatter2_QueryInterface, formatter2_AddRef, formatter2_Release,
    formatter_GetIids, formatter_GetRuntimeClassName, formatter_GetTrustLevel,
    formatter2_FormatInt, formatter2_FormatUInt, formatter2_FormatDouble,
};

/*
 * INumberFormatterOptions
 */
#define OPTIONS_IMPL( name, field, type )                                                          \
    static HRESULT WINAPI options_get_##name( void *iface, type *value )                           \
    {                                                                                              \
        struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, options_vtbl );       \
        TRACE( "value %p.\n", value );                                                             \
        if (!value) return E_POINTER;                                                              \
        *value = impl->field;                                                                      \
        return S_OK;                                                                               \
    }                                                                                              \
    static HRESULT WINAPI options_put_##name( void *iface, type value )                            \
    {                                                                                              \
        struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, options_vtbl );       \
        TRACE( "value %d.\n", (int)value );                                                        \
        impl->field = value;                                                                       \
        return S_OK;                                                                               \
    }
OPTIONS_IMPL( IntegerDigits, integer_digits, INT32 )
OPTIONS_IMPL( FractionDigits, fraction_digits, INT32 )
OPTIONS_IMPL( IsGrouped, is_grouped, boolean )
OPTIONS_IMPL( IsDecimalPointAlwaysDisplayed, always_decimal_point, boolean )

static HRESULT WINAPI options_get_Languages( void *iface, IVectorView_HSTRING **value )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, options_vtbl );
    HSTRING languages[1];
    HRESULT hr;

    TRACE( "value %p.\n", value );

    if (!value) return E_POINTER;
    if (FAILED(hr = WindowsCreateString( impl->language, lstrlenW( impl->language ), &languages[0] )))
        return hr;
    return hstring_vector_create( languages, 1, value );
}

static HRESULT WINAPI options_get_GeographicRegion( void *iface, HSTRING *value )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, options_vtbl );

    TRACE( "value %p.\n", value );

    if (!value) return E_POINTER;
    return WindowsCreateString( impl->region, lstrlenW( impl->region ), value );
}

static HRESULT WINAPI options_get_NumeralSystem( void *iface, HSTRING *value )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, options_vtbl );
    return WindowsDuplicateString( impl->numeral_system, value );
}

static HRESULT WINAPI options_put_NumeralSystem( void *iface, HSTRING value )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, options_vtbl );
    HSTRING copy;
    HRESULT hr;

    TRACE( "value %s.\n", debugstr_hstring(value) );

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->numeral_system );
    impl->numeral_system = copy;
    return S_OK;
}

static HRESULT WINAPI options_get_ResolvedLanguage( void *iface, HSTRING *value )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, options_vtbl );

    TRACE( "value %p -> %s.\n", value, debugstr_w(impl->language) );

    if (!value) return E_POINTER;
    return WindowsCreateString( impl->language, lstrlenW( impl->language ), value );
}

static HRESULT WINAPI options_get_ResolvedGeographicRegion( void *iface, HSTRING *value )
{
    return options_get_GeographicRegion( iface, value );
}

static const void *options_vtbl[] =
{
    options_QueryInterface, options_AddRef, options_Release,
    formatter_GetIids, formatter_GetRuntimeClassName, formatter_GetTrustLevel,
    options_get_Languages, options_get_GeographicRegion,
    options_get_IntegerDigits, options_put_IntegerDigits,
    options_get_FractionDigits, options_put_FractionDigits,
    options_get_IsGrouped, options_put_IsGrouped,
    options_get_IsDecimalPointAlwaysDisplayed, options_put_IsDecimalPointAlwaysDisplayed,
    options_get_NumeralSystem, options_put_NumeralSystem,
    options_get_ResolvedLanguage, options_get_ResolvedGeographicRegion,
};

/*
 * INumberParser: the parsed value comes back as an IReference<T>, or NULL when the text is not a
 * number, which is how the projection produces a nullable.
 */
static HRESULT parse_text( HSTRING text, struct formatter *impl, WCHAR *buffer, UINT32 size )
{
    WCHAR group_sep[8], decimal_sep[8];
    const WCHAR *src = WindowsGetStringRawBuffer( text, NULL );
    UINT32 len = 0;

    if (!src) return E_INVALIDARG;
    locale_info( impl->language, LOCALE_STHOUSAND, group_sep, ARRAY_SIZE(group_sep), L"," );
    locale_info( impl->language, LOCALE_SDECIMAL, decimal_sep, ARRAY_SIZE(decimal_sep), L"." );

    while (*src && len < size - 1)
    {
        if (*src == group_sep[0]) src++;
        else if (*src == decimal_sep[0]) { buffer[len++] = '.'; src++; }
        else buffer[len++] = *src++;
    }
    buffer[len] = 0;
    return S_OK;
}

static HRESULT WINAPI parser_ParseInt( void *iface, HSTRING text, void **value )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, parser_vtbl );
    WCHAR buffer[256], *end;
    INT64 parsed;

    TRACE( "text %s.\n", debugstr_hstring(text) );

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(parse_text( text, impl, buffer, ARRAY_SIZE(buffer) ))) return S_OK;
    parsed = wcstoll( buffer, &end, 10 );
    if (end == buffer) return S_OK;
    FIXME( "parsed %I64d, but IReference<INT64> is not implemented.\n", parsed );
    return S_OK;
}

static HRESULT WINAPI parser_ParseUInt( void *iface, HSTRING text, void **value )
{
    return parser_ParseInt( iface, text, value );
}

static HRESULT WINAPI parser_ParseDouble( void *iface, HSTRING text, void **value )
{
    return parser_ParseInt( iface, text, value );
}

static const void *parser_vtbl[] =
{
    parser_QueryInterface, parser_AddRef, parser_Release,
    formatter_GetIids, formatter_GetRuntimeClassName, formatter_GetTrustLevel,
    parser_ParseInt, parser_ParseUInt, parser_ParseDouble,
};

/*
 * INumberRounderOption, ISignificantDigitsOption, ISignedZeroOption
 */
static HRESULT WINAPI rounder_get_NumberRounder( void *iface, IInspectable **value )
{
    TRACE( "value %p: no rounder set.\n", value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI rounder_put_NumberRounder( void *iface, IInspectable *value )
{
    FIXME( "value %p: ignored.\n", value );
    return S_OK;
}

static const void *rounder_vtbl[] =
{
    rounder_QueryInterface, rounder_AddRef, rounder_Release,
    formatter_GetIids, formatter_GetRuntimeClassName, formatter_GetTrustLevel,
    rounder_get_NumberRounder, rounder_put_NumberRounder,
};

static HRESULT WINAPI significant_get_SignificantDigits( void *iface, INT32 *value )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, significant_vtbl );
    if (!value) return E_POINTER;
    *value = impl->significant_digits;
    return S_OK;
}

static HRESULT WINAPI significant_put_SignificantDigits( void *iface, INT32 value )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, significant_vtbl );
    impl->significant_digits = value;
    return S_OK;
}

static const void *significant_vtbl[] =
{
    significant_QueryInterface, significant_AddRef, significant_Release,
    formatter_GetIids, formatter_GetRuntimeClassName, formatter_GetTrustLevel,
    significant_get_SignificantDigits, significant_put_SignificantDigits,
};

static HRESULT WINAPI signed_zero_get_IsZeroSigned( void *iface, boolean *value )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, signed_zero_vtbl );
    if (!value) return E_POINTER;
    *value = impl->signed_zero;
    return S_OK;
}

static HRESULT WINAPI signed_zero_put_IsZeroSigned( void *iface, boolean value )
{
    struct formatter *impl = CONTAINING_RECORD( iface, struct formatter, signed_zero_vtbl );
    impl->signed_zero = value;
    return S_OK;
}

static const void *signed_zero_vtbl[] =
{
    signed_zero_QueryInterface, signed_zero_AddRef, signed_zero_Release,
    formatter_GetIids, formatter_GetRuntimeClassName, formatter_GetTrustLevel,
    signed_zero_get_IsZeroSigned, signed_zero_put_IsZeroSigned,
};

static HRESULT create_formatter( const WCHAR *language, const WCHAR *region, void **out )
{
    struct formatter *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->formatter_vtbl = formatter_vtbl;
    impl->formatter2_vtbl = formatter2_vtbl;
    impl->options_vtbl = options_vtbl;
    impl->parser_vtbl = parser_vtbl;
    impl->rounder_vtbl = rounder_vtbl;
    impl->significant_vtbl = significant_vtbl;
    impl->signed_zero_vtbl = signed_zero_vtbl;
    impl->ref = 1;
    /* Windows resolves the requested language against the ones the system has; an unresolvable
     * language falls back to the user default, which is what ResolveLocaleName does here. */
    if (!language || !*language || !ResolveLocaleName( language, impl->language, ARRAY_SIZE(impl->language) ))
        GetUserDefaultLocaleName( impl->language, ARRAY_SIZE(impl->language) );
    if (region && *region) lstrcpynW( impl->region, region, ARRAY_SIZE(impl->region) );
    else locale_info( impl->language, LOCALE_SISO3166CTRYNAME, impl->region, ARRAY_SIZE(impl->region), L"US" );
    impl->integer_digits = 1;
    impl->fraction_digits = 2;
    impl->significant_digits = 0;
    impl->is_grouped = FALSE;
    impl->always_decimal_point = FALSE;
    WindowsCreateString( L"LatnNumeralSystem", 17, &impl->numeral_system );

    TRACE( "created formatter %p for %s / %s.\n", impl, debugstr_w(impl->language), debugstr_w(impl->region) );
    *out = &impl->formatter_vtbl;
    return S_OK;
}

/*
 * The activation factory: IActivationFactory for the default constructor and
 * IDecimalFormatterFactory::CreateDecimalFormatter for the (languages, region) one.
 */
struct formatter_factory
{
    const void **activation_vtbl;      /* IActivationFactory */
    const void **decimal_vtbl;         /* IDecimalFormatterFactory, its own vtable: the extra
                                        * method sits at slot 6, where ActivateInstance is. */
};

static const void *activation_vtbl[];
static const void *decimal_factory_vtbl[];
static struct formatter_factory formatter_factory;

static HRESULT WINAPI factory_QueryInterface( void *iface, REFIID iid, void **out )
{
    TRACE( "iid %s.\n", debugstr_guid(iid) );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &formatter_factory.activation_vtbl;
    else if (IsEqualGUID( iid, &IID_IDecimalFormatterFactory ))
        *out = &formatter_factory.decimal_vtbl;
    else
    {
        WARN( "DecimalFormatter factory: no interface %s.\n", debugstr_guid(iid) );
        *out = NULL;
        return E_NOINTERFACE;
    }

    return S_OK;
}

static ULONG WINAPI factory_AddRef( void *iface ) { return 2; }
static ULONG WINAPI factory_Release( void *iface ) { return 1; }

static HRESULT WINAPI factory_GetIids( void *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return formatter_GetRuntimeClassName( NULL, name );
}

static HRESULT WINAPI factory_GetTrustLevel( void *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( void *iface, IInspectable **instance )
{
    TRACE( "instance %p.\n", instance );

    if (!instance) return E_POINTER;
    return create_formatter( NULL, NULL, (void **)instance );
}

/* IDecimalFormatterFactory::CreateDecimalFormatter: only the first language is used, which is what
 * Windows resolves to unless the system has none of them installed. */
static HRESULT WINAPI factory_CreateDecimalFormatter( void *iface, IIterable_HSTRING *languages,
                                                      HSTRING region, void **out )
{
    const WCHAR *language = NULL, *region_str = WindowsGetStringRawBuffer( region, NULL );
    IIterator_HSTRING *iterator;
    HSTRING first = NULL;
    boolean has_current;

    TRACE( "languages %p, region %s, out %p.\n", languages, debugstr_hstring(region), out );

    if (!out) return E_POINTER;
    if (languages && SUCCEEDED(IIterable_HSTRING_First( languages, &iterator )))
    {
        if (SUCCEEDED(IIterator_HSTRING_get_HasCurrent( iterator, &has_current )) && has_current)
            IIterator_HSTRING_get_Current( iterator, &first );
        IIterator_HSTRING_Release( iterator );
    }
    if (first) language = WindowsGetStringRawBuffer( first, NULL );

    return create_formatter( language, region_str, out );
}

static const void *activation_vtbl[] =
{
    factory_QueryInterface, factory_AddRef, factory_Release,
    factory_GetIids, factory_GetRuntimeClassName, factory_GetTrustLevel,
    factory_ActivateInstance,
};

static const void *decimal_factory_vtbl[] =
{
    factory_QueryInterface, factory_AddRef, factory_Release,
    factory_GetIids, factory_GetRuntimeClassName, factory_GetTrustLevel,
    factory_CreateDecimalFormatter,
};

static struct formatter_factory formatter_factory = { activation_vtbl, decimal_factory_vtbl };

IActivationFactory *number_formatting_factory = (IActivationFactory *)&formatter_factory.activation_vtbl;

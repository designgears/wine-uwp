/*
 * XStore
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
#include <string.h>

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdk);

/*
 * The title runs because the Xbox app installed it for the signed-in user, who therefore holds its licence. Queries
 * answer an active full licence, an empty catalogue and no add-ons; the store UI calls complete without doing anything.
 */

enum { XStoreCanLicenseStatus_NotLicensableToUser, XStoreCanLicenseStatus_Licensable };

typedef struct
{
    char skuStoreId[18];
    BOOLEAN isActive;
    BOOLEAN isTrialOwnedByThisUser;
    BOOLEAN isDiscLicense;
    BOOLEAN isTrial;
    UINT32 trialTimeRemainingInSeconds;
    char trialUniqueId[64];
    INT64 expirationDate;
} XStoreGameLicense;

typedef struct
{
    char licensableSku[5];
    UINT32 status;
} XStoreCanAcquireLicenseResult;

typedef struct { UINT32 quantity; } XStoreConsumableResult;
typedef struct { BOOLEAN wasUpdated; } XStoreRateAndReviewResult;

static const char game_license_identity[] = "XStoreQueryGameLicenseAsync";
static const char acquire_package_identity[] = "XStoreAcquireLicenseForPackageAsync";
static const char acquire_durables_identity[] = "XStoreAcquireLicenseForDurablesAsync";
static const char can_acquire_store_id_identity[] = "XStoreCanAcquireLicenseForStoreIdAsync";
static const char can_acquire_package_identity[] = "XStoreCanAcquireLicenseForPackageAsync";

static HRESULT WINAPI game_license_provider( UINT32 op, const XAsyncProviderData *data )
{
    XStoreGameLicense *license;

    switch (op)
    {
    case XAsyncOp_DoWork:
        async_complete( data->async, S_OK, sizeof(XStoreGameLicense) );
        return S_OK;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < sizeof(XStoreGameLicense)) return E_INVALIDARG;
        license = data->buffer;
        memset( license, 0, sizeof(*license) );
        strcpy( license->skuStoreId, "0000000000000000" );
        license->isActive = TRUE;
        return S_OK;
    }
    return S_OK;
}

static HRESULT WINAPI acquire_license_provider( UINT32 op, const XAsyncProviderData *data )
{
    switch (op)
    {
    case XAsyncOp_DoWork:
        async_complete( data->async, S_OK, sizeof(void *) );
        return S_OK;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < sizeof(void *)) return E_INVALIDARG;
        *(void **)data->buffer = (void *)1;
        return S_OK;
    }
    return S_OK;
}

static HRESULT WINAPI can_acquire_provider( UINT32 op, const XAsyncProviderData *data )
{
    XStoreCanAcquireLicenseResult *result;

    switch (op)
    {
    case XAsyncOp_DoWork:
        async_complete( data->async, S_OK, sizeof(XStoreCanAcquireLicenseResult) );
        return S_OK;
    case XAsyncOp_GetResult:
        if (!data->buffer || data->bufferSize < sizeof(XStoreCanAcquireLicenseResult)) return E_INVALIDARG;
        result = data->buffer;
        memset( result, 0, sizeof(*result) );
        strcpy( result->licensableSku, "0000" );
        result->status = XStoreCanLicenseStatus_Licensable;
        return S_OK;
    }
    return S_OK;
}

static HRESULT WINAPI store_QueryInterface( gdk_object *iface, REFIID iid, void **out );
static ULONG WINAPI store_AddRef( gdk_object *iface ) { return 2; }
static ULONG WINAPI store_Release( gdk_object *iface ) { return 1; }

static HRESULT WINAPI store_CreateContext( gdk_object *iface, void *user, void **context )
{
    if (!context) return E_POINTER;
    *context = user ? user : (void *)1;
    TRACE( "user %p -> %p\n", user, *context );
    return S_OK;
}

static void WINAPI store_nothing( gdk_object *iface )
{
}

static HRESULT WINAPI store_notimpl( gdk_object *iface )
{
    return E_NOTIMPL;
}

/* the asynchronous calls that complete with nothing: the async block is their last argument */
#define EMPTY_ASYNC(name, ...) \
    static HRESULT WINAPI store_##name( gdk_object *iface, __VA_ARGS__, XAsyncBlock *block ) \
    { return async_begin_empty( block, "XStore" #name ); }

EMPTY_ASYNC(QueryAssociatedProductsAsync, void *context, UINT32 kind, UINT32 max)
EMPTY_ASYNC(QueryProductsAsync, void *context, UINT32 kind, const char **ids, SIZE_T count, const char **tags,
            SIZE_T tag_count)
EMPTY_ASYNC(QueryEntitledProductsAsync, void *context, UINT32 kind, UINT32 max)
EMPTY_ASYNC(QueryProductForCurrentGameAsync, void *context)
EMPTY_ASYNC(QueryProductForPackageAsync, void *context, UINT32 kind, const char *package)
EMPTY_ASYNC(ProductsQueryNextPageAsync, void *query)
EMPTY_ASYNC(QueryAddOnLicensesAsync, void *context)
EMPTY_ASYNC(QueryConsumableBalanceRemainingAsync, void *context, const char *id)
EMPTY_ASYNC(ReportConsumableFulfillmentAsync, void *context, const char *id, UINT32 quantity, GUID tracking)
EMPTY_ASYNC(ShowPurchaseUIAsync, void *context, const char *id, const char *name, const char *extra)
EMPTY_ASYNC(ShowRateAndReviewUIAsync, void *context)
EMPTY_ASYNC(ShowRedeemTokenUIAsync, void *context, const char *token, const char **ids, SIZE_T count,
            BOOLEAN offline)
EMPTY_ASYNC(QueryGameAndDlcPackageUpdatesAsync, void *context)
EMPTY_ASYNC(DownloadPackageUpdatesAsync, void *context, const char **ids, SIZE_T count)
EMPTY_ASYNC(DownloadAndInstallPackageUpdatesAsync, void *context, const char **ids, SIZE_T count)
EMPTY_ASYNC(DownloadAndInstallPackagesAsync, void *context, const char **ids, SIZE_T count)
EMPTY_ASYNC(ShowAssociatedProductsUIAsync, void *context, const char *id, UINT32 kind)
EMPTY_ASYNC(ShowProductPageUIAsync, void *context, const char *id)

/* a product query's handle is NULL: the catalogue is empty */
static HRESULT WINAPI store_query_result( gdk_object *iface, XAsyncBlock *block, void **query )
{
    if (query) *query = NULL;
    return async_get_status( block, TRUE );
}

static HRESULT WINAPI store_status_result( gdk_object *iface, XAsyncBlock *block )
{
    return async_get_status( block, TRUE );
}

static HRESULT WINAPI store_EnumerateProductsQuery( gdk_object *iface, void *query, void *context, void *callback )
{
    return S_OK;
}

static BOOLEAN WINAPI store_ProductsQueryHasMorePages( gdk_object *iface, void *query )
{
    return FALSE;
}

static HRESULT WINAPI store_AcquireLicenseForPackageAsync( gdk_object *iface, void *context, const char *package,
                                                           XAsyncBlock *block )
{
    TRACE( "%s\n", debugstr_a(package) );
    return async_begin( block, NULL, acquire_package_identity, acquire_package_identity, acquire_license_provider );
}

static HRESULT WINAPI store_AcquireLicenseForPackageResult( gdk_object *iface, XAsyncBlock *block, void **license )
{
    if (!license) return E_POINTER;
    *license = NULL;
    return async_get_result( block, acquire_package_identity, sizeof(*license), license, NULL );
}

static BOOLEAN WINAPI store_IsLicenseValid( gdk_object *iface, void *license )
{
    return license != NULL;
}

static HRESULT WINAPI store_CanAcquireLicenseForStoreIdAsync( gdk_object *iface, void *context, const char *id,
                                                              XAsyncBlock *block )
{
    return async_begin( block, NULL, can_acquire_store_id_identity, can_acquire_store_id_identity,
                        can_acquire_provider );
}

static HRESULT WINAPI store_CanAcquireLicenseForStoreIdResult( gdk_object *iface, XAsyncBlock *block,
                                                               XStoreCanAcquireLicenseResult *result )
{
    if (!result) return E_POINTER;
    return async_get_result( block, can_acquire_store_id_identity, sizeof(*result), result, NULL );
}

static HRESULT WINAPI store_CanAcquireLicenseForPackageAsync( gdk_object *iface, void *context, const char *package,
                                                              XAsyncBlock *block )
{
    return async_begin( block, NULL, can_acquire_package_identity, can_acquire_package_identity,
                        can_acquire_provider );
}

static HRESULT WINAPI store_CanAcquireLicenseForPackageResult( gdk_object *iface, XAsyncBlock *block,
                                                               XStoreCanAcquireLicenseResult *result )
{
    if (!result) return E_POINTER;
    return async_get_result( block, can_acquire_package_identity, sizeof(*result), result, NULL );
}

static HRESULT WINAPI store_QueryGameLicenseAsync( gdk_object *iface, void *context, XAsyncBlock *block )
{
    TRACE( "\n" );
    return async_begin( block, NULL, game_license_identity, game_license_identity, game_license_provider );
}

static HRESULT WINAPI store_QueryGameLicenseResult( gdk_object *iface, XAsyncBlock *block, XStoreGameLicense *license )
{
    if (!license) return E_POINTER;
    return async_get_result( block, game_license_identity, sizeof(*license), license, NULL );
}

static HRESULT WINAPI store_count_result( gdk_object *iface, XAsyncBlock *block, UINT32 *count )
{
    if (count) *count = 0;
    return async_get_status( block, TRUE );
}

static HRESULT WINAPI store_QueryAddOnLicensesResultCount( gdk_object *iface, XAsyncBlock *block, UINT32 *count )
{
    if (!count) return E_POINTER;
    *count = 0;
    return async_get_status( block, TRUE );
}

static HRESULT WINAPI store_indexed_result( gdk_object *iface, XAsyncBlock *block, UINT32 count, void *results )
{
    return async_get_status( block, TRUE );
}

static HRESULT WINAPI store_consumable_result( gdk_object *iface, XAsyncBlock *block, XStoreConsumableResult *result )
{
    if (result) result->quantity = 0;
    return async_get_status( block, TRUE );
}

static HRESULT WINAPI store_GetUserCollectionsIdAsync( gdk_object *iface )
{
    FIXME( "not implemented\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI store_GetUserPurchaseIdAsync( gdk_object *iface )
{
    FIXME( "not implemented\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI store_QueryLicenseTokenAsync( gdk_object *iface )
{
    FIXME( "not implemented\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI store_ShowRateAndReviewUIResult( gdk_object *iface, XAsyncBlock *block,
                                                       XStoreRateAndReviewResult *result )
{
    if (result) result->wasUpdated = FALSE;
    return async_get_status( block, TRUE );
}

static HRESULT WINAPI store_QueryPackageIdentifier( gdk_object *iface, const char *store_id, SIZE_T size, char *buffer )
{
    if (!buffer || size < 2) return E_INVALIDARG;
    buffer[0] = 0;
    return S_OK;
}

static HRESULT WINAPI store_register( gdk_object *iface, void *object, XTaskQueueHandle queue, void *context,
                                      void *callback, XTaskQueueRegistrationToken *token )
{
    if (token) token->token = 1;
    return S_OK;
}

static BOOLEAN WINAPI store_unregister( gdk_object *iface, void *object, XTaskQueueRegistrationToken token,
                                        BOOLEAN wait )
{
    return TRUE;
}

static BOOLEAN WINAPI store_IsAvailabilityPurchasable( gdk_object *iface, const void *availability )
{
    return FALSE;
}

static HRESULT WINAPI store_AcquireLicenseForDurablesAsync( gdk_object *iface, void *context, const char *id,
                                                            XAsyncBlock *block )
{
    return async_begin( block, NULL, acquire_durables_identity, acquire_durables_identity, acquire_license_provider );
}

static HRESULT WINAPI store_AcquireLicenseForDurablesResult( gdk_object *iface, XAsyncBlock *block, void **license )
{
    if (!license) return E_POINTER;
    *license = NULL;
    return async_get_result( block, acquire_durables_identity, sizeof(*license), license, NULL );
}

/* IXStoreImpl4, with Impl2's to Impl4's methods appended */
static const void *const store_vtbl[] =
{
    store_QueryInterface,
    store_AddRef,
    store_Release,
    store_CreateContext,
    store_nothing,                              /* CloseContextHandle */
    store_QueryAssociatedProductsAsync,
    store_query_result,                         /* QueryAssociatedProductsResult */
    store_QueryProductsAsync,
    store_query_result,                         /* QueryProductsResult */
    store_QueryEntitledProductsAsync,
    store_query_result,                         /* QueryEntitledProductsResult */
    store_QueryProductForCurrentGameAsync,
    store_query_result,                         /* QueryProductForCurrentGameResult */
    store_QueryProductForPackageAsync,
    store_query_result,                         /* QueryProductForPackageResult */
    store_EnumerateProductsQuery,
    store_ProductsQueryHasMorePages,
    store_ProductsQueryNextPageAsync,
    store_query_result,                         /* ProductsQueryNextPageResult */
    store_nothing,                              /* CloseProductsQueryHandle */
    store_AcquireLicenseForPackageAsync,
    store_AcquireLicenseForPackageResult,
    store_IsLicenseValid,
    store_nothing,                              /* CloseLicenseHandle */
    store_CanAcquireLicenseForStoreIdAsync,
    store_CanAcquireLicenseForStoreIdResult,
    store_CanAcquireLicenseForPackageAsync,
    store_CanAcquireLicenseForPackageResult,
    store_QueryGameLicenseAsync,
    store_QueryGameLicenseResult,
    store_QueryAddOnLicensesAsync,
    store_QueryAddOnLicensesResultCount,
    store_indexed_result,                       /* QueryAddOnLicensesResult */
    store_QueryConsumableBalanceRemainingAsync,
    store_consumable_result,                    /* QueryConsumableBalanceRemainingResult */
    store_ReportConsumableFulfillmentAsync,
    store_consumable_result,                    /* ReportConsumableFulfillmentResult */
    store_GetUserCollectionsIdAsync,
    store_notimpl,                              /* GetUserCollectionsIdResultSize */
    store_notimpl,                              /* GetUserCollectionsIdResult */
    store_GetUserPurchaseIdAsync,
    store_notimpl,                              /* GetUserPurchaseIdResultSize */
    store_notimpl,                              /* GetUserPurchaseIdResult */
    store_QueryLicenseTokenAsync,
    store_notimpl,                              /* QueryLicenseTokenResultSize */
    store_notimpl,                              /* QueryLicenseTokenResult */
    store_notimpl,                              /* padding */
    store_notimpl,                              /* padding */
    store_notimpl,                              /* padding */
    store_ShowPurchaseUIAsync,
    store_status_result,                        /* ShowPurchaseUIResult */
    store_ShowRateAndReviewUIAsync,
    store_ShowRateAndReviewUIResult,
    store_ShowRedeemTokenUIAsync,
    store_status_result,                        /* ShowRedeemTokenUIResult */
    store_QueryGameAndDlcPackageUpdatesAsync,
    store_count_result,                         /* QueryGameAndDlcPackageUpdatesResultCount */
    store_indexed_result,                       /* QueryGameAndDlcPackageUpdatesResult */
    store_DownloadPackageUpdatesAsync,
    store_status_result,                        /* DownloadPackageUpdatesResult */
    store_DownloadAndInstallPackageUpdatesAsync,
    store_status_result,                        /* DownloadAndInstallPackageUpdatesResult */
    store_DownloadAndInstallPackagesAsync,
    store_count_result,                         /* DownloadAndInstallPackagesResultCount */
    store_indexed_result,                       /* DownloadAndInstallPackagesResult */
    store_QueryPackageIdentifier,
    store_register,                             /* RegisterGameLicenseChanged */
    store_unregister,                           /* UnregisterGameLicenseChanged */
    store_register,                             /* RegisterPackageLicenseLost */
    store_unregister,                           /* UnregisterPackageLicenseLost */
    /* Impl2 */
    store_IsAvailabilityPurchasable,
    /* Impl3 */
    store_AcquireLicenseForDurablesAsync,
    store_AcquireLicenseForDurablesResult,
    /* Impl4 */
    store_ShowAssociatedProductsUIAsync,
    store_status_result,                        /* ShowAssociatedProductsUIResult */
    store_ShowProductPageUIAsync,
    store_status_result,                        /* ShowProductPageUIResult */
};

static gdk_object store_service = { store_vtbl };

static const GUID IID_IXStoreImpl = {0x0dd112ac,0x7c24,0x448c,{0xb9,0x2b,0x39,0x60,0xfb,0x5b,0xd3,0x0c}};
static const GUID IID_IXStoreImpl2 = {0x60b09f4e,0x1b85,0x45b1,{0x82,0x6c,0x16,0x91,0x18,0xe2,0x30,0xe1}};
static const GUID IID_IXStoreImpl3 = {0x2d42fea5,0xe71d,0x4b76,{0x97,0xcd,0xc5,0x0a,0xfb,0xb3,0xae,0x5d}};
static const GUID IID_IXStoreImpl4 = {0xde3dbdd4,0x0b37,0x4bdb,{0xa1,0x0e,0xac,0xf3,0xa3,0x54,0xd0,0x6a}};
static const GUID IID_IXStoreImpl5 = {0x5c48dedf,0x0b67,0x4492,{0xa4,0xb5,0x68,0x29,0xb8,0xe7,0x96,0xe1}};
static const GUID IID_IXStoreImpl6 = {0xb09d803c,0x2414,0x4a05,{0x82,0xc6,0x66,0xdf,0xdc,0x9e,0x9a,0x44}};

static HRESULT WINAPI store_QueryInterface( gdk_object *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualGUID( iid, &IID_IUnknown ) && !IsEqualGUID( iid, &IID_IXStoreImpl ) &&
        !IsEqualGUID( iid, &IID_IXStoreImpl2 ) && !IsEqualGUID( iid, &IID_IXStoreImpl3 ) &&
        !IsEqualGUID( iid, &IID_IXStoreImpl4 ) && !IsEqualGUID( iid, &IID_IXStoreImpl5 ) &&
        !IsEqualGUID( iid, &IID_IXStoreImpl6 ))
        return E_NOINTERFACE;
    *out = iface;
    return S_OK;
}

HRESULT query_xstore( REFIID iid, void **out )
{
    return store_QueryInterface( &store_service, iid, out );
}

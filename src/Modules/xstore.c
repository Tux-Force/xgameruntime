/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XStore
 *
 * Copyright 2026 Olivia Ryan
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

#include <ctype.h>

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_store
{
    IXStoreImpl6 IXStoreImpl6_iface;
    LONG ref;
};

static inline struct x_store *impl_from_IXStoreImpl6( IXStoreImpl6 *iface )
{
    return CONTAINING_RECORD( iface, struct x_store, IXStoreImpl6_iface );
}

static HRESULT WINAPI x_store_QueryInterface( IXStoreImpl6 *iface, REFIID iid, void **out )
{
    struct x_store *impl = impl_from_IXStoreImpl6( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown     ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl  ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl2 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl3 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl4 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl5 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl6 ))
    {
        IXStoreImpl6_AddRef( *out = &impl->IXStoreImpl6_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_store_AddRef( IXStoreImpl6 *iface )
{
    struct x_store *impl = impl_from_IXStoreImpl6( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_store_Release( IXStoreImpl6 *iface )
{
    struct x_store *impl = impl_from_IXStoreImpl6( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

struct XStoreContext
{
    XUserHandle user;
};

static HRESULT WINAPI x_store_XStoreCreateContext( IXStoreImpl6 *iface, const XUserHandle user, XStoreContextHandle *storeContextHandle )
{
    struct XStoreContext *context;

    TRACE( "iface %p, user %p, storeContextHandle %p.\n", iface, user, storeContextHandle );

    if (!storeContextHandle) return E_INVALIDARG;
    if (!(context = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*context) ))) return E_OUTOFMEMORY;
    context->user = user;

    *storeContextHandle = context;
    return S_OK;
}

static void WINAPI x_store_XStoreCloseContextHandle( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle )
{
    TRACE( "iface %p, storeContextHandle %p.\n", iface, storeContextHandle );
    HeapFree( GetProcessHeap(), 0, storeContextHandle );
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, productKinds %#x, maxItemsToRetrievePerPage %u, async %p stub!\n", iface, storeContextHandle, productKinds, maxItemsToRetrievePerPage, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, const char **storeIds, SIZE_T storeIdsCount, const char **actionFilters, SIZE_T actionFiltersCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, productKinds %#x, storeIds %p, storeIdsCount %Iu, actionFilters %p, actionFiltersCount %Iu, async %p stub!\n", iface, storeContextHandle, productKinds, storeIds, storeIdsCount, actionFilters, actionFiltersCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

/*
 * Product queries. Querying the user's collection is not implemented yet, so entitled product
 * queries succeed with an empty result: the game sees no add-ons instead of an error.
 */
#define PRODUCT_QUERY_MAGIC 0x51505358 /* XSPQ */

struct product_query
{
    DWORD magic;
};

static struct product_query *product_query_from_handle( XStoreProductQueryHandle handle )
{
    struct product_query *query = handle;
    return query && query->magic == PRODUCT_QUERY_MAGIC ? query : NULL;
}

static HRESULT WINAPI empty_product_query_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct product_query *query;

    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );

        case XAsyncOp_DoWork:
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, sizeof(XStoreProductQueryHandle) );
            return S_OK;

        case XAsyncOp_GetResult:
            if (data->bufferSize < sizeof(XStoreProductQueryHandle)) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            if (!(query = calloc( 1, sizeof(*query) ))) return E_OUTOFMEMORY;
            query->magic = PRODUCT_QUERY_MAGIC;
            *(XStoreProductQueryHandle *)data->buffer = query;
            return S_OK;

        default:
            return S_OK;
    }
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, productKinds %#x, maxItemsToRetrievePerPage %u, async %p.\n", iface, storeContextHandle, productKinds, maxItemsToRetrievePerPage, async );

    if (!storeContextHandle || !async) return E_INVALIDARG;
    FIXME( "returning an empty list of entitled products.\n" );
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL, x_store_XStoreQueryEntitledProductsAsync, "XStoreQueryEntitledProductsAsync", empty_product_query_provider );
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );

    if (!productQueryHandle) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreQueryEntitledProductsAsync, sizeof(*productQueryHandle), productQueryHandle, NULL );
}

static HRESULT WINAPI x_store_XStoreQueryProductForCurrentGameAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, async %p stub!\n", iface, storeContextHandle, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductForCurrentGameResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductForPackageAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, const char *packageIdentifier, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, productKinds %#x, packageIdentifier %s, async %p stub!\n", iface, storeContextHandle, productKinds, debugstr_a( packageIdentifier ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreEnumerateProductsQuery( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, void *context, XStoreProductQueryCallback *callback )
{
    TRACE( "iface %p, productQueryHandle %p, context %p, callback %p.\n", iface, productQueryHandle, context, callback );

    if (!product_query_from_handle( productQueryHandle ) || !callback) return E_INVALIDARG;
    /* Queries are always empty for now. */
    return S_OK;
}

static BOOLEAN WINAPI x_store_XStoreProductsQueryHasMorePages( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle )
{
    TRACE( "iface %p, productQueryHandle %p.\n", iface, productQueryHandle );
    return FALSE;
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageAsync( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, XAsyncBlock *async )
{
    FIXME( "iface %p, productQueryHandle %p, async %p stub!\n", iface, productQueryHandle, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static void WINAPI x_store_XStoreCloseProductsQueryHandle( IXStoreImpl6 *iface, XStoreProductQueryHandle productQueryHandle )
{
    struct product_query *query = product_query_from_handle( productQueryHandle );

    TRACE( "iface %p, productQueryHandle %p.\n", iface, productQueryHandle );

    if (!query) return;
    query->magic = 0;
    free( query );
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForPackageAsync( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, const char *packageIdentifier, XAsyncBlock *async )
{
    FIXME( "iface %p, productQueryHandle %p, packageIdentifier %s, async %p stub!\n", iface, productQueryHandle, debugstr_a( packageIdentifier ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreLicenseHandle *storeLicenseHandle )
{
    FIXME( "iface %p, async %p, storeLicenseHandle %p stub!\n", iface, async, storeLicenseHandle );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_store_XStoreIsLicenseValid( IXStoreImpl6 *iface, const XStoreLicenseHandle storeLicenseHandle )
{
    FIXME( "iface %p, storeLicenseHandle %p stub!\n", iface, storeLicenseHandle );
    return FALSE;
}

static void WINAPI x_store_XStoreCloseLicenseHandle( IXStoreImpl6 *iface, XStoreLicenseHandle storeLicenseHandle )
{
    FIXME( "iface %p, storeLicenseHandle %p stub!\n", iface, storeLicenseHandle );
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForStoreIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeProductId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeProductId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForStoreIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreCanAcquireLicenseResult *storeCanAcquireLicense )
{
    FIXME( "iface %p, async %p, storeCanAcquireLicense %p stub!\n", iface, async, storeCanAcquireLicense );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForPackageAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *packageIdentifier, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, packageIdentifier %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( packageIdentifier ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreCanAcquireLicenseResult *storeCanAcquireLicense )
{
    FIXME( "iface %p, async %p, storeCanAcquireLicense %p stub!\n", iface, async, storeCanAcquireLicense );
    return E_NOTIMPL;
}

/*
 * Xodus only starts a title after it obtained the user's license from the Microsoft licensing
 * service, and tells the game process about it with XODUS_LICENSE_VERIFIED.
 */
static BOOL is_license_verified(void)
{
    char value[8];
    DWORD len = GetEnvironmentVariableA( "XODUS_LICENSE_VERIFIED", value, sizeof(value) );
    return len && len < sizeof(value) && value[0] == '1';
}

/*
 * A SKU store id is "<product store id>/<sku id>", e.g. "9P38D19T7LRV/0010". Games compare the whole string
 * (Microsoft Flight Simulator 2024 treats a bare product id as a failed license check). The launcher can pass
 * the owned SKU in XODUS_LICENSE_SKU; without it the standard full game SKU 0010 is reported.
 */
static void get_sku_store_id( char *out, SIZE_T size )
{
    char product[13], sku[8];
    DWORD len = GetEnvironmentVariableA( "XODUS_LICENSE_SKU", sku, sizeof(sku) );
    DWORD i;

    out[0] = 0;
    if (!gdkc_config_get_store_id( product, sizeof(product) )) return;
    for (i = 0; i < len && len == 4; i++)
        if (!((sku[i] >= '0' && sku[i] <= '9') || (sku[i] >= 'A' && sku[i] <= 'Z'))) break;
    if (len != 4 || i != len) strcpy( sku, "0010" );
    snprintf( out, size, "%s/%s", product, sku );
}

static HRESULT WINAPI game_license_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    XStoreGameLicense *license;

    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );

        case XAsyncOp_DoWork:
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, sizeof(*license) );
            return S_OK;

        case XAsyncOp_GetResult:
            if (data->bufferSize < sizeof(*license)) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            license = data->buffer;
            memset( license, 0, sizeof(*license) );
            get_sku_store_id( license->skuStoreId, sizeof(license->skuStoreId) );
            license->isActive = is_license_verified();
            return S_OK;

        default:
            return S_OK;
    }
}

static HRESULT WINAPI x_store_XStoreQueryGameLicenseAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, async %p.\n", iface, storeContextHandle, async );

    if (!storeContextHandle || !async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL, x_store_XStoreQueryGameLicenseAsync, "XStoreQueryGameLicenseAsync", game_license_provider );
}

static HRESULT WINAPI x_store_XStoreQueryGameLicenseResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreGameLicense *license )
{
    TRACE( "iface %p, async %p, license %p.\n", iface, async, license );

    if (!license) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreQueryGameLicenseAsync, sizeof(*license), license, NULL );
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, async %p stub!\n", iface, storeContextHandle, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    FIXME( "iface %p, async %p, count %p stub!\n", iface, async, count );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStoreAddonLicense *addOnLicenses )
{
    FIXME( "iface %p, async %p, count %u, addOnLicenses %p stub!\n", iface, async, count, addOnLicenses );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryConsumableBalanceRemainingAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeProductId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeProductId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryConsumableBalanceRemainingResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreConsumableResult *consumableResult )
{
    FIXME( "iface %p, async %p, consumableResult %p stub!\n", iface, async, consumableResult );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreReportConsumableFulfillmentAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, UINT32 quantity, GUID trackingId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeProductId %s, quantity %u, trackingId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeProductId ), quantity, debugstr_guid( &trackingId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreReportConsumableFulfillmentResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreConsumableResult *consumableResult )
{
    FIXME( "iface %p, async %p, consumableResult %p stub!\n", iface, async, consumableResult );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *serviceTicket, const char *publisherUserId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, serviceTicket %s, userId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( serviceTicket ), debugstr_a( publisherUserId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    FIXME( "iface %p, async %p, size %p stub!\n", iface, async, size );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    FIXME( "iface %p, async %p, size %Iu, result %p stub!\n", iface, async, size, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *serviceTicket, const char *publisherUserId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, serviceTicket %s, publisherUserId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( serviceTicket ), debugstr_a( publisherUserId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    FIXME( "iface %p, async %p, size %p stub!\n", iface, async, size );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    FIXME( "iface %p, async %p, size %Iu, result %p stub!\n", iface, async, size, result );
    return E_NOTIMPL;
}

/*
 * License tokens are issued by the Microsoft Store licensing service for the signed in user.
 * The request is forwarded to Xodus through its bridge, which holds the account tokens.
 */
struct license_token_request
{
    char *body;
    char *token;
};

static HRESULT WINAPI license_token_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct license_token_request *request = data->context;
    HRESULT hr;

    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );

        case XAsyncOp_DoWork:
            hr = gdkc_bridge_post( L"/store/license-token", request->body, &request->token );
            if (FAILED(hr)) WARN( "license token request failed, hr %#lx.\n", hr );
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, hr,
                                            SUCCEEDED(hr) ? strlen( request->token ) + 1 : 0 );
            return S_OK;

        case XAsyncOp_GetResult:
            if (data->bufferSize < strlen( request->token ) + 1) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            strcpy( data->buffer, request->token );
            return S_OK;

        case XAsyncOp_Cleanup:
            free( request->body );
            free( request->token );
            free( request );
            return S_OK;

        default:
            return S_OK;
    }
}

/* Builds the bridge request: parent product id, hex encoded custom string, then the product ids. */
static char *build_license_token_body( const char **ids, SIZE_T count, const char *custom )
{
    char parent[64] = "", *body, *p;
    SIZE_T len, i;

    if (!custom) custom = "";
    gdkc_config_get_store_id( parent, sizeof(parent) );

    len = strlen( parent ) + 1 + strlen( custom ) * 2 + 1 + 1;
    for (i = 0; i < count; i++) len += strlen( ids[i] ) + 1;
    if (!(body = malloc( len ))) return NULL;

    p = body + sprintf( body, "%s\n", parent );
    for (i = 0; custom[i]; i++) p += sprintf( p, "%02x", (unsigned char)custom[i] );
    *p++ = '\n';
    for (i = 0; i < count; i++)
    {
        /* Product ids never contain line breaks; drop any that would break the format. */
        if (strpbrk( ids[i], "\r\n" )) continue;
        p += sprintf( p, "%s\n", ids[i] );
    }
    *p = 0;
    return body;
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **productIds, SIZE_T productIdsCount, const char *customDeveloperString, XAsyncBlock *async )
{
    struct license_token_request *request;
    HRESULT hr;
    SIZE_T i;

    TRACE( "iface %p, storeContextHandle %p, productIds %p, productIdsCount %Iu, custom %s, async %p.\n", iface, storeContextHandle, productIds, productIdsCount, debugstr_a( customDeveloperString ), async );

    if (!storeContextHandle || !async || (productIdsCount && !productIds)) return E_INVALIDARG;
    for (i = 0; i < productIdsCount; i++) if (!productIds[i]) return E_INVALIDARG;
    if (!gdkc_bridge_available()) return E_NOTIMPL;

    if (!(request = calloc( 1, sizeof(*request) ))) return E_OUTOFMEMORY;
    if (!(request->body = build_license_token_body( productIds, productIdsCount, customDeveloperString )))
    {
        free( request );
        return E_OUTOFMEMORY;
    }

    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, request, x_store_XStoreQueryLicenseTokenAsync, "XStoreQueryLicenseTokenAsync", license_token_provider )))
    {
        free( request->body );
        free( request );
    }
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    TRACE( "iface %p, async %p, size %p.\n", iface, async, size );
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, size );
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    TRACE( "iface %p, async %p, size %Iu, result %p.\n", iface, async, size, result );

    if (!result) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreQueryLicenseTokenAsync, size, result, NULL );
}

static HRESULT WINAPI __PADDING__( IXStoreImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_2__( IXStoreImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_3__( IXStoreImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

/* Store dialogs have no Linux counterpart: open the product page in the host browser instead and
 * report the dialog as closed. Games treat a failing call here as fatal (Planet Coaster 2). */
static void open_store_page( const char *storeId )
{
    char cmd[160];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    const char *c;

    if (!storeId || !*storeId || strlen( storeId ) > 32) return;
    for (c = storeId; *c; c++) if (!isalnum( (unsigned char)*c )) return;

    sprintf( cmd, "winebrowser.exe https://www.microsoft.com/store/productId/%s", storeId );
    if (!CreateProcessA( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
    {
        WARN( "could not start winebrowser, error %lu.\n", GetLastError() );
        return;
    }
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
}

static HRESULT WINAPI store_ui_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    char *storeId = data->context;

    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );
        case XAsyncOp_DoWork:
            open_store_page( storeId );
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, 0 );
            return S_OK;
        case XAsyncOp_Cleanup:
            free( storeId );
            return S_OK;
        default:
            return S_OK;
    }
}

static HRESULT begin_store_ui( XAsyncBlock *async, const char *storeId, const void *identity, const char *name )
{
    char *copy = NULL;
    HRESULT hr;

    if (!async) return E_INVALIDARG;
    if (storeId && !(copy = strdup( storeId ))) return E_OUTOFMEMORY;
    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, copy, identity, name, store_ui_provider ))) free( copy );
    return hr;
}

static HRESULT WINAPI x_store_XStoreShowPurchaseUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, const char *name, const char *extendedJsonData, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, storeId %s, name %s, extendedJsonData %s, async %p.\n", iface, storeContextHandle, debugstr_a( storeId ), debugstr_a( name ), debugstr_a( extendedJsonData ), async );
    return begin_store_ui( async, storeId, x_store_XStoreShowPurchaseUIAsync, "XStoreShowPurchaseUIAsync" );
}

static HRESULT WINAPI x_store_XStoreShowPurchaseUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreShowPurchaseUIAsync, 0, NULL, NULL );
}

static HRESULT WINAPI x_store_XStoreShowRateAndReviewUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, async %p stub!\n", iface, storeContextHandle, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowRateAndReviewUIResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreRateAndReviewResult *result )
{
    FIXME( "iface %p, async %p, result %p stub!\n", iface, async, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowRedeemTokenUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *token, const char **allowedStoreIds, SIZE_T allowedStoreIdsCount, BOOLEAN disallowCsvRedemption, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, token %s, allowedStoreIds %p, allowedStoreIdsCount %Iu, disallowCsvRedemption %d, async %p stub!\n", iface, storeContextHandle, debugstr_a( token ), allowedStoreIds, allowedStoreIdsCount, disallowCsvRedemption, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowRedeemTokenUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

/* Updates are managed by Xodus or the Windows Xbox app, never from inside the game. */
static HRESULT WINAPI no_updates_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );
        case XAsyncOp_DoWork:
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, 0 );
            return S_OK;
        default:
            return S_OK;
    }
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, async %p.\n", iface, storeContextHandle, async );

    if (!storeContextHandle || !async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL, x_store_XStoreQueryGameAndDlcPackageUpdatesAsync, "XStoreQueryGameAndDlcPackageUpdatesAsync", no_updates_provider );
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, count %p.\n", iface, async, count );

    if (!count) return E_INVALIDARG;
    if (FAILED(hr = IXThreadingImpl_XAsyncGetStatus( x_threading_impl, async, FALSE ))) return hr;
    *count = 0;
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStorePackageUpdate *packageUpdates )
{
    TRACE( "iface %p, async %p, count %u, packageUpdates %p.\n", iface, async, count, packageUpdates );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreQueryGameAndDlcPackageUpdatesAsync, 0, NULL, NULL );
}

static HRESULT WINAPI x_store_XStoreDownloadPackageUpdatesAsync( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, const char **packageIdentifiers, SIZE_T packageIdentifiersCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p stub!\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackageUpdatesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **packageIdentifiers, SIZE_T packageIdentifiersCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p stub!\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **storeIds, SIZE_T storeIdsCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeIds %p, storeIdsCount %Iu, async %p stub!\n", iface, storeContextHandle, storeIds, storeIdsCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    FIXME( "iface %p, async %p, count %p stub!\n", iface, async, count );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, char **packageIdentifiers )
{
    FIXME( "iface %p, async %p, count %u, packageIdentifiers %p stub!\n", iface, async, count, packageIdentifiers );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryPackageIdentifier( IXStoreImpl6 *iface, const char *storeId, SIZE_T size, char *packageIdentifier )
{
    FIXME( "iface %p, storeId %s, size %Iu, packageIdentifier %p stub!\n", iface, debugstr_a( storeId ), size, packageIdentifier );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreRegisterGameLicenseChanged( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, XTaskQueueHandle queue, void *context, XStoreGameLicenseChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    static LONG64 next_token;

    TRACE( "iface %p, storeContextHandle %p, queue %p, context %p, callback %p, token %p.\n", iface, storeContextHandle, queue, context, callback, token );

    if (!storeContextHandle || !callback || !token) return E_INVALIDARG;
    /* The license is checked once when Xodus starts the game, so it never changes while running. */
    token->token = InterlockedIncrement64( &next_token );
    return S_OK;
}

static BOOLEAN WINAPI x_store_XStoreUnregisterGameLicenseChanged( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, storeContextHandle %p, token %I64u, wait %d.\n", iface, storeContextHandle, token.token, wait );
    return TRUE;
}

static HRESULT WINAPI x_store_XStoreRegisterPackageLicenseLost( IXStoreImpl6 *iface, XStoreLicenseHandle storeLicenseHandle, XTaskQueueHandle queue, void *context, XStorePackageLicenseLostCallback *callback, XTaskQueueRegistrationToken *token)
{
    /* Generated by tools/gdkgen.py (never-fires), see tools/gdkgen/events.toml. */
    static LONG64 next_token;

    TRACE( "iface %p, storeLicenseHandle %p, queue %p, context %p, callback %p, token %p.\n", iface, storeLicenseHandle, queue, context, callback, token );

    if (!storeLicenseHandle || !callback || !token) return E_INVALIDARG;
    /* xTux checks the license before the game starts and does not revoke it while the game runs,
     * so a package license is never lost during a run and the callback is never invoked. */
    token->token = InterlockedIncrement64( &next_token );
    return S_OK;
}

static BOOLEAN WINAPI x_store_XStoreUnregisterPackageLicenseLost( IXStoreImpl6 *iface, XStoreLicenseHandle licenseHandle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    /* Generated by tools/gdkgen.py (never-fires), see tools/gdkgen/events.toml. */
    TRACE( "iface %p, licenseHandle %p, token %I64u, wait %d.\n", iface, licenseHandle, token.token, wait );
    return TRUE;
}

static BOOLEAN WINAPI x_store_XStoreIsAvailabilityPurchasable( IXStoreImpl6 *iface, const XStoreAvailability availability )
{
    FIXME( "iface %p, availability %p stub!\n", iface, &availability );
    return FALSE;
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForDurablesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForDurablesResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreLicenseHandle *storeLicenseHandle )
{
    FIXME( "iface %p, async %p, storeLicenseHandle %p stub!\n", iface, async, storeLicenseHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowAssociatedProductsUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XStoreProductKind productKinds, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, storeId %s, productKinds %#x, async %p.\n", iface, storeContextHandle, debugstr_a( storeId ), productKinds, async );
    return begin_store_ui( async, storeId, x_store_XStoreShowAssociatedProductsUIAsync, "XStoreShowAssociatedProductsUIAsync" );
}

static HRESULT WINAPI x_store_XStoreShowAssociatedProductsUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreShowAssociatedProductsUIAsync, 0, NULL, NULL );
}

static HRESULT WINAPI x_store_XStoreShowProductPageUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, storeId %s, async %p.\n", iface, storeContextHandle, debugstr_a( storeId ), async );
    return begin_store_ui( async, storeId, x_store_XStoreShowProductPageUIAsync, "XStoreShowProductPageUIAsync" );
}

static HRESULT WINAPI x_store_XStoreShowProductPageUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreShowProductPageUIAsync, 0, NULL, NULL );
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsForStoreIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, productKinds %#x, maxItemsToRetrievePerPage %u, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), productKinds, maxItemsToRetrievePerPage, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsForStoreIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesAsync( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, const char **packageIdentifiers, SIZE_T packageIdentifiersCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p stub!\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    FIXME( "iface %p, async %p, count %p stub!\n", iface, async, count );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStorePackageUpdate *packageUpdates )
{
    FIXME( "iface %p, async %p, count %u, packageUpdates %p stub!\n", iface, async, count, packageUpdates );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowGiftingUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, const char *name, const char *extendedJsonData, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, name %s, extendedJsonData %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), debugstr_a( name ), debugstr_a( extendedJsonData ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowGiftingUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static const struct IXStoreImpl6Vtbl x_store_vtbl =
{
    x_store_QueryInterface,
    x_store_AddRef,
    x_store_Release,
    /* IXStoreImpl methods */
    x_store_XStoreCreateContext,
    x_store_XStoreCloseContextHandle,
    x_store_XStoreQueryAssociatedProductsAsync,
    x_store_XStoreQueryAssociatedProductsResult,
    x_store_XStoreQueryProductsAsync,
    x_store_XStoreQueryProductsResult,
    x_store_XStoreQueryEntitledProductsAsync,
    x_store_XStoreQueryEntitledProductsResult,
    x_store_XStoreQueryProductForCurrentGameAsync,
    x_store_XStoreQueryProductForCurrentGameResult,
    x_store_XStoreQueryProductForPackageAsync,
    x_store_XStoreQueryProductForPackageResult,
    x_store_XStoreEnumerateProductsQuery,
    x_store_XStoreProductsQueryHasMorePages,
    x_store_XStoreProductsQueryNextPageAsync,
    x_store_XStoreProductsQueryNextPageResult,
    x_store_XStoreCloseProductsQueryHandle,
    x_store_XStoreAcquireLicenseForPackageAsync,
    x_store_XStoreAcquireLicenseForPackageResult,
    x_store_XStoreIsLicenseValid,
    x_store_XStoreCloseLicenseHandle,
    x_store_XStoreCanAcquireLicenseForStoreIdAsync,
    x_store_XStoreCanAcquireLicenseForStoreIdResult,
    x_store_XStoreCanAcquireLicenseForPackageAsync,
    x_store_XStoreCanAcquireLicenseForPackageResult,
    x_store_XStoreQueryGameLicenseAsync,
    x_store_XStoreQueryGameLicenseResult,
    x_store_XStoreQueryAddOnLicensesAsync,
    x_store_XStoreQueryAddOnLicensesResultCount,
    x_store_XStoreQueryAddOnLicensesResult,
    x_store_XStoreQueryConsumableBalanceRemainingAsync,
    x_store_XStoreQueryConsumableBalanceRemainingResult,
    x_store_XStoreReportConsumableFulfillmentAsync,
    x_store_XStoreReportConsumableFulfillmentResult,
    x_store_XStoreGetUserCollectionsIdAsync,
    x_store_XStoreGetUserCollectionsIdResultSize,
    x_store_XStoreGetUserCollectionsIdResult,
    x_store_XStoreGetUserPurchaseIdAsync,
    x_store_XStoreGetUserPurchaseIdResultSize,
    x_store_XStoreGetUserPurchaseIdResult,
    x_store_XStoreQueryLicenseTokenAsync,
    x_store_XStoreQueryLicenseTokenResultSize,
    x_store_XStoreQueryLicenseTokenResult,
    __PADDING__,
    __PADDING_2__,
    __PADDING_3__,
    x_store_XStoreShowPurchaseUIAsync,
    x_store_XStoreShowPurchaseUIResult,
    x_store_XStoreShowRateAndReviewUIAsync,
    x_store_XStoreShowRateAndReviewUIResult,
    x_store_XStoreShowRedeemTokenUIAsync,
    x_store_XStoreShowRedeemTokenUIResult,
    x_store_XStoreQueryGameAndDlcPackageUpdatesAsync,
    x_store_XStoreQueryGameAndDlcPackageUpdatesResultCount,
    x_store_XStoreQueryGameAndDlcPackageUpdatesResult,
    x_store_XStoreDownloadPackageUpdatesAsync,
    x_store_XStoreDownloadPackageUpdatesResult,
    x_store_XStoreDownloadAndInstallPackageUpdatesAsync,
    x_store_XStoreDownloadAndInstallPackageUpdatesResult,
    x_store_XStoreDownloadAndInstallPackagesAsync,
    x_store_XStoreDownloadAndInstallPackagesResultCount,
    x_store_XStoreDownloadAndInstallPackagesResult,
    x_store_XStoreQueryPackageIdentifier,
    x_store_XStoreRegisterGameLicenseChanged,
    x_store_XStoreUnregisterGameLicenseChanged,
    x_store_XStoreRegisterPackageLicenseLost,
    x_store_XStoreUnregisterPackageLicenseLost,
    /* IXStoreImpl2 methods */
    x_store_XStoreIsAvailabilityPurchasable,
    /* IXStoreImpl3 methods */
    x_store_XStoreAcquireLicenseForDurablesAsync,
    x_store_XStoreAcquireLicenseForDurablesResult,
    /* IXStoreImpl4 methods */
    x_store_XStoreShowAssociatedProductsUIAsync,
    x_store_XStoreShowAssociatedProductsUIResult,
    x_store_XStoreShowProductPageUIAsync,
    x_store_XStoreShowProductPageUIResult,
    /* IXStoreImpl5 methods */
    x_store_XStoreQueryAssociatedProductsForStoreIdAsync,
    x_store_XStoreQueryAssociatedProductsForStoreIdResult,
    x_store_XStoreQueryPackageUpdatesAsync,
    x_store_XStoreQueryPackageUpdatesResultCount,
    x_store_XStoreQueryPackageUpdatesResult,
    /* IXStoreImpl6 methods */
    x_store_XStoreShowGiftingUIAsync,
    x_store_XStoreShowGiftingUIResult,
};

static struct x_store x_store =
{
    {&x_store_vtbl},
    0,
};

IXStoreImpl *x_store_impl = (IXStoreImpl *)&x_store.IXStoreImpl6_iface;

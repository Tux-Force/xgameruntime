/* The Store and in-game events through the bridge. The runner supplies a fake bridge that knows three
 * add-ons of the game, of which the user owns the first and has two of the last, a consumable. */
#include <initguid.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <xasync.h>
#include <xasyncprovider.h>
#include <xtaskqueue.h>
#include <xgameerr.h>
#include <xgameevent.h>
#include <xpackage.h>
#include <xstore.h>

typedef HRESULT (WINAPI *query_api_fn)( const GUID *, const GUID *, void ** );

static int failures;
#define CHECK(cond) do { if (cond) printf( "ok   %s\n", #cond ); else { failures++; printf( "FAIL line %d: %s\n", __LINE__, #cond ); } } while (0)
#define CHECK_HR(call, expected) do { HRESULT hr_ = (call); if (hr_ == (HRESULT)(expected)) printf( "ok   %s\n", #call ); \
    else { failures++; printf( "FAIL line %d: %s hr %#lx expected %#lx\n", __LINE__, #call, hr_, (HRESULT)(expected) ); } } while (0)

#define ALL_KINDS (XStoreProductKind_Consumable | XStoreProductKind_Durable | XStoreProductKind_Game)
#define SCID "00000000-0000-0000-0000-00006d0fd16e"

static IXThreadingImpl *threading;
static IXStoreImpl6 *store;
static XAsyncBlock block;
static XTaskQueueHandle queue;

static XAsyncBlock *begin( void )
{
    memset( &block, 0, sizeof(block) );
    block.queue = queue;
    return &block;
}

static HRESULT finish( void )
{
    return IXThreadingImpl_XAsyncGetStatus( threading, &block, TRUE );
}

struct products
{
    int count;
    char ids[8][16];
    BOOLEAN owned[8];
    XStoreProduct first;
    char title[64], currency[8], formatted[16], offer[16];
    BOOLEAN purchasable;
};

static BOOLEAN __stdcall on_product( const XStoreProduct *product, void *context )
{
    struct products *products = context;

    if (!products->count)
    {
        products->first = *product;
        lstrcpynA( products->title, product->title, sizeof(products->title) );
        lstrcpynA( products->currency, product->price.currencyCode, sizeof(products->currency) );
        if (product->skus[0].availabilitiesCount)
        {
            lstrcpynA( products->offer, product->skus[0].availabilities[0].availabilityId, sizeof(products->offer) );
            products->purchasable = IXStoreImpl6_XStoreIsAvailabilityPurchasable( store, product->skus[0].availabilities[0] );
        }
    }
    if (products->count < 8)
    {
        lstrcpynA( products->ids[products->count], product->storeId, 16 );
        products->owned[products->count] = product->isInUserCollection;
    }
    products->count++;
    return TRUE;
}

static void list( XStoreProductQueryHandle query, struct products *products )
{
    memset( products, 0, sizeof(*products) );
    CHECK_HR( IXStoreImpl6_XStoreEnumerateProductsQuery( store, query, products, on_product ), S_OK );
}

int main( void )
{
    HMODULE dll = LoadLibraryA( "xgameruntime.dll" );
    XStoreProductQueryHandle query, next;
    XStoreCanAcquireLicenseResult can_acquire;
    XStoreConsumableResult consumable;
    XStoreAvailability expired = { "X" };
    XStoreContextHandle context;
    XStoreLicenseHandle license;
    XStorePackageUpdate update;
    struct products products;
    query_api_fn query_api;
    IXGameEventImpl *events;
    GUID tracking = { 0x11223344, 0x5566, 0x7788, { 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01 } };
    char key[64], name[200];
    UINT32 count;
    SIZE_T size;
    int i;

    if (!dll) { printf( "FAIL could not load xgameruntime.dll\n" ); return 1; }
    query_api = (query_api_fn)GetProcAddress( dll, "QueryApiImpl" );
    if (FAILED(query_api( &CLSID_XStoreImpl, &IID_IXStoreImpl6, (void **)&store )) ||
        FAILED(query_api( &CLSID_XGameEventImpl, &IID_IXGameEventImpl, (void **)&events )) ||
        FAILED(query_api( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading )))
    {
        printf( "FAIL QueryApiImpl\n" );
        return 1;
    }
    CHECK_HR( IXThreadingImpl_XTaskQueueCreate( threading, XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &queue ), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreCreateContext( store, NULL, &context ), S_OK );

    /* Products of the game, two a page. */
    CHECK_HR( IXStoreImpl6_XStoreQueryAssociatedProductsAsync( store, context, ALL_KINDS, 2, begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreQueryAssociatedProductsResult( store, &block, &query ), S_OK );
    list( query, &products );
    CHECK( products.count == 2 && products.owned[0] && !products.owned[1] );
    CHECK( !strcmp( products.title, "Rally" ) && !strcmp( products.currency, "EUR" ) && !strcmp( products.offer, "9OFFER" ) );
    CHECK( products.purchasable );
    CHECK( products.first.price.basePrice > 19.98f && products.first.price.basePrice < 20.0f );
    CHECK( !strcmp( products.first.price.formattedPrice, "7.99 EUR" ) && products.first.price.isOnSale );
    CHECK( products.first.imagesCount == 1 && products.first.productKind == XStoreProductKind_Durable );
    CHECK( IXStoreImpl6_XStoreProductsQueryHasMorePages( store, query ) );
    CHECK_HR( IXStoreImpl6_XStoreProductsQueryNextPageAsync( store, query, begin() ), S_OK );
    /* The next page does not need the one before. */
    IXStoreImpl6_XStoreCloseProductsQueryHandle( store, query );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreProductsQueryNextPageResult( store, &block, &next ), S_OK );
    list( next, &products );
    /* The installed add-on the Store does not list comes last. */
    CHECK( products.count == 2 && !strcmp( products.ids[0], "9COINS000001" ) && products.owned[0] );
    CHECK( !strcmp( products.ids[1], "9MT1H69FDCW0" ) && !products.owned[1] );
    CHECK( !products.purchasable );
    CHECK( !IXStoreImpl6_XStoreProductsQueryHasMorePages( store, next ) );
    IXStoreImpl6_XStoreCloseProductsQueryHandle( store, next );
    CHECK( !IXStoreImpl6_XStoreIsAvailabilityPurchasable( store, expired ) );

    /* Only what the user owns, only durables. */
    CHECK_HR( IXStoreImpl6_XStoreQueryEntitledProductsAsync( store, context, XStoreProductKind_Durable, 25, begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreQueryEntitledProductsResult( store, &block, &query ), S_OK );
    list( query, &products );
    CHECK( products.count == 1 && !strcmp( products.ids[0], "9PCR7TSM1S96" ) );
    IXStoreImpl6_XStoreCloseProductsQueryHandle( store, query );

    CHECK_HR( IXStoreImpl6_XStoreQueryAssociatedProductsForStoreIdAsync( store, context, "9PCR7TSM1S96", ALL_KINDS, 25, begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreQueryAssociatedProductsForStoreIdResult( store, &block, &query ), S_OK );
    list( query, &products );
    CHECK( products.count == 3 );
    IXStoreImpl6_XStoreCloseProductsQueryHandle( store, query );
    CHECK_HR( IXStoreImpl6_XStoreQueryAssociatedProductsForStoreIdAsync( store, context, "bad\nid", ALL_KINDS, 25, begin() ), E_INVALIDARG );

    /* The Store does not know the game: what the game itself says. */
    CHECK_HR( IXStoreImpl6_XStoreQueryProductForCurrentGameAsync( store, context, begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreQueryProductForCurrentGameResult( store, &block, &query ), S_OK );
    list( query, &products );
    CHECK( products.count == 1 && !strcmp( products.ids[0], "9NNX1VVR3KNQ" ) && products.owned[0] );
    IXStoreImpl6_XStoreCloseProductsQueryHandle( store, query );

    /* Durables that are not installed. */
    CHECK_HR( IXStoreImpl6_XStoreAcquireLicenseForDurablesAsync( store, context, "9PCR7TSM1S96", begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreAcquireLicenseForDurablesResult( store, &block, &license ), S_OK );
    CHECK( IXStoreImpl6_XStoreIsLicenseValid( store, license ) );
    IXStoreImpl6_XStoreCloseLicenseHandle( store, license );
    CHECK_HR( IXStoreImpl6_XStoreAcquireLicenseForDurablesAsync( store, context, "9ZZZZZZZZZZZ", begin() ), S_OK );
    CHECK_HR( finish(), HRESULT_FROM_WIN32( ERROR_NOT_FOUND ) );
    CHECK_HR( IXStoreImpl6_XStoreCanAcquireLicenseForStoreIdAsync( store, context, "9P5H6T4ZVR8R", begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreCanAcquireLicenseForStoreIdResult( store, &block, &can_acquire ), S_OK );
    CHECK( can_acquire.status == XStoreCanLicenseStatus_NotLicensableToUser );

    /* Consumables. */
    CHECK_HR( IXStoreImpl6_XStoreQueryConsumableBalanceRemainingAsync( store, context, "9COINS000001", begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreQueryConsumableBalanceRemainingResult( store, &block, &consumable ), S_OK );
    CHECK( consumable.quantity == 2 );
    CHECK_HR( IXStoreImpl6_XStoreReportConsumableFulfillmentAsync( store, context, "9COINS000001", 1, tracking, begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreReportConsumableFulfillmentResult( store, &block, &consumable ), S_OK );
    CHECK( consumable.quantity == 1 );
    CHECK_HR( IXStoreImpl6_XStoreReportConsumableFulfillmentAsync( store, context, "9COINS000001", 0, tracking, begin() ), E_INVALIDARG );
    CHECK_HR( IXStoreImpl6_XStoreQueryConsumableBalanceRemainingAsync( store, context, "9P5H6T4ZVR8R", begin() ), S_OK );
    CHECK_HR( finish(), E_GAMESTORE_SERVER_ERROR );

    /* Store ID keys. */
    CHECK_HR( IXStoreImpl6_XStoreGetUserCollectionsIdAsync( store, context, "ticket", "player", begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreGetUserCollectionsIdResultSize( store, &block, &size ), S_OK );
    CHECK( size == sizeof("collections-key") );
    CHECK_HR( IXStoreImpl6_XStoreGetUserCollectionsIdResult( store, &block, size, key ), S_OK );
    CHECK( !strcmp( key, "collections-key" ) );
    CHECK_HR( IXStoreImpl6_XStoreGetUserPurchaseIdAsync( store, context, "ticket", NULL, begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreGetUserPurchaseIdResult( store, &block, sizeof(key), key ), S_OK );
    CHECK( !strcmp( key, "purchase-key" ) );
    CHECK_HR( IXStoreImpl6_XStoreGetUserPurchaseIdAsync( store, context, "two\nlines", NULL, begin() ), E_INVALIDARG );

    /* Packages and updates. */
    CHECK_HR( IXStoreImpl6_XStoreQueryPackageUpdatesAsync( store, context, NULL, 0, begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreQueryPackageUpdatesResultCount( store, &block, &count ), S_OK );
    CHECK( count == 0 );
    CHECK_HR( IXStoreImpl6_XStoreQueryPackageUpdatesResult( store, &block, 0, &update ), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreDownloadAndInstallPackageUpdatesAsync( store, context, NULL, 0, begin() ), S_OK );
    CHECK_HR( finish(), S_OK );
    CHECK_HR( IXStoreImpl6_XStoreDownloadAndInstallPackageUpdatesResult( store, &block ), S_OK );
    {
        const char *installed[] = { "9MT1H69FDCW0" }, *missing[] = { "9MT1H69FDCW0", "9P5H6T4ZVR8R" };
        char packages[2][33];

        CHECK_HR( IXStoreImpl6_XStoreDownloadAndInstallPackagesAsync( store, context, installed, 1, begin() ), S_OK );
        CHECK_HR( finish(), S_OK );
        CHECK_HR( IXStoreImpl6_XStoreDownloadAndInstallPackagesResultCount( store, &block, &count ), S_OK );
        CHECK( count == 1 );
        CHECK_HR( IXStoreImpl6_XStoreDownloadAndInstallPackagesResult( store, &block, 1, (char **)packages ), S_OK );
        CHECK( !strcmp( packages[0], "11111111222233334444555555555555" ) );
        CHECK_HR( IXStoreImpl6_XStoreDownloadAndInstallPackagesAsync( store, context, missing, 2, begin() ), S_OK );
        CHECK_HR( finish(), E_GAMEPACKAGE_DOWNLOAD_REQUIRED );
    }

    /* Events, sent in order in the background. */
    for (i = 0; i < 20; i++)
    {
        sprintf( name, "Event%d", i );
        if (FAILED(IXGameEventImpl_XGameEventWrite( events, (XUserHandle)1, SCID, "play", name, "{\r\n\"Map\": \"A\"}", i ? NULL : "{\"Count\":1}" ))) failures++;
    }
    CHECK_HR( IXGameEventImpl_XGameEventWrite( events, NULL, SCID, "play", "Event", NULL, NULL ), E_INVALIDARG );
    CHECK_HR( IXGameEventImpl_XGameEventWrite( events, (XUserHandle)1, NULL, "play", "Event", NULL, NULL ), E_INVALIDARG );
    CHECK_HR( IXGameEventImpl_XGameEventWrite( events, (XUserHandle)1, SCID, "play", "1Event", NULL, NULL ), E_INVALIDARG );
    CHECK_HR( IXGameEventImpl_XGameEventWrite( events, (XUserHandle)1, SCID, "play", "Event\nx", NULL, NULL ), E_INVALIDARG );
    memset( name, 'a', sizeof(name) );
    name[sizeof(name) - 1] = 0;
    CHECK_HR( IXGameEventImpl_XGameEventWrite( events, (XUserHandle)1, SCID, "play", name, NULL, NULL ), E_INVALIDARG );
    Sleep( 3000 );

    IXStoreImpl6_XStoreCloseContextHandle( store, context );
    printf( "%d failures\n", failures );
    return failures ? 1 : 0;
}

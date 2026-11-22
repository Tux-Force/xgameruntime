/* Add-ons (DLC) through XPackage and XStore. The runner supplies a fake bridge with two installed
 * add-ons, of which the user owns the first, and installs a third one while the test waits. */
#include <initguid.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <xasync.h>
#include <xasyncprovider.h>
#include <xtaskqueue.h>
#include <xgameerr.h>
#include <xpackage.h>
#include <xstore.h>

typedef HRESULT (WINAPI *query_api_fn)( const GUID *, const GUID *, void ** );

static int failures;
#define CHECK(cond) do { if (cond) printf( "ok   %s\n", #cond ); else { failures++; printf( "FAIL line %d: %s\n", __LINE__, #cond ); } } while (0)
#define CHECK_HR(call, expected) do { HRESULT hr_ = (call); if (hr_ == (HRESULT)(expected)) printf( "ok   %s\n", #call ); \
    else { failures++; printf( "FAIL line %d: %s hr %#lx expected %#lx\n", __LINE__, #call, hr_, (HRESULT)(expected) ); } } while (0)

#define OWNED_ID    "11111111222233334444555555555555"
#define UNOWNED_ID  "aaaaaaaabbbbccccddddeeeeeeeeeeee"
#define NEW_ID      "99999999888877776666555555555555"
#define GAME_ID     "0123456789abcdef0123456789abcdef"

static IXThreadingImpl *threading;

struct seen
{
    int count;
    char ids[4][40];
    char store_ids[4][16];
    XPackageKind kinds[4];
    UINT32 index[4], total[4];
    XVersion version[4];
};

static BOOLEAN __stdcall on_package( void *context, const XPackageDetails *details )
{
    struct seen *seen = context;
    if (seen->count < 4)
    {
        strcpy( seen->ids[seen->count], details->packageIdentifier );
        strcpy( seen->store_ids[seen->count], details->storeId );
        seen->kinds[seen->count] = details->kind;
        seen->index[seen->count] = details->index;
        seen->total[seen->count] = details->count;
        seen->version[seen->count] = details->version;
    }
    seen->count++;
    return TRUE;
}

struct products
{
    int count;
    char store_ids[4][16];
    BOOLEAN owned[4];
    XStoreProductKind kinds[4];
};

static BOOLEAN __stdcall on_product( const XStoreProduct *product, void *context )
{
    struct products *products = context;
    if (products->count < 4)
    {
        strcpy( products->store_ids[products->count], product->storeId );
        products->owned[products->count] = product->isInUserCollection;
        products->kinds[products->count] = product->productKind;
    }
    products->count++;
    return TRUE;
}

static HANDLE installed_event;
static char installed_id[40];
static void __stdcall on_installed( void *context, const XPackageDetails *details )
{
    (void)context;
    strcpy( installed_id, details->packageIdentifier );
    SetEvent( installed_event );
}

static XAsyncBlock *new_async( XTaskQueueHandle queue )
{
    static XAsyncBlock async;
    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    return &async;
}

static void query_products( IXStoreImpl *store, XStoreProductQueryHandle query, struct products *products )
{
    memset( products, 0, sizeof(*products) );
    CHECK_HR( IXStoreImpl_XStoreEnumerateProductsQuery( store, query, products, on_product ), S_OK );
    IXStoreImpl_XStoreCloseProductsQueryHandle( store, query );
}

int main( void )
{
    HMODULE dll = LoadLibraryA( "xgameruntime.dll" );
    XStoreCanAcquireLicenseResult can_acquire;
    XStoreAddonLicense licenses[4];
    XStoreProductQueryHandle query;
    XTaskQueueRegistrationToken token;
    XStoreLicenseHandle license;
    XPackageMountHandle mount;
    XStoreContextHandle context;
    XTaskQueueHandle queue;
    struct products products;
    IXPackageImpl *packages;
    IXPackageImpl4 *packages4;
    XPackageKind kind;
    IXStoreImpl *store;
    query_api_fn query_api;
    struct seen seen;
    XAsyncBlock *async;
    char path[MAX_PATH], id[64];
    SIZE_T size;
    UINT32 count;

    if (!dll) { printf( "FAIL could not load xgameruntime.dll\n" ); return 1; }
    query_api = (query_api_fn)GetProcAddress( dll, "QueryApiImpl" );
    if (FAILED(query_api( &CLSID_XPackageImpl, &IID_IXPackageImpl, (void **)&packages )) ||
        FAILED(query_api( &CLSID_XPackageImpl, &IID_IXPackageImpl4, (void **)&packages4 )) ||
        FAILED(query_api( &CLSID_XStoreImpl, &IID_IXStoreImpl, (void **)&store )) ||
        FAILED(query_api( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading )))
    {
        printf( "FAIL QueryApiImpl\n" );
        return 1;
    }
    CHECK_HR( IXThreadingImpl_XTaskQueueCreate( threading, XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &queue ), S_OK );
    installed_event = CreateEventW( NULL, FALSE, FALSE, NULL );

    /* XPackage: the game and its add-ons. */
    memset( &seen, 0, sizeof(seen) );
    CHECK_HR( IXPackageImpl_XPackageEnumeratePackages( packages, XPackageKind_Content, XPackageEnumerationScope_ThisOnly, &seen, on_package ), S_OK );
    CHECK( seen.count == 2 );
    CHECK( !strcmp( seen.ids[0], OWNED_ID ) && !strcmp( seen.ids[1], UNOWNED_ID ) );
    CHECK( !strcmp( seen.store_ids[0], "9PCR7TSM1S96" ) && seen.kinds[0] == XPackageKind_Content );
    CHECK( seen.index[1] == 1 && seen.total[1] == 2 );
    CHECK( seen.version[0].major == 1 && seen.version[0].minor == 2 && seen.version[0].build == 3 && seen.version[0].revision == 4 );

    memset( &seen, 0, sizeof(seen) );
    CHECK_HR( IXPackageImpl_XPackageEnumeratePackages( packages, XPackageKind_Game, XPackageEnumerationScope_ThisOnly, &seen, on_package ), S_OK );
    CHECK( seen.count == 1 && !strcmp( seen.ids[0], GAME_ID ) && !strcmp( seen.store_ids[0], "9NNX1VVR3KNQ" ) );
    CHECK( seen.version[0].major == 2 && seen.version[0].revision == 7 );

    CHECK_HR( IXPackageImpl4_XPackageGetKind( packages4, UNOWNED_ID, &kind ), S_OK );
    CHECK( kind == XPackageKind_Content );
    CHECK_HR( IXPackageImpl4_XPackageGetKind( packages4, GAME_ID, &kind ), S_OK );
    CHECK( kind == XPackageKind_Game );

    /* Mounting: only the owned add-on. */
    async = new_async( queue );
    CHECK_HR( IXPackageImpl4_XPackageMountWithUiAsync( packages4, OWNED_ID, async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), S_OK );
    CHECK_HR( IXPackageImpl4_XPackageMountWithUiResult( packages4, async, &mount ), S_OK );
    CHECK_HR( IXPackageImpl_XPackageGetMountPathSize( packages, mount, &size ), S_OK );
    CHECK_HR( IXPackageImpl_XPackageGetMountPath( packages, mount, sizeof(path), path ), S_OK );
    CHECK( size == strlen( path ) + 1 && !strcmp( path, "Z:\\games\\Rally\\Content" ) );
    IXPackageImpl_XPackageCloseMountHandle( packages, mount );

    async = new_async( queue );
    /* The hyphenated upper case form names the same package. */
    CHECK_HR( IXPackageImpl4_XPackageMountWithUiAsync( packages4, "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE", async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), E_ACCESSDENIED );

    async = new_async( queue );
    CHECK_HR( IXPackageImpl4_XPackageMountWithUiAsync( packages4, "ffffffffffffffffffffffffffffffff", async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), HRESULT_FROM_WIN32( ERROR_NOT_FOUND ) );

    /* XStore. */
    CHECK_HR( IXStoreImpl_XStoreCreateContext( store, NULL, &context ), S_OK );

    async = new_async( queue );
    CHECK_HR( IXStoreImpl_XStoreQueryAddOnLicensesAsync( store, context, async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), S_OK );
    CHECK_HR( IXStoreImpl_XStoreQueryAddOnLicensesResultCount( store, async, &count ), S_OK );
    CHECK( count == 1 );
    CHECK_HR( IXStoreImpl_XStoreQueryAddOnLicensesResult( store, async, count, licenses ), S_OK );
    CHECK( !strcmp( licenses[0].skuStoreId, "9PCR7TSM1S96/0010" ) && licenses[0].isActive );

    async = new_async( queue );
    CHECK_HR( IXStoreImpl_XStoreAcquireLicenseForPackageAsync( store, context, OWNED_ID, async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), S_OK );
    CHECK_HR( IXStoreImpl_XStoreAcquireLicenseForPackageResult( store, async, &license ), S_OK );
    CHECK( IXStoreImpl_XStoreIsLicenseValid( store, license ) );
    IXStoreImpl_XStoreCloseLicenseHandle( store, license );

    async = new_async( queue );
    CHECK_HR( IXStoreImpl_XStoreAcquireLicenseForPackageAsync( store, context, UNOWNED_ID, async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), S_OK );
    CHECK_HR( IXStoreImpl_XStoreAcquireLicenseForPackageResult( store, async, &license ), S_OK );
    CHECK( !IXStoreImpl_XStoreIsLicenseValid( store, license ) );
    IXStoreImpl_XStoreCloseLicenseHandle( store, license );

    async = new_async( queue );
    CHECK_HR( IXStoreImpl_XStoreCanAcquireLicenseForStoreIdAsync( store, context, "9PCR7TSM1S96", async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), S_OK );
    CHECK_HR( IXStoreImpl_XStoreCanAcquireLicenseForStoreIdResult( store, async, &can_acquire ), S_OK );
    CHECK( can_acquire.status == XStoreCanLicenseStatus_Licensable && !strcmp( can_acquire.licensableSku, "0010" ) );

    async = new_async( queue );
    CHECK_HR( IXStoreImpl_XStoreCanAcquireLicenseForPackageAsync( store, context, UNOWNED_ID, async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), S_OK );
    CHECK_HR( IXStoreImpl_XStoreCanAcquireLicenseForPackageResult( store, async, &can_acquire ), S_OK );
    CHECK( can_acquire.status == XStoreCanLicenseStatus_NotLicensableToUser );

    async = new_async( queue );
    CHECK_HR( IXStoreImpl_XStoreQueryAssociatedProductsAsync( store, context, XStoreProductKind_Durable, 25, async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), S_OK );
    CHECK_HR( IXStoreImpl_XStoreQueryAssociatedProductsResult( store, async, &query ), S_OK );
    CHECK( !IXStoreImpl_XStoreProductsQueryHasMorePages( store, query ) );
    query_products( store, query, &products );
    CHECK( products.count == 2 && products.owned[0] && !products.owned[1] && products.kinds[0] == XStoreProductKind_Durable );

    async = new_async( queue );
    CHECK_HR( IXStoreImpl_XStoreQueryEntitledProductsAsync( store, context, XStoreProductKind_Durable | XStoreProductKind_Game, 25, async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), S_OK );
    CHECK_HR( IXStoreImpl_XStoreQueryEntitledProductsResult( store, async, &query ), S_OK );
    query_products( store, query, &products );
    CHECK( products.count == 1 && !strcmp( products.store_ids[0], "9PCR7TSM1S96" ) );

    async = new_async( queue );
    CHECK_HR( IXStoreImpl_XStoreQueryProductForCurrentGameAsync( store, context, async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), S_OK );
    CHECK_HR( IXStoreImpl_XStoreQueryProductForCurrentGameResult( store, async, &query ), S_OK );
    query_products( store, query, &products );
    CHECK( products.count == 1 && !strcmp( products.store_ids[0], "9NNX1VVR3KNQ" ) && products.kinds[0] == XStoreProductKind_Game && products.owned[0] );

    {
        const char *ids[] = { "9NNX1VVR3KNQ", "9PCR7TSM1S96", "9ZZZZZZZZZZZ" };
        async = new_async( queue );
        CHECK_HR( IXStoreImpl_XStoreQueryProductsAsync( store, context, XStoreProductKind_Durable | XStoreProductKind_Game, ids, 3, NULL, 0, async ), S_OK );
        CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), S_OK );
        CHECK_HR( IXStoreImpl_XStoreQueryProductsResult( store, async, &query ), S_OK );
        query_products( store, query, &products );
        CHECK( products.count == 2 && !strcmp( products.store_ids[0], "9NNX1VVR3KNQ" ) && !strcmp( products.store_ids[1], "9PCR7TSM1S96" ) );
    }

    async = new_async( queue );
    CHECK_HR( IXStoreImpl_XStoreQueryProductForPackageAsync( store, context, XStoreProductKind_Durable, UNOWNED_ID, async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, async, TRUE ), S_OK );
    CHECK_HR( IXStoreImpl_XStoreQueryProductForPackageResult( store, async, &query ), S_OK );
    query_products( store, query, &products );
    CHECK( products.count == 1 && !strcmp( products.store_ids[0], "9P5H6T4ZVR8R" ) && !products.owned[0] );

    CHECK_HR( IXStoreImpl_XStoreQueryPackageIdentifier( store, "9PCR7TSM1S96", sizeof(id), id ), S_OK );
    CHECK( !strcmp( id, OWNED_ID ) );
    CHECK_HR( IXStoreImpl_XStoreQueryPackageIdentifier( store, "9ZZZZZZZZZZZ", sizeof(id), id ), E_GAMEPACKAGE_NO_PACKAGE_IDENTIFIER );

    /* An add-on installed while the game runs. */
    CHECK_HR( IXPackageImpl_XPackageRegisterPackageInstalled( packages, queue, NULL, on_installed, &token ), S_OK );
    CHECK( WaitForSingleObject( installed_event, 20000 ) == WAIT_OBJECT_0 );
    CHECK( !strcmp( installed_id, NEW_ID ) );
    IXPackageImpl_XPackageUnregisterPackageInstalled( packages, token, FALSE );

    memset( &seen, 0, sizeof(seen) );
    CHECK_HR( IXPackageImpl_XPackageEnumeratePackages( packages, XPackageKind_Content, XPackageEnumerationScope_ThisOnly, &seen, on_package ), S_OK );
    CHECK( seen.count == 3 );

    IXStoreImpl_XStoreCloseContextHandle( store, context );
    printf( "%d failures\n", failures );
    return failures ? 1 : 0;
}

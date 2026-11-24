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
#include <time.h>

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

static BOOL is_license_verified(void);
static void get_sku_store_id( char *out, SIZE_T size );

/*
 * Products and licenses of the game and its add-ons (DLC).
 *
 * Xodus checks the game license before it starts the game. Add-ons are the ones Xodus installed
 * for the game; each one's license is asked from the licensing service through the bridge (see
 * xodusaddons.c). The bridge also knows the Store: the game's other products, prices and what the
 * user owns of them. Without an answer from it only the installed ones are known.
 */
#define PRODUCT_QUERY_MAGIC 0x51505358 /* XSPQ */
#define LICENSE_MAGIC 0x434c5358       /* XSLC */

/* Durable licenses from the Store do not expire; it reports them with this end of time. */
#define NO_EXPIRATION ((time_t)253402300799) /* 9999-12-31 23:59:59 UTC */

/* All products a query found; its pages share them. */
struct product_set
{
    LONG ref;
    UINT32 count, capacity;
    XStoreProduct *products;
};

struct product_query
{
    DWORD magic;
    struct product_set *set;
    UINT32 first, count, page_size;
};

struct store_license
{
    DWORD magic;
    BOOL valid;
};

static struct product_query *product_query_from_handle( XStoreProductQueryHandle handle )
{
    struct product_query *query = handle;
    return query && query->magic == PRODUCT_QUERY_MAGIC ? query : NULL;
}

static struct store_license *license_from_handle( XStoreLicenseHandle handle )
{
    struct store_license *license = handle;
    return license && license->magic == LICENSE_MAGIC ? license : NULL;
}

static void free_product( XStoreProduct *product )
{
    UINT32 i;

    free( (char *)product->storeId );
    free( (char *)product->title );
    free( (char *)product->description );
    free( (char *)product->inAppOfferToken );
    free( (char *)product->price.currencyCode );
    free( product->linkUri );
    if (product->images) free( (char *)product->images->uri );
    free( product->images );
    for (i = 0; i < product->skusCount; i++)
    {
        free( (char *)product->skus[i].skuId );
        if (product->skus[i].availabilities) free( (char *)product->skus[i].availabilities->availabilityId );
        free( product->skus[i].availabilities );
    }
    free( product->skus );
}

static void release_product_set( struct product_set *set )
{
    UINT32 i;

    if (!set || InterlockedDecrement( &set->ref )) return;
    for (i = 0; i < set->count; i++) free_product( &set->products[i] );
    free( set->products );
    free( set );
}

static void free_product_query( void *handle )
{
    struct product_query *query = handle;

    if (!query) return;
    release_product_set( query->set );
    query->magic = 0;
    free( query );
}

/* A page of the set; takes over the caller's reference. */
static struct product_query *new_product_page( struct product_set *set, UINT32 first, UINT32 page_size )
{
    struct product_query *query;

    if (!(query = calloc( 1, sizeof(*query) )))
    {
        release_product_set( set );
        return NULL;
    }
    query->magic = PRODUCT_QUERY_MAGIC;
    query->set = set;
    query->first = first;
    query->page_size = page_size;
    query->count = set->count - first;
    if (page_size && query->count > page_size) query->count = page_size;
    return query;
}

/* What is known about a product. */
struct product_info
{
    const char *store_id, *sku_id, *title, *description, *offer_token, *currency, *availability_id, *image;
    XStoreProductKind kind;
    UINT32 quantity, image_width, image_height;
    float base_price, price;
    time_t offer_end;
    BOOL has_download;
};

static void format_price( char out[16], float price, const char *currency )
{
    snprintf( out, 16, "%.2f %s", price, currency );
}

static const char *or_empty( const char *text )
{
    return text ? text : "";
}

/* Adds a product with a single SKU; strings are copied. */
static HRESULT add_product( struct product_set *set, const struct product_info *info )
{
    XStoreProduct *product, *grown;
    XStoreSku *sku;
    char link[96];

    if (set->count == set->capacity)
    {
        UINT32 capacity = set->capacity ? set->capacity * 2 : 16;
        if (!(grown = realloc( set->products, capacity * sizeof(*grown) ))) return E_OUTOFMEMORY;
        set->products = grown;
        set->capacity = capacity;
    }
    product = &set->products[set->count];
    memset( product, 0, sizeof(*product) );
    if (!(sku = calloc( 1, sizeof(*sku) ))) return E_OUTOFMEMORY;

    snprintf( link, sizeof(link), "https://www.microsoft.com/store/productId/%s", info->store_id );
    product->skus = sku;
    product->skusCount = 1;
    product->storeId = strdup( info->store_id );
    product->title = strdup( or_empty( info->title ) );
    product->description = strdup( or_empty( info->description ) );
    product->inAppOfferToken = strdup( or_empty( info->offer_token ) );
    product->price.currencyCode = strdup( or_empty( info->currency ) );
    product->linkUri = strdup( link );
    sku->skuId = strdup( info->sku_id );
    if (!product->storeId || !product->title || !product->description || !product->inAppOfferToken ||
        !product->price.currencyCode || !product->linkUri || !sku->skuId) goto failed;

    product->language = "";
    product->productKind = info->kind;
    product->hasDigitalDownload = !!info->has_download;
    product->isInUserCollection = info->quantity > 0;
    product->price.basePrice = info->base_price;
    product->price.price = info->price;
    product->price.recurrencePrice = info->price;
    format_price( product->price.formattedBasePrice, info->base_price, product->price.currencyCode );
    format_price( product->price.formattedPrice, info->price, product->price.currencyCode );
    format_price( product->price.formattedRecurrencePrice, info->price, product->price.currencyCode );
    if ((product->price.isOnSale = info->price < info->base_price)) product->price.saleEndDate = info->offer_end;

    if (info->image && *info->image)
    {
        if (!(product->images = calloc( 1, sizeof(*product->images) ))) goto failed;
        product->imagesCount = 1;
        if (!(product->images->uri = strdup( info->image ))) goto failed;
        product->images->width = info->image_width;
        product->images->height = info->image_height;
        product->images->caption = "";
        product->images->imagePurposeTag = "Poster";
    }

    sku->title = product->title;
    sku->description = product->description;
    sku->language = "";
    sku->price = product->price;
    sku->isInUserCollection = info->quantity > 0;
    sku->collectionData.quantity = info->quantity;
    sku->collectionData.endDate = info->quantity ? NO_EXPIRATION : 0;
    sku->collectionData.campaignId = "";
    sku->collectionData.developerOfferId = "";
    if (info->availability_id && *info->availability_id)
    {
        if (!(sku->availabilities = calloc( 1, sizeof(*sku->availabilities) ))) goto failed;
        sku->availabilitiesCount = 1;
        if (!(sku->availabilities->availabilityId = strdup( info->availability_id ))) goto failed;
        sku->availabilities->price = product->price;
        sku->availabilities->endDate = info->offer_end ? info->offer_end : NO_EXPIRATION;
    }
    set->count++;
    return S_OK;

failed:
    free_product( product );
    return E_OUTOFMEMORY;
}

enum store_query
{
    QUERY_ASSOCIATED,
    QUERY_ENTITLED,
    QUERY_STORE_IDS,
    QUERY_CURRENT_GAME,
    QUERY_PACKAGE,
    QUERY_STORE_ID_ADDONS,
};

static BOOL valid_product_id( const char *id )
{
    SIZE_T i;

    if (!id || !*id) return FALSE;
    for (i = 0; id[i]; i++) if (i >= GDKC_STORE_ID_SIZE - 1 || !isalnum( (unsigned char)id[i] )) return FALSE;
    return TRUE;
}

struct store_request
{
    HRESULT (*work)( struct store_request *request );
    enum store_query query;
    XStoreProductKind kinds;
    UINT32 page_size;
    struct product_set *set;
    UINT32 first;
    UINT32 quantity;
    GUID tracking_id;
    char **ids;
    SIZE_T ids_count;
    char *id;
    /* Bytes handed to the caller by the Result call. */
    void *result;
    SIZE_T result_size;
    /* When the result is a handle: frees the object it points to unless the caller took it. */
    void (*release)( void *object );
    BOOL taken;
};

static void free_store_request( struct store_request *request )
{
    SIZE_T i;

    if (request->release && !request->taken && request->result) request->release( *(void **)request->result );
    for (i = 0; i < request->ids_count; i++) free( request->ids[i] );
    free( request->ids );
    free( request->id );
    release_product_set( request->set );
    free( request->result );
    free( request );
}

static HRESULT WINAPI store_request_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct store_request *request = data->context;
    HRESULT hr;

    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );

        case XAsyncOp_DoWork:
            hr = request->work( request );
            if (FAILED(hr)) WARN( "store request failed, hr %#lx.\n", hr );
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, hr, SUCCEEDED(hr) ? request->result_size : 0 );
            return S_OK;

        case XAsyncOp_GetResult:
            if (data->bufferSize < request->result_size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            memcpy( data->buffer, request->result, request->result_size );
            request->taken = TRUE;
            return S_OK;

        case XAsyncOp_Cleanup:
            free_store_request( request );
            return S_OK;

        default:
            return S_OK;
    }
}

static struct store_request *new_store_request( HRESULT (*work)( struct store_request * ), const char *id )
{
    struct store_request *request = calloc( 1, sizeof(*request) );

    if (!request) return NULL;
    request->work = work;
    if (id && !(request->id = strdup( id )))
    {
        free( request );
        return NULL;
    }
    return request;
}

static HRESULT begin_store_request( XAsyncBlock *async, struct store_request *request, const void *identity, const char *name )
{
    HRESULT hr;

    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, request, identity, name, store_request_provider )))
        free_store_request( request );
    return hr;
}

/* Hands out a handle as the result. */
static HRESULT set_handle_result( struct store_request *request, void *handle, void (*release)( void * ) )
{
    if (!(request->result = malloc( sizeof(handle) )))
    {
        release( handle );
        return E_OUTOFMEMORY;
    }
    memcpy( request->result, &handle, sizeof(handle) );
    request->result_size = sizeof(handle);
    request->release = release;
    return S_OK;
}

static BOOL is_game_store_id( const char *store_id )
{
    char game[GDKC_STORE_ID_SIZE];
    return store_id && gdkc_config_get_store_id( game, sizeof(game) ) && !_stricmp( game, store_id );
}

static BOOL is_game_package( const char *package_id )
{
    char game[GDKC_PACKAGE_ID_SIZE], id[GDKC_PACKAGE_ID_SIZE];
    return gdkc_game_package_id( game ) && gdkc_normalize_package_id( package_id, id ) && !strcmp( game, id );
}

static BOOL wants_game_id( const struct store_request *request )
{
    SIZE_T i;

    for (i = 0; i < request->ids_count; i++) if (is_game_store_id( request->ids[i] )) return TRUE;
    return FALSE;
}

static BOOL wants_id( const struct store_request *request, const char *store_id )
{
    SIZE_T i;

    for (i = 0; i < request->ids_count; i++) if (!_stricmp( request->ids[i], store_id )) return TRUE;
    return FALSE;
}

static HRESULT add_game_product( struct product_set *set )
{
    char store_id[GDKC_STORE_ID_SIZE], sku_store_id[32], name[256], *sku;
    struct product_info info = {0};

    if (!gdkc_config_get_store_id( store_id, sizeof(store_id) )) return S_OK;
    if (!gdkc_config_get_display_name( name, sizeof(name) )) strcpy( name, store_id );
    get_sku_store_id( sku_store_id, sizeof(sku_store_id) );
    sku = (sku = strchr( sku_store_id, '/' )) ? sku + 1 : "0010";
    info.store_id = store_id;
    info.sku_id = sku;
    info.title = name;
    info.kind = XStoreProductKind_Game;
    info.quantity = is_license_verified() ? 1 : 0;
    info.has_download = TRUE;
    return add_product( set, &info );
}

static XStoreProductKind parse_product_kind( const char *name )
{
    if (!strcmp( name, "Game" )) return XStoreProductKind_Game;
    if (!strcmp( name, "Durable" )) return XStoreProductKind_Durable;
    if (!strcmp( name, "Consumable" )) return XStoreProductKind_Consumable;
    if (!strcmp( name, "UnmanagedConsumable" )) return XStoreProductKind_UnmanagedConsumable;
    if (!strcmp( name, "Pass" )) return XStoreProductKind_Pass;
    return XStoreProductKind_None;
}

static BOOL has_product( const struct product_set *set, const char *store_id )
{
    UINT32 i;

    for (i = 0; i < set->count; i++) if (!_stricmp( set->products[i].storeId, store_id )) return TRUE;
    return FALSE;
}

/* A price of the Store's list: always with a point, whatever locale the game set. */
static float parse_price( const char *text )
{
    float value = 0, scale = 1;

    for (; *text >= '0' && *text <= '9'; text++) value = value * 10 + (*text - '0');
    if (*text != '.') return value;
    for (text++; *text >= '0' && *text <= '9'; text++) value += (*text - '0') * (scale /= 10);
    return value;
}

/* Adds the products of the bridge's list, see begin_product_query(). Lines that do not fit are left out. */
static HRESULT add_listed_products( struct product_set *set, char *text, const struct store_request *request )
{
    const struct gdkc_addon *addon;
    struct gdkc_addons *addons = NULL;
    char *line, *next, *tab, *fields[16];
    HRESULT hr = S_OK;
    UINT32 i;

    /* The licenses of installed add-ons are known for sure. */
    if (FAILED(gdkc_addons_get( &addons ))) addons = NULL;
    for (line = text; SUCCEEDED(hr) && line && *line; line = next)
    {
        struct product_info info = {0};
        BOOL licensed;

        if ((next = strchr( line, '\n' ))) *next++ = 0;
        for (i = 0; i < ARRAY_SIZE(fields); i++)
        {
            fields[i] = line;
            if (!line) break;
            if ((tab = strchr( line, '\t' ))) *tab = 0;
            line = tab ? tab + 1 : NULL;
        }
        if (i != ARRAY_SIZE(fields) || line) { WARN( "malformed product line.\n" ); continue; }
        info.store_id = fields[0];
        info.kind = parse_product_kind( fields[1] );
        info.sku_id = fields[2];
        info.quantity = strtoul( fields[3], NULL, 10 );
        info.has_download = fields[4][0] == '1';
        info.base_price = parse_price( fields[5] );
        info.price = parse_price( fields[6] );
        info.currency = fields[7];
        info.offer_end = _strtoi64( fields[8], NULL, 10 );
        info.availability_id = fields[9];
        info.offer_token = fields[10];
        info.image = fields[11];
        info.image_width = strtoul( fields[12], NULL, 10 );
        info.image_height = strtoul( fields[13], NULL, 10 );
        info.title = fields[14];
        info.description = fields[15];
        if (!*info.store_id || strlen( info.store_id ) >= GDKC_STORE_ID_SIZE || strlen( info.sku_id ) != 4) continue;
        if (!(info.kind & request->kinds) || has_product( set, info.store_id )) continue;
        if (is_game_store_id( info.store_id )) info.quantity = is_license_verified() ? 1 : 0;
        else if (addons && (addon = gdkc_addons_find( addons, info.store_id )) && SUCCEEDED(gdkc_addon_license( addon, &licensed )))
            info.quantity = licensed ? 1 : 0;
        if (request->query == QUERY_ENTITLED && !info.quantity) continue;
        hr = add_product( set, &info );
    }
    gdkc_addons_release( addons );
    return hr;
}

/* The products of a query as Xodus knows them from the Store, see xodusbridge.c; NULL when it does not answer. */
static char *ask_products( const struct store_request *request )
{
    char game[GDKC_STORE_ID_SIZE], *body, *p, *response = NULL;
    SIZE_T size = 64, i;

    if (!gdkc_bridge_available() || !gdkc_config_get_store_id( game, sizeof(game) )) return NULL;
    for (i = 0; i < request->ids_count; i++) size += strlen( request->ids[i] ) + 1;
    if (request->id) size += strlen( request->id );
    if (!(body = malloc( size ))) return NULL;
    switch (request->query)
    {
        case QUERY_ASSOCIATED:
        case QUERY_ENTITLED:
            sprintf( body, "associated\n%s\n", game );
            break;
        case QUERY_STORE_ID_ADDONS:
            sprintf( body, "associated\n%s\n", request->id );
            break;
        case QUERY_CURRENT_GAME:
            sprintf( body, "products\n%s\n", game );
            break;
        case QUERY_STORE_IDS:
            p = body + sprintf( body, "products\n" );
            for (i = 0; i < request->ids_count; i++)
                if (!strpbrk( request->ids[i], "\r\n" )) p += sprintf( p, "%s\n", request->ids[i] );
            break;
        default:
            free( body );
            return NULL;
    }
    if (FAILED(gdkc_bridge_post( L"/store/products", body, &response ))) response = NULL;
    free( body );
    return response;
}

static HRESULT product_query_work( struct store_request *request )
{
    struct gdkc_addons *addons = NULL;
    struct product_query *query;
    struct product_set *set;
    BOOL want_game = FALSE;
    HRESULT hr = S_OK;
    char *listed;
    UINT32 i;

    if (request->query != QUERY_CURRENT_GAME && FAILED(hr = gdkc_addons_get( &addons ))) return hr;

    if (!(set = calloc( 1, sizeof(*set) ))) { gdkc_addons_release( addons ); return E_OUTOFMEMORY; }
    set->ref = 1;

    if ((listed = ask_products( request )))
    {
        hr = add_listed_products( set, listed, request );
        free( listed );
    }

    /* Installed but not listed, or the Store did not answer: what is known without it. */
    switch (request->query)
    {
        case QUERY_CURRENT_GAME: want_game = TRUE; break;
        case QUERY_STORE_IDS: want_game = (request->kinds & XStoreProductKind_Game) && wants_game_id( request ); break;
        case QUERY_PACKAGE: want_game = (request->kinds & XStoreProductKind_Game) && is_game_package( request->id ); break;
        default: break;
    }
    if (want_game)
    {
        char game[GDKC_STORE_ID_SIZE];
        if (SUCCEEDED(hr) && gdkc_config_get_store_id( game, sizeof(game) ) && !has_product( set, game )) hr = add_game_product( set );
    }

    for (i = 0; SUCCEEDED(hr) && addons && i < addons->count && (request->kinds & XStoreProductKind_Durable); i++)
    {
        const struct gdkc_addon *addon = &addons->items[i];
        struct product_info info = {0};
        BOOL licensed = FALSE;

        if (request->query == QUERY_CURRENT_GAME) break;
        if (request->query == QUERY_STORE_IDS && !wants_id( request, addon->store_id )) continue;
        if (request->query == QUERY_PACKAGE && gdkc_addons_find( addons, request->id ) != addon) continue;
        if (request->query == QUERY_STORE_ID_ADDONS && !is_game_store_id( request->id )) continue;
        if (has_product( set, addon->store_id )) continue;
        if (FAILED(gdkc_addon_license( addon, &licensed ))) licensed = FALSE;
        if (request->query == QUERY_ENTITLED && !licensed) continue;
        info.store_id = addon->store_id;
        info.sku_id = "0010";
        info.title = addon->name;
        info.kind = XStoreProductKind_Durable;
        info.quantity = licensed ? 1 : 0;
        info.has_download = TRUE;
        hr = add_product( set, &info );
    }
    gdkc_addons_release( addons );

    if (FAILED(hr))
    {
        release_product_set( set );
        return hr;
    }
    TRACE( "query %d found %u products.\n", request->query, set->count );
    if (!(query = new_product_page( set, 0, request->page_size ))) return E_OUTOFMEMORY;
    return set_handle_result( request, query, free_product_query );
}

static HRESULT next_page_work( struct store_request *request )
{
    struct product_query *query;

    InterlockedIncrement( &request->set->ref );
    if (!(query = new_product_page( request->set, request->first, request->page_size ))) return E_OUTOFMEMORY;
    return set_handle_result( request, query, free_product_query );
}

static HRESULT begin_product_query( XAsyncBlock *async, enum store_query type, XStoreProductKind kinds, const char *id,
                                    const char **ids, SIZE_T ids_count, UINT32 page_size, const void *identity, const char *name )
{
    struct store_request *request;
    SIZE_T i;

    if (!async) return E_INVALIDARG;
    if (!(request = new_store_request( product_query_work, id ))) return E_OUTOFMEMORY;
    request->query = type;
    request->page_size = page_size;
    request->kinds = kinds;
    if (ids_count)
    {
        if (!(request->ids = calloc( ids_count, sizeof(*request->ids) ))) { free_store_request( request ); return E_OUTOFMEMORY; }
        for (i = 0; i < ids_count; i++)
        {
            if (!(request->ids[i] = strdup( ids[i] ))) { request->ids_count = i; free_store_request( request ); return E_OUTOFMEMORY; }
        }
        request->ids_count = ids_count;
    }
    return begin_store_request( async, request, identity, name );
}

static HRESULT get_product_query_result( XAsyncBlock *async, const void *identity, XStoreProductQueryHandle *productQueryHandle )
{
    if (!productQueryHandle) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, identity, sizeof(*productQueryHandle), productQueryHandle, NULL );
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, productKinds %#x, maxItemsToRetrievePerPage %u, async %p.\n", iface, storeContextHandle, productKinds, maxItemsToRetrievePerPage, async );

    if (!storeContextHandle) return E_INVALIDARG;
    return begin_product_query( async, QUERY_ASSOCIATED, productKinds, NULL, NULL, 0, maxItemsToRetrievePerPage, x_store_XStoreQueryAssociatedProductsAsync, "XStoreQueryAssociatedProductsAsync" );
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return get_product_query_result( async, x_store_XStoreQueryAssociatedProductsAsync, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreQueryProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, const char **storeIds, SIZE_T storeIdsCount, const char **actionFilters, SIZE_T actionFiltersCount, XAsyncBlock *async )
{
    SIZE_T i;

    TRACE( "iface %p, storeContextHandle %p, productKinds %#x, storeIds %p, storeIdsCount %Iu, actionFilters %p, actionFiltersCount %Iu, async %p.\n", iface, storeContextHandle, productKinds, storeIds, storeIdsCount, actionFilters, actionFiltersCount, async );

    if (!storeContextHandle || (storeIdsCount && !storeIds)) return E_INVALIDARG;
    for (i = 0; i < storeIdsCount; i++) if (!storeIds[i]) return E_INVALIDARG;
    return begin_product_query( async, QUERY_STORE_IDS, productKinds, NULL, storeIds, storeIdsCount, 0, x_store_XStoreQueryProductsAsync, "XStoreQueryProductsAsync" );
}

static HRESULT WINAPI x_store_XStoreQueryProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return get_product_query_result( async, x_store_XStoreQueryProductsAsync, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, productKinds %#x, maxItemsToRetrievePerPage %u, async %p.\n", iface, storeContextHandle, productKinds, maxItemsToRetrievePerPage, async );

    if (!storeContextHandle) return E_INVALIDARG;
    return begin_product_query( async, QUERY_ENTITLED, productKinds, NULL, NULL, 0, maxItemsToRetrievePerPage, x_store_XStoreQueryEntitledProductsAsync, "XStoreQueryEntitledProductsAsync" );
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return get_product_query_result( async, x_store_XStoreQueryEntitledProductsAsync, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreQueryProductForCurrentGameAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, async %p.\n", iface, storeContextHandle, async );

    if (!storeContextHandle) return E_INVALIDARG;
    return begin_product_query( async, QUERY_CURRENT_GAME, XStoreProductKind_Game, NULL, NULL, 0, 0, x_store_XStoreQueryProductForCurrentGameAsync, "XStoreQueryProductForCurrentGameAsync" );
}

static HRESULT WINAPI x_store_XStoreQueryProductForCurrentGameResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return get_product_query_result( async, x_store_XStoreQueryProductForCurrentGameAsync, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreQueryProductForPackageAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, const char *packageIdentifier, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, productKinds %#x, packageIdentifier %s, async %p.\n", iface, storeContextHandle, productKinds, debugstr_a( packageIdentifier ), async );

    if (!storeContextHandle || !packageIdentifier) return E_INVALIDARG;
    return begin_product_query( async, QUERY_PACKAGE, productKinds, packageIdentifier, NULL, 0, 0, x_store_XStoreQueryProductForPackageAsync, "XStoreQueryProductForPackageAsync" );
}

static HRESULT WINAPI x_store_XStoreQueryProductForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return get_product_query_result( async, x_store_XStoreQueryProductForPackageAsync, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreEnumerateProductsQuery( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, void *context, XStoreProductQueryCallback *callback )
{
    struct product_query *query = product_query_from_handle( productQueryHandle );
    UINT32 i;

    TRACE( "iface %p, productQueryHandle %p, context %p, callback %p.\n", iface, productQueryHandle, context, callback );

    if (!query || !callback) return E_INVALIDARG;
    for (i = 0; i < query->count; i++) if (!callback( &query->set->products[query->first + i], context )) break;
    return S_OK;
}

static BOOLEAN WINAPI x_store_XStoreProductsQueryHasMorePages( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle )
{
    struct product_query *query = product_query_from_handle( productQueryHandle );

    TRACE( "iface %p, productQueryHandle %p.\n", iface, productQueryHandle );
    return query && query->first + query->count < query->set->count;
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageAsync( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, XAsyncBlock *async )
{
    struct product_query *query = product_query_from_handle( productQueryHandle );
    struct store_request *request;

    TRACE( "iface %p, productQueryHandle %p, async %p.\n", iface, productQueryHandle, async );

    if (!query || !async) return E_INVALIDARG;
    if (!(request = new_store_request( next_page_work, NULL ))) return E_OUTOFMEMORY;
    InterlockedIncrement( &query->set->ref );
    request->set = query->set;
    request->first = query->first + query->count;
    request->page_size = query->page_size;
    return begin_store_request( async, request, x_store_XStoreProductsQueryNextPageAsync, "XStoreProductsQueryNextPageAsync" );
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );

    if (!productQueryHandle) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreProductsQueryNextPageAsync, sizeof(*productQueryHandle), productQueryHandle, NULL );
}

static void WINAPI x_store_XStoreCloseProductsQueryHandle( IXStoreImpl6 *iface, XStoreProductQueryHandle productQueryHandle )
{
    TRACE( "iface %p, productQueryHandle %p.\n", iface, productQueryHandle );
    free_product_query( product_query_from_handle( productQueryHandle ) );
}

/* Whether the user holds a license for the package: the game's own, or an installed add-on. */
static HRESULT package_licensed( const char *package_id, BOOL *licensed, BOOL *found )
{
    const struct gdkc_addon *addon;
    struct gdkc_addons *addons;
    HRESULT hr = S_OK;

    *licensed = *found = FALSE;
    if (is_game_package( package_id ))
    {
        *found = TRUE;
        *licensed = is_license_verified();
        return S_OK;
    }
    if (FAILED(hr = gdkc_addons_get( &addons ))) return hr;
    if ((addon = gdkc_addons_find( addons, package_id )))
    {
        *found = TRUE;
        hr = gdkc_addon_license( addon, licensed );
    }
    gdkc_addons_release( addons );
    return hr;
}

static HRESULT store_id_licensed( const char *store_id, BOOL *licensed, BOOL *found )
{
    const struct gdkc_addon *addon;
    struct gdkc_addons *addons;
    HRESULT hr = S_OK;

    *licensed = *found = FALSE;
    if (is_game_store_id( store_id ))
    {
        *found = TRUE;
        *licensed = is_license_verified();
        return S_OK;
    }
    if (FAILED(hr = gdkc_addons_get( &addons ))) return hr;
    if ((addon = gdkc_addons_find( addons, store_id )))
    {
        *found = TRUE;
        hr = gdkc_addon_license( addon, licensed );
    }
    gdkc_addons_release( addons );

    /* Not installed, or an unlock without files: whether the user owns it. */
    if (SUCCEEDED(hr) && !*found && valid_product_id( store_id ) && gdkc_bridge_available())
    {
        struct store_request request = { .query = QUERY_STORE_IDS, .kinds = XStoreProductKind_Durable | XStoreProductKind_Game,
                                         .ids = (char **)&store_id, .ids_count = 1 };
        struct product_set set = {0};
        char *listed;
        UINT32 i;

        if ((listed = ask_products( &request )))
        {
            hr = add_listed_products( &set, listed, &request );
            free( listed );
            if (set.count)
            {
                *found = TRUE;
                *licensed = set.products[0].isInUserCollection;
            }
            for (i = 0; i < set.count; i++) free_product( &set.products[i] );
            free( set.products );
        }
    }
    return hr;
}

static void free_license( void *handle )
{
    struct store_license *license = handle;

    if (!license) return;
    license->magic = 0;
    free( license );
}

static HRESULT acquire_package_license_work( struct store_request *request )
{
    struct store_license *license;
    BOOL licensed, found;
    HRESULT hr;

    if (FAILED(hr = package_licensed( request->id, &licensed, &found ))) return hr;
    if (!found) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    if (!(license = calloc( 1, sizeof(*license) ))) return E_OUTOFMEMORY;
    license->magic = LICENSE_MAGIC;
    license->valid = licensed;
    return set_handle_result( request, license, free_license );
}

static HRESULT set_can_acquire_result( struct store_request *request, BOOL licensed, BOOL found )
{
    XStoreCanAcquireLicenseResult *result;

    if (!(result = calloc( 1, sizeof(*result) ))) return E_OUTOFMEMORY;
    if (licensed) strcpy( result->licensableSku, "0010" );
    result->status = licensed ? XStoreCanLicenseStatus_Licensable : XStoreCanLicenseStatus_NotLicensableToUser;
    if (!found) TRACE( "%s is not installed.\n", debugstr_a( request->id ) );
    request->result = result;
    request->result_size = sizeof(*result);
    return S_OK;
}

static HRESULT can_acquire_package_work( struct store_request *request )
{
    BOOL licensed, found;
    HRESULT hr;

    if (FAILED(hr = package_licensed( request->id, &licensed, &found ))) return hr;
    return set_can_acquire_result( request, licensed, found );
}

static HRESULT can_acquire_store_id_work( struct store_request *request )
{
    BOOL licensed, found;
    HRESULT hr;

    if (FAILED(hr = store_id_licensed( request->id, &licensed, &found ))) return hr;
    return set_can_acquire_result( request, licensed, found );
}

static HRESULT addon_licenses_work( struct store_request *request )
{
    XStoreAddonLicense *licenses = NULL;
    struct gdkc_addons *addons;
    UINT32 i, count = 0;
    BOOL licensed;
    HRESULT hr;

    if (FAILED(hr = gdkc_addons_get( &addons ))) return hr;
    if (addons->count && !(licenses = calloc( addons->count, sizeof(*licenses) )))
    {
        gdkc_addons_release( addons );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < addons->count; i++)
    {
        if (FAILED(gdkc_addon_license( &addons->items[i], &licensed )) || !licensed) continue;
        snprintf( licenses[count].skuStoreId, sizeof(licenses[count].skuStoreId), "%s/0010", addons->items[i].store_id );
        licenses[count].isActive = TRUE;
        licenses[count].expirationDate = NO_EXPIRATION;
        count++;
    }
    gdkc_addons_release( addons );

    TRACE( "%u add-on licenses.\n", count );
    request->result = licenses;
    request->result_size = count * sizeof(*licenses);
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForPackageAsync( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, const char *packageIdentifier, XAsyncBlock *async )
{
    struct store_request *request;

    TRACE( "iface %p, storeContextHandle %p, packageIdentifier %s, async %p.\n", iface, productQueryHandle, debugstr_a( packageIdentifier ), async );

    if (!productQueryHandle || !packageIdentifier || !async) return E_INVALIDARG;
    if (!(request = new_store_request( acquire_package_license_work, packageIdentifier ))) return E_OUTOFMEMORY;
    return begin_store_request( async, request, x_store_XStoreAcquireLicenseForPackageAsync, "XStoreAcquireLicenseForPackageAsync" );
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreLicenseHandle *storeLicenseHandle )
{
    TRACE( "iface %p, async %p, storeLicenseHandle %p.\n", iface, async, storeLicenseHandle );

    if (!storeLicenseHandle) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreAcquireLicenseForPackageAsync, sizeof(*storeLicenseHandle), storeLicenseHandle, NULL );
}

static BOOLEAN WINAPI x_store_XStoreIsLicenseValid( IXStoreImpl6 *iface, const XStoreLicenseHandle storeLicenseHandle )
{
    struct store_license *license = license_from_handle( storeLicenseHandle );

    TRACE( "iface %p, storeLicenseHandle %p.\n", iface, storeLicenseHandle );
    return license && license->valid;
}

static void WINAPI x_store_XStoreCloseLicenseHandle( IXStoreImpl6 *iface, XStoreLicenseHandle storeLicenseHandle )
{
    TRACE( "iface %p, storeLicenseHandle %p.\n", iface, storeLicenseHandle );
    free_license( license_from_handle( storeLicenseHandle ) );
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForStoreIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, XAsyncBlock *async )
{
    struct store_request *request;

    TRACE( "iface %p, storeContextHandle %p, storeProductId %s, async %p.\n", iface, storeContextHandle, debugstr_a( storeProductId ), async );

    if (!storeContextHandle || !storeProductId || !async) return E_INVALIDARG;
    if (!(request = new_store_request( can_acquire_store_id_work, storeProductId ))) return E_OUTOFMEMORY;
    return begin_store_request( async, request, x_store_XStoreCanAcquireLicenseForStoreIdAsync, "XStoreCanAcquireLicenseForStoreIdAsync" );
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForStoreIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreCanAcquireLicenseResult *storeCanAcquireLicense )
{
    TRACE( "iface %p, async %p, storeCanAcquireLicense %p.\n", iface, async, storeCanAcquireLicense );

    if (!storeCanAcquireLicense) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreCanAcquireLicenseForStoreIdAsync, sizeof(*storeCanAcquireLicense), storeCanAcquireLicense, NULL );
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForPackageAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *packageIdentifier, XAsyncBlock *async )
{
    struct store_request *request;

    TRACE( "iface %p, storeContextHandle %p, packageIdentifier %s, async %p.\n", iface, storeContextHandle, debugstr_a( packageIdentifier ), async );

    if (!storeContextHandle || !packageIdentifier || !async) return E_INVALIDARG;
    if (!(request = new_store_request( can_acquire_package_work, packageIdentifier ))) return E_OUTOFMEMORY;
    return begin_store_request( async, request, x_store_XStoreCanAcquireLicenseForPackageAsync, "XStoreCanAcquireLicenseForPackageAsync" );
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreCanAcquireLicenseResult *storeCanAcquireLicense )
{
    TRACE( "iface %p, async %p, storeCanAcquireLicense %p.\n", iface, async, storeCanAcquireLicense );

    if (!storeCanAcquireLicense) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreCanAcquireLicenseForPackageAsync, sizeof(*storeCanAcquireLicense), storeCanAcquireLicense, NULL );
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
    struct store_request *request;

    TRACE( "iface %p, storeContextHandle %p, async %p.\n", iface, storeContextHandle, async );

    if (!storeContextHandle || !async) return E_INVALIDARG;
    if (!(request = new_store_request( addon_licenses_work, NULL ))) return E_OUTOFMEMORY;
    return begin_store_request( async, request, x_store_XStoreQueryAddOnLicensesAsync, "XStoreQueryAddOnLicensesAsync" );
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    SIZE_T size;
    HRESULT hr;

    TRACE( "iface %p, async %p, count %p.\n", iface, async, count );

    if (!count) return E_INVALIDARG;
    if (FAILED(hr = IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, &size ))) return hr;
    *count = size / sizeof(XStoreAddonLicense);
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStoreAddonLicense *addOnLicenses )
{
    TRACE( "iface %p, async %p, count %u, addOnLicenses %p.\n", iface, async, count, addOnLicenses );

    if (count && !addOnLicenses) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreQueryAddOnLicensesAsync, count * sizeof(*addOnLicenses), addOnLicenses, NULL );
}

/* Consumables and keys: the bridge asks the Store for the signed in user. */
static HRESULT set_quantity_result( struct store_request *request, HRESULT hr, char *response )
{
    XStoreConsumableResult *result;

    if (FAILED(hr)) return hr == HRESULT_FROM_WIN32( ERROR_BAD_NET_RESP ) ? E_GAMESTORE_SERVER_ERROR : hr;
    if (!isdigit( (unsigned char)*response )) hr = E_GAMESTORE_SERVER_ERROR;
    else if (!(result = calloc( 1, sizeof(*result) ))) hr = E_OUTOFMEMORY;
    else
    {
        result->quantity = strtoul( response, NULL, 10 );
        request->result = result;
        request->result_size = sizeof(*result);
    }
    free( response );
    return hr;
}

static HRESULT consumable_balance_work( struct store_request *request )
{
    char *response = NULL;
    HRESULT hr = gdkc_bridge_post( L"/store/consumable", request->id, &response );

    return set_quantity_result( request, hr, response );
}

static HRESULT consume_work( struct store_request *request )
{
    const GUID *id = &request->tracking_id;
    char body[128], *response = NULL;
    HRESULT hr;

    snprintf( body, sizeof(body), "%s\n%u\n%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", request->id, request->quantity,
              id->Data1, id->Data2, id->Data3, id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3], id->Data4[4],
              id->Data4[5], id->Data4[6], id->Data4[7] );
    hr = gdkc_bridge_post( L"/store/consume", body, &response );
    return set_quantity_result( request, hr, response );
}

/* request->id is the body. */
static HRESULT user_key_work( struct store_request *request )
{
    char *response = NULL;
    HRESULT hr;

    if (FAILED(hr = gdkc_bridge_post( L"/store/user-key", request->id, &response )))
        return hr == HRESULT_FROM_WIN32( ERROR_BAD_NET_RESP ) ? E_GAMESTORE_SERVER_ERROR : hr;
    request->result = response;
    request->result_size = strlen( response ) + 1;
    return S_OK;
}

static HRESULT begin_user_key( XAsyncBlock *async, const char *type, const char *ticket, const char *user, const void *identity, const char *name )
{
    struct store_request *request;
    SIZE_T size;
    char *body;

    if (!async || !ticket || !*ticket || strpbrk( ticket, "\r\n" )) return E_INVALIDARG;
    if (!user) user = "";
    if (strpbrk( user, "\r\n" )) return E_INVALIDARG;
    size = strlen( type ) + strlen( ticket ) + strlen( user ) + 4;
    if (!(body = malloc( size ))) return E_OUTOFMEMORY;
    snprintf( body, size, "%s\n%s\n%s\n", type, ticket, user );
    request = new_store_request( user_key_work, body );
    free( body );
    if (!request) return E_OUTOFMEMORY;
    return begin_store_request( async, request, identity, name );
}

static HRESULT WINAPI x_store_XStoreQueryConsumableBalanceRemainingAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, XAsyncBlock *async )
{
    struct store_request *request;

    TRACE( "iface %p, storeContextHandle %p, storeProductId %s, async %p.\n", iface, storeContextHandle, debugstr_a( storeProductId ), async );

    if (!storeContextHandle || !valid_product_id( storeProductId ) || !async) return E_INVALIDARG;
    if (!(request = new_store_request( consumable_balance_work, storeProductId ))) return E_OUTOFMEMORY;
    return begin_store_request( async, request, x_store_XStoreQueryConsumableBalanceRemainingAsync, "XStoreQueryConsumableBalanceRemainingAsync" );
}

static HRESULT WINAPI x_store_XStoreQueryConsumableBalanceRemainingResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreConsumableResult *consumableResult )
{
    TRACE( "iface %p, async %p, consumableResult %p.\n", iface, async, consumableResult );

    if (!consumableResult) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreQueryConsumableBalanceRemainingAsync, sizeof(*consumableResult), consumableResult, NULL );
}

static HRESULT WINAPI x_store_XStoreReportConsumableFulfillmentAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, UINT32 quantity, GUID trackingId, XAsyncBlock *async )
{
    struct store_request *request;

    TRACE( "iface %p, storeContextHandle %p, storeProductId %s, quantity %u, trackingId %s, async %p.\n", iface, storeContextHandle, debugstr_a( storeProductId ), quantity, debugstr_guid( &trackingId ), async );

    if (!storeContextHandle || !valid_product_id( storeProductId ) || !quantity || !async) return E_INVALIDARG;
    if (!(request = new_store_request( consume_work, storeProductId ))) return E_OUTOFMEMORY;
    request->quantity = quantity;
    request->tracking_id = trackingId;
    return begin_store_request( async, request, x_store_XStoreReportConsumableFulfillmentAsync, "XStoreReportConsumableFulfillmentAsync" );
}

static HRESULT WINAPI x_store_XStoreReportConsumableFulfillmentResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreConsumableResult *consumableResult )
{
    TRACE( "iface %p, async %p, consumableResult %p.\n", iface, async, consumableResult );

    if (!consumableResult) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreReportConsumableFulfillmentAsync, sizeof(*consumableResult), consumableResult, NULL );
}

static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *serviceTicket, const char *publisherUserId, XAsyncBlock *async )
{
    /* The service ticket is a secret of the game. */
    TRACE( "iface %p, storeContextHandle %p, async %p.\n", iface, storeContextHandle, async );

    if (!storeContextHandle) return E_INVALIDARG;
    return begin_user_key( async, "collections", serviceTicket, publisherUserId, x_store_XStoreGetUserCollectionsIdAsync, "XStoreGetUserCollectionsIdAsync" );
}

static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    TRACE( "iface %p, async %p, size %p.\n", iface, async, size );

    if (!size) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, size );
}

static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    TRACE( "iface %p, async %p, size %Iu, result %p.\n", iface, async, size, result );

    if (!result) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreGetUserCollectionsIdAsync, size, result, NULL );
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *serviceTicket, const char *publisherUserId, XAsyncBlock *async )
{
    /* The service ticket is a secret of the game. */
    TRACE( "iface %p, storeContextHandle %p, async %p.\n", iface, storeContextHandle, async );

    if (!storeContextHandle) return E_INVALIDARG;
    return begin_user_key( async, "purchase", serviceTicket, publisherUserId, x_store_XStoreGetUserPurchaseIdAsync, "XStoreGetUserPurchaseIdAsync" );
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    TRACE( "iface %p, async %p, size %p.\n", iface, async, size );

    if (!size) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, size );
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    TRACE( "iface %p, async %p, size %Iu, result %p.\n", iface, async, size, result );

    if (!result) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreGetUserPurchaseIdAsync, size, result, NULL );
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
#define REDEEM_PAGE "redeem:"

static void open_store_page( const char *storeId )
{
    char cmd[160];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    const char *c;

    if (!storeId || !*storeId || strlen( storeId ) > 32) return;
    if (!strcmp( storeId, REDEEM_PAGE )) strcpy( cmd, "winebrowser.exe https://redeem.microsoft.com/" );
    else
    {
        for (c = storeId; *c; c++) if (!isalnum( (unsigned char)*c )) return;
        sprintf( cmd, "winebrowser.exe https://www.microsoft.com/store/productId/%s", storeId );
    }
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
    char game[GDKC_STORE_ID_SIZE];

    TRACE( "iface %p, storeContextHandle %p, async %p.\n", iface, storeContextHandle, async );

    if (!storeContextHandle) return E_INVALIDARG;
    if (!gdkc_config_get_store_id( game, sizeof(game) )) return E_GAMEPACKAGE_NO_STORE_ID;
    return begin_store_ui( async, game, x_store_XStoreShowRateAndReviewUIAsync, "XStoreShowRateAndReviewUIAsync" );
}

static HRESULT WINAPI x_store_XStoreShowRateAndReviewUIResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreRateAndReviewResult *result )
{
    TRACE( "iface %p, async %p, result %p.\n", iface, async, result );

    if (!result) return E_INVALIDARG;
    /* Whether the player wrote a review in the browser is not known. */
    result->wasUpdated = FALSE;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreShowRateAndReviewUIAsync, 0, NULL, NULL );
}

static HRESULT WINAPI x_store_XStoreShowRedeemTokenUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *token, const char **allowedStoreIds, SIZE_T allowedStoreIdsCount, BOOLEAN disallowCsvRedemption, XAsyncBlock *async )
{
    /* The token is as good as money: it is neither logged nor put into a URL, the player enters it on the page. */
    TRACE( "iface %p, storeContextHandle %p, allowedStoreIds %p, allowedStoreIdsCount %Iu, disallowCsvRedemption %d, async %p.\n", iface, storeContextHandle, allowedStoreIds, allowedStoreIdsCount, disallowCsvRedemption, async );

    if (!storeContextHandle) return E_INVALIDARG;
    return begin_store_ui( async, REDEEM_PAGE, x_store_XStoreShowRedeemTokenUIAsync, "XStoreShowRedeemTokenUIAsync" );
}

static HRESULT WINAPI x_store_XStoreShowRedeemTokenUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreShowRedeemTokenUIAsync, 0, NULL, NULL );
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
    TRACE( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p.\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );

    if (!storeContextHandle || !async || (packageIdentifiersCount && !packageIdentifiers)) return E_INVALIDARG;
    /* No update is ever reported, so there is none to download. */
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL, x_store_XStoreDownloadPackageUpdatesAsync, "XStoreDownloadPackageUpdatesAsync", no_updates_provider );
}

static HRESULT WINAPI x_store_XStoreDownloadPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreDownloadPackageUpdatesAsync, 0, NULL, NULL );
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackageUpdatesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **packageIdentifiers, SIZE_T packageIdentifiersCount, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p.\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );

    if (!storeContextHandle || !async || (packageIdentifiersCount && !packageIdentifiers)) return E_INVALIDARG;
    /* No update is ever reported, so there is none to download. */
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL, x_store_XStoreDownloadAndInstallPackageUpdatesAsync, "XStoreDownloadAndInstallPackageUpdatesAsync", no_updates_provider );
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreDownloadAndInstallPackageUpdatesAsync, 0, NULL, NULL );
}

/* Xodus installs add-ons, not the game: the ones that are there already are answered, for others it fails. */
static HRESULT install_packages_work( struct store_request *request )
{
    char (*result)[GDKC_PACKAGE_ID_SIZE];
    const struct gdkc_addon *addon;
    struct gdkc_addons *addons;
    HRESULT hr;
    SIZE_T i;

    if (FAILED(hr = gdkc_addons_get( &addons ))) return hr;
    if (!(result = calloc( request->ids_count, sizeof(*result) ))) hr = E_OUTOFMEMORY;
    for (i = 0; SUCCEEDED(hr) && i < request->ids_count; i++)
    {
        if ((addon = gdkc_addons_find( addons, request->ids[i] ))) strcpy( result[i], addon->package_id );
        else
        {
            WARN( "%s is not installed, install it with Xodus.\n", debugstr_a( request->ids[i] ) );
            hr = E_GAMEPACKAGE_DOWNLOAD_REQUIRED;
        }
    }
    gdkc_addons_release( addons );
    if (FAILED(hr))
    {
        free( result );
        return hr;
    }
    request->result = result;
    request->result_size = request->ids_count * sizeof(*result);
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **storeIds, SIZE_T storeIdsCount, XAsyncBlock *async )
{
    struct store_request *request;
    SIZE_T i;

    TRACE( "iface %p, storeContextHandle %p, storeIds %p, storeIdsCount %Iu, async %p.\n", iface, storeContextHandle, storeIds, storeIdsCount, async );

    if (!storeContextHandle || !async || !storeIds || !storeIdsCount) return E_INVALIDARG;
    for (i = 0; i < storeIdsCount; i++) if (!storeIds[i]) return E_INVALIDARG;
    if (!(request = new_store_request( install_packages_work, NULL ))) return E_OUTOFMEMORY;
    if (!(request->ids = calloc( storeIdsCount, sizeof(*request->ids) ))) { free_store_request( request ); return E_OUTOFMEMORY; }
    for (i = 0; i < storeIdsCount; i++, request->ids_count++)
        if (!(request->ids[i] = strdup( storeIds[i] ))) { free_store_request( request ); return E_OUTOFMEMORY; }
    return begin_store_request( async, request, x_store_XStoreDownloadAndInstallPackagesAsync, "XStoreDownloadAndInstallPackagesAsync" );
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    SIZE_T size;
    HRESULT hr;

    TRACE( "iface %p, async %p, count %p.\n", iface, async, count );

    if (!count) return E_INVALIDARG;
    if (FAILED(hr = IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, &size ))) return hr;
    *count = size / GDKC_PACKAGE_ID_SIZE;
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, char **packageIdentifiers )
{
    TRACE( "iface %p, async %p, count %u, packageIdentifiers %p.\n", iface, async, count, packageIdentifiers );

    if (count && !packageIdentifiers) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreDownloadAndInstallPackagesAsync, count * GDKC_PACKAGE_ID_SIZE, packageIdentifiers, NULL );
}

static HRESULT WINAPI x_store_XStoreQueryPackageIdentifier( IXStoreImpl6 *iface, const char *storeId, SIZE_T size, char *packageIdentifier )
{
    char id[GDKC_PACKAGE_ID_SIZE] = "";
    const struct gdkc_addon *addon;
    struct gdkc_addons *addons;
    HRESULT hr;

    TRACE( "iface %p, storeId %s, size %Iu, packageIdentifier %p.\n", iface, debugstr_a( storeId ), size, packageIdentifier );

    if (!storeId || !packageIdentifier) return E_INVALIDARG;
    if (is_game_store_id( storeId ))
    {
        if (!gdkc_game_package_id( id )) return E_GAMEPACKAGE_NO_PACKAGE_IDENTIFIER;
    }
    else
    {
        if (FAILED(hr = gdkc_addons_get( &addons ))) return hr;
        if ((addon = gdkc_addons_find( addons, storeId ))) strcpy( id, addon->package_id );
        gdkc_addons_release( addons );
        if (!*id) return E_GAMEPACKAGE_NO_PACKAGE_IDENTIFIER;
    }
    if (size < strlen( id ) + 1) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    strcpy( packageIdentifier, id );
    return S_OK;
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
    TRACE( "iface %p, availability %s.\n", iface, debugstr_a( availability.availabilityId ) );
    /* Only offers that can be bought are handed out. */
    return availability.availabilityId && *availability.availabilityId && availability.endDate > time( NULL );
}

static HRESULT acquire_durable_license_work( struct store_request *request )
{
    struct store_license *license;
    BOOL licensed, found;
    HRESULT hr;

    if (FAILED(hr = store_id_licensed( request->id, &licensed, &found ))) return hr;
    if (!found) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    if (!(license = calloc( 1, sizeof(*license) ))) return E_OUTOFMEMORY;
    license->magic = LICENSE_MAGIC;
    license->valid = licensed;
    return set_handle_result( request, license, free_license );
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForDurablesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XAsyncBlock *async )
{
    struct store_request *request;

    TRACE( "iface %p, storeContextHandle %p, storeId %s, async %p.\n", iface, storeContextHandle, debugstr_a( storeId ), async );

    if (!storeContextHandle || !valid_product_id( storeId ) || !async) return E_INVALIDARG;
    if (!(request = new_store_request( acquire_durable_license_work, storeId ))) return E_OUTOFMEMORY;
    return begin_store_request( async, request, x_store_XStoreAcquireLicenseForDurablesAsync, "XStoreAcquireLicenseForDurablesAsync" );
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForDurablesResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreLicenseHandle *storeLicenseHandle )
{
    TRACE( "iface %p, async %p, storeLicenseHandle %p.\n", iface, async, storeLicenseHandle );

    if (!storeLicenseHandle) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreAcquireLicenseForDurablesAsync, sizeof(*storeLicenseHandle), storeLicenseHandle, NULL );
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
    TRACE( "iface %p, storeContextHandle %p, storeId %s, productKinds %#x, maxItemsToRetrievePerPage %u, async %p.\n", iface, storeContextHandle, debugstr_a( storeId ), productKinds, maxItemsToRetrievePerPage, async );

    if (!storeContextHandle || !valid_product_id( storeId )) return E_INVALIDARG;
    return begin_product_query( async, QUERY_STORE_ID_ADDONS, productKinds, storeId, NULL, 0, maxItemsToRetrievePerPage, x_store_XStoreQueryAssociatedProductsForStoreIdAsync, "XStoreQueryAssociatedProductsForStoreIdAsync" );
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsForStoreIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return get_product_query_result( async, x_store_XStoreQueryAssociatedProductsForStoreIdAsync, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesAsync( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, const char **packageIdentifiers, SIZE_T packageIdentifiersCount, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p.\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );

    if (!storeContextHandle || !async || (packageIdentifiersCount && !packageIdentifiers)) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL, x_store_XStoreQueryPackageUpdatesAsync, "XStoreQueryPackageUpdatesAsync", no_updates_provider );
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, count %p.\n", iface, async, count );

    if (!count) return E_INVALIDARG;
    if (FAILED(hr = IXThreadingImpl_XAsyncGetStatus( x_threading_impl, async, FALSE ))) return hr;
    *count = 0;
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStorePackageUpdate *packageUpdates )
{
    TRACE( "iface %p, async %p, count %u, packageUpdates %p.\n", iface, async, count, packageUpdates );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreQueryPackageUpdatesAsync, 0, NULL, NULL );
}

static HRESULT WINAPI x_store_XStoreShowGiftingUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, const char *name, const char *extendedJsonData, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, storeId %s, name %s, extendedJsonData %s, async %p.\n", iface, storeContextHandle, debugstr_a( storeId ), debugstr_a( name ), debugstr_a( extendedJsonData ), async );
    return begin_store_ui( async, storeId, x_store_XStoreShowGiftingUIAsync, "XStoreShowGiftingUIAsync" );
}

static HRESULT WINAPI x_store_XStoreShowGiftingUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_store_XStoreShowGiftingUIAsync, 0, NULL, NULL );
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

// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
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

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "private.h"
#include "async_private.h"
#include "taskqueue_private.h"
#include "title_bridge.h"

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

/* A store context binds the account it was created for; handles are validated
 * against this registry before use. Store queries themselves are not
 * implemented yet and report E_NOTIMPL. */
struct store_context
{
    struct store_context *next;
    UINT64 xuid;
};
static SRWLOCK store_lock = SRWLOCK_INIT;
static struct store_context *contexts;

static BOOL store_context_valid( XStoreContextHandle handle )
{
    struct store_context *c;
    AcquireSRWLockShared( &store_lock );
    for (c = contexts; c && (XStoreContextHandle)c != handle; c = c->next) {}
    ReleaseSRWLockShared( &store_lock );
    return c != NULL;
}

/* Entitlements come from Microsoft Store Collections through the Xodus
 * service (title bridge ops 4 and 5): only the user's active entitlements for
 * this title, matched to catalog products. Entries carry the real collection
 * data; prices are not reported for owned products, and media lists are empty. */
struct entitlement
{
    char product[33], sku[33], kind[24], status[16];
    INT64 acquired, start, end;
    UINT32 quantity, trial, trial_seconds;
    char *offer_token, *dev_offer;
};

static BOOL read_text( const unsigned char **p, const unsigned char *end, char *out, SIZE_T size )
{
    const char *text;
    UINT32 length;
    if (!get_str( p, end, &text, &length ) || length >= size) return FALSE;
    memcpy( out, text, length );
    out[length] = 0;
    return TRUE;
}

static char *dup_text( const unsigned char **p, const unsigned char *end )
{
    const char *text;
    UINT32 length;
    char *out;
    if (!get_str( p, end, &text, &length ) || !(out = malloc( length + 1 ))) return NULL;
    memcpy( out, text, length );
    out[length] = 0;
    return out;
}

static BOOL read_entitlement( const unsigned char **p, const unsigned char *end, struct entitlement *e )
{
    memset( e, 0, sizeof(*e) );
    if (!read_text( p, end, e->product, sizeof(e->product) ) || !read_text( p, end, e->sku, sizeof(e->sku) ) ||
        !read_text( p, end, e->kind, sizeof(e->kind) ) || !read_text( p, end, e->status, sizeof(e->status) ) ||
        end - *p < 36) return FALSE;
    memcpy( &e->acquired, *p, 8 );
    memcpy( &e->start, *p + 8, 8 );
    memcpy( &e->end, *p + 16, 8 );
    *p += 24;
    get_u32( p, end, &e->quantity );
    get_u32( p, end, &e->trial );
    get_u32( p, end, &e->trial_seconds );
    if (!(e->offer_token = dup_text( p, end ))) return FALSE;
    if (!(e->dev_offer = dup_text( p, end ))) { free( e->offer_token ); e->offer_token = NULL; return FALSE; }
    return TRUE;
}

static void free_entitlement( struct entitlement *e )
{
    free( e->offer_token );
    free( e->dev_offer );
}

/* Collections uses year 9999 for "never expires"; a 32-bit time_t (i386
 * builds) saturates instead of wrapping. Missing dates are 0. */
static time_t store_time( INT64 value )
{
    if (value <= 0) return 0;
    if (sizeof(time_t) < sizeof(INT64) && value > 0x7fffffff) return 0x7fffffff;
    return value;
}

static XStoreProductKind product_kind( const char *kind )
{
    if (!strcmp( kind, "Game" )) return XStoreProductKind_Game;
    if (!strcmp( kind, "Durable" )) return XStoreProductKind_Durable;
    if (!strcmp( kind, "Consumable" )) return XStoreProductKind_Consumable;
    if (!strcmp( kind, "UnmanagedConsumable" )) return XStoreProductKind_UnmanagedConsumable;
    if (!strcmp( kind, "Pass" )) return XStoreProductKind_Pass;
    return XStoreProductKind_None;
}

static HRESULT WINAPI x_store_XStoreCreateContext( IXStoreImpl6 *iface, const XUserHandle user, XStoreContextHandle *storeContextHandle )
{
    struct store_context *c;
    UINT64 xuid;
    HRESULT hr;
    if (!storeContextHandle) return E_POINTER;
    *storeContextHandle = NULL;
    if (FAILED(hr = user_account( user, &xuid ))) return hr;
    if (!(c = calloc( 1, sizeof(*c) ))) return E_OUTOFMEMORY;
    c->xuid = xuid;
    AcquireSRWLockExclusive( &store_lock );
    c->next = contexts;
    contexts = c;
    ReleaseSRWLockExclusive( &store_lock );
    *storeContextHandle = c;
    TRACE( "user %p context %p.\n", user, c );
    return S_OK;
}

static void WINAPI x_store_XStoreCloseContextHandle( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle )
{
    struct store_context **link, *c;
    AcquireSRWLockExclusive( &store_lock );
    for (link = &contexts; *link && (XStoreContextHandle)*link != storeContextHandle; link = &(*link)->next) {}
    if ((c = *link)) *link = c->next;
    ReleaseSRWLockExclusive( &store_lock );
    free( c );
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

/* Product queries: an immutable, reference-counted product set; each query
 * handle is one page of it. */
struct product_set
{
    LONG refs;
    UINT32 count;
    XStoreProduct *products;
};
struct product_query
{
    struct product_query *next;
    struct product_set *set;
    UINT32 first, page;
};
static struct product_query *queries;

static void free_product( XStoreProduct *p )
{
    UINT32 i;
    for (i = 0; i < p->skusCount; ++i)
    {
        free( (char *)p->skus[i].skuId );
        free( (char *)p->skus[i].title );
        free( (char *)p->skus[i].collectionData.developerOfferId );
    }
    free( p->skus );
    free( (char *)p->storeId );
    free( (char *)p->title );
    free( (char *)p->description );
    free( (char *)p->language );
    free( (char *)p->inAppOfferToken );
    free( p->linkUri );
}

static void release_set( struct product_set *set )
{
    UINT32 i;
    if (InterlockedDecrement( &set->refs )) return;
    for (i = 0; i < set->count; ++i) free_product( &set->products[i] );
    free( set->products );
    free( set );
}

static HRESULT parse_products( const unsigned char *p, const unsigned char *end, struct product_set **out )
{
    static const char empty[] = "";
    struct product_set *set;
    struct entitlement e;
    UINT32 count, skus, downloads, i, j;
    XStoreProduct *product;
    XStoreSku *sku;
    if (!get_u32( &p, end, &count ) || count > 4096) return E_GAMEUSER_FAILED_TO_GET_TOKEN;
    if (!(set = calloc( 1, sizeof(*set) )) || (count && !(set->products = calloc( count, sizeof(*set->products) ))))
    {
        free( set );
        return E_OUTOFMEMORY;
    }
    set->refs = 1;
    for (i = 0; i < count; ++i, ++set->count)
    {
        char kind[24];
        product = &set->products[i];
        if (!(product->storeId = dup_text( &p, end )) || !(product->title = dup_text( &p, end )) ||
            !(product->description = dup_text( &p, end )) || !(product->language = dup_text( &p, end )) ||
            !read_text( &p, end, kind, sizeof(kind) ) || !get_u32( &p, end, &downloads ) ||
            !get_u32( &p, end, &skus ) || skus > 256 || !(product->linkUri = calloc( 1, 1 )) ||
            (skus && !(product->skus = calloc( skus, sizeof(*product->skus) ))))
            goto failed;
        product->productKind = product_kind( kind );
        product->price.currencyCode = empty;
        product->hasDigitalDownload = !!downloads;
        product->isInUserCollection = TRUE;
        for (j = 0; j < skus; ++j)
        {
            /* Counted before filling so cleanup frees partially read SKUs. */
            sku = &product->skus[product->skusCount++];
            if (!(sku->skuId = dup_text( &p, end ))) goto failed;
            if (!(sku->title = dup_text( &p, end ))) goto failed;
            if (!read_entitlement( &p, end, &e )) goto failed;
            sku->description = empty;
            sku->language = product->language;
            sku->price.currencyCode = empty;
            sku->isTrial = !!e.trial;
            sku->isInUserCollection = TRUE;
            sku->isSubscription = product->productKind == XStoreProductKind_Pass;
            sku->collectionData.acquiredDate = store_time( e.acquired );
            sku->collectionData.startDate = store_time( e.start );
            sku->collectionData.endDate = store_time( e.end );
            sku->collectionData.isTrial = !!e.trial;
            sku->collectionData.trialTimeRemainingInSeconds = e.trial ? e.trial_seconds : 0;
            sku->collectionData.quantity = e.quantity;
            sku->collectionData.campaignId = empty;
            sku->collectionData.developerOfferId = e.dev_offer;
            if (!product->inAppOfferToken) product->inAppOfferToken = e.offer_token;
            else free( e.offer_token );
        }
        if (!product->inAppOfferToken && !(product->inAppOfferToken = calloc( 1, 1 ))) goto failed;
    }
    if (p != end) goto invalid;
    *out = set;
    return S_OK;
failed:
    ++set->count; /* free the partially built product too */
invalid:
    release_set( set );
    return E_GAMEUSER_FAILED_TO_GET_TOKEN;
}

static HRESULT new_query( struct product_set *set, UINT32 first, UINT32 page, XStoreProductQueryHandle *out )
{
    struct product_query *q;
    if (!(q = calloc( 1, sizeof(*q) ))) return E_OUTOFMEMORY;
    InterlockedIncrement( &set->refs );
    q->set = set;
    q->first = first;
    q->page = page;
    AcquireSRWLockExclusive( &store_lock );
    q->next = queries;
    queries = q;
    ReleaseSRWLockExclusive( &store_lock );
    *out = q;
    return S_OK;
}

static struct product_query *find_query( XStoreProductQueryHandle handle )
{
    struct product_query *q;
    for (q = queries; q && (XStoreProductQueryHandle)q != handle; q = q->next) {}
    return q;
}

struct products_context
{
    struct writer tail;
    struct title_bridge_params params;
    struct product_set *set;
    UINT32 first, page;
};
static const char products_identity;

static void products_free( void *context )
{
    struct products_context *c = context;
    if (c->set) release_set( c->set );
    free( c->tail.data );
    free( c );
}

static HRESULT WINAPI products_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct products_context *c = data->context;
    unsigned char *response;
    UINT32 size;
    HRESULT hr;
    switch (op)
    {
    case XAsyncOp_Begin:
        return async_schedule( data->async, 0 );
    case XAsyncOp_DoWork:
        if (c->set) hr = S_OK; /* next page of an existing set */
        else if (SUCCEEDED(hr = title_request( TITLE_OP_ENTITLED_PRODUCTS, &c->tail, &c->params, &response, &size )))
        {
            hr = parse_products( response, response + size, &c->set );
            free( response );
        }
        if (InterlockedCompareExchange( &c->params.canceled, 0, 0 )) hr = E_ABORT;
        TRACE( "products async %p hr %#lx count %u.\n", data->async, hr, SUCCEEDED(hr) ? c->set->count : 0 );
        async_complete( data->async, hr, SUCCEEDED(hr) ? sizeof(XStoreProductQueryHandle) : 0 );
        return E_PENDING;
    case XAsyncOp_GetResult:
        return new_query( c->set, c->first, c->page, data->buffer );
    case XAsyncOp_Cancel:
        InterlockedExchange( &c->params.canceled, 1 );
        async_complete( data->async, E_ABORT, 0 );
        return S_OK;
    case XAsyncOp_Cleanup:
        products_free( c );
        return S_OK;
    }
    return E_NOTIMPL;
}

static HRESULT products_result( XAsyncBlock *async, XStoreProductQueryHandle *handle )
{
    if (!handle) return E_POINTER;
    *handle = NULL;
    return async_result( async, &products_identity, sizeof(*handle), handle, NULL );
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    struct products_context *c;
    char store_id[17];
    HRESULT hr;
    if (!async || !maxItemsToRetrievePerPage) return E_INVALIDARG;
    if (!store_context_valid( storeContextHandle )) return E_INVALIDARG;
    if (productKinds & ~(XStoreProductKind_Consumable | XStoreProductKind_Durable | XStoreProductKind_Game |
                         XStoreProductKind_Pass | XStoreProductKind_UnmanagedConsumable)) return E_INVALIDARG;
    if (FAILED(hr = game_store_id( store_id, sizeof(store_id) ))) return hr;
    if (!(c = calloc( 1, sizeof(*c) ))) return E_OUTOFMEMORY;
    c->page = maxItemsToRetrievePerPage;
    put_str( &c->tail, store_id, strlen( store_id ) );
    put_u32( &c->tail, productKinds );
    if (c->tail.failed) { products_free( c ); return E_OUTOFMEMORY; }
    return async_begin_owned( async, c, &products_identity, products_provider, products_free );
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    return products_result( async, productQueryHandle );
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
    struct product_query *q;
    struct product_set *set;
    UINT32 i, last;
    if (!callback) return E_INVALIDARG;
    AcquireSRWLockShared( &store_lock );
    if ((q = find_query( productQueryHandle )))
    {
        set = q->set;
        InterlockedIncrement( &set->refs );
        i = q->first;
        last = min( set->count, q->first + q->page );
    }
    ReleaseSRWLockShared( &store_lock );
    if (!q) return E_INVALIDARG;
    /* Callbacks run without locks; the set reference keeps products alive. */
    for (; i < last; ++i) if (!callback( &set->products[i], context )) break;
    release_set( set );
    return S_OK;
}

static BOOLEAN WINAPI x_store_XStoreProductsQueryHasMorePages( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle )
{
    struct product_query *q;
    BOOLEAN more = FALSE;
    AcquireSRWLockShared( &store_lock );
    if ((q = find_query( productQueryHandle ))) more = q->first + q->page < q->set->count;
    ReleaseSRWLockShared( &store_lock );
    return more;
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageAsync( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, XAsyncBlock *async )
{
    struct products_context *c;
    struct product_query *q;
    if (!async) return E_INVALIDARG;
    if (!(c = calloc( 1, sizeof(*c) ))) return E_OUTOFMEMORY;
    AcquireSRWLockShared( &store_lock );
    if ((q = find_query( productQueryHandle )) && q->first + q->page < q->set->count)
    {
        c->set = q->set;
        InterlockedIncrement( &c->set->refs );
        c->first = q->first + q->page;
        c->page = q->page;
    }
    ReleaseSRWLockShared( &store_lock );
    if (!c->set) { free( c ); return E_INVALIDARG; }
    return async_begin_owned( async, c, &products_identity, products_provider, products_free );
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    return products_result( async, productQueryHandle );
}

static void WINAPI x_store_XStoreCloseProductsQueryHandle( IXStoreImpl6 *iface, XStoreProductQueryHandle productQueryHandle )
{
    struct product_query **link, *q;
    AcquireSRWLockExclusive( &store_lock );
    for (link = &queries; *link && (XStoreProductQueryHandle)*link != productQueryHandle; link = &(*link)->next) {}
    if ((q = *link)) *link = q->next;
    ReleaseSRWLockExclusive( &store_lock );
    if (!q) return;
    release_set( q->set );
    free( q );
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

/* XStoreQueryGameLicense: the title's own Store product from Collections.
 * The license is the active, non-expired entitlement (non-trial first, then
 * the lowest SKU); with none, isActive is FALSE. */
struct license_context { struct writer tail; struct title_bridge_params params; XStoreGameLicense license; };
static const char license_identity;

static void license_free( void *context )
{
    struct license_context *c = context;
    free( c->tail.data );
    free( c );
}

static HRESULT parse_game_license( struct license_context *c, const char *store_id, const unsigned char *p, const unsigned char *end )
{
    struct entitlement e, best = {{0}};
    INT64 now = time( NULL );
    UINT32 count, i;
    BOOL found = FALSE, any = FALSE;
    if (!get_u32( &p, end, &count ) || count > 1024) return E_GAMEUSER_FAILED_TO_GET_TOKEN;
    memset( &c->license, 0, sizeof(c->license) );
    for (i = 0; i < count; ++i)
    {
        if (!read_entitlement( &p, end, &e )) return E_GAMEUSER_FAILED_TO_GET_TOKEN;
        if (!strcmp( e.product, store_id ) && !strcmp( e.kind, "Game" ))
        {
            BOOL active = !strcmp( e.status, "Active" ) && e.start <= now && now < e.end;
            if (!any && strlen( e.sku ) <= 4) snprintf( c->license.skuStoreId, sizeof(c->license.skuStoreId), "%s/%s", e.product, e.sku );
            any = TRUE;
            if (active && strlen( e.sku ) <= 4 &&
                (!found || (best.trial && !e.trial) || (best.trial == e.trial && strcmp( e.sku, best.sku ) < 0)))
            {
                free_entitlement( &best );
                best = e;
                found = TRUE;
                continue;
            }
        }
        free_entitlement( &e );
    }
    if (p != end) { free_entitlement( &best ); return E_GAMEUSER_FAILED_TO_GET_TOKEN; }
    if (found)
    {
        snprintf( c->license.skuStoreId, sizeof(c->license.skuStoreId), "%s/%s", best.product, best.sku );
        c->license.isActive = TRUE;
        c->license.isTrial = c->license.isTrialOwnedByThisUser = !!best.trial;
        c->license.trialTimeRemainingInSeconds = best.trial ? best.trial_seconds : 0;
        c->license.expirationDate = store_time( best.end );
        free_entitlement( &best );
    }
    return S_OK;
}

static HRESULT WINAPI license_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct license_context *c = data->context;
    unsigned char *response;
    char store_id[17];
    UINT32 size;
    HRESULT hr;
    switch (op)
    {
    case XAsyncOp_Begin:
        return async_schedule( data->async, 0 );
    case XAsyncOp_DoWork:
        if (SUCCEEDED(hr = game_store_id( store_id, sizeof(store_id) )) &&
            SUCCEEDED(hr = title_request( TITLE_OP_COLLECTIONS, &c->tail, &c->params, &response, &size )))
        {
            hr = parse_game_license( c, store_id, response, response + size );
            free( response );
        }
        if (InterlockedCompareExchange( &c->params.canceled, 0, 0 )) hr = E_ABORT;
        TRACE( "game license async %p hr %#lx active %u.\n", data->async, hr, SUCCEEDED(hr) && c->license.isActive );
        async_complete( data->async, hr, SUCCEEDED(hr) ? sizeof(XStoreGameLicense) : 0 );
        return E_PENDING;
    case XAsyncOp_GetResult:
        memcpy( data->buffer, &c->license, sizeof(c->license) );
        return S_OK;
    case XAsyncOp_Cancel:
        InterlockedExchange( &c->params.canceled, 1 );
        async_complete( data->async, E_ABORT, 0 );
        return S_OK;
    case XAsyncOp_Cleanup:
        license_free( c );
        return S_OK;
    }
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryGameLicenseAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    struct license_context *c;
    char store_id[17];
    HRESULT hr;
    if (!async) return E_INVALIDARG;
    if (!store_context_valid( storeContextHandle )) return E_INVALIDARG;
    if (FAILED(hr = game_store_id( store_id, sizeof(store_id) ))) return hr;
    if (!(c = calloc( 1, sizeof(*c) ))) return E_OUTOFMEMORY;
    put_u32( &c->tail, 1 );
    put_str( &c->tail, store_id, strlen( store_id ) );
    put_u32( &c->tail, XStoreProductKind_Game );
    put_u32( &c->tail, 0 );
    if (c->tail.failed) { license_free( c ); return E_OUTOFMEMORY; }
    return async_begin_owned( async, c, &license_identity, license_provider, license_free );
}

static HRESULT WINAPI x_store_XStoreQueryGameLicenseResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreGameLicense *license )
{
    if (!license) return E_POINTER;
    memset( license, 0, sizeof(*license) );
    return async_result( async, &license_identity, sizeof(*license), license, NULL );
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

/* XStoreQueryLicenseToken: a signed token from Microsoft's licensing service
 * for this title (parent) and the requested products, carrying the title's
 * developer string, through the Xodus service (title bridge op 6). */
#define MAX_LICENSE_PRODUCTS 64
struct license_token_context { struct writer tail; struct title_bridge_params params; char *token; SIZE_T size; };
static const char license_token_identity;

static void license_token_free( void *context )
{
    struct license_token_context *c = context;
    free( c->token );
    free( c->tail.data );
    free( c );
}

static HRESULT WINAPI license_token_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct license_token_context *c = data->context;
    const unsigned char *p;
    unsigned char *response;
    UINT32 size;
    HRESULT hr;
    switch (op)
    {
    case XAsyncOp_Begin:
        return async_schedule( data->async, 0 );
    case XAsyncOp_DoWork:
        if (SUCCEEDED(hr = title_request( TITLE_OP_LICENSE_TOKEN, &c->tail, &c->params, &response, &size )))
        {
            p = response;
            if (!(c->token = dup_text( &p, response + size )) || p != response + size || !c->token[0])
                hr = E_GAMEUSER_FAILED_TO_GET_TOKEN;
            else c->size = strlen( c->token ) + 1;
            free( response );
        }
        if (InterlockedCompareExchange( &c->params.canceled, 0, 0 )) hr = E_ABORT;
        TRACE( "license token async %p hr %#lx.\n", data->async, hr );
        async_complete( data->async, hr, SUCCEEDED(hr) ? c->size : 0 );
        return E_PENDING;
    case XAsyncOp_GetResult:
        memcpy( data->buffer, c->token, c->size );
        return S_OK;
    case XAsyncOp_Cancel:
        InterlockedExchange( &c->params.canceled, 1 );
        async_complete( data->async, E_ABORT, 0 );
        return S_OK;
    case XAsyncOp_Cleanup:
        license_token_free( c );
        return S_OK;
    }
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **productIds, SIZE_T productIdsCount, const char *customDeveloperString, XAsyncBlock *async )
{
    struct license_token_context *c;
    char store_id[17];
    SIZE_T i, length;
    HRESULT hr;
    if (!async || !customDeveloperString || (productIdsCount && !productIds) || productIdsCount > MAX_LICENSE_PRODUCTS)
        return E_INVALIDARG;
    if (!store_context_valid( storeContextHandle )) return E_INVALIDARG;
    for (i = 0; i < productIdsCount; ++i)
        if (!productIds[i] || !(length = strlen( productIds[i] )) || length > 16) return E_INVALIDARG;
    if (FAILED(hr = game_store_id( store_id, sizeof(store_id) ))) return hr;
    if (!(c = calloc( 1, sizeof(*c) ))) return E_OUTOFMEMORY;
    put_str( &c->tail, store_id, strlen( store_id ) );
    put_u32( &c->tail, productIdsCount );
    for (i = 0; i < productIdsCount; ++i) put_str( &c->tail, productIds[i], strlen( productIds[i] ) );
    put_str( &c->tail, customDeveloperString, strlen( customDeveloperString ) );
    if (c->tail.failed) { license_token_free( c ); return E_INVALIDARG; }
    return async_begin_owned( async, c, &license_token_identity, license_token_provider, license_token_free );
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    if (!size) return E_POINTER;
    return async_size( async, size );
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    if (!result) return E_POINTER;
    return async_result( async, &license_token_identity, size, result, NULL );
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

static HRESULT WINAPI x_store_XStoreShowPurchaseUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, const char *name, const char *extendedJsonData, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, name %s, extendedJsonData %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), debugstr_a( name ), debugstr_a( extendedJsonData ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowPurchaseUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
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

/* XStoreQueryGameAndDlcPackageUpdates: the installed base package version
 * (MicrosoftGame.Config Identity Version) against the latest base package in
 * Microsoft's update service, through the Xodus service (title bridge op 7).
 * DLC packages are not installed separately here, so only the game is checked.
 * The service does not say whether an update is mandatory; isMandatory is
 * FALSE. The package identifier is the content ID without dashes (32 hex
 * characters); this identifier format is an assumption. */
struct updates_context { struct writer tail; struct title_bridge_params params; UINT32 count; XStorePackageUpdate update; };
static const char updates_identity;

static void updates_free( void *context )
{
    struct updates_context *c = context;
    free( c->tail.data );
    free( c );
}

static HRESULT parse_updates( struct updates_context *c, const unsigned char *p, const unsigned char *end )
{
    char latest[32], content[64];
    UINT32 state, i, n = 0;
    if (!get_u32( &p, end, &state ) || state > 2 || !read_text( &p, end, latest, sizeof(latest) ) ||
        !read_text( &p, end, content, sizeof(content) ) || p != end)
        return E_GAMEUSER_FAILED_TO_GET_TOKEN;
    TRACE( "installed package state %u latest %s.\n", state, debugstr_a( latest ) );
    memset( &c->update, 0, sizeof(c->update) );
    c->count = 0;
    if (state != 1) return S_OK;
    for (i = 0; content[i] && n < sizeof(c->update.packageIdentifier) - 1; ++i)
        if (content[i] != '-') c->update.packageIdentifier[n++] = content[i];
    if (content[i]) return E_GAMEUSER_FAILED_TO_GET_TOKEN;
    c->update.isMandatory = FALSE;
    c->count = 1;
    return S_OK;
}

static HRESULT WINAPI updates_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct updates_context *c = data->context;
    unsigned char *response;
    UINT32 size;
    HRESULT hr;
    switch (op)
    {
    case XAsyncOp_Begin:
        return async_schedule( data->async, 0 );
    case XAsyncOp_DoWork:
        if (SUCCEEDED(hr = title_request( TITLE_OP_PACKAGE_UPDATE, &c->tail, &c->params, &response, &size )))
        {
            hr = parse_updates( c, response, response + size );
            free( response );
        }
        if (InterlockedCompareExchange( &c->params.canceled, 0, 0 )) hr = E_ABORT;
        async_complete( data->async, hr, SUCCEEDED(hr) ? c->count * sizeof(XStorePackageUpdate) : 0 );
        return E_PENDING;
    case XAsyncOp_GetResult:
        if (c->count) memcpy( data->buffer, &c->update, sizeof(c->update) );
        return S_OK;
    case XAsyncOp_Cancel:
        InterlockedExchange( &c->params.canceled, 1 );
        async_complete( data->async, E_ABORT, 0 );
        return S_OK;
    case XAsyncOp_Cleanup:
        updates_free( c );
        return S_OK;
    }
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    struct updates_context *c;
    char store_id[17], version[24];
    HRESULT hr;
    if (!async) return E_INVALIDARG;
    if (!store_context_valid( storeContextHandle )) return E_INVALIDARG;
    if (FAILED(hr = game_store_id( store_id, sizeof(store_id) ))) return hr;
    if (FAILED(hr = game_package_version( version, sizeof(version) ))) return hr;
    if (!(c = calloc( 1, sizeof(*c) ))) return E_OUTOFMEMORY;
    put_str( &c->tail, store_id, strlen( store_id ) );
    put_str( &c->tail, version, strlen( version ) );
    if (c->tail.failed) { updates_free( c ); return E_OUTOFMEMORY; }
    return async_begin_owned( async, c, &updates_identity, updates_provider, updates_free );
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    SIZE_T size = 0;
    HRESULT hr;
    if (!count) return E_POINTER;
    *count = 0;
    /* Zero updates complete without a payload and are released at completion. */
    if (FAILED(hr = async_status( async, FALSE ))) return hr;
    if (SUCCEEDED(async_size( async, &size ))) *count = size / sizeof(XStorePackageUpdate);
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStorePackageUpdate *packageUpdates )
{
    SIZE_T size = 0;
    HRESULT hr;
    if (count && !packageUpdates) return E_POINTER;
    if (FAILED(hr = async_status( async, FALSE ))) return hr;
    if (FAILED(async_size( async, &size )) || !size) return count ? E_INVALIDARG : S_OK;
    if (count != size / sizeof(XStorePackageUpdate)) return E_INVALIDARG;
    return async_result( async, &updates_identity, size, packageUpdates, NULL );
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

/* The game license does not change during a session here: the registration
 * is real, but no license-changed event is ever raised. */
static HRESULT WINAPI x_store_XStoreRegisterGameLicenseChanged( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, XTaskQueueHandle queue, void *context, XStoreGameLicenseChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    HRESULT hr;
    if (token) token->token = 0;
    if (!store_context_valid( storeContextHandle )) return E_INVALIDARG;
    if (!runtime_acquire()) return E_GAMERUNTIME_NOT_INITIALIZED;
    hr = eventless_register( EVENTLESS_GAME_LICENSE, queue, callback, token );
    runtime_release();
    TRACE( "context %p queue %p hr %#lx.\n", storeContextHandle, queue, hr );
    return hr;
}

static BOOLEAN WINAPI x_store_XStoreUnregisterGameLicenseChanged( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "context %p token %s wait %d.\n", storeContextHandle, wine_dbgstr_longlong( token.token ), wait );
    return eventless_unregister( EVENTLESS_GAME_LICENSE, token );
}

static HRESULT WINAPI x_store_XStoreRegisterPackageLicenseLost( IXStoreImpl6 *iface, XStoreLicenseHandle storeLicenseHandle, XTaskQueueHandle queue, void *context, XStorePackageLicenseLostCallback *callback, XTaskQueueRegistrationToken *token)
{
    FIXME( "iface %p, storeLicenseHandle %p, queue %p, context %p, callback %p, token %p stub!\n", iface, storeLicenseHandle, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_store_XStoreUnregisterPackageLicenseLost( IXStoreImpl6 *iface, XStoreLicenseHandle licenseHandle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, licenseHandle %p, token %p, wait %d stub!\n", iface, licenseHandle, &token, wait );
    return FALSE;
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
    FIXME( "iface %p, storeContextHandle %p, storeId %s, productKinds %#x, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), productKinds, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowAssociatedProductsUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowProductPageUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowProductPageUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
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

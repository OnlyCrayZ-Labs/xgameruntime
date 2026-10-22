// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/*
 * Xbox Game runtime Library
 *  GDK Component: Networking API -> XNetworking
 *
 * Written by Weather
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
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include "private.h"
#include "taskqueue_private.h"
#include "networking_state.h"
#include "async_private.h"
#include "title_bridge.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_networking
{
    IXNetworkingImpl2 IXNetworkingImpl2_iface;
    LONG ref;
};

static inline struct x_networking *impl_from_IXNetworkingImpl2( IXNetworkingImpl2 *iface )
{
    return CONTAINING_RECORD( iface, struct x_networking, IXNetworkingImpl2_iface );
}

static HRESULT WINAPI x_networking_QueryInterface( IXNetworkingImpl2 *iface, REFIID iid, void **out )
{
    struct x_networking *impl = impl_from_IXNetworkingImpl2( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown          ) ||
        IsEqualGUID( iid, &IID_IXNetworkingImpl  ) ||
        IsEqualGUID( iid, &IID_IXNetworkingImpl2 ))
    {
        IXNetworkingImpl_AddRef( *out = &impl->IXNetworkingImpl2_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_networking_AddRef( IXNetworkingImpl2 *iface )
{
    struct x_networking *impl = impl_from_IXNetworkingImpl2( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_networking_Release( IXNetworkingImpl2 *iface )
{
    struct x_networking *impl = impl_from_IXNetworkingImpl2( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/* The preferred local UDP multiplayer port is the documented default, UDP
 * 3074. Windows falls back automatically when that port is unsuitable or
 * the user overrides it in the Xbox network troubleshooter; neither exists
 * here, so the port never changes and no change event is raised. Nothing
 * is bound or forwarded (no UPnP); as documented, a title must still handle
 * connections that fail on the port. */
#define PREFERRED_UDP_PORT 3074
static const char udp_port_identity;

static HRESULT WINAPI udp_port_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    switch (op)
    {
    case XAsyncOp_Begin:
        return async_schedule( data->async, 0 );
    case XAsyncOp_DoWork:
        async_complete( data->async, S_OK, sizeof(UINT16) );
        return E_PENDING;
    case XAsyncOp_GetResult:
        *(UINT16 *)data->buffer = PREFERRED_UDP_PORT;
        return S_OK;
    case XAsyncOp_Cancel:
        async_complete( data->async, E_ABORT, 0 );
        return S_OK;
    case XAsyncOp_Cleanup:
        return S_OK;
    }
    return E_NOTIMPL;
}

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPort( IXNetworkingImpl2 *iface, UINT16 *preferredLocalUdpMultiplayerPort )
{
    TRACE( "iface %p, preferredLocalUdpMultiplayerPort %p.\n", iface, preferredLocalUdpMultiplayerPort );
    if (!preferredLocalUdpMultiplayerPort) return E_POINTER;
    *preferredLocalUdpMultiplayerPort = PREFERRED_UDP_PORT;
    return S_OK;
}

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock )
{
    TRACE( "iface %p, asyncBlock %p.\n", iface, asyncBlock );
    if (!asyncBlock) return E_INVALIDARG;
    return async_begin( asyncBlock, NULL, &udp_port_identity, udp_port_provider );
}

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, UINT16 *preferredLocalUdpMultiplayerPort )
{
    UINT16 port;
    HRESULT hr;
    TRACE( "iface %p, asyncBlock %p, preferredLocalUdpMultiplayerPort %p.\n", iface, asyncBlock, preferredLocalUdpMultiplayerPort );
    if (!preferredLocalUdpMultiplayerPort) return E_POINTER;
    if (SUCCEEDED(hr = async_result( asyncBlock, &udp_port_identity, sizeof(port), &port, NULL )))
        *preferredLocalUdpMultiplayerPort = port;
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged( IXNetworkingImpl2 *iface, XTaskQueueHandle queue, void *context, XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    HRESULT hr;
    if (token) token->token = 0;
    if (!runtime_acquire()) return E_GAMERUNTIME_NOT_INITIALIZED;
    hr = eventless_register( EVENTLESS_UDP_PORT, queue, callback, token );
    runtime_release();
    TRACE( "queue %p hr %#lx.\n", queue, hr );
    return hr;
}

static BOOLEAN WINAPI x_networking_XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged( IXNetworkingImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "token %s wait %d.\n", wine_dbgstr_longlong( token.token ), wait );
    return eventless_unregister( EVENTLESS_UDP_PORT, token );
}

/* Security information comes from Microsoft's title endpoint list through the
 * Xodus service: TLS protocol flags and the certificate thumbprints Microsoft
 * pins for the URL's endpoint (none for unlisted URLs). */
#define MAX_THUMBPRINTS 16
struct security_context
{
    struct writer tail;
    struct title_bridge_params params;
    UINT32 flags, count;
    XNetworkingThumbprintType types[MAX_THUMBPRINTS];
    UINT32 lengths[MAX_THUMBPRINTS];
    UINT8 prints[MAX_THUMBPRINTS][64];
};
static const char security_identity, security16_identity;

static SIZE_T security_size(const struct security_context *c)
{
    SIZE_T size = sizeof(XNetworkingSecurityInformation) + c->count * sizeof(XNetworkingThumbprint);
    UINT32 i;
    for (i = 0; i < c->count; ++i) size += c->lengths[i];
    return size;
}
static void security_free(void *context)
{
    struct security_context *c = context;
    free(c->tail.data);
    free(c);
}
static HRESULT parse_security(struct security_context *c, const unsigned char *p, const unsigned char *end)
{
    const char *bytes;
    UINT32 i, type;
    if (!get_u32(&p, end, &c->flags) || !get_u32(&p, end, &c->count) || c->count > MAX_THUMBPRINTS)
        return E_GAMEUSER_FAILED_TO_GET_TOKEN;
    for (i = 0; i < c->count; ++i)
    {
        if (!get_u32(&p, end, &type) || type > XNetworkingThumbprintType_Root) return E_GAMEUSER_FAILED_TO_GET_TOKEN;
        if (end - p < 4) return E_GAMEUSER_FAILED_TO_GET_TOKEN;
        memcpy(&c->lengths[i], p, 4);
        p += 4;
        if (!c->lengths[i] || c->lengths[i] > sizeof(c->prints[i]) || (SIZE_T)(end - p) < c->lengths[i])
            return E_GAMEUSER_FAILED_TO_GET_TOKEN;
        bytes = (const char *)p;
        memcpy(c->prints[i], bytes, c->lengths[i]);
        p += c->lengths[i];
        c->types[i] = type;
    }
    return p == end ? S_OK : E_GAMEUSER_FAILED_TO_GET_TOKEN;
}
static HRESULT WINAPI security_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    struct security_context *c = data->context;
    XNetworkingSecurityInformation *info;
    XNetworkingThumbprint *prints;
    unsigned char *response, *bytes;
    UINT32 size, i;
    HRESULT hr;
    switch (op)
    {
    case XAsyncOp_Begin:
        return async_schedule(data->async, 0);
    case XAsyncOp_DoWork:
        if (SUCCEEDED(hr = title_request(TITLE_OP_SECURITY, &c->tail, &c->params, &response, &size)))
        {
            hr = parse_security(c, response, response + size);
            free(response);
        }
        if (InterlockedCompareExchange(&c->params.canceled, 0, 0)) hr = E_ABORT;
        TRACE("security async %p hr %#lx thumbprints %u.\n", data->async, hr, SUCCEEDED(hr) ? c->count : 0);
        async_complete(data->async, hr, SUCCEEDED(hr) ? security_size(c) : 0);
        return E_PENDING;
    case XAsyncOp_GetResult:
        info = data->buffer;
        prints = (XNetworkingThumbprint *)(info + 1);
        bytes = (unsigned char *)(prints + c->count);
        info->enabledHttpSecurityProtocolFlags = c->flags;
        info->thumbprintCount = c->count;
        info->thumbprints = c->count ? prints : NULL;
        for (i = 0; i < c->count; ++i)
        {
            prints[i].thumbprintType = c->types[i];
            prints[i].thumbprintBufferByteCount = c->lengths[i];
            prints[i].thumbprintBuffer = bytes;
            memcpy(bytes, c->prints[i], c->lengths[i]);
            bytes += c->lengths[i];
        }
        return S_OK;
    case XAsyncOp_Cancel:
        InterlockedExchange(&c->params.canceled, 1);
        async_complete(data->async, E_ABORT, 0);
        return S_OK;
    case XAsyncOp_Cleanup:
        security_free(c);
        return S_OK;
    }
    return E_NOTIMPL;
}
static HRESULT begin_security(const char *url, BOOL utf16, XAsyncBlock *async)
{
    struct security_context *c;
    if (!url || !async) return E_INVALIDARG;
    if (!(c = calloc(1, sizeof(*c)))) return E_OUTOFMEMORY;
    put_str(&c->tail, url, strlen(url));
    if (c->tail.failed) { security_free(c); return E_INVALIDARG; }
    return async_begin_owned(async, c, utf16 ? &security16_identity : &security_identity, security_provider, security_free);
}
static HRESULT security_result(const void *identity, XAsyncBlock *async, SIZE_T size, SIZE_T *used,
                               UINT8 *buffer, XNetworkingSecurityInformation **info)
{
    HRESULT hr;
    if (!info) return E_POINTER;
    *info = NULL;
    /* The structure and its thumbprint array are pointer-aligned. */
    if (buffer && ((ULONG_PTR)buffer & (sizeof(void *) - 1))) return E_INVALIDARG;
    if (SUCCEEDED(hr = async_result(async, identity, size, buffer, used))) *info = (XNetworkingSecurityInformation *)buffer;
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsync( IXNetworkingImpl2 *iface, const char *url, XAsyncBlock *asyncBlock )
{
    return begin_security( url, FALSE, asyncBlock );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResultSize( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T *securityInformationBufferByteCount )
{
    if (!securityInformationBufferByteCount) return E_POINTER;
    return async_size( asyncBlock, securityInformationBufferByteCount );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T securityInformationBufferByteCount, SIZE_T *securityInformationBufferByteCountUsed, UINT8 *securityInformationBuffer, XNetworkingSecurityInformation **securityInformation )
{
    return security_result( &security_identity, asyncBlock, securityInformationBufferByteCount,
                            securityInformationBufferByteCountUsed, securityInformationBuffer, securityInformation );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16Async( IXNetworkingImpl2 *iface, const WCHAR *url, XAsyncBlock *asyncBlock )
{
    char *url8 = NULL;
    HRESULT hr;
    int size;
    if (url && (size = WideCharToMultiByte( CP_UTF8, 0, url, -1, NULL, 0, NULL, NULL )) &&
        (url8 = malloc( size )) && !WideCharToMultiByte( CP_UTF8, 0, url, -1, url8, size, NULL, NULL ))
    {
        free( url8 );
        url8 = NULL;
    }
    hr = url && !url8 ? E_OUTOFMEMORY : begin_security( url8, TRUE, asyncBlock );
    free( url8 );
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T *securityInformationBufferByteCount )
{
    if (!securityInformationBufferByteCount) return E_POINTER;
    return async_size( asyncBlock, securityInformationBufferByteCount );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T securityInformationBufferByteCount, SIZE_T *securityInformationBufferByteCountUsed, UINT8 *securityInformationBuffer, XNetworkingSecurityInformation **securityInformation )
{
    return security_result( &security16_identity, asyncBlock, securityInformationBufferByteCount,
                            securityInformationBufferByteCountUsed, securityInformationBuffer, securityInformation );
}

/* With no pinned thumbprints there is nothing beyond the TLS stack's own
 * certificate validation to check. Pinned endpoints are not verified here:
 * report that rather than silently accepting the connection. */
static HRESULT WINAPI x_networking_XNetworkingVerifyServerCertificate( IXNetworkingImpl2 *iface, void *requestHandle, const XNetworkingSecurityInformation *securityInformation )
{
    if (!requestHandle || !securityInformation) return E_INVALIDARG;
    if (!securityInformation->thumbprintCount) return S_OK;
    FIXME( "pinned certificate verification for %Iu thumbprints is not implemented.\n", securityInformation->thumbprintCount );
    return E_NOTIMPL;
}

/* One timer and at most one queued notification per registration. All state
 * except atomic references is protected by networking_lock. No user code runs
 * under a lock. Queued entries own references and are suppressed after removal.
 * Unregister(wait) waits for entered user callbacks, never for a Manual port. */
struct connectivity_registration
{
    struct connectivity_registration *next;
    LONG refs;
    UINT64 token;
    XTaskQueueHandle queue;
    PTP_TIMER timer;
    void *context;
    XNetworkingConnectivityHintChangedCallback *callback;
    XNetworkingConnectivityHint latest;
    CONDITION_VARIABLE idle;
    DWORD callback_thread;
    BOOL registered, pending, delivered, stopping, stopped, polling;
};
static SRWLOCK networking_lock = SRWLOCK_INIT;
static struct connectivity_registration *registrations;
static UINT64 next_token;

static HRESULT connectivity_snapshot(XNetworkingConnectivityHint *hint)
{
    IP_ADAPTER_ADDRESSES *adapters = NULL;
    MIB_IPFORWARD_TABLE2 *routes = NULL;
    ULONG size = 0, ret;
    unsigned int retries;
    memset(hint, 0, sizeof(*hint));
    for (retries = 0; retries < 4; ++retries)
    {
        ret = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS, NULL, adapters, &size);
        if (ret != ERROR_BUFFER_OVERFLOW) break;
        free(adapters);
        if (!(adapters = malloc(size))) return E_OUTOFMEMORY;
    }
    if (!ret || ret == ERROR_NO_DATA)
    {
        if (!ret) GetIpForwardTable2(AF_UNSPEC, &routes);
        networking_reduce(ret ? NULL : adapters, routes, hint);
        if (routes) FreeMibTable(routes);
        ret = ERROR_SUCCESS;
    }
    free(adapters);
    return HRESULT_FROM_WIN32(ret);
}

static void connectivity_release(struct connectivity_registration *r)
{
    if (InterlockedDecrement(&r->refs)) return;
    task_queue_close(r->queue);
    free(r);
}

/* Drop the registry ownership once no user callback can still enter. The
 * caller holds a separate reference and releases the returned registry ref. */
static BOOL connectivity_retire_locked(struct connectivity_registration *r)
{
    struct connectivity_registration **link;
    if (r->registered || !r->stopped || r->callback_thread) return FALSE;
    for (link = &registrations; *link && *link != r; link = &(*link)->next) {}
    if (!*link) return FALSE;
    *link = r->next;
    return TRUE;
}

static void CALLBACK connectivity_dispatch(void *context, BOOLEAN canceled)
{
    struct connectivity_registration *r = context;
    XNetworkingConnectivityHint hint;
    BOOL invoke, retired;
    AcquireSRWLockExclusive(&networking_lock);
    invoke = r->registered && !canceled;
    hint = r->latest;
    if (invoke) r->callback_thread = GetCurrentThreadId();
    ReleaseSRWLockExclusive(&networking_lock);
    if (invoke)
    {
        TRACE("connectivity callback token %s level %u initialized %u.\n",
              wine_dbgstr_longlong(r->token), hint.connectivityLevel, hint.networkInitialized);
        r->callback(r->context, &hint);
    }
    AcquireSRWLockExclusive(&networking_lock);
    r->callback_thread = 0;
    r->pending = FALSE;
    retired = connectivity_retire_locked(r);
    WakeAllConditionVariable(&r->idle);
    ReleaseSRWLockExclusive(&networking_lock);
    if (retired) connectivity_release(r);
    connectivity_release(r);
}

static void CALLBACK connectivity_poll(PTP_CALLBACK_INSTANCE instance, void *context, PTP_TIMER timer)
{
    struct connectivity_registration *r = context;
    XNetworkingConnectivityHint hint;
    HRESULT hr;
    BOOL submit = FALSE;
    AcquireSRWLockExclusive(&networking_lock);
    if (!r->registered || r->pending || r->polling)
    {
        ReleaseSRWLockExclusive(&networking_lock);
        return;
    }
    r->polling = TRUE;
    ReleaseSRWLockExclusive(&networking_lock);
    hr = connectivity_snapshot(&hint);
    if (FAILED(hr)) TRACE("connectivity snapshot hr %#lx.\n", hr);
    AcquireSRWLockExclusive(&networking_lock);
    r->polling = FALSE;
    if (r->registered && !r->pending && (!r->delivered || memcmp(&hint, &r->latest, sizeof(hint))))
    {
        r->latest = hint;
        r->pending = TRUE;
        r->delivered = TRUE;
        InterlockedIncrement(&r->refs);
        submit = TRUE;
    }
    ReleaseSRWLockExclusive(&networking_lock);
    if (!submit) return;
    hr = task_queue_submit(r->queue, XTaskQueuePort_Completion, r, connectivity_dispatch);
    AcquireSRWLockExclusive(&networking_lock);
    if (FAILED(hr)) r->pending = r->delivered = FALSE;
    ReleaseSRWLockExclusive(&networking_lock);
    if (FAILED(hr))
    {
        TRACE("connectivity notification submission hr %#lx.\n", hr);
        connectivity_release(r);
    }
}

static HRESULT WINAPI x_networking_XNetworkingGetConnectivityHint(IXNetworkingImpl2 *iface,
                                                                  XNetworkingConnectivityHint *hint)
{
    HRESULT hr;
    if (!hint) hr = E_INVALIDARG;
    else
    {
        memset(hint, 0, sizeof(*hint));
        if (!runtime_acquire()) hr = E_GAMERUNTIME_NOT_INITIALIZED;
        else
        {
            hr = connectivity_snapshot(hint);
            runtime_release();
        }
    }
    TRACE("GetConnectivityHint hr %#lx level %u initialized %u interface %u.\n", hr,
          hint ? hint->connectivityLevel : 0, hint ? hint->networkInitialized : 0,
          hint ? hint->ianaInterfaceType : 0);
    return hr;
}

static HRESULT register_connectivity_hint(IXNetworkingImpl2 *iface,
        XTaskQueueHandle queue, void *context, XNetworkingConnectivityHintChangedCallback *callback,
        XTaskQueueRegistrationToken *token)
{
    struct connectivity_registration *r;
    LARGE_INTEGER due;
    HRESULT hr;
    if (token) token->token = 0;
    if (!callback || !token) return E_INVALIDARG;
    if (!runtime_acquire()) return E_GAMERUNTIME_NOT_INITIALIZED;
    if (!(r = calloc(1, sizeof(*r)))) { runtime_release(); return E_OUTOFMEMORY; }
    r->refs = 1;
    if (queue) hr = task_queue_duplicate(queue, &r->queue);
    else hr = task_queue_create(XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &r->queue);
    if (FAILED(hr)) goto failed;
    if (!(r->timer = CreateThreadpoolTimer(connectivity_poll, r, NULL)))
    { hr = HRESULT_FROM_WIN32(GetLastError()); goto failed; }
    r->context = context;
    r->callback = callback;
    AcquireSRWLockExclusive(&networking_lock);
    if (next_token == ~(UINT64)0)
    {
        ReleaseSRWLockExclusive(&networking_lock);
        CloseThreadpoolTimer(r->timer);
        hr = E_OUTOFMEMORY;
        goto failed;
    }
    token->token = r->token = ++next_token;
    r->registered = TRUE;
    r->next = registrations;
    registrations = r;
    /* Arm under the lock so unregister cannot close the timer before arming. */
    due.QuadPart = -1;
    SetThreadpoolTimer(r->timer, (FILETIME *)&due, 1000, 0);
    ReleaseSRWLockExclusive(&networking_lock);
    runtime_release();
    TRACE("RegisterConnectivityHintChanged hr 0 token %s.\n", wine_dbgstr_longlong(token->token));
    return S_OK;
failed:
    connectivity_release(r);
    runtime_release();
    TRACE("RegisterConnectivityHintChanged hr %#lx.\n", hr);
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingRegisterConnectivityHintChanged(IXNetworkingImpl2 *iface,
        XTaskQueueHandle queue, void *context, XNetworkingConnectivityHintChangedCallback *callback,
        XTaskQueueRegistrationToken *token)
{
    HRESULT hr = register_connectivity_hint(iface, queue, context, callback, token);
    TRACE("RegisterConnectivityHintChanged queue %p hr %#lx.\n", queue, hr);
    return hr;
}

static BOOLEAN WINAPI x_networking_XNetworkingUnregisterConnectivityHintChanged(IXNetworkingImpl2 *iface,
        XTaskQueueRegistrationToken token, BOOLEAN wait)
{
    struct connectivity_registration *r;
    BOOL stop, complete, retired;
    AcquireSRWLockExclusive(&networking_lock);
    for (r = registrations; r && r->token != token.token; r = r->next) {}
    /* Already completed tokens can be polled repeatedly, as documented. */
    if (!r) { ReleaseSRWLockExclusive(&networking_lock); return TRUE; }
    InterlockedIncrement(&r->refs);
    r->registered = FALSE;
    stop = !r->stopping;
    r->stopping = TRUE;
    ReleaseSRWLockExclusive(&networking_lock);
    if (stop)
    {
        SetThreadpoolTimer(r->timer, NULL, 0, 0);
        WaitForThreadpoolTimerCallbacks(r->timer, TRUE);
        CloseThreadpoolTimer(r->timer);
        task_queue_remove_callback(r->queue, XTaskQueuePort_Completion, r, connectivity_dispatch);
        AcquireSRWLockExclusive(&networking_lock);
        r->stopped = TRUE;
        WakeAllConditionVariable(&r->idle);
        ReleaseSRWLockExclusive(&networking_lock);
    }
    AcquireSRWLockExclusive(&networking_lock);
    /* wait=true from inside this callback is outside the documented contract.
     * Report pending rather than deadlocking on the caller itself. */
    while (wait && (!r->stopped || r->callback_thread) && r->callback_thread != GetCurrentThreadId())
        SleepConditionVariableSRW(&r->idle, &networking_lock, INFINITE, 0);
    complete = r->stopped && !r->callback_thread;
    retired = connectivity_retire_locked(r);
    ReleaseSRWLockExclusive(&networking_lock);
    if (retired) connectivity_release(r);
    connectivity_release(r);
    TRACE("UnregisterConnectivityHintChanged token %s wait %u complete %u.\n",
          wine_dbgstr_longlong(token.token), wait, complete);
    return complete;
}

void networking_cleanup(void)
{
    XTaskQueueRegistrationToken token;
    struct connectivity_registration *r;
    for (;;)
    {
        AcquireSRWLockShared(&networking_lock);
        for (r = registrations; r && !r->registered; r = r->next) {}
        token.token = r ? r->token : 0;
        ReleaseSRWLockShared(&networking_lock);
        if (!token.token) break;
        /* Active user callbacks may call runtime APIs. Never wait for them
         * while holding runtime_lock during final uninitialization. */
        x_networking_XNetworkingUnregisterConnectivityHintChanged(NULL, token, FALSE);
    }
}

static HRESULT WINAPI x_networking_XNetworkingQueryConfigurationSetting( IXNetworkingImpl2 *iface, XNetworkingConfigurationSetting configurationSetting, UINT64 *value )
{
    FIXME( "iface %p, configurationSetting %d, value %p stub!\n", iface, configurationSetting, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_networking_XNetworkingSetConfigurationSetting( IXNetworkingImpl2 *iface, XNetworkingConfigurationSetting configurationParameter, UINT64 value )
{
    FIXME( "iface %p, configurationParameter %d, value %llu stub!\n", iface, configurationParameter, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_networking_XNetworkingQueryStatistics( IXNetworkingImpl2 *iface, XNetworkingStatisticsType statisticsType, XNetworkingStatisticsBuffer *statisticsBuffer )
{
    FIXME( "iface %p, statisticsType %d, statisticsBuffer %p stub!\n", iface, statisticsType, statisticsBuffer );
    return E_NOTIMPL;
}

static const struct IXNetworkingImpl2Vtbl x_networking_vtbl =
{
    x_networking_QueryInterface,
    x_networking_AddRef,
    x_networking_Release,
    /* IXNetworkingImpl methods */
    x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPort,
    x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync,
    x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult,
    x_networking_XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged,
    x_networking_XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged,
    x_networking_XNetworkingQuerySecurityInformationForUrlAsync,
    x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResultSize,
    x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResult,
    x_networking_XNetworkingQuerySecurityInformationForUrlUtf16Async,
    x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize,
    x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult,
    x_networking_XNetworkingVerifyServerCertificate,
    x_networking_XNetworkingGetConnectivityHint,
    x_networking_XNetworkingRegisterConnectivityHintChanged,
    x_networking_XNetworkingUnregisterConnectivityHintChanged,
    /* IXNetworkingImpl2 methods */
    x_networking_XNetworkingQueryConfigurationSetting,
    x_networking_XNetworkingSetConfigurationSetting,
    x_networking_XNetworkingQueryStatistics,
};

static struct x_networking x_networking =
{
    {&x_networking_vtbl},
    0,
};

IXNetworkingImpl *x_networking_impl = (IXNetworkingImpl *)&x_networking.IXNetworkingImpl2_iface;

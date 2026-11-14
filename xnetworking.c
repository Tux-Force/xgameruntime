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

#include "private.h"

#include "winhttp.h"
#include "wine/list.h"

/* The port Xbox titles use by default. */
#define PREFERRED_UDP_PORT 3074

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

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPort( IXNetworkingImpl2 *iface, UINT16 *preferredLocalUdpMultiplayerPort )
{
    TRACE( "iface %p, preferredLocalUdpMultiplayerPort %p.\n", iface, preferredLocalUdpMultiplayerPort );

    if (!preferredLocalUdpMultiplayerPort) return E_INVALIDARG;
    *preferredLocalUdpMultiplayerPort = PREFERRED_UDP_PORT;
    return S_OK;
}

static HRESULT WINAPI udp_port_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );
        case XAsyncOp_DoWork:
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, sizeof(UINT16) );
            return S_OK;
        case XAsyncOp_GetResult:
            if (data->bufferSize < sizeof(UINT16)) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            *(UINT16 *)data->buffer = PREFERRED_UDP_PORT;
            return S_OK;
        default:
            return S_OK;
    }
}

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock )
{
    TRACE( "iface %p, asyncBlock %p.\n", iface, asyncBlock );

    if (!asyncBlock) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, asyncBlock, NULL, x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync", udp_port_provider );
}

static HRESULT WINAPI x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, UINT16 *preferredLocalUdpMultiplayerPort )
{
    TRACE( "iface %p, asyncBlock %p, preferredLocalUdpMultiplayerPort %p.\n", iface, asyncBlock, preferredLocalUdpMultiplayerPort );

    if (!preferredLocalUdpMultiplayerPort) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, asyncBlock, x_networking_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync, sizeof(*preferredLocalUdpMultiplayerPort), preferredLocalUdpMultiplayerPort, NULL );
}

static HRESULT WINAPI x_networking_XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged( IXNetworkingImpl2 *iface, XTaskQueueHandle queue, void *context, XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    static LONG64 next_token;

    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );

    if (!callback || !token) return E_INVALIDARG;
    /* The preferred port never changes, so the callback is never invoked. */
    token->token = InterlockedIncrement64( &next_token );
    return S_OK;
}

static BOOLEAN WINAPI x_networking_XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged( IXNetworkingImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, token %I64u, wait %d.\n", iface, token.token, wait );
    return TRUE;
}

/*
 * Security information for a URL: Windows returns the TLS versions to allow and, for some Xbox
 * services, certificate thumbprints to pin. No pins are known here, so only the protocol versions
 * are reported. WinHTTP still validates every server certificate against the system roots.
 */
#define SECURITY_PROTOCOLS (WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)

static HRESULT WINAPI security_information_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    XNetworkingSecurityInformation *info;

    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );
        case XAsyncOp_DoWork:
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, sizeof(*info) );
            return S_OK;
        case XAsyncOp_GetResult:
            if (data->bufferSize < sizeof(*info)) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            info = data->buffer;
            info->enabledHttpSecurityProtocolFlags = SECURITY_PROTOCOLS;
            info->thumbprintCount = 0;
            info->thumbprints = NULL;
            return S_OK;
        default:
            return S_OK;
    }
}

static HRESULT security_information_result( XAsyncBlock *async, SIZE_T size, SIZE_T *used, UINT8 *buffer, XNetworkingSecurityInformation **info )
{
    HRESULT hr;

    if (!buffer || !info) return E_INVALIDARG;
    if (SUCCEEDED(hr = IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, security_information_provider, size, buffer, used )))
        *info = (XNetworkingSecurityInformation *)buffer;
    return hr;
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsync( IXNetworkingImpl2 *iface, const char *url, XAsyncBlock *asyncBlock )
{
    TRACE( "iface %p, url %s, asyncBlock %p.\n", iface, debugstr_a( url ), asyncBlock );

    if (!url || !asyncBlock) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, asyncBlock, NULL, security_information_provider, "XNetworkingQuerySecurityInformationForUrlAsync", security_information_provider );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResultSize( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T *securityInformationBufferByteCount )
{
    TRACE( "iface %p, asyncBlock %p, securityInformationBufferByteCount %p.\n", iface, asyncBlock, securityInformationBufferByteCount );
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, asyncBlock, securityInformationBufferByteCount );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlAsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T securityInformationBufferByteCount, SIZE_T *securityInformationBufferByteCountUsed, UINT8 *securityInformationBuffer, XNetworkingSecurityInformation **securityInformation )
{
    TRACE( "iface %p, asyncBlock %p, securityInformationBufferByteCount %Iu, securityInformationBufferByteCountUsed %p, securityInformationBuffer %p, securityInformation %p.\n", iface, asyncBlock, securityInformationBufferByteCount, securityInformationBufferByteCountUsed, securityInformationBuffer, securityInformation );
    return security_information_result( asyncBlock, securityInformationBufferByteCount, securityInformationBufferByteCountUsed, securityInformationBuffer, securityInformation );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16Async( IXNetworkingImpl2 *iface, const WCHAR *url, XAsyncBlock *asyncBlock )
{
    TRACE( "iface %p, url %s, asyncBlock %p.\n", iface, debugstr_w( url ), asyncBlock );

    if (!url || !asyncBlock) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, asyncBlock, NULL, security_information_provider, "XNetworkingQuerySecurityInformationForUrlUtf16Async", security_information_provider );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T *securityInformationBufferByteCount )
{
    TRACE( "iface %p, asyncBlock %p, securityInformationBufferByteCount %p.\n", iface, asyncBlock, securityInformationBufferByteCount );
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, asyncBlock, securityInformationBufferByteCount );
}

static HRESULT WINAPI x_networking_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult( IXNetworkingImpl2 *iface, XAsyncBlock *asyncBlock, SIZE_T securityInformationBufferByteCount, SIZE_T *securityInformationBufferByteCountUsed, UINT8 *securityInformationBuffer, XNetworkingSecurityInformation **securityInformation )
{
    TRACE( "iface %p, asyncBlock %p, securityInformationBufferByteCount %Iu, securityInformationBufferByteCountUsed %p, securityInformationBuffer %p, securityInformation %p.\n", iface, asyncBlock, securityInformationBufferByteCount, securityInformationBufferByteCountUsed, securityInformationBuffer, securityInformation );
    return security_information_result( asyncBlock, securityInformationBufferByteCount, securityInformationBufferByteCountUsed, securityInformationBuffer, securityInformation );
}

static HRESULT WINAPI x_networking_XNetworkingVerifyServerCertificate( IXNetworkingImpl2 *iface, void *requestHandle, const XNetworkingSecurityInformation *securityInformation )
{
    TRACE( "iface %p, requestHandle %p, securityInformation %p.\n", iface, requestHandle, securityInformation );

    if (!requestHandle || !securityInformation) return E_INVALIDARG;
    /* No thumbprints are ever reported, and WinHTTP has already validated the chain. */
    if (securityInformation->thumbprintCount) FIXME( "certificate pinning is not supported.\n" );
    return S_OK;
}

static HRESULT WINAPI x_networking_XNetworkingGetConnectivityHint( IXNetworkingImpl2 *iface, XNetworkingConnectivityHint *connectivityHint )
{
    TRACE( "iface %p, connectivityHint %p.\n", iface, connectivityHint );

    if (!connectivityHint) return E_INVALIDARG;
    memset( connectivityHint, 0, sizeof(*connectivityHint) );
    connectivityHint->connectivityLevel = XNetworkingConnectivityLevelHint_InternetAccess;
    connectivityHint->connectivityCost = XNetworkingConnectivityCostHint_Unrestricted;
    connectivityHint->ianaInterfaceType = 6; /* ethernetCsmacd */
    connectivityHint->networkInitialized = TRUE;
    return S_OK;
}

struct hint_registration
{
    struct list entry;
    UINT64 token;
    void *context;
    XNetworkingConnectivityHintChangedCallback *callback;
    BOOL notified;
};

static struct list hint_registrations = LIST_INIT( hint_registrations );
static CRITICAL_SECTION hint_cs;
static CRITICAL_SECTION_DEBUG hint_cs_debug =
{
    0, 0, &hint_cs,
    { &hint_cs_debug.ProcessLocksList, &hint_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": hint_cs") }
};
static CRITICAL_SECTION hint_cs = { &hint_cs_debug, -1, 0, 0, 0, 0 };

/* Delivers the current hint to a registration that has not seen it yet. */
static void WINAPI hint_initial_callback( void *arg, BOOLEAN canceled )
{
    UINT64 token = (UINT64)(UINT_PTR)arg;
    XNetworkingConnectivityHintChangedCallback *callback = NULL;
    XNetworkingConnectivityHint hint;
    struct hint_registration *reg;
    void *context = NULL;

    EnterCriticalSection( &hint_cs );
    LIST_FOR_EACH_ENTRY( reg, &hint_registrations, struct hint_registration, entry )
    {
        if (reg->token != token || reg->notified) continue;
        reg->notified = TRUE;
        callback = reg->callback;
        context = reg->context;
        break;
    }
    LeaveCriticalSection( &hint_cs );

    if (!callback || canceled) return;
    x_networking_XNetworkingGetConnectivityHint( NULL, &hint );
    callback( context, &hint );
}

static HRESULT WINAPI x_networking_XNetworkingRegisterConnectivityHintChanged( IXNetworkingImpl2 *iface, XTaskQueueHandle queue, void *context, XNetworkingConnectivityHintChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    static LONG64 next_token;
    struct hint_registration *reg;
    HRESULT hr;

    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );

    if (!callback || !token) return E_INVALIDARG;
    if (!(reg = calloc( 1, sizeof(*reg) ))) return E_OUTOFMEMORY;

    reg->token = InterlockedIncrement64( &next_token );
    reg->context = context;
    reg->callback = callback;

    EnterCriticalSection( &hint_cs );
    list_add_tail( &hint_registrations, &reg->entry );
    LeaveCriticalSection( &hint_cs );

    /* Like Windows, report the current state once right after registration. Libraries such as
     * XCurl wait for this first notification before they send any request. The state never
     * changes afterwards. */
    hr = IXThreadingImpl_XTaskQueueSubmitCallback( x_threading_impl, queue, XTaskQueuePort_Completion,
                                                   (void *)(UINT_PTR)reg->token, hint_initial_callback );
    if (FAILED(hr)) WARN( "failed to queue the initial connectivity hint, hr %#lx.\n", hr );

    token->token = reg->token;
    return S_OK;
}

static BOOLEAN WINAPI x_networking_XNetworkingUnregisterConnectivityHintChanged( IXNetworkingImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    struct hint_registration *reg;

    TRACE( "iface %p, token %I64u, wait %d.\n", iface, token.token, wait );

    EnterCriticalSection( &hint_cs );
    LIST_FOR_EACH_ENTRY( reg, &hint_registrations, struct hint_registration, entry )
    {
        if (reg->token != token.token) continue;
        list_remove( &reg->entry );
        free( reg );
        break;
    }
    LeaveCriticalSection( &hint_cs );
    return TRUE;
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

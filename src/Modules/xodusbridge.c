/*
 * Xbox Game runtime Library
 *  Client for the Xodus bridge
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

/*
 * Xodus starts a small HTTP server on 127.0.0.1 for every game it runs and passes its port and a
 * random secret in XODUS_BRIDGE_PORT and XODUS_BRIDGE_SECRET. It answers requests that need the
 * Microsoft services, so the game never sees the user's account tokens.
 */

#include "private.h"

#include "winhttp.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

#define BRIDGE_TIMEOUT_MS 30000
#define BRIDGE_MAX_RESPONSE (1024 * 1024)

BOOL gdkc_bridge_available(void)
{
    return GetEnvironmentVariableW( L"XODUS_BRIDGE_PORT", NULL, 0 ) > 0;
}

HRESULT gdkc_bridge_post( const WCHAR *path, const char *body, char **response )
{
    WCHAR port_str[16], secret[128], header[192];
    HINTERNET session = NULL, connection = NULL, request = NULL;
    DWORD status = 0, size = sizeof(status), read, total = 0;
    char *buffer = NULL, *grown;
    HRESULT hr = E_FAIL;
    INTERNET_PORT port;

    *response = NULL;

    if (!GetEnvironmentVariableW( L"XODUS_BRIDGE_PORT", port_str, ARRAY_SIZE(port_str) ) ||
        !GetEnvironmentVariableW( L"XODUS_BRIDGE_SECRET", secret, ARRAY_SIZE(secret) ))
        return HRESULT_FROM_WIN32( ERROR_SERVICE_NOT_ACTIVE );
    if (!(port = (INTERNET_PORT)wcstoul( port_str, NULL, 10 ))) return E_UNEXPECTED;
    swprintf( header, ARRAY_SIZE(header), L"X-Xodus-Secret: %s\r\nContent-Type: text/plain\r\n", secret );

    /* Always talk to the loopback interface directly, never through a proxy. */
    if (!(session = WinHttpOpen( L"xgameruntime", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 )))
        goto done;
    WinHttpSetTimeouts( session, BRIDGE_TIMEOUT_MS, BRIDGE_TIMEOUT_MS, BRIDGE_TIMEOUT_MS, BRIDGE_TIMEOUT_MS );
    if (!(connection = WinHttpConnect( session, L"127.0.0.1", port, 0 ))) goto done;
    if (!(request = WinHttpOpenRequest( connection, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0 )))
        goto done;
    if (!WinHttpSendRequest( request, header, -1, (void *)body, strlen( body ), strlen( body ), 0 )) goto done;
    if (!WinHttpReceiveResponse( request, NULL )) goto done;
    if (!WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL ))
        goto done;

    for (;;)
    {
        if (!(grown = realloc( buffer, total + 4096 + 1 ))) { hr = E_OUTOFMEMORY; goto done; }
        buffer = grown;
        if (!WinHttpReadData( request, buffer + total, 4096, &read )) goto done;
        if (!read) break;
        total += read;
        if (total > BRIDGE_MAX_RESPONSE) { hr = E_UNEXPECTED; goto done; }
    }
    buffer[total] = 0;

    /* 204 means the request needs no answer, e.g. a URL without an Xbox Live token. */
    if (status == 200 || status == 204)
    {
        *response = buffer;
        buffer = NULL;
        hr = S_OK;
    }
    else
    {
        WARN( "bridge %s answered %lu: %s\n", debugstr_w( path ), status, debugstr_a( buffer ) );
        hr = status == 403 ? E_ACCESSDENIED : HRESULT_FROM_WIN32( ERROR_BAD_NET_RESP );
    }

done:
    if (FAILED(hr) && hr == E_FAIL)
    {
        DWORD err = GetLastError();
        WARN( "bridge request %s failed, error %lu.\n", debugstr_w( path ), err );
        hr = HRESULT_FROM_WIN32( err ? err : ERROR_GEN_FAILURE );
    }
    free( buffer );
    if (request) WinHttpCloseHandle( request );
    if (connection) WinHttpCloseHandle( connection );
    if (session) WinHttpCloseHandle( session );
    return hr;
}

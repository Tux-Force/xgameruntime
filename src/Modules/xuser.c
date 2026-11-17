/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XUser
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

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_user
{
    IXUserImpl6 IXUserImpl6_iface;
    IXUserGamertagImpl IXUserGamertagImpl_iface;
    IXUserDeviceImpl2 IXUserDeviceImpl2_iface;
    LONG ref;
};

/*
 * The signed in Xbox user. Xodus authenticates the user before starting the game and hands the
 * identity down through XODUS_XUID, XODUS_GAMERTAG and XODUS_AGE_GROUP.
 */
struct XUser
{
    LONG ref;
    UINT64 xuid;
    char gamertag[32];
    XUserAgeGroup age_group;
};

static struct XUser local_user;
static INIT_ONCE local_user_once = INIT_ONCE_STATIC_INIT;
static BOOL local_user_valid;

static BOOL CALLBACK init_local_user( INIT_ONCE *once, void *param, void **context )
{
    char value[64];
    DWORD len;

    len = GetEnvironmentVariableA( "XODUS_XUID", value, sizeof(value) );
    if (!len || len >= sizeof(value)) return TRUE;
    local_user.xuid = _strtoui64( value, NULL, 10 );

    len = GetEnvironmentVariableA( "XODUS_GAMERTAG", local_user.gamertag, sizeof(local_user.gamertag) );
    if (!len || len >= sizeof(local_user.gamertag)) strcpy( local_user.gamertag, "Player" );

    local_user.age_group = XUserAgeGroup_Adult;
    len = GetEnvironmentVariableA( "XODUS_AGE_GROUP", value, sizeof(value) );
    if (len && len < sizeof(value))
    {
        if (!stricmp( value, "Child" )) local_user.age_group = XUserAgeGroup_Child;
        else if (!stricmp( value, "Teen" )) local_user.age_group = XUserAgeGroup_Teen;
    }

    local_user.ref = 1;
    local_user_valid = local_user.xuid != 0;
    return TRUE;
}

static struct XUser *get_local_user(void)
{
    InitOnceExecuteOnce( &local_user_once, init_local_user, NULL, NULL );
    return local_user_valid ? &local_user : NULL;
}

static HRESULT WINAPI user_add_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct XUser **result;

    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );

        case XAsyncOp_DoWork:
            if (!get_local_user())
                IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, HRESULT_FROM_WIN32( ERROR_NOT_LOGGED_ON ), 0 );
            else
                IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, sizeof(*result) );
            return S_OK;

        case XAsyncOp_GetResult:
            if (data->bufferSize < sizeof(*result)) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            result = data->buffer;
            InterlockedIncrement( &local_user.ref );
            *result = &local_user;
            return S_OK;

        default:
            return S_OK;
    }
}

/*
 * Xbox Live token requests. The Xodus bridge looks up the relying party of the URL and answers
 * the Authorization header for it; URLs that need no token get an empty one, like on Windows.
 * The bridge returns both authorization and any required request signature.
 */
struct token_request
{
    char *body;
    char *token;            /* bridge answer; split into token and signature */
    const char *signature;
    BOOL utf16;
    int token_count, signature_count; /* UTF-16 code units, including terminators */
    SIZE_T result_size;
};

static char *base64_encode( const UINT8 *data, SIZE_T size )
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out, *p;
    SIZE_T i;

    if (!(out = p = malloc( (size + 2) / 3 * 4 + 1 ))) return NULL;
    for (i = 0; i + 2 < size; i += 3)
    {
        *p++ = table[data[i] >> 2];
        *p++ = table[((data[i] & 3) << 4) | (data[i + 1] >> 4)];
        *p++ = table[((data[i + 1] & 15) << 2) | (data[i + 2] >> 6)];
        *p++ = table[data[i + 2] & 63];
    }
    if (i < size)
    {
        *p++ = table[data[i] >> 2];
        if (i + 1 < size)
        {
            *p++ = table[((data[i] & 3) << 4) | (data[i + 1] >> 4)];
            *p++ = table[(data[i + 1] & 15) << 2];
        }
        else
        {
            *p++ = table[(data[i] & 3) << 4];
            *p++ = '=';
        }
        *p++ = '=';
    }
    *p = 0;
    return out;
}

static HRESULT WINAPI token_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct token_request *request = data->context;
    XUserGetTokenAndSignatureData *result;
    SIZE_T token_size;

    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );

        case XAsyncOp_DoWork:
        {
            HRESULT hr = gdkc_bridge_post( L"/xbl/token", request->body, &request->token );
            char *newline;

            if (FAILED(hr)) WARN( "token request failed, hr %#lx.\n", hr );
            else
            {
                /* The answer is the token, a line break and the signature (both may be empty). */
                request->signature = "";
                if ((newline = strchr( request->token, '\n' )))
                {
                    *newline = 0;
                    request->signature = newline + 1;
                }
            }
            if (SUCCEEDED(hr))
            {
                if (request->utf16)
                {
                    request->token_count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, request->token, -1, NULL, 0 );
                    request->signature_count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, request->signature, -1, NULL, 0 );
                    if (!request->token_count || !request->signature_count)
                        hr = HRESULT_FROM_WIN32( ERROR_NO_UNICODE_TRANSLATION );
                    else
                        request->result_size = sizeof(XUserGetTokenAndSignatureUtf16Data) +
                            ((SIZE_T)request->token_count + request->signature_count) * sizeof(WCHAR);
                }
                else request->result_size = sizeof(*result) + strlen( request->token ) + strlen( request->signature ) + 2;
            }
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, hr, SUCCEEDED(hr) ? request->result_size : 0 );
            return S_OK;
        }

        case XAsyncOp_GetResult:
        {
            SIZE_T signature_size = strlen( request->signature ) + 1;

            if (data->bufferSize < request->result_size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            if (request->utf16)
            {
                XUserGetTokenAndSignatureUtf16Data *wide = data->buffer;
                WCHAR *token = (WCHAR *)(wide + 1), *signature = token + request->token_count;
                if (!MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, request->token, -1, token, request->token_count ) ||
                    !MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, request->signature, -1, signature, request->signature_count ))
                    return HRESULT_FROM_WIN32( ERROR_NO_UNICODE_TRANSLATION );
                wide->token = token;
                wide->signature = signature;
                wide->tokenCount = request->token_count - 1;
                wide->signatureCount = request->signature_count - 1;
                return S_OK;
            }
            token_size = strlen( request->token ) + 1;
            if (data->bufferSize < sizeof(*result) + token_size + signature_size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            result = data->buffer;
            result->token = (char *)(result + 1);
            memcpy( (char *)result->token, request->token, token_size );
            result->tokenSize = token_size - 1;
            result->signature = result->token + token_size;
            memcpy( (char *)result->signature, request->signature, signature_size );
            result->signatureSize = signature_size - 1;
            return S_OK;
        }

        case XAsyncOp_Cleanup:
            free( request->body );
            free( request->token );
            free( request );
            return S_OK;

        default:
            return S_OK;
    }
}

static inline struct x_user *impl_from_IXUserImpl6( IXUserImpl6 *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserImpl6_iface );
}

static HRESULT WINAPI x_user_QueryInterface( IXUserImpl6 *iface, REFIID iid, void **out )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown    ) ||
        IsEqualGUID( iid, &IID_IXUserImpl  ) ||
        IsEqualGUID( iid, &IID_IXUserImpl2 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl3 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl4 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl5 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl6 ))
    {
        IXUserImpl6_AddRef( *out = &impl->IXUserImpl6_iface );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IXUserGamertagImpl ))
    {
        IXUserGamertagImpl_AddRef( *out = &impl->IXUserGamertagImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_AddRef( IXUserImpl6 *iface )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_user_Release( IXUserImpl6 *iface )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_user_XUserDuplicateHandle( IXUserImpl6 *iface, XUserHandle handle, XUserHandle *duplicatedHandle )
{
    TRACE( "iface %p, handle %p, duplicatedHandle %p.\n", iface, handle, duplicatedHandle );

    if (!handle || !duplicatedHandle) return E_INVALIDARG;
    InterlockedIncrement( &handle->ref );
    *duplicatedHandle = handle;
    return S_OK;
}

static void WINAPI x_user_XUserCloseHandle( IXUserImpl6 *iface, XUserHandle user )
{
    TRACE( "iface %p, user %p.\n", iface, user );
    /* The local user object is static and lives for the whole process. */
    if (user && user->ref > 1) InterlockedDecrement( &user->ref );
}

static INT32 WINAPI x_user_XUserCompare( IXUserImpl6 *iface, XUserHandle user1, XUserHandle user2 )
{
    TRACE( "iface %p, user1 %p, user2 %p.\n", iface, user1, user2 );

    if (!user1 || !user2) return user1 ? 1 : user2 ? -1 : 0;
    if (user1->xuid == user2->xuid) return 0;
    return user1->xuid < user2->xuid ? -1 : 1;
}

static HRESULT WINAPI x_user_XUserGetMaxUsers( IXUserImpl6 *iface, UINT32 *maxUsers )
{
    TRACE( "iface %p, maxUsers %p.\n", iface, maxUsers );
    *maxUsers = 1;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserAddAsync( IXUserImpl6 *iface, XUserAddOptions options, XAsyncBlock *async )
{
    TRACE( "iface %p, options %d, async %p.\n", iface, options, async );

    if (!async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL, x_user_XUserAddAsync, "XUserAddAsync", user_add_provider );
}

static HRESULT WINAPI x_user_XUserAddResult( IXUserImpl6 *iface, XAsyncBlock *async, XUserHandle *newUser )
{
    TRACE( "iface %p, async %p, newUser %p.\n", iface, async, newUser );

    if (!newUser) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_user_XUserAddAsync, sizeof(*newUser), newUser, NULL );
}

static HRESULT WINAPI x_user_XUserGetLocalId( IXUserImpl6 *iface, XUserHandle user, XUserLocalId *userLocalId )
{
    TRACE( "iface %p, user %p, userLocalId %p.\n", iface, user, userLocalId );

    if (!user || !userLocalId) return E_INVALIDARG;
    userLocalId->value = user->xuid;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserFindUserByLocalId( IXUserImpl6 *iface, XUserLocalId userLocalId, XUserHandle *handle )
{
    struct XUser *user = get_local_user();

    TRACE( "iface %p, userLocalId %I64u, handle %p.\n", iface, userLocalId.value, handle );

    if (!handle) return E_INVALIDARG;
    if (!user || user->xuid != userLocalId.value) return HRESULT_FROM_WIN32( ERROR_NO_SUCH_USER );
    InterlockedIncrement( &user->ref );
    *handle = user;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetId( IXUserImpl6 *iface, XUserHandle user, UINT64 *userId )
{
    TRACE( "iface %p, user %p, userId %p.\n", iface, user, userId );

    if (!user || !userId) return E_INVALIDARG;
    *userId = user->xuid;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserFindUserById( IXUserImpl6 *iface, UINT64 userId, XUserHandle *handle )
{
    struct XUser *user = get_local_user();

    TRACE( "iface %p, userId %I64u, handle %p.\n", iface, userId, handle );

    if (!handle) return E_INVALIDARG;
    if (!user || user->xuid != userId) return HRESULT_FROM_WIN32( ERROR_NO_SUCH_USER );
    InterlockedIncrement( &user->ref );
    *handle = user;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetIsGuest( IXUserImpl6 *iface, XUserHandle user, BOOLEAN *isGuest )
{
    TRACE( "iface %p, user %p, isGuest %p.\n", iface, user, isGuest );

    if (!user || !isGuest) return E_INVALIDARG;
    *isGuest = FALSE;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetState( IXUserImpl6 *iface, XUserHandle user, XUserState *state )
{
    TRACE( "iface %p, user %p, state %p.\n", iface, user, state );

    if (!user || !state) return E_INVALIDARG;
    *state = XUserState_SignedIn;
    return S_OK;
}

static HRESULT WINAPI __PADDING__( IXUserImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureAsync( IXUserImpl6 *iface, XUserHandle user, XUserGamerPictureSize pictureSize, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, pictureSize %d, async %p stub!\n", iface, user, pictureSize, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    FIXME( "iface %p, async %p, bufferSize %p stub!\n", iface, async, bufferSize );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, SIZE_T *bufferUsed )
{
    FIXME( "iface %p, async %p, bufferSize %Iu, buffer %p, bufferUsed %p stub!\n", iface, async, bufferSize, buffer, bufferUsed );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetAgeGroup( IXUserImpl6 *iface, XUserHandle user, XUserAgeGroup *ageGroup )
{
    TRACE( "iface %p, user %p, ageGroup %p.\n", iface, user, ageGroup );

    if (!user || !ageGroup) return E_INVALIDARG;
    *ageGroup = user->age_group;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserCheckPrivilege( IXUserImpl6 *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, BOOLEAN *hasPrivilege, XUserPrivilegeDenyReason *reason )
{
    TRACE( "iface %p, user %p, options %d, privilege %d, hasPrivilege %p, reason %p.\n", iface, user, options, privilege, hasPrivilege, reason );

    if (!user || !hasPrivilege) return E_INVALIDARG;
    /* Privileges depend on the account's Xbox settings; the gamertag was verified at login. */
    *hasPrivilege = TRUE;
    if (reason) *reason = XUserPrivilegeDenyReason_None;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiAsync( IXUserImpl6 *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, options %d, privilege %d, async %p stub!\n", iface, user, options, privilege, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT begin_token_request( XUserHandle user, XUserGetTokenAndSignatureOptions options, const char *method, const char *url, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async, BOOL utf16, const void *identity )
{
    struct token_request *request;
    char msa_app_id[64] = "", *body64;
    UINT32 title_id;
    SIZE_T len;
    HRESULT hr;

    if (!user || !async || !method || !url || (bodySize && !bodyBuffer)) return E_INVALIDARG;
    if (strpbrk( method, "\r\n" ) || strpbrk( url, "\r\n" )) return E_INVALIDARG;
    /* Without Xodus there is nobody to ask for a token: behave like an unsupported service. */
    if (!gdkc_bridge_available()) return E_NOTIMPL;

    if (!(request = calloc( 1, sizeof(*request) ))) return E_OUTOFMEMORY;
    request->utf16 = utf16;
    if (!gdkc_config_get_title_id( &title_id )) title_id = 0;
    gdkc_config_get_msa_app_id( msa_app_id, sizeof(msa_app_id) );
    if (strpbrk( msa_app_id, "\r\n" )) msa_app_id[0] = 0;
    /* The body is only needed to sign the request. */
    if (!(body64 = base64_encode( bodySize ? bodyBuffer : NULL, bodyBuffer ? bodySize : 0 )))
    {
        free( request );
        return E_OUTOFMEMORY;
    }
    len = strlen( method ) + strlen( url ) + strlen( msa_app_id ) + strlen( body64 ) + 32;
    if (!(request->body = malloc( len )))
    {
        free( body64 );
        free( request );
        return E_OUTOFMEMORY;
    }
    sprintf( request->body, "%s\n%s\n%d\n%u\n%s\n%s", method, url, (options & XUserGetTokenAndSignatureOptions_ForceRefresh) ? 1 : 0, title_id, msa_app_id, body64 );
    free( body64 );

    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, request, identity, utf16 ? "XUserGetTokenAndSignatureUtf16Async" : "XUserGetTokenAndSignatureAsync", token_provider )))
    {
        free( request->body );
        free( request );
    }
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureAsync( IXUserImpl6 *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, const char *method, const char *url, SIZE_T headerCount, const XUserGetTokenAndSignatureHttpHeader *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async )
{
    TRACE( "user %p, method %s, url %s, async %p.\n", user, debugstr_a(method), debugstr_a(url), async );
    if (headerCount && !headers) return E_INVALIDARG;
    return begin_token_request( user, options, method, url, bodySize, bodyBuffer, async, FALSE, x_user_XUserGetTokenAndSignatureAsync );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    TRACE( "iface %p, async %p, bufferSize %p.\n", iface, async, bufferSize );

    if (!bufferSize) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, bufferSize );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureData **ptrToBuffer, SIZE_T *bufferUsed )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, bufferSize %Iu, buffer %p, ptrToBuffer %p, bufferUsed %p.\n", iface, async, bufferSize, buffer, ptrToBuffer, bufferUsed );

    if (!buffer) return E_INVALIDARG;
    if (SUCCEEDED(hr = IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_user_XUserGetTokenAndSignatureAsync, bufferSize, buffer, bufferUsed )) && ptrToBuffer)
        *ptrToBuffer = buffer;
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Async( IXUserImpl6 *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, const WCHAR *method, const WCHAR *url, SIZE_T headerCount, const XUserGetTokenAndSignatureUtf16HttpHeader *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async )
{
    char *method8, *url8;
    int method_size, url_size;
    HRESULT hr;

    TRACE( "user %p, method %s, url %s, async %p.\n", user, debugstr_w(method), debugstr_w(url), async );
    if (!method || !url || (headerCount && !headers)) return E_INVALIDARG;
    method_size = WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, method, -1, NULL, 0, NULL, NULL );
    url_size = WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, url, -1, NULL, 0, NULL, NULL );
    if (!method_size || !url_size) return HRESULT_FROM_WIN32( ERROR_NO_UNICODE_TRANSLATION );
    if (!(method8 = malloc(method_size))) return E_OUTOFMEMORY;
    if (!(url8 = malloc(url_size))) { free(method8); return E_OUTOFMEMORY; }
    WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, method, -1, method8, method_size, NULL, NULL );
    WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, url, -1, url8, url_size, NULL, NULL );
    hr = begin_token_request( user, options, method8, url8, bodySize, bodyBuffer, async, TRUE, x_user_XUserGetTokenAndSignatureUtf16Async );
    free(method8);
    free(url8);
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16ResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    if (!async || !bufferSize) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, bufferSize );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Result( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureUtf16Data **ptrToBuffer, SIZE_T *bufferUsed )
{
    HRESULT hr;
    SIZE_T required;

    if (ptrToBuffer) *ptrToBuffer = NULL;
    if (!async || !buffer) return E_INVALIDARG;
    if (FAILED(hr = IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, &required ))) return hr;
    if (bufferSize < required) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    hr = IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_user_XUserGetTokenAndSignatureUtf16Async, bufferSize, buffer, bufferUsed );
    if (SUCCEEDED(hr) && ptrToBuffer) *ptrToBuffer = buffer;
    return hr;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiAsync( IXUserImpl6 *iface, XUserHandle user, const char *url, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, url %s, async %p stub!\n", iface, user, debugstr_a( url ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Async( IXUserImpl6 *iface, XUserHandle user, const WCHAR *url, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, url %s, async %p stub!\n", iface, user, debugstr_w( url ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Result( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserRegisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueHandle queue, void *context, XUserChangeEventCallback *callback, XTaskQueueRegistrationToken *token )
{
    static LONG64 next_token;

    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );

    if (!callback || !token) return E_INVALIDARG;
    /* No user is ever signed in or out behind the game's back yet, so the callback is never invoked. */
    token->token = InterlockedIncrement64( &next_token );
    return S_OK;
}

static BOOLEAN WINAPI x_user_XUserUnregisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, token %I64u, wait %d.\n", iface, token.token, wait );
    return TRUE;
}

static HRESULT WINAPI x_user_XUserGetSignOutDeferral( IXUserImpl6 *iface, XUserSignOutDeferralHandle *deferral )
{
    TRACE( "iface %p, deferral %p.\n", iface, deferral );
    *deferral = NULL;
    return E_GAMEUSER_DEFERRAL_NOT_AVAILABLE;
}

static void WINAPI x_user_XUserCloseSignOutDeferralHandle( IXUserImpl6 *iface, XUserSignOutDeferralHandle deferral )
{
    TRACE( "iface %p, deferral %p.\n", iface, deferral );
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiAsync( IXUserImpl6 *iface, UINT64 userId, XAsyncBlock *async )
{
    FIXME( "iface %p, userId %llu, async %p stub!\n", iface, userId, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async, XUserHandle *newUser )
{
    FIXME( "iface %p, async %p, newUser %p stub!\n", iface, async, newUser );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyAsync( IXUserImpl6 *iface, XUserHandle user, XUserGetMsaTokenSilentlyOptions options, const char *scope, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, options %u, scope %s, async %p stub!\n", iface, user, options, debugstr_a( scope ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T resultTokenSize, char *resultToken, SIZE_T *resultTokenUsed )
{
    FIXME( "iface %p, async %p, resultTokenSize %Iu, resultToken %p, resultTokenUsed %p stub!\n", iface, async, resultTokenSize, resultToken, resultTokenUsed );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *tokenSize )
{
    FIXME( "iface %p, async %p, tokenSize %p stub!\n", iface, async, tokenSize );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsStoreUser( IXUserImpl6 *iface, XUserHandle user )
{
    FIXME( "iface %p, user %p stub!\n", iface, user );
    return TRUE;
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectSetEventHandlers( IXUserImpl6 *iface, XTaskQueueHandle queue, XUserPlatformRemoteConnectEventHandlers *handlers )
{
    FIXME( "iface %p, queue %p, handlers %p stub!\n", iface, queue, handlers );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectCancelPrompt( IXUserImpl6 *iface, XUserPlatformOperation operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptSetEventHandlers( IXUserImpl6 *iface, XTaskQueueHandle queue, XUserPlatformSpopPromptEventHandler *handler, void *context )
{
    FIXME( "iface %p, queue %p, handler %p, context %p stub!\n", iface, queue, handler, context );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptComplete( IXUserImpl6 *iface, XUserPlatformOperation operation, XUserPlatformOperationResult result )
{
    FIXME( "iface %p, operation %p, result %d stub!\n", iface, operation, result );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsSignOutPresent( IXUserImpl6 *iface )
{
    TRACE( "iface %p.\n", iface );
    return FALSE;
}

static HRESULT WINAPI x_user_XUserSignOutAsync( IXUserImpl6 *iface, XUserHandle user, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, async %p stub!\n", iface, user, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserSignOutResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static const struct IXUserImpl6Vtbl x_user_vtbl =
{
    x_user_QueryInterface,
    x_user_AddRef,
    x_user_Release,
    /* IXUserImpl methods */
    x_user_XUserDuplicateHandle,
    x_user_XUserCloseHandle,
    x_user_XUserCompare,
    x_user_XUserGetMaxUsers,
    x_user_XUserAddAsync,
    x_user_XUserAddResult,
    x_user_XUserGetLocalId,
    x_user_XUserFindUserByLocalId,
    x_user_XUserGetId,
    x_user_XUserFindUserById,
    x_user_XUserGetIsGuest,
    x_user_XUserGetState,
    __PADDING__,
    x_user_XUserGetGamerPictureAsync,
    x_user_XUserGetGamerPictureResultSize,
    x_user_XUserGetGamerPictureResult,
    x_user_XUserGetAgeGroup,
    x_user_XUserCheckPrivilege,
    x_user_XUserResolvePrivilegeWithUiAsync,
    x_user_XUserResolvePrivilegeWithUiResult,
    x_user_XUserGetTokenAndSignatureAsync,
    x_user_XUserGetTokenAndSignatureResultSize,
    x_user_XUserGetTokenAndSignatureResult,
    x_user_XUserGetTokenAndSignatureUtf16Async,
    x_user_XUserGetTokenAndSignatureUtf16ResultSize,
    x_user_XUserGetTokenAndSignatureUtf16Result,
    x_user_XUserResolveIssueWithUiAsync,
    x_user_XUserResolveIssueWithUiResult,
    x_user_XUserResolveIssueWithUiUtf16Async,
    x_user_XUserResolveIssueWithUiUtf16Result,
    x_user_XUserRegisterForChangeEvent,
    x_user_XUserUnregisterForChangeEvent,
    x_user_XUserGetSignOutDeferral,
    x_user_XUserCloseSignOutDeferralHandle,
    /* IXUserImpl2 methods */
    x_user_XUserAddByIdWithUiAsync,
    x_user_XUserAddByIdWithUiResult,
    /* IXUserImpl3 methods */
    x_user_XUserGetMsaTokenSilentlyAsync,
    x_user_XUserGetMsaTokenSilentlyResult,
    x_user_XUserGetMsaTokenSilentlyResultSize,
    /* IXUserImpl4 methods */
    x_user_XUserIsStoreUser,
    /* IXUserImpl5 methods */
    x_user_XUserPlatformRemoteConnectSetEventHandlers,
    x_user_XUserPlatformRemoteConnectCancelPrompt,
    x_user_XUserPlatformSpopPromptSetEventHandlers,
    x_user_XUserPlatformSpopPromptComplete,
    /* IXUserImpl6 methods */
    x_user_XUserIsSignOutPresent,
    x_user_XUserSignOutAsync,
    x_user_XUserSignOutResult,
};

static inline struct x_user *impl_from_IXUserGamertagImpl( IXUserGamertagImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserGamertagImpl_iface );
}

static HRESULT WINAPI x_user_gamertag_QueryInterface( IXUserGamertagImpl *iface, REFIID riid, void **out )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_QueryInterface( &impl->IXUserImpl6_iface, riid, out );
}

static ULONG WINAPI x_user_gamertag_AddRef( IXUserGamertagImpl *iface )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_AddRef( &impl->IXUserImpl6_iface );
}

static ULONG WINAPI x_user_gamertag_Release( IXUserGamertagImpl *iface )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_Release( &impl->IXUserImpl6_iface );
}

static HRESULT WINAPI x_user_gamertag_XUserGetGamertag( IXUserGamertagImpl *iface, XUserHandle user, XUserGamertagComponent gamertagComponent, SIZE_T gamertagSize, char *gamertag, SIZE_T *gamertagUsed )
{
    SIZE_T len;

    TRACE( "iface %p, user %p, gamertagComponent %d, gamertagSize %Iu, gamertag %p, gamertagUsed %p.\n", iface, user, gamertagComponent, gamertagSize, gamertag, gamertagUsed );

    if (!user || !gamertag) return E_INVALIDARG;
    /* Only the classic gamertag is known; the modern components are left empty. */
    if (gamertagComponent != XUserGamertagComponent_Classic && gamertagComponent != XUserGamertagComponent_UniqueModern)
    {
        if (gamertagSize < 1) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
        gamertag[0] = 0;
        if (gamertagUsed) *gamertagUsed = 1;
        return S_OK;
    }

    len = strlen( user->gamertag ) + 1;
    if (len > gamertagSize) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    memcpy( gamertag, user->gamertag, len );
    if (gamertagUsed) *gamertagUsed = len;
    return S_OK;
}

static const struct IXUserGamertagImplVtbl x_user_gamertag_vtbl =
{
    x_user_gamertag_QueryInterface,
    x_user_gamertag_AddRef,
    x_user_gamertag_Release,
    /* IXUserGamertag methods */
    x_user_gamertag_XUserGetGamertag,
};

static inline struct x_user *impl_from_IXUserDeviceImpl2( IXUserDeviceImpl2 *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserDeviceImpl2_iface );
}

static HRESULT WINAPI x_user_device_QueryInterface( IXUserDeviceImpl2 *iface, REFIID iid, void **out )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown          ) ||
        IsEqualGUID( iid, &IID_IXUserDeviceImpl  ) ||
        IsEqualGUID( iid, &IID_IXUserDeviceImpl2 ))
    {
        IXUserDeviceImpl2_AddRef( *out = &impl->IXUserDeviceImpl2_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_device_AddRef( IXUserDeviceImpl2 *iface )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );
    return IXUserImpl6_AddRef( &impl->IXUserImpl6_iface );
}

static ULONG WINAPI x_user_device_Release( IXUserDeviceImpl2 *iface )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );
    return IXUserImpl6_Release( &impl->IXUserImpl6_iface );
}

static HRESULT WINAPI x_user_device_XUserFindForDevice( IXUserDeviceImpl2 *iface, const APP_LOCAL_DEVICE_ID *deviceId, XUserHandle *handle )
{
    FIXME( "iface %p, deviceId %p, handle %p stub!\n", iface, deviceId, handle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserRegisterForDeviceAssociationChanged( IXUserDeviceImpl2 *iface, XTaskQueueHandle queue, void *context, XUserDeviceAssociationChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_device_XUserUnregisterForDeviceAssociationChanged( IXUserDeviceImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_user_device_XUserGetDefaultAudioEndpointUtf16( IXUserDeviceImpl2 *iface, XUserLocalId user, XUserDefaultAudioEndpointKind defaultAudioEndpointKind, SIZE_T endpointIdUtf16Count, WCHAR *endpointIdUtf16, SIZE_T *endpointIdUtf16Used )
{
    FIXME( "iface %p, user %p, defaultAudioEndpointKind %d, endpointIdUtf16Count %Iu, endpointIdUtf16 %p, endpointIdUtf16Used %p stub!\n", iface, &user, defaultAudioEndpointKind, endpointIdUtf16Count, endpointIdUtf16, endpointIdUtf16Used );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserRegisterForDefaultAudioEndpointUtf16Changed( IXUserDeviceImpl2 *iface, XTaskQueueHandle queue, void *context, XUserDefaultAudioEndpointUtf16ChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_device_XUserUnregisterForDefaultAudioEndpointUtf16Changed( IXUserDeviceImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_user_device_XUserFindControllerForUserWithUiAsync( IXUserDeviceImpl2 *iface, XUserHandle user, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, async %p stub!\n", iface, user, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserFindControllerForUserWithUiResult( IXUserDeviceImpl2 *iface, XAsyncBlock *async, APP_LOCAL_DEVICE_ID *deviceId )
{
    FIXME( "iface %p, async %p, deviceId %p stub!\n", iface, async, deviceId );
    return E_NOTIMPL;
}

static const struct IXUserDeviceImpl2Vtbl x_user_device_vtbl =
{
    x_user_device_QueryInterface,
    x_user_device_AddRef,
    x_user_device_Release,
    /* IXUserDeviceImpl/IXUserDeviceImpl2 methods */
    x_user_device_XUserFindForDevice,
    x_user_device_XUserRegisterForDeviceAssociationChanged,
    x_user_device_XUserUnregisterForDeviceAssociationChanged,
    x_user_device_XUserGetDefaultAudioEndpointUtf16,
    x_user_device_XUserRegisterForDefaultAudioEndpointUtf16Changed,
    x_user_device_XUserUnregisterForDefaultAudioEndpointUtf16Changed,
    x_user_device_XUserFindControllerForUserWithUiAsync,
    x_user_device_XUserFindControllerForUserWithUiResult,
};

static struct x_user x_user =
{
    {&x_user_vtbl},
    {&x_user_gamertag_vtbl},
    {&x_user_device_vtbl},
    0,
};

IXUserImpl *x_user_impl = (IXUserImpl *)&x_user.IXUserImpl6_iface;
IXUserDeviceImpl *x_user_device_impl = (IXUserDeviceImpl *)&x_user.IXUserDeviceImpl2_iface;

/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XPackage
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

struct x_package
{
    IXPackageImpl4 IXPackageImpl4_iface;
    LONG ref;
};

static inline struct x_package *impl_from_IXPackageImpl4( IXPackageImpl4 *iface )
{
    return CONTAINING_RECORD( iface, struct x_package, IXPackageImpl4_iface );
}

static HRESULT WINAPI x_package_QueryInterface( IXPackageImpl4 *iface, REFIID iid, void **out )
{
    struct x_package *impl = impl_from_IXPackageImpl4( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown       ) ||
        IsEqualGUID( iid, &IID_IXPackageImpl  ) ||
        IsEqualGUID( iid, &IID_IXPackageImpl2 ) ||
        IsEqualGUID( iid, &IID_IXPackageImpl3 ) ||
        IsEqualGUID( iid, &IID_IXPackageImpl4 ))
    {
        IXPackageImpl_AddRef( *out = &impl->IXPackageImpl4_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_package_AddRef( IXPackageImpl4 *iface )
{
    struct x_package *impl = impl_from_IXPackageImpl4( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_package_Release( IXPackageImpl4 *iface )
{
    struct x_package *impl = impl_from_IXPackageImpl4( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

struct XPackageInstallationMonitor
{
    UINT32 interval_ms;
};

struct XPackageMountInstance
{
    char *path;
};

static BOOL is_game_package( const char *packageIdentifier )
{
    char game[GDKC_PACKAGE_ID_SIZE], id[GDKC_PACKAGE_ID_SIZE];

    return gdkc_game_package_id( game ) && gdkc_normalize_package_id( packageIdentifier, id ) && !strcmp( game, id );
}

struct game_details
{
    char package_id[GDKC_PACKAGE_ID_SIZE];
    char name[256];
    char publisher[256];
    char store_id[GDKC_STORE_ID_SIZE];
    char title_id[16];
};

static BOOL get_game_details( struct game_details *game, UINT32 index, UINT32 count, XPackageDetails *details )
{
    UINT32 title_id;

    if (!gdkc_config_available() || !gdkc_game_package_id( game->package_id )) return FALSE;
    if (!gdkc_config_get_display_name( game->name, sizeof(game->name) )) game->name[0] = 0;
    if (!gdkc_config_get_publisher_display_name( game->publisher, sizeof(game->publisher) )) game->publisher[0] = 0;
    if (!gdkc_config_get_store_id( game->store_id, sizeof(game->store_id) )) game->store_id[0] = 0;
    if (gdkc_config_get_title_id( &title_id )) sprintf( game->title_id, "%08X", title_id );
    else game->title_id[0] = 0;

    memset( details, 0, sizeof(*details) );
    details->packageIdentifier = game->package_id;
    gdkc_config_get_version( &details->version );
    details->kind = XPackageKind_Game;
    details->displayName = game->name;
    details->description = "";
    details->publisher = game->publisher;
    details->storeId = game->store_id;
    details->index = index;
    details->count = count;
    details->titleID = game->title_id;
    return TRUE;
}

/* The game's own package and the installed add-ons (DLC) Xodus reports for it. */
static HRESULT enumerate_packages( XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    struct gdkc_addons *addons;
    struct game_details game;
    XPackageDetails details;
    HRESULT hr;
    UINT32 i;

    if (!callback) return E_INVALIDARG;
    if (scope > XPackageEnumerationScope_ThisPublisher) return E_INVALIDARG;

    switch (kind)
    {
        case XPackageKind_Game:
            if (get_game_details( &game, 0, 1, &details )) callback( context, &details );
            return S_OK;

        case XPackageKind_Content:
            if (FAILED(hr = gdkc_addons_get( &addons ))) return hr;
            for (i = 0; i < addons->count; i++)
            {
                gdkc_addon_get_details( &addons->items[i], i, addons->count, &details );
                if (!callback( context, &details )) break;
            }
            gdkc_addons_release( addons );
            return S_OK;

        case XPackageKind_PublisherContent:
            /* Content shared between the publisher's games is not installed by Xodus. */
            return S_OK;

        default:
            return E_INVALIDARG;
    }
}

/* Whether packageIdentifier is the game or one of its installed add-ons. */
static HRESULT find_package( const char *packageIdentifier )
{
    struct gdkc_addons *addons;
    BOOL found;
    HRESULT hr;

    if (!packageIdentifier) return E_INVALIDARG;
    if (is_game_package( packageIdentifier )) return S_OK;
    if (FAILED(hr = gdkc_addons_get( &addons ))) return hr;
    found = gdkc_addons_find( addons, packageIdentifier ) != NULL;
    gdkc_addons_release( addons );
    return found ? S_OK : HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
}

struct mount_request
{
    char *package_id;
    char *path;
};

static HRESULT mount_addon( const char *packageIdentifier, char **path )
{
    const struct gdkc_addon *addon;
    struct gdkc_addons *addons;
    BOOL licensed;
    HRESULT hr;

    if (FAILED(hr = gdkc_addons_get( &addons ))) return hr;
    if (!(addon = gdkc_addons_find( addons, packageIdentifier ))) hr = HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    else if (SUCCEEDED(hr = gdkc_addon_license( addon, &licensed )) && !licensed) hr = E_ACCESSDENIED;
    else if (SUCCEEDED(hr) && !(*path = strdup( addon->path ))) hr = E_OUTOFMEMORY;
    gdkc_addons_release( addons );
    return hr;
}

static HRESULT WINAPI mount_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct mount_request *request = data->context;
    struct XPackageMountInstance *mount;
    HRESULT hr;

    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );

        case XAsyncOp_DoWork:
            hr = mount_addon( request->package_id, &request->path );
            if (FAILED(hr)) WARN( "could not mount %s, hr %#lx.\n", debugstr_a( request->package_id ), hr );
            else TRACE( "mounted %s at %s.\n", debugstr_a( request->package_id ), debugstr_a( request->path ) );
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, hr, SUCCEEDED(hr) ? sizeof(XPackageMountHandle) : 0 );
            return S_OK;

        case XAsyncOp_GetResult:
            if (data->bufferSize < sizeof(XPackageMountHandle)) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            if (!(mount = calloc( 1, sizeof(*mount) ))) return E_OUTOFMEMORY;
            if (!(mount->path = strdup( request->path ))) { free( mount ); return E_OUTOFMEMORY; }
            *(XPackageMountHandle *)data->buffer = mount;
            return S_OK;

        case XAsyncOp_Cleanup:
            free( request->package_id );
            free( request->path );
            free( request );
            return S_OK;

        default:
            return S_OK;
    }
}

static HRESULT WINAPI x_package_XPackageGetCurrentProcessPackageIdentifier( IXPackageImpl4 *iface, SIZE_T bufferSize, char *buffer )
{
    char id[64];
    DWORD len;

    TRACE( "iface %p, bufferSize %Iu, buffer %p.\n", iface, bufferSize, buffer );

    if (!buffer) return E_INVALIDARG;

    /* The package identifier is the content id of the package. Xodus passes it down to the game process. */
    len = GetEnvironmentVariableA( "XODUS_PACKAGE_ID", id, sizeof(id) );
    if (!len || len >= sizeof(id)) return E_GAMERUNTIME_GAMECONFIG_BAD_FORMAT;
    if (len + 1 > bufferSize) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );

    strcpy( buffer, id );
    return S_OK;
}

static BOOLEAN WINAPI x_package_XPackageIsPackagedProcess( IXPackageImpl4 *iface )
{
    TRACE( "iface %p.\n", iface );
    return gdkc_config_available();
}

static HRESULT WINAPI x_package_XPackageCreateInstallationMonitor( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT32 minimumUpdateIntervalMs, XTaskQueueHandle queue, XPackageInstallationMonitorHandle *installationMonitor )
{
    struct XPackageInstallationMonitor *monitor;

    TRACE( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p, minimumUpdateIntervalMs %u, queue %p, installationMonitor %p.\n", iface, debugstr_a( packageIdentifier ), selectorCount, selectors, minimumUpdateIntervalMs, queue, installationMonitor );

    if (!packageIdentifier || !installationMonitor) return E_INVALIDARG;
    if (!(monitor = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*monitor) ))) return E_OUTOFMEMORY;
    monitor->interval_ms = minimumUpdateIntervalMs;

    *installationMonitor = monitor;
    return S_OK;
}

static void WINAPI x_package_XPackageCloseInstallationMonitorHandle( IXPackageImpl4 *iface, XPackageInstallationMonitorHandle installationMonitor )
{
    TRACE( "iface %p, installationMonitor %p.\n", iface, installationMonitor );
    HeapFree( GetProcessHeap(), 0, installationMonitor );
}

static void WINAPI x_package_XPackageGetInstallationProgress( IXPackageImpl4 *iface, XPackageInstallationMonitorHandle installationMonitor, XPackageInstallationProgress *progress )
{
    TRACE( "iface %p, installationMonitor %p, progress %p.\n", iface, installationMonitor, progress );

    if (!progress) return;
    /* Packages downloaded by Xodus are always fully installed. */
    memset( progress, 0, sizeof(*progress) );
    progress->totalBytes = 1;
    progress->installedBytes = 1;
    progress->launchBytes = 1;
    progress->launchable = TRUE;
    progress->completed = TRUE;
}

static BOOLEAN WINAPI x_package_XPackageUpdateInstallationMonitor( IXPackageImpl4 *iface, XPackageInstallationMonitorHandle installationMonitor )
{
    TRACE( "iface %p, installationMonitor %p.\n", iface, installationMonitor );
    return TRUE;
}

static HRESULT WINAPI x_package_XPackageRegisterInstallationProgressChanged( IXPackageImpl4 *iface, XPackageInstallationMonitorHandle installationMonitor, void *context, XPackageInstallationProgressCallback *callback, XTaskQueueRegistrationToken *token )
{
    static LONG64 next_token;

    TRACE( "iface %p, installationMonitor %p, context %p, callback %p, token %p.\n", iface, installationMonitor, context, callback, token );

    if (!installationMonitor || !callback || !token) return E_INVALIDARG;
    /* The installation state never changes, so the callback is never invoked. */
    token->token = InterlockedIncrement64( &next_token );
    return S_OK;
}

static BOOLEAN WINAPI x_package_XPackageUnregisterInstallationProgressChanged( IXPackageImpl4 *iface, XPackageInstallationMonitorHandle installationMonitor, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, installationMonitor %p, token %I64u, wait %d.\n", iface, installationMonitor, token.token, wait );
    return TRUE;
}

static HRESULT WINAPI x_package_XPackageGetUserLocale( IXPackageImpl4 *iface, SIZE_T localeSize, char *locale )
{
    WCHAR name[LOCALE_NAME_MAX_LENGTH];
    int len;

    TRACE( "iface %p, localeSize %Iu, locale %p.\n", iface, localeSize, locale );

    if (!locale) return E_INVALIDARG;
    if (!GetUserDefaultLocaleName( name, ARRAY_SIZE(name) )) return HRESULT_FROM_WIN32( GetLastError() );
    if (!(len = WideCharToMultiByte( CP_UTF8, 0, name, -1, NULL, 0, NULL, NULL ))) return HRESULT_FROM_WIN32( GetLastError() );
    if ((SIZE_T)len > localeSize) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    WideCharToMultiByte( CP_UTF8, 0, name, -1, locale, len, NULL, NULL );
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageFindChunkAvailability( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, XPackageChunkAvailability *availability )
{
    HRESULT hr;

    TRACE( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p, availability %p.\n", iface, debugstr_a( packageIdentifier ), selectorCount, selectors, availability );

    if (!availability || (selectorCount && !selectors)) return E_INVALIDARG;
    if (FAILED(hr = find_package( packageIdentifier ))) return hr;
    /* Xodus only reports packages once they are fully installed. */
    *availability = XPackageChunkAvailability_Ready;
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageEnumerateChunkAvailability( IXPackageImpl4 *iface, const char *packageIdentifier, XPackageChunkSelectorType type, void *context, XPackageChunkAvailabilityCallback *callback )
{
    FIXME( "iface %p, packageIdentifier %s, type %d, context %p, callback %p stub!\n", iface, debugstr_a( packageIdentifier ), type, context, callback );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageChangeChunkInstallOrder( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors )
{
    FIXME( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p stub!\n", iface, debugstr_a( packageIdentifier ), selectorCount, selectors );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageInstallChunks( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT32 minimumUpdateIntervalMs, BOOLEAN suppressUserConfirmation, XTaskQueueHandle queue, XPackageInstallationMonitorHandle *installationMonitor )
{
    FIXME( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p, minimumUpdateIntervalMs %u, suppressUserConfirmation %d, queue %p, installationMonitor %p stub!\n", iface, debugstr_a( packageIdentifier ), selectorCount, selectors, minimumUpdateIntervalMs, suppressUserConfirmation, queue, installationMonitor );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageInstallChunksAsync( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT32 minimumUpdateIntervalMs, BOOLEAN suppressUserConfirmation, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p, minimumUpdateIntervalMs %u, suppressUserConfirmation %d, asyncBlock %p stub!\n", iface, packageIdentifier, selectorCount, selectors, minimumUpdateIntervalMs, suppressUserConfirmation, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageInstallChunksResult( IXPackageImpl4 *iface, XAsyncBlock *asyncBlock, XPackageInstallationMonitorHandle *installationMonitor )
{
    FIXME( "iface %p, asyncBlock %p, installationMonitor %p stub!\n", iface, asyncBlock, installationMonitor );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageEstimateDownloadSize( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT64 *downloadSize, BOOLEAN *shouldPresentUserConfirmation )
{
    FIXME( "iface %p, packageIdentifier %s, selectorCount %u, selectors %p, downloadSize %p, shouldPresentUserConfirmation %p stub!\n", iface, packageIdentifier, selectorCount, selectors, downloadSize, shouldPresentUserConfirmation );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageUninstallChunks( IXPackageImpl4 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors )
{
    FIXME( "iface %p, packageIdentifier %s, selectorCount %u, selectores %p stub!\n", iface, packageIdentifier, selectorCount, selectors );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING__( IXPackageImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_2__( IXPackageImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_package_XPackageUnregisterPackageInstalled( IXPackageImpl4 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, token %I64u, wait %d.\n", iface, token.token, wait );
    return gdkc_addons_unregister_installed( token );
}

static HRESULT WINAPI __PADDING_3__( IXPackageImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageGetMountPathSize( IXPackageImpl4 *iface, XPackageMountHandle mount, SIZE_T *pathSize )
{
    TRACE( "iface %p, mount %p, pathSize %p.\n", iface, mount, pathSize );

    if (!mount || !pathSize) return E_INVALIDARG;
    *pathSize = strlen( mount->path ) + 1;
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageGetMountPath( IXPackageImpl4 *iface, XPackageMountHandle mount, SIZE_T pathSize, char *path )
{
    TRACE( "iface %p, mount %p, pathSize %Iu, path %p.\n", iface, mount, pathSize, path );

    if (!mount || !path) return E_INVALIDARG;
    if (pathSize < strlen( mount->path ) + 1) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    strcpy( path, mount->path );
    return S_OK;
}

static void WINAPI x_package_XPackageCloseMountHandle( IXPackageImpl4 *iface, XPackageMountHandle mount )
{
    TRACE( "iface %p, mount %p.\n", iface, mount );

    if (!mount) return;
    free( mount->path );
    free( mount );
}

static HRESULT WINAPI __PADDING_4__( IXPackageImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageEnumeratePackages( IXPackageImpl4 *iface, XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    TRACE( "iface %p, kind %d, scope %d, context %p, callback %p.\n", iface, kind, scope, context, callback );
    return enumerate_packages( kind, scope, context, callback );
}

static HRESULT WINAPI x_package_XPackageRegisterPackageInstalled( IXPackageImpl4 *iface, XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    return gdkc_addons_register_installed( queue, context, callback, token );
}

static HRESULT WINAPI x_package_XPackageGetWriteStats( IXPackageImpl4 *iface, XPackageWriteStats *writeStats )
{
    FIXME( "iface %p, writeStats %p stub!\n", iface, writeStats );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_5__( IXPackageImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageUninstallUWPInstance( IXPackageImpl4 *iface, const char *packageName )
{
    FIXME( "iface %p, packageName %s stub!\n", iface, debugstr_a( packageName ) );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_package_XPackageEnumerateFeatures( IXPackageImpl4 *iface, const char *packageIdentifier, void *context, XPackageFeatureEnumerationCallback *callback )
{
    TRACE( "iface %p, packageIdentifier %s, context %p, callback %p.\n", iface, debugstr_a( packageIdentifier ), context, callback );

    if (!callback) return E_INVALIDARG;
    /* Packages are always installed as a whole, without optional features. */
    return find_package( packageIdentifier );
}

static BOOLEAN WINAPI x_package_XPackageUninstallPackage( IXPackageImpl4 *iface, const char *packageIdentifier )
{
    FIXME( "iface %p, packageIdentifier %s", iface, debugstr_a( packageIdentifier ) );
    return FALSE;
}

static HRESULT WINAPI x_package_XPackageEnumeratePackages2( IXPackageImpl4 *iface, XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    TRACE( "iface %p, kind %d, scope %d, context %p, callback %p.\n", iface, kind, scope, context, callback );
    return enumerate_packages( kind, scope, context, callback );
}

static HRESULT WINAPI x_package_XPackageRegisterPackageInstalled2( IXPackageImpl4 *iface, XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    return gdkc_addons_register_installed( queue, context, callback, token );
}

static HRESULT WINAPI x_package_XPackageMountWithUiAsync( IXPackageImpl4 *iface, const char *packageIdentifier, XAsyncBlock *async )
{
    struct mount_request *request;
    HRESULT hr;

    TRACE( "iface %p, packageIdentifier %s, async %p.\n", iface, debugstr_a( packageIdentifier ), async );

    if (!packageIdentifier || !async) return E_INVALIDARG;
    if (!(request = calloc( 1, sizeof(*request) ))) return E_OUTOFMEMORY;
    if (!(request->package_id = strdup( packageIdentifier )))
    {
        free( request );
        return E_OUTOFMEMORY;
    }
    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, request, x_package_XPackageMountWithUiAsync, "XPackageMountWithUiAsync", mount_provider )))
    {
        free( request->package_id );
        free( request );
    }
    return hr;
}

static HRESULT WINAPI x_package_XPackageMountWithUiResult( IXPackageImpl4 *iface, XAsyncBlock *async, XPackageMountHandle *mount )
{
    TRACE( "iface %p, async %p, mount %p.\n", iface, async, mount );

    if (!mount) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_package_XPackageMountWithUiAsync, sizeof(*mount), mount, NULL );
}

static HRESULT WINAPI x_package_XPackageEnumeratePackages3( IXPackageImpl4 *iface, XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    TRACE( "iface %p, kind %d, scope %d, context %p, callback %p.\n", iface, kind, scope, context, callback );
    return enumerate_packages( kind, scope, context, callback );
}

static HRESULT WINAPI x_package_XPackageRegisterPackageInstalled3( IXPackageImpl4 *iface, XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    return gdkc_addons_register_installed( queue, context, callback, token );
}

static HRESULT WINAPI x_package_XPackageGetKind( IXPackageImpl4 *iface, const char *packageIdentifier, XPackageKind *kind )
{
    struct gdkc_addons *addons;
    BOOL found;
    HRESULT hr;

    TRACE( "iface %p, packageIdentifier %s, kind %p.\n", iface, debugstr_a( packageIdentifier ), kind );

    if (!packageIdentifier || !kind) return E_INVALIDARG;
    if (is_game_package( packageIdentifier ))
    {
        *kind = XPackageKind_Game;
        return S_OK;
    }
    if (FAILED(hr = gdkc_addons_get( &addons ))) return hr;
    found = gdkc_addons_find( addons, packageIdentifier ) != NULL;
    gdkc_addons_release( addons );
    if (!found) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    *kind = XPackageKind_Content;
    return S_OK;
}

static const struct IXPackageImpl4Vtbl x_package_vtbl =
{
    x_package_QueryInterface,
    x_package_AddRef,
    x_package_Release,
    /* IXPackageImpl methods */
    x_package_XPackageGetCurrentProcessPackageIdentifier,
    x_package_XPackageIsPackagedProcess,
    x_package_XPackageCreateInstallationMonitor,
    x_package_XPackageCloseInstallationMonitorHandle,
    x_package_XPackageGetInstallationProgress,
    x_package_XPackageUpdateInstallationMonitor,
    x_package_XPackageRegisterInstallationProgressChanged,
    x_package_XPackageUnregisterInstallationProgressChanged,
    x_package_XPackageGetUserLocale,
    x_package_XPackageFindChunkAvailability,
    x_package_XPackageEnumerateChunkAvailability,
    x_package_XPackageChangeChunkInstallOrder,
    x_package_XPackageInstallChunks,
    x_package_XPackageInstallChunksAsync,
    x_package_XPackageInstallChunksResult,
    x_package_XPackageEstimateDownloadSize,
    x_package_XPackageUninstallChunks,
    __PADDING__,
    __PADDING_2__,
    x_package_XPackageUnregisterPackageInstalled,
    __PADDING_3__,
    x_package_XPackageGetMountPathSize,
    x_package_XPackageGetMountPath,
    x_package_XPackageCloseMountHandle,
    __PADDING_4__,
    x_package_XPackageEnumeratePackages,
    x_package_XPackageRegisterPackageInstalled,
    x_package_XPackageGetWriteStats,
    __PADDING_5__,
    x_package_XPackageUninstallUWPInstance,
    x_package_XPackageEnumerateFeatures,
    x_package_XPackageUninstallPackage,
    /* IXPackageImpl2 methods */
    x_package_XPackageEnumeratePackages2,
    x_package_XPackageRegisterPackageInstalled2,
    x_package_XPackageMountWithUiAsync,
    x_package_XPackageMountWithUiResult,
    /* IXPackageImpl3 methods */
    x_package_XPackageEnumeratePackages3,
    x_package_XPackageRegisterPackageInstalled3,
    /* IXPackageImpl4 methods */
    x_package_XPackageGetKind,
};

static struct x_package x_package =
{
    {&x_package_vtbl},
    0,
};

IXPackageImpl *x_package_impl = (IXPackageImpl *)&x_package.IXPackageImpl4_iface;

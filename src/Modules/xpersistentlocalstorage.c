/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XPersistentLocalStorage
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

struct x_persistent_local_storage
{
    IXPersistentLocalStorageImpl3 IXPersistentLocalStorageImpl3_iface;
    LONG ref;
};

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static inline struct x_persistent_local_storage *impl_from_IXPersistentLocalStorageImpl3( IXPersistentLocalStorageImpl3 *iface )
{
    return CONTAINING_RECORD( iface, struct x_persistent_local_storage, IXPersistentLocalStorageImpl3_iface );
}

static HRESULT WINAPI x_persistent_local_storage_QueryInterface( IXPersistentLocalStorageImpl3 *iface, REFIID iid, void **out )
{
    struct x_persistent_local_storage *impl = impl_from_IXPersistentLocalStorageImpl3( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown                      ) ||
        IsEqualGUID( iid, &IID_IXPersistentLocalStorageImpl  ) ||
        IsEqualGUID( iid, &IID_IXPersistentLocalStorageImpl2 ) ||
        IsEqualGUID( iid, &IID_IXPersistentLocalStorageImpl3 ))
    {
        IXPersistentLocalStorageImpl_AddRef( *out = &impl->IXPersistentLocalStorageImpl3_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_persistent_local_storage_AddRef( IXPersistentLocalStorageImpl3 *iface )
{
    struct x_persistent_local_storage *impl = impl_from_IXPersistentLocalStorageImpl3( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_persistent_local_storage_Release( IXPersistentLocalStorageImpl3 *iface )
{
    struct x_persistent_local_storage *impl = impl_from_IXPersistentLocalStorageImpl3( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/* Builds (and creates) the title's persistent storage directory in UTF-8. */
HRESULT gdkc_get_storage_path( char *out, SIZE_T size, SIZE_T *used )
{
    char family[256], base[MAX_PATH], path[MAX_PATH * 2];
    DWORD len;
    char *p;

    if (!gdkc_config_get_package_family_name( family, sizeof(family) )) return E_GAMERUNTIME_GAMECONFIG_BAD_FORMAT;
    len = GetEnvironmentVariableA( "LOCALAPPDATA", base, sizeof(base) );
    if (!len || len >= sizeof(base)) return HRESULT_FROM_WIN32( ERROR_ENVVAR_NOT_FOUND );

    snprintf( path, sizeof(path), "%s\\Packages\\%s\\LocalState", base, family );

    /* Create every missing component of the path. */
    for (p = path + 3; *p; p++)
    {
        if (*p != '\\') continue;
        *p = 0;
        CreateDirectoryA( path, NULL );
        *p = '\\';
    }
    CreateDirectoryA( path, NULL );

    *used = strlen( path ) + 1;
    if (out)
    {
        if (*used > size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
        memcpy( out, path, *used );
    }
    return S_OK;
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStorageGetPath( IXPersistentLocalStorageImpl3 *iface, SIZE_T pathSize, char *path, SIZE_T *pathUsed )
{
    SIZE_T used;
    HRESULT hr;

    TRACE( "iface %p, pathSize %Iu, path %p, pathUsed %p.\n", iface, pathSize, path, pathUsed );

    if (!path) return E_INVALIDARG;
    if (SUCCEEDED(hr = gdkc_get_storage_path( path, pathSize, &used )) && pathUsed) *pathUsed = used;
    return hr;
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStorageGetPathSize( IXPersistentLocalStorageImpl3 *iface, SIZE_T *pathSize )
{
    TRACE( "iface %p, pathSize %p.\n", iface, pathSize );

    if (!pathSize) return E_INVALIDARG;
    return gdkc_get_storage_path( NULL, 0, pathSize );
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStorageGetSpaceInfo( IXPersistentLocalStorageImpl3 *iface, XPersistentLocalStorageSpaceInfo *info )
{
    ULARGE_INTEGER available, total, free;
    char path[MAX_PATH * 2];
    SIZE_T used;
    HRESULT hr;

    TRACE( "iface %p, info %p.\n", iface, info );

    if (!info) return E_INVALIDARG;
    if (FAILED(hr = gdkc_get_storage_path( path, sizeof(path), &used ))) return hr;
    if (!GetDiskFreeSpaceExA( path, &available, &total, &free )) return HRESULT_FROM_WIN32( GetLastError() );

    info->availableFreeBytes = available.QuadPart;
    info->totalFreeBytes = free.QuadPart;
    info->totalBytes = total.QuadPart;
    info->usedBytes = total.QuadPart - free.QuadPart;
    return S_OK;
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStorageMountForPackage( IXPersistentLocalStorageImpl3 *iface, const char *packageIdentifier, XPackageMountHandle *mountHandle )
{
    FIXME( "iface %p, packageIdentifier %s, mountHandle %p stub!\n", iface, debugstr_a( packageIdentifier ), mountHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStoragePromptUserForSpaceAsync( IXPersistentLocalStorageImpl3 *iface, UINT64 requestedBytes, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, requestedBytes %llu, asyncBlock %p stub!\n", iface, requestedBytes, asyncBlock );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_persistent_local_storage_XPersistentLocalStoragePromptUserForSpaceResult( IXPersistentLocalStorageImpl3 *iface, XAsyncBlock *asyncBlock )
{
    FIXME( "iface %p, asyncBlock %p stub!\n", iface, asyncBlock );
    return E_NOTIMPL;
}

static const struct IXPersistentLocalStorageImpl3Vtbl x_persistent_local_storage_vtbl =
{
    x_persistent_local_storage_QueryInterface,
    x_persistent_local_storage_AddRef,
    x_persistent_local_storage_Release,
    /* IXPersistentLocalStorageImpl/IXPersistentLocalStorageImpl2 methods */
    x_persistent_local_storage_XPersistentLocalStorageGetPathSize,
    x_persistent_local_storage_XPersistentLocalStorageGetPath,
    x_persistent_local_storage_XPersistentLocalStorageGetSpaceInfo,
    x_persistent_local_storage_XPersistentLocalStoragePromptUserForSpaceAsync,
    x_persistent_local_storage_XPersistentLocalStoragePromptUserForSpaceResult,
    /* IXPersistentLocalStorageImpl3 methods */
    x_persistent_local_storage_XPersistentLocalStorageMountForPackage,
};

static struct x_persistent_local_storage x_persistent_local_storage =
{
    {&x_persistent_local_storage_vtbl},
    0,
};

IXPersistentLocalStorageImpl *x_persistent_local_storage_impl = (IXPersistentLocalStorageImpl *)&x_persistent_local_storage.IXPersistentLocalStorageImpl3_iface;

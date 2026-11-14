/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XGameSave and XGameSaveFiles
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

struct x_game_save
{
    IXGameSaveImpl3 IXGameSaveImpl3_iface;
    LONG ref;
};

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static inline struct x_game_save *impl_from_IXGameSaveImpl3( IXGameSaveImpl3 *iface )
{
    return CONTAINING_RECORD( iface, struct x_game_save, IXGameSaveImpl3_iface );
}

static HRESULT WINAPI x_game_save_QueryInterface( IXGameSaveImpl3 *iface, REFIID iid, void **out )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown        ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl  ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl2 ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl3 ))
    {
        IXGameSaveImpl_AddRef( *out = &impl->IXGameSaveImpl3_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_game_save_AddRef( IXGameSaveImpl3 *iface )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_game_save_Release( IXGameSaveImpl3 *iface )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/*
 * Game save storage backed by plain files.
 *
 * Layout: <persistent storage>\GameSave\<xuid>\<configuration id>\<container>\<blob>
 * Container and blob names are percent encoded so that any name is a valid file name.
 * The optional container display name is kept in the container's ".displayname" file.
 */

#define GAMESAVE_QUOTA (256LL * 1024 * 1024)
#define DISPLAYNAME_FILE ".displayname"
#define PATH_SIZE (MAX_PATH * 2)

struct XGameSaveProvider
{
    char root[PATH_SIZE];
};

struct XGameSaveContainer
{
    char dir[PATH_SIZE];
    char name[260];
};

struct update_op
{
    char *name;
    BYTE *data;
    SIZE_T size;
    BOOL remove;
};

struct XGameSaveUpdate
{
    struct XGameSaveContainer container;
    char display_name[260];
    struct update_op *ops;
    SIZE_T count;
    SIZE_T capacity;
};

struct blob
{
    char *name;
    BYTE *data;
    SIZE_T size;
};

static BOOL encode_name( const char *name, char *out, SIZE_T size )
{
    static const char hex[] = "0123456789ABCDEF";
    SIZE_T len = 0;

    if (!name || !*name) return FALSE;
    for (; *name; name++)
    {
        BYTE c = *name;
        BOOL plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     c == '-' || c == '_' || (c == '.' && len);
        if (len + (plain ? 1 : 3) + 1 > size) return FALSE;
        if (plain) out[len++] = c;
        else
        {
            out[len++] = '%';
            out[len++] = hex[c >> 4];
            out[len++] = hex[c & 15];
        }
    }
    out[len] = 0;
    return TRUE;
}

static int hex_value( char c )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static BOOL decode_name( const char *name, char *out, SIZE_T size )
{
    SIZE_T len = 0;

    for (; *name; name++)
    {
        if (len + 1 >= size) return FALSE;
        if (*name == '%' && hex_value( name[1] ) >= 0 && hex_value( name[2] ) >= 0)
        {
            out[len++] = (hex_value( name[1] ) << 4) | hex_value( name[2] );
            name += 2;
        }
        else out[len++] = *name;
    }
    out[len] = 0;
    return TRUE;
}

static void create_directories( char *path )
{
    char *p;

    for (p = path + 3; *p; p++)
    {
        if (*p != '\\') continue;
        *p = 0;
        CreateDirectoryA( path, NULL );
        *p = '\\';
    }
    CreateDirectoryA( path, NULL );
}

static BOOL join_path( char *out, const char *dir, const char *name )
{
    char encoded[PATH_SIZE];
    int len;

    if (!encode_name( name, encoded, sizeof(encoded) )) return FALSE;
    len = snprintf( out, PATH_SIZE, "%s\\%s", dir, encoded );
    return len > 0 && len < PATH_SIZE;
}

static BOOL read_file_data( const char *path, BYTE **data, SIZE_T *size )
{
    LARGE_INTEGER file_size;
    DWORD read = 0;
    HANDLE file;
    BYTE *buffer;

    file = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx( file, &file_size ) || file_size.QuadPart > 0x7fffffff ||
        !(buffer = HeapAlloc( GetProcessHeap(), 0, file_size.QuadPart + 1 )))
    {
        CloseHandle( file );
        return FALSE;
    }
    if (!ReadFile( file, buffer, file_size.QuadPart, &read, NULL ) || read != file_size.QuadPart)
    {
        HeapFree( GetProcessHeap(), 0, buffer );
        CloseHandle( file );
        return FALSE;
    }
    CloseHandle( file );
    *data = buffer;
    *size = read;
    return TRUE;
}

static BOOL write_file_data( const char *path, const BYTE *data, SIZE_T size )
{
    DWORD written;
    HANDLE file;
    BOOL ok;

    file = CreateFileA( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = !size || (WriteFile( file, data, size, &written, NULL ) && written == size);
    ok = CloseHandle( file ) && ok;
    return ok;
}

static time_t filetime_to_time( const FILETIME *ft )
{
    ULARGE_INTEGER value;
    value.LowPart = ft->dwLowDateTime;
    value.HighPart = ft->dwHighDateTime;
    return (value.QuadPart - 116444736000000000ULL) / 10000000ULL;
}

static HRESULT provider_create( XUserHandle user, const char *configuration_id, struct XGameSaveProvider **out )
{
    struct XGameSaveProvider *provider;
    char storage[PATH_SIZE], config[260];
    UINT64 xuid = 0;
    SIZE_T used;
    HRESULT hr;
    int len;

    if (!user || !configuration_id || !out) return E_INVALIDARG;
    if (FAILED(hr = IXUserImpl_XUserGetId( x_user_impl, user, &xuid ))) return hr;
    if (FAILED(hr = gdkc_get_storage_path( storage, sizeof(storage), &used ))) return hr;
    if (!encode_name( configuration_id, config, sizeof(config) )) return E_INVALIDARG;
    if (!(provider = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*provider) ))) return E_OUTOFMEMORY;

    len = snprintf( provider->root, sizeof(provider->root), "%s\\GameSave\\%I64u\\%s", storage, xuid, config );
    if (len <= 0 || len >= (int)sizeof(provider->root))
    {
        HeapFree( GetProcessHeap(), 0, provider );
        return E_INVALIDARG;
    }
    create_directories( provider->root );

    *out = provider;
    return S_OK;
}

static HRESULT container_info( const struct XGameSaveProvider *provider, const char *encoded, XGameSaveContainerInfoCallback *callback, void *context, BOOL *stop )
{
    char name[260], display[260], dir[PATH_SIZE], pattern[PATH_SIZE], path[PATH_SIZE];
    XGameSaveContainerInfo info = {0};
    WIN32_FIND_DATAA find;
    FILETIME newest = {0};
    HANDLE handle;
    BYTE *data;
    SIZE_T size;

    if (!decode_name( encoded, name, sizeof(name) )) return S_OK;
    snprintf( dir, sizeof(dir), "%s\\%s", provider->root, encoded );
    snprintf( pattern, sizeof(pattern), "%s\\*", dir );
    strcpy( display, name );

    if ((handle = FindFirstFileA( pattern, &find )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!strcmp( find.cFileName, DISPLAYNAME_FILE ))
            {
                snprintf( path, sizeof(path), "%s\\%s", dir, find.cFileName );
                if (read_file_data( path, &data, &size ))
                {
                    if (size && size < sizeof(display))
                    {
                        memcpy( display, data, size );
                        display[size] = 0;
                    }
                    HeapFree( GetProcessHeap(), 0, data );
                }
                continue;
            }
            if (find.cFileName[0] == '.') continue;
            info.blobCount++;
            info.totalSize += ((UINT64)find.nFileSizeHigh << 32) | find.nFileSizeLow;
            if (CompareFileTime( &find.ftLastWriteTime, &newest ) > 0) newest = find.ftLastWriteTime;
        } while (FindNextFileA( handle, &find ));
        FindClose( handle );
    }

    info.name = name;
    info.displayName = display;
    info.lastModifiedTime = (newest.dwLowDateTime || newest.dwHighDateTime) ? filetime_to_time( &newest ) : 0;
    info.needsSync = FALSE;

    if (!callback( &info, context )) *stop = TRUE;
    return S_OK;
}

static HRESULT enumerate_containers( const struct XGameSaveProvider *provider, const char *prefix, const char *only, void *context, XGameSaveContainerInfoCallback *callback )
{
    char pattern[PATH_SIZE], decoded[260];
    WIN32_FIND_DATAA find;
    HANDLE handle;
    BOOL stop = FALSE, found = FALSE;

    if (!provider || !callback) return E_INVALIDARG;

    snprintf( pattern, sizeof(pattern), "%s\\*", provider->root );
    if ((handle = FindFirstFileA( pattern, &find )) == INVALID_HANDLE_VALUE) return only ? HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND ) : S_OK;
    do
    {
        if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || find.cFileName[0] == '.') continue;
        if (!decode_name( find.cFileName, decoded, sizeof(decoded) )) continue;
        if (only && strcmp( decoded, only )) continue;
        if (prefix && strncmp( decoded, prefix, strlen( prefix ) )) continue;
        found = TRUE;
        container_info( provider, find.cFileName, callback, context, &stop );
    } while (!stop && FindNextFileA( handle, &find ));
    FindClose( handle );

    return (only && !found) ? HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND ) : S_OK;
}

static HRESULT enumerate_blobs( const struct XGameSaveContainer *container, const char *prefix, void *context, XGameSaveBlobInfoCallback *callback )
{
    char pattern[PATH_SIZE], decoded[260];
    WIN32_FIND_DATAA find;
    HANDLE handle;

    if (!container || !callback) return E_INVALIDARG;

    snprintf( pattern, sizeof(pattern), "%s\\*", container->dir );
    if ((handle = FindFirstFileA( pattern, &find )) == INVALID_HANDLE_VALUE) return S_OK;
    do
    {
        XGameSaveBlobInfo info;

        if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || find.cFileName[0] == '.') continue;
        if (!decode_name( find.cFileName, decoded, sizeof(decoded) )) continue;
        if (prefix && strncmp( decoded, prefix, strlen( prefix ) )) continue;

        info.name = decoded;
        info.size = find.nFileSizeLow;
        if (!callback( &info, context )) break;
    } while (FindNextFileA( handle, &find ));
    FindClose( handle );
    return S_OK;
}

static void blobs_free( struct blob *blobs, UINT32 count )
{
    UINT32 i;

    if (!blobs) return;
    for (i = 0; i < count; i++)
    {
        HeapFree( GetProcessHeap(), 0, blobs[i].name );
        HeapFree( GetProcessHeap(), 0, blobs[i].data );
    }
    HeapFree( GetProcessHeap(), 0, blobs );
}

struct name_list
{
    char **names;
    UINT32 count;
    UINT32 capacity;
};

static BOOLEAN WINAPI collect_blob_name( const XGameSaveBlobInfo *info, void *context )
{
    struct name_list *list = context;

    if (list->count == list->capacity)
    {
        UINT32 capacity = list->capacity ? list->capacity * 2 : 16;
        char **names = HeapReAlloc( GetProcessHeap(), 0, list->names, capacity * sizeof(*names) );
        if (!names && !list->names) names = HeapAlloc( GetProcessHeap(), 0, capacity * sizeof(*names) );
        if (!names) return FALSE;
        list->names = names;
        list->capacity = capacity;
    }
    list->names[list->count] = HeapAlloc( GetProcessHeap(), 0, strlen( info->name ) + 1 );
    if (!list->names[list->count]) return FALSE;
    strcpy( list->names[list->count++], info->name );
    return TRUE;
}

/* Loads the named blobs (or all blobs when names is NULL) and reports the buffer size they need. */
static HRESULT blobs_load( const struct XGameSaveContainer *container, const char **names, UINT32 count, struct blob **out, UINT32 *out_count, SIZE_T *required )
{
    struct name_list all = {0};
    struct blob *blobs;
    UINT32 i;

    if (!container || !out || !out_count || !required) return E_INVALIDARG;

    if (!names)
    {
        enumerate_blobs( container, NULL, &all, collect_blob_name );
        names = (const char **)all.names;
        count = all.count;
    }

    *out = NULL;
    *out_count = 0;
    *required = 0;
    if (!count)
    {
        HeapFree( GetProcessHeap(), 0, all.names );
        return S_OK;
    }

    if (!(blobs = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(*blobs) ))) return E_OUTOFMEMORY;
    *required = count * sizeof(XGameSaveBlob);

    for (i = 0; i < count; i++)
    {
        char path[PATH_SIZE];

        if (!join_path( path, container->dir, names[i] ) || !read_file_data( path, &blobs[i].data, &blobs[i].size ))
        {
            blobs_free( blobs, i );
            for (i = 0; all.names && i < all.count; i++) HeapFree( GetProcessHeap(), 0, all.names[i] );
            HeapFree( GetProcessHeap(), 0, all.names );
            return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
        }
        blobs[i].name = HeapAlloc( GetProcessHeap(), 0, strlen( names[i] ) + 1 );
        strcpy( blobs[i].name, names[i] );
        *required += strlen( names[i] ) + 1 + blobs[i].size;
    }

    for (i = 0; all.names && i < all.count; i++) HeapFree( GetProcessHeap(), 0, all.names[i] );
    HeapFree( GetProcessHeap(), 0, all.names );

    *out = blobs;
    *out_count = count;
    return S_OK;
}

/* Copies loaded blobs into the caller's buffer: the blob array first, names and data after it. */
static void blobs_copy( const struct blob *blobs, UINT32 count, XGameSaveBlob *buffer )
{
    BYTE *cursor = (BYTE *)(buffer + count);
    UINT32 i;

    for (i = 0; i < count; i++)
    {
        size_t name_len = strlen( blobs[i].name ) + 1;

        memcpy( cursor, blobs[i].name, name_len );
        buffer[i].info.name = (const char *)cursor;
        buffer[i].info.size = blobs[i].size;
        cursor += name_len;

        memcpy( cursor, blobs[i].data, blobs[i].size );
        buffer[i].data = cursor;
        cursor += blobs[i].size;
    }
}

static void update_free( struct XGameSaveUpdate *update )
{
    SIZE_T i;

    for (i = 0; i < update->count; i++)
    {
        HeapFree( GetProcessHeap(), 0, update->ops[i].name );
        HeapFree( GetProcessHeap(), 0, update->ops[i].data );
    }
    HeapFree( GetProcessHeap(), 0, update->ops );
    HeapFree( GetProcessHeap(), 0, update );
}

static HRESULT update_add( struct XGameSaveUpdate *update, const char *name, const BYTE *data, SIZE_T size, BOOL remove )
{
    struct update_op *op;
    char encoded[PATH_SIZE];

    if (!update || !encode_name( name, encoded, sizeof(encoded) )) return E_INVALIDARG;

    if (update->count == update->capacity)
    {
        SIZE_T capacity = update->capacity ? update->capacity * 2 : 8;
        struct update_op *ops = update->ops ? HeapReAlloc( GetProcessHeap(), 0, update->ops, capacity * sizeof(*ops) )
                                            : HeapAlloc( GetProcessHeap(), 0, capacity * sizeof(*ops) );
        if (!ops) return E_OUTOFMEMORY;
        update->ops = ops;
        update->capacity = capacity;
    }

    op = &update->ops[update->count];
    memset( op, 0, sizeof(*op) );
    op->remove = remove;
    op->size = size;
    if (!(op->name = HeapAlloc( GetProcessHeap(), 0, strlen( name ) + 1 ))) return E_OUTOFMEMORY;
    strcpy( op->name, name );
    if (!remove)
    {
        if (!(op->data = HeapAlloc( GetProcessHeap(), 0, size + 1 )))
        {
            HeapFree( GetProcessHeap(), 0, op->name );
            return E_OUTOFMEMORY;
        }
        if (size) memcpy( op->data, data, size );
    }
    update->count++;
    return S_OK;
}

static HRESULT update_submit( const struct XGameSaveUpdate *update )
{
    char path[PATH_SIZE], temp[PATH_SIZE], encoded[PATH_SIZE], dir[PATH_SIZE];
    SIZE_T i;

    if (!update) return E_INVALIDARG;

    strcpy( dir, update->container.dir );
    create_directories( dir );

    if (update->display_name[0])
    {
        snprintf( path, sizeof(path), "%s\\%s", dir, DISPLAYNAME_FILE );
        write_file_data( path, (const BYTE *)update->display_name, strlen( update->display_name ) );
    }

    for (i = 0; i < update->count; i++)
    {
        const struct update_op *op = &update->ops[i];

        if (!join_path( path, dir, op->name )) return E_INVALIDARG;
        if (op->remove)
        {
            DeleteFileA( path );
            continue;
        }

        encode_name( op->name, encoded, sizeof(encoded) );
        snprintf( temp, sizeof(temp), "%s\\.tmp-%s", dir, encoded );
        if (!write_file_data( temp, op->data, op->size ) ||
            !MoveFileExA( temp, path, MOVEFILE_REPLACE_EXISTING ))
        {
            DeleteFileA( temp );
            return HRESULT_FROM_WIN32( GetLastError() );
        }
    }
    return S_OK;
}

static HRESULT delete_container( const struct XGameSaveProvider *provider, const char *name )
{
    char dir[PATH_SIZE], pattern[PATH_SIZE], path[PATH_SIZE];
    WIN32_FIND_DATAA find;
    HANDLE handle;

    if (!provider || !join_path( dir, provider->root, name )) return E_INVALIDARG;

    snprintf( pattern, sizeof(pattern), "%s\\*", dir );
    if ((handle = FindFirstFileA( pattern, &find )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            snprintf( path, sizeof(path), "%s\\%s", dir, find.cFileName );
            DeleteFileA( path );
        } while (FindNextFileA( handle, &find ));
        FindClose( handle );
    }
    RemoveDirectoryA( dir );
    return S_OK;
}

/*
 * Asynchronous variants do their work when they are called and only report the outcome through
 * the XAsync machinery.
 */

enum request_kind
{
    REQUEST_INIT,
    REQUEST_QUOTA,
    REQUEST_VOID,
    REQUEST_READ,
};

struct request
{
    enum request_kind kind;
    HRESULT hr;
    SIZE_T required;
    struct XGameSaveProvider *provider;
    INT64 quota;
    struct blob *blobs;
    UINT32 count;
};

static HRESULT WINAPI request_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct request *request = data->context;

    switch (op)
    {
        case XAsyncOp_Begin:
            return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );

        case XAsyncOp_DoWork:
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, request->hr, SUCCEEDED(request->hr) ? request->required : 0 );
            return S_OK;

        case XAsyncOp_GetResult:
            if (data->bufferSize < request->required) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            if (request->kind == REQUEST_INIT) *(struct XGameSaveProvider **)data->buffer = request->provider;
            else if (request->kind == REQUEST_QUOTA) *(INT64 *)data->buffer = request->quota;
            else if (request->kind == REQUEST_READ) blobs_copy( request->blobs, request->count, data->buffer );
            return S_OK;

        case XAsyncOp_Cleanup:
            blobs_free( request->blobs, request->count );
            HeapFree( GetProcessHeap(), 0, request );
            return S_OK;

        default:
            return S_OK;
    }
}

static HRESULT request_begin( XAsyncBlock *async, struct request *request, const void *identity, const char *name )
{
    HRESULT hr;

    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, request, identity, name, request_provider )))
    {
        blobs_free( request->blobs, request->count );
        HeapFree( GetProcessHeap(), 0, request );
    }
    return hr;
}

static struct request *request_create( enum request_kind kind )
{
    struct request *request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) );
    if (request) request->kind = kind;
    return request;
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProvider( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, BOOLEAN syncOnDemand, XGameSaveProviderHandle *provider )
{
    TRACE( "iface %p, requestingUser %p, configurationId %s, syncOnDemand %d, provider %p.\n", iface, requestingUser, debugstr_a( configurationId ), syncOnDemand, provider );

    if (!provider) return E_INVALIDARG;
    return provider_create( requestingUser, configurationId, provider );
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProviderAsync( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, BOOLEAN syncOnDemand, XAsyncBlock *async )
{
    struct request *request;

    TRACE( "iface %p, requestingUser %p, configurationId %s, syncOnDemand %d, async %p.\n", iface, requestingUser, debugstr_a( configurationId ), syncOnDemand, async );

    if (!async) return E_INVALIDARG;
    if (!(request = request_create( REQUEST_INIT ))) return E_OUTOFMEMORY;
    request->hr = provider_create( requestingUser, configurationId, &request->provider );
    request->required = sizeof(request->provider);
    return request_begin( async, request, x_game_save_XGameSaveInitializeProviderAsync, "XGameSaveInitializeProviderAsync" );
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProviderResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, XGameSaveProviderHandle *provider )
{
    TRACE( "iface %p, async %p, provider %p.\n", iface, async, provider );

    if (!provider) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_game_save_XGameSaveInitializeProviderAsync, sizeof(*provider), provider, NULL );
}

static void WINAPI x_game_save_XGameSaveCloseProvider( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider )
{
    TRACE( "iface %p, provider %p.\n", iface, provider );
    HeapFree( GetProcessHeap(), 0, provider );
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuota( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, INT64 *remainingQuota )
{
    TRACE( "iface %p, provider %p, remainingQuota %p.\n", iface, provider, remainingQuota );

    if (!provider || !remainingQuota) return E_INVALIDARG;
    *remainingQuota = GAMESAVE_QUOTA;
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuotaAsync( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, XAsyncBlock *async )
{
    struct request *request;

    TRACE( "iface %p, provider %p, async %p.\n", iface, provider, async );

    if (!provider || !async) return E_INVALIDARG;
    if (!(request = request_create( REQUEST_QUOTA ))) return E_OUTOFMEMORY;
    request->quota = GAMESAVE_QUOTA;
    request->required = sizeof(request->quota);
    return request_begin( async, request, x_game_save_XGameSaveGetRemainingQuotaAsync, "XGameSaveGetRemainingQuotaAsync" );
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuotaResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, INT64 *remainingQuota )
{
    TRACE( "iface %p, async %p, remainingQuota %p.\n", iface, async, remainingQuota );

    if (!remainingQuota) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_game_save_XGameSaveGetRemainingQuotaAsync, sizeof(*remainingQuota), remainingQuota, NULL );
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainer( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName )
{
    TRACE( "iface %p, provider %p, containerName %s.\n", iface, provider, debugstr_a( containerName ) );
    return delete_container( provider, containerName );
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainerAsync( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, XAsyncBlock *async )
{
    struct request *request;

    TRACE( "iface %p, provider %p, containerName %s, async %p.\n", iface, provider, debugstr_a( containerName ), async );

    if (!async) return E_INVALIDARG;
    if (!(request = request_create( REQUEST_VOID ))) return E_OUTOFMEMORY;
    request->hr = delete_container( provider, containerName );
    return request_begin( async, request, x_game_save_XGameSaveDeleteContainerAsync, "XGameSaveDeleteContainerAsync" );
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainerResult( IXGameSaveImpl3 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_game_save_XGameSaveDeleteContainerAsync, 0, NULL, NULL );
}

static HRESULT WINAPI x_game_save_XGameSaveGetContainerInfo( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, void *context, XGameSaveContainerInfoCallback *callback )
{
    TRACE( "iface %p, provider %p, containerName %s, context %p, callback %p.\n", iface, provider, debugstr_a( containerName ), context, callback );

    if (!containerName) return E_INVALIDARG;
    return enumerate_containers( provider, NULL, containerName, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateContainerInfo( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, void *context, XGameSaveContainerInfoCallback *callback )
{
    TRACE( "iface %p, provider %p, context %p, callback %p.\n", iface, provider, context, callback );
    return enumerate_containers( provider, NULL, NULL, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateContainerInfoByName( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerNamePrefix, void *context, XGameSaveContainerInfoCallback *callback )
{
    TRACE( "iface %p, provider %p, containerNamePrefix %s, context %p, callback %p.\n", iface, provider, debugstr_a( containerNamePrefix ), context, callback );
    return enumerate_containers( provider, containerNamePrefix, NULL, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveCreateContainer( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, XGameSaveContainerHandle *containerContext )
{
    struct XGameSaveContainer *container;

    TRACE( "iface %p, provider %p, containerName %s, containerContext %p.\n", iface, provider, debugstr_a( containerName ), containerContext );

    if (!provider || !containerName || !containerContext || strlen( containerName ) >= sizeof(container->name)) return E_INVALIDARG;
    if (!(container = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*container) ))) return E_OUTOFMEMORY;

    strcpy( container->name, containerName );
    if (!join_path( container->dir, provider->root, containerName ))
    {
        HeapFree( GetProcessHeap(), 0, container );
        return E_INVALIDARG;
    }

    *containerContext = container;
    return S_OK;
}

static void WINAPI x_game_save_XGameSaveCloseContainer( IXGameSaveImpl3 *iface, XGameSaveContainerHandle context )
{
    TRACE( "iface %p, context %p.\n", iface, context );
    HeapFree( GetProcessHeap(), 0, context );
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateBlobInfo( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, void *context, XGameSaveBlobInfoCallback *callback )
{
    TRACE( "iface %p, container %p, context %p, callback %p.\n", iface, container, context, callback );
    return enumerate_blobs( container, NULL, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateBlobInfoByName( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char *blobNamePrefix, void *context, XGameSaveBlobInfoCallback *callback )
{
    TRACE( "iface %p, container %p, blobNamePrefix %s, context %p, callback %p.\n", iface, container, debugstr_a( blobNamePrefix ), context, callback );
    return enumerate_blobs( container, blobNamePrefix, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobData( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char **blobNames, UINT32 *countOfBlobs, SIZE_T blobsSize, XGameSaveBlob *blobData )
{
    struct blob *blobs;
    SIZE_T required;
    UINT32 count;
    HRESULT hr;

    TRACE( "iface %p, container %p, blobNames %p, countOfBlobs %p, blobsSize %Iu, blobData %p.\n", iface, container, blobNames, countOfBlobs, blobsSize, blobData );

    if (!countOfBlobs) return E_INVALIDARG;
    if (FAILED(hr = blobs_load( container, blobNames, blobNames ? *countOfBlobs : 0, &blobs, &count, &required ))) return hr;

    if (!blobData || blobsSize < required)
        hr = HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    else
        blobs_copy( blobs, count, blobData );

    *countOfBlobs = count;
    blobs_free( blobs, count );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobDataAsync( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char **blobNames, UINT32 countOfBlobs, XAsyncBlock *async )
{
    struct request *request;

    TRACE( "iface %p, container %p, blobNames %p, countOfBlobs %u, async %p.\n", iface, container, blobNames, countOfBlobs, async );

    if (!async) return E_INVALIDARG;
    if (!(request = request_create( REQUEST_READ ))) return E_OUTOFMEMORY;
    request->hr = blobs_load( container, blobNames, blobNames ? countOfBlobs : 0, &request->blobs, &request->count, &request->required );
    return request_begin( async, request, x_game_save_XGameSaveReadBlobDataAsync, "XGameSaveReadBlobDataAsync" );
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobDataResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, SIZE_T blobsSize, XGameSaveBlob *blobData, UINT32 *countOfBlobs )
{
    struct request *request = gdkc_async_context( async );
    UINT32 count;

    TRACE( "iface %p, async %p, blobsSize %Iu, blobData %p, countOfBlobs %p.\n", iface, async, blobsSize, blobData, countOfBlobs );

    if (!request || !blobData) return E_INVALIDARG;
    count = request->count;
    if (countOfBlobs) *countOfBlobs = count;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_game_save_XGameSaveReadBlobDataAsync, blobsSize, blobData, NULL );
}

static HRESULT WINAPI x_game_save_XGameSaveCreateUpdate( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char *containerDisplayName, XGameSaveUpdateHandle *updateContext )
{
    struct XGameSaveUpdate *update;

    TRACE( "iface %p, container %p, containerDisplayName %s, updateContext %p.\n", iface, container, debugstr_a( containerDisplayName ), updateContext );

    if (!container || !updateContext) return E_INVALIDARG;
    if (!(update = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*update) ))) return E_OUTOFMEMORY;

    update->container = *container;
    if (containerDisplayName && strlen( containerDisplayName ) < sizeof(update->display_name))
        strcpy( update->display_name, containerDisplayName );

    *updateContext = update;
    return S_OK;
}

static void WINAPI x_game_save_XGameSaveCloseUpdate( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle context )
{
    TRACE( "iface %p, context %p.\n", iface, context );
    if (context) update_free( context );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitBlobWrite( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, const char *blobName, UINT8 *data, SIZE_T byteCount )
{
    TRACE( "iface %p, updateContext %p, blobName %s, data %p, byteCount %Iu.\n", iface, updateContext, debugstr_a( blobName ), data, byteCount );
    return update_add( updateContext, blobName, data, byteCount, FALSE );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitBlobDelete( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, const char *blobName )
{
    TRACE( "iface %p, updateContext %p, blobName %s.\n", iface, updateContext, debugstr_a( blobName ) );
    return update_add( updateContext, blobName, NULL, 0, TRUE );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdate( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext )
{
    TRACE( "iface %p, updateContext %p.\n", iface, updateContext );
    return update_submit( updateContext );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdateAsync( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, XAsyncBlock *async )
{
    struct request *request;

    TRACE( "iface %p, updateContext %p, async %p.\n", iface, updateContext, async );

    if (!updateContext || !async) return E_INVALIDARG;
    if (!(request = request_create( REQUEST_VOID ))) return E_OUTOFMEMORY;
    request->hr = update_submit( updateContext );
    return request_begin( async, request, x_game_save_XGameSaveSubmitUpdateAsync, "XGameSaveSubmitUpdateAsync" );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdateResult( IXGameSaveImpl3 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_game_save_XGameSaveSubmitUpdateAsync, 0, NULL, NULL );
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiAsync( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, XAsyncBlock *async )
{
    FIXME( "iface %p, requestingUser %p, configurationId %s, async %p stub!\n", iface, requestingUser, debugstr_a( configurationId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, SIZE_T folderSize, char *folderResult )
{
    FIXME( "iface %p, async %p, folderSize %Iu, folderResult %p stub!\n", iface, async, folderSize, folderResult );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetRemainingQuota( IXGameSaveImpl3 *iface, XUserHandle userContext, const char *configurationId, INT64 *remainingQuota )
{
    TRACE( "iface %p, userContext %p, configurationId %s, remainingQuota %p.\n", iface, userContext, debugstr_a( configurationId ), remainingQuota );

    if (!userContext || !remainingQuota) return E_INVALIDARG;
    *remainingQuota = GAMESAVE_QUOTA;
    return S_OK;
}

static const struct IXGameSaveImpl3Vtbl x_game_save_vtbl =
{
    x_game_save_QueryInterface,
    x_game_save_AddRef,
    x_game_save_Release,
    /* IXGameSaveImpl methods */
    x_game_save_XGameSaveInitializeProvider,
    x_game_save_XGameSaveInitializeProviderAsync,
    x_game_save_XGameSaveInitializeProviderResult,
    x_game_save_XGameSaveCloseProvider,
    x_game_save_XGameSaveGetRemainingQuota,
    x_game_save_XGameSaveGetRemainingQuotaAsync,
    x_game_save_XGameSaveGetRemainingQuotaResult,
    x_game_save_XGameSaveDeleteContainer,
    x_game_save_XGameSaveDeleteContainerAsync,
    x_game_save_XGameSaveDeleteContainerResult,
    x_game_save_XGameSaveGetContainerInfo,
    x_game_save_XGameSaveEnumerateContainerInfo,
    x_game_save_XGameSaveEnumerateContainerInfoByName,
    x_game_save_XGameSaveCreateContainer,
    x_game_save_XGameSaveCloseContainer,
    x_game_save_XGameSaveEnumerateBlobInfo,
    x_game_save_XGameSaveEnumerateBlobInfoByName,
    x_game_save_XGameSaveReadBlobData,
    x_game_save_XGameSaveReadBlobDataAsync,
    x_game_save_XGameSaveReadBlobDataResult,
    x_game_save_XGameSaveCreateUpdate,
    x_game_save_XGameSaveCloseUpdate,
    x_game_save_XGameSaveSubmitBlobWrite,
    x_game_save_XGameSaveSubmitBlobDelete,
    x_game_save_XGameSaveSubmitUpdate,
    x_game_save_XGameSaveSubmitUpdateAsync,
    x_game_save_XGameSaveSubmitUpdateResult,
    /* IXGameSaveImpl2 methods */
    x_game_save_XGameSaveFilesGetFolderWithUiAsync,
    x_game_save_XGameSaveFilesGetFolderWithUiResult,
    x_game_save_XGameSaveFilesGetRemainingQuota,
};

static struct x_game_save x_game_save =
{
    {&x_game_save_vtbl},
    0,
};

IXGameSaveImpl *x_game_save_impl = (IXGameSaveImpl *)&x_game_save.IXGameSaveImpl3_iface;

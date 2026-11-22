/*
 * Xbox Game runtime Library
 *  Helpers reading the MicrosoftGame.config shipped with a GDK title.
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

#include <bcrypt.h>

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static INIT_ONCE config_once = INIT_ONCE_STATIC_INIT;
static char *config_xml;

static char *read_file( const WCHAR *path )
{
    LARGE_INTEGER size;
    HANDLE file;
    char *data = NULL;
    DWORD read;

    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return NULL;

    if (GetFileSizeEx( file, &size ) && size.QuadPart > 0 && size.QuadPart < 1024 * 1024 &&
        (data = HeapAlloc( GetProcessHeap(), 0, size.QuadPart + 1 )))
    {
        if (!ReadFile( file, data, size.QuadPart, &read, NULL ))
        {
            HeapFree( GetProcessHeap(), 0, data );
            data = NULL;
        }
        else data[read] = 0;
    }
    CloseHandle( file );
    return data;
}

static BOOL CALLBACK load_config( INIT_ONCE *once, void *param, void **context )
{
    WCHAR path[MAX_PATH + 32];
    DWORD len = GetModuleFileNameW( NULL, path, MAX_PATH );
    int i;

    if (!len || len >= MAX_PATH) return TRUE;

    /* Look next to the executable, then in up to two parent directories. */
    for (i = 0; i < 3; i++)
    {
        WCHAR *slash = wcsrchr( path, '\\' );
        if (!slash) break;
        wcscpy( slash + 1, L"MicrosoftGame.config" );
        if ((config_xml = read_file( path )))
        {
            TRACE( "loaded %s.\n", debugstr_w( path ) );
            break;
        }
        *slash = 0;
    }
    return TRUE;
}

static const char *get_config(void)
{
    InitOnceExecuteOnce( &config_once, load_config, NULL, NULL );
    return config_xml;
}

/* Copies the text content of <tag>...</tag> into out. */
static BOOL config_get_tag( const char *tag, char *out, SIZE_T size )
{
    const char *xml = get_config(), *start, *end;
    char open[64];
    SIZE_T len;

    if (!xml) return FALSE;
    snprintf( open, sizeof(open), "<%s>", tag );
    if (!(start = strstr( xml, open ))) return FALSE;
    start += strlen( open );
    if (!(end = strchr( start, '<' ))) return FALSE;

    len = end - start;
    if (len >= size) return FALSE;
    memcpy( out, start, len );
    out[len] = 0;
    return TRUE;
}

/* Copies the value of attribute of the first element into out. */
static BOOL config_get_attr( const char *element, const char *attribute, char *out, SIZE_T size )
{
    const char *xml = get_config(), *start, *end;
    char needle[64];
    SIZE_T len;

    if (!xml) return FALSE;
    snprintf( needle, sizeof(needle), "<%s", element );
    if (!(start = strstr( xml, needle )) || !(end = strchr( start, '>' ))) return FALSE;

    snprintf( needle, sizeof(needle), "%s=\"", attribute );
    if (!(start = strstr( start, needle )) || start > end) return FALSE;
    start += strlen( needle );
    if (!(end = strchr( start, '"' ))) return FALSE;

    len = end - start;
    if (len >= size) return FALSE;
    memcpy( out, start, len );
    out[len] = 0;
    return TRUE;
}

BOOL gdkc_config_available(void)
{
    return get_config() != NULL;
}

BOOL gdkc_config_get_title_id( UINT32 *title_id )
{
    char value[32], *end;

    if (!config_get_tag( "TitleId", value, sizeof(value) )) return FALSE;
    *title_id = strtoul( value, &end, 16 );
    return end != value;
}

/* Derives the 13 character publisher id used in package family and full names. */
static BOOL get_publisher_id( const char *publisher, char out[14] )
{
    static const char alphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
    WCHAR wide[256];
    UCHAR hash[32];
    UINT64 bits = 0;
    int i, len;

    if (!(len = MultiByteToWideChar( CP_UTF8, 0, publisher, -1, wide, ARRAY_SIZE(wide) ))) return FALSE;
    if (BCryptHash( BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (UCHAR *)wide, (len - 1) * sizeof(WCHAR), hash, sizeof(hash) ))
        return FALSE;

    for (i = 0; i < 8; i++) bits = (bits << 8) | hash[i];
    /* 64 bits are encoded as 13 groups of 5 bits; the last group is zero padded. */
    for (i = 0; i < 13; i++)
    {
        int shift = 64 - 5 * (i + 1);
        UINT group = shift >= 0 ? (bits >> shift) & 0x1f : (bits << -shift) & 0x1f;
        out[i] = alphabet[group];
    }
    out[13] = 0;
    return TRUE;
}

/* Builds the package full name: Name_Version_Architecture__PublisherId */
BOOL gdkc_config_get_package_full_name( char *out, SIZE_T size )
{
    char name[128], publisher[128], version[32], publisher_id[14];
    int len;

    if (!config_get_attr( "Identity", "Name", name, sizeof(name) ) ||
        !config_get_attr( "Identity", "Publisher", publisher, sizeof(publisher) ) ||
        !config_get_attr( "Identity", "Version", version, sizeof(version) ) ||
        !get_publisher_id( publisher, publisher_id ))
        return FALSE;

    len = snprintf( out, size, "%s_%s_x64__%s", name, version, publisher_id );
    return len > 0 && (SIZE_T)len < size;
}

/* Builds the package family name: Name_PublisherId */
BOOL gdkc_config_get_package_family_name( char *out, SIZE_T size )
{
    char name[128], publisher[128], publisher_id[14];
    int len;

    if (!config_get_attr( "Identity", "Name", name, sizeof(name) ) ||
        !config_get_attr( "Identity", "Publisher", publisher, sizeof(publisher) ) ||
        !get_publisher_id( publisher, publisher_id ))
        return FALSE;

    len = snprintf( out, size, "%s_%s", name, publisher_id );
    return len > 0 && (SIZE_T)len < size;
}

BOOL gdkc_config_get_store_id( char *out, SIZE_T size )
{
    return config_get_tag( "StoreId", out, size );
}

/* The MSA app id identifies the game to Xbox Live sign-in (title-bound tokens). */
BOOL gdkc_config_get_msa_app_id( char *out, SIZE_T size )
{
    return config_get_tag( "MSAAppId", out, size );
}

BOOL gdkc_config_get_version( XVersion *version )
{
    unsigned int major, minor, build, revision;
    char value[32];

    if (!config_get_attr( "Identity", "Version", value, sizeof(value) )) return FALSE;
    if (sscanf( value, "%u.%u.%u.%u", &major, &minor, &build, &revision ) != 4) return FALSE;
    if (major > 0xffff || minor > 0xffff || build > 0xffff || revision > 0xffff) return FALSE;
    version->major = major;
    version->minor = minor;
    version->build = build;
    version->revision = revision;
    return TRUE;
}

/* Display names may be resource references ("ms-resource:..."), which are of no use to a game. */
static BOOL config_get_display_text( const char *attribute, char *out, SIZE_T size )
{
    return config_get_attr( "ShellVisuals", attribute, out, size ) && strncmp( out, "ms-resource:", 12 );
}

BOOL gdkc_config_get_display_name( char *out, SIZE_T size )
{
    return config_get_display_text( "DefaultDisplayName", out, size );
}

BOOL gdkc_config_get_publisher_display_name( char *out, SIZE_T size )
{
    return config_get_display_text( "PublisherDisplayName", out, size );
}

/*
 * Xbox Game runtime Library
 *  Installed add-ons (DLC) of the running game, as reported by Xodus
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
 * Xodus installs add-ons into folders of their own and reports those of the running game through
 * its bridge (see xodusbridge.c):
 *
 *   POST /packages        answers the current list
 *   POST /packages/wait   body: the known generation; answers the list once it changed (long poll)
 *   POST /store/package-license
 *                         body: a content id; 200 when the user holds a license, 403 otherwise
 *
 * The list is plain text: a generation line, then one add-on per line with the tab separated
 * fields content id, store id, version, Windows path of the files, name and publisher.
 *
 * Games see the content id as the package identifier, in the same form as the identifier of the
 * game itself: 32 lower case hex digits without hyphens.
 */

#include "private.h"

#include <ctype.h>
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

/* How long a fetched list is used before asking again, while nothing watches for changes. */
#define ADDONS_MAX_AGE_MS 2000
/* Pause after a failed wait before the watcher asks again. */
#define WATCH_RETRY_MS 5000

static CRITICAL_SECTION addons_cs;
static void __attribute__((constructor)) init_addons_cs( void )
{
    InitializeCriticalSection( &addons_cs );
}

static struct gdkc_addons *cached;
static DWORD cached_time;
static BOOL watching;

struct license_entry
{
    struct list entry;
    char package_id[GDKC_PACKAGE_ID_SIZE];
    BOOL licensed;
};

static struct list licenses = LIST_INIT( licenses );

struct installed_registration
{
    struct list entry;
    UINT64 token;
    XTaskQueueHandle queue;
    void *context;
    XPackageInstalledCallback *callback;
};

static struct list installed_registrations = LIST_INIT( installed_registrations );

BOOL gdkc_normalize_package_id( const char *id, char out[GDKC_PACKAGE_ID_SIZE] )
{
    int digits = 0;

    if (!id) return FALSE;
    for (; *id; id++)
    {
        if (*id == '-' || *id == '{' || *id == '}') continue;
        if (!isxdigit( (unsigned char)*id ) || digits == 32) return FALSE;
        out[digits++] = tolower( (unsigned char)*id );
    }
    out[digits] = 0;
    return digits == 32;
}

BOOL gdkc_game_package_id( char out[GDKC_PACKAGE_ID_SIZE] )
{
    char id[64];
    DWORD len = GetEnvironmentVariableA( "XODUS_PACKAGE_ID", id, sizeof(id) );

    return len && len < sizeof(id) && gdkc_normalize_package_id( id, out );
}

static BOOL parse_version( const char *text, XVersion *version )
{
    unsigned int parts[4] = {0};
    int i = 0;
    char *end;

    while (i < 4)
    {
        unsigned long value = strtoul( text, &end, 10 );
        if (end == text || value > 0xffff) return FALSE;
        parts[i++] = value;
        if (*end != '.') break;
        text = end + 1;
    }
    if (*end) return FALSE;
    version->major = parts[0];
    version->minor = parts[1];
    version->build = parts[2];
    version->revision = parts[3];
    return TRUE;
}

static BOOL valid_store_id( const char *id )
{
    SIZE_T len = strlen( id ), i;

    if (!len || len >= GDKC_STORE_ID_SIZE) return FALSE;
    for (i = 0; i < len; i++) if (!isalnum( (unsigned char)id[i] )) return FALSE;
    return TRUE;
}

static void free_addons( struct gdkc_addons *addons )
{
    UINT32 i;

    for (i = 0; i < addons->count; i++)
    {
        free( addons->items[i].path );
        free( addons->items[i].name );
        free( addons->items[i].publisher );
    }
    free( addons->items );
    free( addons );
}

void gdkc_addons_release( struct gdkc_addons *addons )
{
    if (addons && !InterlockedDecrement( &addons->ref )) free_addons( addons );
}

/* Splits the next tab separated field off *line. */
static char *next_field( char **line )
{
    char *field = *line, *tab;

    if (!field) return NULL;
    if ((tab = strchr( field, '\t' ))) { *tab = 0; *line = tab + 1; }
    else *line = NULL;
    return field;
}

/* Parses the bridge's list. Lines that do not fit the format are left out. */
static struct gdkc_addons *parse_addons( char *text )
{
    struct gdkc_addons *addons;
    char *line, *next;
    UINT32 lines = 0;

    if (!(addons = calloc( 1, sizeof(*addons) ))) return NULL;
    addons->ref = 1;

    for (line = text; *line; line++) if (*line == '\n') lines++;
    if (lines && !(addons->items = calloc( lines, sizeof(*addons->items) )))
    {
        free( addons );
        return NULL;
    }

    for (line = text; line && *line; line = next)
    {
        struct gdkc_addon *addon = &addons->items[addons->count];
        char *fields[6], *rest;
        int i;

        if ((next = strchr( line, '\n' ))) *next++ = 0;
        if (line == text)
        {
            lstrcpynA( addons->generation, line, sizeof(addons->generation) );
            continue;
        }
        if (addons->count >= lines) break;

        rest = line;
        for (i = 0; i < ARRAY_SIZE(fields); i++) fields[i] = next_field( &rest );
        if (!fields[5] || rest) { WARN( "malformed add-on line %s.\n", debugstr_a( line ) ); continue; }

        if (strlen( fields[0] ) >= sizeof(addon->content_id) ||
            !gdkc_normalize_package_id( fields[0], addon->package_id ) ||
            !valid_store_id( fields[1] ) ||
            !parse_version( fields[2], &addon->version ) || !*fields[3])
        {
            WARN( "invalid add-on line %s.\n", debugstr_a( line ) );
            continue;
        }
        strcpy( addon->content_id, fields[0] );
        strcpy( addon->store_id, fields[1] );
        addon->path = strdup( fields[3] );
        addon->name = strdup( fields[4] );
        addon->publisher = strdup( fields[5] );
        if (!addon->path || !addon->name || !addon->publisher)
        {
            free( addon->path );
            free( addon->name );
            free( addon->publisher );
            memset( addon, 0, sizeof(*addon) );
            continue;
        }
        addons->count++;
    }
    return addons;
}

static struct gdkc_addons *empty_addons(void)
{
    struct gdkc_addons *addons = calloc( 1, sizeof(*addons) );
    if (addons) addons->ref = 1;
    return addons;
}

/* Replaces the cached list; takes over the reference of addons. */
static void set_cached( struct gdkc_addons *addons )
{
    struct gdkc_addons *old;

    EnterCriticalSection( &addons_cs );
    old = cached;
    cached = addons;
    cached_time = GetTickCount();
    LeaveCriticalSection( &addons_cs );
    gdkc_addons_release( old );
}

HRESULT gdkc_addons_get( struct gdkc_addons **out )
{
    struct gdkc_addons *addons;
    char *response;
    HRESULT hr;

    *out = NULL;

    EnterCriticalSection( &addons_cs );
    if (cached && (watching || GetTickCount() - cached_time < ADDONS_MAX_AGE_MS))
    {
        InterlockedIncrement( &cached->ref );
        *out = cached;
    }
    LeaveCriticalSection( &addons_cs );
    if (*out) return S_OK;

    if (!gdkc_bridge_available())
    {
        if (!(*out = empty_addons())) return E_OUTOFMEMORY;
        return S_OK;
    }

    if (FAILED(hr = gdkc_bridge_post( L"/packages", "", &response )))
    {
        WARN( "could not list the add-ons, hr %#lx.\n", hr );
        return hr;
    }
    addons = parse_addons( response );
    free( response );
    if (!addons) return E_OUTOFMEMORY;

    TRACE( "%u add-ons installed.\n", addons->count );
    InterlockedIncrement( &addons->ref );
    set_cached( addons );
    *out = addons;
    return S_OK;
}

const struct gdkc_addon *gdkc_addons_find( const struct gdkc_addons *addons, const char *id )
{
    char package_id[GDKC_PACKAGE_ID_SIZE];
    BOOL by_package = gdkc_normalize_package_id( id, package_id );
    UINT32 i;

    if (!id) return NULL;
    for (i = 0; i < addons->count; i++)
    {
        if (by_package && !strcmp( addons->items[i].package_id, package_id )) return &addons->items[i];
        if (!by_package && !_stricmp( addons->items[i].store_id, id )) return &addons->items[i];
    }
    return NULL;
}

HRESULT gdkc_addon_license( const struct gdkc_addon *addon, BOOL *licensed )
{
    struct license_entry *entry;
    char *response;
    HRESULT hr;

    *licensed = FALSE;

    EnterCriticalSection( &addons_cs );
    LIST_FOR_EACH_ENTRY( entry, &licenses, struct license_entry, entry )
    {
        if (strcmp( entry->package_id, addon->package_id )) continue;
        *licensed = entry->licensed;
        LeaveCriticalSection( &addons_cs );
        return S_OK;
    }
    LeaveCriticalSection( &addons_cs );

    hr = gdkc_bridge_post( L"/store/package-license", addon->content_id, &response );
    if (hr == E_ACCESSDENIED) *licensed = FALSE;
    else if (FAILED(hr))
    {
        WARN( "license check for add-on %s failed, hr %#lx.\n", debugstr_a( addon->content_id ), hr );
        return hr;
    }
    else
    {
        *licensed = TRUE;
        free( response );
    }

    /* The bridge asks the licensing service once per run, so the answer does not change. */
    if ((entry = calloc( 1, sizeof(*entry) )))
    {
        strcpy( entry->package_id, addon->package_id );
        entry->licensed = *licensed;
        EnterCriticalSection( &addons_cs );
        list_add_tail( &licenses, &entry->entry );
        LeaveCriticalSection( &addons_cs );
    }
    TRACE( "add-on %s licensed %d.\n", debugstr_a( addon->content_id ), *licensed );
    return S_OK;
}

void gdkc_addon_get_details( const struct gdkc_addon *addon, UINT32 index, UINT32 count, XPackageDetails *details )
{
    memset( details, 0, sizeof(*details) );
    details->packageIdentifier = addon->package_id;
    details->version = addon->version;
    details->kind = XPackageKind_Content;
    details->displayName = addon->name;
    details->description = "";
    details->publisher = addon->publisher;
    details->storeId = addon->store_id;
    details->installing = FALSE;
    details->index = index;
    details->count = count;
    details->titleID = "";
}

/*
 * XPackageRegisterPackageInstalled: a watcher thread waits on the bridge for changes of the list
 * while any registration exists and tells each registration about add-ons that were not there
 * before (new, or a new version).
 */
struct installed_notification
{
    UINT64 token;
    struct gdkc_addons *addons;
    UINT32 index;
};

static void WINAPI installed_callback( void *arg, BOOLEAN canceled )
{
    struct installed_notification *notification = arg;
    struct installed_registration *reg;
    XPackageInstalledCallback *callback = NULL;
    XPackageDetails details;
    void *context = NULL;

    EnterCriticalSection( &addons_cs );
    LIST_FOR_EACH_ENTRY( reg, &installed_registrations, struct installed_registration, entry )
    {
        if (reg->token != notification->token) continue;
        callback = reg->callback;
        context = reg->context;
        break;
    }
    LeaveCriticalSection( &addons_cs );

    if (callback && !canceled)
    {
        gdkc_addon_get_details( &notification->addons->items[notification->index], 0, 1, &details );
        callback( context, &details );
    }
    gdkc_addons_release( notification->addons );
    free( notification );
}

static BOOL is_new( const struct gdkc_addons *old, const struct gdkc_addon *addon )
{
    const struct gdkc_addon *before = old ? gdkc_addons_find( old, addon->package_id ) : NULL;
    return !before || before->version.Value != addon->version.Value;
}

static void notify_installed( const struct gdkc_addons *old, struct gdkc_addons *addons )
{
    struct installed_registration *reg;
    struct installed_notification *notification;
    UINT32 i;

    EnterCriticalSection( &addons_cs );
    for (i = 0; i < addons->count; i++)
    {
        if (!is_new( old, &addons->items[i] )) continue;
        TRACE( "add-on %s installed.\n", debugstr_a( addons->items[i].content_id ) );
        LIST_FOR_EACH_ENTRY( reg, &installed_registrations, struct installed_registration, entry )
        {
            if (!(notification = malloc( sizeof(*notification) ))) continue;
            notification->token = reg->token;
            notification->addons = addons;
            notification->index = i;
            InterlockedIncrement( &addons->ref );
            if (FAILED(IXThreadingImpl_XTaskQueueSubmitCallback( x_threading_impl, reg->queue, XTaskQueuePort_Completion,
                                                                  notification, installed_callback )))
            {
                InterlockedDecrement( &addons->ref );
                free( notification );
            }
        }
    }
    LeaveCriticalSection( &addons_cs );
}

static DWORD WINAPI watch_thread( void *arg )
{
    struct gdkc_addons *known = arg, *addons;
    char *response;
    HRESULT hr;

    for (;;)
    {
        EnterCriticalSection( &addons_cs );
        if (list_empty( &installed_registrations ))
        {
            watching = FALSE;
            LeaveCriticalSection( &addons_cs );
            break;
        }
        LeaveCriticalSection( &addons_cs );

        if (FAILED(hr = gdkc_bridge_post( L"/packages/wait", known->generation, &response )))
        {
            WARN( "waiting for add-on changes failed, hr %#lx.\n", hr );
            Sleep( WATCH_RETRY_MS );
            continue;
        }
        addons = parse_addons( response );
        free( response );
        if (!addons) { Sleep( WATCH_RETRY_MS ); continue; }

        if (strcmp( addons->generation, known->generation ))
        {
            /* Cache first, so a game that lists the packages from its callback sees the new one. */
            InterlockedIncrement( &addons->ref );
            set_cached( addons );
            notify_installed( known, addons );
            gdkc_addons_release( known );
            known = addons;
        }
        else gdkc_addons_release( addons );
    }

    gdkc_addons_release( known );
    return 0;
}

HRESULT gdkc_addons_register_installed( XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    static LONG64 next_token;
    struct installed_registration *reg;
    struct gdkc_addons *addons = NULL;
    BOOL start = FALSE;
    HANDLE thread;

    if (!callback || !token) return E_INVALIDARG;
    if (!(reg = calloc( 1, sizeof(*reg) ))) return E_OUTOFMEMORY;
    reg->token = InterlockedIncrement64( &next_token );
    reg->queue = queue;
    reg->context = context;
    reg->callback = callback;

    /* The add-ons installed now are the base line: only later installs are reported. */
    if (gdkc_bridge_available() && FAILED(gdkc_addons_get( &addons ))) addons = NULL;

    EnterCriticalSection( &addons_cs );
    list_add_tail( &installed_registrations, &reg->entry );
    if (addons && !watching) start = watching = TRUE;
    LeaveCriticalSection( &addons_cs );

    if (start)
    {
        if ((thread = CreateThread( NULL, 0, watch_thread, addons, 0, NULL ))) CloseHandle( thread );
        else
        {
            WARN( "could not start the add-on watcher, error %lu.\n", GetLastError() );
            EnterCriticalSection( &addons_cs );
            watching = FALSE;
            LeaveCriticalSection( &addons_cs );
            gdkc_addons_release( addons );
        }
    }
    else gdkc_addons_release( addons );

    token->token = reg->token;
    return S_OK;
}

BOOLEAN gdkc_addons_unregister_installed( XTaskQueueRegistrationToken token )
{
    struct installed_registration *reg;

    EnterCriticalSection( &addons_cs );
    LIST_FOR_EACH_ENTRY( reg, &installed_registrations, struct installed_registration, entry )
    {
        if (reg->token != token.token) continue;
        list_remove( &reg->entry );
        free( reg );
        break;
    }
    LeaveCriticalSection( &addons_cs );
    return TRUE;
}

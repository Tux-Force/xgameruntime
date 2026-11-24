/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XGameEvent
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

#include <ctype.h>
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_game_event
{
    IXGameEventImpl IXGameEventImpl_iface;
    LONG ref;
};

static inline struct x_game_event *impl_from_IXGameEventImpl( IXGameEventImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_game_event, IXGameEventImpl_iface );
}

static HRESULT WINAPI x_game_event_QueryInterface( IXGameEventImpl *iface, REFIID iid, void **out )
{
    struct x_game_event *impl = impl_from_IXGameEventImpl( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown        ) ||
        IsEqualGUID( iid, &IID_IXGameEventImpl ))
    {
        IXGameEventImpl_AddRef( *out = &impl->IXGameEventImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_game_event_AddRef( IXGameEventImpl *iface )
{
    struct x_game_event *impl = impl_from_IXGameEventImpl( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_game_event_Release( IXGameEventImpl *iface )
{
    struct x_game_event *impl = impl_from_IXGameEventImpl( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/*
 * Events go to Xodus through its bridge, which uploads them to the Xbox services:
 *
 *   POST /xbox/event   body: service config id, play session id, event name, dimensions and
 *                      measurements (JSON), a line each
 *
 * Games write events from their main loop, so a thread pool callback sends them, in order.
 */
#define MAX_QUEUED_EVENTS 1000
#define MAX_EVENT_JSON (64 * 1024)

struct queued_event
{
    struct list entry;
    char body[1];
};

static CRITICAL_SECTION events_cs;
static void __attribute__((constructor)) init_events_cs( void )
{
    InitializeCriticalSection( &events_cs );
}

static struct list events = LIST_INIT( events );
static UINT32 events_count;
static BOOL sending;

static void CALLBACK send_events( TP_CALLBACK_INSTANCE *instance, void *context )
{
    struct queued_event *event;
    char *response;
    HRESULT hr;

    for (;;)
    {
        EnterCriticalSection( &events_cs );
        if (list_empty( &events ))
        {
            sending = FALSE;
            LeaveCriticalSection( &events_cs );
            return;
        }
        event = LIST_ENTRY( list_head( &events ), struct queued_event, entry );
        list_remove( &event->entry );
        events_count--;
        LeaveCriticalSection( &events_cs );

        if (FAILED(hr = gdkc_bridge_post( L"/xbox/event", event->body, &response ))) WARN( "event not sent, hr %#lx.\n", hr );
        else free( response );
        free( event );
    }
}

/* [A-Za-z]+[A-Za-z0-9_]* */
static BOOL valid_event_name( const char *name )
{
    SIZE_T i;

    if (!name || !isalpha( (unsigned char)*name )) return FALSE;
    for (i = 0; name[i]; i++)
        if (i >= 128 || (!isalnum( (unsigned char)name[i] ) && name[i] != '_')) return FALSE;
    return TRUE;
}

/* Appends a line. Line breaks in JSON are only white space, in the other fields they are not allowed. */
static char *append_line( char *out, const char *text )
{
    for (; text && *text; text++) *out++ = *text == '\r' || *text == '\n' ? ' ' : *text;
    *out++ = '\n';
    return out;
}

static HRESULT WINAPI x_game_event_XGameEventWrite( IXGameEventImpl *iface, XUserHandle user, const char *serviceConfigId, const char *playSessionId, const char *eventName, const char *dimensionsJson, const char *measurementsJson )
{
    struct queued_event *event;
    HRESULT hr = S_OK;
    SIZE_T size;
    char *p;

    TRACE( "iface %p, user %p, serviceConfigId %s, playSessionId %s, eventName %s, dimensionsJson %s, measurementsJson %s.\n", iface, user, debugstr_a( serviceConfigId ), debugstr_a( playSessionId ), debugstr_a( eventName ), debugstr_a( dimensionsJson ), debugstr_a( measurementsJson ) );

    if (!user || !serviceConfigId || !*serviceConfigId || !valid_event_name( eventName )) return E_INVALIDARG;
    if (strlen( serviceConfigId ) > 64 || (playSessionId && strlen( playSessionId ) > 64)) return E_INVALIDARG;
    if ((dimensionsJson && strlen( dimensionsJson ) > MAX_EVENT_JSON) ||
        (measurementsJson && strlen( measurementsJson ) > MAX_EVENT_JSON)) return E_INVALIDARG;
    if (!gdkc_bridge_available()) return E_NOTIMPL;

    size = strlen( serviceConfigId ) + strlen( eventName ) + 5;
    if (playSessionId) size += strlen( playSessionId );
    if (dimensionsJson) size += strlen( dimensionsJson );
    if (measurementsJson) size += strlen( measurementsJson );
    if (!(event = malloc( sizeof(*event) + size ))) return E_OUTOFMEMORY;

    p = append_line( event->body, serviceConfigId );
    p = append_line( p, playSessionId );
    p = append_line( p, eventName );
    p = append_line( p, dimensionsJson );
    p = append_line( p, measurementsJson );
    *p = 0;

    EnterCriticalSection( &events_cs );
    if (events_count >= MAX_QUEUED_EVENTS) hr = E_OUTOFMEMORY;
    else if (!sending && !TrySubmitThreadpoolCallback( send_events, NULL, NULL )) hr = HRESULT_FROM_WIN32( GetLastError() );
    else
    {
        sending = TRUE;
        list_add_tail( &events, &event->entry );
        events_count++;
    }
    LeaveCriticalSection( &events_cs );
    if (FAILED(hr)) free( event );
    return hr;
}

static const struct IXGameEventImplVtbl x_game_event_vtbl =
{
    x_game_event_QueryInterface,
    x_game_event_AddRef,
    x_game_event_Release,
    /* IXGameEventImpl methods */
    x_game_event_XGameEventWrite,
};

static struct x_game_event x_game_event =
{
    {&x_game_event_vtbl},
    0,
};

IXGameEventImpl *x_game_event_impl = &x_game_event.IXGameEventImpl_iface;

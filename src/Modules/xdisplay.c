/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XDisplay and XLauncher
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

#include <shellapi.h>

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct display
{
    IXDisplayImpl IXDisplayImpl_iface;
    IXLauncherImpl IXLauncherImpl_iface;
    LONG ref;
};

static inline struct display *impl_from_IXDisplayImpl( IXDisplayImpl *iface )
{
    return CONTAINING_RECORD( iface, struct display, IXDisplayImpl_iface );
}

static HRESULT WINAPI x_display_QueryInterface( IXDisplayImpl *iface, REFIID iid, void **out )
{
    struct display *impl = impl_from_IXDisplayImpl( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown      ) ||
        IsEqualGUID( iid, &IID_IXDisplayImpl ))
    {
        IXDisplayImpl_AddRef( *out = &impl->IXDisplayImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_display_AddRef( IXDisplayImpl *iface )
{
    struct display *impl = impl_from_IXDisplayImpl( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_display_Release( IXDisplayImpl *iface )
{
    struct display *impl = impl_from_IXDisplayImpl( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI __PADDING__( IXDisplayImpl *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static XDisplayHdrModeResult WINAPI x_display_XDisplayTryEnableHdrMode( IXDisplayImpl *iface, XDisplayHdrModePreference displayModePreference, XDisplayHdrModeInfo *displayHdrModeInfo )
{
    TRACE( "iface %p, displayModePreference %d, displayHdrModeInfo %p.\n", iface, displayModePreference, displayHdrModeInfo );

    /* The state of the display is not known here, so a game has to ask DXGI. */
    if (displayHdrModeInfo) memset( displayHdrModeInfo, 0, sizeof(*displayHdrModeInfo) );
    return XDisplayHdrModeResult_Unknown;
}

static const struct IXDisplayImplVtbl x_display_vtbl =
{
    x_display_QueryInterface,
    x_display_AddRef,
    x_display_Release,
    /* IXDisplayImpl methods */
    __PADDING__,
    x_display_XDisplayTryEnableHdrMode,
};

static inline struct display *impl_from_IXLauncherImpl( IXLauncherImpl *iface )
{
    return CONTAINING_RECORD( iface, struct display, IXLauncherImpl_iface );
}

static HRESULT WINAPI x_launcher_QueryInterface( IXLauncherImpl *iface, REFIID iid, void **out )
{
    struct display *impl = impl_from_IXLauncherImpl( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown       ) ||
        IsEqualGUID( iid, &IID_IXLauncherImpl ))
    {
        IXLauncherImpl_AddRef( *out = &impl->IXLauncherImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_launcher_AddRef( IXLauncherImpl *iface )
{
    struct display *impl = impl_from_IXLauncherImpl( iface );
    return IXDisplayImpl_AddRef( &impl->IXDisplayImpl_iface );
}

static ULONG WINAPI x_launcher_Release( IXLauncherImpl *iface )
{
    struct display *impl = impl_from_IXLauncherImpl( iface );
    return IXDisplayImpl_Release( &impl->IXDisplayImpl_iface );
}

/* Only schemes that open a page or another application's own view, never a local file or program. */
static BOOL is_launch_scheme_allowed( const char *uri, SIZE_T len )
{
    static const char *const schemes[] = { "http", "https", "mailto", "ms-windows-store", "msxbox", "xbox" };
    static const char title_prefix[] = "ms-xbl-";
    SIZE_T i;

    for (i = 0; i < ARRAY_SIZE(schemes); i++)
        if (strlen( schemes[i] ) == len && !_strnicmp( uri, schemes[i], len )) return TRUE;

    if (len <= sizeof(title_prefix) - 1 || _strnicmp( uri, title_prefix, sizeof(title_prefix) - 1 )) return FALSE;
    /* Followed by the title id of the game to open. */
    for (i = sizeof(title_prefix) - 1; i < len; i++)
        if (!isxdigit( (unsigned char)uri[i] )) return FALSE;
    return TRUE;
}

static HRESULT WINAPI x_launcher_XLaunchUri( IXLauncherImpl *iface, XUserHandle user, const char *uri )
{
    const char *colon, *p;
    INT_PTR ret;
    WCHAR *wide;
    int len;

    TRACE( "iface %p, user %p uri %s.\n", iface, user, debugstr_a( uri ) );

    if (!uri || !(colon = strchr( uri, ':' )) || !colon[1]) return E_INVALIDARG;
    /* None of these are valid in a URI, and they could break out of a handler's command line. */
    for (p = uri; *p; p++)
        if ((unsigned char)*p <= ' ' || *p == 0x7f || *p == '"') return E_INVALIDARG;
    if (!is_launch_scheme_allowed( uri, colon - uri ))
    {
        WARN( "refusing to launch %s.\n", debugstr_a( uri ) );
        return E_ACCESSDENIED;
    }

    if (!(len = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, uri, -1, NULL, 0 ))) return E_INVALIDARG;
    if (!(wide = malloc( len * sizeof(*wide) ))) return E_OUTOFMEMORY;
    MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, uri, -1, wide, len );
    ret = (INT_PTR)ShellExecuteW( NULL, L"open", wide, NULL, NULL, SW_SHOWNORMAL );
    free( wide );

    /* Values up to 32 are errors, some of them without a Win32 error of the same number. */
    if (ret > 32) return S_OK;
    WARN( "ShellExecuteW failed, error %d.\n", (int)ret );
    if (ret == SE_ERR_NOASSOC || ret == SE_ERR_ASSOCINCOMPLETE) return HRESULT_FROM_WIN32( ERROR_NO_ASSOCIATION );
    return ret == SE_ERR_ACCESSDENIED ? E_ACCESSDENIED : E_FAIL;
}

struct XDisplayTimeoutDeferral
{
    struct XDisplayTimeoutDeferral *next;
};

/* The execution state belongs to the thread that set it and goes away with that thread, while a
 * deferral can be acquired and closed on any thread. So one thread of our own holds the state. */
static SRWLOCK deferral_lock = SRWLOCK_INIT;
static struct XDisplayTimeoutDeferral *deferrals;
static HANDLE deferral_event;

static DWORD WINAPI deferral_thread( void *arg )
{
    BOOL active, was_active = FALSE;

    while (!WaitForSingleObject( deferral_event, INFINITE ))
    {
        AcquireSRWLockShared( &deferral_lock );
        active = !!deferrals;
        ReleaseSRWLockShared( &deferral_lock );

        if (active == was_active) continue;
        SetThreadExecutionState( ES_CONTINUOUS | (active ? ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : 0) );
        was_active = active;
    }
    return 0;
}

/* Called with deferral_lock held exclusively. */
static HRESULT start_deferral_thread(void)
{
    HMODULE module;
    HANDLE thread;

    if (deferral_event) return S_OK;
    /* The thread runs until the process ends, so this library must stay loaded. */
    if (!GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (const WCHAR *)deferral_thread, &module ) ||
        !(deferral_event = CreateEventW( NULL, FALSE, FALSE, NULL )))
        return HRESULT_FROM_WIN32( GetLastError() );

    if (!(thread = CreateThread( NULL, 0, deferral_thread, NULL, 0, NULL )))
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );
        CloseHandle( deferral_event );
        deferral_event = NULL;
        return hr;
    }
    CloseHandle( thread );
    return S_OK;
}

static HRESULT WINAPI x_launcher_XDisplayAcquireTimeoutDeferral( IXLauncherImpl *iface, XDisplayTimeoutDeferralHandle *handle )
{
    struct XDisplayTimeoutDeferral *deferral;
    HRESULT hr;

    TRACE( "iface %p, handle %p.\n", iface, handle );

    if (!handle) return E_INVALIDARG;
    *handle = NULL;
    if (!(deferral = calloc( 1, sizeof(*deferral) ))) return E_OUTOFMEMORY;

    AcquireSRWLockExclusive( &deferral_lock );
    if (SUCCEEDED(hr = start_deferral_thread()))
    {
        deferral->next = deferrals;
        deferrals = deferral;
        SetEvent( deferral_event );
    }
    ReleaseSRWLockExclusive( &deferral_lock );

    if (FAILED(hr)) free( deferral );
    else *handle = deferral;
    return hr;
}

static void WINAPI x_launcher_XDisplayCloseTimeoutDeferralHandle( IXLauncherImpl *iface, XDisplayTimeoutDeferralHandle handle )
{
    struct XDisplayTimeoutDeferral **entry;

    TRACE( "iface %p, handle %p.\n", iface, handle );

    if (!handle) return;

    AcquireSRWLockExclusive( &deferral_lock );
    for (entry = &deferrals; *entry && *entry != handle; entry = &(*entry)->next) {}
    if (*entry)
    {
        *entry = handle->next;
        free( handle );
        SetEvent( deferral_event );
    }
    else WARN( "invalid handle %p.\n", handle );
    ReleaseSRWLockExclusive( &deferral_lock );
}

static const struct IXLauncherImplVtbl x_launcher_vtbl =
{
    x_launcher_QueryInterface,
    x_launcher_AddRef,
    x_launcher_Release,
    /* IXLauncherImpl methods */
    x_launcher_XLaunchUri,
    x_launcher_XDisplayAcquireTimeoutDeferral,
    x_launcher_XDisplayCloseTimeoutDeferralHandle,
};

static struct display display =
{
    {&x_display_vtbl},
    {&x_launcher_vtbl},
    0,
};

IXDisplayImpl *x_display_impl = &display.IXDisplayImpl_iface;
IXLauncherImpl *x_launcher_impl = &display.IXLauncherImpl_iface;

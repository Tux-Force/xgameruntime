/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XGameUI
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

#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_game_ui
{
    IXGameUiImpl4 IXGameUiImpl4_iface;
    LONG ref;
};

static inline struct x_game_ui *impl_from_IXGameUiImpl4( IXGameUiImpl4 *iface )
{
    return CONTAINING_RECORD( iface, struct x_game_ui, IXGameUiImpl4_iface );
}

static HRESULT WINAPI x_game_ui_QueryInterface( IXGameUiImpl4 *iface, REFIID iid, void **out )
{
    struct x_game_ui *impl = impl_from_IXGameUiImpl4( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown      ) ||
        IsEqualGUID( iid, &IID_IXGameUiImpl  ) ||
        IsEqualGUID( iid, &IID_IXGameUiImpl2 ) ||
        IsEqualGUID( iid, &IID_IXGameUiImpl3 ) ||
        IsEqualGUID( iid, &IID_IXGameUiImpl4 ))
    {
        IXGameUiImpl4_AddRef( *out = &impl->IXGameUiImpl4_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_game_ui_AddRef( IXGameUiImpl4 *iface )
{
    struct x_game_ui *impl = impl_from_IXGameUiImpl4( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_game_ui_Release( IXGameUiImpl4 *iface )
{
    struct x_game_ui *impl = impl_from_IXGameUiImpl4( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/*
 * System UI. On Windows these dialogs come from the Xbox shell; here they are plain windows on a
 * thread of their own, so neither the game's threads nor its task queues wait for the player. A
 * game can also bring its own UI (XGameUiSetUiCallbacks) and answer with XGameUiSet*UiResponse.
 */

enum ui_kind
{
    UI_MESSAGE,
    UI_ERROR,
    UI_TEXT,
    UI_ACHIEVEMENTS,
    UI_PROFILE,
    UI_PICKER,
    UI_SEND_INVITE,
    UI_ACTIVITY_INVITE,
};

struct ui_request
{
    struct list entry;          /* in requests */
    LONG ref;                   /* the async call, the thread or callback working on it */
    LONG done;                  /* somebody has taken on completing the call */
    LONG listed;
    ULONG_PTR id;               /* handle for the game; unlike the address it is never used again */
    enum ui_kind kind;
    XAsyncBlock *async;         /* the provider's block, valid until cleanup */
    XTaskQueueHandle queue;
    XUserHandle user;
    HWND hwnd;                  /* protected by ui_cs */

    char *text[5];              /* title, content, buttons or other strings of the call */
    XGameUiMessageDialogButton default_button, cancel_button, button;
    HRESULT error;
    UINT64 xuid;
    UINT32 title_id;
    UINT32 max_length;
    XGameUiTextEntryInputScope scope;
    char *result_text;
    UINT64 *players;
    UINT32 player_count;
    XGameUiPlayerPickerInfo picker;
    UINT64 *picker_from, *picker_selected;
    const WCHAR *path;          /* bridge request for the text to show */
    char body[64];
    XGameUiUiCallbacks callbacks;
};

static struct list requests = LIST_INIT( requests );
static ULONG_PTR next_id;
static XGameUiUiCallbacks ui_callbacks;
static BOOL prefer_system_ui;
static CRITICAL_SECTION ui_cs;
static void __attribute__((constructor)) init_ui_cs( void )
{
    InitializeCriticalSection( &ui_cs );
}

static void ui_unlist( struct ui_request *request )
{
    EnterCriticalSection( &ui_cs );
    if (request->listed)
    {
        list_remove( &request->entry );
        request->listed = FALSE;
    }
    LeaveCriticalSection( &ui_cs );
}

static void ui_release( struct ui_request *request )
{
    unsigned int i;

    if (InterlockedDecrement( &request->ref )) return;
    ui_unlist( request );
    if (request->queue) IXThreadingImpl_XTaskQueueCloseHandle( x_threading_impl, request->queue );
    if (request->user) IXUserImpl_XUserCloseHandle( x_user_impl, request->user );
    for (i = 0; i < ARRAY_SIZE(request->text); i++) free( request->text[i] );
    free( request->result_text );
    free( request->players );
    free( request->picker_from );
    free( request->picker_selected );
    free( request );
}

static struct ui_request *ui_create( enum ui_kind kind )
{
    struct ui_request *request;

    if (!(request = calloc( 1, sizeof(*request) ))) return NULL;
    request->kind = kind;
    request->ref = 1;
    return request;
}

static BOOL ui_set_text( struct ui_request *request, unsigned int index, const char *text )
{
    if (!text) return TRUE;
    return (request->text[index] = strdup( text )) != NULL;
}

/* Cuts UTF-8 to at most `max` bytes, never within a character. Games have room for just that. */
static void ui_limit_text( char *text, UINT32 max )
{
    if (!max || strlen( text ) <= max) return;
    while (max && ((BYTE)text[max] & 0xc0) == 0x80) max--;
    text[max] = 0;
}

static BOOL ui_set_user( struct ui_request *request, XUserHandle user )
{
    return SUCCEEDED( IXUserImpl_XUserDuplicateHandle( x_user_impl, user, &request->user ) );
}

static SIZE_T ui_result_size( const struct ui_request *request )
{
    if (request->kind == UI_MESSAGE) return sizeof(request->button);
    if (request->kind == UI_TEXT) return strlen( request->result_text ) + 1;
    if (request->kind == UI_PICKER) return request->player_count * sizeof(*request->players);
    return 0;
}

/* The caller has taken on the completion (request->done) and holds a reference. */
static void ui_complete( struct ui_request *request, HRESULT hr )
{
    ui_unlist( request );
    IXThreadingImpl_XAsyncComplete( x_threading_impl, request->async, hr, SUCCEEDED(hr) ? ui_result_size( request ) : 0 );
}

static BOOL ui_claim( struct ui_request *request )
{
    return !InterlockedExchange( &request->done, TRUE );
}

static HRESULT WINAPI ui_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct ui_request *request = data->context;

    switch (op)
    {
        case XAsyncOp_Begin:
            request->async = data->async;
            return S_OK;

        case XAsyncOp_GetResult:
        {
            SIZE_T size = ui_result_size( request );

            if (data->bufferSize < size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            if (request->kind == UI_MESSAGE) memcpy( data->buffer, &request->button, size );
            else if (request->kind == UI_TEXT) memcpy( data->buffer, request->result_text, size );
            else if (request->kind == UI_PICKER && size) memcpy( data->buffer, request->players, size );
            return S_OK;
        }

        case XAsyncOp_Cancel:
            if (ui_claim( request ))
            {
                EnterCriticalSection( &ui_cs );
                if (request->hwnd) PostMessageW( request->hwnd, WM_CLOSE, 0, 0 );
                LeaveCriticalSection( &ui_cs );
                ui_complete( request, E_ABORT );
            }
            return S_OK;

        case XAsyncOp_Cleanup:
            ui_release( request );
            return S_OK;

        default:
            return S_OK;
    }
}

static WCHAR *ui_wide( const char *text )
{
    const char *src = text ? text : "";
    WCHAR *wide, *out, *p;
    int size = MultiByteToWideChar( CP_UTF8, 0, src, -1, NULL, 0 );

    if (!(wide = malloc( size * sizeof(WCHAR) ))) return NULL;
    MultiByteToWideChar( CP_UTF8, 0, src, -1, wide, size );
    /* Controls break lines at CRLF only. */
    if (!(out = malloc( (size * 2) * sizeof(WCHAR) ))) { free( wide ); return NULL; }
    for (p = out, size = 0; wide[size]; size++)
    {
        if (wide[size] == '\n' && (!size || wide[size - 1] != '\r')) *p++ = '\r';
        *p++ = wide[size];
    }
    *p = 0;
    free( wide );
    return out;
}

#define ID_FIRST_BUTTON 100
#define ID_EDIT 200

struct ui_dialog
{
    struct ui_request *request;
    const WCHAR *title;
    const WCHAR *content;
    const WCHAR *buttons[3];
    UINT default_button, cancel_button;
    const WCHAR *edit;          /* initial text of an entry field */
    BOOL scroll;                /* long text in a scrolling view */
    WCHAR *entered;
    HFONT font;
};

static int ui_scale( HDC hdc, int value )
{
    return MulDiv( value, GetDeviceCaps( hdc, LOGPIXELSY ), 96 );
}

static HWND ui_control( struct ui_dialog *dialog, HWND parent, const WCHAR *class, const WCHAR *text, DWORD style, DWORD ex_style, UINT_PTR id, int x, int y, int w, int h )
{
    HWND hwnd = CreateWindowExW( ex_style, class, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)id, GetModuleHandleW( NULL ), NULL );
    SendMessageW( hwnd, WM_SETFONT, (WPARAM)dialog->font, FALSE );
    return hwnd;
}

static void ui_layout( HWND hwnd, struct ui_dialog *dialog )
{
    NONCLIENTMETRICSW metrics = { .cbSize = sizeof(metrics) };
    int margin, width, height, button_w, button_h, x, y, text_h, count = 0, i;
    RECT rect = {0}, work;
    HWND focus = NULL, control;
    HFONT old;
    HDC hdc;

    if (SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 ))
        dialog->font = CreateFontIndirectW( &metrics.lfMessageFont );
    SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );

    hdc = GetDC( hwnd );
    old = SelectObject( hdc, dialog->font ? dialog->font : GetStockObject( DEFAULT_GUI_FONT ) );
    margin = ui_scale( hdc, 16 );
    width = min( ui_scale( hdc, dialog->scroll ? 560 : 460 ), work.right - work.left );
    button_h = ui_scale( hdc, 30 );
    rect.right = width - 2 * margin;
    DrawTextW( hdc, dialog->content, -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS );
    text_h = dialog->scroll ? ui_scale( hdc, 360 ) : min( rect.bottom, (work.bottom - work.top) * 3 / 5 );
    for (i = 0; i < 3; i++) if (dialog->buttons[i]) count++;
    button_w = ui_scale( hdc, 130 );
    if (count) button_w = min( button_w, (width - 2 * margin - (count - 1) * margin / 2) / count );
    /* A long button text gets the room there is. */
    for (i = 0; i < 3; i++)
    {
        SIZE size;
        if (!dialog->buttons[i]) continue;
        GetTextExtentPoint32W( hdc, dialog->buttons[i], lstrlenW( dialog->buttons[i] ), &size );
        button_w = max( button_w, min( size.cx + margin, (width - 2 * margin - (count - 1) * margin / 2) / count ) );
    }
    height = margin + text_h + margin + (dialog->edit ? button_h + margin : 0) + button_h + margin;

    y = margin;
    if (dialog->scroll)
        ui_control( dialog, hwnd, L"EDIT", dialog->content, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, (UINT_PTR)-1, margin, y, width - 2 * margin, text_h );
    else
        ui_control( dialog, hwnd, L"STATIC", dialog->content, SS_LEFT | SS_NOPREFIX, 0, (UINT_PTR)-1, margin, y, width - 2 * margin, text_h );
    y += text_h + margin;
    if (dialog->edit)
    {
        DWORD style = ES_AUTOHSCROLL | WS_TABSTOP;

        if (dialog->request->scope == XGameUiTextEntryInputScope_Password) style |= ES_PASSWORD;
        if (dialog->request->scope == XGameUiTextEntryInputScope_Number) style |= ES_NUMBER;
        focus = ui_control( dialog, hwnd, L"EDIT", dialog->edit, style, WS_EX_CLIENTEDGE, ID_EDIT, margin, y, width - 2 * margin, button_h );
        if (dialog->request->max_length) SendMessageW( focus, EM_SETLIMITTEXT, dialog->request->max_length, 0 );
        SendMessageW( focus, EM_SETSEL, 0, -1 );
        y += button_h + margin;
    }
    x = width - margin - count * button_w - (count ? count - 1 : 0) * (margin / 2);
    for (i = 0; i < 3; i++)
    {
        if (!dialog->buttons[i]) continue;
        control = ui_control( dialog, hwnd, L"BUTTON", dialog->buttons[i], WS_TABSTOP | (i == (int)dialog->default_button ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), 0, ID_FIRST_BUTTON + i, x, y, button_w, button_h );
        if (i == (int)dialog->default_button)
        {
            SendMessageW( hwnd, DM_SETDEFID, ID_FIRST_BUTTON + i, 0 );
            if (!focus) focus = control;
        }
        x += button_w + margin / 2;
    }
    SelectObject( hdc, old );
    ReleaseDC( hwnd, hdc );

    SetRect( &rect, 0, 0, width, height );
    AdjustWindowRectEx( &rect, GetWindowLongW( hwnd, GWL_STYLE ), FALSE, GetWindowLongW( hwnd, GWL_EXSTYLE ) );
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    SetWindowPos( hwnd, HWND_TOPMOST, work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2, width, height, 0 );
    SetWindowTextW( hwnd, dialog->title );
    if (focus) SetFocus( focus );
}

static INT_PTR CALLBACK ui_dialog_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct ui_dialog *dialog = (struct ui_dialog *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
        case WM_INITDIALOG:
            dialog = (struct ui_dialog *)lparam;
            SetWindowLongPtrW( hwnd, DWLP_USER, lparam );
            ui_layout( hwnd, dialog );
            /* From here on a cancelled call closes the window itself. */
            EnterCriticalSection( &ui_cs );
            dialog->request->hwnd = hwnd;
            if (dialog->request->done) PostMessageW( hwnd, WM_CLOSE, 0, 0 );
            LeaveCriticalSection( &ui_cs );
            /* The game may keep the pointer to its window. */
            ClipCursor( NULL );
            SetForegroundWindow( hwnd );
            return FALSE;

        case WM_CLOSE:
            EndDialog( hwnd, dialog->cancel_button );
            return TRUE;

        case WM_COMMAND:
            if (LOWORD(wparam) == IDCANCEL)
            {
                EndDialog( hwnd, dialog->cancel_button );
                return TRUE;
            }
            if (HIWORD(wparam) != BN_CLICKED) return FALSE;
            if (LOWORD(wparam) >= ID_FIRST_BUTTON && LOWORD(wparam) < ID_FIRST_BUTTON + 3)
            {
                if (dialog->edit)
                {
                    HWND edit = GetDlgItem( hwnd, ID_EDIT );
                    int size = GetWindowTextLengthW( edit ) + 1;
                    if ((dialog->entered = calloc( size, sizeof(WCHAR) ))) GetWindowTextW( edit, dialog->entered, size );
                }
                EndDialog( hwnd, LOWORD(wparam) - ID_FIRST_BUTTON );
                return TRUE;
            }
            return FALSE;

        case WM_DESTROY:
            EnterCriticalSection( &ui_cs );
            if (dialog) dialog->request->hwnd = NULL;
            LeaveCriticalSection( &ui_cs );
            return FALSE;
    }
    return FALSE;
}

/* Shows the dialog and answers the index of the button, -1 when it could not be shown. */
static INT_PTR ui_run_dialog( struct ui_dialog *dialog )
{
    /* Template without controls, followed by empty menu, class and title. */
    struct { DLGTEMPLATE header; WORD names[3]; } template = {{ .style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE | DS_MODALFRAME, .dwExtendedStyle = WS_EX_TOPMOST | WS_EX_CONTROLPARENT }};
    HWND previous = GetForegroundWindow();
    INT_PTR ret;

    ret = DialogBoxIndirectParamW( GetModuleHandleW( NULL ), &template.header, NULL, ui_dialog_proc, (LPARAM)dialog );
    if (dialog->font) DeleteObject( dialog->font );
    if (previous) SetForegroundWindow( previous );
    return ret;
}

static HRESULT ui_show( struct ui_request *request )
{
    WCHAR *title = NULL, *content = NULL, *buttons[3] = {0}, *edit = NULL, *system = NULL, message[1024];
    struct ui_dialog dialog = { .request = request };
    char *fetched = NULL;
    HRESULT hr = S_OK;
    INT_PTR ret;
    int i, size;

    switch (request->kind)
    {
        case UI_MESSAGE:
            title = ui_wide( request->text[0] );
            content = ui_wide( request->text[1] );
            for (i = 0; i < 3; i++)
                if (request->text[2 + i] && *request->text[2 + i]) buttons[i] = ui_wide( request->text[2 + i] );
            if (!buttons[0] && !buttons[1] && !buttons[2]) buttons[0] = ui_wide( "OK" );
            for (i = 0; i < 3; i++) dialog.buttons[i] = buttons[i];
            dialog.default_button = request->default_button;
            dialog.cancel_button = request->cancel_button;
            break;

        case UI_ERROR:
            title = ui_wide( request->text[0] );
            FormatMessageW( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, request->error, 0, (WCHAR *)&system, 0, NULL );
            swprintf( message, ARRAY_SIZE(message), L"%ls\r\n(0x%08lx)", system ? system : L"Something went wrong.", request->error );
            LocalFree( system );
            content = _wcsdup( message );
            dialog.buttons[0] = L"OK";
            break;

        case UI_TEXT:
            title = ui_wide( request->text[0] );
            content = ui_wide( request->text[1] );
            dialog.edit = edit = ui_wide( request->text[2] );
            dialog.buttons[0] = L"OK";
            dialog.buttons[1] = L"Cancel";
            dialog.cancel_button = 1;
            break;

        default:
            if (FAILED(hr = gdkc_bridge_post( request->path, request->body, &fetched ))) return hr;
            /* The first line is the title. */
            for (i = 0; fetched[i] && fetched[i] != '\n'; i++) ;
            content = ui_wide( fetched[i] ? fetched + i + 1 : "" );
            fetched[i] = 0;
            title = ui_wide( fetched );
            free( fetched );
            dialog.scroll = request->kind == UI_ACHIEVEMENTS;
            dialog.buttons[0] = L"OK";
            break;
    }
    if (!title || !content || (request->kind == UI_TEXT && !edit)) hr = E_OUTOFMEMORY;

    if (SUCCEEDED(hr))
    {
        dialog.title = title;
        dialog.content = content;
        if ((ret = ui_run_dialog( &dialog )) < 0) hr = HRESULT_FROM_WIN32( ERROR_CANNOT_MAKE );
        else if (request->kind == UI_MESSAGE) request->button = ret;
        else if (request->kind == UI_TEXT)
        {
            if (ret) hr = E_ABORT;
            else if (!dialog.entered) hr = E_OUTOFMEMORY;
            else
            {
                size = WideCharToMultiByte( CP_UTF8, 0, dialog.entered, -1, NULL, 0, NULL, NULL );
                if (!(request->result_text = malloc( size ))) hr = E_OUTOFMEMORY;
                else
                {
                    WideCharToMultiByte( CP_UTF8, 0, dialog.entered, -1, request->result_text, size, NULL, NULL );
                    ui_limit_text( request->result_text, request->max_length );
                }
            }
        }
    }

    free( dialog.entered );
    free( edit );
    for (i = 0; i < 3; i++) free( buttons[i] );
    free( content );
    free( title );
    return hr;
}

static HMODULE ui_module;

static DWORD WINAPI ui_thread( void *arg )
{
    struct ui_request *request = arg;
    HRESULT hr = ui_show( request );

    if (ui_claim( request )) ui_complete( request, hr );
    ui_release( request );
    /* Started with a reference on this library, a game may unload it before the window is gone. */
    FreeLibraryAndExitThread( ui_module, 0 );
    return 0;
}

static BOOL ui_has_callback( const struct ui_request *request )
{
    switch (request->kind)
    {
        case UI_MESSAGE:         return request->callbacks.showMessageDialogCallback != NULL;
        case UI_ERROR:           return request->callbacks.showErrorDialogCallback != NULL;
        case UI_TEXT:            return request->callbacks.showTextEntryCallback != NULL;
        case UI_ACHIEVEMENTS:    return request->callbacks.showAchievementsCallback != NULL;
        case UI_PROFILE:         return request->callbacks.showPlayerProfileCardCallback != NULL;
        case UI_PICKER:          return request->callbacks.showPlayerPickerCallback != NULL;
        case UI_SEND_INVITE:     return request->callbacks.showSendGameInviteCallback != NULL;
        case UI_ACTIVITY_INVITE: return request->callbacks.showMultiplayerActivityGameInviteCallback != NULL;
    }
    return FALSE;
}

static void __stdcall ui_callback( void *context, BOOLEAN canceled )
{
    struct ui_request *request = context;
    XGameUiCallbackHandle handle = (XGameUiCallbackHandle)request->id;
    XGameUiUiCallbacks *c = &request->callbacks;

    if (canceled)
    {
        if (ui_claim( request )) ui_complete( request, E_ABORT );
        ui_release( request );
        return;
    }
    if (!request->done) switch (request->kind)
    {
        case UI_MESSAGE:
            c->showMessageDialogCallback( handle, request->queue, request->text[0], request->text[1], request->text[2], request->text[3], request->text[4], request->default_button, request->cancel_button, c->context );
            break;
        case UI_ERROR:
            c->showErrorDialogCallback( handle, request->queue, request->error, request->text[1], c->context );
            break;
        case UI_TEXT:
            c->showTextEntryCallback( handle, request->queue, request->text[0], request->text[1], request->text[2], request->scope, request->max_length, c->context );
            break;
        case UI_ACHIEVEMENTS:
            c->showAchievementsCallback( handle, request->queue, request->user, request->title_id, c->context );
            break;
        case UI_PROFILE:
            c->showPlayerProfileCardCallback( handle, request->queue, request->user, request->xuid, c->context );
            break;
        case UI_PICKER:
            c->showPlayerPickerCallback( handle, request->queue, &request->picker, c->context );
            break;
        case UI_SEND_INVITE:
            c->showSendGameInviteCallback( handle, request->queue, request->user, request->text[0], request->text[1], request->text[2], request->text[3], request->text[4], c->context );
            break;
        case UI_ACTIVITY_INVITE:
            c->showMultiplayerActivityGameInviteCallback( handle, request->queue, request->user, c->context );
            break;
    }
    ui_release( request );
}

/* Starts the call, taking over the request. `system` tells that the kind has UI of our own. */
static HRESULT ui_begin( XAsyncBlock *async, struct ui_request *request, BOOL system, const void *identity, const char *name )
{
    BOOL use_callback, prefer;
    HANDLE thread;
    HRESULT hr;

    EnterCriticalSection( &ui_cs );
    request->callbacks = ui_callbacks;
    prefer = prefer_system_ui;
    LeaveCriticalSection( &ui_cs );
    use_callback = ui_has_callback( request ) && !(prefer && system);

    if (!use_callback && !system)
    {
        FIXME( "%s needs UI of the game (XGameUiSetUiCallbacks).\n", name );
        ui_release( request );
        return E_NOTIMPL;
    }
    if (use_callback)
    {
        XTaskQueueHandle queue = async->queue;

        if (!queue)
        {
            if (!IXThreadingImpl_XTaskQueueGetCurrentProcessTaskQueue( x_threading_impl, &request->queue )) request->queue = NULL;
        }
        else if (FAILED(hr = IXThreadingImpl_XTaskQueueDuplicateHandle( x_threading_impl, queue, &request->queue )))
        {
            ui_release( request );
            return hr;
        }
        if (!request->queue)
        {
            ui_release( request );
            return E_INVALIDARG;
        }
    }

    /* Cancelling the call from here on only drops the reference of the call itself. */
    InterlockedIncrement( &request->ref );
    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, request, identity, name, ui_provider )))
    {
        ui_release( request );
        ui_release( request );
        return hr;
    }
    if (use_callback)
    {
        EnterCriticalSection( &ui_cs );
        if (!request->done)
        {
            request->id = ++next_id;
            list_add_tail( &requests, &request->entry );
            request->listed = TRUE;
        }
        LeaveCriticalSection( &ui_cs );
        hr = IXThreadingImpl_XTaskQueueSubmitCallback( x_threading_impl, request->queue, XTaskQueuePort_Completion, request, ui_callback );
    }
    else if (!GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (const WCHAR *)ui_thread, &ui_module ))
        hr = HRESULT_FROM_WIN32( GetLastError() );
    else if ((thread = CreateThread( NULL, 0, ui_thread, request, 0, NULL ))) CloseHandle( thread );
    else
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        FreeLibrary( ui_module );
    }

    if (FAILED(hr))
    {
        /* The call has begun, so it reports the failure itself. */
        if (ui_claim( request )) ui_complete( request, hr );
        ui_release( request );
    }
    return S_OK;
}

/* XAsyncGetResult drops the result when the buffer is too small, so a game could not ask again. */
static HRESULT ui_sized_result( XAsyncBlock *async, const void *identity, SIZE_T size, void *buffer, SIZE_T *used )
{
    SIZE_T required;
    HRESULT hr;

    if (FAILED(hr = IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, &required ))) return hr;
    if (size < required) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, identity, size, buffer, used );
}

/* The request a game answers, claimed and with a reference, NULL when it is not waiting. */
static struct ui_request *ui_take( XGameUiCallbackHandle handle, enum ui_kind kind )
{
    struct ui_request *request, *found = NULL;

    EnterCriticalSection( &ui_cs );
    LIST_FOR_EACH_ENTRY( request, &requests, struct ui_request, entry )
    {
        if (request->id != (ULONG_PTR)handle || request->kind != kind) continue;
        if (ui_claim( request ))
        {
            InterlockedIncrement( &request->ref );
            found = request;
        }
        break;
    }
    LeaveCriticalSection( &ui_cs );
    return found;
}

static HRESULT ui_respond( XGameUiCallbackHandle handle, enum ui_kind kind )
{
    struct ui_request *request;

    if (!(request = ui_take( handle, kind ))) return E_INVALIDARG;
    ui_complete( request, S_OK );
    ui_release( request );
    return S_OK;
}

static HRESULT ui_result( XAsyncBlock *async, const void *identity )
{
    if (!async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, identity, 0, NULL, NULL );
}

static HRESULT WINAPI x_game_ui_XGameUiShowMessageDialogAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, const char *titleText, const char *contentText, const char *firstButtonText, const char *secondButtonText, const char *thirdButtonText, XGameUiMessageDialogButton defaultButton, XGameUiMessageDialogButton cancelButton )
{
    TRACE( "iface %p, async %p, titleText %s, contentText %s, firstButtonText %s, secondButtonText %s, thirdButtonText %s, defaultButton %d, cancelButton %d.\n", iface, async, debugstr_a( titleText ), debugstr_a( contentText ), debugstr_a( firstButtonText ), debugstr_a( secondButtonText ), debugstr_a( thirdButtonText ), defaultButton, cancelButton );

    struct ui_request *request;

    if (!async || !titleText || !contentText) return E_INVALIDARG;
    if (defaultButton > XGameUiMessageDialogButton_Third || cancelButton > XGameUiMessageDialogButton_Third) return E_INVALIDARG;
    if (!(request = ui_create( UI_MESSAGE ))) return E_OUTOFMEMORY;
    if (!ui_set_text( request, 0, titleText ) || !ui_set_text( request, 1, contentText ) || !ui_set_text( request, 2, firstButtonText ) ||
        !ui_set_text( request, 3, secondButtonText ) || !ui_set_text( request, 4, thirdButtonText ))
    {
        ui_release( request );
        return E_OUTOFMEMORY;
    }
    request->default_button = defaultButton;
    request->cancel_button = cancelButton;
    return ui_begin( async, request, TRUE, x_game_ui_XGameUiShowMessageDialogAsync, "XGameUiShowMessageDialogAsync" );
}

static HRESULT WINAPI x_game_ui_XGameUiShowMessageDialogResult( IXGameUiImpl4 *iface, XAsyncBlock *async, XGameUiMessageDialogButton *resultButton )
{
    TRACE( "iface %p, async %p, resultButton %p.\n", iface, async, resultButton );

    if (!async || !resultButton) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, x_game_ui_XGameUiShowMessageDialogAsync, sizeof(*resultButton), resultButton, NULL );
}

static HRESULT WINAPI x_game_ui_XGameUiShowSendGameInviteAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, const char *sessionConfigurationId, const char *sessionTemplateName, const char *sessionId, const char *invitationText, const char *customActivationContext )
{
    TRACE( "iface %p, async %p, requestingUser %p, sessionConfigurationId %s, sessionTemplateName %s, sessionId %s, invitationText %s, customActivationContext %s.\n", iface, async, requestingUser, debugstr_a( sessionConfigurationId ), debugstr_a( sessionTemplateName ), debugstr_a( sessionId ), debugstr_a( invitationText ), debugstr_a( customActivationContext ) );

    struct ui_request *request;

    if (!async || !requestingUser || !sessionConfigurationId || !sessionTemplateName || !sessionId) return E_INVALIDARG;
    if (!(request = ui_create( UI_SEND_INVITE ))) return E_OUTOFMEMORY;
    if (!ui_set_text( request, 0, sessionConfigurationId ) || !ui_set_text( request, 1, sessionTemplateName ) || !ui_set_text( request, 2, sessionId ) ||
        !ui_set_text( request, 3, invitationText ) || !ui_set_text( request, 4, customActivationContext ) || !ui_set_user( request, requestingUser ))
    {
        ui_release( request );
        return E_OUTOFMEMORY;
    }
    return ui_begin( async, request, FALSE, x_game_ui_XGameUiShowSendGameInviteAsync, "XGameUiShowSendGameInviteAsync" );
}

static HRESULT WINAPI x_game_ui_XGameUiShowSendGameInviteResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p async %p.\n", iface, async );

    return ui_result( async, x_game_ui_XGameUiShowSendGameInviteAsync );
}

static HRESULT WINAPI x_game_ui_XGameUiShowPlayerProfileCardAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, UINT64 targetPlayer )
{
    TRACE( "iface %p, async %p, requestingUser %p, targetPlayer %llu.\n", iface, async, requestingUser, targetPlayer );

    struct ui_request *request;

    if (!async || !requestingUser || !targetPlayer) return E_INVALIDARG;
    if (!(request = ui_create( UI_PROFILE ))) return E_OUTOFMEMORY;
    if (!ui_set_user( request, requestingUser ))
    {
        ui_release( request );
        return E_INVALIDARG;
    }
    request->xuid = targetPlayer;
    request->path = L"/xbox/profile";
    sprintf( request->body, "%I64u", targetPlayer );
    return ui_begin( async, request, gdkc_bridge_available(), x_game_ui_XGameUiShowPlayerProfileCardAsync, "XGameUiShowPlayerProfileCardAsync" );
}

static HRESULT WINAPI x_game_ui_XGameUiShowPlayerProfileCardResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );

    return ui_result( async, x_game_ui_XGameUiShowPlayerProfileCardAsync );
}

static HRESULT WINAPI x_game_ui_XGameUiShowAchievementsAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, UINT32 titleId )
{
    TRACE( "iface %p, async %p, requestingUser %p, titleId %u.\n", iface, async, requestingUser, titleId );

    struct ui_request *request;

    if (!async || !requestingUser) return E_INVALIDARG;
    if (!(request = ui_create( UI_ACHIEVEMENTS ))) return E_OUTOFMEMORY;
    if (!ui_set_user( request, requestingUser ))
    {
        ui_release( request );
        return E_INVALIDARG;
    }
    request->title_id = titleId;
    request->path = L"/xbox/achievements";
    sprintf( request->body, "%u", titleId );
    return ui_begin( async, request, gdkc_bridge_available(), x_game_ui_XGameUiShowAchievementsAsync, "XGameUiShowAchievementsAsync" );
}

static HRESULT WINAPI x_game_ui_XGameUiShowAchievementsResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );

    return ui_result( async, x_game_ui_XGameUiShowAchievementsAsync );
}

static HRESULT WINAPI x_game_ui_XGameUiShowPlayerPickerAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, const char *promptText, UINT32 selectFromPlayersCount, const UINT64 *selectFromPlayers, UINT32 preSelectedPlayersCount, UINT64 *preSelectedPlayers, UINT32 minSelectionCount, UINT32 maxSelectionCount )
{
    TRACE( "iface %p, async %p, requestingUser %p, promptText %s, selectFromPlayersCount %u, selectFromPlayers %p, preSelectedPlayersCount %u, preSelectedPlayers %p, minSelectionCount %u, maxSelectionCount %u.\n", iface, async, requestingUser, debugstr_a( promptText ), selectFromPlayersCount, selectFromPlayers, preSelectedPlayersCount, preSelectedPlayers, minSelectionCount, maxSelectionCount );

    struct ui_request *request;

    if (!async || !requestingUser || !promptText || !selectFromPlayersCount || !selectFromPlayers || (preSelectedPlayersCount && !preSelectedPlayers)) return E_INVALIDARG;
    if (minSelectionCount > maxSelectionCount || preSelectedPlayersCount > maxSelectionCount) return E_INVALIDARG;
    if (!(request = ui_create( UI_PICKER ))) return E_OUTOFMEMORY;
    request->picker_from = malloc( selectFromPlayersCount * sizeof(UINT64) );
    request->picker_selected = malloc( (preSelectedPlayersCount + 1) * sizeof(UINT64) );
    if (!request->picker_from || !request->picker_selected || !ui_set_text( request, 0, promptText ) || !ui_set_user( request, requestingUser ))
    {
        ui_release( request );
        return E_OUTOFMEMORY;
    }
    memcpy( request->picker_from, selectFromPlayers, selectFromPlayersCount * sizeof(UINT64) );
    if (preSelectedPlayersCount) memcpy( request->picker_selected, preSelectedPlayers, preSelectedPlayersCount * sizeof(UINT64) );
    request->picker.requestingUser = request->user;
    request->picker.promptText = request->text[0];
    request->picker.selectFromPlayersCount = selectFromPlayersCount;
    request->picker.selectFromPlayers = request->picker_from;
    request->picker.preSelectedPlayersCount = preSelectedPlayersCount;
    request->picker.preSelectedPlayers = request->picker_selected;
    request->picker.minSelectionCount = minSelectionCount;
    request->picker.maxSelectionCount = maxSelectionCount;
    return ui_begin( async, request, FALSE, x_game_ui_XGameUiShowPlayerPickerAsync, "XGameUiShowPlayerPickerAsync" );
}

static HRESULT WINAPI x_game_ui_XGameUiShowPlayerPickerResultCount( IXGameUiImpl4 *iface, XAsyncBlock *async, UINT32 *resultPlayersCount )
{
    TRACE( "iface %p, async %p, resultPlayersCount %p.\n", iface, async, resultPlayersCount );

    SIZE_T size;
    HRESULT hr;

    if (!async || !resultPlayersCount) return E_INVALIDARG;
    if (SUCCEEDED(hr = IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, &size ))) *resultPlayersCount = size / sizeof(UINT64);
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowPlayerPickerResult( IXGameUiImpl4 *iface, XAsyncBlock *async, UINT32 resultPlayersCount, UINT64 *resultPlayers, UINT32 *resultPlayersUsed )
{
    TRACE( "iface %p, async %p, resultPlayersCount %u, resultPlayers %p, resultPlayersUsed %p.\n", iface, async, resultPlayersCount, resultPlayers, resultPlayersUsed );

    SIZE_T used = 0;
    HRESULT hr;

    if (!async || (resultPlayersCount && !resultPlayers)) return E_INVALIDARG;
    hr = ui_sized_result( async, x_game_ui_XGameUiShowPlayerPickerAsync, resultPlayersCount * sizeof(UINT64), resultPlayers, &used );
    if (SUCCEEDED(hr) && resultPlayersUsed) *resultPlayersUsed = used / sizeof(UINT64);
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowErrorDialogAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, HRESULT errorCode, const char *context )
{
    TRACE( "iface %p, async %p, errorCode %#lx, context %s.\n", iface, async, errorCode, debugstr_a( context ) );

    struct ui_request *request;

    if (!async || SUCCEEDED(errorCode)) return E_INVALIDARG;
    if (!(request = ui_create( UI_ERROR ))) return E_OUTOFMEMORY;
    if (!ui_set_text( request, 0, "Error" ) || !ui_set_text( request, 1, context ))
    {
        ui_release( request );
        return E_OUTOFMEMORY;
    }
    request->error = errorCode;
    return ui_begin( async, request, TRUE, x_game_ui_XGameUiShowErrorDialogAsync, "XGameUiShowErrorDialogAsync" );
}

static HRESULT WINAPI x_game_ui_XGameUiShowErrorDialogResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );

    return ui_result( async, x_game_ui_XGameUiShowErrorDialogAsync );
}

static HRESULT WINAPI x_game_ui_XGameUiSetNotificationPositionHint( IXGameUiImpl4 *iface, XGameUiNotificationPositionHint position )
{
    TRACE( "iface %p, position %d.\n", iface, position );

    if (position > XGameUiNotificationPositionHint_TopRight) return E_INVALIDARG;
    /* Only a hint: the desktop decides where its notifications appear. */
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiShowTextEntryAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, const char *titleText, const char *descriptionText, const char *defaultText, XGameUiTextEntryInputScope inputScope, UINT32 maxTextLength )
{
    TRACE( "iface %p, async %p, titleText %s, descriptionText %s, defaultText %s, inputScope %d, maxTextLength %u.\n", iface, async, debugstr_a( titleText ), debugstr_a( descriptionText ), debugstr_a( defaultText ), inputScope, maxTextLength );

    struct ui_request *request;

    if (!async) return E_INVALIDARG;
    if (!(request = ui_create( UI_TEXT ))) return E_OUTOFMEMORY;
    if (!ui_set_text( request, 0, titleText ) || !ui_set_text( request, 1, descriptionText ) || !ui_set_text( request, 2, defaultText ))
    {
        ui_release( request );
        return E_OUTOFMEMORY;
    }
    request->scope = inputScope;
    request->max_length = maxTextLength;
    return ui_begin( async, request, TRUE, x_game_ui_XGameUiShowTextEntryAsync, "XGameUiShowTextEntryAsync" );
}

static HRESULT WINAPI x_game_ui_XGameUiShowTextEntryResultSize( IXGameUiImpl4 *iface, XAsyncBlock *async, UINT32 *resultTextBufferSize )
{
    TRACE( "iface %p, async %p, resultTextBufferSize %p.\n", iface, async, resultTextBufferSize );

    SIZE_T size;
    HRESULT hr;

    if (!async || !resultTextBufferSize) return E_INVALIDARG;
    if (SUCCEEDED(hr = IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, &size ))) *resultTextBufferSize = size;
    return hr;
}

static HRESULT WINAPI x_game_ui_XGameUiShowTextEntryResult( IXGameUiImpl4 *iface, XAsyncBlock *async, UINT32 resultTextBufferSize, char *resultTextBuffer, UINT32 *resultTextBufferUsed )
{
    TRACE( "iface %p, async %p, resultTextBufferSize %u, resultTextBuffer %p, resultTextBufferUsed %p.\n", iface, async, resultTextBufferSize, resultTextBuffer, resultTextBufferUsed );

    SIZE_T used = 0;
    HRESULT hr;

    if (!async || !resultTextBuffer) return E_INVALIDARG;
    hr = ui_sized_result( async, x_game_ui_XGameUiShowTextEntryAsync, resultTextBufferSize, resultTextBuffer, &used );
    if (SUCCEEDED(hr) && resultTextBufferUsed) *resultTextBufferUsed = used;
    return hr;
}

static HRESULT WINAPI __PADDING__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_2__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_3__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_4__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiShowWebAuthenticationAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, const char *requestUri, const char *completionUri )
{
    FIXME( "iface %p, async %p, requestingUser %p, requestUri %s, completionUri %s stub!\n", iface, async, requestingUser, debugstr_a( requestUri ), debugstr_a( completionUri ) );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiShowWebAuthenticationResultSize( IXGameUiImpl4 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    FIXME( "iface %p, async %p, bufferSize %p stub!\n", iface, async, bufferSize );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiShowWebAuthenticationResult( IXGameUiImpl4 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XGameUiWebAuthenticationResultData **ptrToBuffer, SIZE_T *bufferUsed )
{
    FIXME( "iface %p, async %p, bufferSize %Iu, buffer %p, ptrToBuffer %p, bufferUsed %p stub!\n", iface, async, bufferSize, buffer, ptrToBuffer, bufferUsed );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiShowWebAuthenticationWithOptionsAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, const char *requestUri, const char *completionUri, XGameUiWebAuthenticationOptions options )
{
    FIXME( "iface %p, async %p, requestingUser %p, requestUri %s, completionUri %s, options %d stub!\n", iface, async, requestingUser, debugstr_a( requestUri ), debugstr_a( completionUri ), options );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_5__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_6__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiShowMultiplayerActivityGameInviteAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser )
{
    TRACE( "iface %p, async %p, requestingUser %p.\n", iface, async, requestingUser );

    struct ui_request *request;

    if (!async || !requestingUser) return E_INVALIDARG;
    if (!(request = ui_create( UI_ACTIVITY_INVITE ))) return E_OUTOFMEMORY;
    if (!ui_set_user( request, requestingUser ))
    {
        ui_release( request );
        return E_INVALIDARG;
    }
    return ui_begin( async, request, FALSE, x_game_ui_XGameUiShowMultiplayerActivityGameInviteAsync, "XGameUiShowMultiplayerActivityGameInviteAsync" );
}

static HRESULT WINAPI x_game_ui_XGameUiShowMultiplayerActivityGameInviteResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p.\n", iface, async );

    return ui_result( async, x_game_ui_XGameUiShowMultiplayerActivityGameInviteAsync );
}

static HRESULT WINAPI __PADDING_7__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_8__( IXGameUiImpl4 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryOpen( IXGameUiImpl4 *iface, const XGameUiTextEntryOptions *options, UINT32 maxLength, const char *initialText, UINT32 initialCursorIndex, XGameUiTextEntryHandle *handle )
{
    FIXME( "iface %p, options %p, maxLength %u, initialText %s, initialCursorIndex %u, handle %p stub!\n", iface, options, maxLength, debugstr_a( initialText ), initialCursorIndex, handle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryClose( IXGameUiImpl4 *iface, XGameUiTextEntryHandle handle )
{
    FIXME( "iface %p, handle %p stub!\n", iface, handle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryGetState( IXGameUiImpl4 *iface, XGameUiTextEntryHandle handle, XGameUiTextEntryChangeTypeFlags *changeType, UINT32 *cursorIndex, UINT32 *imeClauseStartIndex, UINT32 *imeClauseEndIndex, UINT32 bufferSize, char *buffer )
{
    FIXME( "iface %p, handle %p, changeType %p, cursorIndex %p, imeClauseStartIndex %p, imeClauseEndIndex %p, bufferSize %u, buffer %p stub!\n", iface, handle, changeType, cursorIndex, imeClauseStartIndex, imeClauseEndIndex, bufferSize, buffer );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryGetExtents( IXGameUiImpl4 *iface, XGameUiTextEntryHandle handle, XGameUiTextEntryExtents *extents )
{
    FIXME( "iface %p, handle %p, extents %p stub!\n", iface, handle, extents );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryUpdatePositionHint( IXGameUiImpl4 *iface, XGameUiTextEntryHandle handle, XGameUiTextEntryPositionHint positionHint )
{
    FIXME( "iface %p, handle %p, positionHint %d stub!\n", iface, handle, positionHint );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiTextEntryUpdateVisibility( IXGameUiImpl4 *iface, XGameUiTextEntryHandle handle, XGameUiTextEntryVisibilityFlags visibilityFlags )
{
    FIXME( "iface %p, handle %p, visibilityFlags %d stub!\n", iface, handle, visibilityFlags );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiShowStateShareAsync( IXGameUiImpl4 *iface, XAsyncBlock *async, XUserHandle requestingUser, const char *linkToken )
{
    FIXME( "iface %p, async %p, requestingUser %p, linkToken %s stub!\n", iface, async, requestingUser, debugstr_a( linkToken ) );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiShowStateShareResult( IXGameUiImpl4 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_ui_XGameUiSetUiCallbacks( IXGameUiImpl4 *iface, const XGameUiUiCallbacks *callbacks, BOOLEAN useSystemUiIfAvailable )
{
    TRACE( "iface %p, callbacks %p, useSystemUiIfAvailable %d.\n", iface, callbacks, useSystemUiIfAvailable );

    EnterCriticalSection( &ui_cs );
    if (callbacks) ui_callbacks = *callbacks;
    else memset( &ui_callbacks, 0, sizeof(ui_callbacks) );
    prefer_system_ui = useSystemUiIfAvailable;
    LeaveCriticalSection( &ui_cs );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetMessageDialogUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle, XGameUiMessageDialogButton response )
{
    TRACE( "iface %p, callbackHandle %p, response %d.\n", iface, callbackHandle, response );

    struct ui_request *request;

    if (response > XGameUiMessageDialogButton_Third) return E_INVALIDARG;
    if (!(request = ui_take( callbackHandle, UI_MESSAGE ))) return E_INVALIDARG;
    request->button = response;
    ui_complete( request, S_OK );
    ui_release( request );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetPlayerPickerUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle, UINT32 playerCount, const UINT64 *players )
{
    TRACE( "iface %p, callbackHandle %p, playerCount %u, players %p.\n", iface, callbackHandle, playerCount, players );

    struct ui_request *request;
    UINT64 *copy = NULL;

    if (playerCount && !players) return E_INVALIDARG;
    if (playerCount)
    {
        if (!(copy = malloc( playerCount * sizeof(*copy) ))) return E_OUTOFMEMORY;
        memcpy( copy, players, playerCount * sizeof(*copy) );
    }
    if (!(request = ui_take( callbackHandle, UI_PICKER )))
    {
        free( copy );
        return E_INVALIDARG;
    }
    request->players = copy;
    request->player_count = playerCount;
    ui_complete( request, S_OK );
    ui_release( request );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetTextEntryUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle, const char *response )
{
    TRACE( "iface %p, callbackHandle %p, response %s.\n", iface, callbackHandle, debugstr_a( response ) );

    struct ui_request *request;
    char *copy;

    if (!response) return E_INVALIDARG;
    if (!(copy = strdup( response ))) return E_OUTOFMEMORY;
    if (!(request = ui_take( callbackHandle, UI_TEXT )))
    {
        free( copy );
        return E_INVALIDARG;
    }
    ui_limit_text( copy, request->max_length );
    request->result_text = copy;
    ui_complete( request, S_OK );
    ui_release( request );
    return S_OK;
}

static HRESULT WINAPI x_game_ui_XGameUiSetPlayerProfileCardUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle )
{
    TRACE( "iface %p, callbackHandle %p.\n", iface, callbackHandle );

    return ui_respond( callbackHandle, UI_PROFILE );
}

static HRESULT WINAPI x_game_ui_XGameUiSetSendGameInviteUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle )
{
    TRACE( "iface %p, callbackHandle %p.\n", iface, callbackHandle );

    return ui_respond( callbackHandle, UI_SEND_INVITE );
}

static HRESULT WINAPI x_game_ui_XGameUiSetAchievementsUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle )
{
    TRACE( "iface %p, callbackHandle %p.\n", iface, callbackHandle );

    return ui_respond( callbackHandle, UI_ACHIEVEMENTS );
}

static HRESULT WINAPI x_game_ui_XGameUiSetMultiplayerActivityGameInviteUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle )
{
    TRACE( "iface %p, callbackHandle %p.\n", iface, callbackHandle );

    return ui_respond( callbackHandle, UI_ACTIVITY_INVITE );
}

static HRESULT WINAPI x_game_ui_XGameUiSetErrorDialogUiResponse( IXGameUiImpl4 *iface, XGameUiCallbackHandle callbackHandle )
{
    TRACE( "iface %p, callbackHandle %p.\n", iface, callbackHandle );

    return ui_respond( callbackHandle, UI_ERROR );
}

static const struct IXGameUiImpl4Vtbl x_game_ui_vtbl =
{
    x_game_ui_QueryInterface,
    x_game_ui_AddRef,
    x_game_ui_Release,
    /* IXGameUiImpl methods */
    x_game_ui_XGameUiShowMessageDialogAsync,
    x_game_ui_XGameUiShowMessageDialogResult,
    x_game_ui_XGameUiShowSendGameInviteAsync,
    x_game_ui_XGameUiShowSendGameInviteResult,
    x_game_ui_XGameUiShowPlayerProfileCardAsync,
    x_game_ui_XGameUiShowPlayerProfileCardResult,
    x_game_ui_XGameUiShowAchievementsAsync,
    x_game_ui_XGameUiShowAchievementsResult,
    x_game_ui_XGameUiShowPlayerPickerAsync,
    x_game_ui_XGameUiShowPlayerPickerResultCount,
    x_game_ui_XGameUiShowPlayerPickerResult,
    x_game_ui_XGameUiShowErrorDialogAsync,
    x_game_ui_XGameUiShowErrorDialogResult,
    x_game_ui_XGameUiSetNotificationPositionHint,
    x_game_ui_XGameUiShowTextEntryAsync,
    x_game_ui_XGameUiShowTextEntryResultSize,
    x_game_ui_XGameUiShowTextEntryResult,
    __PADDING__,
    __PADDING_2__,
    __PADDING_3__,
    __PADDING_4__,
    x_game_ui_XGameUiShowWebAuthenticationAsync,
    x_game_ui_XGameUiShowWebAuthenticationResultSize,
    x_game_ui_XGameUiShowWebAuthenticationResult,
    x_game_ui_XGameUiShowWebAuthenticationWithOptionsAsync,
    __PADDING_5__,
    __PADDING_6__,
    /* IXGameUiImpl2 methods */
    x_game_ui_XGameUiShowMultiplayerActivityGameInviteAsync,
    x_game_ui_XGameUiShowMultiplayerActivityGameInviteResult,
    __PADDING_7__,
    __PADDING_8__,
    x_game_ui_XGameUiTextEntryOpen,
    x_game_ui_XGameUiTextEntryClose,
    x_game_ui_XGameUiTextEntryGetState,
    x_game_ui_XGameUiTextEntryGetExtents,
    x_game_ui_XGameUiTextEntryUpdatePositionHint,
    x_game_ui_XGameUiTextEntryUpdateVisibility,
    /* IXGameUiImpl3 methods */
    x_game_ui_XGameUiShowStateShareAsync,
    x_game_ui_XGameUiShowStateShareResult,
    /* IXGameUiImpl4 methods */
    x_game_ui_XGameUiSetUiCallbacks,
    x_game_ui_XGameUiSetMessageDialogUiResponse,
    x_game_ui_XGameUiSetPlayerPickerUiResponse,
    x_game_ui_XGameUiSetTextEntryUiResponse,
    x_game_ui_XGameUiSetPlayerProfileCardUiResponse,
    x_game_ui_XGameUiSetSendGameInviteUiResponse,
    x_game_ui_XGameUiSetAchievementsUiResponse,
    x_game_ui_XGameUiSetMultiplayerActivityGameInviteUiResponse,
    x_game_ui_XGameUiSetErrorDialogUiResponse,
};

static struct x_game_ui x_game_ui =
{
    {&x_game_ui_vtbl},
    0,
};

IXGameUiImpl *x_game_ui_impl = (IXGameUiImpl *)&x_game_ui.IXGameUiImpl4_iface;

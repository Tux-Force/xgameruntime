/* System UI (XGameUi) and gamer picture tests; the runner supplies a local fake bridge. The dialogs are real windows,
 * operated from here. */
#include <initguid.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <xasync.h>
#include <xasyncprovider.h>
#include <xtaskqueue.h>
#include <xuser.h>
#include <xgameui.h>

typedef HRESULT (WINAPI *query_api_fn)( const GUID *, const GUID *, void ** );
#define CHECK_HR(call, expected) do { HRESULT result = (call); if (result != (HRESULT)(expected)) { \
    printf( "FAIL line %d: hr %#lx expected %#lx\n", __LINE__, result, (HRESULT)(expected) ); return 1; } } while (0)
#define CHECK(value) do { if (!(value)) { printf( "FAIL line %d: %s\n", __LINE__, #value ); return 1; } } while (0)

static IXThreadingImpl *threading;
static IXGameUiImpl4 *ui;
static XGameUiCallbackHandle last_handle;
static HANDLE called;
static int context_value;

static HWND wait_window( const WCHAR *title )
{
    HWND hwnd;
    int i;

    for (i = 0; i < 400; i++)
    {
        if ((hwnd = FindWindowW( NULL, title )) && IsWindowVisible( hwnd )) return hwnd;
        Sleep( 25 );
    }
    return NULL;
}

static BOOL has_text( HWND hwnd, const WCHAR *text )
{
    WCHAR buffer[2048];
    HWND child = NULL;

    while ((child = FindWindowExW( hwnd, child, NULL, NULL )))
    {
        buffer[0] = 0;
        SendMessageW( child, WM_GETTEXT, ARRAYSIZE(buffer), (LPARAM)buffer );
        if (wcsstr( buffer, text )) return TRUE;
    }
    return FALSE;
}

static void __stdcall on_message( XGameUiCallbackHandle handle, XTaskQueueHandle queue, const char *title, const char *content, const char *first, const char *second, const char *third, XGameUiMessageDialogButton def, XGameUiMessageDialogButton cancel, void *context )
{
    if (queue && !strcmp( title, "T" ) && !strcmp( content, "C" ) && first && !second && context == &context_value) last_handle = handle;
    SetEvent( called );
}

static void __stdcall on_text( XGameUiCallbackHandle handle, XTaskQueueHandle queue, const char *title, const char *description, const char *def, XGameUiTextEntryInputScope scope, UINT32 max, void *context )
{
    last_handle = handle;
    SetEvent( called );
}

static void __stdcall on_picker( XGameUiCallbackHandle handle, XTaskQueueHandle queue, XGameUiPlayerPickerInfo *info, void *context )
{
    if (info->selectFromPlayersCount == 2 && info->selectFromPlayers[1] == 22 && info->maxSelectionCount == 2) last_handle = handle;
    SetEvent( called );
}

static int dialogs( XTaskQueueHandle queue, XUserHandle user )
{
    XAsyncBlock async = { .queue = queue };
    XGameUiMessageDialogButton button;
    char text[64];
    UINT32 size, used;
    HWND hwnd;
    int i;

    /* Each button, and closing the window. */
    for (i = 0; i < 4; i++)
    {
        memset( &async, 0, sizeof(async) );
        async.queue = queue;
        CHECK_HR( IXGameUiImpl4_XGameUiShowMessageDialogAsync( ui, &async, "Title \xc3\xa4", "Line 1\nLine 2", "One", "Two", "Three", XGameUiMessageDialogButton_First, XGameUiMessageDialogButton_Second ), S_OK );
        CHECK( (hwnd = wait_window( L"Title ä" )) );
        CHECK( has_text( hwnd, L"Line 1\r\nLine 2" ) && has_text( hwnd, L"Three" ) );
        CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, FALSE ), E_PENDING );
        PostMessageW( hwnd, i < 3 ? WM_COMMAND : WM_CLOSE, i < 3 ? 100 + i : 0, 0 );
        CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), S_OK );
        CHECK_HR( IXGameUiImpl4_XGameUiShowMessageDialogResult( ui, &async, &button ), S_OK );
        CHECK( button == (i < 3 ? i : 1) );
    }

    /* Cancelling the call closes the window. */
    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    CHECK_HR( IXGameUiImpl4_XGameUiShowMessageDialogAsync( ui, &async, "Cancel me", "x", NULL, NULL, NULL, 0, 0 ), S_OK );
    CHECK( (hwnd = wait_window( L"Cancel me" )) );
    IXThreadingImpl_XAsyncCancel( threading, &async );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), E_ABORT );
    for (i = 0; i < 200 && IsWindow( hwnd ); i++) Sleep( 25 );
    CHECK( !IsWindow( hwnd ) );
    /* Also when it comes before the window. */
    for (i = 0; i < 20; i++)
    {
        memset( &async, 0, sizeof(async) );
        async.queue = queue;
        CHECK_HR( IXGameUiImpl4_XGameUiShowMessageDialogAsync( ui, &async, "Quick", "x", NULL, NULL, NULL, 0, 0 ), S_OK );
        IXThreadingImpl_XAsyncCancel( threading, &async );
        CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), E_ABORT );
    }
    for (i = 0; i < 200 && FindWindowW( NULL, L"Quick" ); i++) Sleep( 25 );
    CHECK( !FindWindowW( NULL, L"Quick" ) );

    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    CHECK_HR( IXGameUiImpl4_XGameUiShowErrorDialogAsync( ui, &async, 0x80070005, NULL ), S_OK );
    CHECK( (hwnd = wait_window( L"Error" )) && has_text( hwnd, L"0x80070005" ) );
    PostMessageW( hwnd, WM_COMMAND, 100, 0 );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), S_OK );
    CHECK_HR( IXGameUiImpl4_XGameUiShowErrorDialogResult( ui, &async ), S_OK );

    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    CHECK_HR( IXGameUiImpl4_XGameUiShowTextEntryAsync( ui, &async, "Entry", "Name?", "old", XGameUiTextEntryInputScope_Default, 20 ), S_OK );
    CHECK( (hwnd = wait_window( L"Entry" )) && has_text( hwnd, L"old" ) );
    SetDlgItemTextW( hwnd, 200, L"näw" );
    PostMessageW( hwnd, WM_COMMAND, 100, 0 );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), S_OK );
    CHECK_HR( IXGameUiImpl4_XGameUiShowTextEntryResultSize( ui, &async, &size ), S_OK );
    CHECK( size == 5 );
    CHECK_HR( IXGameUiImpl4_XGameUiShowTextEntryResult( ui, &async, sizeof(text), text, &used ), S_OK );
    CHECK( used == 5 && !strcmp( text, "n\xc3\xa4w" ) );
    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    CHECK_HR( IXGameUiImpl4_XGameUiShowTextEntryAsync( ui, &async, "Entry", NULL, NULL, 0, 20 ), S_OK );
    CHECK( (hwnd = wait_window( L"Entry" )) );
    PostMessageW( hwnd, WM_COMMAND, IDCANCEL, 0 );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), E_ABORT );

    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    CHECK_HR( IXGameUiImpl4_XGameUiShowAchievementsAsync( ui, &async, user, 7 ), S_OK );
    CHECK( (hwnd = wait_window( L"Achievements" )) && has_text( hwnd, L"line\r\nline" ) );
    PostMessageW( hwnd, WM_CLOSE, 0, 0 );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), S_OK );
    CHECK_HR( IXGameUiImpl4_XGameUiShowAchievementsResult( ui, &async ), S_OK );

    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    CHECK_HR( IXGameUiImpl4_XGameUiShowPlayerProfileCardAsync( ui, &async, user, 2533274790395904ull ), S_OK );
    CHECK( (hwnd = wait_window( L"Pläyer" )) && has_text( hwnd, L"Gamerscore: 12" ) );
    PostMessageW( hwnd, WM_COMMAND, 100, 0 );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), S_OK );
    CHECK_HR( IXGameUiImpl4_XGameUiShowPlayerProfileCardResult( ui, &async ), S_OK );

    /* No UI of our own for these. */
    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    CHECK_HR( IXGameUiImpl4_XGameUiShowMultiplayerActivityGameInviteAsync( ui, &async, user ), E_NOTIMPL );
    CHECK_HR( IXGameUiImpl4_XGameUiSetNotificationPositionHint( ui, XGameUiNotificationPositionHint_TopLeft ), S_OK );
    return 0;
}

static int callbacks( XTaskQueueHandle queue, XUserHandle user )
{
    XGameUiUiCallbacks table = { .context = &context_value, .showMessageDialogCallback = on_message, .showTextEntryCallback = on_text, .showPlayerPickerCallback = on_picker };
    const UINT64 from[] = { 11, 22 };
    XAsyncBlock async = { .queue = queue };
    XGameUiMessageDialogButton button;
    UINT64 players[2];
    UINT32 count;
    HWND hwnd;
    char text[8];

    CHECK_HR( IXGameUiImpl4_XGameUiSetUiCallbacks( ui, &table, FALSE ), S_OK );
    CHECK_HR( IXGameUiImpl4_XGameUiShowMessageDialogAsync( ui, &async, "T", "C", "A", NULL, NULL, 0, 0 ), S_OK );
    CHECK( !WaitForSingleObject( called, 5000 ) && last_handle );
    CHECK_HR( IXGameUiImpl4_XGameUiSetTextEntryUiResponse( ui, last_handle, "wrong kind" ), E_INVALIDARG );
    CHECK_HR( IXGameUiImpl4_XGameUiSetMessageDialogUiResponse( ui, last_handle, XGameUiMessageDialogButton_Third ), S_OK );
    CHECK_HR( IXGameUiImpl4_XGameUiSetMessageDialogUiResponse( ui, last_handle, XGameUiMessageDialogButton_First ), E_INVALIDARG );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), S_OK );
    CHECK_HR( IXGameUiImpl4_XGameUiShowMessageDialogResult( ui, &async, &button ), S_OK );
    CHECK( button == XGameUiMessageDialogButton_Third );

    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    CHECK_HR( IXGameUiImpl4_XGameUiShowTextEntryAsync( ui, &async, "T", "C", NULL, 0, 4 ), S_OK );
    CHECK( !WaitForSingleObject( called, 5000 ) );
    CHECK_HR( IXGameUiImpl4_XGameUiSetTextEntryUiResponse( ui, last_handle, "abc" ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), S_OK );
    CHECK_HR( IXGameUiImpl4_XGameUiShowTextEntryResult( ui, &async, sizeof(text), text, NULL ), S_OK );
    CHECK( !strcmp( text, "abc" ) );

    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    last_handle = NULL;
    CHECK_HR( IXGameUiImpl4_XGameUiShowPlayerPickerAsync( ui, &async, user, "Pick", 2, from, 0, NULL, 1, 2 ), S_OK );
    CHECK( !WaitForSingleObject( called, 5000 ) && last_handle );
    CHECK_HR( IXGameUiImpl4_XGameUiSetPlayerPickerUiResponse( ui, last_handle, 1, from + 1 ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), S_OK );
    CHECK_HR( IXGameUiImpl4_XGameUiShowPlayerPickerResultCount( ui, &async, &count ), S_OK );
    CHECK( count == 1 );
    CHECK_HR( IXGameUiImpl4_XGameUiShowPlayerPickerResult( ui, &async, 2, players, &count ), S_OK );
    CHECK( count == 1 && players[0] == 22 );

    /* Cancelled while the game shows its UI; its answer comes too late. */
    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    CHECK_HR( IXGameUiImpl4_XGameUiShowMessageDialogAsync( ui, &async, "T", "C", "A", NULL, NULL, 0, 0 ), S_OK );
    CHECK( !WaitForSingleObject( called, 5000 ) );
    IXThreadingImpl_XAsyncCancel( threading, &async );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), E_ABORT );
    CHECK_HR( IXGameUiImpl4_XGameUiSetMessageDialogUiResponse( ui, last_handle, 0 ), E_INVALIDARG );

    /* The game prefers the system UI where there is one. */
    CHECK_HR( IXGameUiImpl4_XGameUiSetUiCallbacks( ui, &table, TRUE ), S_OK );
    memset( &async, 0, sizeof(async) );
    async.queue = queue;
    CHECK_HR( IXGameUiImpl4_XGameUiShowMessageDialogAsync( ui, &async, "System", "C", "A", NULL, NULL, 0, 0 ), S_OK );
    CHECK( (hwnd = wait_window( L"System" )) );
    PostMessageW( hwnd, WM_COMMAND, 100, 0 );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), S_OK );
    CHECK_HR( IXGameUiImpl4_XGameUiShowMessageDialogResult( ui, &async, &button ), S_OK );
    CHECK_HR( IXGameUiImpl4_XGameUiSetUiCallbacks( ui, NULL, FALSE ), S_OK );
    return 0;
}

static int picture( IXUserImpl *users, XTaskQueueHandle queue, XUserHandle user )
{
    XAsyncBlock async = { .queue = queue };
    BYTE buffer[1024];
    SIZE_T size, used;
    int i;

    CHECK_HR( IXUserImpl_XUserGetGamerPictureAsync( users, user, 4, &async ), E_INVALIDARG );
    CHECK_HR( IXUserImpl_XUserGetGamerPictureAsync( users, user, XUserGamerPictureSize_Medium, &async ), S_OK );
    CHECK_HR( IXThreadingImpl_XAsyncGetStatus( threading, &async, TRUE ), S_OK );
    CHECK_HR( IXUserImpl_XUserGetGamerPictureResultSize( users, &async, &size ), S_OK );
    CHECK( size == 769 );
    CHECK_HR( IXUserImpl_XUserGetGamerPictureResult( users, &async, sizeof(buffer), buffer, &used ), S_OK );
    CHECK( used == size && buffer[768] == 'x' );
    for (i = 0; i < 768; i++) CHECK( buffer[i] == (i & 0xff) );
    return 0;
}

int main( void )
{
    query_api_fn query;
    XTaskQueueHandle queue;
    IXUserImpl *users;
    XUserHandle user;
    HMODULE dll;

    CHECK( (dll = LoadLibraryA( "xgameruntime.dll" )) );
    CHECK( (query = (query_api_fn)GetProcAddress( dll, "QueryApiImpl" )) );
    CHECK_HR( query( &CLSID_XUserImpl, &IID_IXUserImpl, (void **)&users ), S_OK );
    CHECK_HR( query( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&threading ), S_OK );
    CHECK_HR( query( &CLSID_XGameUiImpl, &IID_IXGameUiImpl4, (void **)&ui ), S_OK );
    CHECK_HR( IXUserImpl_XUserFindUserById( users, 2533274790395900ull, &user ), S_OK );
    CHECK_HR( IXThreadingImpl_XTaskQueueCreate( threading, XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &queue ), S_OK );
    called = CreateEventW( NULL, FALSE, FALSE, NULL );

    if (picture( users, queue, user ) || dialogs( queue, user ) || callbacks( queue, user )) return 1;
    printf( "ok\n" );
    return 0;
}

/* Regression coverage for the GDK save error contract, through the public COM API. */
#include <initguid.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <xgameerr.h>
#include <xgamesave.h>
#include <xuser.h>

typedef HRESULT (WINAPI *query_api_fn)( const GUID *, const GUID *, void ** );

#define CHECK_HR(call, expected) do { HRESULT result = (call); \
    if (result != (expected)) { printf("FAIL line %d: %#lx, expected %#lx\n", \
        __LINE__, result, (HRESULT)(expected)); return 1; } } while (0)

int main(void)
{
    HMODULE dll;
    query_api_fn query;
    IXUserImpl *users;
    IXGameSaveImpl *saves;
    XUserHandle user;
    XGameSaveProviderHandle provider;
    XGameSaveContainerHandle container;
    XGameSaveUpdateHandle update;
    const char *names[] = { "data" };
    BYTE buffer[256];
    UINT32 count = 1;
    XGameSaveBlob *blob = (XGameSaveBlob *)buffer;

    char temp[MAX_PATH], local[MAX_PATH];

    /* Keep the saves out of the real profile: the storage path follows LOCALAPPDATA. */
    if (!GetTempPathA(sizeof(temp), temp)) return 2;
    snprintf(local, sizeof(local), "%sxgameruntime-gamesave-test-%lu", temp, GetCurrentProcessId());
    CreateDirectoryA(local, NULL);
    SetEnvironmentVariableA("LOCALAPPDATA", local);
    SetEnvironmentVariableA("XODUS_XUID", "123");
    dll = LoadLibraryA("xgameruntime.dll");
    if (!dll) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 2; }
    query = (query_api_fn)GetProcAddress(dll, "QueryApiImpl");
    if (!query) { printf("QueryApiImpl export missing: %lu\n", GetLastError()); return 2; }
    CHECK_HR(query(&CLSID_XUserImpl, &IID_IXUserImpl, (void **)&users), S_OK);
    CHECK_HR(query(&CLSID_XGameSaveImpl, &IID_IXGameSaveImpl, (void **)&saves), S_OK);
    CHECK_HR(IXUserImpl_XUserFindUserById(users, 123, &user), S_OK);
    CHECK_HR(IXGameSaveImpl_XGameSaveInitializeProvider(saves, user, "gdk-error-regression", FALSE, &provider), S_OK);
    CHECK_HR(IXGameSaveImpl_XGameSaveDeleteContainer(saves, provider, "errors"), S_OK);
    CHECK_HR(IXGameSaveImpl_XGameSaveCreateContainer(saves, provider, "errors", &container), S_OK);

    /* A new profile is absent, not a general Windows file failure. */
    CHECK_HR(IXGameSaveImpl_XGameSaveReadBlobData(saves, container, names, &count, sizeof(buffer), blob), E_GS_BLOB_NOT_FOUND);
    CHECK_HR(IXGameSaveImpl_XGameSaveCreateUpdate(saves, container, "Errors", &update), S_OK);
    CHECK_HR(IXGameSaveImpl_XGameSaveSubmitBlobWrite(saves, update, "data", (UINT8 *)"hello", 5), S_OK);
    CHECK_HR(IXGameSaveImpl_XGameSaveSubmitUpdate(saves, update), S_OK);
    IXGameSaveImpl_XGameSaveCloseUpdate(saves, update);

    count = 1;
    CHECK_HR(IXGameSaveImpl_XGameSaveReadBlobData(saves, container, names, &count, 1, blob), E_GS_PROVIDED_BUFFER_TOO_SMALL);
    count = 1;
    CHECK_HR(IXGameSaveImpl_XGameSaveReadBlobData(saves, container, names, &count, sizeof(buffer), blob), S_OK);
    if (count != 1 || blob->info.size != 5 || memcmp(blob->data, "hello", 5)) return 1;

    IXGameSaveImpl_XGameSaveCloseContainer(saves, container);
    CHECK_HR(IXGameSaveImpl_XGameSaveDeleteContainer(saves, provider, "errors"), S_OK);
    IXGameSaveImpl_XGameSaveCloseProvider(saves, provider);
    IXUserImpl_XUserCloseHandle(users, user);
    IXGameSaveImpl_Release(saves);
    IXUserImpl_Release(users);
    puts("Game save missing-blob, small-buffer and round-trip checks passed.");
    return 0;
}

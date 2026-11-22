/* XUser token ABI and UTF-16 regression tests. The runner supplies a local fake bridge. */
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

typedef HRESULT (WINAPI *query_api_fn)( const GUID *, const GUID *, void ** );
#define CHECK_HR(call, expected) do { HRESULT result = (call); if (result != (expected)) { \
    printf("FAIL line %d: hr %#lx expected %#lx\n", __LINE__, result, (HRESULT)(expected)); return 1; } } while (0)
#define CHECK(value) do { if (!(value)) { printf("FAIL line %d: %s\n",__LINE__,#value); return 1; } } while (0)

int main(void)
{
    HMODULE dll;
    query_api_fn query;
    IXUserImpl *users;
    IXThreadingImpl *threading;
    XUserHandle user;
    XTaskQueueHandle queue;
    XAsyncBlock async = {0};
    XUserGetTokenAndSignatureUtf16Data *wide;
    XUserGetTokenAndSignatureData *narrow;
    SIZE_T size, used;
    void *buffer;
    const BYTE body[] = {'a',0,'b'};
    const WCHAR invalid[] = {0xd800,0};
    const WCHAR expected_token[] = L"test-token-\u00e4-\U0001f30d";
    const WCHAR expected_signature[] = L"sig-\u03b2";

    SetEnvironmentVariableA("XODUS_XUID","123");
    dll = LoadLibraryA("xgameruntime.dll"); CHECK(dll);
    query=(query_api_fn)GetProcAddress(dll,"QueryApiImpl"); CHECK(query);
    CHECK_HR(query(&CLSID_XUserImpl,&IID_IXUserImpl,(void **)&users),S_OK);
    CHECK_HR(query(&CLSID_XThreadingImpl,&IID_IXThreadingImpl,(void **)&threading),S_OK);
    CHECK_HR(IXUserImpl_XUserFindUserById(users,123,&user),S_OK);
    CHECK_HR(IXThreadingImpl_XTaskQueueCreate(threading,XTaskQueueDispatchMode_ThreadPool,XTaskQueueDispatchMode_ThreadPool,&queue),S_OK);
    async.queue=queue;

    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureAsync(users,user,0,"GET","https://example.test/utf8",0,NULL,0,NULL,&async),S_OK);
    CHECK_HR(IXThreadingImpl_XAsyncGetStatus(threading,&async,TRUE),S_OK);
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureResultSize(users,&async,&size),S_OK);
    buffer=malloc(size);CHECK(buffer);
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureResult(users,&async,size,buffer,&narrow,&used),S_OK);
    CHECK(used==size && narrow==buffer);
    CHECK(!strcmp(narrow->token,"test-token-\xc3\xa4-\xf0\x9f\x8c\x8d"));
    CHECK(narrow->tokenSize==strlen(narrow->token) && !strcmp(narrow->signature,"sig-\xce\xb2"));
    free(buffer);

    memset(&async,0,sizeof(async));async.queue=queue;
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16Async(users,user,XUserGetTokenAndSignatureOptions_ForceRefresh,L"POST",L"https://example.test/utf16/\u00e4",0,NULL,sizeof(body),body,&async),S_OK);
    CHECK_HR(IXThreadingImpl_XAsyncGetStatus(threading,&async,TRUE),S_OK);
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16ResultSize(users,&async,&size),S_OK);
    CHECK(size==sizeof(*wide)+(wcslen(expected_token)+wcslen(expected_signature)+2)*sizeof(WCHAR));
    buffer=malloc(size);CHECK(buffer);wide=(void *)1;
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16Result(users,&async,size-1,buffer,&wide,NULL),HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    CHECK(wide==NULL);
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16Result(users,&async,size,buffer,&wide,&used),S_OK);
    CHECK(wide==buffer && used==size);
    CHECK(!wcscmp(wide->token,expected_token) && !wcscmp(wide->signature,expected_signature));
    CHECK(wide->tokenCount==wcslen(expected_token) && wide->signatureCount==wcslen(expected_signature));
    free(buffer);

    memset(&async,0,sizeof(async));async.queue=queue;
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16Async(users,user,0,L"GET",L"https://example.test/denied",0,NULL,0,NULL,&async),S_OK);
    CHECK_HR(IXThreadingImpl_XAsyncGetStatus(threading,&async,TRUE),E_ACCESSDENIED);

    memset(&async,0,sizeof(async));async.queue=queue;
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16Async(users,user,0,L"GET",L"https://example.test/empty",0,NULL,0,NULL,&async),S_OK);
    CHECK_HR(IXThreadingImpl_XAsyncGetStatus(threading,&async,TRUE),S_OK);
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16ResultSize(users,&async,&size),S_OK);
    buffer=malloc(size);CHECK(buffer);
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16Result(users,&async,size,buffer,&wide,&used),S_OK);
    CHECK(wide->tokenCount==0 && wide->signatureCount==0 && !*wide->token && !*wide->signature);
    free(buffer);

    memset(&async,0,sizeof(async));async.queue=queue;
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16Async(users,user,0,L"GET",L"https://example.test/invalid",0,NULL,0,NULL,&async),S_OK);
    CHECK_HR(IXThreadingImpl_XAsyncGetStatus(threading,&async,TRUE),HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION));
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16Async(users,user,0,L"GET",invalid,0,NULL,0,NULL,&async),HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION));
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16Async(users,user,0,L"GET",L"https://example.test/\r\ninjected",0,NULL,0,NULL,&async),E_INVALIDARG);
    CHECK_HR(IXUserImpl_XUserGetTokenAndSignatureUtf16Async(users,user,0,L"GET",L"https://example.test",0,NULL,1,NULL,&async),E_INVALIDARG);

    IXUserImpl_XUserCloseHandle(users,user);
    IXThreadingImpl_XTaskQueueCloseHandle(threading,queue);
    IXUserImpl_Release(users);IXThreadingImpl_Release(threading);
    puts("UTF-8 and UTF-16 token bridge, Unicode buffers, error propagation and validation passed.");
    return 0;
}

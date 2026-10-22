// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* XUser/XAsync contract tests. Synthetic identities only in fixtures.
 * Copyright 2026 Xodus contributors. LGPL-2.1-or-later. */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <initguid.h>
#include "xasyncprovider.h"
#include "xuser.h"
#include "xgameerr.h"
static IXThreadingImpl *api;
static IXUserImpl6 *user;
static LONG checks, failures, callbacks, cleanups;
static DWORD callback_thread;
static int heap_identity;
static HRESULT picker_expected=S_OK;
#define check(e) do { ++checks; if (!(e)) { ++failures; printf("FAIL %u: %s\n",__LINE__,#e); } } while(0)
#define status(a,w) IXThreadingImpl_XAsyncGetStatus(api,a,w)
#define result(a,u) IXUserImpl6_XUserAddResult(user,a,u)
#define add(a) IXUserImpl6_XUserAddAsync(user,XUserAddOptions_AddDefaultUserSilently,a)
#define dispatch(q,p) IXThreadingImpl_XTaskQueueDispatch(api,q,p,1000)
static void CALLBACK callback(XAsyncBlock *block)
{
    InterlockedIncrement(&callbacks);
    callback_thread = GetCurrentThreadId();
}
static HRESULT WINAPI provider(XAsyncOp op, const XAsyncProviderData *data)
{
    switch (op)
    {
    case XAsyncOp_Begin: return IXThreadingImpl_XAsyncSchedule(api,data->async,0);
    case XAsyncOp_DoWork:
        IXThreadingImpl_XAsyncComplete(api,data->async,S_OK,sizeof(UINT32));
        return E_PENDING;
    case XAsyncOp_GetResult: *(UINT32 *)data->buffer=42; return S_OK;
    case XAsyncOp_Cancel: IXThreadingImpl_XAsyncComplete(api,data->async,E_ABORT,0); return S_OK;
    case XAsyncOp_Cleanup: InterlockedIncrement(&cleanups); return S_OK;
    }
    return E_NOTIMPL;
}
static void CALLBACK consume_and_free(XAsyncBlock *block)
{
    UINT32 value=0;
    check(IXThreadingImpl_XAsyncGetResult(api,block,&heap_identity,4,&value,NULL)==S_OK && value==42);
    free(block);
    ++callbacks;
}
static void defaults_and_heap(void)
{
    XAsyncBlock a={0}, *heap;
    XTaskQueueHandle q;
    UINT32 value=0;
    LONG before=cleanups;
    check(IXThreadingImpl_XAsyncBegin(api,&a,NULL,&heap_identity,"null queue",provider)==S_OK);
    check(status(&a,TRUE)==S_OK);
    check(IXThreadingImpl_XAsyncGetResult(api,&a,&heap_identity,4,&value,NULL)==S_OK && value==42);
    check(IXThreadingImpl_XTaskQueueCreate(api,XTaskQueueDispatchMode_Manual,XTaskQueueDispatchMode_Manual,&q)==S_OK);
    heap=calloc(1,sizeof(*heap)); heap->queue=q; heap->callback=consume_and_free;
    check(IXThreadingImpl_XAsyncBegin(api,heap,NULL,&heap_identity,"heap callback",provider)==S_OK);
    check(dispatch(q,XTaskQueuePort_Work));
    check(dispatch(q,XTaskQueuePort_Completion));
    /* No more references to heap are permitted: the callback freed it. */
    check(cleanups>=before+1);
    check(IXThreadingImpl_XTaskQueueTerminate(api,q,FALSE,NULL,NULL)==S_OK);
    while(dispatch(q,XTaskQueuePort_Work)) {}
    while(dispatch(q,XTaskQueuePort_Completion)) {}
    IXThreadingImpl_XTaskQueueCloseHandle(api,q);
}
static void drain(XTaskQueueHandle q)
{
    check(IXThreadingImpl_XTaskQueueTerminate(api,q,FALSE,NULL,NULL)==S_OK);
    while(dispatch(q,XTaskQueuePort_Work)) {}
    while(dispatch(q,XTaskQueuePort_Completion)) {}
    IXThreadingImpl_XTaskQueueCloseHandle(api,q);
}
static void contracts(void)
{
    XTaskQueueHandle q;
    XAsyncBlock a={0};
    UINT32 value=0;
    SIZE_T size=0;
    int identity;
    check(IXThreadingImpl_XTaskQueueCreate(api,XTaskQueueDispatchMode_Manual,XTaskQueueDispatchMode_Manual,&q)==S_OK);
    a.queue=q; a.callback=callback;
    callbacks=cleanups=0;
    check(IXThreadingImpl_XAsyncBegin(api,&a,NULL,&identity,"test",provider)==S_OK);
    check(IXThreadingImpl_XAsyncBegin(api,&a,NULL,&identity,"test",provider)==E_INVALIDARG);
    check(status(&a,FALSE)==E_PENDING);
    check(IXThreadingImpl_XAsyncGetResultSize(api,&a,&size)==E_PENDING && !size);
    check(dispatch(q,XTaskQueuePort_Work));
    check(status(&a,FALSE)==S_OK && callbacks==0 && cleanups==0);
    check(IXThreadingImpl_XAsyncGetResultSize(api,&a,&size)==S_OK && size==4);
    check(IXThreadingImpl_XAsyncGetResult(api,&a,NULL,4,&value,NULL)==E_INVALIDARG);
    check(IXThreadingImpl_XAsyncGetResult(api,&a,&identity,1,&value,NULL)==HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    check(dispatch(q,XTaskQueuePort_Completion));
    check(callbacks==1 && callback_thread==GetCurrentThreadId());
    check(IXThreadingImpl_XAsyncGetResult(api,&a,&identity,4,&value,&size)==S_OK && value==42 && size==4);
    check(cleanups==1);
    check(IXThreadingImpl_XAsyncGetResult(api,&a,&identity,4,&value,NULL)==E_INVALIDARG);
    check(IXThreadingImpl_XAsyncBegin(api,&a,NULL,&identity,"cancel",provider)==S_OK);
    IXThreadingImpl_XAsyncCancel(api,&a);
    check(status(&a,FALSE)==E_ABORT);
    check(dispatch(q,XTaskQueuePort_Work));
    check(dispatch(q,XTaskQueuePort_Completion));
    check(callbacks==2 && cleanups==2);
    drain(q);
}
struct blocked_context { HANDLE started, gate, cleaned; };
static HRESULT WINAPI blocked_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    struct blocked_context *context=data->context;
    switch(op)
    {
    case XAsyncOp_Begin: return IXThreadingImpl_XAsyncSchedule(api,data->async,0);
    case XAsyncOp_DoWork:
        SetEvent(context->started);
        WaitForSingleObject(context->gate,10000);
        IXThreadingImpl_XAsyncComplete(api,data->async,S_OK,0);
        return E_PENDING;
    case XAsyncOp_Cancel: IXThreadingImpl_XAsyncComplete(api,data->async,E_ABORT,0); return S_OK;
    case XAsyncOp_Cleanup: SetEvent(context->cleaned); return S_OK;
    default: return E_NOTIMPL;
    }
}
static void CALLBACK free_canceled(XAsyncBlock *block)
{
    check(status(block,FALSE)==E_ABORT);
    free(block);
    ++callbacks;
}
static void cancel_active(void)
{
    struct blocked_context c;
    XAsyncBlock *a=calloc(1,sizeof(*a));
    XTaskQueueHandle q;
    callbacks=0;
    c.started=CreateEventW(NULL,TRUE,FALSE,NULL);
    c.gate=CreateEventW(NULL,TRUE,FALSE,NULL);
    c.cleaned=CreateEventW(NULL,TRUE,FALSE,NULL);
    check(IXThreadingImpl_XTaskQueueCreate(api,XTaskQueueDispatchMode_ThreadPool,XTaskQueueDispatchMode_Manual,&q)==S_OK);
    a->queue=q; a->callback=free_canceled;
    check(IXThreadingImpl_XAsyncBegin(api,a,&c,NULL,"blocked",blocked_provider)==S_OK);
    check(WaitForSingleObject(c.started,10000)==WAIT_OBJECT_0);
    IXThreadingImpl_XAsyncCancel(api,a);
    check(status(a,FALSE)==E_ABORT);
    check(dispatch(q,XTaskQueuePort_Completion));
    check(callbacks==1 && WaitForSingleObject(c.cleaned,0)==WAIT_TIMEOUT);
    /* The external block is gone; worker still owns provider context. */
    SetEvent(c.gate);
    check(WaitForSingleObject(c.cleaned,10000)==WAIT_OBJECT_0);
    drain(q);
    check(callbacks==1);
    CloseHandle(c.started); CloseHandle(c.gate); CloseHandle(c.cleaned);
}
static void missing(void)
{
    XTaskQueueHandle q;
    XAsyncBlock a={0};
    XUserHandle u=(void *)1;
    char old[32768];
    DWORD len=GetEnvironmentVariableA("XODUS_SOCKET_PATH",old,sizeof(old));
    SetEnvironmentVariableA("XODUS_SOCKET_PATH",NULL);
    callbacks=0;
    check(add(NULL)==E_INVALIDARG);
    check(IXThreadingImpl_XTaskQueueCreate(api,XTaskQueueDispatchMode_Manual,XTaskQueueDispatchMode_Manual,&q)==S_OK);
    a.queue=q; a.callback=callback;
    check(add(&a)==S_OK && status(&a,FALSE)==E_PENDING);
    check(result(&a,&u)==E_PENDING && !u);
    check(dispatch(q,XTaskQueuePort_Work));
    check(status(&a,FALSE)==E_GAMEUSER_NO_DEFAULT_USER && !callbacks);
    check(dispatch(q,XTaskQueuePort_Completion));
    check(callbacks==1 && result(&a,&u)==E_GAMEUSER_NO_DEFAULT_USER && !u);
    check(add(&a)==S_OK);
    IXThreadingImpl_XAsyncCancel(api,&a);
    check(status(&a,FALSE)==E_ABORT);
    check(dispatch(q,XTaskQueuePort_Work));
    check(dispatch(q,XTaskQueuePort_Completion));
    check(result(&a,&u)==E_ABORT && !u && callbacks==2);
    drain(q);
    SetEnvironmentVariableA("XODUS_SOCKET_PATH",len ? old : NULL);
}
/* Concurrent outstanding picker requests without an account must each complete on their queue,
 * including cancellation and repetition after the caller consumes failure. */
static void picker(void)
{
    XTaskQueueHandle q;
    XAsyncBlock a[8]={{0}};
    XUserHandle u;
    unsigned int i, round;
    char old[32768];
    DWORD length=GetEnvironmentVariableA("XODUS_SOCKET_PATH",old,sizeof(old));
    SetEnvironmentVariableA("XODUS_SOCKET_PATH",NULL);
    check(IXUserImpl6_XUserAddAsync(user,XUserAddOptions_None,NULL)==E_INVALIDARG);
    check(IXThreadingImpl_XTaskQueueCreate(api,XTaskQueueDispatchMode_ThreadPool,XTaskQueueDispatchMode_Manual,&q)==S_OK);
    for (round=0; round<2; ++round)
    {
        callbacks=0;
        for(i=0;i<8;++i)
        {
            a[i].queue=q; a[i].callback=callback;
            check(IXUserImpl6_XUserAddAsync(user,XUserAddOptions_None,&a[i])==S_OK);
        }
        for(i=0;i<8;++i) check(status(&a[i],TRUE)==E_GAMEUSER_NO_DEFAULT_USER);
        check(!callbacks);
        for(i=0;i<8;++i) check(dispatch(q,XTaskQueuePort_Completion));
        check(callbacks==8);
        for(i=0;i<8;++i)
        {
            u=(void *)1;
            check(result(&a[i],&u)==E_GAMEUSER_NO_DEFAULT_USER && !u);
        }
    }
    drain(q);
    check(IXThreadingImpl_XTaskQueueCreate(api,XTaskQueueDispatchMode_Manual,XTaskQueueDispatchMode_Manual,&q)==S_OK);
    memset(a,0,sizeof(a)); a[0].queue=q; a[0].callback=callback;
    callbacks=0;
    check(IXUserImpl6_XUserAddAsync(user,XUserAddOptions_None,&a[0])==S_OK);
    IXThreadingImpl_XAsyncCancel(api,&a[0]);
    check(status(&a[0],FALSE)==E_ABORT);
    check(dispatch(q,XTaskQueuePort_Work));
    check(dispatch(q,XTaskQueuePort_Completion));
    u=(void *)1;
    check(callbacks==1 && result(&a[0],&u)==E_ABORT && !u);
    drain(q);
    SetEnvironmentVariableA("XODUS_SOCKET_PATH",length ? old : NULL);
}
static void identity_flow(BOOL live, HRESULT expected)
{
    XTaskQueueHandle q;
    XAsyncBlock a={0}, b={0};
    XUserHandle u=NULL,d=NULL,found=NULL,second=NULL;
    XUserLocalId local,local2;
    XUserAgeGroup age;
    XUserState state;
    UINT64 xid=0;
    BOOLEAN guest=TRUE;
    HRESULT hr;
    callbacks=0;
    check(IXThreadingImpl_XTaskQueueCreate(api,XTaskQueueDispatchMode_ThreadPool,XTaskQueueDispatchMode_Manual,&q)==S_OK);
    a.queue=q; a.callback=callback;
    check(add(&a)==S_OK);
    hr=status(&a,TRUE);
    check(hr==expected);
    check(dispatch(q,XTaskQueuePort_Completion));
    check(callbacks==1);
    check(result(&a,&u)==expected);
    if (SUCCEEDED(hr) && u)
    {
        check(IXUserImpl6_XUserGetId(user,u,&xid)==S_OK && xid && (live || xid==123));
        check(IXUserImpl6_XUserGetLocalId(user,u,&local)==S_OK && local.value);
        check(IXUserImpl6_XUserGetState(user,u,&state)==S_OK && state==XUserState_SignedIn);
        check(IXUserImpl6_XUserGetIsGuest(user,u,&guest)==S_OK && !guest);
        check(IXUserImpl6_XUserGetAgeGroup(user,u,&age)==S_OK && (live || age==XUserAgeGroup_Unknown));
        check(IXUserImpl6_XUserDuplicateHandle(user,u,&d)==S_OK && d);
        check(IXUserImpl6_XUserCompare(user,u,d)==0);
        check(IXUserImpl6_XUserFindUserById(user,xid,&found)==S_OK && found);
        IXUserImpl6_XUserCloseHandle(user,found);
        b.queue=q;
        check(add(&b)==S_OK && status(&b,TRUE)==S_OK);
        check(result(&b,&second)==S_OK && second);
        check(IXUserImpl6_XUserGetLocalId(user,second,&local2)==S_OK && local2.value==local.value);
        IXUserImpl6_XUserCloseHandle(user,second);
        if (!live)
        {
            XAsyncBlock pending[8]={{0}};
            unsigned int i;
            LONG before=callbacks;
            for(i=0;i<8;++i)
            {
                pending[i].queue=q; pending[i].callback=callback;
                check(IXUserImpl6_XUserAddAsync(user,i%2 ? XUserAddOptions_None : XUserAddOptions_AddDefaultUserSilently,&pending[i])==S_OK);
            }
            for(i=0;i<8;++i) check(status(&pending[i],TRUE)==(i%2 ? picker_expected : S_OK));
            for(i=0;i<8;++i) check(dispatch(q,XTaskQueuePort_Completion));
            check(callbacks==before+8);
            for(i=0;i<8;++i)
            {
                second=NULL;
                check(result(&pending[i],&second)==(i%2 ? picker_expected : S_OK));
                if(i%2 && FAILED(picker_expected)) check(!second);
                else
                {
                    check(IXUserImpl6_XUserGetLocalId(user,second,&local2)==S_OK && local2.value==local.value);
                    IXUserImpl6_XUserCloseHandle(user,second);
                }
            }
        }
        IXUserImpl6_XUserCloseHandle(user,u);
        check(IXUserImpl6_XUserGetId(user,d,&xid)==S_OK);
        check(IXUserImpl6_XUserFindUserByLocalId(user,local,&found)==S_OK && found);
        IXUserImpl6_XUserCloseHandle(user,found);
        IXUserImpl6_XUserCloseHandle(user,d);
        check(IXUserImpl6_XUserGetId(user,d,&xid)==E_GAMERUNTIME_INVALID_HANDLE && !xid);
        check(IXUserImpl6_XUserFindUserByLocalId(user,local,&found)==E_GAMEUSER_USER_NOT_FOUND && !found);
        IXUserImpl6_XUserCloseHandle(user,d);
    }
    else
    {
        check(!u);
        b.queue=q; b.callback=callback;
        check(IXUserImpl6_XUserAddAsync(user,XUserAddOptions_None,&b)==S_OK);
        check(status(&b,TRUE)==expected);
        check(dispatch(q,XTaskQueuePort_Completion));
        second=(void *)1;
        check(callbacks==2 && result(&b,&second)==expected && !second);
    }
    drain(q);
}
static void cancel_ipc(void)
{
    XTaskQueueHandle q;
    XAsyncBlock a={0};
    XUserHandle u=(void *)1;
    char ready[1024];
    unsigned int i;
    check(GetEnvironmentVariableA("XODUS_TEST_READY",ready,sizeof(ready))>0);
    check(IXThreadingImpl_XTaskQueueCreate(api,XTaskQueueDispatchMode_ThreadPool,XTaskQueueDispatchMode_Manual,&q)==S_OK);
    a.queue=q; a.callback=callback; callbacks=0;
    check(IXUserImpl6_XUserAddAsync(user,XUserAddOptions_None,&a)==S_OK);
    for(i=0;i<500 && GetFileAttributesA(ready)==INVALID_FILE_ATTRIBUTES;++i) Sleep(10);
    check(i<500 && status(&a,FALSE)==E_PENDING);
    IXThreadingImpl_XAsyncCancel(api,&a);
    check(status(&a,FALSE)==E_ABORT);
    check(dispatch(q,XTaskQueuePort_Completion));
    check(callbacks==1 && result(&a,&u)==E_ABORT && !u);
    drain(q);
}
/* Single native picker probe: success is validated against a fresh silent add.
 * This mode must only be run with a desktop user present. */
static void interactive_flow(HRESULT expected)
{
    XTaskQueueHandle q;
    XAsyncBlock a={0};
    XUserHandle initial=NULL, selected=NULL;
    UINT64 before=0, after=0;
    XUserLocalId first, second;
    check(IXThreadingImpl_XTaskQueueCreate(api,XTaskQueueDispatchMode_ThreadPool,XTaskQueueDispatchMode_Manual,&q)==S_OK);
    a.queue=q; a.callback=callback;
    callbacks=0;
    check(add(&a)==S_OK && status(&a,TRUE)==S_OK);
    check(dispatch(q,XTaskQueuePort_Completion));
    check(result(&a,&initial)==S_OK && initial);
    check(IXUserImpl6_XUserGetId(user,initial,&before)==S_OK && before);
    check(IXUserImpl6_XUserAddAsync(user,XUserAddOptions_None,&a)==S_OK);
    check(status(&a,TRUE)==expected);
    check(dispatch(q,XTaskQueuePort_Completion));
    check(callbacks==2 && result(&a,&selected)==expected);
    if(SUCCEEDED(expected))
    {
        check(IXUserImpl6_XUserGetId(user,selected,&after)==S_OK && after==before);
        check(IXUserImpl6_XUserGetLocalId(user,initial,&first)==S_OK);
        check(IXUserImpl6_XUserGetLocalId(user,selected,&second)==S_OK && first.value==second.value);
        IXUserImpl6_XUserCloseHandle(user,selected);
    }
    else check(!selected);
    IXUserImpl6_XUserCloseHandle(user,initial);
    drain(q);
}
int main(int argc, char **argv)
{
    HMODULE module;
    HRESULT (WINAPI *init)(ULONG,ULONG);
    HRESULT (WINAPI *query)(REFCLSID,REFIID,void **);
    void (WINAPI *uninit)(void);
    HRESULT expected=S_OK;
    if(argc<2 || !(module=LoadLibraryA(argv[1]))) return 2;
    init=(void *)GetProcAddress(module,"InitializeApiImpl");
    query=(void *)GetProcAddress(module,"QueryApiImpl");
    uninit=(void *)GetProcAddress(module,"UninitializeApiImpl");
    if(!init || !query || !uninit || FAILED(init(0,0))) return 2;
    if(FAILED(query(&CLSID_XThreadingImpl,&IID_IXThreadingImpl,(void **)&api)) ||
       FAILED(query(&CLSID_XUserImpl,&IID_IXUserImpl6,(void **)&user))) return 2;
    contracts(); defaults_and_heap(); cancel_active(); missing(); picker();
    {
        UINT64 invalid=99;
        XUserHandle invalid_handle=(void *)1;
        check(IXUserImpl6_XUserGetId(user,invalid_handle,&invalid)==E_GAMERUNTIME_INVALID_HANDLE && !invalid);
        check(IXUserImpl6_XUserDuplicateHandle(user,invalid_handle,&invalid_handle)==E_GAMERUNTIME_INVALID_HANDLE && !invalid_handle);
    }
    if(argc>2)
    {
        if(argc>3) expected=(HRESULT)strtoul(argv[3],NULL,16);
        if(argc>4) picker_expected=(HRESULT)strtoul(argv[4],NULL,16);
        if(!strcmp(argv[2],"cancel-ipc")) cancel_ipc();
        else if(!strcmp(argv[2],"interactive")) interactive_flow(expected);
        else identity_flow(!strcmp(argv[2],"live"),expected);
    }
    IXUserImpl6_Release(user); IXThreadingImpl_Release(api); uninit();
    printf("%ld checks, %ld failures\n",checks,failures);
    return failures ? 1 : 0;
}

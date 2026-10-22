// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* Connectivity contract and deterministic provider tests. LGPL-2.1-or-later. */
#define COBJMACROS
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <initguid.h>
#include "private.h"
#include "networking_state.h"

static LONG checks, failures, calls;
static IXNetworkingImpl2 *net;
static IXThreadingImpl *threading;
static HRESULT (WINAPI *initialize)(ULONG, ULONG);
static void (WINAPI *uninitialize)(void);
static HANDLE entered, leave_callback, finished;
static BOOL block_callback, self_unregister;
static LONG last_level;
static XTaskQueueRegistrationToken current;
#define check(e) do { InterlockedIncrement(&checks); if (!(e)) { InterlockedIncrement(&failures); printf("FAIL %u: %s\n", __LINE__, #e); } } while (0)
#define get(h) IXNetworkingImpl2_XNetworkingGetConnectivityHint(net,h)
#define reg(q,c,t) IXNetworkingImpl2_XNetworkingRegisterConnectivityHintChanged(net,q,NULL,c,t)
#define unreg(t,w) IXNetworkingImpl2_XNetworkingUnregisterConnectivityHintChanged(net,t,w)
#define create(q,m) IXThreadingImpl_XTaskQueueCreate(threading,m,m,q)
#define dispatch(q) IXThreadingImpl_XTaskQueueDispatch(threading,q,XTaskQueuePort_Completion,3000)
#define closeq(q) IXThreadingImpl_XTaskQueueCloseHandle(threading,q)

static void CALLBACK changed(void *context, const XNetworkingConnectivityHint *hint)
{
    InterlockedIncrement(&calls);
    check(hint != NULL);
    InterlockedExchange(&last_level,hint->connectivityLevel);
    SetEvent(entered);
    if (self_unregister) check(!unreg(current,FALSE));
    if (block_callback) check(WaitForSingleObject(leave_callback,5000)==WAIT_OBJECT_0);
    SetEvent(finished);
}
static void reducer(void)
{
    IP_ADAPTER_ADDRESSES a={0}, b={0};
    IP_ADAPTER_UNICAST_ADDRESS address={0};
    IP_ADAPTER_GATEWAY_ADDRESS_LH gateway={0};
    MIB_IPFORWARD_TABLE2 routes={0};
    XNetworkingConnectivityHint h;
    check(sizeof(h)==16);
    check(offsetof(XNetworkingConnectivityHint,networkInitialized)==12);
    check(offsetof(XNetworkingConnectivityHint,roaming)==15);
    networking_reduce(NULL,NULL,&h);
    check(h.networkInitialized && h.connectivityLevel==XNetworkingConnectivityLevelHint_None);
    a.OperStatus=IfOperStatusUp; a.IfType=IF_TYPE_SOFTWARE_LOOPBACK; a.FirstUnicastAddress=&address;
    networking_reduce(&a,NULL,&h);
    check(h.connectivityLevel==XNetworkingConnectivityLevelHint_None);
    a.IfType=IF_TYPE_ETHERNET_CSMACD;
    networking_reduce(&a,NULL,&h);
    check(h.connectivityLevel==XNetworkingConnectivityLevelHint_LocalAccess && h.ianaInterfaceType==IF_TYPE_ETHERNET_CSMACD);
    check(h.connectivityCost==XNetworkingConnectivityCostHint_Unknown && !h.roaming && !h.overDataLimit && !h.approachingDataLimit);
    b=a; a.Next=&b; b.IfType=IF_TYPE_IEEE80211; b.FirstGatewayAddress=&gateway;
    networking_reduce(&a,NULL,&h);
    check(h.ianaInterfaceType==IF_TYPE_IEEE80211);
    a.IfIndex=1; b.IfIndex=2;
    routes.NumEntries=1;
    routes.Table[0].DestinationPrefix.Prefix.si_family=AF_INET;
    routes.Table[0].InterfaceIndex=1;
    routes.Table[0].ValidLifetime=100;
    networking_reduce(&a,&routes,&h);
    check(h.ianaInterfaceType==IF_TYPE_ETHERNET_CSMACD);
    b.OperStatus=IfOperStatusDown;
    networking_reduce(&a,NULL,&h);
    check(h.ianaInterfaceType==IF_TYPE_ETHERNET_CSMACD);
    a.FirstUnicastAddress=NULL;
    networking_reduce(&a,NULL,&h);
    check(h.connectivityLevel==XNetworkingConnectivityLevelHint_None);
}
static DWORD WINAPI unregister_thread(void *context)
{
    check(unreg(current,TRUE));
    SetEvent(context);
    return 0;
}
static DWORD WINAPI concurrent_registrations(void *unused)
{
    unsigned int i;
    XTaskQueueHandle q;
    XTaskQueueRegistrationToken t;
    for(i=0;i<50;++i)
    {
        check(create(&q,XTaskQueueDispatchMode_Manual)==S_OK);
        check(reg(q,changed,&t)==S_OK);
        check(unreg(t,TRUE));
        while(IXThreadingImpl_XTaskQueueDispatch(threading,q,XTaskQueuePort_Completion,0)) {}
        closeq(q);
    }
    return 0;
}
static void lifecycle(void)
{
    XTaskQueueHandle q, duplicate;
    XTaskQueueRegistrationToken t={0}, old;
    XNetworkingConnectivityHint h;
    HANDLE workers[4];
    HANDLE worker, done=CreateEventW(NULL,TRUE,FALSE,NULL);
    LONG before;
    unsigned int i;
    check(get(NULL)==E_INVALIDARG);
    check(reg(NULL,NULL,&t)==E_INVALIDARG && !t.token);
    check(reg(NULL,changed,NULL)==E_INVALIDARG);
    check(reg((XTaskQueueHandle)(ULONG_PTR)0x1234,changed,&t)==E_INVALIDARG && !t.token);
    check(unreg(t,TRUE));
    check(get(&h)==S_OK && h.networkInitialized);
    printf("Observed hint: level %u interface %u initialized %u\n",h.connectivityLevel,h.ianaInterfaceType,h.networkInitialized);
    check(create(&q,XTaskQueueDispatchMode_Manual)==S_OK);
    check(reg(q,changed,&t)==S_OK && t.token);
    check(!IXThreadingImpl_XTaskQueueDispatch(threading,q,XTaskQueuePort_Work,0));
    check(dispatch(q));
    check(calls==1);
    check(!IXThreadingImpl_XTaskQueueDispatch(threading,q,XTaskQueuePort_Completion,1100));
    check(unreg(t,TRUE));
    check(unreg(t,FALSE));
    old=t;
    /* Queued Manual notifications are suppressed, without waiting for dispatch. */
    before=calls;
    check(reg(q,changed,&t)==S_OK && t.token!=old.token);
    Sleep(100);
    check(unreg(t,TRUE));
    while(IXThreadingImpl_XTaskQueueDispatch(threading,q,XTaskQueuePort_Completion,0)) {}
    check(calls==before);
    /* Registration duplicates ownership of caller queue. */
    check(IXThreadingImpl_XTaskQueueDuplicateHandle(threading,q,&duplicate)==S_OK);
    check(reg(q,changed,&t)==S_OK);
    closeq(q);
    check(dispatch(duplicate));
    check(calls==before+1);
    check(unreg(t,TRUE));
    closeq(duplicate);
    /* A callback can request nonwaiting removal of itself. */
    check(create(&q,XTaskQueueDispatchMode_Manual)==S_OK);
    self_unregister=TRUE;
    check(reg(q,changed,&current)==S_OK);
    check(dispatch(q));
    self_unregister=FALSE;
    check(unreg(current,TRUE));
    closeq(q);
    /* Null queues have an owned ThreadPool fallback. */
    ResetEvent(entered);
    check(reg(NULL,changed,&t)==S_OK);
    check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    check(unreg(t,TRUE));
    /* wait=true blocks until an entered callback exits. */
    ResetEvent(entered); ResetEvent(leave_callback);
    block_callback=TRUE;
    check(reg(NULL,changed,&current)==S_OK);
    check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    worker=CreateThread(NULL,0,unregister_thread,done,0,NULL);
    check(WaitForSingleObject(done,100)==WAIT_TIMEOUT);
    SetEvent(leave_callback);
    check(WaitForSingleObject(done,5000)==WAIT_OBJECT_0);
    WaitForSingleObject(worker,5000); CloseHandle(worker);
    block_callback=FALSE;
    /* Nonwaiting unregister and final runtime release do not wait for users. */
    ResetEvent(entered); ResetEvent(leave_callback); ResetEvent(finished);
    block_callback=TRUE;
    check(reg(NULL,changed,&t)==S_OK);
    check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    check(!unreg(t,FALSE));
    check(!unreg(t,FALSE));
    check(WaitForSingleObject(finished,0)==WAIT_TIMEOUT);
    SetEvent(leave_callback);
    check(WaitForSingleObject(finished,5000)==WAIT_OBJECT_0);
    ResetEvent(entered); ResetEvent(leave_callback); ResetEvent(finished);
    check(reg(NULL,changed,&t)==S_OK);
    check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    uninitialize();
    check(WaitForSingleObject(finished,0)==WAIT_TIMEOUT);
    check(!unreg(t,FALSE));
    SetEvent(leave_callback);
    check(WaitForSingleObject(finished,5000)==WAIT_OBJECT_0);
    block_callback=FALSE;
    check(initialize(10001,3193)==S_OK);
    /* Terminated queues never execute user callbacks. */
    before=calls;
    check(create(&q,XTaskQueueDispatchMode_Manual)==S_OK);
    check(IXThreadingImpl_XTaskQueueTerminate(threading,q,FALSE,NULL,NULL)==S_OK);
    check(reg(q,changed,&t)==S_OK);
    Sleep(100);
    check(unreg(t,TRUE));
    closeq(q);
    check(calls==before);
    for(i=0;i<100;++i)
    {
        check(create(&q,XTaskQueueDispatchMode_Manual)==S_OK);
        check(reg(q,changed,&t)==S_OK);
        check(unreg(t,FALSE));
        while(IXThreadingImpl_XTaskQueueDispatch(threading,q,XTaskQueuePort_Completion,0)) {}
        closeq(q);
    }
    before=calls;
    for(i=0;i<4;++i) workers[i]=CreateThread(NULL,0,concurrent_registrations,NULL,0,NULL);
    check(WaitForMultipleObjects(4,workers,TRUE,20000)==WAIT_OBJECT_0);
    for(i=0;i<4;++i) CloseHandle(workers[i]);
    check(calls==before);
    /* Final uninitialize suppresses pending callbacks and rejects retained iface. */
    check(create(&q,XTaskQueueDispatchMode_Manual)==S_OK);
    check(reg(q,changed,&t)==S_OK);
    Sleep(100);
    before=calls;
    uninitialize();
    check(get(&h)==E_GAMERUNTIME_NOT_INITIALIZED && !h.networkInitialized);
    check(reg(q,changed,&old)==E_GAMERUNTIME_NOT_INITIALIZED && !old.token);
    check(unreg(t,FALSE));
    while(IXThreadingImpl_XTaskQueueDispatch(threading,q,XTaskQueuePort_Completion,0)) {}
    check(calls==before);
    closeq(q);
    check(initialize(10001,3193)==S_OK);
    check(get(&h)==S_OK && h.networkInitialized);
    CloseHandle(done);
}
static volatile LONG pump_stop;
static DWORD WINAPI pump_thread(void *context)
{
    while(!InterlockedCompareExchange(&pump_stop,0,0))
        IXThreadingImpl_XTaskQueueDispatch(threading,context,XTaskQueuePort_Completion,50);
    return 0;
}
/* Title pattern seen in the 000-2001 capture: create a queue, query once,
 * register immediately and receive the initial non-canceled notification on
 * a caller-created ThreadPool queue or a Manual queue pumped by another thread. */
static void title_patterns(void)
{
    XTaskQueueHandle q;
    XTaskQueueRegistrationToken t;
    XNetworkingConnectivityHint h;
    HANDLE pump;
    LONG before;
    before=calls;
    ResetEvent(entered);
    check(IXThreadingImpl_XTaskQueueCreate(threading,XTaskQueueDispatchMode_ThreadPool,XTaskQueueDispatchMode_ThreadPool,&q)==S_OK);
    check(get(&h)==S_OK);
    check(reg(q,changed,&t)==S_OK);
    check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    check(calls==before+1);
    check(last_level==(LONG)h.connectivityLevel);
    check(unreg(t,TRUE));
    closeq(q);
    before=calls;
    ResetEvent(entered);
    check(create(&q,XTaskQueueDispatchMode_Manual)==S_OK);
    pump_stop=0;
    pump=CreateThread(NULL,0,pump_thread,q,0,NULL);
    check(get(&h)==S_OK);
    check(reg(q,changed,&t)==S_OK);
    check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    check(calls==before+1);
    check(unreg(t,TRUE));
    InterlockedExchange(&pump_stop,1);
    check(WaitForSingleObject(pump,5000)==WAIT_OBJECT_0);
    CloseHandle(pump);
    closeq(q);
}
/* The namespace fixture changes only its private dummy interface in response
 * to these markers; the host routes and game prefix are never touched. */
static void transitions(void)
{
    XTaskQueueRegistrationToken t;
    DWORD start;
    check(reg(NULL,changed,&t)==S_OK);
    check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    check(last_level==XNetworkingConnectivityLevelHint_LocalAccess);
    ResetEvent(entered);
    puts("NETWORK_READY_DOWN"); fflush(stdout);
    start=GetTickCount();
    while(InterlockedCompareExchange(&last_level,0,0)!=XNetworkingConnectivityLevelHint_None && GetTickCount()-start<8000) Sleep(20);
    check(last_level==XNetworkingConnectivityLevelHint_None);
    puts("NETWORK_READY_UP"); fflush(stdout);
    start=GetTickCount();
    while(InterlockedCompareExchange(&last_level,0,0)!=XNetworkingConnectivityLevelHint_LocalAccess && GetTickCount()-start<8000) Sleep(20);
    check(last_level==XNetworkingConnectivityLevelHint_LocalAccess);
    check(unreg(t,TRUE));
    calls=0;
}
/* Preferred local UDP multiplayer port: the documented default 3074,
 * synchronously and asynchronously; its change registration never fires. */
static HANDLE port_done;
static void CALLBACK port_completed(XAsyncBlock *block)
{
    UINT16 port=0;
    check(IXNetworkingImpl2_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult(net,block,&port)==S_OK);
    check(port==3074);
    SetEvent(port_done);
}
static void CALLBACK port_changed(void *context, UINT64 port)
{
    InterlockedIncrement(&calls);
}
static void udp_port(void)
{
    XAsyncBlock block;
    XTaskQueueHandle q;
    XTaskQueueRegistrationToken t, t2;
    UINT16 port=0;
    LONG before;
    check(IXNetworkingImpl2_XNetworkingQueryPreferredLocalUdpMultiplayerPort(net,NULL)==E_POINTER);
    check(IXNetworkingImpl2_XNetworkingQueryPreferredLocalUdpMultiplayerPort(net,&port)==S_OK && port==3074);
    check(IXNetworkingImpl2_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(net,NULL)==E_INVALIDARG);
    /* Default (null) queue, waited for with XAsyncGetStatus. */
    memset(&block,0,sizeof(block));
    check(IXNetworkingImpl2_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(net,&block)==S_OK);
    check(IXThreadingImpl_XAsyncGetStatus(threading,&block,TRUE)==S_OK);
    check(IXNetworkingImpl2_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult(net,&block,NULL)==E_POINTER);
    port=0;
    check(IXNetworkingImpl2_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult(net,&block,&port)==S_OK && port==3074);
    port=0;
    check(FAILED(IXNetworkingImpl2_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult(net,&block,&port)) && !port);
    /* Callback on a Manual queue dispatched by the caller. */
    port_done=CreateEventW(NULL,TRUE,FALSE,NULL);
    check(create(&q,XTaskQueueDispatchMode_Manual)==S_OK);
    memset(&block,0,sizeof(block));
    block.queue=q;
    block.callback=port_completed;
    check(IXNetworkingImpl2_XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(net,&block)==S_OK);
    while(WaitForSingleObject(port_done,0)!=WAIT_OBJECT_0 &&
          (IXThreadingImpl_XTaskQueueDispatch(threading,q,XTaskQueuePort_Work,100) ||
           IXThreadingImpl_XTaskQueueDispatch(threading,q,XTaskQueuePort_Completion,100))) {}
    check(WaitForSingleObject(port_done,5000)==WAIT_OBJECT_0);
    /* Registrations validate their arguments and never call back. */
    before=calls;
    check(IXNetworkingImpl2_XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(net,q,NULL,NULL,&t)==E_INVALIDARG);
    check(IXNetworkingImpl2_XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(net,q,NULL,port_changed,NULL)==E_INVALIDARG);
    check(IXNetworkingImpl2_XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(net,q,NULL,port_changed,&t)==S_OK && t.token);
    check(IXNetworkingImpl2_XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(net,NULL,NULL,port_changed,&t2)==S_OK && t2.token!=t.token);
    check(!IXThreadingImpl_XTaskQueueDispatch(threading,q,XTaskQueuePort_Completion,200));
    check(calls==before);
    check(IXNetworkingImpl2_XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(net,t,TRUE));
    check(IXNetworkingImpl2_XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(net,t2,FALSE));
    check(IXNetworkingImpl2_XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(net,t,TRUE));
    closeq(q);
    CloseHandle(port_done);
}
int main(int argc, char **argv)
{
    HMODULE dll;
    HRESULT (WINAPI *query)(REFCLSID,REFIID,void **);
    if(argc!=2 && argc!=3) return 2;
    reducer();
    dll=LoadLibraryA(argv[1]);
    if(!dll) { printf("LoadLibrary error %lu\n",GetLastError()); return 2; }
    initialize=(void *)GetProcAddress(dll,"InitializeApiImpl");
    uninitialize=(void *)GetProcAddress(dll,"UninitializeApiImpl");
    query=(void *)GetProcAddress(dll,"QueryApiImpl");
    if(!initialize || !uninitialize || !query) return 2;
    check(initialize(10001,3193)==S_OK);
    check(query(&CLSID_XNetworkingImpl,&IID_IXNetworkingImpl2,(void **)&net)==S_OK);
    check(query(&CLSID_XThreadingImpl,&IID_IXThreadingImpl,(void **)&threading)==S_OK);
    entered=CreateEventW(NULL,TRUE,FALSE,NULL);
    leave_callback=CreateEventW(NULL,TRUE,FALSE,NULL);
    finished=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(argc==3) transitions();
    lifecycle();
    title_patterns();
    udp_port();
    uninitialize();
    CloseHandle(entered); CloseHandle(leave_callback); CloseHandle(finished);
    printf("%ld checks, %ld failures\n",checks,failures);
    return failures ? 1 : 0;
}

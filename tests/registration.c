// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* Eventless registration contract tests (user change, invite, protocol).
 * Copyright 2026 Xodus contributors. LGPL-2.1-or-later. */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <initguid.h>
#include "xasyncprovider.h"
#include "xuser.h"
#include "xgameinvite.h"
#include "xgameprotocol.h"
#include "xgameerr.h"

static LONG checks, failures, calls;
static IXThreadingImpl *threading;
static IXUserImpl6 *user;
static IXGameInviteImpl2 *invite;
static IXGameProtocolImpl *protocol;
static HRESULT (WINAPI *initialize)(ULONG, ULONG);
static void (WINAPI *uninitialize)(void);
#define check(e) do { InterlockedIncrement(&checks); if (!(e)) { InterlockedIncrement(&failures); printf("FAIL %u: %s\n", __LINE__, #e); } } while (0)

static void CALLBACK user_changed(void *context, XUserLocalId id, XUserChangeEvent event) { InterlockedIncrement(&calls); }
static void CALLBACK invited(void *context, const char *uri) { InterlockedIncrement(&calls); }
static void CALLBACK activated(void *context, const char *uri) { InterlockedIncrement(&calls); }

#define ureg(q,t) IXUserImpl6_XUserRegisterForChangeEvent(user,q,NULL,user_changed,t)
#define uunreg(t,w) IXUserImpl6_XUserUnregisterForChangeEvent(user,t,w)
#define ireg(q,t) IXGameInviteImpl2_XGameInviteRegisterForEvent(invite,q,NULL,invited,t)
#define iunreg(t,w) IXGameInviteImpl2_XGameInviteUnregisterForEvent(invite,t,w)
#define preg(q,t) IXGameProtocolImpl_XGameProtocolRegisterForActivation(protocol,q,NULL,activated,t)
#define punreg(t,w) IXGameProtocolImpl_XGameProtocolUnregisterForActivation(protocol,t,w)
#define create(q,m) IXThreadingImpl_XTaskQueueCreate(threading,m,m,q)
#define closeq(q) IXThreadingImpl_XTaskQueueCloseHandle(threading,q)

static void contract(void)
{
    XTaskQueueHandle q, duplicate;
    XTaskQueueRegistrationToken u={0}, i={0}, p={0}, again;
    unsigned int n;
    /* Argument validation leaves a zero token. */
    check(IXUserImpl6_XUserRegisterForChangeEvent(user,NULL,NULL,NULL,&u)==E_INVALIDARG && !u.token);
    check(ureg(NULL,NULL)==E_INVALIDARG);
    check(ureg((XTaskQueueHandle)(ULONG_PTR)0x1234,&u)==E_INVALIDARG && !u.token);
    check(ireg((XTaskQueueHandle)(ULONG_PTR)0x1234,&i)==E_INVALIDARG && !i.token);
    check(preg((XTaskQueueHandle)(ULONG_PTR)0x1234,&p)==E_INVALIDARG && !p.token);
    /* Title pattern: one Manual queue for all three registrations. */
    check(create(&q,XTaskQueueDispatchMode_Manual)==S_OK);
    check(ureg(q,&u)==S_OK && u.token);
    check(ireg(q,&i)==S_OK && i.token);
    check(preg(q,&p)==S_OK && p.token);
    check(u.token!=i.token && i.token!=p.token && u.token!=p.token);
    /* No event is ever raised, so nothing is queued. */
    check(!IXThreadingImpl_XTaskQueueDispatch(threading,q,XTaskQueuePort_Completion,200));
    check(calls==0);
    /* A token from one source cannot remove another source's registration. */
    check(iunreg(u,TRUE));
    check(uunreg(u,TRUE));
    check(uunreg(u,FALSE));
    check(iunreg(i,FALSE));
    check(punreg(p,TRUE));
    check(punreg(p,TRUE));
    /* Registration owns a queue duplicate; the caller's close is independent. */
    check(IXThreadingImpl_XTaskQueueDuplicateHandle(threading,q,&duplicate)==S_OK);
    check(ureg(q,&u)==S_OK);
    closeq(q);
    check(uunreg(u,TRUE));
    closeq(duplicate);
    /* Null queues are accepted. */
    check(ureg(NULL,&u)==S_OK && u.token);
    check(uunreg(u,TRUE));
    for(n=0;n<1000;++n)
    {
        check(create(&q,XTaskQueueDispatchMode_ThreadPool)==S_OK);
        check(ureg(q,&u)==S_OK && ireg(q,&i)==S_OK && preg(q,&p)==S_OK);
        closeq(q);
        check(uunreg(u,FALSE) && iunreg(i,FALSE) && punreg(p,FALSE));
    }
    /* Final uninitialize releases remaining registrations and rejects new ones. */
    check(create(&q,XTaskQueueDispatchMode_Manual)==S_OK);
    check(ureg(q,&u)==S_OK && ireg(q,&i)==S_OK);
    uninitialize();
    check(ureg(q,&again)==E_GAMERUNTIME_NOT_INITIALIZED && !again.token);
    check(preg(q,&again)==E_GAMERUNTIME_NOT_INITIALIZED && !again.token);
    check(uunreg(u,TRUE) && iunreg(i,TRUE));
    closeq(q);
    check(initialize(10001,3193)==S_OK);
    check(calls==0);
}

int main(int argc, char **argv)
{
    HMODULE dll;
    HRESULT (WINAPI *query)(REFCLSID,REFIID,void **);
    if(argc!=2) return 2;
    if(!(dll=LoadLibraryA(argv[1]))) { printf("LoadLibrary error %lu\n",GetLastError()); return 2; }
    initialize=(void *)GetProcAddress(dll,"InitializeApiImpl");
    uninitialize=(void *)GetProcAddress(dll,"UninitializeApiImpl");
    query=(void *)GetProcAddress(dll,"QueryApiImpl");
    if(!initialize || !uninitialize || !query) return 2;
    check(initialize(10001,3193)==S_OK);
    if(FAILED(query(&CLSID_XThreadingImpl,&IID_IXThreadingImpl,(void **)&threading)) ||
       FAILED(query(&CLSID_XUserImpl,&IID_IXUserImpl6,(void **)&user)) ||
       FAILED(query(&CLSID_XGameInviteImpl,&IID_IXGameInviteImpl2,(void **)&invite)) ||
       FAILED(query(&CLSID_XGameProtocolImpl,&IID_IXGameProtocolImpl,(void **)&protocol))) return 2;
    contract();
    uninitialize();
    printf("%ld checks, %ld failures\n",checks,failures);
    return failures ? 1 : 0;
}

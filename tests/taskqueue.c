// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* XTaskQueue synthetic tests. LGPL-2.1-or-later. */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <initguid.h>
#include "xasyncprovider.h"

static IXThreadingImpl *api;
static HRESULT (WINAPI *initialize)(ULONG, ULONG);
static void (WINAPI *uninitialize)(void);
static HRESULT (WINAPI *query_api)(REFCLSID, REFIID, void **);
static LONG checks, failures;
#define check(expr) do { InterlockedIncrement(&checks); if (!(expr)) { InterlockedIncrement(&failures); printf("FAIL line %u: %s\n", __LINE__, #expr); } } while (0)
#define create(w,c,q) IXThreadingImpl_XTaskQueueCreate(api,w,c,q)
#define submit(q,p,c,f) IXThreadingImpl_XTaskQueueSubmitCallback(api,q,p,c,f)
#define dispatch(q,p,t) IXThreadingImpl_XTaskQueueDispatch(api,q,p,t)
#define closeq(q) IXThreadingImpl_XTaskQueueCloseHandle(api,q)
#define terminate(q,w,c,f) IXThreadingImpl_XTaskQueueTerminate(api,q,w,c,f)
#define duplicate(q,d) IXThreadingImpl_XTaskQueueDuplicateHandle(api,q,d)
#define MANUAL XTaskQueueDispatchMode_Manual
#define POOL XTaskQueueDispatchMode_ThreadPool
#define SERIAL XTaskQueueDispatchMode_SerializedThreadPool
#define IMMEDIATE XTaskQueueDispatchMode_Immediate
#define WORK XTaskQueuePort_Work
#define COMPLETION XTaskQueuePort_Completion

struct counts { LONG calls, canceled; DWORD thread; HANDLE event; };
static void CALLBACK count(void *context, BOOLEAN canceled)
{
    struct counts *c = context;
    InterlockedExchange((LONG *)&c->thread, GetCurrentThreadId());
    if (canceled) InterlockedIncrement(&c->canceled);
    InterlockedIncrement(&c->calls);
    if (c->event) SetEvent(c->event);
}
static void CALLBACK finished(void *context) { SetEvent(context); }
static void drain(XTaskQueueHandle q)
{
    check(terminate(q, FALSE, NULL, NULL) == S_OK);
    while (dispatch(q, WORK, 1000)) {}
    while (dispatch(q, COMPLETION, 1000)) {}
    closeq(q);
}
static void invalid(void)
{
    XTaskQueueHandle q = (void *)1, d = (void *)1;
    struct counts c = {0};
    check(create(MANUAL, MANUAL, NULL) == E_POINTER);
    check(create((XTaskQueueDispatchMode)-1, MANUAL, &q) == E_INVALIDARG && !q);
    q = (void *)1;
    check(create(MANUAL, (XTaskQueueDispatchMode)(IMMEDIATE + 1), &q) == E_INVALIDARG && !q);
    check(duplicate(NULL, &d) == E_INVALIDARG && !d);
    check(duplicate((void *)0x1234, &d) == E_INVALIDARG && !d);
    check(!dispatch(NULL, WORK, 0));
    check(!dispatch((void *)0x1234, WORK, 0));
    closeq(NULL);
    closeq((void *)0x1234);
    check(submit(NULL, WORK, &c, count) == E_INVALIDARG);
    check(terminate(NULL, FALSE, NULL, NULL) == E_INVALIDARG);
    check(create(MANUAL, MANUAL, &q) == S_OK && q);
    check(submit(q, (XTaskQueuePort)-1, &c, count) == E_INVALIDARG);
    check(submit(q, WORK, NULL, NULL) == E_INVALIDARG);
    check(!dispatch(q, (XTaskQueuePort)2, 0));
    check(duplicate(q, NULL) == E_POINTER);
    {
        XTaskQueuePortHandle port = (void *)1;
        XTaskQueueHandle unsupported = (void *)1;
        check(IXThreadingImpl_XTaskQueueGetPort(api,q,WORK,NULL) == E_POINTER);
        check(IXThreadingImpl_XTaskQueueGetPort(api,(void *)0x1234,WORK,&port) == E_INVALIDARG && !port);
        port = (void *)1;
        check(IXThreadingImpl_XTaskQueueGetPort(api,q,(XTaskQueuePort)2,&port) == E_INVALIDARG && !port);
        check(IXThreadingImpl_XTaskQueueCreateComposite(api,NULL,NULL,NULL) == E_POINTER);
        check(IXThreadingImpl_XTaskQueueCreateComposite(api,NULL,NULL,&unsupported) == E_INVALIDARG && !unsupported);
        unsupported = (void *)1;
        check(IXThreadingImpl_XTaskQueueCreateComposite(api,(void *)0x1234,(void *)0x1234,&unsupported) == E_INVALIDARG && !unsupported);
        check(IXThreadingImpl_XTaskQueueSubmitDelayedCallback(api,q,WORK,1,NULL,NULL) == E_INVALIDARG);
        check(IXThreadingImpl_XTaskQueueSubmitDelayedCallback(api,q,WORK,0,&c,count) == S_OK);
        check(dispatch(q,WORK,0) && c.calls == 1);
        check(IXThreadingImpl_XTaskQueueSubmitDelayedCallback(api,q,WORK,1,&c,count) == S_OK);
        check(dispatch(q,WORK,1000) && c.calls == 2);
    }
    drain(q);
}
struct ordered { unsigned int value; unsigned int *next; };
static void CALLBACK ordered(void *context, BOOLEAN canceled)
{
    struct ordered *o = context;
    check(!canceled);
    check(o->value == (*o->next)++);
}
static void manual(void)
{
    XTaskQueueHandle q, d;
    struct counts c = {0};
    struct ordered items[100];
    unsigned int i, next = 0;
    ULONGLONG start;
    check(create(MANUAL, MANUAL, &q) == S_OK);
    check(duplicate(q, &d) == S_OK && d);
    closeq(q);
    q = d;
    check(duplicate(q, &q) == S_OK && q);
    closeq(d);
    check(submit(q, COMPLETION, &c, count) == S_OK);
    Sleep(30);
    check(!c.calls);
    check(!dispatch(q, WORK, 0));
    check(dispatch(q, COMPLETION, 0));
    check(c.calls == 1 && !c.canceled && c.thread == GetCurrentThreadId());
    for (i = 0; i < 100; ++i)
    {
        items[i].value = i;
        items[i].next = &next;
        check(submit(q, WORK, &items[i], ordered) == S_OK);
    }
    for (i = 0; i < 100; ++i) check(dispatch(q, WORK, 0));
    check(next == 100 && !dispatch(q, WORK, 0));
    start = GetTickCount64();
    check(!dispatch(q, COMPLETION, 60));
    check(GetTickCount64() - start >= 45 && GetTickCount64() - start < 2000);
    drain(q);
}
struct producer { XTaskQueueHandle q; struct counts *c; XTaskQueuePort port; };
static DWORD WINAPI produce(void *context)
{
    struct producer *p = context;
    unsigned int i;
    for (i = 0; i < 500; ++i) check(submit(p->q, p->port, p->c, count) == S_OK);
    return 0;
}
static void concurrent(void)
{
    XTaskQueueHandle q;
    struct counts c = {0};
    struct producer p;
    HANDLE threads[8];
    unsigned int i;
    ULONGLONG deadline;
    check(create(POOL, MANUAL, &q) == S_OK);
    p.q = q; p.c = &c; p.port = WORK;
    for (i = 0; i < 8; ++i) threads[i] = CreateThread(NULL, 0, produce, &p, 0, NULL);
    check(WaitForMultipleObjects(8, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (i = 0; i < 8; ++i) CloseHandle(threads[i]);
    deadline = GetTickCount64() + 10000;
    while (InterlockedCompareExchange(&c.calls,0,0) < 4000 && GetTickCount64() < deadline) Sleep(1);
    check(c.calls == 4000 && !c.canceled);
    c.calls = 0;
    p.port = COMPLETION;
    for (i = 0; i < 8; ++i) threads[i] = CreateThread(NULL, 0, produce, &p, 0, NULL);
    check(WaitForMultipleObjects(8, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (i = 0; i < 8; ++i) CloseHandle(threads[i]);
    check(!c.calls);
    for (i = 0; i < 4000; ++i) check(dispatch(q, COMPLETION, 0));
    check(c.calls == 4000 && !c.canceled);
    drain(q);
}
struct blocking { XTaskQueueHandle q; HANDLE entered, release, completed; LONG canceled; };
static void CALLBACK blocked(void *context, BOOLEAN canceled)
{
    struct blocking *b = context;
    SetEvent(b->entered);
    check(WaitForSingleObject(b->release,10000) == WAIT_OBJECT_0);
    check(!canceled);
    SetEvent(b->completed);
}
static DWORD WINAPI wait_dispatch(void *context)
{
    check(dispatch(context, COMPLETION, INFINITE));
    return 0;
}
struct post { XTaskQueueHandle q; struct counts *c; };
static void CALLBACK post_completion(void *context, BOOLEAN canceled)
{
    struct post *p = context;
    check(submit(p->q, COMPLETION, p->c, count) == S_OK);
    (void)canceled;
}
static void shutdown_tests(void)
{
    XTaskQueueHandle q, stale;
    struct counts c = {0}, pending = {0};
    struct blocking b;
    struct post p;
    HANDLE done, thread;
    unsigned int i;
    done = CreateEventW(NULL, TRUE, FALSE, NULL);
    check(create(POOL, MANUAL, &q) == S_OK);
    c.event = CreateEventW(NULL, TRUE, FALSE, NULL);
    thread = CreateThread(NULL,0,wait_dispatch,q,0,NULL);
    Sleep(30);
    check(WaitForSingleObject(thread,0) == WAIT_TIMEOUT);
    check(submit(q, COMPLETION, &c, count) == S_OK);
    check(WaitForSingleObject(thread,3000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    check(c.calls == 1);
    check(submit(q, COMPLETION, &pending, count) == S_OK);
    check(terminate(q, FALSE, done, finished) == S_OK);
    check(submit(q, WORK, &c, count) == E_ABORT);
    check(WaitForSingleObject(done,30) == WAIT_TIMEOUT);
    while (dispatch(q, COMPLETION, 3000)) {}
    check(WaitForSingleObject(done,0) == WAIT_OBJECT_0);
    check(pending.calls == 1 && pending.canceled == 1);
    check(!dispatch(q, WORK, INFINITE));
    check(!dispatch(q, COMPLETION, INFINITE));
    check(submit(q, COMPLETION, &c, count) == E_ABORT);
    stale = q; closeq(q);
    check(duplicate(stale, &q) == E_INVALIDARG && !q);
    CloseHandle(c.event);

    /* Last handle closure does not cancel a queued pool callback. */
    check(create(POOL, POOL, &q) == S_OK);
    ResetEvent(done);
    b.q=q; b.entered=CreateEventW(NULL,TRUE,FALSE,NULL);
    b.release=CreateEventW(NULL,TRUE,FALSE,NULL); b.completed=done;
    check(submit(q, WORK, &b, blocked) == S_OK);
    check(WaitForSingleObject(b.entered,3000) == WAIT_OBJECT_0);
    closeq(q);
    SetEvent(b.release);
    check(WaitForSingleObject(done,3000) == WAIT_OBJECT_0);
    CloseHandle(b.entered); CloseHandle(b.release);

    /* Work cancellation can post completion before completion shutdown. */
    check(create(MANUAL, MANUAL, &q) == S_OK);
    c.calls=0; c.canceled=0; c.event=NULL;
    p.q=q; p.c=&c;
    check(submit(q, WORK, &p, post_completion) == S_OK);
    check(terminate(q, FALSE, NULL, NULL) == S_OK);
    while (dispatch(q, WORK, 0)) {}
    while (dispatch(q, COMPLETION, 0)) {}
    check(c.calls == 1 && c.canceled == 1);
    closeq(q);

    /* Repeated concurrent close/submit/terminate lifetime exercise. */
    for (i=0; i<100; ++i)
    {
        check(create(POOL, POOL, &q) == S_OK);
        ResetEvent(done);
        c.calls=0; c.canceled=0;
        check(submit(q, WORK, &c, count) == S_OK);
        check(terminate(q, TRUE, done, finished) == S_OK);
        check(c.calls == 1 && WaitForSingleObject(done,0) == WAIT_OBJECT_0);
        closeq(q);
    }
    CloseHandle(done);
}

struct race { XTaskQueueHandle q; struct counts *c; HANDLE start; LONG submitted; };
static DWORD WINAPI race_submit(void *context)
{
    struct race *r = context;
    XTaskQueueHandle d;
    HRESULT hr;
    unsigned int i;
    check(duplicate(r->q, &d) == S_OK);
    check(WaitForSingleObject(r->start, 3000) == WAIT_OBJECT_0);
    for (i=0; i<200; ++i)
    {
        hr=submit(d, WORK, r->c, count);
        check(hr == S_OK || hr == E_ABORT);
        if (hr == S_OK) InterlockedIncrement(&r->submitted);
        check(duplicate(d, &r->q) == S_OK);
        closeq(r->q);
    }
    closeq(d);
    return 0;
}
static void lifetime_races(void)
{
    XTaskQueueHandle q;
    struct counts c={0};
    struct race r[4];
    HANDLE threads[4], start=CreateEventW(NULL,TRUE,FALSE,NULL);
    unsigned int i, total=0;
    check(create(POOL, POOL, &q) == S_OK);
    for (i=0; i<4; ++i)
    {
        r[i].q=q; r[i].c=&c; r[i].start=start; r[i].submitted=0;
        threads[i]=CreateThread(NULL,0,race_submit,&r[i],0,NULL);
    }
    SetEvent(start);
    check(terminate(q,TRUE,NULL,NULL) == S_OK);
    check(WaitForMultipleObjects(4,threads,TRUE,10000) == WAIT_OBJECT_0);
    for (i=0; i<4; ++i)
    {
        total+=r[i].submitted;
        CloseHandle(threads[i]);
    }
    check(c.calls == (LONG)total);
    closeq(q);
    CloseHandle(start);
}
struct active_shutdown { struct blocking *b; XTaskQueueHandle q; HANDLE done; };
static DWORD WINAPI wait_termination(void *context)
{
    struct active_shutdown *s=context;
    check(terminate(s->q,TRUE,s->done,finished) == S_OK);
    return 0;
}
static void active_shutdown(void)
{
    XTaskQueueHandle q;
    struct blocking b[2];
    struct active_shutdown s;
    HANDLE thread, done=CreateEventW(NULL,TRUE,FALSE,NULL);
    unsigned int i;
    check(create(POOL, POOL, &q) == S_OK);
    for (i=0; i<2; ++i)
    {
        b[i].q=q; b[i].entered=CreateEventW(NULL,TRUE,FALSE,NULL);
        b[i].release=CreateEventW(NULL,TRUE,FALSE,NULL);
        b[i].completed=CreateEventW(NULL,TRUE,FALSE,NULL);
        check(submit(q,WORK,&b[i],blocked) == S_OK);
    }
    /* Both callbacks must enter before either is released: parallel pool mode. */
    for (i=0; i<2; ++i) check(WaitForSingleObject(b[i].entered,3000) == WAIT_OBJECT_0);
    /* Final runtime release must neither wait for nor cancel active callbacks. */
    uninitialize(); uninitialize();
    check(initialize(0, 0) == S_OK);
    s.q=q; s.done=done; s.b=b;
    thread=CreateThread(NULL,0,wait_termination,&s,0,NULL);
    check(WaitForSingleObject(done,50) == WAIT_TIMEOUT);
    for (i=0; i<2; ++i) SetEvent(b[i].release);
    check(WaitForSingleObject(thread,3000) == WAIT_OBJECT_0);
    check(WaitForSingleObject(done,0) == WAIT_OBJECT_0);
    closeq(q);
    for (i=0; i<2; ++i)
    {
        CloseHandle(b[i].entered); CloseHandle(b[i].release); CloseHandle(b[i].completed);
    }
    CloseHandle(thread); CloseHandle(done);
}

static DWORD WINAPI cycle_runtime(void *context)
{
    unsigned int i;
    for (i = 0; i < 500; ++i)
    {
        check(initialize(0, 0) == S_OK);
        uninitialize();
    }
    return 0;
}

/* SerializedThreadPool: one callback at a time, FIFO, on a pool thread. */
struct serial { LONG inside, overlap, calls, target; unsigned int next; DWORD submitter; HANDLE done; };
static void CALLBACK serial_item(void *context, BOOLEAN canceled)
{
    struct serial *s = context;
    if (InterlockedIncrement(&s->inside) != 1) InterlockedIncrement(&s->overlap);
    if (GetCurrentThreadId() == s->submitter) InterlockedIncrement(&s->overlap);
    Sleep(0);
    InterlockedDecrement(&s->inside);
    if (InterlockedIncrement(&s->calls) == s->target) SetEvent(s->done);
}
struct serial_ordered { struct serial *s; unsigned int value; };
static void CALLBACK serial_in_order(void *context, BOOLEAN canceled)
{
    struct serial_ordered *o = context;
    check(!canceled);
    check(o->value == o->s->next++);
    serial_item(o->s, canceled);
}
struct serial_producer { XTaskQueueHandle q; struct serial *s; };
static DWORD WINAPI serial_produce(void *context)
{
    struct serial_producer *p = context;
    unsigned int i;
    for (i = 0; i < 250; ++i) check(submit(p->q, WORK, p->s, serial_item) == S_OK);
    return 0;
}
/* Immediate: the callback has run on the submitting thread before submit returns. */
struct immediate { LONG calls, canceled, depth; DWORD thread; XTaskQueueHandle q; HANDLE entered, release; };
static void CALLBACK immediate_item(void *context, BOOLEAN canceled)
{
    struct immediate *m = context;
    m->thread = GetCurrentThreadId();
    if (canceled) InterlockedIncrement(&m->canceled);
    InterlockedIncrement(&m->calls);
    if (m->depth && m->depth--) check(submit(m->q, COMPLETION, m, immediate_item) == S_OK);
    if (m->entered) SetEvent(m->entered);
    if (m->release) check(WaitForSingleObject(m->release, 5000) == WAIT_OBJECT_0);
}
static DWORD WINAPI immediate_submit(void *context)
{
    struct immediate *m = context;
    check(submit(m->q, COMPLETION, m, immediate_item) == S_OK);
    return 0;
}
static void CALLBACK serial_to_immediate(void *context, BOOLEAN canceled)
{
    struct immediate *m = context;
    check(submit(m->q, COMPLETION, m, immediate_item) == S_OK);
    check(m->thread == GetCurrentThreadId());
}
static void serialized_and_immediate(void)
{
    XTaskQueueHandle q;
    struct serial s = {0};
    struct serial_ordered items[200];
    struct serial_producer p;
    struct immediate m = {0};
    HANDLE threads[4], done = CreateEventW(NULL, TRUE, FALSE, NULL), thread;
    unsigned int i;
    /* FIFO from one producer, never overlapping, never on the submitter. */
    check(create(SERIAL, SERIAL, &q) == S_OK && q);
    s.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    s.submitter = GetCurrentThreadId();
    s.target = 200;
    for (i = 0; i < 200; ++i)
    {
        items[i].s = &s; items[i].value = i;
        check(submit(q, COMPLETION, &items[i], serial_in_order) == S_OK);
    }
    check(WaitForSingleObject(s.done, 10000) == WAIT_OBJECT_0);
    check(s.calls == 200 && s.next == 200 && !s.overlap);
    /* Concurrent producers still never overlap. */
    ResetEvent(s.done); s.calls = 0; s.target = 1000; s.submitter = 0;
    p.q = q; p.s = &s;
    for (i = 0; i < 4; ++i) threads[i] = CreateThread(NULL, 0, serial_produce, &p, 0, NULL);
    check(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (i = 0; i < 4; ++i) CloseHandle(threads[i]);
    check(WaitForSingleObject(s.done, 10000) == WAIT_OBJECT_0);
    check(s.calls == 1000 && !s.overlap);
    /* Termination drains every queued item before the final callback. */
    ResetEvent(s.done); s.calls = 0; s.target = 0;
    for (i = 0; i < 100; ++i) check(submit(q, WORK, &s, serial_item) == S_OK);
    check(terminate(q, TRUE, done, finished) == S_OK);
    check(WaitForSingleObject(done, 0) == WAIT_OBJECT_0 && s.calls == 100 && !s.overlap);
    check(submit(q, WORK, &s, serial_item) == E_ABORT);
    closeq(q);
    CloseHandle(s.done);

    /* Immediate runs before submit returns, on the submitting thread, re-entrantly. */
    check(create(IMMEDIATE, IMMEDIATE, &q) == S_OK && q);
    m.q = q;
    check(submit(q, WORK, &m, immediate_item) == S_OK);
    check(m.calls == 1 && !m.canceled && m.thread == GetCurrentThreadId());
    m.depth = 3;
    check(submit(q, COMPLETION, &m, immediate_item) == S_OK);
    check(m.calls == 5 && !m.depth);
    check(!dispatch(q, WORK, 0) && !dispatch(q, COMPLETION, 0));
    m.calls = 0;
    thread = CreateThread(NULL, 0, immediate_submit, &m, 0, NULL);
    check(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    check(m.calls == 1 && m.thread == GetThreadId(thread));
    CloseHandle(thread);
    /* Termination waits for an active Immediate callback, then rejects submissions. */
    m.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    m.release = CreateEventW(NULL, TRUE, FALSE, NULL);
    ResetEvent(done);
    thread = CreateThread(NULL, 0, immediate_submit, &m, 0, NULL);
    check(WaitForSingleObject(m.entered, 5000) == WAIT_OBJECT_0);
    check(terminate(q, FALSE, done, finished) == S_OK);
    check(WaitForSingleObject(done, 100) == WAIT_TIMEOUT);
    SetEvent(m.release);
    check(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0);
    check(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    CloseHandle(m.entered); CloseHandle(m.release);
    m.entered = m.release = NULL;
    check(submit(q, COMPLETION, &m, immediate_item) == E_ABORT);
    closeq(q);
    /* Canceled completions posted while Manual work drains run immediately as canceled. */
    memset(&m, 0, sizeof(m));
    check(create(MANUAL, IMMEDIATE, &q) == S_OK);
    m.q = q;
    ResetEvent(done);
    check(terminate(q, FALSE, done, finished) == S_OK);
    check(submit(q, COMPLETION, &m, immediate_item) == S_OK);
    check(m.calls == 1 && m.canceled == 1);
    check(WaitForSingleObject(done, 100) == WAIT_TIMEOUT);
    check(dispatch(q, WORK, 1000));
    check(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0);
    closeq(q);
    /* Serialized work posting to an Immediate completion port completes inline. */
    memset(&m, 0, sizeof(m));
    check(create(SERIAL, IMMEDIATE, &q) == S_OK);
    m.q = q;
    s.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    check(submit(q, WORK, &m, serial_to_immediate) == S_OK);
    ResetEvent(done);
    check(terminate(q, TRUE, done, finished) == S_OK);
    check(m.calls == 1 && m.thread != GetCurrentThreadId());
    closeq(q);
    CloseHandle(s.done);
    CloseHandle(done);
}
/* Composite queues: borrowed ports, owner-independent termination. */
#define getport(q,p,h) IXThreadingImpl_XTaskQueueGetPort(api,q,p,h)
#define composite(w,c,q) IXThreadingImpl_XTaskQueueCreateComposite(api,w,c,q)
static void composites(void)
{
    XTaskQueueHandle a, b, c, c2, d;
    XTaskQueuePortHandle work, completion, again;
    struct counts x = {0}, y = {0};
    struct serial s = {0};
    struct immediate m = {0};
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
    unsigned int i;
    /* Manual owner ports: composite submissions land on the owner's ports. */
    check(create(MANUAL, MANUAL, &a) == S_OK);
    check(getport(a, WORK, &work) == S_OK && work);
    check(getport(a, COMPLETION, &completion) == S_OK && completion && completion != work);
    check(composite(work, completion, &c) == S_OK && c && c != a);
    check(getport(c, WORK, &again) == S_OK && again == work);
    check(composite(again, completion, &c2) == S_OK);
    check(submit(c, WORK, &x, count) == S_OK);
    check(submit(c, COMPLETION, &y, count) == S_OK);
    check(dispatch(a, WORK, 0) && x.calls == 1 && !x.canceled);
    check(dispatch(c, COMPLETION, 0) && y.calls == 1 && !y.canceled);
    check(!dispatch(a, COMPLETION, 0));
    /* Terminating the composite cancels only its own pending entries. */
    x.calls = y.calls = 0;
    check(submit(c, WORK, &x, count) == S_OK);
    check(submit(a, WORK, &y, count) == S_OK);
    ResetEvent(done);
    check(terminate(c, FALSE, done, finished) == S_OK);
    check(terminate(c, FALSE, NULL, NULL) == E_ABORT);
    check(submit(c, WORK, &x, count) == E_ABORT);
    check(WaitForSingleObject(done, 50) == WAIT_TIMEOUT);
    check(dispatch(a, WORK, 0) && x.calls == 1 && x.canceled == 1);
    check(dispatch(a, WORK, 0) && y.calls == 1 && !y.canceled);
    /* Final callback runs on the borrowed completion port's executor. */
    check(WaitForSingleObject(done, 50) == WAIT_TIMEOUT);
    check(dispatch(a, COMPLETION, 0));
    check(WaitForSingleObject(done, 0) == WAIT_OBJECT_0);
    check(submit(c, COMPLETION, &x, count) == E_ABORT);
    /* The owner and the other composite are unaffected. */
    check(submit(a, WORK, &y, count) == S_OK && dispatch(a, WORK, 0) && y.calls == 2);
    check(submit(c2, WORK, &y, count) == S_OK && dispatch(a, WORK, 0) && y.calls == 3);
    closeq(c);
    /* A composite keeps borrowed ports alive after the owner's handles close. */
    closeq(a);
    check(getport(c2, WORK, &again) == S_OK && again == work);
    check(composite(work, completion, &d) == E_INVALIDARG && !d);
    check(submit(c2, WORK, &y, count) == S_OK && dispatch(c2, WORK, 0) && y.calls == 4);
    drain(c2);

    /* Owner termination rejects and cancels composite traffic on its ports. */
    check(create(MANUAL, MANUAL, &a) == S_OK);
    check(getport(a, WORK, &work) == S_OK && getport(a, COMPLETION, &completion) == S_OK);
    check(composite(work, completion, &c) == S_OK);
    memset(&x, 0, sizeof(x));
    check(submit(c, WORK, &x, count) == S_OK);
    check(terminate(a, FALSE, NULL, NULL) == S_OK);
    check(submit(c, WORK, &x, count) == E_ABORT);
    check(dispatch(a, WORK, 0) && x.calls == 1 && x.canceled == 1);
    while (dispatch(a, WORK, 100)) {}
    while (dispatch(a, COMPLETION, 100)) {}
    ResetEvent(done);
    check(terminate(c, TRUE, done, finished) == S_OK);
    check(WaitForSingleObject(done, 0) == WAIT_OBJECT_0);
    closeq(c);
    closeq(a);

    /* Mixed owners and modes: Serialized work from one queue, Immediate completion from another. */
    check(create(SERIAL, MANUAL, &a) == S_OK);
    check(create(MANUAL, IMMEDIATE, &b) == S_OK);
    check(getport(a, WORK, &work) == S_OK && getport(b, COMPLETION, &completion) == S_OK);
    check(composite(work, completion, &c) == S_OK);
    s.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    s.target = 300;
    for (i = 0; i < 300; ++i) check(submit(c, WORK, &s, serial_item) == S_OK);
    check(WaitForSingleObject(s.done, 10000) == WAIT_OBJECT_0);
    check(s.calls == 300 && !s.overlap);
    m.q = c;
    check(submit(c, COMPLETION, &m, immediate_item) == S_OK && m.calls == 1 && m.thread == GetCurrentThreadId());
    ResetEvent(done);
    check(terminate(c, TRUE, done, finished) == S_OK);
    check(WaitForSingleObject(done, 0) == WAIT_OBJECT_0);
    check(submit(a, WORK, &s, serial_item) == S_OK);
    closeq(c);
    drain(a);
    drain(b);
    /* Composite handles can be duplicated, and closing composites is independent. */
    check(create(POOL, POOL, &a) == S_OK);
    check(getport(a, WORK, &work) == S_OK && getport(a, COMPLETION, &completion) == S_OK);
    for (i = 0; i < 200; ++i)
    {
        check(composite(work, completion, &c) == S_OK);
        check(IXThreadingImpl_XTaskQueueDuplicateHandle(api, c, &d) == S_OK && d == c);
        closeq(c);
        check(terminate(d, TRUE, NULL, NULL) == S_OK);
        closeq(d);
    }
    drain(a);
    CloseHandle(s.done);
    CloseHandle(done);
}
/* Delayed submissions: not before their delay, FIFO by due time, expedited
 * as canceled by termination, and counted by composite termination. */
#define delayed(q,p,d,c,f) IXThreadingImpl_XTaskQueueSubmitDelayedCallback(api,q,p,d,c,f)
struct stamp { ULONGLONG at; LONG calls, canceled; HANDLE event; };
static void CALLBACK stamped(void *context, BOOLEAN canceled)
{
    struct stamp *s = context;
    s->at = GetTickCount64();
    if (canceled) InterlockedIncrement(&s->canceled);
    InterlockedIncrement(&s->calls);
    if (s->event) SetEvent(s->event);
}
static XAsyncBlock delayed_block;
static ULONGLONG delayed_scheduled, delayed_ran;
static HRESULT WINAPI delayed_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    switch (op)
    {
    case XAsyncOp_Begin:
        delayed_scheduled = GetTickCount64();
        return IXThreadingImpl_XAsyncSchedule(api, data->async, 150);
    case XAsyncOp_DoWork:
        delayed_ran = GetTickCount64();
        IXThreadingImpl_XAsyncComplete(api, data->async, S_OK, 0);
        return E_PENDING;
    default:
        return S_OK;
    }
}
static void delays(void)
{
    XTaskQueueHandle q, c;
    XTaskQueuePortHandle work, completion;
    struct stamp a = {0}, b = {0}, x = {0}, y = {0};
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
    ULONGLONG start;
    /* Pool port: fires after its delay, earlier due time first. */
    check(create(POOL, POOL, &q) == S_OK);
    a.event = CreateEventW(NULL, TRUE, FALSE, NULL);
    b.event = CreateEventW(NULL, TRUE, FALSE, NULL);
    start = GetTickCount64();
    check(delayed(q, WORK, 300, &a, stamped) == S_OK);
    check(delayed(q, COMPLETION, 100, &b, stamped) == S_OK);
    check(WaitForSingleObject(b.event, 5000) == WAIT_OBJECT_0);
    check(WaitForSingleObject(a.event, 5000) == WAIT_OBJECT_0);
    check(b.at - start >= 90 && a.at - start >= 290 && b.at < a.at);
    check(!a.canceled && !b.canceled);
    /* Termination expedites a long delay as a canceled callback. */
    ResetEvent(a.event); a.calls = 0;
    check(delayed(q, WORK, 60000, &a, stamped) == S_OK);
    start = GetTickCount64();
    check(terminate(q, TRUE, done, finished) == S_OK);
    check(a.calls == 1 && a.canceled == 1 && GetTickCount64() - start < 5000);
    check(WaitForSingleObject(done, 0) == WAIT_OBJECT_0);
    check(delayed(q, WORK, 10, &a, stamped) == E_ABORT);
    closeq(q);
    /* Manual port: the item becomes dispatchable only after the delay. */
    check(create(MANUAL, MANUAL, &q) == S_OK);
    memset(&x, 0, sizeof(x));
    check(delayed(q, WORK, 200, &x, stamped) == S_OK);
    check(!dispatch(q, WORK, 50) && !x.calls);
    check(dispatch(q, WORK, 2000) && x.calls == 1 && !x.canceled);
    /* Composite termination expedites only its own delayed entries. */
    check(IXThreadingImpl_XTaskQueueGetPort(api, q, WORK, &work) == S_OK);
    check(IXThreadingImpl_XTaskQueueGetPort(api, q, COMPLETION, &completion) == S_OK);
    check(IXThreadingImpl_XTaskQueueCreateComposite(api, work, completion, &c) == S_OK);
    memset(&x, 0, sizeof(x));
    check(delayed(c, WORK, 60000, &x, stamped) == S_OK);
    check(delayed(q, WORK, 1500, &y, stamped) == S_OK);
    ResetEvent(done);
    check(terminate(c, FALSE, done, finished) == S_OK);
    check(dispatch(q, WORK, 2000) && x.calls == 1 && x.canceled == 1);
    check(dispatch(q, COMPLETION, 2000));
    check(WaitForSingleObject(done, 0) == WAIT_OBJECT_0);
    check(!y.calls);
    check(dispatch(q, WORK, 5000) && y.calls == 1 && !y.canceled);
    closeq(c);
    drain(q);
    /* XAsyncSchedule honours a nonzero delay. */
    check(create(POOL, POOL, &q) == S_OK);
    delayed_block.queue = q;
    check(IXThreadingImpl_XAsyncBegin(api, &delayed_block, NULL, NULL, "delayed", delayed_provider) == S_OK);
    check(IXThreadingImpl_XAsyncGetStatus(api, &delayed_block, TRUE) == S_OK);
    check(delayed_ran - delayed_scheduled >= 140);
    drain(q);
    CloseHandle(a.event); CloseHandle(b.event); CloseHandle(done);
}
static void lifecycle(void)
{
    IXThreadingImpl *other = (void *)1;
    XTaskQueueHandle q, d;
    struct counts c = {0};
    HANDLE threads[4];
    unsigned int i;
    uninitialize(); uninitialize();
    check(query_api(&CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&other)
          == (HRESULT)0x89240100 && !other);
    check(initialize(0, 0) == S_OK);
    check(initialize(0, 0) == S_OK);
    check(query_api(&CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&api) == S_OK);
    check(create(MANUAL, MANUAL, &q) == S_OK);
    check(duplicate(q, &d) == S_OK);
    check(submit(q, WORK, &c, count) == S_OK);
    for (i = 0; i < 4; ++i) threads[i] = CreateThread(NULL, 0, cycle_runtime, NULL, 0, NULL);
    check(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (i = 0; i < 4; ++i) CloseHandle(threads[i]);
    uninitialize();
    check(query_api(&CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&other) == S_OK);
    IXThreadingImpl_Release(other);
    uninitialize(); uninitialize();
    check(query_api(&CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&other)
          == (HRESULT)0x89240100 && !other);
    closeq(q);
    check(dispatch(d, WORK, 0) && c.calls == 1 && !c.canceled);
    check(submit(d, COMPLETION, &c, count) == S_OK);
    check(dispatch(d, COMPLETION, 0) && c.calls == 2 && !c.canceled);
    check(initialize(0, 0) == S_OK);
    drain(d);
}
int main(int argc, char **argv)
{
    HMODULE module;
    HRESULT (WINAPI *query)(REFCLSID, REFIID, void **);
    if (argc != 2 || !(module=LoadLibraryA(argv[1])))
    { printf("LoadLibrary failed %lu; supply absolute DLL path\n", GetLastError()); return 2; }
    initialize=(void *)GetProcAddress(module,"InitializeApiImpl");
    uninitialize=(void *)GetProcAddress(module,"UninitializeApiImpl");
    query_api=query=(void *)GetProcAddress(module,"QueryApiImpl");
    if (!initialize || !uninitialize || !query) return 2;
    lifecycle();
    if (!query || query(&CLSID_XThreadingImpl,&IID_IXThreadingImpl,(void **)&api) != S_OK) return 2;
    {
        XTaskQueueHandle probe;
        if (create(MANUAL,MANUAL,&probe) != S_OK)
        { printf("Wrong DLL: queue creation is unavailable\n"); return 2; }
        drain(probe);
    }
    invalid(); manual(); concurrent(); shutdown_tests(); lifetime_races(); active_shutdown();
    serialized_and_immediate(); composites(); delays();
    uninitialize();
    printf("%ld checks, %ld failures\n", checks, failures);
    /* Pool callbacks may still be returning through the DLL: process exit owns
       unloading, just as it does for normal application shutdown. */
    return failures ? 1 : 0;
}

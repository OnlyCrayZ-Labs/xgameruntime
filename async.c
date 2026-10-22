// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* XAsync provider subset, Copyright 2026 Xodus contributors. LGPL-2.1-or-later. */
#include <stdlib.h>
#include "private.h"
#include "taskqueue_private.h"
#include "async_private.h"

struct async_state
{
    struct async_state *next;
    LONG refs;
    XAsyncBlock *caller, shadow;
    XAsyncCompletionRoutine *callback;
    XAsyncProviderData data;
    XAsyncProvider *provider;
    const void *identity;
    HRESULT status;
    SIZE_T size;
    CONDITION_VARIABLE ready;
    BOOL linked, scheduled, retrieving, canceling;
};
static SRWLOCK lock = SRWLOCK_INIT;
static struct async_state *states;
static const char status_marker;

static struct async_state *acquire(XAsyncBlock *block)
{
    struct async_state *s;
    AcquireSRWLockExclusive(&lock);
    for (s = states; s; s = s->next)
        if (s->caller == block || &s->shadow == block) break;
    if (s) InterlockedIncrement(&s->refs);
    ReleaseSRWLockExclusive(&lock);
    return s;
}
static void release(struct async_state *s)
{
    if (!InterlockedDecrement(&s->refs))
    {
        s->provider(XAsyncOp_Cleanup, &s->data);
        task_queue_close(s->shadow.queue);
        free(s);
    }
}
static void detach(struct async_state *s)
{
    struct async_state **p;
    BOOL linked;
    AcquireSRWLockExclusive(&lock);
    if ((linked = s->linked))
    {
        for (p = &states; *p != s; p = &(*p)->next) {}
        *p = s->next;
        s->linked = FALSE;
    }
    ReleaseSRWLockExclusive(&lock);
    if (linked) release(s);
}
static void CALLBACK completed(void *context, BOOLEAN canceled)
{
    struct async_state *s = context;
    /* A queued callback owns the state even if GetResult consumed its payload. */
    if (FAILED(s->status) || !s->size) detach(s);
    s->callback(s->caller);
    release(s);
}
void async_complete(XAsyncBlock *block, HRESULT hr, SIZE_T size)
{
    struct async_state *s = acquire(block);
    BOOL callback;
    HRESULT submit_hr;
    if (!s) return;
    AcquireSRWLockExclusive(&lock);
    if (s->status != E_PENDING || hr == E_PENDING)
    {
        ReleaseSRWLockExclusive(&lock);
        release(s);
        return;
    }
    s->status = hr;
    s->size = SUCCEEDED(hr) ? size : 0;
    /* Cache terminal status before handing control to caller code. */
    s->caller->internal[0] = (void *)(ULONG_PTR)(ULONG)hr;
    s->caller->internal[1] = (void *)&status_marker;
    callback = !!s->callback;
    if (callback) InterlockedIncrement(&s->refs);
    WakeAllConditionVariable(&s->ready);
    ReleaseSRWLockExclusive(&lock);
    if (callback)
    {
        submit_hr = task_queue_submit(s->shadow.queue, XTaskQueuePort_Completion, s, completed);
        /* A queue already drained its completion port. Do not run user code
         * on the wrong executor; preserve a failed terminal status for polling. */
        if (FAILED(submit_hr))
        {
            AcquireSRWLockExclusive(&lock);
            s->status = submit_hr;
            s->size = 0;
            s->caller->internal[0] = (void *)(ULONG_PTR)(ULONG)submit_hr;
            ReleaseSRWLockExclusive(&lock);
            detach(s);
            release(s);
        }
    }
    else if (FAILED(hr) || !size) detach(s);
    release(s);
}
static void CALLBACK work(void *context, BOOLEAN canceled)
{
    struct async_state *s = context;
    HRESULT hr;
    BOOL pending;
    AcquireSRWLockExclusive(&lock);
    s->scheduled = FALSE;
    pending = s->status == E_PENDING;
    ReleaseSRWLockExclusive(&lock);
    if (pending)
    {
        if (canceled) async_cancel(&s->shadow);
        else
        {
            hr = s->provider(XAsyncOp_DoWork, &s->data);
            if (hr != E_PENDING) async_complete(&s->shadow, FAILED(hr) ? hr : E_UNEXPECTED, 0);
        }
    }
    release(s);
}
HRESULT async_begin_owned(XAsyncBlock *block, void *context, const void *identity, XAsyncProvider *provider, void (*reject)(void *))
{
    struct async_state *s, *other;
    HRESULT hr;
    if (!block || !provider) { if (reject) reject(context); return E_INVALIDARG; }
    if (!(s = calloc(1, sizeof(*s)))) { if (reject) reject(context); return E_OUTOFMEMORY; }
    if (block->queue) hr = task_queue_duplicate(block->queue, &s->shadow.queue);
    else hr = task_queue_create(XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &s->shadow.queue);
    if (FAILED(hr)) { free(s); if (reject) reject(context); return hr; }
    s->refs = 2; /* registry and this call */
    s->caller = block;
    s->callback = block->callback;
    s->data.async = &s->shadow;
    s->data.context = context;
    s->provider = provider;
    s->identity = identity;
    s->status = E_PENDING;
    AcquireSRWLockExclusive(&lock);
    for (other = states; other && other->caller != block; other = other->next) {}
    if (other)
    {
        ReleaseSRWLockExclusive(&lock);
        task_queue_close(s->shadow.queue);
        free(s);
        if (reject) reject(context);
        return E_INVALIDARG;
    }
    memset(block->internal, 0, sizeof(block->internal));
    s->next = states;
    states = s;
    s->linked = TRUE;
    ReleaseSRWLockExclusive(&lock);
    hr = provider(XAsyncOp_Begin, &s->data);
    if (FAILED(hr)) detach(s);
    release(s);
    return hr;
}
HRESULT async_schedule(XAsyncBlock *block, UINT32 delay)
{
    struct async_state *s;
    HRESULT hr;
    if (!(s = acquire(block))) return E_INVALIDARG;
    AcquireSRWLockExclusive(&lock);
    if (s->status != E_PENDING || s->scheduled) hr = E_UNEXPECTED;
    else { s->scheduled = TRUE; InterlockedIncrement(&s->refs); hr = S_OK; }
    ReleaseSRWLockExclusive(&lock);
    if (SUCCEEDED(hr))
    {
        hr = task_queue_submit_delayed(s->shadow.queue, XTaskQueuePort_Work, delay, s, work);
        if (FAILED(hr))
        {
            AcquireSRWLockExclusive(&lock);
            s->scheduled = FALSE;
            ReleaseSRWLockExclusive(&lock);
            release(s);
        }
    }
    release(s);
    return hr;
}
HRESULT async_status(XAsyncBlock *block, BOOLEAN wait)
{
    struct async_state *s;
    HRESULT hr;
    if (!block) return E_INVALIDARG;
    if (!(s = acquire(block)))
    {
        AcquireSRWLockShared(&lock);
        hr = block->internal[1] == &status_marker ? (HRESULT)(ULONG_PTR)block->internal[0] : E_INVALIDARG;
        ReleaseSRWLockShared(&lock);
        return hr;
    }
    AcquireSRWLockExclusive(&lock);
    while (wait && s->status == E_PENDING) SleepConditionVariableSRW(&s->ready, &lock, INFINITE, 0);
    hr = s->status;
    ReleaseSRWLockExclusive(&lock);
    release(s);
    return hr;
}
HRESULT async_size(XAsyncBlock *block, SIZE_T *size)
{
    struct async_state *s;
    HRESULT hr;
    if (!size) return E_POINTER;
    *size = 0;
    if (!(s = acquire(block))) return FAILED(hr = async_status(block, FALSE)) ? hr : E_INVALIDARG;
    AcquireSRWLockExclusive(&lock);
    hr = s->status;
    if (SUCCEEDED(hr)) *size = s->size;
    ReleaseSRWLockExclusive(&lock);
    release(s);
    return hr;
}
HRESULT async_result(XAsyncBlock *block, const void *identity, SIZE_T size, void *buffer, SIZE_T *used)
{
    struct async_state *s;
    XAsyncProviderData data;
    HRESULT hr;
    if (used) *used = 0;
    if (!(s = acquire(block))) return FAILED(hr = async_status(block, FALSE)) ? hr : E_INVALIDARG;
    AcquireSRWLockExclusive(&lock);
    hr = s->status;
    if (SUCCEEDED(hr))
    {
        if (identity != s->identity) hr = E_INVALIDARG;
        else if (size < s->size) hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        else if (s->size && !buffer) hr = E_POINTER;
        else if (s->retrieving) hr = E_UNEXPECTED;
        else s->retrieving = TRUE;
    }
    data = s->data;
    data.buffer = buffer;
    data.bufferSize = size;
    ReleaseSRWLockExclusive(&lock);
    if (SUCCEEDED(hr))
    {
        hr = s->provider(XAsyncOp_GetResult, &data);
        if (SUCCEEDED(hr) && used) *used = s->size;
        detach(s);
    }
    release(s);
    return hr;
}
void async_cancel(XAsyncBlock *block)
{
    struct async_state *s = acquire(block);
    BOOL cancel;
    if (!s) return;
    AcquireSRWLockExclusive(&lock);
    cancel = s->status == E_PENDING && !s->canceling;
    if (cancel) s->canceling = TRUE;
    ReleaseSRWLockExclusive(&lock);
    if (cancel) s->provider(XAsyncOp_Cancel, &s->data);
    release(s);
}

HRESULT async_begin(XAsyncBlock *block, void *context, const void *identity, XAsyncProvider *provider)
{
    return async_begin_owned(block, context, identity, provider, NULL);
}

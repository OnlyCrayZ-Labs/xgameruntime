// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XAsync, XTaskQueue and XThread
 *
 * Copyright 2026 Xodus contributors
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


#include <stdlib.h>
#include <windows.h>
#include "xtaskqueue.h"
#include "taskqueue_private.h"

/* The registry validates opaque handles without dereferencing caller memory.
 * Lock order: registry lock is never held while taking a queue lock. References
 * held by API calls, queued entries and pool invocations protect queue storage.
 * A composite queue's lock is never held while taking a port owner's lock.
 */
struct queue_entry
{
    struct queue_entry *next;
    XTaskQueueCallback *callback;
    void *context;
    struct XTaskQueueObject *via; /* composite submitter, owning one reference */
    unsigned int via_port;
};

/* A delayed submission waits on its own timer, then is queued like any other
 * entry. Waiting entries hold back the termination barrier; termination fires
 * their timers at once so they run promptly as canceled callbacks. */
struct delayed_entry
{
    struct delayed_entry *next;
    struct queue_port *port;
    struct XTaskQueueObject *via;
    unsigned int via_port;
    XTaskQueueCallback *callback;
    void *context;
    PTP_TIMER timer;
};

struct queue_port
{
    struct XTaskQueueObject *queue;
    XTaskQueueDispatchMode mode;
    struct queue_entry *head, *tail;
    struct delayed_entry *delayed;
    CONDITION_VARIABLE ready;
    PTP_WORK pool;
    unsigned int active, waiting;
    BOOL canceled, stopping, stopped, barrier;
};

struct XTaskQueueObject
{
    struct XTaskQueueObject *next;
    LONG refs;
    unsigned int handles;
    SRWLOCK lock;
    CONDITION_VARIABLE terminated;
    struct queue_port ports[2];
    BOOL terminating, done;
    XTaskQueueTerminatedCallback *callback;
    void *context;
    /* Composite queues borrow ports owned by ordinary queues and hold a
     * reference to each owner. Their termination only cancels and drains the
     * entries submitted through them, counted per composite port. */
    struct queue_port *target[2];
    struct { unsigned int pending; BOOL canceled, stopping; } via[2];
    BOOL finishing;
};

static SRWLOCK registry_lock = SRWLOCK_INIT;
static struct XTaskQueueObject *queues;

static struct XTaskQueueObject *queue_acquire( XTaskQueueHandle handle )
{
    struct XTaskQueueObject *q;
    AcquireSRWLockExclusive( &registry_lock );
    for (q = queues; q && q != handle; q = q->next) {}
    if (q && !q->handles) q = NULL;
    if (q) ++q->refs;
    ReleaseSRWLockExclusive( &registry_lock );
    return q;
}

static void queue_addref( struct XTaskQueueObject *q )
{
    AcquireSRWLockExclusive( &registry_lock );
    ++q->refs;
    ReleaseSRWLockExclusive( &registry_lock );
}

static void queue_release( struct XTaskQueueObject *q )
{
    struct XTaskQueueObject **link;
    BOOL destroy;
    AcquireSRWLockExclusive( &registry_lock );
    if ((destroy = !--q->refs))
    {
        for (link = &queues; *link != q; link = &(*link)->next) {}
        *link = q->next;
    }
    ReleaseSRWLockExclusive( &registry_lock );
    if (!destroy) return;
    if (q->ports[0].pool) CloseThreadpoolWork( q->ports[0].pool );
    if (q->ports[1].pool) CloseThreadpoolWork( q->ports[1].pool );
    if (q->target[0]) queue_release( q->target[0]->queue );
    if (q->target[1]) queue_release( q->target[1]->queue );
    free( q );
}

static struct queue_port *queue_port( struct XTaskQueueObject *q, unsigned int index )
{
    return q->target[index] ? q->target[index] : &q->ports[index];
}

static HRESULT port_submit( struct queue_port *port, struct XTaskQueueObject *via, unsigned int via_port,
                            void *context, XTaskQueueCallback *callback, BOOL accepted );

static BOOL composite_canceled( struct XTaskQueueObject *c, unsigned int index )
{
    BOOL canceled;
    AcquireSRWLockShared( &c->lock );
    canceled = c->via[index].canceled;
    ReleaseSRWLockShared( &c->lock );
    return canceled;
}

/* Called under the composite's lock. Completion stops once work submitted
 * through the composite drains; returns TRUE once when the final callback may run. */
static BOOL composite_advance( struct XTaskQueueObject *c )
{
    if (!c->terminating || c->finishing) return FALSE;
    if (!c->via[0].pending) c->via[1].stopping = TRUE;
    if (!c->via[1].stopping || c->via[1].pending) return FALSE;
    c->finishing = TRUE;
    return TRUE;
}

static void CALLBACK composite_finished( void *context, BOOLEAN canceled )
{
    struct XTaskQueueObject *c = context;
    if (c->callback) c->callback( c->context );
    AcquireSRWLockExclusive( &c->lock );
    c->done = TRUE;
    WakeAllConditionVariable( &c->terminated );
    ReleaseSRWLockExclusive( &c->lock );
    queue_release( c ); /* termination reference */
}

/* The final callback runs on the completion port's executor, or on this thread
 * when that port no longer accepts callbacks. */
static void composite_finish( struct XTaskQueueObject *c )
{
    if (FAILED(port_submit( c->target[1], NULL, 0, c, composite_finished, FALSE ))) composite_finished( c, TRUE );
}

static void composite_entry_done( struct XTaskQueueObject *c, unsigned int index )
{
    BOOL finish;
    AcquireSRWLockExclusive( &c->lock );
    --c->via[index].pending;
    finish = composite_advance( c );
    ReleaseSRWLockExclusive( &c->lock );
    if (finish) composite_finish( c );
    queue_release( c ); /* entry reference */
}

/* Called under the queue lock. Each pool invocation owns a separate reference,
 * including invocations whose item was already taken by a manual dispatcher. */
static void port_signal( struct queue_port *port )
{
    WakeAllConditionVariable( &port->ready );
    if (port->pool)
    {
        queue_addref( port->queue );
        SubmitThreadpoolWork( port->pool );
    }
}

static void port_check_barrier( struct queue_port *port )
{
    if (!port->stopping || port->stopped || port->barrier || port->active || port->head || port->waiting) return;
    port->barrier = TRUE;
    port_signal( port );
}

/* A SerializedThreadPool port runs at most one callback at a time, in FIFO
 * order; its next item becomes ready only when the active callback returns. */
static BOOL port_ready( struct queue_port *port )
{
    return port->head && !(port->mode == XTaskQueueDispatchMode_SerializedThreadPool && port->active);
}

/* One item per dispatch. A termination barrier is itself dispatched on the
 * port's chosen executor, so even an empty manual port must be pumped. */
static BOOL port_dispatch( struct queue_port *port, DWORD timeout )
{
    struct XTaskQueueObject *q = port->queue, *via;
    struct queue_entry *entry;
    ULONGLONG deadline = GetTickCount64() + timeout, now;
    DWORD remaining = timeout;
    unsigned int via_port;
    BOOL canceled;
    XTaskQueueTerminatedCallback *callback;
    void *context;

    AcquireSRWLockExclusive( &q->lock );
    while (!port_ready( port ) && !port->barrier && !port->stopped)
    {
        if (!remaining) break;
        if (!SleepConditionVariableSRW( &port->ready, &q->lock, remaining, 0 ) &&
            GetLastError() == ERROR_TIMEOUT) break;
        if (timeout != INFINITE)
        {
            now = GetTickCount64();
            remaining = now >= deadline ? 0 : deadline - now;
        }
    }
    if (port_ready( port ))
    {
        entry = port->head;
        port->head = entry->next;
        if (!port->head) port->tail = NULL;
        ++port->active;
        canceled = port->canceled;
        ReleaseSRWLockExclusive( &q->lock );
        via = entry->via;
        via_port = entry->via_port;
        if (via && !canceled) canceled = composite_canceled( via, via_port );
        entry->callback( entry->context, canceled );
        free( entry );
        AcquireSRWLockExclusive( &q->lock );
        --port->active;
        if (port->mode == XTaskQueueDispatchMode_SerializedThreadPool && port->head) port_signal( port );
        port_check_barrier( port );
        ReleaseSRWLockExclusive( &q->lock );
        queue_release( q ); /* entry reference */
        if (via) composite_entry_done( via, via_port );
        return TRUE;
    }
    if (!port->barrier)
    {
        ReleaseSRWLockExclusive( &q->lock );
        return FALSE;
    }
    port->barrier = FALSE;
    port->stopped = TRUE;
    WakeAllConditionVariable( &port->ready );
    if (port == &q->ports[0])
    {
        q->ports[1].stopping = TRUE;
        port_check_barrier( &q->ports[1] );
        ReleaseSRWLockExclusive( &q->lock );
    }
    else
    {
        callback = q->callback;
        context = q->context;
        ReleaseSRWLockExclusive( &q->lock );
        if (callback) callback( context );
        AcquireSRWLockExclusive( &q->lock );
        q->done = TRUE;
        WakeAllConditionVariable( &q->terminated );
        ReleaseSRWLockExclusive( &q->lock );
        queue_release( q ); /* termination reference */
    }
    return TRUE;
}

static void CALLBACK pool_dispatch( PTP_CALLBACK_INSTANCE instance, void *context, PTP_WORK work )
{
    struct queue_port *port = context;
    struct XTaskQueueObject *q = port->queue;
    port_dispatch( port, 0 );
    queue_release( q );
}

HRESULT task_queue_create( XTaskQueueDispatchMode work, XTaskQueueDispatchMode completion, XTaskQueueHandle *out )
{
    struct XTaskQueueObject *q;
    XTaskQueueDispatchMode modes[2] = {work, completion};
    unsigned int i;

    if (!out) return E_POINTER;
    *out = NULL;
    if ((unsigned int)work > XTaskQueueDispatchMode_Immediate ||
        (unsigned int)completion > XTaskQueueDispatchMode_Immediate) return E_INVALIDARG;
    if (!(q = calloc( 1, sizeof(*q) ))) return E_OUTOFMEMORY;
    InitializeSRWLock( &q->lock );
    InitializeConditionVariable( &q->terminated );
    q->refs = q->handles = 1;
    for (i = 0; i < 2; ++i)
    {
        q->ports[i].queue = q;
        q->ports[i].mode = modes[i];
        InitializeConditionVariable( &q->ports[i].ready );
        /* Immediate ports never queue callbacks; their pool work only runs
         * the termination barrier, which has no submitting thread. */
        if (modes[i] != XTaskQueueDispatchMode_Manual &&
            !(q->ports[i].pool = CreateThreadpoolWork( pool_dispatch, &q->ports[i], NULL )))
        {
            if (q->ports[0].pool) CloseThreadpoolWork( q->ports[0].pool );
            free( q );
            return E_OUTOFMEMORY;
        }
    }
    AcquireSRWLockExclusive( &registry_lock );
    q->next = queues;
    queues = q;
    ReleaseSRWLockExclusive( &registry_lock );
    *out = q;
    return S_OK;
}

HRESULT task_queue_duplicate( XTaskQueueHandle handle, XTaskQueueHandle *out )
{
    struct XTaskQueueObject *q;
    if (!out) return E_POINTER;
    /* Read the input before clearing output: aliasing is permitted. */
    *out = NULL;
    if (!(q = queue_acquire( handle ))) return E_INVALIDARG;
    AcquireSRWLockExclusive( &registry_lock );
    ++q->handles;
    ReleaseSRWLockExclusive( &registry_lock );
    *out = q; /* transfer acquire reference to the caller */
    return S_OK;
}

void task_queue_close( XTaskQueueHandle handle )
{
    struct XTaskQueueObject *q;
    BOOL closed = FALSE;
    if (!(q = queue_acquire( handle ))) return;
    AcquireSRWLockExclusive( &registry_lock );
    if (q->handles)
    {
        --q->handles;
        closed = TRUE;
    }
    ReleaseSRWLockExclusive( &registry_lock );
    if (closed) queue_release( q );
    queue_release( q );
}

/* Submit to a port owned by port->queue. A composite submitter has already
 * counted the entry and is notified once it has been consumed. An accepted
 * (formerly delayed) entry is queued even after termination began. */
static HRESULT port_submit( struct queue_port *port, struct XTaskQueueObject *via, unsigned int via_port,
                            void *context, XTaskQueueCallback *callback, BOOL accepted )
{
    struct XTaskQueueObject *q = port->queue;
    struct queue_entry *entry;
    BOOL canceled;
    if (!(entry = malloc( sizeof(*entry) ))) return E_OUTOFMEMORY;
    entry->next = NULL;
    entry->callback = callback;
    entry->context = context;
    entry->via = via;
    entry->via_port = via_port;
    AcquireSRWLockExclusive( &q->lock );
    if (port->stopping && !accepted)
    {
        ReleaseSRWLockExclusive( &q->lock );
        free( entry );
        return E_ABORT;
    }
    if (port->mode == XTaskQueueDispatchMode_Immediate)
    {
        /* Run on the submitting thread, outside the lock. The active count
         * holds the termination barrier until the callback returns. */
        canceled = port->canceled;
        ++port->active;
        ReleaseSRWLockExclusive( &q->lock );
        free( entry );
        if (via && !canceled) canceled = composite_canceled( via, via_port );
        callback( context, canceled );
        AcquireSRWLockExclusive( &q->lock );
        --port->active;
        port_check_barrier( port );
        ReleaseSRWLockExclusive( &q->lock );
        if (via) composite_entry_done( via, via_port );
        return S_OK;
    }
    queue_addref( q ); /* entry reference */
    if (port->tail) port->tail->next = entry;
    else port->head = entry;
    port->tail = entry;
    port_signal( port );
    ReleaseSRWLockExclusive( &q->lock );
    return S_OK;
}

static void CALLBACK delayed_fire( PTP_CALLBACK_INSTANCE instance, void *context, PTP_TIMER timer )
{
    struct delayed_entry *d = context, **link;
    struct queue_port *port = d->port;
    struct XTaskQueueObject *q = port->queue;
    AcquireSRWLockExclusive( &q->lock );
    for (link = &port->delayed; *link != d; link = &(*link)->next) {}
    *link = d->next;
    ReleaseSRWLockExclusive( &q->lock );
    /* Queue before releasing the barrier hold; failure is allocation only. */
    if (FAILED(port_submit( port, d->via, d->via_port, d->context, d->callback, TRUE )))
    {
        d->callback( d->context, TRUE );
        if (d->via) composite_entry_done( d->via, d->via_port );
    }
    AcquireSRWLockExclusive( &q->lock );
    --port->waiting;
    port_check_barrier( port );
    ReleaseSRWLockExclusive( &q->lock );
    CloseThreadpoolTimer( timer );
    free( d );
    queue_release( q ); /* delayed reference */
}

static HRESULT port_submit_delayed( struct queue_port *port, struct XTaskQueueObject *via, unsigned int via_port,
                                    UINT32 delay, void *context, XTaskQueueCallback *callback )
{
    struct XTaskQueueObject *q = port->queue;
    struct delayed_entry *d;
    LARGE_INTEGER due;
    if (!delay) return port_submit( port, via, via_port, context, callback, FALSE );
    if (!(d = calloc( 1, sizeof(*d) ))) return E_OUTOFMEMORY;
    d->port = port;
    d->via = via;
    d->via_port = via_port;
    d->callback = callback;
    d->context = context;
    if (!(d->timer = CreateThreadpoolTimer( delayed_fire, d, NULL )))
    {
        free( d );
        return E_OUTOFMEMORY;
    }
    AcquireSRWLockExclusive( &q->lock );
    if (port->stopping)
    {
        ReleaseSRWLockExclusive( &q->lock );
        CloseThreadpoolTimer( d->timer );
        free( d );
        return E_ABORT;
    }
    queue_addref( q ); /* delayed reference */
    ++port->waiting;
    d->next = port->delayed;
    port->delayed = d;
    due.QuadPart = -(LONGLONG)delay * 10000;
    SetThreadpoolTimer( d->timer, (FILETIME *)&due, 0, 0 );
    ReleaseSRWLockExclusive( &q->lock );
    return S_OK;
}

/* Called under the port owner's lock: fire waiting timers (of one composite, or all). */
static void port_expedite( struct queue_port *port, struct XTaskQueueObject *via )
{
    struct delayed_entry *d;
    LARGE_INTEGER due;
    due.QuadPart = -1;
    for (d = port->delayed; d; d = d->next)
        if (!via || d->via == via) SetThreadpoolTimer( d->timer, (FILETIME *)&due, 0, 0 );
}

HRESULT task_queue_submit_delayed( XTaskQueueHandle handle, XTaskQueuePort index, UINT32 delay,
                                   void *context, XTaskQueueCallback *callback )
{
    struct XTaskQueueObject *q;
    HRESULT hr;
    if ((unsigned int)index > XTaskQueuePort_Completion || !callback) return E_INVALIDARG;
    if (!(q = queue_acquire( handle ))) return E_INVALIDARG;
    if (!q->target[index]) hr = port_submit_delayed( &q->ports[index], NULL, 0, delay, context, callback );
    else
    {
        AcquireSRWLockExclusive( &q->lock );
        if (q->via[index].stopping) hr = E_ABORT;
        else
        {
            ++q->via[index].pending;
            hr = S_OK;
        }
        ReleaseSRWLockExclusive( &q->lock );
        if (SUCCEEDED(hr))
        {
            queue_addref( q ); /* entry reference, released by composite_entry_done */
            if (FAILED(hr = port_submit_delayed( q->target[index], q, index, delay, context, callback )))
                composite_entry_done( q, index );
        }
    }
    queue_release( q );
    return hr;
}

HRESULT task_queue_submit( XTaskQueueHandle handle, XTaskQueuePort index, void *context, XTaskQueueCallback *callback )
{
    return task_queue_submit_delayed( handle, index, 0, context, callback );
}

/* Remove only an internal owner's exact pending entry. Cancellation is a
 * cleanup invocation outside the queue lock, never a dispatch of user code.
 * An entry already taken by a dispatcher remains that dispatcher's property. */
BOOLEAN task_queue_remove_callback( XTaskQueueHandle handle, XTaskQueuePort index,
                                    void *context, XTaskQueueCallback *callback )
{
    struct XTaskQueueObject *q, *owner, *via;
    struct queue_port *port;
    struct queue_entry **link, *entry, *previous = NULL;
    if ((unsigned int)index > XTaskQueuePort_Completion) return FALSE;
    if (!(q = queue_acquire(handle))) return FALSE;
    port = queue_port(q, index);
    owner = port->queue;
    via = q->target[index] ? q : NULL;
    AcquireSRWLockExclusive(&owner->lock);
    for (link = &port->head; *link; link = &(*link)->next)
    {
        if ((*link)->context == context && (*link)->callback == callback && (*link)->via == via) break;
        previous = *link;
    }
    if ((entry = *link))
    {
        *link = entry->next;
        if (port->tail == entry) port->tail = previous;
        port_check_barrier(port);
    }
    ReleaseSRWLockExclusive(&owner->lock);
    if (entry)
    {
        entry->callback(entry->context, TRUE);
        free(entry);
        queue_release(owner); /* entry reference */
        if (via) composite_entry_done(via, index);
    }
    queue_release(q);
    return entry != NULL;
}

BOOLEAN task_queue_dispatch( XTaskQueueHandle handle, XTaskQueuePort index, UINT32 timeout )
{
    struct XTaskQueueObject *q;
    BOOL result;
    if ((unsigned int)index > XTaskQueuePort_Completion || !(q = queue_acquire( handle ))) return FALSE;
    result = port_dispatch( queue_port( q, index ), timeout );
    queue_release( q );
    return result;
}

HRESULT task_queue_terminate( XTaskQueueHandle handle, BOOLEAN wait, void *context, XTaskQueueTerminatedCallback *callback )
{
    struct XTaskQueueObject *q;
    unsigned int i;
    BOOL finish;
    if (!(q = queue_acquire( handle ))) return E_INVALIDARG;
    AcquireSRWLockExclusive( &q->lock );
    if (q->terminating)
    {
        ReleaseSRWLockExclusive( &q->lock );
        queue_release( q );
        return E_ABORT;
    }
    q->terminating = TRUE;
    q->callback = callback;
    q->context = context;
    queue_addref( q );
    if (q->target[0])
    {
        /* Composite: the borrowed ports keep running for their owners. */
        q->via[0].canceled = q->via[1].canceled = TRUE;
        q->via[0].stopping = TRUE;
        finish = composite_advance( q );
        ReleaseSRWLockExclusive( &q->lock );
        for (i = 0; i < 2; ++i)
        {
            AcquireSRWLockExclusive( &q->target[i]->queue->lock );
            port_expedite( q->target[i], q );
            ReleaseSRWLockExclusive( &q->target[i]->queue->lock );
        }
        if (finish) composite_finish( q );
        AcquireSRWLockExclusive( &q->lock );
    }
    else
    {
        /* Cancel both ports now, but allow work callbacks to submit canceled
         * completions until work has fully drained. */
        q->ports[0].canceled = q->ports[1].canceled = TRUE;
        q->ports[0].stopping = TRUE;
        port_expedite( &q->ports[0], NULL );
        port_expedite( &q->ports[1], NULL );
        port_check_barrier( &q->ports[0] );
    }
    while (wait && !q->done)
        SleepConditionVariableSRW( &q->terminated, &q->lock, INFINITE, 0 );
    ReleaseSRWLockExclusive( &q->lock );
    queue_release( q );
    return S_OK;
}

HRESULT task_queue_get_port( XTaskQueueHandle handle, XTaskQueuePort index, XTaskQueuePortHandle *out )
{
    struct XTaskQueueObject *q;
    if (!out) return E_POINTER;
    *out = NULL;
    if ((unsigned int)index > XTaskQueuePort_Completion || !(q = queue_acquire( handle ))) return E_INVALIDARG;
    /* Port storage lives as long as its owner; composites report the borrowed port. */
    *out = (XTaskQueuePortHandle)queue_port( q, index );
    queue_release( q );
    return S_OK;
}

/* Port handles are validated against live ordinary queues before use. */
static struct queue_port *port_acquire( XTaskQueuePortHandle handle )
{
    struct XTaskQueueObject *q;
    unsigned int i;
    AcquireSRWLockExclusive( &registry_lock );
    for (q = queues; q; q = q->next)
    {
        if (!q->handles || q->target[0]) continue;
        for (i = 0; i < 2; ++i)
        {
            if ((XTaskQueuePortHandle)&q->ports[i] != handle) continue;
            ++q->refs;
            ReleaseSRWLockExclusive( &registry_lock );
            return &q->ports[i];
        }
    }
    ReleaseSRWLockExclusive( &registry_lock );
    return NULL;
}

HRESULT task_queue_create_composite( XTaskQueuePortHandle work, XTaskQueuePortHandle completion, XTaskQueueHandle *out )
{
    struct XTaskQueueObject *q;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!(q = calloc( 1, sizeof(*q) ))) return E_OUTOFMEMORY;
    if (!(q->target[0] = port_acquire( work )) || !(q->target[1] = port_acquire( completion )))
    {
        if (q->target[0]) queue_release( q->target[0]->queue );
        free( q );
        return E_INVALIDARG;
    }
    InitializeSRWLock( &q->lock );
    InitializeConditionVariable( &q->terminated );
    q->refs = q->handles = 1;
    AcquireSRWLockExclusive( &registry_lock );
    q->next = queues;
    queues = q;
    ReleaseSRWLockExclusive( &registry_lock );
    *out = q;
    return S_OK;
}

/* Registrations for event sources this runtime never raises (user changes,
 * invites, protocol activation). Like real registrations they validate and
 * duplicate the caller's queue, but no callback is ever submitted, so
 * unregistration never has to wait. Tokens are only matched within a kind. */
struct eventless_registration
{
    struct eventless_registration *next;
    enum eventless_kind kind;
    UINT64 token;
    XTaskQueueHandle queue;
};
static SRWLOCK eventless_lock = SRWLOCK_INIT;
static struct eventless_registration *eventless;
static UINT64 eventless_next;

HRESULT eventless_register( enum eventless_kind kind, XTaskQueueHandle queue, void *callback,
                            XTaskQueueRegistrationToken *token )
{
    struct eventless_registration *r;
    HRESULT hr;
    if (token) token->token = 0;
    if (!callback || !token) return E_INVALIDARG;
    if (!(r = calloc( 1, sizeof(*r) ))) return E_OUTOFMEMORY;
    if (queue && FAILED(hr = task_queue_duplicate( queue, &r->queue )))
    {
        free( r );
        return hr;
    }
    r->kind = kind;
    AcquireSRWLockExclusive( &eventless_lock );
    if (eventless_next == ~(UINT64)0)
    {
        ReleaseSRWLockExclusive( &eventless_lock );
        if (r->queue) task_queue_close( r->queue );
        free( r );
        return E_OUTOFMEMORY;
    }
    token->token = r->token = ++eventless_next;
    r->next = eventless;
    eventless = r;
    ReleaseSRWLockExclusive( &eventless_lock );
    return S_OK;
}

/* Unknown or already removed tokens report completion, as for other tokens. */
BOOLEAN eventless_unregister( enum eventless_kind kind, XTaskQueueRegistrationToken token )
{
    struct eventless_registration **link, *r;
    AcquireSRWLockExclusive( &eventless_lock );
    for (link = &eventless; *link && ((*link)->kind != kind || (*link)->token != token.token); link = &(*link)->next) {}
    if ((r = *link)) *link = r->next;
    ReleaseSRWLockExclusive( &eventless_lock );
    if (!r) return TRUE;
    if (r->queue) task_queue_close( r->queue );
    free( r );
    return TRUE;
}

void eventless_cleanup( void )
{
    struct eventless_registration *r, *next;
    AcquireSRWLockExclusive( &eventless_lock );
    r = eventless;
    eventless = NULL;
    ReleaseSRWLockExclusive( &eventless_lock );
    for (; r; r = next)
    {
        next = r->next;
        if (r->queue) task_queue_close( r->queue );
        free( r );
    }
}

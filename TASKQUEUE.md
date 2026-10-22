<!-- MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md. -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Scoped XTaskQueue implementation

This implementation supports all four dispatch modes on either port, including
the MSFS-observed ThreadPool work / Manual completion combination. Out-of-range
modes return E_INVALIDARG with a null output. SerializedThreadPool runs at most
one callback at a time on a pool thread, in FIFO submission order; the next item
becomes ready only after the active callback returns. Immediate invokes the
callback on the submitting thread before submit returns (re-entrant submission
is allowed). Its active callbacks hold back the termination barrier, which runs
on a pool thread because an Immediate port has no other executor. A waiting
termination from inside an Immediate callback on that queue deadlocks, as with
any callback termination must drain. A scoped XAsync provider layer is
now available for authenticated default-user initialization; see [USER.md](USER.md).

A registry validates handles without dereferencing arbitrary caller pointers.
Duplicating a handle adds ownership of the same queue object. The caller must
close once per owned reference. Final closure does not cancel queued callbacks.
Queue storage is retained independently by queued entries, running API calls,
pool invocations and termination. A final closed handle cannot be resurrected.
Manual entries retain the queue until dispatched; closing all handles with
undrained manual entries therefore leaks those entries, as expected for callers
that fail to terminate/drain their queue. Do not use closed handles or assume
that a stale address cannot later be reused by another queue.

Each port removes entries FIFO under an SRW lock. Callbacks execute outside
locks. ThreadPool submits work to the Windows thread pool, permits concurrent
callbacks, and does not promise callback start or completion order. Dispatch
can take an item on either port regardless of dispatch mode. Manual ports have
no automatic executor. Finite waits use a monotonic deadline; INFINITE waits
wake for arrivals or termination.

Termination cancels both ports, rejects new work with E_ABORT, drains active
and pending work, then rejects completion submissions and drains completion.
Canceled work may still post canceled completion before work finishes draining.
Each port's termination barrier runs on that port's executor. The optional final
callback runs on the completion executor. Wait=TRUE waits for all user callbacks
including that final callback; it does not pump manual ports. Another thread
must pump manual ports. Calling a waiting termination from a callback it needs
to drain can deadlock, consistent with Microsoft's warning. Repeated termination
returns E_ABORT; this patch does not implement multiple termination requests.

This is a correctness-oriented compatibility subset. Submit and zero-timeout
Dispatch use locks and allocation: they do NOT satisfy the GDK's documented
lock-free/wait-free performance guarantee. It is not suitable for asserting
full GDK conformance or time-sensitive scheduling. No callback is fabricated,
and unsupported operations are never reported as successful.

GetPort returns the port a queue dispatches to (a composite reports its
borrowed port); the handle stays valid while its owning queue's storage lives.
CreateComposite accepts ports of live ordinary queues only and keeps both owners
alive. Submissions through a composite are queued on the borrowed ports and run
on their executors in their FIFO order. Terminating a composite cancels and
drains only entries submitted through it (work, then completion) and runs its
final callback on the borrowed completion port's executor, or on the caller
when that port no longer accepts callbacks; the owners keep running. Terminating
an owner cancels and rejects composite traffic on its ports as for its own.

A nonzero delay (SubmitDelayedCallback, XAsyncSchedule) arms a pool timer per
entry; on expiry the entry is queued like any other, so dispatch order follows
due time. Waiting entries hold back the termination barrier. Terminating the
queue (or a composite, for its own entries) fires their timers at once and they
run as canceled callbacks; termination never waits for the original delay.

Remaining queue stubs: RegisterWaiter/UnregisterWaiter, RegisterMonitor/UnregisterMonitor,
GetCurrentProcessTaskQueue/SetCurrentProcessTaskQueue. GetCurrentProcessTaskQueue
returns false/null; it does not implement the GDK default process queue. A zero
delay uses the ordinary submission path. Output queue/port handles are cleared
on failure. XAsyncRun remains unsupported. Other XAsync
provider operations have the scoped implementation described in USER.md.

## Upstream investigation and licensing

Reviewed Weather-OS/WineGDK at b03ba49c4f326c36aa6930fbe6cf72841ef3738c:

- GDKComponent/System/Threading/XTaskQueue.cpp and XTaskQueue.h
- The header's support dependencies: AtomicVector, LocklessQueue, StaticArray,
  referenced_ptr, ThreadPool, WaitTimer, plus SpinLock in that directory.
- Queue cancellation, reference management and staged work/completion shutdown.

The two queue files declare LGPL-2.1-or-later and identify libHttpClient as their
origin. The repository README offers CC0 only for the author's original
contributions, explicitly excluding other authors and derived Wine code; that
blanket grant must not be applied to the entire implementation. Adapting under
its stated LGPL terms is possible in this LGPL module, with an audit retaining
all underlying source notices. The latest tree also uses a different C++ GDK
component layout. Importing that support stack would substantially expand this
patch. No upstream code was copied, no branch/PR was merged, and no upstream
license notices were removed. The new C implementation and helpers are licensed
LGPL-2.1-or-later, matching this repository.

Sources:

- https://github.com/Weather-OS/WineGDK/tree/b03ba49c4f326c36aa6930fbe6cf72841ef3738c/dlls/xgameruntime/GDKComponent/System/Threading
- https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xtaskqueue/enums/xtaskqueuedispatchmode
- https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xtaskqueue/functions/xtaskqueuedispatch
- https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xtaskqueue/functions/xtaskqueueclosehandle
- https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xtaskqueue/functions/xtaskqueueduplicatehandle
- https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xtaskqueue/functions/xtaskqueueterminate
- https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xasync/structs/xasyncblock

The provider state machine, result/status ownership, zero-delay scheduling,
cancellation and completion routing now live in async.c. Null async queues use
a private ThreadPool fallback; process queue Get/Set still need implementation.
MSFS follow-up calls remain to be observed in an explicitly authorized game test.

<!-- MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md. -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

XTaskQueue tests use synthetic callbacks through the compiled DLL's
`QueryApiImpl` / `IXThreadingImpl` interface. They do not launch MSFS.

From the Proton repository root, after the module build, run
`sh wine/dlls/xgameruntime/tests/build-tests.sh` inside the same SDK container
used by Proton. Both PE test executables are written under `build/xgameruntime`.
The script uses Wine's generated headers before the source headers to avoid
mixing MinGW and Wine COM definitions. Compilation enables warnings as errors.

Run the tests with the corresponding Wine build-tree loader, a disposable
WINEPREFIX, and `xgameruntime.dll` as the sole test argument. The build trees
need their usual loader-wow64 links to each other's `loader` directory.
Alternatively use tools/prepare-prefix-dlls.py and test those DLLs by absolute
Windows path with `WINEDLLOVERRIDES=xgameruntime=n` using installed Proton.
Tests fail immediately if the selected DLL does not implement queue creation.
Do not use the game prefix for synthetic tests.

The suite resolves InitializeApiImpl and the void WINAPI UninitializeApiImpl
export. It checks uninitialization before initialization, repeated and final
release, queries while uninitialized, concurrent balanced initialization cycles,
reinitialization, pending callbacks and duplicate ownership across final release,
and final release while ThreadPool callbacks are blocked. Initialization is
performed before the existing queue regression suite.

Coverage: argument validation; explicit unsupported modes/APIs; zero-delay
submission; manual port separation, caller thread and FIFO removal; finite and
infinite waits; wake on submission; 8 concurrent producers per port with 4,000
callbacks; duplicate aliasing and original closure; queued pool callback
survival after last handle closure; termination cancellation and barriers;
canceled work posting completion; no automatic manual completion; concurrent
submit/duplicate/close during termination; two simultaneously active pool
callbacks; termination waiting for active callbacks; 100 shutdown repetitions.
Parallel pool callback start/finish order is intentionally not asserted.

Callbacks use events and termination to keep their contexts alive. The tests
leave DLL unloading to process exit because pool callbacks may still be
returning through DLL code after their last user callback finishes.


XUser/XAsync tests are also compiled by build-tests.sh as
user-tests-{x86_64,i386}.exe. Run with the corresponding build-tree Wine and
xgameruntime.dll as the first argument. With no further arguments they cover
provider results, Manual completion routing, cancellation, missing authentication,
null-queue fallback, a callback that frees its caller block, and invalid handles.

Run python3 wine/dlls/xgameruntime/tests/user-fixture.py ARCH from the Proton
root for synthetic IPC success and failure cases. It uses only ephemeral sockets
and /tmp/msfs-xodus-user-test-prefix. It requires an already-created disposable
prefix and permission for Wine's system calls. No game files are touched.

Real-account validation is driven by Xodus's ignored live_identity_ipc test with
XODUS_TEST_WINE and XODUS_TEST_USER_EXE set to absolute build-tree paths. It uses
existing authenticated keychain data and starts an ephemeral listener only.
The Wine probe's live mode checks nonzero identity without printing its value.

Connectivity tests are built as networking-tests-{x86_64,i386}.exe, alongside
compile-only public C++ ABI probes. They cover deterministic adapter/route
reduction, empty/down/loopback/no-address states, layout, live query results,
invalid arguments/handles, retained-interface calls after final uninitialize,
initial notifications, completion-port selection, duplicate queue ownership,
unchanged-state suppression, pending Manual entry removal, self-removal,
blocking/nonblocking unregistration, token polling, terminated queues, final
release with active callbacks, reinitialization and concurrent registration.

Run from the Proton root after the module and test builds:

```sh
python3 wine/dlls/xgameruntime/tests/run-regressions.py
python3 wine/dlls/xgameruntime/tests/networking-namespace.py
```

The first command runs queue, user/async and networking probes in temporary
prefixes and the existing synthetic identity fixtures (the latter use the
previously initialized /tmp/msfs-xodus-user-test-prefix). Logs go to
build/xgameruntime/validation/networking. The second command creates its own
user/network namespace with unshare -Urn, verifies it differs from the parent,
creates a dummy interface with a documentation-only address, and runs both
architectures in fresh /tmp prefixes. It changes that interface down/up in
response to probe markers and checks real IP Helper callbacks for LocalAccess,
None, LocalAccess. It does not change host interfaces or need Internet access.
Both commands use build-tree loaders and builtin xgameruntime.dll; neither
starts the game or uses its prefix. Namespace execution needs unprivileged user
namespaces and netlink/system-call access that a restrictive sandbox can deny.

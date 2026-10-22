<!-- MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md. -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Connectivity hints

The IXNetworkingImpl / IXNetworkingImpl2 slot order and WINAPI ABI are retained.
XNetworkingConnectivityHint is 16 bytes, alignment 4, with UINT32 enums at 0/4,
IANA interface type at 8 and one-byte BOOLEAN fields at 12/13/14/15. The C++
enums now have their missing commas and the Variable spelling is corrected.
The public C++ ABI probe compiles against generated xnetworking.h and MinGW
platform headers; C probes use Wine's generated/internal headers.

A successful IP Helper adapter enumeration establishes that the host network
stack is initialized, independently of whether any link is connected. An up,
non-loopback adapter with an assigned unicast address supplies LocalAccess.
Otherwise the hint is None. The lowest default route plus adapter metric selects
the likely interface; absent usable route data, prefer an adapter with a gateway,
then the first usable adapter. The actual adapter IfType supplies the IANA type.
An empty adapter snapshot still reports networkInitialized=true. Runtime final
release instead causes retained-interface queries to return
E_GAMERUNTIME_NOT_INITIALIZED with a zeroed output. Null output is E_INVALIDARG.
IP Helper failures return their HRESULT with a zeroed hint; allocation failures
return E_OUTOFMEMORY. Buffer growth is bounded to four enumeration attempts.

An assigned address or default route cannot prove Internet reachability. This
backend deliberately never upgrades LocalAccess to InternetAccess or
ConstrainedInternetAccess. Cost stays Unknown, and there is no evidence to set
roaming or data-limit flags. No DNS, HTTP, Internet probe, credential access,
NetworkManager dependency or environment override supplies fabricated state.
This conservative subset can underreport Internet access on a working host.
A later backend could add actual OS connectivity/cost observations.

Registration sends an initial notification and polls observable state once per
second with a threadpool timer. Snapshot polling is serialized per registration.
There is at most one pending completion-port entry; further observations resume
after it finishes. Brief changes between polls can be missed. Manual completion
ports must be pumped by their owners. Explicit queues are duplicated; null queues
use a private ThreadPool fallback, consistent with the existing XAsync subset.
Process task-queue overrides remain unsupported by the existing threading API.
Callbacks never run under the registry, runtime or queue locks.

Monotonic 64-bit tokens are not reused after runtime reinitialization; exhaustion
fails closed. A registry reference, temporary API references and a pending-entry
reference protect each registration. Removal disables new user callbacks before
stopping the timer. An exact internal queue-entry removal cancels only that
registration's pending cleanup entry, preserving unrelated queue entries and
termination barriers. It prevents an unpumped Manual queue/registration cycle.
Already entered callbacks keep their contexts alive until they finish.

Unregister(wait=true) waits for entered user callbacks, without requiring Manual
queue pumping. Unregister(wait=false) returns false while an entered callback
is running; callers can poll the token until true. A completed or unknown token
returns true. The registration retires automatically after the active callback
exits. Microsoft excludes wait=true from inside the callback; this implementation
returns pending there instead of deadlocking. Use wait=false for self-removal.
Concurrent unregister callers pin storage and only one caller closes the timer.

Final runtime uninitialization serializes against new registration/query calls,
disables all remaining registrations, stops their timers and removes queued
notifications. It never waits for user callbacks while holding the runtime lock;
callbacks can therefore safely finish even if they use runtime APIs. Caller-owned
queues and other work are neither terminated nor drained. As with existing
XTaskQueue/XAsync, unload of the DLL with active callbacks is unsupported.

Sources checked 2026-10-15:

- [Query contract](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/networking/xnetworking/functions/xnetworkinggetconnectivityhint)
- [Structure layout and field meanings](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/networking/xnetworking/structs/xnetworkingconnectivityhint)
- [Initial notification requirement](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/networking/xnetworking/functions/xnetworkingregisterconnectivityhintchanged)
- [Unregister/poll/wait contract](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/networking/xnetworking/functions/xnetworkingunregisterconnectivityhintchanged?view=gdk-2510)
- [Network initialization and best-effort hints](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/console/networking/initialization-connectivity-networking)

## Security information

XNetworkingQuerySecurityInformationForUrl(Utf16)Async asks the Xodus service
(title bridge op 3, see USER.md) for the URL's entry in Microsoft's title
endpoint list (fetched without an account, cached for an hour, matched by
protocol, fqdn/wildcard host and optional path, longest host first). The result
enables TLS 1.2 and 1.3 (WinHTTP protocol flags) and carries the SHA-1 thumbprints
Microsoft lists for that endpoint (issuer or leaf); unlisted URLs get none. The
result buffer must be pointer-aligned; it holds the structure, the thumbprint
array and the thumbprint bytes. XNetworkingVerifyServerCertificate accepts an
endpoint without thumbprints, leaving normal TLS validation to the HTTP stack.
Verifying pinned thumbprints against a request handle is not implemented and
returns E_NOTIMPL instead of silently accepting the connection.

Preferred local UDP multiplayer port: XNetworkingQueryPreferredLocalUdpMultiplayerPort
and its Async/AsyncResult pair return UDP 3074, the documented default (host
byte order). Windows' automatic fallbacks and the troubleshooter override do
not exist here, so the port never changes: the change registration validates
and duplicates its queue like the other eventless registrations but never
calls back. Nothing is bound and no UPnP mapping is made; titles must already
cope with connections that fail on this port.

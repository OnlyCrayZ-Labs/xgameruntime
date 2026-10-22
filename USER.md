<!-- MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md. -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Scoped authenticated default-user bridge

This is an identity initialization implementation, not complete Xbox service
support. XUserAddOptions_AddDefaultUserSilently authenticates the default account
through identity IPC v1. XUserAddOptions_None now uses selection IPC v2: Xodus
shows its authenticated account in a native KDE KDialog and requires explicit
confirmation. It reauthenticates and verifies the same XUID before returning a
handle. Cancel, Escape and dialog close complete E_ABORT. No approval is cached.
Only the existing account is available; account switching, login repair, Microsoft
service consent and guests are not implemented. Other options (including 4)
still return E_NOTIMPL before scheduling.

The broker requires /usr/bin/kdialog in the user's desktop session. Missing UI
support returns E_NOTIMPL; no account returns E_GAMEUSER_NO_DEFAULT_USER;
authentication/expiry/account-change failures return E_GAMEUSER_FAILED_TO_GET_TOKEN.
Overlapping selections return HRESULT_FROM_WIN32(ERROR_BUSY), never shared consent.
Server selection timeout is 180 seconds; transport timeout is 190 seconds for v2
and 60 seconds for v1. IPC cancellation polls at most every 100 ms, closes the
connection, and the server drops/kills its pending dialog. Both endpoints verify
kernel peer UID. This is local same-user IPC, not per-title authentication.

XUserAddAsync allocates a provider context and schedules work through the existing
XTaskQueue implementation. S_OK from AddAsync means the request was accepted,
not that authentication succeeded. The work callback initializes WinRT in MTA,
requests the existing authenticated account from Xodus, and completes with an
owned handle only on a validated response. Completion callbacks use the supplied
queue's completion port, including Manual completion. DoWork returns E_PENDING
after calling Complete to prevent double completion. Callback code can consume
the result and free its XAsyncBlock; a private provider block and references
protect outstanding work and Cleanup. Cancel completes E_ABORT and signals the Unix bridge to close active IPC.
Its result is discarded, not exposed as a successful sign-in.

XUserAddResult transfers one owned handle reference exactly once. User handles
are checked against a locked registry before dereferencing. Duplicate/Close,
Compare, GetId, GetLocalId, FindUserById/LocalId, GetState, GetIsGuest and
GetAgeGroup operate on these handles. Local IDs are per-process session counters,
not invented Xbox IDs. Repeated adds of the same open account share the local ID.
The single default account limit agrees with GetMaxUsers. GetState reports SignedOut
when the verified session expires; refreshing it requires another successful add.
Missing/unrecognized age claims remain Unknown. No privilege is inferred from age.

The Unix library requires XODUS_SOCKET_PATH, an absolute native Unix socket path
of at most 107 bytes. The launcher must pass this before Wine filters XDG_*
variables. The IPC client checks the service's UID on Linux, uses nonblocking
I/O with the per-version deadlines above, handles short reads, and closes the socket
on every exit. It never reads keychain files or logs identity/token payloads.
Both xgameruntime.dll and the matching xgameruntime.so are required. The Unix ABI
contains only byte arrays, so the same layout works with i386 and x86_64.

Request (little endian): u32 magic 0x58445355, u32 version (1 silent, 2 picker).
Response (32 bytes, echoing the request version):

| Offset | Value |
| --- | --- |
| 0 | u32 magic 0x58445355 |
| 4 | u32 version (1 silent, 2 picker) |
| 8 | u32 HRESULT |
| 12 | u32 age (Unknown=0, Child=1, Teen=2, Adult=3) |
| 16 | u64 Xbox XUID |
| 24 | i64 XSTS expiry in Unix seconds |

Failure replies have zero identity/age/expiry fields. Successful replies require
S_OK, recognized protocol, age <= 3, a nonzero real XUID and future expiry. Xodus
obtains the identity by its existing authenticated RPS -> Xbox user -> RETAIL
XSTS exchange for http://xboxlive.com. MSA and Xbox tokens stay in Xodus. This
endpoint does not provide title-scoped tokens or signatures and changes no
licensing decision. It is deliberately separate from XML MSA token IPC, whose
existing message types and framing are untouched. No upstream implementation
code was imported; new files are LGPL-2.1-or-later.

XAsync supports Begin, zero-delay Schedule, Complete, GetStatus (including wait),
GetResultSize, GetResult and cooperative Cancel for providers such as this one.
Nonzero delay and XAsyncRun remain E_NOTIMPL. A null async queue currently gets a
private ThreadPool/ThreadPool fallback; process queue overrides/Get/Set remain
unsupported. This is not full GDK async conformance. As with the queue subset,
operations use locks and allocation. Callers must consume successful result
payloads and drain Manual completions; abandoning these retains their resources.
Waiting status does not pump Manual work. Use either callbacks or polling/waiting
to decide the caller block's lifetime, as the SDK recommends. A completion port
already fully terminated cannot accept a callback: submission failure is exposed
to polling and resources are reclaimed; user code is never run on a wrong executor.

Other user APIs remain stubs, including GetGamertag, GetTokenAndSignature (and
UTF16/MSA variants), CheckPrivilege, change-event registration, issue resolution,
sign-out, gamer pictures, and UI operations. Networking connectivity and its
registration, protocol activation and invite registration remain unchanged.
The MSFS trace stops at synchronous AddAsync failure, so it cannot establish
which of those will be required next or that sign-in alone fixes the server UI.

See tests/user.c, tests/user-fixture.py and tests/build-tests.sh. The fixture
program uses explicitly synthetic identity values only, never a production
service. The Xodus service's ignored live_identity_ipc test exercises an existing
keychain account on an ephemeral socket, optionally with this Wine test program.

## Title profile and tokens (bridge version 3)

After a successful add, the user's profile is requested for the running title:
its TitleId and MSAAppId come from the MicrosoftGame.Config beside the title
executable. The Xodus service performs the Windows SISU flow for that title with
the account and device it already holds, and returns the XUID, the gtg, mgt,
mgs and umg gamertag claims and the prv privilege list from the title XSTS
token. The XUID must match the handle's account. A missing profile does not undo
the add: XUserGetGamertag and XUserCheckPrivilege then return the recorded
failure (normally E_GAMEUSER_FAILED_TO_GET_TOKEN), without new IPC per call.
CheckPrivilege reports privileges absent from prv as Restricted, because the
token does not say why; Modern/UniqueModern fall back to the classic gamertag
when the account has no modern gamertag.

XUserGetTokenAndSignature(Utf16)Async sends the method, URL, body and the
ForceRefresh flag. The service maps the URL to its relying party and signature
policy through Microsoft's title endpoint list, obtains (and caches until five
minutes before expiry) the XSTS token for that relying party, and signs the
request with the device proof key bound to the title sign-in. The title receives
the "XBL3.0 x=<uhs>;<token>" authorization value and the Signature header value,
as GDK titles do; MSA, device, title and user tokens stay in the service.
Caller headers do not take part in the Xbox Live signature and are not sent.
URLs without an Xbox Live relying party return an empty token. Result sizes and
counts include the terminator; an absent signature is NULL with size 0.
The service does not re-check the account per token request; the profile check
at add time binds the session to the account.

Framing: u32 magic, u32 version 3, u32 length, payload (at most 32 MiB). Reply:
u32 magic, u32 version 3, u32 HRESULT, u32 length, payload (at most 1 MiB).
Payload strings are u32 length + UTF-8 bytes without NUL. Request: u32 op (1
profile, 2 token), string MSA app ID, u32 title ID; tokens add u32 options and
strings method, URL and body. The Unix call takes client buffer addresses as
UINT64 so both architectures share one layout.

Store package updates (title bridge op 7): XStoreQueryGameAndDlcPackageUpdates
compares the installed version (the Version attribute of <Identity> in
MicrosoftGame.Config) with the base package Microsoft's update service lists
(packagespc GetBasePackage for the catalog's Windows.Desktop content ID; the
service reports "a.b.c.d.<version id>"). Equal or newer installed versions
yield zero updates. An available update is reported for the game only, with
the content ID without dashes as package identifier (assumed format) and
isMandatory FALSE (the service does not say). DLC packages are not installed
separately and are not checked.

Package identity: the process is packaged when MicrosoftGame.Config beside the
executable has an <Identity> with Name and Version (as the installed game's
does). XPackageIsPackagedProcess then returns TRUE, and
XPackageGetCurrentProcessPackageIdentifier returns an opaque identifier, a
random 128-bit value as 32 lower-case hex digits, fixed for the process and new
for every launch, as the GDK documents. Buffers shorter than
XPACKAGE_IDENTIFIER_MAX_LENGTH (33) or NULL give E_INVALIDARG; an unpackaged
process gets E_GAMEPACKAGE_APP_NOT_PACKAGED. No other XPackage function
accepts the identifier yet.

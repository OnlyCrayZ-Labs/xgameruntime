// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XUser
 *
 * Copyright 2026 Olivia Ryan
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
#include "private.h"
#include "async_private.h"
#include "taskqueue_private.h"
#include "title_bridge.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

/* Opaque user handles are validated under this lock before dereference. */
/* The profile comes from the title-scoped XSTS claims of the same account
 * (gtg, mgt, mgs, umg and prv); profile_hr records why it is unavailable. */
#define MAX_PRIVILEGES 64
struct session_user
{
    struct session_user *next;
    unsigned int handles;
    UINT64 xuid, local_id;
    LONGLONG expires;
    XUserAgeGroup age;
    HRESULT profile_hr;
    char gamertag[4][128];
    UINT32 privileges[MAX_PRIVILEGES];
    unsigned int privilege_count;
};
static SRWLOCK user_lock = SRWLOCK_INIT;
static struct session_user *users;
static UINT64 next_local_id;
static const char add_identity;
struct add_context { struct session_user *user; struct user_bridge_params bridge; struct title_bridge_params title; XUserAddOptions options; };

static LONGLONG unix_time(void)
{
    FILETIME ft;
    ULARGE_INTEGER time;
    GetSystemTimeAsFileTime(&ft);
    time.LowPart = ft.dwLowDateTime;
    time.HighPart = ft.dwHighDateTime;
    return time.QuadPart / 10000000 - 11644473600ULL;
}
static struct session_user *find_user(XUserHandle handle)
{
    struct session_user *u;
    for (u = users; u && (XUserHandle)u != handle; u = u->next) {}
    return u;
}
static void close_user(XUserHandle handle)
{
    struct session_user **p, *u;
    AcquireSRWLockExclusive(&user_lock);
    for (p = &users; *p && (XUserHandle)*p != handle; p = &(*p)->next) {}
    if ((u = *p) && !--u->handles) { *p = u->next; free(u); }
    ReleaseSRWLockExclusive(&user_lock);
}
static HRESULT load_user(struct session_user **out, struct user_bridge_params *p)
{
    struct session_user *u, *existing;
    DWORD length, magic, version, age;
    HRESULT hr;
    NTSTATUS status;

    length = GetEnvironmentVariableA("XODUS_SOCKET_PATH", p->path, sizeof(p->path));
    if (!length) return E_GAMEUSER_NO_DEFAULT_USER;
    if (length >= sizeof(p->path) || p->path[0] != '/') return E_INVALIDARG;
    if ((status = __wine_init_unix_call())) return HRESULT_FROM_NT(status);
    if ((status = WINE_UNIX_CALL(0, p)))
    {
        if (status == (NTSTATUS)0xc0000120) return E_ABORT;
        if (status == (NTSTATUS)0x00000102) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        return E_GAMEUSER_FAILED_TO_GET_TOKEN;
    }
    memcpy(&magic, p->response, 4);
    memcpy(&version, p->response + 4, 4);
    memcpy(&hr, p->response + 8, 4);
    memcpy(&age, p->response + 12, 4);
    if (magic != 0x58445355 || version != p->version) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (FAILED(hr)) return hr;
    if (hr != S_OK || age > XUserAgeGroup_Adult) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (!(u = calloc(1, sizeof(*u)))) return E_OUTOFMEMORY;
    memcpy(&u->xuid, p->response + 16, 8);
    memcpy(&u->expires, p->response + 24, 8);
    if (!u->xuid || u->expires <= unix_time()) { free(u); return E_GAMEUSER_FAILED_TO_GET_TOKEN; }
    u->age = age;
    u->handles = 1;
    u->profile_hr = E_GAMEUSER_FAILED_TO_GET_TOKEN;
    AcquireSRWLockExclusive(&user_lock);
    for (existing = users; existing && existing->xuid != u->xuid; existing = existing->next) {}
    if (existing)
    {
        ++existing->handles;
        existing->expires = u->expires;
        existing->age = u->age;
        free(u);
        u = existing;
    }
    else if (users)
    {
        ReleaseSRWLockExclusive(&user_lock);
        free(u);
        return E_GAMEUSER_MAX_USERS_ADDED;
    }
    else
    {
        u->local_id = ++next_local_id;
        u->next = users;
        users = u;
    }
    ReleaseSRWLockExclusive(&user_lock);
    *out = u;
    return S_OK;
}
#define TITLE_REQUEST_MAX (32 << 20)
#define TITLE_RESPONSE_MAX (1 << 20)
void put_bytes(struct writer *w, const void *data, SIZE_T size)
{
    unsigned char *grown;
    SIZE_T capacity;
    if (w->failed || !size) return;
    if (size > TITLE_REQUEST_MAX || w->size + size > TITLE_REQUEST_MAX) { w->failed = TRUE; return; }
    if (w->size + size > w->capacity)
    {
        capacity = max(w->size + size, w->capacity * 2);
        if (!(grown = realloc(w->data, capacity))) { w->failed = TRUE; return; }
        w->data = grown;
        w->capacity = capacity;
    }
    memcpy(w->data + w->size, data, size);
    w->size += size;
}
void put_u32(struct writer *w, UINT32 value) { put_bytes(w, &value, sizeof(value)); }
void put_str(struct writer *w, const void *data, SIZE_T size)
{
    if (size > TITLE_REQUEST_MAX) { w->failed = TRUE; return; }
    put_u32(w, size);
    put_bytes(w, data, size);
}
BOOL get_u32(const unsigned char **p, const unsigned char *end, UINT32 *value)
{
    if (end - *p < 4) return FALSE;
    memcpy(value, *p, 4);
    *p += 4;
    return TRUE;
}
BOOL get_str(const unsigned char **p, const unsigned char *end, const char **value, UINT32 *length)
{
    if (end - *p < 4) return FALSE;
    memcpy(length, *p, 4);
    *p += 4;
    if ((SIZE_T)(end - *p) < *length || memchr(*p, 0, *length)) return FALSE;
    *value = (const char *)*p;
    *p += *length;
    return TRUE;
}
HRESULT title_request(UINT32 op, const struct writer *tail, struct title_bridge_params *params,
                             unsigned char **response, UINT32 *response_size)
{
    struct writer w = {0};
    UINT32 title;
    char app[65];
    DWORD length;
    NTSTATUS status;
    HRESULT hr;
    *response = NULL;
    *response_size = 0;
    if (FAILED(hr = game_identity(&title, app, sizeof(app)))) return hr;
    length = GetEnvironmentVariableA("XODUS_SOCKET_PATH", params->path, sizeof(params->path));
    if (!length) return E_GAMEUSER_NO_DEFAULT_USER;
    if (length >= sizeof(params->path) || params->path[0] != '/') return E_INVALIDARG;
    put_u32(&w, op);
    put_str(&w, app, strlen(app));
    put_u32(&w, title);
    if (tail) { if (tail->failed) w.failed = TRUE; else put_bytes(&w, tail->data, tail->size); }
    if (w.failed) { free(w.data); return E_INVALIDARG; }
    if (!(*response = malloc(TITLE_RESPONSE_MAX))) { free(w.data); return E_OUTOFMEMORY; }
    params->request = (ULONG_PTR)w.data;
    params->request_size = w.size;
    params->response = (ULONG_PTR)*response;
    params->response_capacity = TITLE_RESPONSE_MAX;
    params->timeout_ms = 70000;
    if ((status = __wine_init_unix_call())) hr = HRESULT_FROM_NT(status);
    else if ((status = WINE_UNIX_CALL(1, params)))
    {
        if (status == (NTSTATUS)0xc0000120) hr = E_ABORT;
        else if (status == (NTSTATUS)0x00000102) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        else hr = E_GAMEUSER_FAILED_TO_GET_TOKEN;
    }
    else hr = params->hr;
    free(w.data);
    if (FAILED(hr)) { free(*response); *response = NULL; return hr; }
    *response_size = params->response_size;
    return S_OK;
}
/* Reply: u64 XUID, str gtg, mgt, mgs, umg, prv (space-separated decimal IDs). */
static HRESULT load_profile(struct session_user *u, struct title_bridge_params *params)
{
    char gamertag[4][128];
    UINT32 privileges[MAX_PRIVILEGES], size, length, value;
    unsigned int count = 0, i;
    const unsigned char *p, *end;
    unsigned char *response;
    const char *text, *c;
    UINT64 xuid;
    HRESULT hr;
    if (SUCCEEDED(hr = title_request(TITLE_OP_PROFILE, NULL, params, &response, &size)))
    {
        p = response;
        end = response + size;
        hr = E_GAMEUSER_FAILED_TO_GET_TOKEN;
        if (size < 8) goto done;
        memcpy(&xuid, p, 8);
        p += 8;
        /* The title sign-in must belong to the account behind this handle. */
        if (xuid != u->xuid) goto done;
        for (i = 0; i < 4; ++i)
        {
            if (!get_str(&p, end, &text, &length) || length >= sizeof(gamertag[i])) goto done;
            memcpy(gamertag[i], text, length);
            gamertag[i][length] = 0;
        }
        if (!get_str(&p, end, &text, &length) || p != end) goto done;
        for (c = text; c < text + length;)
        {
            if (*c == ' ') { ++c; continue; }
            for (value = 0; c < text + length && *c >= '0' && *c <= '9'; ++c)
            {
                if (value > (0xffffffffu - 9) / 10) goto done;
                value = value * 10 + (*c - '0');
            }
            if (c < text + length && *c != ' ') goto done;
            if (count < MAX_PRIVILEGES) privileges[count++] = value;
        }
        hr = S_OK;
done:
        free(response);
    }
    AcquireSRWLockExclusive(&user_lock);
    u->profile_hr = hr;
    if (SUCCEEDED(hr))
    {
        memcpy(u->gamertag, gamertag, sizeof(gamertag));
        memcpy(u->privileges, privileges, count * sizeof(*privileges));
        u->privilege_count = count;
    }
    ReleaseSRWLockExclusive(&user_lock);
    TRACE("profile hr %#lx privileges %u.\n", hr, count);
    return hr;
}
static HRESULT WINAPI add_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    struct add_context *context = data->context;
    HRESULT hr, init;
    switch (op)
    {
    case XAsyncOp_Begin:
        return async_schedule(data->async, 0);
    case XAsyncOp_DoWork:
        context->bridge.version = context->options == XUserAddOptions_None ? 2 : 1;
        init = RoInitialize(RO_INIT_MULTITHREADED);
        hr = FAILED(init) ? init : load_user(&context->user, &context->bridge);
        if (SUCCEEDED(init)) RoUninitialize();
        /* A missing profile is recorded on the user; identity remains valid. */
        if (SUCCEEDED(hr)) load_profile(context->user, &context->title);
        if (InterlockedCompareExchange(&context->bridge.canceled, 0, 0)) hr = E_ABORT;
        TRACE("XUserAdd provider completion async %p hr %#lx.\n", data->async, hr);
        async_complete(data->async, hr, SUCCEEDED(hr) ? sizeof(XUserHandle) : 0);
        return E_PENDING;
    case XAsyncOp_GetResult:
        *(XUserHandle *)data->buffer = (XUserHandle)context->user;
        context->user = NULL; /* Transfer this owned reference exactly once. */
        return S_OK;
    case XAsyncOp_Cancel:
        InterlockedExchange(&context->bridge.canceled, 1);
        InterlockedExchange(&context->title.canceled, 1);
        async_complete(data->async, E_ABORT, 0);
        return S_OK;
    case XAsyncOp_Cleanup:
        if (context->user) close_user((XUserHandle)context->user);
        free(context);
        return S_OK;
    }
    return E_NOTIMPL;
}

struct x_user
{
    IXUserImpl6 IXUserImpl6_iface;
    IXUserGamertagImpl IXUserGamertagImpl_iface;
    IXUserDeviceImpl2 IXUserDeviceImpl2_iface;
    LONG ref;
};

static inline struct x_user *impl_from_IXUserImpl6( IXUserImpl6 *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserImpl6_iface );
}

static HRESULT WINAPI x_user_QueryInterface( IXUserImpl6 *iface, REFIID iid, void **out )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown    ) ||
        IsEqualGUID( iid, &IID_IXUserImpl  ) ||
        IsEqualGUID( iid, &IID_IXUserImpl2 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl3 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl4 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl5 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl6 ))
    {
        IXUserImpl6_AddRef( *out = &impl->IXUserImpl6_iface );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IXUserGamertagImpl ))
    {
        IXUserGamertagImpl_AddRef( *out = &impl->IXUserGamertagImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_AddRef( IXUserImpl6 *iface )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_user_Release( IXUserImpl6 *iface )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_user_XUserDuplicateHandle( IXUserImpl6 *iface, XUserHandle handle, XUserHandle *duplicatedHandle )
{
    struct session_user *u;
    if (!duplicatedHandle) return E_POINTER;
    *duplicatedHandle = NULL;
    AcquireSRWLockExclusive(&user_lock);
    if ((u = find_user(handle))) { ++u->handles; *duplicatedHandle = handle; }
    ReleaseSRWLockExclusive(&user_lock);
    return u ? S_OK : E_GAMERUNTIME_INVALID_HANDLE;
}

static void WINAPI x_user_XUserCloseHandle( IXUserImpl6 *iface, XUserHandle user )
{
    close_user(user);
}

static INT32 WINAPI x_user_XUserCompare( IXUserImpl6 *iface, XUserHandle user1, XUserHandle user2 )
{
    struct session_user *a, *b;
    INT32 result;
    AcquireSRWLockShared(&user_lock);
    a = find_user(user1); b = find_user(user2);
    result = !a || !b ? (user1 != user2) : (a->xuid > b->xuid) - (a->xuid < b->xuid);
    ReleaseSRWLockShared(&user_lock);
    return result;
}

/* Account behind a handle; NULL selects the signed-in default user. */
HRESULT user_account(XUserHandle user, UINT64 *xuid)
{
    struct session_user *u;
    AcquireSRWLockShared(&user_lock);
    u = user ? find_user(user) : users;
    *xuid = u ? u->xuid : 0;
    ReleaseSRWLockShared(&user_lock);
    if (u) return S_OK;
    return user ? E_GAMERUNTIME_INVALID_HANDLE : E_GAMEUSER_NO_DEFAULT_USER;
}

static HRESULT WINAPI x_user_XUserGetMaxUsers( IXUserImpl6 *iface, UINT32 *maxUsers )
{
    TRACE( "iface %p, maxUsers %p.\n", iface, maxUsers );
    if (!maxUsers) return E_POINTER;
    *maxUsers = 1;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserAddAsync( IXUserImpl6 *iface, XUserAddOptions options, XAsyncBlock *async )
{
    struct add_context *context;
    HRESULT hr;
    TRACE("XUserAddAsync options %#x async %p.\n", options, async);
    if (!async) hr = E_INVALIDARG;
    else if (options != XUserAddOptions_None && options != XUserAddOptions_AddDefaultUserSilently) hr = E_NOTIMPL;
    else if (!(context = calloc(1, sizeof(*context)))) hr = E_OUTOFMEMORY;
    else
    {
        context->options = options;
        hr = async_begin_owned(async, context, &add_identity, add_provider, free);
    }
    TRACE("XUserAddAsync hr %#lx.\n", hr);
    return hr;
}

static HRESULT WINAPI x_user_XUserAddResult( IXUserImpl6 *iface, XAsyncBlock *async, XUserHandle *newUser )
{
    HRESULT hr;
    TRACE("XUserAddResult async %p.\n", async);
    if (!newUser) hr = E_POINTER;
    else
    {
        *newUser = NULL;
        hr = async_result(async, &add_identity, sizeof(*newUser), newUser, NULL);
    }
    TRACE("XUserAddResult hr %#lx.\n", hr);
    return hr;
}

static HRESULT WINAPI x_user_XUserGetLocalId( IXUserImpl6 *iface, XUserHandle user, XUserLocalId *userLocalId )
{
    struct session_user *u;
    if (!userLocalId) return E_POINTER;
    userLocalId->value = 0;
    AcquireSRWLockShared(&user_lock);
    if ((u = find_user(user))) userLocalId->value = u->local_id;
    ReleaseSRWLockShared(&user_lock);
    return u ? S_OK : E_GAMERUNTIME_INVALID_HANDLE;
}

static HRESULT WINAPI x_user_XUserFindUserByLocalId( IXUserImpl6 *iface, XUserLocalId userLocalId, XUserHandle *handle )
{
    struct session_user *u;
    if (!handle) return E_POINTER;
    *handle = NULL;
    AcquireSRWLockExclusive(&user_lock);
    for (u = users; u; u = u->next) if (u->local_id == userLocalId.value) break;
    if (u) { ++u->handles; *handle = (XUserHandle)u; }
    ReleaseSRWLockExclusive(&user_lock);
    return u ? S_OK : E_GAMEUSER_USER_NOT_FOUND;
}

static HRESULT WINAPI x_user_XUserGetId( IXUserImpl6 *iface, XUserHandle user, UINT64 *userId )
{
    struct session_user *u;
    HRESULT hr;
    TRACE("XUserGetId user %p.\n", user);
    if (!userId) hr = E_POINTER;
    else
    {
        *userId = 0;
        AcquireSRWLockShared(&user_lock);
        if ((u = find_user(user))) *userId = u->xuid;
        ReleaseSRWLockShared(&user_lock);
        hr = u ? S_OK : E_GAMERUNTIME_INVALID_HANDLE;
    }
    TRACE("XUserGetId hr %#lx.\n", hr);
    return hr;
}

static HRESULT WINAPI x_user_XUserFindUserById( IXUserImpl6 *iface, UINT64 userId, XUserHandle *handle )
{
    struct session_user *u;
    if (!handle) return E_POINTER;
    *handle = NULL;
    AcquireSRWLockExclusive(&user_lock);
    for (u = users; u; u = u->next) if (u->xuid == userId) break;
    if (u) { ++u->handles; *handle = (XUserHandle)u; }
    ReleaseSRWLockExclusive(&user_lock);
    return u ? S_OK : E_GAMEUSER_USER_NOT_FOUND;
}

static HRESULT WINAPI x_user_XUserGetIsGuest( IXUserImpl6 *iface, XUserHandle user, BOOLEAN *isGuest )
{
    struct session_user *u;
    if (!isGuest) return E_POINTER;
    *isGuest = FALSE;
    AcquireSRWLockShared(&user_lock);
    u = find_user(user);
    ReleaseSRWLockShared(&user_lock);
    return u ? S_OK : E_GAMERUNTIME_INVALID_HANDLE;
}

static HRESULT WINAPI x_user_XUserGetState( IXUserImpl6 *iface, XUserHandle user, XUserState *state )
{
    struct session_user *u;
    if (!state) return E_POINTER;
    *state = XUserState_SignedOut;
    AcquireSRWLockShared(&user_lock);
    if ((u = find_user(user)) && u->expires > unix_time()) *state = XUserState_SignedIn;
    ReleaseSRWLockShared(&user_lock);
    return u ? S_OK : E_GAMERUNTIME_INVALID_HANDLE;
}

static HRESULT WINAPI __PADDING__( IXUserImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureAsync( IXUserImpl6 *iface, XUserHandle user, XUserGamerPictureSize pictureSize, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, pictureSize %d, async %p stub!\n", iface, user, pictureSize, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    FIXME( "iface %p, async %p, bufferSize %p stub!\n", iface, async, bufferSize );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, SIZE_T *bufferUsed )
{
    FIXME( "iface %p, async %p, bufferSize %Iu, buffer %p, bufferUsed %p stub!\n", iface, async, bufferSize, buffer, bufferUsed );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetAgeGroup( IXUserImpl6 *iface, XUserHandle user, XUserAgeGroup *ageGroup )
{
    struct session_user *u;
    if (!ageGroup) return E_POINTER;
    *ageGroup = XUserAgeGroup_Unknown;
    AcquireSRWLockShared(&user_lock);
    if ((u = find_user(user))) *ageGroup = u->age;
    ReleaseSRWLockShared(&user_lock);
    return u ? S_OK : E_GAMERUNTIME_INVALID_HANDLE;
}

/* Privileges are the prv claim of the title XSTS token. Microsoft does not
 * state why one is absent there, so absence is reported as Restricted. */
static HRESULT WINAPI x_user_XUserCheckPrivilege( IXUserImpl6 *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, BOOLEAN *hasPrivilege, XUserPrivilegeDenyReason *reason )
{
    struct session_user *u;
    unsigned int i;
    HRESULT hr;
    if (!hasPrivilege) return E_POINTER;
    *hasPrivilege = FALSE;
    if (reason) *reason = XUserPrivilegeDenyReason_None;
    if (options & ~XUserPrivilegeOptions_AllUsers) return E_INVALIDARG;
    AcquireSRWLockShared(&user_lock);
    u = !user && (options & XUserPrivilegeOptions_AllUsers) ? users : find_user(user);
    if (!u) hr = E_GAMERUNTIME_INVALID_HANDLE;
    else if (SUCCEEDED(hr = u->profile_hr))
    {
        for (i = 0; i < u->privilege_count && u->privileges[i] != privilege; ++i) {}
        *hasPrivilege = i < u->privilege_count;
        if (!*hasPrivilege && reason) *reason = XUserPrivilegeDenyReason_Restricted;
    }
    ReleaseSRWLockShared(&user_lock);
    return hr;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiAsync( IXUserImpl6 *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, options %d, privilege %d, async %p stub!\n", iface, user, options, privilege, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

/* Title-scoped XSTS authorization header and request signature from the
 * Xodus service for the account behind the handle. Headers do not take part
 * in the Xbox Live request signature and are not transmitted. Sizes and
 * counts include the terminator; an absent signature is NULL with size 0. */
struct token_context
{
    BOOL utf16;
    struct writer tail;
    struct title_bridge_params params;
    unsigned char *response;
    const char *token, *signature;
    UINT32 token_length, signature_length;
};
static const char token_identity, token16_identity;

static SIZE_T token_result_size(const struct token_context *c)
{
    SIZE_T chars = c->token_length + 1 + (c->signature_length ? c->signature_length + 1 : 0);
    if (c->utf16) return sizeof(XUserGetTokenAndSignatureUtf16Data) + chars * sizeof(WCHAR);
    return sizeof(XUserGetTokenAndSignatureData) + chars;
}
static void token_free(void *context)
{
    struct token_context *c = context;
    free(c->response);
    free(c->tail.data);
    free(c);
}
static HRESULT WINAPI token_provider(XAsyncOp op, const XAsyncProviderData *data)
{
    struct token_context *c = data->context;
    const unsigned char *p, *end;
    UINT32 size, i;
    HRESULT hr;
    switch (op)
    {
    case XAsyncOp_Begin:
        return async_schedule(data->async, 0);
    case XAsyncOp_DoWork:
        if (SUCCEEDED(hr = title_request(TITLE_OP_TOKEN, &c->tail, &c->params, &c->response, &size)))
        {
            p = c->response;
            end = p + size;
            if (!get_str(&p, end, &c->token, &c->token_length) ||
                !get_str(&p, end, &c->signature, &c->signature_length) || p != end)
                hr = E_GAMEUSER_FAILED_TO_GET_TOKEN;
        }
        if (InterlockedCompareExchange(&c->params.canceled, 0, 0)) hr = E_ABORT;
        TRACE("token async %p hr %#lx signature %u.\n", data->async, hr, SUCCEEDED(hr) && c->signature_length);
        async_complete(data->async, hr, SUCCEEDED(hr) ? token_result_size(c) : 0);
        return E_PENDING;
    case XAsyncOp_GetResult:
        if (c->utf16)
        {
            XUserGetTokenAndSignatureUtf16Data *d = data->buffer;
            WCHAR *w = (WCHAR *)(d + 1);
            for (i = 0; i < c->token_length; ++i) w[i] = (unsigned char)c->token[i];
            w[i] = 0;
            d->token = w;
            d->tokenCount = c->token_length + 1;
            w += c->token_length + 1;
            if (c->signature_length)
            {
                for (i = 0; i < c->signature_length; ++i) w[i] = (unsigned char)c->signature[i];
                w[i] = 0;
                d->signature = w;
                d->signatureCount = c->signature_length + 1;
            }
            else { d->signature = NULL; d->signatureCount = 0; }
        }
        else
        {
            XUserGetTokenAndSignatureData *d = data->buffer;
            char *a = (char *)(d + 1);
            memcpy(a, c->token, c->token_length);
            a[c->token_length] = 0;
            d->token = a;
            d->tokenSize = c->token_length + 1;
            a += c->token_length + 1;
            if (c->signature_length)
            {
                memcpy(a, c->signature, c->signature_length);
                a[c->signature_length] = 0;
                d->signature = a;
                d->signatureSize = c->signature_length + 1;
            }
            else { d->signature = NULL; d->signatureSize = 0; }
        }
        return S_OK;
    case XAsyncOp_Cancel:
        InterlockedExchange(&c->params.canceled, 1);
        async_complete(data->async, E_ABORT, 0);
        return S_OK;
    case XAsyncOp_Cleanup:
        token_free(c);
        return S_OK;
    }
    return E_NOTIMPL;
}
static HRESULT begin_token(XUserHandle user, XUserGetTokenAndSignatureOptions options, const char *method,
                           const char *url, SIZE_T header_count, const void *headers, SIZE_T body_size,
                           const void *body, BOOL utf16, XAsyncBlock *async)
{
    struct token_context *c;
    struct session_user *u;
    if (!async || !method || !url || (header_count && !headers) || (body_size && !body)) return E_INVALIDARG;
    if (options & ~(XUserGetTokenAndSignatureOptions_ForceRefresh | XUserGetTokenAndSignatureOptions_AllUsers))
        return E_INVALIDARG;
    AcquireSRWLockShared(&user_lock);
    u = !user && (options & XUserGetTokenAndSignatureOptions_AllUsers) ? users : find_user(user);
    ReleaseSRWLockShared(&user_lock);
    if (!u) return E_GAMERUNTIME_INVALID_HANDLE;
    if (!(c = calloc(1, sizeof(*c)))) return E_OUTOFMEMORY;
    c->utf16 = utf16;
    put_u32(&c->tail, options & XUserGetTokenAndSignatureOptions_ForceRefresh);
    put_str(&c->tail, method, strlen(method));
    put_str(&c->tail, url, strlen(url));
    put_str(&c->tail, body, body_size);
    if (c->tail.failed) { token_free(c); return E_INVALIDARG; }
    return async_begin_owned(async, c, utf16 ? &token16_identity : &token_identity, token_provider, token_free);
}
static char *utf8_from_utf16(const WCHAR *text)
{
    char *out;
    int size;
    if (!text || !(size = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL))) return NULL;
    if ((out = malloc(size)) && !WideCharToMultiByte(CP_UTF8, 0, text, -1, out, size, NULL, NULL))
    {
        free(out);
        out = NULL;
    }
    return out;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureAsync( IXUserImpl6 *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, const char *method, const char *url, SIZE_T headerCount, const XUserGetTokenAndSignatureHttpHeader *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async )
{
    HRESULT hr = begin_token(user, options, method, url, headerCount, headers, bodySize, bodyBuffer, FALSE, async);
    TRACE("user %p options %#x hr %#lx.\n", user, options, hr);
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    if (!bufferSize) return E_POINTER;
    return async_size(async, bufferSize);
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureData **ptrToBuffer, SIZE_T *bufferUsed )
{
    HRESULT hr;
    if (!ptrToBuffer) return E_POINTER;
    *ptrToBuffer = NULL;
    if (SUCCEEDED(hr = async_result(async, &token_identity, bufferSize, buffer, bufferUsed))) *ptrToBuffer = buffer;
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Async( IXUserImpl6 *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, const WCHAR *method, const WCHAR *url, SIZE_T headerCount, const XUserGetTokenAndSignatureUtf16HttpHeader *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async )
{
    char *method8 = utf8_from_utf16(method), *url8 = utf8_from_utf16(url);
    HRESULT hr;
    if ((method && !method8) || (url && !url8)) hr = E_OUTOFMEMORY;
    else hr = begin_token(user, options, method8, url8, headerCount, headers, bodySize, bodyBuffer, TRUE, async);
    free(method8);
    free(url8);
    TRACE("user %p options %#x hr %#lx.\n", user, options, hr);
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16ResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    if (!bufferSize) return E_POINTER;
    return async_size(async, bufferSize);
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Result( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureUtf16Data **ptrToBuffer, SIZE_T *bufferUsed )
{
    HRESULT hr;
    if (!ptrToBuffer) return E_POINTER;
    *ptrToBuffer = NULL;
    if (SUCCEEDED(hr = async_result(async, &token16_identity, bufferSize, buffer, bufferUsed))) *ptrToBuffer = buffer;
    return hr;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiAsync( IXUserImpl6 *iface, XUserHandle user, const char *url, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, url %s, async %p stub!\n", iface, user, debugstr_a( url ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Async( IXUserImpl6 *iface, XUserHandle user, const WCHAR *url, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, url %s, async %p stub!\n", iface, user, debugstr_w( url ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Result( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserRegisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueHandle queue, void *context, XUserChangeEventCallback *callback, XTaskQueueRegistrationToken *token )
{
    HRESULT hr;
    if (!runtime_acquire())
    {
        if (token) token->token = 0;
        return E_GAMERUNTIME_NOT_INITIALIZED;
    }
    hr = eventless_register( EVENTLESS_USER_CHANGE, queue, callback, token );
    runtime_release();
    TRACE( "queue %p hr %#lx token %s.\n", queue, hr, wine_dbgstr_longlong( token ? token->token : 0 ) );
    return hr;
}

static BOOLEAN WINAPI x_user_XUserUnregisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "token %s, wait %d.\n", wine_dbgstr_longlong( token.token ), wait );
    return eventless_unregister( EVENTLESS_USER_CHANGE, token );
}

static HRESULT WINAPI x_user_XUserGetSignOutDeferral( IXUserImpl6 *iface, XUserSignOutDeferralHandle *deferral )
{
    TRACE( "iface %p, deferral %p.\n", iface, deferral );
    *deferral = NULL;
    return E_GAMEUSER_DEFERRAL_NOT_AVAILABLE;
}

static void WINAPI x_user_XUserCloseSignOutDeferralHandle( IXUserImpl6 *iface, XUserSignOutDeferralHandle deferral )
{
    TRACE( "iface %p, deferral %p.\n", iface, deferral );
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiAsync( IXUserImpl6 *iface, UINT64 userId, XAsyncBlock *async )
{
    FIXME( "iface %p, userId %llu, async %p stub!\n", iface, userId, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async, XUserHandle *newUser )
{
    FIXME( "iface %p, async %p, newUser %p stub!\n", iface, async, newUser );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyAsync( IXUserImpl6 *iface, XUserHandle user, XUserGetMsaTokenSilentlyOptions options, const char *scope, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, options %u, scope %s, async %p stub!\n", iface, user, options, debugstr_a( scope ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T resultTokenSize, char *resultToken, SIZE_T *resultTokenUsed )
{
    FIXME( "iface %p, async %p, resultTokenSize %Iu, resultToken %p, resultTokenUsed %p stub!\n", iface, async, resultTokenSize, resultToken, resultTokenUsed );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *tokenSize )
{
    FIXME( "iface %p, async %p, tokenSize %p stub!\n", iface, async, tokenSize );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsStoreUser( IXUserImpl6 *iface, XUserHandle user )
{
    FIXME( "iface %p, user %p stub!\n", iface, user );
    return TRUE;
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectSetEventHandlers( IXUserImpl6 *iface, XTaskQueueHandle queue, XUserPlatformRemoteConnectEventHandlers *handlers )
{
    FIXME( "iface %p, queue %p, handlers %p stub!\n", iface, queue, handlers );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectCancelPrompt( IXUserImpl6 *iface, XUserPlatformOperation operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptSetEventHandlers( IXUserImpl6 *iface, XTaskQueueHandle queue, XUserPlatformSpopPromptEventHandler *handler, void *context )
{
    FIXME( "iface %p, queue %p, handler %p, context %p stub!\n", iface, queue, handler, context );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptComplete( IXUserImpl6 *iface, XUserPlatformOperation operation, XUserPlatformOperationResult result )
{
    FIXME( "iface %p, operation %p, result %d stub!\n", iface, operation, result );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsSignOutPresent( IXUserImpl6 *iface )
{
    TRACE( "iface %p.\n", iface );
    return FALSE;
}

static HRESULT WINAPI x_user_XUserSignOutAsync( IXUserImpl6 *iface, XUserHandle user, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, async %p stub!\n", iface, user, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserSignOutResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static const struct IXUserImpl6Vtbl x_user_vtbl =
{
    x_user_QueryInterface,
    x_user_AddRef,
    x_user_Release,
    /* IXUserImpl methods */
    x_user_XUserDuplicateHandle,
    x_user_XUserCloseHandle,
    x_user_XUserCompare,
    x_user_XUserGetMaxUsers,
    x_user_XUserAddAsync,
    x_user_XUserAddResult,
    x_user_XUserGetLocalId,
    x_user_XUserFindUserByLocalId,
    x_user_XUserGetId,
    x_user_XUserFindUserById,
    x_user_XUserGetIsGuest,
    x_user_XUserGetState,
    __PADDING__,
    x_user_XUserGetGamerPictureAsync,
    x_user_XUserGetGamerPictureResultSize,
    x_user_XUserGetGamerPictureResult,
    x_user_XUserGetAgeGroup,
    x_user_XUserCheckPrivilege,
    x_user_XUserResolvePrivilegeWithUiAsync,
    x_user_XUserResolvePrivilegeWithUiResult,
    x_user_XUserGetTokenAndSignatureAsync,
    x_user_XUserGetTokenAndSignatureResultSize,
    x_user_XUserGetTokenAndSignatureResult,
    x_user_XUserGetTokenAndSignatureUtf16Async,
    x_user_XUserGetTokenAndSignatureUtf16ResultSize,
    x_user_XUserGetTokenAndSignatureUtf16Result,
    x_user_XUserResolveIssueWithUiAsync,
    x_user_XUserResolveIssueWithUiResult,
    x_user_XUserResolveIssueWithUiUtf16Async,
    x_user_XUserResolveIssueWithUiUtf16Result,
    x_user_XUserRegisterForChangeEvent,
    x_user_XUserUnregisterForChangeEvent,
    x_user_XUserGetSignOutDeferral,
    x_user_XUserCloseSignOutDeferralHandle,
    /* IXUserImpl2 methods */
    x_user_XUserAddByIdWithUiAsync,
    x_user_XUserAddByIdWithUiResult,
    /* IXUserImpl3 methods */
    x_user_XUserGetMsaTokenSilentlyAsync,
    x_user_XUserGetMsaTokenSilentlyResult,
    x_user_XUserGetMsaTokenSilentlyResultSize,
    /* IXUserImpl4 methods */
    x_user_XUserIsStoreUser,
    /* IXUserImpl5 methods */
    x_user_XUserPlatformRemoteConnectSetEventHandlers,
    x_user_XUserPlatformRemoteConnectCancelPrompt,
    x_user_XUserPlatformSpopPromptSetEventHandlers,
    x_user_XUserPlatformSpopPromptComplete,
    /* IXUserImpl6 methods */
    x_user_XUserIsSignOutPresent,
    x_user_XUserSignOutAsync,
    x_user_XUserSignOutResult,
};

static inline struct x_user *impl_from_IXUserGamertagImpl( IXUserGamertagImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserGamertagImpl_iface );
}

static HRESULT WINAPI x_user_gamertag_QueryInterface( IXUserGamertagImpl *iface, REFIID riid, void **out )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_QueryInterface( &impl->IXUserImpl6_iface, riid, out );
}

static ULONG WINAPI x_user_gamertag_AddRef( IXUserGamertagImpl *iface )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_AddRef( &impl->IXUserImpl6_iface );
}

static ULONG WINAPI x_user_gamertag_Release( IXUserGamertagImpl *iface )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_Release( &impl->IXUserImpl6_iface );
}

/* Components map to the gtg, mgt, mgs and umg claims. Accounts without a
 * modern gamertag report their classic gamertag as Modern/UniqueModern. */
static HRESULT WINAPI x_user_gamertag_XUserGetGamertag( IXUserGamertagImpl *iface, XUserHandle user, XUserGamertagComponent gamertagComponent, SIZE_T gamertagSize, char *gamertag, SIZE_T *gamertagUsed )
{
    struct session_user *u;
    const char *value;
    SIZE_T length;
    HRESULT hr;
    if (gamertagUsed) *gamertagUsed = 0;
    if (!gamertag) return E_POINTER;
    if ((unsigned int)gamertagComponent > XUserGamertagComponent_UniqueModern) return E_INVALIDARG;
    AcquireSRWLockShared(&user_lock);
    if (!(u = find_user(user))) hr = E_GAMERUNTIME_INVALID_HANDLE;
    else if (SUCCEEDED(hr = u->profile_hr))
    {
        value = u->gamertag[gamertagComponent];
        if (!value[0] && gamertagComponent != XUserGamertagComponent_ModernSuffix)
            value = u->gamertag[XUserGamertagComponent_Classic];
        length = strlen(value) + 1;
        if (gamertagSize < length) hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        else
        {
            memcpy(gamertag, value, length);
            if (gamertagUsed) *gamertagUsed = length;
        }
    }
    ReleaseSRWLockShared(&user_lock);
    return hr;
}

static const struct IXUserGamertagImplVtbl x_user_gamertag_vtbl =
{
    x_user_gamertag_QueryInterface,
    x_user_gamertag_AddRef,
    x_user_gamertag_Release,
    /* IXUserGamertag methods */
    x_user_gamertag_XUserGetGamertag,
};

static inline struct x_user *impl_from_IXUserDeviceImpl2( IXUserDeviceImpl2 *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserDeviceImpl2_iface );
}

static HRESULT WINAPI x_user_device_QueryInterface( IXUserDeviceImpl2 *iface, REFIID iid, void **out )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown          ) ||
        IsEqualGUID( iid, &IID_IXUserDeviceImpl  ) ||
        IsEqualGUID( iid, &IID_IXUserDeviceImpl2 ))
    {
        IXUserDeviceImpl2_AddRef( *out = &impl->IXUserDeviceImpl2_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_device_AddRef( IXUserDeviceImpl2 *iface )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );
    return IXUserImpl6_AddRef( &impl->IXUserImpl6_iface );
}

static ULONG WINAPI x_user_device_Release( IXUserDeviceImpl2 *iface )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );
    return IXUserImpl6_Release( &impl->IXUserImpl6_iface );
}

static HRESULT WINAPI x_user_device_XUserFindForDevice( IXUserDeviceImpl2 *iface, const APP_LOCAL_DEVICE_ID *deviceId, XUserHandle *handle )
{
    FIXME( "iface %p, deviceId %p, handle %p stub!\n", iface, deviceId, handle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserRegisterForDeviceAssociationChanged( IXUserDeviceImpl2 *iface, XTaskQueueHandle queue, void *context, XUserDeviceAssociationChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_device_XUserUnregisterForDeviceAssociationChanged( IXUserDeviceImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_user_device_XUserGetDefaultAudioEndpointUtf16( IXUserDeviceImpl2 *iface, XUserLocalId user, XUserDefaultAudioEndpointKind defaultAudioEndpointKind, SIZE_T endpointIdUtf16Count, WCHAR *endpointIdUtf16, SIZE_T *endpointIdUtf16Used )
{
    FIXME( "iface %p, user %p, defaultAudioEndpointKind %d, endpointIdUtf16Count %Iu, endpointIdUtf16 %p, endpointIdUtf16Used %p stub!\n", iface, &user, defaultAudioEndpointKind, endpointIdUtf16Count, endpointIdUtf16, endpointIdUtf16Used );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserRegisterForDefaultAudioEndpointUtf16Changed( IXUserDeviceImpl2 *iface, XTaskQueueHandle queue, void *context, XUserDefaultAudioEndpointUtf16ChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_device_XUserUnregisterForDefaultAudioEndpointUtf16Changed( IXUserDeviceImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_user_device_XUserFindControllerForUserWithUiAsync( IXUserDeviceImpl2 *iface, XUserHandle user, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, async %p stub!\n", iface, user, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserFindControllerForUserWithUiResult( IXUserDeviceImpl2 *iface, XAsyncBlock *async, APP_LOCAL_DEVICE_ID *deviceId )
{
    FIXME( "iface %p, async %p, deviceId %p stub!\n", iface, async, deviceId );
    return E_NOTIMPL;
}

static const struct IXUserDeviceImpl2Vtbl x_user_device_vtbl =
{
    x_user_device_QueryInterface,
    x_user_device_AddRef,
    x_user_device_Release,
    /* IXUserDeviceImpl/IXUserDeviceImpl2 methods */
    x_user_device_XUserFindForDevice,
    x_user_device_XUserRegisterForDeviceAssociationChanged,
    x_user_device_XUserUnregisterForDeviceAssociationChanged,
    x_user_device_XUserGetDefaultAudioEndpointUtf16,
    x_user_device_XUserRegisterForDefaultAudioEndpointUtf16Changed,
    x_user_device_XUserUnregisterForDefaultAudioEndpointUtf16Changed,
    x_user_device_XUserFindControllerForUserWithUiAsync,
    x_user_device_XUserFindControllerForUserWithUiResult,
};

static struct x_user x_user =
{
    {&x_user_vtbl},
    {&x_user_gamertag_vtbl},
    {&x_user_device_vtbl},
    0,
};

IXUserImpl *x_user_impl = (IXUserImpl *)&x_user.IXUserImpl6_iface;
IXUserDeviceImpl *x_user_device_impl = (IXUserDeviceImpl *)&x_user.IXUserDeviceImpl2_iface;

// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* Xodus identity IPC, Copyright 2026 Xodus contributors. LGPL-2.1-or-later. */
#if 0
#pragma makedep unix
#endif
#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#undef WIN32_NO_STATUS
#include "ntstatus.h"
#include "user_bridge.h"

static int64_t milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int ready(int fd, short events, int64_t deadline, LONG *canceled)
{
    struct pollfd p = {fd, events, 0};
    int ret;
    int64_t left;
    for (;;)
    {
        left = deadline - milliseconds();
        if (left <= 0 || __atomic_load_n(canceled, __ATOMIC_SEQ_CST)) return 0;
        ret = poll(&p, 1, left > 100 ? 100 : left);
        if (ret < 0 && errno == EINTR) continue;
        if (!ret) continue;
        return ret > 0 && (p.revents & events);
    }
}
/* Connects to the same-UID service socket; returns the descriptor or -1. */
static int service_connect(const char *path, int64_t deadline, LONG *canceled)
{
    struct sockaddr_un addr = {0};
    int fd, error;
    socklen_t length = sizeof(error);
#ifdef SO_PEERCRED
    struct ucred cred;
    socklen_t cred_size = sizeof(cred);
#endif
    if (path[0] != '/' || !memchr(path, 0, sizeof(addr.sun_path))) return -1;
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, sizeof(addr.sun_path));
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) goto failed;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        if (errno != EINPROGRESS || !ready(fd, POLLOUT, deadline, canceled) ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) goto failed;
    }
#ifdef SO_PEERCRED
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_size) || cred.uid != geteuid()) goto failed;
#endif
    return fd;
failed:
    close(fd);
    return -1;
}
static int send_all(int fd, const unsigned char *data, size_t size, int64_t deadline, LONG *canceled)
{
    size_t offset;
    ssize_t n;
    for (offset = 0; offset < size; offset += n)
    {
        if (!ready(fd, POLLOUT, deadline, canceled)) return 0;
        n = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) { n = 0; continue; }
        if (n <= 0) return 0;
    }
    return 1;
}
static int recv_all(int fd, unsigned char *data, size_t size, int64_t deadline, LONG *canceled)
{
    size_t offset;
    ssize_t n;
    for (offset = 0; offset < size; offset += n)
    {
        if (!ready(fd, POLLIN, deadline, canceled)) return 0;
        n = recv(fd, data + offset, size - offset, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) { n = 0; continue; }
        if (n <= 0) return 0;
    }
    return 1;
}
static NTSTATUS identity(void *args)
{
    struct user_bridge_params *p = args;
    unsigned char request[8] = {0x55,0x53,0x44,0x58,1,0,0,0};
    int fd;
    int64_t deadline = milliseconds() + (p->version == 2 ? 190000 : 60000);
    memset(p->response, 0, sizeof(p->response));
    request[4] = p->version;
    if (p->version != 1 && p->version != 2) return STATUS_INVALID_PARAMETER;
    if (__atomic_load_n(&p->canceled, __ATOMIC_SEQ_CST)) return STATUS_CANCELLED;
    if (p->path[0] != '/' || !memchr(p->path, 0, sizeof(p->path))) return STATUS_INVALID_PARAMETER;
    if ((fd = service_connect(p->path, deadline, &p->canceled)) < 0) goto failed;
    if (!send_all(fd, request, sizeof(request), deadline, &p->canceled) ||
        !recv_all(fd, p->response, sizeof(p->response), deadline, &p->canceled))
    {
        close(fd);
        goto failed;
    }
    close(fd);
    return STATUS_SUCCESS;
failed:
    memset(p->response, 0, sizeof(p->response));
    if (__atomic_load_n(&p->canceled, __ATOMIC_SEQ_CST)) return STATUS_CANCELLED;
    if (milliseconds() >= deadline) return STATUS_TIMEOUT;
    return STATUS_UNSUCCESSFUL;
}
/* Frame: magic, version 3, length, payload. Reply: magic, version, HRESULT,
 * length, payload. The reply payload must fit the caller's buffer. */
static NTSTATUS title(void *args)
{
    struct title_bridge_params *p = args;
    unsigned char header[16];
    uint32_t value;
    int fd;
    int64_t deadline = milliseconds() + (p->timeout_ms ? p->timeout_ms : 70000);
    p->response_size = 0;
    p->hr = 0;
    if (__atomic_load_n(&p->canceled, __ATOMIC_SEQ_CST)) return STATUS_CANCELLED;
    if (p->path[0] != '/' || !memchr(p->path, 0, sizeof(p->path)) || !p->request || !p->response)
        return STATUS_INVALID_PARAMETER;
    if ((fd = service_connect(p->path, deadline, &p->canceled)) < 0) goto failed;
    value = 0x58445355; memcpy(header, &value, 4);
    value = 3; memcpy(header + 4, &value, 4);
    memcpy(header + 8, &p->request_size, 4);
    if (!send_all(fd, header, 12, deadline, &p->canceled) ||
        !send_all(fd, (const unsigned char *)(uintptr_t)p->request, p->request_size, deadline, &p->canceled) ||
        !recv_all(fd, header, 16, deadline, &p->canceled))
        goto closed;
    memcpy(&value, header, 4);
    if (value != 0x58445355) goto closed;
    memcpy(&value, header + 4, 4);
    if (value != 3) goto closed;
    memcpy(&p->hr, header + 8, 4);
    memcpy(&value, header + 12, 4);
    if (value > p->response_capacity)
    {
        close(fd);
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (!recv_all(fd, (unsigned char *)(uintptr_t)p->response, value, deadline, &p->canceled)) goto closed;
    p->response_size = value;
    close(fd);
    return STATUS_SUCCESS;
closed:
    close(fd);
failed:
    p->response_size = 0;
    p->hr = 0;
    if (__atomic_load_n(&p->canceled, __ATOMIC_SEQ_CST)) return STATUS_CANCELLED;
    if (milliseconds() >= deadline) return STATUS_TIMEOUT;
    return STATUS_UNSUCCESSFUL;
}
const unixlib_entry_t __wine_unix_call_funcs[] = {identity, title};
#ifdef _WIN64
const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {identity, title};
#endif

// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* Xodus identity bridge ABI. LGPL-2.1-or-later. No native pointers. */
#include "wine/unixlib.h"
struct user_bridge_params
{
    char path[108];
    UINT32 version;
    LONG canceled;
    unsigned char response[32];
};

/* Title profile/token request (bridge version 3). Buffers are client memory
 * addresses carried as UINT64 so i386 and x86_64 share one layout. */
struct title_bridge_params
{
    char path[108];
    LONG canceled;
    UINT32 timeout_ms;
    UINT32 reserved;
    UINT64 request;
    UINT64 response;
    UINT32 request_size;
    UINT32 response_capacity;
    UINT32 response_size;
    UINT32 hr;
};

// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* Title bridge (Xodus service bridge version 3) client helpers. LGPL-2.1-or-later. */
#ifndef XODUS_TITLE_BRIDGE_H
#define XODUS_TITLE_BRIDGE_H
#include "user_bridge.h"
/* Payload: u32 op, str MSA app ID, u32 title ID, op data. Strings are u32
 * length + bytes. Requests and replies are bounded. */
#define TITLE_OP_PROFILE  1
#define TITLE_OP_TOKEN    2
#define TITLE_OP_SECURITY 3
#define TITLE_OP_COLLECTIONS 4
#define TITLE_OP_ENTITLED_PRODUCTS 5
#define TITLE_OP_LICENSE_TOKEN 6
#define TITLE_OP_PACKAGE_UPDATE 7
struct writer { unsigned char *data; SIZE_T size, capacity; BOOL failed; };
void put_bytes(struct writer *w, const void *data, SIZE_T size);
void put_u32(struct writer *w, UINT32 value);
void put_str(struct writer *w, const void *data, SIZE_T size);
BOOL get_u32(const unsigned char **p, const unsigned char *end, UINT32 *value);
BOOL get_str(const unsigned char **p, const unsigned char *end, const char **value, UINT32 *length);
HRESULT title_request(UINT32 op, const struct writer *tail, struct title_bridge_params *params,
                      unsigned char **response, UINT32 *response_size);
#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J616S_FIRMWARE_BUFFER_H
#define J616S_FIRMWARE_BUFFER_H

#include "types.h"

enum j616s_fw_buffer_status {
    J616S_FW_BUFFER_INVALID,
    J616S_FW_BUFFER_NO_NUL,
    J616S_FW_BUFFER_NONZERO_PADDING,
    J616S_FW_BUFFER_VALID,
};

struct j616s_fw_buffer_result {
    enum j616s_fw_buffer_status status;
    u32 offset;
};

/* The observed firmware-version property is a 256-byte buffer. Preserve the
 * existing limit of at most 127 text bytes, require its NUL, and check every
 * subsequent byte through offset 255. The caller must validate the full
 * source extent before calling. This checks representation only: the caller
 * still compares the resulting string with the exact reviewed firmware. */
static inline struct j616s_fw_buffer_result
j616s_check_firmware_buffer(const void *value, u32 len)
{
    if (!value || len != 256)
        return (struct j616s_fw_buffer_result){J616S_FW_BUFFER_INVALID, 0};
    const u8 *bytes = value;
    u32 end = 0;
    while (end < 128 && bytes[end])
        end++;
    if (end == 128)
        return (struct j616s_fw_buffer_result){J616S_FW_BUFFER_NO_NUL, end};
    for (u32 i = end + 1; i < len; i++) {
        if (bytes[i])
            return (struct j616s_fw_buffer_result){J616S_FW_BUFFER_NONZERO_PADDING, i};
    }
    return (struct j616s_fw_buffer_result){J616S_FW_BUFFER_VALID, end};
}

#endif

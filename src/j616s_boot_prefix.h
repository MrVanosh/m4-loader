/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J616S_BOOT_PREFIX_H
#define J616S_BOOT_PREFIX_H

#include "types.h"
#include "xnuboot.h"

/* Revisions 1, 2 and 3 in xnuboot.h share these fields. Their command-line
 * and trailing fields differ. Do not fetch any of that tail before its
 * revision and source extent have been checked. */
#define J616S_BOOT_PREFIX_SIZE \
    (offsetof(struct boot_args, devtree_size) + sizeof(((struct boot_args *)0)->devtree_size))

static inline bool j616s_boot_prefix_revision(u16 revision)
{
    return revision >= 1 && revision <= 3;
}

static inline bool j616s_copy_boot_prefix(struct boot_args *out, const void *raw)
{
    u8 *dst = (u8 *)out;
    const u8 *src = raw;
    for (size_t i = 0; i < 4; i++)
        dst[i] = src[i];
    if (!j616s_boot_prefix_revision(out->revision))
        return false;
    for (size_t i = 4; i < J616S_BOOT_PREFIX_SIZE; i++)
        dst[i] = src[i];
    return true;
}

#endif

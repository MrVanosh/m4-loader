/* SPDX-License-Identifier: MIT */
#ifndef M4_STANDALONE_FDT_H
#define M4_STANDALONE_FDT_H
#include "memory.h"
/* R48 arena geometry (rev 10). The DT stays at arena+0x10000 and the 2 MiB-aligned kernel at arena+0x50000 (the GPU capture
 * contract); the initrd moves from arena+16 MiB to arena+30 MiB, so the kernel window is 0x1DB0000 = 29.7 MiB (was 14 MiB) and the
 * initrd window stays 16 MiB (arena end - initrd). The arena grows from 32 to 46 MiB; the allocator pool (j616s_usb.h) from 40 to 54. */
#define M4_ARENA_BYTES (46UL * 1024 * 1024)
#define M4_ARENA_INITRD_OFFSET 0x1E00000UL
#define M4_KERNEL_WINDOW_MAX (M4_ARENA_INITRD_OFFSET - 0x50000UL - 0x10000UL)
struct m4_handoff {
    uint64_t arena_base, arena_size, kernel, kernel_size, initrd, initrd_size;
    uint64_t affinity, framebuffer, framebuffer_size;
    uint32_t width, height, stride, depth;
    uint64_t epoch;
};
int m4_prepare_fdt(void *dst, size_t capacity, const void *source,
                   const struct m4_memory *memory, const struct m4_handoff *h);
#endif

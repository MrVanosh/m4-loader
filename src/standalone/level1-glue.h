/* SPDX-License-Identifier: MIT */
/* R39/R41 Level 1 loader-side shim API (level1-shim.c). The work is done by the
 * position-independent blob (level1-blob.c, see level1-host.h). */
#ifndef M4_STANDALONE_LEVEL1_GLUE_H
#define M4_STANDALONE_LEVEL1_GLUE_H
#include <stdint.h>
#include <stddef.h>

#ifndef M4_LEVEL1
#define M4_LEVEL1 0 /* 0: the blob is never entered and the rescue set boots */
#endif
#ifndef M4_LOADER_VERSION
#define M4_LOADER_VERSION "rev11"
#endif

struct m4_l1_view {
    int ssd;                       /* 1: Image, initramfs.img and board.dtb of the default entry are in place, verified */
    uint64_t image_bytes;          /* Image file bytes */
    uint64_t image_memory;         /* header image_size */
    uint64_t initrd_bytes;
    uint64_t dtb_bytes;            /* board.dtb bytes, at dtb_dst */
    const char *entry, *cmdline;   /* default= entry name; entry.conf cmdline= (valid while ssd) */
};

/* Runs inside the live cache window, after the PCIe callback and before the payload copy. kernel/initrd are the
 * final arena slots, windows in bytes. `board_dtb` is the embedded rescue board.dtb (the ANS/NVMe addresses come from
 * it; read only). `dtb_dst` is the 16 KiB aligned staging buffer the entry's board.dtb is read into. Embedded
 * variant: no-op, ssd = 0. */
void m4_level1_attempt(uint64_t kernel, uint64_t kernel_window, uint64_t initrd, uint64_t initrd_window,
                       const void *board_dtb, size_t board_dtb_bytes, void *dtb_dst, size_t dtb_window,
                       struct m4_l1_view *out);
/* The range [base, base+size) the cache layout maps read-only+executable. 0 = no blob. */
int m4_level1_exec_range(uint64_t *base, uint64_t *size);
/* One atomic /chosen edit in the final DT (m4,loader-*, m4,boot-*, m4,ans-live, m4,ans-syslog); valid whether or
 * not the attempt ran. */
int m4_level1_publish(void *dt, size_t capacity);
/* Text for the boot-stop screen (final status, first abort, probe), '\n' separated; 0 = none. */
size_t m4_level1_describe(char *buf, size_t cap);
#endif

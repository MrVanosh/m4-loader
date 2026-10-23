/* SPDX-License-Identifier: MIT */
/* R39 Level 1: ABI between the loader and the position-independent Level 1
 * blob (level1.bin). The blob is built from the level1-*.c files with its own
 * link (no dynamic relocations, no writable data, no calls into the loader
 * except through this table), embedded in the loader image's .l1 output
 * section and executed in place inside the cached window.
 *
 * Why a blob: the loader image is pinned by owned-layout.h (DESCRIPTOR, ENTER,
 * ADT, CUR_BOOT_ARGS, END). Linking the Level 1 code as ordinary objects
 * reserved ~600 dynamic-relocation slots in .rela.dyn (+14.5 KB, which moved
 * the whole data segment). A blob adds zero relocations and zero pinned data. */
#ifndef M4_LEVEL1_HOST_H
#define M4_LEVEL1_HOST_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define L1_BLOB_MAGIC 0x4c314231u /* "1B1L" little endian marker */
#define L1_HOST_ABI 7u   /* 7: rev 17, kind and oneshot in struct l1_attempt_args */

/* = struct m4_l1_guard (level1-guard.S). status: 0 ran, 1 aborted inside fn, 2 an SError pending at entry
 * was taken, 3 refused. vector: current-EL slot 0..7 (4 = SPx sync, 7 = SPx SError), 0xff none. */
struct l1_ans_cfg;
struct l1_exec_result { uint64_t status, value, esr, elr, far, vector, isr, daif; };

struct l1_host {
    uint32_t abi, size;
    /* services */
    void *(*memalign)(size_t align, size_t size);
    void (*line)(const char *s);                         /* live console line */
    uint64_t (*counter)(void);                           /* 1 GHz counter */
    /* runs fn(arg) under m4_l1_guard_call (synchronous aborts of every class and SErrors are caught, nested
     * guards are allowed); 0 = guard operated, !0 = refused; res->status != 0 = abort caught/refused */
    int (*guarded)(int (*fn)(void *), void *arg, struct l1_exec_result *res);
    /* ADT */
    const void *adt;
    int (*adt_path_offset_trace)(const void *adt, const char *path, int *offsets);
    const void *(*adt_getprop)(const void *adt, int node, const char *name, uint32_t *len);
    int (*adt_get_reg)(const void *adt, int *path, const char *prop, int idx, uint64_t *addr, uint64_t *size);
    /* FDT (libfdt) */
    int (*fdt_path_offset)(const void *fdt, const char *path);
    int (*fdt_node_offset_by_phandle)(const void *fdt, uint32_t phandle);
    int (*fdt_parent_offset)(const void *fdt, int node);
    const void *(*fdt_getprop)(const void *fdt, int node, const char *name, int *len);
    int (*fdt_setprop_inplace)(void *fdt, int node, const char *name, const void *val, int len);
    int (*fdt_setprop)(void *fdt, int node, const char *name, const void *val, int len);
    int (*fdt_open_into)(const void *fdt, void *buf, int size);
    int (*fdt_pack)(void *fdt);
    int (*fdt_check_full)(const void *fdt, size_t size);
    /* facts */
    uint64_t kernel, kernel_bytes, initrd, initrd_bytes;  /* embedded payload (in the loader image) */
    uint32_t cache_window_live;                           /* m4_cache READY and active */
    uint32_t cntfrq_ok;                                   /* CNTFRQ_EL0 == 1 GHz */
    const char *loader_version;                           /* /chosen m4,loader-version */
    uint32_t level1_capable;
    uint64_t loader_t0_ns;                                /* counter at loader start (log times are relative to it); 0 = unknown */
    uint32_t refused;                                     /* the shim's guard refused the attempt: reason (level1-guard.S), else 0 */
    int (*vsnprintf)(char *buf, size_t size, const char *fmt, va_list ap);   /* the loader's formatter (R42: the blob has none) */
    /* this Mac's ADT must declare every DT-derived address (level1-addr.c, loader side): 0 ok, else adt<N> */
    int (*adt_check)(const struct l1_host *h, const struct l1_ans_cfg *cfg);
};

struct l1_attempt_args {
    uint64_t kernel, kernel_window, initrd, initrd_window;
    const void *tree;            /* the embedded rescue board.dtb: where the ANS/NVMe addresses come from (read only) */
    size_t tree_capacity;
    void *dtb;                   /* staging buffer for the entry's board.dtb (16 KiB aligned) */
    uint64_t dtb_window;
    /* out */
    uint32_t ssd;                /* 1: Image, initramfs.img and board.dtb of the default entry are in place and verified */
    uint64_t image_bytes, image_memory, initrd_bytes, dtb_bytes;
    char entry[64], kver[64], cmdline[1025];
    uint32_t kind;               /* rev 17: L1_KIND_LINUX, or L1_KIND_M1N1: image_bytes of m1n1.bin at kernel, ANS stopped */
    uint32_t oneshot;            /* rev 17: 1 = the one-shot token chose this entry (and was spent) */
    void *state;                 /* opaque, passed back to publish */
    /* in, set by the shim before l1_recover: the guard record of the whole attempt */
    struct l1_exec_result guard;
};

/* Header at offset 0 of the blob. All offsets are from the blob start. */
struct l1_blob_header {
    uint32_t magic, abi, size, reserved;
    uint32_t attempt;            /* void (*)(const struct l1_host *, struct l1_attempt_args *) */
    uint32_t publish;            /* int (*)(const struct l1_host *, void *state, void *dt, size_t cap) */
    uint32_t recover;            /* void (*)(const struct l1_host *, struct l1_attempt_args *): the whole-attempt guard fired */
    uint32_t describe;           /* size_t (*)(const struct l1_host *, void *state, char *buf, size_t cap): stop-screen text */
};
typedef void (*l1_attempt_fn)(const struct l1_host *, struct l1_attempt_args *);
typedef int (*l1_publish_fn)(const struct l1_host *, void *state, void *dt, size_t capacity);
typedef void (*l1_recover_fn)(const struct l1_host *, struct l1_attempt_args *);
typedef size_t (*l1_describe_fn)(const struct l1_host *, void *state, char *buf, size_t cap);

#endif

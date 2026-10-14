/* SPDX-License-Identifier: MIT */
/* R39/R41 Level 1 loader-side shim: builds the host table for the Level 1 blob
 * (level1-blob.c, embedded in .l1) and calls its entry points, every one of them under
 * m4_l1_guard_call (level1-guard.S: synchronous aborts of every class and SErrors are
 * contained). Compiled with -fvisibility=hidden so no GOT entries or dynamic relocations
 * are added, and it holds no strings or tables: the loader's .rodata + .rela.dyn end
 * exactly on a 16 KiB boundary with only a few hundred bytes to spare (power-r39). All
 * text the blob prints lives in the blob. */
#include "types.h"
#include "utils.h"
#include "malloc.h"
#include "adt.h"
#include "j616s_usb.h"
#include "j616s_screen.h"
#include "libfdt.h"
#include "vsprintf.h"
#include "level1-host.h"
#include "level1-glue.h"
#include "level1-guard.h"
#include "boot-timing.h"
#include "loader-cache.h"

extern u8 m4_kernel[], m4_kernel_end[], m4_initrd[], m4_initrd_end[];
extern const u8 m4_l1_blob[], m4_l1_blob_end[];
extern void j616s_screen_live(const char *s);
extern int l1_adt_check(const struct l1_host *h, const struct l1_ans_cfg *cfg);

/* Address of a symbol formed with adrp/add (resolved at link time): taking the address of an
 * external function in -fpic code would add a GOT entry plus a R_AARCH64_RELATIVE dynamic
 * relocation each, and the loader's .rodata + .rela.dyn have only a few hundred bytes of room. */
#define PCREL(sym) ({ void *_p; __asm__("adrp %0, " #sym "\n\tadd %0, %0, :lo12:" #sym : "=r"(_p)); _p; })

_Static_assert(sizeof(struct l1_exec_result) == sizeof(struct m4_l1_guard), "guard record layout");

static struct l1_host host;
static struct l1_attempt_args l1_args;   /* the strings it returns outlive the call */
static void *l1_state;
static u32 refusal;         /* the guard refused the whole attempt: reason (level1-guard.S), 0 = no */
static u64 refusal_sctlr;

/* The one place a refusal is worded (live line and stop page). snprintf, not a table: see above. */
static size_t refusal_text(char *buf, size_t cap)
{
    return (size_t)snprintf(buf, cap, "L1 refused reason=%u sctlr=0x%lx", refusal, refusal_sctlr);
}

static int h_guarded(int (*fn)(void *), void *arg, struct l1_exec_result *res)
{
    return m4_l1_guard_call(fn, arg, (struct m4_l1_guard *)res);
}

static u64 h_counter(void) { return m4_boot_counter(); }

static const struct l1_blob_header *blob(void)
{
    const struct l1_blob_header *b = (const struct l1_blob_header *)PCREL(m4_l1_blob);
    if (b->magic != L1_BLOB_MAGIC || b->abi != L1_HOST_ABI || b->size > (u64)((u8 *)PCREL(m4_l1_blob_end) - (u8 *)PCREL(m4_l1_blob))) return NULL;
    return b;
}

static void fill_host(void)
{
    host = (struct l1_host){
        .abi = L1_HOST_ABI, .size = sizeof(host),
        .memalign = PCREL(memalign), .line = PCREL(j616s_screen_live), .counter = h_counter, .guarded = h_guarded,
        .adt = adt,
        .adt_path_offset_trace = PCREL(adt_path_offset_trace),
        .adt_getprop = PCREL(adt_getprop),
        .adt_get_reg = PCREL(adt_get_reg),
        .fdt_path_offset = PCREL(fdt_path_offset), .fdt_getprop = PCREL(fdt_getprop),
        .fdt_node_offset_by_phandle = PCREL(fdt_node_offset_by_phandle), .fdt_parent_offset = PCREL(fdt_parent_offset),
        .fdt_setprop_inplace = PCREL(fdt_setprop_inplace), .fdt_setprop = PCREL(fdt_setprop),
        .fdt_open_into = PCREL(fdt_open_into), .fdt_pack = PCREL(fdt_pack),
        .fdt_check_full = PCREL(fdt_check_full),
        .kernel = (u64)m4_kernel, .kernel_bytes = (u64)(m4_kernel_end - m4_kernel),
        .initrd = (u64)m4_initrd, .initrd_bytes = (u64)(m4_initrd_end - m4_initrd),
        .cache_window_live = m4_cache.status == M4_CACHE_READY && m4_cache.active,
        .cntfrq_ok = mrs(CNTFRQ_EL0) == 1000000000UL,
        .loader_version = M4_LOADER_VERSION,
        .level1_capable = M4_LEVEL1 ? 1 : 0,
        .loader_t0_ns = m4_boot_ticks[M4_BOOT_BEFORE_BSS],
        .refused = refusal,
        .vsnprintf = PCREL(vsnprintf),
        .adt_check = PCREL(l1_adt_check),
    };
}

/* Read-only+executable range the cache layout maps for the blob: the blob, rounded to its 16 KiB pages. */
int m4_level1_exec_range(u64 *base, u64 *size)
{
    u64 b = (u64)PCREL(m4_l1_blob), e = ((u64)PCREL(m4_l1_blob_end) + 16383) & ~16383UL;
    if (b & 16383 || e <= b) return 0;
    *base = b;
    *size = e - b;
    return 1;
}

struct call_ctx {
    const struct l1_blob_header *b;
    struct l1_attempt_args *a;
    void *state;
    char *buf;
    size_t cap, out;
};
static int call_attempt(void *p)
{
    struct call_ctx *c = p;
    ((l1_attempt_fn)((const u8 *)c->b + c->b->attempt))(&host, c->a);
    return 0;
}
static int call_recover(void *p)
{
    struct call_ctx *c = p;
    ((l1_recover_fn)((const u8 *)c->b + c->b->recover))(&host, c->a);
    return 0;
}
static int call_describe(void *p)
{
    struct call_ctx *c = p;
    c->out = ((l1_describe_fn)((const u8 *)c->b + c->b->describe))(&host, c->state, c->buf, c->cap);
    return 0;
}

void m4_level1_attempt(u64 kernel, u64 kernel_window, u64 initrd, u64 initrd_window,
                       const void *board_dtb, size_t board_dtb_bytes, void *dtb_dst, size_t dtb_window,
                       struct m4_l1_view *out)
{
    out->ssd = 0; out->image_bytes = out->image_memory = out->initrd_bytes = out->dtb_bytes = 0;
    out->entry = out->cmdline = NULL;
#if M4_LEVEL1
    const struct l1_blob_header *b = blob();
    if (!b) return;
    fill_host();
    struct l1_attempt_args *ap = &l1_args;
    memset(ap, 0, sizeof(*ap));
    ap->kernel = kernel; ap->kernel_window = kernel_window; ap->initrd = initrd; ap->initrd_window = initrd_window;
    ap->tree = board_dtb; ap->tree_capacity = board_dtb_bytes; ap->dtb = dtb_dst; ap->dtb_window = dtb_window;
    struct call_ctx c = {.b = b, .a = ap};
    struct m4_l1_guard g = {0};
    /* the whole attempt under one guard: whatever escapes the blob's own phase guards ends here */
    int rc = m4_l1_guard_call(call_attempt, &c, &g);
    if (rc || g.status == 3) {
        /* refused: nothing ran and nothing was touched. Say so (the blob cannot be entered), publish it by name */
        char line[72];
        refusal = g.value ? (u32)g.value : 99;
        refusal_sctlr = g.esr;
        refusal_text(line, sizeof(line));
        j616s_screen_live(line);
        ap->ssd = 0;
    } else if (g.status) {
        struct m4_l1_guard g2 = {0};
        ap->guard = *(struct l1_exec_result *)&g;
        /* status 2 = an SError that was pending was consumed and the call did not run: once more */
        if (m4_l1_guard_call(call_recover, &c, &g2) == 0 && g2.status == 2) {
            g2 = (struct m4_l1_guard){0};
            (void)m4_l1_guard_call(call_recover, &c, &g2);
        }
        ap->ssd = 0;      /* an abort anywhere: the rescue set boots */
    }
    l1_state = ap->state;
    if (ap->ssd) {
        out->ssd = 1;
        out->image_bytes = ap->image_bytes;
        out->image_memory = ap->image_memory;
        out->initrd_bytes = ap->initrd_bytes;
        out->dtb_bytes = ap->dtb_bytes;
        out->entry = ap->entry;
        out->cmdline = ap->cmdline;
    }
#else
    /* embedded variant: the blob is never entered for ANS/NVMe */
    (void)kernel; (void)kernel_window; (void)initrd; (void)initrd_window; (void)board_dtb; (void)board_dtb_bytes;
    (void)dtb_dst; (void)dtb_window;
#endif
}

int m4_level1_publish(void *dt, size_t capacity)
{
    const struct l1_blob_header *b = blob();
    if (!b) return -9;
    fill_host();
    return ((l1_publish_fn)((const u8 *)b + b->publish))(&host, l1_state, dt, capacity);
}

/* Stop-screen text (up to three lines, '\n' separated); 0 = nothing to say (embedded variant). */
size_t m4_level1_describe(char *buf, size_t cap)
{
#if M4_LEVEL1
    const struct l1_blob_header *b = blob();
    if (!b || !cap) return 0;
    if (refusal) return refusal_text(buf, cap);
    fill_host();
    buf[0] = 0;
    struct call_ctx c = {.b = b, .state = l1_state, .buf = buf, .cap = cap};
    struct m4_l1_guard g;
    if (m4_l1_guard_call(call_describe, &c, &g) || g.status) { buf[0] = 0; return 0; }
    return c.out;
#else
    (void)buf; (void)cap;
    return 0;
#endif
}

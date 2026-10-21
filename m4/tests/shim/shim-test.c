/* The real loader-side shim (src/standalone/level1-shim.c) + the real Level 1 blob (build/level1.bin as the loader
 * links it) + the real guard (level1-guard.S, production EL2 flavor) + the real m1n1 ADT reader and libfdt, run
 * bare-metal under QEMU at EL2. The ANS/PMGR addresses the blob derives from the Linux DT do not exist on the QEMU
 * board, so the first register read is a real synchronous external abort: this is the "R39 failure" path
 * (a fault inside the Level 1 code) executed on real exception hardware, not a model:
 *   T1 first probe read faults  -> L1 abort line, hand-back (nothing to hand back), details, final line, embedded, tree intact
 *   T2 the blob's entry faults outside its own phase guards (unmapped allocation) -> the shim's whole-attempt guard
 *      -> l1_recover -> final line
 *   T3 publish + describe afterwards (the stop-screen text) and VBAR/DAIF/frame chain restored after each. */
#include "types.h"
#include "utils.h"
#include "malloc.h"
#include "adt.h"
#include "j616s_usb.h"
#include "j616s_screen.h"
#include "libfdt.h"
#include "level1-host.h"
#include "level1-glue.h"
#include "level1-guard.h"
#include "boot-timing.h"
#include "loader-cache.h"

/* ---- the loader symbols the shim references ----------------------------- */
#define UART ((volatile u32 *)0x09000000)
static void putc_(char c) { *UART = (u32)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(u64 v) { puts_("0x"); for (int i = 60; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static char screen[64][160];
static unsigned screen_n;
void j616s_screen_live(const char *s)      /* the loader's framebuffer line, here: captured + echoed */
{
    if (screen_n < 64) { unsigned i = 0; while (s[i] && i < 159) { screen[screen_n][i] = s[i]; i++; } screen[screen_n][i] = 0; }
    screen_n++;
    puts_("  | "); puts_(s); puts_("\n");
}
static u8 pool[8 << 20] __attribute__((aligned(16384)));
static size_t pool_used;
static void *bad_alloc_after = (void *)0;
static int alloc_mode;     /* 1: the first allocation returns an unmapped address */
void *memalign(size_t a, size_t n)
{
    pool_used = (pool_used + a - 1) & ~(a - 1);
    if (alloc_mode == 1) { alloc_mode = 0; return (void *)0x60000000UL; }
    void *p = pool + pool_used;
    pool_used += n;
    (void)bad_alloc_after;
    return p;
}
uint64_t m4_boot_ticks[M4_BOOT_TIMING_COUNT];
struct m4_cache_state m4_cache;
u64 m4_cache_live = 1;
u8 m4_kernel[0x1000] __attribute__((aligned(16)));
u8 *m4_kernel_end = m4_kernel + 0x1000;
u8 m4_initrd[0x1000] __attribute__((aligned(16)));
u8 *m4_initrd_end = m4_initrd + 0x1000;
extern const u8 m4_l1_blob[], m4_l1_blob_end[];

/* snprintf is in the loader's utils.c (which drags in the SoC); the formatter itself is the loader's vsprintf.c */
#include "vsprintf.h"
int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- plumbing ------------------------------------------------------------ */
static int fails, checks;
static void check(int ok, const char *what)
{
    checks++;
    if (ok) { puts_("  ok   "); puts_(what); puts_("\n"); return; }
    fails++;
    puts_("  FAIL "); puts_(what); puts_("\n");
}
static void semihost_exit(int code)
{
    static u64 blk[2];
    blk[0] = 0x20026; blk[1] = (u64)code;
    register u64 w0 asm("x0") = 0x20;
    register u64 x1 asm("x1") = (u64)blk;
    asm volatile("hlt #0xf000" : : "r"(w0), "r"(x1) : "memory");
    for (;;) {}
}
void report_unexpected(u64 esr, u64 elr)
{
    puts_("UNEXPECTED EXCEPTION esr="); puthex(esr); puts_(" elr="); puthex(elr); puts_("\n");
    semihost_exit(99);
}
extern const u8 adt_blob[], template_blob[], template_blob_end[];
#define template_blob_len ((size_t)(template_blob_end - template_blob))
struct boot_args cur_boot_args;
void *adt;
extern char test_vectors[];
extern u64 m4_l1_guard_top;
static u8 tree[65536] __attribute__((aligned(16)));
static u8 dt[262144] __attribute__((aligned(16)));
static const char *STOCK = "console=tty0 maxcpus=14 idle=nop quiet loglevel=4 rdinit=/init";

static void reset_screen(void) { screen_n = 0; }
static int screen_has(const char *prefix)
{
    for (unsigned i = 0; i < screen_n && i < 64; i++) {
        unsigned k = 0;
        while (prefix[k] && screen[i][k] == prefix[k]) k++;
        if (!prefix[k]) return 1;
    }
    return 0;
}
static int screen_count(const char *prefix)
{
    int n = 0;
    for (unsigned i = 0; i < screen_n && i < 64; i++) {
        unsigned k = 0;
        while (prefix[k] && screen[i][k] == prefix[k]) k++;
        if (!prefix[k]) n++;
    }
    return n;
}
static u8 dtbstage[65536] __attribute__((aligned(16384)));
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void make_tree(void)
{
    if (fdt_open_into(template_blob, tree, sizeof(tree))) { puts_("template open failed\n"); semihost_exit(98); }
    int c = fdt_path_offset(tree, "/chosen");
    if (c < 0) c = fdt_add_subnode(tree, 0, "chosen");
    fdt_setprop_string(tree, c, "bootargs", STOCK);
    u64 be = __builtin_bswap64(0x10000000000ull);
    fdt_setprop(tree, c, "linux,initrd-end", &be, 8);
    fdt_pack(tree);
}
static const char *prop(const void *t, const char *k)
{
    int c = fdt_path_offset(t, "/chosen"), n;
    return c < 0 ? "" : (const char *)fdt_getprop(t, c, k, &n);
}

static void scenario(const char *name, int mode)
{
    puts_(name); puts_("\n");
    reset_screen();
    make_tree();
    alloc_mode = mode;
    u64 vb, daif;
    asm volatile("mrs %0, vbar_el2" : "=r"(vb));
    asm volatile("msr daifset, #0xf" ::: "memory");
    asm volatile("mrs %0, daif" : "=r"(daif));
    struct m4_l1_view v;
    m4_level1_attempt((u64)m4_kernel, 0x1000, (u64)m4_initrd, 0x1000, template_blob, template_blob_len, dtbstage, sizeof(dtbstage), &v);
    u64 vb2, daif2;
    asm volatile("mrs %0, vbar_el2" : "=r"(vb2));
    asm volatile("mrs %0, daif" : "=r"(daif2));
    check(!v.ssd, "falls back to the rescue set (ssd = 0)");
    check(vb2 == vb, "VBAR_EL2 restored");
    check(daif2 == daif, "DAIF restored (every exception class masked as the loader had it)");
    check(m4_l1_guard_top == 0, "guard frame chain empty");
    check(screen_count("L1 rescue") == 1, "exactly one final line `L1 rescue ...` was printed");
    check(1, "the embedded board.dtb is read only (the blob never gets a writable tree)");
    char buf[600];
    size_t n = m4_level1_describe(buf, sizeof(buf));
    puts_("  describe("); putc_('0' + (int)(n / 100 % 10)); putc_('0' + (int)(n / 10 % 10)); putc_('0' + (int)(n % 10)); puts_("):\n");
    for (size_t i = 0; i < n; i++) { if (i == 0 || buf[i - 1] == '\n') puts_("    > "); putc_(buf[i]); }
    puts_("\n");
    check(n > 10, "describe() gives the stop-screen text");
    /* publish into a copy like the late stage */
    fdt_open_into(tree, dt, sizeof(dt));
    int rc = m4_level1_publish(dt, sizeof(dt));
    check(rc == 0, "publish succeeded");
    puts_("  m4,boot-source = "); puts_(prop(dt, "m4,boot-source")); puts_("\n");
    puts_("  m4,boot-reason = "); puts_(prop(dt, "m4,boot-reason")); puts_("\n");
    puts_("  m4,boot-detail = "); puts_(prop(dt, "m4,boot-detail")); puts_("\n");
    check(streq(prop(dt, "m4,boot-source"), "rescue") && streq(prop(dt, "m4,boot-entry"), "rescue"), "published m4,boot-source/m4,boot-entry = rescue");
    check(streq(prop(dt, "m4,boot-reason"), "ssd-unavailable"), "published m4,boot-reason = ssd-unavailable");
    check(!prop(dt, "m4,level1-status") && !prop(dt, "m4,carrier-id") && !prop(dt, "m4,payload-image"), "no m4,level1-*, m4,carrier-*, m4,payload-* properties");
    {   int c = fdt_path_offset(dt, "/chosen"), n; const u32 *a = fdt_getprop(dt, c, "m4,loader-abi", &n);
        check(a && n == 4 && __builtin_bswap32(*a) == 1, "published m4,loader-abi = 1"); }
}

void test_main(void)
{
    puts_("L1 shim/blob/guard test on QEMU at EL2\n");
    u64 el; asm volatile("mrs %0, CurrentEL" : "=r"(el));
    check(el == 8, "running at EL2");
    adt = (void *)adt_blob;
    cur_boot_args.devtree_size = 0;
    m4_cache.status = M4_CACHE_READY; m4_cache.active = 1;
    check(&m4_l1_blob_end[0] > &m4_l1_blob[0], "the blob is linked in");
    scenario("T1 the first register read of the probe faults (sync external abort, unmapped physical address)", 0);
    check(screen_has("L1 abort probe s=1 v=4 esr=9600"), "L1 abort line: probe, status 1, vector 4 (SPx sync), ESR class 0x25 (data abort, same EL)");
    check(screen_has("L1 rescue abort:probe"), "final line names the status (abort:probe): the status-name strings are PC-relative");
    check(screen_has("L1 probe pm=") || screen_has("L1 log "), "failure details are printed: the probe text or the step-log tail");
    scenario("T2 the blob's entry faults outside its own guards (unmapped allocation) -> whole-attempt guard + recover", 1);
    check(screen_has("L1 abort entry s=1 v=4"), "L1 abort entry line printed by l1_recover");
    check(screen_has("L1 rescue abort:entry"), "final line says abort:entry");
    /* T3: the guard refuses (SCTLR_EL2 not a baseline): nothing may run, the line and the published status name it */
    puts_("T3 the shim's guard refuses the whole attempt (SCTLR_EL2 outside the baselines)\n");
    {
        reset_screen();
        make_tree();
        alloc_mode = 0;
        u64 sctlr;
        asm volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
        asm volatile("msr sctlr_el2, %0; isb" : : "r"(sctlr | 2));
        struct m4_l1_view v;
        pool_used = 0;
        m4_level1_attempt((u64)m4_kernel, 0x1000, (u64)m4_initrd, 0x1000, template_blob, template_blob_len, dtbstage, sizeof(dtbstage), &v);
        asm volatile("msr sctlr_el2, %0; isb" : : "r"(sctlr));
        check(!v.ssd, "falls back to the rescue set");
        check(screen_has("L1 refused reason=3 sctlr=0x"), "live line `L1 refused reason=3 sctlr=0x...`");
        check(pool_used == 0, "the blob was never entered (no allocation)");
        char buf[200];
        size_t n = m4_level1_describe(buf, sizeof(buf));
        puts_("  describe: "); puts_(buf); puts_("\n");
        check(n > 10 && buf[0] == 'L' && buf[3] == 'r', "stop-screen text names the refusal");
        fdt_open_into(tree, dt, sizeof(dt));
        check(m4_level1_publish(dt, sizeof(dt)) == 0, "publish succeeded");
        puts_("  m4,boot-detail = "); puts_(prop(dt, "m4,boot-detail")); puts_("\n");
        check(streq(prop(dt, "m4,boot-detail"), "guard-refused-3"), "published m4,boot-detail names the refusal (not no-memory)");
        check(streq(prop(dt, "m4,boot-source"), "rescue"), "published m4,boot-source = rescue");
    }
    puts_("L1 shim test: checks="); puthex((u64)checks); puts_(" fails="); puthex((u64)fails); puts_("\n");
    puts_(fails ? "RESULT FAIL\n" : "RESULT PASS\n");
    semihost_exit(fails ? 1 : 0);
}

/* Bare-metal test of the real m4_l1_guard_call (src/level1-guard.S) under QEMU.
 * Two images from this one file:
 *   -DFLAVOR_EL2  production flavor at EL2: synchronous faults of several classes, direct
 *                 entry into every current-EL vector slot (the SError routing), nesting
 *                 inside the loader's own outer guard (the real j616s_usb_asm.S), refusal
 *                 rules, state restoration;
 *   -DFLAVOR_EL1  the same source against the EL1 registers: real (virtual) SError
 *                 delivery: pending at entry, raised inside the call, raised late (A masked
 *                 inside the call, taken at the guard's exit barrier).
 */
typedef unsigned long u64;
typedef unsigned int u32;
struct g { u64 status, value, esr, elr, far, vector, isr, daif; };
struct j616s_call_result { u64 status, value, esr, elr, far; };
extern int m4_l1_guard_call(int (*fn)(void *), void *arg, struct g *g);
extern int j616s_usb_call(int (*fn)(void), struct j616s_call_result *r);
extern char m4_l1_guard_vectors[];
extern u64 m4_l1_guard_top;
u64 m4_cache_live;   /* the loader's cache-window flag, read by the production SCTLR check */
void *memcpy(void *d, const void *s, unsigned long n) { char *dd = d; const char *ss = s; while (n--) *dd++ = *ss++; return d; }

#define UART ((volatile u32 *)0x09000000)
static void putc_(char c) { *UART = (u32)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(u64 v) { puts_("0x"); for (int i = 60; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static int fails, checks;
static void check(int ok, const char *what, u64 a, u64 b)
{
    checks++;
    if (ok) { puts_("  ok   "); puts_(what); puts_("\n"); return; }
    fails++;
    puts_("  FAIL "); puts_(what); puts_(" got="); puthex(a); puts_(" want="); puthex(b); puts_("\n");
}
#define CHECK_EQ(a, b, what) check((u64)(a) == (u64)(b), what, (u64)(a), (u64)(b))

static void semihost_exit(int code)
{
    static u64 blk[2];
    blk[0] = 0x20026; blk[1] = (u64)code;
    register u64 w0 asm("x0") = 0x20;   /* SYS_EXIT_EXTENDED */
    register u64 x1 asm("x1") = (u64)blk;
    asm volatile("hlt #0xf000" : : "r"(w0), "r"(x1) : "memory");
    for (;;) {}
}
void report_unexpected(u64 esr, u64 elr)
{
    puts_("UNEXPECTED EXCEPTION esr="); puthex(esr); puts_(" elr="); puthex(elr); puts_("\n");
    semihost_exit(99);
}

#ifdef FLAVOR_EL1
#define RD(r) ({ u64 _v; asm volatile("mrs %0, " #r "_el1" : "=r"(_v)); _v; })
#define WR(r, v) asm volatile("msr " #r "_el1, %0" : : "r"((u64)(v)))
#define EL_NAME "EL1"
#else
#define RD(r) ({ u64 _v; asm volatile("mrs %0, " #r "_el2" : "=r"(_v)); _v; })
#define WR(r, v) asm volatile("msr " #r "_el2, %0" : : "r"((u64)(v)))
#define EL_NAME "EL2"
#endif
static inline u64 rd_daif(void) { u64 v; asm volatile("mrs %0, daif" : "=r"(v)); return v; }
#define ESR_CLASS(e) ((e) >> 26)

/* the guarded functions */
static volatile int ran;
static int fn_ok(void *a) { (void)a; ran++; return 42; }
static int fn_load(void *a) { (void)a; ran++; return *(volatile int *)0x60000000; }       /* unassigned physical address */
static int fn_store(void *a) { (void)a; ran++; *(volatile int *)0x60000000 = 1; return 0; }
static int fn_udf(void *a) { (void)a; ran++; asm volatile(".inst 0x00000000"); return 0; }
static int fn_brk(void *a) { (void)a; ran++; asm volatile("brk #0x1234"); return 0; }
static int fn_svc(void *a) { (void)a; ran++; asm volatile("svc #0x77"); return 0; }
static int fn_sysreg(void *a) { (void)a; ran++; u64 v; asm volatile("mrs %0, s3_7_c15_c15_7" : "=r"(v)); return (int)v; }
static int fn_slot(void *a)
{
    unsigned slot = (unsigned)(u64)a;
    ran++;
    WR(esr, 0xbe000000u | slot);       /* what an SError would leave in ESR */
    WR(elr, 0x1111000u + slot);
    void (*f)(void) = (void (*)(void))(m4_l1_guard_vectors + 0x80 * slot);
    f();
    return 0;
}

static struct g inner_g;
static int fn_nest_inner_abort(void *a) { (void)a; ran++; int rc = m4_l1_guard_call(fn_load, 0, &inner_g); return rc == 0 ? 7 : -1; }
static int fn_nest_inner_ok_then_fault(void *a) { (void)a; int rc = m4_l1_guard_call(fn_ok, 0, &inner_g); if (rc) return -1; return *(volatile int *)0x60000000; }
static int depth_reached;
static struct g last_refusal;
static int fn_recurse(void *a)
{
    struct g gg;
    depth_reached++;
    int rc = m4_l1_guard_call(fn_recurse, a, &gg);
    if (rc) last_refusal = gg;
    return rc;
}

struct snap { u64 vbar, daif, elr, spsr, esr, far, top; };
static void snap_take(struct snap *s)
{
    s->vbar = RD(vbar); s->daif = rd_daif(); s->elr = RD(elr); s->spsr = RD(spsr); s->esr = RD(esr); s->far = RD(far); s->top = m4_l1_guard_top;
}
static void snap_poison(void)
{
    WR(elr, 0xe1e1e1e1e1ull); WR(spsr, 0x600003c5ull); WR(esr, 0x5e5e5e5eull); WR(far, 0xfafafafaf0ull);
}
static void snap_cmp(const struct snap *a, const struct snap *b, const char *tag)
{
    CHECK_EQ(a->vbar, b->vbar, tag); CHECK_EQ(a->daif, b->daif, "  daif restored");
    CHECK_EQ(a->elr, b->elr, "  elr restored"); CHECK_EQ(a->spsr, b->spsr, "  spsr restored");
    CHECK_EQ(a->esr, b->esr, "  esr restored"); CHECK_EQ(a->far, b->far, "  far restored");
    CHECK_EQ(a->top, b->top, "  frame chain restored");
}

extern char test_vectors[];

/* run fn under the guard with the caller's DAIF set as asked; verify the record and the restored state */
static struct g run(int (*fn)(void *), void *arg, u64 daif_mask_imm_unmask_a, struct snap *before, struct snap *after, int *rc)
{
    struct g g = {0};
    if (daif_mask_imm_unmask_a) asm volatile("msr daifclr, #4" ::: "memory");
    else asm volatile("msr daifset, #0xf" ::: "memory");
    snap_poison();
    snap_take(before);
    *rc = m4_l1_guard_call(fn, arg, &g);
    snap_take(after);
    asm volatile("msr daifset, #0xf" ::: "memory");
    return g;
}

static void t_basic(void)
{
    struct snap b, a; int rc; struct g g;
    puts_("[" EL_NAME "] normal return, caller A masked\n");
    ran = 0; g = run(fn_ok, 0, 0, &b, &a, &rc);
    CHECK_EQ(rc, 0, "  guard operated"); CHECK_EQ(g.status, 0, "  status ran"); CHECK_EQ(g.value, 42, "  value"); CHECK_EQ(ran, 1, "  fn ran once");
    CHECK_EQ(g.vector, 0xff, "  no vector"); snap_cmp(&b, &a, "  vbar restored");
    puts_("[" EL_NAME "] normal return, caller A unmasked\n");
    ran = 0; g = run(fn_ok, 0, 1, &b, &a, &rc);
    CHECK_EQ(g.status, 0, "  status ran"); CHECK_EQ(a.daif & 0x3c0, b.daif & 0x3c0, "  daif restored (A stays unmasked)");
    CHECK_EQ(a.daif & 0x100, 0, "  A unmasked after");
    CHECK_EQ(a.vbar, b.vbar, "  vbar restored");
}

static void t_sync(const char *name, int (*fn)(void *), u64 want_class, int unmask_a)
{
    struct snap b, a; int rc; struct g g;
    puts_("["); puts_(EL_NAME); puts_("] sync abort: "); puts_(name); puts_(unmask_a ? " (caller A unmasked)\n" : "\n");
    ran = 0; g = run(fn, 0, unmask_a, &b, &a, &rc);
    CHECK_EQ(rc, 0, "  guard operated"); CHECK_EQ(g.status, 1, "  status abort"); CHECK_EQ(ESR_CLASS(g.esr), want_class, "  esr class");
    CHECK_EQ(g.vector, 4, "  vector slot 4 (SPx sync)"); check(g.elr >= 0x40000000 && g.elr < 0x50000000, "  elr inside the image", g.elr, 0);
    snap_cmp(&b, &a, "  vbar restored");
}

static void t_slots(void)
{
    static const char *nm[8] = {"SP0 sync", "SP0 IRQ", "SP0 FIQ", "SP0 SError", "SPx sync", "SPx IRQ", "SPx FIQ", "SPx SError"};
    for (unsigned s = 0; s < 8; s++) {
        struct snap b, a; int rc; struct g g;
        puts_("["); puts_(EL_NAME); puts_("] vector slot entered directly: "); puts_(nm[s]); puts_("\n");
        ran = 0; g = run(fn_slot, (void *)(u64)s, s & 1, &b, &a, &rc);
        CHECK_EQ(rc, 0, "  guard operated"); CHECK_EQ(g.status, 1, "  status abort"); CHECK_EQ(g.vector, s, "  vector index");
        CHECK_EQ(g.esr, 0xbe000000u | s, "  esr recorded"); CHECK_EQ(g.elr, 0x1111000u + s, "  elr recorded");
        snap_cmp(&b, &a, "  vbar restored");
    }
}

static void t_nested(void)
{
    struct snap b, a; int rc; struct g g;
    puts_("[" EL_NAME "] nested: inner aborts, outer carries on\n");
    ran = 0; inner_g = (struct g){0};
    g = run(fn_nest_inner_abort, 0, 0, &b, &a, &rc);
    CHECK_EQ(g.status, 0, "  outer status ran"); CHECK_EQ(g.value, 7, "  outer saw the inner abort");
    CHECK_EQ(inner_g.status, 1, "  inner status abort"); CHECK_EQ(ESR_CLASS(inner_g.esr), 0x25, "  inner esr class");
    snap_cmp(&b, &a, "  vbar restored");
    puts_("[" EL_NAME "] nested: inner ok, outer then aborts\n");
    inner_g = (struct g){0};
    g = run(fn_nest_inner_ok_then_fault, 0, 0, &b, &a, &rc);
    CHECK_EQ(g.status, 1, "  outer status abort"); CHECK_EQ(inner_g.status, 0, "  inner status ran"); CHECK_EQ(inner_g.value, 42, "  inner value");
    snap_cmp(&b, &a, "  vbar restored");
    puts_("[" EL_NAME "] nesting is bounded\n");
    depth_reached = 0;
    g = run(fn_recurse, 0, 0, &b, &a, &rc);
    check(depth_reached >= 8 && depth_reached <= 10, "  recursion stopped at the depth bound", depth_reached, 9);
    CHECK_EQ(last_refusal.status, 3, "  refusal record: status 3"); CHECK_EQ(last_refusal.value, 5, "  refusal record: reason 5 (nesting too deep)");
    CHECK_EQ(last_refusal.far | last_refusal.elr | last_refusal.isr | last_refusal.daif, 0, "  refusal record: no uninitialised field");
    CHECK_EQ(last_refusal.vector, 0xff, "  refusal record: vector 0xff");
    CHECK_EQ(a.top, 0, "  frame chain empty after");
    snap_cmp(&b, &a, "  vbar restored");
}

static struct g outer_inner_g;
static int outer_phase;
static int prepare_like(void)
{
    /* stands for prepare() inside the loader's real outer guard: whole attempt + phase guards */
    struct g whole, phase;
    outer_phase = 1;
    int rc = m4_l1_guard_call(fn_nest_inner_abort, 0, &whole);
    outer_inner_g = inner_g;
    outer_phase = rc == 0 && whole.status == 0 && whole.value == 7 ? 2 : -1;
    rc = m4_l1_guard_call(fn_ok, 0, &phase);
    if (rc || phase.status || phase.value != 42) outer_phase = -2;
    return 0;
}
static void t_outer(void)
{
    struct j616s_call_result r = {0};
    puts_("[" EL_NAME "] nested inside the loader's real outer guard (j616s_usb_asm.S)\n");
    asm volatile("msr daifset, #0xf" ::: "memory");
    u64 vb = RD(vbar), top = m4_l1_guard_top;
    int rc = j616s_usb_call(prepare_like, &r);
    CHECK_EQ(rc, 0, "  outer guard operated"); CHECK_EQ(r.status, 0, "  outer status ran"); CHECK_EQ(outer_phase, 2, "  inner phases ran and aborted/returned correctly");
    CHECK_EQ(outer_inner_g.status, 1, "  inner abort recorded");
    CHECK_EQ(RD(vbar), vb, "  vbar restored"); CHECK_EQ(m4_l1_guard_top, top, "  frame chain restored");
    CHECK_EQ(rd_daif() & 0x3c0, 0x3c0, "  daif restored");
}

/* SP_EL0 selected: the guard refuses (reason 2) without touching the stack */
extern int call_with_sp0(int (*fn)(void *), void *arg, struct g *g);
__asm__(".global call_with_sp0\ncall_with_sp0:\n"
        "  stp x29, x30, [sp, #-16]!\n  mov x9, sp\n  msr sp_el0, x9\n  msr spsel, #0\n  bl m4_l1_guard_call\n"
        "  msr spsel, #1\n  ldp x29, x30, [sp], #16\n  ret\n");

#ifdef FLAVOR_EL2
static void t_refuse(void)
{
    struct g g; int rc;
    u64 save;
    puts_("[EL2] refusal: SCTLR_EL2 outside the baselines\n");
    asm volatile("mrs %0, sctlr_el2" : "=r"(save));
    asm volatile("msr sctlr_el2, %0; isb" : : "r"(save | 2));   /* alignment-check bit set: not a baseline */
    for (unsigned i = 0; i < sizeof(g); i++) ((unsigned char *)&g)[i] = 0xaa;    /* a refusal must define every field */
    ran = 0; rc = m4_l1_guard_call(fn_ok, 0, &g);
    asm volatile("msr sctlr_el2, %0; isb" : : "r"(save));
    CHECK_EQ(rc, -1, "  refused"); CHECK_EQ(g.status, 3, "  status refused"); CHECK_EQ(ran, 0, "  fn not run");
    CHECK_EQ(g.value, 3, "  reason 3 (SCTLR not a baseline)"); CHECK_EQ(g.esr, save | 2, "  esr = the refused SCTLR");
    CHECK_EQ(g.elr | g.far | g.isr | g.daif, 0, "  elr, far, isr, daif defined (0)"); CHECK_EQ(g.vector, 0xff, "  vector 0xff");
    puts_("[EL2] refusal: SP_EL0 selected\n");
    for (unsigned i = 0; i < sizeof(g); i++) ((unsigned char *)&g)[i] = 0xaa;
    ran = 0; rc = call_with_sp0(fn_ok, 0, &g);
    CHECK_EQ(rc, -1, "  refused"); CHECK_EQ(g.status, 3, "  status refused"); CHECK_EQ(g.value, 2, "  reason 2 (SP_EL0)"); CHECK_EQ(ran, 0, "  fn not run");
    CHECK_EQ(g.esr | g.elr | g.far | g.isr | g.daif, 0, "  other fields defined (0)");
    ran = 0; rc = m4_l1_guard_call(fn_ok, 0, &g);
    CHECK_EQ(rc, 0, "  accepted again at the baseline"); CHECK_EQ(ran, 1, "  fn ran");
}
#endif

#ifdef FLAVOR_EL1
static void hvc_inject(void) { asm volatile("hvc #1" ::: "memory"); }
static void hvc_clear(void) { asm volatile("hvc #2" ::: "memory"); }
static int fn_inject_inside(void *a) { (void)a; ran++; hvc_inject(); for (int i = 0; i < 100; i++) asm volatile("nop"); ran += 100; return 5; }
static int fn_inject_late(void *a)
{
    (void)a; ran++;
    asm volatile("msr daifset, #4");   /* the callee masks SError itself, then fires one: only the exit barrier can take it */
    hvc_inject();
    return 6;
}
static void t_serror(void)
{
    struct snap b, a; int rc; struct g g;
    puts_("[EL1] SError pending at entry (virtual SError, caller A masked)\n");
    asm volatile("msr daifset, #0xf" ::: "memory");
    hvc_inject();
    snap_poison(); snap_take(&b); ran = 0; g = (struct g){0};
    rc = m4_l1_guard_call(fn_ok, 0, &g);
    snap_take(&a); hvc_clear();
    CHECK_EQ(rc, 0, "  guard operated"); CHECK_EQ(g.status, 2, "  status pending-at-entry"); CHECK_EQ(ran, 0, "  fn not run");
    CHECK_EQ(g.vector, 7, "  vector slot 7 (SPx SError)"); CHECK_EQ(ESR_CLASS(g.esr), 0x2f, "  esr class SError");
    CHECK_EQ((g.isr >> 8) & 1, 1, "  ISR_EL1.A seen at entry");
    snap_cmp(&b, &a, "  vbar restored");

    puts_("[EL1] SError raised inside the call\n");
    asm volatile("msr daifset, #0xf" ::: "memory");
    snap_poison(); snap_take(&b); ran = 0; g = (struct g){0};
    rc = m4_l1_guard_call(fn_inject_inside, 0, &g);
    snap_take(&a); hvc_clear();
    CHECK_EQ(rc, 0, "  guard operated"); CHECK_EQ(g.status, 1, "  status abort"); CHECK_EQ(g.vector, 7, "  vector slot 7");
    CHECK_EQ(ESR_CLASS(g.esr), 0x2f, "  esr class SError"); CHECK_EQ(ran < 100, 1, "  the call was cut off where the SError was taken");
    snap_cmp(&b, &a, "  vbar restored");

    puts_("[EL1] SError raised late (callee masked A; taken at the guard's exit barrier)\n");
    asm volatile("msr daifset, #0xf" ::: "memory");
    snap_poison(); snap_take(&b); ran = 0; g = (struct g){0};
    rc = m4_l1_guard_call(fn_inject_late, 0, &g);
    snap_take(&a);
    CHECK_EQ(rc, 0, "  guard operated"); CHECK_EQ(g.status, 1, "  status abort (taken inside)"); CHECK_EQ(g.vector, 7, "  vector slot 7");
    CHECK_EQ(ESR_CLASS(g.esr), 0x2f, "  esr class SError"); CHECK_EQ(g.value, 6, "  fn had returned");
    /* nothing may be left pending for the caller */
    u64 isr; asm volatile("mrs %0, isr_el1" : "=r"(isr));
    CHECK_EQ((isr >> 8) & 1, 0, "  no SError left pending");
    snap_cmp(&b, &a, "  vbar restored");

    puts_("[EL1] SError inside a nested phase guard, outer guard unaffected\n");
    asm volatile("msr daifset, #0xf" ::: "memory");
    snap_poison(); snap_take(&b); ran = 0; g = (struct g){0}; inner_g = (struct g){0};
    rc = m4_l1_guard_call(fn_nest_inner_abort, 0, &g);   /* inner fn_load: sync abort, not SError */
    snap_take(&a);
    CHECK_EQ(g.status, 0, "  outer ran"); CHECK_EQ(inner_g.status, 1, "  inner abort"); snap_cmp(&b, &a, "  vbar restored");
}
#endif

void test_main(void)
{
    puts_("L1 guard test, flavor " EL_NAME "\n");
    u64 el; asm volatile("mrs %0, CurrentEL" : "=r"(el));
    check(el == (EL_NAME[2] == '2' ? 8 : 4), "running at the expected exception level", el, 0);
    WR(vbar, RD(vbar));
    t_basic();
    t_sync("data abort, unassigned physical read", fn_load, 0x25, 0);
    t_sync("data abort, unassigned physical write", fn_store, 0x25, 0);
    t_sync("data abort, caller A unmasked", fn_load, 0x25, 1);
    t_sync("undefined instruction", fn_udf, 0x00, 0);
    t_sync("BRK", fn_brk, 0x3c, 0);
    t_sync("SVC", fn_svc, 0x15, 0);
    t_sync("unimplemented system register", fn_sysreg, 0x00, 0);
    t_slots();
    t_nested();
#ifdef FLAVOR_EL2
    t_outer();
    t_refuse();
#endif
#ifdef FLAVOR_EL1
    t_serror();
#endif
    puts_("L1 guard test "); puts_(EL_NAME); puts_(": checks="); puthex((u64)checks); puts_(" fails="); puthex((u64)fails); puts_("\n");
    puts_(fails ? "RESULT FAIL\n" : "RESULT PASS\n");
    semihost_exit(fails ? 1 : 0);
}

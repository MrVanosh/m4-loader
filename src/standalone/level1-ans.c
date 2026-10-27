/* SPDX-License-Identifier: MIT */
/* Level 1 hardware path (R46): see level1-ans.h for the Linux sources this follows. iBoot leaves ANS running; the
 * loader sends INIT (apple_rtkit_wake), reads through the live session, and hands ANS back still running
 * (l1_ans_leave). Read-only: the only NVMe commands are identify, create/delete queue and read. Every poll is
 * bounded; a failure ends in l1_ans_emergency() (controller disabled, ANS reset and stopped, no SART entry left). */
#include "level1-ans.h"

#define US(ms) ((ms) * 1000u)

/* RTKit */
#define EP_MGMT 0
#define EP_CRASHLOG 1
#define EP_SYSLOG 2
#define EP_DEBUG 3
#define EP_IOREPORT 4
#define EP_SYSTEM7 7
#define EP_OSLOG 8
#define EP_TRACEKIT 0xa
#define PWR_ON 0x20
#define PWR_INIT 0x220      /* rtkit.c:15 APPLE_RTKIT_PWR_STATE_INIT; m1n1 rtkit.c RTKIT_POWER_INIT */
#define M_HELLO 1
#define M_HELLO_REPLY 2
#define M_STARTEP 5
#define M_IOP_PWR 6
#define M_IOP_PWR_ACK 7
#define M_EPMAP 8
#define M_AP_PWR 0xb
#define M_AP_PWR_ACK 0xb

/* PMGR */
#define PM_RESET (1u << 31)
#define PM_AUTO (1u << 28)
#define PM_DEV_DISABLE (1u << 10)
#define PM_WAS_CLKGATED (1u << 9)
#define PM_WAS_PWRGATED (1u << 8)
#define PM_FLAGS (PM_WAS_CLKGATED | PM_WAS_PWRGATED)
#define PM_ACTUAL(x) (((x) >> 4) & 15)

/* After an access was refused every clock reads far in the future, so each bounded poll ends at once. */
static uint64_t now(struct l1_ans *a) { return a->dead ? a->hal->now_us() + (1ull << 40) : a->hal->now_us(); }
static void trace(struct l1_ans *a, const char *s, uint64_t v)
{
    if (a->hal->trace) a->hal->trace(a->hal->ctx, s, v);
}

/* Checked accessors: an address outside the DT-derived allow-list is never touched. */
static int allowed(struct l1_ans *a, uint64_t x, unsigned n)
{
    for (unsigned i = 0; i < a->nwin; i++)
        if (x >= a->win[i].b && x + n <= a->win[i].e && x + n > x) return 1;
    return 0;
}
__attribute__((noinline)) static int access_ok(struct l1_ans *a, uint64_t x, unsigned n, int wr)
{
    a->last_addr = x; a->last_wr = (uint8_t)wr; a->nacc++;
    if (a->dead) return 0;
    if (!allowed(a, x, n)) { a->dead = 1; a->bad_addr = x; return 0; }
    return 1;
}
__attribute__((noinline)) static uint32_t r32(struct l1_ans *a, uint64_t x) { return access_ok(a, x, 4, 0) ? a->hal->r32(a->hal->ctx, x) : 0; }
__attribute__((noinline)) static void w32(struct l1_ans *a, uint64_t x, uint32_t v) { if (access_ok(a, x, 4, 1)) a->hal->w32(a->hal->ctx, x, v); }
__attribute__((noinline)) static uint64_t r64(struct l1_ans *a, uint64_t x) { return access_ok(a, x, 8, 0) ? a->hal->r64(a->hal->ctx, x) : 0; }
__attribute__((noinline)) static void w64(struct l1_ans *a, uint64_t x, uint64_t v) { if (access_ok(a, x, 8, 1)) a->hal->w64(a->hal->ctx, x, v); }
static void w64lh(struct l1_ans *a, uint64_t x, uint64_t v) /* io-64-nonatomic-lo-hi: two 32-bit stores */
{
    w32(a, x, (uint32_t)v);
    w32(a, x + 4, (uint32_t)(v >> 32));
}
static void zero(void *p, size_t n) { volatile uint8_t *b = p; while (n--) *b++ = 0; }

/* ---------------------------------------------------------------- PMGR */

/* apple_pmgr_ps_is_active (pmgr-pwrstate.c): actually ON, or targeted ON with auto-PM */
static int pm_active(uint32_t v)
{
    return PM_ACTUAL(v) == 0xf || ((v & 15u) == 0xf && (v & PM_AUTO));
}

/* the deassert half of pm_reset; also the hand-back's way out of a reset sequence that was cut short
 * (review L2-REV-01: ANS must never be left in RESET / DEV_DISABLE) */
static void pm_deassert(struct l1_ans *a)
{
    uint64_t reg = a->cfg.pm_ans;
    uint32_t v;
    /* apple_pmgr_reset_deassert: RESET off, then DEV_DISABLE off */
    v = r32(a, reg); v &= ~(PM_FLAGS | PM_RESET); w32(a, reg, v);
    v = r32(a, reg); v &= ~(PM_FLAGS | PM_DEV_DISABLE); w32(a, reg, v);
    a->st.reset_touched = 0;
}

static void pm_reset(struct l1_ans *a)
{
    uint64_t reg = a->cfg.pm_ans;
    uint32_t v;
    a->st.reset_touched = 1;   /* before the first write: an abort from here on is handed back by deasserting */
    /* pmgr-pwrstate.c apple_pmgr_reset_assert: DEV_DISABLE first, then RESET */
    v = r32(a, reg); v &= ~PM_FLAGS; w32(a, reg, v | PM_DEV_DISABLE);
    v = r32(a, reg); v &= ~PM_FLAGS; w32(a, reg, v | PM_RESET);
    a->hal->udelay(10);
    pm_deassert(a);
}

/* ---------------------------------------------------------------- SART v3 */

static void sart_set(struct l1_ans *a, unsigned i, uint8_t flags, uint64_t pa, uint64_t size)
{
    w32(a, a->cfg.sart + 0x40 + 4 * i, (uint32_t)(pa >> 12));
    w32(a, a->cfg.sart + 0x80 + 4 * i, (uint32_t)(size >> 12));
    w32(a, a->cfg.sart + 0x00 + 4 * i, flags);
}

static void sart_snapshot(struct l1_ans *a)
{
    a->sart_protected = 0;
    for (unsigned i = 0; i < 16; i++)
        if (r32(a, a->cfg.sart + 4 * i) & 0xff) a->sart_protected |= (uint16_t)(1u << i);
}

static int sart_add(struct l1_ans *a, uint64_t pa, uint64_t size)
{
    if ((pa | size) & 0xfff || !size || (size >> 12) > 0x3fffffffu) return -1;
    for (unsigned i = 0; i < 16; i++) {
        if ((a->sart_protected >> i) & 1 || a->sart_ent[i].used) continue;
        a->sart_ent[i].used = 1; a->sart_ent[i].pa = pa; a->sart_ent[i].size = (uint32_t)size;
        sart_set(a, i, 0xff, pa, size);
        a->st.sart_added++;
        return 0;
    }
    return -1;
}

static void sart_remove_all(struct l1_ans *a)
{
    for (unsigned i = 0; i < 16; i++) {
        if (!a->sart_ent[i].used) continue;
        sart_set(a, i, 0, 0, 0);
        a->sart_ent[i].used = 0;
        a->st.sart_removed++;
    }
}

/* -------------------------------------------------------- mailbox / RTKit */

static int mbox_send(struct l1_ans *a, uint8_t ep, uint64_t msg)
{
    uint64_t t0 = now(a);
    uint64_t m = a->cfg.mbox;
    while (r32(a, m + L1R_A2I_CONTROL) & (1u << 16)) { /* FULL */
        if (now(a) - t0 > US(200)) return -1;
        a->hal->udelay(1);
    }
    w64(a, m + L1R_A2I_SEND0, msg);
    w64(a, m + L1R_A2I_SEND1, ep);
    return 0;
}

static int mgmt_send(struct l1_ans *a, unsigned type, uint64_t msg)
{
    msg &= ~(0xffull << 52);
    msg |= (uint64_t)type << 52;
    return mbox_send(a, EP_MGMT, msg);
}

static void set_ep(struct l1_ans *a, unsigned ep) { a->eps[ep >> 3] |= (uint8_t)(1u << (ep & 7)); }
static int has_ep(struct l1_ans *a, unsigned ep) { return (a->eps[ep >> 3] >> (ep & 7)) & 1; }

static void start_ep(struct l1_ans *a, unsigned ep)
{
    if (mgmt_send(a, M_STARTEP, ((uint64_t)ep << 32) | 2u)) return;
    if (a->live_n < sizeof(a->live_eps)) a->live_eps[a->live_n++] = (uint8_t)ep;
}

/* Buffer request answered like apple_rtkit_common_rx_get_buffer + the
 * nvme-apple shmem_setup op (no firmware-chosen iova, SART entry added). */
static void get_buffer(struct l1_ans *a, unsigned ep, uint64_t msg)
{
    uint64_t size, iova;
    if (ep == EP_OSLOG) {
        size = (msg >> 36) & 0xfffff;           /* GENMASK(55,36), bytes as Linux uses it */
        iova = (msg & 0xfffffffffull) << 12;
    } else {
        size = ((msg >> 44) & 0xff) << 12;
        iova = msg & 0xfffffffffffull;
    }
    /* the log value carries the endpoint above bit 40 and the size below it */
    if (iova || !size || (size & 0xfff)) { trace(a, "buf_refused", ((uint64_t)ep << 40) | (size & 0xffffffffffull)); return; }
    uint64_t at = (a->shmem_used + 0x3fff) & ~0x3fffull;
    if (at + size > a->shmem_size) { trace(a, "buf_nospace", ((uint64_t)ep << 40) | (size & 0xffffffffffull)); return; }
    uint8_t *p = a->mem + L1_SHMEM_OFF + at;
    zero(p, size);
    a->hal->clean(p, size);
    if (sart_add(a, (uint64_t)(uintptr_t)p, size)) { trace(a, "sart_full", (uint64_t)ep << 40); return; }
    a->shmem_used = at + size;
    uint64_t pa = (uint64_t)(uintptr_t)p, reply;
    if (ep == EP_OSLOG) {
        reply = (1ull << 56) | (size << 36) | (pa >> 12);
    } else {
        unsigned type = ep == EP_SYSTEM7 ? 5 : 1;
        reply = ((uint64_t)type << 52) | ((size >> 12) << 44) | pa;
    }
    mbox_send(a, (uint8_t)ep, reply);
    if (ep == EP_SYSLOG) { a->sys_buf = pa; a->sys_size = size; }
}

static void syslog_line(struct l1_ans *a, uint64_t msg)
{
    unsigned idx = msg & 0xff;
    if (a->sys_buf && a->sys_msg_size && idx <= a->sys_entries) {
        uint64_t entry = 0x20 + a->sys_msg_size, off = idx * entry + 8;
        if (off + 24 + a->sys_msg_size <= a->sys_size) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)a->sys_buf + off;
            a->hal->inval(p, 24 + a->sys_msg_size);
            char line[96];
            unsigned n = 0, k;
            for (k = 0; k < 23 && p[k] && n < 20; k++) line[n++] = (char)(p[k] >= 32 && p[k] < 127 ? p[k] : '.');
            line[n++] = ':';
            const uint8_t *m = p + 24;
            unsigned len = a->sys_msg_size;
            while (len && (m[len - 1] == 0 || m[len - 1] == ' ' || m[len - 1] == '\n' || m[len - 1] == '\r')) len--;
            for (k = 0; k < len && n < 90; k++) line[n++] = (char)(m[k] >= 32 && m[k] < 127 ? m[k] : '.');
            line[n] = 0;
            a->st.syslog_lines++;
            if (a->hal->syslog) a->hal->syslog(a->hal->ctx, line);
        }
    }
    mbox_send(a, EP_SYSLOG, msg); /* the firmware waits for the echo */
}

static void rtk_rx(struct l1_ans *a, uint8_t ep, uint64_t msg)
{
    unsigned type = (msg >> 52) & 0xff;
    switch (ep) {
    case EP_MGMT:
        if (type == M_HELLO) {
            unsigned min = msg & 0xffff, max = (msg >> 16) & 0xffff;
            a->hello_seen = 1;
            if (min > 12 || max < 11) { a->crashed = 1; return; }
            unsigned want = max < 12 ? max : 12;
            a->st.rtkit_version = want;
            trace(a, "hello", want);
            mgmt_send(a, M_HELLO_REPLY, (uint64_t)want | ((uint64_t)want << 16));
        } else if (type == M_EPMAP) {
            unsigned base = (msg >> 32) & 7;
            uint32_t bitmap = (uint32_t)msg;
            for (unsigned i = 0; i < 32; i++)
                if ((bitmap >> i) & 1) set_ep(a, 32 * base + i);
            uint64_t reply = (uint64_t)base << 32;
            reply |= (msg & (1ull << 51)) ? (1ull << 51) : 1ull;
            mgmt_send(a, M_EPMAP, reply);
            if (msg & (1ull << 51)) {
                for (unsigned e = 1; e < 0x20; e++) {
                    if (!has_ep(a, e)) continue;
                    if (e == EP_CRASHLOG || e == EP_SYSLOG || e == EP_DEBUG || e == EP_IOREPORT ||
                        e == EP_SYSTEM7 || e == EP_OSLOG || e == EP_TRACEKIT)
                        start_ep(a, e);
                }
                a->epmap_done = 1;
            }
        } else if (type == M_IOP_PWR_ACK) {
            a->iop_state = msg & 0xffff;
            a->iop_ack++;
        } else if (type == M_AP_PWR_ACK) {
            a->ap_state = msg & 0xffff;
            a->ap_ack++;
        }
        break;
    case EP_CRASHLOG:
        if (type == 1) {
            if (!a->crash_buf) { a->crash_buf = 1; get_buffer(a, ep, msg); }
            else { a->crashed = 1; trace(a, "crashlog", msg); }
        }
        break;
    case EP_SYSLOG:
        if (type == 1) get_buffer(a, ep, msg);
        else if (type == 8) { a->sys_entries = msg & 0xff; a->sys_msg_size = (msg >> 24) & 0xff; }
        else if (type == 5) syslog_line(a, msg);
        break;
    case EP_IOREPORT:
        if (type == 1) get_buffer(a, ep, msg);
        else if (type == 8 || type == 0xc) mbox_send(a, EP_IOREPORT, msg);
        break;
    case EP_SYSTEM7:
        if (type == 1) get_buffer(a, ep, msg);
        break;
    case EP_OSLOG:
        if (((msg >> 56) & 0xff) == 1) get_buffer(a, ep, msg);
        break;
    default:
        break;
    }
}

static int rtk_poll(struct l1_ans *a)
{
    uint64_t m = a->cfg.mbox;
    int n = 0;
    while (!(r32(a, m + L1R_I2A_CONTROL) & (1u << 17)) && n < 64) { /* not EMPTY */
        uint64_t msg0 = r64(a, m + L1R_I2A_RECV0);
        uint64_t msg1 = r64(a, m + L1R_I2A_RECV1);
        rtk_rx(a, (uint8_t)(msg1 & 0xff), msg0);
        n++;
    }
    return n;
}

/* Polls the mailbox until done() or timeout. which: 1 epmap, 2 iop ack, 3 ap ack, 4 boot status */
static int rtk_wait(struct l1_ans *a, int which, uint32_t seq, uint32_t timeout_us)
{
    uint64_t t0 = now(a);
    uint64_t limit = t0 + timeout_us;
    if (a->rtk_deadline_us && limit > a->rtk_deadline_us) limit = a->rtk_deadline_us; /* RTKit boot <= 2 s in total */
    for (;;) {
        rtk_poll(a);
        if (a->crashed) return -2;
        switch (which) {
        case 1: if (a->epmap_done) return 0; break;
        case 2: if (a->iop_ack != seq) return 0; break;
        case 3: if (a->ap_ack != seq) return 0; break;
        case 4: if (r32(a, a->cfg.nvme + L1R_NVME_BOOT_STATUS) == L1R_BOOT_STATUS_OK) return 0; break;
        }
        if (now(a) > limit) return -1;
        a->hal->udelay(2);
    }
}

static int rtk_boot(struct l1_ans *a)
{
    if (rtk_wait(a, 1, 0, US(900))) return -1;
    trace(a, "epmap", a->st.rtkit_version);
    if (a->iop_ack == 0 && rtk_wait(a, 2, 0, US(500))) return -2;
    trace(a, "iop_ack", a->iop_state);
    uint32_t seq = a->ap_ack;
    if (mgmt_send(a, M_AP_PWR, PWR_ON)) return -3;
    if (rtk_wait(a, 3, seq, US(500))) return -4;
    if ((a->ap_state & 0xff) != PWR_ON || (a->iop_state & 0xff) != PWR_ON) return -5;
    a->st.rtkit_up = 1;
    return 0;
}

/* ------------------------------------------------------------------ NVMe */

#define OFF_ASQ 0x0000
#define OFF_ACQ 0x1000
#define OFF_IOSQ 0x2000
#define OFF_IOCQ 0x3000
#define OFF_ATCB 0x4000
#define OFF_ITCB 0x6000
#define OFF_IDENT 0x8000
#define OFF_PRPL 0x9000
#define QDEPTH 64u /* IO queue depth; NUM_TCBS = 63 */
#define AQDEPTH 2u

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

struct nvme_cmd {
    uint8_t opcode;
    uint32_t nsid;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12;
};

/* One command at a time. Tags as Linux: admin queue tag 0 (tags 0-1 are reserved for it, depth 2),
 * IO queue tag 2 (apple.c: tagset.reserved_tags = APPLE_NVME_AQ_DEPTH). The controller allows tags < 0x40. */
static int nvme_exec(struct l1_ans *a, int admin, const struct nvme_cmd *c, uint32_t timeout_us)
{
    const unsigned tag = admin ? 0 : 2;
    uint8_t *sq = a->mem + (admin ? OFF_ASQ : OFF_IOSQ) + tag * 64;
    uint8_t *tcb = a->mem + (admin ? OFF_ATCB : OFF_ITCB) + tag * 128;
    uint8_t *cq = a->mem + (admin ? OFF_ACQ : OFF_IOCQ);
    uint16_t *head = admin ? &a->admin_head : &a->io_head;
    uint8_t *phase = admin ? &a->admin_phase : &a->io_phase;
    unsigned depth = admin ? AQDEPTH : QDEPTH;
    zero(sq, 64);
    sq[0] = c->opcode;
    sq[2] = (uint8_t)tag;
    put32(sq + 4, c->nsid);
    put64(sq + 24, c->prp1);
    put64(sq + 32, c->prp2);
    put32(sq + 40, c->cdw10);
    put32(sq + 44, c->cdw11);
    put32(sq + 48, c->cdw12);
    /* apple_nvme_submit_cmd_t8103 */
    zero(tcb, 128);
    tcb[0] = 0;
    tcb[1] = !c->prp1 ? 0 : (c->opcode & 1) ? 2 : 1; /* TO_DEVICE : FROM_DEVICE */
    tcb[2] = (uint8_t)tag;
    tcb[4] = (uint8_t)(c->cdw12 & 0xff); tcb[5] = (uint8_t)((c->cdw12 >> 8) & 0xff); /* rw.length */
    put64(tcb + 24, c->prp1);
    put64(tcb + 32, c->prp2);
    a->hal->clean(sq, 64);
    a->hal->clean(tcb, 128);
    w32(a, a->cfg.nvmmu + (admin ? L1R_NVMMU_ASQ_DB : L1R_NVMMU_IOSQ_DB), tag);
    uint64_t t0 = now(a);
    for (;;) {
        uint8_t *e = cq + *head * 16;
        a->hal->inval(e, 16);
        uint32_t status = get16(e + 14);
        if ((status & 1) == *phase) {
            uint32_t id = get16(e + 12);
            w32(a, a->cfg.nvmmu + L1R_NVMMU_TCB_INVAL, id);
            (void)r32(a, a->cfg.nvmmu + L1R_NVMMU_TCB_STAT);
            if (++*head == depth) { *head = 0; *phase ^= 1; a->st.cq_phase_flips++; }
            w32(a, a->cfg.nvme + (admin ? L1R_NVME_ACQ_DB : L1R_NVME_IOCQ_DB), *head);
            if (id != tag) return -3;
            return (status >> 1) ? -2 : 0;
        }
        rtk_poll(a); /* keep the firmware's syslog flowing */
        if (a->crashed) return -4;
        if (now(a) - t0 > timeout_us) return -1;
        a->hal->udelay(1);
    }
}

static int nvme_wait_rdy(struct l1_ans *a, uint32_t want, uint32_t timeout_us)
{
    uint64_t t0 = now(a);
    while ((r32(a, a->cfg.nvme + L1R_NVME_CSTS) & 1) != want) {
        rtk_poll(a);
        if (now(a) - t0 > timeout_us) return -1;
        a->hal->udelay(2);
    }
    return 0;
}

static void nvme_state_zero(struct l1_ans *a)
{
    a->admin_head = a->io_head = 0;
    a->admin_phase = a->io_phase = 1;
}

/* ----------------------------------------------------------- public API */

static void win_add(struct l1_ans *a, uint64_t base, uint64_t size)
{
    if (!base || !size || base + size < base || a->nwin >= L1_WIN_MAX) return;
    a->win[a->nwin].b = base;
    a->win[a->nwin].e = base + size;
    a->nwin++;
}

void l1_ans_init(struct l1_ans *a, const struct l1_hal *hal, const struct l1_ans_cfg *cfg, void *mem, size_t mem_bytes)
{
    zero(a, sizeof(*a));
    a->hal = hal;
    a->cfg = *cfg;
    a->mem = mem;
    a->shmem_size = mem_bytes > L1_SHMEM_OFF ? mem_bytes - L1_SHMEM_OFF : 0;
    nvme_state_zero(a);
    /* the only addresses this driver may touch */
    win_add(a, cfg->asc, cfg->asc_size);
    win_add(a, cfg->mbox, cfg->mbox_size);
    win_add(a, cfg->nvme, cfg->nvme_size);
    win_add(a, cfg->nvmmu, cfg->nvmmu_size);
    win_add(a, cfg->sart, cfg->sart_size);
    for (unsigned i = 0; i < cfg->pm_count && i < L1_PM_MAX; i++) win_add(a, cfg->pm_reg[i], 4);
}

static uint32_t ms_since(struct l1_ans *a, uint64_t t0)
{
    return (uint32_t)((now(a) - t0) / 1000);
}

/* ---- probe: read-only, one register per call (the caller guards each call) ---- */

unsigned l1_ans_probe_count(const struct l1_ans *a) { return a->cfg.pm_count + L1P_DEV_COUNT; }

int l1_ans_probe(struct l1_ans *a, unsigned item, uint32_t *v, uint64_t *addr)
{
    uint64_t x;
    if (item < a->cfg.pm_count) x = a->cfg.pm_reg[item];
    else {
        unsigned d = item - a->cfg.pm_count;
        /* only registers Linux itself reads in this state (apple.c reset_work reads CPU_CONTROL first, the SART
         * driver reads its config at probe): mailbox control and NVMe CC/CSTS/BOOT_STATUS of a stopped ANS are
         * read by nobody before RTKit is up, and a bus hang is the one fault no guard contains */
        if (d == L1P_CPU) x = a->cfg.asc + L1R_CPU_CONTROL;
        else if (d >= L1P_SART0 && d < L1P_SART0 + 16) x = a->cfg.sart + 4 * (d - L1P_SART0);
        else return -1;
    }
    if (addr) *addr = x;
    *v = r32(a, x);
    return a->dead ? -1 : 0;
}

/* ---- bring-up steps ---------------------------------------------------- */

/* iBoot leaves every domain of the nvme node ON and ANS running (R41 native probe). Anything else is refused: the
 * loader writes no power register and starts no firmware. */
enum l1_status l1_ans_power(struct l1_ans *a)
{
    a->t0_us = now(a);
    zero(a->mem, L1_SHMEM_OFF);
    a->hal->clean(a->mem, L1_SHMEM_OFF);
    /* the ANS block is held in RESET or DEV_DISABLE: its registers may not answer; nothing is read from it */
    uint32_t w = r32(a, a->cfg.pm_ans);
    if (pm_active(w) && (w & (PM_RESET | PM_DEV_DISABLE))) {
        a->why = "ans-held";
        trace(a, "ans-held", w);
        return L1_ANS_POWER;
    }
    for (unsigned i = 0; i < a->cfg.pm_count; i++) {
        if (pm_active(r32(a, a->cfg.pm_reg[i]))) continue;
        a->bad_idx = i; a->why = a->dead ? "addr-check" : "pm-off";
        trace(a, "pm_off", i);
        return L1_ANS_POWER;
    }
    if (a->dead) { a->why = "addr-check"; return L1_ANS_POWER; }
    if (w & (PM_RESET | PM_DEV_DISABLE)) { a->why = "ans-held"; trace(a, "ans-held", w); return L1_ANS_POWER; }
    a->st.power_on = 1;
    trace(a, "power", 0);
    return L1_OK;
}

enum l1_status l1_ans_check_cpu(struct l1_ans *a)
{
    uint32_t cc = r32(a, a->cfg.asc + L1R_CPU_CONTROL);
    if (a->dead) { a->why = "addr-check"; return L1_ANS_POWER; }
    if (!(cc & L1R_CPU_RUN)) { trace(a, "ans_stopped", cc); a->why = "ans-stopped"; return L1_ANS_POWER; }
    return L1_OK;
}

enum l1_status l1_ans_start(struct l1_ans *a)
{
    uint64_t t0 = a->t0_us ? a->t0_us : now(a);
    sart_snapshot(a);
    /* apple_rtkit_wake (rtkit.c:1087-1107): SET_IOP_PWR_STATE(INIT), then apple_rtkit_boot (:961-1007) = wait EPMAP,
     * wait IOP ack, AP power ON; m1n1 rtkit_boot (rtkit.c:496-505) sends INIT unconditionally. No reset, no power
     * write, no CPU_CONTROL write. */
    trace(a, "wake", 0);
    a->rtk_deadline_us = now(a) + US(2000);   /* RTKit boot incl. the ANS boot status word: 2 s in total */
    a->st.cpu_started = 1;     /* before the INIT store: from here the hand-back is responsible */
    a->st.iop_woken = 1;
    if (mgmt_send(a, M_IOP_PWR, PWR_INIT)) { a->why = "wake-send"; return L1_RTKIT_BOOT; }
    /* apple_rtkit_boot, then the ANS boot status word */
    if (rtk_boot(a)) { trace(a, "rtkit_fail", 0); a->why = a->dead ? "addr-check" : "rtkit"; return L1_RTKIT_BOOT; }
    if (rtk_wait(a, 4, 0, US(900))) {
        trace(a, "ans_boot_status", r32(a, a->cfg.nvme + L1R_NVME_BOOT_STATUS));
        a->why = a->dead ? "addr-check" : "boot-status";
        return L1_RTKIT_BOOT;
    }
    a->rtk_deadline_us = 0;
    a->st.ms_ans_boot = ms_since(a, t0);
    trace(a, "rtkit_ok", a->st.ms_ans_boot);
    return L1_OK;
}

enum l1_status l1_ans_nvme(struct l1_ans *a)
{
    uint64_t t0 = a->t0_us ? a->t0_us : now(a);
    /* NVMMU / admin queue / enable (fw26: no LINEAR_SQ_CTRL, no MAX_PEND_CMDS write) */
    uint64_t base = (uint64_t)(uintptr_t)a->mem;
    w32(a, a->cfg.nvmmu + L1R_NVMMU_NUM_TCBS, QDEPTH - 1);
    w32(a, a->cfg.nvme + L1R_NVME_AQA, (AQDEPTH - 1) | ((AQDEPTH - 1) << 16));
    w64lh(a, a->cfg.nvme + L1R_NVME_ASQ, base + OFF_ASQ);
    w64lh(a, a->cfg.nvme + L1R_NVME_ACQ, base + OFF_ACQ);
    w64lh(a, a->cfg.nvmmu + L1R_NVMMU_ASQ_TCB, base + OFF_ATCB);
    w64lh(a, a->cfg.nvmmu + L1R_NVMMU_IOSQ_TCB, base + OFF_ITCB);
    (void)r32(a, a->cfg.nvme + 0x00); /* CAP, as nvme_enable_ctrl reads it */
    uint32_t cap_hi = r32(a, a->cfg.nvme + 0x04);
    /* nvme_enable_ctrl: CC.CSS = CSI when CAP.CSS has bit 6, MPS = 0 (4 KiB), AMS RR,
     * IOSQES=6 IOCQES=4, then |EN */
    a->cc = (((cap_hi >> 5) & 0xff) & 0x40 ? 6u << 4 : 0) | (6u << 16) | (4u << 20);
    w32(a, a->cfg.nvme + L1R_NVME_CC, a->cc);
    (void)r32(a, a->cfg.nvme + L1R_NVME_CC);
    trace(a, "csts0", r32(a, a->cfg.nvme + L1R_NVME_CSTS));
    if (nvme_wait_rdy(a, 0, US(2000)))
        trace(a, "rdy0_timeout", r32(a, a->cfg.nvme + L1R_NVME_CSTS));
    a->cc |= 1;
    a->st.nvme_enabled = 1;    /* before the EN store, same reason */
    w32(a, a->cfg.nvme + L1R_NVME_CC, a->cc);
    if (nvme_wait_rdy(a, 1, US(10000))) {
        trace(a, "csts", r32(a, a->cfg.nvme + L1R_NVME_CSTS));
        a->why = a->dead ? "addr-check" : "csts";
        return L1_NVME_ENABLE;
    }
    trace(a, "nvme_rdy", ms_since(a, t0));
    nvme_state_zero(a);
    /* identify controller, identify namespace 1 (4 KiB LBAs) */
    struct nvme_cmd c;
    uint8_t *ident = a->mem + OFF_IDENT;
    zero(&c, sizeof(c));
    c.opcode = 0x06; c.prp1 = base + OFF_IDENT; c.cdw10 = 1;
    zero(ident, 4096); a->hal->clean(ident, 4096);
    if (nvme_exec(a, 1, &c, US(200))) { a->why = a->dead ? "addr-check" : "identify"; return L1_NVME_IDENTIFY; }
    a->hal->inval(ident, 4096);
    unsigned mdts = ident[77];
    a->mdts_blocks = mdts && mdts < 16 ? (1u << mdts) : 0; /* units of the 4 KiB min page */
    trace(a, "id_ctrl", mdts);
    zero(&c, sizeof(c));
    c.opcode = 0x06; c.nsid = 1; c.prp1 = base + OFF_IDENT; c.cdw10 = 0;
    zero(ident, 4096); a->hal->clean(ident, 4096);
    if (nvme_exec(a, 1, &c, US(200))) { a->why = a->dead ? "addr-check" : "identify"; return L1_NVME_IDENTIFY; }
    a->hal->inval(ident, 4096);
    {
        uint64_t nsze = get32(ident) | (uint64_t)get32(ident + 4) << 32;
        unsigned fl = ident[26] & 0xf;
        unsigned lbads = ident[128 + 4 * fl + 2];
        trace(a, "id_ns", nsze);
        if (lbads != 12 || nsze < L1_P4_END) { a->why = "namespace"; return L1_NVME_IDENTIFY; }
    }
    /* IO queues: create CQ, create SQ, then the T8132-style register writes */
    zero(&c, sizeof(c));
    c.opcode = 0x05; c.prp1 = base + OFF_IOCQ; c.cdw10 = 1 | ((QDEPTH - 1) << 16); c.cdw11 = 3;
    if (nvme_exec(a, 1, &c, US(200))) { a->why = a->dead ? "addr-check" : "create-cq"; return L1_NVME_QUEUES; }
    zero(&c, sizeof(c));
    c.opcode = 0x01; c.prp1 = base + OFF_IOSQ; c.cdw10 = 1 | ((QDEPTH - 1) << 16); c.cdw11 = 1 | (1u << 16);
    if (nvme_exec(a, 1, &c, US(200))) { a->why = a->dead ? "addr-check" : "create-sq"; return L1_NVME_QUEUES; }
    w64lh(a, a->cfg.nvme + L1R_NVME_IOQ_CQES, base + OFF_IOCQ);
    w64lh(a, a->cfg.nvme + L1R_NVME_IOQ_CMDS, base + OFF_IOSQ);
    w32(a, a->cfg.nvme + L1R_NVME_IOQ_SIZE, (QDEPTH - 1) | ((QDEPTH - 1) << 16));
    zero(a->mem + OFF_ITCB, 0x2000);
    zero(a->mem + OFF_IOCQ, QDEPTH * 16);
    a->hal->clean(a->mem + OFF_ITCB, 0x2000);
    a->hal->clean(a->mem + OFF_IOCQ, QDEPTH * 16);
    a->io_head = 0; a->io_phase = 1;
    a->st.ioq_up = 1;
    a->st.ms_nvme_ready = ms_since(a, t0);
    trace(a, "ioq", a->st.ms_nvme_ready);
    return a->dead ? L1_NVME_QUEUES : L1_OK;
}

enum l1_status l1_ans_bringup(struct l1_ans *a)
{
    enum l1_status st = l1_ans_power(a);
    if (st == L1_OK) st = l1_ans_check_cpu(a);
    if (st == L1_OK) st = l1_ans_start(a);
    if (st == L1_OK) st = l1_ans_nvme(a);
    return st;
}

/* One read of n blocks (n <= max chunk) into dst. */
static int read_chunk(struct l1_ans *a, uint64_t lba, uint32_t n, uint8_t *dst)
{
    uint64_t pa = (uint64_t)(uintptr_t)dst, base = (uint64_t)(uintptr_t)a->mem;
    struct nvme_cmd c;
    zero(&c, sizeof(c));
    c.opcode = 0x02; c.nsid = 1; c.prp1 = pa;
    c.cdw10 = (uint32_t)lba; c.cdw11 = (uint32_t)(lba >> 32); c.cdw12 = n - 1;
    if (n == 2) c.prp2 = pa + 4096;
    else if (n > 2) {
        uint8_t *list = a->mem + OFF_PRPL;
        for (uint32_t i = 1; i < n; i++) put64(list + 8 * (i - 1), pa + 4096ull * i);
        a->hal->clean(list, 8 * (n - 1));
        c.prp2 = base + OFF_PRPL;
    }
    a->hal->clean(dst, (size_t)n * 4096); /* flush stale/dirty lines before the device writes */
    int rc = nvme_exec(a, 0, &c, US(200) + n * 50u);
    a->hal->inval(dst, (size_t)n * 4096); /* drop anything the CPU prefetched meanwhile */
    return rc;
}

int l1_ans_read(void *ctx, uint64_t lba, uint32_t n, void *dst)
{
    struct l1_ans *a = ctx;
    uint8_t *p = dst;
    uint64_t t0 = now(a);
    if (!a->st.ioq_up || ((uintptr_t)dst & 4095)) return L1_NVME_READ;
    if (a->deadline_us && now(a) > a->deadline_us) return L1_TIMEOUT;
    uint32_t max = 256; /* 1 MiB per command, one PRP list page */
    if (a->mdts_blocks && a->mdts_blocks < max) max = a->mdts_blocks;
    while (n) {
        uint32_t k = n < max ? n : max;
        if (a->deadline_us && now(a) > a->deadline_us) return L1_TIMEOUT;
        int rc = read_chunk(a, lba, k, p);
        if (rc) return rc == -1 ? L1_TIMEOUT : L1_NVME_READ;
        lba += k; n -= k; p += (size_t)k * 4096;
        a->st.reads++; a->st.blocks += k;
    }
    a->st.ms_read += (uint32_t)((now(a) - t0) / 1000);
    return 0;
}

/* rev 17: the one write, a single 4 KiB block (the spent one-shot token): Write, then Flush so the block is on the media
 * before the boot goes on (the next stage may hang and be powered off). The caller has range-checked lba. */
int l1_ans_write(void *ctx, uint64_t lba, const void *src)
{
    struct l1_ans *a = ctx;
    uint64_t pa = (uint64_t)(uintptr_t)src;
    if (!a->st.ioq_up || (pa & 4095)) return L1_NVME_READ;
    if (a->deadline_us && now(a) > a->deadline_us) return L1_TIMEOUT;
    struct nvme_cmd c;
    zero(&c, sizeof(c));
    c.opcode = 0x01; c.nsid = 1; c.prp1 = pa;
    c.cdw10 = (uint32_t)lba; c.cdw11 = (uint32_t)(lba >> 32); c.cdw12 = 0;
    a->hal->clean((void *)(uintptr_t)src, 4096);    /* the device reads the block from memory */
    if (nvme_exec(a, 0, &c, US(200))) return L1_NVME_READ;
    zero(&c, sizeof(c));
    c.opcode = 0x00; c.nsid = 1;                       /* Flush */
    if (nvme_exec(a, 0, &c, US(1000))) return L1_NVME_READ;
    a->st.writes++;
    return 0;
}

/* R44 hand-back for a wake-mode session (RTKit up): ANS stays RUNNING with its host session, exactly as iBoot left it
 * (R41/R42 native: INIT to a running firmware restarts the RTKit handshake at once; a firmware we put to sleep is what
 * Linux never woke). Delete the IO queues, CC.EN = 0 (no CC.SHN: nothing was written) and wait RDY clear, bounded.
 * No RTKit power message, no PMGR write, no reset, CPU_CONTROL untouched (RUN stays set), our SART entries stay: the
 * firmware keeps its syslog/crashlog/ioreport/oslog buffer addresses until Linux's INIT gives it new ones; those buffers
 * and the NVMe/NVMMU memory are in the loader's own pool, outside the Linux arena (reserved-memory direct-desktop-host1). */
enum l1_status l1_ans_leave(struct l1_ans *a)
{
    struct nvme_cmd c;
    uint64_t t0 = now(a);
    a->dead = 0;    /* hand-back only touches allow-listed registers: always allowed to run */
    a->rtk_deadline_us = 0;
    if (!a->st.rtkit_up) return L1_OK;   /* the session never came up: nothing to leave, the emergency stops ANS */
    if (a->st.ioq_up) {
        zero(&c, sizeof(c));
        c.opcode = 0x00; c.cdw10 = 1; /* delete SQ 1 */
        (void)nvme_exec(a, 1, &c, US(200));
        zero(&c, sizeof(c));
        c.opcode = 0x04; c.cdw10 = 1; /* delete CQ 1 */
        (void)nvme_exec(a, 1, &c, US(200));
        a->st.ioq_up = 0;
    }
    if (a->st.nvme_enabled) {
        a->cc &= ~((3u << 14) | 1u);
        w32(a, a->cfg.nvme + L1R_NVME_CC, a->cc);                              /* EN = 0 */
        (void)nvme_wait_rdy(a, 0, US(300));
        a->st.nvme_enabled = 0;
    }
    a->st.left_rdy = (uint8_t)(r32(a, a->cfg.nvme + L1R_NVME_CSTS) & 1);
    a->st.sart_kept = a->st.sart_added - a->st.sart_removed;
    a->st.cpu_started = 0;
    a->st.rtkit_up = 0;
    a->st.left = 1;
    a->st.ms_shutdown = ms_since(a, t0);
    trace(a, "handback", a->st.left_rdy);
    return L1_OK;
}

/* Last resort, its own guard (review L2-REV-06): the emergency is one guard and dies at its first faulting
 * access; a block that keeps faulting must not keep CPU_CONTROL.RUN set (Linux would take the RTKit wake path) or
 * the ANS PMGR word in RESET / DEV_DISABLE. Order: reset deasserted, RUN cleared, then the SART entries. */
/* rev 17, after l1_ans_leave for an m1n1 entry: ANS stays RUNNING as iBoot left it (m1n1's rtkit_boot sends INIT, as
 * Linux does), but our SART entries go: the firmware's syslog/ioreport buffers are in the loader's pool, which m1n1 does
 * not know is reserved, so until its INIT hands out new ones the firmware must not reach them. iBoot's entries (the
 * snapshot) stay. */
void l1_ans_release(struct l1_ans *a)
{
    a->dead = 0;
    sart_remove_all(a);
    a->st.sart_kept = 0;
}

void l1_ans_stop(struct l1_ans *a)
{
    a->dead = 0;
    if (a->st.reset_touched) pm_deassert(a);
    if (a->st.cpu_started) { w32(a, a->cfg.asc + L1R_CPU_CONTROL, 0); a->st.cpu_started = 0; }
    sart_remove_all(a);        /* last, after RUN is clear: a faulting SART block can no longer cost the RUN clear */
}

void l1_ans_emergency(struct l1_ans *a)
{
    a->dead = 0;
    a->rtk_deadline_us = 0;
    if (a->st.nvme_enabled) {
        a->cc &= ~((3u << 14) | 1u);
        w32(a, a->cfg.nvme + L1R_NVME_CC, a->cc);
        (void)nvme_wait_rdy(a, 0, US(200));
        a->st.nvme_enabled = 0;
    }
    sart_remove_all(a);
    if (a->st.reset_touched) pm_deassert(a);
    if (a->st.cpu_started) {         /* ANS was talked to and cannot be left live: reset it, RUN clear */
        pm_reset(a);
        w32(a, a->cfg.asc + L1R_CPU_CONTROL, 0);
        a->st.cpu_started = 0;
    }
    a->st.rtkit_up = 0;
    a->st.ioq_up = 0;
    trace(a, "emergency", 0);
}

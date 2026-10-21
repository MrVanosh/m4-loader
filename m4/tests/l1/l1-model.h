/* SPDX-License-Identifier: MIT */
/* Scripted model of the J616s ANS (PMGR, ASC mailbox v4, RTKit v12, SARTv3, NVMe fw26 + NVMMU).
 * Shared by l1-trace.c (register trace) and l1-e2e.c (whole attempt). */
#ifndef L1_MODEL_H
#define L1_MODEL_H
#include "level1-ans.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
static size_t uni_size = 8u << 20;
#define UNI uni_size
static uint8_t *U, *D;           /* CPU view, device view of the DMA universe */
static uint8_t *dma_mem;         /* ANS region inside U */
static uint8_t *rdbuf;           /* read destination inside U */

enum fault { F_NONE, F_NO_HELLO, F_NO_EPMAP, F_NO_RDY, F_READ_ERR, F_NO_COMPLETE,
             F_BAD_LBADS, F_CRASH, F_NO_BOOT_STATUS, F_NO_AP_ACK,
             F_PROTECTED_SART, F_ABORT_AT_READ, F_NO_CQ_DELETE, F_NOISE, F_PARENT_OFF, F_OWN_OFF, F_ANS_DEV_DISABLE, F_ANS_RESET_HELD, F_ANS_OFF_RESET,
             F_ANS_STOPPED, F_NO_SYSLOG_INIT, F_NO_SYSLOG_BUF };
static enum fault fault;
static int wake_start = 1;  /* the start state: ANS as iBoot leaves it on this Mac (RUN set, firmware running, SART entries 0-4,6 set); the only one the loader runs */

/* ---- address map: the Linux DT template's (nvme@44dcc0000 + phandles), as the loader derives them ---- */
#define ASC 0x409600000ull
#define MBOX 0x409608000ull
#define NVME 0x44dcc0000ull
#define NVMMU 0x40dcc0000ull
#define SART 0x40dc50000ull
/* power-controller@OFF: AFC (pmgr@502800000), FAB3, ANS, APCIE_ST0, SYS_ST0, APCIE_ST1, SYS_ST1 (pmgr@502280000) */
static const uint64_t PM[7] = {0x502800178ull, 0x502280118ull, 0x502280370ull, 0x502280108ull, 0x502280380ull, 0x502280110ull, 0x502280388ull};
static const uint8_t PM_OWN[7] = {0, 0, 1, 0, 1, 0, 1};
#define PM_ANS 0x502280370ull
#define PM_IDX_ANS 2
static unsigned pm_n;     /* distinct power registers touched (the e2e asserts all 7 and no others) */
static uint8_t pm_seen[7];
static int pm_find(uint64_t a)
{
    for (unsigned i = 0; i < 7; i++) if (PM[i] == a) { if (!pm_seen[i]) { pm_seen[i] = 1; pm_n++; } return (int)i; }
    return -1;
}
static int pm_range(uint64_t a) { return (a >> 20) == 0x5022 || (a >> 20) == 0x5028; } /* anywhere in the two PMGR blocks */

static struct {
    uint32_t pm[16];
    int pm_stuck_reads, pm_parent_writes, pm_writes;
    uint32_t cpu_ctrl, cc, csts_extra;
    int alive, boot_ok, hello_sent, shut;
    struct { uint64_t m0; uint32_t ep; } i2a[256];
    unsigned ih, it;
    uint64_t a2i0;
    uint32_t sart_cfg[16], sart_pa[16], sart_sz[16];
    uint32_t aqa; uint64_t asq, acq, ioq_cqes, ioq_cmds; uint32_t ioq_size;
    uint32_t num_tcbs; uint64_t asq_tcb, iosq_tcb;
    int io_registered, io_cq_created, io_sq_created, admin_phase, io_phase;
    unsigned admin_head_dev, io_head_dev, admin_cmds, io_cmds, inval_expected;
    uint32_t inval_tags, last_inval;
    uint32_t ap_state, iop_state;
    uint64_t syslog_buf, syslog_size; int syslog_started;
    uint32_t reads_ok;
    int violations;
    char viol[8][96];
    uint32_t cap_delay;
    int ans_reset_count;
    uint32_t shn;
    int ever_run;                       /* CPU_CONTROL.RUN was written at least once */
    int inits;                          /* SET_IOP_PWR_STATE(INIT) messages received (wake path) */
    uint32_t sart_init_cfg[16], sart_init_pa[16], sart_init_sz[16];   /* what iBoot left (wake start) */
} M;

#define VIOL(...) do { if (M.violations < 8) snprintf(M.viol[M.violations], 96, __VA_ARGS__); M.violations++; } while (0)

/* ---- trace -------------------------------------------------------------- */
struct ev { char s[64]; };
static struct ev trace_buf[20000];
static unsigned tn;
static void ev(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void ev(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    if (tn < 20000) vsnprintf(trace_buf[tn++].s, 64, fmt, ap);
    va_end(ap);
}
static int find_after(const char *needle, int from)
{
    for (int i = from; i < (int)tn; i++) if (!strncmp(trace_buf[i].s, needle, strlen(needle))) return i;
    return -1;
}
/* the endpoints the loader started (mgmt STARTEP), in start order */
static unsigned startep_list(uint8_t *out, unsigned cap)
{
    unsigned n = 0;
    for (unsigned i = 0; i < tn && n < cap; i++)
        if (!strncmp(trace_buf[i].s, "STARTEP ", 8)) out[n++] = (uint8_t)strtoul(trace_buf[i].s + 8, 0, 10);
    return n;
}
/* every needle must occur, in the given order (subsequence) */
static int expect_order(const char *const *needles, int n, const char *what)
{
    int at = 0;
    for (int i = 0; i < n; i++) {
        at = find_after(needles[i], at);
        if (at < 0) { printf("  ORDER FAIL (%s): '%s' not found after previous step\n", what, needles[i]); return 1; }
        at++;
    }
    return 0;
}

/* ---- time --------------------------------------------------------------- */
static uint64_t t_us;
static uint64_t f_now(void) { return t_us; }
static void f_udelay(uint32_t u) { t_us += u ? u : 1; }

/* ---- fault injection into the register path: the n-th access (any register) is an abort ---- */
/* fault_at_access: the first fault (n-th register access). fault2_off: a second fault that many accesses after the
 * first was taken. persist_*: from access persist_from on, every access to [persist_lo, persist_hi) faults (a wedged
 * block). All three are permanent parts of the e2e sweeps (review-l1v2 round 2). */
static long fault_at_access = -1, access_n, fault2_off = -1, fault2_at = -1;
static uint64_t persist_lo, persist_hi;
static long persist_from = -1;
static void (*abort_hook)(void);        /* the e2e's h_guarded longjmps from here */
static void maybe_abort(void)
{
    long n = access_n++;
    if (fault_at_access >= 0 && n == fault_at_access && abort_hook) { if (fault2_off >= 0) fault2_at = n + 1 + fault2_off; abort_hook(); }
    else if (fault2_at >= 0 && n == fault2_at && abort_hook) { fault2_at = -1; abort_hook(); }
}
static long persist_hits;       /* accesses that persist_check turned into a fault */
static void persist_check(uint64_t a)
{
    if (persist_from >= 0 && access_n >= persist_from && a >= persist_lo && a < persist_hi && abort_hook) { persist_hits++; abort_hook(); }
}
/* Writes: where the fault is reported relative to the register effect. An SError of a store is raised AFTER the
 * store was issued, so the realistic model is "after" (review L2-REV-01); "before" is the R39 model. Both are swept. */
static int fault_after_effect = 1;
static int abort_due(void)
{
    long n = access_n++;
    if (fault_at_access >= 0 && n == fault_at_access && abort_hook) { if (fault2_off >= 0) fault2_at = n + 1 + fault2_off; return 1; }
    if (fault2_at >= 0 && n == fault2_at && abort_hook) { fault2_at = -1; return 1; }
    return 0;
}

/* ---- mailbox model ------------------------------------------------------- */
static void push(uint64_t m0, uint32_t ep) { M.i2a[M.it++ & 255].m0 = m0; M.i2a[(M.it - 1) & 255].ep = ep; }
static void push_mgmt(unsigned type, uint64_t body) { push(((uint64_t)type << 52) | body, 0); }

static void sart_check_cover(uint64_t pa, uint64_t n, const char *what)
{
    for (unsigned i = 0; i < 16; i++) {
        if (!(M.sart_cfg[i] & 0xff)) continue;
        uint64_t b = (uint64_t)M.sart_pa[i] << 12, s = (uint64_t)M.sart_sz[i] << 12;
        if (pa >= b && pa + n <= b + s) return;
    }
    VIOL("%s not covered by SART", what);
}

static void fw_buffer_request(unsigned ep, unsigned pages)
{
    if (ep == 8) push((1ull << 56) | ((uint64_t)pages << 12 << 36), ep);
    else push(((uint64_t)1 << 52) | ((uint64_t)pages << 44), ep);
}

static void model_rx(uint64_t m0, uint32_t ep)
{
    unsigned type = (m0 >> 52) & 0xff;
    if (ep == 0) {
        if (type == 2) { /* HELLO_REPLY: send endpoint map */
            unsigned min = m0 & 0xffff, max = (m0 >> 16) & 0xffff;
            if (min != 12 || max != 12) VIOL("hello reply %u..%u", min, max);
            if (fault == F_NO_EPMAP) return;
            /* eps 0,1,2,3,4,7,8,0xa in base 0; LAST */
            push_mgmt(8, (1ull << 51) | 0x59fu);
        } else if (type == 8) { /* EPMAP reply */
            if (!(m0 & (1ull << 51)) && !(m0 & 1)) VIOL("epmap reply flags");
            if (m0 & (1ull << 51)) { /* all endpoints acknowledged: firmware proceeds */
                if (fault != F_NO_SYSLOG_BUF) fw_buffer_request(2, 16);   /* syslog */
                if (fault != F_NO_SYSLOG_INIT) push(((uint64_t)8 << 52) | (8u) | (0x80u << 24), 2); /* syslog INIT: 8 entries, 0x80 B messages */
                fw_buffer_request(1, 1);    /* crashlog */
                fw_buffer_request(4, 4);    /* ioreport */
                fw_buffer_request(7, 4);    /* system7 */
                fw_buffer_request(8, 1);    /* oslog (size field in bytes as Linux reads it: 4096) */
                push_mgmt(7, 0x220);        /* IOP power ack: INIT */
                if (fault == F_NOISE) { push(0xabcdef, 0x21); push(0x1, 0xfe); push_mgmt(0x42, 0); push(((uint64_t)0x77 << 52), 4); push(((uint64_t)0x33 << 52), 7); }
            }
        } else if (type == 5) { /* STARTEP */
            ev("STARTEP %u", (unsigned)((m0 >> 32) & 0xff));
        } else if (type == 0xb) { /* SET_AP_PWR_STATE */
            if (fault == F_NO_AP_ACK && (m0 & 0xffff) == 0x20) return;
            M.ap_state = m0 & 0xffff;
            push_mgmt(0xb, M.ap_state);
            if (M.ap_state == 0x20) {
                M.boot_ok = 1;
                /* the ANS firmware now logs a line */
                if (M.syslog_buf) {
                    uint8_t *e = D + (M.syslog_buf - (uint64_t)(uintptr_t)U) + 0 * (0x20 + 0x80) + 8;
                    memset(e, 0, 24 + 0x80);
                    strcpy((char *)e, "ANS");
                    strcpy((char *)e + 24, "boot: mode normal   ");
                    push(((uint64_t)5 << 52) | 0, 2);
                }
            }
        } else if (type == 6 && (m0 & 0xffff) == 0x220) { /* SET_IOP_PWR_STATE(INIT): firmware (re)starts its boot protocol */
            ev("IOP_INIT");
            M.inits++;
            M.alive = 1; M.boot_ok = 0; M.hello_sent = 0;
            M.cc = 0; M.io_registered = 0; M.io_cq_created = M.io_sq_created = 0; M.shn = 0;      /* the NVMe side restarts with it */
            M.admin_head_dev = M.io_head_dev = 0; M.admin_phase = M.io_phase = 1;
            if (fault == F_NO_HELLO) return;
            if (fault == F_NOISE) {                         /* unknown endpoints and management types before HELLO */
                push(0x1234567800ull, 0x30); push(0xdeadbeef, 5); push_mgmt(0x7f, 0x55); push(((uint64_t)9 << 52) | 1, 3);
            }
            push_mgmt(1, (12u << 16) | 11u);                /* HELLO min 11 max 12 */
            if (fault == F_CRASH) push(((uint64_t)1 << 52) | 0x1000, 1); /* spurious crashlog before buffers */
        } else if (type == 6) { /* SET_IOP_PWR_STATE */
            M.iop_state = m0 & 0xffff;
            push_mgmt(7, M.iop_state);
            if (M.iop_state == 1) M.alive = 0;
        } else {
            VIOL("unknown mgmt type %u", type);
        }
    } else if (ep == 2) {
        ev("SYSLOG-ECHO");
    } else if (ep == 1 || ep == 4 || ep == 7 || ep == 8) {
        /* buffer replies: check the SART entry covers it */
        uint64_t pa, size;
        if (ep == 8) { size = (m0 >> 36) & 0xfffff; pa = (m0 & 0xfffffffffull) << 12; }
        else { size = ((m0 >> 44) & 0xff) << 12; pa = m0 & 0xfffffffffffull; }
        sart_check_cover(pa, size, "firmware buffer");
        if (ep == 2) { M.syslog_buf = pa; M.syslog_size = size; }
        ev("BUF ep%u pa+%llx size %llx", ep, (unsigned long long)(pa - (uint64_t)(uintptr_t)U), (unsigned long long)size);
    }
    if (ep == 2 && type == 1) { M.syslog_buf = (m0 & 0xfffffffffffull); M.syslog_size = ((m0 >> 44) & 0xff) << 12; sart_check_cover(M.syslog_buf, M.syslog_size, "syslog buffer"); }
}

/* ---- NVMe model ---------------------------------------------------------- */
static uint8_t *dv(uint64_t pa) { return D + (pa - (uint64_t)(uintptr_t)U); }

static const uint8_t *pattern_block(uint64_t lba)
{
    static uint8_t b[4096];
    for (unsigned i = 0; i < 4096; i++) b[i] = (uint8_t)((lba * 2654435761u >> 7) ^ (i * 31u) ^ (lba >> 3));
    return b;
}
static const uint8_t *(*disk_fn)(uint64_t lba) = pattern_block;
static int allow_gpt_reads;
#define disk_block(l) disk_fn(l)

static void complete(int admin, unsigned tag, unsigned status)
{
    uint64_t cq = admin ? M.acq : M.ioq_cqes;
    unsigned *head = admin ? &M.admin_head_dev : &M.io_head_dev;
    unsigned depth = admin ? 2 : 64;
    int *phase = admin ? &M.admin_phase : &M.io_phase;
    uint8_t *e = dv(cq) + *head * 16;
    memset(e, 0, 16);
    e[12] = (uint8_t)tag; e[13] = 0;
    uint16_t st = (uint16_t)((status << 1) | (*phase & 1));
    e[14] = (uint8_t)st; e[15] = (uint8_t)(st >> 8);
    if (++*head == depth) { *head = 0; *phase ^= 1; }
    M.inval_expected = 1;
}

static void nvme_doorbell(int admin, unsigned tag)
{
    uint64_t sqb = admin ? M.asq : M.ioq_cmds, tcbb = admin ? M.asq_tcb : M.iosq_tcb;
    if (!admin && !(M.io_cq_created && M.io_sq_created)) { VIOL("IO doorbell before queues created"); return; }
    if (!admin && !M.io_registered) { VIOL("IO doorbell before 0x1208/0x1200/0x1210"); return; }
    if (tag >= 63 + 1) { VIOL("tag %u", tag); return; }
    if (M.inval_expected && M.inval_tags == 0) { /* previous completion never invalidated */ }
    const uint8_t *sq = dv(sqb) + tag * 64, *tcb = dv(tcbb) + tag * 128;
    uint8_t op = sq[0];
    uint64_t prp1 = 0, prp2 = 0, t1 = 0, t2 = 0;
    memcpy(&prp1, sq + 24, 8); memcpy(&prp2, sq + 32, 8); memcpy(&t1, tcb + 24, 8); memcpy(&t2, tcb + 32, 8);
    if (prp1 != t1 || prp2 != t2) VIOL("TCB prp != SQE op %02x", op);
    unsigned want_flags = !prp1 ? 0 : (op & 1) ? 2 : 1;
    if (tcb[1] != want_flags) VIOL("TCB dma_flags %u want %u op %02x", tcb[1], want_flags, op);
    if (tcb[0] != 0 || tcb[2] != tag) VIOL("TCB opcode/tag");
    if (admin ? tag != 0 : tag != 2) VIOL("tag %u on %s queue (Linux: admin 0, IO 2)", tag, admin ? "admin" : "IO");
    if (M.num_tcbs != 63) VIOL("NUM_TCBS %u", M.num_tcbs);
    unsigned cdw10 = 0, cdw11 = 0, cdw12 = 0;
    memcpy(&cdw10, sq + 40, 4); memcpy(&cdw11, sq + 44, 4); memcpy(&cdw12, sq + 48, 4);
    unsigned nsid = 0; memcpy(&nsid, sq + 4, 4);
    if (fault == F_NO_COMPLETE && !admin) return;
    if (admin) {
        M.admin_cmds++;
        if (op == 0x06) {
            uint8_t *buf = dv(prp1);
            memset(buf, 0, 4096);
            if (cdw10 == 1) { buf[77] = 8; }               /* mdts: 2^8 pages = 1 MiB */
            else {
                uint64_t nsze = 244276265ull; memcpy(buf, &nsze, 8);
                buf[26] = 1;                                 /* flbas -> lbaf 1 */
                buf[128 + 4 * 1 + 2] = fault == F_BAD_LBADS ? 9 : 12;
                if (nsid != 1) VIOL("identify ns nsid %u", nsid);
            }
            complete(1, tag, 0);
        } else if (op == 0x05) {
            if (prp1 != M.asq + 0x3000) VIOL("create CQ prp1");
            if ((cdw10 & 0xffff) != 1 || (cdw10 >> 16) != 63 || cdw11 != 3) VIOL("create CQ cdw %x %x", cdw10, cdw11);
            M.io_cq_created = 1; complete(1, tag, 0);
        } else if (op == 0x01) {
            if (!M.io_cq_created) VIOL("create SQ before CQ");
            if (prp1 != M.asq + 0x2000) VIOL("create SQ prp1");
            if ((cdw10 & 0xffff) != 1 || (cdw10 >> 16) != 63 || cdw11 != (1 | (1u << 16))) VIOL("create SQ cdw");
            M.io_sq_created = 1; complete(1, tag, 0);
        } else if (op == 0x00) {
            M.io_sq_created = 0; M.io_registered = 0; complete(1, tag, 0);
        } else if (op == 0x04) {
            if (fault == F_NO_CQ_DELETE) return;
            M.io_cq_created = 0; complete(1, tag, 0);
        } else {
            VIOL("unexpected admin opcode %02x", op);
        }
    } else {
        M.io_cmds++;
        if (op != 0x02) { VIOL("non-read IO opcode %02x", op); return; }
        uint64_t lba = cdw10 | (uint64_t)cdw11 << 32;
        unsigned n = (cdw12 & 0xffff) + 1;
        unsigned tl = tcb[4] | (unsigned)tcb[5] << 8;
        if (tl != n - 1) VIOL("TCB length %u vs %u", tl, n - 1);
        if (!(allow_gpt_reads && lba + n <= 8) && (lba < L1_P4_FIRST || lba + n > L1_P4_END))
            VIOL("read outside p4: %llu+%u", (unsigned long long)lba, n);
        if (fault == F_READ_ERR && M.reads_ok >= 1) { complete(0, tag, 0x2); return; }
        /* PRP walk */
        uint64_t page[1024]; unsigned pn = 0;
        page[pn++] = prp1;
        if (n == 2) page[pn++] = prp2;
        else if (n > 2) {
            const uint8_t *list = dv(prp2);
            for (unsigned i = 1; i < n; i++) { memcpy(&page[pn++], list + 8 * (i - 1), 8); }
        }
        for (unsigned i = 0; i < n; i++) {
            if (page[i] < (uint64_t)(uintptr_t)U || page[i] + 4096 > (uint64_t)(uintptr_t)U + UNI) { VIOL("PRP outside universe"); return; }
            memcpy(dv(page[i]), disk_block(lba + i), 4096);
        }
        M.reads_ok++;
        complete(0, tag, 0);
    }
}

/* ---- register file ------------------------------------------------------- */
static uint32_t f_r32(void *c, uint64_t a)
{
    (void)c;
    persist_check(a);
    maybe_abort();
    int pi = pm_find(a);
    if (pi >= 0) {
        return M.pm[pi];
    }
    if (pm_range(a)) { VIOL("read of a PMGR register the DT does not list: %llx", (unsigned long long)a); return 0; }
    /* only registers Linux itself reads while the coprocessor has never been started on this boot: CPU_CONTROL, SART.
     * Mailbox and NVMe registers of a stopped ANS are read by nobody; a bus hang is not contained by any guard. */
    if ((M.pm[PM_IDX_ANS] & 0x80000400u) && ((a >= ASC && a < ASC + 0x4000) || (a >= SART && a < SART + 0xc000)))
        VIOL("read of %llx while the ANS block is held in RESET/DEV_DISABLE", (unsigned long long)a);
    if (!M.ever_run && ((a >= NVME && a < NVME + 0x10000) || (a >= MBOX && a < MBOX + 0x4000)))
        VIOL("read of %llx before the ANS coprocessor was started", (unsigned long long)a);
    if (a == ASC + L1R_CPU_CONTROL) return M.cpu_ctrl;
    if (a == ASC + 0x8000 + L1R_A2I_CONTROL) return 0;
    if (a == ASC + 0x8000 + L1R_I2A_CONTROL) return M.ih == M.it ? (1u << 17) : 0;
    if (a >= SART && a < SART + 0xc0) {
        unsigned o = (unsigned)(a - SART);
        if (o < 0x40) return M.sart_cfg[o / 4];
        if (o < 0x80) return M.sart_pa[(o - 0x40) / 4];
        return M.sart_sz[(o - 0x80) / 4];
    }
    if (a == NVME + 0x00) return 0x1fff;
    if (a == NVME + 0x04) return 0;
    if (a == NVME + L1R_NVME_CC) return M.cc;
    if (a == NVME + L1R_NVME_CSTS) {
        uint32_t s = 0;
        if (M.cc & 1) s |= fault == F_NO_RDY ? 0 : 1;
        if (M.shn) s |= 2u << 2;
        return s;
    }
    if (a == NVME + L1R_NVME_BOOT_STATUS) return (M.boot_ok && fault != F_NO_BOOT_STATUS) ? L1R_BOOT_STATUS_OK : 0;
    if (a == NVMMU + L1R_NVMMU_TCB_STAT) return 0;
    return 0;
}

static void f_w32_in(void *c, uint64_t a, uint32_t v)
{
    (void)c;
    int pi = pm_find(a);
    if (pi < 0 && pm_range(a)) { VIOL("write to a PMGR register the DT does not list: %llx", (unsigned long long)a); return; }
    if (pi >= 0) {
        uint32_t was = M.pm[pi];
        M.pm_writes++;
        if (!PM_OWN[pi]) { M.pm_parent_writes++; VIOL("write to parent domain PM[%d] = %08x", pi, v); }
        if ((v & 0x80000000u) && !(was & 0x80000000u)) {
            M.ans_reset_count++; ev("PM.RESET assert");
            M.cc = 0; M.shn = 0; M.io_registered = 0; M.io_cq_created = M.io_sq_created = 0;   /* a PMGR reset resets the NVMe block too */
        }
        if ((was & 0x80000000u) && !(v & 0x80000000u)) { ev("PM.RESET deassert"); if (M.cpu_ctrl & 0x10) { /* reset stops the CPU */ M.cpu_ctrl = 0; M.alive = 0; } }
        M.pm[pi] = (v & ~0xf0u) | ((v & 0xf) << 4);
        ev("PM[%u]=%08x", pi, v);
        return;
    }
    if (a == ASC + L1R_CPU_CONTROL) {
        ev("ASC.CPU_CONTROL=%x", v);
        if (v & 0x10) M.ever_run = 1;
        if (!v) M.alive = 0;
        M.cpu_ctrl = v;
        return;
    }
    if (a >= SART && a < SART + 0xc0) {
        unsigned o = (unsigned)(a - SART);
        if (o < 0x40) { M.sart_cfg[o / 4] = v; ev("SART.CFG[%u]=%x", o / 4, v); }
        else if (o < 0x80) { M.sart_pa[(o - 0x40) / 4] = v; ev("SART.PA[%u]=%x", (o - 0x40) / 4, v); }
        else { M.sart_sz[(o - 0x80) / 4] = v; ev("SART.SZ[%u]=%x", (o - 0x80) / 4, v); }
        return;
    }
    if (a >= NVME && a < NVME + 0x10000) {
        unsigned o = (unsigned)(a - NVME);
        switch (o) {
        case L1R_NVME_CC:
            ev("NVME.CC=%08x", v);
            if ((v & 0xc000) == 0x4000) M.shn = 1;
            if (!(v & 1)) { M.shn = (v & 0xc000) == 0x4000; }
            M.cc = v;
            if (!(v & 1) && !(M.cc & 0x4000)) M.shn = 0;
            break;
        case L1R_NVME_AQA: M.aqa = v; ev("NVME.AQA=%x", v); break;
        case L1R_NVME_ASQ: M.asq = (M.asq & ~0xffffffffull) | v; ev("NVME.ASQ.lo"); break;
        case L1R_NVME_ASQ + 4: M.asq = (M.asq & 0xffffffffull) | (uint64_t)v << 32; ev("NVME.ASQ.hi"); break;
        case L1R_NVME_ACQ: M.acq = (M.acq & ~0xffffffffull) | v; ev("NVME.ACQ.lo"); break;
        case L1R_NVME_ACQ + 4: M.acq = (M.acq & 0xffffffffull) | (uint64_t)v << 32; ev("NVME.ACQ.hi"); break;
        case L1R_NVME_ACQ_DB: ev("NVME.ACQ_DB=%u", v); break;
        case L1R_NVME_IOCQ_DB: ev("NVME.IOCQ_DB=%u", v); break;
        case L1R_NVME_IOQ_CQES: M.ioq_cqes = (M.ioq_cqes & ~0xffffffffull) | v; ev("NVME.IOQ_CQES.lo"); break;
        case L1R_NVME_IOQ_CQES + 4: M.ioq_cqes = (M.ioq_cqes & 0xffffffffull) | (uint64_t)v << 32; ev("NVME.IOQ_CQES.hi"); break;
        case L1R_NVME_IOQ_CMDS: M.ioq_cmds = (M.ioq_cmds & ~0xffffffffull) | v; ev("NVME.IOQ_CMDS.lo"); break;
        case L1R_NVME_IOQ_CMDS + 4: M.ioq_cmds = (M.ioq_cmds & 0xffffffffull) | (uint64_t)v << 32; ev("NVME.IOQ_CMDS.hi"); break;
        case L1R_NVME_IOQ_SIZE:
            M.ioq_size = v; ev("NVME.IOQ_SIZE=%x", v);
            if (M.ioq_cqes && M.ioq_cmds && v == (63u | 63u << 16)) M.io_registered = 1;
            break;
        default:
            VIOL("write to unknown NVMe reg %x", o);
        }
        return;
    }
    if (a >= NVMMU && a < NVMMU + 0x30000) {
        unsigned o = (unsigned)(a - NVMMU);
        switch (o) {
        case L1R_NVMMU_NUM_TCBS: M.num_tcbs = v; ev("NVMMU.NUM_TCBS=%u", v); break;
        case L1R_NVMMU_ASQ_TCB: M.asq_tcb = (M.asq_tcb & ~0xffffffffull) | v; break;
        case L1R_NVMMU_ASQ_TCB + 4: M.asq_tcb = (M.asq_tcb & 0xffffffffull) | (uint64_t)v << 32; ev("NVMMU.ASQ_TCB"); break;
        case L1R_NVMMU_IOSQ_TCB: M.iosq_tcb = (M.iosq_tcb & ~0xffffffffull) | v; break;
        case L1R_NVMMU_IOSQ_TCB + 4: M.iosq_tcb = (M.iosq_tcb & 0xffffffffull) | (uint64_t)v << 32; ev("NVMMU.IOSQ_TCB"); break;
        case L1R_NVMMU_TCB_INVAL: M.last_inval = v; M.inval_tags++; M.inval_expected = 0; ev("NVMMU.TCB_INVAL=%u", v); break;
        case L1R_NVMMU_ASQ_DB: ev("NVMMU.ASQ_DB=%u", v); if (M.alive) nvme_doorbell(1, v); break;
        case L1R_NVMMU_IOSQ_DB: ev("NVMMU.IOSQ_DB=%u", v); if (M.alive) nvme_doorbell(0, v); break;
        case 0x24908: VIOL("LINEAR_SQ_CTRL written (fw26 must not)"); break;
        default: VIOL("write to unknown NVMMU reg %x", o);
        }
        return;
    }
    if (a >= ASC + 0x8000 && a < ASC + 0x9000) { VIOL("32-bit write to mailbox %x", (unsigned)(a - ASC)); return; }
    VIOL("write32 to unmapped %llx", (unsigned long long)a);
}

static uint64_t f_r64(void *c, uint64_t a)
{
    (void)c;
    persist_check(a);
    maybe_abort();
    if (a == ASC + 0x8000 + L1R_I2A_RECV0) {
        if (M.ih == M.it) { VIOL("RECV0 on empty FIFO"); return 0; }
        return M.i2a[M.ih & 255].m0;
    }
    if (a == ASC + 0x8000 + L1R_I2A_RECV1) {
        if (M.ih == M.it) { VIOL("RECV1 on empty FIFO"); return 0; }
        return M.i2a[M.ih++ & 255].ep;
    }
    VIOL("read64 from %llx", (unsigned long long)a);
    return 0;
}
static void f_w64_in(void *c, uint64_t a, uint64_t v)
{
    (void)c;
    if (a == ASC + 0x8000 + L1R_A2I_SEND0) { M.a2i0 = v; return; }
    if (a == ASC + 0x8000 + L1R_A2I_SEND1) {
        if (M.alive || ((M.cpu_ctrl & 0x10) && ((M.a2i0 >> 52) & 0xff) == 6 && (M.a2i0 & 0xffff) == 0x220 && (uint32_t)v == 0))
            model_rx(M.a2i0, (uint32_t)v);
        return;
    }
    VIOL("write64 to %llx", (unsigned long long)a);
}

static void f_w32(void *c, uint64_t a, uint32_t v)
{
    persist_check(a);
    int ab = abort_due();
    if (ab && !fault_after_effect) abort_hook();
    f_w32_in(c, a, v);
    if (ab && fault_after_effect) abort_hook();
}
static void f_w64(void *c, uint64_t a, uint64_t v)
{
    persist_check(a);
    int ab = abort_due();
    if (ab && !fault_after_effect) abort_hook();
    f_w64_in(c, a, v);
    if (ab && fault_after_effect) abort_hook();
}

/* ---- cache model: CPU view U, device view D ----------------------------- */
static void f_clean(const void *p, size_t n)
{
    size_t off = (size_t)((const uint8_t *)p - U);
    if (off + n > UNI) { VIOL("clean outside universe"); return; }
    memcpy(D + off, U + off, n);
}
static void f_inval(const void *p, size_t n)
{
    size_t off = (size_t)((const uint8_t *)p - U);
    if (off + n > UNI) { VIOL("inval outside universe"); return; }
    memcpy(U + off, D + off, n);
}
static unsigned syslog_count;
static char syslog_first[96];
static void f_syslog(void *c, const char *l) { (void)c; if (!syslog_count++) snprintf(syslog_first, sizeof(syslog_first), "%s", l); }
static void f_trace(void *c, const char *s, uint64_t v) { (void)c; ev("T:%s=%llx", s, (unsigned long long)v); }

static const struct l1_hal HAL = {f_r32, f_w32, f_r64, f_w64, f_udelay, f_now, f_clean, f_inval, f_syslog, f_trace, 0};

/* ANS as Linux needs it after our hand-back: stopped (RUN clear, no RESET/DEV_DISABLE, controller disabled, our SART
 * entries removed), or (R44, wake start) still RUNNING as iBoot left it: RUN set, RTKit alive and ON (no power message
 * sent), controller disabled, iBoot's SART entries untouched and ours left in place; or a wake start that never touched
 * ANS (below). Returns 1 if clean, else 0 with the reason in why. */
static int ans_state_ok(char *why, size_t cap)
{
    why[0] = 0;
    if (M.pm[PM_IDX_ANS] & 0x80000400u) { snprintf(why, cap, "ANS PMGR left in RESET/DEV_DISABLE (%08x)", M.pm[PM_IDX_ANS]); return 0; }
    /* wake start and INIT never sent: ANS exactly as iBoot left it (running, controller enabled): nothing to hand back */
    int untouched = wake_start && M.inits == 0 && M.cpu_ctrl == 0x10 && M.iop_state == 0x20 && M.cc == 0x460001 && M.pm_writes == 0;
    int running = wake_start && M.cpu_ctrl == 0x10 && M.alive && M.iop_state == 0x20 && M.ap_state == 0x20 && M.inits == 1 && M.pm_writes == 0 &&
                  M.ans_reset_count == 0 && !(M.cc & 1);
    if (M.cc & 1 && !untouched) { snprintf(why, cap, "CC.EN set"); return 0; }
    if (M.cpu_ctrl && !untouched && !running) { snprintf(why, cap, "CPU_CONTROL=%x", M.cpu_ctrl); return 0; }
    for (unsigned i = 0; i < 16; i++) {
        int keep = (M.sart_init_cfg[i] & 0xff) != 0;
        if (keep ? (M.sart_cfg[i] != M.sart_init_cfg[i] || M.sart_pa[i] != M.sart_init_pa[i] || M.sart_sz[i] != M.sart_init_sz[i])
                 : (!running && ((M.sart_cfg[i] & 0xff) || M.sart_pa[i] || M.sart_sz[i]))) {
            snprintf(why, cap, "SART[%u] %s", i, keep ? "iBoot entry changed" : "left");
            return 0;
        }
    }
    if (M.violations) { snprintf(why, cap, "violation: %s", M.viol[0]); return 0; }
    return 1;
}

static void model_reset(void)
{
    memset(&M, 0, sizeof(M));
    pm_n = 0; memset(pm_seen, 0, sizeof(pm_seen));
    /* iBoot's state: the parents (AFC, FAB3_SOC, APCIE_ST0/1) are on; the nvme node's own domains are on unless a
     * scenario says otherwise (F_OWN_OFF); F_PARENT_OFF switches AFC off */
    for (unsigned i = 0; i < 7; i++) M.pm[i] = (PM_OWN[i] && fault == F_OWN_OFF) ? 0 : 0xff;
    if (fault == F_PARENT_OFF) M.pm[0] = 0;
    if (fault == F_ANS_DEV_DISABLE) M.pm[PM_IDX_ANS] |= 0x400u;           /* ON, DEV_DISABLE set */
    if (fault == F_ANS_RESET_HELD) M.pm[PM_IDX_ANS] |= 0x80000000u;       /* ON, RESET set */
    if (fault == F_ANS_OFF_RESET) M.pm[PM_IDX_ANS] = 0x80000000u;         /* OFF, RESET set: power-on keeps bit 31 */
    if (wake_start) {   /* the state the R41 native probe saw: every domain ON, RUN set, SART 0-4 and 6 configured */
        M.cpu_ctrl = 0x10; M.alive = 1; M.ever_run = 1; M.boot_ok = 1; M.iop_state = 0x20; M.ap_state = 0x20;
        M.cc = 0x460001;
        static const int used[] = {0, 1, 2, 3, 4, 6};
        for (unsigned i = 0; i < sizeof(used) / sizeof(*used); i++) {
            int k = used[i];
            M.sart_cfg[k] = 0xff; M.sart_pa[k] = 0x80000 + 0x100 * k; M.sart_sz[k] = 4 + k;
            M.sart_init_cfg[k] = 0xff; M.sart_init_pa[k] = M.sart_pa[k]; M.sart_init_sz[k] = M.sart_sz[k];
        }
    }
    if (fault == F_ANS_STOPPED) { M.cpu_ctrl = 0; M.alive = 0; M.boot_ok = 0; M.iop_state = M.ap_state = 0; M.cc = 0; }
    access_n = 0; fault2_at = -1;
    M.admin_phase = M.io_phase = 1;
    memset(U, 0xAA, UNI);                                      /* CPU-side poison */
    memset(D, 0x00, UNI);
    tn = 0; t_us = 1000; syslog_count = 0; syslog_first[0] = 0;
}


#endif

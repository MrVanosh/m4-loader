/* SPDX-License-Identifier: MIT */
/* Level 1 (R46): ANS (Apple NVMe) wake of the session iBoot left running, NVMe fw26 bring-up, read-only IO and
 * the hand-back that leaves ANS running. Follows the running Linux driver step by step:
 *   drivers/nvme/host/apple.c  apple_nvme_reset_work / apple_nvme_disable
 *   drivers/soc/apple/rtkit.c  apple_rtkit_wake / apple_rtkit_boot / buffer requests
 *   drivers/soc/apple/mailbox.c asc-mailbox-v4 FIFOs
 *   drivers/soc/apple/sart.c   SARTv3 layout
 *   drivers/pmdomain/apple/pmgr-pwrstate.c  power on / reset assert+deassert
 * All register access goes through struct l1_hal so the same file runs on the
 * host against a scripted fake (register-trace test). */
#ifndef M4_LEVEL1_ANS_H
#define M4_LEVEL1_ANS_H
#include "level1.h"

struct l1_hal {
    /* every access goes through l1_ans's checked accessors first (allow-list of DT-derived windows) */
    uint32_t (*r32)(void *ctx, uint64_t addr);
    void (*w32)(void *ctx, uint64_t addr, uint32_t v);
    uint64_t (*r64)(void *ctx, uint64_t addr);
    void (*w64)(void *ctx, uint64_t addr, uint64_t v);
    void (*udelay)(uint32_t us);
    uint64_t (*now_us)(void);
    void (*clean)(const void *p, size_t n);  /* write back (before the device reads) */
    void (*inval)(const void *p, size_t n);  /* discard (after the device wrote) */
    void (*syslog)(void *ctx, const char *line); /* ANS syslog text, already trimmed */
    void (*trace)(void *ctx, const char *step, uint64_t value); /* progress */
    void *ctx;
};

#define L1_PM_MAX 12
/* R41: every address comes from the Linux DT template (nvme node reg/reg-names, mboxes and apple,sart
 * phandles, power-domains and their parents) and is cross-checked against this Mac's ADT before use. */
struct l1_ans_cfg {
    uint64_t asc, asc_size;     /* nvme node reg "ans": CPU_CONTROL at +0x44 */
    uint64_t mbox, mbox_size;   /* the mailbox node named by mboxes (asc + 0x8000 on this Mac) */
    uint64_t nvme, nvme_size;   /* nvme node reg "nvme" */
    uint64_t nvmmu, nvmmu_size; /* nvme node reg "nvmmu" */
    uint64_t sart, sart_size;   /* the node named by apple,sart */
    unsigned pm_count;          /* PMGR power-state registers, parents first */
    uint64_t pm_reg[L1_PM_MAX];
    uint8_t pm_own[L1_PM_MAX];  /* 1: a power domain of the nvme node, 0: a parent (the ADT cross-check uses it; no power register is ever written) */
    uint64_t pm_ans;            /* the ANS PMGR register (reset target) */
};

/* DMA memory layout inside the caller's region (16 KiB aligned, >= L1_ANS_MEM). */
#define L1_ANS_MEM 0x110000u        /* rings/TCBs + 1 MiB firmware buffers */
#define L1_ANS_MEM_MIN 0x50000u     /* fallback: 256 KiB of firmware buffers */
#define L1_SHMEM_OFF 0x10000u

struct l1_ans_stats {
    uint32_t ms_ans_boot, ms_nvme_ready, ms_read, ms_shutdown;
    uint32_t reads, blocks, sart_added, sart_removed, syslog_lines, rtkit_version;
    uint32_t cq_phase_flips;
    uint32_t writes;            /* rev 17: one-shot token blocks written (0 or 1) */
    uint8_t power_on, cpu_started, rtkit_up, nvme_enabled, ioq_up;
    uint8_t reset_touched;      /* the ANS PMGR reset sequence was begun and not finished (hand-back deasserts) */
    uint8_t iop_woken;          /* SET_IOP_PWR_STATE(INIT) was (about to be) sent */
    uint8_t left;               /* handed back running (R44): RUN set, RTKit up, our SART entries kept */
    uint8_t left_rdy;           /* CSTS.RDY as the hand-back left it */
    uint32_t sart_kept;         /* SART entries left in place by the running hand-back */
};

/* allow-list: one window per DT-derived device block, 4 bytes per power register */
#define L1_WIN_MAX (6 + L1_PM_MAX)

/* read-only probe items: 0..pm_count-1 are the power registers, then these (device registers Linux reads in that state) */
enum {
    L1P_CPU = 0,
    L1P_SART0, /* .. L1P_SART0 + 15 */
    L1P_DEV_COUNT = L1P_SART0 + 16
};

struct l1_ans {
    const struct l1_hal *hal;
    struct l1_ans_cfg cfg;
    struct { uint64_t b, e; } win[L1_WIN_MAX];
    unsigned nwin;
    int dead;               /* a refused access: no further hardware access, all clocks run out */
    uint64_t bad_addr;      /* the refused address */
    uint64_t last_addr;     /* last register touched (attribution of a fault) */
    uint32_t nacc;          /* accesses made so far */
    uint8_t last_wr;
    const char *why;        /* short reason of the last failing step (static string) */
    unsigned bad_idx;       /* power register that failed / was not on */
    uint8_t *mem;           /* DMA region (identity mapped: pointer == bus address) */
    /* RTKit */
    uint32_t iop_state, ap_state;
    int epmap_done, hello_seen, crashed, crash_buf;
    uint32_t iop_ack, ap_ack;
    uint8_t eps[32];        /* bitmap of discovered endpoints (256 bits) */
    uint8_t live_eps[16];   /* system endpoints started, in start order: /chosen m4,ans-live when ANS is left live */
    uint8_t live_n;
    uint64_t shmem_used, shmem_size;
    struct { uint64_t pa; uint32_t size; uint8_t used; } sart_ent[16];
    uint16_t sart_protected;
    uint64_t sys_buf, sys_size; uint32_t sys_entries, sys_msg_size;
    /* NVMe */
    uint32_t cc;
    uint16_t admin_head, io_head; uint8_t admin_phase, io_phase;
    uint32_t mdts_blocks;
    uint64_t t0_us, deadline_us, rtk_deadline_us;   /* deadline_us: absolute, 0 = none (whole SSD attempt) */
    struct l1_ans_stats st;
};

/* The bring-up in the steps the loader prints and guards separately. l1_ans_bringup() runs all four. */
enum l1_status l1_ans_power(struct l1_ans *a);       /* every domain of the nvme node must already be ON (nothing is written) */
enum l1_status l1_ans_check_cpu(struct l1_ans *a);   /* CPU_CONTROL.RUN set: stop, touch nothing */
enum l1_status l1_ans_start(struct l1_ans *a);       /* SART snapshot, reset, RUN, RTKit, boot status */
enum l1_status l1_ans_nvme(struct l1_ans *a);        /* NVMMU, enable, identify, IO queues */
enum l1_status l1_ans_bringup(struct l1_ans *a);
/* One read-only probe item (see L1P_*); 0 or -1 (address refused). A faulting read faults in the caller's guard. */
unsigned l1_ans_probe_count(const struct l1_ans *a);
int l1_ans_probe(struct l1_ans *a, unsigned item, uint32_t *v, uint64_t *addr);
enum l1_status l1_ans_leave(struct l1_ans *a);      /* hand ANS back RUNNING: queues deleted, EN=0, RDY clear, SART kept */
void l1_ans_emergency(struct l1_ans *a);       /* best-effort clean hand-off after a failure */
void l1_ans_stop(struct l1_ans *a);            /* last resort: reset deasserted, RUN clear, then SART entries removed */
void l1_ans_release(struct l1_ans *a);         /* rev 17: after leave, m1n1 entry: our SART entries removed, ANS running */
int l1_ans_read(void *a, uint64_t lba, uint32_t n, void *dst); /* struct l1_dev callback */
int l1_ans_write(void *a, uint64_t lba, const void *src);     /* rev 17: one block + Flush (struct l1_dev write) */
void l1_ans_init(struct l1_ans *a, const struct l1_hal *hal, const struct l1_ans_cfg *cfg, void *mem, size_t mem_bytes);

/* Register map (nvme-apple fw26 on J616s); exported for the trace test. */
#define L1R_CPU_CONTROL 0x44
#define L1R_CPU_RUN 0x10
#define L1R_MBOX 0x8000
#define L1R_A2I_CONTROL 0x110
#define L1R_A2I_SEND0 0x800
#define L1R_A2I_SEND1 0x808
#define L1R_I2A_CONTROL 0x114
#define L1R_I2A_RECV0 0x830
#define L1R_I2A_RECV1 0x838
#define L1R_NVME_CC 0x14
#define L1R_NVME_CSTS 0x1c
#define L1R_NVME_AQA 0x24
#define L1R_NVME_ASQ 0x28
#define L1R_NVME_ACQ 0x30
#define L1R_NVME_ACQ_DB 0x1004
#define L1R_NVME_IOCQ_DB 0x100c
#define L1R_NVME_IOQ_CMDS 0x1200
#define L1R_NVME_IOQ_CQES 0x1208
#define L1R_NVME_IOQ_SIZE 0x1210
#define L1R_NVME_BOOT_STATUS 0x1300
#define L1R_NVMMU_ASQ_DB 0x2490c
#define L1R_NVMMU_IOSQ_DB 0x24910
#define L1R_NVMMU_NUM_TCBS 0x28100
#define L1R_NVMMU_ASQ_TCB 0x28108
#define L1R_NVMMU_IOSQ_TCB 0x28110
#define L1R_NVMMU_TCB_INVAL 0x28118
#define L1R_NVMMU_TCB_STAT 0x28120
#define L1R_BOOT_STATUS_OK 0xde71ce55u

#endif

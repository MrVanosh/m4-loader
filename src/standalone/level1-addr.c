/* SPDX-License-Identifier: MIT */
/* R42: this Mac's ADT cross-check of the DT-derived addresses, as loader-side code (not part of the 32 KiB blob): called
 * through the host table (l1_host.adt_check). R41 had it inside the blob; the R42 wake path needed the bytes.
 * Same code and return codes as R41; the e2e and shim harnesses link this file.
 *
 * int l1_adt_check(h, cfg): 0 = this Mac's ADT declares every address; else the number of the failed check
 * (status ans-power:addr-check:adt<N>): 1 /arm-io/ans missing, 2 nvme-secure-bar/linear-sq flags, 3 ASC reg[0],
 * 4 mailbox not asc+0x8000 or outside it, 5 NVMMU reg[3], 6 NVMe reg[9], 7 /arm-io/sart-ans, 8 sart-version != 3,
 * 9..11 /arm-io/pmgr devices/ps-regs, 12 a power register or the ANS/APCIE_SYS_ST0/1 device does not match. */
#include <stdint.h>
#include <stddef.h>
#include "level1-ans.h"
#include "level1-host.h"

#define PMGR_FLAG_VIRTUAL 0x10

/* ------------------------------------------------------------ ADT: what this Mac declares
 * R39 took every address from here. R41 takes them from the Linux DT (below) and only uses this to
 * refuse anything this Mac's ADT does not declare: the device windows must match / lie inside the
 * ADT's reg ranges of /arm-io/ans and /arm-io/sart-ans, and every power register must be the
 * power-state register of some device in /arm-io/pmgr's device list (ps-regs/ps-groups + offset). */

static inline uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

static int name_is(const uint8_t *n, const char *w)
{
    unsigned k = 0;
    while (k < 16 && w[k] && n[k] == (uint8_t)w[k]) k++;
    return (k == 16 || !w[k]) && (k == 16 || !n[k]);
}

int l1_adt_check(const struct l1_host *h, const struct l1_ans_cfg *d)
{
    int path[8], sp[8], pp[8];
    uint32_t len = 0;
    uint64_t b, s;
    int n = h->adt_path_offset_trace(h->adt, "/arm-io/ans", path);
    if (n < 0) return 1;
    if (!h->adt_getprop(h->adt, n, "nvme-secure-bar", &len) || !h->adt_getprop(h->adt, n, "nvme-linear-sq", &len)) return 2;
    /* the reg[] indices R39 and the Linux driver use: 0 ASC wrapper, 3 NVMMU, 9 NVMe */
    if (h->adt_get_reg(h->adt, path, "reg", 0, &b, &s) < 0 || b != d->asc || d->asc_size > s) return 3;
    if (d->mbox != d->asc + L1R_MBOX || !d->mbox_size || d->mbox + d->mbox_size > b + s) return 4;
    if (h->adt_get_reg(h->adt, path, "reg", 3, &b, &s) < 0 || b != d->nvmmu || d->nvmmu_size > s) return 5;
    if (h->adt_get_reg(h->adt, path, "reg", 9, &b, &s) < 0 || b != d->nvme || d->nvme_size > s) return 6;
    int sn = h->adt_path_offset_trace(h->adt, "/arm-io/sart-ans", sp);
    if (sn < 0) return 7;
    const uint8_t *v = h->adt_getprop(h->adt, sn, "sart-version", &len);
    if (!v || len != 4 || rd32(v) != 3) return 8;
    if (h->adt_get_reg(h->adt, sp, "reg", 0, &b, &s) < 0 || b != d->sart || d->sart_size > s) return 7;
    /* PMGR: every register of the Linux DT must be a power-state register this ADT names */
    int pn = h->adt_path_offset_trace(h->adt, "/arm-io/pmgr", pp);
    if (pn < 0) return 9;
    const uint8_t *dev = h->adt_getprop(h->adt, pn, "devices", &len);
    if (!dev || len < 96) return 10;
    uint32_t cnt = len / 48;
    int group = 0;
    const uint32_t *psg = h->adt_getprop(h->adt, pn, "ps-regs", &len);
    if (!psg || !len) {
        group = 1;
        psg = h->adt_getprop(h->adt, pn, "ps-groups", &len);
        if (!psg || !len) return 11;
    }
    uint64_t die_off = ((d->nvme >> 37) & 3) * 0x2000000000ull;
    uint32_t found = 0, named = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *r = dev + 48 * i;
        uint32_t idx, ro, off;
        if (r[0] & PMGR_FLAG_VIRTUAL) continue;
        if (group) {
            uint32_t g = rd32(r + 16) >> 24;
            off = rd32(r + 16) & 0xffffff; idx = psg[3 * g]; ro = psg[3 * g + 1];
        } else {
            idx = psg[3 * r[11]]; ro = psg[3 * r[11] + 1]; off = (uint32_t)r[10] << 3;
        }
        if (h->adt_get_reg(h->adt, pp, "reg", (int)idx, &b, 0) < 0) continue;
        uint64_t a = b + ro + off + die_off;
        for (unsigned k = 0; k < d->pm_count; k++) if (d->pm_reg[k] == a) found |= 1u << k;
        /* the three domains of the nvme node are named in the ADT: that device's register must be one of ours */
        unsigned which = name_is(r + 32, "ANS") ? 1 : name_is(r + 32, "APCIE_SYS_ST0") ? 2 : name_is(r + 32, "APCIE_SYS_ST1") ? 4 : 0;
        if (which) {
            unsigned k = 0;
            while (k < d->pm_count && !(d->pm_reg[k] == a && d->pm_own[k])) k++;
            if (k < d->pm_count) named |= which;
        }
    }
    if (!d->pm_count || found != (1u << d->pm_count) - 1 || named != 7) return 12;
    return 0;
}


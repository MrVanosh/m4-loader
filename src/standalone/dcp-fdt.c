/* SPDX-License-Identifier: MIT */
/* J616s internal-panel DCP nodes from the accepted display capture.
 * RAM/FDT work only: no DART, mailbox or DCP access. Every ADT value the
 * nodes depend on is checked against the qualified 26.6.2 layout first; any
 * mismatch leaves the tree as it was apart from /chosen/m4,dcp-fdt-status.
 *
 * Built only when M4_DCP_STAGE is set (the default loader is unchanged).
 * The stage selects what Linux may probe:
 *   1  nodes and reserved mappings only, everything disabled
 *   2  also enable both locked DARTs (checks mapping preservation)
 *   3  also enable mailbox, DCP and display-subsystem (M1 attach) */
#include "display-handoff.h"
#include "fixups.h"
#include <stdio.h>
#include "adt.h"
#include "libfdt.h"
#include "utils.h"
#include <stdint.h>
#include <string.h>

#if !defined(M4_DCP_STAGE) || M4_DCP_STAGE < 1 || M4_DCP_STAGE > 3
#error "build dcp-fdt.c only with M4_DCP_STAGE 1, 2 or 3"
#endif

#define ARMIO UINT64_C(0x200000000)
#define MAX_REGIONS 32u

/* ADT address/size pairs; physical = ADT + ARMIO. */
static const uint64_t dcp_reg[] = {0x202e00000, 0x88000};
static const uint64_t dcp_dart_reg[] = {0x202340000, 0x20000};
static const uint64_t disp0_dart_reg[] = {0x202300000, 0xc000};
static const uint64_t disp0_reg[] = {0x200000000, 0x4000000, 0x202240000, 0x10000,
                                     0x2023a8000, 0x4000, 0x202800000, 0x800000,
                                     0x304800000, 0xbc000};
/* ADT order is not-empty, empty per pair; emitted as empty, not-empty. */
static const uint32_t dcp_irq[] = {0x478, 0x477, 0x47a, 0x479};
static const char boot_firmware[] = "mBoot-18000.161.10";

struct region { uint64_t pa, size, iova[3]; uint32_t users; };

static int adt_bytes(const char *path, const char *key, const void *want, uint32_t bytes)
{
    uint32_t len;
    int node = adt_path_offset(adt, path);
    const void *p = node < 0 ? NULL : adt_getprop(adt, node, key, &len);
    return p && len >= bytes && !memcmp(p, want, bytes);
}

static int adt_regs(const char *path, const uint64_t *want, unsigned words)
{
    return adt_bytes(path, "reg", want, words * sizeof(*want));
}

static int adt_layout(void)
{
    const uint64_t range[] = {0, ARMIO};
    const uint32_t one = 1, dart_irq = 0x442, dcp_sid = 0x17, piodma_sid = 0x10;
    return adt_bytes("/chosen", "firmware-version", boot_firmware, sizeof(boot_firmware)) &&
           adt_bytes("/arm-io", "ranges", range, sizeof(range)) &&
           adt_regs("/arm-io/dcp", dcp_reg, 2) &&
           adt_bytes("/arm-io/dcp", "interrupts", dcp_irq, sizeof(dcp_irq)) &&
           adt_bytes("/arm-io/dcp/iop-dcp-nub", "running", &one, 4) &&
           adt_regs("/arm-io/dart-dcp", dcp_dart_reg, 2) &&
           adt_bytes("/arm-io/dart-dcp", "interrupts", &dart_irq, 4) &&
           adt_bytes("/arm-io/dart-dcp/mapper-dcp", "reg", &dcp_sid, 4) &&
           adt_regs("/arm-io/dart-disp0", disp0_dart_reg, 2) &&
           adt_bytes("/arm-io/dart-disp0", "interrupts", &dart_irq, 4) &&
           adt_bytes("/arm-io/dart-disp0/mapper-disp0-piodma", "reg", &piodma_sid, 4) &&
           adt_regs("/arm-io/disp0", disp0_reg, 10);
}

/* Merge identical spans across the dcp/disp0/piodma streams. A partial
 * overlap between streams is not a layout we have seen; refuse it. */
static int collect(const struct m4_display_handoff *h, struct region *out, unsigned *count)
{
    unsigned n = 0;
    for (unsigned s = 0; s < 3; s++) {
        const struct m4_dt_result *r = &h->result[s];
        for (unsigned j = 0; j < r->spans; j++) {
            const struct m4_dt_span *m = &r->map[j];
            unsigned k;
            for (k = 0; k < n; k++) {
                if (out[k].pa == m->physical && out[k].size == m->size) break;
                if (m->physical < out[k].pa + out[k].size && out[k].pa < m->physical + m->size)
                    return -2;
            }
            if (k == n) {
                if (n == MAX_REGIONS) return -3;
                out[n] = (struct region){m->physical, m->size, {0, 0, 0}, 0};
                n++;
            }
            if (out[k].users & (1u << s)) return -4;
            out[k].users |= 1u << s;
            out[k].iova[s] = m->iova;
        }
    }
    *count = n;
    return n ? 0 : -5;
}

static int reg_prop(void *t, int node, const uint64_t *pairs, unsigned n, uint64_t offset)
{
    fdt64_t v[12];
    if (n > 12) return -FDT_ERR_NOSPACE;
    for (unsigned i = 0; i < n; i++) v[i] = cpu_to_fdt64(pairs[i] + (i & 1 ? 0 : offset));
    return fdt_setprop(t, node, "reg", v, n * sizeof(*v));
}

static int u32s(void *t, int node, const char *name, const uint32_t *w, unsigned n)
{
    fdt32_t v[16];
    if (n > 16) return -FDT_ERR_NOSPACE;
    for (unsigned i = 0; i < n; i++) v[i] = cpu_to_fdt32(w[i]);
    return fdt_setprop(t, node, name, v, n * sizeof(*v));
}

static int strs(void *t, int node, const char *name, const char *list, int bytes)
{
    return fdt_setprop(t, node, name, list, bytes);
}

#define S(t, n, k, lit) strs(t, n, k, lit, sizeof(lit))
#define TRY(x) do { if ((ret = (x)) < 0) return ret; } while (0)

static int add(void *t, int parent, const char *name, uint32_t *phandle)
{
    int node = fdt_add_subnode(t, parent, name), ret;
    if (node < 0 || !phandle) return node;
    TRY(fdt_generate_phandle(t, phandle));
    TRY(fdt_setprop_u32(t, node, "phandle", *phandle));
    return node;
}

static const char *status(int enabled) { return enabled ? "okay" : "disabled"; }

static int build(void *t, const struct region *rg, unsigned count, uint64_t framebuffer)
{
    int ret, node, aic = fdt_path_offset(t, "/interrupt-controller@502400000");
    uint32_t aic_ph = aic < 0 ? 0 : fdt_get_phandle(t, aic);
    uint32_t pmgr_ph, disp_dart_ph, dcp_dart_ph, mbox_ph, dcp_ph, piodma_ph, display_ph;
    uint32_t region_ph[MAX_REGIONS];
    static char region_name[MAX_REGIONS][32];
    if (!aic_ph) return -FDT_ERR_NOTFOUND;
    if (fdt_path_offset(t, "/dcp@402e00000") >= 0) return -FDT_ERR_EXISTS;

    TRY(node = add(t, 0, "power-management@504800000", &pmgr_ph));
    TRY(reg_prop(t, node, &disp0_reg[8], 2, ARMIO));
    TRY(S(t, node, "reg-names", "dcp-fw-pmgr"));
    TRY(fdt_setprop_u32(t, node, "#apple,bw-scratch-cells", 3));

    /* Locked 3-level DARTs: Linux limits them to 36-bit IOVAs, so no
     * apple,dma-range; reserved IOVAs are the captured bare ones. */
    const struct { const char *name; const uint64_t *reg; uint32_t *ph; } darts[] = {
        {"iommu@402300000", disp0_dart_reg, &disp_dart_ph},
        {"iommu@402340000", dcp_dart_reg, &dcp_dart_ph}};
    for (unsigned i = 0; i < 2; i++) {
        const uint32_t irq[] = {0, 0x442, 4};
        TRY(node = add(t, 0, darts[i].name, darts[i].ph));
        TRY(S(t, node, "compatible", "apple,t6040-dart\0apple,t8110-dart"));
        TRY(reg_prop(t, node, darts[i].reg, 2, ARMIO));
        TRY(fdt_setprop_u32(t, node, "#iommu-cells", 1));
        TRY(fdt_setprop_u32(t, node, "interrupt-parent", aic_ph));
        TRY(u32s(t, node, "interrupts", irq, 3));
        TRY(fdt_setprop_string(t, node, "status", status(M4_DCP_STAGE >= 2)));
    }

    const uint32_t mbox_irq[] = {0, dcp_irq[1], 4, 0, dcp_irq[0], 4,
                                 0, dcp_irq[3], 4, 0, dcp_irq[2], 4};
    const uint64_t mbox_reg[] = {dcp_reg[0] + 0x8000, 0x4000};
    TRY(node = add(t, 0, "mailbox@402e08000", &mbox_ph));
    TRY(S(t, node, "compatible", "apple,t6040-asc-mailbox\0apple,asc-mailbox-v4"));
    TRY(reg_prop(t, node, mbox_reg, 2, ARMIO));
    TRY(fdt_setprop_u32(t, node, "interrupt-parent", aic_ph));
    TRY(u32s(t, node, "interrupts", mbox_irq, 12));
    TRY(S(t, node, "interrupt-names", "send-empty\0send-not-empty\0recv-empty\0recv-not-empty"));
    TRY(fdt_setprop_u32(t, node, "#mbox-cells", 0));
    TRY(fdt_setprop_string(t, node, "status", status(M4_DCP_STAGE >= 3)));

    /* coproc is the first 16K of the ASC block; disp-0..3 keep ADT order. */
    const uint64_t dcp_regs[] = {dcp_reg[0], 0x4000, disp0_reg[0], disp0_reg[1],
                                 disp0_reg[2], disp0_reg[3], disp0_reg[4], disp0_reg[5],
                                 disp0_reg[6], disp0_reg[7]};
    TRY(node = add(t, 0, "dcp@402e00000", &dcp_ph));
    TRY(S(t, node, "compatible", "apple,t6040-dcp\0apple,dcp"));
    TRY(fdt_setprop_u32(t, node, "mboxes", mbox_ph));
    TRY(S(t, node, "mbox-names", "mbox"));
    const uint32_t dcp_iommu[] = {dcp_dart_ph, 0x17};
    TRY(u32s(t, node, "iommus", dcp_iommu, 2));
    TRY(S(t, node, "reg-names", "coproc\0disp-0\0disp-1\0disp-2\0disp-3"));
    TRY(reg_prop(t, node, dcp_regs, 10, ARMIO));
    /* Scratch offset 0x20000 is t8132's; unverified on T6040 (O2). */
    const uint32_t scratch[] = {pmgr_ph, 0, 4, 0x20000};
    TRY(u32s(t, node, "apple,bw-scratch", scratch, 4));
    /* R38: M4_DCP_SURFACES = 2 (default) grants a second IOMFB surface and layer so appledrm can expose a cursor
     * plane (appledrm cursor_plane=1; without that parameter the second surface is an unused overlay plane).
     * t8132 uses four surfaces in order <2 0 1 3>; those are not tried here. -DM4_DCP_SURFACES=1 restores the
     * single primary surface of R37 exactly. */
#ifndef M4_DCP_SURFACES
#define M4_DCP_SURFACES 2
#endif
#if M4_DCP_SURFACES != 1 && M4_DCP_SURFACES != 2
#error "M4_DCP_SURFACES must be 1 or 2"
#endif
#if M4_DCP_SURFACES == 2
    const uint32_t surfaces[] = {1, 1, 0, 0}, order[] = {0, 1};
#else
    const uint32_t surfaces[] = {1, 0, 0, 0}, order[] = {0};
#endif
    TRY(u32s(t, node, "apple,iomfb-surfaces", surfaces, 4));
    TRY(u32s(t, node, "apple,iomfb-surface-order", order, M4_DCP_SURFACES));
    TRY(fdt_setprop_u32(t, node, "apple,iomfb-primary-surface", 0));
    TRY(fdt_setprop_u32(t, node, "apple,iomfb-max-layers", M4_DCP_SURFACES));
    const uint32_t version[] = {26, 6, 2};
    TRY(u32s(t, node, "apple,firmware-version", version, 3));
    TRY(u32s(t, node, "apple,firmware-compat", version, 3));
    TRY(fdt_setprop_u32(t, node, "#address-cells", 2));
    TRY(fdt_setprop_u32(t, node, "#size-cells", 2));
    TRY(fdt_setprop_string(t, node, "status", status(M4_DCP_STAGE >= 3)));

    int dcp = node;
    TRY(node = add(t, dcp, "piodma", &piodma_ph));
    const uint32_t piodma_iommu[] = {disp_dart_ph, 0x10};
    TRY(u32s(t, node, "iommus", piodma_iommu, 2));

    /* Disabled panel: eDP connector, no max-brightness, so the driver
     * refuses backlight registration and never drives the mini-LED (O6). */
    dcp = fdt_path_offset(t, "/dcp@402e00000");
    TRY(node = add(t, dcp, "panel", NULL));
    TRY(S(t, node, "compatible", "apple,panel-mini-led\0apple,panel"));
    TRY(fdt_setprop_u32(t, node, "width-mm", 345));
    TRY(fdt_setprop_u32(t, node, "height-mm", 223));
    TRY(fdt_setprop_string(t, node, "status", "disabled"));

    TRY(node = add(t, 0, "display-subsystem", &display_ph));
    TRY(S(t, node, "compatible", "apple,display-subsystem"));
    const uint32_t display_iommu[] = {disp_dart_ph, 0};
    TRY(u32s(t, node, "iommus", display_iommu, 2));
    TRY(fdt_setprop_empty(t, node, "dma-noncoherent"));
    TRY(fdt_setprop_string(t, node, "status", status(M4_DCP_STAGE >= 3)));

    /* Captured regions are outside every Linux memory node; no-map only
     * describes them so iommu-addresses can keep the live translations. */
    const uint32_t owner[3] = {dcp_ph, display_ph, piodma_ph};
    int resv = fdt_path_offset(t, "/reserved-memory");
    TRY(resv);
    for (unsigned i = 0; i < count; i++) {
        char name[40];
        uint32_t cells[15]; unsigned n = 0;
        /* appledrm finds the boot framebuffer (to evict simpledrm) by the m1n1
         * convention: memory-region-names "framebuffer" -> compatible "framebuffer". */
        int fb = framebuffer >= rg[i].pa && framebuffer - rg[i].pa < rg[i].size;
        snprintf(region_name[i], sizeof(region_name[i]), fb ? "framebuffer" : "dcp-map@%lx",
                 (unsigned long)rg[i].pa);
        snprintf(name, sizeof(name), "%s@%lx", fb ? "framebuffer" : "dcp-map",
                 (unsigned long)rg[i].pa);
        resv = fdt_path_offset(t, "/reserved-memory");
        TRY(node = add(t, resv, name, &region_ph[i]));
        if (fb)
            TRY(S(t, node, "compatible", "framebuffer"));
        else
            TRY(S(t, node, "compatible", "apple,asc-mem"));
        const uint64_t reg[] = {rg[i].pa, rg[i].size};
        TRY(reg_prop(t, node, reg, 2, 0));
        TRY(fdt_setprop_empty(t, node, "no-map"));
        for (unsigned s = 0; s < 3; s++)
            if (rg[i].users & (1u << s)) {
                cells[n++] = owner[s];
                cells[n++] = (uint32_t)(rg[i].iova[s] >> 32); cells[n++] = (uint32_t)rg[i].iova[s];
                cells[n++] = (uint32_t)(rg[i].size >> 32); cells[n++] = (uint32_t)rg[i].size;
            }
        TRY(u32s(t, node, "iommu-addresses", cells, n));
    }
    const char *paths[3] = {"/dcp@402e00000", "/display-subsystem", "/dcp@402e00000/piodma"};
    for (unsigned s = 0; s < 3; s++)
        for (unsigned i = 0; i < count; i++)
            if (rg[i].users & (1u << s)) {
                TRY(node = fdt_path_offset(t, paths[s]));
                TRY(fdt_appendprop_u32(t, node, "memory-region", region_ph[i]));
                TRY(fdt_appendprop_string(t, node, "memory-region-names", region_name[i]));
            }

    TRY(node = fdt_path_offset(t, "/aliases"));
    TRY(fdt_setprop_string(t, node, "dcp", "/dcp@402e00000"));
    TRY(fdt_setprop_string(t, node, "disp0", "/display-subsystem"));
    TRY(fdt_setprop_string(t, node, "disp0_piodma", "/dcp@402e00000/piodma"));
    return 0;
}
#undef TRY
#undef S

/* /chosen/m4,dcp-fdt-status = <stage code detail>: code 0 emitted,
 * 1 capture not accepted, 2 ADT layout differs, 3 unexpected spans
 * (detail = collect() reason), 4 FDT edit failed (detail = libfdt error). */
static int finish(void *tree, size_t bytes, void *scratch, size_t capacity,
                  int32_t code, int32_t detail)
{
    const fdt32_t v[] = {cpu_to_fdt32(M4_DCP_STAGE), cpu_to_fdt32((uint32_t)code),
                         cpu_to_fdt32((uint32_t)detail)};
    int ret, chosen = fdt_path_offset(scratch, "/chosen");
    if (chosen < 0) return chosen;
    if ((ret = fdt_setprop(scratch, chosen, "m4,dcp-fdt-status", v, sizeof(v)))) return ret;
    /* detail stays a libfdt error: a tree past the caller's reserve is
     * no space (-3), not -4/-5 (= -FDT_ERR_BADOFFSET/-FDT_ERR_BADPATH). */
    if ((ret = fdt_pack(scratch)) || (ret = fdt_check_full(scratch, capacity))) return ret;
    if (fdt_totalsize(scratch) > bytes) return -FDT_ERR_NOSPACE;
    memcpy(tree, scratch, fdt_totalsize(scratch));
    return 0;
}

int m4_dcp_fdt_write(void *tree, size_t bytes, void *scratch, size_t capacity,
                     const struct m4_display_handoff *h)
{
    static struct region regions[MAX_REGIONS];
    unsigned count = 0;
    int32_t code, detail = 0;
    int ret;
    if (!tree || !scratch || !h || capacity > INT32_MAX || bytes < sizeof(struct fdt_header) ||
        fdt_check_full(tree, bytes)) return -1;
    if (!m4_display_handoff_accepted(h)) code = 1;
    else if (!adt_layout()) code = 2;
    else if ((detail = collect(h, regions, &count))) code = 3;
    else {
        if ((ret = fdt_open_into(tree, scratch, (int)capacity))) return ret;
        detail = build(scratch, regions, count, h->framebuffer);
        if (!detail && !(detail = finish(tree, bytes, scratch, capacity, 0, 0))) return 0;
        code = 4;
    }
    /* Refused or failed: publish only the reason on an untouched copy. */
    if ((ret = fdt_open_into(tree, scratch, (int)capacity))) return ret;
    if ((ret = finish(tree, bytes, scratch, capacity, code, detail))) return ret;
    return code;
}

/* ---------------------------------------------------------------- rev 11 */
#define POOL 16u
static int phandle_of(void *t, int node, uint32_t *ph)
{
    int ret;
    uint32_t v = fdt_get_phandle(t, node);
    if (!v) {
        if ((ret = fdt_generate_phandle(t, &v)) < 0 || (ret = fdt_setprop_u32(t, node, "phandle", v)) < 0) return ret;
    }
    *ph = v;
    return 0;
}
static void delete_pool(void *t)
{
    int resv = fdt_path_offset(t, "/reserved-memory");
    for (unsigned i = 0; i < POOL && resv >= 0; i++) {
        char name[24];
        snprintf(name, sizeof(name), "dcp-map-pool%u", i);
        int n = fdt_subnode_offset(t, resv, name);
        if (n >= 0) fdt_del_node(t, n);
    }
}
/* The DTS carries the DCP/DART/mailbox/display nodes disabled and POOL region placeholders. An accepted capture
 * adopts the placeholders (same names, reg and iommu-addresses rev 10 emitted), fills the three memory-region lists
 * and enables the five stage-3 nodes. Otherwise the pool is deleted and every node stays disabled. */
int m4_dcp_fixup(void *t, size_t capacity, const struct m4_display_handoff *h)
{
    static struct region regions[MAX_REGIONS];
    static char region_name[POOL][32];
    unsigned count = 0;
    int ret, node;
    if ((ret = fdt_open_into(t, t, (int)capacity)) < 0) return ret;
    int ok = h && m4_display_handoff_accepted(h) && adt_layout() && !collect(h, regions, &count) && count <= POOL;
    if (!ok) { delete_pool(t); return fdt_pack(t) ? -1 : 1; }
    uint32_t owner[3], region_ph[POOL];
    static const char *const paths[3] = {"/dcp@402e00000", "/display-subsystem", "/dcp@402e00000/piodma"};
    for (unsigned s = 0; s < 3; s++) {
        if ((node = fdt_path_offset(t, paths[s])) < 0) return node;
        if ((ret = phandle_of(t, node, &owner[s])) < 0) return ret;
        fdt_delprop(t, node, "memory-region"); fdt_delprop(t, node, "memory-region-names");
    }
    for (unsigned i = 0; i < count; i++) {
        char name[40], pool[24];
        uint32_t cells[15]; unsigned n = 0;
        int fb = h->framebuffer >= regions[i].pa && h->framebuffer - regions[i].pa < regions[i].size;
        snprintf(region_name[i], sizeof(region_name[i]), fb ? "framebuffer" : "dcp-map@%lx", (unsigned long)regions[i].pa);
        snprintf(name, sizeof(name), "%s@%lx", fb ? "framebuffer" : "dcp-map", (unsigned long)regions[i].pa);
        snprintf(pool, sizeof(pool), "dcp-map-pool%u", i);
        int resv = fdt_path_offset(t, "/reserved-memory");
        node = resv < 0 ? resv : fdt_subnode_offset(t, resv, pool);
        if (node < 0) return node;
        if ((ret = phandle_of(t, node, &region_ph[i])) < 0) return ret;
        if ((ret = fdt_setprop_string(t, node, "compatible", fb ? "framebuffer" : "apple,asc-mem")) < 0) return ret;
        const uint64_t reg[] = {regions[i].pa, regions[i].size};
        if ((ret = reg_prop(t, node, reg, 2, 0)) < 0 || (ret = fdt_setprop_empty(t, node, "no-map")) < 0) return ret;
        fdt_delprop(t, node, "status");
        for (unsigned s = 0; s < 3; s++)
            if (regions[i].users & (1u << s)) {
                cells[n++] = owner[s];
                cells[n++] = (uint32_t)(regions[i].iova[s] >> 32); cells[n++] = (uint32_t)regions[i].iova[s];
                cells[n++] = (uint32_t)(regions[i].size >> 32); cells[n++] = (uint32_t)regions[i].size;
            }
        if ((ret = u32s(t, node, "iommu-addresses", cells, n)) < 0) return ret;
        if ((ret = fdt_set_name(t, node, name)) < 0) return ret;
    }
    {
        int resv = fdt_path_offset(t, "/reserved-memory");
        for (unsigned i = count; i < POOL && resv >= 0; i++) {
            char pool[24];
            snprintf(pool, sizeof(pool), "dcp-map-pool%u", i);
            int n = fdt_subnode_offset(t, resv, pool);
            if (n >= 0 && (ret = fdt_del_node(t, n)) < 0) return ret;
        }
    }
    for (unsigned s = 0; s < 3; s++)
        for (unsigned i = 0; i < count; i++)
            if (regions[i].users & (1u << s)) {
                if ((node = fdt_path_offset(t, paths[s])) < 0) return node;
                if ((ret = fdt_appendprop_u32(t, node, "memory-region", region_ph[i])) < 0 ||
                    (ret = fdt_appendprop_string(t, node, "memory-region-names", region_name[i])) < 0) return ret;
            }
    static const char *const enable[] = {"/mailbox@402e08000", "/iommu@402340000", "/iommu@402300000",
                                         "/dcp@402e00000", "/display-subsystem"};
    for (unsigned i = 0; i < sizeof(enable) / sizeof(*enable); i++) {
        if ((node = fdt_path_offset(t, enable[i])) < 0) return node;
        if ((ret = fdt_setprop_string(t, node, "status", "okay")) < 0) return ret;
    }
    return fdt_pack(t);
}

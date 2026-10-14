/* SPDX-License-Identifier: MIT
 * Rev 11 DT fix-ups. board.dtb carries every node (dts/t6040-j616s.dts in the kernel tree); the loader writes only
 * what it alone knows: memory ranges, its own reservations, the boot framebuffer, /chosen descriptors, CPU release
 * addresses, per-unit blobs from the ADT. No node is created here: a placeholder is adopted (renamed, filled) or
 * deleted. Every check of the rev 10 emitters that concerns the loader's own layout is kept. */
#include "fixups.h"
#include "direct-capture.h"
#include "direct-bluetooth.h"
#include "bluetooth-fdt.h"
#include "adt.h"
#include "libfdt.h"
#include <string.h>
#ifdef M4_LOADER
#include "vsprintf.h"
#include "utils.h"
#else
#include <stdio.h>
#endif
#include "keyboard_types.h"

#define TRY(x) do { int r_ = (x); if (r_) return r_; } while (0)

static int u64prop(void *dt, int node, const char *name, uint64_t v)
{
    fdt64_t x = cpu_to_fdt64(v);
    return fdt_setprop(dt, node, name, &x, sizeof(x));
}
static int regprop(void *dt, int node, uint64_t base, uint64_t size)
{
    fdt64_t r[2] = {cpu_to_fdt64(base), cpu_to_fdt64(size)};
    return fdt_setprop(dt, node, "reg", r, sizeof(r));
}
static int disjoint(struct m4_range a, struct m4_range b)
{
    return a.size && b.size && a.base + a.size > a.base && b.base + b.size > b.base &&
           (a.base + a.size <= b.base || b.base + b.size <= a.base);
}
static int is_memory(const void *dt, int n)
{
    int len;
    const char *t = fdt_getprop(dt, n, "device_type", &len);
    return t && len == 7 && !memcmp(t, "memory", 7);
}
/* no /memory range and not the boot framebuffer overlaps own */
static int excluded(const void *dt, struct m4_range own)
{
    int n, bytes;
    fdt_for_each_subnode(n, dt, 0) {
        if (!is_memory(dt, n)) continue;
        const fdt64_t *p = fdt_getprop(dt, n, "reg", &bytes);
        if (!p || bytes < 16 || bytes % 16) return 0;
        for (int i = 0; i < bytes / 16; i++)
            if (!disjoint(own, (struct m4_range){fdt64_ld(p + 2 * i), fdt64_ld(p + 2 * i + 1)})) return 0;
    }
    n = fdt_node_offset_by_compatible(dt, -1, "simple-framebuffer");
    const fdt64_t *p = n < 0 ? NULL : fdt_getprop(dt, n, "reg", &bytes);
    return p && bytes == 16 && disjoint(own, (struct m4_range){fdt64_ld(p), fdt64_ld(p + 1)});
}
static int word_eq(const void *dt, int node, const char *key, uint32_t v)
{
    int len;
    const fdt32_t *p = fdt_getprop(dt, node, key, &len);
    return p && len == 4 && fdt32_to_cpu(*p) == v;
}

int m4_fixup_base(void *dt, size_t cap, const struct m4_memory *m, const struct m4_handoff *h, const char *cmdline)
{
    if (!dt || !m || !h || !cmdline || cap > M4_FDT_CAPACITY || cap < 16384 || !m->free_count ||
        m->free_count > M4_MAX_FREE || strlen(cmdline) > 1024) return -1;
    struct m4_range arena = {h->arena_base, h->arena_size}, fb = {h->framebuffer, h->framebuffer_size};
    if (!m4_contains(m->usable, arena) || arena.base + arena.size > m->top ||
        !m4_contains(arena, (struct m4_range){h->kernel, h->kernel_size}) ||
        !m4_contains(arena, (struct m4_range){h->initrd, h->initrd_size}) ||
        h->kernel_size == 0 || h->kernel + h->kernel_size >= h->initrd ||
        (h->kernel & 0x1fffff) || ((arena.base | arena.size) & 16383) ||
        !m4_range_valid(fb) || m4_overlaps(arena, fb) ||
        h->width == 0 || h->width > 16384 || h->height == 0 || h->height > 16384 ||
        h->stride < h->width * 4 || h->stride > 65536 || (h->stride & 3) ||
        (uint64_t)h->stride * h->height > h->framebuffer_size ||
        (h->depth != 30 && h->depth != 32) || (h->affinity & ~0xff00ffffffULL)) return -2;
    for (unsigned i = 0; i < m->free_count; i++)
        if (m4_overlaps(m->free[i], arena) || m4_overlaps(m->free[i], fb)) return -3;
    if (fdt_check_full(dt, cap) || fdt_node_check_compatible(dt, 0, "apple,j616s") ||
        !word_eq(dt, 0, "#address-cells", 2) || !word_eq(dt, 0, "#size-cells", 2)) return -4;
    TRY(fdt_open_into(dt, dt, (int)cap));
    /* memory: the one placeholder node takes the kernel/arena range first, then every free range */
    int node = -1, n;
    fdt_for_each_subnode(n, dt, 0) if (is_memory(dt, n)) { if (node >= 0) return -5; node = n; }
    if (node < 0) return -5;
    fdt64_t regs[2 * (1 + M4_MAX_FREE)];
    unsigned c = 0;
    regs[c++] = cpu_to_fdt64(h->kernel); regs[c++] = cpu_to_fdt64(arena.base + arena.size - h->kernel);
    for (unsigned i = 0; i < m->free_count; i++) {
        regs[c++] = cpu_to_fdt64(m->free[i].base); regs[c++] = cpu_to_fdt64(m->free[i].size);
    }
    TRY(fdt_setprop(dt, node, "reg", regs, c * sizeof(*regs)));
    char name[48];
    snprintf(name, sizeof(name), "memory@%llx", (unsigned long long)h->kernel);
    TRY(fdt_set_name(dt, node, name));
    /* the boot CPU must be the DTS's /cpus/cpu@<affinity> */
    snprintf(name, sizeof(name), "cpu@%llx", (unsigned long long)h->affinity);
    int cpus = fdt_path_offset(dt, "/cpus");
    if (cpus < 0 || fdt_subnode_offset(dt, cpus, name) < 0) return -6;
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0) return -7;
    TRY(fdt_setprop_string(dt, chosen, "bootargs", cmdline));
    TRY(u64prop(dt, chosen, "linux,initrd-start", h->initrd));
    TRY(u64prop(dt, chosen, "linux,initrd-end", h->initrd + h->initrd_size));
    TRY(fdt_setprop_empty(dt, chosen, "lab,standalone"));
    int fbn = fdt_subnode_offset(dt, chosen, "framebuffer");
    if (fbn < 0) return -8;
    TRY(regprop(dt, fbn, h->framebuffer, (uint64_t)h->stride * h->height));
    TRY(fdt_setprop_u32(dt, fbn, "width", h->width));
    TRY(fdt_setprop_u32(dt, fbn, "height", h->height));
    TRY(fdt_setprop_u32(dt, fbn, "stride", h->stride));
    TRY(fdt_setprop_string(dt, fbn, "format", h->depth == 30 ? "x2r10g10b10" : "x8r8g8b8"));
    TRY(fdt_setprop_string(dt, fbn, "status", "okay"));
    snprintf(name, sizeof(name), "framebuffer@%llx", (unsigned long long)h->framebuffer);
    TRY(fdt_set_name(dt, fbn, name));
    TRY(fdt_pack(dt));
    return fdt_totalsize(dt) <= cap ? 0 : -9;
}

/* Same source as m1n1 kboot.c dt_set_ipd(): the ADT kblang-calibration layout byte through keyboard_types. A
 * keyboard node without the property, or any doubt, leaves the DTS value (0 = not ISO, no swap). */
static void keyboard_country(void *dt)
{
    int ipd = adt_path_offset(adt, "/arm-io/dockchannel-mtp/mtp-transport/keyboard");
    uint32_t len = 0;
    const unsigned char *kb = ipd < 0 ? NULL : adt_getprop(adt, ipd, "kblang-calibration", &len);
    if (!kb || len < 2 || (uint64_t)kb < (uint64_t)adt || (uint64_t)kb + len > (uint64_t)adt + adt_get_size()) return;
    if (kb[1] >= sizeof(keyboard_types) / sizeof(keyboard_types[0])) return;
    const char *path = fdt_get_alias(dt, "keyboard");
    int node = path ? fdt_path_offset(dt, path) : -1;
    if (node < 0 || !fdt_getprop(dt, node, "hid-country-code", NULL)) return;
    fdt_setprop_u32(dt, node, "hid-country-code", (uint32_t)keyboard_types[kb[1]]);
}

/* Adopt the reserved-memory placeholder `label` for range r. `covered` = wholly inside another reservation
 * emitted here (the host0/host1 image reservations): the node then stays but is disabled, as in rev 10. */
static int adopt(void *dt, const char *label, struct m4_range r, int covered)
{
    if (!r.size || (r.base | r.size) & (M4_SMP_PAGE - 1) || r.base + r.size <= r.base || !excluded(dt, r)) return -1;
    int parent = fdt_path_offset(dt, "/reserved-memory");
    int n = parent < 0 ? parent : fdt_subnode_offset(dt, parent, label);
    if (n < 0) return -1;
    char name[72];
    snprintf(name, sizeof(name), "%s@%llx", label, (unsigned long long)r.base);
    if (regprop(dt, n, r.base, r.size) || fdt_setprop_empty(dt, n, "no-map")) return -1;
    if (covered) { if (fdt_setprop_string(dt, n, "status", "disabled")) return -1; }
    else if (fdt_getprop(dt, n, "status", NULL) && fdt_delprop(dt, n, "status")) return -1;
    if (fdt_set_name(dt, n, name) || fdt_add_mem_rsv(dt, r.base, r.size)) return -1;
    return 0;
}

int m4_fixup_direct(void *dt, size_t capacity)
{
    unsigned count = 0; uint64_t release = 0;
    const struct m4_range *own = m4_direct_smp_regions(&count, &release);
    unsigned peer_count = 0;
    const struct m4_direct_peer_record *peers = m4_direct_smp_peer_records(&peer_count);
    const struct m4_direct_capture_descriptor *c = &m4_direct_capture.descriptor;
    const struct m4_handoff *h = &m4_direct_capture.handoff;
    struct m4_range loader = {c->loader_base, c->loader_end - c->loader_base};
    struct m4_range arena = {c->arena_base, c->arena_size};
    struct m4_range capture = {(uint64_t)&m4_direct_capture, sizeof(m4_direct_capture)};
    if (!dt || capacity != M4_FDT_CAPACITY || fdt_check_full(dt, capacity) ||
        !own || !peers || count != 5 || !peer_count || peer_count > M4_SMP_MAX_PEERS ||
        own[2].size != M4_SMP_PAGE * M4_SMP_MAX_PEERS || own[3].size != M4_SMP_STACK * M4_SMP_MAX_PEERS ||
        c->peer_count != peer_count || c->peer_table != (uint64_t)peers || c->peer_entry_bytes != sizeof(*peers) ||
        !m4_contains(own[4], (struct m4_range){(uint64_t)peers, sizeof(*peers) * M4_SMP_MAX_PEERS}) ||
        release != own[2].base + 64 || c->release_addr != release ||
        c->release_page_base != own[2].base || c->release_page_bytes != own[2].size ||
        c->magic != M4_DIRECT_CAPTURE_MAGIC || c->version != 2 || c->bytes != sizeof(*c) || c->phase ||
        c->primary_mpidr != 0x10100 || c->secondary_mpidr != 0x10101 ||
        c->handoff_addr != (uint64_t)h || c->arena_size != M4_ARENA_BYTES ||
        c->linux_dt != arena.base + 0x10000 || c->arena_base != h->arena_base ||
        c->arena_size != h->arena_size || c->kernel != h->kernel || c->kernel_memory != h->kernel_size ||
        c->initrd != h->initrd || c->initrd_bytes != h->initrd_size ||
        c->framebuffer != h->framebuffer || c->framebuffer_bytes != h->framebuffer_size ||
        !m4_contains(loader, arena) || !m4_contains(loader, capture) || !disjoint(arena, capture) ||
        !m4_contains(loader, (struct m4_range){c->bootargs_addr, 108}) ||
        !disjoint(arena, (struct m4_range){c->bootargs_addr, 108}) ||
        !disjoint(arena, (struct m4_range){c->adt, c->adt_bytes}) ||
        !excluded(dt, (struct m4_range){c->adt, c->adt_bytes}) ||
        !excluded(dt, (struct m4_range){c->bootargs_addr, 108})) return -10;
    /* no MMIO window of the tree aliases a memory node */
    int mmio, bytes;
    fdt_for_each_subnode(mmio, dt, 0) {
        if (is_memory(dt, mmio)) continue;
        const fdt64_t *regs = fdt_getprop(dt, mmio, "reg", &bytes);
        if (!regs) continue;
        if (bytes <= 0 || bytes % 16) return -10;
        for (int i = 0; i < bytes / 16; i++)
            if (!excluded(dt, (struct m4_range){fdt64_ld(regs + 2 * i), fdt64_ld(regs + 2 * i + 1)})) return -10;
    }
    int chosen = fdt_path_offset(dt, "/chosen");
    int fb = fdt_node_offset_by_compatible(dt, -1, "simple-framebuffer");
    if (chosen < 0 || fb < 0 || fdt_parent_offset(dt, fb) != chosen ||
        fdt_first_subnode(dt, fb) != -FDT_ERR_NOTFOUND ||
        fdt_node_offset_by_compatible(dt, fb, "simple-framebuffer") != -FDT_ERR_NOTFOUND ||
        !word_eq(dt, chosen, "#address-cells", 2) || !word_eq(dt, chosen, "#size-cells", 2)) return -11;
    for (unsigned i = 0; i < count; i++) {
        if (!m4_contains(loader, own[i]) || !disjoint(arena, own[i]) || !excluded(dt, own[i])) return -12;
        for (unsigned j = 0; j < i; j++) if (!disjoint(own[i], own[j])) return -12;
        if (!disjoint(own[i], capture)) return -12;
    }
    for (unsigned n = 0; n < peer_count; n++) {
        const struct m4_direct_peer_record *p = peers + n;
        if (p->state_base != own[2].base + n * M4_SMP_PAGE || p->state_bytes != M4_SMP_PAGE ||
            p->stack_base != own[3].base + n * M4_SMP_STACK || p->stack_bytes != M4_SMP_STACK ||
            p->release_addr != p->state_base + 64 || (!p->affinity && (p->cpu_id || p->adt_reg)) ||
            p->affinity == 0x10100 || (p->affinity & ~UINT64_C(0xff00ffffff)) || p->cpu_id > 14 ||
            p->cpu_id == 9 || p->cpu_id == 4 || p->adt_reg > 0xffffff || p->adt_reg == 0x100) return -12;
        for (unsigned j = 0; j < n; j++)
            if (p->cpu_id == peers[j].cpu_id || p->adt_reg == peers[j].adt_reg ||
                p->affinity == peers[j].affinity) return -12;
    }
    if (peers[0].cpu_id != 5 || peers[0].adt_reg != 0x101 || peers[0].affinity != 0x10101) return -12;
    TRY(fdt_open_into(dt, dt, (int)capacity));

    /* every DTS CPU the peers describe gets its spin-table release address */
    for (unsigned n = 0; n < peer_count; n++) {
        char name[48];
        snprintf(name, sizeof(name), "cpu@%llx", (unsigned long long)peers[n].affinity);
        int cpu = fdt_subnode_offset(dt, fdt_path_offset(dt, "/cpus"), name);
        if (cpu < 0 || u64prop(dt, cpu, "cpu-release-addr", peers[n].release_addr)) return -13;
    }
    const char *const names[] = {"direct-smp-vector", "direct-smp-code", "direct-smp-records", "direct-smp-stacks",
                                 "direct-smp-peers"};
    uint64_t end = arena.base + arena.size;
    struct m4_range host0 = {loader.base, arena.base - loader.base};
    struct m4_range host1 = {end, end < c->loader_end ? c->loader_end - end : 0};
#define IN_HOST(r) (m4_contains(host0, (r)) || (host1.size && m4_contains(host1, (r))))
    for (unsigned i = 0; i < count; i++) if (adopt(dt, names[i], own[i], IN_HOST(own[i]))) return -13;
    if (adopt(dt, "direct-desktop-capture", capture, IN_HOST(capture)) ||
        adopt(dt, "direct-desktop-host0", host0, 0)) return -13;
    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (host1.size) { if (adopt(dt, "direct-desktop-host1", host1, 0)) return -13; }
    else { int hn = resv < 0 ? resv : fdt_subnode_offset(dt, resv, "direct-desktop-host1"); if (hn >= 0 && fdt_del_node(dt, hn)) return -13; }
#undef IN_HOST
    chosen = fdt_path_offset(dt, "/chosen");
    /* iBoot's /chosen/random-seed becomes rng-seed (m1n1 kboot.c dt_set_rng_seed_adt): the CRNG is ready at early init */
    uint32_t seed_bytes = 0;
    int adt_chosen = adt_path_offset(adt, "/chosen");
    const unsigned char *seed = adt_chosen < 0 ? NULL : adt_getprop(adt, adt_chosen, "random-seed", &seed_bytes);
    if (!seed || seed_bytes < 32 || seed_bytes > 512 || (uint64_t)seed < (uint64_t)adt ||
        (uint64_t)seed + seed_bytes > (uint64_t)adt + adt_get_size()) seed_bytes = 0;
    fdt64_t descriptor[] = {cpu_to_fdt64(capture.base), cpu_to_fdt64(sizeof(*c))};
    fdt64_t peer_wire[M4_SMP_MAX_PEERS * 8];
    for (unsigned n = 0; n < peer_count; n++) {
        uint64_t values[8];
        memcpy(values, peers + n, sizeof(values));
        for (unsigned f = 0; f < 8; f++) peer_wire[n * 8 + f] = cpu_to_fdt64(values[f]);
    }
    if (fdt_setprop_empty(dt, chosen, "lab,direct-desktop1") ||
        fdt_setprop_string(dt, chosen, "m4,direct-desktop-contract", M4_DIRECT_DESKTOP_CONTRACT) ||
        fdt_setprop(dt, chosen, "lab,direct-desktop-capture-v2", descriptor, sizeof(descriptor)) ||
        fdt_setprop(dt, chosen, "m4,direct-peers-v2", peer_wire, peer_count * 8 * sizeof(fdt64_t)) ||
        (seed_bytes && fdt_setprop(dt, chosen, "rng-seed", seed, seed_bytes))) return -14;
    keyboard_country(dt);
    if (fdt_pack(dt) || fdt_totalsize(dt) > M4_FDT_CAPACITY || fdt_check_full(dt, capacity)) return -14;
    fdt_set_boot_cpuid_phys(dt, 0x10100);
    return 0;
}

static const void *adt_prop(int node, const char *key, uint32_t expected)
{
    uint32_t length = 0, total = adt_get_size();
    const void *p = node < 0 ? NULL : adt_getprop(adt, node, key, &length);
    uintptr_t base = (uintptr_t)adt, address = (uintptr_t)p;
    if (!p || (expected && length != expected) || address < base || length > total || address - base > total - length)
        return NULL;
    return p;
}
/* Wi-Fi: per-unit calibration and antenna SKU from the ADT. Bluetooth: per-unit address and beamforming calibration.
 * Any missing ADT item leaves the DTS node without that property; a refused Bluetooth admission removes the node. */
int m4_fixup_wireless(void *dt, size_t capacity, int bt_state)
{
    if (!dt || fdt_check_full(dt, capacity)) return -1;
    TRY(fdt_open_into(dt, dt, (int)capacity));
    /* libfdt offsets move on every insertion or deletion: every node is re-resolved by path before it is used */
#define WIFI_PATH M4_BT_BRIDGE "/wifi@0,0"
    if (fdt_path_offset(dt, WIFI_PATH) < 0 || fdt_path_offset(dt, M4_BT_PATH) < 0) return -3;
    int wl = adt_path_offset(adt, "/arm-io/wlan");
    uint32_t n = 0;
    const void *cal = wl < 0 ? NULL : adt_getprop(adt, wl, "wifi-calibration-msf", &n);
    if (cal && n && n <= 16384 && adt_prop(wl, "wifi-calibration-msf", n))
        TRY(fdt_setprop(dt, fdt_path_offset(dt, WIFI_PATH), "brcm,cal-blob", cal, n));
    const uint8_t *sku = adt_prop(wl, "wifi-antenna-sku-info", 16);
    if (sku && sku[8] && sku[15] == 0 && !sku[11])
        TRY(fdt_setprop_string(dt, fdt_path_offset(dt, WIFI_PATH), "apple,antenna-sku", (const char *)sku + 8));
    if (bt_state == M4_BT_STATE_REFUSED) {
        int aliases = fdt_path_offset(dt, "/aliases");
        if (aliases >= 0 && fdt_getprop(dt, aliases, "bluetooth0", NULL)) TRY(fdt_delprop(dt, aliases, "bluetooth0"));
        TRY(fdt_del_node(dt, fdt_path_offset(dt, M4_BT_PATH)));
        return fdt_pack(dt);
    }
    int chosen = adt_path_offset(adt, "/chosen"), bn = adt_path_offset(adt, "/arm-io/bluetooth");
    const uint8_t *address = adt_prop(chosen, "mac-address-bluetooth0", 6);
    const void *bf = adt_prop(bn, "bluetooth-taurus-calibration-bf", M4_BT_BF_BYTES);
    static const uint8_t zero[6];
    if (!address || (address[0] & 1) || !memcmp(address, zero, 6) || !bf) return -4;
    uint8_t bdaddr[6];
    for (unsigned i = 0; i < 6; i++) bdaddr[i] = address[5 - i]; /* ADT display order; Linux uses bdaddr_t order */
    TRY(fdt_setprop(dt, fdt_path_offset(dt, M4_BT_PATH), "local-bd-address", bdaddr, sizeof(bdaddr)));
    TRY(fdt_setprop(dt, fdt_path_offset(dt, M4_BT_PATH), "brcm,taurus-bf-cal-blob", bf, M4_BT_BF_BYTES));
    TRY(fdt_pack(dt));
    return fdt_check_full(dt, capacity);
#undef WIFI_PATH
}

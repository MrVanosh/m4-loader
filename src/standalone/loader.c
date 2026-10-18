/* SPDX-License-Identifier: MIT */
/* One shot, direct two-core Omarchy desktop; Linux owns all runtime services. */
#include "types.h"
#include "utils.h"
#include "string.h"
#include "malloc.h"
#include "j616s_screen.h"
#include "j616s_usb.h"
#include "fdt.h"
#include "libfdt/libfdt.h"
#include "payload_sizes.h"
#include "direct-smp.h"
#include "direct-capture.h"
#include "direct-bluetooth.h"
#include "dart-capture.h"
#include "display-handoff.h"
#include "boot-timing.h"
#include "boot-profile.h"
#include "cpu-idle-regs.h"
#include "ram.h"
#include "adt.h"
#include "direct-peripheral-adt.h"
#include "xnuboot.h"
#include "loader-cache.h"
#include "loader-speed.h"
#include "media-probe.h"
#include "level1-glue.h"
#include "fixups.h"
#include "pmp-publish.h"

extern char _base[], _end[], _rodata_end[];
extern u8 m4_kernel[], m4_kernel_end[], m4_initrd[], m4_initrd_end[], m4_template[];
extern u8 m4_pcie[], m4_pcie_end[], m4_template_end[];
extern const char m4_rescue_cmdline[];
extern int m4_metadata(struct m4_memory *, struct m4_handoff *, u64, u64);
extern int m4_validate_tz(const u64 *);
extern u64 m4_mcc(volatile u64 *, u64, u64, u64, u64);
extern u64 m4_watchdog(u64 *, u64, u64, u64, u64);
extern u64 m4_dapf(u64 *, u64, u64, u64, u64);
extern void m4_direct_enter(u64, u64) __attribute__((noreturn));
extern void j616s_screen_show_error_first(const char *, unsigned);
static struct m4_memory memory;
struct m4_direct_capture_page m4_direct_capture __attribute__((aligned(M4_SMP_PAGE)));
/* GPU capture ABI keeps the accepted handoff layout without a duplicate. */
__asm__(".global handoff\n.set handoff,m4_direct_capture+256\n");
#define handoff m4_direct_capture.handoff
_Static_assert(M4_FDT_CAPACITY == 0x20000, "DT slot capacity");
static u8 tree[M4_FDT_CAPACITY] __attribute__((aligned(16384)));
static u8 peripheral_tree[M4_FDT_CAPACITY] __attribute__((aligned(16384)));
static u64 scratch[2048] __attribute__((aligned(16384)));
static u64 lower_bound, upper_bound, dt_address;
static bool attempted;
/* R39 Level 1: where the payload bytes come from. NULL = already in its final
 * slot (loaded from the SSD, SHA-256 verified before it got here). */
static const u8 *payload_kernel_src, *payload_initrd_src;
/* The entry's board.dtb or its fix-ups were refused: the rescue set boots and the /chosen record says why. */
/* board.dtb limit: the final DT must fit the 128 KiB slot after the fix-ups add ~27 KB (Wi-Fi and Bluetooth blobs, DCP) */
#define M4_BOARD_DTB_MAX (96u * 1024)
static const char *demote_why;
static int demote_rc;
static char rescue_args[1100];
/* rescue cmdline: the embedded text, always with the m4.safe=1 token (units that load hardware skip themselves) */
static const char *rescue_cmdline(void)
{
    size_t n = strlen(m4_rescue_cmdline);
    if (n > 1024 - 10) n = 1024 - 10;
    memcpy(rescue_args, m4_rescue_cmdline, n);
    rescue_args[n] = 0;
    int have = 0;
    for (size_t i = 0; i + 9 <= n; i++)
        if (!memcmp(rescue_args + i, "m4.safe=1", 9) && (i == 0 || rescue_args[i - 1] == ' ') && (i + 9 == n || rescue_args[i + 9] == ' ')) have = 1;
    if (!have) { if (n) rescue_args[n++] = ' '; memcpy(rescue_args + n, "m4.safe=1", 10); }
    return rescue_args;
}
/* board.dtb in `tree` (entry) or the embedded rescue copy, then every fix-up. 0 ok, else which step refused. */
static int build_dt(int entry, const char *cmdline)
{
    if (!entry) memcpy(tree, m4_template, (size_t)(m4_template_end - m4_template));
    if (m4_fixup_base(tree, sizeof(tree), &memory, &handoff, cmdline)) return 1;
    if (m4_fixup_direct(tree, sizeof(tree))) return 2;
    if (m4_prepare_direct_peripherals(adt, cur_boot_args.devtree_size, tree, sizeof(tree))) return 3;
    if (m4_direct_bluetooth_fdt(tree, peripheral_tree, sizeof(tree))) return 4;
    return 0;
}
static const u64 pcie_base = 0x10200000000UL;

/* Identity tables for the cached windows (m1n1 mmu_init/mmu_shutdown style).
 * A refusal keeps every window uncached, which is the previous loader. */
static void cache_build(struct m4_range protected)
{
#ifdef M4_LOADER_UNCACHED
    (void)protected;
    return;
#endif
    void *tables = memalign(16384, M4_CACHE_MAX_TABLES * 16384);
    struct m4_cache_layout layout = {
        .image_base = (u64)_base, .image_ro_end = (u64)_rodata_end, .image_end = (u64)_end,
        /* Only pages wholly inside the ADT: no neighbouring iBoot byte is ever
         * cacheable (m1n1 keeps /vram and the iBoot handoff page uncached). */
        .adt_base = ((u64)adt + 16383) & ~16383UL,
        .adt_end = ((u64)adt + cur_boot_args.devtree_size) & ~16383UL,
        .protected = protected};
    /* A partial-page or over-2 MiB ADT (the GPU capture bound) stays Device. */
    if (!cur_boot_args.devtree_size || cur_boot_args.devtree_size > 0x200000 ||
        layout.adt_end <= layout.adt_base)
        layout.adt_base = layout.adt_end = 0;
    layout.hole_count = m4_direct_smp_owned_pages(layout.holes, M4_CACHE_MAX_HOLES);
    (void)m4_level1_exec_range(&layout.exec.base, &layout.exec.size); /* R39: blob range, mapped RX */
    j616s_usb_pool(&layout.pool.base, &layout.pool.size);
    if (tables && layout.hole_count == 3)
        (void)m4_cache_build(&layout, tables, M4_CACHE_MAX_TABLES);
    m4_speed.table_bytes = (u64)m4_cache.tables_used * 16384;
}

/* After the copy window the copies are compared through RAM (MMU off). A
 * mismatch means a missed clean: scrub the cached ranges so no stale dirty
 * line can be evicted later, redo the copies uncached as the previous loader
 * did, and compare again. Still different: refuse the branch to Linux. */
static void payload_copy(size_t image_bytes)
{
    m4_ram_zero((void *)(handoff.kernel + image_bytes), handoff.kernel_size - image_bytes);
    if (payload_kernel_src) m4_ram_copy((void *)handoff.kernel, payload_kernel_src, image_bytes);
    if (payload_initrd_src) m4_ram_copy((void *)handoff.initrd, payload_initrd_src, handoff.initrd_size);
    m4_ram_copy((void *)dt_address, tree, fdt_totalsize(tree));
}
static unsigned payload_compare(size_t image_bytes)
{
    u64 tail_start = (handoff.kernel + image_bytes + 7) & ~7UL;
    const volatile u64 *tail = (const volatile u64 *)tail_start;
    size_t tail_words = (handoff.kernel + handoff.kernel_size - tail_start) / 8;
    /* SSD payload: the CPU never wrote it (DMA + read-only hashing), so only the zero tail and the DT are compared */
    unsigned bad = (payload_kernel_src ? m4_cache_compare((void *)handoff.kernel, payload_kernel_src, image_bytes, 0x40000) : 0) +
        (payload_initrd_src ? m4_cache_compare((void *)handoff.initrd, payload_initrd_src, handoff.initrd_size, 0) : 0) +
        m4_cache_compare((void *)dt_address, tree, fdt_totalsize(tree), fdt_totalsize(tree));
    for (size_t i = 0; i < tail_words; i += 512) bad += tail[i] != 0;
    if (tail_words) bad += tail[tail_words - 1] != 0;
    return bad;
}
static int payload_check(size_t image_bytes)
{
    if (!m4_cache.enters) return 0;
    unsigned bad = payload_compare(image_bytes);
    m4_speed.payload_mismatches = bad;
    m4_speed.payload_check = bad ? M4_COPY_RECOPIED : M4_COPY_MATCHED;
    if (!bad) return 0;
    m4_cache_scrub();
    payload_copy(image_bytes);
    return payload_compare(image_bytes) ? -1 : 0;
}

static int prepare(void)
{
    m4_boot_ticks[M4_BOOT_PREPARE_START] = m4_boot_counter();
    m4_boot_profile_ticks[M4_PROFILE_PREPARE_START]=m4_boot_ticks[M4_BOOT_PREPARE_START];
    if (!m4_direct_indexed_admitted()) return 90; /* closed before all payload/MMIO work */
    if (attempted) return 1;
    attempted = true;
    if (mrs(CurrentEL) != 8 || (mrs(SCTLR_EL2) & 0x1005) ||
        (mrs(HCR_EL2) & 1) || (mrs(MPIDR_EL1) & 0xff00ffffffUL) != 0x10100 ||
        mrs(CNTFRQ_EL0) != 1000000000UL) return 2;
    struct m4_range protected = {lower_bound & ~16383UL,
        ((upper_bound + 4096 + 16383) & ~16383UL) - (lower_bound & ~16383UL)};
    cache_build(protected);
    m4_cache_enter(); /* window: ADT metadata (no MMIO) */
    if (m4_dart_adt_bounds(adt, cur_boot_args.devtree_size) ||
        m4_metadata(&memory, &handoff, lower_bound, upper_bound)) return 3;
    m4_boot_profile_mark(M4_PROFILE_ADT_METADATA_READY);
    m4_media_probe_gates(); /* ADT-only media gates while the ADT is cached (R36: 0.1 s uncached) */
    /* MCC, primary init_cpu and its register capture keep the uncached contract. */
    m4_cache_leave();
    if (m4_mcc(scratch, 1600, 0x60416041, 0, 0) != 0x4d4343 || m4_validate_tz(scratch)) return 4;
    m4_boot_profile_mark(M4_PROFILE_MCC_READY);
    void *arena = m4_arena_place(M4_ARENA_BYTES);
    handoff.arena_base = (u64)arena; handoff.arena_size = M4_ARENA_BYTES;
    struct m4_range span = {handoff.arena_base, handoff.arena_size};
    m4_speed.late_tree = memalign(16384, M4_FDT_WINDOW);
    m4_speed.late_scratch = memalign(16384, M4_FDT_WINDOW);
    if (m4_speed.late_tree && m4_speed.late_scratch) m4_speed.late_capacity = M4_FDT_WINDOW;
    if (!arena || !j616s_usb_owned(arena, handoff.arena_size) ||
        !m4_contains((struct m4_range){(u64)_rodata_end, (u64)(_end - _rodata_end)}, span) ||
        m4_overlaps(span, protected)) return 5;
    u64 header[8];
    memcpy(header, m4_kernel, sizeof(header));
    if ((header[7] & 0xffffffffUL) != 0x644d5241 || header[1] ||
        header[2] != M4_KERNEL_MEMORY_BYTES || header[2] < (u64)(m4_kernel_end - m4_kernel) ||
        header[2] > M4_KERNEL_WINDOW_MAX || (header[3] & 7) != 4) return 6;
    if (m4_direct_smp_prepare(&memory, (struct m4_range){(u64)_base, (u64)(_end - _base)},
                              span, protected)) return 10;
    m4_boot_profile_mark(M4_PROFILE_ARENA_CPU_READY);
    m4_cache_enter(); /* window: ownership and the base/direct/peripheral DT */
    handoff.kernel = ((u64)arena + 0x20000 + 0x1fffff) & ~0x1fffffUL;
    handoff.kernel_size = header[2];
    handoff.initrd = (u64)arena + M4_ARENA_INITRD_OFFSET;
    handoff.initrd_size = m4_initrd_end - m4_initrd;
    handoff.affinity = 0x10100;
    handoff.epoch = M4_BUILD_EPOCH;
    struct m4_range loader = {(u64)_base, (u64)(_end - _base)};
    struct m4_range capture = {(u64)&m4_direct_capture, sizeof(m4_direct_capture)};
    if (!handoff.initrd_size || !m4_contains(span, (struct m4_range){handoff.initrd, handoff.initrd_size}) ||
        handoff.kernel + handoff.kernel_size >= handoff.initrd ||
        !m4_contains(loader,capture) || m4_overlaps(span,capture) || m4_overlaps(protected,capture) ||
        !m4_contains(loader,(struct m4_range){(u64)&cur_boot_args,sizeof(cur_boot_args)}) ||
        m4_direct_desktop_ownership(&memory,loader,span)) return 7;
    m4_boot_profile_mark(M4_PROFILE_OWNERSHIP_READY);
    dt_address = (u64)arena + 0x10000;
    unsigned count; u64 release;
    const struct m4_range *regions = m4_direct_smp_regions(&count,&release);
    unsigned peer_count;
    const struct m4_direct_peer_record *peers=m4_direct_smp_peer_records(&peer_count);
    if (!peers || !regions || count != 5 || release != regions[2].base+64) return 12;
    m4_direct_capture.descriptor = (struct m4_direct_capture_descriptor){
        .magic=M4_DIRECT_CAPTURE_MAGIC, .version=M4_DIRECT_CAPTURE_VERSION, .bytes=sizeof(m4_direct_capture.descriptor),
        .loader_base=loader.base, .loader_end=loader.base+loader.size,
        .arena_base=span.base, .arena_size=span.size,
        .kernel=handoff.kernel, .kernel_memory=handoff.kernel_size,
        .initrd=handoff.initrd, .initrd_bytes=handoff.initrd_size,
        .linux_dt=dt_address, .handoff_addr=(u64)&handoff,
        .adt=(u64)adt, .adt_bytes=cur_boot_args.devtree_size,
        .bootargs_addr=(u64)&cur_boot_args, .release_addr=release,
        .release_page_base=regions[2].base, .release_page_bytes=regions[2].size,
        .primary_mpidr=0x10100, .secondary_mpidr=0x10101,
        .framebuffer=handoff.framebuffer, .framebuffer_bytes=handoff.framebuffer_size,
        .peer_count=peer_count, .peer_table=(u64)peers, .peer_entry_bytes=sizeof(*peers)};
    unsigned found=0;
    for (unsigned i=0;i<memory.free_count;i++)
        if (m4_contains(memory.free[i],(struct m4_range){pcie_base,1024*1024})) found++;
    if (found != 1 || !(m4_pcie_end-m4_pcie) || (u64)(m4_pcie_end-m4_pcie)>1024*1024 ||
        m4_overlaps(loader,(struct m4_range){pcie_base,1024*1024})) return 14;
    j616s_screen_printf("R21-direct: Image=0x%lx DT=0x%lx initrd=0x%lx\n",
                        handoff.kernel, dt_address, handoff.initrd);
    j616s_screen_printf("Inherited EL2 HCR=0x%lx CNTFRQ=%lu; three P cores; Linux owns devices\n",
                        mrs(HCR_EL2), mrs(CNTFRQ_EL0));
    j616s_screen_flush();
    /* Watchdog, DAPF and the opaque PCIe callback run uncached, as before. */
    m4_cache_leave();
    /* Existing bounded, read-back-checked disable of the inherited watchdog. */
    if (m4_watchdog(scratch, 64, 0x50836c000UL, 0x508330008UL, 0x6163) != 0x574454) return 9;
    m4_boot_profile_mark(M4_PROFILE_WATCHDOG_READY);
    /* Accepted returning desktop initializers; no resident execution service. */
    if (m4_dapf(scratch,512,0x616444415046UL,0,0) != 0x44415046) return 15;
    m4_boot_profile_mark(M4_PROFILE_DAPF_READY);
    memcpy((void *)pcie_base,m4_pcie,m4_pcie_end-m4_pcie);
    __asm__ volatile("dsb sy;ic iallu;dsb sy;isb":::"memory");
    u64 (*pcie)(u64 *,u64,u64,u64,u64)=(void *)pcie_base;
    m4_boot_profile_mark(M4_PROFILE_BEFORE_PCIE);
    /* No ADT admission/index is shared across this inherited opaque callback. */
    u64 pcie_rc=pcie(scratch,16384,(u64)adt,cur_boot_args.devtree_size,0x50434945494e4954UL);
    m4_boot_profile_mark(M4_PROFILE_AFTER_PCIE);
    if (pcie_rc != 0x50434945 || scratch[1] != 2 || scratch[2] || scratch[8]) return 16;
    m4_cache_enter(); /* window: Level 1 attempt, DT fix-ups and payload copies */
    if (!m4_contains(loader,(struct m4_range){(u64)peripheral_tree,sizeof(peripheral_tree)}) ||
        m4_overlaps(span,(struct m4_range){(u64)peripheral_tree,sizeof(peripheral_tree)})) return 17;
    m4_boot_ticks[M4_BOOT_DEVICES_READY] = m4_boot_counter();
    /* Boot ABI v1: the default entry of /boot/m4/loader.conf (Image, initramfs.img and board.dtb, size + CRC-32 checked)
     * if boot-pending is absent and the file system is clean, else the embedded rescue set. The entry's board.dtb is
     * read into `tree`; the rescue board.dtb is copied there. Every fix-up then runs on `tree`. */
    struct m4_l1_view l1 = {0};
    m4_level1_attempt(handoff.kernel, handoff.initrd - handoff.kernel - 0x10000, handoff.initrd,
                      (u64)arena + handoff.arena_size - handoff.initrd, m4_template,
                      (size_t)(m4_template_end - m4_template), tree, M4_BOARD_DTB_MAX, &l1);
    const u64 rescue_kernel_memory = header[2];
    const u64 rescue_initrd_bytes = (u64)(m4_initrd_end - m4_initrd);
    size_t image_bytes;
    int built = 1;
    if (l1.ssd) {
        image_bytes = l1.image_bytes;
        handoff.kernel_size = l1.image_memory;
        handoff.initrd_size = l1.initrd_bytes;
        m4_direct_capture.descriptor.kernel_memory = l1.image_memory;
        m4_direct_capture.descriptor.initrd_bytes = l1.initrd_bytes;
        /* the peers' records follow the booted image, not the embedded one */
        if (m4_direct_smp_kernel_memory(l1.image_memory)) return 20;
        payload_kernel_src = payload_initrd_src = NULL;
        built = build_dt(1, l1.cmdline);
        if (built) {   /* the entry's board.dtb does not take the fix-ups: the rescue set boots */
            demote_why = "bad-board-dtb";
            demote_rc = built;
            l1.ssd = 0;
        }
    }
    if (!l1.ssd) {
        image_bytes = m4_kernel_end - m4_kernel;
        handoff.kernel_size = rescue_kernel_memory;
        handoff.initrd_size = rescue_initrd_bytes;
        m4_direct_capture.descriptor.kernel_memory = rescue_kernel_memory;
        m4_direct_capture.descriptor.initrd_bytes = rescue_initrd_bytes;
        if (m4_direct_smp_kernel_memory(rescue_kernel_memory)) return 20;
        payload_kernel_src = m4_kernel;
        payload_initrd_src = m4_initrd;
        if (build_dt(0, rescue_cmdline())) return 17;
    }
    m4_boot_profile_mark(M4_PROFILE_BLUETOOTH_READY);
    if (fdt_totalsize(tree) > M4_FDT_CAPACITY ||
        dt_address + fdt_totalsize(tree) > (u64)arena + 0x10000 + M4_FDT_CAPACITY ||
        dt_address + fdt_totalsize(tree) >= handoff.kernel ||
        !m4_contains(span, (struct m4_range){dt_address, fdt_totalsize(tree)})) return 18;
    /* The admitted Image overwrites its whole prefix; retain the required zero tail. */
    m4_ram_zero((void *)(handoff.kernel + image_bytes), handoff.kernel_size - image_bytes);
    m4_boot_ticks[M4_BOOT_KERNEL_TAIL_ZEROED] = m4_boot_counter();
    if (payload_kernel_src) m4_ram_copy((void *)handoff.kernel, payload_kernel_src, image_bytes);
    m4_boot_ticks[M4_BOOT_KERNEL_COPIED] = m4_boot_counter();
    if (payload_initrd_src) m4_ram_copy((void *)handoff.initrd, payload_initrd_src, handoff.initrd_size);
    m4_boot_ticks[M4_BOOT_INITRD_COPIED] = m4_boot_counter();
    m4_ram_copy((void *)dt_address, tree, fdt_totalsize(tree));
    /* Peers, the GPU capture and Linux read these through RAM: clean them. */
    m4_cache_leave();
    if (payload_check(image_bytes)) return 19;
    m4_boot_ticks[M4_BOOT_INITIAL_DT_COPIED] = m4_boot_counter();
    __asm__ volatile("dsb sy;ic iallu;dsb sy;isb":::"memory");
    if (m4_direct_smp_start()) return 11;
    m4_boot_ticks[M4_BOOT_PEERS_READY] = m4_boot_counter();
    m4_direct_smp_report();
    __asm__ volatile("dsb sy":::"memory");
    m4_direct_capture.descriptor.phase=M4_DIRECT_CAPTURE_ENTERING;
    __asm__ volatile("dsb sy":::"memory");
    return 0;
}

/* Optional reference is captured in its own restored abort guard. The pair
 * is already in its reserved NOP holding pen; this does not release a core. */
static int capture_display_reference(void)
{
    m4_cache_enter(); /* window: read-only DART/display capture; MMIO stays Device */
    int rc = m4_display_handoff_capture(&memory,
        (struct m4_range){(u64)_base,(u64)(_end-_base)},
        (struct m4_range){handoff.arena_base,handoff.arena_size},
        handoff.framebuffer,handoff.framebuffer_size);
    m4_cache_leave();
    return rc;
}

void j616s_standalone_report(u64 lower, u64 upper)
{
    lower_bound = lower; upper_bound = upper;
    struct j616s_call_result result = {0};
    /* R36 E0: read-only record of CYC_OVRD/ACC_CFG/ACC_OVRD on this (primary) CPU, one guarded read each.
     * Never writes, never stops the boot; published into /chosen below. */
    m4_cpu_idle_regs_read();
    int guard = j616s_usb_call(prepare, &result);
    m4_cache_leave(); /* an abort inside a cached window returns here with the MMU on */
    if (!guard && !result.status && !result.value) {
        struct j616s_call_result reference = {0};
        int reference_guard = j616s_usb_call(capture_display_reference, &reference);
        m4_cache_leave();
        m4_boot_ticks[M4_BOOT_DISPLAY_CAPTURE_FINISHED] = m4_boot_counter();
        m4_boot_profile_ticks[M4_PROFILE_DISPLAY_CAPTURE_RETURNED]=m4_boot_ticks[M4_BOOT_DISPLAY_CAPTURE_FINISHED];
        /* R36 media probe: single guarded reads (AOP ASC, PMGR power state, AVD
         * words when already powered). Records aborts, never writes, never
         * stops the boot; SCTLR is back at the guard baseline here. */
        m4_cache_enter(); /* window: publication; ends before the branch to Linux */
        /* The late records use the 256 KiB DT window that ends at the kernel.
         * Without the window buffers this is the 128 KiB slot. */
        void *dt = tree, *work = peripheral_tree;
        size_t capacity = sizeof(tree);
        u64 limit = dt_address + M4_FDT_CAPACITY;
        if (m4_speed.late_capacity &&
            !fdt_open_into(tree, m4_speed.late_tree, (int)m4_speed.late_capacity)) {
            dt = m4_speed.late_tree; work = m4_speed.late_scratch;
            capacity = m4_speed.late_capacity; limit = dt_address + capacity;
        }
        if (limit > handoff.kernel) limit = handoff.kernel;
        /* R36 E0: one small /chosen property; a refusal leaves the tree untouched. */
        (void)m4_cpu_idle_regs_publish(dt, capacity);
        (void)m4_level1_publish(dt, capacity); /* /chosen m4,loader-*, m4,boot-*, m4,ans-live, m4,ans-syslog */
        /* rev 12: the board.dtb limit this loader accepts, for m4-boot (absent on rev 11 = 36,864 B) */
        if (!fdt_open_into(dt, dt, (int)capacity)) {
            int c = fdt_path_offset(dt, "/chosen");
            if (c >= 0) fdt_setprop_u32(dt, c, "m4,board-dtb-max", M4_BOARD_DTB_MAX);
            fdt_pack(dt);
        }
        /* rev 13: the PMP firmware's ADT properties for the kernel's PMP node, if it has one;
         * all or nothing, never status (pmp-publish.h) */
        (void)m4_pmp_publish(dt, capacity, (size_t)(limit - dt_address));
        if (demote_why && !fdt_open_into(dt, dt, (int)capacity)) {
            int c = fdt_path_offset(dt, "/chosen");
            char detail[24];
            snprintf(detail, sizeof(detail), "fixup-step-%d", demote_rc);
            if (c >= 0) {
                fdt_setprop_string(dt, c, "m4,boot-source", "rescue");
                fdt_setprop_string(dt, c, "m4,boot-entry", "rescue");
                fdt_setprop_string(dt, c, "m4,boot-reason", demote_why);
                fdt_setprop_string(dt, c, "m4,boot-detail", detail);
            }
            fdt_pack(dt);
        }
        int published = m4_display_handoff_publish(dt, capacity, dt == tree ? NULL : work, capacity,
                                                   reference_guard, &reference);
        /* Every record below is an atomic edit of dt; one copy follows them. */
        if (!published && (fdt_check_full(dt, capacity) || !fdt_totalsize(dt) ||
                           dt_address + fdt_totalsize(dt) > limit)) published = -1001;
        m4_boot_ticks[M4_BOOT_DISPLAY_PUBLISHED] = m4_boot_counter();
        m4_boot_profile_ticks[M4_PROFILE_DISPLAY_DT_COPIED]=m4_boot_ticks[M4_BOOT_DISPLAY_PUBLISHED];
        /* An unavailable reference never overturns the already accepted CPU,
         * storage, input or GPU handoff. Its inert chosen record reports fault. */
        if (published || reference_guard || reference.status || reference.value)
            j616s_screen_printf("Display reference incomplete: guard=%d stage=%lu fault=%lu publish=%d\n",
                reference_guard, reference.value, reference.status, published);
        j616s_screen_puts("R21-direct: entering Linux at EL2; loader will not resume.");
        j616s_screen_flush();
        /* Outside the guard: its VBAR/DAIF/recovery frame has been restored. */
        m4_boot_profile_mark(M4_PROFILE_BEFORE_PROFILE_PUBLISH);
        u64 bytes = fdt_totalsize(dt);
        if (!fdt_check_full(dt, capacity) && bytes && bytes <= capacity && dt_address + bytes <= limit) {
            m4_ram_copy((void *)dt_address, dt, bytes);
            __asm__ volatile("dsb sy":::"memory");
            m4_speed.dt_bytes = bytes;
        }
        /* Final clean: DT, descriptor and peer-visible records reach RAM. */
        m4_cache_leave();
        int intact = 1;
        if (m4_speed.dt_bytes && m4_cache.enters) {
            unsigned bad = m4_cache_compare((void *)dt_address, dt, bytes, bytes);
            m4_speed.dt_mismatches = bad;
            m4_speed.dt_check = bad ? M4_COPY_RECOPIED : M4_COPY_MATCHED;
            if (bad) {
                m4_cache_scrub();
                m4_ram_copy((void *)dt_address, dt, bytes);
                intact = !m4_cache_compare((void *)dt_address, dt, bytes, bytes);
            }
        }
        if (intact) {
            m4_loader_speed_finish((void *)dt_address);
            __asm__ volatile("dsb sy":::"memory");
            m4_direct_enter(handoff.kernel, dt_address);
        }
        /* RAM does not hold the DT the loader wrote: never branch on it. */
        result.value = 20;
    }
    if (m4_direct_smp_launched() && !m4_direct_smp_diagnostics_safe()) {
        /* A late/failed peer owns its allocations; never reuse the framebuffer. */
        __asm__ volatile("msr daifset,#15; dsb sy":::"memory");
        for (;;) __asm__ volatile("nop":::"memory");
    }
    /* Retained boot output is revealed only after a failed admission. R41: the prepare() guard result
     * (what stopped the boot, with ESR/ELR/FAR) and the Level 1 summary are the FIRST lines under the
     * title, before the replay and the peer records (R39's stop page lost the primary fault off the
     * bottom of the screen behind 14 peer dumps). */
    char first[480];
    int n = snprintf(first, sizeof(first), "R21-direct STOP: guard=%d stage=%lu fault=%lu\n",
                     guard, result.value, result.status);
    n += snprintf(first + n, sizeof(first) - n, "ESR=0x%lx ELR=0x%lx FAR=0x%lx\n", result.esr, result.elr, result.far);
    if (n > 0 && (size_t)n < sizeof(first) - 8) {
        size_t d = m4_level1_describe(first + n, sizeof(first) - n - 2);
        if (d) { first[n + d] = '\n'; first[n + d + 1] = 0; }
    }
    j616s_screen_show_error_first(first, 8);
    m4_direct_smp_report();
    j616s_screen_puts("Linux not entered. Photograph this page; hold power to turn off.");
    j616s_screen_flush();
    for (;;) __asm__ volatile("nop");
}

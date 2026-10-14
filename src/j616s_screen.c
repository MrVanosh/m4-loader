/* SPDX-License-Identifier: GPL-2.0-only */
#include "../build/build_cfg.h"
#ifdef J616S_SCREEN_DIAGNOSTIC
#include "j616s_screen.h"
#include "j616s_boot_prefix.h"
#include "adt.h"
#include "string.h"
#include "vsprintf.h"

extern char _base[], _end[];
extern u8 _binary_build_font_bin_start[], _binary_build_font_bin_end[];
extern u8 _binary_build_font_retina_bin_start[], _binary_build_font_retina_bin_end[];
static struct j616s_screen screen;
#include "standalone/boot-output.h"
static struct m4_boot_output boot_output = {.visible = 0};
static void output_pixel(void *opaque, unsigned char c) {
    (void)opaque; j616s_screen_char(&screen, c);
}
static void output_char(unsigned char c) {
    m4_boot_output_char(&boot_output, c, output_pixel, NULL);
}
void j616s_screen_show_error(void) {
    m4_boot_output_reveal(&boot_output, output_pixel, NULL, NULL, 16);
}
/* R41: the primary fault first (printed right after the title), then at most max_lines of the retained output. */
void j616s_screen_show_error_first(const char *first, unsigned max_lines) {
    m4_boot_output_reveal(&boot_output, output_pixel, NULL, first, max_lines);
}
void j616s_screen_show_status(void) {
    boot_output.visible = 1; boot_output.count = 0;
}

static void quiet_stop(void) __attribute__((noreturn));
static void quiet_stop(void)
{
    /* An unvalidated console is not an excuse to initialize other hardware. */
    for (;;)
        __asm__ volatile("nop");
}

unsigned j616s_screen_columns(void)
{
    return screen.cols;
}

void j616s_screen_puts(const char *s)
{
    while (*s)
        output_char(*s++);
    output_char('\n');
}

int j616s_screen_printf(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        screen.truncated = true;
        return n;
    }
    for (int i = 0; i < n; i++)
        output_char(buf[i]);
    return n;
}

/* R39 Level 1: one progress line straight to the framebuffer, even while the
 * boot output is retained. The last line on a photo names the step that hung.
 * Not retained, so a later error reveal does not repeat it. */
void j616s_screen_live(const char *s)
{
    while (*s)
        output_pixel(NULL, (unsigned char)*s++);
    output_pixel(NULL, '\n');
    j616s_screen_flush();
}

void j616s_screen_flush(void)
{
    if (screen.pixels && screen.truncated) {
        screen.row = screen.rows;
        screen.rows++;
        screen.col = 0;
        j616s_screen_puts("OUTPUT TRUNCATED: NOT A COMPLETE CAPTURE");
        screen.rows--;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool property(int node, const char *name, void *out, u32 expected)
{
    u32 len = 0;
    const void *p = node < 0 ? NULL : adt_getprop(adt, node, name, &len);
    if (!p || len != expected)
        return false;
    memcpy(out, p, expected);
    return true;
}

static bool string_property(int node, const char *name, const char *expected)
{
    u32 len = 0;
    const char *p = node < 0 ? NULL : adt_getprop(adt, node, name, &len);
    size_t expected_len = strlen(expected) + 1;
    return p && len == expected_len && !memcmp(p, expected, expected_len);
}

void j616s_screen_entry(void)
{
    u64 el, sctlr;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    /* Only direct EL2 entry is supported. At EL1 an unseen EL2 configuration
     * could override cacheability even with SCTLR_EL1.M/C clear. */
    if (el != 8)
        quiet_stop();
    __asm__ volatile("mrs %0, SCTLR_EL2" : "=r"(sctlr));
    /* Require physical, uncached stores. Do not alter inherited translation
     * or cache configuration and do not perform cache-maintenance writes. */
    if (sctlr & 5)
        quiet_stop();
    if (!j616s_boot_prefix_revision(cur_boot_args.revision) || adt_check_header(adt) < 0)
        quiet_stop();
    int chosen = adt_path_offset(adt, "/chosen");
    int vram_node = adt_path_offset(adt, "/vram");
    u32 chip, addr_cells, size_cells;
    u64 dram, dram_size, vram[2];
    if (!string_property(0, "target-type", "J616s") ||
        !property(chosen, "chip-id", &chip, sizeof(chip)) || chip != 0x6040 ||
        !property(chosen, "dram-base", &dram, sizeof(dram)) ||
        !property(chosen, "dram-size", &dram_size, sizeof(dram_size)) ||
        !property(0, "#address-cells", &addr_cells, sizeof(addr_cells)) || addr_cells != 2 ||
        !property(0, "#size-cells", &size_cells, sizeof(size_cells)) || size_cells != 2 ||
        !property(vram_node, "reg", vram, sizeof(vram)))
        quiet_stop();
    /* /vram is a root child: the two little-endian u64 reg fields are physical
     * base and size. Refuse any other layout instead of guessing translation. */
    struct j616s_live_range live[] = {
        {(u64)_base, (u64)_end - (u64)_base},
        {(u64)adt, cur_boot_args.devtree_size},
        {boot_args_addr, sizeof(cur_boot_args)},
    };
    if (!j616s_range_aligned(dram, dram_size) ||
        !j616s_range_contains(dram, dram_size, cur_boot_args.phys_base,
                              cur_boot_args.mem_size) ||
        !j616s_range_contains(cur_boot_args.phys_base, cur_boot_args.mem_size,
                              live[0].start, live[0].size))
        quiet_stop();
    bool ready = j616s_screen_configure(&screen, &cur_boot_args.video, dram, dram_size,
        vram[0], vram[1], live, 3, _binary_build_font_retina_bin_start,
        _binary_build_font_retina_bin_end - _binary_build_font_retina_bin_start, 2);
    if (!ready)
        ready = j616s_screen_configure(&screen, &cur_boot_args.video, dram, dram_size,
            vram[0], vram[1], live, 3, _binary_build_font_bin_start,
            _binary_build_font_bin_end - _binary_build_font_bin_start, 1);
    if (!ready)
        quiet_stop();
    j616s_screen_puts("J616S SCREEN R21-direct 20261007 -- FRAMEBUFFER BOUNDS CHECKED");
    j616s_screen_printf("bootargs revision=%u version=%u prefix=%lu bytes\n",
        cur_boot_args.revision, cur_boot_args.version, (u64)J616S_BOOT_PREFIX_SIZE);
    j616s_screen_printf("EL=%lu SCTLR=0x%lx video=%lux%lu stride=%lu depth=0x%lx\n",
        el >> 2, sctlr, cur_boot_args.video.width, cur_boot_args.video.height,
        cur_boot_args.video.stride, cur_boot_args.video.depth);
    j616s_screen_puts("Two P cores, guarded Omarchy desktop; direct Linux entry.");
    j616s_screen_puts("Checking revision and firmware/controller baseline next.");
    j616s_screen_flush();
    /* The validator above checked the entire conservative boot-args live
     * extent against DRAM and the framebuffer. Only revision 3's metadata
     * path may read its single required trailing field. Older revisions can
     * show a rejection reason without an out-of-layout tail read. */
    if (cur_boot_args.revision == 3)
        memcpy(&cur_boot_args.rv3.mem_size_actual,
               (const u8 *)(uintptr_t)boot_args_addr +
                   offsetof(struct boot_args, rv3.mem_size_actual),
               sizeof(cur_boot_args.rv3.mem_size_actual));
    j616s_diag_run();
}
#endif

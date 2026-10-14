/* SPDX-License-Identifier: GPL-2.0-only */
/* Linked to generated host copies of the real screen/metadata C routines.
 * Only CPU reads, barriers, terminal stop loops, linker symbols and log taps
 * are substituted by the validation script. All decision logic and framebuffer
 * writes remain the implementation under test. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../src/j616s_boot_prefix.h"
#include "../src/j616s_screen.h"
#include "../src/j616s_probe.h"
#include "../src/adt.h"

struct boot_args cur_boot_args;
u64 boot_args_addr, test_el, test_sctlr;
void *adt;
char *test_image_base, *test_image_end;
u8 *test_font_start, *test_font_end, *test_retina_start, *test_retina_end;
static u8 *arena;
enum { ARENA_SIZE = 0x2800000 };
static u64 dram, dram_size, vram[2], region[2];
static u32 chip, cells;
static const char *fw, *sys_fw, *mcc;
static const u8 *fw_raw;
static u32 fw_length;
static bool fw_override;
static int fw_override_node;
static const char *fw_override_name;
static const void *mapping_data[3];
static u32 mapping_lengths[3];
static char output[16384];
static size_t used;
static int property_reads;
static unsigned probe_calls, fail_probe, usb_calls;
static bool trap_apple;
static jmp_buf stop;

void test_finish(int why) __attribute__((noreturn));
void test_finish(int why) { longjmp(stop, why); }
void test_log(const char *s)
{
    size_t n = strlen(s);
    assert(used + n + 2 < sizeof(output));
    memcpy(output + used, s, n);
    used += n;
    output[used] = 0;
}
void test_barrier(void) {}
int adt_check_header(const void *dt) { assert(dt == adt); return 0; }
int adt_path_offset(const void *dt, const char *p)
{
    assert(dt == adt);
    if (!strcmp(p, "/chosen")) return 1;
    if (!strcmp(p, "/arm-io/mcc")) return 2;
    if (!strcmp(p, "/vram")) return 3;
    if (!strcmp(p, "/chosen/carveout-memory-map")) return 4;
    if (!strcmp(p, "/defaults")) return 5;
    if (!strcmp(p, "/arm-io")) return 6;
    return -1;
}
const void *adt_getprop(const void *dt, int node, const char *name, u32 *len)
{
    assert(dt == adt);
    property_reads++;
    const void *p = NULL;
    u32 n = 0;
#define VALUE(nodeid, key, value) \
    if (node == nodeid && !strcmp(name, key)) { p = &(value); n = sizeof(value); }
#define TEXT(nodeid, key, value) \
    if (node == nodeid && !strcmp(name, key) && (value)) { p = value; n = strlen(value) + 1; }
    TEXT(0, "target-type", "J616s");
    VALUE(0, "#address-cells", cells);
    VALUE(0, "#size-cells", cells);
    VALUE(1, "chip-id", chip);
    VALUE(1, "dram-base", dram);
    VALUE(1, "dram-size", dram_size);
    TEXT(1, "firmware-version", fw);
    TEXT(1, "system-firmware-version", sys_fw);
    TEXT(2, "compatible", mcc);
    const char *mapping_names[] = {"iboot-handoff", "pmap-io-ranges", "ranges"};
    const int mapping_nodes[] = {1, 5, 6};
    for (int i = 0; i < 3; i++) {
        if (node == mapping_nodes[i] && !strcmp(name, mapping_names[i])) {
            p = mapping_data[i];
            n = mapping_lengths[i];
        }
    }
    VALUE(3, "reg", vram);
    if (node == 4 && !strncmp(name, "region-id-", 10)) { p = region; n = sizeof(region); }
    if (fw_override && node == fw_override_node && !strcmp(name, fw_override_name)) {
        p = fw_raw;
        n = fw_length;
    }
    if (len) *len = n;
    return p;
}

static void setup(u16 revision)
{
    memset(arena, 0, ARENA_SIZE);
    dram = (uintptr_t)arena;
    dram_size = ARENA_SIZE;
    test_image_base = (char *)arena;
    test_image_end = (char *)arena + 0x4000;
    adt = arena + 0x4000;
    boot_args_addr = dram + 0x8000;
    struct boot_args *raw = (void *)(uintptr_t)boot_args_addr;
    raw->revision = revision;
    raw->version = 2;
    raw->phys_base = dram;
    raw->virt_base = dram;
    raw->mem_size = dram_size;
    raw->devtree = adt;
    raw->devtree_size = 0x1000;
    raw->video = (struct boot_video){dram + 0x100000, 1, 1040 * 4, 1024, 832, 32};
    raw->rv3.mem_size_actual = dram_size;
    memset(&cur_boot_args, 0, sizeof(cur_boot_args));
    j616s_copy_boot_prefix(&cur_boot_args, raw);
    vram[0] = raw->video.base;
    vram[1] = raw->video.stride * raw->video.height;
    region[0] = dram + 0xc000;
    region[1] = 0x4000;
    test_font_start = arena + 0x10000;
    test_font_end = test_font_start + 95 * 8 * 16;
    test_retina_start = arena + 0x20000;
    test_retina_end = test_retina_start + 95 * 16 * 32;
    memset(test_font_start, 255, test_font_end - test_font_start);
    memset(test_retina_start, 255, test_retina_end - test_retina_start);
    test_el = 8;
    test_sctlr = 0;
    chip = 0x6040;
    cells = 2;
    fw = "mBoot-18000.161.10";
    sys_fw = "mBoot-20457.1.29";
    fw_override = false;
    fw_override_node = 1;
    fw_override_name = "firmware-version";
    mcc = "mcc,t6041";
    for (int i = 0; i < 3; i++) {
        mapping_data[i] = (u8 *)adt + 0x500 + 0x100 * i;
        mapping_lengths[i] = i ? 24 : 16;
    }
    const u64 handoff[] = {dram, 0x4000};
    const u64 pmap[] = {dram + 0x400000, 0x4000, 0x1234567860004016};
    const u64 armio[] = {0, 0x400000000, 0x200000000};
    memcpy((void *)mapping_data[0], handoff, sizeof(handoff));
    memcpy((void *)mapping_data[1], pmap, sizeof(pmap));
    memcpy((void *)mapping_data[2], armio, sizeof(armio));
    output[0] = 0;
    used = 0;
    property_reads = 0;
    usb_calls = 0; probe_calls = 0; fail_probe = 99; trap_apple = false;
}

static int run(void)
{
    int why = setjmp(stop);
    if (!why) j616s_screen_entry();
    return why;
}

static void raw_firmware(const void *bytes, u32 length)
{
    fw_override = true;
    fw_raw = bytes;
    fw_length = length;
}

static void rejected(void)
{
    assert(run() == 2);
    assert(strstr(output, "missing/malformed string firmware-version"));
    assert(strstr(output, "STOP"));
    assert(!strstr(output, "metadata captured"));
}

static void rejected_property_tests(void)
{
    /* All byte fixtures are inside the simulated ADT except the explicit
     * outside-extent case. Assert real output, not merely a helper return. */
    setup(3);
    raw_firmware(NULL, 0);
    rejected();
    assert(strstr(output, "firmware-version present=0 length=0"));
    assert(strstr(output, "no bytes to read"));

    setup(3);
    u8 *property = (u8 *)adt + 0x200;
    raw_firmware(property, 0);
    rejected();
    assert(strstr(output, "firmware-version present=1 length=0"));

    setup(3);
    memcpy(property, "mBoot-18000.161.10", sizeof("mBoot-18000.161.10"));
    raw_firmware(property, 32); /* valid text plus extra NUL padding */
    rejected();
    assert(strstr(output, "showing 32/32 bytes"));
    assert(strstr(output, "6d 42 6f 6f 74"));
    assert(strstr(output, "0010: 31 30 00 00"));

    setup(3);
    memcpy(property, "ABCD", 4); /* no NUL */
    raw_firmware(property, 4);
    rejected();
    assert(strstr(output, "0000: 41 42 43 44 |ABCD|"));

    setup(3);
    const u8 binary[] = {0xff, 0, 0x0a, 0x7f};
    memcpy(property, binary, sizeof(binary));
    raw_firmware(property, sizeof(binary));
    rejected();
    assert(strstr(output, "0000: ff 00 0a 7f |....|"));

    setup(3);
    memset(property, 'A', 128);
    raw_firmware(property, 128);
    rejected();
    assert(strstr(output, "showing 128/128 bytes"));
    assert(strstr(output, "0070: ") && !strstr(output, "0080: "));

    /* Place only 128 readable bytes immediately before an inaccessible page.
     * A large declared extent is inside the simulated ADT, but the dump must
     * never read byte 129 even when that extent claims more readable data. */
    setup(3);
    long page = sysconf(_SC_PAGESIZE);
    assert(page == 4096);
    cur_boot_args.devtree_size = 2 * page;
    u8 *guard = (u8 *)adt + page;
    property = guard - 128;
    memset(property, 'B', 128);
    assert(mprotect(guard, page, PROT_NONE) == 0);
    raw_firmware(property, 512);
    rejected();
    assert(strstr(output, "showing 128/512 bytes"));
    assert(strstr(output, "0070: ") && !strstr(output, "0080: "));
    /* Now the property is outside the declared ADT. Even its first byte
     * is inaccessible, so the diagnostic must refuse without any read. */
    cur_boot_args.devtree_size = page;
    raw_firmware(guard, 512);
    output[0] = 0;
    used = 0;
    rejected();
    assert(strstr(output, "extent outside ADT; bytes not read"));
    assert(!strstr(output, "0000: "));
    raw_firmware(guard, 256);
    output[0] = 0;
    used = 0;
    rejected();
    assert(strstr(output, "extent outside ADT; bytes not read"));
    assert(!strstr(output, "all padding checked"));
    assert(mprotect(guard, page, PROT_READ | PROT_WRITE) == 0);
}

static void firmware_buffer_tests(void)
{
    setup(3);
    u8 *property = (u8 *)adt + 0x200;
    memcpy(property, "mBoot-18000.161.10", sizeof("mBoot-18000.161.10"));
    raw_firmware(property, 256);
    assert(run() == 2);
    assert(strstr(output, "length=256 text=18; all padding checked"));
    assert(strstr(output, "os-fw=mBoot-18000.161.10 system-fw=mBoot-20457.1.29 mcc=mcc,t6041"));
    assert(strstr(output, "observed firmware pair matched"));
    assert(strstr(output, "metadata captured"));
    assert(!strstr(output, "missing/malformed"));

    /* Visible padding and the previously unobserved half are equally strict. */
    const u32 bad_offsets[] = {19, 100, 128, 200, 255};
    for (unsigned i = 0; i < sizeof(bad_offsets) / sizeof(bad_offsets[0]); i++) {
        setup(3);
        memcpy(property, "mBoot-18000.161.10", sizeof("mBoot-18000.161.10"));
        property[bad_offsets[i]] = 'X';
        raw_firmware(property, 256);
        rejected();
        char message[80];
        snprintf(message, sizeof(message), "nonzero padding at offset=%u byte=0x58",
                 bad_offsets[i]);
        assert(strstr(output, message));
    }

    setup(3);
    memcpy(property, "mBoot-unreviewed", sizeof("mBoot-unreviewed"));
    raw_firmware(property, 256);
    assert(run() == 2);
    assert(strstr(output, "all padding checked"));
    assert(strstr(output, "outside the reviewed firmware/controller baseline"));
    assert(!strstr(output, "metadata captured"));

    setup(3); /* An empty fixed buffer is never a matching firmware version. */
    raw_firmware(property, 256);
    assert(run() == 2 && strstr(output, "outside the reviewed firmware"));
    assert(!strstr(output, "metadata captured"));

    setup(3);
    memset(property, 'A', 256);
    raw_firmware(property, 256);
    rejected();
    assert(strstr(output, "no terminator within first 128 bytes"));
    for (u32 length = 255; length <= 257; length += 2) {
        setup(3);
        memcpy(property, "mBoot-18000.161.10", sizeof("mBoot-18000.161.10"));
        raw_firmware(property, length);
        rejected();
        assert(!strstr(output, "all padding checked"));
    }

    /* No other property gains padded-buffer support. */
    setup(3);
    memcpy(property, "mBoot-18000.161.10", sizeof("mBoot-18000.161.10"));
    raw_firmware(property, 256);
    fw_override_name = "system-firmware-version";
    assert(run() == 2);
    assert(strstr(output, "missing/malformed string system-firmware-version"));
    assert(!strstr(output, "metadata captured"));
    setup(3);
    memcpy(property, "mcc,t6041", sizeof("mcc,t6041"));
    raw_firmware(property, 256);
    fw_override_node = 2;
    fw_override_name = "compatible";
    assert(run() == 2 && strstr(output, "missing/malformed string compatible"));
    assert(!strstr(output, "metadata captured"));
}

int j616s_probe_once(unsigned op, struct j616s_probe_result *r)
{
    assert(op == probe_calls++);
    if (op == fail_probe) return -1;
    *r = (struct j616s_probe_result){0, 8, 0, 0};
    if (op == J616S_PROBE_BRK)
        *r = (struct j616s_probe_result){1, 0, 0xf2000616, 0x1000};
    if (op == J616S_PROBE_UDF || (trap_apple && op >= J616S_PROBE_LOWER))
        *r = (struct j616s_probe_result){1, 0, 0x02000000, 0x1000 + op * 4};
    if (!trap_apple && op >= J616S_PROBE_LOWER) r->value = 0x1000000c000ull + op * 0x1000;
    return 0;
}

void j616s_usb_report(u64 lower, u64 upper)
{
    assert(probe_calls == 5 && !trap_apple && fail_probe == 99);
    assert(lower == 0x1000000f000ull && upper == 0x10000010000ull);
    usb_calls++;
    /* Reserve the maximum report's line count to verify the real console fit. */
    for (unsigned i = 0; i < 20; i++) j616s_screen_puts("USB test report placeholder");
}

static void register_probe_tests(void)
{
    for (unsigned fail = 0; fail < 5; fail++) {
        setup(3); fail_probe = fail;
        assert(run() == 2);
        assert(probe_calls == fail + 1 && usb_calls == 0);
        assert(strstr(output, "PROBE REFUSED"));
        assert(!strstr(output, "PROBE COMPLETE"));
        assert(strstr(output, "STOP"));
    }
    for (unsigned trapped = 0; trapped < 2; trapped++) {
        setup(3); trap_apple = trapped;
        struct boot_args *raw = (void *)(uintptr_t)boot_args_addr;
        raw->video = (struct boot_video){dram + 0x100000, 1, 13824, 3456, 2234, 0x1001e};
        cur_boot_args.video = raw->video;
        vram[1] = raw->video.stride * raw->video.height;
        assert(run() == 2 && probe_calls == 5);
        assert(usb_calls == !trapped);
        assert(strstr(output, "PROBE SELF-TEST PASS"));
        assert(strstr(output, "PROBE COMPLETE"));
        assert(strstr(output, "S3_0_C11_C0_0") && strstr(output, "S3_0_C11_C0_1"));
        assert(strstr(output, trapped ? "TRAPPED ESR=" : "RAW=0x"));
        assert(!strstr(output, trapped ? "RAW=0x" : "TRAPPED ESR="));
        assert(!strstr(output, "OUTPUT TRUNCATED"));
        assert(strstr(output, "watchdog unchanged"));
    }
    setup(3); fw = "mBoot-unreviewed";
    assert(run() == 2 && probe_calls == 0);
    setup(3); mcc = "mcc,unreviewed";
    assert(run() == 2 && probe_calls == 0);
}

int main(void)
{
    arena = aligned_alloc(0x4000, ARENA_SIZE);
    assert(arena);
    setup(3);
    assert(run() == 2);
    assert(strstr(output, "SCREEN DIAGNOSTIC v15"));
    assert(strstr(output, "metadata captured"));
    assert(strstr(output, "STOP"));
    assert(cur_boot_args.rv3.mem_size_actual == dram_size);
    assert(*(u32 *)(uintptr_t)(vram[0] + (64 * 1040 + 32) * 4) == 0xffffff);
    for (u16 rev = 1; rev <= 2; rev++) {
        setup(rev);
        assert(run() == 2);
        assert(strstr(output, "SCREEN DIAGNOSTIC v15"));
        assert(strstr(output, "unsupported boot-args revision"));
        assert(!strstr(output, "metadata captured"));
        assert(cur_boot_args.rv3.mem_size_actual == 0);
    }
    setup(3); fw = "mBoot-unreviewed";
    assert(run() == 2);
    assert(strstr(output, "SCREEN DIAGNOSTIC v15"));
    assert(strstr(output, "mBoot-unreviewed"));
    assert(strstr(output, "outside the reviewed firmware"));
    assert(!strstr(output, "metadata captured"));
    /* Each field is independent; reject old, swapped and near-match pairs. */
    const char *bad_system[] = {"mBoot-18000.161.10", "mBoot-20457.1.28",
                               "mBoot-20457.1.290", "mBoot-unreviewed", ""};
    for (unsigned i = 0; i < sizeof(bad_system) / sizeof(bad_system[0]); i++) {
        setup(3); sys_fw = bad_system[i];
        assert(run() == 2 && strstr(output, "outside the reviewed firmware"));
        assert(!strstr(output, "metadata captured"));
    }
    setup(3); fw = "mBoot-20457.1.29"; sys_fw = "mBoot-18000.161.10";
    assert(run() == 2 && strstr(output, "outside the reviewed firmware"));
    assert(!strstr(output, "metadata captured"));
    setup(3); sys_fw = NULL;
    assert(run() == 2 && strstr(output, "missing/malformed string system-firmware-version"));
    assert(!strstr(output, "metadata captured"));
    setup(3); mcc = "mcc,unreviewed";
    assert(run() == 2 && strstr(output, "outside the reviewed firmware"));
    setup(3); fw = NULL;
    assert(run() == 2 && strstr(output, "missing/malformed string firmware-version"));
    setup(3); vram[0] += 4;
    assert(run() == 1 && used == 0);
    assert(*(u32 *)(arena + 0x100000) == 0);
    setup(4);
    assert(run() == 1 && used == 0 && property_reads == 0);
    setup(3); test_el = 4;
    assert(run() == 1 && used == 0 && property_reads == 0);
    setup(3); test_sctlr = 1;
    assert(run() == 1 && used == 0 && property_reads == 0);
    setup(3); test_sctlr = 4;
    assert(run() == 1 && used == 0 && property_reads == 0);
    rejected_property_tests();
    firmware_buffer_tests();
    register_probe_tests();
    free(arena);
    puts("screen flow: fixed firmware buffer, exact baseline, padding/extent rejection and guards: PASS");
}

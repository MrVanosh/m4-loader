/* SPDX-License-Identifier: MIT */
/* The loader's decision for a /boot/m4, on the host: the production policy (level1-core.c: boot-pending, ext4 clean,
 * loader.conf, entry.conf, size + CRC-32 of Image / initramfs.img / board.dtb, Image header, DTB check) run over an ext4 image.
 *   loader-decision [--offset BYTES] [--window-kernel B] [--window-initrd B] [--boots N] <ext4-or-disk-image>
 * rev 17: the one-shot token's write goes to an in-memory overlay (the image is never written); --boots N runs the
 * decision N times over that overlay, one line per boot, so a spent token's next boot is visible.
 * A whole-disk image (GPT) is searched for the ext4 partition that holds /boot/m4. Prints one line:
 *   source=entry|rescue entry=<name> reason=<word> detail=<status> kver=<kver> cmdline=<cmdline>
 * and, on stderr, the step log. Exit 0 when a decision was made (rescue is a decision), 2 on usage / I/O errors. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "level1.h"
#include "libfdt.h"

static int fd = -1;
static uint64_t part_off;
#define OVERLAY_MAX 8
static struct { uint64_t lba; uint8_t data[4096]; } overlay[OVERLAY_MAX];
static unsigned overlay_n, writes;
static int dev_read(void *ctx, uint64_t lba, uint32_t n, void *dst)
{
    (void)ctx;
    uint64_t rel = lba - L1_P4_FIRST;     /* the policy addresses the SSD partition; the image is that partition */
    size_t bytes = (size_t)n * 4096;
    ssize_t r = pread(fd, dst, bytes, (off_t)(part_off + rel * 4096));
    if (r < 0) return L1_NVME_READ;
    if ((size_t)r < bytes) memset((uint8_t *)dst + r, 0, bytes - (size_t)r);
    for (unsigned i = 0; i < overlay_n; i++)
        if (overlay[i].lba >= lba && overlay[i].lba < lba + n) memcpy((uint8_t *)dst + (overlay[i].lba - lba) * 4096, overlay[i].data, 4096);
    return 0;
}
static int dev_write(void *ctx, uint64_t lba, const void *src)
{
    (void)ctx;
    unsigned i = 0;
    while (i < overlay_n && overlay[i].lba != lba) i++;
    if (i == overlay_n) { if (overlay_n == OVERLAY_MAX) return L1_NVME_READ; overlay_n++; }
    overlay[i].lba = lba;
    memcpy(overlay[i].data, src, 4096);
    writes++;
    return 0;
}
static void step(void *ctx, const char *name, uint64_t v) { (void)ctx; fprintf(stderr, "step %s %llu\n", name, (unsigned long long)v); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static int has_boot_m4(uint64_t off, uint64_t bytes)
{
    uint8_t sb[2048];
    if (pread(fd, sb, sizeof(sb), (off_t)(off + 0)) != (ssize_t)sizeof(sb)) return 0;
    return sb[1024 + 0x38] == 0x53 && sb[1024 + 0x39] == 0xef;   /* ext4 magic; the policy checks /boot/m4 itself */
    (void)bytes;
}
static void *aligned(size_t n) { void *p; return posix_memalign(&p, 16384, n) ? NULL : p; }

int main(int argc, char **argv)
{
    uint64_t kw = 29949952, iw = 16u << 20, dw = 96 * 1024; /* rev 12 board.dtb window (rev 11: 36 * 1024) */
    unsigned boots = 1;
    const char *img = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--offset") && i + 1 < argc) part_off = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--window-kernel") && i + 1 < argc) kw = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--window-initrd") && i + 1 < argc) iw = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--window-dtb") && i + 1 < argc) dw = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--boots") && i + 1 < argc) boots = (unsigned)strtoul(argv[++i], 0, 0);
        else img = argv[i];
    }
    if (!img) { fprintf(stderr, "usage: loader-decision [--offset BYTES] <ext4-or-disk-image>\n"); return 2; }
    fd = open(img, O_RDONLY);
    if (fd < 0) { perror(img); return 2; }
    if (!part_off && !has_boot_m4(0, 0)) {   /* a disk image: first GPT partition (512 or 4096 byte sectors) with an ext4 superblock */
        for (unsigned ss = 512; ss <= 4096 && !part_off; ss *= 8) {
            uint8_t h[512];
            if (pread(fd, h, sizeof(h), (off_t)ss) != (ssize_t)sizeof(h) || memcmp(h, "EFI PART", 8)) continue;
            uint64_t elba = le64(h + 72); uint32_t num = le32(h + 80), esz = le32(h + 84);
            for (uint32_t i = 0; i < num && i < 256; i++) {
                uint8_t e[256];
                if (esz > sizeof(e) || pread(fd, e, esz, (off_t)(elba * ss + (uint64_t)i * esz)) != (ssize_t)esz) break;
                uint64_t first = le64(e + 32);
                if (first && has_boot_m4(first * ss, 0)) { part_off = first * ss; break; }
            }
        }
    }
    off_t size = lseek(fd, 0, SEEK_END);
    struct l1_dev dev = {dev_read, NULL, 0, 0, dev_write};
    uint8_t *meta = aligned(4 * 4096);
    struct l1_load L = {0};
    L.dev = &dev; L.meta = meta; L.step = step;
    L.image_dst = aligned(kw); L.image_window = kw;
    L.initrd_dst = aligned(iw); L.initrd_window = iw;
    L.dtb_dst = aligned(dw); L.dtb_window = dw;
    L.image_floor = 64;
    uint64_t blocks = ((uint64_t)size - part_off) / 4096;
    for (unsigned boot = 0; boot < boots; boot++) {
        struct l1_ext4 fs;
        struct l1_result R;
        memset(&R, 0, sizeof(R));
        int rc = l1_ext4_mount(&fs, &dev, L1_P4_FIRST, blocks, meta);
        if (rc) { R.status = L1_EXT4; R.detail = fs.detail; }
        else rc = l1_run_fs(&L, &fs, &R);
        if (R.status == L1_OK && R.kind == L1_KIND_LINUX &&
            (R.dtb_bytes > L.dtb_window || fdt_check_full(L.dtb_dst, (size_t)R.dtb_bytes) ||
             fdt_totalsize(L.dtb_dst) != R.dtb_bytes)) R.status = L1_BAD_DTB;
        int ok = R.status == L1_OK;
        printf("source=%s entry=%s reason=%s detail=%s%s%s kver=%s cmdline=%s kind=%s oneshot=%u writes=%u\n",
               ok ? "entry" : "rescue", ok ? R.entry : "rescue", l1_reason(R.status, R.detail), l1_status_name(R.status),
               R.file[0] && !ok ? ":" : "", R.file[0] && !ok ? R.file : "", ok ? R.kver : "", ok ? R.cmdline : "",
               R.kind == L1_KIND_M1N1 ? "m1n1" : "linux", R.oneshot, writes);
    }
    return 0;
}

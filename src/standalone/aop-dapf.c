/* SPDX-License-Identifier: MIT */
/* rev 14: the AOP DART's DAPF from the ADT; see aop-dapf.h. */
#include "aop-dapf.h"
#include "adt.h"
#include "string.h"
#include <stdio.h>
#include "libfdt/libfdt.h"

#ifndef AOP_DAPF_TEST
#define read32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define read64(a) (*(volatile uint64_t *)(uintptr_t)(a))
#define write32(a, v) (*(volatile uint32_t *)(uintptr_t)(a) = (uint32_t)(v))
#define write64(a, v) (*(volatile uint64_t *)(uintptr_t)(a) = (uint64_t)(v))
#define SYNC() __asm__ volatile("dsb sy" ::: "memory")
#endif

struct slice {
    uint64_t start, end;
    uint32_t r4, r20, r0;
};

static struct slice want[M4_AOP_DAPF_MAX], had[M4_AOP_DAPF_MAX];
static unsigned count, bad_slice;
static uint64_t base;
static int state = M4_AOP_DAPF_NOT_RUN;

int m4_aop_dapf_collect(void)
{
    int path[8] = {0};
    int node = adt_path_offset_trace(adt, "/arm-io/dart-aop", path);
    uint32_t n = 0;
    uint64_t b = 0, size = 0;
    if (node < 0)
        return state = M4_AOP_DAPF_NO_ADT;
    const uint8_t *inst = adt_getprop(adt, node, "instance", &n);
    if (!inst || n < 32 || memcmp(inst + 16, "FPAD", 4)) /* record 1 is the DAPF block */
        return state = M4_AOP_DAPF_NO_DAPF;
    if (adt_get_reg(adt, path, "reg", 1, &b, &size) || b != M4_AOP_DAPF_BASE || size < 0x4000)
        return state = M4_AOP_DAPF_BAD_BASE;
    const uint8_t *d = adt_getprop(adt, node, "dapf-instance-0", &n);
    if (!d || !n || n % 52 || n / 52 > M4_AOP_DAPF_MAX)
        return state = M4_AOP_DAPF_BAD_TABLE;
    count = n / 52;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *r = d + 52 * i;
        struct slice *s = &want[i];
        memcpy(&s->start, r, 8);
        memcpy(&s->end, r + 8, 8);
        memcpy(&s->r20, r + 16, 4);
        memcpy(&s->r4, r + 24, 4);
        s->r0 = ((uint32_t)r[49] << 4) | r[50];
        if (s->end < s->start)
            return state = M4_AOP_DAPF_BAD_TABLE;
    }
    base = b;
    return state = M4_AOP_DAPF_COLLECTED;
}

static void put(uint64_t a, const struct slice *s)
{
    write32(a + 0x04, s->r4);
    write64(a + 0x08, s->start);
    write64(a + 0x10, s->end);
    write32(a + 0x00, s->r0);
    write32(a + 0x20, s->r20);
}

int m4_aop_dapf_program(void)
{
    if (state != M4_AOP_DAPF_COLLECTED)
        return state;
    if (read32(M4_AOP_ASC_BASE + 0x44) & 0x10)
        return state = M4_AOP_DAPF_AOP_RUNNING;
    for (unsigned i = 0; i < count; i++) {
        uint64_t a = base + 0x40 * i;
        had[i] = (struct slice){read64(a + 0x08), read64(a + 0x10), read32(a + 0x04), read32(a + 0x20), read32(a + 0x00)};
    }
    for (unsigned i = 0; i < count; i++)
        put(base + 0x40 * i, &want[i]);
    SYNC();
    for (unsigned i = 0; i < count; i++) {
        uint64_t a = base + 0x40 * i;
        if (read32(a + 0x04) != want[i].r4 || read64(a + 0x08) != want[i].start ||
            (read64(a + 0x10) & ~3ULL) != (want[i].end & ~3ULL) || (read32(a + 0x00) & 0xff) != want[i].r0 ||
            read32(a + 0x20) != want[i].r20) {
            bad_slice = i;
            for (unsigned k = 0; k < count; k++)
                put(base + 0x40 * k, &had[k]);
            SYNC();
            return state = M4_AOP_DAPF_MISMATCH;
        }
    }
    return state = M4_AOP_DAPF_PROGRAMMED;
}

int m4_aop_dapf_state(void)
{
    return state;
}

int m4_aop_dapf_publish(void *dt, size_t capacity)
{
    static const char reasons[] = "not-run\0collected-not-programmed\0programmed\0no-adt-node\0no-dapf-record\0"
                                  "unexpected-base\0bad-table\0aop-running\0read-back-mismatch\0";
    if (fdt_open_into(dt, dt, (int)capacity))
        return -1;
    int c = fdt_path_offset(dt, "/chosen"), ret = -1;
    if (c >= 0) {
        if (state == M4_AOP_DAPF_PROGRAMMED && fdt_path_offset(dt, M4_AOP_DAPF_FDT_NODE) >= 0) {
            ret = fdt_setprop_string(dt, c, "m4,dapf-programmed", M4_AOP_DAPF_FDT_NODE);
        } else {
            const char *r = reasons;
            char text[48];
            for (int i = 0; i < state && *r; i++)
                r += strlen(r) + 1;
            if (state == M4_AOP_DAPF_PROGRAMMED)
                r = "kernel-dt-has-no-node";
            if (state == M4_AOP_DAPF_MISMATCH)
                snprintf(text, sizeof(text), "%s slice %u", r, bad_slice);
            else
                snprintf(text, sizeof(text), "%s", r);
            ret = fdt_setprop_string(dt, c, "m4,dapf-aop-refused", text);
        }
    }
    fdt_pack(dt);
    return ret;
}

#ifdef AOP_DAPF_TEST
void aop_dapf_test_reset(void) { state = M4_AOP_DAPF_NOT_RUN; count = 0; bad_slice = 0; base = 0; }
#endif

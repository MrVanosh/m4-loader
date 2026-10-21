/* Host tests for loader rev 14: aop-dapf.c (fake MMIO, real ADT), the Bluetooth admission record and the timing record.
 * rev14-test ADT.bin FINAL.dtb */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "adt.h"
#include "libfdt/libfdt.h"

/* fake MMIO for aop-dapf.c (compiled with -DAOP_DAPF_TEST and these hooks) */
#define SLOTS 32
static uint64_t dapf[SLOTS][9];          /* per slice: word index = offset / 8 (u32 at +0x00/+0x04/+0x20 stored by offset) */
static uint32_t asc44;
static int stuck_slice = -1, writes;
static uint32_t *u32at(uint64_t a) {
    uint64_t off = a - 0x510f10000ULL; unsigned s = off / 0x40, o = off % 0x40;
    if (a == 0x510600044ULL) return &asc44;
    if (a < 0x510f10000ULL || s >= SLOTS) { printf("FAIL: access outside the DAPF block %#llx\n", (unsigned long long)a); exit(1); }
    return (uint32_t *)((uint8_t *)dapf[s] + o);
}
uint32_t t_read32(uint64_t a) { return *u32at(a); }
uint64_t t_read64(uint64_t a) { uint64_t v; memcpy(&v, u32at(a), 8); return v; }
void t_write32(uint64_t a, uint32_t v) { writes++; *u32at(a) = v; }
void t_write64(uint64_t a, uint64_t v) { writes++; uint64_t off = a - 0x510f10000ULL; if ((int)(off / 0x40) == stuck_slice && off % 0x40 == 8) return; memcpy(u32at(a), &v, 8); }

#include "aop-dapf.h"
int m4_direct_bluetooth_publish(void *dt, size_t capacity);
void adt_double_init(void *p, uint32_t n);
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static void *slurp(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); } fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); void *b = calloc(1, *n + 256 * 1024); if (fread(b, 1, *n, f) != *n) exit(2); fclose(f); return b; }
void aop_dapf_test_reset(void);

int main(int argc, char **argv)
{
    size_t an, tn; void *adtbuf = slurp(argv[1], &an), *tree = slurp(argv[2], &tn);
    adt_double_init(adtbuf, (uint32_t)an);
    /* 1. collect from the real ADT */
    CHECK(m4_aop_dapf_collect() == M4_AOP_DAPF_COLLECTED, "collect state %d", m4_aop_dapf_state());
    /* 2. AOP running: refused, no writes */
    asc44 = 0x10; writes = 0;
    CHECK(m4_aop_dapf_program() == M4_AOP_DAPF_AOP_RUNNING && writes == 0, "running: state %d writes %d", m4_aop_dapf_state(), writes);
    /* 3. normal program */
    aop_dapf_test_reset(); CHECK(m4_aop_dapf_collect() == M4_AOP_DAPF_COLLECTED, "recollect");
    asc44 = 0; memset(dapf, 0, sizeof(dapf)); writes = 0;
    CHECK(m4_aop_dapf_program() == M4_AOP_DAPF_PROGRAMMED, "program state %d", m4_aop_dapf_state());
    CHECK(writes == 12 * 5, "writes %d", writes);
    uint64_t s0, e0, s11, e11; memcpy(&s0, (uint8_t *)dapf[0] + 8, 8); memcpy(&e0, (uint8_t *)dapf[0] + 16, 8);
    memcpy(&s11, (uint8_t *)dapf[11] + 8, 8); memcpy(&e11, (uint8_t *)dapf[11] + 16, 8);
    CHECK(s0 == 0x5081e4020ULL && e0 == 0x5081e4043ULL && *(uint32_t *)dapf[0] == 0x31 && *(uint32_t *)((uint8_t *)dapf[0] + 0x20) == 1,
          "slice 0 %#llx..%#llx", (unsigned long long)s0, (unsigned long long)e0);
    CHECK(s11 == 0x50801c000ULL && e11 == 0x50801ffffULL, "slice 11");
    CHECK(*(uint32_t *)dapf[12] == 0 && t_read64(0x510f10000ULL + 12 * 0x40 + 8) == 0, "slice 12 untouched");
    /* 4. mismatch on slice 5: every slice restored to its previous (sentinel) values */
    aop_dapf_test_reset(); m4_aop_dapf_collect();
    for (int s = 0; s < SLOTS; s++) for (int w = 0; w < 9; w++) dapf[s][w] = 0x1111111100000000ULL | (s << 8) | w;
    uint64_t before[SLOTS][9]; memcpy(before, dapf, sizeof(dapf)); stuck_slice = 5;
    CHECK(m4_aop_dapf_program() == M4_AOP_DAPF_MISMATCH, "mismatch state %d", m4_aop_dapf_state());
    CHECK(!memcmp(before, dapf, sizeof(dapf)), "mismatch did not restore every slice");
    stuck_slice = -1;
    /* 5. publish: refused reason (mismatch slice 5) */
    size_t cap = 128 * 1024; void *t = calloc(1, cap); memcpy(t, tree, tn);
    CHECK(!m4_aop_dapf_publish(t, cap), "publish refused rc");
    const char *v = fdt_getprop(t, fdt_path_offset(t, "/chosen"), "m4,dapf-aop-refused", NULL);
    CHECK(v && !strcmp(v, "read-back-mismatch slice 5"), "refused text %s", v ? v : "(none)");
    CHECK(!fdt_getprop(t, fdt_path_offset(t, "/chosen"), "m4,dapf-programmed", NULL), "marker on failure");
    /* 6. publish: programmed marker */
    aop_dapf_test_reset(); m4_aop_dapf_collect(); memset(dapf, 0, sizeof(dapf)); m4_aop_dapf_program();
    memset(t, 0, cap); memcpy(t, tree, tn);
    CHECK(!m4_aop_dapf_publish(t, cap), "publish rc");
    v = fdt_getprop(t, fdt_path_offset(t, "/chosen"), "m4,dapf-programmed", NULL);
    CHECK(v && !strcmp(v, "/iommu@510f00000"), "marker %s", v ? v : "(none)");
    /* 7. bt record (not run in this test binary) */
    CHECK(!m4_direct_bluetooth_publish(t, cap), "bt publish rc");
    v = fdt_getprop(t, fdt_path_offset(t, "/chosen"), "m4,bt-admission", NULL);
    CHECK(v && !strcmp(v, "not-run"), "bt text %s", v ? v : "(none)");
    CHECK(!fdt_check_full(t, fdt_totalsize(t)), "tree check");
    printf("final tree %zu -> %u B\n%s (%d failures)\n", tn, fdt_totalsize(t), fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}

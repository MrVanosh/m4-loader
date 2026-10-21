/* Host test of rev 13's m4_pmp_publish() on this Mac's real ADT and a real final tree.
 * pmp-test ADT.bin FINAL-WITH-PMP.dtb FINAL-WITHOUT-PMP.dtb */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "adt.h"
#include "libfdt/libfdt.h"
#include "pmp-publish.h"

void adt_double_init(void *p, uint32_t n);
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void *slurp(const char *p, size_t *n, size_t pad)
{
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = calloc(1, *n + pad); if (fread(b, 1, *n, f) != *n) exit(2); fclose(f); return b;
}

static const char *SKIP[] = {"compatible","name","AAPL,phandle","region-base","region-size","segment-names",
    "segment-ranges","uuid","pre-loaded","running","no-shutdown","user-power-managed","firmware-name",
    "coredump-enable","KDebugCoreID","ptd-driver-version","sram-index","dram_stats","dvfs-domain-afc-bin",
    "dram-channel-disable","dram-capacity","pmc-msg-disabled","soc-chip-variant",NULL};
static int skip(const char *n) { for (int i = 0; SKIP[i]; i++) if (!strcmp(n, SKIP[i])) return 1; return 0; }

/* every property of every node of a must be in b with the same bytes */
static int subset(const void *a, const void *b, int *extra_ok_node)
{
    int bad = 0;
    for (int na = 0; na >= 0; na = fdt_next_node(a, na, NULL)) {
        char path[512]; fdt_get_path(a, na, path, sizeof(path));
        int nb = fdt_path_offset(b, path);
        if (nb < 0) { printf("  missing node %s\n", path); bad++; continue; }
        int pa;
        fdt_for_each_property_offset(pa, a, na) {
            const char *name; int la; const void *va = fdt_getprop_by_offset(a, pa, &name, &la);
            int lb; const void *vb = fdt_getprop(b, nb, name, &lb);
            if (!vb || la != lb || memcmp(va, vb, la)) { printf("  changed %s:%s\n", path, name); bad++; }
        }
    }
    (void)extra_ok_node;
    return bad;
}

static int count_props(const void *t, int node)
{
    int n = 0, p; fdt_for_each_property_offset(p, t, node) n++; return n;
}

int main(int argc, char **argv)
{
    size_t an, wn, on;
    void *adtbuf = slurp(argv[1], &an, 0);
    adt_double_init(adtbuf, (uint32_t)an);
    void *with = slurp(argv[2], &wn, 0), *without = slurp(argv[3], &on, 0);
    const size_t CAPS[] = {128 * 1024, 256 * 1024};

    int chosen = adt_path_offset(adt, "/chosen"), nub = adt_path_offset(adt, M4_PMP_ADT_NUB);
    CHECK(chosen >= 0 && nub >= 0, "ADT /chosen or nub missing");
    u32 board = 0, vendor = 0, l; memcpy(&board, adt_getprop(adt, chosen, "board-id", &l), 4);
    memcpy(&vendor, adt_getprop(adt, chosen, "dram-vendor-id", &l), 4);
    int expect_tunables = 0; size_t value_bytes = 0;
    int props = adt_get_property_count(adt, nub), off = adt_first_property_offset(adt, nub);
    for (int i = 0; i < props; i++, off = adt_next_property_offset(adt, off)) {
        const char *n; u32 len; adt_getprop_by_offset(adt, off, &n, &len);
        if (!skip(n)) { expect_tunables++; value_bytes += len; }
    }
    printf("ADT iop-pmp-nub: %d properties, %d tunables to copy, %zu value bytes\n", props, expect_tunables, value_bytes);

    for (int c = 0; c < 2; c++) {
        size_t cap = CAPS[c];
        /* S1: node present, room */
        void *t = calloc(1, cap); memcpy(t, with, wn);
        int rc = m4_pmp_publish(t, cap, cap);
        int node = fdt_node_offset_by_compatible(t, -1, M4_PMP_COMPATIBLE);
        printf("[cap %zu KiB] S1 rc=%d, tree %zu -> %u B (+%u)\n", cap / 1024, rc, wn, fdt_totalsize(t), fdt_totalsize(t) - (unsigned)wn);
        CHECK(rc == expect_tunables + 3, "S1 rc %d != %d", rc, expect_tunables + 3);
        CHECK(!fdt_check_full(t, fdt_totalsize(t)), "S1 tree check");
        CHECK(subset(with, t, NULL) == 0, "S1 an original property changed");
        int orig_node = fdt_node_offset_by_compatible(with, -1, M4_PMP_COMPATIBLE);
        CHECK(count_props(t, node) == count_props(with, orig_node) + rc, "S1 property count");
        const fdt32_t *v = fdt_getprop(t, node, "apple,board-id", NULL);
        CHECK(v && fdt32_to_cpu(*v) == board, "S1 board-id");
        v = fdt_getprop(t, node, "apple,dram-vendor-id", NULL);
        CHECK(v && fdt32_to_cpu(*v) == vendor, "S1 dram-vendor-id");
        u32 dchd; memcpy(&dchd, adt_getprop(adt, nub, "dram-channel-disable", &l), 4);
        v = fdt_getprop(t, node, "apple,dram-channel-disable", NULL);
        CHECK(v && fdt32_to_cpu(*v) == dchd, "S1 dram-channel-disable");
        CHECK(!fdt_getprop(t, node, "apple,dram-capacity", NULL) && !fdt_getprop(t, node, "apple,pmc-msg-disabled", NULL) &&
              !fdt_getprop(t, node, "apple,soc-chip-variant", NULL), "S1 absent boot args were added");
        const char *st = fdt_getprop(t, node, "status", NULL);
        CHECK(st && !strcmp(st, "disabled"), "S1 status changed");
        int copied = 0; off = adt_first_property_offset(adt, nub);
        for (int i = 0; i < props; i++, off = adt_next_property_offset(adt, off)) {
            const char *n; u32 len; const void *val = adt_getprop_by_offset(adt, off, &n, &len);
            char key[64]; snprintf(key, sizeof(key), "apple,tunable-%s", n);
            int fl; const void *fv = fdt_getprop(t, node, key, &fl);
            if (skip(n)) CHECK(!fv, "S1 skipped %s was copied", n);
            else { CHECK(fv && fl == (int)len && !memcmp(fv, val, len), "S1 %s differs", key); copied += fv != NULL; }
        }
        CHECK(copied == expect_tunables, "S1 copied %d", copied);
        if (c == 0) { FILE *f = fopen("final-rev13-128k.dtb", "wb"); fwrite(t, 1, fdt_totalsize(t), f); fclose(f); }
        unsigned grown = fdt_totalsize(t);

        /* S2: no PMP node -> nothing, byte-identical */
        void *u = calloc(1, cap); memcpy(u, without, on);
        rc = m4_pmp_publish(u, cap, cap);
        CHECK(rc == 0 && fdt_totalsize(u) == on && !memcmp(u, without, on), "S2 rc=%d size %u", rc, fdt_totalsize(u));

        /* S3: room 1 byte short of what S1 used + reserve -> nothing, byte-identical */
        memset(t, 0, cap); memcpy(t, with, wn);
        rc = m4_pmp_publish(t, cap, grown + M4_PMP_RESERVE - 1);
        CHECK(rc == -5 && fdt_totalsize(t) == wn && !memcmp(t, with, wn), "S3 rc=%d", rc);
        /* S3b: exactly enough room -> added */
        memset(t, 0, cap); memcpy(t, with, wn);
        rc = m4_pmp_publish(t, cap, grown + M4_PMP_RESERVE);
        CHECK(rc == expect_tunables + 3, "S3b rc=%d", rc);
        /* S4: capacity too small -> nothing */
        size_t small = grown + M4_PMP_RESERVE - 1;
        void *s = calloc(1, small); memcpy(s, with, wn);
        rc = m4_pmp_publish(s, small, cap);
        CHECK(rc == -5 && fdt_totalsize(s) == wn && !memcmp(s, with, wn), "S4 rc=%d", rc);
        printf("[cap %zu KiB] S2-S4 done\n", cap / 1024);
        free(t); free(u); free(s);
    }

    /* S5: no iop-pmp-nub in the ADT (its name patched) -> -2, unchanged */
    void *a2 = malloc(an); memcpy(a2, adtbuf, an);
    adt_double_init(a2, (uint32_t)an);
    u32 nl; char *nm = (char *)adt_getprop(a2, adt_path_offset(a2, M4_PMP_ADT_NUB), "name", &nl);
    nm[strlen(nm) - 1] = 'X';
    CHECK(adt_path_offset(a2, M4_PMP_ADT_NUB) < 0, "S5 setup: nub still found");
    void *t = calloc(1, 256 * 1024); memcpy(t, with, wn);
    int rc = m4_pmp_publish(t, 256 * 1024, 256 * 1024);
    CHECK(rc == -2 && fdt_totalsize(t) == wn && !memcmp(t, with, wn), "S5 rc=%d", rc);
    /* S6: /chosen board-id renamed -> -3, unchanged */
    memcpy(a2, adtbuf, an);
    const struct adt_property *bp = adt_get_property(a2, adt_path_offset(a2, "/chosen"), "board-id");
    ((char *)bp->name)[7] = 'X';
    memset(t, 0, 256 * 1024); memcpy(t, with, wn);
    rc = m4_pmp_publish(t, 256 * 1024, 256 * 1024);
    CHECK(rc == -3 && fdt_totalsize(t) == wn && !memcmp(t, with, wn), "S6 rc=%d", rc);
    printf("S5-S6 done\n%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}

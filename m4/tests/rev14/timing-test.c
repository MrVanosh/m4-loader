#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt/libfdt.h"
#include "boot-timing.h"
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); static char t[131072], w[131072], orig[131072]; size_t n = fread(t, 1, sizeof(t), f); fclose(f);
    memcpy(orig, t, n);
    for (unsigned i = 0; i < M4_BOOT_TIMING_COUNT; i++) m4_boot_ticks[i] = 1000000000ULL + i * 1000;
    int rc = m4_boot_timing_publish(t, w, sizeof(t), 1000000000ULL, 0x80010100ULL);
    int len = 0; const void *p = fdt_getprop(t, fdt_path_offset(t, "/chosen"), M4_BOOT_TIMING_PROPERTY, &len);
    printf("publish rc=%d len=%d tree %zu -> %u\n", rc, len, n, fdt_totalsize(t));
    memcpy(t, orig, n); m4_boot_ticks[9] = 0;
    int rc2 = m4_boot_timing_publish(t, w, sizeof(t), 1000000000ULL, 0x80010100ULL);
    printf("missing mark rc=%d unchanged=%d\n", rc2, !memcmp(t, orig, n));
    int ok = rc == 0 && len == 160 && rc2 == -2 && !memcmp(t, orig, n);
    printf("%s\n", ok ? "PASS" : "FAIL"); return !ok;
}

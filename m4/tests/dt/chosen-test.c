/* rev 12: the late /chosen m4,board-dtb-max edit from loader.c, on a real final DT (host, real libfdt) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "libfdt.h"
#define M4_BOARD_DTB_MAX (96u * 1024)
static uint8_t late[0x40000];
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); static uint8_t in[0x40000]; size_t n = fread(in, 1, sizeof(in), f); fclose(f);
    if (fdt_open_into(in, late, sizeof(late))) return 2;
    void *dt = late; size_t capacity = sizeof(late);
    /* the loader.c lines, verbatim */
        if (!fdt_open_into(dt, dt, (int)capacity)) {
            int c = fdt_path_offset(dt, "/chosen");
            if (c >= 0) fdt_setprop_u32(dt, c, "m4,board-dtb-max", M4_BOARD_DTB_MAX);
            fdt_pack(dt);
        }
    int c = fdt_path_offset(dt, "/chosen"), l;
    const fdt32_t *v = fdt_getprop(dt, c, "m4,board-dtb-max", &l);
    printf("in=%zu out=%u check=%d prop_len=%d value=%u\n", n, fdt_totalsize(dt), fdt_check_full(dt, fdt_totalsize(dt)), l, v ? fdt32_to_cpu(*v) : 0);
    FILE *o = fopen(argv[2], "wb"); fwrite(dt, 1, fdt_totalsize(dt), o); fclose(o);
    return !(v && l == 4 && fdt32_to_cpu(*v) == M4_BOARD_DTB_MAX && !fdt_check_full(dt, fdt_totalsize(dt)));
}

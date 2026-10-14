/* SPDX-License-Identifier: MIT */
/* Optional diagnostic extension; the accepted 160-byte boot-timing ABI stays separate. */
#include "boot-profile.h"
#include "ram.h"
#include "libfdt.h"
#include "loader-speed.h" /* 256 KiB DT window bound */
#include <string.h>
uint64_t m4_boot_profile_ticks[M4_PROFILE_COUNT];
int64_t m4_boot_profile_display_rc, m4_boot_profile_media_rc;
_Static_assert(M4_BOOT_PROFILE_WORDS*8==296,"bounded profile ABI");
_Static_assert(M4_PROFILE_COUNT<64,"profile presence mask");
int m4_boot_profile_publish(void *tree,void *scratch,size_t capacity,
                            uint64_t frequency,uint64_t mpidr)
{
    if (!tree || !scratch || capacity<16384 || capacity>M4_FDT_WINDOW ||
        frequency!=UINT64_C(1000000000) || (mpidr&UINT64_C(0xff00ffffff))!=0x10100 ||
        fdt_check_full(tree,capacity)) return -1;
    uintptr_t a=(uintptr_t)tree,b=(uintptr_t)scratch; size_t bytes=fdt_totalsize(tree);
    if (a>UINTPTR_MAX-bytes || b>UINTPTR_MAX-capacity ||
        (a<b+capacity && b<a+bytes)) return -1;
    uint64_t mask=0,previous=0;
    if (!m4_boot_profile_ticks[0] || !m4_boot_profile_ticks[M4_PROFILE_COUNT-1]) return -2;
    for (unsigned i=0;i<M4_PROFILE_COUNT;i++) {
        uint64_t tick=m4_boot_profile_ticks[i];
        if (!tick) continue;
        if (tick<previous || tick-m4_boot_profile_ticks[0]>frequency*86400) return -2;
        mask|=UINT64_C(1)<<i; previous=tick;
    }
    uint64_t wire[M4_BOOT_PROFILE_WORDS]={cpu_to_fdt64(M4_BOOT_PROFILE_MAGIC),
        cpu_to_fdt64(1),cpu_to_fdt64(sizeof(wire)),cpu_to_fdt64(1),
        cpu_to_fdt64(frequency),cpu_to_fdt64(mpidr&UINT64_C(0xff00ffffff)),
        cpu_to_fdt64(M4_PROFILE_COUNT),cpu_to_fdt64(mask),
        cpu_to_fdt64((uint64_t)m4_boot_profile_display_rc),cpu_to_fdt64((uint64_t)m4_boot_profile_media_rc)};
    for (unsigned i=0;i<M4_PROFILE_COUNT;i++) wire[10+i]=cpu_to_fdt64(m4_boot_profile_ticks[i]);
    int ret=fdt_open_into(tree,scratch,capacity); if (ret) return ret;
    int chosen=fdt_path_offset(scratch,"/chosen");
    if (chosen<0 || fdt_getprop(scratch,chosen,M4_BOOT_PROFILE_PROPERTY,NULL)) return -3;
    if ((ret=fdt_setprop(scratch,chosen,M4_BOOT_PROFILE_PROPERTY,wire,sizeof(wire))) ||
        (ret=fdt_pack(scratch)) || fdt_check_full(scratch,capacity)) return ret?ret:-4;
    m4_ram_copy(tree,scratch,fdt_totalsize(scratch));
    return 0;
}

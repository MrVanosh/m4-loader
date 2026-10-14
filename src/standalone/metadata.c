/* SPDX-License-Identifier: MIT
 * Live ADT validation and interval collection. Uses m1n1's existing parser.
 * This stage performs no MMIO and writes only its output structs/private FDT.
 */
#include "adt.h"
#include "xnuboot.h"
#include "j616s_boot_prefix.h"
#include "j616s_diag.h"
#include "memory.h"
#include "fdt.h"
#include "string.h"
#include "gates.h"
unsigned m4_gate_failed;
static const void *property(const char *path,const char *name,u32 *length) {
    int node=adt_path_offset(adt,path);if(node<0)return NULL;
    const void *p=adt_getprop(adt,node,name,length);
    if(!p)return NULL;
    if(!*length) {
        if((u64)p<(u64)adt||(u64)p>(u64)adt+cur_boot_args.devtree_size)return NULL;
    } else if(!j616s_range_contains((u64)adt,cur_boot_args.devtree_size,(u64)p,*length))return NULL;
    return p;
}
static int exact(const char *path,const char *name,void *out,u32 bytes) {
    u32 len;const void *p=property(path,name,&len);
    if(!p||len!=bytes)return -1;
    memcpy(out,p,bytes);return 0;
}
int m4_metadata(struct m4_memory *m,struct m4_handoff *h,u64 lower,u64 upper) {
    if(cur_boot_args.revision!=3||cur_boot_args.version!=2||adt_check_header(adt))return -1;
    u32 spmi_gen;
    if(exact("/arm-io/nub-spmi0","gen",&spmi_gen,4)||spmi_gen!=3)return -9;
    for(unsigned i=0;i<sizeof(gates)/sizeof(gates[0]);i++) {
        u32 n;const void *p=property(gates[i].path,gates[i].key,&n);
        if(!p||n!=gates[i].size||memcmp(p,gates[i].bytes,n)){m4_gate_failed=i+1;return -2;}
    }
    memset(m,0,sizeof(*m));
    m->usable=(struct m4_range){cur_boot_args.phys_base,cur_boot_args.mem_size};
    m->top=cur_boot_args.top_of_kernel_data;m->ctrr_low=lower;m->ctrr_high=upper;
    if(exact("/chosen","dram-base",&m->dram.base,8)||exact("/chosen","dram-size",&m->dram.size,8))return -3;
    struct m4_range container;
    if(exact("/chosen/carveout-memory-map","region-id-24",&container,16)||memcmp(&container,&m->usable,16))return -3;
    int node=adt_path_offset(adt,"/chosen/carveout-memory-map");
    ADT_FOREACH_PROPERTY(adt,node,p) {
        if(strncmp(p->name,"region-id-",10)||!strcmp(p->name,"region-id-24"))continue;
        struct m4_range r;
        if(p->size!=16)return -4;
        memcpy(&r,p->value,16);
        if((r.base|r.size)&16383||m4_reserve(m,r.base,r.size))return -4;
    }
    node=adt_path_offset(adt,"/chosen/memory-map");if(node<0)return -5;
    ADT_FOREACH_PROPERTY(adt,node,p) {
        if(p->size!=16)continue;
        struct m4_range r;memcpy(&r,p->value,16);
        if(!r.size||(r.base==UINT64_MAX&&r.size==UINT64_MAX))continue;
        if(m4_reserve(m,r.base,r.size))return -5;
    }
    u32 len;const unsigned char *p=property("/defaults","pmap-io-ranges",&len);
    if(!p||len%24)return -6;
    for(unsigned i=0;i<len;i+=24){struct m4_range r;memcpy(&r,p+i,16);if(m4_reserve(m,r.base,r.size))return -6;}
    p=property("/arm-io","ranges",&len);if(!p||len%24)return -6;
    for(unsigned i=0;i<len;i+=24){struct m4_range r;memcpy(&r,p+i+8,16);if(m4_reserve(m,r.base,r.size))return -6;}
    if(m4_memory_finish(m))return -7;
    struct m4_range vram,carve;
    if(exact("/vram","reg",&vram,16)||exact("/chosen/carveout-memory-map","region-id-14",&carve,16)||memcmp(&vram,&carve,16))return -8;
    if(vram.base!=cur_boot_args.video.base||!cur_boot_args.video.display)return -8;
    h->framebuffer=vram.base;h->framebuffer_size=vram.size;
    h->width=cur_boot_args.video.width;h->height=cur_boot_args.video.height;
    h->stride=cur_boot_args.video.stride;h->depth=cur_boot_args.video.depth&255;
    return 0;
}
int m4_validate_tz(const u64 *w) {
    if(w[0]!=0x4d4343545a303031UL||w[1]!=192||w[3]!=1)return -1;
    for(unsigned i=1;i<16;i++)if(memcmp(w+8,w+8+i*12,96))return -1;
    struct m4_range a,b;
    if(exact("/chosen/carveout-memory-map","region-id-2",&a,16)||exact("/chosen/carveout-memory-map","region-id-4",&b,16))return -1;
    unsigned found=0;
    for(unsigned i=0;i<4;i++) {
        const u64 *r=w+8+i*3;if(r[2]>1)return -1;if(!r[2])continue;
        if(r[1]<r[0]||r[1]>=UINT64_MAX/4096)return -1;
        struct m4_range tz={r[0]<<12,((r[1]+1-r[0])<<12)};
        if(tz.base==a.base&&tz.size==a.size)found|=1;
        else if(tz.base==b.base&&tz.size==b.size)found|=2;
        else return -1;
    }
    return found==3?0:-1;
}

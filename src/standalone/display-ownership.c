/* Accepted R11 eleven-extent checker adapted only from cooperative layout
 * containers to the single direct loader/arena/current memory plan. */
/* SPDX-License-Identifier: MIT */
#include "direct-capture.h"
struct m4_display_ownership_result { unsigned checked, index; int error; };
#include "dart-capture.h"
#include "adt.h"
#include <string.h>

static int copy(int node, const char *key, void *out, uint32_t size)
{
    uint32_t n=0; const void *p=adt_getprop(adt,node,key,&n);
    uintptr_t a=(uintptr_t)p, b=(uintptr_t)adt;
    if(!p || n!=size || a<b || n>adt_get_size() || a-b>adt_get_size()-n)return -1;
    memcpy(out,p,n);return 0;
}
static int aligned(struct m4_range r)
{ return m4_range_valid(r) && !((r.base|r.size)&(M4_PAGE-1)); }
static int same(struct m4_range a,struct m4_range b)
{ return a.base==b.base && a.size==b.size; }
static int memory_valid(const struct m4_memory *m)
{
    if(!aligned(m->dram) || m->reserved_count>M4_MAX_RESERVED || m->free_count>M4_MAX_FREE)return 0;
    for(unsigned n=0;n<m->reserved_count;n++)if(!m4_range_valid(m->reserved[n]))return 0;
    for(unsigned n=0;n<m->free_count;n++)
        if(!aligned(m->free[n]) || !m4_contains(m->dram,m->free[n]))return 0;
    return 1;
}
static int reserved(const struct m4_memory *m,struct m4_range owner)
{
    /* metadata.c retains each complete ADT carveout. Require that exact owner,
     * not an unrelated containing range which might hide a missing carveout. */
    for(unsigned n=0;n<m->reserved_count;n++)if(same(m->reserved[n],owner))return 1;
    return 0;
}
static int protected(const struct m4_memory *memory, struct m4_range loader,
                     struct m4_range arena, struct m4_range r,struct m4_range owner)
{
    if(!aligned(r) || !aligned(owner) || !m4_contains(memory->dram,owner) ||
       !m4_contains(owner,r))return -5;
    if(!reserved(memory,owner))return -6;
    /* The entire ELF must be disjoint, including the arena's guest suffix.
     * This catches OS-log aliases inside the otherwise reusable region-id-45
     * parent allocation before ANY clear/copy, without excluding that whole
     * parent (which legitimately contains the ELF). */
    if(m4_overlaps(r,loader) || m4_overlaps(r,arena))return -7;
    for(unsigned n=0;n<memory->free_count;n++)if(m4_overlaps(r,memory->free[n]))return -7;
    return 0;
}
static int fail(struct m4_display_ownership_result *r,int error)
{ r->error=error;return error; }

int m4_direct_desktop_ownership(const struct m4_memory *memory, struct m4_range loader,
                               struct m4_range arena)
{
    struct m4_display_ownership_result output={0};
    struct m4_display_ownership_result *result=&output;
    if(!memory || !memory_valid(memory) || !aligned(loader) || !aligned(arena) ||
       !m4_contains(loader,arena))return -1;
    /* This existing entry point validates the entire ADT and exact board,
     * firmware, DART resources and mapper relationships. It reads ADT RAM only.
     * Deliberately never call m4_dart_collect or any DART initialization API. */
    struct m4_dart_capture metadata={0};
    if(m4_dart_prepare(&metadata,adt,adt_get_size()))return fail(result,-2);
    int chosen=adt_path_offset(adt,"/chosen");
    int carveouts=adt_path_offset(adt,"/chosen/carveout-memory-map");
    int nub=adt_path_offset(adt,"/arm-io/dcp/iop-dcp-nub");
    struct m4_range dram;
    if(chosen<0 || carveouts<0 || nub<0 || copy(chosen,"dram-base",&dram.base,8) ||
       copy(chosen,"dram-size",&dram.size,8) || !same(dram,memory->dram))return fail(result,-3);

    struct adt_segment_ranges segments[8];
    if(copy(nub,"segment-ranges",segments,sizeof(segments)))return fail(result,-4);
    static const char *owners[]={"region-id-49","region-id-50","region-id-45",
        "region-id-57","region-id-94","region-id-95","region-id-14","region-id-157"};
    static const uint32_t flags[]={1,0,10,2,2,2,2,2};
    int dcp=adt_path_offset(adt,"/arm-io/dart-dcp");
    struct m4_range window;
    if(dcp<0 || copy(dcp,"vm-base",&window.base,8) || copy(dcp,"vm-size",&window.size,8) ||
       !aligned(window) || window.size!=UINT64_C(0x1000000000) ||
       (window.base&(window.size-1)))return fail(result,-4);
    uint64_t total=0;
    for(unsigned n=0;n<8;n++) {
        result->index=n;
        const struct adt_segment_ranges *s=&segments[n];
        struct m4_range owner,r={s->phys,((uint64_t)s->size+M4_PAGE-1)&~(M4_PAGE-1)};
        uint64_t legacy=n==0?0:n==1?segments[0].size:n==2?(uint64_t)segments[0].size+segments[1].size:UINT64_MAX;
        total+=r.size;
        if(!s->size || r.size>UINT64_C(0x10000000) || total>UINT64_C(0x20000000) ||
           (s->remap&(M4_PAGE-1)) || !m4_contains(window,(struct m4_range){s->remap,r.size}) ||
           s->unk!=flags[n] || s->iova!=legacy || copy(carveouts,owners[n],&owner,16))return fail(result,-4);
        /* Current descriptors bind seven measured physical mapping extents to
         * their carveouts. OS-log (2) is only contained in its loader parent;
         * the successful r6 table capture showed its actual low36 translation
         * inside segment 6 instead. Never infer identity from remap or flags. */
        if(n!=2 && !same(r,owner))return fail(result,-5);
        int error=protected(memory,loader,arena,r,owner);
        if(error)return fail(result,error);
        result->checked++;
    }
    static const char *paths[]={"/arm-io/dart-dcp","/arm-io/dart-disp0","/arm-io/dart-disp0"};
    static const char *pt[]={"pt-region-23","pt-region-0","pt-region-16"};
    static const char *l2[]={"l2-tt-23","l2-tt-0","l2-tt-16"};
    static const char *pt_owner[]={"region-id-18","region-id-16","region-id-17"};
    struct m4_range pools[3];
    for(unsigned n=0;n<3;n++) {
        result->index=8+n;
        int node=adt_path_offset(adt,paths[n]);uint64_t bounds[2],root[2];uint32_t page;
        struct m4_range owner,w;
        if(node<0 || copy(node,pt[n],bounds,16) || copy(node,l2[n],root,16) ||
           copy(node,"page-size",&page,4) || copy(node,"vm-base",&w.base,8) ||
           copy(node,"vm-size",&w.size,8) || copy(carveouts,pt_owner[n],&owner,16) ||
           page!=M4_PAGE || bounds[1]<=bounds[0] || root[1]!=2 ||
           (root[0]&(M4_PAGE-1)) || !same(w,window))return fail(result,-8);
        pools[n]=(struct m4_range){bounds[0],bounds[1]-bounds[0]};
        if(pools[n].size>32*M4_PAGE || !same(pools[n],owner) ||
           !m4_contains(pools[n],(struct m4_range){root[0],M4_PAGE}))return fail(result,-8);
        int error=protected(memory,loader,arena,pools[n],owner);
        if(error)return fail(result,error);
        for(unsigned j=0;j<n;j++)if(m4_overlaps(pools[n],pools[j]))return fail(result,-8);
        for(unsigned j=0;j<8;j++) {
            struct m4_range segment={segments[j].phys,((uint64_t)segments[j].size+M4_PAGE-1)&~(M4_PAGE-1)};
            if(m4_overlaps(pools[n],segment))return fail(result,-8);
        }
        result->checked++;
    }
    return result->checked == 11 ? 0 : -9;
}

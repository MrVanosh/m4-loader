/* SPDX-License-Identifier: MIT */
/* Read-only, current-boot T8110 table evidence. No DART initialization. */
#include "dart-tables.h"
#include "adt.h"
#include <string.h>

/* Exact retained Linux io-pgtable-dart.c DART2 PA format, 42-bit output. */
#define PA_MASK UINT64_C(0x3ffffffc00)
static int range(struct m4_dt_range r)
{ return r.size && r.base <= UINT64_MAX - r.size; }
static int contains(struct m4_dt_range r, uint64_t a, uint64_t n)
{ return range(r) && n && a >= r.base && n <= r.size && a-r.base <= r.size-n; }
static int overlaps(struct m4_dt_range a,struct m4_dt_range b)
{ return a.base<b.base+b.size && b.base<a.base+a.size; }
static uint64_t pa(uint64_t pte) { return (pte & PA_MASK) << 4; }
static const void *property(int node, const char *key, uint32_t size)
{
    uint32_t n=0; const void *p=adt_getprop(adt,node,key,&n);
    uintptr_t a=(uintptr_t)p, b=(uintptr_t)adt;
    return p && n==size && a>=b && n<=adt_get_size() && a-b<=adt_get_size()-n ? p:0;
}
static int copy(int node, const char *key, void *out, uint32_t size)
{ const void *p=property(node,key,size); if(!p)return -1; memcpy(out,p,size); return 0; }
static int input_valid(const struct m4_dt_input *in)
{
    if(!in || in->excluded_count>6)return 0;
    for(unsigned i=0;i<in->excluded_count;i++)
        if(!range(in->excluded[i]) || !range(in->tables) || overlaps(in->tables,in->excluded[i]))return 0;
    return range(in->dram) && range(in->tables) &&
        !((in->dram.base|in->dram.size|in->tables.base|in->tables.size|in->root)&(M4_DT_PAGE-1)) &&
        in->tables.size <= sizeof(struct m4_dt_workspace) &&
        contains(in->dram,in->tables.base,in->tables.size) &&
        contains(in->tables,in->root,M4_DT_PAGE) &&
        range(in->window) && in->window.size==M4_DT_WINDOW &&
        !(in->window.base&(M4_DT_WINDOW-1)) &&
        ((in->params[0]>>24)&15)==14 && in->params[2]==0x2a2a0202 &&
        (in->params[3]&0x1ff)>in->sid && (in->params[3]&0x1ff)<=256 &&
        (in->protect&1) && in->tcr==1 && (in->ttbr&3)==1 &&
        !(in->ttbr&0xc0000000u) && in->root==((uint64_t)(in->ttbr>>2)<<14) &&
        ((in->sid==23 && in->controller==UINT64_C(0x402340000)) ||
         ((in->sid==0 || in->sid==16) && in->controller==UINT64_C(0x402300000)));
}
int m4_dt_prepare(const struct m4_dart_capture *c, struct m4_dt_input out[3],
                  struct m4_dt_segment segments[M4_DT_SEGMENTS])
{
    if(!c || !out || !segments || c->phase!=DART_COMPLETE || c->error || c->completed!=16 ||
       c->adt_address!=(uintptr_t)adt || c->adt_bytes!=adt_get_size())return -1;
    /* Reuse the already-reviewed complete ADT/resource/firmware preflight. */
    struct m4_dart_capture now={0};
    if(m4_dart_prepare(&now,adt,adt_get_size()))return -2;
    for(unsigned i=0;i<16;i++)if(!c->read[i].done || c->read[i].address!=now.read[i].address)return -3;
    int chosen=adt_path_offset(adt,"/chosen"), nub=adt_path_offset(adt,"/arm-io/dcp/iop-dcp-nub");
    struct m4_dt_range dram;
    if(chosen<0 || nub<0 || copy(chosen,"dram-base",&dram.base,8) ||
       copy(chosen,"dram-size",&dram.size,8) ||
       copy(nub,"segment-ranges",segments,sizeof(*segments)*M4_DT_SEGMENTS))return -4;
    static const char *paths[]={"/arm-io/dart-dcp","/arm-io/dart-disp0","/arm-io/dart-disp0"};
    static const char *pt[]={"pt-region-23","pt-region-0","pt-region-16"};
    static const char *l2[]={"l2-tt-23","l2-tt-0","l2-tt-16"};
    static const unsigned sid[]={23,0,16}, first[]={0,7,7}, control[]={5,12,14};
    for(unsigned i=0;i<3;i++) {
        int node=adt_path_offset(adt,paths[i]); uint64_t region[2], root[2]; uint32_t pages;
        struct m4_dt_input *d=&out[i]; *d=(struct m4_dt_input){.dram=dram,.sid=sid[i]};
        if(node<0 || copy(node,pt[i],region,16) || copy(node,l2[i],root,16) ||
           copy(node,"vm-base",&d->window.base,8) || copy(node,"vm-size",&d->window.size,8) ||
           copy(node,"page-size",&pages,4) || pages!=M4_DT_PAGE || region[1]<=region[0] || root[1]!=2)return -5;
        d->tables=(struct m4_dt_range){region[0],region[1]-region[0]}; d->root=root[0];
        d->controller=now.base[i!=0];
        for(unsigned j=0;j<4;j++)d->params[j]=c->read[first[i]+j].value;
        d->protect=c->read[first[i]+4].value; d->tcr=c->read[control[i]].value; d->ttbr=c->read[control[i]+1].value;
        if(!input_valid(d))return -6;
    }
    uint64_t total=0;
    for(unsigned i=0;i<M4_DT_SEGMENTS;i++) {
        const struct m4_dt_segment *s=&segments[i];
        uint64_t size=((uint64_t)s->size+M4_DT_PAGE-1)&~(uint64_t)(M4_DT_PAGE-1);
        total+=size;
        if(!s->size || size>UINT64_C(0x10000000) || total>UINT64_C(0x20000000) ||
           ((s->physical|s->remap)&(M4_DT_PAGE-1)) ||
           !contains(dram,s->physical,size) || !contains(out[0].window,s->remap,size))return -7;
        /* Overlap and flags are observations, not an identity-map assertion. */
    }
    return 0;
}
static int fail(struct m4_dt_result *r,int error) { r->error=error;r->phase=9;return error; }
static int controls(const struct m4_dt_input *in,struct m4_dt_result *r)
{
    const uint64_t offsets[]={0x200,0x1000+4*in->sid,0x1400+4*in->sid};
    const uint32_t values[]={in->protect,in->tcr,in->ttbr};
    for(unsigned i=0;i<3;i++) {
        r->active_address=in->controller+offsets[i];
        if(m4_dt_progress(r->phase==1?2:3,r->active_address))return fail(r,-3);
        __asm__ volatile("":::"memory");
        if(m4_dt_read32(r->active_address)!=values[i])return fail(r,-10);
    }
    return 0;
}
static int load(const struct m4_dt_input *in,struct m4_dt_result *r,uint64_t address,uint64_t *value)
{
    if((address&7) || !contains(in->dram,address,8) || !contains(in->tables,address,8))return fail(r,-11);
    for(unsigned i=0;i<in->excluded_count;i++)
        if(overlaps((struct m4_dt_range){address,8},in->excluded[i]))return fail(r,-11);
    r->active_address=address;
    __asm__ volatile("":::"memory");
    *value=m4_dt_read64(address);r->loads++;
    return 0;
}
static int append(struct m4_dt_result *r,uint64_t iova,uint64_t pte,const struct m4_dt_input *in)
{
    uint64_t physical=pa(pte),attributes=pte&~PA_MASK;
    if(pte & (UINT64_C(3)<<38))return fail(r,-12); /* PA > advertised 42 bits */
    r->leaf_entries++;
    if(!contains(in->dram,physical,M4_DT_PAGE))r->outside_dram++;
    struct m4_dt_span *last=r->spans?&r->map[r->spans-1]:0;
    if(last && last->iova+last->size==iova && last->physical+last->size==physical && last->attributes==attributes) {
        last->size+=M4_DT_PAGE; return 0;
    }
    if(r->spans==M4_DT_SPANS)return fail(r,-13);
    r->map[r->spans++]=(struct m4_dt_span){iova,physical,M4_DT_PAGE,attributes}; return 0;
}
int m4_dt_collect(const struct m4_dt_input *in,const struct m4_dt_segment *segments,
                  struct m4_dt_workspace *w,struct m4_dt_result *r)
{
    if(!r || r->phase)return -1;
    r->phase=1;
    if(!w || !segments || !input_valid(in))return fail(r,-2);
    r->begin=m4_dt_counter();
    if(controls(in,r))return r->error;
    /* Read the complete ADT-declared PT pool twice, with exact word comparison.
     * No PTE is followed until the complete second sweep agrees. This is a
     * two-sample observation, not an atomic snapshot of live firmware. */
    unsigned words=in->tables.size/8;
    for(unsigned pass=0;pass<2;pass++) {
        r->phase=2+pass;
        for(unsigned i=0;i<words;i++) {
            uint64_t address=in->tables.base+8*i,value;
            if(!(i%M4_DT_WORDS) && m4_dt_progress(pass,address))return fail(r,-3);
            if(load(in,r,address,&value))return r->error;
            if(!pass)w->words[i]=value;
            else if(value!=w->words[i])return fail(r,-4);
        }
    }
    if(controls(in,r))return r->error;
    r->stable=1; r->phase=4; r->checksum=UINT64_C(14695981039346656037);
    for(unsigned i=0;i<words;i++) {
        /* Noncryptographic correlation checksum; never used for acceptance. */
        r->checksum^=w->words[i];r->checksum*=UINT64_C(1099511628211);
    }
    const uint64_t *root=w->words+(in->root-in->tables.base)/8;
    for(unsigned i=0;i<M4_DT_WORDS;i++) {
        uint64_t pte=root[i];
        if(!(pte&1)) { if(pte)r->invalid_nonzero++;continue; }
        uint64_t table=pa(pte);
        if((pte&~(PA_MASK|1)) || table==in->root ||
           !contains(in->dram,table,M4_DT_PAGE) || !contains(in->tables,table,M4_DT_PAGE))return fail(r,-5);
        r->root_entries++;
        const uint64_t *leaf=w->words+(table-in->tables.base)/8;
        for(unsigned j=0;j<M4_DT_WORDS;j++) {
            if(!(leaf[j]&1)) { if(leaf[j])r->invalid_nonzero++;continue; }
            uint64_t iova=((uint64_t)i<<25)+((uint64_t)j<<14);
            if(append(r,iova,leaf[j],in))return r->error;
        }
    }
    uint64_t total=0;
    for(unsigned i=0;i<M4_DT_SEGMENTS;i++) {
        const struct m4_dt_segment *s=&segments[i];struct m4_dt_segment_result *x=&r->segment[i];
        uint64_t bytes=((uint64_t)s->size+M4_DT_PAGE-1)&~(uint64_t)(M4_DT_PAGE-1);
        total+=bytes;
        if(!s->size || bytes>UINT64_C(0x10000000) || total>UINT64_C(0x20000000) ||
           ((s->physical|s->remap)&(M4_DT_PAGE-1)) ||
           !contains(in->dram,s->physical,bytes) || !contains(in->window,s->remap,bytes))return fail(r,-6);
        /* Subtract only the explicit current-ADT window. We report this
         * projection, not proof the device's high alias bits are honored. */
        uint64_t low=s->remap-in->window.base;
        x->pages=bytes/M4_DT_PAGE;
        for(unsigned page=0;page<x->pages;page++) {
            uint64_t offset=(uint64_t)page*M4_DT_PAGE,actual=0; unsigned found=0;
            for(unsigned m=0;m<r->spans;m++) {
                const struct m4_dt_span *v=&r->map[m];
                if(low+offset>=v->iova && low+offset-v->iova<v->size) {
                    actual=v->physical+low+offset-v->iova;found=1;break;
                }
            }
            if(!found)x->absent++;
            else { if(!page)x->first_pa=actual; if(actual==s->physical+offset)x->same++;else x->different++; }
        }
    }
    r->end=m4_dt_counter();if(r->end<r->begin)return fail(r,-7);
    r->phase=5; return 0;
}

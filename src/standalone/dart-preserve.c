/* SPDX-License-Identifier: MIT */
#include "dart-preserve.h"
#include "libfdt.h"
#include <string.h>
#include <stdio.h>

#define PA_MASK UINT64_C(0x3ffffffc00)
#define FULL_PAGE UINT64_C(0x000fff0000000000)
static int inside(struct m4_dt_range r, uint64_t a, uint64_t n)
{ return r.size && r.base <= UINT64_MAX-r.size && n && a>=r.base && n<=r.size && a-r.base<=r.size-n; }
static int covered(const struct m4_dt_range *r,size_t nr,uint64_t a,uint64_t n)
{
    /* Covering one exact owner is intentional; no union across a missing byte. */
    for(size_t i=0;i<nr;i++)if(inside(r[i],a,n))return 1;
    return 0;
}
static int overlaps(struct m4_dt_range r,uint64_t a,uint64_t n)
{ return r.base<a+n && a<r.base+r.size; }
static int snapshot(const struct m4_dt_result *r)
{
    if(!r || r->phase!=5 || r->error || r->stable!=1 || r->outside_dram ||
       r->invalid_nonzero || !r->spans || r->spans>M4_DT_SPANS)return 0;
    uint64_t pages=0,end=0;
    for(unsigned i=0;i<r->spans;i++) {
        const struct m4_dt_span *s=&r->map[i];
        if(!inside((struct m4_dt_range){0,M4_DT_WINDOW},s->iova,s->size) ||
           !inside((struct m4_dt_range){0,UINT64_C(1)<<42},s->physical,s->size) ||
           ((s->iova|s->physical|s->size)&(M4_DT_PAGE-1)) || s->iova<end ||
           (s->attributes&~UINT64_C(15))!=FULL_PAGE || !(s->attributes&1))return 0;
        pages+=s->size/M4_DT_PAGE; end=s->iova+s->size;
    }
    return pages==r->leaf_entries;
}
int m4_dp_check_mapping(const struct m4_dt_result *r,uint64_t iova,
                        uint64_t physical,uint64_t size,unsigned flags)
{
    if(!snapshot(r) || !flags || (flags&~7u) || !(flags&3u) ||
       ((iova|physical|size)&(M4_DT_PAGE-1)) ||
       !inside((struct m4_dt_range){0,M4_DT_WINDOW},iova,size) ||
       !inside((struct m4_dt_range){0,UINT64_C(1)<<42},physical,size))return -1;
    uint64_t at=iova, end=iova+size;
    for(unsigned i=0;i<r->spans && at<end;i++) {
        const struct m4_dt_span *s=&r->map[i];
        if(at>=s->iova+s->size)continue;
        if(at<s->iova)return -2;
        if(s->physical+(at-s->iova)!=physical+(at-iova))return -3;
        /* APPLE_DART2 bits: no-cache=1, no-write=2, no-read=3. */
        if(((flags&1) && (s->attributes&8)) ||
           ((flags&2) && (s->attributes&4)) ||
           ((flags&4) && (s->attributes&2)))return -4;
        uint64_t next=s->iova+s->size;at=next<end?next:end;
    }
    return at==end?0:-2;
}
int m4_dp_plan(const struct m4_dt_input *in,const struct m4_dt_result *r,
               const struct m4_dt_workspace *w,const struct m4_dt_range *owners,
               size_t owner_count,struct m4_dp_plan *out)
{
    struct m4_dp_plan p={0};
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!in || !w || !owners || !owner_count || owner_count>M4_DP_MAX_OWNERS || !snapshot(r) ||
       in->excluded_count>6 ||
       in->tables.size>sizeof(*w) || in->tables.size<M4_DT_PAGE ||
       ((in->tables.base|in->tables.size|in->root)&(M4_DT_PAGE-1)) ||
       !inside(in->tables,in->root,M4_DT_PAGE) ||
       !inside(in->dram,in->tables.base,in->tables.size) ||
       !covered(owners,owner_count,in->tables.base,in->tables.size) ||
       in->tcr!=1 || !(in->protect&1) || in->params[2]!=0x2a2a0202 ||
       ((in->params[0]>>24)&15)!=14 || (in->ttbr&3)!=1 ||
       in->root!=((uint64_t)(in->ttbr>>2)<<14))return -1;
    for(unsigned i=0;i<in->excluded_count;i++)
        if(!inside((struct m4_dt_range){0,UINT64_MAX},in->excluded[i].base,in->excluded[i].size) ||
           overlaps(in->excluded[i],in->tables.base,in->tables.size))return -1;
    if(!((in->sid==23 && in->controller==UINT64_C(0x402340000)) ||
         ((in->sid==0 || in->sid==16) && in->controller==UINT64_C(0x402300000))))return -2;
    const uint64_t *root=&w->words[(in->root-in->tables.base)/8];
    uint64_t leaves=0;unsigned span=0;
    for(unsigned i=0;i<M4_DT_WORDS;i++) {
        uint64_t entry=root[i];
        if(!entry)continue;
        uint64_t table=(entry&PA_MASK)<<4;
        if(!(entry&1) || (entry&~(PA_MASK|1)) || table==in->root ||
           !inside(in->tables,table,M4_DT_PAGE))return -3;
        if(p.slots>=M4_DT_TABLES-1)return -4;
        const uint64_t *leaf=&w->words[(table-in->tables.base)/8];
        for(unsigned j=0;j<M4_DT_WORDS;j++) {
            uint64_t value=leaf[j],iova=(uint64_t)i*M4_DP_SLOT+(uint64_t)j*M4_DT_PAGE;
            if(!value)continue;
            if(!(value&1))return -3;
            while(span<r->spans && r->map[span].iova+r->map[span].size<=iova)span++;
            if(span==r->spans || iova<r->map[span].iova ||
               ((value&PA_MASK)<<4)!=r->map[span].physical+iova-r->map[span].iova ||
               (value&~PA_MASK)!=r->map[span].attributes)return -3;
            leaves++;
        }
        uint64_t base=(uint64_t)i*M4_DP_SLOT;
        if(p.count && p.iova[p.count-1].base+p.iova[p.count-1].size==base)
            p.iova[p.count-1].size+=M4_DP_SLOT;
        else {
            if(p.count==M4_DP_RANGES)return -4;
            p.iova[p.count++]=(struct m4_dt_range){base,M4_DP_SLOT};
        }
        p.slots++;
    }
    if(!p.slots || p.slots!=r->root_entries || leaves!=r->leaf_entries)return -5;
    for(unsigned i=0;i<r->spans;i++) {
        const struct m4_dt_span *s=&r->map[i];
        if(!inside(in->dram,s->physical,s->size) ||
           !covered(owners,owner_count,s->physical,s->size) ||
           !covered(p.iova,p.count,s->iova,s->size))return -6;
        for(unsigned j=0;j<in->excluded_count;j++)
            if(overlaps(in->excluded[j],s->physical,s->size))return -6;
        p.pages+=s->size/M4_DT_PAGE;
        if(s->attributes&4)p.read_only_pages+=s->size/M4_DT_PAGE;
        if(s->attributes&2)p.uncached_pages+=s->size/M4_DT_PAGE;
    }
    p.sid=in->sid;p.table_root=in->root;p.table_checksum=r->checksum;
    *out=p;return 0;
}
static int cells(const void *fdt,int node,const char *key)
{
    int n;const fdt32_t *p=fdt_getprop(fdt,node,key,&n);
    return p && n==4 && fdt32_ld(p)==2;
}
int m4_dp_write_reservations(const void *base,size_t base_bytes,
                             void *scratch,size_t capacity,
                             const char *device,const struct m4_dp_plan *p)
{
    if(!base || !scratch || !device || !p || !p->count || p->count>M4_DP_RANGES ||
       base_bytes<sizeof(struct fdt_header) || fdt_check_full(base,base_bytes) ||
       capacity>INT32_MAX)return -1;
    uintptr_t b=(uintptr_t)base,s=(uintptr_t)scratch;
    size_t n=fdt_totalsize(base);
    if(b>UINTPTR_MAX-n || s>UINTPTR_MAX-capacity || (b<s+capacity && s<b+n))return -1;
    int ret=fdt_open_into(base,scratch,capacity);if(ret)return ret;
    int dev=fdt_path_offset(scratch,device),len;
    const char *status=fdt_getprop(scratch,dev,"status",&len);
    if(dev<0 || !status || len!=9 || memcmp(status,"disabled",9) ||
       !cells(scratch,dev,"#address-cells") || !cells(scratch,dev,"#size-cells") ||
       !cells(scratch,0,"#address-cells") || !cells(scratch,0,"#size-cells"))return -2;
    uint32_t ph=fdt_get_phandle(scratch,dev);
    if(!ph || ph==UINT32_MAX || fdt_getprop(scratch,dev,"memory-region",NULL))return -3;
    int rm=fdt_path_offset(scratch,"/reserved-memory");
    if(rm<0) {
        if(rm!=-FDT_ERR_NOTFOUND)return rm;
        rm=fdt_add_subnode(scratch,0,"reserved-memory");if(rm<0)return rm;
        if((ret=fdt_setprop_u32(scratch,rm,"#address-cells",2)) ||
           (ret=fdt_setprop_u32(scratch,rm,"#size-cells",2)) ||
           (ret=fdt_setprop_empty(scratch,rm,"ranges")))return ret;
    }
    if(!cells(scratch,rm,"#address-cells") || !cells(scratch,rm,"#size-cells"))return -4;
    const void *ranges=fdt_getprop(scratch,rm,"ranges",&len);
    if(!ranges || len)return -4;
    for(unsigned i=0;i<p->count;i++) {
        if(!inside((struct m4_dt_range){0,M4_DT_WINDOW},p->iova[i].base,p->iova[i].size) ||
           ((p->iova[i].base|p->iova[i].size)&(M4_DP_SLOT-1)))return -5;
        char name[64];snprintf(name,sizeof(name),"m4-fw-iova-%x-%u",p->sid,i);
        rm=fdt_path_offset(scratch,"/reserved-memory");
        if(fdt_subnode_offset(scratch,rm,name)!=-FDT_ERR_NOTFOUND)return -6;
        int node=fdt_add_subnode(scratch,rm,name);if(node<0)return node;
        uint32_t resv;if((ret=fdt_generate_phandle(scratch,&resv)))return ret;
        fdt32_t map[]={cpu_to_fdt32(ph),cpu_to_fdt32(p->iova[i].base>>32),
            cpu_to_fdt32(p->iova[i].base),cpu_to_fdt32(p->iova[i].size>>32),cpu_to_fdt32(p->iova[i].size)};
        if((ret=fdt_setprop_u32(scratch,node,"phandle",resv)) ||
           (ret=fdt_setprop(scratch,node,"iommu-addresses",map,sizeof(map))))return ret;
        dev=fdt_path_offset(scratch,device);
        if((ret=fdt_appendprop_u32(scratch,dev,"memory-region",resv)))return ret;
    }
    return fdt_pack(scratch);
}

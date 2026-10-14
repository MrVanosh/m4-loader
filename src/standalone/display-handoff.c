/* SPDX-License-Identifier: MIT */
/* Inert current-boot display mapping evidence; no device is enabled. */
#include "display-handoff.h"
#include "libfdt.h"
#include <string.h>

static int range(struct m4_dt_range r)
{ return r.size && r.base <= UINT64_MAX-r.size; }
static int contains(struct m4_dt_range r, uint64_t base, uint64_t bytes)
{ return range(r) && bytes && base >= r.base && bytes <= r.size && base-r.base <= r.size-bytes; }
int m4_display_handoff_plan(const struct m4_dt_input *input,const struct m4_dt_result *result,
                           const struct m4_dt_workspace *workspace,const struct m4_dt_range *owners,
                           unsigned count,struct m4_dt_range framebuffer,struct m4_dp_plan *plan)
{
    if(!input || input->excluded_count!=4 ||
       input->excluded[3].base!=framebuffer.base || input->excluded[3].size!=framebuffer.size ||
       !contains(input->dram,framebuffer.base,framebuffer.size))return -1;
    /* Table reads exclude the framebuffer. The inherited mapping may point
     * at that exact reserved framebuffer; keep all other loader exclusions. */
    struct m4_dt_input mapped=*input;mapped.excluded_count=3;
    return m4_dp_plan(&mapped,result,workspace,owners,count,plan);
}
static int good(const struct m4_display_handoff *h)
{
    if (h->status || h->completed != 3 || h->metadata_completed != 16 ||
        h->metadata_phase != DART_COMPLETE || !range(h->dram) ||
        !contains(h->dram,h->framebuffer,h->framebuffer_bytes) ||
        h->frequency != UINT64_C(1000000000)) return 0;
    for (unsigned i=0;i<3;i++) {
        const struct m4_dt_input *d=&h->input[i];
        const struct m4_dt_result *r=&h->result[i];
        const struct m4_dp_plan *p=&h->plan[i];
        if (r->phase != 5 || r->error || r->stable != 1 ||
            r->invalid_nonzero || r->outside_dram || !r->spans || r->spans>M4_DT_SPANS ||
            !p->count || p->count>M4_DP_RANGES || !p->slots ||
            p->sid!=d->sid || p->table_root!=d->root || p->table_checksum!=r->checksum ||
            p->slots!=r->root_entries || p->pages!=r->leaf_entries ||
            p->read_only_pages>p->pages || p->uncached_pages>p->pages ||
            d->dram.base!=h->dram.base || d->dram.size!=h->dram.size ||
            d->window.size!=M4_DT_WINDOW || (d->window.base&(M4_DT_WINDOW-1)) ||
            d->params[2]!=0x2a2a0202 || ((d->params[0]>>24)&15)!=14 ||
            !(d->protect&1) || d->tcr!=1 || (d->ttbr&3)!=1 ||
            d->root!=((uint64_t)(d->ttbr>>2)<<14) ||
            d->sid!=(i==0?23:i==1?0:16) ||
            d->controller!=(i==0?UINT64_C(0x402340000):UINT64_C(0x402300000)) ||
            !contains(d->tables,d->root,M4_DT_PAGE) ||
            !contains(h->dram,d->tables.base,d->tables.size)) return 0;
        uint64_t end=0,pages=0,readonly=0,uncached=0;
        for (unsigned j=0;j<r->spans;j++) {
            const struct m4_dt_span *s=&r->map[j];
            if (((s->iova|s->physical|s->size)&(M4_DT_PAGE-1)) ||
                !contains((struct m4_dt_range){0,M4_DT_WINDOW},s->iova,s->size) ||
                !contains(h->dram,s->physical,s->size) || s->iova<end ||
                (s->attributes&~UINT64_C(15))!=UINT64_C(0x000fff0000000000) ||
                !(s->attributes&1)) return 0;
            uint64_t n=s->size/M4_DT_PAGE;pages+=n;
            if(s->attributes&4)readonly+=n;if(s->attributes&2)uncached+=n;
            end=s->iova+s->size;
        }
        if (pages!=p->pages || readonly!=p->read_only_pages || uncached!=p->uncached_pages) return 0;
        end=0;uint64_t slots=0;
        for (unsigned j=0;j<p->count;j++) {
            if (((p->iova[j].base|p->iova[j].size)&(M4_DP_SLOT-1)) ||
                !contains((struct m4_dt_range){0,M4_DT_WINDOW},p->iova[j].base,p->iova[j].size) ||
                p->iova[j].base<end) return 0;
            slots+=p->iova[j].size/M4_DP_SLOT;end=p->iova[j].base+p->iova[j].size;
        }
        if(slots!=p->slots)return 0;
        for(unsigned j=0;j<r->spans;j++) {
            unsigned covered=0;
            for(unsigned k=0;k<p->count;k++)covered|=contains(p->iova[k],r->map[j].iova,r->map[j].size);
            if(!covered)return 0;
        }
    }
    uint64_t total=0;
    for(unsigned i=0;i<M4_DT_SEGMENTS;i++) {
        const struct m4_dt_segment *s=&h->segment[i];
        uint64_t size=((uint64_t)s->size+M4_DT_PAGE-1)&~(uint64_t)(M4_DT_PAGE-1);total+=size;
        if(!s->size || size>UINT64_C(0x10000000) || total>UINT64_C(0x20000000) ||
           ((s->physical|s->remap)&(M4_DT_PAGE-1)) || !contains(h->dram,s->physical,size) ||
           !contains(h->input[0].window,s->remap,size))return 0;
    }
    return 1;
}
#ifdef M4_DCP_STAGE
int m4_display_handoff_accepted(const struct m4_display_handoff *h)
{ return h && good(h); }
#endif
int m4_display_handoff_write(void *tree,size_t bytes,void *scratch,size_t capacity,
                            const struct m4_display_handoff *h)
{
    /* ponytail: one-shot loader writer; keep the 10 KiB buffer off its guarded
     * stack. A concurrent host tool would supply a caller-owned buffer. */
    static uint64_t wire[M4_DISPLAY_HANDOFF_WORDS]; unsigned n=0;
    if (!tree || !scratch || !h || bytes<sizeof(struct fdt_header) ||
        capacity>INT32_MAX || fdt_check_full(tree,bytes)) return -1;
    uintptr_t a=(uintptr_t)tree,b=(uintptr_t)scratch; size_t used=fdt_totalsize(tree);
    if (a>UINTPTR_MAX-used || b>UINTPTR_MAX-capacity ||
        (a<b+capacity && b<a+used)) return -1;
    int accepted=good(h);
    if (!h->status && !accepted) return -2; /* Never relabel malformed success. */
#define PUT(v) do { if(n==M4_DISPLAY_HANDOFF_WORDS)return -3; wire[n++]=cpu_to_fdt64((uint64_t)(v)); } while(0)
    PUT(M4_DISPLAY_HANDOFF_MAGIC); PUT(1); PUT(0); PUT(h->status);
    PUT(accepted?3:0); PUT(accepted?M4_DT_SEGMENTS:0);
    PUT(h->dram.base); PUT(h->dram.size); PUT(h->framebuffer); PUT(h->framebuffer_bytes);
    PUT(h->frequency); PUT(h->metadata_completed); PUT(h->metadata_phase);
    PUT(h->active); PUT(h->esr); PUT(h->far);
    PUT(h->capture_phase); PUT(h->capture_error); PUT(h->active_address); PUT(h->elapsed);
    for (unsigned i=0;accepted && i<3;i++) {
        const struct m4_dt_input *d=&h->input[i];
        const struct m4_dt_result *r=&h->result[i]; const struct m4_dp_plan *p=&h->plan[i];
        PUT(d->controller); PUT(d->sid);
        for(unsigned j=0;j<4;j++)PUT(d->params[j]);
        PUT(d->protect); PUT(d->tcr); PUT(d->ttbr); PUT(d->tables.base); PUT(d->tables.size);
        PUT(d->root); PUT(d->window.base); PUT(d->window.size);
        PUT(r->checksum); PUT(r->loads); PUT(r->root_entries); PUT(r->leaf_entries);
        PUT(r->stable); PUT(r->spans); PUT(p->count); PUT(p->slots);
        PUT(p->read_only_pages); PUT(p->uncached_pages);
        for(unsigned j=0;j<r->spans;j++) {
            PUT(r->map[j].iova); PUT(r->map[j].physical); PUT(r->map[j].size); PUT(r->map[j].attributes);
        }
        for(unsigned j=0;j<p->count;j++) { PUT(p->iova[j].base); PUT(p->iova[j].size); }
    }
    for(unsigned i=0;accepted && i<M4_DT_SEGMENTS;i++) {
        PUT(h->segment[i].physical); PUT(h->segment[i].legacy); PUT(h->segment[i].remap);
        PUT(((uint64_t)h->segment[i].size<<32)|h->segment[i].flags);
    }
    wire[2]=cpu_to_fdt64(n*8);
#undef PUT
    int ret=fdt_open_into(tree,scratch,capacity); if(ret)return ret;
    int chosen=fdt_path_offset(scratch,"/chosen"); if(chosen<0)return chosen;
    if(fdt_getprop(scratch,chosen,"m4-display-handoff",NULL))return -4;
    ret=fdt_setprop(scratch,chosen,"m4-display-handoff",wire,n*8); if(ret)return ret;
    if((ret=fdt_pack(scratch)) || fdt_check_full(scratch,capacity))return ret?ret:-5;
    if(fdt_totalsize(scratch)>bytes)return -6;
    memcpy(tree,scratch,fdt_totalsize(scratch)); return 0;
}

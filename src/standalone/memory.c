/* SPDX-License-Identifier: MIT
 * Portable counterpart of the native-qualified tools/m4_memory_plan.py.
 * Only computes intervals; never probes or writes the memory it describes.
 * Reservations are rounded outwards; holes smaller than 1MiB stay excluded.
 */
#include "memory.h"
int m4_range_valid(struct m4_range r) {
    return r.size && r.base < UINT64_MAX-r.size;
}
int m4_contains(struct m4_range o,struct m4_range i) {
    return m4_range_valid(o)&&m4_range_valid(i)&&i.base>=o.base&&i.base+i.size<=o.base+o.size;
}
int m4_overlaps(struct m4_range a,struct m4_range b) {
    return m4_range_valid(a)&&m4_range_valid(b)&&a.base<b.base+b.size&&b.base<a.base+a.size;
}
int m4_reserve(struct m4_memory *m,uint64_t b,uint64_t n) {
    struct m4_range r={b,n};
    if(!m||!m4_range_valid(r)||m->reserved_count>=M4_MAX_RESERVED)return -1;
    m->reserved[m->reserved_count++]=r;return 0;
}
static int add_free(struct m4_memory *m,uint64_t b,uint64_t end) {
    if(end<b)return -1;
    if(end-b<1024*1024)return 0;
    if(m->free_count>=M4_MAX_FREE)return -1;
    m->free[m->free_count++]=(struct m4_range){b,end-b};
    m->free_bytes+=end-b;return 0;
}
int m4_memory_finish(struct m4_memory *m) {
    if(!m||m->reserved_count>M4_MAX_RESERVED||
       !m4_contains(m->dram,m->usable)||
       ((m->usable.base|m->usable.size|m->dram.base|m->dram.size)&(M4_PAGE-1))||
       m->top<m->usable.base||m->top>m->usable.base+m->usable.size||
       m->ctrr_low<m->usable.base||m->ctrr_high<m->ctrr_low||m->ctrr_high>=m->top||
       m->ctrr_high>UINT64_MAX-4096-M4_PAGE)return -1;
    m->free_count=0;m->free_bytes=0;
    if(m4_reserve(m,m->ctrr_low&~(M4_PAGE-1),
       ((m->ctrr_high+4096+M4_PAGE-1)&~(M4_PAGE-1))-(m->ctrr_low&~(M4_PAGE-1))))return -1;
    /* Insertion sort is bounded by 512 metadata records, no allocation. */
    for(unsigned i=1;i<m->reserved_count;i++) {
        struct m4_range r=m->reserved[i];unsigned j=i;
        while(j&&m->reserved[j-1].base>r.base){m->reserved[j]=m->reserved[j-1];j--;}
        m->reserved[j]=r;
    }
    uint64_t cursor=(m->top+M4_PAGE-1)&~(M4_PAGE-1),end=m->usable.base+m->usable.size;
    for(unsigned i=0;i<m->reserved_count;i++) {
        struct m4_range r=m->reserved[i];
        if(!m4_range_valid(r)||r.base+r.size>UINT64_MAX-M4_PAGE)return -1;
        uint64_t lo=r.base&~(M4_PAGE-1),hi=(r.base+r.size+M4_PAGE-1)&~(M4_PAGE-1);
        if(lo>=end||hi<=cursor)continue;
        if(lo<cursor)lo=cursor;
        if(hi>end)hi=end;
        if(lo>cursor&&add_free(m,cursor,lo))return -1;
        if(hi>cursor)cursor=hi;
    }
    if(add_free(m,cursor,end)||!m->free_count)return -1;
    return 0;
}

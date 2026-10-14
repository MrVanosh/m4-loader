/* SPDX-License-Identifier: GPL-2.0-only */
#include "../build/build_cfg.h"
#ifdef J616S_SCREEN_DIAGNOSTIC
#include "j616s_usb_plan.h"
#include "j616s_diag.h"
#include "adt.h"
#include "xnuboot.h"
#include "string.h"

/* Pure ADT parsing: no allocation, register accesses or normal pmgr_init.
 * Every selected clock and its parents must resolve before any writes. */
struct context {
    struct j616s_usb_plan *p;
    int arm, pmgr;
    u64 dram, dram_size;
    const u8 *ranges, *devices, *ps;
    u32 nranges, ndev, nps;
    bool u8id, groups;
    u8 visit[1024];
};
static u32 get32(const void *p) { u32 v; memcpy(&v,p,4); return v; }
static u64 get64(const void *p) { u64 v; memcpy(&v,p,8); return v; }
static const u8 *prop(int node, const char *name, u32 *size)
{
    *size=0;
    if (node<0) return NULL;
    const void *v=adt_getprop(adt,node,name,size);
    if (!v || !j616s_range_contains((u64)adt,cur_boot_args.devtree_size,(u64)v,*size))
        return NULL;
    return v;
}
static bool value32(int node,const char *name,u32 expected)
{
    u32 n; const u8 *v=prop(node,name,&n);
    return v && n==4 && get32(v)==expected;
}
static bool reg(struct context *c,int node,unsigned index,u64 need,u64 *addr)
{
    u32 n; const u8 *v=prop(node,"reg",&n);
    if (!v || n%16 || index>=n/16) return false;
    u64 a=get64(v+index*16), s=get64(v+index*16+8);
    if (!j616s_range_valid(a,s) || s<need) return false;
    unsigned matched=0;
    for (unsigned i=0;i<c->nranges;i++) {
        const u8 *r=c->ranges+24*i;
        u64 child=get64(r),phys=get64(r+8),size=get64(r+16);
        if (!j616s_range_valid(child,size) || !j616s_range_valid(phys,size)) return false;
        if (j616s_range_contains(child,size,a,s)) {
            if (++matched!=1) return false;
            *addr=phys+(a-child);
        }
    }
    return matched==1 && !(*addr&3) && *addr+s<=(1UL<<42) &&
        !j616s_range_overlaps(*addr,s,c->dram,c->dram_size);
}
static u16 dev_id(struct context *c,const u8 *d)
{ return c->u8id ? d[3] : (u16)d[26] | ((u16)d[27]<<8); }
static bool visit(struct context *c,u16 id,unsigned depth)
{
    if (!id || depth>32) return false;
    unsigned idx=c->ndev;
    for (unsigned i=0;i<c->ndev;i++)
        if (dev_id(c,c->devices+48*i)==id) {
            if(idx!=c->ndev) return false;
            idx=i;
        }
    if (idx==c->ndev || c->visit[idx]==1) return false;
    if (c->visit[idx]==2) return true;
    c->visit[idx]=1;
    const u8 *d=c->devices+48*idx;
    for (unsigned j=0;j<2;j++) {
        u16 parent=c->u8id ? d[4+j] : (u16)d[4+2*j] | ((u16)d[5+2*j]<<8);
        if (parent && !visit(c,parent,depth+1)) return false;
    }
    if (!(d[0]&0x10)) {
        unsigned group=c->groups ? d[19] : d[11];
        u64 offset=c->groups ? get32(d+16)&0xffffff : (u64)d[10]*8;
        if (group>=c->nps || c->p->count==J616S_USB_POWER_MAX) return false;
        const u8 *ps=c->ps+group*12;
        offset+=get32(ps+4);
        u64 base;
        if ((offset&3) || !reg(c,c->pmgr,get32(ps),offset+4,&base)) return false;
        u64 addr=base+offset;
        for (unsigned i=0;i<c->p->count;i++) if (c->p->power[i]==addr) return false;
        c->p->ids[c->p->count]=id;
        c->p->power[c->p->count++]=addr;
    }
    c->visit[idx]=2;
    return true;
}
static bool gates(struct context *c,int node)
{
    u32 n; const u8 *v=prop(node,"clock-gates",&n);
    if (!v || n%4 || !n || n>64) return false;
    for(unsigned i=0;i<n;i+=4) {
        u32 id=get32(v+i);
        /* This target is single-die. Reject reserved bits and other dies. */
        if (id>>16 || !visit(c,id,0)) return false;
    }
    return true;
}
bool j616s_usb_plan_build(struct j616s_usb_plan *p)
{
    memset(p,0,sizeof(*p));
    struct context c={.p=p};
    p->error=1;
    c.arm=adt_path_offset(adt,"/arm-io");
    c.pmgr=adt_path_offset(adt,"/arm-io/pmgr");
    int chosen=adt_path_offset(adt,"/chosen");
    u32 n; const u8 *v=prop(chosen,"dram-base",&n);
    if (!v || n!=8) return false;
    c.dram=get64(v); v=prop(chosen,"dram-size",&n);
    if (!v || n!=8) return false;
    c.dram_size=get64(v);
    if (!j616s_range_valid(c.dram,c.dram_size) || c.pmgr<0 ||
        !value32(c.arm,"#address-cells",2) || !value32(c.arm,"#size-cells",2) ||
        !value32(0,"#address-cells",2) || !value32(0,"#size-cells",2) ||
        adt_path_offset(adt,"/arm-io/nub-spmi-a0/hpm0")<0) return false;
    v=prop(c.arm,"die-count",&n);
    if (v && (n!=4 || get32(v)!=1)) return false;
    c.ranges=prop(c.arm,"ranges",&n);
    if (!c.ranges || n%24 || !n || n>32*24) return false;
    c.nranges=n/24;
    int phy=adt_path_offset(adt,"/arm-io/atc-phy0");
    int drd=adt_path_offset(adt,"/arm-io/usb-drd0");
    int dart=adt_path_offset(adt,"/arm-io/dart-usb0");
    int mapper=adt_path_offset(adt,"/arm-io/dart-usb0/mapper-usb0");
    p->error=2;
    v=prop(dart,"compatible",&n);
    bool compatible=false;
    if (!v || n>256 || v[n-1]) return false;
    for (unsigned i=0;i<n;) {
        size_t len=strnlen((const char *)v+i,n-i);
        if (len==n-i) return false;
        if (len==10 && !memcmp(v+i,"dart,t8110",10)) compatible=true;
        i+=len+1;
    }
    for(unsigned i=0;i<n && i<sizeof(p->compatibility)-1;i++)
        p->compatibility[i]=v[i] ? v[i] : ' ';
    if (!compatible) return false;
    p->error=3;
    if (!reg(&c,phy,0,0x20,&p->phy) || !reg(&c,drd,0,0xcd00,&p->drd) ||
        !reg(&c,drd,3,0x24,&p->pipe) || !reg(&c,dart,1,0x1800,&p->dart)) return false;
    /* Disjoint validated apertures: the offsets below stay within each. */
    u64 a[]={p->phy,p->drd,p->pipe,p->dart}, s[]={0x20,0xcd00,0x24,0x1800};
    for(unsigned i=0;i<4;i++) for(unsigned j=0;j<i;j++)
        if(j616s_range_overlaps(a[i],s[i],a[j],s[j])) return false;
    v=prop(mapper,"reg",&n);
    if (!v || n!=4 || get32(v)>255) return false;
    p->sid=get32(v);
    p->error=4;
    c.devices=prop(c.pmgr,"devices",&n);
    if (!c.devices || n%48 || n<96 || n>1024*48) return false;
    c.ndev=n/48; c.u8id=c.devices[3]!=c.devices[51];
    c.ps=prop(c.pmgr,"ps-regs",&n);
    if (!c.ps) { c.groups=true; c.ps=prop(c.pmgr,"ps-groups",&n); }
    if (!c.ps || n%12 || !n || n>256*12) return false;
    c.nps=n/12;
    if(!gates(&c,phy) || !gates(&c,dart) || !gates(&c,drd) || !p->count) return false;
    for(unsigned i=0;i<p->count;i++) for(unsigned j=0;j<4;j++)
        if(j616s_range_overlaps(p->power[i],4,a[j],s[j])) return false;
    p->error=0;
    return true;
}
#endif

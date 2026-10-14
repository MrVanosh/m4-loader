/* SPDX-License-Identifier: GPL-2.0-only */
#include "../build/build_cfg.h"
#ifdef J616S_SCREEN_DIAGNOSTIC
#include "j616s_proxy.h"
#include "j616s_diag.h"
#include "proxy.h"
#include "string.h"

/* Gravity/m1n1 wire format: 64-byte request, 36-byte reply; protocol
 * checksums are intrinsic framing, with no host/Mac checksum utility. */
#define REQ_NOP 0x00aa55ffU
#define REQ_PROXY 0x01aa55ffU
#define REQ_READ 0x02aa55ffU
#define REQ_WRITE 0x03aa55ffU
#define BAD_COMMAND -1
#define INVALID -2
#define TRANSFER_ERROR -3
#define BAD_CHECKSUM -4
#define MAX_TRANSFER (1024*1024UL)
#define PROGRESS_MS 10000
struct session {
    const struct j616s_stream *io;
    const struct j616s_proxy_memory *m;
    struct j616s_proxy_stats *stats;
    u64 used;
};
static u32 get32(const void *p){u32 v;memcpy(&v,p,4);return v;}
static u64 get64(const void *p){u64 v;memcpy(&v,p,8);return v;}
static void put32(void *p,u32 v){memcpy(p,&v,4);}
static void put64(void *p,u64 v){memcpy(p,&v,8);}
static u32 checksum(const void *p,size_t len)
{
    const u8 *b=p;u32 sum=0xdeadbeef;
    while(len--) sum=sum*31337+(*b++^0x5a);
    return sum^0xaddedbad;
}
static bool transfer(struct session *s,void *buf,size_t len,bool send)
{
    u8 *p=buf;u64 progress=s->io->milliseconds(s->io->opaque);
    while(len) {
        size_t n=len<16384?len:16384;
        ssize_t done=send?s->io->write(s->io->opaque,p,n):s->io->read(s->io->opaque,p,n);
        if(done<0 || (size_t)done>n) return false;
        u64 now=s->io->milliseconds(s->io->opaque);
        if(done) {p+=done;len-=done;progress=now;}
        else if(now-progress>=PROGRESS_MS) return false;
    }
    return !len;
}
static bool frame(struct session *s,u8 req[64])
{
    u32 sync=0;
    for(;;) {
        u8 b;ssize_t got=s->io->read(s->io->opaque,&b,1);
        if(got<0 || got>1) return false;
        if(!got) continue;
        sync=(sync>>8)|((u32)b<<24);
        if((sync&0xffffff)==0xaa55ff) {
            put32(req,sync);
            return transfer(s,req+4,60,false);
        }
    }
    return false;
}
static bool allocated(struct session *s,u64 p,u64 size)
{return j616s_range_contains(s->m->heap,s->used,p,size);}
static bool readable(struct session *s,u64 p,u64 size)
{
    return allocated(s,p,size) ||
        j616s_range_contains(s->m->boot,s->m->boot_size,p,size) ||
        j616s_range_contains(s->m->adt,s->m->adt_size,p,size);
}
static u64 allocate(struct session *s,u64 align,u64 size)
{
    if(!size || !align || (align&(align-1)) || align>0x4000) return 0;
    if(align<16) align=16;
    u64 offset=(s->used+align-1)&~(align-1);
    if(offset>s->m->heap_size || size>s->m->heap_size-offset) return 0;
    s->used=offset+size;
    void *p=(void *)(uintptr_t)(s->m->heap+offset);
    memset(p,0,size);
    return (u64)p;
}
static bool program(struct session *s,u64 addr)
{
    if(addr&3) return false;
    return allocated(s,addr,4);
}
static bool proxy(struct session *s,const u8 *req,u8 *reply)
{
    u64 op=get64(req+4),a[6],value=0;s64 status=0;bool exit=false;
    for(unsigned i=0;i<6;i++) a[i]=get64(req+12+8*i);
    switch(op) {
    case P_WORKBENCH_INFO:
        switch(a[0]) {
        case 0:value=15;break;
        case 1:value=s->m->heap;break;
        case 2:value=s->m->heap_size;break;
        case 3:value=s->used;break;
        case 4:value=s->stats->calls;break;
        default:status=INVALID;break;
        }
        break;
    case P_WORKBENCH_RESET:
        /* Explicit only: no implicit reset on a new host connection. */
        memset((void *)(uintptr_t)s->m->heap,0,s->used);s->used=0;break;
    case P_WORKBENCH_FAULT:
        if(!s->io->fault || a[0]>4)status=INVALID;
        else value=s->io->fault(a[0]);
        break;
    case P_NOP: break;
    case P_GET_BOOTARGS:value=s->m->boot;break;
    case P_GET_BASE:value=s->m->base;break;
    case P_GET_CHIPID:value=0x6040;break;
    case P_EXIT:exit=true;break;
    case P_MALLOC:value=allocate(s,16,a[0]);break;
    case P_MEMALIGN:value=allocate(s,a[0],a[1]);break;
    case P_FREE:if(a[0]&&!allocated(s,a[0],1))status=INVALID;break;
    case P_CALL:
        if(!program(s,a[0]) || !s->io->execute) status=INVALID;
        else {
            value=s->io->execute(a[0],a+1);s->stats->calls++;
            if(s->io->fault && s->io->fault(0)) status=-5;
        }
        break;
    case P_READ64:case P_READ32:case P_READ16:case P_READ8: {
        unsigned n=8>>(op-P_READ64);
        if(!readable(s,a[0],n)) status=INVALID;
        else memcpy(&value,(void *)(uintptr_t)a[0],n);
        break;
    }
    case P_WRITE64:case P_WRITE32:case P_WRITE16:case P_WRITE8: {
        unsigned n=8>>(op-P_WRITE64);
        if(!allocated(s,a[0],n)) status=INVALID;
        else memcpy((void *)(uintptr_t)a[0],a+1,n);
        break;
    }
    default:status=BAD_COMMAND;break;
    }
    if(status) s->stats->rejected++;
    put64(reply+8,op);put64(reply+16,status);put64(reply+24,value);
    return exit;
}
int j616s_proxy_run(const struct j616s_stream *io,const struct j616s_proxy_memory *m,
                   struct j616s_proxy_stats *stats)
{
    if(!io || !io->read || !io->write || !io->milliseconds || !m || !stats ||
       !j616s_range_valid(m->boot,m->boot_size) || m->boot_size!=108 ||
       !j616s_range_valid(m->adt,m->adt_size) || m->adt_size>4*1024*1024 ||
       !j616s_range_valid(m->heap,m->heap_size) || (m->heap&0x3fff) ||
       m->heap_size>J616S_UPLOAD_SIZE ||
       j616s_range_overlaps(m->heap,m->heap_size,m->boot,m->boot_size) ||
       j616s_range_overlaps(m->heap,m->heap_size,m->adt,m->adt_size)) return 10;
    memset(stats,0,sizeof(*stats));
    struct session s={io,m,stats,0};
    for(;;) {
        u8 req[64],reply[36]={0};
        if(!frame(&s,req)) return 7;
        u32 type=get32(req);put32(reply,type);
        s32 status=0;bool exit=false,terminal=false,send_data=false;
        u64 addr=get64(req+4),size=get64(req+12);
        if(checksum(req,60)!=get32(req+60)) {status=BAD_CHECKSUM;terminal=true;}
        else {
            stats->commands++;
            switch(type) {
            case REQ_NOP:break; /* Advertise zero optional features. */
            case REQ_PROXY:exit=proxy(&s,req,reply);break;
            case REQ_READ:
                if(size>MAX_TRANSFER || (size&&!readable(&s,addr,size))) status=INVALID;
                else {put32(reply+8,checksum((void *)(uintptr_t)addr,size));send_data=true;}
                break;
            case REQ_WRITE:
                if(size>MAX_TRANSFER) {status=INVALID;terminal=true;break;}
                if(size&&!allocated(&s,addr,size)) {
                    /* The stock client sends data before receiving a reply.
                     * Drain a bounded invalid request without touching memory. */
                    u8 discard[256];u64 left=size;
                    while(left) {
                        size_t n=left<sizeof(discard)?left:sizeof(discard);
                        if(!transfer(&s,discard,n,false))return 8;
                        left-=n;
                    }
                    status=INVALID;break;
                }
                if(!transfer(&s,(void *)(uintptr_t)addr,size,false))return 8;
                if(checksum((void *)(uintptr_t)addr,size)!=get32(req+20)) {
                    /* A failed upload must not leave executable input behind. */
                    if(size)memset((void *)(uintptr_t)addr,0,size);
                    status=TRANSFER_ERROR;
                } else stats->writes+=size;
                break;
            default:status=BAD_COMMAND;break;
            }
        }
        if(status) stats->rejected++;
        put32(reply+4,status);put32(reply+32,checksum(reply,32));
        if(!transfer(&s,reply,sizeof(reply),true))return 8;
        if(send_data && size) {
            if(!transfer(&s,(void *)(uintptr_t)addr,size,true))return 8;
            stats->reads+=size;
        }
        if(terminal)return 9;
        if(exit)return 0;
    }
    return 7;
}
#endif

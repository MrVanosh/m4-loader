/* SPDX-License-Identifier: GPL-2.0-only */
#include "../build/build_cfg.h"
#ifdef J616S_SCREEN_DIAGNOSTIC
#include "j616s_usb.h"
#include "j616s_proxy.h"
#include "j616s_boot_prefix.h"
#include "adt.h"
#include "malloc.h"
#include "j616s_usb_plan.h"
#include "j616s_screen.h"
#include "j616s_diag.h"
#include "usb_dwc3.h"
#include "usb_dwc3_regs.h"
#include "dart.h"
#include "pmgr.h"
#include "string.h"
#include "utils.h"
#include "vsprintf.h"

extern char _base[],_rodata_end[],_end[];
static u8 arena[J616S_USB_ARENA_SIZE] __attribute__((aligned(0x4000)));
static size_t used;
static unsigned reason, stage, logs;
static char last_log[192];
static struct j616s_usb_plan plan;
static struct j616s_call_result result;
_Static_assert(sizeof(result)==40,"USB guard ABI");

bool j616s_usb_owned(const void *p,size_t len)
{ return j616s_range_contains((u64)arena,sizeof(arena),(u64)p,len); }
/* Allocator pool bounds, for the direct loader's cache maintenance ranges. */
void j616s_usb_pool(u64 *base,u64 *bytes)
{ *base=(u64)arena; *bytes=sizeof(arena); }
void j616s_usb_abort(unsigned why)
{
    reason=why;
    __asm__ volatile("brk #0x6163" ::: "memory");
    for(;;) __asm__ volatile("nop");
}
void *__wrap_memalign(size_t align,size_t len)
{
    if(!align || align&(align-1) || align>0x4000 || !len) return NULL;
    if(align<16) align=16;
    size_t offset=(used+align-1)&~(align-1);
    if(offset>sizeof(arena) || len>sizeof(arena)-offset) return NULL;
    used=offset+len;
    return arena+offset;
}
void *__wrap_malloc(size_t len) { return __wrap_memalign(16,len); }
void *__wrap_calloc(size_t n,size_t len)
{
    if(!n || !len || len>SIZE_MAX/n) return NULL;
    void *p=__wrap_malloc(n*len);
    if(p) memset(p,0,n*len);
    return p;
}
void __wrap_free(void *p)
{
    /* Buffers and DART tables remain reserved until power-off, including
     * timeout/fault paths. Never hand memory back while DMA might exist. */
    if(p && !j616s_usb_owned(p,1)) j616s_usb_abort(90);
}
void *__wrap_realloc(void *p,size_t len)
{ (void)p; (void)len; j616s_usb_abort(91); }
void __wrap___assert_fail(const char *a,const char *b,int line,const char *fn)
{ (void)a;(void)b;(void)line;(void)fn; j616s_usb_abort(92); }
static int logv(const char *fmt,va_list ap)
{
    logs++;
    int n=vsnprintf(last_log,sizeof(last_log),fmt,ap);
    for(unsigned i=0;i<sizeof(last_log) && last_log[i];i++)
        if(last_log[i]=='\r' || last_log[i]=='\n') last_log[i]=' ';
    return n;
}
int __wrap_printf(const char *fmt,...)
{ va_list ap;va_start(ap,fmt);int n=logv(fmt,ap);va_end(ap);return n; }
void __wrap_debug_printf(const char *fmt,...)
{ va_list ap;va_start(ap,fmt);logv(fmt,ap);va_end(ap); }
/* The Rust ADT parser's panic path must not enter the normal console's
 * spinlocks, poll unrelated devices, or call the normal reboot sequence. */
void __wrap_iodev_console_write(const void *p,size_t n)
{
    j616s_screen_printf("RUNTIME: %.*s\n",(int)min(n,160),(const char *)p);
}
void __wrap_flush_and_reboot(void) __attribute__((noreturn));
void __wrap_flush_and_reboot(void)
{
    j616s_screen_puts("USB STOP: parser/runtime panic; preserve this screen");
    j616s_screen_flush();
    __asm__ volatile("msr DAIFSet, #15" ::: "memory");
    for(;;) __asm__ volatile("nop");
}
static void next(unsigned n,const char *s)
{
    stage=n;
    j616s_screen_printf("USB NEXT %u: %s\n",n,s);
    j616s_screen_flush();
}
static int selftest(void)
{
    /* Volatile local forces a real C frame across the deliberate exception. */
    volatile u64 frame[16]; frame[0]=0x6163;
    j616s_usb_abort((unsigned)frame[0]);
}
struct stream_context {dwc3_dev_t *dev;u64 start,freq;};
static u64 milliseconds(void *opaque)
{
    struct stream_context *c=opaque;
    return (mrs(CNTPCT_EL0)-c->start)*1000/c->freq;
}
static void service(struct stream_context *c)
{
    usb_dwc3_handle_events(c->dev);
    usb_dwc3_try_write(c->dev,CDC_ACM_PIPE_0,NULL,0);
}
static ssize_t receive(void *opaque,void *p,size_t n)
{
    struct stream_context *c=opaque;service(c);
    return usb_dwc3_try_read(c->dev,CDC_ACM_PIPE_0,p,n);
}
static ssize_t transmit(void *opaque,const void *p,size_t n)
{
    struct stream_context *c=opaque;service(c);
    return usb_dwc3_try_write(c->dev,CDC_ACM_PIPE_0,p,n);
}
struct saved {u64 v[12];};
static struct saved state(void);
static struct j616s_call_result execution_result;
static u64 execution_address,execution_args[5],execution_value;
static int uploaded_call(void)
{
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
    u64 (*f)(u64,u64,u64,u64,u64)=(void *)(uintptr_t)execution_address;
    execution_value=f(execution_args[0],execution_args[1],execution_args[2],
                      execution_args[3],execution_args[4]);
    return 0;
}
static u64 execute(u64 addr,const u64 *a)
{
    execution_address=addr;memcpy(execution_args,a,sizeof(execution_args));
    execution_value=0;memset(&execution_result,0,sizeof(execution_result));
    struct saved before=state();
    int rc=j616s_exec_call(uploaded_call,&execution_result);
    struct saved after=state();
    unsigned diff=0;
    for(unsigned i=0;i<12;i++)if(before.v[i]!=after.v[i])diff|=1U<<i;
    /* Do not resume USB if trusted code returned with a changed MMU/control
     * baseline. This guard is recovery from ordinary faults, not a sandbox. */
    if(diff) {
        j616s_screen_printf("PROGRAM STOP: changed control registers mask=0x%x\n",diff);
        j616s_screen_flush();
        __asm__ volatile("msr DAIFSet, #15" ::: "memory");
        for(;;)__asm__ volatile("nop");
    }
    if(rc)execution_result.status=2;
    execution_result.value=execution_value;
    return execution_value;
}
static u64 fault(unsigned field)
{
    switch(field) {
    case 0:return execution_result.status;
    case 1:return execution_result.value;
    case 2:return execution_result.esr;
    case 3:return execution_result.elr;
    case 4:return execution_result.far;
    default:return 0;
    }
}
static int attempt(void)
{
    next(1,"power USB0 and its validated parents only");
    for(unsigned i=0;i<plan.count;i++) {
        /* Preserve the exact last index if a register faults or times out. */
        reason=0x100+i;
        if(pmgr_set_mode(plan.power[i],PMGR_PS_ACTIVE)) return 1;
    }
    reason=0;
    next(2,"existing M3/M4 PHY and pipe-handler sequence");
    write32(plan.phy+8,0x01c1000f);
    write32(plan.phy+4,3); write32(plan.phy+4,0);
    write32(plan.phy+0x1c,0x008c0813); write32(plan.phy,2);
    write32(plan.pipe+0xc,0x22); write32(plan.pipe+0x1c,1);
    write32(plan.pipe+0x20,0x9332);
    next(3,"check DWC3 ID; halt/reset BEFORE changing the DART");
    u32 id=read32(plan.drd+DWC3_GSNPSID);
    u32 lock=read32(plan.dart+0x200);
    j616s_screen_printf("USB hardware: SNPSID=0x%x DART-protect=0x%x SID=%u\n",id,lock,plan.sid);
    j616s_screen_flush();
    if((id&DWC3_GSNPSID_MASK)!=0x33310000 || lock&1) return 2;
    clear32(plan.drd+DWC3_DCTL,DWC3_DCTL_RUN_STOP);
    if(poll32(plan.drd+DWC3_DSTS,DWC3_DSTS_DEVCTRLHLT,DWC3_DSTS_DEVCTRLHLT,10000)) return 3;
    set32(plan.drd+DWC3_DCTL,DWC3_DCTL_CSFTRST);
    if(poll32(plan.drd+DWC3_DCTL,DWC3_DCTL_CSFTRST,0,10000)) return 4;
    next(4,"private DART tables and DMA buffers; translation required");
    dart_dev_t *dart=dart_init(plan.dart,plan.sid,false,DART_T8110);
    if(!dart) return 5;
    dwc3_dev_t *dev=usb_dwc3_init(plan.drd,dart);
    if(!dev) return 6;
    u64 freq=mrs(CNTFRQ_EL0);
    void *upload=memalign(0x4000,J616S_UPLOAD_SIZE);
    if(!upload || !freq || freq>1000000000 || !j616s_usb_owned(upload,J616S_UPLOAD_SIZE)) {
        usb_dwc3_shutdown(dev);
        return 10;
    }
    struct stream_context context={dev,mrs(CNTPCT_EL0),freq};
    struct j616s_stream stream={&context,receive,transmit,milliseconds,execute,fault};
    struct j616s_proxy_memory memory={boot_args_addr,J616S_BOOT_PREFIX_SIZE,
        (u64)adt,cur_boot_args.devtree_size,(u64)_base,(u64)upload,J616S_UPLOAD_SIZE};
    struct j616s_proxy_stats stats={0};
    next(5,"WORKBENCH READY: leave Mac on; start Dell V15 bridge; no session timeout");
    j616s_screen_printf("PROXY memory: boot-prefix=%u ADT=%u upload=%u bytes; arena-used=%lu\n",
        (unsigned)J616S_BOOT_PREFIX_SIZE,cur_boot_args.devtree_size,(unsigned)J616S_UPLOAD_SIZE,used);
    j616s_screen_flush();
    int rc=j616s_proxy_run(&stream,&memory,&stats);
    next(6,"drain final USB response; bounded halt; retain private buffers");
    u64 start=milliseconds(&context);
    while(!usb_dwc3_tx_idle(dev,CDC_ACM_PIPE_0) && milliseconds(&context)-start<1000)
        service(&context);
    if(!usb_dwc3_tx_idle(dev,CDC_ACM_PIPE_0) && !rc) rc=11;
    j616s_screen_printf("PROXY result: rc=%d requests=%lu read=%lu written=%lu calls=%lu rejected=%lu\n",
        rc,stats.commands,stats.reads,stats.writes,stats.calls,stats.rejected);
    usb_dwc3_shutdown(dev);
    return rc;
}

static struct saved state(void)
{
    return (struct saved){{mrs(SCTLR_EL2),mrs(TCR_EL2),mrs(TTBR0_EL2),mrs(MAIR_EL2),
        mrs(VBAR_EL2),mrs(DAIF),mrs(ELR_EL2),mrs(SPSR_EL2),mrs(ESR_EL2),mrs(FAR_EL2),
        mrs(PAR_EL1),mrs(HCR_EL2)}};
}
static unsigned call(int (*fn)(void))
{
    struct saved a=state(); int rc=j616s_usb_call(fn,&result); struct saved b=state();
    unsigned diff=0;
    for(unsigned i=0;i<12;i++) if(a.v[i]!=b.v[i]) diff|=1U<<i;
    if(rc || diff) {
        j616s_screen_printf("USB GUARD REFUSED: rc=%d restored-diff=0x%x\n",rc,diff);
        return 1;
    }
    return 0;
}
void j616s_usb_report(u64 lower,u64 upper)
{
    u64 base=(u64)_base,end=(u64)_end,ro=(u64)_rodata_end;
    if(mrs(SCTLR_EL2)!=0x30000180 || !lower || lower>upper ||
        (lower&0xfff) || (upper&0xfff) || upper>=(1UL<<42) ||
        !j616s_range_contains(cur_boot_args.phys_base,cur_boot_args.mem_size,lower,upper-lower+0x1000) ||
        j616s_range_overlaps(base,end-base,lower&~0x3fffUL,
                            ((upper+0x4000)&~0x3fffUL)-(lower&~0x3fffUL)) ||
        !j616s_range_contains(ro,end-ro,(u64)arena,sizeof(arena)) || end>=(1UL<<42)) {
        j616s_screen_puts("USB REFUSED: baseline or private arena/raw-bounds extent");return;
    }
    if(!j616s_usb_plan_build(&plan)) {
        j616s_screen_printf("USB PLAN REFUSED: gate=%u compatible=%s; no controller writes\n",plan.error,plan.compatibility);
        return;
    }
    j616s_screen_printf("USB plan0: PHY=0x%lx pipe=0x%lx DRD=0x%lx DART=0x%lx\n",plan.phy,plan.pipe,plan.drd,plan.dart);
    j616s_screen_printf("USB plan: %u power nodes; SID=%u; private 40MiB arena=0x%lx\n",plan.count,plan.sid,(u64)arena);
    j616s_screen_flush();
    if(call(selftest) || result.status!=1 || (result.esr>>26)!=0x3c || reason!=0x6163) {
        j616s_screen_puts("USB REFUSED: callback exception self-test");return;
    }
    j616s_screen_puts("USB GUARD PASS: C-frame unwind and all checked registers restored");
    reason=0;
    if(call(attempt)) return;
    j616s_screen_printf("USB final: stage=%u rc=%lu fault=%lu reason=0x%x driver-messages=%u\n",stage,result.value,result.status,reason,logs);
    if(result.status) j616s_screen_printf("USB exception: ESR=0x%lx ELR=0x%lx FAR=0x%lx\n",result.esr,result.elr,result.far);
    if(logs) j616s_screen_printf("USB last driver message: %.160s\n",last_log);
    j616s_screen_puts(!result.status && !result.value ?
        "PROXY COMPLETE: session ended and USB halted; confirm transfer/tests in Dell log" :
        "PROXY INCOMPLETE: preserve this screen and Dell log");
    j616s_screen_flush();
}
#endif

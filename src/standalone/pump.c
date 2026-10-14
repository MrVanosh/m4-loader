/* SPDX-License-Identifier: MIT */
#include "pump.h"
static uint64_t counter(void){uint64_t v;__asm__ volatile("mrs %0,CNTPCT_EL0":"=r"(v));return v;}
static uint64_t frequency(void){uint64_t v;__asm__ volatile("mrs %0,CNTFRQ_EL0":"=r"(v));return v;}
int m4_run_linux(uint64_t *ctx,m4_runner run,uint64_t kernel,uint64_t fdt,m4_observer observer,void *opaque) {
    if(!ctx||!run||!observer||((uint64_t)ctx&16383)||!kernel||!fdt||ctx[0]||ctx[8]!=UINT64_MAX)return -1;
    uint64_t hz=frequency();if(hz<1000||hz>10000000000ULL)return -1;
    uint64_t mode=0;
    for(;;) {
        uint64_t ret=run(ctx,16384,mode,kernel,fdt);mode=1;
        if(ctx[0]!=0x4d34434f4f503031ULL||ctx[1]!=1||ctx[2]!=ret||ctx[9]>4096||ctx[22]>2048||ret<1||ret>7||ret==4)return -2;
        /* Native Wi-Fi bypasses HVC. No USB peer exists in standalone mode;
         * unexpected SLIP output is discarded so it cannot stall scheduling. */
        ctx[22]=0;
        int result=observer(ctx,opaque);if(result)return result>0?1:-3;
        if(ret==5)return 0;
        if(ret==2||ret==3||ret==7) {
            uint64_t start=counter();while(counter()-start<hz/10000)__asm__ volatile("nop");
        }
    }
}

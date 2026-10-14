/* Source: archived Gravity src/wdt.c wdt_disable(), exact observed ADT v2.
 * Only writes primary+0x1c and secondary control; never arms/restarts a timer.
 */
typedef unsigned long u64;typedef unsigned int u32;
#ifndef TEST
#define PRIMARY 0x50836c000UL
#define SECONDARY 0x508330008UL
#define BARRIER() __asm__ volatile("dsb sy; isb" ::: "memory")
#else
extern u32 test_primary[16],test_secondary;
#define PRIMARY ((u64)test_primary)
#define SECONDARY ((u64)&test_secondary)
#define BARRIER() do{}while(0)
#endif
__attribute__((section(".text.m4_watchdog")))
u64 m4_watchdog(u64 *o,u64 bytes,u64 primary,u64 secondary,u64 token){
 if(bytes<64 || primary!=PRIMARY || secondary!=SECONDARY || token!=0x6163)return 0;
 volatile u32 *p=(void*)(primary+0x1c),*s=(void*)secondary;
 o[0]=0x574454;o[1]=*p;o[2]=*s;
 if((o[1]!=4 && o[1]!=0) || (o[2]!=0x7d && o[2]!=0))return 1;
 *p=0;BARRIER();o[3]=*p;
 if(o[3])return 2;
 *s=0;BARRIER();o[4]=*s;
 return o[4]?3:0x574454;
}

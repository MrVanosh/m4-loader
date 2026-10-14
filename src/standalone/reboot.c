/* SPDX-License-Identifier: MIT
 * Gravity bootloader950e6b7f src/wdt.c wdt_reboot sequence, on the ADT-checked
 * J616s WD1. WD1 is the normal restart timer; WD0 is not accessed.
 */
typedef unsigned long u64;typedef unsigned int u32;
#ifndef REBOOT_TEST
#define REG(r) ({u64 v;__asm__ volatile("mrs %0, " #r:"=r"(v));v;})
#define READ(a) (*(volatile u32 *)(u64)(a))
#define WRITE(a,v) (*(volatile u32 *)(u64)(a)=(u32)(v))
#define SYNC() __asm__ volatile("dsb sy":::"memory")
#endif
u64 m4_reboot(u64 *o,u64 bytes,u64 base,u64 token,u64 unused){
 (void)unused;
 if(bytes!=64 || ((u64)o&7) || base!=0x50836c000UL || token!=0x4d345245424f4f54UL)return 1;
 if(REG(CurrentEL)!=8 || REG(SCTLR_EL2)&0x1005)return 2;
 if(READ(base+0x1c)!=0)return 3;
 o[0]=0x5245424f;o[1]=base;
 WRITE(base+0x14,0x100000);
 WRITE(base+0x10,0);
 WRITE(base+0x1c,4);
 SYNC();return 0x5245424f;
}

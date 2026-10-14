/* SPDX-License-Identifier: MIT
 * Reuses Gravity/m1n1 spmi.c. Only PMU legacy boot record reads and, with an
 * explicit token, boot_stage=0 (Linux macsmc-reboot's clean shutdown value).
 * Host validates ADT gen=3, physical register, SID and scratchpad base first.
 */
#include "types.h"
#define SPMI_ERR_UNKNOWN 1
#define SPMI_ERR_BUS_IO 2
#define SPMI_ERR_INVALID_PARAM 3
typedef struct m4_pmu_spmi_dev m4_pmu_spmi_dev_t;
static int ignored_printf(const char *fmt, ...){(void)fmt;return 0;}
#define printf ignored_printf
#ifndef PMU_TEST
#define REG(r) ({u64 v;__asm__ volatile("mrs %0, " #r:"=r"(v));v;})
static u32 read32(u64 a){return *(volatile u32 *)a;}
static void write32(u64 a,u32 v){*(volatile u32 *)a=v;__asm__ volatile("dsb sy":::"memory");}
static void udelay(u32 us){u64 end=REG(CNTPCT_EL0)+(REG(CNTFRQ_EL0)/1000000)*us;while(REG(CNTPCT_EL0)<end);}
#endif
#include "upstream-spmi.inc"
__attribute__((section(".text.m4_pmu")))
u64 m4_pmu(u64 *out,u64 bytes,u64 token,u64 clean,u64 reserved){
 if(bytes!=64 || ((u64)out&7) || token!=0x504d55424f4f5452UL || clean>1 || reserved)return 1;
 if(REG(CurrentEL)!=8 || (REG(SCTLR_EL2)&0x1005))return 2;
 struct m4_pmu_spmi_dev dev={.base=0x509014000UL,.regs=&regs_gen1};
 out[0]=0;out[1]=0;out[2]=0;out[3]=0;out[4]=0;out[5]=0;out[6]=0;out[7]=0;
 u8 stage,counts,flags,after;
 int ret=m4_pmu_spmi_ext_read_long(&dev,14,0xf801,&stage,1);
 if(ret){out[1]=1;out[2]=(u64)ret;return 3;}
 ret=m4_pmu_spmi_ext_read_long(&dev,14,0xf802,&counts,1);
 if(ret){out[1]=2;out[2]=(u64)ret;return 3;}
 ret=m4_pmu_spmi_ext_read_long(&dev,14,0xf80f,&flags,1);
 if(ret){out[1]=3;out[2]=(u64)ret;return 3;}
 out[2]=stage;out[3]=counts;out[4]=flags;out[5]=stage;
 if(clean){
  /* Only a known boot stage may be transitioned to clean shutdown. */
  if(stage!=0 && stage!=0x2f && stage!=0x30){out[1]=4;return 4;}
  u8 zero=0;
  ret=m4_pmu_spmi_ext_write_long(&dev,14,0xf801,&zero,1);
  if(ret){out[1]=5;out[6]=(u64)ret;return 5;}
  ret=m4_pmu_spmi_ext_read_long(&dev,14,0xf801,&after,1);
  if(ret || after){out[1]=6;out[6]=(u64)ret;return 6;}
  out[5]=after;out[7]=1;
 }
 out[0]=0x504d55424f4f5452UL;return 0x504d5542;
}

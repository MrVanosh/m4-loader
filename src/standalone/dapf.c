/* SPDX-License-Identifier: MIT
 * Exact dapf_init_t8110a from Gravity bootloader950e6b7f; fixed J616s ADT data.
 * Fresh stopped MTP only. No other DAPF, coprocessor, clock or disk changes. */
typedef unsigned long u64; typedef unsigned int u32; typedef unsigned char u8;
#define PACKED __attribute__((packed))
#ifndef DAPF_TEST
#define REG(r) ({u64 v;__asm__ volatile("mrs %0, " #r:"=r"(v));v;})
#define read32(a) (*(volatile u32 *)(u64)(a))
#define read64(a) (*(volatile u64 *)(u64)(a))
#define write32(a,v) (*(volatile u32 *)(u64)(a)=(u32)(v))
#define write64(a,v) (*(volatile u64 *)(u64)(a)=(u64)(v))
#define SYNC() __asm__ volatile("dsb sy":::"memory")
#endif
struct dapf_t8110_config {
    u64 start;
    u64 end;
    u32 r20;
    u32 unk1;
    u32 r4;
    u32 unk2[5];
    u8 unk3;
    u8 r0_hi;
    u8 r0_lo;
    u8 unk4;
} PACKED;

static struct dapf_t8110_config config[] = {
 {.start=0x50830c000UL,.end=0x50830c06fUL,.r20=1,.r4=0,.r0_hi=3,.r0_lo=1},
 {.start=0x508320880UL,.end=0x508320a27UL,.r20=1,.r4=0,.r0_hi=3,.r0_lo=1},
 {.start=0x5081e40d0UL,.end=0x5081e40d3UL,.r20=1,.r4=0,.r0_hi=3,.r0_lo=1},
 {.start=0x508360100UL,.end=0x50836010bUL,.r20=1,.r4=0,.r0_hi=3,.r0_lo=1},
 {.start=0x508900014UL,.end=0x508900017UL,.r20=1,.r4=0,.r0_hi=3,.r0_lo=1},
 {.start=0x508338020UL,.end=0x508338023UL,.r20=1,.r4=0,.r0_hi=3,.r0_lo=1},
};
static int dapf_init_t8110a(u64 base, struct dapf_t8110_config *config, u32 length)
{
    int count = length / sizeof(*config);

    for (int i = 0; i < count; i++) {
        write32(base + 0x04, config[i].r4);
        write64(base + 0x08, config[i].start);
        write64(base + 0x10, config[i].end);
        write32(base + 0x00, (config[i].r0_hi << 4) | config[i].r0_lo);
        write32(base + 0x20, config[i].r20);
        base += 0x40;
    }
    return 0;
}


__attribute__((section(".text.m4_dapf")))
u64 m4_dapf(u64 *o,u64 bytes,u64 cookie,u64 b,u64 c){
 (void)b;(void)c;
 if(bytes!=512 || ((u64)o&7) || cookie!=0x616444415046UL)return 1;
 if(REG(CurrentEL)!=8 || REG(SCTLR_EL2)&0x1005)return 2;
 if(read32(0x514600044UL)&16 || read32(0x514800200UL)&1 || read32(0x514800100UL))return 3;
 if(!(read32(0x514608110UL)&0x20000) || !(read32(0x514608114UL)&0x20000))return 3;
 o[0]=0x444150464d5450UL;o[1]=6;o[2]=0;o[3]=0;
 for(u32 i=0;i<6;i++){
  u64 a=0x514810000UL+i*64;
  o[4+i*5]=read32(a+4);o[5+i*5]=read64(a+8);o[6+i*5]=read64(a+16);
  o[7+i*5]=read32(a);o[8+i*5]=read32(a+32);
 }
 dapf_init_t8110a(0x514810000UL,config,sizeof(config));SYNC();o[2]=30;
 u64 ok=1;
 for(u32 i=0;i<6;i++){
  u64 a=0x514810000UL+i*64;
  u64 r4=read32(a+4),start=read64(a+8),end=read64(a+16),r0=read32(a),r20=read32(a+32);
  o[34+i*5]=r4;o[35+i*5]=start;o[36+i*5]=end;o[37+i*5]=r0;o[38+i*5]=r20;
  if(r4!=config[i].r4 || start!=config[i].start || end!=config[i].end ||
     (r0&0xff)!=((u32)config[i].r0_hi<<4|config[i].r0_lo) || r20!=config[i].r20)ok=0;
 }
 o[3]=ok;return ok?0x44415046:4;
}

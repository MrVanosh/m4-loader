/* T6041 TZ candidate registers, pinned Yureka reference a4388f6c.
 * Read-only MMIO. The host validates the current ADT apertures before execution.
 * Resident V15 catches a synchronous fault; completed/current fields survive.
 */
typedef unsigned long u64;
typedef unsigned int u32;
#ifndef MCC_TEST
#define SYSREG(r) ({u64 v; __asm__ volatile("mrs %0, " #r : "=r"(v)); v;})
#define READ32(a) (*(volatile u32 *)(a))
#define BARRIER() __asm__ volatile("dsb sy" ::: "memory")
#endif

__attribute__((section(".text.m4_mcc")))
u64 m4_mcc(volatile u64 *o, u64 bytes, u64 cookie, u64 unused1, u64 unused2)
{
    (void)unused1; (void)unused2;
    if (bytes != 1600 || cookie != 0x60416041 || ((u64)o & 7))
        return 1;
    if (SYSREG(CurrentEL) != 8 || (SYSREG(SCTLR_EL2) & 0x1005))
        return 2;
    o[0] = 0x4d4343545a303031UL;
    o[1] = 0; /* completed reads */
    o[2] = 0; /* current address, retained on fault */
    o[3] = 0; /* complete flag */
    o[4] = SYSREG(MPIDR_EL1);
    o[5] = o[6] = o[7] = 0;
    for (u64 mcc = 0; mcc < 4; mcc++) {
        u64 base = 0x220000000UL + mcc * 0x2000000UL;
        for (u64 plane = 0; plane < 4; plane++) {
            for (u64 slot = 0; slot < 4; slot++) {
                for (u64 field = 0; field < 3; field++) {
                    u64 offset = field == 0 ? 0xaf0 : field == 1 ? 0xb00 : 0xb20;
                    u64 index = ((mcc * 4 + plane) * 4 + slot) * 3 + field;
                    u64 addr = base + plane * 0x40000 + slot * 4 + offset;
                    o[2] = addr;
                    BARRIER();
                    o[8 + index] = READ32(addr);
                    o[1] = index + 1;
                }
            }
        }
    }
    BARRIER();
    o[3] = 1;
    return 0x4d4343;
}

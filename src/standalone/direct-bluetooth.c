/* SPDX-License-Identifier: MIT
 * Rev 11: the Bluetooth/Wi-Fi graph is in board.dtb. This file keeps the one part that needs the guard: the read-only
 * PCIe configuration admission of the root port (R15 current-ADT/ECAM flow), then m4_fixup_wireless fills the per-unit
 * data from the ADT (or removes the Bluetooth node when the admission refuses it).
 */
#include "types.h"
#include "utils.h"
#include "string.h"
#include "j616s_usb.h"
#include "direct-bluetooth.h"
#include "bt-boot-admission.h"
#include "fixups.h"
#include "libfdt/libfdt.h"
static volatile u64 bt_admission_address;
static struct m4_bt_boot_report bt_admission_report;
static int bt_admission_result = -100;
static void *tree;
struct bt_control_state {u64 values[12];};
static struct bt_control_state bt_control_state(void) {
    return (struct bt_control_state){{mrs(SCTLR_EL2),mrs(TCR_EL2),mrs(TTBR0_EL2),mrs(MAIR_EL2),
        mrs(VBAR_EL2),mrs(DAIF),mrs(ELR_EL2),mrs(SPSR_EL2),mrs(ESR_EL2),mrs(FAR_EL2),
        mrs(PAR_EL1),mrs(HCR_EL2)}};
}
static int bt_config_read32(void *opaque, uint64_t address, uint32_t *value) {
    (void)opaque;
    if(!value||(address&3)||address<0x1cb0000000UL||address>0x1cb0fffffcUL)return -1;
    bt_admission_address=address;
    __asm__ volatile("dsb sy":::"memory");
    *value=*(volatile const u32 *)(uintptr_t)address;
    __asm__ volatile("dsb sy":::"memory");
    return 0;
}
/* Only this read-only callback is inside the independent nested guard. */
static int prepare_bluetooth(void) {
    bt_admission_result=m4_bt_boot_admit_current(tree,fdt_totalsize(tree),bt_config_read32,NULL,&bt_admission_report);
    return bt_admission_result;
}

int m4_direct_bluetooth_fdt(void *selected, void *workspace, size_t capacity)
{
    (void)workspace;
    if (!selected || capacity != M4_FDT_CAPACITY || fdt_check_full(selected, fdt_totalsize(selected))) return -1;
    tree=selected;
    struct j616s_call_result bt_guard={0};
    struct bt_control_state before=bt_control_state();
    int guarded=j616s_exec_call(prepare_bluetooth,&bt_guard);
    struct bt_control_state after=bt_control_state();
    for(unsigned i=0;i<12;i++)if(before.values[i]!=after.values[i])return 13; /* continuing after changed EL2 state is unsafe */
    int state=M4_BT_STATE_REFUSED;
    if(!guarded&&!bt_guard.status) {
        if(bt_admission_result==M4_BT_BOOT_DEFERRED)state=M4_BT_STATE_DEFERRED; /* bus 0 before Linux enumerates: the graph is kept */
        else if(!bt_admission_result)state=M4_BT_STATE_ADMITTED;
    }
    return m4_fixup_wireless(tree,capacity,state) ? 14 : 0;
}

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
static int bt_state = -1, bt_faulted; /* rev 14: for /chosen/m4,bt-admission */
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
    bt_state=state; bt_faulted=guarded||bt_guard.status;
    return m4_fixup_wireless(tree,capacity,state) ? 14 : 0;
}

/* rev 14: why Bluetooth was admitted or refused, as a /chosen string (claude-loader-linux-build-request.md addendum). */
int m4_direct_bluetooth_publish(void *dt, size_t capacity)
{
    static const char tags[] = "fdt-pcie-shape\0adt-identity\0input\0";  /* -12, -11, -10 */
    static const char cfg[] = "\0\0ecam\0config-read\0root-port\0endpoint-ids\0bt-command\0bt-capability\0bt-status\0changed\0";
    char text[64];
    const char *why = "";
    if (bt_state < 0) snprintf(text, sizeof(text), "not-run");
    else if (bt_state == M4_BT_STATE_ADMITTED) snprintf(text, sizeof(text), "admitted");
    else if (bt_state == M4_BT_STATE_DEFERRED) snprintf(text, sizeof(text), "deferred");
    else if (bt_faulted) snprintf(text, sizeof(text), "refused: fault");
    else {
        int rc = bt_admission_result;
        if (rc <= -10 && rc >= -12) { why = tags; for (int i = -12; i < rc; i++) why += strlen(why) + 1; }
        else if (rc <= -1 && rc >= -9) { why = cfg; for (int i = 0; i < -rc; i++) why += strlen(why) + 1; }
        snprintf(text, sizeof(text), "refused %d: %s", rc, why);
    }
    if (fdt_open_into(dt, dt, (int)capacity)) return -1;
    int c = fdt_path_offset(dt, "/chosen"), ret = c < 0 ? c : fdt_setprop_string(dt, c, "m4,bt-admission", text);
    fdt_pack(dt);
    return ret;
}

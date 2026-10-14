/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J616S_USB_PLAN_H
#define J616S_USB_PLAN_H
#include "types.h"
#define J616S_USB_POWER_MAX 64
struct j616s_usb_plan {
    u64 phy, pipe, drd, dart;
    u32 sid, count;
    u64 power[J616S_USB_POWER_MAX];
    u16 ids[J616S_USB_POWER_MAX];
    unsigned error;
    char compatibility[80];
};
bool j616s_usb_plan_build(struct j616s_usb_plan *p);
#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J616S_USB_H
#define J616S_USB_H
#include "types.h"
#define J616S_USB_ARENA_SIZE (54UL*1024*1024)
struct j616s_call_result { u64 status, value, esr, elr, far; };
int j616s_usb_call(int (*fn)(void), struct j616s_call_result *result);
int j616s_exec_call(int (*fn)(void), struct j616s_call_result *result);
void j616s_usb_abort(unsigned reason) __attribute__((noreturn));
bool j616s_usb_owned(const void *p, size_t len);
void j616s_usb_pool(u64 *base, u64 *bytes);
void j616s_usb_report(u64 lower, u64 upper);
#endif

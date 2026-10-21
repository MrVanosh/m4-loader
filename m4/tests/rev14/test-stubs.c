/* link stubs for direct-bluetooth.c in the host test (never called) */
#include <stddef.h>
struct j616s_call_result;
int j616s_exec_call(int (*f)(void), struct j616s_call_result *r) { (void)f; (void)r; return -1; }
int m4_fixup_wireless(void *t, size_t c, int s) { (void)t; (void)c; (void)s; return 0; }
int m4_bt_boot_admit_current(const void *a, size_t b, void *c, void *d, void *e) { (void)a; (void)b; (void)c; (void)d; (void)e; return -1; }

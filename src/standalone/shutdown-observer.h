/* SPDX-License-Identifier: MIT */
#ifndef M4_SHUTDOWN_OBSERVER_H
#define M4_SHUTDOWN_OBSERVER_H
#include <stddef.h>
struct m4_shutdown_state {
    char line[192], token[33];
    unsigned used, overflow, attempted, clean, reset;
};
typedef int (*m4_shutdown_action)(void *);
int m4_shutdown_feed(struct m4_shutdown_state *,const char *,size_t,
                     m4_shutdown_action,m4_shutdown_action,void *);
#endif

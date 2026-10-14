/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../src/j616s_boot_prefix.h"

int main(void)
{
    _Static_assert(J616S_BOOT_PREFIX_SIZE == 108, "ARM64 common prefix changed");
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    u8 *map = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(map != MAP_FAILED);
    assert(mprotect(map + page, page, PROT_NONE) == 0);
    /* Exactly the shared prefix is readable; any trailing read faults. */
    u8 *raw = map + page - J616S_BOOT_PREFIX_SIZE;
    struct boot_args out;
    for (u16 rev = 1; rev <= 3; rev++) {
        memset(raw, 0xa5, J616S_BOOT_PREFIX_SIZE);
        memcpy(raw, &rev, sizeof(rev));
        memset(&out, 0x5a, sizeof(out));
        assert(j616s_copy_boot_prefix(&out, raw));
        assert(!memcmp(&out, raw, J616S_BOOT_PREFIX_SIZE));
        for (size_t i = J616S_BOOT_PREFIX_SIZE; i < sizeof(out); i++)
            assert(((u8 *)&out)[i] == 0x5a);
    }
    /* Unknown revisions must stop after the four-byte header. */
    raw = map + page - 4;
    const u16 bad[] = {0, 4, 0xffff};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(raw, 0, 4);
        memcpy(raw, &bad[i], 2);
        memset(&out, 0x5a, sizeof(out));
        assert(!j616s_copy_boot_prefix(&out, raw));
        for (size_t j = 4; j < sizeof(out); j++)
            assert(((u8 *)&out)[j] == 0x5a);
    }
    assert(munmap(map, 2 * page) == 0);
    puts("boot prefix: bounded reads at guard page, revisions 1/2/3, unknown-header stop: PASS");
}

/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../src/j616s_firmware_buffer.h"

int main(void)
{
    long page = sysconf(_SC_PAGESIZE);
    assert(page >= 256);
    u8 *map = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(map != MAP_FAILED);
    assert(mprotect(map + page, page, PROT_NONE) == 0);
    u8 *bytes = map + page - 256;
    const char *expected = "mBoot-18000.161.10";
    memset(bytes, 0, 256);
    memcpy(bytes, expected, strlen(expected));
    struct j616s_fw_buffer_result r = j616s_check_firmware_buffer(bytes, 256);
    assert(r.status == J616S_FW_BUFFER_VALID && r.offset == 18);
    /* Every possible nonzero padding position must refuse, including all
     * bytes beyond the 128-byte prefix captured in the hardware photo. */
    for (u32 i = 19; i < 256; i++) {
        bytes[i] = 0xff;
        r = j616s_check_firmware_buffer(bytes, 256);
        assert(r.status == J616S_FW_BUFFER_NONZERO_PADDING && r.offset == i);
        bytes[i] = 0;
    }
    memset(bytes, 'A', 256);
    r = j616s_check_firmware_buffer(bytes, 256);
    assert(r.status == J616S_FW_BUFFER_NO_NUL);
    bytes[128] = 0;
    assert(j616s_check_firmware_buffer(bytes, 256).status == J616S_FW_BUFFER_NO_NUL);
    memset(bytes + 127, 0, 129);
    r = j616s_check_firmware_buffer(bytes, 256);
    assert(r.status == J616S_FW_BUFFER_VALID && r.offset == 127);
    memset(bytes, 0, 256);
    r = j616s_check_firmware_buffer(bytes, 256);
    assert(r.status == J616S_FW_BUFFER_VALID && r.offset == 0);
    /* Length/null refusal must not touch even an inaccessible source. */
    const u32 invalid[] = {0, 1, 128, 255, 257, 0xffffffff};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(j616s_check_firmware_buffer(map + page, invalid[i]).status ==
               J616S_FW_BUFFER_INVALID);
    assert(j616s_check_firmware_buffer(NULL, 256).status == J616S_FW_BUFFER_INVALID);
    assert(munmap(map, 2 * page) == 0);
    puts("firmware buffer: full padding validation and exact guard-page read bounds: PASS");
}

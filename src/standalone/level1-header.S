/* SPDX-License-Identifier: MIT */
/* First bytes of the Level 1 blob (see level1-host.h struct l1_blob_header).
 * Linked at address 0, so symbol values are offsets from the blob start. */
.section .l1header,"a"
.balign 16
.global l1_header
l1_header:
    .long 0x4c314231        /* L1_BLOB_MAGIC */
    .long 6                 /* L1_HOST_ABI */
    .long _l1_size          /* bytes used */
    .long 0
    .long l1_attempt
    .long l1_publish
    .long l1_recover
    .long l1_describe
.section .note.GNU-stack,"",%progbits

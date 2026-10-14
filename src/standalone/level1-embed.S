/* SPDX-License-Identifier: MIT */
/* The Level 1 blob (build/level1.bin), placed by the linker script in .l1. */
.section .l1blob,"a"
.balign 16384
.global m4_l1_blob, m4_l1_blob_end
m4_l1_blob:
.incbin "build/level1.bin"
.balign 16
m4_l1_blob_end:
.section .note.GNU-stack,"",%progbits

/* Rescue set (embedded) and the opaque PCIe callback. The symbol names are the loader's, the content is the N1
 * release's: m4_kernel = rescue Image, m4_initrd = rescue initramfs, m4_template = rescue board.dtb. */
.section .rodata.m4_payload,"a"
.balign 16
.global m4_kernel,m4_kernel_end
m4_kernel:
.incbin "/work/n1/loader/payloads/kernel.bin"
m4_kernel_end:
.balign 16
.global m4_initrd,m4_initrd_end
m4_initrd:
.incbin "/work/n1/loader/payloads/initrd.bin"
m4_initrd_end:
.balign 16
.global m4_template,m4_template_end
m4_template:
.incbin "/work/n1/loader/payloads/template.bin"
m4_template_end:
.balign 16
.global m4_pcie,m4_pcie_end
m4_pcie:
.incbin "/work/n1/loader/payloads/pcie.bin"
m4_pcie_end:
.balign 16
.global m4_rescue_cmdline
m4_rescue_cmdline:
.incbin "/work/n1/loader/payloads/rescue-cmdline.txt"
.byte 0

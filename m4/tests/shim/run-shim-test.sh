#!/bin/bash
# Builds and runs the real shim + the production blob + the real guard under QEMU at EL2, with the blob placed at two offsets.
# usage: run-shim-test.sh BUILT-TREE WORK-DIR ADT.bin TEMPLATE.dtb     (BUILT-TREE = the OUT/source of build.sh: needs build/level1.bin)
# env: QEMU (default qemu-system-aarch64)
set -eu
TREE=$(cd "${1:?usage: run-shim-test.sh BUILT-TREE WORK-DIR ADT.bin TEMPLATE.dtb}" && pwd)
ADT=$(readlink -f "${3:?ADT.bin}"); TPL=$(readlink -f "${4:?TEMPLATE.dtb}")
T=$(dirname "$(readlink -f "$0")")
mkdir -p "${2:?WORK-DIR}"; W=$(cd "$2" && pwd)/shim-test
Q=${QEMU:-qemu-system-aarch64}
CC=aarch64-linux-gnu-
rm -rf "$W"; mkdir -p "$W/build"
rc_all=0
for PAD in 0 49152; do
W=${W%-pad*}-pad$PAD; mkdir -p "$W/build"
echo "===== blob placed $PAD bytes (x16 KiB pages) further into the image"
echo '#define J616S_SCREEN_DIAGNOSTIC 1' > "$W/build/build_cfg.h"
[ -f $TREE/build/build_cfg.h ] && cp $TREE/build/build_cfg.h "$W/build/build_cfg.h"
cat > $W/blobs.S <<ASM
.section .rodata
.balign 16
.global adt_blob, template_blob, template_blob_end, m4_l1_blob, m4_l1_blob_end
adt_blob: .incbin "$ADT"
.balign 16
template_blob: .incbin "$TPL"
template_blob_end:
.skip $PAD
.balign 16384
m4_l1_blob: .incbin "$TREE/build/level1.bin"
.balign 16
m4_l1_blob_end:
.section .note.GNU-stack,"",%progbits
ASM
S=$TREE/src
CF="-O1 -g -ffreestanding -nostdinc -isystem $(${CC}gcc -print-file-name=include) -isystem $TREE/sysinc -mgeneral-regs-only -fno-pic -fno-PIE \
    -fno-stack-protector -mstrict-align -march=armv8.2-a -fvisibility=hidden -Wall -Wno-unused-parameter -fno-common \
    -I$S -I$S/libfdt -I$S/standalone -I$W -DM4_LOADER -DM4_LEVEL1=1"
OBJ=""
for f in standalone/level1-shim.c vsprintf.c string.c libfdt/fdt.c libfdt/fdt_ro.c libfdt/fdt_rw.c libfdt/fdt_wip.c libfdt/fdt_strerror.c \
         libfdt/fdt_addresses.c libfdt/fdt_empty_tree.c libfdt/fdt_sw.c standalone/fdt-check.c; do
  o=$W/$(echo $f | tr / _).o
  ${CC}gcc -c $CF -o $o $S/$f
  OBJ="$OBJ $o"
done
${CC}gcc -c $CF -DFLAVOR_EL2 -o $W/test.o $T/shim-test.c
${CC}gcc -c $CF -o $W/start.o $T/start.S
${CC}gcc -c $CF -o $W/guard.o $S/standalone/level1-guard.S
${CC}gcc -c $CF -o $W/addr.o $S/standalone/level1-addr.c
OBJ="$OBJ $W/addr.o"
${CC}gcc -c $CF -o $W/blobs.o $W/blobs.S
${CC}gcc -c $CF -o $W/adt-port.o $T/adt-port.c
OBJ="$OBJ $W/adt-port.o"
${CC}ld -T $T/link.ld -o $W/shim.elf $W/start.o $W/test.o $W/guard.o $W/blobs.o $OBJ 2>&1 | grep -v "RWX" || true
echo "== image: $(ls -la $W/shim.elf | awk '{print $5}') bytes"
set +e
timeout 120 $Q -machine virt,virtualization=on -cpu max -m 256M -nographic -semihosting -kernel $W/shim.elf > $W/shim.log 2>&1
r=$?
set -e
cat $W/shim.log
echo "== qemu exit $r"
[ $r -eq 0 ] && echo "SHIM_TEST_PASS (pad $PAD)" || { echo "SHIM_TEST_FAIL (pad $PAD)"; rc_all=1; }
done
exit $rc_all

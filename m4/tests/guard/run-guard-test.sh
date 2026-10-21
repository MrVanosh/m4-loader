#!/bin/bash
# Builds and runs the real level1-guard.S under QEMU (both flavors, EL2 and EL1).
# usage: run-guard-test.sh SRC-CHECKOUT WORK-DIR      env: QEMU (default qemu-system-aarch64), CROSS (default aarch64-linux-gnu-)
set -eu
TREE=$(cd "${1:?usage: run-guard-test.sh SRC-CHECKOUT WORK-DIR}" && pwd); SRC=$TREE/src/standalone
T=$(dirname "$(readlink -f "$0")")
mkdir -p "${2:?usage: run-guard-test.sh SRC-CHECKOUT WORK-DIR}"; W=$(cd "$2" && pwd)/guard-test
Q=${QEMU:-qemu-system-aarch64}
CC=${CROSS:-aarch64-linux-gnu-}
rm -rf "$W"; mkdir -p "$W/build"
echo '#define J616S_SCREEN_DIAGNOSTIC 1' > "$W/build/build_cfg.h"
mkdir -p "$W/src"
cp "$TREE/src/j616s_usb_asm.S" "$W/src/"
CF="-O1 -g -ffreestanding -nostdlib -mgeneral-regs-only -fno-pic -fno-stack-protector -mstrict-align -Wall"
rc=0
for fl in EL2 EL1; do
  extra=""; [ $fl = EL1 ] && extra="-DM4_L1G_EL1 -DM4_L1G_NOCHECK"
  ${CC}gcc $CF -DFLAVOR_$fl $extra -I"$W" -T $T/link.ld -o $W/guard-$fl.elf \
     $T/start.S $T/guard-test.c $SRC/level1-guard.S $W/src/j616s_usb_asm.S
  echo "== flavor $fl: $(${CC}size $W/guard-$fl.elf | tail -1)"
  set +e
  timeout 60 $Q -machine virt,virtualization=on -cpu max -m 256M -nographic -semihosting -kernel $W/guard-$fl.elf > $W/guard-$fl.log 2>&1
  r=$?
  set -e
  cat $W/guard-$fl.log
  echo "== qemu exit $r"
  [ $r -eq 0 ] || rc=1
done
[ $rc -eq 0 ] && echo "GUARD_TEST_PASS" || echo "GUARD_TEST_FAIL"
exit $rc

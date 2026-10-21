#!/bin/bash
# Rev 14 host tests: aop-dapf.c on fake MMIO with the real ADT, the AOP segment iommu-addresses and the Bluetooth admission
# fix through the real fix-up chain (rev12-harness), and the /chosen records m4,dapf-*, m4,bt-admission, m4,boot-timing-v1.
#   run-rev14-test.sh TREE ADT-REF.bin BOARD.dtb HARNESS WORK
# Prints `obs KEY VALUE` lines for check.sh. Host compiler (gcc), dtc/fdtput.
set -eu
TREE=$1; ADT=$2; BOARD=$3; H=$4; W=$5; HERE=$(cd "$(dirname "$0")" && pwd); T=$HERE/..
S=$TREE/src/standalone; mkdir -p "$W/obj"
INC="-idirafter $TREE/src -idirafter $TREE/src/libfdt -idirafter $TREE/sysinc -idirafter $S"
# fixtures: the board with an AOP phandle (media-probe writes iommu-addresses only then), and with the second PCIe port
# of the m4.13 kernel DT (six reg entries, bus-range 0..2), which rev 13's Bluetooth check refused (-12)
python3 -I $T/dt/make-pmp-board.py "$BOARD" "$W/t-base.dtb" > /dev/null
N=/pcie@1cb0000000
cp "$W/t-base.dtb" "$W/t-aopph.dtb"; fdtput -t x "$W/t-aopph.dtb" /aop@510600000 phandle 7777
cp "$W/t-aopph.dtb" "$W/t-port1.dtb"
fdtput -t x "$W/t-port1.dtb" $N reg 1c b0000000 0 1000000 4 14000000 0 4000 4 10028000 0 4000 4 17020000 0 4000 4 11028000 0 4000 4 17024000 0 4000
fdtput -t s "$W/t-port1.dtb" $N reg-names config rc port0 phy0 port1 phy1
fdtput -t x "$W/t-port1.dtb" $N bus-range 0 2
for d in t-aopph t-port1; do
  "$H" "$ADT" "$W/$d.dtb" "$W/final-$d.dtb" > "$W/$d.log" 2>&1 || { cat "$W/$d.log"; exit 1; }
  echo "obs rev14.$d.bt-admit $(sed -n 's/.*wireless admit=\([-0-9]*\).*/\1/p' "$W/$d.log" | head -1)"
done
# AOP segments: iommu-addresses = <aop-phandle iova size>; print the IOVA unless it equals the physical address (per firmware)
F=$W/final-t-aopph.dtb
for n in $(fdtget -l "$F" /reserved-memory | grep '^aop-' | LC_ALL=C sort); do
  r=($(fdtget -t x "$F" /reserved-memory/$n reg)); i=($(fdtget -t x "$F" /reserved-memory/$n iommu-addresses 2>/dev/null || echo none))
  if [ "${i[0]}" = none ]; then v=none
  elif [ "${i[0]}" != 7777 ] || [ "${i[3]}:${i[4]}" != "${r[2]}:${r[3]}" ]; then v="bad:${i[*]}"
  elif [ "${i[1]}:${i[2]}" = "${r[0]}:${r[1]}" ]; then v=iova=phys
  else v="iova=0x${i[1]}_${i[2]},size=0x${i[4]}"; fi
  echo "obs rev14.iommu.${n%@*} $v"
done
for f in fdt fdt_ro fdt_rw fdt_wip fdt_empty_tree fdt_strerror fdt_addresses fdt_sw; do gcc -c -w -I$TREE/src -I$TREE/src/libfdt $TREE/src/libfdt/$f.c -o $W/obj/lf-$f.o; done
gcc -c -w $INC $T/tools/adt-double.c -o $W/obj/adt-double.o
gcc -c -w -O1 -DM4_LOADER $INC $S/fdt-check.c -o $W/obj/fdt-check.o
gcc -c -O1 -w -DM4_LOADER -DAOP_DAPF_TEST -include $HERE/test-mmio.h $INC $S/aop-dapf.c -o $W/obj/aop-dapf.o
gcc -c -O1 -w -DM4_LOADER $INC $S/direct-bluetooth.c -o $W/obj/direct-bluetooth.o
gcc -c -O1 -w $HERE/test-stubs.c -o $W/obj/test-stubs.o
gcc -c -O1 -w $INC $HERE/rev14-test.c -o $W/obj/rev14-test.o
gcc $W/obj/*.o -o $W/rev14-test
"$W/rev14-test" "$ADT" "$F" | tee "$W/rev14-test.out"
echo "obs rev14.dapf-test $(tail -1 "$W/rev14-test.out" | sed 's/ /_/g')"
gcc -O1 -w -DM4_LOADER $INC $HERE/timing-test.c $S/boot-timing.c $W/obj/lf-*.o $W/obj/fdt-check.o -o $W/timing-test
"$W/timing-test" "$F" | tee "$W/timing-test.out"
echo "obs rev14.timing-test $(tail -1 "$W/timing-test.out")"

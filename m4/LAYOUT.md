# Loader ↔ kernel layout contract (N1)

This file is owned by lane L. Section **GPU node** was added by lane G2 (`gpu-driver`, 2026-10-14 19:35 UTC); lane L added **Loader side**. Lane L
extends the rest. Where the two disagree, write it in `/reports/n1/loader/URGENT.md` and tell G2.

## GPU node (G2)

The GPU driver (`m4_gpu_driver`, in-tree) receives *everything* about the firmware-owned memory from the device
tree. It no longer reads the loader's arena or any loader header (`owned-layout.h` is not used by the driver).

**Source of truth:** `/reports/n1/gpu-driver/dts/t6040-j616s-gpu.dtsi` (commit `c6a56af34` in the kernel branch
`n1-gpu-driver`; also `arch/arm64/boot/dts/apple/t6040-j616s-gpu.dtsi`). Include it from `t6040-j616s.dts`; it adds
five children to `/reserved-memory` and one node at the root. Test: `dt_guard_test.py --tree <linux> <board.dtb>`
(in `drivers/gpu/drm/asahi/tests/`) must print `PASS`.

### Node

```
gpu: gpu@288000000 {
	compatible = "apple,agx-t6040";
	reg = <0x2 0x8a600000 0x0 0x4000>, <0x2 0x88000000 0x0 0x3758000>;   /* ASC, SGX */
	reg-names = "asc", "sgx";
	memory-region = <&gpu_firmware>, <&gpu_ttbs>, <&gpu_pagetables>, <&gpu_l2>, <&gpu_handoff>;
	memory-region-names = "firmware", "ttbs", "pagetables", "l2", "handoff";
	status = "disabled";           /* the driver attaches the node itself; do not enable it */
	apple,firmware-build = "mBoot-18000.161.10";
	apple,m4-firmware-text = <0x100 0xbe4000 0x0 0x60000>;   /* __TEXT: phys, size (two big-endian u64) */
	apple,sgx-*  /* 53 raw ADT /arm-io/sgx properties */, apple,m4-* /* offline-checked calibration */
};
```
`#address-cells = <2>`, `#size-cells = <2>` at the node's parent (absolute physical addresses).

### Reserved memory (iBoot's GPU regions, announced by the ADT)

| node | `reg` (phys, size) | announced by ADT property | restored on a failed start |
|---|---|---|---|
| `gpu-firmware@10001d44000` | `0x10001d44000`, `0x138000` | `gfx-data-base` / `-size` (the firmware `__DATA` segment) | yes |
| `gpu-ttbs@10bfffb8000` | `0x10bfffb8000`, `0x4000` | `gpu-region-base` / `-size` | yes |
| `gpu-pagetables@10bfff78000` | `0x10bfff78000`, `0x40000` | `gfx-shared-region-base` / `-size` | yes |
| `gpu-l2@10bfff74000` | `0x10bfff74000`, `0x4000` | `gfx-shared-l2-region-base` / `-size` | yes |
| `gpu-handoff@10bfff70000` | `0x10bfff70000`, `0x4000` | `gfx-handoff-base` / `-size` | yes |

All five are `no-map`. **The loader must:**

1. keep these ranges out of System RAM (they sit inside DRAM; `no-map` reserved-memory removes them from memblock;
   the driver additionally refuses a region that intersects System RAM);
2. not place any of its own carve-outs (`aop-*`, `direct-*`, `dcp-map-pool*`, the DT/initrd windows) over them;
3. not touch the GPU: PMGR GPU domains at `0x288e80000+0x100/0x110`, ASC `0x28a600000`, fender gating
   `0x288d06030`; the driver requires the ASC cold (`0x44 == 0`, `0x48 == 0x6a`);
4. emit the values as the ADT has them. The `apple,sgx-*` properties are the ADT `/arm-io/sgx` properties byte for
   byte (property name prefixed `apple,sgx-`; raw little-endian ADT bytes as DT cells). They may be refreshed from
   the live ADT; the driver compares 12 of them (and `apple,firmware-build`, `apple,m4-firmware-text`) with the
   validated values and refuses, naming the property, if one differs. `apple,firmware-build` is the ADT
   firmware-version string (rev 10 produced it the same way; keep that code path).

The `apple,m4-*` calibration and `apple,sgx-*` values in the dtsi are the exact R53 config node
(`config-device-dtb.h`, 7,182 bytes decoded); L's DT equivalence gate should treat the node as unchanged.

### What the driver does with it (for diagnosing)

`m4_native.c` (`m4_gpu_native_start`): find the node → `check_node()` (names the failed check) → for each
`memory-region-names` entry: `of_reserved_mem_region_to_resource_byname`, region ∩ System RAM = ∅, `memremap` WB,
copy a backup → PMGR power domains → cold-ASC check → handoff words → ASC start → build the platform device from
`reg`/`reg-names` → probe → DRM register → pin the module. Refusals before the power step change nothing.

### Not in the contract any more

`M4_CAPTURE_*` offsets, the loader descriptor/handoff page, the CPU peer table, the raw ADT copy: the GPU driver does
not read them. If another consumer still needs the arena (the CPU-release path), that is L/K's, not the GPU's.

## Loader side (L, 2026-10-14 20:05 UTC)

The device tree is the only channel from the loader to the kernel and its modules. Nothing is compiled into a header;
no consumer should read a loader address from anywhere but a `/chosen` or `/reserved-memory` property.

### What the loader writes, and where (rev 11)

| consumer needs | DT form | notes |
|---|---|---|
| System RAM | one `/memory@<kernel-base>` node, `reg` = `<kernel arena-end-kernel>` then every free range | first range is the Image + DT + initrd arena (in System RAM, not reserved); the node is a placeholder in the DTS, adopted at boot |
| loader-owned memory | `/reserved-memory/direct-smp-{vector,code,records,stacks,peers}@<pa>`, `direct-desktop-{capture,host0,host1}@<pa>`; `no-map` | name = base name + `@` + unit address; match with `of_node_name_eq` (base name) or the full name. A node wholly inside host0/host1 carries `status = "disabled"` (of_reserved_mem skips it; the `/memreserve/` entry stays) |
| spin-table | `/cpus/cpu@<affinity>` `cpu-release-addr` = the peer's release word; `enable-method = "spin-table"` | primary `cpu@10100` has none |
| capture descriptor (legacy consumers) | `/chosen/lab,direct-desktop-capture-v2` = `<addr size>`, `m4,direct-peers-v2`, `m4,direct-desktop-contract` | struct unchanged from rev 10 (216-byte descriptor, 64-byte peer records). The GPU driver does **not** use it |
| boot framebuffer | `/chosen/framebuffer@<pa>` (`apple,simple-framebuffer`) | same node rev 10 emitted |
| DCP/display DART mappings | `/reserved-memory/{dcp-map,framebuffer}@<pa>` + `iommu-addresses`, `memory-region(-names)` on `dcp`, `display-subsystem`, `piodma` | from the live DART capture; if the capture is refused the DCP nodes stay `disabled` and no region node exists |
| AOP segments | `/reserved-memory/aop-etext@<pa>`, `aop-oslog@<pa>` | from the ADT `segment-ranges`, page-rounded |
| initrd, cmdline | `/chosen/linux,initrd-{start,end}`, `bootargs` (verbatim `entry.conf` cmdline) | |
| ANS session | `/chosen/m4,ans-live`, `m4,ans-syslog` | nvme-apple adopts it |
| why this boot | `/chosen/m4,boot-{source,entry,reason,detail}`, `m4,loader-{abi,version}` | see `BOOT-ABI.md` |

### GPU (G2's section above)

* The loader does **not** write the GPU node: `t6040-j616s-gpu.dtsi` is part of `board.dtb` (`/include/` from
  `t6040-j616s.dts`), and nothing in the loader reads or refreshes `apple,sgx-*` (an ADT refresh is possible, but
  unproven behaviour ships off: the driver compares the validated values and says which differs).
* The five GPU regions are no-map `reserved-memory` nodes in the DTS. The loader's regions are placed by the same
  rules as before (outside every ADT carveout/memory-map range: `m4_metadata` + `m4_direct_desktop_ownership`).
  The host gate asserts GPU regions ∩ System RAM = ∅ and GPU regions ∩ every other reservation = ∅ on the synthetic
  layout of this Mac's ADT (`gate/*.txt`: "GPU regions (5) disjoint …").
* The loader never touches the GPU: no PMGR GPU domain, no ASC access, no fender register. (`m4_media_*` AOP/AVD
  gates read only PMGR/ADT words and are gone in rev 11 except the ADT gates.)
* `owned-layout.h` (the pinned DESCRIPTOR/HANDOFF/ENTER/ADT offsets of rev 10) is **not** part of the contract any more.
  The loader image is still linked so that those offsets resolve (`tools/loader-layout.py`: `PASS_PAIRED_DIRECT_ELF…`),
  which costs nothing; no consumer may depend on them.

### Declared differences from rev 10 (gate)

`apple,always-on` on `atc2_usb_aon`; GPU node + 5 GPU regions; one multi-range `/memory`; no `m4,*` diagnostic
records; DCP nodes present-but-disabled instead of absent when the display capture is refused; Bluetooth node deleted and
alias removed (iommu-map keeps its SID-2 entry) when the PCIe admission refuses it.

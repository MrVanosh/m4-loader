# Building and checking the loader

Everything here was proved on 2026-10-21 for rev 12 (`7bf9b881…`) and rev 13 (`c8068164…`), byte for byte, from a fresh clone.

## 1. The build container

```
docker build -t m4-loader-build -f m4/docker/Dockerfile m4/docker      # Fedora 44 aarch64: gcc 16.2.1-2, binutils 2.46.1-1, QEMU 10.2.2
```
The compiler decides the bytes. A different gcc/binutils gives a **different but equivalent** image (see "Native builds").

## 2. The inputs (not in git)

| Variable | What |
|---|---|
| `INPUTS` | `Image`, `board.dtb`, `rescue-initramfs.img`, `rescue-cmdline.txt` (or the template with `@ROOT_UUID@` + `ROOT_UUID=`), `pcie.bin`, `librust.a` |
| `PRIVATE` | `private-gates-unit.h`, `private-level1-install.h` (built into the image), `private-adt.bin`, `private-adt-ref.bin` (tests), `private-reference-revNN.txt` (optional) |

## 3. Build and test

```
SRC=. INPUTS=… PRIVATE=… OUT=/tmp/out VERSION=rev13 m4/build.sh    # → /tmp/out/source/build/m1n1.bin, result.json, layout.log
INPUTS=… PRIVATE=… WORK=/tmp/w m4/check.sh                          # build, objects, qemu, e2e, decision, harness, pmp, audit
```
`check.sh` prints PASS/FAIL per step and exits non-zero on any failure. About 15 s. It compares with `m4/reference/reference-rev13.txt`
(`REFERENCE=` selects another). `NATIVE=1` skips the toolchain-bound hashes (m1n1.bin, level1.bin, objects); everything else must still match.

## 4. Native builds (Arch: gcc 16.1, binutils 2.46)

* GCC only. `USE_CLANG=1` fails: the Level 1 blob is compiled with gcc-only flags (`-fno-tree-switch-conversion`, `-fno-tree-loop-distribute-patterns`) that keep jump tables and pointer tables out of a blob that is never relocated.
* Expect **different bytes** (gcc 16.0.1 vs 16.2.1 already changes 34 of 170 objects and `level1.bin`) and **identical**: size 77,578,240 B, all layout offsets, the owned-layout hash `be63e8e8…`, `level1.bin` size 26,196 B, `PASS_PAIRED_DIRECT_ELF_ACTUAL_C_CAPTURE_ABI`, every test. Run `NATIVE=1 m4/check.sh`.
* For an image you install, prefer the container build: it reproduces the reference bytes, and it is what the owner's earlier installs were.

## 5. Adding a revision (rev 14 and later)

```
# 1. commit the new tree (HA's tool: replaces the tree, moves per-unit text out, audits, commits as the owner, tags, rebuilds the tag from a fresh clone)
EXPECTED_SHA256=<m1n1.bin sha256> VERSION=rev14 INPUTS=… PRIVATE=… WORK=… ADT=…/private-adt.bin ROOT_UUID=… \
    m4/tools/add-rev.sh REPO TREE rev14 "2026-10-22T04:10:00+00:00" msg-rev14.txt
# 2. run the whole suite against the OLD reference: expected differences only where the revision changes things
cd REPO && INPUTS=… PRIVATE=… WORK=/tmp/w REF_SRC=<rev13 checkout> REF_BUILD=<rev13 build dir> VERSION=rev14 m4/check.sh
# 3. when the differences are the intended ones, make the new reference
cp /tmp/w/observed.txt m4/reference/reference-rev14.txt; cp /tmp/w/obs/private-harness.txt "$PRIVATE/private-reference-rev14.txt"   # then edit the header comment
git add m4/reference && git commit -m "m4: reference results of rev14"
```

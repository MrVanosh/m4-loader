#!/bin/bash
# Build one loader image (the 77.5 MB m1n1.bin with the embedded rescue set) and run the layout check.
#
#   SRC=/path/to/checkout INPUTS=/path/to/inputs OUT=/path/to/out VERSION=rev13 m4/build.sh
#
# Variables (environment or VAR=value arguments):
#   SRC      the loader source checkout (default: the repo this script lives in)
#   INPUTS   directory holding Image, board.dtb, rescue-initramfs.img, rescue-cmdline.txt, pcie.bin, librust.a
#            (each can be overridden: IMAGE, BOARD_DTB, INITRD, CMDLINE, PCIE, LIBRUST)
#   OUT      output directory (created, WIPED if it exists): payloads/, source/, binding/, result.json,
#            build.log, layout.log; the image is OUT/source/build/m1n1.bin
#   VERSION  the stamp written to /chosen/m4,loader-version (required; rev13 for the reference build)
#   PRIVATE  directory with private-gates-unit.h and private-level1-install.h (the per-unit/per-install headers that are not in git);
#            needed when the tree includes them, copied into the build copy (the checkout is not touched)
#   ROOT_UUID  only if rescue-cmdline.txt contains the placeholder @ROOT_UUID@
#   M1N1_VERSION_TAG  the m1n1 BUILD_TAG (default `unknown`, which is what rev 12/13 contain)
#   M4_JOBS  make -j (default 8; the output does not depend on it)
# Exit 0 only if the build and the layout check (PASS_PAIRED_DIRECT_ELF_ACTUAL_C_CAPTURE_ABI) pass.
set -eu
for kv in "$@"; do case "$kv" in *=*) export "$kv" ;; *) echo "usage: build.sh [VAR=value ...]" >&2; exit 2 ;; esac; done
. "$(dirname "$0")/env.sh"
SRC=${SRC:-$M4_ROOT}; : "${INPUTS:?set INPUTS}" "${OUT:?set OUT}" "${VERSION:?set VERSION}"
IMAGE=${IMAGE:-$INPUTS/Image}; BOARD_DTB=${BOARD_DTB:-$INPUTS/board.dtb}; INITRD=${INITRD:-$INPUTS/rescue-initramfs.img}
CMDLINE=${CMDLINE:-$INPUTS/rescue-cmdline.txt}; PCIE=${PCIE:-$INPUTS/pcie.bin}; LIBRUST=${LIBRUST:-$INPUTS/librust.a}
for f in "$IMAGE" "$BOARD_DTB" "$INITRD" "$CMDLINE" "$PCIE" "$LIBRUST" "$SRC/Makefile"; do [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }; done
export M1N1_VERSION_TAG=${M1N1_VERSION_TAG:-unknown}
if grep -q '@ROOT_UUID@' "$CMDLINE"; then
  : "${ROOT_UUID:?rescue-cmdline.txt has @ROOT_UUID@: set ROOT_UUID to the root filesystem UUID}"
  tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT; sed "s/@ROOT_UUID@/$ROOT_UUID/g" "$CMDLINE" > "$tmp"; CMDLINE=$tmp
fi
python3 -I "$M4_HERE/tools/build-rev11.py" "$OUT" --image "$IMAGE" --initrd "$INITRD" --board-dtb "$BOARD_DTB" \
  --cmdline "$CMDLINE" --version "$VERSION" --src "$SRC" --pcie "$PCIE" --librust "$LIBRUST" ${PRIVATE:+--private "$PRIVATE"} > "$OUT.stdout" 2>&1 \
  || { tail -30 "$OUT.stdout" >&2; echo "BUILD FAILED (see $OUT/build.log)" >&2; exit 1; }
mv "$OUT.stdout" "$OUT/result.stdout"
sha=$(sha256sum "$OUT/source/build/m1n1.bin" | cut -d' ' -f1)
echo "built $OUT/source/build/m1n1.bin $(stat -c %s "$OUT/source/build/m1n1.bin") B sha256 $sha"
tail -1 "$OUT/layout.log"

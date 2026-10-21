#!/bin/bash
# Prove that a tag of the repo rebuilds to the expected image: a fresh clone of the bundle (or repo), the tag exported
# without .git, the two private build inputs and librust.a added, built with m4/build.sh.
#
#   INPUTS=DIR PRIVATE=DIR WORK=DIR m4/tools/verify-repo.sh BUNDLE TAG VERSION EXPECTED_SHA256
#
#   INPUTS   Image, board.dtb, rescue-initramfs.img, rescue-cmdline.txt (or the .template and ROOT_UUID=...), pcie.bin, librust.a
#   PRIVATE  private-gates-unit.h and private-level1-install.h (from the kit's private/; or generate them, see m4/payloads/README)
#   WORK     scratch directory (created; only $WORK/v-* is touched)
# Reference values 2026-10-21: rev11 VERSION=N1-20261014 3352f5e5fe02e2bf6509ddf9d02947b3e004756737a98f1478f7fa7e11e4a469
#                              rev12 VERSION=rev12       7bf9b8816ae2ebf3defe5076a0a3f6d071e65ef554b2029be258781c2b594122
#                              rev13 VERSION=rev13       c8068164653c0a4ce483d6d33c1d7673337e915d988e122abbb2aed0093c577c
set -eu
B=$1; TAG=$2; VERSION=$3; WANT=$4
: "${INPUTS:?}" "${PRIVATE:?}" "${WORK:?}"
V=$WORK/v-$TAG; rm -rf "$V"; mkdir -p "$V"
git clone -q "$B" "$V/repo"                                  # HEAD of the clone supplies m4/build.sh
mkdir "$V/src"; git -C "$V/repo" archive "$TAG" | tar -x -C "$V/src"
mkdir -p "$V/src/build"; cp "$INPUTS/librust.a" "$V/src/build/"
cp "$PRIVATE/private-gates-unit.h" "$V/src/src/standalone/gates-unit.h"
cp "$PRIVATE/private-level1-install.h" "$V/src/src/standalone/level1-install.h"
SRC=$V/src INPUTS=$INPUTS OUT=$V/out VERSION=$VERSION "$V/repo/m4/build.sh"
GOT=$(sha256sum "$V/out/source/build/m1n1.bin" | cut -d' ' -f1)
if [ "$GOT" = "$WANT" ]; then echo "PASS $TAG $GOT"; else echo "FAIL $TAG got $GOT want $WANT"; exit 1; fi

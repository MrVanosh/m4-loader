#!/bin/bash
# sha256 of every object file of a build tree after stripping debug info, .comment and .note* (the integrator's
# object-equivalence method: `objcopy --strip-debug -R .comment -R '.note*'`, then compare bytes).
#   objhash.sh BUILD-DIR      (the OUT/source/build of build.sh)   ->  "sha256  relative/path.o", sorted by path
# The stripped copies do not depend on the build directory path, so two builds of the same source in different
# directories give the same list.
set -eu
. "$(dirname "$0")/env.sh"
b=$(cd "${1:?usage: objhash.sh BUILD-DIR}" && pwd); t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
(cd "$b" && find . -name '*.o' -type f | LC_ALL=C sort) | while read -r f; do
  f=${f#./}; mkdir -p "$t/$(dirname "$f")"
  aarch64-linux-gnu-objcopy --strip-debug -R .comment -R '.note*' "$b/$f" "$t/$f"
  printf '%s  %s\n' "$(sha256sum "$t/$f" | cut -d' ' -f1)" "$f"
done

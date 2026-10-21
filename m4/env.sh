# Sourced by build.sh and check.sh. Sets M4_HERE (this directory), M4_ROOT (the repo checkout) and makes the
# `aarch64-linux-gnu-*` tool names resolve on an aarch64 host that only has plain gcc/ld/objcopy
# (the Dockerfile symlinks them; an Arch host gets a private bin dir in $TMPDIR).
M4_HERE=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE:-$0}")" && pwd -P)
M4_ROOT=$(CDPATH= cd -- "$M4_HERE/.." && pwd -P)
need() { for t in "$@"; do command -v "$t" >/dev/null 2>&1 || { echo "missing tool: $t" >&2; return 1; }; done; }
if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  if [ "$(uname -m)" = aarch64 ]; then
    M4_XBIN=${TMPDIR:-/tmp}/m4-xbin.$(id -u); mkdir -p "$M4_XBIN"
    for t in gcc cc as ld objcopy objdump readelf nm ar size strip; do
      p=$(command -v $t) && ln -sf "$p" "$M4_XBIN/aarch64-linux-gnu-$t"
    done
    PATH=$M4_XBIN:$PATH; export PATH
  else
    echo "no aarch64-linux-gnu-gcc and this is not an aarch64 host: install a cross toolchain" >&2
  fi
fi

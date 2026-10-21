#!/bin/bash
# Compare two builds object by object (stripped, see objhash.sh), the way object-equivalence.txt compares rev 13 with rev 12.
#   objcmp.sh BUILD-DIR-A BUILD-DIR-B     (each an OUT/source/build)
# Prints the number of common objects, the ones that differ (with .text sizes), and the objects only in one build.
# Exit 0 when the two builds have the same objects; 1 otherwise (differences are normal between revisions; read them).
set -eu
here=$(dirname "$0")
a=$(mktemp); b=$(mktemp); trap 'rm -f "$a" "$b"' EXIT
"$here/objhash.sh" "$1" > "$a"; "$here/objhash.sh" "$2" > "$b"
python3 -I - "$a" "$b" "$1" "$2" <<'PY'
import subprocess, sys
def load(p): return {l.split('  ', 1)[1].strip(): l.split('  ', 1)[0] for l in open(p) if l.strip()}
A, B = load(sys.argv[1]), load(sys.argv[2])
common = sorted(set(A) & set(B))
diff = [o for o in common if A[o] != B[o]]
def text(d, o):
    out = subprocess.run(['aarch64-linux-gnu-size', '-A', d + '/' + o], capture_output=True, text=True).stdout
    return sum(int(l.split()[1]) for l in out.splitlines() if l.startswith(('.text', '.rodata', '.data')))
print('%d common objects, %d identical, %d differ' % (len(common), len(common) - len(diff), len(diff)))
for o in diff: print('  differs  %-40s alloc bytes %d -> %d' % (o, text(sys.argv[3], o), text(sys.argv[4], o)))
for o in sorted(set(A) - set(B)): print('  only in A', o)
for o in sorted(set(B) - set(A)): print('  only in B', o)
sys.exit(1 if diff or set(A) ^ set(B) else 0)
PY

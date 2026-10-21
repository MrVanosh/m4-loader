#!/usr/bin/env python3
"""make-pad.py IN.dtb OUT.dtb TARGET_BYTES: IN + a disabled /m4-gate-pad node, sized so OUT is exactly TARGET bytes."""
import subprocess, sys, tempfile, os
src, out, target = sys.argv[1], sys.argv[2], int(sys.argv[3])
dts = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', src], capture_output=True, text=True, check=True).stdout
end = dts.rstrip().rindex('};')
def build(n, k=0):
    node = '\n\tm4-gate-pad {\n\t\tcompatible = "m4,gate-pad";\n\t\tstatus = "disabled";\n\t\tpad%s = [%s];\n\t};\n' % ('x' * k, ' '.join(['5a'] * n))
    with tempfile.NamedTemporaryFile('w', suffix='.dts', delete=False) as f:
        f.write(dts[:end] + node + dts[end:]); p = f.name
    subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', out, p], check=True); os.unlink(p)
    return os.path.getsize(out)
base = build(0)
n = target - base
for _ in range(8):
    got = build(max(n, 0))
    if got <= target and target - got < 4: break
    n += target - got
for k in range(8):
    if build(max(n, 0), k) == target: break
assert os.path.getsize(out) == target, (os.path.getsize(out), target)
print(out, target)

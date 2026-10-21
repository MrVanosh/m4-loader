#!/usr/bin/env python3 -I
"""make-pmp-board.py IN.dtb OUT.dtb: IN + a bare /pmp@500700000 node with compatible = "apple,t6040-pmp-v2" (what the kernel DT carries
for the PMP driver; the node rev 13's m4_pmp_publish() fills). The fixture for pmp-test when the real kernel DT is not at hand."""
import subprocess, sys, tempfile, os
src, out = sys.argv[1], sys.argv[2]
dts = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', src], capture_output=True, text=True, check=True).stdout
end = dts.rstrip().rindex('};')
node = '\n\tpmp@500700000 {\n\t\tcompatible = "apple,t6040-pmp-v2";\n\t\tstatus = "disabled";\n\t};\n'
with tempfile.NamedTemporaryFile('w', suffix='.dts', delete=False) as f:
    f.write(dts[:end] + node + dts[end:]); p = f.name
subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', out, p], check=True); os.unlink(p)
print(out, os.path.getsize(out))

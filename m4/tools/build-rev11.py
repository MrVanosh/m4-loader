#!/usr/bin/env python3 -I
"""Build loader rev 11 (desktop carrier, Boot ABI v1) with an embedded rescue set.
  build-rev11.py --image Image --initrd initramfs.img --board-dtb board.dtb --cmdline rescue-cmdline.txt --src TREE --pcie pcie.bin --librust librust.a [--private DIR] OUT [--version STR]
The rescue set is exactly the N1 release's Image + board.dtb (built from the kernel tree) + lane U's rescue initramfs.
Limits are checked here, not at boot: Image header (ARM\\x64, text_offset 0, 16 KiB pages, memory <= 29,949,952 B window),
initramfs <= 16 MiB (zero-padded to 16 KiB here; the true size and sha256 stay in result.json), board.dtb a complete DTB <= 36 KiB, cmdline <= 1024 B (m4.safe=1 is added when missing)."""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys
from pathlib import Path
ap = argparse.ArgumentParser()
ap.add_argument('out'); ap.add_argument('--image', required=True); ap.add_argument('--initrd', required=True)
ap.add_argument('--board-dtb', required=True); ap.add_argument('--cmdline', required=True)
ap.add_argument('--version', default='rev11'); ap.add_argument('--src', required=True)
# handover: --pcie and --librust replace the hard-coded /work/n1/loader/payloads/pcie.bin and the librust.a inside the source tree
ap.add_argument('--pcie', required=True); ap.add_argument('--librust', required=True)
# per-install / per-unit headers that are not in git (gates-unit.h, level1-install.h): copied into the build copy when the tree includes them and does not carry them
ap.add_argument('--private', default=None)
a = ap.parse_args()
out = Path(a.out).resolve(); L = Path(__file__).resolve().parent
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
img, ird, dtb = Path(a.image).read_bytes(), Path(a.initrd).read_bytes(), Path(a.board_dtb).read_bytes()
KWIN, IWIN, DTBMAX = 29949952, 16 << 20, 36 * 1024
assert img[56:60] == b'ARM\x64', 'Image magic'
text_off, memory, flags = struct.unpack_from('<QQQ', img, 8)
assert text_off == 0 and (flags & 7) == 4 and memory >= len(img) and memory % 4096 == 0, 'Image header'
assert memory <= KWIN, 'Image memory %d > window %d' % (memory, KWIN)
assert len(ird) <= IWIN, 'initramfs %d > 16 MiB' % len(ird)
ird_true = ird
ird = ird + bytes(-len(ird) % 16384)     # the loader image links the slot content on 16 KiB pages (loader-layout.py asserts it)
assert dtb[:4] == b'\xd0\x0d\xfe\xed' and struct.unpack_from('>I', dtb, 4)[0] == len(dtb) and len(dtb) <= DTBMAX, 'board.dtb'
cmd = Path(a.cmdline).read_text().strip()
assert '\n' not in cmd and all(32 <= ord(c) < 127 for c in cmd), 'cmdline must be one printable line'
if 'm4.safe=1' not in cmd.split(): cmd += ' m4.safe=1'
assert len(cmd) <= 1024, 'cmdline'
shutil.rmtree(out, ignore_errors=True); out.mkdir(parents=True)
pay = out / 'payloads'; pay.mkdir()
(pay / 'kernel.bin').write_bytes(img); (pay / 'initrd.bin').write_bytes(ird); (pay / 'template.bin').write_bytes(dtb)
(pay / 'rescue-cmdline.txt').write_text(cmd)
shutil.copy(a.pcie, pay / 'pcie.bin')
src = out / 'source'
subprocess.run(['cp', '-a', '--reflink=auto', a.src, str(src)], check=True)
for priv, dest, user in (('private-gates-unit.h', 'src/standalone/gates-unit.h', 'src/standalone/gates.h'),
                         ('private-level1-install.h', 'src/standalone/level1-install.h', 'src/standalone/level1.h')):
    if (src / dest).exists() or Path(dest).name not in (src / user).read_text(): continue
    if not a.private or not (Path(a.private) / priv).is_file(): sys.exit('%s includes %s, which is not in git: pass --private DIR with %s (see m4/payloads/README)' % (user, Path(dest).name, priv))
    shutil.copy(Path(a.private) / priv, src / dest)
shutil.rmtree(src / '.git', ignore_errors=True)   # version.sh would otherwise stamp `git describe` into the image; without .git it stamps `unknown`, as for rev 12/13
shutil.rmtree(src / 'build' / 'standalone', ignore_errors=True); shutil.rmtree(src / 'build' / 'level1', ignore_errors=True)
for f in (src / 'build').glob('*'):
    shutil.rmtree(f) if f.is_dir() else f.unlink()
(src / 'build').mkdir(exist_ok=True)
shutil.copy(a.librust, src / 'build' / 'librust.a')   # prebuilt (rev 13: 2026-10-08); the make line below never rebuilds it
s = (src / 'src/standalone/payload.S').read_text()
s = re.sub(r'"[^"]*/payloads/', '"' + str(pay) + '/', s)
assert s.count(str(pay)) == 5, 'payload.S paths'
(src / 'src/standalone/payload.S').write_text(s)
(src / 'src/standalone/payload_sizes.h').write_text('#define M4_KERNEL_MEMORY_BYTES %dUL\n#define M4_BUILD_EPOCH 0UL\n' % memory)
r = subprocess.run(['make', '-j' + os.environ.get('M4_JOBS', '8'), 'J616S_SCREEN_DIAGNOSTIC=1', 'M4_DCP_STAGE=3', 'M4_LEVEL1=1', 'M4_LOADER_VERSION=' + a.version,
                    '-o', 'build/librust.a', 'build/m1n1.bin', 'build/m1n1.elf'], cwd=src, capture_output=True, text=True)
(out / 'build.log').write_text(r.stdout + r.stderr)
assert r.returncode == 0, r.stderr[-2000:]
b = subprocess.run(['python3', '-B', str(L / 'loader-layout.py'), str(src), str(out / 'binding')], capture_output=True, text=True)
(out / 'layout.log').write_text(b.stdout + b.stderr); assert b.returncode == 0, b.stdout + b.stderr
lay = json.loads((out / 'binding/loader-layout.json').read_text())
res = dict(version=a.version, m1n1_bin=str(src / 'build/m1n1.bin'), m1n1_bin_sha256=sha(src / 'build/m1n1.bin'),
           bytes=(src / 'build/m1n1.bin').stat().st_size, level1_blob_bytes=(src / 'build/level1.bin').stat().st_size,
           rescue=dict(image_bytes=len(img), image_memory=memory, image_sha256=hashlib.sha256(img).hexdigest(),
                       initramfs_bytes=len(ird_true), initramfs_sha256=hashlib.sha256(ird_true).hexdigest(),
                       initramfs_padded_bytes=len(ird), initramfs_padded_sha256=hashlib.sha256(ird).hexdigest(),
                       board_dtb_bytes=len(dtb), board_dtb_sha256=hashlib.sha256(dtb).hexdigest(), cmdline=cmd),
           owned_layout_sha256=sha(out / 'binding/owned-layout.h'), offsets=lay['offsets'], status=b.stdout.strip().splitlines()[-1])
json.dump(res, open(out / 'result.json', 'w'), indent=1)
print(json.dumps(res, indent=1))

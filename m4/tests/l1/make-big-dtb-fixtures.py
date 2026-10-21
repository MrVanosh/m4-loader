#!/usr/bin/env python3
"""rev 12 fixtures: entries whose board.dtb is above rev 11's 36,864 B window (needs mke2fs and debugfs: e2fsprogs; the root UUID is a dummy, not the owner's).
usage: make-big-dtb-fixtures.py OUTDIR NAME=BOARD.DTB ...   (same layout and feature set as tests/make-abi-fixtures.py)"""
import os, random, struct, subprocess, sys, zlib, shutil, hashlib
E = os.environ.get('E2FSPROGS_SBIN', '')   # '' = from PATH (Homebrew: /opt/homebrew/opt/e2fsprogs/sbin/)
FEAT = ('has_journal,ext_attr,resize_inode,dir_index,filetype,extent,64bit,flex_bg,metadata_csum,'
        'metadata_csum_seed,sparse_super,large_file,huge_file,dir_nlink,extra_isize')
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
def rnd(n, seed): return bytearray(random.Random(seed).randbytes(n))
def kimage(total, memory, seed):
    b = rnd(total, seed); b[0:8] = struct.pack('<II', 0x91005a4d, 0x14000000)
    struct.pack_into('<QQQQ', b, 8, 0, memory, 4, 0); b[56:60] = b'ARM\x64'; return bytes(b)
IMAGE = kimage(1024 * 1024 + 123, 2 * 1024 * 1024, 1)
INITRD = bytes(rnd(512 * 1024 + 7, 2))
crc = lambda b: '%08x' % (zlib.crc32(b) & 0xffffffff)
CMD = 'root=UUID=00000000-0000-4000-8000-00000000e2e0 rw quiet loglevel=3 m4.test=1'
for arg in sys.argv[2:]:
    name, path = arg.split('=', 1); DTB = open(path, 'rb').read()
    L = ['abi=1', 'kver=6.17.0-m4', 'cmdline=' + CMD]
    for fn, data in (('Image', IMAGE), ('initramfs.img', INITRD), ('board.dtb', DTB)):
        L += ['%s_size=%d' % (fn, len(data)), '%s_crc32=%s' % (fn, crc(data)), '%s_sha256=%s' % (fn, hashlib.sha256(data).hexdigest())]
    fs = out + '/' + name + '.ext4'
    if os.path.exists(fs): os.unlink(fs)
    r = subprocess.run([E + 'mke2fs', '-q', '-F', '-t', 'ext4', '-b', '4096', '-I', '256', '-O', FEAT, '-U',
                        '00000000-0000-4000-8000-00000000e2e0', fs, '2048'], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    tmp = out + '/t.' + name; os.makedirs(tmp, exist_ok=True)
    def put(rel, data):
        p = tmp + '/' + rel; open(p, 'wb').write(data); return p
    cmds = ['mkdir boot', 'cd boot', 'mkdir m4', 'write %s /boot/m4/loader.conf' % put('loader.conf', b'abi=1\ndefault=e1\n'),
            'mkdir /boot/m4/entries', 'mkdir /boot/m4/entries/e1',
            'write %s /boot/m4/entries/e1/entry.conf' % put('entry.conf', ('\n'.join(L) + '\n').encode())]
    for fn, data in (('Image', IMAGE), ('initramfs.img', INITRD), ('board.dtb', DTB)):
        cmds.append('write %s /boot/m4/entries/e1/%s' % (put(fn, data), fn))
    open(tmp + '/cmds', 'w').write('\n'.join(cmds) + '\n')
    subprocess.run([E + 'debugfs', '-w', '-f', tmp + '/cmds', fs], capture_output=True, text=True, check=True)
    shutil.rmtree(tmp)
    r = subprocess.run([E + 'e2fsck', '-fn', fs], capture_output=True, text=True)
    print(name, len(DTB), 'e2fsck rc', r.returncode)

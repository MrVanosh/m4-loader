#!/usr/bin/env python3 -I
"""Boot ABI v1 test fixtures (needs mke2fs and debugfs: e2fsprogs; the root UUID is a dummy, not the owner's). One small ext4 image per scenario (the filesystem
carries the native mkfs feature set), plus fx/{Image,initramfs.img,board.dtb}.
usage: make-abi-fixtures.py OUTDIR BOARD.DTB"""
import os, random, struct, subprocess, sys, zlib, shutil
E = os.environ.get('E2FSPROGS_SBIN', '')   # '' = from PATH (Homebrew: /opt/homebrew/opt/e2fsprogs/sbin/)
FEAT = ('has_journal,ext_attr,resize_inode,dir_index,filetype,extent,64bit,flex_bg,metadata_csum,'
        'metadata_csum_seed,sparse_super,large_file,huge_file,dir_nlink,extra_isize')
out, board = sys.argv[1], sys.argv[2]
os.makedirs(out + '/fx', exist_ok=True)
def rnd(n, seed): return bytearray(random.Random(seed).randbytes(n))
def kimage(total, memory, seed, flags=4, text_off=0, magic=b'ARM\x64'):
    b = rnd(total, seed)
    b[0:8] = struct.pack('<II', 0x91005a4d, 0x14000000)
    struct.pack_into('<QQQQ', b, 8, text_off, memory, flags, 0)
    b[56:60] = magic
    return bytes(b)
IMAGE = kimage(1024 * 1024 + 123, 2 * 1024 * 1024, 1)
INITRD = bytes(rnd(512 * 1024 + 7, 2))
DTB = open(board, 'rb').read()
open(out + '/fx/Image', 'wb').write(IMAGE); open(out + '/fx/initramfs.img', 'wb').write(INITRD); open(out + '/fx/board.dtb', 'wb').write(DTB)
crc = lambda b: '%08x' % (zlib.crc32(b) & 0xffffffff)
CMD = 'root=UUID=00000000-0000-4000-8000-00000000e2e0 rw quiet loglevel=3 m4.test=1'
def entry_conf(files, cmd=CMD, abi='1', kver='6.17.0-m4', extra='', drop=()):
    L = []
    if 'abi' not in drop: L.append('abi=%s' % abi)
    if 'kver' not in drop: L.append('kver=%s' % kver)
    if 'cmdline' not in drop: L.append('cmdline=%s' % cmd)
    for name, data, size_off, crc_val in files:
        if name + '_size' not in drop: L.append('%s_size=%d' % (name, len(data) + size_off))
        if name + '_crc32' not in drop: L.append('%s_crc32=%s' % (name, crc_val if crc_val else crc(data)))
        L.append('%s_sha256=%s' % (name, __import__('hashlib').sha256(data).hexdigest()))
    return ('\n'.join(L) + '\n' + extra).encode()
GOOD = [('Image', IMAGE, 0, None), ('initramfs.img', INITRD, 0, None), ('board.dtb', DTB, 0, None)]
def scenario(name, loader='abi=1\ndefault=e1\n', entry=None, files=None, pending=None, state=None, feature=None, entry_name='e1', skip=()):
    fs = out + '/' + name + '.ext4'
    if os.path.exists(fs): os.unlink(fs)
    r = subprocess.run([E + 'mke2fs', '-q', '-F', '-t', 'ext4', '-b', '4096', '-I', '256', '-O', FEAT, '-U',
                        '00000000-0000-4000-8000-00000000e2e0', fs, '2048'], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    tmp = out + '/t.' + name; os.makedirs(tmp, exist_ok=True)
    cmds = ['mkdir boot', 'cd boot', 'mkdir m4']
    def put(rel, data):
        p = tmp + '/' + rel.replace('/', '_'); open(p, 'wb').write(data); return p
    if loader is not None: cmds.append('write %s /boot/m4/loader.conf' % put('loader.conf', loader.encode()))
    if entry is not None or files is not None:
        cmds.append('mkdir /boot/m4/entries'); cmds.append('mkdir /boot/m4/entries/' + entry_name)
        ec = entry if entry is not None else entry_conf(files if files is not None else GOOD)
        cmds.append('write %s /boot/m4/entries/%s/entry.conf' % (put('entry.conf', ec), entry_name))
        for fn, data in (('Image', IMAGE), ('initramfs.img', INITRD), ('board.dtb', DTB)):
            if fn in skip: continue
            d = data
            cmds.append('write %s /boot/m4/entries/%s/%s' % (put(fn, d), entry_name, fn))
    if pending is not None: cmds.append('write %s /boot/m4/boot-pending' % put('boot-pending', pending))
    if state is not None: cmds.append('ssv state %d' % state)
    if feature: cmds.append('feature ' + feature)
    open(tmp + '/cmds', 'w').write('\n'.join(cmds) + '\n')
    r = subprocess.run([E + 'debugfs', '-w', '-f', tmp + '/cmds', fs], capture_output=True, text=True)
    shutil.rmtree(tmp)
    return fs
def custom_files(**kw):
    f = [list(x) for x in GOOD]
    for k, v in kw.items():
        for x in f:
            if x[0] == k: x[3] = v
    return [tuple(x) for x in f]
S = {}
S['ok'] = scenario('ok', entry=None, files=GOOD)
S['pending'] = scenario('pending', files=GOOD, pending=b'e1\n')
S['pending-empty'] = scenario('pending-empty', files=GOOD, pending=b'')
S['dirty-state'] = scenario('dirty-state', files=GOOD, state=0)
S['dirty-recover'] = scenario('dirty-recover', files=GOOD, feature='needs_recovery')
S['noconf'] = scenario('noconf', loader=None, files=GOOD)
S['loader-abi2'] = scenario('loader-abi2', loader='abi=2\ndefault=e1\n', files=GOOD)
S['loader-noabi'] = scenario('loader-noabi', loader='default=e1\n', files=GOOD)
S['loader-baddefault'] = scenario('loader-baddefault', loader='abi=1\ndefault=../e1\n', files=GOOD)
S['loader-dupkey'] = scenario('loader-dupkey', loader='abi=1\ndefault=e1\ndefault=e2\n', files=GOOD)
S['no-entry'] = scenario('no-entry', loader='abi=1\ndefault=nothere\n', files=GOOD)
S['entry-abi2'] = scenario('entry-abi2', entry=entry_conf(GOOD, abi='2'))
S['entry-nocmdline'] = scenario('entry-nocmdline', entry=entry_conf(GOOD, drop=('cmdline',)))
S['entry-longcmdline'] = scenario('entry-longcmdline', entry=entry_conf(GOOD, cmd='x' * 1025))
S['entry-maxcmdline'] = scenario('entry-maxcmdline', entry=entry_conf(GOOD, cmd='a=' + 'y' * 1022))
S['entry-nocrc'] = scenario('entry-nocrc', entry=entry_conf(GOOD, drop=('Image_crc32',)))
S['entry-nosize'] = scenario('entry-nosize', entry=entry_conf(GOOD, drop=('board.dtb_size',)))
S['missing-image'] = scenario('missing-image', files=GOOD, skip=('Image',))
S['size-image'] = scenario('size-image', files=custom_files() and [('Image', IMAGE, 1, None), GOOD[1], GOOD[2]])
S['size-dtb'] = scenario('size-dtb', files=[GOOD[0], GOOD[1], ('board.dtb', DTB, -1, None)])
S['crc-initrd'] = scenario('crc-initrd', files=[GOOD[0], ('initramfs.img', INITRD, 0, '00000000'), GOOD[2]])
S['crc-image'] = scenario('crc-image', files=[('Image', IMAGE, 0, 'deadbeef'), GOOD[1], GOOD[2]])
S['crc-dtb'] = scenario('crc-dtb', files=[GOOD[0], GOOD[1], ('board.dtb', DTB, 0, '12345678')])
for fn, data in (('Image', kimage(1024 * 1024, 2 * 1024 * 1024, 5, magic=b'ARM\x00')),):
    pass
# bad Image header with a matching crc: the entry.conf describes the bad file, the file on disk is the bad file
def scenario_bad_file(name, fname, data):
    fs = scenario(name, files=[(n, d, 0, None) for (n, d) in
                               [('Image', data if fname == 'Image' else IMAGE), ('initramfs.img', INITRD),
                                ('board.dtb', data if fname == 'board.dtb' else DTB)]], skip=(fname,))
    tmp = out + '/t.' + name; os.makedirs(tmp, exist_ok=True)
    open(tmp + '/f', 'wb').write(data)
    subprocess.run([E + 'debugfs', '-w', '-R', 'write %s /boot/m4/entries/e1/%s' % (tmp + '/f', fname), fs], capture_output=True)
    shutil.rmtree(tmp)
    return fs
S['bad-image-magic'] = scenario_bad_file('bad-image-magic', 'Image', kimage(1024 * 1024, 2 * 1024 * 1024, 5, magic=b'ARM\x00'))
S['image-window'] = scenario_bad_file('image-window', 'Image', kimage(1024 * 1024, 40 * 1024 * 1024, 6))
S['image-textoff'] = scenario_bad_file('image-textoff', 'Image', kimage(1024 * 1024, 2 * 1024 * 1024, 7, text_off=0x80000))
S['bad-dtb'] = scenario_bad_file('bad-dtb', 'board.dtb', bytes(rnd(5000, 8)))
S['truncated-dtb'] = scenario_bad_file('truncated-dtb', 'board.dtb', DTB[:len(DTB) - 100])
print('\n'.join(sorted(S)))

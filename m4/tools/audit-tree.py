#!/usr/bin/env python3
"""Check a loader source tree (or any directory) for per-unit and per-install values before it goes into git.

  python3 -I m4/tools/audit-tree.py DIR [--adt ADT.bin] [--install level1-install.h] [--root-uuid UUID ...]

Reports (path, line, what) and exits 1 on any hit; never prints a value:
  * every UUID-shaped string that is not a known public GPT/APFS type GUID or the placeholder @ROOT_UUID@;
  * every --root-uuid given (text, any case);
  * with --install (the private level1-install.h): the partition's first/end block as decimal or hex literals and its
    PARTUUID bytes in C arrays, anywhere in DIR (the file itself is skipped);
  * the all-zero-prefixed test UUIDs `00000000-...` are accepted;
  * with --adt (the flat ADT file, see m4/tools/gates-unit.py): every C byte array of >= 8 bytes that equals, or
    contains, or is contained in, the value (>= 8 bytes, >= 4 distinct byte values) of an ADT property whose name looks
    per-unit: serial, mlb, ecid, unique-chip/device-id, udid, mac-address, bd-addr, calibration, wifi, nonce, seed, module-instance,
    imei, die-id, sku, kblang, antenna, dram-vendor/channel/capacity, hmac, random.
Not a proof of absence: it finds what the ADT of this unit contains. Run it on the ADT of every unit you build for.
"""
import os, re, struct, sys, importlib.util

# public GPT type GUIDs that appear in the tree (level1-core.c: basic data, Linux file system; rust/src/apfs.rs)
ALLOW = {'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7', '0fc63daf-8483-4772-8e79-3d69d8477de4',
         '69646961-6700-11aa-aa11-00306543ecac'}
KEYS = re.compile(r'serial|mlb|ecid|unique-(chip|device)|unique-id|udid|mac-address|local-mac|bd-addr|calib|wifi|nonce|seed|module-instance|'
                  r'imei|die-id|sku|kblang|antenna|dram-vendor|dram-channel|dram-capacity|-hmac|random', re.I)
UUID = re.compile(rb'(?<![0-9a-fA-F-])[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}(?![0-9a-fA-F-])')
ARR = re.compile(rb'\{\s*((?:0[xX][0-9a-fA-F]{1,2}\s*,\s*)+0[xX][0-9a-fA-F]{1,2})\s*,?\s*\}')


def load_adt(path):
    spec = importlib.util.spec_from_file_location('gu', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gates-unit.py'))
    src = open(spec.origin).read().replace('\nmain()\n', '\n')          # reuse parse() without running main
    ns = {}
    exec(compile(src, spec.origin, 'exec'), ns)
    return ns['parse'](open(path, 'rb').read())


def main():
    args = sys.argv[1:]
    root = args[0]
    adtfile = args[args.index('--adt') + 1] if '--adt' in args else None
    uuids = [u.lower().encode() for i, u in enumerate(args) if i and args[i - 1] == '--root-uuid']
    props = []
    if adtfile:
        for p, d in load_adt(adtfile).items():
            for k, v in d.items():
                if KEYS.search(k) and len(v) >= 8 and len(set(v)) >= 4:      # low-entropy values cannot identify a unit
                    props.append((p, k, v))
    nums, guid = [], None
    if '--install' in args:
        inst = open(args[args.index('--install') + 1]).read()
        nums = [int(x) for x in re.findall(r'L1_P4_(?:FIRST|END) (\d+)ull', inst)]
        guid = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{1,2})', re.search(r'L1_P4_GUID_BYTES \{([^}]*)\}', inst).group(1)))
        nums += [n - 1 for n in nums]            # the last block of the range is END - 1
    numre = [re.compile(rb'(?<![0-9a-zA-Z_])(?:%d|0[xX]%x)(?![0-9a-zA-Z_])' % (n, n)) for n in nums]
    bad = 0
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in ('.git', 'build')]
        for f in files:
            p = os.path.join(d, f)
            if os.path.getsize(p) > 3_000_000:
                continue
            b = open(p, 'rb').read()
            low = b.lower()
            rel = os.path.relpath(p, root)
            for m in UUID.finditer(b):
                if m.group(0).decode().lower() not in ALLOW and not m.group(0).startswith(b'00000000-'):
                    bad += 1; print('UUID   %s:%d' % (rel, b.count(b'\n', 0, m.start()) + 1))
            for u in uuids:
                if u in low:
                    bad += 1; print('ROOT   %s' % rel)
            if os.path.basename(p) != 'level1-install.h':
                for rx in numre:
                    mm = rx.search(b)
                    if mm:
                        bad += 1; print('INSTALL %s:%d  partition block number' % (rel, b.count(b'\n', 0, mm.start()) + 1))
            if not props and not guid:
                continue
            for m in ARR.finditer(b):
                arr = bytes(int(t, 16) for t in re.findall(rb'0[xX]([0-9a-fA-F]{1,2})', m.group(1)))
                if guid and guid in arr and os.path.basename(p) != 'level1-install.h':
                    bad += 1; print('INSTALL %s:%d  partition PARTUUID bytes' % (rel, b.count(b'\n', 0, m.start()) + 1))
                if len(arr) < 8 or len(set(arr)) < 2:
                    continue
                for pa, k, v in props:
                    if v == arr or (len(v) >= 8 and v in arr) or (len(arr) >= 16 and len(set(arr)) >= 4 and arr in v):
                        bad += 1; print('ADT    %s:%d  = %s:%s (%d B)' % (rel, b.count(b'\n', 0, m.start()) + 1, pa, k, len(v)))
    print('audit-tree: %d finding(s)' % bad)
    sys.exit(1 if bad else 0)


main()

#!/usr/bin/env python3 -I
"""Builds a binary ADT (Apple device tree) from the ioreg IODeviceTree plist
dumped on this Mac (r39-level1/adt/iodevicetree.plist), for the Level 1
discovery test. Node = {u32 nprops, u32 nchildren, props..., children...};
prop = {char name[32], u32 size, bytes padded to 4}."""
import plistlib, struct, sys
plist, out = sys.argv[1:3]
d = plistlib.load(open(plist, 'rb'))
SKIP_PREFIX = ('IORegistry', 'IOObject', 'IOService', 'IOInterrupt', 'IODevice', 'IOReport', 'IOKit', 'IOConsole',
               'OSKernel', 'OSPrelink', 'OS Build')
def props(n):
    for k, v in n.items():
        if k.startswith(SKIP_PREFIX) or k == 'AAPL,phandle' and False:
            continue
        if isinstance(v, bytes):
            yield k, v
        elif isinstance(v, str):
            yield k, v.encode() + b'\0'
        elif isinstance(v, bool):
            if v: yield k, b''
        elif isinstance(v, int):
            yield k, struct.pack('<I', v & 0xffffffff)
def name_of(n):
    return n.get('name') if isinstance(n.get('name'), bytes) else None
def emit(n):
    p = list(props(n))
    nm = n.get('IORegistryEntryName') or 'device-tree'
    if not any(k == 'name' for k, _ in p):
        p.insert(0, ('name', nm.encode() + b'\0'))
    kids = n.get('IORegistryEntryChildren', [])
    b = struct.pack('<II', len(p), len(kids))
    for k, v in p:
        b += k.encode()[:31].ljust(32, b'\0') + struct.pack('<I', len(v)) + v + b'\0' * (-len(v) % 4)
    for c in kids:
        b += emit(c)
    return b
root = d['IORegistryEntryChildren'][0]   # device-tree
blob = emit(root)
open(out, 'wb').write(blob)
print('adt bytes', len(blob), 'root', root.get('IORegistryEntryName'))

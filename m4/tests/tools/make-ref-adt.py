#!/usr/bin/env python3 -I
"""Reference ADT for the DT harness: the real ioreg IODeviceTree dump of this Mac plus the three things ioreg hides
(AAPL,unit-string on the PCIe bridges, /chosen/memory-map with the DeviceTree entry).
usage: make-ref-adt.py iodevicetree.plist OUT.adt DT_ADDR DT_BYTES"""
import plistlib, struct, sys
plist, out, dt_addr, dt_bytes = sys.argv[1], sys.argv[2], int(sys.argv[3], 0), int(sys.argv[4], 0)
d = plistlib.load(open(plist, 'rb'))
SKIP = ('IORegistry', 'IOObject', 'IOService', 'IOInterrupt', 'IODevice', 'IOReport', 'IOKit', 'IOConsole',
        'OSKernel', 'OSPrelink', 'OS Build')
def props(n):
    for k, v in n.items():
        if k.startswith(SKIP): continue
        if isinstance(v, bytes): yield k, v
        elif isinstance(v, str): yield k, v.encode() + b'\0'
        elif isinstance(v, bool):
            if v: yield k, b''
        elif isinstance(v, int): yield k, struct.pack('<I', v & 0xffffffff)
def find(n, path):
    for part in [p for p in path.split('/') if p]:
        n = next(c for c in n.get('IORegistryEntryChildren', []) if c.get('IORegistryEntryName') == part)
    return n
root = d['IORegistryEntryChildren'][0]
for path, val in (('/arm-io/apcie0/pci-bridge0', b'00000000\0'), ('/arm-io/apcie0/pci-bridge0/wlan', b'00000000\0'),
                  ('/arm-io/apcie0/pci-bridge0/bluetooth-pcie', b'00000001\0'),
                  ('/arm-io/apcie0/pci-bridge1', b'00010000\0'), ('/arm-io/apcie0/pci-bridge1/pcie-sdreader', b'00000000\0')):
    find(root, path)['AAPL,unit-string'] = val
chosen = find(root, '/chosen')
mm = next(c for c in chosen['IORegistryEntryChildren'] if c.get('IORegistryEntryName') in ('memory-map', None, '') and any(k.startswith('AuxKC') for k in c))
mm['name'] = b'memory-map\0'
mm['DeviceTree'] = struct.pack('<QQ', dt_addr, (dt_bytes + 0x3fff) & ~0x3fff)
def emit(n):
    p = list(props(n))
    nm = n.get('IORegistryEntryName') or 'device-tree'
    if not any(k == 'name' for k, _ in p): p.insert(0, ('name', nm.encode() + b'\0'))
    kids = n.get('IORegistryEntryChildren', [])
    b = struct.pack('<II', len(p), len(kids))
    for k, v in p: b += k.encode()[:31].ljust(32, b'\0') + struct.pack('<I', len(v)) + v + b'\0' * (-len(v) % 4)
    for c in kids: b += emit(c)
    return b
blob = emit(root)
open(out, 'wb').write(blob)
print('adt bytes', len(blob))

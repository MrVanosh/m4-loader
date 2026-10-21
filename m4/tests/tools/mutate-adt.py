#!/usr/bin/env python3 -I
"""usage: mutate-adt.py IN OUT PATH PROP  -- flips the first byte of one ADT property (an ADT the gates no longer match)"""
import struct, sys
b = bytearray(open(sys.argv[1], 'rb').read()); want, prop = sys.argv[3], sys.argv[4]
def walk(off, parent):
    np, nc = struct.unpack_from('<II', b, off); o = off + 8; pr = {}
    for _ in range(np):
        k = bytes(b[o:o+32]).split(b'\0')[0].decode(); sz = struct.unpack_from('<I', b, o+32)[0] & 0x7fffffff
        pr[k] = o + 36; o += 36 + ((sz + 3) & ~3)
    nm = bytes(b[pr['name']:pr['name']+32]).split(b'\0')[0].decode()
    path = '/' if parent is None else parent.rstrip('/') + '/' + nm
    if path == want: b[pr[prop]] ^= 0xff
    for _ in range(nc): o = walk(o, path)
    return o
walk(0, None); open(sys.argv[2], 'wb').write(b)

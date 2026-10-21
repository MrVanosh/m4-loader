#!/usr/bin/env python3
"""Write src/standalone/level1-install.h: the partition Level 1 mounts, from the GPT of the disk.

  python3 -I m4/tools/level1-install.py --disk DISK --partuuid 00000000-0000-0000-0000-000000000000 OUT.h   # your root partition's PARTUUID
  python3 -I m4/tools/level1-install.py --disk DISK --index 4 OUT.h          # GPT entry number, 1-based, as on the disk

DISK is the whole internal SSD (`/dev/nvme0n1` on Linux, read only) or an image of its first 1 MiB. The Apple NVMe
reports 4 KiB logical blocks, so the GPT header is at byte 4096 and the entries follow at the header's LBA; a disk
image with 512-byte sectors is also understood (--sector 512). The loader's Level 1 accepts exactly one GPT entry
whose first and last LBA equal the pinned range and whose PARTUUID or type GUID matches (level1-core.c, l1_gpt_check);
there is no search by index and no search by file-system UUID.

Prints first/end and the PARTUUID text (the same facts you would see in `sfdisk -d`); writes nothing but OUT.h.
"""
import argparse, struct, uuid, zlib

ap = argparse.ArgumentParser()
ap.add_argument('--disk', required=True)
ap.add_argument('--partuuid')
ap.add_argument('--index', type=int)
ap.add_argument('--sector', type=int, default=4096)
ap.add_argument('out')
a = ap.parse_args()
if bool(a.partuuid) == bool(a.index):
    ap.error('give exactly one of --partuuid, --index')
with open(a.disk, 'rb') as f:
    f.seek(a.sector)
    hdr = f.read(92)
    assert hdr[:8] == b'EFI PART', 'no GPT header at LBA 1 (try --sector 512)'
    hsize = struct.unpack_from('<I', hdr, 12)[0]
    assert zlib.crc32(hdr[:16] + b'\0\0\0\0' + hdr[20:92]) & 0xffffffff == struct.unpack_from('<I', hdr, 16)[0] or hsize != 92
    elba, num, esz = struct.unpack_from('<QII', hdr, 72)
    f.seek(elba * a.sector)
    ents = f.read(num * esz)
found = []
for i in range(num):
    e = ents[i * esz:(i + 1) * esz]
    if e[:16] == bytes(16):
        continue
    puid = e[16:32]
    first, last = struct.unpack_from('<QQ', e, 32)
    if (a.index == i + 1) or (a.partuuid and puid == uuid.UUID(a.partuuid).bytes_le):
        found.append((i + 1, first, last, puid))
assert len(found) == 1, 'expected exactly one matching GPT entry, found %d' % len(found)
idx, first, last, puid = found[0]
if a.sector != 4096:       # the loader counts 4 KiB units
    k = 4096 // a.sector
    assert first % k == 0 and (last + 1) % k == 0, 'partition is not 4 KiB aligned'
    first, last = first // k, (last + 1) // k - 1
guid = ', '.join('0x%02x' % b for b in puid)
open(a.out, 'w').write(
    '/* PRIVATE per install: the Linux root partition Level 1 mounts (4 KiB units, [first, end)) and its PARTUUID\n'
    ' * (GPT mixed-endian bytes). Generate with m4/tools/level1-install.py. */\n'
    '#define L1_P4_FIRST %dull          /* [first, end) in 4 KiB units */\n#define L1_P4_END %dull\n'
    '#define L1_P4_GUID_BYTES {%s}\n' % (first, last + 1, guid))
print('GPT entry %d: first %d, end %d (%.1f GiB), PARTUUID %s' % (
    idx, first, last + 1, (last + 1 - first) * 4096 / 2**30, uuid.UUID(bytes_le=puid)))

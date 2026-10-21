#!/usr/bin/env python3
"""Bind GPU admission to a paired direct carrier and its compiled C ABI."""
from pathlib import Path
import hashlib, json, struct, subprocess, sys

source, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
build = source/'build'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
elf, binary = build/'m1n1-raw.elf', build/'m1n1.bin'
image = binary.read_bytes()
paired = output/'paired.bin'
subprocess.run(['aarch64-linux-gnu-objcopy','-O','binary',str(elf),str(paired)],check=True)
assert paired.read_bytes() == image, 'raw ELF and carrier differ'
paired.unlink()
symbols = {}
for line in subprocess.check_output(['nm','-n',str(elf)],text=True).splitlines():
    row = line.split()
    if len(row) == 3:
        symbols[row[2]] = int(row[0],16)
assert symbols['_base'] == 0
import re
# R48: the allocator pool size is read from the loader source (54 MiB since rev 10; was the fixed 40 MiB)
pool_bytes = int(re.search(r'J616S_USB_ARENA_SIZE \((\d+)UL\*1024\*1024\)', (source/'src/j616s_usb.h').read_text()).group(1))*1024*1024
pool = []
for line in subprocess.check_output(['nm','-S','-n',str(elf)],text=True).splitlines():
    row=line.split()
    if len(row)==4 and row[3]=='arena' and int(row[1],16)==pool_bytes:
        pool.append((int(row[0],16),int(row[1],16)))
assert len(pool)==1,'actual fixed loader allocator pool'
handoff = ['arena_base','arena_size','kernel','kernel_size','initrd','initrd_size',
           'affinity','framebuffer','framebuffer_size','width','height','stride','depth','epoch']
descriptor = ['magic','version','bytes','phase','loader_base','loader_end','arena_base','arena_size',
              'kernel','kernel_memory','initrd','initrd_bytes','linux_dt','handoff_addr','adt','adt_bytes',
              'bootargs_addr','release_addr','release_page_base','release_page_bytes','primary_mpidr',
              'secondary_mpidr','framebuffer','framebuffer_bytes','peer_count','peer_table','peer_entry_bytes']
values_c = ['sizeof(struct m4_handoff)','sizeof(struct m4_direct_capture_descriptor)',
            'sizeof(struct m4_direct_capture_page)','offsetof(struct m4_direct_capture_page,handoff)',
            'offsetof(struct m4_direct_smp_state,release)']
values_c += ['offsetof(struct m4_handoff,'+n+')' for n in handoff]
values_c += ['offsetof(struct m4_direct_capture_descriptor,'+n+')' for n in descriptor]
abi = output/'capture-abi.c'
abi.write_text('#include "direct-capture.h"\n#include <stddef.h>\n'
    '__attribute__((section(".capture_abi"),used)) const unsigned long long capture_abi[] = {\n'
    + ',\n'.join(values_c)+'\n};\n')
subprocess.run(['aarch64-linux-gnu-gcc','-I'+str(source/'src'),'-I'+str(source/'src/standalone'),
                '-c',str(abi),'-o',str(output/'capture-abi.o')],check=True)
subprocess.run(['aarch64-linux-gnu-objcopy','--dump-section',
                '.capture_abi='+str(output/'capture-abi.bin'),str(output/'capture-abi.o')],check=True)
values = struct.unpack('<'+'Q'*len(values_c),(output/'capture-abi.bin').read_bytes())
assert list(values[:5]) == [96,216,16384,256,64]
assert list(values[5:19]) == [0,8,16,24,32,40,48,56,64,72,76,80,84,88]
assert list(values[19:]) == list(range(0,216,8))
offsets = {name:symbols[symbol] for name,symbol in {
    'ADT':'adt','CUR_BOOT_ARGS':'cur_boot_args','END':'_end','ENTER':'m4_direct_enter',
    'DESCRIPTOR':'m4_direct_capture','HANDOFF':'handoff','RELEASE_PAGE':'m4_direct_smp_states',
    'PEER_TABLE':'m4_direct_smp_peers','STACKS':'secondary_stacks'}.items()}
offsets['RELEASE_FIELD'] = values[4]
assert offsets['HANDOFF'] == offsets['DESCRIPTOR'] + 256
assert offsets['DESCRIPTOR'] % 16384 == offsets['RELEASE_PAGE'] % 16384 == offsets['END'] % 16384 == 0
for name,n in [('DESCRIPTOR',16384),('RELEASE_PAGE',13*16384),('PEER_TABLE',16384),('STACKS',13*65536),('CUR_BOOT_ARGS',108),('ADT',8),('ENTER',48)]:
    assert 0 < offsets[name] < offsets['END']-n
owned=[('DESCRIPTOR',16384),('RELEASE_PAGE',13*16384),('PEER_TABLE',16384),('STACKS',13*65536)]
for n,(name,size) in enumerate(owned):
    assert offsets[name] % 16384 == 0
    assert offsets[name]+size <= pool[0][0] or pool[0][0]+pool[0][1] <= offsets[name]
    for old,old_size in owned[:n]:
        assert offsets[name]+size <= offsets[old] or offsets[old]+old_size <= offsets[name]

kernel = image[symbols['m4_kernel']:symbols['m4_kernel_end']]
kernel_memory, = struct.unpack_from('<Q',kernel,16)
assert kernel[56:60] == b'ARM\x64' and len(kernel) <= kernel_memory <= 0x1DA0000   # R48: the kernel window (was 14 MiB)
initrd_bytes = symbols['m4_initrd_end']-symbols['m4_initrd']
_fdt_h = (source/'src/standalone/fdt.h').read_text()
_window = eval(re.search(r'#define M4_ARENA_BYTES \(([^)]*)\)', _fdt_h).group(1).replace('UL','')) - \
          int(re.search(r'#define M4_ARENA_INITRD_OFFSET (0x[0-9A-Fa-f]+)UL', _fdt_h).group(1), 16)
# R51: the embedded initrd slot is any 16 KiB multiple that fits the loader's initrd window (16 MiB since rev 10); it was fixed at 14 MiB
assert initrd_bytes % 16384 == 0 and 0 < initrd_bytes <= _window, (initrd_bytes, _window)
entry = image[offsets['ENTER']:offsets['ENTER']+48]
assert list(struct.unpack('<12I',entry)) == [
    0xd5034fdf,0xaa0003e9,0xaa0103e0,0xaa1f03e1,0xaa1f03e2,0xaa1f03e3,
    0xaa1f03e4,0xd5033f9f,0xd508751f,0xd5033f9f,0xd5033fdf,0xd61f0120]
lines = ['/* Generated from paired direct raw ELF and actual AArch64 C ABI. */']
for name,value in offsets.items():
    lines.append(f'#define M4_CAPTURE_{name}_OFFSET 0x{value:x}ULL')
fdt_h = (source/'src/standalone/fdt.h').read_text()
arena_bytes = eval(re.search(r'#define M4_ARENA_BYTES \(([^)]*)\)', fdt_h).group(1).replace('UL',''))
initrd_offset = int(re.search(r'#define M4_ARENA_INITRD_OFFSET (0x[0-9A-Fa-f]+)UL', fdt_h).group(1), 16)
lines += [f'#define M4_CAPTURE_KERNEL_MEMORY 0x{kernel_memory:x}ULL',
          f'#define M4_CAPTURE_INITRD_BYTES 0x{initrd_bytes:x}ULL',
          # R48 arena geometry the GPU capture checks (was the literals 0x2000000 and initrd-0x1000000)
          f'#define M4_CAPTURE_ARENA_BYTES 0x{arena_bytes:x}ULL',
          f'#define M4_CAPTURE_INITRD_OFFSET 0x{initrd_offset:x}ULL']
for name,body in [('header',image[:32]),('enter',entry)]:
    lines.append(f'static const unsigned char m4_capture_{name}[{len(body)}] = {{'+
                 ','.join(map(str,body))+'};')
header = output/'owned-layout.h'
header.write_text('\n'.join(lines)+'\n')
report = dict(status='PASS_PAIRED_DIRECT_ELF_ACTUAL_C_CAPTURE_ABI',
    loader_sha256=sha(binary),raw_elf_sha256=sha(elf),header_sha256=sha(header),
    offsets=offsets,handoff_size=values[0],descriptor_size=values[1],capture_page_size=values[2],
    handoff_fields=dict(zip(handoff,values[5:19])),descriptor_fields=dict(zip(descriptor,values[19:])),
    kernel_memory=kernel_memory,guest_kernel_sha256=hashlib.sha256(kernel).hexdigest(),initrd_bytes=initrd_bytes,
    initrd_sha256=hashlib.sha256(image[symbols['m4_initrd']:symbols['m4_initrd_end']]).hexdigest(),
    actual_c_source_sha256={n:sha(source/'src/standalone'/n) for n in ['direct-capture.h','direct-smp.h','loader.c','direct-entry.S','fixups.c']},
    test_pool_offset=pool[0][0],test_pool_bytes=pool[0][1],
    generator_sha256=sha(Path(__file__)),
    scope='Offline immutable layout binding; final fixed-slot initrd join must reproduce owned-layout.h exactly.')
(output/'loader-layout.json').write_text(json.dumps(report,indent=2)+'\n')
# This smaller layout is embedded in the runtime: a whole-carrier or raw-ELF
# hash here would depend on the containing initrd and create a selfhash loop.
runtime=dict(direct_contract='j616s-direct-indexed-desktop-v2',
    header_sha256=report['header_sha256'],offsets=offsets,
    handoff_size=96,descriptor_size=216,capture_page_size=16384,
    handoff_fields=report['handoff_fields'],descriptor_fields=report['descriptor_fields'],
    kernel_memory=kernel_memory,guest_kernel_sha256=report['guest_kernel_sha256'],initrd_bytes=initrd_bytes,
    arena_bytes=arena_bytes,initrd_offset=initrd_offset,
    actual_c_source_sha256=report['actual_c_source_sha256'],
    scope='Immutable paired ABI/header/entry binding; whole carrier identity belongs to the outer receipt.')
(output/'runtime-loader-layout.json').write_text(json.dumps(runtime,indent=2)+'\n')
print(report['status'],report['header_sha256'])

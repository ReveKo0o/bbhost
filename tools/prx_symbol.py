#!/usr/bin/env python3
"""A function's code from one of the dump's own system modules, by NID.

    tools/prx_symbol.py sce_module/libc.prx NID [out.bin]

The dump ships the game's libc and a few other modules as SELFs whose
segments are plain (the form shadPS4 loads). This finds the export whose
name starts with NID (src/core/nid_table.inc has the names), prints its
module address and size, and writes its bytes (at least 256) to out.bin, for
`objdump -D -b binary -m i386:x86-64 out.bin`. What the real function does is
what an HLE must do: `_Assert` (-QgqOT5u2Vk) prints and *returns*, which is
how the host's aborting one was found to be wrong.
"""
import struct
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, want = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else None
    d = open(path, 'rb').read()
    if d[:4] != b'\x4f\x15\x3d\x1d':
        sys.exit('not a SELF')
    nseg = struct.unpack_from('<H', d, 0x18)[0]
    segs = {}
    for i in range(nseg):
        t, o, cs, ds = struct.unpack_from('<QQQQ', d, 32 + i * 32)
        if t & 0x800:  # a program header's data; bits 20.. name which one
            segs[(t >> 20) & 0xfff] = (o, ds)
    elf = 32 + nseg * 32
    phoff = struct.unpack_from('<Q', d, elf + 0x20)[0]
    phnum = struct.unpack_from('<H', d, elf + 0x38)[0]
    dynlib = dyn = text = None
    for i in range(phnum):
        pt, pf, po, pv, pp, pfs, pms, pa = struct.unpack_from('<IIQQQQQQ', d, elf + phoff + i * 56)
        if pt == 0x61000000:
            dynlib = (i, po, pfs)
        elif pt == 2:
            dyn = (i, po, pfs)
        elif pt == 1 and pf & 1 and text is None:
            text = (i, pv)
    di, dpo, dsz = dynlib
    db = d[segs[di][0]:segs[di][0] + segs[di][1]]
    yi, ypo, ysz = dyn
    dynb = db[ypo - dpo:ypo - dpo + ysz] if dpo <= ypo < dpo + dsz else d[segs[yi][0]:segs[yi][0] + ysz]
    tags = {}
    for k in range(0, len(dynb), 16):
        tag, val = struct.unpack_from('<qQ', dynb, k)
        tags.setdefault(tag & 0xffffffffffffffff, val)
    strtab, symtab, symsz = tags[0x61000035], tags[0x61000039], tags[0x6100003f]
    found = False
    for k in range(0, symsz, 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', db, symtab + k)
        name = db[strtab + st_name:strtab + st_name + 64].split(b'\0')[0].decode('latin1')
        if not name.startswith(want) or not st_value:
            continue
        found = True
        print('%s at 0x%x, %d bytes' % (name, st_value, st_size))
        if out:
            at = segs[text[0]][0] + (st_value - text[1])
            open(out, 'wb').write(d[at:at + max(st_size, 256)])
    if not found:
        sys.exit('no export named %s' % want)


if __name__ == '__main__':
    main()

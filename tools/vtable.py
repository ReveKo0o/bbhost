#!/usr/bin/env python3
"""Resolve a relocated vtable in the eboot.

Binary Ninja shows these as **zeros**, because a PS4 ELF fills them from its
relocation table at load: `hexdump 0x5744500` is sixty-four zero bytes and
`get_xrefs_to` finds only the constructor and destructor, which is why a class
whose behaviour lives entirely in its virtuals reads as a dead end. Three
sessions of menu work took vtables to be unreadable for that reason.

The entries are R_X86_64_RELATIVE relocations whose `r_offset` is the slot.
This walks them and prints what each slot will hold at runtime.

    tools/vtable.py 0x5744500 [0x5745ee0 ...]   # Binary Ninja addresses
    tools/vtable.py --slots 24 0x579abe0

Addresses in and out are Binary Ninja's, so they can be pasted straight back.
"""
import argparse
import struct
import sys

BN_BASE = 0x400000
PT_LOAD = 1
PT_DYNAMIC = 2
PT_SCE_DYNLIBDATA = 0x61000000
DT_SCE_RELA = 0x61000033
DT_SCE_RELASZ = 0x61000031
DT_SCE_RELAENT = 0x6100002F
R_X86_64_RELATIVE = 8


def relocations(blob):
    e_phoff = struct.unpack_from("<Q", blob, 0x20)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", blob, 0x36)
    dynlib = dyn = None
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        t, _fl, off, _va, _pa, fsz = struct.unpack_from("<IIQQQQ", blob, o)
        if t == PT_SCE_DYNLIBDATA:
            dynlib = off
        elif t == PT_DYNAMIC:
            dyn = (off, fsz)
    if dynlib is None or dyn is None:
        sys.exit("not a PS4 ELF: no PT_SCE_DYNLIBDATA / PT_DYNAMIC")
    rela = relasz = relaent = 0
    off, size = dyn
    for i in range(off, off + size, 16):
        tag, val = struct.unpack_from("<QQ", blob, i)
        if tag == DT_SCE_RELA:
            rela = val
        elif tag == DT_SCE_RELASZ:
            relasz = val
        elif tag == DT_SCE_RELAENT:
            relaent = val
    # This 1.09 eboot swaps the two: the table's offset is in RELAENT and the
    # entry size in RELA. src/core/elf.cpp identifies it the same way, by
    # magnitude rather than by trusting the tag.
    if rela == 24 and relaent > 24:
        rela, relaent = relaent, rela
    if relaent != 24:
        sys.exit("unexpected relocation entry size %d" % relaent)
    return dynlib + rela, relasz


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addresses", nargs="+", help="vtable addresses, as Binary Ninja shows them")
    ap.add_argument("--eboot", default="eboot-109-decrypted.bin")
    ap.add_argument("--slots", type=int, default=12, help="how many slots to print (default 12)")
    a = ap.parse_args()

    blob = open(a.eboot, "rb").read()
    base, size = relocations(blob)
    for text in a.addresses:
        vt = int(text, 0)
        lo = vt - BN_BASE
        hi = lo + a.slots * 8
        print("vtable 0x%x" % vt)
        found = 0
        for i in range(base, base + size, 24):
            r_off, r_info, r_add = struct.unpack_from("<QQq", blob, i)
            if lo <= r_off < hi and (r_info & 0xFFFFFFFF) == R_X86_64_RELATIVE:
                print("  slot %2d (+0x%02x) -> 0x%x" % ((r_off - lo) // 8, r_off - lo, r_add + BN_BASE))
                found += 1
        if not found:
            print("  no relative relocations in that range - not a vtable, or not that many slots")


if __name__ == "__main__":
    main()

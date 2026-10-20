#!/usr/bin/env python3
"""Build src/nid_table.inc from a decrypted eboot and an aerolib-style NID csv."""

import argparse
import struct
from pathlib import Path


def parse_phdrs(data: bytes):
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)
    dynlib = None
    dynamic = None
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, _flags = struct.unpack_from("<II", data, o)
        p_offset, _va, _pa, p_filesz, _ms, _al = struct.unpack_from("<QQQQQQ", data, o + 8)
        if p_type == 0x61000000:
            dynlib = data[p_offset : p_offset + p_filesz]
        if p_type == 2:
            dynamic = data[p_offset : p_offset + p_filesz]
    return dynlib, dynamic


def dyn_val(dynamic: bytes, tag: int) -> int:
    for i in range(len(dynamic) // 16):
        t, v = struct.unpack_from("<QQ", dynamic, i * 16)
        if t == 0:
            break
        if t == tag:
            return v
    return 0


def full_str(dynlib: bytes, strtab: int, st_name: int) -> str:
    if st_name == 0:
        return ""
    p = strtab + st_name
    if p >= len(dynlib):
        return ""
    while p > strtab and dynlib[p - 1] != 0:
        p -= 1
    raw = dynlib[p:].split(b"\0", 1)[0]
    return raw.decode("ascii", "replace")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("eboot")
    ap.add_argument("aerolib_csv")
    ap.add_argument("-o", "--output", default="src/nid_table.inc")
    args = ap.parse_args()

    data = Path(args.eboot).read_bytes()
    dynlib, dynamic = parse_phdrs(data)
    if not dynlib or not dynamic:
        raise SystemExit("missing PT_SCE_DYNLIBDATA or PT_DYNAMIC")

    strtab = dyn_val(dynamic, 0x6100003D)
    symtab = dyn_val(dynamic, 0x61000039)
    jmprel = dyn_val(dynamic, 0x61000029)
    pltrelsz = dyn_val(dynamic, 0x6100002D)

    aerolib = {}
    for line in Path(args.aerolib_csv).read_text(errors="replace").splitlines():
        parts = line.split()
        if len(parts) >= 2:
            aerolib[parts[0]] = parts[1]

    by_name = {}
    for i in range(pltrelsz // 24):
        _off, info, _add = struct.unpack_from("<QQq", dynlib, jmprel + i * 24)
        sym_i = info >> 32
        st_name = struct.unpack_from("<I", dynlib, symtab + sym_i * 24)[0]
        s = full_str(dynlib, strtab, st_name)
        nid = s.split("#")[0] if s else ""
        name = aerolib.get(nid)
        if name and nid:
            by_name[name] = nid

    lines = [
        "// Generated NID → name table. Contains no game code.",
        "#pragma once",
        "struct NidEntry { const char* nid; const char* name; };",
        "static const NidEntry kNidTable[] = {",
    ]
    for name in sorted(by_name, key=str.lower):
        lines.append(f'    {{"{by_name[name]}", "{name}"}},')
    lines.append("};")
    lines.append(f"static constexpr int kNidTableCount = {len(by_name)};")
    Path(args.output).write_text("\n".join(lines) + "\n")
    print(f"wrote {args.output} ({len(by_name)} names)")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Unpack From's DCX-compressed BND4 bundles and inventory shader binaries.

usage: tools/shaderbnd.py <bundle.dcx> [--extract DIR] [--list]

Prints one line per entry: size, name. With --extract writes each entry
under DIR (path separators from the Windows-style names preserved).
"""
import os
import struct
import sys
import zlib


def dcx_decompress(data: bytes) -> bytes:
    if data[:4] != b"DCX\0":
        return data
    fmt = data[0x28:0x2C]
    i = data.find(b"DCA\0")
    dca_size = struct.unpack(">I", data[i + 4:i + 8])[0]
    comp = data[i + dca_size:]
    if fmt == b"DFLT":
        return zlib.decompress(comp)
    raise SystemExit(f"unsupported DCX format {fmt!r}")


def bnd4_entries(b: bytes):
    if b[:4] != b"BND4":
        raise SystemExit("not a BND4")
    unicode_names = b[0x30] != 0
    n = struct.unpack("<I", b[0xC:0x10])[0]
    hdr = struct.unpack("<Q", b[0x10:0x18])[0]
    esize = struct.unpack("<Q", b[0x20:0x28])[0]
    out = []
    for k in range(n):
        e = b[hdr + k * esize:hdr + (k + 1) * esize]
        flags = struct.unpack("<Q", e[0:8])[0]
        size = struct.unpack("<Q", e[8:16])[0]
        data_off = struct.unpack("<I", e[0x18:0x1C])[0] if esize >= 0x1C else 0
        ident = struct.unpack("<I", e[0x1C:0x20])[0] if esize >= 0x20 else -1
        name_off = struct.unpack("<I", e[esize - 4:esize])[0]
        if unicode_names:
            s = b[name_off:name_off + 2048]
            s = s[:s.find(b"\0\0") + 1].decode("utf-16le", "replace")
        else:
            s = b[name_off:b.find(b"\0", name_off)].decode("ascii", "replace")
        out.append((s, size, data_off, ident, flags))
    return out


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    extract = None
    if "--extract" in args:
        extract = args[args.index("--extract") + 1]
    raw = open(path, "rb").read()
    b = dcx_decompress(raw)
    entries = bnd4_entries(b)
    total = 0
    kinds = {}
    for name, size, off, ident, flags in entries:
        total += size
        ext = os.path.splitext(name)[1].lower() or "(none)"
        kinds[ext] = kinds.get(ext, 0) + 1
        if extract:
            rel = name.replace("\\", "/").lstrip("/")
            if ":" in rel:
                rel = rel.split(":", 1)[1].lstrip("/")
            dest = os.path.join(extract, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(b[off:off + size])
        else:
            print(f"{size:10d} {ident:6d} {name}")
    print(f"# {len(entries)} entries, {total} bytes; by extension: " +
          ", ".join(f"{k}={v}" for k, v in sorted(kinds.items(), key=lambda kv: -kv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())

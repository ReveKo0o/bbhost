#!/usr/bin/env python3
"""Inventory GCN (Sea Islands) instruction formats/opcodes across shader bundles.

usage: tools/gcn_stats.py <bundle.dcx>... [--top N]

Walks each shader's code by encoding format (no semantic decode) and counts
(format, opcode). Literal constants (operand 255) and 64-bit formats advance
the cursor correctly, so a wrong count shows up as a stream that does not end
on s_endpgm.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shaderbnd  # noqa: E402


def fmt_of(w):
    hi = w >> 26
    if (w >> 31) == 0:
        top7 = w >> 25
        if top7 == 0b0111110:
            return "VOPC"
        if top7 == 0b0111111:
            return "VOP1"
        return "VOP2"
    if (w >> 30) == 0b10:
        top9 = w >> 23
        if top9 == 0b101111101:
            return "SOP1"
        if top9 == 0b101111110:
            return "SOPC"
        if top9 == 0b101111111:
            return "SOPP"
        if (w >> 28) == 0b1011:
            return "SOPK"
        return "SOP2"
    if (w >> 27) == 0b11000:
        return "SMRD"
    return {0b110100: "VOP3", 0b110010: "VINTRP", 0b110110: "DS", 0b111000: "MUBUF",
            0b111010: "MTBUF", 0b111100: "MIMG", 0b111110: "EXP"}.get(hi, "UNK")


def op_of(fmt, w):
    return {
        "SOP2": (w >> 23) & 0x7f, "SOPK": (w >> 23) & 0x1f, "SOP1": (w >> 8) & 0xff,
        "SOPC": (w >> 16) & 0x7f, "SOPP": (w >> 16) & 0x7f, "SMRD": (w >> 22) & 0x1f,
        "VOP2": (w >> 25) & 0x3f, "VOP1": (w >> 9) & 0xff, "VOPC": (w >> 17) & 0xff,
        "VOP3": (w >> 17) & 0x1ff, "VINTRP": (w >> 16) & 3, "DS": (w >> 18) & 0xff,
        "MUBUF": (w >> 18) & 0x7f, "MTBUF": (w >> 16) & 7, "MIMG": (w >> 18) & 0x7f, "EXP": 0,
    }.get(fmt, 0)


def has_literal(fmt, w):
    # SOP2/SOPC/SOP1: any source == 255; VOP2/VOPC/VOP1: src0 == 255; SOPK/SOPP: none.
    if fmt in ("SOP2", "SOPC"):
        return (w & 0xff) == 255 or ((w >> 8) & 0xff) == 255
    if fmt == "SOP1":
        return (w & 0xff) == 255
    if fmt in ("VOP2", "VOPC", "VOP1"):
        return (w & 0x1ff) == 255
    return False


def walk(code):
    """Yield (fmt, op) and report whether the stream ended cleanly."""
    words = struct.unpack("<%dI" % (len(code) // 4), code[:len(code) // 4 * 4])
    i = 0
    n = len(words)
    ended = False
    while i < n:
        w = words[i]
        fmt = fmt_of(w)
        op = op_of(fmt, w)
        size = 2 if fmt in ("VOP3", "DS", "MUBUF", "MTBUF", "MIMG", "EXP") else 1
        if has_literal(fmt, w):
            size += 1
        yield fmt, op
        if fmt == "SOPP" and op == 1:  # s_endpgm
            ended = True
        i += size
    return ended


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--") and not a.isdigit()]
    top = 40
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
    hist = {}
    fmts = {}
    shaders = 0
    bad = 0
    for path in args:
        b = shaderbnd.dcx_decompress(open(path, "rb").read())
        for name, size, off, ident, flags in shaderbnd.bnd4_entries(b):
            blob = b[off:off + size]
            try:
                code = gcn_code(blob)
            except SystemExit:
                continue
            shaders += 1
            gen = walk(code)
            try:
                while True:
                    fmt, op = next(gen)
                    hist[(fmt, op)] = hist.get((fmt, op), 0) + 1
                    fmts[fmt] = fmts.get(fmt, 0) + 1
            except StopIteration as stop:
                if not stop.value:
                    bad += 1
    print(f"shaders={shaders} streams-not-ending-in-s_endpgm={bad}")
    print("formats: " + ", ".join(f"{k}={v}" for k, v in sorted(fmts.items(), key=lambda kv: -kv[1])))
    print(f"distinct (format, opcode) pairs: {len(hist)}")
    for (fmt, op), n in sorted(hist.items(), key=lambda kv: -kv[1])[:top]:
        print(f"{n:9d} {fmt:6s} op=0x{op:03x}")
    return 0


def gcn_code(blob):
    i = blob.find(b"Shdr")
    j = blob.find(b"OrbShdr")
    if i < 0 or j < 0 or j <= i + 0x50:
        raise SystemExit("no code")
    return blob[i + 0x50:j]


if __name__ == "__main__":
    sys.exit(main())

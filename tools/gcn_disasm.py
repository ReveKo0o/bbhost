#!/usr/bin/env python3
"""Disassemble the GCN code inside a PS4 shader binary (.vpo/.ppo/.cpo/...).

Sony's container: 0x24-byte file header, "Shdr" header (0x50 bytes), the
GCN code, then an "OrbShdr" footer. PS4 is GFX7 (Sea Islands).

usage: tools/gcn_disasm.py <shader> [--hist]   # --hist prints opcode counts
"""
import re
import subprocess
import sys


def gcn_code(blob: bytes) -> bytes:
    i = blob.find(b"Shdr")
    j = blob.find(b"OrbShdr")
    if i < 0 or j < 0 or j <= i + 0x50:
        raise SystemExit("not a Sony shader binary")
    return blob[i + 0x50:j]


def disassemble(code: bytes) -> str:
    hexbytes = " ".join(f"0x{b:02x}" for b in code)
    r = subprocess.run(
        ["llvm-mc", "--disassemble", "-triple=amdgcn--", "-mcpu=gfx701"],
        input=hexbytes, capture_output=True, text=True)
    return r.stdout


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    code = gcn_code(open(sys.argv[1], "rb").read())
    text = disassemble(code)
    if "--hist" in sys.argv:
        hist = {}
        for line in text.splitlines():
            m = re.match(r"\s*([a-z_0-9]+)", line)
            if m:
                hist[m.group(1)] = hist.get(m.group(1), 0) + 1
        for k, v in sorted(hist.items(), key=lambda kv: -kv[1]):
            print(f"{v:6d} {k}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

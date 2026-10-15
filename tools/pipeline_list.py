#!/usr/bin/env python3
"""Pipeline manifests and lists: what bbhost recorded, and a list made from them.

bbhost records every graphics pipeline a draw creates in
<data>/bbhost/pipeline-manifest.bin (render.cpp, PipelineRecipe) and builds
them all again at the next start, so no pipeline is compiled on first sight in
an area an earlier run has seen. A list is the same records made from one or
more players' or harness runs' manifests - for instance a tour of every area -
which bbhost reads beside its executable (pipeline-list.bin) or from
BBHOST_PIPELINE_LIST, so a first start has them too.

A manifest holds the programs' words (the game's shader code, from the
player's own files). A list holds none: each program is named as the game's
files name it (its footer's hash), with its word count and hash, and bbhost
takes the words from the player's own files at the start - the bundles, or
the eboot for the eboot's own programs (YEBIS's, Scaleform's), with --eboot -
patched as the draws read them, dropping a record whose hash does not match.
Records that need a fetch shader's words (made by the engine at run time, in
no file) are left out of a list; `list` says how many.

  pipeline_list.py stats <file>...
  pipeline_list.py merge --out <manifest> <manifest>...
  pipeline_list.py list --app0 <game folder> [--eboot <eboot>] --out <list> <manifest>...
  pipeline_list.py check --app0 <game folder> [--eboot <eboot>] <list>

Whether a list is shipped with releases is the owner's decision; this tool
only makes one.
"""
import argparse
import os
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shaderbnd  # noqa: E402

MAGIC = 0x4D504242  # "BBPM"
VERSION = 1
VS_REF, PS_REF = 256, 512
HAS_VERTEX_INPUT, RELINK = 4, 8
GX_BLEND, GX_DEPTH, GX_RASTER = 66, 16, 20  # sizeof GxBlendDesc, GxDepthStencilDesc, GxRasterDesc
BUNDLES = ("gxshader.shaderbnd.dcx", "gxrenderershader.shaderbnd.dcx", "gxposteffect.shaderbnd.dcx", "gxgui.shaderbnd.dcx",
           "gxffxshader.shaderbnd.dcx", "gxdecal.shaderbnd.dcx", "gxflvershader.shaderbnd.dcx", "gxflvershader.shaderbnddebug.dcx")


def fnv1a(data, h=1469598103934665603):
    """bbhost's fnv1a (host/gpu.cpp): eight bytes at a time, with a fold."""
    m = (1 << 64) - 1
    n8 = len(data) // 8 * 8
    for (v,) in struct.iter_unpack("<Q", data[:n8]):
        h = ((h ^ v) * 1099511628211) & m
        h ^= h >> 32
    for b in data[n8:]:
        h = ((h ^ b) * 1099511628211) & m
    return h


class Reader:
    def __init__(self, data, at=0, end=None):
        self.d, self.p, self.end = data, at, len(data) if end is None else end

    def take(self, n):
        if self.p + n > self.end:
            raise ValueError("short record")
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def u8(self):
        return self.take(1)[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def words(self):
        n = self.u32()
        return self.take(4 * n)

    def str(self):
        return self.take(self.u32()).decode("latin-1")


def w32(v):
    return struct.pack("<I", v)


def wstr(s):
    b = s.encode("latin-1")
    return w32(len(b)) + b


class Recipe:
    """One record, kept as the bytes around its programs so it is written back as it was."""

    def parse(self, body):
        r = Reader(body)
        self.flags = r.u32()
        self.key = r.u64()
        self.uses = r.u32()
        self.last_run = r.u32()
        state_at = r.p
        r.take(19 * 4 + 8 + (8 + 8 + 32 + 8 + 1) * 4 + GX_BLEND + GX_DEPTH + GX_RASTER)
        self.state = body[state_at:r.p]
        if self.flags & VS_REF:
            self.vs = None
            self.vs_ref = (r.u32(), r.u64())
        else:
            self.vs = r.words()
        self.fetch = r.words()
        if self.flags & PS_REF:
            self.ps = None
            self.ps_ref = (r.u32(), r.u64())
        else:
            self.ps = r.words()
        self.vs_name = r.str()
        self.ps_name = r.str()
        self.tail = body[r.p:]  # dims, modes, vertex input, the relink's specialization
        return self

    def body(self):
        out = [w32(self.flags), struct.pack("<Q", self.key), w32(self.uses), w32(self.last_run), self.state]
        if self.flags & VS_REF:
            out.append(w32(self.vs_ref[0]) + struct.pack("<Q", self.vs_ref[1]))
        else:
            out.append(w32(len(self.vs) // 4) + self.vs)
        out.append(w32(len(self.fetch) // 4) + self.fetch)
        if self.flags & PS_REF:
            out.append(w32(self.ps_ref[0]) + struct.pack("<Q", self.ps_ref[1]))
        else:
            out.append(w32(len(self.ps) // 4) + self.ps)
        out += [wstr(self.vs_name), wstr(self.ps_name), self.tail]
        return b"".join(out)


def read_file(path):
    d = Path(path).read_bytes()
    r = Reader(d)
    if r.u32() != MAGIC or r.u32() != VERSION:
        raise ValueError("%s: not a pipeline manifest this tool reads (version %d)" % (path, VERSION))
    run = r.u32()
    origin = {"translator": r.str(), "vendor": r.u32(), "device": r.u32(), "driver": r.u32(), "uuid": r.take(16).hex(),
              "libraries": bool(r.u8())}
    recipes = []
    for _ in range(r.u32()):
        n = r.u32()
        recipes.append(Recipe().parse(r.take(n)))
    return run, origin, recipes


def write_file(path, run, origin, recipes):
    head = [w32(MAGIC), w32(VERSION), w32(run), wstr(origin["translator"]), w32(origin["vendor"]), w32(origin["device"]),
            w32(origin["driver"]), bytes.fromhex(origin["uuid"]), bytes([1 if origin["libraries"] else 0]), w32(len(recipes))]
    body = b"".join(w32(len(b)) + b for b in (rc.body() for rc in recipes))
    tmp = str(path) + ".tmp"
    Path(tmp).write_bytes(b"".join(head) + body)
    os.replace(tmp, path)


def bundle_programs(app0):
    """name -> code words (bytes) of every shader in the game's bundles, as bbhost's index names them."""
    out = {}
    for b in BUNDLES:
        path = Path(app0) / "dvdroot_ps4" / "shader" / b
        if not path.is_file():
            continue
        plain = shaderbnd.dcx_decompress(path.read_bytes())
        for name, size, off, ident, flags in shaderbnd.bnd4_entries(plain):
            c = plain[off:off + size]
            foot = c.rfind(b"OrbShdr")
            if foot < 0 or foot + 20 > len(c):
                continue
            h0 = struct.unpack_from("<I", c, foot + 16)[0]
            key = "%08x" % h0
            if key in out:
                continue
            # The code: the container's ShaderFileHeader gives the code's offset
            # (gcn::shader_code): 'Shdr' at s, header size in dwords at s + 9,
            # the code right after it; its length in the footer's word at +8.
            s = c.find(b"Shdr")
            first = c.find(b"OrbShdr", s) if s >= 0 else -1
            if s < 0 or s + 16 > len(c) or first < 0 or first + 12 > len(c):
                continue
            length = struct.unpack_from("<I", c, first + 8)[0] >> 8
            code_at = s + 16 + c[s + 9] * 4
            if code_at + length > first or length == 0 or length % 4:
                continue
            out[key] = c[code_at:code_at + length]
    return out


def eboot_programs(eboot):
    """name -> code words of the shader containers the eboot carries itself
    (host/shader_bundles.cpp eboot_shaders: a 'Shdr' header of a vertex, pixel
    or compute shader, the first 'OrbShdr' footer after it)."""
    out = {}
    if not eboot or not Path(eboot).is_file():
        return out
    d = Path(eboot).read_bytes()
    at = 0
    while True:
        s = d.find(b"Shdr", at)
        if s < 0:
            break
        at = s + 1
        if s >= 3 and d[s - 3:s] == b"Orb":
            continue
        if s + 0x60 > len(d) or (d[s + 8] & 0xF) not in (1, 2, 4):
            continue
        foot = d.find(b"OrbShdr", s + 16, s + (1 << 20))
        if foot < 0 or foot + 28 > len(d):
            continue
        name = "%08x" % struct.unpack_from("<I", d, foot + 16)[0]
        if name in out:
            continue
        length = struct.unpack_from("<I", d, foot + 8)[0] >> 8
        code_at = s + 16 + d[s + 9] * 4
        if code_at + length > foot or length == 0 or length % 4:
            continue
        out[name] = d[code_at:code_at + length]
    return out


def describe(path, run, origin, recipes):
    names = Counter()
    for rc in recipes:
        names[rc.vs_name] += 1
        names[rc.ps_name] += 1
    words = sum(len(rc.vs or b"") + len(rc.ps or b"") + len(rc.fetch) for rc in recipes)
    print("%s: run %d, %d pipelines (%d with an optimized relink's specialization, %d on vertex input), %d programs by name; "
          "%.1f KiB of program words; made by translator %s on device %04x:%04x driver %08x%s" % (
              path, run, len(recipes), sum(1 for rc in recipes if rc.flags & RELINK),
              sum(1 for rc in recipes if rc.flags & HAS_VERTEX_INPUT), len(names), words / 1024.0, origin["translator"][:12],
              origin["vendor"], origin["device"], origin["driver"], ", with pipeline libraries" if origin["libraries"] else ", whole pipelines"))
    used = Counter(rc.uses for rc in recipes)
    print("  starts that used each: %s" % ", ".join("%d: %d" % (k, used[k]) for k in sorted(used)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("stats")
    p.add_argument("files", nargs="+")
    p = sub.add_parser("merge")
    p.add_argument("--out", required=True)
    p.add_argument("files", nargs="+")
    p = sub.add_parser("list")
    p.add_argument("--app0", required=True)
    p.add_argument("--eboot", help="the 1.09 eboot: its own programs are named too (else left out)")
    p.add_argument("--out", required=True)
    p.add_argument("files", nargs="+")
    p = sub.add_parser("check")
    p.add_argument("--app0", required=True)
    p.add_argument("--eboot")
    p.add_argument("file")
    args = ap.parse_args()

    if args.cmd == "stats":
        for f in args.files:
            describe(f, *read_file(f))
        return 0

    if args.cmd in ("merge", "list"):
        merged, origin, run = {}, None, 0
        for f in args.files:
            r, o, recipes = read_file(f)
            origin = origin or o
            run = max(run, r)
            for rc in recipes:
                have = merged.get(rc.key)
                if have is None or rc.last_run > have.last_run:
                    merged[rc.key] = rc
        recipes = list(merged.values())
        if args.cmd == "merge":
            write_file(args.out, run, origin, recipes)
            describe(args.out, *read_file(args.out))
            return 0
        progs = bundle_programs(args.app0)
        if not progs:
            print("pipeline_list: no shader bundles under %s" % args.app0, file=sys.stderr)
            return 2
        for name, code in eboot_programs(args.eboot).items():
            progs.setdefault(name, code)
        out, left = [], Counter()
        plain = patched = 0
        for rc in recipes:
            if rc.vs_name not in progs or (rc.ps is not None and rc.ps and rc.ps_name not in progs):
                left["a program neither the bundles nor the eboot hold" if args.eboot else
                     "a program the bundles do not hold (the eboot's own: give --eboot)"] += 1
                continue
            if rc.fetch and not rc.flags & HAS_VERTEX_INPUT:
                left["a fetch shader's words (made at run time)"] += 1
                continue
            for prog, name in ((rc.vs, rc.vs_name), (rc.ps, rc.ps_name)):
                if prog:
                    if progs[name] == prog:
                        plain += 1
                    else:
                        patched += 1  # bbhost patched it (host/shader_patch.cpp): the hash is the patched words'
            rc.flags |= VS_REF
            rc.vs_ref = (len(rc.vs) // 4, fnv1a(rc.vs))
            rc.vs = None
            if rc.ps:
                rc.flags |= PS_REF
                rc.ps_ref = (len(rc.ps) // 4, fnv1a(rc.ps))
                rc.ps = None
            rc.fetch = b""  # unused on vertex input (vs_translate_options)
            rc.uses = rc.last_run = 0
            out.append(rc)
        write_file(args.out, 0, origin, out)
        size = Path(args.out).stat().st_size
        print("%s: %d of %d pipelines (%.1f KiB, no program words: %d programs as the game's files have them, %d as bbhost patches them)" % (
            args.out, len(out), len(recipes), size / 1024.0, plain, patched))
        for why, n in left.most_common():
            print("  left out: %d with %s" % (n, why))
        return 0

    run, origin, recipes = read_file(args.file)
    progs = bundle_programs(args.app0)
    for name, code in eboot_programs(args.eboot).items():
        progs.setdefault(name, code)
    ok = bad = patched = 0
    for rc in recipes:
        for flag, ref, name in ((VS_REF, getattr(rc, "vs_ref", None), rc.vs_name), (PS_REF, getattr(rc, "ps_ref", None), rc.ps_name)):
            if not rc.flags & flag:
                continue
            code = progs.get(name)
            if code is None:
                bad += 1
            elif len(code) // 4 == ref[0] and fnv1a(code) == ref[1]:
                ok += 1
            else:
                patched += 1
    print("%s: %d pipelines; program references: %d match the game's files, %d differ (bbhost's shader patches, checked at its start), "
          "%d not in these files" % (args.file, len(recipes), ok, patched, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

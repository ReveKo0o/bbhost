#!/usr/bin/env python3
"""Read and edit Bloodborne's draw parameters (GPARAM), through the overlay.

`param/drawparam/*.gparam.dcx` is where this engine keeps post-processing:
SSAO, depth of field, motion blur, bloom, light shafts, colour grading,
anti-alias, tone mapping. That is what DS3 exposes as CSGraphicsConfig, and
Bloodborne has the same knobs - they are data rather than a settings class,
which is why the port can reach them without a single guest patch.

Two routes were eliminated first:
the named YEBIS flags reachable from the debug-menu builder are a mirror the
renderer does not read, and `Yebis2ParamFile.filterparam` is never requested
at all.

Format (version 3, magic "filt" in UTF-16):

  0x08 int   version (3)
  0x10 int   group count
  0x18 int   -> group offset table (one int per group, relative to the next)
  0x1c int   -> group headers: {int paramCount, int unk, wchar editor[], wchar name[]}
  0x24 int   -> parameter definitions
  0x28 int   -> **parameter values**

A parameter definition is

  [u32 value offset][u32 unk offset][u32 type][wchar editorName][wchar name]

padded to a 4-byte boundary before the next one.

and its first value is the int32 at <values base> + <value offset>. Confirmed
against five defaults the eboot's own constructor writes - LensDistortion 1,
MotionBlur-Dof 1, Gaussian 0, FeedBackEffect 1, LightShaft 1 - so a base that
merely parses cannot pass for the right one.

**A parameter can hold several values.** The type's top byte is how many
(one per value id of its group), its low byte the kind: 3 and 5 are ints, 9 a
float, 12-14 vectors wider than four bytes, which this does not edit. An
area's Tone Map group has two values, `Yebis-ToneMapExposure` is type 0x209,
and the game's draw-parameter bank takes the second (read back from the live
entry): an edit of the first alone reached nothing. So `--set` writes every
value of every parameter with the name - five names occur twice in a file.
A count of 0 is real (a lighting variant's group with no value ids), and then
there is nothing to write: the bytes at its offset belong to the next one.

  tools/gparam.py --list "$DUMP/dvdroot_ps4/param/drawparam/default.gparam.dcx"
  tools/gparam.py --set "Yebis-Enable MotionBlur-Dof=0" \\
      "$DUMP" data/mods

With a dump root and an overlay root it rewrites **every** .gparam.dcx it
finds: an effect that only goes off in one area is not a setting. The dump is
never written.
"""
import argparse
import glob
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------- DCX


def dcx_unpack(data):
    if data[:4] != b"DCX\0":
        return data, None
    fmt = data[0x28:0x2C]
    if fmt != b"DFLT":
        raise SystemExit("unsupported DCX format %r" % fmt)
    i = data.find(b"DCA\0")
    dca_size = struct.unpack(">I", data[i + 4:i + 8])[0]
    return zlib.decompress(data[i + dca_size:]), data[:i + dca_size]


def dcx_pack(header, payload):
    """The original header with the two sizes in its DCS block patched."""
    comp = zlib.compress(payload, 9)
    out = bytearray(header)
    dcs = out.find(b"DCS\0")
    struct.pack_into(">I", out, dcs + 4, len(payload))
    struct.pack_into(">I", out, dcs + 8, len(comp))
    return bytes(out) + comp


# ---------------------------------------------------------------- BND4


class Bnd4:
    def __init__(self, b):
        if b[:4] != b"BND4":
            raise SystemExit("not a BND4")
        self.head = b[:0x40]
        self.esize = struct.unpack_from("<Q", b, 0x20)[0]
        n = struct.unpack_from("<I", b, 0x0C)[0]
        self.unicode = b[0x30] != 0
        self.files = []
        for k in range(n):
            e = b[0x40 + k * self.esize: 0x40 + (k + 1) * self.esize]
            flags, size, usize = struct.unpack_from("<QQQ", e, 0)
            off, ident, name_off = struct.unpack_from("<III", e, 0x18)
            if self.unicode:
                s = b[name_off:name_off + 4096]
                s = s[:s.find(b"\0\0") + 1].decode("utf-16le", "replace")
            else:
                s = b[name_off:b.find(b"\0", name_off)].decode("ascii", "replace")
            self.files.append({"flags": flags, "usize": usize, "id": ident,
                               "name": s, "data": b[off:off + size]})

    def pack(self):
        names = b""
        name_off = []
        base = 0x40 + len(self.files) * self.esize
        for f in self.files:
            name_off.append(base + len(names))
            names += f["name"].encode("utf-16le") + b"\0\0" if self.unicode else f["name"].encode("ascii") + b"\0"
        data_start = base + len(names)
        blobs, offs = b"", []
        for f in self.files:
            pad = (-len(blobs)) % 0x10
            blobs += b"\0" * pad
            offs.append(data_start + len(blobs))
            blobs += f["data"]
        out = bytearray(self.head)
        struct.pack_into("<Q", out, 0x28, data_start)
        for k, f in enumerate(self.files):
            e = bytearray(self.esize)
            struct.pack_into("<QQQ", e, 0, f["flags"], len(f["data"]), len(f["data"]))
            struct.pack_into("<III", e, 0x18, offs[k], f["id"], name_off[k])
            out += e
        out += names + blobs
        return bytes(out)

MAGIC = b"f\x00i\x00l\x00t\x00"


def wstr(b, i):
    j = i
    while j < len(b) - 1 and not (b[j] == 0 and b[j + 1] == 0):
        j += 2
    return b[i:j].decode("utf-16le", "replace"), j + 2


class Gparam:
    def __init__(self, raw):
        # `raw` is either a .dcx or the already-decompressed bytes of a bundle
        # entry; dcx_unpack passes the latter through with no header.
        d, head = dcx_unpack(raw)
        self.dcx_head = head
        if d[:8] != MAGIC:
            raise ValueError("not a GPARAM")
        self.d = bytearray(d)
        self.version = struct.unpack_from("<i", d, 8)[0]
        if self.version != 3:
            raise ValueError("GPARAM version %d is not the one this reads" % self.version)
        self.group_count = struct.unpack_from("<i", d, 0x10)[0]
        self.defs_off = struct.unpack_from("<I", d, 0x24)[0]
        self.values_off = struct.unpack_from("<I", d, 0x28)[0]
        self.params = {}  # editor name -> [(value offset, type)], every parameter of that name
        self._walk()

    def _walk(self):
        i = self.defs_off
        while i + 12 < self.values_off:
            v_off, _unk, typ = struct.unpack_from("<III", self.d, i)
            i += 12
            editor, i = wstr(self.d, i)
            _name, i = wstr(self.d, i)
            i = (i + 3) & ~3  # the next header is 4-aligned; without this the walk stops at one
            if not editor:
                break
            self.params.setdefault(editor, []).append((v_off, typ))

    @staticmethod
    def count(typ):
        # 0 is real: a lighting variant's group can have no value ids at all,
        # and the four bytes at its offset are the next parameter's.
        return typ >> 8

    @staticmethod
    def scalar(typ):
        return (typ & 0xFF) in (3, 5, 9)

    def get(self, editor):
        """Every value of the first parameter with this name: floats for kind
        9, ints otherwise (a vector's first word)."""
        entries = self.params.get(editor)
        if not entries:
            return None
        off, typ = entries[0]
        fmt = "<f" if (typ & 0xFF) == 9 else "<i"
        return [struct.unpack_from(fmt, self.d, self.values_off + off + 4 * k)[0]
                for k in range(self.count(typ) if self.scalar(typ) else min(1, self.count(typ)))]

    def set(self, editor, value):
        """`value` is an int, or a float when the parameter holds one - the
        file stores both as four bytes and only the name says which. Writes
        every value of every scalar parameter with this name."""
        done = False
        fmt = "<f" if isinstance(value, float) else "<i"
        for off, typ in self.params.get(editor, []):
            if not self.scalar(typ):
                continue
            for k in range(self.count(typ)):
                struct.pack_into(fmt, self.d, self.values_off + off + 4 * k, value)
            done = True
        return done

    def pack(self):
        return dcx_pack(self.dcx_head, bytes(self.d)) if self.dcx_head else bytes(self.d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+", help="a .gparam.dcx, or <dump root> <overlay root>")
    ap.add_argument("--list", action="store_true", help="every parameter and its value")
    ap.add_argument("--grep", default="", help="with --list, only names containing this")
    ap.add_argument("--set", action="append", default=[],
                    help='"<editor name>=<int|float>", repeatable')
    a = ap.parse_args()

    if a.list:
        g = Gparam(open(a.paths[0], "rb").read())
        print("%s: version %d, %d groups, %d parameters"
              % (os.path.basename(a.paths[0]), g.version, g.group_count, len(g.params)))
        for name in sorted(g.params):
            if a.grep and a.grep.lower() not in name.lower():
                continue
            vals = g.get(name)
            print("  %-52s %s" % (name, ", ".join(("%g" if isinstance(v, float) else "%d") % v for v in vals)
                                  if vals else "(no values)"))
        return 0

    if not a.set or len(a.paths) != 2:
        ap.error("--set needs a dump root and an overlay root")
    sets = {}
    for s in a.set:
        k, _, v = s.rpartition("=")
        # A value with a dot or an exponent is a float; the file stores both as
        # four bytes and only the parameter's meaning says which it is.
        sets[k] = float(v) if ("." in v or "e" in v.lower()) else int(v, 0)

    dump, mods = a.paths
    src_dir = os.path.join(dump, "dvdroot_ps4/param/drawparam")
    # **Both** the loose files and the bundles. Only the four templates -
    # default, empty, m_template and s_template - are ever requested loose; an
    # area's live parameters come out of its `.gparambnd.dcx`, a BND4 of
    # gparams. Editing only the loose ones changes nothing, measured.
    files = sorted(glob.glob(os.path.join(src_dir, "*.gparam.dcx")) +
                   glob.glob(os.path.join(src_dir, "*.gparambnd.dcx")))
    if not files:
        sys.exit("no draw parameters under %s" % src_dir)
    written = skipped = bundles = 0
    for src in files:
        raw = open(src, "rb").read()
        out = None
        if src.endswith("bnd.dcx"):
            payload, head = dcx_unpack(raw)
            try:
                bnd = Bnd4(payload)
            except SystemExit:
                skipped += 1
                continue
            touched = False
            for f in bnd.files:
                try:
                    g = Gparam(f["data"])
                except ValueError:
                    continue
                if [g.set(k, v) for k, v in sets.items()].count(True):
                    f["data"] = g.pack()
                    touched = True
            if touched:
                out = dcx_pack(head, bnd.pack()) if head else bnd.pack()
                bundles += 1
        else:
            try:
                g = Gparam(raw)
            except ValueError as e:
                print("  %s: %s; skipped" % (os.path.basename(src), e))
                skipped += 1
                continue
            if [g.set(k, v) for k, v in sets.items()].count(True):  # not any(): it stops at the first
                out = g.pack()
        if out is None:
            skipped += 1
            continue
        dst = os.path.join(mods, "dvdroot_ps4/param/drawparam", os.path.basename(src))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(out)
        written += 1
    print("%d file(s) written (%d of them bundles), %d without any of these parameters"
          % (written, bundles, skipped))
    for k, v in sets.items():
        print("  %s = %s" % (k, v))
    return 0


if __name__ == "__main__":
    sys.exit(main())

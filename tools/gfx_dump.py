#!/usr/bin/env python3
"""Dump the structure of a Scaleform GFX movie: sprites, instance names, labels.

The PC port needs to know what a menu movie contains before it can add to one.
GFX is SWF with extra tag ids, so the tag walk is the
standard one; only the tags this needs are decoded.

  tools/gfx_dump.py FILE                  # the whole tag list, one line each
  tools/gfx_dump.py FILE --sprite NAME    # one exported sprite, as a tree
  tools/gfx_dump.py FILE --exports        # exported symbols only
"""
import struct
import sys

TAGS = {
    0: "End", 1: "ShowFrame", 4: "PlaceObject", 5: "RemoveObject", 9: "SetBackgroundColor",
    12: "DoAction", 20: "DefineBitsLossless", 21: "DefineBitsJPEG2", 22: "DefineShape2",
    24: "Protect", 26: "PlaceObject2", 28: "RemoveObject2", 32: "DefineShape3",
    33: "DefineText2", 34: "DefineButton2", 36: "DefineBitsLossless2", 37: "DefineEditText",
    39: "DefineSprite", 43: "FrameLabel", 46: "DefineMorphShape", 48: "DefineFont2",
    56: "ExportAssets", 57: "ImportAssets", 59: "DoInitAction", 60: "DefineVideoStream",
    69: "FileAttributes", 70: "PlaceObject3", 73: "DefineFontAlignZones",
    74: "CSMTextSettings", 75: "DefineFont3", 76: "SymbolClass", 77: "Metadata",
    78: "DefineScalingGrid", 82: "DoABC", 83: "DefineShape4", 84: "DefineMorphShape2",
    86: "DefineSceneAndFrameLabelData", 88: "DefineFontName",
    1000: "GFX_ExporterInfo", 1001: "GFX_DefineExternalImage", 1002: "GFX_FontTextureInfo",
    1003: "GFX_DefineExternalGradientImage", 1004: "GFX_DefineGradientMap",
    1005: "GFX_DefineCompactedFont", 1006: "GFX_DefineExternalSound",
    1007: "GFX_DefineExternalStreamSound", 1008: "GFX_DefineSubImage",
    1009: "GFX_DefineExternalImage2", 1010: "GFX_DefineExternalFont",
}


def cstr(b, i):
    j = b.index(b"\x00", i)
    return b[i:j].decode("utf-8", "replace"), j + 1


def tags(b, i, end):
    """Yields (code, name, body_start, body_end)."""
    while i < end:
        (th,) = struct.unpack_from("<H", b, i)
        i += 2
        code, length = th >> 6, th & 0x3F
        if length == 0x3F:
            (length,) = struct.unpack_from("<I", b, i)
            i += 4
        yield code, TAGS.get(code, "Tag%d" % code), i, i + length
        if code == 0:
            return
        i += length


def body_start(b):
    """Skip the header: magic, version, length, RECT, frame rate, frame count."""
    i = 8
    nbits = b[i] >> 3
    i += (5 + 4 * nbits + 7) // 8
    return i + 4


def place_name(b, code, s, e):
    """The instance name a PlaceObject2/3 gives its character, if any."""
    i = s
    if code == 26:
        flags = b[i]
        i += 3  # flags + depth
        if flags & 0x02:  # has character
            i += 2
        if flags & 0x04:  # has matrix
            i = skip_matrix(b, i)
        if flags & 0x08:  # has colour transform
            i = skip_cxform(b, i)
        if flags & 0x10:  # has ratio
            i += 2
        if flags & 0x20:  # has name
            return cstr(b, i)[0]
        return None
    if code == 70:
        flags, flags2 = b[i], b[i + 1]
        i += 4  # two flag bytes + depth
        if flags2 & 0x08:  # has class name
            _, i = cstr(b, i)
        if flags & 0x02:
            i += 2
        if flags & 0x04:
            i = skip_matrix(b, i)
        if flags & 0x08:
            i = skip_cxform(b, i)
        if flags & 0x10:
            i += 2
        if flags & 0x20:
            return cstr(b, i)[0]
    return None


class Bits:
    def __init__(self, b, i):
        self.b, self.i, self.bit = b, i, 0

    def read(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.b[self.i] >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8:
                self.bit, self.i = 0, self.i + 1
        return v

    def align(self):
        if self.bit:
            self.bit, self.i = 0, self.i + 1
        return self.i


def skip_matrix(b, i):
    r = Bits(b, i)
    if r.read(1):
        n = r.read(5)
        r.read(n * 2)
    if r.read(1):
        n = r.read(5)
        r.read(n * 2)
    n = r.read(5)
    r.read(n * 2)
    return r.align()


def skip_cxform(b, i):
    r = Bits(b, i)
    has_add, has_mul = r.read(1), r.read(1)
    n = r.read(4)
    if has_mul:
        r.read(n * 4)
    if has_add:
        r.read(n * 4)
    return r.align()


def exports(b):
    """character id -> exported name. ExportAssets is AS2's, SymbolClass AS3's;
    both Bloodborne's and DS3's menu movies are AS3, so SymbolClass is the one
    that carries the names."""
    out = {}
    for code, _, s, e in tags(b, body_start(b), len(b)):
        if code in (56, 76):
            (n,) = struct.unpack_from("<H", b, s)
            i = s + 2
            for _ in range(n):
                (cid,) = struct.unpack_from("<H", b, i)
                name, i = cstr(b, i + 2)
                out[cid] = name
    return out


def walk(b, s, e, names, depth, out):
    for code, name, ts, te in tags(b, s, e):
        if code == 39:
            cid, frames = struct.unpack_from("<HH", b, ts)
            label = names.get(cid, "")
            out.append("%s- sprite %d%s (%d frame%s)" %
                       ("  " * depth, cid, " '%s'" % label if label else "", frames,
                        "" if frames == 1 else "s"))
            walk(b, ts + 4, te, names, depth + 1, out)
        elif code in (26, 70):
            n = place_name(b, code, ts, te)
            if n:
                cid = None
                flags = b[ts]
                if code == 26 and (flags & 0x02):
                    (cid,) = struct.unpack_from("<H", b, ts + 3)
                out.append("%s  place '%s'%s" % ("  " * depth, n,
                                                 " -> char %d" % cid if cid is not None else ""))
        elif code == 43:
            out.append("%s  label '%s'" % ("  " * depth, cstr(b, ts)[0]))


def main():
    path = sys.argv[1]
    b = open(path, "rb").read()
    if b[:3] not in (b"GFX", b"FWS"):
        sys.exit("%s: not an uncompressed GFX/SWF (%r)" % (path, b[:4]))
    names = exports(b)
    if "--exports" in sys.argv:
        for cid, n in sorted(names.items()):
            print("%5d  %s" % (cid, n))
        return
    if "--sprite" in sys.argv:
        want = sys.argv[sys.argv.index("--sprite") + 1]
        target = {cid for cid, n in names.items() if n == want}
        if not target:
            sys.exit("%s: no exported sprite named %r" % (path, want))
        for code, _, ts, te in tags(b, body_start(b), len(b)):
            if code == 39:
                (cid,) = struct.unpack_from("<H", b, ts)
                if cid in target:
                    out = []
                    frames = struct.unpack_from("<H", b, ts + 2)[0]
                    print("sprite %d '%s' (%d frames)" % (cid, want, frames))
                    walk(b, ts + 4, te, names, 1, out)
                    print("\n".join(out))
        return
    out = []
    walk(b, body_start(b), len(b), names, 0, out)
    print("\n".join(out))


main()

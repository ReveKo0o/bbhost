#!/usr/bin/env python3
"""A movie's named placements in **stage** coordinates, transforms composed.

A placement's matrix is relative to the sprite that holds it, and that sprite
is itself placed with a matrix of its own. `menu/commandlist.gfx` puts its rows
at a 74.30 px pitch inside their sprite while the System list measures 67.50 on
screen - the difference is a 0.908 scale on the parent placement. So local
coordinates are not an answer, they are an input.

Scaleform does this composition itself: `SprjScaleformValue::HitTest` (DS3
`sub_140e63aa0`) fetches the display object's **world** matrix, inverts it,
maps the point into local space and compares against local bounds. Doing the
same composition statically gives the same answer for a movie whose menus do
not animate their containers, without any call into the guest.

    tools/gfx_world.py <movie.gfx> [--filter Item_]

Prints every named placement reachable from the main timeline, with the
composed matrix applied, in stage pixels.
"""
import argparse
import struct
import sys
import zlib

PLACE2, PLACE3, DEFINE_SPRITE = 26, 70, 39


class Bits:
    def __init__(self, b, pos):
        self.b, self.pos = b, pos * 8

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.b[self.pos >> 3] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def s(self, n):
        if n == 0:
            return 0
        v = self.u(n)
        return v - (1 << n) if v & (1 << (n - 1)) else v

    def align(self):
        self.pos = (self.pos + 7) & ~7

    @property
    def byte(self):
        return self.pos >> 3


def read_matrix(b, off):
    """SWF MATRIX -> (a, b, c, d, tx, ty), tx/ty in twips. Scale and rotate are
    16.16 fixed point; a missing field is the identity's."""
    r = Bits(b, off)
    sx = sy = 1.0
    r0 = r1 = 0.0
    if r.u(1):
        n = r.u(5)
        sx, sy = r.s(n) / 65536.0, r.s(n) / 65536.0
    if r.u(1):
        n = r.u(5)
        r0, r1 = r.s(n) / 65536.0, r.s(n) / 65536.0
    n = r.u(5)
    tx, ty = r.s(n), r.s(n)
    r.align()
    return (sx, r0, r1, sy, float(tx), float(ty)), r.byte


IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


def compose(p, c):
    """p then c: the child's matrix applied inside the parent's."""
    pa, pb, pc, pd, ptx, pty = p
    ca, cb, cc, cd, ctx, cty = c
    return (pa * ca + pc * cb, pb * ca + pd * cb,
            pa * cc + pc * cd, pb * cc + pd * cd,
            pa * ctx + pc * cty + ptx, pb * ctx + pd * cty + pty)


def tags(b, i, end):
    while i < end - 1:
        (th,) = struct.unpack_from("<H", b, i)
        i += 2
        code, ln = th >> 6, th & 0x3F
        if ln == 0x3F:
            (ln,) = struct.unpack_from("<I", b, i)
            i += 4
        if code == 0:
            return
        yield code, i, i + ln
        i += ln


def parse_place(b, code, ts, te):
    """-> (depth, char_id or None, name or None, matrix or None)."""
    p = ts
    flags = b[p]
    p += 1
    if code == PLACE3:
        flags2 = b[p]
        p += 1
    depth = struct.unpack_from("<H", b, p)[0]
    p += 2
    cid = None
    if flags & 0x02:
        cid = struct.unpack_from("<H", b, p)[0]
        p += 2
    m = None
    if flags & 0x04:
        m, p = read_matrix(b, p)
    if flags & 0x08:  # CXFORMWITHALPHA
        r = Bits(b, p)
        has_add, has_mul = r.u(1), r.u(1)
        n = r.u(4)
        if has_mul:
            for _ in range(4):
                r.s(n)
        if has_add:
            for _ in range(4):
                r.s(n)
        r.align()
        p = r.byte
    if flags & 0x10:
        p += 2  # ratio
    name = None
    if flags & 0x20:
        e = b.index(b"\x00", p)
        name = b[p:e].decode("latin1")
    return depth, cid, name, m


def sprites(b, body):
    """char id -> (body_start, body_end) for every DefineSprite."""
    out = {}
    for code, ts, te in tags(b, body, len(b)):
        if code == DEFINE_SPRITE:
            cid = struct.unpack_from("<H", b, ts)[0]
            out[cid] = (ts + 4, te)
    return out


def resting_frame(b, start, end):
    """The timeline state a menu actually sits at.

    These sprites animate: an option section has 19 frames labelled FadeIn,
    Loop and FadeOut, and **re-places every item on each one**, so the first
    matrix a tag walk meets belongs to a fade rather than to the menu at rest.
    That is a 74.30 px row pitch read where the screen shows 67.50.

    So run the timeline, letting a later PlaceObject at a depth replace an
    earlier one, and stop at `Loop` - the label the engine parks on.
    """
    state = {}
    for code, ts, te in tags(b, start, end):
        if code in (PLACE2, PLACE3):
            depth, cid, name, m = parse_place(b, code, ts, te)
            prev = state.get(depth)
            # A move-only place keeps the character and the name it had.
            if prev:
                cid = cid if cid is not None else prev[0]
                name = name or prev[1]
                m = m if m is not None else prev[2]
            state[depth] = (cid, name, m)
        elif code == 43:  # FrameLabel
            e = b.index(b"\x00", ts)
            if b[ts:e].decode("latin1") == "Loop":
                break
    return state


def walk(b, start, end, sprs, at, out, depth, seen, path):
    """Names repeat: every option section in optionsetting.gfx has its own
    Item_0_0, as does the Top list above them. So a placement is identified by
    its **path**, not its name, or a flat table silently keeps whichever sprite
    happened to be walked first."""
    if depth > 8:
        return
    for cid, name, m in resting_frame(b, start, end).values():
        here = compose(at, m) if m else at
        child = path + "." + name if name else path
        if name:
            out.append((child, here))
        if cid is not None and cid in sprs and cid not in seen:
            s, e = sprs[cid]
            walk(b, s, e, sprs, here, out, depth + 1, seen | {cid}, child)


def body_start(b):
    i = 8
    nbits = b[i] >> 3
    i += (5 + 4 * nbits + 7) // 8
    return i + 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("movie")
    ap.add_argument("--filter", default="")
    a = ap.parse_args()
    b = open(a.movie, "rb").read()
    if b[:3] == b"CFX":
        b = b[:8] + zlib.decompress(b[8:])
    elif b[:3] != b"GFX":
        sys.exit("not a GFX movie")
    body = body_start(b)
    sprs = sprites(b, body)
    out = []
    walk(b, body, len(b), sprs, IDENTITY, out, 0, set(), "")
    for name, m in out:
        if a.filter and a.filter not in name:
            continue
        print("%-52s x=%8.2f  y=%8.2f  scale=%.4f,%.4f" %
              (name.lstrip("."), m[4] / 20.0, m[5] / 20.0, m[0], m[3]))


if __name__ == "__main__":
    main()

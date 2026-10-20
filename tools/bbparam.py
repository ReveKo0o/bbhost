#!/usr/bin/env python3
"""Read Bloodborne's game params offline, by the game's own field names.

    tools/bbparam.py TABLE [--app0 DIR] [--id N] [--fields a,b,c]

gameparam.parambnd.dcx and paramdef.paramdefbnd.dcx are DCX (DFLT: zlib at
0x4c) around a BND4: header 0x40 bytes, file count at 0x0c, then 0x24-byte
file headers (flags, -1, compressed size u64, size u64, data offset u32,
id u32, name offset u32; names UTF-16). A PARAM: row count u16 at 0x0a,
type string at 0x0c, format byte at 0x2d, flags at 0x2e; rows at 0x40, 0x18
bytes each (id u32, pad, data offset u64) when flags & 2, else 0x0c bytes.
A PARAMDEF (format 201): field count u16 at 0x08, field size 0xd0 at 0x0a,
fields from the u64 at 0x30; each: type at +0x40 (8 bytes), byte count at
+0x64, internal name at +0x90 ("name:bits" a bitfield, "name[n]" an array)
- the same layout engine/paramdef.cpp reads live.
"""
import argparse
import os
import struct
import sys
import zlib

APP0 = os.environ.get('BBHOST_APP0', '')  # the dump: the folder that contains dvdroot_ps4
SIZES = {'s8': 1, 'u8': 1, 'dummy8': 1, 'fixstr': 1, 's16': 2, 'u16': 2, 'fixstrW': 2, 's32': 4, 'u32': 4, 'f32': 4}
FMT = {'s8': 'b', 'u8': 'B', 's16': 'h', 'u16': 'H', 's32': 'i', 'u32': 'I', 'f32': 'f'}


def dcx(path):
    d = open(path, 'rb').read()
    return zlib.decompress(d[0x4c:]) if d[:4] == b'DCX\0' else d


def bnd4(b):
    n = struct.unpack_from('<i', b, 0x0c)[0]
    hs = struct.unpack_from('<q', b, 0x20)[0]
    out = {}
    for i in range(n):
        o = 0x40 + i * hs
        size = struct.unpack_from('<q', b, o + 0x10)[0]
        data, _id, name_o = struct.unpack_from('<IiI', b, o + 0x18)
        e = name_o
        while b[e:e + 2] != b'\0\0':
            e += 2
        name = b[name_o:e].decode('utf-16le').replace('\\', '/').split('/')[-1]
        out[name] = b[data:data + size]
    return out


def cstr(b, o, n):
    return b[o:o + n].split(b'\0')[0].decode('ascii', 'replace')


def paramdef(b):
    count, fsize = struct.unpack_from('<HH', b, 0x08)
    ptype = cstr(b, 0x0c, 0x20)
    at = struct.unpack_from('<Q', b, 0x30)[0]
    fields, off, unit = [], 0, None   # unit: [offset, bytes, used bits]
    for i in range(count):
        f = at + i * fsize
        typ = cstr(b, f + 0x40, 8)
        nbytes = struct.unpack_from('<I', b, f + 0x64)[0]
        name = cstr(b, f + 0x90, 0x20).strip()
        bits, cnt = 0, 1
        if ':' in name:
            name, bits = name.split(':')[0].strip(), int(name.split(':')[1])
        if '[' in name:
            cnt = int(name[name.index('[') + 1:name.index(']')])
            name = name[:name.index('[')].strip()
        es = SIZES.get(typ, 0)
        if bits and es:
            if not unit or unit[1] != es or unit[2] + bits > es * 8:
                if unit:
                    off += unit[1]
                unit = [off, es, 0]
            fields.append(dict(name=name, type=typ, offset=unit[0], bytes=es, bit=unit[2], bits=bits, count=1))
            unit[2] += bits
        else:
            if unit:
                off += unit[1]
                unit = None
            n = nbytes or es * cnt
            fields.append(dict(name=name, type=typ, offset=off, bytes=n, bit=0, bits=0, count=cnt))
            off += n
    return ptype, fields


def param_rows(b):
    rows, = struct.unpack_from('<H', b, 0x0a)
    ptype = cstr(b, 0x0c, 0x20)
    flags = b[0x2e]
    out = []
    for i in range(rows):
        if flags & 2:
            rid, = struct.unpack_from('<I', b, 0x40 + 0x18 * i)
            data, = struct.unpack_from('<Q', b, 0x40 + 0x18 * i + 8)
        else:
            rid, data = struct.unpack_from('<II', b, 0x40 + 0x0c * i)
        out.append((rid, data))
    return ptype, out


def value(b, o, f):
    if f['bits']:
        u = int.from_bytes(b[o + f['offset']:o + f['offset'] + f['bytes']], 'little')
        return (u >> f['bit']) & ((1 << f['bits']) - 1)
    if f['type'] == 'fixstr':
        return cstr(b, o + f['offset'], f['bytes'])
    if f['type'] == 'fixstrW':
        return b[o + f['offset']:o + f['offset'] + f['bytes']].decode('utf-16le', 'replace').split('\0')[0]
    c = FMT.get(f['type'])
    if not c:
        return None
    es = SIZES[f['type']]
    vals = [struct.unpack_from('<' + c, b, o + f['offset'] + k * es)[0] for k in range(f['count'])]
    return vals[0] if f['count'] == 1 else vals


def load_table(table, app0=APP0):
    """[(id, {field: value})] of a gameparam table ("ReturnPointParam")."""
    params = bnd4(dcx(app0 + '/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx'))
    defs = {}
    for _n, raw in bnd4(dcx(app0 + '/dvdroot_ps4/paramdef/paramdef.paramdefbnd.dcx')).items():
        try:
            t, f = paramdef(raw)
            defs[t] = f
        except (struct.error, ValueError):
            pass
    raw = params.get(table + '.param')
    if raw is None:
        raise KeyError('no %s.param (have %s)' % (table, ', '.join(sorted(params))))
    ptype, rows = param_rows(raw)
    fields = defs[ptype]
    return [(rid, {f['name']: value(raw, o, f) for f in fields if not f['name'].startswith('pad') and f['type'] != 'dummy8'})
            for rid, o in rows]


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('table')
    ap.add_argument('--app0', default=APP0)
    ap.add_argument('--id', type=int)
    ap.add_argument('--fields')
    a = ap.parse_args()
    want = a.fields.split(',') if a.fields else None
    for rid, row in load_table(a.table, a.app0):
        if a.id is not None and rid != a.id:
            continue
        print(rid, {k: v for k, v in row.items() if not want or k in want})

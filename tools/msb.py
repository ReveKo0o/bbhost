#!/usr/bin/env python3
"""A minimal reader for Bloodborne's map layouts (map/mapstudio/*.msb.dcx).

    tools/msb.py FILE.msb.dcx [--type N] [--name REGEX]

The file is a DCX (DFLT: a zlib stream at 0x4c) holding an MSB: "MSB " then
param lists (MODEL, EVENT, POINT, PARTS). A list is (version i32, count+1
i32, name offset i64) and count+1 entry offsets, the last one the next
list. Offsets inside an entry are relative to the entry.

A part: +0x00 description offset, +0x08 name offset (UTF-16), +0x14 type
(0 map piece, 1 object, 2 enemy, 4 player start, 5 collision, ...), +0x18
index, +0x1c model index, +0x28 position (x, y, z; y up), +0x34 rotation
(degrees), +0x40 scale. A point (region): +0x00 name offset, +0x10 shape,
+0x14 position, +0x20 rotation (by shape: sphere, box, ...). Only the fields
this reads were checked against a running game (tools/map_validation.sh).
"""
import re
import struct
import sys
import zlib


def load(path):
    d = open(path, 'rb').read()
    if d[:4] == b'DCX\0':
        d = zlib.decompress(d[0x4c:])
    if d[:4] != b'MSB ':
        raise ValueError('not an MSB: ' + path)
    return d


def _u16(b, o):
    e = o
    while b[e:e + 2] != b'\0\0':
        e += 2
    return b[o:e].decode('utf-16le', 'replace')


def lists(b):
    out = {}
    off = 0x10
    while off:
        _ver, cnt, nameo = struct.unpack_from('<iiq', b, off)
        offs = struct.unpack_from('<%dq' % cnt, b, off + 16)
        out[_u16(b, nameo)] = offs[:-1]
        off = offs[-1]
    return out


def parts(b):
    ls = lists(b)
    models = [_u16(b, o + struct.unpack_from('<q', b, o)[0]) for o in ls.get('MODEL_PARAM_ST', ())]
    res = []
    for o in ls.get('PARTS_PARAM_ST', ()):
        desc_o, name_o = struct.unpack_from('<qq', b, o)
        _id, typ, idx, midx = struct.unpack_from('<iiii', b, o + 0x10)
        pos = struct.unpack_from('<3f', b, o + 0x28)
        rot = struct.unpack_from('<3f', b, o + 0x34)
        res.append({'name': _u16(b, o + name_o), 'desc': _u16(b, o + desc_o), 'type': typ, 'index': idx,
                    'model': models[midx] if 0 <= midx < len(models) else None,
                    'pos': [round(v, 3) for v in pos], 'rot': [round(v, 3) for v in rot], 'at': o})
    return res


def map_offset(b):
    """The block's MapOffset event (EVENT_PARAM_ST type 9): [x, y, z], or
    [0, 0, 0] when there is none. An event: +0x00 name offset, +0x0c type,
    +0x20 type-data offset (x, y, z, rotation y)."""
    for o in lists(b).get('EVENT_PARAM_ST', ()):
        typ, = struct.unpack_from('<i', b, o + 0x0c)
        td, = struct.unpack_from('<q', b, o + 0x20)
        if typ == 9 and td:
            return [round(v, 4) for v in struct.unpack_from('<3f', b, o + td)]
    return [0.0, 0.0, 0.0]


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--type', type=int)
    ap.add_argument('--name')
    a = ap.parse_args()
    for p in parts(load(a.file)):
        if a.type is not None and p['type'] != a.type:
            continue
        if a.name and not re.search(a.name, p['name']):
            continue
        print(p['type'], p['name'], p['model'], *p['pos'], p['rot'][1], p['desc'])

#!/usr/bin/env python3
"""Every lamp: entity, event flags, travel-menu slot and return point.

    tools/mapval_lamps.py [--app0 DIR] [--json]

Sources, all in the game's data:
  - dvdroot_ps4/event/m*.emevd.dcx: each map's event script starts event
    7000 once per lamp with (warp object entity, lamp entity, the flag that
    must be on before the lamp can be lit - 999 for none, flag base). Found
    by scanning the scripts' argument data for 7000 followed by an entity
    pair ending x950..x959 whose second is the first + 1000.
  - The lamp DB (FrpgNetMan +0xc70; include/bbhost/engine/frpg/bonfire_db.hpp)
    registers the base: base+10 is the lamp's lit flag, base+11 "selected"
    (the lamp last rested at in that map).
  - ReturnPointParam (tools/bbparam.py): row id = lamp entity + 1000, its
    area/block, warpChairNo (the Hunter's Dream headstone menu slot; 0-9 are
    the chalice dungeons), returnPointEntityId (an MSB "respawn" event whose
    point is where travel and death put the player).
engine/world_chr.cpp keeps the same table for BBHOST_TEST_UNLOCK_LAMPS.
"""
import argparse
import glob
import json
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bbparam  # noqa: E402


def lamps(app0):
    out = {}
    for p in sorted(glob.glob(os.path.join(app0, 'dvdroot_ps4', 'event', 'm*.emevd.dcx'))):
        d = open(p, 'rb').read()
        b = zlib.decompress(d[0x4c:]) if d[:4] == b'DCX\0' else d
        for off in range(4):
            v = struct.unpack_from('<%di' % ((len(b) - off) // 4), b, off)
            for i in range(len(v) - 5):
                if v[i] == 7000 and v[i + 2] == v[i + 1] + 1000 and 950 <= v[i + 2] % 1000 <= 959:
                    out[v[i + 2]] = dict(lamp=v[i + 2], warp_object=v[i + 1], requires_flag=v[i + 3], flag_base=v[i + 4],
                                         lit_flag=v[i + 4] + 10, selected_flag=v[i + 4] + 11,
                                         map='m%02d_%02d_00_00' % (v[i + 2] // 100000, (v[i + 2] // 10000) % 10))
    rp = dict(bbparam.load_table('ReturnPointParam', app0))
    for l in out.values():
        r = rp.get(l['lamp'] + 1000)
        l['return_point'] = l['lamp'] + 1000 if r else None
        l['warp_chair'] = r['warpChairNo'] if r else None
    return [out[k] for k in sorted(out)]


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--app0', default=bbparam.APP0)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    L = lamps(a.app0)
    if a.json:
        print(json.dumps(L, indent=1))
    else:
        print('map          lamp     object   requires  base      lit       return   chair')
        for l in L:
            print('%s %-8d %-8d %-9d %-9d %-9d %-8s %s' % (l['map'], l['lamp'], l['warp_object'], l['requires_flag'], l['flag_base'],
                                                        l['lit_flag'], l['return_point'], l['warp_chair']))

#!/usr/bin/env python3
"""A multi-block tour for tools/map_validation.sh --blocks (the navmesh
points need numpy).

    tools/mapval_blocks.py --app0 DIR --out DIR --blocks m23_00_00_00,m22_00_00_00
        [--navmesh N] [--hold S] [--walk S]

For each block, in order: lamp travel to its first lamp (ReturnPointParam,
tools/bbparam.py: the row of the lowest warpChairNo in that area/block; the
Hunter's Dream has only 2102950), then warps to every lamp in the block's
MSB (o009900), the player starts and two message notes, a walk from the
first lamp (the driver holds forward), N points spread over the block's
navmesh (the walkable polygons' face centroids, picked farthest-first, from
the worldmap navmesh module in MAPVAL_NAVMESH_TOOLS), and a fall death from
30 m onto the first navmesh point (the player respawns at the lamp it
travelled to).

Positions are the MSB's own frame: the lamp travel put the player exactly
on the MSB respawn point's coordinates (m23 at (126.4, -65.25, 36.0), no
MapOffset applied), so the warps use MSB coordinates as they are; the
navmesh tool returns block-local polygons, which get the MapOffset added
back. targets.json keeps both frames (msb, and local = msb - offset).
"""
import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bbparam  # noqa: E402
import msb  # noqa: E402

NAVMESH_TOOLS = os.environ.get('MAPVAL_NAVMESH_TOOLS', '')  # a directory holding the worldmap navmesh module
MAIN_BLOCKS = ['m21_00_00_00', 'm21_01_00_00', 'm22_00_00_00', 'm23_00_00_00', 'm24_00_00_00', 'm24_01_00_00', 'm24_02_00_00',
               'm25_00_00_00', 'm26_00_00_00', 'm27_00_00_00', 'm28_00_00_00', 'm32_00_00_00', 'm33_00_00_00', 'm34_00_00_00',
               'm35_00_00_00', 'm36_00_00_00']


def block_id(m):
    a, b, c, d = (int(x) for x in m[1:].split('_'))
    return (a << 24) | (b << 16) | (c << 8) | d


def return_points(app0):
    """block name -> [(warpChairNo, return point id)] of the lamps."""
    out = {}
    for rid, r in bbparam.load_table('ReturnPointParam', app0):
        if r['areaNo'] <= 0 or r['areaNo'] == 29:
            continue
        m = 'm%02d_%02d_00_00' % (r['areaNo'], r['blockNo'])
        chair = r['warpChairNo']
        if chair < 0 and m != 'm21_00_00_00':
            continue
        out.setdefault(m, []).append((chair if chair >= 0 else 99, rid))
    return {m: sorted(v) for m, v in out.items()}


def navmesh_points(m, app0, offset, n):
    if n <= 0:
        return []
    try:
        sys.path.insert(0, NAVMESH_TOOLS)
        import numpy as np
        from worldmap.msb import MSB
        from worldmap import navmesh
    except ImportError as e:
        print('  (no navmesh: %s)' % e)
        return []
    raw = open(os.path.join(app0, 'dvdroot_ps4', 'map', 'mapstudio', m + '.msb.dcx'), 'rb').read()
    polys, _st = navmesh.block_navmesh(m, [os.path.join(app0, 'dvdroot_ps4', 'map')], MSB(raw))
    if not polys:
        return []
    cent = np.array([p.mean(axis=0) for p in polys], np.float64)
    # Farthest-first over x/z from the centroid nearest the middle.
    mid = cent[:, [0, 2]].mean(axis=0)
    pick = [int(np.argmin(((cent[:, [0, 2]] - mid) ** 2).sum(1)))]
    d = ((cent[:, [0, 2]] - cent[pick[0], [0, 2]]) ** 2).sum(1)
    while len(pick) < min(n, len(cent)):
        i = int(np.argmax(d))
        pick.append(i)
        d = np.minimum(d, ((cent[:, [0, 2]] - cent[i, [0, 2]]) ** 2).sum(1))
    return [[float(cent[i, k] + offset[k]) for k in range(3)] for i in pick]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--app0', default=bbparam.APP0)
    ap.add_argument('--out', required=True)
    ap.add_argument('--blocks', default='all')
    ap.add_argument('--navmesh', type=int, default=20)
    ap.add_argument('--hold', type=float, default=4.5)
    ap.add_argument('--walk', type=float, default=12.0)
    ap.add_argument('--lift', type=float, default=0.3)
    a = ap.parse_args()
    blocks = MAIN_BLOCKS if a.blocks == 'all' else [b.strip() for b in a.blocks.split(',') if b.strip()]
    rps = return_points(a.app0)
    lines, targets, plan = [], [], []
    for m in blocks:
        path = os.path.join(a.app0, 'dvdroot_ps4', 'map', 'mapstudio', m + '.msb.dcx')
        if m not in rps or not os.path.exists(path):
            print('%s: no lamp return point or no MSB - skipped' % m)
            plan.append(dict(map=m, skipped='no lamp return point or MSB'))
            continue
        raw = msb.load(path)
        off = msb.map_offset(raw)
        parts = msb.parts(raw)
        rp = rps[m][0][1]
        lines += ['# %s: travel to return point %d, MapOffset %s' % (m, rp, off), 'travel %d %08x %s' % (rp, block_id(m), m)]
        tg = []
        for p in parts:
            if p['type'] == 1 and p['model'] == 'o009900':
                tg.append(dict(kind='lamp', **p))
        seen = set()
        for p in parts:
            if p['type'] == 4:
                k = tuple(round(v, 0) for v in p['pos'])
                if k not in seen and len(seen) < 4:
                    seen.add(k)
                    tg.append(dict(kind='player_start', **p))
        tg += [dict(kind='note', **p) for p in parts if p['type'] == 1 and p['model'] == 'o000700'][:2]
        nav = navmesh_points(m, a.app0, off, a.navmesh)
        tg += [dict(kind='navmesh', name='nav%02d' % i, desc='navmesh face centroid', type=-1, index=i, model=None, pos=[round(v, 3) for v in p],
                    rot=[0, 0, 0]) for i, p in enumerate(nav)]
        # The walk comes after the lamps, starts and notes (a first visit
        # opens with an area-intro camera that takes input away), from the
        # first lamp again, or the first target.
        walk_after = max((i for i, t in enumerate(tg) if t['kind'] != 'navmesh'), default=0)
        walk_from = next((t for t in tg if t['kind'] == 'lamp'), tg[0] if tg else None)
        for i, t in enumerate(tg):
            t.pop('at', None)
            t['map'] = m
            t['name'] = '%s:%s' % (m, t['name'])
            t['offset'] = off
            t['local'] = [round(t['pos'][k] - off[k], 3) for k in range(3)]
            t['id'] = t['name']
            x, y, z = t['pos']
            lines.append('warp %s %.3f %.3f %.3f 0 %.1f' % (t['name'], x, y + a.lift, z, a.hold))
            if i == walk_after and walk_from:
                x, y, z = walk_from['pos']
                lines.append('warp walkstart_%s %.3f %.3f %.3f 0 1.0' % (m, x, y + a.lift, z))
                lines.append('walk walk_%s %.1f' % (m, a.walk))
        if nav:
            lines += ['falldeath fall_%s %.3f %.3f %.3f 30' % (m, *nav[0]), 'wait 3']
        targets += tg
        plan.append(dict(map=m, return_point=rp, offset=off, targets=len(tg), lamps=sum(t['kind'] == 'lamp' for t in tg),
                         navmesh=len(nav), fall=nav[0] if nav else None))
        print('%s: return point %d, offset %s, %d targets (%d lamps, %d navmesh)' % (m, rp, off, len(tg),
              sum(t['kind'] == 'lamp' for t in tg), len(nav)))
    lines.append('wait 5')
    os.makedirs(a.out, exist_ok=True)
    open(os.path.join(a.out, 'tour.txt'), 'w').write('\n'.join(lines) + '\n')
    json.dump({'blocks': plan, 'targets': targets}, open(os.path.join(a.out, 'targets.json'), 'w'), ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()

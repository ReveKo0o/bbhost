#!/usr/bin/env python3
"""Map-validation ground truth through the world map's transforms.

    tools/mapval_pixels.py --dataset DIR/dataset.json [--transforms JSON] [--lookup PY]

For every ground-truth point in the dataset (tools/mapval_dataset.py) - each
tour target and each death/fall/kill - this asks the website's own lookup
(server/worldmap.py of the world-map branch, game_to_map(map, x, y, z) with
play-log coordinates) where it lands on the painting twice: once for the
truth (the MSB part, turned into block-local metres by subtracting the
block's offset in the transform table; for a death, the probe's position)
and once for what the play log / history.db recorded. It writes
DIR/pixels.csv and DIR/pixels.json with both pixels, the pixel distance and
that distance in metres (pixels / the region's px_per_m), and, for lamps,
the distance to the painted lamp the transform was fitted to when the
table's control points name it.

Without the transform table (not written yet) it says so and does nothing:
run it again once server/game_data/map_transforms.json exists.
"""
import argparse
import csv
import importlib.util
import json
import math
import os
import sys

WORLDMAP = os.environ.get('MAPVAL_WORLDMAP', '')  # the world map server's checkout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dataset', required=True)
    ap.add_argument('--transforms', default=os.environ.get('MAPVAL_TRANSFORMS', os.path.join(WORLDMAP, 'game_data', 'map_transforms.json')))
    ap.add_argument('--lookup', default=os.environ.get('MAPVAL_LOOKUP', os.path.join(WORLDMAP, 'worldmap.py')))
    a = ap.parse_args()
    if not os.path.exists(a.transforms):
        print('pixels: no transform table at %s yet - rerun: %s --dataset %s' % (a.transforms, sys.argv[0], a.dataset))
        return
    spec = importlib.util.spec_from_file_location('worldmap_lookup', a.lookup)
    wm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(wm)
    tab = wm.load(a.transforms)
    ds = json.load(open(a.dataset))
    out = []

    def px_of(mp, p):
        if not p:
            return None
        r = wm.game_to_map(mp, *p)
        return (r, wm.region_of(mp, *p)) if r else None

    def scale(region):
        return (tab['regions'].get(region) or {}).get('px_per_m') or float('nan')

    def ctrl_for(region, block, name):
        """The region's control point for this lamp: its painted pixel and the
        table's own leave-one-out error for it (px, m), or None."""
        reg = tab['regions'].get(region) or {}
        stats = reg.get('stats') or {}
        for i, c in enumerate(reg.get('control_points') or []):
            if isinstance(c, dict) and c.get('part') == name and c.get('block') == block and isinstance(c.get('px'), list):
                loo = stats.get('loo_px') or []
                loo_m = stats.get('loo_m') or []
                return c['px'][:2], (loo[i] if i < len(loo) else None), (loo_m[i] if i < len(loo_m) else None)
        return None

    for r in ds['targets']:
        if not r.get('reached'):
            continue
        off = (tab['blocks'].get(r['map_name']) or {}).get('offset') or [0, 0, 0]
        truth = [r['msb'][i] - off[i] for i in range(3)]
        rows = [('truth', truth), ('regularlog', r.get('regularlog_median')), ('track', r.get('track_nearest'))]
        res = {k: px_of(r['map_name'], p) for k, p in rows}
        row = dict(seed=r['seed'], map_name=r['map_name'], point=r['target'], kind=r['kind'], truth_local=truth)
        if res['truth']:
            (tx, ty, status), region = res['truth']
            row.update(region=region, status=status, truth_px=[round(tx, 1), round(ty, 1)])
            for k in ('regularlog', 'track'):
                if res[k]:
                    (x, y, _), reg2 = res[k]
                    d = math.hypot(x - tx, y - ty)
                    row[k + '_px'] = [round(x, 1), round(y, 1)]
                    row[k + '_err_px'] = round(d, 2)
                    row[k + '_err_m'] = round(d / scale(region), 3)
                    row[k + '_region'] = reg2
            if r['kind'] == 'lamp':
                c = ctrl_for(region, r['map_name'], r['target'])
                if c:
                    d = math.hypot(c[0][0] - tx, c[0][1] - ty)
                    row.update(painted_lamp_px=c[0], painted_err_px=round(d, 1), painted_err_m=round(d / scale(region), 2),
                               loo_px=c[1], loo_m=c[2])
        else:
            row['note'] = 'not on the painting (or no transform for the block)'
        out.append(row)
    for e in ds['events']:
        truth, rec = e.get('truth'), e.get('history_pos')
        row = dict(seed=e['seed'], map_name=e['map'], point=e['kind'], kind=e['kind'])
        t, h = px_of(e['map'], truth), px_of(e['map'], rec)
        if t and h:
            (tx, ty, status), region = t
            (x, y, _), _r = h
            d = math.hypot(x - tx, y - ty)
            row.update(region=region, status=status, truth_px=[round(tx, 1), round(ty, 1)], history_px=[round(x, 1), round(y, 1)],
                       history_err_px=round(d, 2), history_err_m=round(d / scale(region), 3))
        out.append(row)
    base = os.path.dirname(os.path.abspath(a.dataset))
    json.dump(out, open(os.path.join(base, 'pixels.json'), 'w'), ensure_ascii=False, indent=1)
    keys = sorted({k for r in out for k in r})
    with open(os.path.join(base, 'pixels.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in out:
            w.writerow({k: json.dumps(v) if isinstance(v, (list, dict)) else v for k, v in r.items()})
    errs = [r['regularlog_err_px'] for r in out if 'regularlog_err_px' in r]
    print('pixels: %d points through %s; play log vs truth: median %.2f px, max %.2f px' % (
        len(out), a.transforms, sorted(errs)[len(errs) // 2] if errs else float('nan'), max(errs) if errs else float('nan')))
    shown = set()
    for r in out:
        if 'painted_err_px' in r and (r['map_name'], r['point']) not in shown:
            shown.add((r['map_name'], r['point']))
            print('  %s lamp %s (%s): the fit puts it %.1f px (%.2f m) from its painted lamp; leave-one-out %s px (%s m)' % (
                r['map_name'], r['point'], r['region'], r['painted_err_px'], r['painted_err_m'], r['loo_px'], r['loo_m']))
        elif r.get('truth_px') and r['kind'] != 'lamp':
            pass
    regs = {}
    for r in out:
        if r.get('region'):
            regs.setdefault(r['region'], []).append(r['point'])
    for reg, pts in sorted(regs.items()):
        st = (tab['regions'][reg].get('status'), tab['regions'][reg].get('px_per_m'))
        print('  region %s (%s, %.2f px/m): %d ground-truth points' % (reg, st[0], st[1] or 0, len(pts)))


if __name__ == '__main__':
    main()

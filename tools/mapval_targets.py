#!/usr/bin/env python3
"""The target list for one map-validation tour (tools/map_validation.sh).

    tools/mapval_targets.py --app0 DIR --map m24_01_00_00 --out DIR
        [--kill "R X Y Z TEAM"] [--fall NAME] [--hold S] [--notes N]

Reads the block's layout (map/mapstudio/<map>.msb.dcx, tools/msb.py) and
writes DIR/targets.json (every target with its MSB name, description and
position) and DIR/tour.txt (the steps tools/mapval_plugin.c runs):

  - every lamp in the block: the object parts of model o009900 (the lamp's
    MSB description names it "ワープOBJ" / "ワープ椅子", the warp object);
  - the player start points (part type 4), one per distinct position;
  - up to N message notes (model o000700, "紙片"), landmarks with their own
    description;
  - M enemy placements (part type 2), spread over the block (farthest point
    first from a per-seed start), so each seed visits other ground points;
  - one kill (the nearest live hostile around a point, or the player);
  - one fall death: dropped from 30 m onto the first player start (a known
    ground point), last, because the player respawns at a lamp after it.

A warp goes to the part's position raised by 0.3 m (the character settles
onto the ground under it); the dataset compares the MSB position itself.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import msb  # noqa: E402

LAMP = 'o009900'
NOTE = 'o000700'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--app0', required=True)
    ap.add_argument('--map', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--kill', default='80', help='"R [X Y Z [TEAM]]" for the kill step, "" for none')
    ap.add_argument('--fall', default='start', help='"start" (first player start) or "X Y Z", "" for none')
    ap.add_argument('--hold', type=float, default=6.0)
    ap.add_argument('--notes', type=int, default=3)
    ap.add_argument('--lift', type=float, default=0.3)
    ap.add_argument('--enemies', type=int, default=8, help='enemy placements to visit, spread over the block')
    ap.add_argument('--enemy-start', type=int, default=0, help='which placement the spread starts from (one per seed)')
    a = ap.parse_args()

    path = os.path.join(a.app0, 'dvdroot_ps4', 'map', 'mapstudio', a.map + '.msb.dcx')
    parts = msb.parts(msb.load(path))
    targets = []
    for p in parts:
        if p['type'] == 1 and p['model'] == LAMP:
            targets.append(dict(kind='lamp', **p))
    seen = set()
    for p in parts:
        if p['type'] == 4:
            k = tuple(round(v, 1) for v in p['pos'])
            if k in seen:
                continue
            seen.add(k)
            targets.append(dict(kind='player_start', **p))
    notes = [p for p in parts if p['type'] == 1 and p['model'] == NOTE][:a.notes]
    targets += [dict(kind='note', **p) for p in notes]
    # Enemy placements (part type 2): ground points all over the block,
    # picked farthest-first from a seed-specific start so the seeds' tours
    # cover different ones.
    enemies = [p for p in parts if p['type'] == 2 and not p['name'].startswith('c0000')]
    picked = []
    if enemies and a.enemies > 0:
        picked.append(enemies[a.enemy_start % len(enemies)])
        while len(picked) < min(a.enemies, len(enemies)):
            far = max(enemies, key=lambda e: min((e['pos'][0] - q['pos'][0]) ** 2 + (e['pos'][2] - q['pos'][2]) ** 2 for q in picked))
            picked.append(far)
    targets += [dict(kind='enemy_placement', **p) for p in picked]
    for t in targets:
        t['map'] = a.map
        t.pop('at', None)
        t['id'] = '%s:%s' % (a.map, t['name'])

    os.makedirs(a.out, exist_ok=True)
    lines = ['# %s: %d targets from %s' % (a.map, len(targets), path)]
    for t in targets:
        x, y, z = t['pos']
        # Facing is not part of the check: yaw 0 for every warp.
        lines.append('warp %s %.3f %.3f %.3f %.4f %.1f' % (t['name'], x, y + a.lift, z, 0.0, a.hold))
    if a.kill.strip():
        lines.append('kill ' + a.kill.strip())
        # The driver's attack clicks run ~22 s from kill-ready; a click that
        # lands after the fall death's respawn, next to a lamp, can send the
        # character to the Hunter's Dream. Let them run out first.
        lines.append('wait 25')
    fall = None
    if a.fall.strip():
        if a.fall.strip() == 'start':
            st = [t for t in targets if t['kind'] == 'player_start']
            fall = st[0]['pos'] if st else None
        else:
            fall = [float(v) for v in a.fall.split()]
        if fall:
            lines.append('falldeath fall_%s %.3f %.3f %.3f 30' % (a.map, *fall))
    lines.append('wait 5')
    open(os.path.join(a.out, 'tour.txt'), 'w').write('\n'.join(lines) + '\n')
    json.dump({'map': a.map, 'msb': path, 'targets': targets, 'fall': fall, 'kill': a.kill},
              open(os.path.join(a.out, 'targets.json'), 'w'), ensure_ascii=False, indent=1)
    print('%s: %d targets (%d lamps), kill %r, fall %s' % (
        a.map, len(targets), sum(t['kind'] == 'lamp' for t in targets), a.kill, fall))


if __name__ == '__main__':
    main()

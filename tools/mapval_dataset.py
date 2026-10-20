#!/usr/bin/env python3
"""The map-validation dataset from one tools/map_validation.sh run.

    tools/mapval_dataset.py --out DIR --server DIR/server --seeds central ...
        [--corpus HISTORY_DB]

Run it with the server's python (it imports the server's own play-log
decoder and track codec from DIR/server/server). Inputs per seed: DIR/<seed>/
run.log (the probe's lines, tools/mapval_plugin.c) and targets.json; from the
server: the uploaded play logs (playlogs/raw, decoded in full: every
RegularLog sample) and history.db (what the website reads: thinned tracks,
deaths/falls/kills).

Writes DIR/dataset.json (everything), targets.csv, events.csv,
frame_check.json, and prints the summary.

Times: the probe logs epoch seconds (host clock); a play-log event's
created_at is the game's clock (sceRtc, UTC to the second); history.db's
`at` is the server's receive time minus the event's distance from the file's
last event. The frame check finds the offset between the first two (whole
seconds, expected 0) and compares only samples taken while the player stood
still, so a second's rounding does not show up as distance.
"""
import argparse
import bisect
import calendar
import csv
import glob
import json
import math
import os
import re
import sqlite3
import statistics as st
import sys

POS = re.compile(r'mapval pos ([\d.]+) ([0-9a-f]{8}) (\S+) (\S+) (\S+) (\S+) (-?\d+)')
AT = re.compile(r'mapval at (\S+) ([\d.]+) ([0-9a-f]{8}) (\S+) (\S+) (\S+) yaw (\S+)')
HOLD_END = re.compile(r'mapval hold-end (\S+) ([\d.]+)')
KILL_T = re.compile(r'mapval kill-target (\S+) ([\d.]+) (\S+) (\S+) (\S+)')
KILL_D = re.compile(r'mapval kill-done (\S*) ([\d.]+) (\S+) (\S+) (\S+) player (\S+) (\S+) (\S+)')
FALL_S = re.compile(r'mapval step \d+ falldeath (\S+) ([\d.]+) target (\S+) (\S+) (\S+) from (\S+)')
FALL_D = re.compile(r'mapval fall-dead (\S+) ([\d.]+) last (\S+) (\S+) (\S+)')
TOUR = re.compile(r'mapval tour-(start|done) ([\d.]+)')
WALK = re.compile(r'mapval walk-(ready|end) (\S+) ([\d.]+) (\S+) (\S+) (\S+)')
TRAVEL = re.compile(r'mapval travel-(done|failed) (\S+) ([\d.]+) (.*)')
NEAR = re.compile(r'mapval near (\S+) ([\d.]+) (\S+) (\S+) (\S+) (-?\d+)')
USER = re.compile(r'user_id=(\d+)')


def block_name(b):
    return 'm%02d_%02d_%02d_%02d' % (b >> 24, (b >> 16) & 255, (b >> 8) & 255, b & 255)


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def hdist(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def r3(v):
    return [round(x, 3) for x in v] if v is not None else None


def parse_run(path):
    run = dict(pos=[], at={}, hold_end={}, kill_target=None, kill_done=None, fall_start=None, fall_dead=None,
               tour={}, user_id=None, near=[], falls_start=[], falls_dead=[], walks=[], travels=[])
    if not os.path.exists(path):
        return run
    for line in open(path, errors='replace'):
        if 'mapval' not in line:
            if run['user_id'] is None:
                m = USER.search(line)
                if m and '127.0.0.1' in line:
                    run['user_id'] = int(m.group(1))
            continue
        m = POS.search(line)
        if m:
            t, b, x, y, z, yaw, hp = m.groups()
            run['pos'].append((float(t), int(b, 16), float(x), float(y), float(z), float(yaw), int(hp)))
            continue
        m = AT.search(line)
        if m:
            n, t, b, x, y, z, yaw = m.groups()
            run['at'][n] = dict(t=float(t), block=int(b, 16), pos=[float(x), float(y), float(z)], yaw=float(yaw))
            continue
        m = HOLD_END.search(line)
        if m:
            run['hold_end'][m.group(1)] = float(m.group(2))
            continue
        m = KILL_T.search(line)
        if m:
            run['kill_target'] = dict(chr=m.group(1), t=float(m.group(2)), pos=[float(v) for v in m.groups()[2:]])
            continue
        m = KILL_D.search(line)
        if m:
            g = m.groups()
            run['kill_done'] = dict(chr=g[0], t=float(g[1]), pos=[float(v) for v in g[2:5]], player=[float(v) for v in g[5:8]])
            continue
        m = FALL_S.search(line)
        if m:
            g = m.groups()
            run['fall_start'] = dict(name=g[0], t=float(g[1]), target=[float(v) for v in g[2:5]], height=float(g[5]))
            run['falls_start'].append(run['fall_start'])
            continue
        m = FALL_D.search(line)
        if m:
            g = m.groups()
            run['fall_dead'] = dict(name=g[0], t=float(g[1]), last=[float(v) for v in g[2:5]])
            run['falls_dead'].append(run['fall_dead'])
            continue
        m = TOUR.search(line)
        if m:
            run['tour'][m.group(1)] = float(m.group(2))
            continue
        m = WALK.search(line)
        if m:
            g = m.groups()
            if g[0] == 'ready':
                run['walks'].append(dict(name=g[1], t0=float(g[2]), start=[float(v) for v in g[3:6]]))
            elif run['walks'] and run['walks'][-1]['name'] == g[1]:
                run['walks'][-1].update(t1=float(g[2]), end=[float(v) for v in g[3:6]])
            continue
        m = TRAVEL.search(line)
        if m:
            g = m.groups()
            run['travels'].append(dict(result=g[0], name=g[1], t=float(g[2]), detail=g[3]))
            continue
        m = NEAR.search(line)
        if m:
            g = m.groups()
            run['near'].append(dict(chr=g[0], t=float(g[1]), pos=[float(v) for v in g[2:5]], hp=int(g[5])))
    return run


def client_seconds(v):
    try:
        return calendar.timegm((int(v[0:4]), int(v[5:7]), int(v[8:10]), int(v[11:13]), int(v[14:16]), int(v[17:19]), 0, 0, 0))
    except (ValueError, TypeError, IndexError):
        return None


def uid_of(e):
    m = re.match(r'^\d+_(-?\d+)_\d+$', str(e.get('playlog_id', '')))
    return int(m.group(1)) if m else None


def load_playlogs(srv):
    sys.path.insert(0, srv)
    from server.ingest import decoder  # noqa: E402
    events = []
    for f in sorted(glob.glob(os.path.join(srv, 'playlogs', 'raw', '*', '*'))):
        try:
            evs, _bad = decoder.decode_raw(open(f, 'rb').read())
        except Exception as ex:  # a file the worker rejected is not ours to judge here
            print('  (could not decode %s: %s)' % (f, ex))
            continue
        for e in evs:
            e['_file'] = os.path.relpath(f, os.path.join(srv, 'playlogs'))
            e['_t'] = client_seconds(e.get('created_at'))
            e['_uid'] = uid_of(e)
        events += evs
    return events


def load_history(srv):
    sys.path.insert(0, srv)
    from server.ingest.history import decode_track  # noqa: E402
    path = os.path.join(srv, 'data', 'history.db')
    out = dict(tracks=[], deaths=[], sessions={})
    if not os.path.exists(path):
        return out
    # Read-write on purpose: this is the run's own scratch copy, and a
    # read-only open of a WAL database the worker just closed can miss the
    # rows still in its -wal file.
    db = sqlite3.connect(path)
    for sid, uid in db.execute('SELECT id, user_id FROM play_sessions'):
        out['sessions'][sid] = uid
    for tid, sid, mp, data in db.execute('SELECT id, session_id, map, data FROM tracks ORDER BY id'):
        for s in decode_track(data):
            out['tracks'].append(dict(track=tid, session=sid, user_id=out['sessions'].get(sid), map=mp, t=s[0],
                                      pos=[s[1], s[2], s[3]], hp=s[4], max_hp=s[5]))
    cols = [r[1] for r in db.execute('PRAGMA table_info(deaths)')]
    for row in db.execute('SELECT * FROM deaths ORDER BY at'):
        d = dict(zip(cols, row))
        d['user_id'] = out['sessions'].get(d['session_id'])
        out['deaths'].append(d)
    return out


class Probe:
    """The probe's once-a-second positions of one seed, by time."""

    def __init__(self, pos):
        self.p = sorted(pos)
        self.t = [p[0] for p in self.p]

    def near(self, t, within=0.75):
        i = bisect.bisect_left(self.t, t)
        best = None
        for j in (i - 1, i):
            if 0 <= j < len(self.p) and abs(self.p[j][0] - t) <= within:
                if best is None or abs(self.p[j][0] - t) < abs(best[0] - t):
                    best = self.p[j]
        return best

    def still(self, t, span=2.5, tol=0.05):
        """Positions over [t-span, t+span], if the player did not move."""
        lo, hi = bisect.bisect_left(self.t, t - span), bisect.bisect_right(self.t, t + span)
        w = self.p[lo:hi]
        if len(w) < 3:
            return None
        a = w[0][2:5]
        if all(dist(a, q[2:5]) <= tol for q in w) and len({q[1] for q in w}) == 1:
            return w[len(w) // 2]
        return None

    def at(self, t):
        """Position at t, linear between the two samples around it (<= 1.5 s apart)."""
        i = bisect.bisect_left(self.t, t)
        if i <= 0 or i >= len(self.p):
            return None
        a, b = self.p[i - 1], self.p[i]
        if b[0] - a[0] > 1.5 or a[1] != b[1]:
            return None
        f = (t - a[0]) / (b[0] - a[0])
        return [a[2 + k] + (b[2 + k] - a[2 + k]) * f for k in range(3)]

    def last_alive_before(self, t):
        i = bisect.bisect_right(self.t, t)
        for j in range(i - 1, -1, -1):
            if self.p[j][6] > 0:
                return self.p[j]
        return None


_OFFSETS = {}


def block_offset(name, app0=os.environ.get('BBHOST_APP0', '')):
    """The block's MSB MapOffset (tools/msb.py), [0, 0, 0] without one."""
    if name not in _OFFSETS:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import msb as msbmod
        f = os.path.join(app0, 'dvdroot_ps4', 'map', 'mapstudio', '%s.msb.dcx' % name)
        _OFFSETS[name] = msbmod.map_offset(msbmod.load(f)) if name and os.path.exists(f) else [0.0, 0.0, 0.0]
    return _OFFSETS[name]


def frame_check(seed_runs, events):
    """RegularLog pos3 against the probe at the same second, standing still.
    The probe reads the engine's position, which is in the MSB's frame; the
    play log's is block-local, the engine's minus the block's MapOffset
    (found in the first block tours: m23 logs (probe - (50, -30, 0))). Both
    differences are kept: d (pos3 - probe) and d_local (pos3 - (probe -
    MapOffset of the sample's map_name))."""
    rl = [e for e in events if e.get('rep_format') == 'SprjPlaylog_RegularLog' and e['_t'] is not None
          and isinstance(e.get('pos3'), list)]
    by_uid = {}
    for e in rl:
        by_uid.setdefault(e['_uid'], []).append(e)
    res = dict(offsets={}, pairs=[], per_seed={})
    for seed, run in seed_runs.items():
        uid = run['user_id']
        samples = by_uid.get(uid, [])
        probe = Probe(run['pos'])
        if not samples or not probe.p:
            res['per_seed'][seed] = dict(samples=len(samples), probe=len(probe.p), note='nothing to compare')
            continue
        # The clock offset: the shift (0.1 s steps) that puts the play log's
        # positions on the probe's path best, over every sample (moving ones
        # are what pin it down; created_at is truncated, hence the +0.5).
        best = None
        for k in range(-30, 31):
            o = k / 10
            ds = [dist([p[k] - block_offset(e.get('map_name'))[k] for k in range(3)], e['pos3'][:3]) for e in samples
                  for p in [probe.at(e['_t'] + 0.5 + o)] if p]
            if len(ds) >= 10:
                m = st.median(ds)
                if best is None or m < best[1]:
                    best = (o, m)
        off = best[0] if best else 0.0
        res['offsets'][seed] = off
        pairs = []
        for e in samples:
            # created_at is truncated to the second: the sample was taken
            # within [t, t+1); the middle is t + 0.5.
            q = probe.still(e['_t'] + 0.5 + off)
            if not q:
                continue
            d = [e['pos3'][i] - q[2 + i] for i in range(3)]
            mo = block_offset(e.get('map_name'))
            dl = [e['pos3'][i] - (q[2 + i] - mo[i]) for i in range(3)]
            pairs.append(dict(seed=seed, t=e['_t'], map_name=e.get('map_name'), mapuid=e.get('mapuid'),
                              probe_block=block_name(q[1]), probe_block_id=q[1], pos3=e['pos3'][:3], probe=list(q[2:5]),
                              yaw_log=(e.get('angle') or [None, None])[1], yaw_probe=q[5], d=d, map_offset=mo, d_local=dl, hit_name=e.get('hit_name')))
        res['pairs'] += pairs
        res['per_seed'][seed] = dict(samples=len(samples), still_pairs=len(pairs), clock_offset_s=off,
                                     median_path_error_m=round(best[1], 3) if best else None)
    P = res['pairs']
    if P:
        # Against the probe minus the block's MapOffset (the play log's frame).
        ab = lambda k: [abs(p['d_local'][k]) for p in P]
        res['summary'] = dict(
            pairs=len(P),
            median_abs_dx=st.median(ab(0)), median_abs_dy=st.median(ab(1)), median_abs_dz=st.median(ab(2)),
            max_abs=max(max(ab(0)), max(ab(1)), max(ab(2))),
            mean_d=[st.mean(p['d_local'][k] for p in P) for k in range(3)],
            within_1cm=sum(max(abs(v) for v in p['d_local']) <= 0.01 for p in P),
            within_5cm=sum(max(abs(v) for v in p['d_local']) <= 0.05 for p in P),
            map_name_matches_block=sum(p['map_name'] == p['probe_block'] for p in P),
            mapuid_matches_block=sum(p['mapuid'] is not None and int(p['mapuid']) == p['probe_block_id'] for p in P),
            yaw_matches=sum(p['yaw_log'] is not None and abs(p['yaw_log'] - p['yaw_probe']) <= 0.011 for p in P))
        # Which probe axis is the play log's second component (the height)?
        perms = {}
        for name, idx in (('x,y,z', (0, 1, 2)), ('x,z,y', (0, 2, 1)), ('z,y,x', (2, 1, 0)), ('y,x,z', (1, 0, 2))):
            perms[name] = st.mean(dist(p['pos3'], [p['probe'][i] - p['map_offset'][i] for i in idx]) for p in P)
        res['summary']['mean_error_by_axis_order'] = perms
        # Per block: the play log minus the engine's position (a MapOffset
        # applied on one side would show here as a constant).
        per = {}
        for p in P:
            per.setdefault(p['map_name'], []).append(p)
        res['per_map'] = {m: dict(pairs=len(v), map_offset=v[0]['map_offset'],
                                  median_pos3_minus_probe=r3([st.median(p['d'][k] for p in v) for k in range(3)]),
                                  median_abs_d_local=r3([st.median(abs(p['d_local'][k]) for p in v) for k in range(3)]),
                                  within_5cm_local=sum(max(abs(x) for x in p['d_local']) <= 0.05 for p in v))
                          for m, v in sorted(per.items())}
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--server', required=True)
    ap.add_argument('--seeds', nargs='+', required=True)
    ap.add_argument('--corpus', help='a history.db with real players in blocks that have an MSB MapOffset')
    a = ap.parse_args()

    runs = {s: parse_run(os.path.join(a.out, s, 'run.log')) for s in a.seeds}
    targets = {}
    for s in a.seeds:
        p = os.path.join(a.out, s, 'targets.json')
        targets[s] = json.load(open(p)) if os.path.exists(p) else dict(targets=[])
    events = load_playlogs(a.server)
    hist = load_history(a.server)
    fc = frame_check(runs, events)

    rl_by_uid = {}
    for e in events:
        if e.get('rep_format') == 'SprjPlaylog_RegularLog' and e['_t'] is not None and isinstance(e.get('pos3'), list):
            rl_by_uid.setdefault(e['_uid'], []).append(e)

    rows = []
    for s in a.seeds:
        run = runs[s]
        uid = run['user_id']
        off = fc['offsets'].get(s, 0)
        for tg in targets[s]['targets']:
            at = run['at'].get(tg['name'])
            row = dict(seed=s, map_name=tg['map'], target=tg['name'], kind=tg['kind'], desc=tg['desc'],
                       msb=tg['pos'], local=tg.get('local', tg['pos']), offset=tg.get('offset', [0, 0, 0]), reached=bool(at))
            if not at:
                rows.append(row)
                continue
            t0, t1 = at['t'], run['hold_end'].get(tg['name'], at['t'] + 6)
            row.update(t_from=round(t0, 3), t_to=round(t1, 3), readback=at['pos'], readback_block=block_name(at['block']),
                       d_readback_msb=r3([at['pos'][i] - tg['pos'][i] for i in range(3)]),
                       h_readback_msb=round(hdist(at['pos'], tg['pos']), 3))
            # created_at is the second the sample fell in; the last second of
            # the hold may already hold the next warp's samples.
            logs = [e for e in rl_by_uid.get(uid, []) if t0 - 0.5 <= e['_t'] + off <= t1 - 1.5]
            row['regularlog'] = [dict(t=e['_t'], created_at=e.get('created_at'), map_name=e.get('map_name'), pos3=e['pos3'][:3],
                                      hit_name=e.get('hit_name'), file=e['_file']) for e in logs]
            if logs:
                m = [st.median(e['pos3'][i] for e in logs) for i in range(3)]
                row['regularlog_median'] = r3(m)
                row['regularlog_map_names'] = sorted({e.get('map_name') for e in logs})
                row['d_log_readback'] = r3([m[i] - at['pos'][i] for i in range(3)])
                row['d_log_msb'] = r3([m[i] - tg['pos'][i] for i in range(3)])
                row['h_log_msb'] = round(hdist(m, tg['pos']), 3)
                row['d_log_local'] = r3([m[i] - row['local'][i] for i in range(3)])
                row['h_log_local'] = round(hdist(m, row['local']), 3)
            # history.db's thinned track, by the server's clock: the samples
            # of this user within the hold (+-3 s for the receive lag).
            tr = [x for x in hist['tracks'] if x['user_id'] == uid and t0 - 1 <= x['t'] <= t1 + 1]
            row['track'] = [dict(t=x['t'], map=x['map'], pos=x['pos'], track=x['track']) for x in tr]
            if tr:
                near = min(tr, key=lambda x: dist(x['pos'], at['pos']))
                row['track_nearest'] = near['pos']
                row['d_track_msb'] = r3([near['pos'][i] - tg['pos'][i] for i in range(3)])
                row['h_track_msb'] = round(hdist(near['pos'], tg['pos']), 3)
                row['h_track_local'] = round(hdist(near['pos'], row['local']), 3)
            rows.append(row)

    # Deaths, falls and kills: what history.db holds against what the probe saw.
    evrows = []
    for s in a.seeds:
        run = runs[s]
        uid = run['user_id']
        probe = Probe(run['pos'])
        # This run's seconds only: a rerun with the same online id is the
        # same server user, and its earlier run's rows are not this probe's.
        lo = min((q[0] for q in run['pos']), default=0) - 30
        hi = max((q[0] for q in run['pos']), default=0) + 30
        mine = [d for d in hist['deaths'] if d['user_id'] == uid and lo <= d['at'] <= hi]
        raw = [e for e in events if e['_uid'] == uid and e.get('rep_format') in ('SprjPlaylog_ChrDead', 'SprjPlaylog_ChrFallDead')]
        for d in mine:
            pos = [d['x'], d['y'], d['z']]
            r = dict(seed=s, kind=d['kind'], map=d['map'], at=d['at'], victim=d['victim'], attacker=d['attacker'], history_pos=pos)
            # The same event in the raw log (its client time), and the truth.
            cand = [e for e in raw if e.get('pos3') and dist(e['pos3'][:3], pos) < 0.02]
            if cand:
                r['created_at'] = cand[0].get('created_at')
                r['client_t'] = cand[0]['_t']
            truth, how = None, None
            if d['kind'] == 'kill':
                kt, kd = run['kill_target'], run['kill_done']
                if kd and kd['chr'] == d['victim'] and any(v != 0 for v in kd['pos']):
                    truth, how = kd['pos'], 'probe: the enemy ChrIns position when its HP reached 0'
                else:
                    # Another enemy the attack killed: its last position the
                    # probe logged (every 0.5 s) before its HP reached 0.
                    seen_ = [n for n in run['near'] if n['chr'] == d['victim']]
                    dead = [n for n in seen_ if n['hp'] <= 0]
                    alive = [n for n in seen_ if n['hp'] > 0 and (not dead or n['t'] <= dead[0]['t'])]
                    if dead:
                        truth, how = dead[0]['pos'], 'probe: the enemy ChrIns position at the first 0.5 s sample with HP 0'
                    elif alive:
                        truth, how = alive[-1]['pos'], 'probe: the enemy ChrIns position at its last 0.5 s sample'
                    elif kt and kt['chr'] == d['victim']:
                        truth, how = kt['pos'], 'probe: the enemy ChrIns position when picked'
            elif d['kind'] == 'fall':
                # A scripted fall (the probe logged the moment its HP reached
                # 0) within 3 s, else the probe's position at the event's
                # second: most falls here are kill planes under a navmesh
                # point, where the game logs ChrFallDead every few frames
                # until the respawn, each at the falling position.
                ct = r.get('client_t')
                fd = min(run['falls_dead'], key=lambda f: abs(f['t'] - (ct or d['at'])), default=None)
                fs = min(run['falls_start'], key=lambda f: abs(f['t'] - (ct or d['at'])), default=None)
                if fd and abs(fd['t'] - (ct or d['at'])) <= 3:
                    truth, how = fd['last'], 'probe: the player position when its HP reached 0 (scripted fall)'
                    r['fall_target_msb'] = fs['target'] if fs else None
                    if fs:
                        mo = block_offset(d['map'])
                        r['h_history_fall_target'] = round(hdist(pos, [fs['target'][k] - mo[k] for k in range(3)]), 3)
                elif ct is not None:
                    tt = ct + 0.5 + fc['offsets'].get(s, 0)
                    q = probe.near(tt, within=0.8)
                    if q:
                        truth, how = list(q[2:5]), 'probe: the position at the same second (a kill-plane fall; the player moves ~10 m/s)'
                        # A teleport within +-1.5 s makes "the same second"
                        # ambiguous: flag it (the stats leave these out).
                        span = [x for x in run['pos'] if abs(x[0] - tt) <= 1.5]
                        if span and max(hdist(a[2:5], b[2:5]) for a in span for b in span) > 20:
                            r['at_teleport'] = True
            elif d['kind'] == 'death' and r.get('client_t') is not None:
                q = probe.last_alive_before(r['client_t'] + 1 + fc['offsets'].get(s, 0))
                if q:
                    truth, how = list(q[2:5]), 'probe: the last position with HP > 0 before the death (1 s samples)'
            if truth:
                # The probe reads the engine's (MSB-frame) position; the play
                # log's is that minus the block's MapOffset.
                mo = block_offset(d['map']) if d['map'] else [0.0, 0.0, 0.0]
                if any(mo):
                    r['truth_engine'] = truth
                    truth = [truth[k] - mo[k] for k in range(3)]
                    how += ', minus the block MapOffset %s' % mo
                r['truth'] = truth
                r['truth_from'] = how
                r['d_history_truth'] = r3([pos[i] - truth[i] for i in range(3)])
                r['dist_history_truth'] = round(dist(pos, truth), 3)
                r['h_history_truth'] = round(hdist(pos, truth), 3)
            evrows.append(r)

    # The walks: the probe's path, and what the play log and history.db
    # recorded over the same seconds.
    walks = []
    for s in a.seeds:
        run = runs[s]
        uid = run['user_id']
        off = fc['offsets'].get(s, 0)
        for w in run['walks']:
            t0, t1 = w['t0'], w.get('t1', w['t0'] + 10)
            probe = [dict(t=q[0], block=block_name(q[1]), pos=list(q[2:5])) for q in run['pos'] if t0 - 1 <= q[0] <= t1 + 1]
            logs = [dict(t=e['_t'], map_name=e.get('map_name'), pos3=e['pos3'][:3]) for e in rl_by_uid.get(uid, [])
                    if t0 - 0.5 <= e['_t'] + off <= t1 + 0.5]
            tr = [dict(t=x['t'], map=x['map'], pos=x['pos']) for x in hist['tracks'] if x['user_id'] == uid and t0 - 1 <= x['t'] <= t1 + 1]
            # Each play-log sample against the probe at its second.
            P = Probe(run['pos'])
            mp = w['name'].replace('walk_', '')
            moff = next((b.get('offset') for b in targets[s].get('blocks', []) if b.get('map') == mp and b.get('offset')), [0, 0, 0])
            # The play log is block-local: the probe (the engine, the MSB's
            # frame) minus the block's MapOffset.
            ds = [dist(e['pos3'], [p[k] - moff[k] for k in range(3)]) for e in logs if e['map_name'] == mp
                  for p in [P.at(e['t'] + 0.5 + off)] if p]
            walks.append(dict(seed=s, name=w['name'], map=w['name'].replace('walk_', ''), t0=t0, t1=t1, start=w['start'], end=w.get('end'),
                              moved_m=round(dist(w['start'], w['end']), 2) if w.get('end') else None,
                              path_m=round(sum(dist(a['pos'], b['pos']) for a, b in zip(probe, probe[1:]) if a['block'] == b['block']), 2),
                              probe=probe, regularlog=logs,
                              track=tr, offset=moff, median_log_vs_probe_minus_offset_m=round(st.median(ds), 3) if ds else None))
    json.dump(walks, open(os.path.join(a.out, 'walks.json'), 'w'), ensure_ascii=False, indent=1)
    travels = {s: runs[s]['travels'] for s in a.seeds if runs[s]['travels']}

    corpus = None
    if a.corpus and os.path.exists(a.corpus):
        corpus = offset_check(a.corpus, os.path.dirname(os.path.abspath(__file__)))

    ds = dict(generated_by='tools/mapval_dataset.py', out=a.out, seeds={s: dict(user_id=runs[s]['user_id'], probe_samples=len(runs[s]['pos']),
                                                                                 tour=runs[s]['tour']) for s in a.seeds},
              frame_check={k: v for k, v in fc.items() if k != 'pairs'}, targets=rows, events=evrows, offset_check=corpus,
              walks=[{k: v for k, v in w.items() if k not in ('probe', 'regularlog', 'track')} for w in walks], travels=travels,
              notes=['positions are metres in the block\'s own frame (y up); the MSB part position is that frame plus the '
                     'block\'s MapOffset event (0 or absent for m24_00 and m24_01, so here the two are the same)',
                     'readback: the probe\'s player position 1.5 s after the warp; msb: the part itself (warps aim 0.3 m above it)'])
    json.dump(ds, open(os.path.join(a.out, 'dataset.json'), 'w'), ensure_ascii=False, indent=1)
    json.dump(fc, open(os.path.join(a.out, 'frame_check.json'), 'w'), ensure_ascii=False, indent=1)
    with open(os.path.join(a.out, 'targets.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['seed', 'map_name', 'target', 'kind', 'msb_x', 'msb_y', 'msb_z', 'readback_x', 'readback_y', 'readback_z',
                    'log_x', 'log_y', 'log_z', 'log_samples', 'track_x', 'track_y', 'track_z', 'track_samples',
                    'h_readback_msb', 'h_log_msb', 'h_track_msb', 'dy_log_msb', 'desc'])
        for r in rows:
            rb, lg, tk = r.get('readback') or [''] * 3, r.get('regularlog_median') or [''] * 3, r.get('track_nearest') or [''] * 3
            w.writerow([r['seed'], r['map_name'], r['target'], r['kind'], *r['msb'], *rb, *lg, len(r.get('regularlog', [])), *tk,
                        len(r.get('track', [])), r.get('h_readback_msb', ''), r.get('h_log_msb', ''), r.get('h_track_msb', ''),
                        (r.get('d_log_msb') or ['', '', ''])[1], r['desc']])
    with open(os.path.join(a.out, 'events.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['seed', 'kind', 'map', 'at', 'created_at', 'victim', 'attacker', 'x', 'y', 'z', 'truth_x', 'truth_y', 'truth_z',
                    'dist_history_truth', 'truth_from'])
        for r in evrows:
            tr = r.get('truth') or [''] * 3
            w.writerow([r['seed'], r['kind'], r['map'], r['at'], r.get('created_at', ''), r['victim'], r['attacker'], *r['history_pos'],
                        *tr, r.get('dist_history_truth', ''), r.get('truth_from', '')])
    summary(ds, rows, evrows, fc, corpus, walks)


def offset_check(path, tools):
    """Real players' tracks in blocks whose MSB has a MapOffset: are the
    play log's positions the MSB's (world) or the block's own (local)?
    Scored by the median distance from each track sample to the nearest MSB
    part (map pieces, objects, enemies, player starts), both ways."""
    sys.path.insert(0, tools)
    import msb as msbmod
    app0 = os.environ.get('BBHOST_APP0', '')
    here = os.path.dirname(os.path.dirname(os.path.abspath(path)))
    sys.path.insert(0, here)
    try:
        from server.ingest.history import decode_track
    except ImportError:
        return None
    db = sqlite3.connect('file:%s?mode=ro' % path, uri=True)
    out = {}
    for (mp,) in db.execute('SELECT DISTINCT map FROM tracks'):
        f = os.path.join(app0, 'dvdroot_ps4', 'map', 'mapstudio', mp + '.msb.dcx')
        if not os.path.exists(f):
            continue
        raw = msbmod.load(f)
        offset = msbmod.map_offset(raw)
        if not offset or not any(offset):
            continue
        pts = [p['pos'] for p in msbmod.parts(raw) if p['type'] in (0, 1, 2, 4)]
        S = []
        for (d,) in db.execute('SELECT data FROM tracks WHERE map = ?', (mp,)):
            S += decode_track(d)
        S = S[::max(1, len(S) // 400)]
        if not S or not pts:
            continue

        def score(shift):
            return st.median(min(dist([s[1] + shift[0], s[2] + shift[1], s[3] + shift[2]], p) for p in pts) for s in S)
        out[mp] = dict(offset=offset, samples=len(S), median_m_as_local=round(score(offset), 2),
                       median_m_as_world=round(score([0, 0, 0]), 2))
    return out


def summary(ds, rows, evrows, fc, corpus, walks=()):
    print('== map validation dataset: %s' % os.path.join(ds['out'], 'dataset.json'))
    for s, v in ds['seeds'].items():
        print('seed %-10s user %s, %d probe samples, tour %s' % (s, v['user_id'], v['probe_samples'],
              ', '.join('%s %.0f' % kv for kv in v['tour'].items()) or 'did not run'))
    sm = fc.get('summary')
    if sm:
        print('frame check: %d still pairs (RegularLog pos3 vs the probe - MapOffset, same second); clock offsets %s' % (sm['pairs'], fc['offsets']))
        print('  median |d| x %.3f y %.3f z %.3f m, max %.3f, mean d %s; within 1 cm %d, 5 cm %d' % (
            sm['median_abs_dx'], sm['median_abs_dy'], sm['median_abs_dz'], sm['max_abs'], r3(sm['mean_d']), sm['within_1cm'], sm['within_5cm']))
        print('  map_name == probe block %d/%d, mapuid == block %d/%d, yaw == angle[1] %d/%d' % (
            sm['map_name_matches_block'], sm['pairs'], sm['mapuid_matches_block'], sm['pairs'], sm['yaw_matches'], sm['pairs']))
        print('  mean error by axis order: %s' % ', '.join('%s %.2f' % kv for kv in sm['mean_error_by_axis_order'].items()))
    else:
        print('frame check: no pairs')
    for m, v in (fc.get('per_map') or {}).items():
        print('  %s: %d pairs; median pos3 - probe %s (MapOffset %s); vs probe - MapOffset: median |d| %s, %d within 5 cm' % (
            m, v['pairs'], v['median_pos3_minus_probe'], v['map_offset'], v['median_abs_d_local'], v['within_5cm_local']))
    reached = [r for r in rows if r['reached']]
    print('targets: %d, reached %d, with RegularLog samples %d, with history track samples %d' % (
        len(rows), len(reached), sum(bool(r.get('regularlog')) for r in reached), sum(bool(r.get('track')) for r in reached)))
    for k in ('h_readback_msb', 'h_log_msb', 'h_track_msb'):
        v = [r[k] for r in reached if k in r]
        if v:
            print('  %-15s median %.3f m, max %.3f m (n %d)' % (k, st.median(v), max(v), len(v)))
    dy = [r['d_log_msb'][1] for r in reached if r.get('d_log_msb')]
    if dy:
        print('  dy_log_msb      median %.3f m, range %.3f..%.3f (the character stands on the ground, the part may not)' % (st.median(dy), min(dy), max(dy)))
    for r in reached:
        print('  %-9s %-14s %-12s msb %s log %s track %s' % (r['seed'], r['map_name'], r['target'], r3(r['msb']),
              r.get('regularlog_median'), r.get('track_nearest')))
    bym = {}
    for r in rows:
        bym.setdefault(r['map_name'], []).append(r)
    if len(bym) > 1 or any(any(r.get('offset', [0, 0, 0])) for r in rows):
        print('per block (h = horizontal metres; msb = the MSB part as written, local = msb - MapOffset):')
        for m, rr in sorted(bym.items()):
            ok = [r for r in rr if r['reached']]
            hm = [r['h_log_msb'] for r in ok if 'h_log_msb' in r]
            hl = [r['h_log_local'] for r in ok if 'h_log_local' in r]
            print('  %s offset %s: %d/%d reached, play log vs msb median %s, vs local median %s' % (
                m, rr[0].get('offset'), len(ok), len(rr), round(st.median(hm), 3) if hm else None, round(st.median(hl), 3) if hl else None))
    for w in walks:
        print('  walk %s: path %s m, %d probe / %d play-log / %d track samples, play log vs probe - offset median %s m' % (
            w['map'], w['path_m'], len(w['probe']), len(w['regularlog']), len(w['track']), w['median_log_vs_probe_minus_offset_m']))
    print('events (history.db deaths table):')
    for r in evrows:
        print('  %-9s %-5s %s at %s by %s pos %s truth %s -> %s m' % (r['seed'], r['kind'], r['victim'], r.get('created_at'), r['attacker'],
              r['history_pos'], r3(r.get('truth')), r.get('dist_history_truth')))
    if corpus:
        print('offset check (real tracks in blocks with an MSB MapOffset; median metres to the nearest MSB part):')
        for mp, v in corpus.items():
            print('  %s offset %s: as block-local %.2f m, as MSB/world %.2f m (%d samples)' % (
                mp, r3(v['offset']), v['median_m_as_local'], v['median_m_as_world'], v['samples']))


if __name__ == '__main__':
    main()

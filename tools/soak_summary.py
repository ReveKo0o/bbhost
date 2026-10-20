#!/usr/bin/env python3
"""Summarise a bbhost run's frame statistics (BBHOST_FRAME_STATS=1), Linux or
Windows log alike - what tools/perf_soak.sh prints, for any log.

    tools/soak_summary.py LOG [--after-world 5] [--skip 40]

By default the window starts 5 s after the first in-game frame (the
"world:" line engine/loading.cpp writes), so runs whose load takes different
wall time (the laptop's ends ~85 s in, this box's ~40 s) compare the same
part of the game; --skip N counts N seconds of frame lines from the start
instead. Prints fps, main-loop work, the long frames, the process's and the
busiest threads' CPU, and the last GPU memory lines.
"""
import argparse
import re
import sys

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--after-world', type=float, default=5.0, help='seconds after the world loads to start (default 5)')
ap.add_argument('--skip', type=int, help='skip this many seconds of frame lines from the start instead')
ap.add_argument('--keep-loads', action='store_true',
                help='keep the seconds of a load and the 10 after it (a death and respawn mid-run), left out by default')
a = ap.parse_args()

lines = open(a.log, errors='replace').read().replace('\r', '').split('\n')
frames = [i for i, l in enumerate(lines) if l.startswith('[bbhost] frames:')]
if a.skip is not None:
    if len(frames) <= a.skip:
        sys.exit('only %d seconds of frame statistics' % len(frames))
    start = frames[a.skip]
else:
    # "world:" is logged at the first in-game frame (engine/loading.cpp);
    # older logs fall back to the first glare line, which the title also has.
    world = next((i for i, l in enumerate(lines) if l.startswith('[bbhost] world: the first in-game frame')), None)
    if world is None:
        world = next((i for i, l in enumerate(lines) if 'graphics: area glare luminance' in l), None)
    if world is None:
        sys.exit('the run never reached the world (no "world:" line)')
    after = [i for i in frames if i > world]
    n = int(a.after_world)
    if len(after) <= n:
        sys.exit('only %d seconds of frame statistics after the world loaded' % len(after))
    start = after[n]

# A load mid-run (the character died; the respawn reloads the area) is not
# steady play: its seconds and the 10 after it are left out. The "loading:"
# line comes at its end and says how long it took.
dropped = set()
if not a.keep_loads:
    for j, l in enumerate(lines):
        m = re.match(r'\[bbhost\] loading: ([0-9.]+) s', l)
        if not m or j < start:
            continue
        before = [i for i in frames if i < j][-(int(float(m.group(1))) + 2):]
        after = [i for i in frames if i > j][:10]
        dropped.update(before + after)
    # A load still going when the log ends (its loading screen runs uncapped).
    begins = [j for j, l in enumerate(lines) if l.startswith('[bbhost] loading: begins')]
    ends = [j for j, l in enumerate(lines) if re.match(r'\[bbhost\] loading: [0-9.]+ s', l)]
    if begins and (not ends or ends[-1] < begins[-1]):
        dropped.update(i for i in frames if i > begins[-1])
rows = []
threads = {}
gpu_busy = []  # ms a second bbhost's command buffers kept the GPU busy (host/gpu_busy.cpp)
for i in (i for i in frames if i >= start and i not in dropped):
    l = lines[i]
    fps = float(re.search(r'\(([0-9.]+)/s\)', l).group(1))
    work = re.search(r'main loop work avg ([0-9.]+) p95 ([0-9.]+) max ([0-9.]+) ms, (\d+) over', l)
    waited = re.search(r'main loop waited (\d+) ms', l)
    cpu = re.search(r'cpu (\d+)%:(.*?);', l)
    rows.append((fps, work, int(waited.group(1)) if waited else 0, int(cpu.group(1)) if cpu else 0))
    gb = re.search(r'gpu busy ([0-9.]+) ms/s', l)
    if gb:
        gpu_busy.append(float(gb.group(1)))
    if cpu:
        for m in re.finditer(r' (.+?):\d+ (\d+)%', cpu.group(2)):
            threads.setdefault(m.group(1), []).append(int(m.group(2)))
n = len(rows)
flips = sum(r[0] for r in rows)
w = [r[1] for r in rows if r[1]]
# Long frames and stalls in the kept seconds only: a line belongs to the
# second whose frame line follows it.
kept = set(i for i in frames if i >= start and i not in dropped)
owner = {}
nxt = None
for i in range(len(lines) - 1, start - 1, -1):
    if i in frames:
        nxt = i
    owner[i] = nxt
tail = [lines[i] for i in range(start, len(lines)) if owner.get(i) in kept]
stalls = sum(1 for l in tail if l.startswith('[bbhost] stall:'))
long_frames = [float(m.group(1)) for l in tail for m in [re.search(r'main loop: a ([0-9.]+) ms frame', l)] if m]
print('%s: %d s: fps mean %.2f, seconds under 58: %d%s' % (a.log, n, flips / n, sum(1 for r in rows if r[0] < 58),
      ' (%d s around mid-run loads left out)' % len(dropped) if dropped else ''))
if w:
    over = sum(int(x.group(4)) for x in w)
    print('  main loop work avg %.2f ms, p95 %.2f, worst %.1f; over 16.7: %d frames (%.1f%%); waited %.0f ms/s' %
          (sum(float(x.group(1)) for x in w) / len(w), sum(float(x.group(2)) for x in w) / len(w),
           max(float(x.group(3)) for x in w), over, 100.0 * over / max(1, flips), sum(r[2] for r in rows) / n))
print('  stalls %d; main-loop frames >= 33 ms %d, >= 50 ms %d (worst %.0f)' %
      (stalls, sum(1 for x in long_frames if x >= 33), sum(1 for x in long_frames if x >= 50), max(long_frames) if long_frames else 0))
if any(r[3] for r in rows):
    # A thread missing from a second's top list was under the cut there: averaged over the seconds it showed.
    top = sorted(threads.items(), key=lambda kv: -sum(kv[1]) / n)[:10]
    print('  process cpu %.0f%%; threads (avg over the window): %s' %
          (sum(r[3] for r in rows) / n, ', '.join('%s %.0f%%' % (k, sum(v) / n) for k, v in top)))
if gpu_busy:
    gb = sorted(gpu_busy)
    print('  gpu busy avg %.0f ms/s (%.1f%%), p95 %.0f, max %.0f; %.2f ms a flip' %
          (sum(gb) / len(gb), sum(gb) / len(gb) / 10.0, gb[min(len(gb) - 1, int(0.95 * len(gb)))], gb[-1],
           sum(gb) / max(1.0, flips)))
loads = [l.split('loading: ', 1)[1].strip() for l in lines if l.startswith('[bbhost] loading: ') and 'begins' not in l and 'quick re-entry' not in l]
if loads:
    print('  loads: ' + '; '.join(loads[:8]))
for key in ('gpu: memory by heap', 'gpu: memory by site'):
    last = [l for l in lines if key in l]
    if last:
        print('  ' + last[-1].split('] ', 1)[-1].strip()[:400])

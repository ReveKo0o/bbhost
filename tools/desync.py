#!/usr/bin/env python3
"""Compare where two bbhost instances think the same characters are.

Both logs carry `np pos <epoch ms> me x y z | <slot> <handle> x y z ...` once a
second (BBHOST_NP_TEST=probe). One instance's own position ("me") is matched
against the other instance's view of it, and the divergence over time is
printed as a table plus a summary; the slot on the far side is picked as the
one whose track is closest overall, unless --slot says which.

    tools/desync.py build/pair-host/run.log build/pair-guest/run.log

prints host-me vs guest's view and guest-me vs host's view. Samples are paired
by wall clock (the nearest line within --window ms, default 600). A "desync"
here is a difference beyond --threshold metres (default 0.5) that lasts
--persist samples (default 3).
"""
import argparse
import math
import re
import sys

LINE = re.compile(r"np pos (\d+) me (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)(.*)")
SLOT = re.compile(r"\| (\d+) ([0-9a-f]{8}) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)")


def parse(path):
    """-> list of (ms, (x,y,z), {slot: (x,y,z)})"""
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if not m:
                continue
            ms = int(m.group(1))
            me = tuple(float(m.group(i)) for i in (2, 3, 4))
            slots = {}
            for s in SLOT.finditer(m.group(5)):
                slots[int(s.group(1))] = tuple(float(s.group(i)) for i in (3, 4, 5))
            out.append((ms, me, slots))
    return out


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def nearest(samples, ms, window):
    """binary search for the sample nearest in time"""
    lo, hi = 0, len(samples)
    while lo < hi:
        mid = (lo + hi) // 2
        if samples[mid][0] < ms:
            lo = mid + 1
        else:
            hi = mid
    best = None
    for k in (lo - 1, lo):
        if 0 <= k < len(samples) and abs(samples[k][0] - ms) <= window:
            if best is None or abs(samples[k][0] - ms) < abs(samples[best][0] - ms):
                best = k
    return best


def compare(own, other, slot, window, threshold, persist, label):
    pairs = []
    for ms, me, _ in own:
        k = nearest(other, ms, window)
        if k is None:
            continue
        view = other[k][2]
        if slot is None:
            continue
        if slot not in view:
            continue
        pairs.append((ms, me, view[slot], dist(me, view[slot])))
    if not pairs:
        print(f"{label}: no paired samples (slot {slot})")
        return
    ds = [p[3] for p in pairs]
    ds_sorted = sorted(ds)
    p50 = ds_sorted[len(ds) // 2]
    p95 = ds_sorted[int(len(ds) * 0.95)]
    worst = max(pairs, key=lambda p: p[3])
    runs, run, start = [], 0, None
    for ms, _, _, d in pairs:
        if d > threshold:
            run += 1
            start = start or ms
            if run == persist:
                runs.append(start)
        else:
            run, start = 0, None
    t0 = pairs[0][0]
    print(f"{label}: slot {slot}, {len(pairs)} paired samples over {(pairs[-1][0]-t0)/1000:.0f} s; "
          f"distance p50 {p50:.2f} m, p95 {p95:.2f} m, max {worst[3]:.2f} m at +{(worst[0]-t0)/1000:.0f} s; "
          f"{len(runs)} desync episode(s) (> {threshold} m for {persist} s)")
    for ms in runs[:10]:
        print(f"   episode from +{(ms-t0)/1000:.0f} s")
    # How much of the distance is plain delay: shift the far side's view in time and find the lag
    # at which the tracks agree best (the far side draws us where we were `lag` ago).
    # (Only meaningful at a few samples a second, BBHOST_NP_POS_HZ=5; at 1 Hz the nearest sample is
    # the same one for every lag under half a second.)
    gaps = sorted(b[0] - a[0] for a, b in zip(own, own[1:]) if b[0] > a[0])
    interval = gaps[len(gaps) // 2] if gaps else 1000
    scan_window = max(interval // 2 + 1, 60)
    scores = {}
    for tenths in range(-30, 31):
        lag = tenths * 100
        ds_l = []
        for ms, me, _ in own:
            k = nearest(other, ms + lag, scan_window)
            if k is None or slot not in other[k][2]:
                continue
            ds_l.append(dist(me, other[k][2][slot]))
        if len(ds_l) >= 5:
            scores[lag] = sum(ds_l) / len(ds_l)
    moving = [p for p in pairs if p[3] > 0.05]
    if scores:
        best_lag = min(scores, key=scores.get)
        print(f"   samples every {interval} ms; mean distance {scores.get(0, float('nan')):.2f} m as logged, "
              f"{scores[best_lag]:.2f} m when the far side is read {best_lag/1000:+.1f} s later "
              f"(the far side draws us where we were that long ago); "
              f"{len(moving)} of {len(pairs)} samples differ by more than 5 cm")
    step = max(1, len(pairs) // 20)
    print("   t(s)    own x y z              seen x y z             m")
    for ms, me, seen, d in pairs[::step]:
        print(f"   {(ms-t0)/1000:6.0f}  {me[0]:8.2f} {me[1]:7.2f} {me[2]:8.2f}   {seen[0]:8.2f} {seen[1]:7.2f} {seen[2]:8.2f}  {d:5.2f}")


def pick_slot(own, other, window):
    """the far-side slot whose track is closest to our own positions"""
    best, best_d = None, None
    for slot in range(7):
        tot, n = 0.0, 0
        for ms, me, _ in own:
            k = nearest(other, ms, window)
            if k is None or slot not in other[k][2]:
                continue
            tot += dist(me, other[k][2][slot])
            n += 1
        if n >= 5:
            avg = tot / n
            if best is None or avg < best_d:
                best, best_d = slot, avg
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--window", type=int, default=600)
    ap.add_argument("--threshold", type=float, default=0.5)
    ap.add_argument("--persist", type=int, default=3)
    ap.add_argument("--slot-a", type=int, help="slot in B's log that is A")
    ap.add_argument("--slot-b", type=int, help="slot in A's log that is B")
    args = ap.parse_args()
    a, b = parse(args.a), parse(args.b)
    if not a or not b:
        print(f"no np pos lines: {args.a} {len(a)}, {args.b} {len(b)}")
        return 1
    sa = args.slot_a if args.slot_a is not None else pick_slot(a, b, args.window)
    sb = args.slot_b if args.slot_b is not None else pick_slot(b, a, args.window)
    compare(a, b, sa, args.window, args.threshold, args.persist, f"{args.a} as seen by {args.b}")
    compare(b, a, sb, args.window, args.threshold, args.persist, f"{args.b} as seen by {args.a}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

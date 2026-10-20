#!/usr/bin/env python3
"""Measure the decomp surface: which of the eboot's functions a
world run executes, and how many of them have names.

    tools/surface.py build/samples-surface [--flips 2500:5000] [--top 30] [--json out.json]

Input is what BBHOST_SAMPLE wrote (host/sampler.h): one stack a line, guest
frames as `g<Binary Ninja address>`. Function boundaries come from a local
symbols/1.09-functions.txt.gz (every function start Binary Ninja found, one
hex address a line) and names from symbols/1.09.json ({"functions":
{"0x...": name}}); neither is distributed with bbhost. A sampled
address is attributed to the function whose start precedes it.

Prints one row: functions in the
eboot / seen by the sampler (on top of a stack, anywhere on a stack) /
named among the seen; then the busiest functions, named and unnamed, per
thread.
"""
import argparse
import bisect
import collections
import gzip
import json
import os
import re
import sys

ap = argparse.ArgumentParser()
ap.add_argument('prefix', help='BBHOST_SAMPLE_OUT (without .txt)')
ap.add_argument('--flips', help='first:last flip to include')
ap.add_argument('--top', type=int, default=25)
ap.add_argument('--json', help='write the numbers and the seen set here')
ap.add_argument('--symbols', default=os.path.join(os.path.dirname(__file__), '..', 'symbols'))
a = ap.parse_args()

first, last = 0, 1 << 62
if a.flips:
    f0, f1 = a.flips.split(':')
    first, last = int(f0 or 0), int(f1 or 1 << 62)

with gzip.open(os.path.join(a.symbols, '1.09-functions.txt.gz'), 'rt') as f:
    starts = sorted(int(l, 16) for l in f if l and not l.startswith('#'))
names = json.load(open(os.path.join(a.symbols, '1.09.json')))['functions']
named = {int(k, 16): v for k, v in names.items() if not re.fullmatch(r'j_sub_[0-9a-f]+', v)}
ours = set()
ours_path = os.path.join(a.symbols, '1.09-ours.txt')
if os.path.exists(ours_path):
    for l in open(ours_path):
        l = l.strip()
        if l and not l.startswith('#'):
            ours.add(int(l.split()[0], 16))
image_end = starts[-1] + 0x10000


def function_of(addr):
    i = bisect.bisect_right(starts, addr) - 1
    return starts[i] if i >= 0 and addr < image_end else None


threads = {}
self_count = collections.Counter()          # function -> samples on top
incl_count = collections.Counter()          # function -> samples anywhere on the stack
per_thread_self = collections.defaultdict(collections.Counter)
per_thread_seen = collections.defaultdict(set)
samples = 0
guest_samples = 0
for line in open(a.prefix + '.txt'):
    if line.startswith('#thread'):
        _, tid, name = line.split(None, 2)
        threads[int(tid)] = name.strip()
        continue
    parts = line.split()
    if len(parts) < 3:
        continue
    flip, tid = int(parts[0]), int(parts[1])
    if not first <= flip <= last:
        continue
    samples += 1
    frames = parts[2:]
    guest = [function_of(int(x[1:], 16)) for x in frames if x[0] == 'g']
    guest = [g for g in guest if g is not None]
    if not guest:
        continue
    guest_samples += 1
    name = threads.get(tid, str(tid))
    if frames[0][0] == 'g':
        self_count[guest[0]] += 1
        per_thread_self[name][guest[0]] += 1
    for g in set(guest):
        incl_count[g] += 1
        per_thread_seen[name].add(g)

seen_top = set(self_count)
seen_any = set(incl_count)
named_top = sum(1 for g in seen_top if g in named)
named_any = sum(1 for g in seen_any if g in named)


def label(g):
    return named.get(g, 'sub_%x' % g)


print('functions in the eboot: %d (Binary Ninja), %d named (not sub_/j_sub_)' % (len(starts), len(named)))
print('samples: %d, %d with guest frames, flips %s' % (samples, guest_samples, a.flips or 'all'))
print('seen on top of a stack: %d functions (%d named, %.1f%%)' % (len(seen_top), named_top, 100.0 * named_top / max(1, len(seen_top))))
print('seen anywhere on a stack: %d functions (%d named, %.1f%%)' % (len(seen_any), named_any, 100.0 * named_any / max(1, len(seen_any))))
ours_seen = sorted(seen_any & ours)
print('ours among the seen: %d of %d listed (%s)' % (len(ours_seen), len(ours), ', '.join('0x%x' % g for g in ours_seen)))
print()
print('| Functions in the eboot | On a stack in the walk | Named | Ours |')
print('|---|---|---|---|')
print('| %d | %d | %d (%.1f%%) | %d |' % (len(starts), len(seen_any), named_any, 100.0 * named_any / max(1, len(seen_any)), len(ours_seen)))
print()
print('| Threads | Functions on top | Functions on a stack |')
print('|---|---|---|')
for t in sorted(per_thread_seen, key=lambda t: -len(per_thread_seen[t])):
    print('| %s | %d | %d |' % (t, len(per_thread_self[t]), len(per_thread_seen[t])))
print()
print('-- busiest on top (all threads)')
for g, n in self_count.most_common(a.top):
    print('  %5.1f%%  %-40s 0x%x' % (100.0 * n / max(1, guest_samples), label(g), g))
print('-- busiest anywhere on a stack')
for g, n in incl_count.most_common(a.top):
    print('  %5.1f%%  %-40s 0x%x' % (100.0 * n / max(1, guest_samples), label(g), g))
print('-- busiest unnamed on top (what to name first)')
for g, n in [(g, n) for g, n in self_count.most_common() if g not in named][:a.top]:
    print('  %5.1f%%  sub_%x' % (100.0 * n / max(1, guest_samples), g))

if a.json:
    json.dump({
        'functions': len(starts), 'named': len(named), 'samples': samples, 'guest_samples': guest_samples,
        'seen_top': len(seen_top), 'seen_top_named': named_top, 'seen_any': len(seen_any), 'seen_any_named': named_any,
        'threads': {t: {'top': len(per_thread_self[t]), 'any': len(per_thread_seen[t])} for t in per_thread_seen},
        'ours_seen': ['0x%x' % g for g in ours_seen],
        'seen': sorted('0x%x' % g for g in seen_any),
    }, open(a.json, 'w'), indent=1)

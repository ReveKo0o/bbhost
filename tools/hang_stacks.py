#!/usr/bin/env python3
"""Symbolize the watchdog's thread dumps in a bbhost log (src/host/sampler.cpp).

    tools/hang_stacks.py build/bbhost-run.log [--exe build/bbhost] [--all]

A stalled run logs "watchdog: no flip for N s ... threads" and a line per
thread: guest frames as Binary Ninja addresses (g...), ours as bbhost+offset,
a library's as module!export+offset. This names our frames with addr2line
against the binary that ran (--exe; a Deck or Windows run's own copy, not a
newer build) and folds threads whose stacks are the same into one line with a
count. Threads parked in the usual waits (a guest thread in its condition
wait, a worker on its queue) still print; --all keeps every thread on a line
of its own.
"""
import argparse
import collections
import re
import subprocess

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--exe', default='build/bbhost', help='the binary that wrote the log')
ap.add_argument('--all', action='store_true', help='one line per thread, no folding')
a = ap.parse_args()

dumps = []  # (header, [(tid, name, [frames])])
cur = None
for line in open(a.log, errors='replace'):
    line = line.rstrip('\n')
    m = re.search(r'watchdog: (no flip for .* threads.*)$', line)
    if m:
        cur = (m[1], [])
        dumps.append(cur)
        continue
    if cur is None:
        continue
    # "  TID NAME: frames" - a name may hold colons of its own (GXWorker:1).
    m = re.match(r'^\[bbhost\]\s{3}(\d+) (.*)$', line)
    if not m or ': ' not in m[2]:
        cur = None
        continue
    name, frames = m[2].rsplit(': ', 1) if not m[2].endswith(': (no answer)') else (m[2][:-len(': (no answer)')], '(no-answer)')
    cur[1].append((int(m[1]), name, frames.split()))


def strip_params(fn):
    """The parameter list at the end of a demangled name is noise here."""
    if fn.endswith(' const'):
        fn = fn[:-6]
    if not fn.endswith(')'):
        return fn
    depth = 0
    for i in range(len(fn) - 1, -1, -1):
        depth += {')': 1, '(': -1}.get(fn[i], 0)
        if depth == 0:
            return fn[:i]
    return fn


ours = sorted({int(f.split('+0x')[1], 16) for _, threads in dumps for _, _, frames in threads
               for f in frames if f.startswith('bbhost+0x')})
names = {}
if ours:
    # Return addresses: the call is the byte before.
    r = subprocess.run(['addr2line', '-f', '-C', '-e', a.exe] + ['%x' % (x - 1) for x in ours], capture_output=True, text=True)
    out = r.stdout.splitlines()
    for i, x in enumerate(ours):
        fn = out[2 * i] if 2 * i < len(out) else '??'
        fn = strip_params(fn)
        names[x] = fn if fn != '??' else 'bbhost+%x' % x


def show(f):
    if f.startswith('bbhost+0x'):
        return names.get(int(f.split('+0x')[1], 16), f)
    if f.startswith('libc.so.6+') or f.startswith('libpthread'):
        return None  # the futex and syscall frames under a named wait
    return f


for header, threads in dumps:
    print('== ' + header)
    groups = collections.OrderedDict()
    for tid, name, frames in threads:
        stack = ' < '.join(s for s in (show(f) for f in frames) if s)
        key = (name if a.all else re.sub(r'[:\d]+$', '', name), stack)
        if a.all:
            key = (tid,) + key
        groups.setdefault(key, []).append(tid)
    for key, tids in groups.items():
        name, stack = key[-2], key[-1]
        count = '' if len(tids) == 1 else ' x%d' % len(tids)
        print('  %s%s [%s]: %s' % (name, count, ','.join(map(str, tids[:4])) + (',...' if len(tids) > 4 else ''), stack))

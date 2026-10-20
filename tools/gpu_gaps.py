#!/usr/bin/env python3
"""Name the GPU time between profiled draws (BBHOST_GPU_PROFILE=2), or the
command processor's in-place recording sites (BBHOST_CMD_CENSUS=1).

    tools/gpu_gaps.py LOG [--exe build/bbhost] [--top 25] [--windows] [--census]

Each 300-flip report's "gpu gaps:" line charges the GPU time between two
profiled draws or dispatches (and before the first and after the last of a
command buffer) to the recording sites - g_cmd()'s and rec()'s callers, as
offsets in bbhost's image - that recorded in that gap. This sums every line
of LOG, names the sites with addr2line, and prints the gaps by total time.
With --census it reads the "cmd census:" lines instead (the recorder's
report: who still records in place, after waiting for the command stream,
and how often per flip) and prints the sites by their mean per flip. A
Windows log takes --exe build/win/bbhost-win-<rev>.debug.exe --windows (the
offsets are added to the PE's link base).
"""
import argparse
import collections
import re
import subprocess

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--exe', default='build/bbhost')
ap.add_argument('--top', type=int, default=25)
ap.add_argument('--windows', action='store_true', help='offsets are from a Windows run: add the PE link base')
ap.add_argument('--census', action='store_true', help='the "cmd census:" lines (BBHOST_CMD_CENSUS=1) instead of the gaps')
a = ap.parse_args()

total = collections.Counter()
count = collections.Counter()
reports = 0
for line in open(a.log, errors='replace'):
    if a.census:
        if 'cmd census:' not in line:
            continue
        reports += 1
        for m in re.finditer(r' ([0-9a-f]+)=([0-9.]+)/flip', line):
            total[m.group(1)] += float(m.group(2))
        continue
    if 'gpu gaps:' not in line:
        continue
    reports += 1
    for m in re.finditer(r' ([0-9a-f+]+|none)=([0-9.]+)ms\((\d+)\)', line):
        total[m.group(1)] += float(m.group(2))
        count[m.group(1)] += int(m.group(3))
if not reports:
    raise SystemExit('no "cmd census:" lines (run with BBHOST_CMD_CENSUS=1)' if a.census else
                     'no "gpu gaps:" lines (run with BBHOST_GPU_PROFILE=2)')

base = 0
if a.windows:
    out = subprocess.run(['objdump', '-p', a.exe], capture_output=True, text=True).stdout
    m = re.search(r'ImageBase\s+([0-9a-fA-F]+)', out)
    base = int(m.group(1), 16) if m else 0x140000000
sites = sorted({s for k in total for s in k.split('+') if s != 'none'})
names = {}
if sites:
    out = subprocess.run(['addr2line', '-f', '-C', '-s', '-e', a.exe] + ['%x' % (int(s, 16) + base) for s in sites],
                         capture_output=True, text=True).stdout.split('\n')
    for i, s in enumerate(sites):
        fn = out[2 * i] if 2 * i < len(out) else '?'
        loc = out[2 * i + 1] if 2 * i + 1 < len(out) else '?'
        fn = re.sub(r'\(.*', '', fn)
        names[s] = '%s (%s)' % (fn, loc)
if a.census:
    # A site missing from a report's top twelve counts 0 there.
    print('%d reports; in-place recording sites, mean per flip' % reports)
    for site, n in total.most_common(a.top):
        print('%8.1f /flip  %s' % (n / reports, names.get(site, site)))
    raise SystemExit(0)
grand = sum(total.values())
print('%d reports, %.0f ms of GPU time outside profiled draws in all' % (reports, grand))
for key, ms in total.most_common(a.top):
    label = ' + '.join(names.get(s, s) for s in key.split('+'))
    print('%8.0f ms %5.1f%% %8d gaps  %s' % (ms, 100 * ms / grand if grand else 0, count[key], label))

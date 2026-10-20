#!/usr/bin/env python3
"""Summarize the rendering warnings in a run log, one line per class.

    tools/log_health.py build/<run>/run.log [more logs...]

Each class is a way a frame has gone wrong before, as the host logs it; the
count is how many lines the log has (most classes log only their first few
occurrences, so a count at its cap means "at least"), and the first line is
shown. The exit report's own totals (draw failures and their reasons, stale
reads, region copies, level aliases, dummy images) are quoted as they are.
tools/f12_check.py reads a dump the same way.
"""
import re
import sys

CLASSES = [
    ('crash', r'dumped core|Segmentation fault|DL_PANIC from|DL_PANIC [^v]\S*\(\d+\)'),
    ('hang', r'^\[bbhost\] hang: no flip'),
    ('draw failed: pipeline', r'render: pipeline \S+ failed to build'),
    ('no texture (black)', r'render: \S+ binding \d+ \(.*\) has no texture'),
    ('invalid T# (zero)', r'texture: dummy-zero for invalid T#'),
    ('unsupported T#', r'texture: unsupported T#'),
    ('unsupported colour format', r'render: unsupported color format'),
    ('unsupported primitive', r'render: unsupported primitive type'),
    ('T# unmapped', r'texture: T# at 0x[0-9a-f]+ \(\d+ bytes\) not mapped'),
    ('target re-created, contents lost', r'render: target 0x[0-9a-f]+ re-created: .*\(contents lost\)'),
    ('T# larger than its target (clipped)', r'texture: T# \d+x\d+ samples render target .* of only'),
    ('stale upload over a target', r'texture: STALE\? .* render target'),
    ('stale upload over a GPU texture', r'texture: STALE\? .* GPU-written texture'),
    ('shader-written texels lost', r'the GPU-written texels were LOST'),
    ('copy token refused', r'texture: copy token refused'),
    ('target copy left as memory (stale image)', r"render: copy \w+ 0x[0-9a-f]+ -> 0x[0-9a-f]+ 0x[0-9a-f]+ bytes copied as memory, the destination's image"),
    ('target copy: a larger source\'s overlap', r'render: copy \w+ 0x[0-9a-f]+ -> 0x[0-9a-f]+ 0x[0-9a-f]+ bytes, fewer than the \d+x\d+ source holds'),
    ('index/indirect not imported', r'render: (index buffer|indirect args) 0x[0-9a-f]+ not in imported memory'),
    ('descriptor set allocation failed', r'render: descriptor set allocation failed'),
    ('fallback binds differently', r'its fallback binds differently'),
    ('fetch shader unreadable', r'expects a fetch shader .* unreadable'),
    ('validation', r'VUID-|vkvalidation|Validation Error'),
    ('device lost', r'VK_ERROR_DEVICE_LOST|device lost'),
    ('compute compile slow', r'compute shaders? .*(still compiling|took \d{4,} ms)'),
]
TOTALS = [
    r'gpu: dispatches=.*',
    r'gpu: draw failures by reason: .*',
    r'texture: uploads from memory the GPU holds newer bits for.*',
    r'texture: render targets sampled through a smaller T#.*',
    r'texture: copy-image tokens .*',
    r'render: targets=\d+ .*target-copies-refused=.*',
    r'glitch: .*catches.*',
]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        lines = open(path, errors='replace').read().split('\n')
        print('%s: %d lines' % (path, len(lines)))
        for name, rx in CLASSES:
            r = re.compile(rx)
            hits = [l for l in lines if r.search(l)]
            if hits:
                print('  %-38s %6d  %s' % (name, len(hits), hits[0].replace('[bbhost] ', '')[:150]))
        for rx in TOTALS:
            r = re.compile(rx)
            last = [l for l in lines if r.search(l)]
            if last:
                print('  = ' + last[-1].replace('[bbhost] ', '')[:260])


if __name__ == '__main__':
    main()

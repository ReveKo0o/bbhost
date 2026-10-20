#!/usr/bin/env python3
"""Per-thread CPU of any running process over a window, busiest first (Linux):
what BBHOST_FRAME_STATS prints for bbhost, for another program run beside it
(shadPS4, say).

    tools/thread_cpu.py PID [SECONDS]
"""
import os
import sys
import time

pid = sys.argv[1]
window = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0


def snapshot():
    out = {}
    for tid in os.listdir(f'/proc/{pid}/task'):
        try:
            stat = open(f'/proc/{pid}/task/{tid}/stat').read()
        except OSError:
            continue
        name = stat[stat.index('(') + 1:stat.rindex(')')]
        fields = stat[stat.rindex(')') + 2:].split()
        out[tid] = (name, int(fields[11]) + int(fields[12]))  # utime + stime
    return out


a = snapshot()
time.sleep(window)
b = snapshot()
hz = os.sysconf('SC_CLK_TCK')
rows = sorted(((b[t][1] - a[t][1]) * 100.0 / hz / window, b[t][0]) for t in b if t in a)
rows.reverse()
print('total %.0f%% over %.1f s' % (sum(r[0] for r in rows), window))
for pct, name in rows[:16]:
    print('%6.1f%%  %s' % (pct, name))

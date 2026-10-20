#!/usr/bin/env python3
"""Summarise a bbhost run log: unbound imports hit, flips, exit reason.

usage: tools/coverage.py build/bbhost-run.log
"""
import collections
import re
import sys


def main(path):
    stubs = collections.Counter()
    flips = 0
    fakes = collections.Counter()
    dumped = False
    for line in open(path, errors="replace"):
        m = re.search(r"HLE stub #\d+ (\S+)", line)
        if m:
            stubs[m.group(1)] += 1
        if "sceGnmSubmitAndFlip" in line:
            flips += 1
        m = re.search(r"HLE fake: (\S+)", line)
        if m:
            fakes[m.group(1)] += 1
        if "dumped core" in line:
            dumped = True
    print(f"flips logged: {flips}")
    print(f"core dump: {'yes' if dumped else 'no'}")
    if stubs:
        print("unbound imports hit:")
        for name, n in stubs.most_common():
            print(f"  {n:6d}  {name}")
    else:
        print("unbound imports hit: none")
    if fakes:
        print("HLE fakes hit:")
        for name, n in fakes.most_common():
            print(f"  {n:6d}  {name}")
    return 1 if (stubs or dumped) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build/bbhost-run.log"))

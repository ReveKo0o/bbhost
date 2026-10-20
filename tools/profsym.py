#!/usr/bin/env python3
# Symbolize a gperftools CPU profile (BBHOST_CPUPROFILE) without pprof:
#   tools/profsym.py tmp/cp.prof build/bbhost
# Prints self and inclusive time per function. bbhost omits frame pointers, so
# only the self column is reliable. go tool pprof ran out of memory on the
# ~1,300-line /proc/self/maps section these profiles carry.
import bisect, collections, re, struct, subprocess, sys
prof, binary = sys.argv[1], sys.argv[2]
data = open(prof, 'rb').read()
words = struct.unpack_from('<%dQ' % (len(data) // 8), data)
i = 5  # header: 0, 3, 0, period, 0
samples = []
while i + 2 < len(words):
    count, depth = words[i], words[i + 1]
    if count == 0 and depth == 1 and words[i + 2] == 0:
        i += 3
        break
    samples.append((count, words[i + 2:i + 2 + depth]))
    i += 2 + depth
maps_text = data[i * 8:].decode('latin-1')
maps = []
for line in maps_text.splitlines():
    m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \S+\s*(.*)', line)
    if m and 'x' in m.group(3) and m.group(5).startswith('/'):
        maps.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(4), 16), m.group(5)))
maps.sort()
starts = [m[0] for m in maps]
base_of = {}
for line in maps_text.splitlines():
    m = re.match(r'([0-9a-f]+)-[0-9a-f]+ \S+ ([0-9a-f]+) \S+ \S+\s*(/.*)', line)
    if m and int(m.group(2), 16) == 0 and m.group(3) not in base_of:
        base_of[m.group(3)] = int(m.group(1), 16)
symtabs = {}
def symtab(path):
    if path in symtabs:
        return symtabs[path]
    addrs, names = [], []
    for flags in (['-C', '--defined-only'], ['-C', '-D', '--defined-only']):
        try:
            out = subprocess.run(['nm', *flags, path], capture_output=True, text=True, timeout=120).stdout
        except Exception:
            out = ''
        for l in out.splitlines():
            parts = l.split(' ', 2)
            if len(parts) == 3 and parts[1] in 'tTwW' and parts[0]:
                addrs.append(int(parts[0], 16)); names.append(parts[2])
        if addrs:
            break
    order = sorted(range(len(addrs)), key=lambda k: addrs[k])
    symtabs[path] = ([addrs[k] for k in order], [names[k] for k in order])
    return symtabs[path]
cache = {}
def name(pc):
    if pc in cache:
        return cache[pc]
    k = bisect.bisect_right(starts, pc) - 1
    if k < 0 or pc >= maps[k][1]:
        cache[pc] = '[guest or unknown]'
        return cache[pc]
    start, end, off, path = maps[k]
    rel = pc - base_of.get(path, start - off)
    addrs, names = symtab(path)
    j = bisect.bisect_right(addrs, rel) - 1
    short = path.rsplit('/', 1)[-1]
    n = names[j] if j >= 0 else '?'
    n = re.sub(r'\(.*', '', n) if len(n) > 120 else n
    cache[pc] = n if short == 'bbhost' else f'{short}!{n}'
    return cache[pc]
total = sum(c for c, _ in samples)
flat, cum = collections.Counter(), collections.Counter()
for c, pcs in samples:
    if not pcs:
        continue
    flat[name(pcs[0])] += c
    seen = set()
    for idx, pc in enumerate(pcs):
        n = name(pc if idx == 0 else pc - 1)
        if n not in seen:
            seen.add(n); cum[n] += c
print(f'samples={total} period_us={words[3]}')
print('--- self')
for n, c in flat.most_common(35):
    print(f'{100.0 * c / total:5.1f}%  {n[:150]}')
print('--- inclusive')
for n, c in cum.most_common(45):
    print(f'{100.0 * c / total:5.1f}%  {n[:150]}')

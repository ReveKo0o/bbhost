#!/usr/bin/env python3
"""Every host function the guest can call must carry GUEST_ABI.

On Linux the attribute is empty, so a missing one is invisible until the
Windows build passes rcx where the SysV caller put rdi (the 6.3 wine run
found six: gx_hook and five prologue hooks). This walks the registrations
(REG/REG_RAW/register_hle_fn*), the thunk wrappers (thunk_wrap,
thunk_wrap_capture_frame, hle_wrap_fn) and the prologue-hook installers
(engine_prologue_hook, install_jump_hook) and checks each named function's
definition or declaration for GUEST_ABI. Exit 1 when one lacks it.

    tools/abi_audit.py            (from the repo root)
"""
import glob
import re
import sys

files = glob.glob('src/**/*.cpp', recursive=True) + glob.glob('src/**/*.h', recursive=True)
src = {f: open(f).read() for f in files}

PATTERNS = [
    r'thunk_wrap(?:_capture_frame)?\(reinterpret_cast<void\*>\(&?([A-Za-z_:0-9]+)\)\)',
    r'\bREG(?:_RAW)?\("[^"]+",\s*([A-Za-z_:0-9]+)\)',
    r'register_hle_fn(?:_raw)?\("[^"]+",\s*reinterpret_cast<void\*>\(&?([A-Za-z_:0-9]+)\)\)',
    r'hle_wrap_fn\(reinterpret_cast<void\*>\(&?([A-Za-z_:0-9]+)\)\)',
    r'(?:engine_prologue_hook|install_jump_hook)\([^;]*?reinterpret_cast<void\*>\(&?([A-Za-z_:0-9]+)\)\)',
]
# Asm entries and data the registrations name; their ABI is the asm's.
KNOWN_ASM = {'hle_printf', 'hle_snprintf', 'hle_sprintf', 'hle_sprintf_s', 'hle_fprintf', 'hle_fscanf', 'hle_sscanf',
             'hle_swscanf', 'hle_setjmp_raw', 'hle_longjmp_raw', 'g_dummy_object'}

names = set()
for f, s in src.items():
    for pat in PATTERNS:
        for m in re.finditer(pat, s, re.S):
            names.add((m.group(1), f))

# Definitions and declarations, found line by line: "<type and attributes> name(".
sig_lines = {}
for g, s in src.items():
    for line in s.split('\n'):
        m = re.match(r'^\s*(?:extern "C" )?(?:static |inline )?([A-Za-z_:<>*&, 0-9]+?)\b([A-Za-z_0-9]+)\s*\(', line)
        if not m:
            continue
        head = m.group(1)
        if any(k in head for k in ('REG', 'thunk_wrap', 'register_hle', 'hle_wrap_fn', 'return', '=', 'if', 'else')):
            continue
        sig_lines.setdefault(m.group(2), []).append(head)
bad = []
for name, where in sorted(names):
    base = name.split('::')[-1]
    if base in KNOWN_ASM:
        continue
    heads = sig_lines.get(base)
    if not heads:
        bad.append((name, where, 'no definition found'))
    elif not any('GUEST_ABI' in h for h in heads):
        bad.append((name, where, 'lacks GUEST_ABI'))

# Prologue-hook bodies handed over through variables escape the patterns
# above; their signature is distinctive, so check every one by shape.
for g, s in src.items():
    for m in re.finditer(r'^([^\n]*?)\b([A-Za-z_0-9]+_hook)\(std::uint64_t[^,\n]*,\s*const std::uint64_t\*', s, re.M):
        if 'GUEST_ABI' not in m.group(1) and not m.group(1).strip().startswith('//'):
            bad.append((m.group(2), g, 'prologue hook lacks GUEST_ABI'))

for name, where, why in bad:
    print(f'{where}: {name}: {why}')
print(f'{len(names)} guest-callable functions checked, {len(bad)} problems')
sys.exit(1 if bad else 0)

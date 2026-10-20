#!/usr/bin/env python3
"""The debug menu's text, for translating it (the Debug Menu plugin,
plugins/debug_menu/strings_en.tsv).

The table holds no game text: each row is a string's Binary Ninja address,
a hash of the game's own UTF-16 text there (64-bit FNV-1a over its bytes,
the terminator left out) and the English. This reads the Japanese from your
own eboot to show beside it.

    tools/debug_menu_strings.py list    [--eboot E] [--tsv T]   every string: address, hash, Japanese, English
    tools/debug_menu_strings.py missing [--eboot E] [--tsv T]   the ones without English yet
    tools/debug_menu_strings.py check   [--eboot E] [--tsv T]   every row against the eboot

check fails on a row whose address does not hold the string its hash names,
whose English drops, adds or reorders a printf conversion (%s, %d, %.1f...),
or uses a character the debug font does not have. The eboot is the 1.09
one, decrypted (default: eboot-109-decrypted.bin in the checkout; the
BBHOST_EBOOT environment variable also names it).

The strings are the UTF-16 ones with Japanese in them that the game's code
loads with a lea or that a relocated pointer in its data names.
"""
import argparse
import os
import re
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
BN_BASE = 0x400000
SHA_109 = '941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7'


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def japanese(c):
    return 0x3040 <= c <= 0x30ff or 0x4e00 <= c <= 0x9fff or 0xff00 <= c <= 0xffef or 0x3000 <= c <= 0x303f


# Strings that keep the game's text: replace_text moves every reference to a
# string, and something other than a reader uses these (Binary Ninja
# addresses, end exclusive).
KEEP = [
    (0x4db9d80, 0x4db9fb0, "values the play log uploads (throw, matching, breakdown and heal types, alert states, "
                           "the survival-time name): the server reads them in Japanese"),
    (0x4db4214, 0x4db4216, "the menu font's name, which the game looks up"),
    (0x4d4f354, 0x4d4f480, "debug menu node keys, looked up by name and never shown"),
    (0x4d51080, 0x4d510c0, "debug menu node keys, looked up by name and never shown"),
    (0x4d6f952, 0x4d6f954, "a HUD part's name (F20-00_arms_r), which the game looks up"),
    (0x4d984a0, 0x4d984a2, "the line format of the normal menus' button hints, not the debug menu's"),
    # The tables at 0x5734490 (56 act names) and 0x5734810 (129 env names)
    # become maps from name to function number (sub_1e12720, sub_1e15d30), and
    # SprjChrBehaviorScriptModule looks up every call a character's script
    # makes in them. A name not found is no act, and an env that answers 0:
    # in English every env answered 0 (the scripts' HP query among them), and
    # the player and every enemy died a few seconds into each load.
    (0x4d91d18, 0x4d92bfa, "the names the character scripts (action/script/*.hks) call the game's act and env "
                           "functions by"),
]


def kept(bn):
    return next((why for lo, hi, why in KEEP if lo <= bn < hi), None)


# 【】《》〈〉「」『』≪≫（）［］: the menu marks its values with these.
BRACKETS = {0x3010, 0x3011, 0x300a, 0x300b, 0x3008, 0x3009, 0x300c, 0x300d, 0x300e, 0x300f, 0x226a, 0x226b,
            0xff08, 0xff09, 0xff3b, 0xff3d}

# CP932's readings of JIS codes that JIS's own mapping spells otherwise.
CP932_EXTRA = {0xff5e, 0xff0d, 0xff3c, 0xffe0, 0xffe1, 0xffe2, 0xffe3, 0xffe5, 0x2225, 0x2014}


def text_char(c):
    if c < 0x20 and c not in (9, 10, 13):
        return False
    if 0x20 <= c < 0x7f or c in (9, 10) or 0xa0 <= c <= 0xff or c in CP932_EXTRA or 0xff61 <= c <= 0xff9f:
        return True
    try:
        chr(c).encode('iso2022_jp')
        return True
    except UnicodeEncodeError:
        return False


def plausible(cs):
    """Japanese text rather than code or ASCII read at an odd byte: every
    character in JIS X 0208 (or ASCII, Latin-1), and kana - or kanji that
    are not mostly two printable ASCII bytes, as misread ASCII is."""
    if not cs or not all(text_char(c) for c in cs):
        return False
    if any(0x3040 <= c <= 0x30ff for c in cs):
        return True
    # Full-width brackets no ASCII misread can make: the menu's value markers.
    if any(c in BRACKETS for c in cs):
        return True
    # ASCII read at an odd byte ends in at most one ASCII character (the last
    # one, against the terminator): two or more mean real text ("優先度 %d").
    if sum(1 for c in cs if c < 0x80) >= 2:
        return True
    cjk = [c for c in cs if 0x4e00 <= c <= 0x9fff]
    pairs = sum(1 for c in cjk if 0x20 <= (c & 0xff) < 0x7f and 0x20 <= (c >> 8) < 0x7f)
    return len(cjk) >= 2 and pairs * 2 <= len(cjk)


class Eboot:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        if d[:4] != b'\x7fELF':
            raise SystemExit('%s: not an ELF (the decrypted eboot)' % path)
        phoff, = struct.unpack_from('<Q', d, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
        self.loads, self.dynlib = [], None
        for i in range(phnum):
            typ, flags, off, va, _pa, filesz, memsz, _al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
            if typ == 1:
                self.loads.append((off, va, filesz, flags))
            elif typ == 0x61000000:
                self.dynlib = (off, filesz)

    def off_of(self, va):
        for off, sva, size, _f in self.loads:
            if sva <= va < sva + size:
                return off + va - sva
        return None

    def wstring(self, va, limit=4096):
        """The UTF-16 string at an ELF address; (None, b'') when there is
        none, or no terminator within `limit` characters (a table, not text)."""
        off = self.off_of(va)
        if off is None:
            return None, b''
        end = off
        while end + 1 < len(self.d) and end - off < 2 * limit and (self.d[end] or self.d[end + 1]):
            end += 2
        if end - off >= 2 * limit:
            return None, b''
        raw = self.d[off:end]
        return raw.decode('utf-16-le', 'replace'), raw

    def strings(self):
        """{ELF address: (text, raw, (how, refs))} of the Japanese UTF-16
        strings the code or data refers to."""
        import bisect
        d = self.d
        code_off, code_va, code_size, _f = next(l for l in self.loads if l[3] & 1)
        # Every rip-relative lea's target, and every relocated pointer, with
        # where each one is.
        leas, ptrs = {}, {}
        for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', d[code_off:code_off + code_size]):
            at = code_off + m.start()
            disp, = struct.unpack_from('<i', d, at + 3)
            leas.setdefault(code_va + m.start() + 7 + disp, []).append(code_va + m.start())
        if self.dynlib:
            off, size = self.dynlib
            for k in range(off, off + size - 24, 8):
                r_offset, r_info, r_addend = struct.unpack_from('<QQq', d, k)
                if (r_info & 0xffffffff) == 8 and 0 < r_addend < 0x10000000 and r_offset < 0x10000000:
                    ptrs.setdefault(r_addend, []).append(r_offset)
        candidates = {}
        for va in set(leas) | set(ptrs):
            if va & 1:
                continue
            text, raw = self.wstring(va, 1024)
            if not text or '\ufffd' in text:
                continue
            cs = [ord(c) for c in text]
            off = self.off_of(va)
            # Japanese, every character the font's, and a string's start: the
            # byte before it ends whatever came before.
            if not any(japanese(c) for c in cs) or not all(text_char(c) for c in cs) or off < 1 or d[off - 1] != 0:
                continue
            candidates[va] = (text, raw, cs)
        sure = {va for va, (text, raw, cs) in candidates.items() if len(raw) >= 4 and plausible(cs)}
        # A short word ("毒", "両手") looks like ASCII read at an odd byte; it
        # counts when its reference sits beside one to a string already sure:
        # the same function, or the same table.
        near_code = sorted(x for va in sure for x in leas.get(va, []))
        near_data = sorted(x for va in sure for x in ptrs.get(va, []))

        def beside(sorted_list, x, reach):
            i = bisect.bisect_left(sorted_list, x - reach)
            return i < len(sorted_list) and sorted_list[i] <= x + reach

        # ... and lies among them: in the read-only part, past the code.
        first = min(sure) if sure else 0
        ro = next((l for l in self.loads if not l[3] & 2 and l[1] <= first < l[1] + l[2]), None)
        for va, (text, raw, cs) in candidates.items():
            if va in sure or len(cs) > 8 or not ro or not first <= va < ro[1] + ro[2]:
                continue
            if any(beside(near_code, x, 0x100) for x in leas.get(va, [])) or \
                    any(beside(near_data, x, 0x40) for x in ptrs.get(va, [])):
                sure.add(va)
        out = {}
        for va in sorted(sure):
            text, raw, cs = candidates[va]
            refs = len(leas.get(va, [])) + len(ptrs.get(va, []))
            out[va] = (text, raw, ('lea' if va in leas else 'ptr', refs))
        return out


def read_tsv(path):
    rows = {}
    if not os.path.exists(path):
        return rows
    with open(path, encoding='utf-8', newline='') as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            parts = line.split('\t')
            if len(parts) != 3:
                raise SystemExit('%s:%d: expected address, hash and English separated by tabs' % (path, n))
            rows[int(parts[0], 16)] = (parts[1].lower(), unescape(parts[2]), n)
    return rows


def unescape(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            out.append({'t': '\t', 'n': '\n', 'r': '\r'}.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


def escape(s):
    return s.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n').replace('\r', '\\r')


CONVERSION = re.compile(r'%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|L|z|j|t|I64|I32|I)?[diouxXeEfFgGcCsSpaAn%]')


def conversions(s):
    """The printf conversions that take an argument, in order (%% takes none,
    so the English may add or drop one)."""
    return [c for c in CONVERSION.findall(s) if c != '%%']


def in_font(ch):
    c = ord(ch)
    if 0x20 <= c < 0x7f or 0xa0 <= c <= 0xff or c in (9, 10) or c in CP932_EXTRA or 0xff61 <= c <= 0xff9f:
        return True
    try:
        ch.encode('iso2022_jp')
        return True
    except UnicodeEncodeError:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('mode', choices=('list', 'missing', 'check'))
    ap.add_argument('--eboot', default=os.environ.get('BBHOST_EBOOT', os.path.join(ROOT, 'eboot-109-decrypted.bin')))
    ap.add_argument('--tsv', default=os.path.join(ROOT, 'plugins', 'debug_menu', 'strings_en.tsv'))
    a = ap.parse_args()
    if not os.path.exists(a.eboot):
        print('no eboot at %s (--eboot or BBHOST_EBOOT)' % a.eboot)
        if a.mode != 'check':
            return 1
        # A row for a kept string is wrong whatever the eboot, so a check
        # without one (CI) still fails on it.
        bad = 0
        for bn, (_h, _english, line) in sorted(read_tsv(a.tsv).items()):
            if kept(bn):
                print('%s:%d (%x): must keep the game\'s text - %s' % (os.path.basename(a.tsv), line, bn, kept(bn)))
                bad += 1
        return 1 if bad else 77
    import hashlib
    if hashlib.sha256(open(a.eboot, 'rb').read()).hexdigest() != SHA_109:
        print('%s is not the 1.09 eboot' % a.eboot)
        return 1
    e = Eboot(a.eboot)
    found = e.strings()
    rows = read_tsv(a.tsv)
    if a.mode in ('list', 'missing'):
        for va, (text, raw, (how, refs)) in found.items():
            bn = va + BN_BASE
            row = rows.get(bn)
            if (a.mode == 'missing' and row) or kept(bn):
                continue
            esc = escape(text)
            print('%x\t%016x\t%s\t%s' % (bn, fnv1a64(raw), esc, escape(row[1]) if row else ''))
        done = sum(1 for va in found if va + BN_BASE in rows)
        keep = sum(1 for va in found if kept(va + BN_BASE))
        print('# %d strings, %d in English, %d kept as they are' % (len(found), done, keep), file=sys.stderr)
        return 0
    bad = 0
    for bn, (h, english, line) in sorted(rows.items()):
        text, raw = e.wstring(bn - BN_BASE)
        where = '%s:%d (%x)' % (os.path.basename(a.tsv), line, bn)
        if kept(bn):
            print('%s: must keep the game\'s text - %s' % (where, kept(bn)))
            bad += 1
            continue
        if text is None or '%016x' % fnv1a64(raw) != h:
            print('%s: the eboot has another string there' % where)
            bad += 1
            continue
        if conversions(text) != conversions(english):
            print('%s: conversions %s, the original has %s' % (where, conversions(english), conversions(text)))
            bad += 1
        elif '%' in CONVERSION.sub('', english) and '%' not in CONVERSION.sub('', text):
            print('%s: a %% that is no conversion (write %%%% for a percent sign)' % where)
            bad += 1
        missing = sorted({c for c in english if not in_font(c)})
        if missing:
            print('%s: the debug font has no %s' % (where, ' '.join('U+%04X' % ord(c) for c in missing)))
            bad += 1
    print('%d rows, %d of %d strings in English, %d problems' % (len(rows), sum(1 for b in rows if b - BN_BASE in found),
                                                                len(found), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

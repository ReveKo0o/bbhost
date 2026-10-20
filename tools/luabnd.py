#!/usr/bin/env python3
"""An AI bundle (script/<area>.luabnd.dcx): its programs, goals and globals, and
a disassembly of its compiled Lua 5.0 ("\\x1bLuaP", LuaPlus) functions.

  tools/luabnd.py m24_01_00_00                    # programs and goals
  tools/luabnd.py m23_00_00_00 --globals 209000_battle.lua
  tools/luabnd.py m23_00_00_00 --dis 209000_battle.lua Battle_Activate

The bundle's <area>.luainfo lists the goals the engine can start (id, battle
or logic, the program's function name, a logic goal's interrupt handler);
<area>.luagnl every global its programs define (SETGLOBAL), exactly. An enemy
whose NpcThinkParam goals are registered in neither aiCommon's nor its area's
.luainfo never starts its AI. plugins/randomizer has the C++ of the same.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bbparam  # noqa: E402

OPS = ['MOVE', 'LOADK', 'LOADBOOL', 'LOADNIL', 'GETUPVAL', 'GETGLOBAL', 'GETTABLE', 'SETGLOBAL', 'SETUPVAL', 'SETTABLE',
       'NEWTABLE', 'SELF', 'ADD', 'SUB', 'MUL', 'DIV', 'POW', 'UNM', 'NOT', 'CONCAT', 'JMP', 'EQ', 'LT', 'LE', 'TEST',
       'CALL', 'TAILCALL', 'RETURN', 'FORLOOP', 'TFORLOOP', 'TFORPREP', 'SETLIST', 'SETLISTO', 'CLOSE', 'CLOSURE']


def wide(b, o):
    e = o
    while b[e:e + 2] != b'\0\0':
        e += 2
    return b[o:e].decode('utf-16le')


def goals(b):
    """[(id, battle, logic, name, interrupt)] of a .luainfo."""
    n = struct.unpack_from('<I', b, 8)[0]
    out = []
    for k in range(n):
        gid, bat, log, _pad, no, io = struct.unpack_from('<IBBHQQ', b, 0x10 + k * 0x18)
        out.append((gid, bat, log, wide(b, no), wide(b, io) if io else None))
    return out


def names(b):
    """The globals a .luagnl lists."""
    out = []
    for k in range(len(b) // 8):
        o = struct.unpack_from('<Q', b, k * 8)[0]
        if not o:
            break
        out.append(wide(b, o))
    return out


class Reader:
    def __init__(self, b):
        self.b, self.o = b, 22  # past the header (sizes, a test number)

    def take(self, fmt):
        v = struct.unpack_from('<' + fmt, self.b, self.o)
        self.o += struct.calcsize(fmt)
        return v[0]

    def string(self):
        n = self.take('Q')
        if not n:
            return None
        s = self.b[self.o:self.o + n - 1].decode('latin1')
        self.o += n
        return s

    def function(self):
        """A function and its nested ones: {'k': constants, 'code', 'protos', 'line', 'params'}."""
        self.string()
        line = self.take('i')
        _nups, params, _va, _stack = self.take('B'), self.take('B'), self.take('B'), self.take('B')
        lines = self.take('i')  # (read first: `self.o += f()` would lose what f advances)
        self.o += 4 * lines
        for _ in range(self.take('i')):  # locals
            self.string()
            self.o += 8
        for _ in range(self.take('i')):  # upvalue names
            self.string()
        k = []
        for _ in range(self.take('i')):
            t = self.take('B')
            k.append(self.take('d') if t == 3 else self.string() if t == 4 else None)
        protos = [self.function() for _ in range(self.take('i'))]
        code = [self.take('I') for _ in range(self.take('i'))]
        return {'k': k, 'code': code, 'protos': protos, 'line': line, 'params': params}


def walk(f):
    yield f
    for p in f['protos']:
        yield from walk(p)


def globals_of(data):
    """(set, read): the globals a program's functions SETGLOBAL and GETGLOBAL."""
    sets, gets = set(), set()
    for f in walk(Reader(data).function()):
        for ins in f['code']:
            op, bx = ins & 0x3f, (ins >> 6) & 0x3ffff
            if op in (5, 7):
                (gets if op == 5 else sets).add(f['k'][bx])
    return sets, gets


def disassemble(f, name):
    print('function %s (line %d, %d parameters)' % (name, f['line'], f['params']))
    rk = lambda x: repr(f['k'][x - 250]) if x >= 250 else 'R%d' % x  # noqa: E731
    for pc, ins in enumerate(f['code']):
        op = ins & 0x3f
        a, b, c, bx = (ins >> 24) & 0xff, (ins >> 15) & 0x1ff, (ins >> 6) & 0x1ff, (ins >> 6) & 0x3ffff
        o = OPS[op] if op < len(OPS) else '?%d' % op
        if o in ('GETGLOBAL', 'SETGLOBAL', 'LOADK'):
            arg = 'R%d %r' % (a, f['k'][bx])
        elif o in ('GETTABLE', 'SELF'):
            arg = 'R%d R%d %s' % (a, b, rk(c))
        elif o == 'SETTABLE':
            arg = 'R%d %s %s' % (a, rk(b), rk(c))
        elif o in ('EQ', 'LT', 'LE', 'ADD', 'SUB', 'MUL', 'DIV'):
            arg = '%d %s %s' % (a, rk(b), rk(c))
        elif o == 'JMP':
            arg = '-> %d' % (pc + 1 + bx - 131071)
        elif o == 'CLOSURE':
            arg = 'R%d function %d' % (a, bx)
        else:
            arg = '%d %d %d' % (a, b, c)
        print('  %4d %-9s %s' % (pc, o, arg))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('bundle', help='an area (m24_01_00_00, aicommon) or a .luabnd(.dcx) path')
    ap.add_argument('--app0', default=bbparam.APP0)
    ap.add_argument('--globals', metavar='PROGRAM')
    ap.add_argument('--dis', nargs='+', metavar=('PROGRAM', 'FUNCTION'))
    a = ap.parse_args()
    path = a.bundle if os.path.exists(a.bundle) else os.path.join(a.app0, 'dvdroot_ps4/script', a.bundle + '.luabnd.dcx')
    b = bbparam.bnd4(bbparam.dcx(path))
    if a.globals:
        sets, gets = globals_of(b[a.globals])
        print('defines:', ' '.join(sorted(sets)))
        print('reads:', ' '.join(sorted(gets - sets)))
        return
    if a.dis:
        top = Reader(b[a.dis[0]]).function()
        for pc, ins in enumerate(top['code']):  # a function's name: the global its closure is stored in
            if ins & 0x3f == 34:
                nxt = top['code'][pc + 1]
                name = top['k'][(nxt >> 6) & 0x3ffff] if nxt & 0x3f == 7 else '?'
                if len(a.dis) < 2 or a.dis[1] in name:
                    disassemble(top['protos'][(ins >> 6) & 0x3ffff], name)
        return
    info = next(v for k, v in b.items() if k.lower().endswith('.luainfo'))
    gnl = next(v for k, v in b.items() if k.lower().endswith('.luagnl'))
    programs = sorted(k for k in b if k.endswith('.lua'))
    print('%d programs (%.2f MB), %d globals' % (len(programs), sum(len(b[k]) for k in programs) / 1e6, len(names(gnl))))
    for k in programs:
        print('  %-24s %7d bytes' % (k, len(b[k])))
    print('goals:')
    for gid, bat, log, name, intr in goals(info):
        print('  %7d %-7s %s%s' % (gid, 'battle' if bat else 'logic' if log else 'sub', name, ' / ' + intr if intr else ''))


if __name__ == '__main__':
    main()

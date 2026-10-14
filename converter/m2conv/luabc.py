# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Lua 5.1 bytecode (lua_Number=float, int/size_t=4) model: parse, serialize, inspect."""
import struct
OPS = ['MOVE','LOADK','LOADBOOL','LOADNIL','GETUPVAL','GETGLOBAL','GETTABLE','SETGLOBAL','SETUPVAL','SETTABLE',
       'NEWTABLE','SELF','ADD','SUB','MUL','DIV','MOD','POW','UNM','NOT','LEN','CONCAT','JMP','EQ','LT','LE',
       'TEST','TESTSET','CALL','TAILCALL','RETURN','FORLOOP','FORPREP','TFORLOOP','SETLIST','CLOSE','CLOSURE','VARARG']
OP = {n: i for i, n in enumerate(OPS)}
def op(i): return i & 0x3f
def A(i): return (i >> 6) & 0xff
def C(i): return (i >> 14) & 0x1ff
def B(i): return (i >> 23) & 0x1ff
def Bx(i): return (i >> 14) & 0x3ffff
def mk_ABx(o, a, bx): return o | (a << 6) | (bx << 14)
def mk_ABC(o, a, b, c): return o | (a << 6) | (c << 14) | (b << 23)
class Proto:
    __slots__ = ('source', 'line', 'lastline', 'nups', 'nparams', 'vararg', 'maxstack', 'code', 'k', 'p', 'lineinfo', 'locvars', 'upvals')
class Reader:
    def __init__(s, b, e): s.b, s.o, s.e = b, 0, e
    def u8(s): v = s.b[s.o]; s.o += 1; return v
    def i(s): v = struct.unpack_from(s.e + 'i', s.b, s.o)[0]; s.o += 4; return v
    def u(s): v = struct.unpack_from(s.e + 'I', s.b, s.o)[0]; s.o += 4; return v
    def string(s):
        n = s.u()
        if n == 0: return None
        v = bytes(s.b[s.o:s.o + n - 1]); s.o += n; return v
def read_proto(r):
    p = Proto()
    p.source = r.string(); p.line = r.i(); p.lastline = r.i()
    p.nups, p.nparams, p.vararg, p.maxstack = r.u8(), r.u8(), r.u8(), r.u8()
    p.code = [r.u() for _ in range(r.i())]
    p.k = []
    for _ in range(r.i()):
        t = r.u8()
        if t == 0: p.k.append((0, None))
        elif t == 1: p.k.append((1, r.u8()))
        elif t == 3:
            raw = r.b[r.o:r.o + 4]; r.o += 4
            p.k.append((3, struct.unpack(r.e + 'f', raw)[0], struct.unpack(r.e + 'I', raw)[0]))
        elif t == 4: p.k.append((4, r.string()))
        else: raise ValueError('bad const type %d' % t)
    p.p = [read_proto(r) for _ in range(r.i())]
    p.lineinfo = [r.i() for _ in range(r.i())]
    p.locvars = [(r.string(), r.i(), r.i()) for _ in range(r.i())]
    p.upvals = [r.string() for _ in range(r.i())]
    return p
def load(b):
    assert b[:5] == b'\x1bLuaQ', 'not lua 5.1'
    e = '<' if b[6] == 1 else '>'
    assert b[7:12] == b'\x04\x04\x04\x04\x00', b[7:12]
    r = Reader(b, e); r.o = 12
    return read_proto(r)
class Writer:
    def __init__(s): s.out = bytearray()
    def u8(s, v): s.out.append(v)
    def i(s, v): s.out += struct.pack('<i', v)
    def u(s, v): s.out += struct.pack('<I', v)
    def string(s, v):
        if v is None: s.u(0)
        else: s.u(len(v) + 1); s.out += v + b'\0'
def write_proto(w, p, strip):
    w.string(None if strip else p.source)
    w.i(p.line); w.i(p.lastline)
    for v in (p.nups, p.nparams, p.vararg, p.maxstack): w.u8(v)
    w.i(len(p.code))
    for c in p.code: w.u(c)
    w.i(len(p.k))
    for k in p.k:
        w.u8(k[0])
        if k[0] == 1: w.u8(k[1])
        elif k[0] == 3: w.u(k[2])
        elif k[0] == 4: w.string(k[1])
    w.i(len(p.p))
    for q in p.p: write_proto(w, q, strip)
    if strip: w.i(0); w.i(0); w.i(0)
    else:
        w.i(len(p.lineinfo))
        for x in p.lineinfo: w.i(x)
        w.i(len(p.locvars))
        for n, a, b in p.locvars: w.string(n); w.i(a); w.i(b)
        w.i(len(p.upvals))
        for n in p.upvals: w.string(n)
HEADER_LE = b'\x1bLuaQ\x00\x01\x04\x04\x04\x04\x00'
def dump(p, strip=False):
    w = Writer(); w.out += HEADER_LE; write_proto(w, p, strip); return bytes(w.out)
def norm(p):
    """structural signature without debug info / line numbers"""
    w = Writer(); 
    def wp(q):
        for v in (q.nups, q.nparams, q.vararg, q.maxstack): w.u8(v)
        w.i(len(q.code))
        for c in q.code: w.u(c)
        w.i(len(q.k))
        for k in q.k:
            w.u8(k[0])
            if k[0] == 1: w.u8(k[1])
            elif k[0] == 3: w.u(k[2])
            elif k[0] == 4: w.string(k[1])
        w.i(len(q.p))
        for s in q.p: wp(s)
    wp(p); return bytes(w.out)
def consts(p, out=None):
    out = set() if out is None else out
    for k in p.k:
        if k[0] == 4: out.add(k[1])
    for q in p.p: consts(q, out)
    return out

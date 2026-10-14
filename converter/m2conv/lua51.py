# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Lua 5.1 bytecode endian conversion (Xbox big-endian <-> PC little-endian), optional debug strip."""
import struct
class R:
    def __init__(s, b, e): s.b, s.o, s.e = b, 0, e
    def u8(s): v = s.b[s.o]; s.o += 1; return v
    def i(s): v = struct.unpack_from(s.e + 'i', s.b, s.o)[0]; s.o += 4; return v
    def u(s): v = struct.unpack_from(s.e + 'I', s.b, s.o)[0]; s.o += 4; return v
    def f(s): v = s.b[s.o:s.o + 4]; s.o += 4; return v
    def raw(s, n): v = s.b[s.o:s.o + n]; s.o += n; return v
    def string(s):
        n = s.u()
        return None if n == 0 else s.raw(n)
def read_func(r):
    fn = {}
    fn['source'] = r.string()
    fn['line'] = r.i(); fn['lastline'] = r.i()
    fn['nups'], fn['nparams'], fn['vararg'], fn['maxstack'] = r.u8(), r.u8(), r.u8(), r.u8()
    n = r.i(); fn['code'] = [r.u() for _ in range(n)]
    n = r.i(); ks = []
    for _ in range(n):
        t = r.u8()
        if t == 0: ks.append((0, None))
        elif t == 1: ks.append((1, r.u8()))
        elif t == 3: ks.append((3, r.f()))
        elif t == 4: ks.append((4, r.string()))
        else: raise ValueError('const type %d' % t)
    fn['k'] = ks
    n = r.i(); fn['p'] = [read_func(r) for _ in range(n)]
    n = r.i(); fn['lineinfo'] = [r.i() for _ in range(n)]
    n = r.i(); fn['locvars'] = [(r.string(), r.i(), r.i()) for _ in range(n)]
    n = r.i(); fn['upvals'] = [r.string() for _ in range(n)]
    return fn
class W:
    def __init__(s, e): s.e, s.out = e, bytearray()
    def u8(s, v): s.out.append(v)
    def i(s, v): s.out += struct.pack(s.e + 'i', v)
    def u(s, v): s.out += struct.pack(s.e + 'I', v)
    def string(s, v):
        if v is None: s.u(0)
        else: s.u(len(v)); s.out += v
def write_func(w, fn, src_be, strip):
    w.string(None if strip else fn['source'])
    w.i(fn['line']); w.i(fn['lastline'])
    for k in ('nups', 'nparams', 'vararg', 'maxstack'): w.u8(fn[k])
    w.i(len(fn['code']))
    for c in fn['code']: w.u(c)
    w.i(len(fn['k']))
    for t, v in fn['k']:
        w.u8(t)
        if t == 1: w.u8(v)
        elif t == 3: w.out += (v[::-1] if (src_be != (w.e == '>')) else v)
        elif t == 4: w.string(v)
    w.i(len(fn['p']))
    for p in fn['p']: write_func(w, p, src_be, strip)
    if strip:
        w.i(0); w.i(0); w.i(0)
    else:
        w.i(len(fn['lineinfo']))
        for x in fn['lineinfo']: w.i(x)
        w.i(len(fn['locvars']))
        for n, a, b in fn['locvars']: w.string(n); w.i(a); w.i(b)
        w.i(len(fn['upvals']))
        for n in fn['upvals']: w.string(n)
def convert(b, to_le=True, strip=False):
    assert b[:5] == b'\x1bLuaQ'
    be = b[6] == 0
    r = R(b, '>' if be else '<'); r.o = 12
    fn = read_func(r)
    hdr = bytearray(b[:12]); hdr[6] = 1 if to_le else 0
    w = W('<' if to_le else '>'); w.out += hdr
    write_func(w, fn, be, strip)
    return bytes(w.out), r.o

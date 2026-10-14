# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Havok 5.5 binary packfile endian conversion (BE Xbox 360 -> LE PC) driven by hkClass reflection."""
import struct, pickle, os
HERE = os.path.dirname(os.path.abspath(__file__))
CLASSES, BYNAME = pickle.load(open(__import__('cfg').w('hkclasses.pkl'), 'rb'))
T_SIZE = {1: 1, 2: 1, 3: 1, 4: 1, 5: 2, 6: 2, 7: 4, 8: 4, 9: 8, 10: 8, 11: 4, 12: 16, 13: 16, 14: 48, 15: 48, 16: 48,
          17: 64, 18: 64, 20: 4, 21: 4, 22: 12, 26: 8, 27: 12, 28: 8, 29: 4, 30: 4}
T_WORD = {1: 1, 2: 1, 3: 1, 4: 1, 5: 2, 6: 2, 7: 4, 8: 4, 9: 8, 10: 8, 11: 4, 12: 4, 13: 4, 14: 4, 15: 4, 16: 4, 17: 4, 18: 4, 20: 4, 21: 4, 29: 4, 30: 4}
def class_chain(cobj):
    out = []
    while cobj and cobj in CLASSES:
        out.append(CLASSES[cobj]); cobj = CLASSES[cobj]['parent']
    return out[::-1]
def all_members(cobj):
    m = []
    for c in class_chain(cobj): m += c['members']
    return m
def class_size(cobj): return CLASSES[cobj]['size'] if cobj in CLASSES else 0
def member_size(m):
    t = m['type']
    if t in (24, 31): s = T_SIZE.get(m['sub'], 4)
    elif t == 25: s = class_size(m['cls'])
    elif t == 23: s = 0   # in-place array: unknown here
    else: s = T_SIZE.get(t, 0)
    return s * (m['carray'] or 1)
class Pack:
    def __init__(s, b):
        s.b = bytearray(b)
        s.be = b[17] == 0
        s.e = '>' if s.be else '<'
        h = struct.unpack_from(s.e + 'IIII', b, 0)
        s.layout = b[16:20]
        s.nsec, s.csec, s.coff, s.cnsec, s.cnoff = struct.unpack_from(s.e + '5I', b, 20)
        s.sections = []
        for i in range(s.nsec):
            o = 64 + 48 * i
            tag = b[o:o + 19].split(b'\0')[0]
            vals = struct.unpack_from(s.e + '7I', b, o + 20)
            s.sections.append(dict(tag=tag, hdr=o, start=vals[0], local=vals[1], glob=vals[2], virt=vals[3], exp=vals[4], imp=vals[5], end=vals[6]))
        s.done = bytearray(len(b))      # 1 = converted
    def sw(s, o, w, n=1):
        """swap n items of width w at absolute offset o (in place) unless already converted"""
        for k in range(n):
            p = o + k * w
            if p + w > len(s.b): return
            if s.done[p]: continue
            if w > 1: s.b[p:p + w] = s.b[p:p + w][::-1]
            for q in range(p, p + w): s.done[q] = 1
    def mark(s, o, n):
        for q in range(o, min(o + n, len(s.b))): s.done[q] = 1
    def u32(s, o): return struct.unpack_from(s.e + 'I', s.b, o)[0]
    def convert(s, log):
        if not s.be: return bytes(s.b)
        b = s.b; e = s.e
        # header
        s.sw(0, 4, 4); b[17] = 1; s.mark(16, 4); s.sw(20, 4, 5); s.mark(40, 16); s.sw(56, 4, 2)
        secs = s.sections
        for sc in secs: s.mark(sc['hdr'], 20); s.sw(sc['hdr'] + 20, 4, 7)
        # fixup tables (read BE values first)
        cn = secs[s.cnsec]; classname_at = {}
        cs = cn['start']; o = cs
        while o + 5 <= cs + cn['local']:
            sig = s.u32(o)
            if sig == 0xffffffff: break
            if b[o + 4] != 9: break
            ne = b.index(b'\0', o + 5); classname_at[o + 5 - cs] = b[o + 5:ne].decode('latin1')
            s.sw(o, 4); s.mark(o + 4, ne + 1 - o - 4); o = ne + 1
        s.mark(cs, cn['end'])
        self_locals = {}
        objs = []
        for si, sc in enumerate(secs):
            st = sc['start']
            loc = [(s.u32(st + sc['local'] + 8 * k), s.u32(st + sc['local'] + 8 * k + 4)) for k in range((sc['glob'] - sc['local']) // 8)]
            glo = [struct.unpack_from(e + '3I', b, st + sc['glob'] + 12 * k) for k in range((sc['virt'] - sc['glob']) // 12)]
            vir = [struct.unpack_from(e + '3I', b, st + sc['virt'] + 12 * k) for k in range((sc['exp'] - sc['virt']) // 12)]
            s.sw(st + sc['local'], 4, (sc['end'] - sc['local']) // 4)
            sc['L'] = {a: c for a, c in loc if a != 0xffffffff}
            sc['G'] = {a: (x, c) for a, x, c in glo if a != 0xffffffff}
            for a, x, c in vir:
                if a != 0xffffffff: objs.append((si, a, classname_at.get(c, '?')))
        s.secs = secs
        unknown = set()
        for si, a, cname in objs:
            if cname not in BYNAME: unknown.add(cname); continue
            s.conv_struct(si, secs[si]['start'] + a, BYNAME[cname][0], log, 0)
        if unknown: log.append('havok: classes without reflection: %s' % sorted(unknown))
        # report unconverted non-zero bytes in data sections
        rest = 0
        for sc in secs:
            if sc['tag'] != b'__data__': continue
            for q in range(sc['start'], sc['start'] + sc['local']):
                if not s.done[q] and b[q]: rest += 1
        s.unconverted = rest
        return bytes(b)
    def target(s, si, absaddr):
        sc = s.secs[si]; rel = absaddr - sc['start']
        if rel in sc['L']: return si, sc['start'] + sc['L'][rel]
        if rel in sc['G']:
            x, c = sc['G'][rel]; return x, s.secs[x]['start'] + c
        return None, None
    def conv_value(s, si, o, m, log, depth):
        t = m['type']; n = m['carray'] or 1
        if t in T_WORD:
            w = T_WORD[t]; cnt = (T_SIZE[t] // w) * n; s.sw(o, w, cnt)
            if t == 29:   # cstring
                for k in range(n):
                    ts, to = s.target(si, o + 4 * k)
                    if to is not None: s.mark(to, s.b.index(b'\0', to) + 1 - to)
            if t == 20 and m['sub'] == 25 and m['cls']:      # pointer to struct (non-virtual)
                for k in range(n):
                    ts, to = s.target(si, o + 4 * k)
                    if to is not None and not s.done[to]: s.conv_struct(ts, to, m['cls'], log, depth + 1)
            if t == 20 and m['sub'] == 29:
                for k in range(n):
                    ts, to = s.target(si, o + 4 * k)
                    if to is not None: s.mark(to, s.b.index(b'\0', to) + 1 - to)
        elif t in (24, 31):
            w = T_SIZE.get(m['sub'], 4); s.sw(o, w, n)
        elif t == 25:
            sz = class_size(m['cls'])
            for k in range(n): s.conv_struct(si, o + k * sz, m['cls'], log, depth + 1)
        elif t in (22, 26, 27):
            base = o + (4 if t == 27 else 0)
            if t == 27: s.sw(o, 4)
            s.sw(base, 4, 3 if t == 22 else 2)
            cnt = struct.unpack_from('<I', s.b, base + 4)[0]     # already swapped
            ts, to = s.target(si, base)
            if to is None or cnt == 0: return
            if t == 27:
                log.append('havok: homogeneous array not handled'); return
            sub = m['sub']
            em = dict(type=sub, sub=0, cls=m['cls'], carray=0, flags=0, off=0)
            if sub == 25: esz = class_size(m['cls'])
            else: esz = T_SIZE.get(sub, 0)
            if esz == 0: log.append('havok: array subtype %d' % sub); return
            for k in range(min(cnt, 10000000)):
                s.conv_value(ts, to + k * esz, em, log, depth + 1)
        elif t == 28:   # variant: object ptr + class ptr
            s.sw(o, 4, 2)
        elif t == 23: log.append('havok: inplace array %s' % m['name'])
    def conv_struct(s, si, o, cobj, log, depth):
        if depth > 64: return
        for m in all_members(cobj):
            s.conv_value(si, o + m['off'], m, log, depth)

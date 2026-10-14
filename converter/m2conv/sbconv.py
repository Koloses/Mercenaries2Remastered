# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Xbox (BE) -> PC (LE) SoundBank (type 0x9f8bca10) converter.
Default: every 32-bit word byte-reversed; byte/u16 fields listed per structure."""
import struct
def be32(b, o): return struct.unpack_from('>I', b, o)[0]
class Conv:
    def __init__(s, b):
        s.b = b; s.o = bytearray(len(b)); s.done = bytearray(len(b)); s.ctx = {}
    def w(s, o, kind, ctx=''):
        x = s.b[o:o + 4]
        if len(x) < 4: s.o[o:o + len(x)] = x; return
        if kind == 4 and 0 < x[0] <= 0x20 and max(x) <= 0x20 and x[1] <= 1: kind = 1   # u8 field + pad (as u32/float it would be absurd)
        if kind == 4: y = x[::-1]
        elif kind == 2: y = x[1::-1] + x[3:1:-1]
        elif kind == 1: y = x
        elif kind == 21: y = x[1::-1] + x[2:4]          # u16 + 2 bytes
        else: raise ValueError(kind)
        s.o[o:o + 4] = y; s.done[o:o + 4] = b'\1' * 4; s.ctx[o] = ctx
    def words(s, a, e, kind=4, ctx=''):
        for o in range(a, e - 3, 4): s.w(o, kind, ctx)
def convert(b):
    c = Conv(b)
    c.w(0, 1, 'hdr'); c.w(4, 4, 'hdr'); c.w(8, 2, 'hdr'); c.words(12, 0x20, 4, 'hdr')
    n, k = struct.unpack_from('>HH', b, 8)
    oA, oB, oC, oT = struct.unpack_from('>4I', b, 16)
    B = struct.unpack_from('>%dI' % n, b, oB)
    c.words(oB, oC, 4, 'Btab')
    for i in range(n):                                    # sound records
        s = 0x20 + B[i]; e = 0x20 + B[i + 1] if i + 1 < n else oB
        ext = b[s + 12]
        c.words(s, e, 4, 'snd')
        c.w(s + 12, 1, 'snd'); c.w(s + 20, 1, 'snd')
        hl = 11 + (15 if ext else 2)
        if ext: c.w(s + 44, 1, 'sndx'); c.w(s + 72, 1, 'sndx')
        for o in range(s + 4 * hl, e, 12): c.w(o + 4, 2, 'snd.el')
    T = list(struct.unpack_from('>%dI' % k, b, oT)); c.words(oT, len(b), 4, 'Ttab')
    srt = sorted(T) + [oT - oC]
    for t in T:
        s = oC + t; L = srt[srt.index(t) + 1] - t
        O0 = struct.unpack_from('>7I', b, s + 0x28) if b[s + 5] else (0x18,)*7
        for o in range(s, s + L - 3, 4):
            r = o - s; reg = sum(1 for x in O0 if r >= x)
            c.w(o, 4, 'ev.r%d+%x' % (reg, r - (O0[reg - 1] if reg else 0)))
        c.w(s + 4, 1, 'ev.hdr')
        if b[s + 5] == 0:                                  # simple event
            c.w(s + 20, 2, 'ev.simple'); fix_simple(b, s, c)
            continue
        c.w(s + 16, 1, 'ev.hdr'); c.w(s + 20, 1, 'ev.hdr')
        O = struct.unpack_from('>7I', b, s + 0x28)
        # R1 subregion [O5, O6) has a byte field at +12 ? (learned)
        nodes = struct.unpack_from('>%dI' % ((O[4] - O[3]) // 4), b, s + O[3])
        r2, r2e = s + O[2], s + O[3]
        ns = sorted(nodes) + [O[3] - O[2]]
        for nt in nodes:
            ns_ = r2 + nt; ne = r2 + ns[ns.index(nt) + 1]
            conv_node(c, b, ns_, ne)
    return bytes(c.o), c
def conv_node(c, b, s, e):
    NO = struct.unpack_from('>4I', b, s + 12)
    bnd = [0x1c] + list(NO) + [e - s]
    for o in range(s, e - 3, 4):
        r = o - s
        if r < 0x1c: c.w(o, 4, 'node.hdr+%x' % r); continue
        reg = max(i for i in range(4) if r >= NO[i]); c.w(o, 4, 'node.R%d+%x' % (reg, r - NO[reg]))
    c.w(s, 1, 'node.hdr')
    conv_curves(c, b, s + NO[0], s + NO[1])
    r2 = s + NO[2]; r2e = s + NO[3]
    if r2e - r2 >= 16:
        c.w(r2, 1, 'node.r2'); c.w(r2 + 4, 1, 'node.r2')
        off = be32(b, r2 + 12)
        for o in range(r2 + off, r2e, 12): c.w(o + 4, 2, 'node.el')

def conv_curves(c, b, a, e):
    """curve headers: [u32 type][u32 0][u8 n,0,0,0][u32 hash][u32 0x14][u32 0] ; mark the n byte"""
    for o in range(a, e - 0x17, 4):
        if 0 < be32(b, o) < 32 and be32(b, o + 4) == 0 and b[o + 9:o + 12] == b'\0\0\0' and be32(b, o + 16) == 0x14 and be32(b, o + 20) == 0:
            c.w(o + 8, 1, 'curve.n')

SIMPLE_HI = {'999a': '993e', '0000': '0000', 'd70a': 'a33d', 'cccd': 'cc3d', '8937': '813f', '3333': '333f', '51ec': 'b83d', 'c8b4': 'b63e', 'c28f': '753d'}   # low half (Xbox) -> high half (PC) of the truncated float in simple events; filled from corpus
def fix_simple(b, o, c):
    lo = b[o + 22:o + 24].hex()
    hi = SIMPLE_HI.get(lo, '0000')
    c.o[o + 22:o + 24] = bytes.fromhex(hi)

def convert_db(b):
    """SoundDb 0xe5273c14: header 0x1c (u8 ver, u32 hash, u16 n, u16, ...), n x (u32, u32, u16, u16)"""
    o = bytearray(len(b)); o[0:4] = b[0:4]
    for i in range(4, len(b) - 3, 4): o[i:i + 4] = b[i:i + 4][::-1]
    o[8:10] = b[8:10][::-1]; o[10:12] = b[10:12][::-1]
    n = struct.unpack_from('>H', b, 8)[0]
    o[12:14] = b[12:14][::-1]; o[14:16] = b[14:16][::-1]      # u16 count, u16
    off = struct.unpack_from('>I', b, 0x10)[0]
    for k in range(n):
        r = off + 12 * k + 8
        o[r:r + 2] = b[r:r + 2][::-1]; o[r + 2:r + 4] = b[r + 2:r + 4][::-1]
    return bytes(o)

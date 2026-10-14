# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""PHY2 chunk: 48-byte header, Havok packfile, then Pandemic physics records + tail data."""
import struct
from hkpack import Pack
def sw(b, n): return b''.join(b[i:i + n][::-1] for i in range(0, len(b) // n * n, n)) + b[len(b) // n * n:]
def convert(x, log):
    h = struct.unpack_from('>12I', x, 0)
    n1, n2, n3, n4, psize = h[2], h[3], h[4], h[5], h[8]
    out = sw(x[:48], 4)
    pk = x[48:48 + psize]
    if psize:
        if pk[:4] != b'\x57\xe0\xe0\x57': log.append('PHY2 bad packfile'); return None
        p = Pack(pk); out += p.convert(log)
    o = 48 + psize
    rec = n1 * 0x44 + (n2 + n3) * 0x6c + n4 * 0x118
    recs_at = o
    out += sw(x[o:o + rec], 4); o += rec
    tail = bytearray(sw(x[o:], 2)); tstart = o
    # chain nodes (0x110 bytes, u32 fields) referenced from n1 records +0x30
    for r in range(n1):
        p = struct.unpack_from('>I', x, recs_at + r * 0x44 + 0x30)[0]
        seen = 0
        while p != 0xffffffff and seen < 10000 and tstart <= p < len(x):
            seen += 1
            q = p - tstart
            tail[q:q + 0x110] = sw(x[p:p + 0x110], 4)
            p = struct.unpack_from('>I', x, p + 0x10c)[0]
    # n3 shape records (0x6c): +4 ptrA, +8 countA (u32 array), +0xc ptrB, +0x10 countB (bit set, >=1 word);
    # vertex data (+0x50, u16 x3) stays u16
    r3_at = recs_at + n1 * 0x44 + n2 * 0x6c
    def u32s(p, nw):
        if tstart <= p and p + nw * 4 <= len(x):
            q = p - tstart; tail[q:q + nw * 4] = sw(x[p:p + nw * 4], 4)
    for r in range(n3):
        b = r3_at + r * 0x6c
        pa, ca, pb, cb = struct.unpack_from('>IIII', x, b + 4)
        if pa != 0xffffffff: u32s(pa, ca)
        if pb != 0xffffffff: u32s(pb, max(1, (cb + 31) // 32))
    out += bytes(tail)
    return out

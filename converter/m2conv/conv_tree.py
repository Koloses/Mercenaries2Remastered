# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""SpeedTree-style forest asset 0x34612f86 ('tree' chunk): Xbox BE -> PC LE.
file: u32 version, u32 count, count x { u32 id(0x8000xxxx), u32, u32 (small), f32, u8 flag, body }
body (u32/float unless noted): 0x00..0x77 fields, +0x78 nv, +0x7c ni, nv x 5 floats, ni x u16 indices,
then a u16-pair word at E+0x68 (and at E+0xe0 when nv>0), E = end of indices; then hashes, u32 N, N bytes
of u32 data and a u32 tail up to the next record header. Verified byte-exact against the PC copy."""
import struct
def sw4(b): return b''.join(b[i:i + 4][::-1] for i in range(0, len(b) // 4 * 4, 4)) + b[len(b) // 4 * 4:]
def sw2(b): return b''.join(b[i:i + 2][::-1] for i in range(0, len(b) // 2 * 2, 2))
def _is_hdr(x, o):
    if o + 17 > len(x): return False
    i, a, b = struct.unpack_from('>III', x, o)
    return (i & 0xffff0000) == 0x80000000 and 0 < a < 64 and 0 < b < 64 and x[o + 16] in (0, 1)
def convert(x, log=None):
    ver, cnt = struct.unpack_from('>II', x, 0)
    out = bytearray(sw4(x[:8])); o = 8
    for r in range(cnt):
        if not struct.unpack_from('>I', x, o)[0] & 0x80000000: raise ValueError('tree record %d header not found at %#x' % (r, o))
        out += sw4(x[o:o + 16]); out.append(x[o + 16]); b = o + 17
        nv, ni = struct.unpack_from('>II', x, b + 0x78)
        E = 0x80 + nv * 20 + ni * 2
        out += sw4(x[b:b + 0x80 + nv * 20]); out += sw2(x[b + 0x80 + nv * 20:b + E])
        pairs = [E + 0x68] + ([E + 0xe0] if nv else [])
        p = E
        for q in pairs:
            out += sw4(x[b + p:b + q]); out += sw2(x[b + q:b + q + 4]); p = q + 4
        # rest of the record: u32 words up to the next header (or end of file)
        e = b + p
        nxt = len(x)
        N = struct.unpack_from('>I', x, e + 0x14)[0]      # 5 hashes, u32 N, N bytes
        if r + 1 < cnt: nxt = e + 0x18 + N + 408     # fixed 408-byte tail (bounds, counts, LOD tables)
        out += sw4(x[e:nxt]); o = nxt
    if o != len(x): out += sw4(x[o:])
    return bytes(out)

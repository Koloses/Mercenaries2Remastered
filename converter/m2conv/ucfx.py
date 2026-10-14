# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Block contents: header table + UCFX asset objects (both endians)."""
import struct
def parse_block(b, be):
    e = '>' if be else '<'
    n = struct.unpack_from(e + 'I', b, 0)[0]
    hdr = [struct.unpack_from(e + 'IIII', b, 4 + i * 16) for i in range(n)]
    o = 4 + n * 16
    objs = []
    magic = b'XFCU' if be else b'UCFX'
    while o < len(b) and b[o:o+4] == magic:
        hs, a, c, nch = struct.unpack_from(e + 'IIII', b, o + 4)
        chunks = []
        for k in range(nch):
            tag = b[o + 20 + k * 20: o + 24 + k * 20]
            if be: tag = tag[::-1]
            off, size, x1, x2 = struct.unpack_from(e + 'IIII', b, o + 24 + k * 20)
            chunks.append((tag, off, size, x1, x2))
        data_start = o + hs
        end = data_start
        for tag, off, size, x1, x2 in chunks:
            if off != 0xFFFFFFFF: end = max(end, data_start + off + size)
        objs.append(dict(at=o, hs=hs, a=a, c=c, chunks=chunks, data=data_start, end=end))
        o = end
        # objects are 4-aligned? try to find next magic nearby
        if b[o:o+4] != magic:
            nx = b.find(magic, o, o + 64)
            if nx > 0: o = nx
    return hdr, objs, o

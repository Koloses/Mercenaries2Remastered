# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Read PC/Xbox WAD into a neutral structure (chunk data left in source byte order)."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wad import Wad
from ucfx import parse_block
def nodes_from_obj(b, o):
    flat = o['chunks']
    def build(i, count):
        out = []
        while count > 0 and i < len(flat):
            tag, off, size, x1, x2 = flat[i]
            data = None if off == 0xFFFFFFFF else bytes(b[o['data'] + off:o['data'] + off + size])
            kids, _ = build(i + 1, x2) if x2 else ([], None)
            out.append((tag, data, kids))
            i += 1 + x2; count -= 1
            if x1 == 0: break
        return out, i
    res, _ = build(0, 1 << 30)
    return res
def read_block(W, i):
    b = W.block(i); hdr, objs, end = parse_block(b, W.be)
    return [(h, t, po, nodes_from_obj(b, o)) for (h, t, po, sz), o in zip(hdr, objs)]
def info(W):
    h = W.read(0, 0x100); e = W.e
    ntypes = W.chunks[b'DATA'][1]
    types = [struct.unpack_from(e + 'I', h, 0x48 + 4 * k)[0] for k in range(ntypes)]
    o, c = W.chunks[b'ASET']; d = W.read(o, c * 16)
    rows = []
    for k in range(c):
        hh, b3, b2, b1, b0 = struct.unpack_from(e + 'IHHHH', d, k * 16); ti = d[k * 16 + 12]
        rows.append((hh, (b3, b2, b1, b0), ti))
    return types, rows
def ent_info(W, i):
    pg, npg, fl = W.ents[i]
    return dict(page=pg, P=npg >> 24, npages=npg & 0xffffff, flags=fl)

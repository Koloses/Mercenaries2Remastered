# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Quick asset index from each block's header table (first compressed segment only)."""
import sys, os, pickle, time, struct, zlib
sys.path.insert(0, os.path.dirname(__file__))
from wad import Wad
from xboxsrc import iso_wad
def first_seg(w, i):
    pg, npg, fl = w.ents[i]
    d = w.read(pg * w.page, 0x8000)
    if d[:4] not in (b'sges', b'segs'): return d
    e = '>' if d[:4] == b'segs' else '<'
    ver, n, full, packed = struct.unpack_from(e + 'HHII', d, 4)
    ps, us, off = struct.unpack_from(e + 'HHI', d, 16)
    ps = ps or 0x10000; src = off & ~1
    need = src + ps
    if need > len(d): d = w.read(pg * w.page, need)
    try: return zlib.decompressobj(-15).decompress(d[src:src + ps])
    except Exception: return d[src:src + ps]
def head_bytes(w, i, need):
    """decompress segments until at least need bytes"""
    raw = w.raw_block(i)
    if raw[:4] not in (b'sges', b'segs'): return raw
    e = '>' if raw[:4] == b'segs' else '<'
    ver, n, full, packed = struct.unpack_from(e + 'HHII', raw, 4)
    out = b''
    for k in range(n):
        ps, us, off = struct.unpack_from(e + 'HHI', raw, 16 + k * 8)
        ps = ps or 0x10000; src = off & ~1
        out += zlib.decompressobj(-15).decompress(raw[src:src + ps]) if off & 1 else raw[src:src + ps]
        if len(out) >= need: break
    return out
def qindex(w):
    e = w.e; idx = {}
    for i in range(len(w.ents)):
        b = first_seg(w, i)
        n = struct.unpack_from(e + 'I', b, 0)[0]
        if 4 + n * 16 > len(b): b = head_bytes(w, i, 4 + n * 16)
        if 4 + n * 16 > len(b): print('skip', i, n); continue
        for k in range(n):
            h, t, z, sz = struct.unpack_from(e + 'IIII', b, 4 + k * 16)
            idx.setdefault(h, []).append((i, t, sz))
    return idx
def build(which):
    import cfg, assets
    w = assets.open_src(which)
    T = time.time(); idx = qindex(w)
    pickle.dump((w.names, idx), open(cfg.w('q_%s.pkl' % which), 'wb'))
    return len(idx), time.time() - T
if __name__ == '__main__':
    print(sys.argv[1], build(sys.argv[1]))

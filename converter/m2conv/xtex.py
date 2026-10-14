# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Xbox 360 texture -> PC texture (numpy)."""
import struct, numpy as np
FMT = {0x12: ('DXT1', 4, 8), 0x13: ('DXT3', 4, 16), 0x14: ('DXT5', 4, 16), 0x06: ('ARGB', 1, 4), 0x31: ('DXN', 4, 16), 0x02: ('L8', 1, 1), 0x04: ('R5G6B5', 1, 2), 0x07: ('A8L8?', 1, 2)}
def log2c(v): return max(0, (v - 1).bit_length())
def tiled_offsets(xs, ys, pitch, tp):
    """vectorised XGAddress2DTiledOffset (blocks)."""
    x = xs.astype(np.int64); y = ys.astype(np.int64)
    aw = (pitch + 31) & ~31
    lb = (tp >> 2) + ((tp >> 1) >> (tp >> 2))
    macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << lb
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4)
    return (((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb
def untile(buf, off, wb, hb, tp, pitch, ox=0, oy=0):
    ys, xs = np.mgrid[0:hb, 0:wb]
    idx = tiled_offsets((xs + ox).ravel(), (ys + oy).ravel(), pitch, tp)
    a = np.frombuffer(buf, np.uint8)
    src = off + idx[:, None] * tp + np.arange(tp)[None, :]
    if src.max() >= len(a):            # truncated data: pad
        a = np.concatenate([a, np.zeros(int(src.max()) + 1 - len(a), np.uint8)])
    return a[src].tobytes()
def packed_offset(w, h, mip, bw):
    """Xenia GetPackedMipOffset: returns (xblocks, yblocks) or None if level not packed."""
    lw, lh = log2c(w), log2c(h)
    ls = min(lw, lh)
    if ls > 4 + mip: return None
    base = ls - 4 if ls > 4 else 0
    pm = mip - base
    if pm < 3:
        if lw > lh: x, y = 0, 16 >> pm
        else: x, y = 16 >> pm, 0
    else:
        if lw > lh: o = (1 << (lw - base)) >> (pm - 2); x, y = o, 0
        else: o = (1 << (lh - base)) >> (pm - 2); x, y = 0, o
        if o < 4: x, y = 0, 0   # 1x1 blocks along Z for volumes; 2D: stays in first block row
    return x // bw, y // bw
def lvl(w, h, L): return max(1, w >> L), max(1, h >> L)
def tiled_level_size(w, h, bw, tp):
    """bytes of one non-packed tiled level (pow2 rounded, 32x32-block tiles, 4KB aligned)"""
    wb = max(1, (1 << log2c(w)) // bw); hb = max(1, (1 << log2c(h)) // bw)
    s = ((wb + 31) & ~31) * ((hb + 31) & ~31) * tp
    return (s + 4095) & ~4095
def xbox_to_linear(w, h, nlev, fmt, base, mips):
    name, bw, tp = FMT[fmt]
    out = []
    def one(buf, off, L, packed):
        lw_, lh_ = lvl(w, h, L)
        wb, hb = max(1, (lw_ + bw - 1) // bw), max(1, (lh_ + bw - 1) // bw)
        pitch = max(32, ((1 << log2c(lw_)) // bw + 31) & ~31) if not packed else max(32, ((1 << log2c(lvl(w, h, packed[2])[0])) // bw + 31) & ~31)
        ox, oy = packed[:2] if packed else (0, 0)
        return untile(buf, off, wb, hb, tp, pitch, ox, oy)
    # base level
    po = packed_offset(w, h, 0, bw)
    out.append(one(base, 0, 0, (po[0], po[1], 0) if po else None))
    off = 0
    for L in range(1, nlev):
        po = packed_offset(w, h, L, bw)
        if po is None:
            lw_, lh_ = lvl(w, h, L)
            out.append(one(mips, off, L, None)); off += tiled_level_size(lw_, lh_, bw, tp)
        else:
            tl = L                                   # first packed level: tail tile at 'off'
            for L2 in range(L, nlev):
                p2 = packed_offset(w, h, L2, bw)
                out.append(one(mips, off, L2, (p2[0], p2[1], tl)))
            break
    if tp >= 2 and name not in ('ARGB',):
        out = [swap_endian(o, 2) for o in out]
    elif name == 'ARGB':
        out = [swap_endian(o, 4) for o in out]       # BE ARGB -> LE BGRA
    return out
def swap_endian(b, n):
    a = np.frombuffer(b, np.uint8).reshape(-1, n)[:, ::-1]
    return a.tobytes()
def pc_level_size(w, h, L, name):
    _, bw, tp = [v for v in FMT.values() if v[0] == name][0]
    lw_, lh_ = lvl(w, h, L)
    return max(1, (lw_ + bw - 1) // bw) * max(1, (lh_ + bw - 1) // bw) * tp

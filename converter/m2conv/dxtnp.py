# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""numpy DXT1 decode and grey/alpha DXT5 encode (for normal map swizzle Xbox DXT1 -> PC DXT5)."""
import numpy as np
def _565(c):
    r = ((c >> 11) & 31) * 255 // 31; g = ((c >> 5) & 63) * 255 // 63; b = (c & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)
def dxt1_decode_blocks(data):
    """data: n*8 bytes LE -> (n,16,4) RGBA"""
    a = np.frombuffer(data, np.uint8).reshape(-1, 8)
    c0 = a[:, 0].astype(np.int32) | a[:, 1].astype(np.int32) << 8
    c1 = a[:, 2].astype(np.int32) | a[:, 3].astype(np.int32) << 8
    idx = (a[:, 4].astype(np.uint32) | a[:, 5].astype(np.uint32) << 8 | a[:, 6].astype(np.uint32) << 16 | a[:, 7].astype(np.uint32) << 24)
    p0, p1 = _565(c0), _565(c1)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)                       # n,4,3
    al = np.stack([np.full(len(a), 255), np.full(len(a), 255), np.full(len(a), 255), np.where(four[:, 0], 255, 0)], 1)
    sh = np.arange(16, dtype=np.uint32) * 2
    ix = ((idx[:, None] >> sh) & 3).astype(np.int64)          # n,16
    rgb = np.take_along_axis(pal, ix[..., None].repeat(3, -1), 1)
    alp = np.take_along_axis(al, ix, 1)
    return np.concatenate([rgb, alp[..., None]], -1)
def enc_alpha(v):
    """v: (n,16) 0..255 -> (n,8) DXT5 alpha block (8-value mode, a0>a1)."""
    v = v.astype(np.int32)
    a0 = v.max(1); a1 = v.min(1)
    same = a0 == a1
    a0 = np.where(same & (a0 < 255), a0 + 1, a0); a1 = np.where(same & (a0 == 255) & (a1 == 255), 254, a1)
    a1 = np.where(same & (a1 == a0), a0 - 1, a1)
    pal = np.stack([a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)], 1)   # n,8
    d = np.abs(v[:, :, None] - pal[:, None, :]); ix = d.argmin(2).astype(np.uint64)
    bits = np.zeros(len(v), np.uint64)
    for i in range(16): bits |= ix[:, i] << np.uint64(3 * i)
    out = np.zeros((len(v), 8), np.uint8); out[:, 0] = a0; out[:, 1] = a1
    for k in range(6): out[:, 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    return out
def enc_grey(v):
    """v: (n,16) grey -> (n,8) DXT1 colour block in 4-colour mode."""
    v = v.astype(np.int32)
    hi = v.max(1); lo = v.min(1)
    def q(x):  # grey -> 565
        r = (x * 31 + 127) // 255; g = (x * 63 + 127) // 255
        return (r << 11) | (g << 5) | r
    c0 = q(hi); c1 = q(lo)
    swap = c0 < c1; c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    eq = c0 == c1
    c0 = np.where(eq & (c0 < 0xFFFF), c0 + 0, c0)
    p0 = _565(c0)[:, 1]; p1 = _565(c1)[:, 1]
    pal = np.stack([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3], 1)
    ix = np.abs(v[:, :, None] - pal[:, None, :]).argmin(2).astype(np.uint32)
    ix = np.where(eq[:, None], 0, ix)
    bits = np.zeros(len(v), np.uint32)
    for i in range(16): bits |= ix[:, i] << np.uint32(2 * i)
    out = np.zeros((len(v), 8), np.uint8)
    out[:, 0] = c0 & 255; out[:, 1] = c0 >> 8; out[:, 2] = c1 & 255; out[:, 3] = c1 >> 8
    for k in range(4): out[:, 4 + k] = (bits >> np.uint32(8 * k)) & 255
    return out
def normal_dxt1_to_dxt5(data):
    """Xbox normal map (X in R, Y in G) DXT1 -> PC DXT5 (RGB=Y, A=X)."""
    px = dxt1_decode_blocks(data)
    return np.concatenate([enc_alpha(px[:, :, 0]), enc_grey(px[:, :, 1])], 1).tobytes()

# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Xbox index buffers (strips with 0xFFFF restart) -> PC (degenerate-joined strips)."""
import struct, numpy as np
def join_range(a, base):
    """a: index array (np >u2) of one PRMT range starting at xbox position base.
    returns (pc list, segments [(xb_pos, pc_pos_rel, length)])"""
    out = []; segs = []
    i = 0; n = len(a)
    while i < n:
        if a[i] == 0xffff: i += 1; continue
        j = i
        while j < n and a[j] != 0xffff: j += 1
        s = a[i:j]
        if out:
            out.append(out[-1]); out.append(int(s[0]))
            if len(out) % 2: out.append(int(s[0]))
        segs.append((base + i, len(out), len(s)))
        out.extend(int(v) for v in s)
        i = j
    return out, segs
def convert_prmg_indices(xidx, xprmt, rec20=False):
    """returns (pc index bytes, pc PRMT bytes, segment map [(xb_pos, pc_pos, len)])
    model PRMT: 16-byte (hash, start, u16 count, u16 x3); terrain/lowres PRMT: 20-byte (hash, material, start, u16 count, u16 x3)"""
    a = np.frombuffer(xidx, '>u2')
    if rec20:
        n = len(xprmt) // 20
        r20 = [struct.unpack_from('>IIIHHHH', xprmt, r * 20) for r in range(n)]
        recs = [(m, st, cn, b, c, d) for m, mat, st, cn, b, c, d in r20]
    else:
        n = len(xprmt) // 16
        recs = [struct.unpack_from('>IIHHHH', xprmt, r * 16) for r in range(n)]
    out = []; prmt = b''; done = {}; segmap = []
    for key in sorted({(r[1], r[2]) for r in recs}):
        start, cnt = key
        js, segs = join_range(a[start:start + cnt + 2], start)
        done[key] = (len(out), max(0, len(js) - 2))
        segmap += [(xs, ps + len(out), L) for xs, ps, L in segs]
        out.extend(js)
    for k, (m, start, cnt, b, c, d) in enumerate(recs):
        ps, pc = done[(start, cnt)]
        if rec20: prmt += struct.pack('<IIIHHHH', m, r20[k][1], ps, pc, b, c, d)
        else: prmt += struct.pack('<IIHHHH', m, ps, pc, b, c, d)
    return np.array(out, '<u2').tobytes(), prmt, segmap
def convert_area(xarea, segmap, pc_nidx):
    x = np.frombuffer(xarea, '>u2'); out = np.zeros(max(0, pc_nidx - 2), '<u2')
    for xs, ps, L in segmap:
        if L >= 3: out[ps:ps + L - 2] = x[xs:xs + L - 2]
    return out.tobytes()

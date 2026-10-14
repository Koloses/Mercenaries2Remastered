# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Xbox texture asset parts -> PC parts.  parts: {P: nodes}; returns {P: nodes}"""
import struct
from xtex import FMT, xbox_to_linear, tiled_level_size, packed_offset, pc_level_size, lvl
from dxtnp import normal_dxt1_to_dxt5
FOURCC = {0x12: b'DXT1', 0x13: b'DXT3', 0x14: b'DXT5', 0x31: b'ATI2', 0x06: b'\x15\x00\x00\x00'}
def convert(parts, log):
    hdrP = [p for p, n in parts.items() if any(t == b'INFO' for t, d, k in n)]
    if not hdrP: log.append('texture without INFO'); return None
    hp = hdrP[0]; hn = {t: d for t, d, k in parts[hp]}
    xi = hn[b'INFO']
    w, h, d, n, fl, f5, f6 = struct.unpack_from('>HHHHHHH', xi)
    xf = struct.unpack_from('>I', xi, 14)[0] & 0x3f
    lod = xi[18:22][::-1]
    masks = struct.unpack_from('>HHHH', xi, len(xi) - 8)    # P3,P2,P1,P0
    if xf not in FMT or d != 1 or (fl & 0x40 and False):
        log.append('texture fmt %x depth %d unsupported' % (xf, d)); return None
    name, bw, tp = FMT[xf]
    bodies = [(p, {t: dd for t, dd, k in parts[p]}.get(b'BODY')) for p in sorted(parts, reverse=True)]
    full = b''.join(b for p, b in bodies if b)
    bs = tiled_level_size(w, h, bw, tp) if packed_offset(w, h, 0, bw) is None else 0
    try: levels = xbox_to_linear(w, h, n, xf, full, full[bs:] if bs else full)
    except Exception as e: log.append('texture untile failed %r' % e); return None
    fourcc = FOURCC.get(xf); pfl = fl
    if name == 'DXT1' and fl & 0x10:       # normal map -> DXT5 (RGB=Y, A=X)
        levels = [normal_dxt1_to_dxt5(l) if len(l) % 8 == 0 else l for l in levels]; fourcc = b'DXT5'; pfl = fl & ~0x10
        name = 'DXT5'
    pfl &= ~0x10                           # PC keeps no normal-map flag (DXT5 normal maps verified against PC copies)
    if name == 'ARGB': fourcc = b'\x15\x00\x00\x00'
    size = sum(len(l) for l in levels)
    info = struct.pack('<HHHHHHH', w, h, d, n, pfl, f5, f6) + fourcc + lod + struct.pack('<I', size) + struct.pack('<HHHH', *masks)
    out = {}
    for i, P in enumerate((3, 2, 1, 0)):
        m = masks[i]
        if not m and P != hp: continue
        body = b''.join(levels[L] for L in range(n) if m >> L & 1)
        nodes = []
        if P == hp:
            for t, dd, k in parts[hp]:
                if t == b'NAME': nodes.append((t, dd, []))
            nodes.append((b'INFO', info, []))
        nodes.append((b'BODY', body, []))
        out[P] = nodes
    return out

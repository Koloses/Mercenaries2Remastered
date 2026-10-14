# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Xbox 360 vertex declarations/streams -> PC D3D9."""
import struct, numpy as np
# xbox type -> (xsize, pc_type, pcsize, kind)
XT = {0x1a2360: (8, 16, 8, 'h4'), 0x2c235f: (4, 15, 4, 'h2'), 0x182886: (4, 4, 4, 'color'),
      0x2a2190: (4, 16, 8, 'hend3n'), 0x1a2187: (4, 16, 8, 'dec4n'), 0x1a2286: (4, 5, 4, 'ub4'),
      0x1a2086: (4, 8, 4, 'ub4n'), 0x2a23b9: (12, 2, 12, 'f3'), 0x2a23a6: (8, 1, 8, 'f2'), 0x1a23a6: (16, 3, 16, 'f4')}
def parse_xdecl(d):
    els = []
    for i in range(0, len(d), 12):
        s, o, t, m, u, ui, _ = struct.unpack_from('>HHIBBBB', d, i)
        if s == 0xff: break
        els.append(dict(s=s, o=o, t=t, m=m, u=u, ui=ui))
    return els
def pc_layout(els):
    """assign PC offsets per stream; returns new elements and per-stream strides"""
    off = {}; out = []
    for e in els:
        xs, pt, ps, kind = XT[e['t']]
        o = off.get(e['s'], 0)
        out.append(dict(e, po=o, pt=pt, ps=ps, kind=kind, xs=xs)); off[e['s']] = o + ps
    return out, off
def pc_decl(pels):
    b = b''.join(struct.pack('<HHBBBB', e['s'], e['po'], e['pt'], e['m'], e['u'], e['ui']) for e in pels)
    return b + struct.pack('<HHBBBB', 0xff, 0, 17, 0, 0, 0)
def _signed(v, bits):
    v = v.astype(np.int32); return np.where(v >= (1 << (bits - 1)), v - (1 << bits), v)
def conv_stream(data, pels, stream, xstride, count):
    a = np.frombuffer(data[:xstride * count], np.uint8).reshape(count, xstride)
    cols = []
    for e in [e for e in pels if e['s'] == stream]:
        x = a[:, e['o']:e['o'] + e['xs']]; k = e['kind']
        if k in ('h4', 'h2'): cols.append(x.reshape(count, -1, 2)[:, :, ::-1].reshape(count, -1))
        elif k in ('f3', 'f2', 'f4'): cols.append(x.reshape(count, -1, 4)[:, :, ::-1].reshape(count, -1))
        elif k in ('color', 'ub4', 'ub4n'): cols.append(x[:, ::-1])
        elif k == 'hend3n':
            v = x.view('>u4').reshape(count).astype(np.uint32)
            nx = _signed(v & 0x7ff, 11) / 1023.0; ny = _signed((v >> 11) & 0x7ff, 11) / 1023.0; nz = _signed(v >> 22, 10) / 511.0
            h = np.stack([nx, ny, nz, np.ones(count)], 1).astype(np.float16).view(np.uint8).reshape(count, 8)
            cols.append(h)
        elif k == 'dec4n':
            v = x.view('>u4').reshape(count).astype(np.uint32)
            nx = _signed(v & 0x3ff, 10) / 511.0; ny = _signed((v >> 10) & 0x3ff, 10) / 511.0; nz = _signed((v >> 20) & 0x3ff, 10) / 511.0
            w = np.where((v >> 30) == 3, -1.0, 1.0)
            h = np.stack([nx, ny, nz, w], 1).astype(np.float16).view(np.uint8).reshape(count, 8)
            cols.append(h)
    if not cols: return b''
    out = np.ascontiguousarray(np.concatenate(cols, 1))
    # PC skinned vertices keep blend weights in descending byte order; some Xbox streams store them the
    # other way round -> flip weights and indices together on those rows
    sel = [e for e in pels if e['s'] == stream]
    pos = {}; o = 0
    for e in sel: pos[(e['kind'], e['u'])] = o; o += e['ps']
    if ('ub4n', 1) in pos and ('ub4', 2) in pos:
        wo, io = pos[('ub4n', 1)], pos[('ub4', 2)]
        w = out[:, wo:wo + 4].astype(np.int16)
        desc = (w[:, 0] >= w[:, 1]) & (w[:, 1] >= w[:, 2]) & (w[:, 2] >= w[:, 3])
        flip = ~desc
        if flip.any():
            out[flip, wo:wo + 4] = out[flip, wo:wo + 4][:, ::-1]
            out[flip, io:io + 4] = out[flip, io:io + 4][:, ::-1]
    return out.tobytes()

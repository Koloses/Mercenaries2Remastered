# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Geometry assets: Model 0x5b724250, Scrub 0x600b904e, terrainmesh 0x7c569307, lowres 0x1602815c."""
import struct
import vtx, ibuf, phy2
from mtrl import conv_mtrl
def sw(b, n): return b''.join(b[i:i + n][::-1] for i in range(0, len(b) // n * n, n)) + b[len(b) // n * n:]
def per_record(b, size, fn):
    return b''.join(fn(b[i:i + size]) for i in range(0, len(b), size))
def hier(b): return per_record(b, 176, lambda r: sw(r[:4], 4) + sw(r[4:12], 2) + sw(r[12:], 4))
def segm(b): return per_record(b, 4, lambda r: r[:2][::-1] + r[2:4])
def ptch(b): return per_record(b, 56, lambda r: sw(r[:52], 4) + sw(r[52:56], 2))
def ptms(b): return per_record(b, 8, lambda r: r[:4][::-1] + sw(r[4:8], 2))
def model_info(b): return sw(b, 4) if len(b) != 72 else sw(b[:64], 4) + sw(b[64:], 2)   # +64: u16 LOD masks per detail part (P003..P000)
def terrain_mtrl(b, n):
    o = 0; out = bytearray()
    for i in range(n):
        a, c = struct.unpack_from('>HH', b, o + 104); e = o + 108 + (c + 2) * 4
        out += conv_mtrl(b[o:e]); o = e
    return bytes(out) + sw(b[o:], 4)
def prmg_info44(b): return sw(b[:12], 4) + sw(b[12:20], 2) + sw(b[20:24], 4) + sw(b[24:32], 2) + sw(b[32:36], 4) + sw(b[36:44], 2)
def skin_info(b):
    # 6 u32 then u16 fields (bone palette counts/ranges); verified against PC copies
    return sw(b[:24], 4) + sw(b[24:], 2)
def conv_strm(kids, log):
    c = {t: d for t, d, k in kids}
    xi = struct.unpack('>III', c[b'info'])
    els = vtx.parse_xdecl(c[b'decl'])
    unknown = [hex(e['t']) for e in els if e['t'] not in vtx.XT]
    if unknown: log.append('vertex types unknown %s' % unknown); return None, None
    pels, strides = vtx.pc_layout(els)
    s0 = els[0]['s'] if els else 0
    data = vtx.conv_stream(c[b'data'], pels, s0, xi[1], xi[2]) if els else c[b'data']
    info = struct.pack('<III', xi[0], strides.get(s0, xi[1]), xi[2])
    return [(b'info', info, []), (b'decl', vtx.pc_decl(pels), []), (b'data', data, [])], strides.get(s0, xi[1])
def conv_prmg(kids, log, kind, rec20=False):
    k = {}
    for t, d, kk in kids: k.setdefault(t, (d, kk))
    out = []
    pc_idx = None; prmt = None; segmap = None
    if b'IBUF' in k:
        ib = {t: d for t, d, kk in k[b'IBUF'][1]}
        if b'PRMT' in k:
            pc_idx, prmt, segmap = ibuf.convert_prmg_indices(ib[b'data'], k[b'PRMT'][0], rec20)
        else:
            a = __import__('numpy').frombuffer(ib[b'data'], '>u2')
            js, segs = ibuf.join_range(a, 0)
            pc_idx = __import__('numpy').array(js, '<u2').tobytes(); segmap = segs
    for t, d, kk in kids:
        if t == b'INFO':
            if kind == 'skin': out.append((t, skin_info(d), []))
            elif len(d) == 44: out.append((t, prmg_info44(d), []))
            else: out.append((t, sw(d, 4), []))
        elif t == b'STRM':
            r, _ = conv_strm(kk, log)
            if r is None: return None
            out.append((t, None, r))
        elif t == b'IBUF':
            out.append((t, None, [(b'info', struct.pack('<I', len(pc_idx) // 2), []), (b'data', pc_idx, [])]))
        elif t == b'AREA':
            ad = {x: y for x, y, z in kk}
            area = ibuf.convert_area(ad[b'data'], segmap, len(pc_idx) // 2)
            out.append((t, None, [(b'info', struct.pack('<I', len(area) // 2), []), (b'data', area, [])]))
        elif t == b'PRMT': out.append((t, prmt, []))
        elif t == b'BSHI': out.append((t, sw(d, 2), []))
        elif t == b'BSHP':
            bc = {x: y for x, y, z in kk}
            els = vtx.parse_xdecl(bc[b'decl']); pels, _ = vtx.pc_layout(els)
            out.append((t, None, [(b'info', sw(bc[b'info'], 4), []), (b'decl', vtx.pc_decl(pels), []), (b'data', sw(bc[b'data'], 2), [])]))
        elif t == b'MTRL': out.append((t, conv_mtrl(d), []))
        elif t == b'BNDS': out.append((t, sw(d, 4), []))
        else:
            log.append('prmg: unhandled %s' % t); out.append((t, sw(d, 4) if d else d, kk))
    return out
def walk(nodes, path, log, ctx):
    out = []
    for t, d, kids in nodes:
        p = path + '/' + t.decode('latin1')
        if t in (b'PRMG', b'SCRB') or (d is None and any(kt in (b'STRM', b'IBUF') for kt, kd, kk in kids)):
            r = conv_prmg(kids, log, 'skin' if '/SKIN/' in p else 'mesh', ctx.get('type') in (0x7c569307, 0x1602815c))
            if r is None: return None
            out.append((t, None, r)); continue
        if d is None:
            r = walk(kids, p, log, ctx)
            if r is None: return None
            out.append((t, None, r)); continue
        if t == b'MTRL': nd = terrain_mtrl(d, ctx['terrain_n']) if ctx.get('terrain_n') is not None else conv_mtrl(d)
        elif t == b'HIER': nd = hier(d)
        elif t == b'SEGM': nd = segm(d)
        elif t == b'PTCH': nd = ptch(d)
        elif t == b'PTMS': nd = ptms(d)
        elif t in (b'INST', b'INDX', b'BSHI'): nd = sw(d, 2)
        elif t == b'PHY2': nd = phy2.convert(d, log)
        # MIXR (characters with interchangeable parts: heads, gear): INFO u16 x3 (.., part count n, profile
        # count), NAME n part hashes (u32), EXMX n x n exclusion bytes, PROF per profile a u32 hash + n bytes.
        # Left unswapped, no part hash matched and those characters were drawn without any parts (invisible).
        elif t == b'INFO' and path == '/MIXR': nd = sw(d, 2); ctx['mixr_n'] = struct.unpack_from('>H', d, 2)[0]
        elif t == b'NAME' and path == '/MIXR': nd = sw(d, 4)
        elif t == b'PROF' and path == '/MIXR' and ctx.get('mixr_n'): nd = per_record(d, 4 + ctx['mixr_n'], lambda r: r[:4][::-1] + r[4:])
        elif t == b'NAME' or t == b'EXMX': nd = d
        elif t == b'INFO' and path == '' and ctx['type'] == 0x5b724250: nd = model_info(d)
        else: nd = sw(d, 4)
        if nd is None: return None
        out.append((t, nd, []))
    return out
def convert(nodes, typ, log):
    ctx = {'type': typ}
    if typ == 0x7c569307:
        info = [d for t, d, k in nodes if t == b'INFO']
        if info: ctx['terrain_n'] = struct.unpack_from('>I', info[0], 24)[0]
    return walk(nodes, '', log, ctx)

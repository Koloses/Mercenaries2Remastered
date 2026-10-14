# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Xbox layer-type asset (0xe6b81a54 layer, 0x5647c35d patch layer) -> PC nodes."""
import struct, pickle, os
from layerconv import parse_schema, layout, convert_data, parse_entries
from preimage import Pre, ph
DB = None; ENUM = None; PRE = None; LAYOUT_CACHE = {}
def init(enum_chunk):
    global DB, ENUM, PRE
    DB, _ = pickle.load(open(__import__('cfg').w('schemadb.pkl'), 'rb'))
    ENUM = enum_chunk; PRE = Pre()
def sw4(b): return b''.join(b[i:i + 4][::-1] for i in range(0, len(b) // 4 * 4, 4)) + b[len(b) // 4 * 4:]
def sw8(b): return b''.join(b[i:i + 8][::-1] for i in range(0, len(b) // 8 * 8, 8)) + b[len(b) // 8 * 8:]
NAME_CH = None
NAMES = None
SCRIPT_HASHES = set()     # hashes of all string constants in PC + DLC scripts
def set_script_names(strs):
    """PC layers flag a Name entry (trailing u8 = 1) when scripts look the object up by name
    (253 of 254 script-referenced names in vz carry it, 0.2% of the rest); the stripped Xbox
    Name component lost that byte, so we rebuild it from the script constants."""
    global NAMES
    if NAMES is None: NAMES = pickle.load(open(__import__('cfg').w('namedb.pkl'), 'rb'))
    for k in strs:
        if not k: continue
        hv = ph(k); SCRIPT_HASHES.add(hv)
        NAMES.setdefault(hv, k)
def name_for(hv):
    global NAMES
    if NAMES is None: NAMES = pickle.load(open(__import__('cfg').w('namedb.pkl'), 'rb'))
    s = NAMES.get(hv)
    if s is None:
        s = (PRE.find(hv) or 'dlc_%08x' % hv).encode(); NAMES[hv] = s
    return s
def conv_name(xd, log):
    for mode in ('g', 'k'):
        ents = parse_entries(xd, [('v', 4)], '>', mode)
        if ents is not None: break
    else:
        log.append('Name parse failed'); return b''
    out = bytearray()
    for ids, s, e in ents:
        if mode == 'g': out += struct.pack('<I', len(ids))
        out += struct.pack('<%dI' % len(ids), *ids)
        hv = struct.unpack_from('>I', xd, s)[0]
        out += name_for(hv) + b'\0' + (b'\1' if hv in SCRIPT_HASHES else b'\0')
    return bytes(out)
def conv_name_full(xd, log):
    """unstripped Name component: (id u32 BE, cstring, u8) keyed or grouped"""
    for mode in ('k', 'g'):
        ents = parse_entries(xd, [('s',), ('v', 1)], '>', mode)
        if ents is not None: break
    else:
        log.append('Name(full) parse failed'); return xd
    out = bytearray()
    for ids, s0, e in ents:
        if mode == 'g': out += struct.pack('<I', len(ids))
        out += struct.pack('<%dI' % len(ids), *ids); out += xd[s0:e]
    return bytes(out)
def conv_comp(kids, log):
    c = {t: d for t, d, _ in kids}
    xi = c[b'info']
    if b'schm' in c:            # unstripped: name + BE schema present
        name = xi.split(b'\0')[0]; ch, a, cnt, fl = struct.unpack_from('>IIII', xi, len(name) + 1)
        xs = c[b'schm']; schm = sw4(xs[:8]) + b''.join(sw4(xs[o:o + 12]) + xs[o + 12:o + 14][::-1] + xs[o + 14:o + 16] for o in range(8, len(xs), 16))
    else:
        ch, a, cnt, fl = struct.unpack_from('>IIII', xi)
        ent = DB.get(ch)
        if ent is None: log.append('unknown class %08x' % ch); return None
        name = ent['name']; schm = ent['schm'].most_common(1)[0][0]
    info = name + b'\0' + struct.pack('<IIII', ch, a, cnt, fl)
    xd = c.get(b'data') or b''
    if name == b'Name' and b'schm' in c: data = conv_name_full(xd, log)
    elif name == b'Name': data = conv_name(xd, log)
    else:
        key = ch
        cand = LAYOUT_CACHE.get(key)
        f, rs = parse_schema(schm)
        data = None
        tries = [cand] if cand else []
        for s6 in (False, True):
            for mode in ('g', 'k'): tries.append((s6, mode))
        for s6, mode in tries:
            items = layout(f, rs, s6, name)
            data = convert_data(xd, items, mode)
            if data is not None: LAYOUT_CACHE[key] = (s6, mode); break
        if data is None: log.append('data parse failed for %s (%d bytes)' % (name.decode(), len(xd))); data = sw4(xd)
    return [(b'info', info, []), (b'schm', schm, []), (b'data', data, [])]
def convert(nodes, log):
    out = []
    for t, d, kids in nodes:
        if t == b'CHDR':
            a, b, fl = struct.unpack_from('>HHI', d)
            out.append((b'CHDR', struct.pack('<HHI', a, b, fl & ~4), []))
            out.append((b'enum', ENUM, []))
        elif t == b'enum': continue
        elif t == b'COMP':
            k = conv_comp(kids, log)
            if k is not None: out.append((b'COMP', None, k))
        elif t == b'UNIQ' or (t == b'flgt' and len(d) == 4): out.append((t, sw4(d), []))
        elif t == b'flgt':
            n = struct.unpack_from('>I', d)[0]; o = 4; r = bytearray(struct.pack('<I', n))
            for _ in range(n):
                e = d.index(b'\0', o + 4) + 1; r += d[o:o + 4][::-1] + d[o + 4:e]; o = e
            r += sw4(d[o:])
            out.append((t, bytes(r), []))
        elif t == b'flgs':
            n = struct.unpack_from('>I', d)[0]; o = 4; r = bytearray(struct.pack('<I', n))
            while o < len(d):
                r += d[o:o + 4][::-1] + sw8(d[o + 4:o + 36]); o += 36
            out.append((t, bytes(r), []))
        else:
            log.append('unknown top chunk %s' % t); out.append((t, sw4(d) if d else d, []))
    return out

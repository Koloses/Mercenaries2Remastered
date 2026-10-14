# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Schema-driven conversion of layer component data (Xbox BE -> PC LE)."""
import struct
SIZES = {5: 4, 6: 4, 7: 4, 9: 4, 10: 12, 11: 32, 2: 1, 3: 2}
def parse_schema(schm):
    nf, rs = struct.unpack_from('<II', schm)
    f = []
    for i in range(nf):
        t, nh, a, off = struct.unpack_from('<IIII', schm, 8 + 16 * i)
        f.append(dict(t=t, h=nh, a=a, off=off & 0xffff, bitpos=(off >> 16) & 0xff, width=off >> 24))
    return f, rs
MSB_ENUM_CLASSES = {b'NetCategoryInfo'}
def layout(fields, rs, str6=False, cls=None):
    """-> items in file order: ('v', size) swap as one value | ('w', size) swap per 4-byte word |
    ('raw', n) | ('s',) string | ('bits', size, [(pos,width)], enum)"""
    fs = sorted(fields, key=lambda x: (x['off'], x['bitpos']))
    items = []; i = 0; cur = 0
    def gap(n):
        if n <= 0: return
        if n % 4 == 0: items.append(('w', n))
        else: items.append(('raw', n))
    while i < len(fs):
        f = fs[i]; t = f['t']
        if f['off'] > cur: gap(f['off'] - cur); cur = f['off']
        if t == 4 and f['width'] > 0:
            grp = [f]; j = i + 1
            while j < len(fs) and fs[j]['t'] == 4 and fs[j]['width'] > 0: grp.append(fs[j]); j += 1
            nb = max(g['bitpos'] + g['width'] for g in grp)
            size = 1 if nb <= 8 else 2 if nb <= 16 else 4
            items.append(('bits', size, [(g['bitpos'], g['width']) for g in grp], 'msb' if cls in MSB_ENUM_CLASSES else True))
            nxt = fs[j]['off'] if j < len(fs) else rs
            cur = max(cur, nxt); i = j; continue
        if t in (1, 5) and f['width'] > 0:
            grp = [f]; j = i + 1
            while j < len(fs) and fs[j]['off'] == f['off'] and fs[j]['t'] in (1, 5) and fs[j]['width'] > 0: grp.append(fs[j]); j += 1
            nxt = fs[j]['off'] if j < len(fs) else rs
            size = nxt - f['off']
            items.append(('bits', size, [(g['bitpos'], g['width']) for g in grp], False))
            cur = nxt; i = j; continue
        nxt = fs[i + 1]['off'] if i + 1 < len(fs) else rs
        if t == 8 or (t == 6 and str6): items.append(('s',)); cur = nxt
        elif t == 4: items.append(('v', max(1, nxt - f['off']))); cur = nxt
        elif t in (1, 2): items.append(('v', 1)); cur = f['off'] + 1
        elif t == 3: items.append(('v', 2)); cur = f['off'] + 2
        elif t in (10, 11): n = SIZES[t]; items.append(('w', n)); cur = f['off'] + n
        else: items.append(('v', 4)); cur = f['off'] + 4
        i += 1
    if rs > cur: gap(rs - cur)
    return items
def conv_record(src, o, items, be_in=True, out=None):
    for it in items:
        k = it[0]
        if k == 's':
            e = src.index(b'\0', o) + 1
            if out is not None: out += src[o:e]
            o = e; continue
        n = it[1]; b = src[o:o + n]
        if len(b) < n: raise ValueError('short')
        if out is not None:
            if not be_in: out += b
            elif k == 'v': out += b[::-1]
            elif k == 'w': out += b''.join(b[q:q + 4][::-1] for q in range(0, n, 4))
            elif k == 'raw': out += b
            else:
                bits, enum = it[2], it[3]
                v = int.from_bytes(b, 'big'); r = 0; N = n * 8
                if enum == 'msb':
                    for pos, w in bits: r |= ((v >> (N - pos - w)) & ((1 << w) - 1)) << pos
                elif enum:
                    r = v
                else:
                    for pos, w in bits:
                        for q in range(pos, pos + w): r |= ((v >> (N - 1 - q)) & 1) << q
                out += r.to_bytes(n, 'little')
        o += n
    return o
def parse_entries(data, items, e, mode):
    """mode 'g' grouped (count, refs, rec) or 'k' keyed (id, rec). returns list of (ids tuple, rec_start, rec_end) or None"""
    o = 0; out = []
    try:
        while o < len(data):
            if mode == 'g':
                n = struct.unpack_from(e + 'I', data, o)[0]
                if n == 0 or n > 200000: return None
                ids = struct.unpack_from(e + '%dI' % n, data, o + 4); o += 4 + 4 * n
            else:
                ids = (struct.unpack_from(e + 'I', data, o)[0],); o += 4
            s = o; o = conv_record(data, o, items, False, None)
            out.append((ids, s, o))
        return out if o == len(data) else None
    except Exception:
        return None
def convert_data(xdata, items, mode):
    ents = parse_entries(xdata, items, '>', mode)
    if ents is None: return None
    out = bytearray(); o = 0
    for ids, s, e in ents:
        if mode == 'g': out += struct.pack('<I', len(ids))
        out += struct.pack('<%dI' % len(ids), *ids)
        conv_record(xdata, s, items, True, out)
    return bytes(out)
def detect(pdata, fields, rs, cls=None):
    """find (items, mode) that parses PC data exactly"""
    for str6 in (False, True):
        items = layout(fields, rs, str6, cls)
        for mode in ('g', 'k'):
            if parse_entries(pdata, items, '<', mode) is not None: return items, mode, str6
    return None

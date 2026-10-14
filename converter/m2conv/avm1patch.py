# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Minimal AVM1 code insertion for the PC shell movie (decompressed SWF body, i.e. after the 8-byte header).
insert(swf, at, code) inserts raw action bytes at absolute offset `at` (must be an action boundary inside a
DoAction), fixing: branch offsets that cross `at`, enclosing DefineFunction/DefineFunction2/With sizes, and the
lengths of the enclosing DoAction / DefineSprite tags."""
import struct
def _rect_len(d): nb = d[0] >> 3; return (5 + 4 * nb + 7) // 8
def _tags(d, o, end, path, at, hits):
    while o < end:
        h = struct.unpack_from('<H', d, o)[0]; t, l = h >> 6, h & 63; hl = 2
        if l == 63: l = struct.unpack_from('<I', d, o + 2)[0]; hl = 6
        body = o + hl
        if body <= at < body + l or (at == body + l and t == 12):
            hits.append((o, hl, t, l))
            if t == 39: _tags(d, body + 4, body + l, path, at, hits)
            return
        o = body + l
def _actions(d, o, end, out):
    """list (offset, op, len, payload_offset) for all actions incl. nested function bodies"""
    while o < end:
        op = d[o]
        if op == 0: out.append((o, op, 0, o + 1)); o += 1; continue
        if op < 0x80: out.append((o, op, 0, o + 1)); o += 1; continue
        ln = struct.unpack_from('<H', d, o + 1)[0]; p = o + 3
        out.append((o, op, ln, p)); o = p + ln
    return out
def insert(d, at, code):
    d = bytearray(d); n = len(code)
    hits = []; _tags(d, _rect_len(d) + 4, len(d), [], at, hits)
    act = [h for h in hits if h[2] == 12]
    if not act: raise ValueError('offset not inside a DoAction')
    ao, ahl, _, al = act[-1]; abody = ao + ahl
    acts = _actions(d, abody, abody + al, [])
    fixes = []
    for o, op, ln, p in acts:
        if op in (0x99, 0x9D):
            off = struct.unpack_from('<h', d, p)[0]; src = p + 2; tgt = src + off
            if src <= at < tgt or tgt <= at < src:
                no = off + n if tgt > src else off - n
                if not -32768 <= no < 32768: raise ValueError('branch out of range')
                fixes.append((p, no))
        elif op in (0x9B, 0x8E, 0x94):
            if op == 0x94: sp = p; body = p + 2
            else:
                q = p
                q = d.index(b'\0', q) + 1
                cnt = struct.unpack_from('<H', d, q)[0]; q += 2
                if op == 0x8E:
                    q += 3
                    for i in range(cnt): q += 1; q = d.index(b'\0', q) + 1
                else:
                    for i in range(cnt): q = d.index(b'\0', q) + 1
                sp = q; body = q + 2
            size = struct.unpack_from('<H', d, sp)[0]
            if body <= at <= body + size:
                fixes.append(('size', sp, size + n))
    for f in fixes:
        if f[0] == 'size': struct.pack_into('<H', d, f[1], f[2])
        else: struct.pack_into('<h', d, f[0], f[1])
    for o, hl, t, l in hits:
        if hl != 6: raise ValueError('short tag header around insertion')
        struct.pack_into('<I', d, o + 2, l + n)
    d[at:at] = code
    return bytes(d)
def push(*vals):
    b = bytearray()
    for v in vals:
        if isinstance(v, str): b += b'\x00' + v.encode() + b'\0'
        elif isinstance(v, bool): b += b'\x05' + (b'\x01' if v else b'\x00')
        elif isinstance(v, int): b += b'\x07' + struct.pack('<i', v)
        elif isinstance(v, tuple) and v[0] == 'r': b += b'\x04' + bytes([v[1]])
        else: raise TypeError(v)
    return b'\x96' + struct.pack('<H', len(b)) + bytes(b)
def jump(off): return b'\x99\x02\x00' + struct.pack('<h', off)
GETMEMBER, CALLMETHOD, CALLFUNC, POP, SETVAR = b'\x4e', b'\x52', b'\x3d', b'\x17', b'\x1d'

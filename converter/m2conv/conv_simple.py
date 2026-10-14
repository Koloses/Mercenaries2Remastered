# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Simple field-swap converters."""
def sw(b, n): return b''.join(b[i:i + n][::-1] for i in range(0, len(b) // n * n, n)) + b[len(b) // n * n:]
def path_like(nodes, log):
    out = []
    for t, d, k in nodes:
        if t == b'data' and d is not None: out.append((t, sw(d[:8], 2) + sw(d[8:], 4), []))
        else: out.append((t, d, k))
    return out
import struct
def table207(nodes, log):
    """0x207359c7: INFO (u16s), TYPE (name\\0 + u16)*, VALU (u32s), ASTO (u32s)"""
    out = []
    for t, d, k in nodes:
        if t == b'INFO': out.append((t, sw(d, 2), []))
        elif t == b'TYPE':
            r = bytearray(); o = 0
            while o < len(d):
                e = d.index(b'\0', o) + 1; r += d[o:e] + d[e:e + 2][::-1]; o = e + 2
            out.append((t, bytes(r), []))
        elif t in (b'VALU', b'ASTO'): out.append((t, sw(d, 4), []))
        else: log.append('207: unknown chunk %s' % t); out.append((t, d, k))
    return out
def delta_apply(pc_base, xb_base, xb_new, log, name=''):
    """same-length structure: copy PC base, convert changed 4-byte-aligned words using widths learned from the pair."""
    if pc_base is None or xb_base is None or not (len(pc_base) == len(xb_base) == len(xb_new)): return None
    out = bytearray(pc_base); n = len(xb_new); changed = 0
    for o in range(0, n - n % 4, 4):
        if xb_new[o:o + 4] == xb_base[o:o + 4]: continue
        changed += 1
        pb, xb = pc_base[o:o + 4], xb_base[o:o + 4]
        if pb == xb[::-1] and pb != pb[::-1]: out[o:o + 4] = xb_new[o:o + 4][::-1]
        elif pb[0:2] == xb[0:2][::-1] and pb[2:4] == xb[2:4][::-1] and pb != xb: out[o:o + 4] = xb_new[o:o + 2][::-1] + xb_new[o + 2:o + 4][::-1]
        elif pb == xb: out[o:o + 4] = xb_new[o:o + 4] if not (pb == pb[::-1]) else xb_new[o:o + 4][::-1]
        else: out[o:o + 4] = xb_new[o:o + 4][::-1]
    for o in range(n - n % 4, n): out[o] = xb_new[o]
    log.append('delta %s: %d words changed' % (name, changed))
    return bytes(out)
def guidmap(nodes, log):
    d = nodes[0][1]
    v, z, n1, n2 = struct.unpack_from('>HHII', d, 0)
    out = bytearray(struct.pack('<HHII', v, z, n1, n2)) + d[12:16] + d[16:20][::-1] + d[20:24][::-1]
    o = 24
    out += sw(d[o:o + 4 * n1], 4); o += 4 * n1
    out += d[o:o + n1 - 4]; o += n1 - 4
    out += sw(d[o:o + 4 * n2], 4); o += 4 * n2
    out += sw(d[o:o + 2 * n2], 2); o += 2 * n2
    if o != len(d): log.append('guidmap size mismatch %d/%d' % (o, len(d)))
    return [(nodes[0][0], bytes(out), [])]

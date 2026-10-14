# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Localized string table (0x39e5e978): INFO, KEYS, STRS."""
import struct
def sw4(b): return b''.join(b[i:i + 4][::-1] for i in range(0, len(b) // 4 * 4, 4)) + b[len(b) // 4 * 4:]
def sw2(b): return b''.join(b[i:i + 2][::-1] for i in range(0, len(b) // 2 * 2, 2)) + b[len(b) // 2 * 2:]
def convert(nodes, log):
    out = []
    for t, d, k in nodes:
        if t == b'INFO': out.append((t, sw4(d), []))
        elif t == b'KEYS': out.append((t, sw4(d), []))
        elif t == b'STRS': out.append((t, d[:4][::-1] + sw2(d[4:]), []))
        else: log.append('strings: unknown chunk %s' % t); out.append((t, d, k))
    return out

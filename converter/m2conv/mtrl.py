# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
import struct
def sw4(b): return b''.join(b[i:i + 4][::-1] for i in range(0, len(b) // 4 * 4, 4))
def conv_mtrl(x):
    out = bytearray(); o = 0
    while o + 108 <= len(x):
        out += sw4(x[o:o + 104])
        a, n = struct.unpack_from('>HH', x, o + 104)
        out += struct.pack('<HH', a, n)
        e = o + 108 + (n + 2) * 4
        out += sw4(x[o + 108:e]); o = e
    if o != len(x): raise ValueError('MTRL parse mismatch %d/%d' % (o, len(x)))
    return bytes(out)

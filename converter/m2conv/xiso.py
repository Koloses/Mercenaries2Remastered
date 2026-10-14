# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Xbox 360 XDVDFS (XGD2/XGD3 redump ISO) reader."""
import struct, os, sys
BASES = (0xFD90000, 0x2080000, 0x0)
class XISO:
    def __init__(s, path):
        s.f = open(path, 'rb')
        for b in BASES:
            s.f.seek(b + 0x10000)
            h = s.f.read(0x20)
            if h[:20] == b'MICROSOFT*XBOX*MEDIA':
                s.base = b; break
        else: raise ValueError('not an XDVDFS image')
        s.root_sec, s.root_size = struct.unpack('<II', h[20:28])
    def _read(s, sec, size):
        s.f.seek(s.base + sec * 2048); return s.f.read(size)
    def _walk(s, sec, size, prefix, out):
        if size == 0: return
        d = s._read(sec, size)
        stack = [0]
        while stack:
            o = stack.pop()
            if o * 4 + 14 > len(d): continue
            l, r, fsec, fsize, attr, nlen = struct.unpack_from('<HHIIBB', d, o * 4)
            if l == 0xFFFF: continue
            name = d[o*4+14:o*4+14+nlen].decode('latin1')
            if l: stack.append(l)
            if r: stack.append(r)
            p = prefix + name
            if attr & 0x10: s._walk(fsec, fsize, p + '/', out)
            else: out.append((p, fsec, fsize))
    def files(s):
        out = []; s._walk(s.root_sec, s.root_size, '', out); return out
    def extract(s, sec, size, dst, chunk=1 << 24):
        s.f.seek(s.base + sec * 2048)
        with open(dst, 'wb') as o:
            while size:
                n = min(chunk, size); o.write(s.f.read(n)); size -= n
if __name__ == '__main__':
    x = XISO(sys.argv[1])
    for p, sec, size in sorted(x.files()): print(size, p)

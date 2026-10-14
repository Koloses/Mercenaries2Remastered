# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Pandemic WAD (FFCS little-endian PC / SCFF big-endian Xbox 360) reader."""
import struct, zlib, os
class Wad:
    def __init__(s, paths):
        if isinstance(paths, (str, tuple)): paths = [paths]
        s.parts, s.sizes, s.bases = [], [], []
        for p in paths:            # path, or (path, offset, size) for a file inside an image
            if isinstance(p, tuple):
                s.parts.append(open(p[0], 'rb')); s.bases.append(p[1]); s.sizes.append(p[2])
            else:
                s.parts.append(open(p, 'rb')); s.bases.append(0); s.sizes.append(os.path.getsize(p))
        h = s.read(0, 0x100)
        s.be = h[:4] == b'SCFF'
        s.e = '>' if s.be else '<'
        n = struct.unpack_from(s.e + 'I', h, 8)[0]
        s.chunks = {}
        for i in range(n):
            o = 12 + i * 12
            tag = h[o:o+4][::-1] if s.be else h[o:o+4]
            s.chunks[tag] = struct.unpack_from(s.e + 'II', h, o + 4)
        io, cnt = s.chunks[b'INDX']
        ind = s.read(io, cnt * 12)
        s.ents = [struct.unpack_from(s.e + 'III', ind, i * 12) for i in range(cnt)]
        po, pc = s.chunks[b'PTHS']
        raw = s.read(po, 0x400000)
        names = raw.split(b'\0')
        s.names = [x.decode('latin1') for x in names[:pc]]
        s.page = 0x8000
    def read(s, off, n):
        out = b''
        for f, sz, b in zip(s.parts, s.sizes, s.bases):
            if off < sz:
                f.seek(b + off); chunk = f.read(min(n - len(out), sz - off)); out += chunk
                off = 0
                if len(out) >= n: break
            else: off -= sz
        return out
    def raw_block(s, i):
        pg, npg, fl = s.ents[i]
        return s.read(pg * s.page, (npg & 0xffffff) * s.page)
    def block(s, i):
        return unsegs(s.raw_block(i), s.be)
def unsegs(d, be=None):
    if d[:4] not in (b'sges', b'segs'): return d
    be = d[:4] == b'segs'
    e = '>' if be else '<'
    ver, n, full, packed = struct.unpack_from(e + 'HHII', d, 4)
    o = bytearray()
    for i in range(n):
        ps, us, off = struct.unpack_from(e + 'HHI', d, 16 + i * 8)
        ps = ps or 0x10000; us = us or 0x10000
        src = off & ~1
        try: o += zlib.decompressobj(-15).decompress(d[src:src + ps])
        except Exception: o += d[src:src + us]
    return bytes(o)
def assets(w):
    o, c = w.chunks[b'ASET']; d = w.read(o, c * 16)
    out = {}
    for i in range(c):
        h, a, b, cc, dd, t = struct.unpack_from(w.e + 'IHHHHI', d, i * 16)
        out[h] = (a, b, cc, dd, t)
    return out

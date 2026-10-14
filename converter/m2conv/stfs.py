# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Xbox 360 STFS (CON/LIVE/PIRS) package reader - contiguous and chained files."""
import struct, sys, os
def i24le(b, o): return b[o] | b[o+1] << 8 | b[o+2] << 16
def i24be(b, o): return b[o] << 16 | b[o+1] << 8 | b[o+2]
class STFS:
    def __init__(s, path):
        s.f = open(path, 'rb')
        h = s.f.read(0x1000)
        s.magic = h[:4]
        if s.magic not in (b'LIVE', b'CON ', b'PIRS'): raise ValueError('not STFS')
        hs = struct.unpack_from('>I', h, 0x340)[0]
        s.data_start = (hs + 0xFFF) & 0xF000
        sep = h[0x37B]
        s.shift = 0 if (sep & 1) else 1              # read-only packages keep one copy of each hash table
        s.ft_count = struct.unpack_from('<H', h, 0x37C)[0]
        s.ft_block = i24le(h, 0x37E)
        s.title = struct.unpack_from('>I', h, 0x360)[0]
        s.content_type = struct.unpack_from('>I', h, 0x344)[0]
        s.display = h[0x411:0x411 + 0x80].decode('utf-16-be', 'ignore').split('\0')[0]
        s.entries = s._files()
    def fix(s, b):
        adj = 0
        if b >= 0xAA: adj += ((b // 0xAA) + 1) << s.shift
        if b >= 0x70E4: adj += ((b // 0x70E4) + 1) << s.shift
        return b + adj
    def boff(s, b): return s.data_start + ((1 << s.shift) << 12) + (s.fix(b) << 12)
    def hash_off(s, b):
        n = b // 0xAA
        if n == 0: base = s.data_start
        else: base = s.boff(n * 0xAA) - ((1 << s.shift) << 12)
        return base + (b % 0xAA) * 0x18
    def next_block(s, b):
        s.f.seek(s.hash_off(b)); e = s.f.read(0x18)
        return i24be(e, 0x15)
    def read_block(s, b):
        s.f.seek(s.boff(b)); return s.f.read(0x1000)
    def _files(s):
        raw = b''; b = s.ft_block
        for i in range(s.ft_count):
            raw += s.read_block(b + i)
        out = []
        for o in range(0, len(raw), 0x40):
            e = raw[o:o+0x40]
            fl = e[0x28]
            nlen = fl & 0x3F
            if nlen == 0: continue
            name = e[:nlen].decode('latin1')
            out.append(dict(name=name, dir=bool(fl & 0x80 and False) or bool(fl & 0x40) and False, flags=fl,
                            isdir=bool(fl & 0x80), contig=bool(fl & 0x40),
                            blocks=i24le(e, 0x29), start=i24le(e, 0x2F),
                            parent=struct.unpack_from('>h', e, 0x32)[0], size=struct.unpack_from('>I', e, 0x34)[0]))
        for e in out:
            p, n = e['parent'], e['name']
            while p != -1 and 0 <= p < len(out): n = out[p]['name'] + '/' + n; p = out[p]['parent']
            e['path'] = n
        return out
    def read(s, e):
        out = bytearray(); b = e['start']; left = e['size']
        for i in range(e['blocks']):
            out += s.read_block(b)
            b = b + 1 if e['contig'] else s.next_block(b)
        return bytes(out[:left])
    def extract_all(s, dst):
        for e in s.entries:
            if e['isdir']: continue
            p = os.path.join(dst, e['path']); os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(s.read(e))
if __name__ == '__main__':
    p = STFS(sys.argv[1])
    print(p.magic, hex(p.title), hex(p.content_type), p.display, 'shift', p.shift)
    for e in p.entries: print('%10d %s %s' % (e['size'], 'D' if e['isdir'] else ('c' if e['contig'] else 'x'), e['path']))
    if len(sys.argv) > 2: p.extract_all(sys.argv[2])

# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""PC (little-endian 'FFCS') WAD writer.
Asset object = list of chunk nodes: (tag:bytes4, data:bytes|None, children:list) ; data None => container.
"""
import struct, zlib
PAGE = 0x8000
def _flatten(nodes, out):
    """returns list of (tag, data, x1, x2) in tree order"""
    for i, (tag, data, kids) in enumerate(nodes):
        pos = len(out); out.append(None)
        _flatten(kids or [], out)
        out[pos] = (tag, data, len(nodes) - 1 - i, len(out) - pos - 1)
    return out
def ucfx(nodes):
    flat = _flatten(nodes, [])
    hs = 20 + 20 * len(flat)
    hdr = bytearray(b'UCFX' + struct.pack('<IIII', hs, 0, 0, len(flat)))
    body = bytearray()
    for tag, data, x1, x2 in flat:
        if data is None: hdr += tag + struct.pack('<IIII', 0xFFFFFFFF, 0, x1, x2)
        else:
            hdr += tag + struct.pack('<IIII', len(body), len(data), x1, x2); body += data
    obj = bytes(hdr + body)
    return obj + b'CSUM' + struct.pack('<I', zlib.crc32(obj) & 0xffffffff)
def block(objs):
    """objs: list of (hash, typehash, part_offset, nodes) -> decompressed block bytes"""
    blobs = [ucfx(n) for _, _, _, n in objs]
    out = bytearray(struct.pack('<I', len(objs)))
    for (h, t, po, _), b in zip(objs, blobs): out += struct.pack('<IIII', h, t, po, len(b))
    for b in blobs: out += b
    return bytes(out), [len(b) for b in blobs]
def segs(data, level=int(__import__('os').environ.get('M2ZLEVEL', '6'))):
    """'sges' v4 segmented raw-deflate container"""
    chunks = [data[i:i + 0x10000] for i in range(0, len(data), 0x10000)] or [b'']
    n = len(chunks)
    pos = (16 + 8 * n + 15) & ~15
    table = []; payload = bytearray()
    for c in chunks:
        co = zlib.compressobj(level, zlib.DEFLATED, -15); z = co.compress(c) + co.flush()
        comp = len(z) < len(c)
        d = z if comp else c
        table.append((len(d) & 0xffff, len(c) & 0xffff, (pos + len(payload)) | (1 if comp else 0)))
        payload += d
        while len(payload) % 16: payload += b'\0'
    total = pos + len(payload)
    out = bytearray(b'sges' + struct.pack('<HHII', 4, n, len(data), total))
    for t in table: out += struct.pack('<HHI', *t)
    out += b'\0' * (pos - len(out)) + payload
    return bytes(out)
TYPES_DEFAULT = None
def write_wad(path, blocks, aset_rows, types, block_flags=None, build_id=b''):
    """blocks: list of (name, P, decompressed bytes); aset_rows: list of (hash, (b3,b2,b1,b0), typeindex);
    types: list of 36 type hashes."""
    comp = [segs(b) for _, _, b in blocks]
    names = b''.join(n.encode('latin1') + b'\0' for n, _, _ in blocks) + build_id
    indx_off = PAGE
    aset_off = indx_off + 12 * len(blocks)
    pths_off = aset_off + 16 * len(aset_rows)
    data_off = max(0x208000, (pths_off + len(names) + PAGE - 1) // PAGE * PAGE)
    ents = []; pg = data_off // PAGE
    for i, ((n, P, raw), c) in enumerate(zip(blocks, comp)):
        npg = (len(c) + PAGE - 1) // PAGE           # pages the packed data occupies in the file
        npg_full = (len(raw) + PAGE - 1) // PAGE    # INDX count = decompressed size (engine buffer)
        # flags: 0x80000000 | hi << 16 | lo ; lo = packed pages the engine reads, hi = caller-provided (texture memory, 4K units)
        hi = ((block_flags[i] >> 16) & 0x7fff) if block_flags and block_flags[i] is not None else 0
        fl = 0x80000000 | (hi << 16) | npg
        ents.append((pg, (P << 24) | max(npg, npg_full), fl)); pg += npg
    tail_pages = max((e[0] + (e[1] & 0xffffff)) for e in ents) - pg if ents else 0
    csum = zlib.crc32(b''.join(struct.pack('<III', *e) for e in ents)) & 0xffffffff
    hdr = bytearray(b'FFCS' + struct.pack('<II', 2, 7))
    for tag, a, b in ((b'INDX', indx_off, len(blocks)), (b'DATA', data_off, len(types)), (b'CSUM', csum, len(aset_rows)),
                      (b'ASET', aset_off, len(aset_rows)), (b'PTHS', pths_off, len(blocks))):
        hdr += tag + struct.pack('<II', a, b)
    for t in types: hdr += struct.pack('<I', t)
    with open(path, 'wb') as f:
        f.write(hdr + b'\0' * (PAGE - len(hdr)))
        f.write(b''.join(struct.pack('<III', *e) for e in ents))
        f.write(b''.join(struct.pack('<IHHHHI', h, *bl, ti) for h, bl, ti in aset_rows))
        f.write(names)
        f.write(b'\0' * (data_off - f.tell()))
        for c in comp:
            f.write(c); f.write(b'\0' * ((-len(c)) % PAGE))
        if tail_pages > 0: f.write(b'\0' * (tail_pages * PAGE))   # last block may be read as npages
    return ents

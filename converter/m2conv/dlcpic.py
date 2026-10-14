# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""DLC picture for the Extras > Downloadable Content details page.
The PC shell movie has an empty 274x339 frame (shape 800, depth 58 of DLCdetails_mc, sprite 809) and no code that
fills it.  We add a GFx external image (tag 1001) + a bitmap-filled rectangle and place it inside the frame; the
image is a shell texture asset of the same name shipped in shell-patch.wad."""
import struct, io, os
NAME = 'shell_dlc01_preview'
TEX_W = TEX_H = 512
FRAME = (433, -305, 703, 30)          # px, inside the frame's 4px border
SPRITE, FRAME_DEPTH, DEPTH = 809, 58, 62
def _tags(d, p, end):
    while p < end:
        h = struct.unpack_from('<H', d, p)[0]; hp = p; p += 2
        t = h >> 6; l = h & 63
        if l == 63: l = struct.unpack_from('<I', d, p)[0]; p += 4
        yield t, hp, p, l
        p += l
        if t == 0: break
def _tag(t, body):
    return struct.pack('<HI', (t << 6) | 63, len(body)) + body
class _BW:
    def __init__(s): s.bits = []
    def u(s, n, v):
        for i in range(n - 1, -1, -1): s.bits.append((v >> i) & 1)
    def s_(s, n, v): s.u(n, v & ((1 << n) - 1))
    def bytes(s):
        b = s.bits + [0] * (-len(s.bits) % 8)
        return bytes(int(''.join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))
def _nb(*v): return max(max(abs(x).bit_length() + 1 for x in v), 1)
def _rect(x0, x1, y0, y1):
    w = _BW(); n = _nb(x0, x1, y0, y1); w.u(5, n)
    for v in (x0, x1, y0, y1): w.s_(n, v)
    return w.bytes()
def _matrix(sx, sy, tx, ty):
    w = _BW(); w.u(1, 1); n = _nb(sx, sy); w.u(5, n); w.s_(n, sx); w.s_(n, sy)
    w.u(1, 0); n = _nb(tx, ty); w.u(5, n); w.s_(n, tx); w.s_(n, ty)
    return w.bytes()
def _shape(cid, bid, rect=None, tw=None, th=None):
    x0, y0, x1, y1 = rect or [v * 20 for v in FRAME]
    TW, TH = tw or TEX_W, th or TEX_H
    W, H = x1 - x0, y1 - y0
    b = struct.pack('<H', cid) + _rect(x0, x1, y0, y1)
    b += bytes([1, 0x41]) + struct.pack('<H', bid) + _matrix(round(W / TW * 65536), round(H / TH * 65536), x0, y0)
    b += bytes([0])                                    # no line styles
    w = _BW(); w.u(4, 1); w.u(4, 0)
    # style change: moveTo + fillStyle1
    w.u(1, 0); w.u(1, 0); w.u(1, 0); w.u(1, 1); w.u(1, 0); w.u(1, 1)
    n = _nb(x0, y0); w.u(5, n); w.s_(n, x0); w.s_(n, y0); w.u(1, 1)
    for dx, dy in ((W, 0), (0, H), (-W, 0), (0, -H)):   # clockwise (y down): interior on the right = fillStyle1
        n = _nb(dx, dy); w.u(1, 1); w.u(1, 1); w.u(4, n - 2); w.u(1, 0)
        if dx: w.u(1, 0); w.s_(n, dx)
        else: w.u(1, 1); w.s_(n, dy)
    w.u(6, 0)
    return _tag(2, b + w.bytes())
def _extimg(cid, name=None, w=None, h=None):
    name = name or NAME; n = name.encode(); f = (name + '.tga').encode()
    return _tag(1001, struct.pack('<HHHH', cid, 13, w or TEX_W, h or TEX_H) + bytes([len(n)]) + n + bytes([len(f)]) + f)
def patch_big(raw):
    """raw: decompressed CFX body (starts at the stage RECT). Returns patched bytes."""
    d = bytes(raw)
    nb = d[0] >> 3; p0 = (5 + 4 * nb + 7) // 8 + 4
    ids = set(); sp = None
    for t, hp, p, l in _tags(d, p0, len(d)):
        if t in (2, 22, 32, 83, 39, 37, 46, 84, 1001, 11, 10, 48, 75, 6, 21, 35, 36, 20, 7, 34):
            ids.add(struct.unpack_from('<H', d, p)[0])
        if t == 39 and struct.unpack_from('<H', d, p)[0] == SPRITE: sp = (hp, p, l)
    assert sp, 'DLCdetails sprite not found'
    hp, p, l = sp
    assert NAME.encode() not in d, 'already patched'
    bid = max(ids) + 1; sid = bid + 1
    # sprite body: find the PlaceObject2 of the frame (depth 58) and insert ours after it
    body = bytearray(d[p:p + l]); ins = None
    for t, h2, q, l2 in _tags(body, 4, len(body)):
        if t == 26 and struct.unpack_from('<H', body, q + 1)[0] == FRAME_DEPTH: ins = q + l2
        if t == 26 and struct.unpack_from('<H', body, q + 1)[0] == DEPTH: raise AssertionError('depth %d in use' % DEPTH)
    assert ins, 'frame placement not found'
    po = bytes([0x06]) + struct.pack('<HH', DEPTH, sid) + bytes([0])
    body[ins:ins] = struct.pack('<H', (26 << 6) | len(po)) + po
    newsprite = _tag(39, bytes(body))
    return d[:hp] + _extimg(bid) + _shape(sid, bid) + newsprite + d[p + l:]
def texture_nodes(png=None):
    """(NAME, INFO, BODY) nodes of a 512x512 DXT1 shell texture holding the preview picture."""
    from PIL import Image
    import mkpreview
    im = (Image.open(png).convert('RGB') if png else mkpreview.make()).resize((TEX_W, TEX_H), Image.LANCZOS)
    b = io.BytesIO(); im.save(b, 'DDS', pixel_format='DXT1'); dds = b.getvalue()
    size = TEX_W * TEX_H // 2
    body = dds[-size:] if len(dds) >= 128 + size else None
    assert body and dds[:4] == b'DDS ' and struct.unpack_from('<I', dds, 12)[0] == 512 and struct.unpack_from('<I', dds, 16)[0] == 512
    body = dds[128:128 + size]
    info = struct.pack('<HHHHHHH', TEX_W, TEX_H, 1, 1, 0, 1, 1) + b'DXT1' + struct.pack('<II', 0, size) + bytes(6) + struct.pack('<H', 1)
    assert len(info) == 34
    return [(b'NAME', NAME.encode() + b'\0', []), (b'INFO', info, []), (b'BODY', body, [])]

# ---- list page: small picture in the expanded row (dlc_info_mc, sprite 787: frame 29.9..119.5 x 82.15..171.75) ----
THUMB = 'shell_dlc01_thumb'; THUMB_W = THUMB_H = 128
ROW_SPRITE, ROW_AFTER, ROW_DEPTH = 787, 1, 8
THUMB_RECT = (640, 1683, 2350, 3395)          # twips, inside the 4px border
def _push(*items):
    b = b''
    for v in items:
        if isinstance(v, str): b += b'\0' + v.encode() + b'\0'
        else: b += b'\1' + struct.pack('<f', v)
    return b'\x96' + struct.pack('<H', len(b)) + b
def _visible_code():
    # setProperty("", _visible, _parent.mapname_txt.text != "")  -- rows without a DLC entry stay empty
    c = _push('', 7.0) + _push('_parent') + b'\x1c' + _push('mapname_txt') + b'\x4e' + _push('text') + b'\x4e'
    c += _push('') + b'\x49' + b'\x12' + b'\x23' + b'\x00'
    return _tag(12, c)
def _thumb_sprite(sid, shid):
    po = bytes([0x02]) + struct.pack('<HH', 1, shid)
    body = struct.pack('<HH', sid, 2)
    body += struct.pack('<H', (26 << 6) | len(po)) + po + _visible_code() + struct.pack('<H', 1 << 6)
    body += _visible_code() + struct.pack('<H', 1 << 6) + struct.pack('<H', 0)
    return _tag(39, body)
def _insert(d, sprite, after_depth, depth, cid, defs):
    nb = d[0] >> 3; p0 = (5 + 4 * nb + 7) // 8 + 4
    sp = None
    for t, hp, p, l in _tags(d, p0, len(d)):
        if t == 39 and struct.unpack_from('<H', d, p)[0] == sprite: sp = (hp, p, l)
    assert sp, 'sprite %d not found' % sprite
    hp, p, l = sp
    body = bytearray(d[p:p + l]); ins = None
    for t, h2, q, l2 in _tags(body, 4, len(body)):
        if t == 26 and struct.unpack_from('<H', body, q + 1)[0] == after_depth and ins is None: ins = q + l2
        if t == 26 and struct.unpack_from('<H', body, q + 1)[0] == depth: raise AssertionError('depth %d in use' % depth)
    assert ins, 'placement not found'
    po = bytes([0x06]) + struct.pack('<HH', depth, cid) + bytes([0])
    body[ins:ins] = struct.pack('<H', (26 << 6) | len(po)) + po
    return d[:hp] + defs + _tag(39, bytes(body)) + d[p + l:]
def _maxid(d):
    nb = d[0] >> 3; p0 = (5 + 4 * nb + 7) // 8 + 4; ids = set()
    for t, hp, p, l in _tags(d, p0, len(d)):
        if t in (2, 22, 32, 83, 39, 37, 46, 84, 1001, 11, 10, 48, 75, 6, 21, 35, 36, 20, 7, 34):
            ids.add(struct.unpack_from('<H', d, p)[0])
    return max(ids)
def patch_thumb(raw):
    d = bytes(raw)
    assert THUMB.encode() not in d, 'already patched'
    b = _maxid(d) + 1; sh = b + 1; sp = b + 2
    defs = _extimg(b, THUMB, THUMB_W, THUMB_H) + _shape(sh, b, THUMB_RECT, THUMB_W, THUMB_H) + _thumb_sprite(sp, sh)
    return _insert(d, ROW_SPRITE, ROW_AFTER, ROW_DEPTH, sp, defs)
def patch(raw):
    return patch_thumb(patch_big(raw))
def thumb_nodes():
    from PIL import Image, ImageFilter
    im = Image.open(__import__('cfg').w('art', 'dlc_thumb.png')).convert('RGB')
    im = im.resize((THUMB_W, THUMB_H), Image.LANCZOS).filter(ImageFilter.UnsharpMask(1.5, 60, 2))
    b = io.BytesIO(); im.save(b, 'DDS', pixel_format='DXT1'); dds = b.getvalue()
    size = THUMB_W * THUMB_H // 2; body = dds[128:128 + size]; assert len(body) == size
    info = struct.pack('<HHHHHHH', THUMB_W, THUMB_H, 1, 1, 0, 1, 1) + b'DXT1' + struct.pack('<II', 0, size) + bytes(6) + struct.pack('<H', 1)
    return [(b'NAME', THUMB.encode() + b'\0', []), (b'INFO', info, []), (b'BODY', body, [])]

# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Xbox button prompt data for Mercenaries2Remastered (<game>\\Mercenaries2Remastered\\prompts.bin), taken from the user's own PC game files.

The PC game ships the Xbox 360 button atlases (common_20_xbox_buttons, font_16_xbox_buttons) in vz.wad but
never uses them; Mercenaries2Remastered paints those glyphs over the PC action icons while a gamepad is used.
The file holds: PC atlas cells (by CRC of the texture data), the Xbox glyphs as RGBA and the Xbox font_16 atlas.

  prompts.py --game <game folder> [--out <file>]

Format (little endian): 'M2PR' u32 version(1)
  u32 natlas; natlas x { char name[32]; u32 crc, w, h, n; n x {u8 x0,y0,x1,y1,act} }
  u32 nglyph; nglyph x { u32 off; u8 w, h; u16 0 }; u32 npix; pix (RGBA)
  u32 f16crc; u32 nf16; f16 bytes"""
import os, sys, struct, zlib, io, argparse
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wad import Wad

ACT = ['NONE', 'Forward', 'Backward', 'MoveLeft', 'MoveRight', 'PrimaryAttack', 'PrimarySwitch', 'Reload',
       'SecondaryAttack', 'SecondarySwitch', 'MeleeAttack', 'Use', 'Jump', 'Crouch', 'Binoculars', 'PDA',
       'LookUp', 'LookDown', 'LookLeft', 'LookRight', 'Up', 'Down', 'Right', 'Walk', 'Sprint', 'Start', 'MOVE', 'LOOK']
A = {n: i for i, n in enumerate(ACT)}
# action shown by each icon cell of the PC atlases (cells in reading order)
PC_MAP = {
    'common_20_buttons': ['LookLeft', 'LookRight', 'LookDown', 'Binoculars', 'Forward', 'Backward', 'Crouch', 'Up',
        'Jump', 'MeleeAttack', 'Reload', 'Use', 'PDA', 'Start', 'MoveLeft', 'MoveRight', 'SecondarySwitch', 'PrimaryAttack',
        'SecondaryAttack', 'PrimarySwitch', 'MOVE', 'LOOK', 'Sprint', 'LookUp', 'Up', 'Down', 'PrimarySwitch', 'Right'],
    'common_18_buttons': ['LookLeft', 'LookRight', 'LookDown', 'Backward', 'Binoculars', 'NONE', 'Forward', 'Up',
        'Jump', 'MeleeAttack', 'Reload', 'Use', 'Crouch', 'PrimarySwitch', 'Start', 'PrimarySwitch', 'SecondarySwitch', 'MOVE',
        'MoveLeft', 'MoveRight', 'SecondaryAttack', 'Sprint', 'PDA', 'PrimaryAttack', 'LOOK', 'LookUp', 'Up', 'Down', 'Right'],
}
GLYPHS = ['LB', 'RB', 'START', 'BACK', 'DPAD', 'DPAD_UP', 'DPAD_DOWN', 'DPAD_RIGHT', 'DPAD_LEFT', 'DPAD_UD', 'DPAD_LR',
          'LS', 'RS', 'LS_LEFT', 'LS_RIGHT', 'RS_LEFT', 'RS_RIGHT', 'LS_CLICK', 'RS_CLICK', 'A', 'B', 'X', 'Y', 'LT', 'RT',
          'LS_UP', 'LS_DOWN', 'RS_UP', 'RS_DOWN']

def dds(w, h, fmt, data):
    hd = bytearray(128)
    struct.pack_into('<4sIIIIIII', hd, 0, b'DDS ', 124, 0x1007, h, w, 0, 0, 1)
    struct.pack_into('<II4s', hd, 76, 32, 4, fmt)
    struct.pack_into('<I', hd, 108, 0x1000)
    return Image.open(io.BytesIO(bytes(hd) + data)).convert('RGBA')

def textures(d):
    """(name, fmt, w, h, data offset, size) of every texture in a decompressed block"""
    out = []; p = 0
    while True:
        i = d.find(b'DXT', p)
        if i < 0: return out
        fmt = d[i:i + 4]
        w, h = struct.unpack('<HH', d[i - 14:i - 10])
        size = struct.unpack('<I', d[i + 8:i + 12])[0]
        j = d.rfind(b'.ftga', 0, i); k = d.rfind(b'\\', 0, j)
        name = d[k + 1:j].decode(errors='replace') if j > 0 and i - j < 40 else 'tex'
        out.append((name, fmt, w, h, i + 20, size)); p = i + 4

def decode(d, t):
    name, fmt, w, h, off, size = t
    return dds(w, h, fmt, d[off:off + size])

def rowbands(a):
    prof = a.any(1); bands = []; s = None
    for y, v in enumerate(prof):
        if v and s is None: s = y
        if not v and s is not None: bands.append((s, y)); s = None
    if s is not None: bands.append((s, len(prof)))
    return bands

def cells(im, thr=20, colgap=2):
    """icon rectangles of an atlas: alpha row bands split at transparent column gaps"""
    a = np.array(im)[:, :, 3] > thr
    out = []
    for y0, y1 in rowbands(a):
        p = a[y0:y1].any(0); s = None; z = 0; last = 0
        for x, v in enumerate(list(p) + [False] * (colgap + 1)):
            if v:
                if s is None: s = x
                z = 0; last = x
            elif s is not None:
                z += 1
                if z >= colgap: out.append([s, y0, last + 1, y1]); s = None
    return out

def parts(path):
    out = [path]; k = 1
    while os.path.exists('%s_%d' % (path, k)): out.append('%s_%d' % (path, k)); k += 1
    return out

def block(w, name):
    for i, n in enumerate(w.names):
        if n.lower().endswith('\\' + name.lower()): return w.block(i)
    raise KeyError(name)

def build(game, out):
    vz = Wad(parts(os.path.join(game, 'data', 'vz.wad')))
    sh = Wad(parts(os.path.join(game, 'data', 'shell.wad')))
    common = block(sh, 'common_P000_Q3.block')
    xb20 = block(vz, 'common_20_xbox_buttons_P000_Q3.block')
    f16pc = block(sh, 'font_16_buttons_P000_Q3.block')
    f16xb = block(vz, 'font_16_xbox_buttons_P000_Q3.block')
    T = {t[0]: t for t in textures(common)}
    b = bytearray(b'M2PR' + struct.pack('<I', 1))
    b += struct.pack('<I', len(PC_MAP))
    for name, acts in PC_MAP.items():
        t = T[name]; raw = common[t[4]:t[4] + t[5]]
        cs = cells(decode(common, t))
        if len(cs) != len(acts): raise RuntimeError('%s: %d icons, expected %d' % (name, len(cs), len(acts)))
        b += name.encode().ljust(32, b'\0') + struct.pack('<IIII', zlib.crc32(raw) & 0xffffffff, t[2], t[3], len(cs))
        for c, a in zip(cs, acts): b += bytes([c[0], c[1], c[2], c[3], A[a]])
    t = textures(xb20)[0]; im = decode(xb20, t); xc = cells(im, 20, 1)
    if len(xc) != len(GLYPHS): raise RuntimeError('xbox atlas: %d glyphs' % len(xc))
    pix = bytearray(); meta = []
    for c in xc:
        g = im.crop(tuple(c)).convert('RGBA'); w, h = g.size
        meta.append((len(pix), w, h)); pix += g.tobytes()
    b += struct.pack('<I', len(meta))
    for off, w, h in meta: b += struct.pack('<IBBH', off, w, h, 0)
    b += struct.pack('<I', len(pix)) + pix
    tp = textures(f16pc)[0]; tx = textures(f16xb)[0]
    rp = f16pc[tp[4]:tp[4] + tp[5]]; rx = f16xb[tx[4]:tx[4] + tx[5]]
    b += struct.pack('<II', zlib.crc32(rp) & 0xffffffff, len(rx)) + rx
    os.makedirs(os.path.dirname(out), exist_ok=True)
    tmp = out + '.new'
    open(tmp, 'wb').write(b); os.replace(tmp, out)
    return len(b)

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True); ap.add_argument('--out')
    a = ap.parse_args()
    out = a.out or os.path.join(a.game, 'Mercenaries2Remastered', 'prompts.bin')
    try:
        n = build(a.game, out)
    except Exception as e:
        print('ERROR: button prompt data not created: %s' % e); sys.exit(1)
    print('prompts: %s (%d bytes)' % (out, n))

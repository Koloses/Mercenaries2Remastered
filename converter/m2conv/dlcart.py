# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Aerial picture of the DLC map (background of the DLC menu picture) from the converted DLC01.wad."""
import os, re, struct
from wad import Wad
from wadr import read_block
from dxt import dds
from xtex import pc_level_size
from PIL import Image
def terrain(dlcwad, out, tile=256):
    W = Wad(dlcwad); tiles = {}
    for i in range(len(W.ents)):
        for h, t, po, n in read_block(W, i):
            if t != 0xf011157a: continue
            d = {tg: dd for tg, dd, k in n}
            v = tiles.setdefault(h, {'bodies': []})
            if b'NAME' in d: v['name'] = d[b'NAME'][:-1].decode('latin1')
            if b'INFO' in d: v['info'] = d[b'INFO']
            if b'BODY' in d: v['bodies'].append(d[b'BODY'])
    g = {}
    for h, v in tiles.items():
        m = re.match(r'dlc01_terrainglobal_r(\d+)_c(\d+)', v.get('name', ''))
        if not m or 'info' not in v or not v['bodies']: continue
        w, hh = struct.unpack_from('<HH', v['info']); f = v['info'][14:18]
        fm = 'DXT1' if f == b'DXT1' else 'DXT5'
        b = max(v['bodies'], key=len); s = w
        while s > 4 and pc_level_size(s, s, 0, fm) > len(b): s //= 2
        g[(int(m[1]), int(m[2]))] = dds(s, s, f, b[:pc_level_size(s, s, 0, fm)]).resize((tile, tile))
    if not g: raise RuntimeError('no DLC terrain textures found')
    R = max(r for r, c in g) + 1; C = max(c for r, c in g) + 1
    im = Image.new('RGB', (C * tile, R * tile))
    for (r, c), t in g.items(): im.paste(t.convert('RGB'), (c * tile, r * tile))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    im.save(out)
    return im.size

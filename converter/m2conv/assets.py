# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Fetch individual assets (chunk bytes) from WADs using the quick index."""
import sys, os, pickle, struct
sys.path.insert(0, os.path.dirname(__file__))
from wad import Wad
from ucfx import parse_block
from xboxsrc import iso_wad
import cfg
def open_src(which):
    return {'pcvz': lambda: Wad(cfg.pc_parts('vz.wad')), 'xvz': lambda: iso_wad(cfg.ISO, 'vz.wad'),
            'pcshell': lambda: Wad(cfg.pc_parts('shell.wad')), 'xshell': lambda: iso_wad(cfg.ISO, 'shell.wad'),
            'dlc': lambda: Wad(cfg.w('dlc', 'DLC01.doh')),
            'tu4vz': lambda: Wad(cfg.w('tu4', 'vz-patch.wad')), 'tu4shell': lambda: Wad(cfg.w('tu4', 'shell-patch.wad'))}[which]()
class Src:
    def __init__(s, which):
        s.w = open_src(which)
        p = cfg.w('q_%s.pkl' % which)
        if os.path.exists(p): s.names, s.idx = pickle.load(open(p, 'rb'))
        s.cache = {}
    def block(s, i):
        if i not in s.cache:
            if len(s.cache) > 8: s.cache.clear()
            b = s.w.block(i); hdr, objs, end = parse_block(b, s.w.be)
            s.cache[i] = (b, hdr, objs, end)
        return s.cache[i]
    def get(s, h, which=0):
        bi, t, sz = s.idx[h][which]
        b, hdr, objs, end = s.block(bi)
        for k, e in enumerate(hdr):
            if e[0] == h and k < len(objs):
                o = objs[k]
                ch = {}
                for tag, off, size, x1, x2 in o['chunks']:
                    ch[tag.decode('latin1')] = None if off == 0xFFFFFFFF else b[o['data'] + off:o['data'] + off + size]
                return dict(type=t, hdr=e, chunks=ch, obj=o, block=bi, raw=b[o['at']:o['end']])
        return None
def get_info(S, h):
    for k in range(len(S.idx[h])):
        a = S.get(h, k)
        if a and 'INFO' in a['chunks']: return a
    return None

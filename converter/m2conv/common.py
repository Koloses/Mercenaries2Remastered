# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assets import Src
from wad import assets as wad_assets
from meshpairs import chunk_list
_aset = {}
def aset(S):
    k = id(S)
    if k not in _aset: _aset[k] = wad_assets(S.w)
    return _aset[k]
def parts(S, h):
    """{P: (block, asset dict)} using ASET row (P3,P2,P1,P0)."""
    row = aset(S)[h]
    blk = {3: row[0], 2: row[1], 1: row[2], 0: row[3]}
    out = {}
    for k, (bi, t, sz) in enumerate(S.idx[h]):
        a = S.get(h, k)
        P = [p for p, b in blk.items() if b == bi]
        out[P[0] if P else -1] = a
    return out

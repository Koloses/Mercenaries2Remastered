# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
from common import *
from wadr import read_block
_bc={}
def get_nodes(S,h,k=0):
    bi=S.idx[h][k][0]
    key=(id(S),bi)
    if key not in _bc:
        if len(_bc)>6: _bc.clear()
        _bc[key]=read_block(S.w,bi)
    for hh,t,po,nodes in _bc[key]:
        if hh==h: return nodes
def show(nodes,d=0,lim=40,n=[0]):
    for tag,data,kids in nodes:
        n[0]+=1
        if n[0]>lim: return
        print('  '*d+tag.decode('latin1'), None if data is None else len(data), '' if data is None else data[:32].hex())
        show(kids,d+1,lim,n)
def get_nodes_t(S,h,t):
    if h not in S.idx: return None
    for bi,tt,sz in S.idx[h]:
        if tt!=t: continue
        key=(id(S),bi)
        if key not in _bc:
            if len(_bc)>6: _bc.clear()
            _bc[key]=read_block(S.w,bi)
        for hh,t2,po,nodes in _bc[key]:
            if hh==h and t2==t: return nodes
    return None

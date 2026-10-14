# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
import sys,struct,random,collections,time; 
from assets import Src
def chunk_list(S,a):
    b,hdr,objs,end=S.block(a['block']); o=a['obj']
    return [(t.decode(),None if off==0xffffffff else b[o['data']+off:o['data']+off+size],x1,x2) for t,off,size,x1,x2 in o['chunks']]
def decls(cl):
    out=[]
    for i,(t,d,a,c) in enumerate(cl):
        if t=='decl': out.append((i,d))
    return out

# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""PC layer tables used by the layer converter, built from the user's PC vz.wad:
   namedb.pkl   hash -> original string of every layer Name component
   schemadb.pkl classhash -> component name / schema / info counters
Resumable: progress is kept in pcdb_part.pkl (time limit per call)."""
import os, pickle, struct, time, collections
from common import *
from wadr import read_block
from preimage import ph
import cfg
LT = (0xe6b81a54, 0x5647c35d)
def build(limit=1e9):
    part = cfg.w('pcdb_part.pkl')
    names, db, done = pickle.load(open(part, 'rb')) if os.path.exists(part) else ({}, {}, set())
    P = Src('pcvz')
    blocks = sorted({l[0][0] for h, l in P.idx.items() if l[0][1] in LT})
    T = time.time()
    for bi in blocks:
        if bi in done: continue
        if time.time() - T > limit: break
        for h, t, po, nodes in read_block(P.w, bi):
            if t not in LT: continue
            for tg, d, kids in nodes:
                if tg != b'COMP': continue
                c = {a: b for a, b, _ in kids}
                info = c.get(b'info'); schm = c.get(b'schm')
                if info and info.startswith(b'Name\0'):
                    for s in (c.get(b'data') or b'').split(b'\0'):
                        k = len(s)
                        while k > 0 and 32 <= s[k - 1] < 127: k -= 1
                        st = s[k:]
                        if len(st) >= 2: names[ph(st)] = st
                if not info or schm is None: continue
                nm = info.split(b'\0')[0]; ch = struct.unpack_from('<I', info, len(nm) + 1)[0]
                ent = db.setdefault(ch, {'name': nm, 'schm': collections.Counter(), 'rest': collections.Counter()})
                ent['schm'][schm] += 1; ent['rest'][info[len(nm) + 5:]] += 1
        done.add(bi)
    if len(done) < len(blocks):
        pickle.dump((names, db, done), open(part, 'wb'))
        return False, len(done), len(blocks)
    pickle.dump(names, open(cfg.w('namedb.pkl'), 'wb'))
    pickle.dump((db, done), open(cfg.w('schemadb.pkl'), 'wb'))
    if os.path.exists(part): os.remove(part)
    return True, len(done), len(blocks)

# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Script asset (0x42498680): INFO/DEPS/BINN, with 3-way merge against PC when available."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lua51 import convert as lua_conv
from luamerge import merge
from merge_rules import RULES
def info_conv(d):
    ver = d[0]; n = struct.unpack_from('>H', d, 1)[0]; name = d[3:3 + n]
    rest = d[3 + n + 1:]
    a = struct.unpack_from('>H', rest, 0)[0]
    return bytes([ver]) + struct.pack('<H', n) + name + b'\0' + struct.pack('<H', a) + rest[2:], name.decode()
def deps_conv(d):
    n = d[0]; return bytes([n]) + b''.join(d[1 + 4 * i:5 + 4 * i][::-1] for i in range(n))
def deps_union(a, b):
    ha = [a[1 + 4 * i:5 + 4 * i] for i in range(a[0])] if a else []
    hb = [b[1 + 4 * i:5 + 4 * i] for i in range(b[0])] if b else []
    out = ha + [h for h in hb if h not in ha]
    return bytes([len(out)]) + b''.join(out)
NO_BRIEFING = (b'Obama', b'Palin', b'ChickenSuit')    # odd in briefings (faces, chicken suit)
def briefing_safe(binn, log):
    """wifpmcinterior IsCostumeBriefingSafe (return outfit and not outfit.bNoBriefing) becomes
    return outfit and outfit.Name is none of NO_BRIEFING: briefings keep every other outfit instead of
    switching to the original one"""
    from luabc import load, dump, op, A, B, C, mk_ABC, mk_ABx
    q = load(binn); n = 0
    def K(p, v):
        for i, k in enumerate(p.k):
            if k[0] == 4 and k[1] == v: return i
        p.k.append((4, v)); return len(p.k) - 1
    for p in q.p:
        if not any(k[0] == 4 and k[1] == b'bNoBriefing' for k in p.k) or len(p.code) > 32: continue
        t = [i for i in range(len(p.code) - 3) if op(p.code[i]) == 27 and op(p.code[i + 1]) == 22 and op(p.code[i + 3]) == 19]
        if len(t) != 1: continue
        t = t[0]; ra, rb = A(p.code[t]), B(p.code[t])
        if any(op(c) == 36 for c in p.code): continue                 # closures: keep it simple
        def jmp(frm, to): return mk_ABx(22, 0, to - (frm + 1) + 131071)
        code = p.code[:t] + [p.code[t]]                               # TESTSET ra rb 0: falsy outfit -> return it
        ret_at = t + 3 + 2 * len(NO_BRIEFING) + 1                     # after the name checks and LOADBOOL true
        code.append(jmp(len(code), ret_at))
        code.append(mk_ABC(6, ra, rb, 256 + K(p, b'Name')))          # GETTABLE ra rb 'Name'
        fail = ret_at + 1
        for nm in NO_BRIEFING:
            code.append(mk_ABC(23, 1, ra, 256 + K(p, nm)))           # EQ: name == nm -> next (jump to false)
            code.append(jmp(len(code), fail))
        code.append(mk_ABC(2, ra, 1, 0))                             # LOADBOOL ra true
        assert len(code) == ret_at
        code.append(mk_ABC(30, ra, 2, 0))                            # RETURN ra
        code.append(mk_ABC(2, ra, 0, 0))                             # LOADBOOL ra false
        code.append(mk_ABC(30, ra, 2, 0))                            # RETURN ra
        code.append(mk_ABC(30, 0, 1, 0))
        line = p.lineinfo[t] if p.lineinfo else 0
        if p.lineinfo: p.lineinfo = p.lineinfo[:t] + [line] * (len(code) - t)
        p.code = code; n += 1
    if n: log.append('script wifpmcinterior: outfits briefing safe except %s' % b', '.join(NO_BRIEFING).decode())
    return dump(q, False) if n else binn
def convert(nodes, log, pc_nodes=None, base_nodes=None):
    c = {t: d for t, d, k in nodes}
    info, name = info_conv(c[b'INFO'])
    deps = deps_conv(c[b'DEPS']) if c.get(b'DEPS') else None
    th = lua_conv(c[b'BINN'], True, False)[0]
    binn = th
    if pc_nodes and base_nodes:
        pcc = {t: d for t, d, k in pc_nodes}; bc = {t: d for t, d, k in base_nodes}
        if pcc.get(b'BINN') and bc.get(b'BINN'):
            xb = lua_conv(bc[b'BINN'], True, False)[0]
            mlog = []
            binn = merge(pcc[b'BINN'], xb, th, mlog, rules=RULES.get(name))
            log.append('script %s merged: %s' % (name, '; '.join(mlog)))
            deps = deps_union(pcc.get(b'DEPS'), deps)
            info = pcc[b'INFO']
    if binn is th:
        from luabc import load, dump
        q = load(th)
        if q.source is None: q.source = name.encode() if isinstance(name, str) else name
        binn = dump(q, False)
    if name == 'wifpmcinterior': binn = briefing_safe(binn, log)
    out = [(b'INFO', info, [])]
    if deps is not None and deps[0]: out.append((b'DEPS', deps, []))
    out.append((b'BINN', binn, []))
    return out

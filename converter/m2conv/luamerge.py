# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""3-way merge of Lua 5.1 bytecode modules at top-level-function granularity.
base = Xbox original, ours = PC, theirs = TU4/DLC.  Result based on PC main chunk."""
import struct
from luabc import *
DEBUGISH = lambda c: c is None or b' ' in c or c in (b'Debug', b'Printf', b'print', b'tostring', b'ASSERT', b'format', b'GetCallstack', b'...', b')', b'(') or c.endswith((b'=', b':', b'(')) or b'@' in c or b'!!' in c
def closures(mainp):
    """map name -> (closure pc, proto index, capture instrs list, set instr pc). Handles SETGLOBAL and SETTABLE R(k)."""
    out = {}; code = mainp.code; i = 0
    while i < len(code):
        ins = code[i]
        if op(ins) == OP['CLOSURE']:
            a = A(ins); pi = Bx(ins); np_ = mainp.p[pi].nups
            caps = code[i + 1:i + 1 + np_]
            j = i + 1 + np_
            name = None
            if j < len(code):
                n = code[j]
                if op(n) == OP['SETGLOBAL'] and A(n) == a: name = ('G', mainp.k[Bx(n)][1])
                elif op(n) == OP['SETTABLE'] and C(n) == a:
                    # table register B(n): find what it holds - previous GETGLOBAL into that reg
                    b = B(n); key = B(n)
                    kb = n >> 23 & 0x1ff
                    keyc = mainp.k[kb - 256][1] if kb >= 256 else None
                    tname = None
                    for q in range(j - 1, -1, -1):
                        pq = code[q]
                        if op(pq) == OP['GETGLOBAL'] and A(pq) == A(n): tname = mainp.k[Bx(pq)][1]; break
                    name = ('T', tname, keyc)
            if name is not None and name not in out: out[name] = (i, pi, caps, j)
            i = j + 1; continue
        i += 1
    return out
def proto_sig(p): return norm(p)
def debug_only_change(pc_p, base_p):
    """PC change consists only of debug-ish constants (Printf etc.)"""
    extra = consts(pc_p) - consts(base_p)
    return all(DEBUGISH(c) for c in extra)
def merge(pc_b, xb_b, th_b, log, prefer_theirs_on_conflict=True, rules=None):
    rules = rules or {}
    pc = load(pc_b); xb = load(xb_b); th = load(th_b)
    cpc, cxb, cth = closures(pc), closures(xb), closures(th)
    main_pc_changed = norm_main(pc) != norm_main(xb)
    main_th_changed = norm_main(th) != norm_main(xb)
    # choose base for the main chunk
    force = rules.get('main')
    if force == 'theirs' or (force is None and main_th_changed and (not main_pc_changed or debug_only_main(pc, xb))):
        res = th; resc = cth; other, otherc = pc, cpc; mode = 'theirs-main'
        if res.source is None: res.source = pc.source
    else:
        res = pc; resc = cpc; other, otherc = th, cth; mode = 'pc-main'
        if main_th_changed and main_pc_changed: log.append('CONFLICT main chunk (kept PC main)')
    log.append('main: ' + mode)
    for name in set(cpc) | set(cxb) | set(cth):
        sp = proto_sig(pc.p[cpc[name][1]]) if name in cpc else None
        sx = proto_sig(xb.p[cxb[name][1]]) if name in cxb else None
        st = proto_sig(th.p[cth[name][1]]) if name in cth else None
        pc_changed = sp != sx; th_changed = st != sx
        if pc_changed and name in cpc and name in cxb and debug_only_change(pc.p[cpc[name][1]], xb.p[cxb[name][1]]): pc_changed_real = False
        else: pc_changed_real = pc_changed
        want = None   # which side's proto to use
        key = name[1] if name[0] == 'G' else name[2]
        if key in rules.get('fn', {}):
            want = rules['fn'][key]
        elif th_changed and not pc_changed_real: want = 'th'
        elif pc_changed and not th_changed: want = 'pc'
        elif th_changed and pc_changed_real:
            want = 'th' if prefer_theirs_on_conflict else 'pc'; log.append('CONFLICT %s -> %s' % (str(name), want))
        else: want = 'th' if mode == 'theirs-main' else 'pc'
        src, srcc = (th, cth) if want == 'th' else (pc, cpc)
        if name not in srcc:
            if name in resc and want == 'th': log.append('removed by theirs: %s (kept)' % str(name))
            continue
        if src is res: continue
        if name in resc:
            ri = resc[name]; si = srcc[name]
            if res.p[ri[1]].nups != src.p[si[1]].nups or res.code[ri[0] + 1:ri[0] + 1 + res.p[ri[1]].nups] != src.code[si[0] + 1:si[0] + 1 + src.p[si[1]].nups]:
                log.append('upvalue mismatch %s: kept %s' % (str(name), 'res')); continue
            res.p[ri[1]] = src.p[si[1]]
        else:
            add_function(res, name, src.p[srcc[name][1]], log)
    for fname, repl in rules.get('kpatch', {}).items():
        ent = [v for k, v in closures(res).items() if (k[1] if k[0] == 'G' else k[2]) == fname]
        if not ent: log.append('kpatch: no %s' % fname); continue
        q = res.p[ent[0][1]]
        for i, k in enumerate(q.k):
            if k[0] == 4 and k[1] in repl: q.k[i] = (4, repl[k[1]])
        log.append('kpatched %s' % fname)
    for fname, (anchor, last) in rules.get('graft', {}).items():
        log.append('graft %s: %s' % (fname, graft_after(res, pc, fname, anchor, last)))
    for fname, (reg, field, label) in rules.get('trace', {}).items():
        log.append('trace %s: %s' % (fname, trace_arg_field(res, fname, reg, field, label)))
    for mod in rules.get('prepend_import', []):
        prepend_call(res, b'import', mod); log.append('prepended import %s' % mod)
    return dump(res, False)
def prepend_call(res, fn, arg):
    r = res.maxstack; res.maxstack += 2
    kf = kidx(res, fn); ka = kidx(res, arg)
    ins = [mk_ABx(OP['GETGLOBAL'], r, kf), mk_ABx(OP['LOADK'], r + 1, ka), mk_ABC(OP['CALL'], r, 2, 1)]
    # jump targets are relative, prepending at 0 keeps them valid
    res.code[0:0] = ins
    if res.lineinfo: res.lineinfo[0:0] = [res.lineinfo[0]] * 3
def norm_main(p):
    """main chunk signature: instructions with constants resolved, excluding named-closure definitions"""
    named = closures(p); skip = set()
    for name, (ci, pi, caps, si) in named.items():
        skip.update(range(ci, si + 1))
    out = []
    for i, ins in enumerate(p.code):
        if i in skip: continue
        o = op(ins)
        if o in (OP['LOADK'], OP['GETGLOBAL'], OP['SETGLOBAL']): out.append((OPS[o], A(ins), repr(p.k[Bx(ins)][1:])))
        elif o == OP['CLOSURE']: out.append(('CLOSURE', A(ins), norm(p.p[Bx(ins)])))
        elif o in (OP['JMP'], OP['FORLOOP'], OP['FORPREP']): out.append((OPS[o], A(ins)))
        else:
            b, c = B(ins), C(ins)
            rb = repr(p.k[b - 256][1:]) if b >= 256 and o not in (OP['CALL'], OP['NEWTABLE'], OP['RETURN']) else b
            rc = repr(p.k[c - 256][1:]) if c >= 256 and o not in (OP['CALL'], OP['NEWTABLE'], OP['RETURN']) else c
            out.append((OPS[o], A(ins), rb, rc))
    return repr(out).encode()
def debug_only_main(pc, xb):
    extra = set(k[1] for k in pc.k if k[0] == 4) - set(k[1] for k in xb.k if k[0] == 4)
    named = closures(pc)
    extra -= {n[1] for n in named if n[0] == 'G'} | {n[2] for n in named if n[0] == 'T'}
    return all(DEBUGISH(c) for c in extra)
def kidx(p, s):
    for i, k in enumerate(p.k):
        if k[0] == 4 and k[1] == s: return i
    p.k.append((4, s)); return len(p.k) - 1
def add_function(res, name, proto, log):
    if proto.nups:
        log.append('cannot add %s (has upvalues)' % str(name)); return
    if name[0] != 'G':
        log.append('cannot add %s (table member)' % str(name)); return
    # insert before final RETURN: CLOSURE r, idx ; SETGLOBAL r, k
    r = res.maxstack; res.maxstack += 1
    res.p.append(proto); pi = len(res.p) - 1; ki = kidx(res, name[1])
    if ki > 0x3ffff: raise ValueError('too many constants')
    last = len(res.code) - 1
    assert op(res.code[last]) == OP['RETURN']
    res.code[last:last] = [mk_ABx(OP['CLOSURE'], r, pi), mk_ABx(OP['SETGLOBAL'], r, ki)]
    if res.lineinfo: res.lineinfo[last:last] = [res.lineinfo[last]] * 2
    log.append('added %s' % str(name))
def trace_arg_field(res, fname, argreg, field, label):
    """insert at start of function `fname`: if R(argreg)[field] then Debug.Printf(label, R(argreg)[field]) end"""
    ent = [v for k, v in closures(res).items() if (k[1] if k[0] == 'G' else k[2]) == fname]
    if not ent: return False
    q = res.p[ent[0][1]]
    r = q.maxstack; q.maxstack += 3
    kD, kP, kL, kF = kidx(q, b'Debug'), kidx(q, b'Printf'), kidx(q, label), kidx(q, field)
    J = lambda o, a, sbx: o | (a << 6) | ((sbx + 131071) << 14)
    ins = [mk_ABC(6, r + 2, argreg, kF | 256), mk_ABC(26, r + 2, 0, 0), J(22, 0, 4),
           mk_ABx(5, r, kD), mk_ABC(6, r, r, kP | 256), mk_ABx(1, r + 1, kL), mk_ABC(28, r, 3, 1)]
    q.code[0:0] = ins
    if q.lineinfo: q.lineinfo[0:0] = [q.lineinfo[0]] * len(ins)
    return True
def guard_global_index(res, fname, gname, argreg=0):
    """prepend to function `fname`: if gname[R(argreg)] == nil then return end"""
    ent = [v for k, v in closures(res).items() if (k[1] if k[0] == 'G' else k[2]) == fname]
    if not ent: return False
    q = res.p[ent[0][1]]
    r = q.maxstack; q.maxstack += 1
    kg = kidx(q, gname)
    J = lambda sbx: 22 | ((sbx + 131071) << 14)
    ins = [mk_ABx(5, r, kg), mk_ABC(6, r, r, argreg), mk_ABC(26, r, 0, 1), J(1), mk_ABC(30, 0, 1, 0)]   # TEST r 1: truthy -> jump over RETURN
    q.code[0:0] = ins
    if q.lineinfo: q.lineinfo[0:0] = [q.lineinfo[0]] * len(ins)
    return True
def _fn(p, fname):
    ent = [v for k, v in closures(p).items() if (k[1] if k[0] == 'G' else k[2]) == fname]
    return p.p[ent[0][1]] if ent else None
def _call_after(q, const):
    """index of the CALL that calls the function fetched with constant `const`"""
    for i, ins in enumerate(q.code):
        if op(ins) in (OP['GETTABLE'], OP['SELF']) and C(ins) >= 256 and q.k[C(ins) - 256][1] == const:
            for j in range(i + 1, len(q.code)):
                if op(q.code[j]) == OP['CALL'] and A(q.code[j]) == A(ins): return j
    return None
def graft_after(res, pc, fname, anchor, last):
    """Copy the PC function's instructions that follow the call of `anchor` up to the last call of `last`
    into the same place of the result's function (the theirs version lacks a PC-only addition there).
    Both functions must use the same registers around the anchor."""
    q, s = _fn(res, fname), _fn(pc, fname)
    if q is None or s is None: return 'function missing'
    if any(op(c) == OP['SELF'] and C(c) >= 256 and q.k[C(c) - 256][1] == last for c in q.code): return 'already there'
    qa, sa = _call_after(q, anchor), _call_after(s, anchor)
    if qa is None or sa is None: return 'anchor missing'
    if q.code[qa] != s.code[sa]: return 'anchor call differs'
    se = None
    for i in range(sa + 1, len(s.code)):
        c = s.code[i]
        if op(c) == OP['SELF'] and C(c) >= 256 and s.k[C(c) - 256][1] == last:
            for j in range(i + 1, len(s.code)):
                if op(s.code[j]) == OP['CALL'] and A(s.code[j]) == A(c): se = j; break
    if se is None: return 'block end missing'
    block = []
    for c in s.code[sa + 1:se + 1]:
        o = op(c)
        if o in (OP['LOADK'], OP['GETGLOBAL']): c = mk_ABx(o, A(c), kidx(q, s.k[Bx(c)][1]))
        elif o in (OP['SELF'], OP['GETTABLE']):
            b, cc = B(c), C(c)
            if cc >= 256: cc = 256 + kidx(q, s.k[cc - 256][1])
            c = mk_ABC(o, A(c), b, cc)
        elif o in (OP['CALL'], OP['NEWTABLE'], OP['MOVE']): pass
        else: return 'unsupported %s' % OPS[o]
        block.append(c)
    at = qa + 1; n = len(block)
    for i, c in enumerate(q.code):                       # relative jumps across the insertion point
        if op(c) in (OP['JMP'], OP['FORLOOP'], OP['FORPREP']):
            sbx = Bx(c) - 131071; t = i + 1 + sbx
            if i < at <= t: sbx += n
            elif t < at <= i: sbx -= n
            else: continue
            q.code[i] = (c & 0x3fff) | ((sbx + 131071) << 14)
    q.code[at:at] = block
    if q.lineinfo: q.lineinfo[at:at] = [q.lineinfo[qa]] * n
    q.maxstack = max(q.maxstack, s.maxstack)
    return '%d instructions' % n

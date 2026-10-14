# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Convert Xbox 360 DLC01.doh -> PC DLC01.wad (Mercenaries2Remastered content)."""
import sys, os, struct, pickle, time, collections, traceback
from getn import *
from wadr import read_block, info, ent_info
from wadw import ucfx, block, write_wad
import conv_layer, conv_strings, conv_script, conv_texture, conv_simple, conv_mesh, hkpack
import cfg
OUT = sys.argv[1] if len(sys.argv) > 1 else cfg.OUT
LIMIT = float(os.environ.get('M2LIMIT', '1e9'))
os.makedirs(OUT, exist_ok=True)
P = Src('pcvz'); X = Src('xvz'); T = Src('tu4vz'); D = Src('dlc')
enum = [d for t, d, k in get_nodes(P, [h for h, l in P.idx.items() if l[0][1] == 0xe6b81a54][0]) if t == b'enum'][0]
conv_layer.init(enum)
def _script_consts():
    from luabc import load
    def kall(p, o):
        for k in p.k:
            if k[0] == 4: o.add(k[1].rstrip(b'\0'))
        for q in p.p: kall(q, o)
        return o
    ks = set()
    for S in (D, P):
        for h, l in S.idx.items():
            if any(t == 0x42498680 for b, t, s in l):
                try: ks |= kall(load([d for tg, d, k in get_nodes_t(S, h, 0x42498680) if tg == b'BINN'][0]), set())
                except Exception: pass
    return ks
conv_layer.set_script_names(_script_consts())
W = D.w; types, rows = info(W)
log = []; stats = collections.Counter()
# 1. gather objects per asset
objs = collections.defaultdict(dict)    # (h,t) -> {P: (block index, po, nodes)}
order = []
T0 = time.time()
for i in range(len(W.ents)):
    e = ent_info(W, i)
    for h, t, po, nodes in read_block(W, i):
        objs[(h, t)][e['P']] = (i, po, nodes)
        order.append((i, h, t, e['P']))
print('read', len(objs), 'assets', int(time.time() - T0), 's'); sys.stdout.flush()
def anim_events(d):
    n = struct.unpack_from('>I', d, 0)[0]; o = bytearray(struct.pack('<I', n)); p = 4
    for _ in range(n):
        if p + 4 > len(d): break
        o += d[p:p + 4][::-1]; p += 4
        for _ in range(2):
            e = d.index(b'\0', p) + 1; o += d[p:e]; p = e
    return bytes(o) + d[p:]
def anim(nodes, lg):
    out = []
    for t, d, k in nodes:
        if t == b'data' and d and d[:4] == b'\x57\xe0\xe0\x57':
            p = hkpack.Pack(d); out.append((t, p.convert(lg), []))
        elif t == b'info': out.append((t, conv_simple.sw(d, 2), []))
        elif t == b'trnm': out.append((t, conv_simple.sw(d[:4], 2) + conv_simple.sw(d[4:], 4), []))   # u16 count, u16, u32 hashes
        elif t == b'MINF': out.append((t, conv_simple.sw(d[:4], 4) + conv_simple.sw(d[4:], 2), []))
        elif t == b'TRCK': out.append((t, conv_simple.sw(d[:8], 4) + conv_simple.sw(d[8:], 2), []))
        elif t == b'evnt': out.append((t, anim_events(d), []))
        elif d is None: out.append((t, None, anim(k, lg)))
        else: out.append((t, conv_simple.sw(d, 4), []))
    return out
EFFECT_SW2 = {b'EFCT', b'EMTR'}
def effect(nodes, lg, path=''):
    out = []
    for t, d, k in nodes:
        p = path + '/' + t.decode('latin1')
        if d is None: out.append((t, None, effect(k, lg, p))); continue
        w = 2 if (t in EFFECT_SW2 or p.endswith('EMIT/GEOM')) else 4
        out.append((t, conv_simple.sw(d, w), effect(k, lg, p) if k else []))
    return out
SKIP_T = set()
import wbconv, sbconv
XAUD = cfg.w('dlc', 'audios')
AUDOUT = os.path.join(OUT, 'Data', 'Audios'); os.makedirs(AUDOUT, exist_ok=True)
def audio(h, t, d, lg):
    if t == 0x9f8bca10: o, c = sbconv.convert(d); return o
    if t == 0xe5273c14:
        if struct.unpack_from('>H', d, 8)[0] == 0: lg.append('WARN sounddb %08x unusual header' % h)
        return sbconv.convert_db(d)
    if d[10] == 0: return wbconv.convert_mem(d)
    n, hs, E = wbconv.parse(d)
    name = d[0x18:hs].split(b'\0')[0].decode().lower()[:-4]
    need = max(e[11] + e[6] for e in E)
    cands = sorted(f for f in os.listdir(XAUD) if f.lower().startswith(name) and os.path.getsize(os.path.join(XAUD, f)) == need)
    used = getattr(audio, 'used', set()); audio.used = used
    cands = [f for f in cands if f not in used] or cands
    src = cands[0]; used.add(src)
    bank, pws = wbconv.convert_stream(d, os.path.join(XAUD, src))
    open(os.path.join(AUDOUT, src), 'wb').write(pws); lg.append('stream bank %08x -> %s (%d bytes)' % (h, src, len(pws)))
    return bank
_TU4M = None
def tu4_merged():
    global _TU4M
    if _TU4M is None:
        _TU4M = {}
        from wad import Wad
        W = Wad(cfg.w('tu4pass1', 'vz-patch.wad'))
        for i in range(len(W.ents)):
            for hh, tt, po, nn in read_block(W, i):
                if tt == 0x42498680: _TU4M[hh] = nn
    return _TU4M
def pc_script(h):
    # the PC side of the merge is our TU4-merged PC script when TU4 changed it, else the PC original
    if h in tu4_merged(): return tu4_merged()[h]
    for S in (P,):
        if h in S.idx: return get_nodes_t(S, h, 0x42498680)
    return None
# The DLC ships the Xbox 360 copies of the base game's PDA movies (map 0xa87f3101, pages 0x2dffe719): pad-only,
# with the Xbox button legend and no keyboard / mouse handling. Their PC versions differ only by the PC port's
# keyboard and mouse controls and prompts, so those are used. The DLC minimap (0x71a70b2a, DLC roads) stays.
PC_UI = (0xa87f3101, 0x2dffe719)
def parts_of(S, h, t):
    """{detail part: nodes} of asset h / type t in a source wad"""
    out = {}
    for bi, tt, sz in S.idx.get(h, []):
        if tt != t: continue
        for hh, t2, po, nodes in read_block(S.w, bi):
            if hh == h and t2 == t: out[ent_info(S.w, bi)['P']] = nodes
    return out
def conv_asset(h, t, parts, lg):
    if t == 0xfe0e8320 and h in PC_UI and h in P.idx:
        pc = get_nodes_t(P, h, t)
        if pc:
            lg.append('pc movie %08x' % h)
            return {p: pc for p in parts}
    # unchanged base asset: take the PC game's own copy (exact) instead of converting. Also for assets split
    # into detail parts (P000..P003): base characters and vehicles the DLC carries converted from the Xbox copy
    # were drawn invisible (skinned models), the PC copies of every part are used when all parts are unchanged.
    if t != 0xf011157a and h in X.idx and h in P.idx:
        xp = parts_of(X, h, t); pp = parts_of(P, h, t)
        if set(xp) == set(parts) == set(pp) and all(xp[q] == parts[q][2] for q in parts):
            lg.append('pc original %08x %08x (%d parts)' % (t, h, len(parts)))
            return pp
    if t in (0xf753f6d0, 0x9f8bca10, 0xe5273c14):
        return {p: [(n[0][0], audio(h, t, n[0][1], lg), [])] for p, (b, po, n) in parts.items()}
    if t == 0xf011157a:
        return conv_texture.convert({p: n for p, (b, po, n) in parts.items()}, lg)
    out = {}
    for p, (b, po, nodes) in parts.items():
        if t == 0x42498680:
            base = get_nodes_t(T, h, t) or get_nodes_t(X, h, t)
            cv = conv_script.convert(nodes, lg, pc_script(h), base)
        elif t == 0x39e5e978: cv = conv_strings.convert(nodes, lg)
        elif t in (0x5647c35d, 0xe6b81a54): cv = conv_layer.convert(nodes, lg)
        elif t in (0xbcfe6314, 0x6310807f): cv = conv_simple.path_like(nodes, lg)
        elif t == 0x207359c7: cv = conv_simple.table207(nodes, lg)
        elif t == 0x140e8728: cv = conv_simple.guidmap(nodes, lg)
        elif t in (0x5b724250, 0x600b904e, 0x7c569307, 0x1602815c): cv = conv_mesh.convert(nodes, t, lg)
        elif t == 0x18166555: cv = anim(nodes, lg)
        elif t == 0x5608bd5a: cv = effect(nodes, lg)
        elif t == 0x34612f86:
            import conv_tree
            cv = [(a, conv_tree.convert(b2, lg) if a == b'tree' else b2, c) for a, b2, c in nodes]
        elif t in (0xea4829d5, 0xfe0e8320): cv = nodes
        elif t in (0xfa0b8dbc, 0xde982d61): cv = [(a, conv_simple.sw(b2, 4) if b2 else b2, c) for a, b2, c in nodes]
        elif t == 0x4d7d30c4 and h in X.idx and h in P.idx:
            xb = (get_nodes_t(X, h, t) or [[0, None]])[0][1]; pc = (get_nodes_t(P, h, t) or [[0, None]])[0][1]
            r = conv_simple.delta_apply(pc, xb, nodes[0][1], lg, 'watr')
            cv = [(nodes[0][0], r, [])] if r else None
        else:
            lg.append('UNHANDLED type %08x asset %08x' % (t, h)); return None
        if cv is None: return None
        out[p] = cv
    return out
CACHE = cfg.w('dlccache'); os.makedirs(CACHE, exist_ok=True)
conv = {}
pending = 0
NOBJ = len(objs)
for k, ((h, t), parts) in enumerate(objs.items()):
    if k % 50 == 0: print('@%.3f %d/%d assets' % (0.95 * k / NOBJ, k, NOBJ)); sys.stdout.flush()
    cf = os.path.join(CACHE, '%08x_%08x.pkl' % (t, h))
    if os.path.exists(cf):
        r, lg = pickle.load(open(cf, 'rb'))
    else:
        if time.time() - T0 > LIMIT: pending += 1; continue
        if t in SKIP_T: r, lg = None, ['skip audio %08x' % t]
        else:
            lg = []
            try: r = conv_asset(h, t, parts, lg)
            except Exception as e:
                r = None; lg.append('EXC %08x %08x: %r' % (t, h, e))
        pickle.dump((r, lg), open(cf, 'wb'))
    if r is None: stats['fail %08x' % t] += 1
    else: stats['ok %08x' % t] += 1; conv[(h, t)] = r
    log.extend(lg)
print('converted', len(conv), 'pending', pending, int(time.time() - T0), 's'); sys.stdout.flush()
if pending or os.environ.get('M2CONVERTONLY'):
    for k, v in sorted(stats.items()): print(k, v)
    sys.exit(0)
# 2. write blocks in original layout
blocks = []; flags = []; newidx = {}; psize = {}
byblock = collections.defaultdict(list)
for (i, h, t, Pl) in order:
    if (h, t) in conv and Pl in conv[(h, t)]: byblock[i].append((h, t, Pl))
Pl_of = {}
for i in range(len(W.ents)):
    e = ent_info(W, i)
    items = byblock.get(i, [])
    if not items: continue
    objl = []
    for h, t, Pl in items:
        parts = conv[(h, t)]
        # part offset = sum of sizes of lower P parts of this asset
        if (h, t) not in psize: psize[(h, t)] = {q: len(ucfx(parts[q])) for q in parts}
        po = sum(v for q, v in psize[(h, t)].items() if q < Pl)
        objl.append((h, t, po, parts[Pl])); Pl_of[(h, t, po)] = Pl
    raw, sizes = block(objl)
    tex = sum(sz for (h, t, po, n), sz in zip(objl, sizes) if t == 0xf011157a and Pl_of.get((h, t, po)) != 0)
    lo = max(1, -(-(len(raw) - tex) // 32768)); hi = -(-tex // 4096)
    newidx[i] = len(blocks); blocks.append((W.names[i], e['P'], raw)); flags.append(0x80000000 | (hi << 16) | lo)
trows = []
for h, bl, ti in rows:
    t = types[ti]
    if (h, t) not in conv: continue
    trows.append((h, tuple(newidx.get(b, 0xffff) if b != 0xffff else 0xffff for b in bl), ti))
print('@0.97 writing DLC01.wad'); sys.stdout.flush()
write_wad(os.path.join(OUT, 'DLC01.wad'), blocks, trows, types, flags)
print('@1.0 done')
print('wrote', len(blocks), 'blocks', len(trows), 'assets', int(time.time() - T0), 's')
for k, v in sorted(stats.items()): print(k, v)
c = collections.Counter(l[:70] for l in log)
for k, v in c.most_common(40): print(v, k)

# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
import sys, os, struct, pickle
from getn import *
from wadr import read_block, info, ent_info
from wadw import block, write_wad
import conv_layer, conv_strings, conv_script
import cfg
OUT = sys.argv[1] if len(sys.argv) > 1 else cfg.OUT
os.makedirs(OUT, exist_ok=True)
P = Src('pcvz'); X = Src('xvz'); PS = Src('pcshell'); XS = Src('xshell')
enum = [d for t, d, k in get_nodes(P, [h for h, l in P.idx.items() if l[0][1] == 0xe6b81a54][0]) if t == b'enum'][0]
conv_layer.init(enum)
SKIP = {0x140e8728, 0xfe0e8320} | ({int(x,16) for x in os.environ.get('M2SKIP','').split(',') if x})
SKIPH = {int(x,16) for x in os.environ.get('M2SKIPH','').split(',') if x}
def lofix(name, v):
    for x in os.environ.get('M2LOFIX','').split(','):
        if x and x.split(':')[0] in name: return int(x.split(':')[1])
    return v
def shell_gfx_dlc(S, h, t, log):
    """PC shell movie with the Extras > Downloadable Content button left visible (PC build hid it)."""
    import zlib
    nn = get_nodes_t(S, h, t)
    def walk(l):
        out = []
        for tg, d, k in l:
            if d and d[:3] == b'CFX':
                raw = bytearray(zlib.decompress(d[8:])); len0 = len(raw)
                o = 0xcf31d
                assert raw[o:o + 12] == bytes.fromhex('960400081305004f96020008'), raw[o:o + 12].hex()
                raw[o + 6] = 1                       # Push '_visible', False -> True
                from extrapatch import patch_extras_up
                raw = bytearray(patch_extras_up(bytes(raw)))   # gamepad: Up from the 4th entry reaches Downloadable Content
                log.append('shell gfx: extras Up -> DLC entry')
                import dlcpic
                raw = bytearray(dlcpic.patch(bytes(raw)))     # picture in the empty DLC details frame
                log.append('shell gfx: DLC pictures %s, %s' % (dlcpic.NAME, dlcpic.THUMB))
                hl = struct.unpack_from('<I', d, 4)[0] + (len(raw) - len0)    # header length field grows with the code
                d = d[:4] + struct.pack('<I', hl) + zlib.compress(bytes(raw), 9)
                log.append('shell gfx %08x: DLC button enabled' % h)
            out.append((tg, d, walk(k) if k else k))
        return out
    return walk(nn)
DLCWAD = os.environ.get('M2_DLCWAD') or os.path.join(cfg.OUT, 'DLC01.wad')
COSTUME_MODELS = ['pmc_hum_obama', 'pmc_hum_sarah', 'pmc_hum_mattias_v5']
def dlc_costume_assets(log):
    """DLC-only costumes (Obama, Palin, GrandpaMattias) for the campaign: the three models, every texture they
    use that the PC game lacks, and the DLC string tables ([SHELL.Costumes.*]). Parts are taken from the converted
    DLC01.wad with their streaming layout (one block per asset and P level, as in the DLC)."""
    import numpy as np
    from wad import Wad
    from preimage import ph
    if not os.path.exists(DLCWAD): log.append('no DLC01.wad, costumes skipped'); return [], {}, set()
    W = Wad(DLCWAD); types, rows = info(W)
    ass = {}
    for i in range(len(W.ents)):
        for h, t, po, nodes in read_block(W, i): ass.setdefault(h, []).append((t, i, po, nodes))
    def flat(n):
        for t, d, k in n:
            if d: yield d
            if k: yield from flat(k)
    need = set(); q = [ph(m) for m in COSTUME_MODELS]
    while q:
        h = q.pop()
        if h in need or h not in ass: continue
        if h in P.idx and any(t == 0xf011157a for t, i, po, n in ass[h]): continue   # the PC game has it
        need.add(h)
        for t, i, po, nodes in ass[h]:
            if t != 0x5b724250: continue
            for d in flat(nodes):
                for a in range(4):
                    for v in set(np.frombuffer(d[a:a + (len(d) - a) // 4 * 4], '<u4').tolist()):
                        if v in ass and v not in need: q.append(v)
    for lang in ('english', 'french', 'german', 'italian', 'spanish', 'russian'):
        need.add(ph(lang + '_dlc01'))
        # costume chatter: resident VO events + stream bank header (audio data streams from vo_stream_dlctest.<lang>.pws)
        need.add(ph('vo_resident_DLCTest.' + lang)); need.add(ph('vo_stream_DLCTest.' + lang))
    need.add(ph('Obama_Obama01')); need.add(ph('Palin_Palin01'))
    newblocks = {}   # source block index -> list of objs
    out_rows = []
    for h, bl, ti in rows:
        if h not in need: continue
        t = types[ti]
        for b in bl:
            if b == 0xffff: continue
            for tt, i, po, nodes in ass[h]:
                if i == b and tt == t: newblocks.setdefault(b, []).append((h, t, po, nodes))
        out_rows.append((h, bl, t))
    log.append('dlc costumes: %d assets in %d blocks' % (len(out_rows), len(newblocks)))
    return out_rows, newblocks, W
def graft_dlc_main(objs, log):
    """campaign wifpmcinterior / mrxplayer: take the DLC build's main chunk (outfit tables with Obama, Palin and
    GrandpaMattias); functions stay the campaign (TU4) versions."""
    from wad import Wad
    from preimage import ph
    from luabc import load, dump
    if not os.path.exists(DLCWAD): return objs
    W = Wad(DLCWAD); src = {}
    want = {ph(n): n for n in ('wifpmcinterior', 'mrxplayer')}
    for i in range(len(W.ents)):
        for h, t, po, nodes in read_block(W, i):
            if t == 0x42498680 and h in want: src[h] = [d for tg, d, k in nodes if tg == b'BINN'][0]
    out = []
    for h, t, po, nodes in objs:
        if t == 0x42498680 and h in src:
            a = load([d for tg, d, k in nodes if tg == b'BINN'][0]); b = load(src[h])
            if len(a.p) == len(b.p):
                a.code, a.k, a.lineinfo, a.locvars, a.maxstack = b.code, b.k, b.lineinfo, b.locvars, max(a.maxstack, b.maxstack)
                if want[h] == 'mrxplayer':
                    # player creation: Human.SetChatterSet(Obama_Obama01 / Palin_Palin01) for the DLC costumes
                    idx = [i for i, q in enumerate(b.p) if any(k[0] == 4 and k[1] == b'Obama_Obama01' for k in q.k)]
                    for i in idx: a.p[i] = b.p[i]
                    log.append('script mrxplayer: costume chatter function grafted (%s)' % idx)
                if want[h] == 'wifpmcinterior':
                    # wardrobe menu: the DLC _SelectOutfit lists bAlwaysVisible outfits (Obama, Palin, GrandpaMattias)
                    # regardless of how many campaign outfits are unlocked
                    def refs(q, v): return any(k[0] == 4 and k[1] == v for k in q.k)
                    idx = [i for i, q in enumerate(b.p) if refs(q, b'bAlwaysVisible') and refs(q, b'_ChangeOutfit')]
                    ok = [i for i in idx if refs(a.p[i], b'_ChangeOutfit') and refs(a.p[i], b'GetProfileCostume') and a.p[i].nups == b.p[i].nups]
                    for i in ok: a.p[i] = b.p[i]
                    log.append('script wifpmcinterior: outfit menu grafted %s (candidates %s)' % (ok, idx))
                    # outfit change: the DLC version also switches the voice set (Human.SetChatterSet)
                    idx2 = [i for i, q in enumerate(b.p) if refs(q, b'SetChatterSet') and refs(q, b'SetProfileCostume')]
                    ok2 = [i for i in idx2 if refs(a.p[i], b'SetProfileCostume') and refs(a.p[i], b'SetOutfit') and a.p[i].nups == b.p[i].nups]
                    for i in ok2: a.p[i] = b.p[i]
                    log.append('script wifpmcinterior: outfit change grafted %s (candidates %s)' % (ok2, idx2))
                nodes = [(tg, dump(a, False) if tg == b'BINN' else d, k) for tg, d, k in nodes]
                if want[h] == 'wifpmcinterior':
                    nodes = [(tg, conv_script.briefing_safe(d, log) if tg == b'BINN' else d, k) for tg, d, k in nodes]
                log.append('script %s: DLC outfit tables grafted' % want[h])
            else: log.append('script %s: proto count differs, graft skipped' % want[h])
        out.append((h, t, po, nodes))
    return out
def append_calls(p, calls):
    """insert NS.fn('arg') calls before the final RETURN of proto p (registers r0/r1)"""
    from luabc import mk_ABx, mk_ABC
    def K(v):
        for i, k in enumerate(p.k):
            if k[0] == 4 and k[1] == v: return i
        p.k.append((4, v)); return len(p.k) - 1
    ins = []
    for ns, fn, arg in calls:
        ins += [mk_ABx(5, 0, K(ns)), mk_ABx(1, 1, K(fn)), mk_ABC(6, 0, 0, 1), mk_ABx(1, 1, K(arg)), mk_ABC(28, 0, 2, 1)]
    at = len(p.code) - 1
    p.code[at:at] = ins
    if p.lineinfo: p.lineinfo[at:at] = [p.lineinfo[at]] * len(ins)
    p.maxstack = max(p.maxstack, 2)
def soundbootstrap_dlc_vo(pcS, log):
    """PC mrxsoundbootstrap: LoadBanks/UnloadBanks also (un)load the DLC VO banks (costume chatter)"""
    from luabc import load, dump, op, A, Bx
    from preimage import ph
    h = ph('mrxsoundbootstrap'); nn = get_nodes_t(pcS, h, 0x42498680)
    pr = load([d for tg, d, k in nn if tg == b'BINN'][0])
    def proto_of(name):
        for i in range(len(pr.code) - 1):
            a, b = pr.code[i], pr.code[i + 1]
            if op(a) == 36 and op(b) == 7 and A(a) == A(b) and pr.k[Bx(b)][1] == name: return pr.p[Bx(a)]
    lb, ub = proto_of(b'LoadBanks'), proto_of(b'UnloadBanks')
    if not lb or not ub: log.append('mrxsoundbootstrap: LoadBanks/UnloadBanks not found'); return None
    append_calls(lb, [(b'MrxSoundBanks', b'LoadWaveBank', b'vo_stream_DLCTest'), (b'MrxSoundBanks', b'LoadSoundBank', b'vo_resident_DLCTest')])
    append_calls(ub, [(b'MrxSoundBanks', b'UnloadWaveBank', b'vo_stream_DLCTest'), (b'MrxSoundBanks', b'UnloadSoundBank', b'vo_resident_DLCTest')])
    log.append('mrxsoundbootstrap: DLC VO banks added to LoadBanks/UnloadBanks')
    return h, [(tg, dump(pr, False) if tg == b'BINN' else d, k) for tg, d, k in nn]
def build(src, pcS, xbS, outname):
    T = Src(src); W = T.w
    types, rows = info(W)
    log = []; blocks = []; flags = []; keep_rows = {}
    newidx = {}
    MOVE = {int(a,16): b for a, b in (x.split(':') for x in os.environ.get('M2MOVE','').split(',') if x)}
    moved = {}; per = []; extra_rows = []; texhi = {}
    for i in range(len(W.ents)):
        e = ent_info(W, i); objs = []
        for h, t, po, nodes in read_block(W, i):
            if src == 'tu4shell' and t == 0xfe0e8320 and h == 0x51bff7b1:
                objs.append((h, t, po, shell_gfx_dlc(pcS, h, t, log))); keep_rows[(h, t)] = True
                import dlcpic
                from preimage import ph as _ph
                th = _ph(dlcpic.NAME); objs.append((th, 0xf011157a, 0, dlcpic.texture_nodes())); keep_rows[(th, 0xf011157a)] = True
                extra_rows.append((th, i, 0xf011157a)); texhi[i] = texhi.get(i, 0) + 32
                th2 = _ph(dlcpic.THUMB); objs.append((th2, 0xf011157a, 0, dlcpic.thumb_nodes())); keep_rows[(th2, 0xf011157a)] = True
                extra_rows.append((th2, i, 0xf011157a)); texhi[i] += 2
                log.append('shell: texture %s added (%08x)' % (dlcpic.NAME, th)); continue
            if t in SKIP or h in SKIPH: log.append('skip %08x type %08x' % (h, t)); continue
            if t == 0x42498680:
                pcn = get_nodes_t(pcS, h, t)
                xbn = get_nodes_t(xbS, h, t)
                if pcn is None or xbn is None: log.append('NO MERGE BASE %08x (pc %s, xbox %s)' % (h, pcn is not None, xbn is not None))
                cv = conv_script.convert(nodes, log, pcn, xbn)
            elif t == 0x39e5e978: cv = conv_strings.convert(nodes, log)
            elif t in (0x5647c35d, 0xe6b81a54):
                cv = conv_layer.convert(nodes, log)
                pcn = get_nodes_t(pcS, h, t); xbn = get_nodes_t(xbS, h, t)
                if pcn is not None and xbn is not None:
                    cx = conv_layer.convert(xbn, [])
                    st = [0, 0]
                    def lmerge(a, b, c):
                        if len(a) != len(b) or len(b) != len(c) or [x[0] for x in a] != [x[0] for x in c]: return None
                        out = []
                        for (ta, da, ka), (tb, db, kb), (tc, dc, kc) in zip(a, b, c):
                            if ka or kb or kc:
                                m = lmerge(ka, kb, kc)
                                if m is None: return None
                                out.append((ta, da, m))
                            elif db == dc: out.append((ta, da, [])); st[0] += 1
                            else: out.append((tc, dc, [])); st[1] += 1
                        return out
                    m = lmerge(pcn, cx, cv)
                    if os.environ.get('M2PCLAYER'): m = pcn; log.append('layer: PC original verbatim (test)')
                    if m is not None: cv = m; log.append('layer %08x delta-merged on PC: %d kept, %d changed' % (h, st[0], st[1]))
                    else: log.append('layer %08x: shape mismatch, full conversion used' % h)
            elif t == 0xea4829d5: cv = nodes
            else: log.append('UNHANDLED type %08x' % t); continue
            if h in MOVE: moved.setdefault(MOVE[h], []).append((i, h, t, po, cv)); keep_rows[(h, t)] = True; continue
            objs.append((h, t, po, cv)); keep_rows[(h, t)] = True
        per.append((i, e, objs))
    remap = {}
    # extra PC-script fixes shipped in the patch (guards for DLC mode with campaign saves)
    if src == 'tu4vz':
        from luabc import load, dump
        from luamerge import guard_global_index
        for sh, fn, gn in ((0x0, b'CreateStarter', b'WifStarterData'),):
            pass
        for hh, l in pcS.idx.items():
            for bi, tt, sz in l:
                if tt != 0x42498680: continue
                nn = get_nodes_t(pcS, hh, tt); bb = [d for tg, d, k in nn if tg == b'BINN'][0]
                pr = load(bb)
                if pr.source == b'mrxstartermanager' and (hh, tt) not in keep_rows:
                    ok = guard_global_index(pr, b'CreateStarter', b'WifStarterData')
                    nn2 = [(tg, dump(pr, False) if tg == b'BINN' else d, k) for tg, d, k in nn]
                    for i0, e0, objs0 in per:
                        if 'resident' in W.names[i0]: objs0.append((hh, tt, 0, nn2)); keep_rows[(hh, tt)] = True; extra_rows.append((hh, i0, tt)); break
                    log.append('guarded mrxstartermanager.CreateStarter: %s' % ok)
        if not os.environ.get('M2NOCOSTUMES'):
            r = soundbootstrap_dlc_vo(pcS, log)
            if r and (r[0], 0x42498680) not in keep_rows:
                for i0, e0, objs0 in per:
                    if 'resident' in W.names[i0]:
                        objs0.append((r[0], 0x42498680, 0, r[1])); keep_rows[(r[0], 0x42498680)] = True
                        extra_rows.append((r[0], i0, 0x42498680)); break
    for tgt, lst in moved.items():
        for k, (i, e, objs) in enumerate(per):
            if tgt in W.names[i]:
                for (si, h, t, po, cv) in lst: objs.append((h, t, po, cv)); remap[(h, si)] = i
                log.append('moved %d asset(s) into %s' % (len(lst), W.names[i])); break
    if src == 'tu4vz':
        per = [(i, e, graft_dlc_main(objs, log)) for i, e, objs in per]
    costume = None
    if src == 'tu4vz' and not os.environ.get('M2NOCOSTUMES'):
        # DLC string tables (<lang>_dlc01) go into the matching <lang>_patch01 block, which the game reliably
        # loads; in their own streaming block the costume names stayed unresolved ([SHELL.Costumes.obama])
        from preimage import ph as _ph
        crow, cblocks, DW = dlc_costume_assets(log)
        langs = {_ph(l + '_dlc01'): l for l in ('english', 'french', 'german', 'italian', 'spanish', 'russian')}
        moved_s = set()
        for b in list(cblocks):
            keep = []
            for o in cblocks[b]:
                if o[1] == 0x39e5e978 and o[0] in langs:
                    tgt = [(i, objs) for i, e, objs in per if (langs[o[0]] + '_patch01') in W.names[i].lower()]
                    if tgt:
                        tgt[0][1].append(o); extra_rows.append((o[0], tgt[0][0], o[1])); moved_s.add(o[0])
                        log.append('string table %s_dlc01 -> %s' % (langs[o[0]], W.names[tgt[0][0]])); continue
                keep.append(o)
            if keep: cblocks[b] = keep
            else: del cblocks[b]
        crow = [r for r in crow if not (r[0] in moved_s and r[2] == 0x39e5e978)]
        costume = (crow, cblocks, DW)
    for i, e, objs in per:
        if not objs: log.append('drop empty block %s' % W.names[i]); continue
        raw, sizes = block(objs)
        newidx[i] = len(blocks)
        blocks.append((W.names[i], e['P'], raw)); flags.append(0x80000000 | (((e['flags'] >> 16) & 0x7fff) + texhi.get(i, 0)) << 16 if i in texhi else 0x80000000)
    trows = []
    for h, bl, ti in rows:
        t = types[ti]
        if (h, t) not in keep_rows: continue
        trows.append((h, tuple(newidx.get(remap.get((h, b), b), 0xffff) if b != 0xffff else 0xffff for b in bl), ti))
    for hh, bi0, tt in extra_rows:
        trows.append((hh, (0xffff, 0xffff, 0xffff, newidx[bi0]), types.index(tt)))
    if src == 'tu4vz' and not os.environ.get('M2NOCOSTUMES'):
        crow, cblocks, DW = costume
        if crow:
            from wadr import ent_info as _ei
            bmap = {}
            for b, objl in sorted(cblocks.items()):
                raw, sizes = block(objl)
                Pl = _ei(DW, b)['P']
                tex = sum(sz for (h, t, po, n), sz in zip(objl, sizes) if t == 0xf011157a and Pl != 0)
                hi = -(-tex // 4096)
                nm = DW.names[b].replace('blocks\\dlc01\\', 'blocks\\vz\\dlc01_')
                bmap[b] = len(blocks); blocks.append((nm, Pl, raw)); flags.append(0x80000000 | (hi << 16))
            have = {(h, types[ti]) for h, bl, ti in trows}
            for h, bl, t in crow:
                if (h, t) in have: continue
                if t not in types: log.append('type %08x missing in patch types' % t); continue
                trows.append((h, tuple(bmap.get(b, 0xffff) if b != 0xffff else 0xffff for b in bl), types.index(t)))
    write_wad(os.path.join(OUT, outname), blocks, trows, types, flags)
    return log
ONLY = os.environ.get('M2ONLY', '')
for src, pcS, xbS, nm in (('tu4vz', P, X, 'vz-patch.wad'), ('tu4shell', PS, XS, 'shell-patch.wad')):
    if ONLY and ONLY not in src: continue
    print('@%.2f %s' % (0.0 if src == 'tu4vz' else 0.8, nm)); sys.stdout.flush()
    log = build(src, pcS, xbS, nm)
    print('==', nm); print('\n'.join(log))

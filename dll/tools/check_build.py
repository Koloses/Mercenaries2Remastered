#!/usr/bin/env python3
# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Checks every Expect(address, "pattern") of the DLL sources against both game executables.

Usage: check_build.py <Mercenaries2.exe 1.0> <Mercenaries2.exe 1.1>      (run from dll/)
The sources are retargeted with v11.map (tools/retarget.py), preprocessed with i686-w64-mingw32-gcc -E, and each
Expect whose address is a constant expression is compared with the bytes of the matching exe. When a 1.1 pattern
differs only by moved addresses (absolute ones in v11.map or .data, or call/jump targets), the P line that fixes
it is printed for v11.map. A new game address in the sources must first get its own line in v11.map."""
import os, re, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import retarget

B = 0x400000
def image(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]
    oh = struct.unpack_from('<H', d, pe + 20)[0]; size = struct.unpack_from('<I', d, pe + 24 + 56)[0]
    m = bytearray(size)
    m[:0x1000] = d[:0x1000]
    for i in range(ns):
        s = pe + 24 + oh + 40 * i
        vs, va, rs, ro = struct.unpack_from('<IIII', d, s + 8)
        n = min(rs, vs or rs); m[va:va + n] = d[ro:ro + n]
    return bytes(m)

def expects(srcdir, f, *defs):
    pp = subprocess.run(['i686-w64-mingw32-gcc', '-E', '-P', '-DM2R_SHOT', *defs, os.path.join(srcdir, f)],
                        capture_output=True, text=True).stdout
    return re.findall(r'\bExpect\s*\(\s*([^;]*?)\s*,\s*"([^"]*)"\s*\)', pp)

def ev(e):
    e = re.sub(r'\b(0x[0-9A-Fa-f]+)[uUlL]+', r'\1', e)
    e = re.sub(r'\((DWORD|UINT_PTR|unsigned|int)\)', '', e)
    try: return int(eval(e, {'__builtins__': {}}))
    except Exception: return None

def match(M, a, pat):
    bs = pat.split(); o = a - B
    return 0 <= o and o + len(bs) <= len(M) and all(b.startswith('?') or M[o + i] == int(b, 16) for i, b in enumerate(bs))

def mapped(m, v):
    if v in m: return m[v]
    if 0xBF6000 <= v < 0x19FA000: return v - 0x1000
    return None

def derive(M0, M1, m, a, b, pat):
    bs = pat.split(); n = len(bs)
    old, new = M0[a - B:a - B + n], M1[b - B:b - B + n]
    ok = [bs[i].startswith('?') or old[i] == new[i] for i in range(n)]
    for i in range(n - 3):
        if all(ok[i:i + 4]): continue
        ov, nv = struct.unpack_from('<I', old, i)[0], struct.unpack_from('<I', new, i)[0]
        good = mapped(m, ov) == nv
        if not good and i >= 1 and (old[i - 1] in (0xE8, 0xE9) or (i >= 2 and old[i - 2] == 0x0F and 0x80 <= old[i - 1] <= 0x8F)):
            ot = (a + i + 4 + struct.unpack_from('<i', old, i)[0]) & 0xFFFFFFFF
            nt = (b + i + 4 + struct.unpack_from('<i', new, i)[0]) & 0xFFFFFFFF
            good = mapped(m, ot) == nt
        if good: ok[i:i + 4] = [True] * 4
    return ' '.join(x if x.startswith('?') else '%02X' % new[i] for i, x in enumerate(bs)) if all(ok) else None

def main():
    M0, M1 = image(sys.argv[1]), image(sys.argv[2])
    m, pats = retarget.load('v11.map')
    tmp = tempfile.mkdtemp()
    for f in os.listdir('src'):
        p = os.path.join('src', f)
        if os.path.isfile(p):
            t = open(p, encoding='latin-1').read()
            if f.endswith(('.c', '.h')) and 'm2r:no-retarget' not in t[:400]: t, _ = retarget.retarget(t, m, pats, f)
            open(os.path.join(tmp, f), 'w', encoding='latin-1', newline='').write(t)
    bad = tot = 0
    for f in sorted(x for x in os.listdir('src') if x.endswith('.c')):
        A, C = expects('src', f), expects(tmp, f, '-DM2R_GAME11')
        sm = __import__('difflib').SequenceMatcher(None, [p for _, p in A], [p for _, p in C], autojunk=False)
        pairs = []
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag in ('equal', 'replace') and i2 - i1 == j2 - j1: pairs += list(zip(A[i1:i2], C[j1:j2]))
            else: pairs += [(a, None) for a in A[i1:i2]] + [(None, c) for c in C[j1:j2]]
        for pa_, pc_ in pairs:
            if pa_ is None:          # only in the 1.1 build
                b = ev(pc_[0]); tot += 1
                if b is None or not match(M1, b, pc_[1]): bad += 1; print('%s: 1.1-only Expect(%s, "%s") MISMATCH' % (f, pc_[0], pc_[1]))
                continue
            if pc_ is None: continue
            (ea, pa), (eb, pb) = pa_, pc_
            a, b = ev(ea), ev(eb)
            if a is None or b is None: continue
            tot += 1
            ok0, ok1 = match(M0, a, pa), b and match(M1, b, pb)
            if ok0 and ok1: continue
            bad += 1
            if ok0 and b:
                fix = derive(M0, M1, m, a, b, pa)
                if fix: print('%s: Expect(%s): add to v11.map:  P "%s" "%s"' % (f, ea, pa, fix)); continue
            print('%s: Expect(%s, "%s"): 1.0 %s, 1.1 (%s) %s' % (f, ea, pa, 'ok' if ok0 else 'MISMATCH',
                  '%X' % b if b else 'no address', 'ok' if ok1 else 'MISMATCH'))
    print('%d Expect checks, %d to look at' % (tot, bad))

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Writes a copy of the DLL sources for another game build.

The sources address the 1.0 executable. A map file lists, for every game address used in the sources
(outside comments), the address in the other build:
    OLD NEW [note]                  hex, NEW = ???????? when the build has no equivalent (build fails)
    P "old pattern" "new pattern"   replacement of an Expect() byte pattern (when the code bytes differ)
Usage: retarget.py MAPFILE SRCDIR OUTDIR"""
import os, re, sys

def load(path):
    m, pats = {}, {}
    for ln in open(path, encoding='utf-8'):
        ln = ln.split('#', 1)[0].strip()
        if not ln: continue
        if ln.startswith('P '):
            a, b = re.findall(r'"([^"]*)"', ln)
            pats[a] = b
            continue
        f = ln.split()
        m[int(f[0], 16)] = None if f[1].startswith('?') else int(f[1], 16)
    return m, pats

TOK = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|0[xX][0-9A-Fa-f]+|\w+|\s+|.', re.S)
LO, HI = 0x401000, 0x3800000

def retarget(text, m, pats, name):
    out, missing = [], []
    def lit(h):
        v = int(h, 16)
        if not (LO <= v < HI): return h
        if v not in m: missing.append('%s: %s not in the map' % (name, h)); return h
        if m[v] is None: missing.append('%s: %s has no equivalent' % (name, h)); return h
        return '0x%X' % m[v]
    for t in TOK.finditer(text):
        s = t.group(0)
        if s.startswith('//') or s.startswith('/*'): out.append(s); continue
        if s.startswith('"'):
            body = s[1:-1]
            if body in pats: s = '"%s"' % pats[body]
            s = re.sub(r'0[xX][0-9A-Fa-f]+', lambda h: lit(h.group(0)), s)
        elif re.match(r'0[xX][0-9A-Fa-f]+$', s): s = lit(s)
        out.append(s)
    return ''.join(out), missing

def main():
    mp, src, dst = sys.argv[1:4]
    m, pats = load(mp)
    os.makedirs(dst, exist_ok=True)
    bad = []
    for f in sorted(os.listdir(src)):
        p = os.path.join(src, f)
        if not os.path.isfile(p): continue
        data = open(p, encoding='latin-1').read()
        if f.endswith(('.c', '.h')): data, miss = retarget(data, m, pats, f); bad += miss
        open(os.path.join(dst, f), 'w', encoding='latin-1', newline='').write(data)
    if bad:
        print('\n'.join(bad), file=sys.stderr); sys.exit(1)

if __name__ == '__main__':
    main()

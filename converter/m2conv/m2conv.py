# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Mercenaries2Remastered content builder: converts the Xbox 360 Title Update 4 and the
"Blow It Up Again Pack" DLC into PC content for Mercenaries2Remastered.

  m2conv.py --game <PC game folder> --iso <Xbox 360 game image>
            --tu <TU4 package or .zip> --dlc <DLC package or .zip>
            [--work <dir>] [--out <dir>] [--limit seconds] [--force]

Output (default <game>\\Mercenaries2Remastered\\content): vz-patch.wad, shell-patch.wad, DLC01.wad,
Data\\Audios\\*.pws. Every stage is resumable; progress lines start with '@'."""
import sys, os, json, time, argparse, hashlib, subprocess, zipfile, shutil, struct, traceback
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

EXPECT = {
    'tu4/vz-patch.wad': '794361914d3858593b558c4d593f0b17',
    'tu4/shell-patch.wad': 'e177fca383ebdc9e808a2547ecd8f1f2',
    'dlc/DLC01.doh': 'f2efef57daf2209934af052c298f011a',
}
ISO_SIZES = {'vz.wad': 2000486400, 'shell.wad': 11894784}
STAGES = ['inputs', 'index', 'pcdb', 'tu4pass1', 'dlc', 'art', 'tu4', 'finish']
TITLES = {'inputs': 'Reading the Xbox 360 files', 'index': 'Indexing game archives',
          'pcdb': 'Reading PC layer tables', 'tu4pass1': 'Converting Title Update 4 scripts',
          'dlc': 'Converting the DLC (longest step)', 'art': 'Making the DLC menu pictures',
          'tu4': 'Building TU4 + costume patches', 'finish': 'Installing content'}

def say(*a):
    print(*a); sys.stdout.flush()
def progress(stage, frac, text=''):
    k = STAGES.index(stage)
    say('@%d/%d %.3f %s%s' % (k + 1, len(STAGES), frac, TITLES[stage], (': ' + text) if text else ''))
def md5(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()

class Fail(Exception): pass

def stfs_source(path, tmpdir, want):
    """path: STFS package or a .zip holding one -> path of the STFS file"""
    import stfs
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for n in z.namelist():
                if n.endswith('/'): continue
                with z.open(n) as f: head = f.read(4)
                if head in (b'LIVE', b'CON ', b'PIRS'):
                    os.makedirs(tmpdir, exist_ok=True)
                    dst = os.path.join(tmpdir, 'pkg_' + want)
                    with z.open(n) as f, open(dst, 'wb') as o: shutil.copyfileobj(f, o, 1 << 20)
                    return dst
        raise Fail('%s: no Xbox 360 package inside the zip' % path)
    with open(path, 'rb') as f:
        if f.read(4) not in (b'LIVE', b'CON ', b'PIRS'): raise Fail('%s is not an Xbox 360 package (or a zip with one)' % path)
    return path

def st_inputs(a, cfg):
    import stfs, xiso
    progress('inputs', 0.0, 'game image')
    try: x = xiso.XISO(a.iso)
    except Exception: raise Fail('%s is not an Xbox 360 game image (XDVDFS .iso)' % a.iso)
    files = {p.lower(): z for p, s, z in x.files()}
    for n, sz in ISO_SIZES.items():
        if n not in files: raise Fail('the game image has no %s - wrong disc?' % n)
        if files[n] != sz and not a.force: raise Fail('the game image\'s %s has an unexpected size (%d) - other release? (use --force to try anyway)' % (n, files[n]))
    tmp = cfg.w('tmp')
    progress('inputs', 0.2, 'Title Update 4')
    p = stfs.STFS(stfs_source(a.tu, tmp, 'tu'))
    if p.title != 0x45410828: raise Fail('the title update is not for Mercenaries 2')
    os.makedirs(cfg.w('tu4'), exist_ok=True)
    got = set()
    for e in p.entries:
        if e['path'] in ('vz-patch.wad', 'shell-patch.wad'):
            open(cfg.w('tu4', e['path']), 'wb').write(p.read(e)); got.add(e['path'])
    if len(got) != 2: raise Fail('the title update has no vz-patch.wad / shell-patch.wad - not TU4?')
    progress('inputs', 0.4, 'DLC package')
    d = stfs.STFS(stfs_source(a.dlc, tmp, 'dlc'))
    if d.title != 0x45410828: raise Fail('the DLC package is not for Mercenaries 2')
    os.makedirs(cfg.w('dlc', 'audios'), exist_ok=True)
    names = [e['path'] for e in d.entries if not e['isdir']]
    if not any(n.lower() == 'dlc01.doh' for n in names): raise Fail('the DLC package has no DLC01.doh - not the "Blow It Up Again Pack"?')
    for k, e in enumerate(d.entries):
        if e['isdir']: continue
        n = e['path']; low = n.lower()
        if low == 'dlc01.doh': dst = cfg.w('dlc', 'DLC01.doh')
        elif low.endswith('.pws'): dst = cfg.w('dlc', 'audios', os.path.basename(n))
        else: continue
        with open(dst, 'wb') as o: o.write(d.read(e))
        progress('inputs', 0.4 + 0.5 * k / len(d.entries), os.path.basename(n))
    # menu thumbnail from the package header
    with open(d.f.name, 'rb') as f: h = f.read(0x9800)
    ts = struct.unpack_from('>I', h, 0x1712)[0]
    os.makedirs(cfg.w('art'), exist_ok=True)
    open(cfg.w('art', 'dlc_thumb.png'), 'wb').write(h[0x171A:0x171A + ts])
    d.f.close(); p.f.close()
    shutil.rmtree(tmp, ignore_errors=True)
    for rel, m in EXPECT.items():
        got = md5(cfg.w(*rel.split('/')))
        if got != m:
            msg = '%s differs from the expected release (md5 %s)' % (os.path.basename(rel), got)
            if not a.force: raise Fail(msg + ' (use --force to try anyway)')
            say('WARNING: ' + msg)
    progress('inputs', 1.0)

def st_index(a, cfg):
    import qindex
    srcs = ['pcvz', 'pcshell', 'xvz', 'xshell', 'tu4vz', 'tu4shell', 'dlc']
    for k, s in enumerate(srcs):
        if os.path.exists(cfg.w('q_%s.pkl' % s)): continue
        progress('index', k / len(srcs), s)
        qindex.build(s)
    progress('index', 1.0)

def st_pcdb(a, cfg):
    import pcdb
    if not os.path.exists(cfg.w('hkclasses.pkl')):
        import hkreflect
        progress('pcdb', 0.0, 'Havok class tables from Mercenaries2.exe')
        hkreflect.build(os.path.join(a.game, 'Mercenaries2.exe'), cfg.w('hkclasses.pkl'))
    while True:
        done, n, tot = pcdb.build(limit=a.limit if a.limit else 1e9)
        progress('pcdb', n / max(1, tot), '%d/%d' % (n, tot))
        if done: return
        if a.limit: raise Pending()

class Pending(Exception): pass

def run_script(name, args, env_extra, stage, a):
    env = dict(os.environ); env.update(env_extra)
    env['PYTHONIOENCODING'] = 'utf-8'; env['PYTHONHASHSEED'] = '0'
    if a.limit: env['M2LIMIT'] = str(a.limit)
    cmd = [sys.executable, '-u', os.path.join(HERE, name)] + args
    logp = a.cfg.w('%s.log' % stage)
    flags = 0x08000000 if os.name == 'nt' else 0
    with open(logp, 'w', encoding='utf-8') as lf:
        pr = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=HERE,
                              text=True, encoding='utf-8', errors='replace', creationflags=flags)
        for line in pr.stdout:
            lf.write(line)
            if line.startswith('@'):
                try:
                    fr, txt = line[1:].strip().split(' ', 1)
                    progress(stage, float(fr), txt)
                except ValueError: pass
        rc = pr.wait()
    if rc: raise Fail('%s failed (exit %d) - see %s' % (name, rc, logp))
    return open(logp, encoding='utf-8').read()

def st_tu4pass1(a, cfg):
    progress('tu4pass1', 0.0)
    run_script('build_tu4.py', [cfg.w('tu4pass1')], {'M2ONLY': 'vz', 'M2NOCOSTUMES': '1', 'M2_DLCWAD': cfg.w('none.wad')}, 'tu4pass1', a)

def st_dlc(a, cfg):
    out = run_script('build_dlc.py', [cfg.w('content')], {}, 'dlc', a)
    if 'wrote ' not in out:
        if 'pending' in out and a.limit: raise Pending()
        raise Fail('DLC conversion did not finish - see %s' % cfg.w('dlc.log'))

def st_art(a, cfg):
    import dlcart
    progress('art', 0.0)
    dlcart.terrain(os.path.join(cfg.w('content'), 'DLC01.wad'), cfg.w('art', 'dlc_terrain.png'))
    progress('art', 1.0)

def st_tu4(a, cfg):
    run_script('build_tu4.py', [cfg.w('content')], {'M2_DLCWAD': cfg.w('content', 'DLC01.wad')}, 'tu4', a)

def st_finish(a, cfg):
    src = cfg.w('content')
    files = []
    for root, ds, fs in os.walk(src):
        for f in fs: files.append(os.path.relpath(os.path.join(root, f), src))
    for k, rel in enumerate(sorted(files)):
        progress('finish', k / max(1, len(files)), rel)
        dst = os.path.join(a.out, rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
        tmp = dst + '.new'
        shutil.copyfile(os.path.join(src, rel), tmp)
        os.replace(tmp, dst)
    progress('finish', 1.0)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True); ap.add_argument('--iso', required=True)
    ap.add_argument('--tu', required=True); ap.add_argument('--dlc', required=True)
    ap.add_argument('--work'); ap.add_argument('--out'); ap.add_argument('--limit', type=float, default=0)
    ap.add_argument('--force', action='store_true'); ap.add_argument('--clean', action='store_true')
    ap.add_argument('--keep-work', action='store_true', help='keep the build folder (caches, extracted files) after success')
    a = ap.parse_args()
    a.game = os.path.abspath(a.game)
    a.out = os.path.abspath(a.out or os.path.join(a.game, 'Mercenaries2Remastered', 'content'))
    a.work = os.path.abspath(a.work or os.path.join(a.game, 'Mercenaries2Remastered', 'build'))
    if a.clean: shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work, exist_ok=True)
    os.environ.update(M2_PCDATA=os.path.join(a.game, 'data'), M2_ISO=os.path.abspath(a.iso), M2_WORK=a.work,
                      M2_OUT=os.path.join(a.work, 'content'))
    import cfg
    a.cfg = cfg
    for n in ('vz.wad', 'shell.wad'):
        if not os.path.exists(os.path.join(a.game, 'data', n)): return say('ERROR: %s not found in %s\\data - wrong game folder?' % (n, a.game)) or 2
    stp = cfg.w('state.json')
    state = json.load(open(stp)) if os.path.exists(stp) else {}
    inputs = [a.iso, a.tu, a.dlc]
    if state.get('inputs_src') != inputs: state = {'inputs_src': inputs}
    T = time.time()
    try:
        for s in STAGES:
            if state.get(s): continue
            t0 = time.time()
            globals()['st_' + s](a, cfg)
            state[s] = round(time.time() - t0, 1)
            json.dump(state, open(stp, 'w'))
            say('stage %s done in %.0f s' % (s, state[s]))
    except Pending:
        json.dump(state, open(stp, 'w')); say('PENDING'); return 3
    except Fail as e:
        say('ERROR: %s' % e); return 1
    except Exception:
        say('ERROR: unexpected failure'); traceback.print_exc(file=sys.stdout); return 1
    if not a.keep_work:
        shutil.rmtree(a.work, ignore_errors=True)
    say('DONE in %.0f s' % (time.time() - T))
    return 0

if __name__ == '__main__':
    sys.exit(main())

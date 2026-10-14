# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Xbox wavebank (0xf753f6d0, XMA) -> PC wavebank (PCM16 in-memory / IMA ADPCM streaming .pws)."""
import struct, numpy as np
from xma import xma2_wav, decode
import ima
ENT = 'IBBBBIIIIIII'
def parse(d):
    n = struct.unpack_from('>H', d, 8)[0]; hs = struct.unpack_from('>I', d, 16)[0]
    ents = [list(struct.unpack_from('>' + ENT, d, hs + 36 * i)) for i in range(n)]
    return n, hs, ents
def header(d, hs, n):
    h = bytearray(d[:hs])
    h[1:4] = b'\0\0\0'
    struct.pack_into('<I', h, 4, struct.unpack_from('>I', d, 4)[0])
    struct.pack_into('<H', h, 8, n)
    struct.pack_into('<I', h, 12, struct.unpack_from('>I', d, 12)[0])
    struct.pack_into('<I', h, 16, hs)
    struct.pack_into('<I', h, 20, struct.unpack_from('>I', d, 20)[0])
    return h
def xma_pcm(data, rate, ch, samples):
    pcm, err = decode(xma2_wav(data, rate, ch, samples))
    a = np.frombuffer(pcm, '<i2')
    if not len(a): raise RuntimeError('xma decode failed: ' + err)
    return a
def convert_mem(d, log=None):
    n, hs, E = parse(d)
    out = header(d, hs, n); out += b'\0' * (36 * n)
    datas = []
    for i, e in enumerate(E):
        ea = hs + 36 * i
        raw = d[ea + e[11]: ea + e[11] + e[6]]
        if e[3] == 5: pcm = xma_pcm(raw, e[5], e[2], e[7]).tobytes()
        else: raise RuntimeError('codec %d' % e[3])
        datas.append(pcm)
    for i, (e, pcm) in enumerate(zip(E, datas)):
        while len(out) % 16: out += b'\0'
        ea = hs + 36 * i
        pe = [e[0], 0, e[2], 2, 0, e[5], len(pcm), len(pcm) // (2 * e[2]), 0, 0, 0, len(out) - ea]
        struct.pack_into('<' + ENT, out, ea, *pe)
        out += pcm
    return bytes(out)
def convert_stream(d, pws_path, log=None):
    """returns (pc bank bytes, pc pws bytes)"""
    n, hs, E = parse(d)
    out = header(d, hs, n); out += b'\0' * (36 * n)
    pws = bytearray()
    with open(pws_path, 'rb') as f:
        for i, e in enumerate(E):
            f.seek(e[11]); raw = f.read(e[6])
            pcm = xma_pcm(raw, e[5], e[2], e[7])
            frames = len(pcm) // e[2]
            adp = ima.encode64(pcm, e[2])
            pe = [e[0], 0, e[2], 4, 0, e[5], len(adp), frames * 2 * e[2], 0, 0, 0, len(pws)]
            struct.pack_into('<' + ENT, out, hs + 36 * i, *pe)
            pws += adp
    return bytes(out), bytes(pws)

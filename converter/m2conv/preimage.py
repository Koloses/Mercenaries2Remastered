# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Find lowercase strings with a given engine hash: fnv1a(lower(s)) ^ 0x2a, * prime."""
import numpy as np
P = 0x1000193; PINV = pow(P, -1, 1 << 32); M = 0xffffffff
ALPHA = np.frombuffer(b'abcdefghijklmnopqrstuvwxyz0123456789', np.uint8).astype(np.uint64)
def ph(s):
    h = 0x811c9dc5
    for c in s.encode() if isinstance(s, str) else s: h = ((h ^ (c | 0x20)) * P) & M
    return ((h ^ 0x2a) * P) & M
class Pre:
    def __init__(s, prefix=b'dlc_'):
        s.prefix = prefix
        h = 0x811c9dc5
        for c in prefix: h = ((h ^ (c | 0x20)) * P) & M
        hs = np.array([h], np.uint64); codes = np.zeros(1, np.uint64)
        for k in range(4):
            hs = ((hs[:, None] ^ ALPHA[None, :]) * np.uint64(P)) & np.uint64(M)
            codes = codes[:, None] * np.uint64(36) + np.arange(36, dtype=np.uint64)[None, :]
            hs = hs.ravel(); codes = codes.ravel()
        order = np.argsort(hs); s.fh = hs[order]; s.fc = codes[order]
        # tails (3 chars), stored as index arrays
        t = np.arange(36 ** 3, dtype=np.uint64)
        s.t = [(t // 1296) % 36, (t // 36) % 36, t % 36]
    def find(s, target):
        hn = np.uint64((((target * PINV) & M) ^ 0x2a))
        h = np.full(36 ** 3, hn, np.uint64)
        for k in (2, 1, 0):
            h = ((h * np.uint64(PINV)) & np.uint64(M)) ^ ALPHA[s.t[k]]
        idx = np.searchsorted(s.fh, h); idx[idx >= len(s.fh)] = 0
        hit = np.nonzero(s.fh[idx] == h)[0]
        if len(hit) == 0: return None
        j = hit[0]; code = int(s.fc[idx[j]])
        mid = ''.join(chr(ALPHA[(code // 36 ** (3 - q)) % 36]) for q in range(4))
        tail = ''.join(chr(ALPHA[s.t[q][j]]) for q in range(3))
        r = s.prefix.decode() + mid + tail
        assert ph(r) == target, (r, hex(ph(r)), hex(target))
        return r
if __name__ == '__main__':
    import time; T = time.time(); pr = Pre(); print('init', time.time() - T)
    for tg in (0xfcaba0d8, 0x4bfe4286, 0x12345678, 0xdeadbeef):
        T = time.time(); r = pr.find(tg); print(hex(tg), r, round(time.time() - T, 3))

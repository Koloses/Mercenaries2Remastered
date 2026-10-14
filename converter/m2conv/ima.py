# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
import ctypes, os, numpy as np
_lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'bin', 'imaenc.dll' if os.name == 'nt' else 'imaenc.so'))
def encode(pcm16, block_align=36):
    a = np.ascontiguousarray(pcm16, dtype='<i2'); n = len(a)
    spb = 1 + 2 * (block_align - 4)
    out = np.zeros(((n + spb - 1) // spb) * block_align, np.uint8)
    m = _lib.ima_encode_mono(a.ctypes.data_as(ctypes.c_void_p), n, out.ctypes.data_as(ctypes.c_void_p), block_align)
    return out[:m].tobytes()
def encode64(pcm16, ch):
    """pcm16: interleaved int16 array; returns MS-IMA blocks of 64 frames (36*ch bytes each)"""
    a = np.ascontiguousarray(pcm16, dtype='<i2'); frames = len(a) // ch
    out = np.zeros(((frames + 63) // 64) * 36 * ch, np.uint8)
    _lib.ima_encode64.restype = ctypes.c_int
    m = _lib.ima_encode64(a.ctypes.data_as(ctypes.c_void_p), frames, ch, out.ctypes.data_as(ctypes.c_void_p))
    return out[:m].tobytes()

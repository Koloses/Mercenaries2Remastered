# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
import struct, subprocess, os, tempfile
import cfg
FF = cfg.FFMPEG
_NOWIN = 0x08000000 if os.name == 'nt' else 0
def xma2_wav(data, rate, ch, samples, block=0x10000):
    nblocks = (len(data) + block - 1) // block
    fmt = struct.pack('<HHIIHHH', 0x166, ch, rate, rate * 2 * ch, 2048, 16, 34)
    fmt += struct.pack('<HIIIIIIIBBI H', 1, 0 if ch == 1 else 3, samples, block, 0, samples, 0, 0, 0, 3, nblocks, 0)[:34]
    riff = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data
    return b'RIFF' + struct.pack('<I', len(riff)) + riff
def xma1_wav(data, rate, ch):
    fmt = struct.pack('<HHIIHHH', 0x165, ch, rate, rate * 2 * ch, 2048, 16, 32)
    fmt += struct.pack('<HHIIIIBBH', 0, 1, 0, 0, 0, 0, 0, 0, 0)  # simplified
    fmt += struct.pack('<IIIIBBH', 0, rate, 0, 0, 0, ch, 0)
    riff = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data
    return b'RIFF' + struct.pack('<I', len(riff)) + riff
def decode(wav):
    with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as f: f.write(wav); n = f.name
    r = subprocess.run([FF, '-hide_banner', '-loglevel', 'error', '-i', n, '-f', 's16le', '-acodec', 'pcm_s16le', '-'], capture_output=True, creationflags=_NOWIN)
    os.unlink(n)
    return r.stdout, r.stderr.decode()[:300]

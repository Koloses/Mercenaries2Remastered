# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Minimal DXT1/3/5 decode via Pillow (for validation)."""
import io, struct
from PIL import Image
def dds(w, h, fourcc, data):
    hd = bytearray(128)
    struct.pack_into('<4sIIIIIII', hd, 0, b'DDS ', 124, 0x1007, h, w, 0, 0, 1)
    struct.pack_into('<II4s', hd, 76, 32, 4, fourcc)
    struct.pack_into('<I', hd, 108, 0x1000)
    return Image.open(io.BytesIO(bytes(hd) + data)).convert('RGBA')

# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Paths for the converter (set by m2conv.py through environment variables)."""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
PCDATA = os.environ.get('M2_PCDATA', '')            # <game>\data
ISO = os.environ.get('M2_ISO', '')                  # Xbox 360 game image
WORK = os.environ.get('M2_WORK', os.path.join(HERE, 'work'))
OUT = os.environ.get('M2_OUT', os.path.join(WORK, 'content'))
FFMPEG = os.environ.get('M2_FFMPEG', os.path.join(HERE, 'bin', 'ffmpeg.exe' if os.name == 'nt' else 'ffmpeg'))
DATA = os.path.join(HERE, 'data')
def w(*p): return os.path.join(WORK, *p)
def pc_parts(name):
    """<name> plus split continuation files <name>_1, <name>_2 ..."""
    b = os.path.join(PCDATA, name); out = [b]; k = 1
    while os.path.exists('%s_%d' % (b, k)): out.append('%s_%d' % (b, k)); k += 1
    return out if len(out) > 1 else b

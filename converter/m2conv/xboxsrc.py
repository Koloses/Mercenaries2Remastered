# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Locate the Xbox game files (ISO + title update + DLC) for the porting tools."""
import os, glob, sys
sys.path.insert(0, os.path.dirname(__file__))
from xiso import XISO
from wad import Wad
def iso_wad(iso_path, name):
    x = XISO(iso_path)
    for p, sec, size in x.files():
        if p.lower() == name.lower():
            return Wad((iso_path, x.base + sec * 2048, size))
    raise KeyError(name)

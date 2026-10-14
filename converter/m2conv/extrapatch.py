# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
import sys,os,struct; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from avm1patch import *
def patch_extras_up(d):
    # Extras page, entry 4: Up / W go to the Downloadable Content entry (the PC build skipped it)
    AT = 0xd1013; IFAT = 0xd076e; BACK = 0xd1001
    assert d[IFAT] == 0x9D and d[AT - 1] == 0x1d, (hex(d[IFAT]), hex(d[AT - 1]))
    blk = bytearray()
    for frm, to in (('_out', 'extrasbutton4_btn'), ('_over', 'extrasbutton1_btn')):
        blk += push(frm, 1, ('r', 1), 'extraspage_mc') + GETMEMBER + push(to) + GETMEMBER + push('gotoAndPlay') + CALLMETHOD + POP
    blk += push(0, 'soundClick') + CALLFUNC + POP + push('extras_btnSelect', 1) + SETVAR
    skip_len = 5
    start = AT + skip_len
    end_jump_src = start + len(blk) + 5
    blk += jump(BACK - end_jump_src)
    code = jump(len(blk)) + bytes(blk)
    d = insert(d, AT, code)
    struct.pack_into('<h', d := bytearray(d), IFAT + 3, start - (IFAT + 5))
    # W (keyboard, and the left stick, which the engine sends as the Forward key) takes the same way as Up
    IFW = IFAT + 0x10
    assert d[IFW] == 0x9D and d[IFW - 5:IFW - 1] == bytes.fromhex('57000000'), d[IFW - 8:IFW + 5].hex()
    struct.pack_into('<h', d, IFW + 3, start - (IFW + 5))
    return bytes(d)
if __name__ == '__main__':
    # extrapatch.py <decompressed shell movie> <out>
    d = bytearray(open(sys.argv[1], 'rb').read()); d[0xcf31d + 6] = 1
    out = patch_extras_up(bytes(d))
    open(sys.argv[2], 'wb').write(out); print(len(d), len(out))

# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Havok 5.5 hkClass reflection (class layouts used to convert Xbox Havok packfiles) from the user's
Mercenaries2.exe (PC), read from the hkClass static constructors.  build(exe, out) -> number of classes"""
import struct, capstone, pickle, sys
def build(EXE, OUT):
  d = open(EXE, 'rb').read()
  pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]; oh = struct.unpack_from('<H', d, pe + 20)[0]
  ib = struct.unpack_from('<I', d, pe + 52)[0]; sec = pe + 24 + oh
  S = [struct.unpack_from('<8sIIII', d, sec + i * 40) for i in range(ns)]
  va2o = lambda v: next((v - ib - va + ro for n, vs, va, rs, ro in S if ib + va <= v < ib + va + rs), None)
  o2va = lambda o: next((o - ro + va + ib for n, vs, va, rs, ro in S if ro <= o < ro + rs), None)
  def cstr(v):
      o = va2o(v)
      if o is None or v == 0: return None
      e = d.find(b'\0', o); return d[o:e].decode('latin1')
  cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
  classes = {}
  text = [s for s in S if s[0].startswith(b'.text')][0]
  tstart, tsize = text[4], text[3]
  # the hkClass constructor (0x895770 in exe 1.0, 0x895AB0 in 1.1): the function most often called as
  # `push x12 / mov ecx, imm32 / call`
  from collections import Counter
  cnt = Counter(); p = tstart
  while True:
      p = d.find(b'\xb9', p + 1, tstart + tsize)
      if p < 0: break
      if d[p + 5] != 0xe8 or d[p - 2] != 0x6a and d[p - 5] != 0x68: continue
      cnt[o2va(p) + 10 + struct.unpack_from('<i', d, p + 6)[0]] += 1
  CTOR = cnt.most_common(1)[0][0]
  call_pat = b'\xe8'
  p = tstart
  while True:
      p = d.find(b'\xb9', p + 1, tstart + tsize)          # mov ecx, imm32
      if p < 0: break
      if d[p + 5] != 0xe8: continue
      va = o2va(p)
      tgt = va + 5 + 5 + struct.unpack_from('<i', d, p + 6)[0]
      if tgt != CTOR: continue
      obj = struct.unpack_from('<I', d, p + 1)[0]
      # decode preceding pushes (walk back up to 80 bytes, take the last 12 pushes)
      pushes = []
      for st in range(p - 80, p):
          ins = list(cs.disasm(d[st:p], o2va(st)))
          if ins and ins[-1].address + ins[-1].size == va and all(i.mnemonic == 'push' for i in ins):
              pushes = [i.op_str for i in ins]
              break
      if len(pushes) < 12: continue
      args = [int(x, 16) if x.startswith('0x') else int(x) for x in reversed(pushes[-12:])]
      name, parent, size, ifc, nifc, enums, nenums, members, nmembers, defaults, attrs, flags = args
      mem = []
      if members and nmembers:
          o = va2o(members)
          for k in range(nmembers):
              mn, mc, me, mt, mst, mca, mfl, moff, mat = struct.unpack_from('<IIIBBhHHI', d, o + 24 * k)
              mem.append(dict(name=cstr(mn), cls=mc, enum=me, type=mt, sub=mst, carray=mca, flags=mfl, off=moff))
      classes[obj] = dict(name=cstr(name), parent=parent, size=size, members=mem)
  byname = {}
  # duplicates (several layouts under one name): the largest, then the lowest object address (not the code order,
  # which differs between exe builds); hkClass itself: the Havok 5.5 layout (with 'attributes'), which moved in 1.1
  key = lambda k, c: (c['size'], len(c['members']),
                      c['name'] == 'hkClass' and any(m['name'] == 'attributes' for m in c['members']), -k)
  for k, c in classes.items():
      cur = byname.get(c['name'])
      if cur is None or key(k, c) > key(*cur): byname[c['name']] = (k, c)
  pickle.dump((classes, byname), open(OUT, 'wb'))
  return len(classes)
if __name__ == '__main__':
    print(build(sys.argv[1], sys.argv[2]))

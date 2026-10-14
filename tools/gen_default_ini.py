# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""Write the DLL's built-in default Mercenaries2Remastered.ini (g_defaultIni in dll/src/util.c) to a file."""
import re, sys
s = open(sys.argv[1]).read()
i = s.index('g_defaultIni[] ='); j = s.index(';\n', i)
body = re.sub(r'#ifdef M2R_SHOT.*?#endif', '', s[i + len('g_defaultIni[] ='):j], flags=re.S) if '--shot' not in sys.argv else s[i + len('g_defaultIni[] ='):j]
txt = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', body)).encode().decode('unicode_escape')
open(sys.argv[2], 'w', newline='').write(txt)

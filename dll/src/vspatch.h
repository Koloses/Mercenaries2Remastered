/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
#pragma once
#include <windows.h>
#define VSP_FREQ    2.6     /* crease frequency (1/m) */
#define VSP_WRINKLE 0.30    /* crease strength relative to the dent */
typedef struct { int objReg, base, ireg, nd, skinned, gsem; } VsPatchInfo;
int  VsPatchInit(void);
void VsPatchSetCap(float maxDisplacement);
UINT VsCodeSize(const DWORD *code);
/* patched copy of a vs_3_0 shader (malloc'd) or 0 with a reason */
int  VsPatch(const DWORD *code, int nd, int glass, VsPatchInfo *pi, DWORD **out, char *why, int whyn);
/* pixel shader twin for broken glass: kills pixels where texcoord<sem>.w (breakage) is high */
int  PsPatch(const DWORD *code, int sem, DWORD **out, char *why, int whyn);

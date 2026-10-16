/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Paper animations. Patch 1.1 ("paper animations removed") added `cmp ecx,0x8000A23D / je return 0` to the two
 * effect spawn functions, which turns off one effect: global_particle_flotsum_papers, the sheets of paper the
 * atmosphere's ambient effect slots blow along the streets around the camera. 1.0 and the Xbox 360 build spawn it.
 * The 1.1 build removes the two checks again. [Graphics] PaperAnimations. */
#include "m2r.h"

void PaperInit(void)
{
#ifdef M2R_GAME11
    if (!IniInt("Graphics", "PaperAnimations", 1)) return;
    if (Expect(A11(0x48C095), "81 F9 3D A2 00 80 74 F1") && Expect(A11(0x48C17F), "81 F9 3D A2 00 80 74 F1")) {
        static const BYTE nop2[2] = {0x90, 0x90};
        WriteMem(A11(0x48C09B), nop2, 2);
        WriteMem(A11(0x48C185), nop2, 2);
        Log("paper: blowing paper effect back on (removed by patch 1.1)");
    } else Log("paper: code mismatch, skipped");
#endif
}

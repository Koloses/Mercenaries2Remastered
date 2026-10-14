/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Twitching stowed weapons, holsters and other loose parts ([Fixes] TwitchFix).
 *
 * Loose parts of a model (the weapon slung on the back, holsters, pouches, straps) are swung by a secondary-motion
 * node of the animation graph (vtable 0xBCD280, apply FUN_00677770, run for every character each animation
 * update FUN_00695D10). It simulates in fixed 1/60 s steps: the frame time is accumulated at +0xA8 and the node
 * returns straight away (0x6777AA) while less than one step has built up - without writing its bone. That bone
 * then keeps the plain animated pose for the frame, and the stowed item attached to it jumps between the
 * animated and the simulated pose. At 30 fps (what the game was made for) every frame holds a step; at 60 fps
 * the frame times alternate around 1/60 s (15 / 18 ms) and the item flips by a centimetre or two every frame.
 *
 * The node now always runs to the end: with less than a step built up it keeps the time for the next frame
 * (+0xA8) and skips only the simulation, then writes the bone from its current state (+0xAC) as on every other
 * frame. */
#include "m2r.h"

static const float g_step = 1.f / 60.f;   /* [0xBCD278] */

/* replaces movss xmm0,[esp+0x10] / comiss xmm0,[0.0] / jbe 0x6779FE at 0x6778B0; [esp+0x10] = time built up,
 * +0xA8 was cleared at 0x6777BF */
__attribute__((naked)) static void StepCheck(void)
{
    __asm__ volatile(
        "movss 0x10(%%esp), %%xmm0\n\t"
        "comiss %0, %%xmm0\n\t"
        "jae 1f\n\t"
        "movss %%xmm0, 0xA8(%%ebx)\n\t"            /* not a whole step: keep it, no simulation */
        "push $0x6779FE\n\t"
        "ret\n\t"
        "1:\n\t"
        "push $0x6778C3\n\t"
        "ret\n\t" :: "m"(g_step));
}

void JitterInit(void)
{
    if (!IniInt("Fixes", "TwitchFix", 1)) return;
    if (!Expect(0x6777AA, "0F 87 42 03 00 00") || !Expect(0x6778B0, "F3 0F 10 44 24 10 0F 2F 05 90 B6 B9 00 0F 86 3B 01 00 00") ||
        *(const float *)0xBCD278 != g_step) {
        Log("twitch: unexpected code, off");
        return;
    }
    static const BYTE nop6[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    WriteMem(0x6777AA, nop6, 6);
    WriteJmp(0x6778B0, (void *)StepCheck, 19);
    Log("twitch: secondary motion bones written every frame");
}

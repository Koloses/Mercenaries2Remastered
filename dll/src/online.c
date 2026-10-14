/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * Offline by default.
 *
 * Continue / New Game / Load Game send fscommand("onlineLTIIsConnected", n)
 * from the shell. The handler (0x614540, hash 0x9708B7F2) then pops the
 * "Play online?" box (LTIonlineMsgBox ... out-func 3 = LTIProfileOnlinePlay).
 * The online servers are gone, so we answer it right away: call the shell's
 * own LTIProfileOnlinePlay(2) = "play offline", exactly like the handler
 * already does for "Join game" (0x614D16) with argument 1.
 */
#include "m2r.h"

static const double g_two = 2.0;

/* jumped to from 0x614D46 (same stack frame as the original block) */
__attribute__((naked)) static void OfflineStub(void)
{
    __asm__ volatile (
        "movsd %0, %%xmm0\n\t"
        "push $0\n\t"
        "push $1\n\t"
        "lea 0x20(%%esp), %%edx\n\t"
        "push %%edx\n\t"
        "mov $0xBBC524, %%edi\n\t"          /* "LTIProfileOnlinePlay" */
        "movl $3, 0x24(%%esp)\n\t"           /* GFx value type: number */
        "movsd %%xmm0, 0x2c(%%esp)\n\t"
        "mov $0x61C550, %%eax\n\t"
        "call *%%eax\n\t"                    /* invoke shell function */
        "pop %%edi\n\t"
        "pop %%esi\n\t"
        "pop %%ebx\n\t"
        "mov %%ebp, %%esp\n\t"
        "pop %%ebp\n\t"
        "ret\n\t"
        :: "m"(g_two));
}

void OnlineInit(void)
{
    if (!IniInt("Gameplay", "SkipOnlinePrompt", 1)) return;
    if (Expect(0x614D46, "68 50 EA 6C 00 6A 06 6A 10") && Expect(0x614D2B, "BF 24 C5 BB 00")) {
        WriteJmp(0x614D46, (void *)OfflineStub, 5);
        Log("online: 'play online?' prompt answered with offline");
    } else Log("online: prompt site mismatch, not patched");
}

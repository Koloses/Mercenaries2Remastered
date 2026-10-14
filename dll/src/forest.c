/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Forest instance decoder (FUN_0049F700), called for every tree instance in view every frame (about 20 000 a
 * frame in towns, 1.2 million a second) to build the distant tree billboards. Each instance is a bit-packed
 * record in the forest cell's bit array (cell table + 0x198), starting at node +0x2C:
 *   4 bits flags | x (width table +0x3C[k]) | y (+0x90[k]) | z (+0xE4[k]) | 4 bits w      (k = node +0x18)
 * position = node box min + (box max - min) * step[width] * q (step table 0xB9C538), w = q4 * [0xB9C548],
 * returns (flags & mask) != 0. The game reads each field with a 64-bit shift helper call and x87 conversions;
 * this does the same in a few integer and SSE operations, with identical results (same field order, the
 * same 32-bit-width quirk of the mask, single-precision arithmetic as the game's FPU setting).
 * [Performance] FastForest. */
#include "m2r.h"

static inline DWORD Field(const DWORD *w, DWORD pos, DWORD width, DWORD mask)
{
    const DWORD *p = w + (pos >> 5);
    unsigned long long v = ((unsigned long long)p[0] << 32) | p[1];
    DWORD sh = (64 - (pos & 31) - width) & 0xFF;
    return sh >= 64 ? 0 : (DWORD)(v >> sh) & mask;
}

static int __cdecl Decode(const BYTE *tab, const BYTE *node, float *out, DWORD sel)
{
    DWORD k = *(const DWORD *)(node + 0x18);
    DWORD wx = *(const DWORD *)(tab + 0x3C + k * 4), wy = *(const DWORD *)(tab + 0x90 + k * 4), wz = *(const DWORD *)(tab + 0xE4 + k * 4);
    const DWORD *bits = *(const DWORD *const *)(tab + 0x198);
    DWORD pos = *(const DWORD *)(node + 0x2C);
    DWORD flags = Field(bits, pos, 4, 0xF); pos += 4;
    DWORD qx = Field(bits, pos, wx, (1u << (wx & 31)) - 1); pos += wx;
    DWORD qy = Field(bits, pos, wy, (1u << (wy & 31)) - 1); pos += wy;
    DWORD qz = Field(bits, pos, wz, (1u << (wz & 31)) - 1); pos += wz;
    DWORD q4 = Field(bits, pos, 4, 0xF);
    const float *bb = (const float *)node, *step = (const float *)0xB9C538;
    out[0] = (bb[3] - bb[0]) * step[wx] * (float)qx + bb[0];
    out[1] = (bb[4] - bb[1]) * step[wy] * (float)qy + bb[1];
    out[2] = (bb[5] - bb[2]) * step[wz] * (float)qz + bb[2];
    out[3] = (float)q4 * *(const float *)0xB9C548;
    return (flags & sel) != 0;
}

/* eax = cell table, stack: node, out, mask; ret 0xC; al = result */
__attribute__((naked)) static void DecodeEntry(void)
{
    __asm__ volatile(
        "push 12(%%esp)\n\t"
        "push 12(%%esp)\n\t"
        "push 12(%%esp)\n\t"
        "push %%eax\n\t"
        "call %P0\n\t"
        "add $16, %%esp\n\t"
        "ret $12\n\t" :: "i"(Decode));
}

/* Billboard clipping (FUN_0049FF10, called from the forest draw at 0x6BC0FB with a node of the forest's cell
 * table +0x34) is reached while a forest is still being set up after a teleport or load: the table is not there
 * yet and the node pointer is null (crash at 0x49FF1E). Such a call now adds nothing (returns eax unchanged). */
__attribute__((naked)) static void ClipEntry(void)
{
    __asm__ volatile(
        "cmpl $0, 8(%esp)\n\t"
        "jne 1f\n\t"
        "ret\n\t"
        "1:\n\t"
        "sub $0x460, %esp\n\t"
        "push $0x49FF16\n\t"
        "ret");
}

/* Tree spawning (FUN_004A2850, called from the forest update 0x4A419A for each forest in turn) reads the
 * forest's tree data at forest +4, which is null while that forest is still being set up after a load (crash at
 * 0x4A2911 when the DLC map started). Such a forest now spawns nothing this frame. */
__attribute__((naked)) static void SpawnEntry(void)
{
    __asm__ volatile(
        "cmpl $0, 4(%edi)\n\t"
        "jne 1f\n\t"
        "ret $8\n\t"
        "1:\n\t"
        "push %ebp\n\t"
        "mov %esp, %ebp\n\t"
        "and $-16, %esp\n\t"
        "push $0x4A2856\n\t"
        "ret");
}

void ForestInit(void)
{
    if (Expect(0x4A2850, "55 8B EC 83 E4 F0 83 EC 38 85 C0 8B 4F 04")) WriteJmp(0x4A2850, (void *)SpawnEntry, 6);
    else Log("forest: tree spawn code mismatch");
    if (Expect(0x49FF10, "81 EC 60 04 00 00 53 8B 9C 24 6C 04 00 00")) WriteJmp(0x49FF10, (void *)ClipEntry, 6);
    else Log("forest: billboard clip code mismatch");
    if (!IniInt("Performance", "FastForest", 1)) return;
    if (!Expect(0x49F700, "83 EC 18 53 8B 5C 24 20 8B 4B 18 8B 54 88 3C 55 89 54 24 24") ||
        !Expect(0x49F71B, "8B 8C 88 E4 00 00 00") || !Expect(0x49F8E0, "D8 0D 48 C5 B9 00")) {
        Log("forest: decoder code mismatch, skipped");
        return;
    }
    WriteJmp(0x49F700, (void *)DecodeEntry, 5);
    Log("forest: fast instance decoder on");
}

/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Per-pass culling. The scene view (FUN_00466D40) gathers the visible objects once and then draws them in several
 * passes, each a loop over the list calling one renderable method per object:
 *   depth pre-pass   FUN_00468E40  vtable +0x1C, for every object in the view frustum (0x468E78)
 *   shadow maps      FUN_00468CA0  vtable +0x18, per cascade (0x468E0D)
 *   main colour      FUN_00468BB0  vtable +0x14, only objects the occlusion queries saw within the last 3 frames
 *   water reflection FUN_004677D0  vtable +0x14, objects flagged to reflect (0x467A9F)
 * Draw submission is what limits the frame rate in towns (thousands of draws per pass on one CPU thread), and the
 * extra passes draw much that cannot be seen:
 *   - the depth pre-pass draws everything in the frustum, also what the occlusion queries found hidden behind
 *     buildings; it is skipped for objects the main pass would not draw (no query hit within the last 3 frames,
 *     the same rule and frame counter the main pass uses: object +0x1D0 = last frame seen, view +0x2BE4 = frame),
 *     so those are only drawn when they come back into view, exactly as in the colour pass
 *   - shadows and the 426x240 reflection draw objects whose projected size is a pixel or two; an object is skipped
 *     there when its size (largest edge of its local bounds +0x94..+0xA8) over its distance to the camera (per
 *     view at +0x18) is below [Performance] ShadowCull / ReflectionCull.
 * The main colour pass is untouched, so nothing visible disappears.
 *
 * The water reflection (FUN_00486FA0, called from the scene view at 0x46732C) is redrawn only every
 * [Performance] ReflectionRate frames (default 2). It renders into its own 426x240 target, which keeps its
 * contents, and the reflection matrix it stores for the water shader stays with that picture, so the water
 * shows the reflection from one frame earlier. */
#include "m2r.h"

static float g_shadowK, g_reflK, g_depthK;
static int g_depthOcc, g_reflRate;

static float Ratio(const BYTE *o)
{
    WORD vp = *(WORD *)(*(BYTE **)0xDFC2F8 + 0x2B92);
    float d = *(const float *)(o + 0x18 + vp * 4);
    const float *mn = (const float *)(o + 0x94), *mx = (const float *)(o + 0xA0);
    float s = mx[0] - mn[0], t = mx[1] - mn[1], u = mx[2] - mn[2];
    if (t > s) s = t;
    if (u > s) s = u;
    if (!(d > 1.f) || !(s > 0.f)) return 1e9f;          /* unknown or near: keep */
    return s / d;
}
static int __fastcall SkipShadow(const BYTE *o) { return Ratio(o) < g_shadowK; }
static int __fastcall SkipRefl(const BYTE *o) { return Ratio(o) < g_reflK; }
static int __fastcall SkipDepth(const BYTE *o)
{
    if (g_depthOcc) {
        WORD q = *(const WORD *)(o + 0x1C0);
        DWORD idx = *(const DWORD *)(o + 0x1C4 + q * 4);
        BYTE *dev = *(BYTE **)0x1176288;
        if (idx != 0xFFFF && dev && (*(DWORD *)(dev + 0x5E4) >> 4 & 1)) {
            DWORD frame = *(DWORD *)(*(BYTE **)0xDFC2F8 + 0x2BE4);
            if (frame - *(const DWORD *)(o + 0x1D0) > 3) return 1;
        }
    }
    return g_depthK > 0.f && Ratio(o) < g_depthK;
}

/* 0x468E78: mov edx,[edi] / mov eax,[edx+0x1C] / push ebp / mov ecx,edi / call eax  (10 bytes) */
__attribute__((naked)) static void DepthDraw(void)
{
    __asm__ volatile(
        "mov %%edi, %%ecx\n\t"
        "call %P0\n\t"
        "test %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "mov (%%edi), %%edx\n\t"
        "push %%ebp\n\t"
        "mov %%edi, %%ecx\n\t"
        "call *0x1C(%%edx)\n\t"
        "1: ret\n\t" :: "i"(SkipDepth));
}
/* 0x468E0D: mov ecx,[ebp+8] / mov eax,[esi] / mov edx,[eax+0x18] / push ecx / mov ecx,esi / call edx  (13 bytes) */
__attribute__((naked)) static void ShadowDraw(void)
{
    __asm__ volatile(
        "mov %%esi, %%ecx\n\t"
        "call %P0\n\t"
        "test %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "mov 8(%%ebp), %%ecx\n\t"
        "mov (%%esi), %%eax\n\t"
        "push %%ecx\n\t"
        "mov %%esi, %%ecx\n\t"
        "call *0x18(%%eax)\n\t"
        "1: ret\n\t" :: "i"(SkipShadow));
}
/* 0x467A9F: mov edx,[edi] / mov eax,[ebp+8] / mov edx,[edx+0x14] / push eax / mov ecx,edi / call edx  (13 bytes) */
__attribute__((naked)) static void ReflDraw(void)
{
    __asm__ volatile(
        "mov %%edi, %%ecx\n\t"
        "call %P0\n\t"
        "test %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "mov (%%edi), %%edx\n\t"
        "mov 8(%%ebp), %%eax\n\t"
        "push %%eax\n\t"
        "mov %%edi, %%ecx\n\t"
        "call *0x14(%%edx)\n\t"
        "1: ret\n\t" :: "i"(SkipRefl));
}

/* 0x46732C: call 0x486FA0 (eax = water system) */
static int g_reflN;
__attribute__((naked)) static void ReflPass(void)
{
    __asm__ volatile(
        "incl %0\n\t"
        "push %%eax\n\t"
        "mov %0, %%eax\n\t"
        "xor %%edx, %%edx\n\t"
        "divl %1\n\t"
        "test %%edx, %%edx\n\t"
        "pop %%eax\n\t"
        "jnz 1f\n\t"
        "push $0x486FA0\n\t"
        "1: ret\n\t" : "+m"(g_reflN) : "m"(g_reflRate) : "edx");
}

void CullInit(void)
{
    g_depthOcc = IniInt("Performance", "DepthPassOcclusion", 1);
    g_depthK = IniFloat("Performance", "DepthCull", 0.f);
    g_shadowK = IniFloat("Performance", "ShadowCull", 0.02f);
    g_reflK = IniFloat("Performance", "ReflectionCull", 0.05f);
    g_reflRate = IniInt("Performance", "ReflectionRate", 2);
    if (g_reflRate > 4) g_reflRate = 4;
    if (g_reflRate > 1 && Expect(0x46732C, "E8 6F FC 01 00")) WriteCall(0x46732C, (void *)ReflPass, 5);
    else if (g_reflRate > 1) Log("cull: reflection call site mismatch, skipped");
    if ((g_depthOcc || g_depthK > 0.f) && Expect(0x468E78, "8B 17 8B 42 1C 55 8B CF FF D0"))
        WriteCall(0x468E78, (void *)DepthDraw, 10);
    else if (g_depthOcc || g_depthK > 0.f) Log("cull: depth pass code mismatch, skipped");
    if (g_shadowK > 0.f && Expect(0x468E0D, "8B 4D 08 8B 06 8B 50 18 51 8B CE FF D2"))
        WriteCall(0x468E0D, (void *)ShadowDraw, 13);
    else if (g_shadowK > 0.f) Log("cull: shadow pass code mismatch, skipped");
    if (g_reflK > 0.f && Expect(0x467A9F, "8B 17 8B 45 08 8B 52 14 50 8B CF FF D2"))
        WriteCall(0x467A9F, (void *)ReflDraw, 13);
    else if (g_reflK > 0.f) Log("cull: reflection pass code mismatch, skipped");
    Log("cull: depth pass occlusion %d (size %.3f), shadow size %.3f, reflection size %.3f, every %d frames", g_depthOcc, g_depthK,
        g_shadowK, g_reflK, g_reflRate > 1 ? g_reflRate : 1);
}

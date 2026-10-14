/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* View distances: grass and foliage ([Graphics] GrassDistanceScale), all other model parts and objects
 * ([Graphics] ObjectDistanceScale), model detail levels ([Graphics] LodDistanceScale), 3D trees
 * ([Graphics] TreeDistanceScale) and world streaming ([Graphics] WorldDistanceScale); 1 = game.
 *
 * Ground vegetation comes from two systems, and their model parts (stride 0x1D0, model +0x1C) carry their own
 * draw distances: +4 fade start, +8 max distance.
 *   Scrub: ScrubObject placements (FUN_0066C500, the only caller of FUN_00665CB0) register every LOD group of
 *   the scrub model with the largest of its parts' max distances (FUN_00665CB0 + group offset -> FUN_00517440),
 *   so that value is how far away a patch is found for drawing at all.
 *   Vegetation objects (vtable 0xBAC998, FUN_004A5830) test every part against it while drawing.
 * The two values are scaled once per part by the grass scale, whatever the part's shader (the scrub of the
 * different regions uses several material types: only scaling the foliage material left other regions' grass
 * at the object scale), before either user of them sees it:
 *   FUN_00665CB0, hooked at its entry
 *   FUN_004A5830 (0x4A592C: movss xmm0,[edi+8] / comiss xmm1,xmm0)
 */
#include "m2r.h"
#include <math.h>
#include <string.h>

static float g_scale = 1.f;
static int g_logged;

/* Every part is scaled once. Parts are remembered with the distance they were left at: model memory is reused
 * when scrub and vegetation models are unloaded and others loaded (streaming, helicopter transfers), and a new
 * part at a remembered address (a different distance there) is scaled again - remembering only the address left
 * the grass of every later loaded area at the game's distance. */
#define PSET (1 << 20)
typedef struct { void *p; float v; } PartSeen;
static PartSeen g_parts[PSET];
static CRITICAL_SECTION g_partLock;
static void __cdecl ScalePart(BYTE *part)
{
    if (!part) return;
    float k = g_scale, *f = (float *)(part + 4);
    DWORD h = ((DWORD)(UINT_PTR)part >> 4) * 2654435761u;
    EnterCriticalSection(&g_partLock);
    PartSeen *e = NULL;
    for (int i = 0; i < 64; i++) {
        PartSeen *q = &g_parts[(h + i) & (PSET - 1)];
        if (q->p == part || !q->p) { e = q; break; }
    }
    if (e && !(e->p == part && e->v == f[1])) {                  /* new part (or a new one at this address) */
        if (k > 1.001f && f[1] > 0.f && f[1] < 5000.f) {
            if (g_logged++ < 4) Log("lod: part distances %.0f/%.0f m -> %.0f/%.0f m", f[0], f[1], f[0] * k, f[1] * k);
            f[0] *= k; f[1] *= k;
        }
        e->p = part; e->v = f[1];
    }                                                            /* table crowded: leave it */
    LeaveCriticalSection(&g_partLock);
}
/* FUN_00665CB0: ecx = model, eax = LOD group */
static void __cdecl ScaleModel(BYTE *model, int grp)
{
    BYTE *g = *(BYTE **)(model + 0x24) + grp * 0x38;
    unsigned n = *(WORD *)(g + 0x36), first = *(WORD *)(g + 0x34);
    BYTE *refs = *(BYTE **)(model + 0x34), *parts = *(BYTE **)(model + 0x1C);
    for (unsigned i = 0; i < n; i++) ScalePart(parts + *(WORD *)(refs + (first + i) * 8 + 6) * 0x1D0);
}
__attribute__((naked)) static void MaxDistStub(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "push %%eax\n\t"
        "push %%ecx\n\t"
        "call %P0\n\t"
        "add $8, %%esp\n\t"
        "popal\n\t"
        "push %%ecx\n\t"                        /* the 5 replaced bytes: push ecx / xorps xmm0,xmm0 / push ebx */
        "xorps %%xmm0, %%xmm0\n\t"
        "push %%ebx\n\t"
        "push $0x665CB5\n\t"
        "ret\n\t" :: "i"(ScaleModel));
}
/* 0x4A592C in the foliage draw loop, edi = part */
__attribute__((naked)) static void DrawCmp(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "push %%edi\n\t"
        "call %P0\n\t"
        "add $4, %%esp\n\t"
        "popal\n\t"
        "movss 8(%%edi), %%xmm0\n\t"
        "comiss %%xmm0, %%xmm1\n\t"
        "ret\n\t" :: "i"(ScalePart));
}

/* Trees and palms (forests: FUN_004A0C40 loads them, FUN_004A4060 updates them every frame).
 * Every tree of a forest is drawn as a flat billboard (PgBillboardTree shaders, one static vertex buffer per
 * forest). Trees within the forest's near radius of a viewer are spawned as real objects (FUN_004A28F0: up to
 * 30 per frame, 6 forests per frame, 2000 at most) and cross-faded in; their billboard is switched off through
 * the per-tree state bits, and FUN_004A2EF0 despawns them again outside the radius. The radius comes from the
 * level data and is multiplied by 0.75 when the forest is loaded (0x4A17DA: mulss xmm0, [0.75]; also on Xbox),
 * then kept at forest +0x6D4 and squared at +0x6D8, used by the spawn query, the despawn test and the clamp of
 * the spawned tree's draw distance. That multiplier is replaced by 0.75 x TreeDistanceScale, and the per-frame
 * spawn budget and forest count are raised so the larger area fills as fast. The 2000 limit stays: trees past it
 * simply stay billboards. */
static float g_treeMul = 0.75f;
static void TreeInit(void)
{
    float k = IniFloat("Graphics", "TreeDistanceScale", 3.0f);
    if (k <= 1.01f) return;
    if (k > 6.f) k = 6.f;
    if (!Expect(0x4A17DA, "F3 0F 59 05 6C 77 B9 00") || !Expect(0x4A4140, "83 F8 06 76 07 BB 06 00 00 00 EB 0C 83 F8 1E 8B D8 76 05 BB 1E 00 00 00") ||
        !Expect(0x4A4174, "83 F8 1E") || !Expect(0x4A4189, "BA 1E 00 00 00")) { Log("trees: code mismatch, skipped"); return; }
    g_treeMul = 0.75f * k;
    float *p = &g_treeMul;
    WriteMem(0x4A17DE, &p, 4);                                  /* mulss xmm0, [&g_treeMul] */
    static const BYTE forests = 40, budget = 64;                /* forests per frame (all), spawns per frame */
    WriteMem(0x4A4142, &forests, 1); WriteMem(0x4A4146, &forests, 1);
    WriteMem(0x4A414E, &forests, 1); WriteMem(0x4A4154, &forests, 1);
    WriteMem(0x4A4176, &budget, 1); WriteMem(0x4A418A, &budget, 1);
    Log("trees: 3D tree radius x%.2f (forest data x%.3f), up to %d spawns per frame", k, g_treeMul, budget);
}

/* World objects (terrain pieces, mountains, buildings) are kept loaded and drawn within their own distance x the
 * world distance scale (grid +0x38 = 0x179C7E8, set by FUN_0050C830 from 0xD1E3E0, default 1.0, clamped to 1.0
 * at 0x50C88F). The streaming stepper FUN_00735640 loads an entry while a viewer is within radius x scale (+7 m)
 * and object draw distances are distance x scale (0x466ACF, 0x665E29); its quick reject (0x7356BA) compares the
 * unscaled radius with the distance to the cell, so it is made to compare distance / scale (0xD1BF34 = 1/scale,
 * kept by FUN_0050C830). The renderer's per-frame tables that more objects in view overflow are enlarged in
 * drawpool.c. [Graphics] WorldDistanceScale. */
static float g_worldScale = 1.f;
static void WorldInit(void)
{
    float k = IniFloat("Graphics", "WorldDistanceScale", 2.0f);
    if (k <= 1.001f) return;
    if (k > 3.f) k = 3.f;
    if (!Expect(0x50C88A, "0F 2F C8 76 03 0F 28 C8") || !Expect(0x7356BA, "D9 45 0C D8 66 3C D9 5C 24 1C D9 44 24 1C") ||
        !Expect(0xD1E3E0, "00 00 80 3F")) { Log("world: code mismatch, skipped"); return; }
    g_worldScale = k;
    static const BYTE nop3[3] = {0x90, 0x90, 0x90};
    WriteMem(0x50C88F, nop3, 3);                                /* no clamp to 1.0 */
    BYTE q[14] = {0xD9, 0x45, 0x0C,                             /* fld [ebp+0xc]   cell distance */
                  0xD8, 0x66, 0x3C,                             /* fsub [esi+0x3c] - 7 m */
                  0xD8, 0x0D, 0, 0, 0, 0,                       /* fmul [0xD1BF34] / scale */
                  0x90, 0x90};
    *(DWORD *)(q + 8) = 0xD1BF34;
    WriteMem(0x7356BA, q, sizeof q);
    WriteMem(0xD1E3E0, &g_worldScale, 4);
    Log("world: object load and draw distance x%.2f", k);
}

/* Object draw distance. Every drawable object keeps its distance to each viewer at +0x18[viewer] and fades out
 * between +0x1B0 (start) and +0x1B0 + +0x1B4: alpha = 1 - (d - start) / range, hidden at 0. Three places compute
 * that (FUN_00477490 for models, 0x4A7237 and 0x4A77F7 for two other object kinds); the distance they use is
 * divided by [Graphics] ObjectDistanceScale, which moves start and range out by the same factor. The stored
 * distance (+0x18, used for LOD choice and sorting) stays the real one. */
static float g_objInv = 1.f;
__attribute__((naked)) static void ObjFadeSSE(void)
{
    __asm__ volatile(
        "movss 0x18(%%esi,%%eax,4), %%xmm4\n\t"
        "mulss %0, %%xmm4\n\t"
        "subss 0x1B0(%%esi), %%xmm4\n\t"
        "divss 0x1B4(%%esi), %%xmm4\n\t"
        "ret\n\t" :: "m"(g_objInv));
}
__attribute__((naked)) static void ObjFadeX87(void)
{
    __asm__ volatile(
        "fmuls %0\n\t"
        "fsubs 0x1B0(%%esi)\n\t"
        "ret\n\t" :: "m"(g_objInv));
}
/* Every placed object type has a GenericLOD template (table 0x17BD190, registered through FUN_0064A600 from
 * the binary database at 0x63AA23 and the text schema at 0x65F445):
 *   +0 u16 Distance  how far objects of the type are kept and drawn (most props: 100 m)
 *   +2 u8 Q2, +3 u8 Q1, +4 u8 Q0   mesh quality switch distances (props: 255/240/80 m, others 160/60/20)
 * Poles, signs, fences and other props are kept and drawn within that Distance. Near-range types (below
 * 400 m) are scaled by [Graphics] ObjectDistanceScale (up to 1000 m), the quality switches with them (bytes,
 * so up to 255 m). Long-range types (400..5000 m: buildings, rock faces, terrain pieces) by the smaller
 * [Graphics] FarObjectDistanceScale (up to 6000 m): scaling them as much as props made loading run out of
 * memory. Types of 5000 m and more are kept for the whole map anyway. */
static float g_genScale = 1.f, g_farScale = 1.f;
static void __cdecl GenLod(DWORD name, BYTE *t)
{
    if (!t) return;
    WORD *dist = (WORD *)t;
    unsigned d0 = *dist, d;
    if (d0 < 400) {
        if (g_genScale <= 1.001f) return;
        d = (unsigned)(d0 * g_genScale + 0.5f);
        *dist = (WORD)(d > 1000 ? 1000 : d);
        for (int i = 2; i <= 4; i++) { unsigned q = (unsigned)(t[i] * g_genScale + 0.5f); t[i] = (BYTE)(q > 255 ? 255 : q); }
    } else {
        /* long-range types (buildings, rock faces, terrain pieces, mountains) */
        if (g_farScale <= 1.001f || d0 >= 5000) return;
        d = (unsigned)(d0 * g_farScale + 0.5f);
        *dist = (WORD)(d > 6000 ? 6000 : d);
    }
    static WORD seen[32]; static int ns; int k;
    for (k = 0; k < ns && seen[k] != d0; k++) ;
    if (k == ns && ns < 32) { seen[ns++] = (WORD)d0; Log("objects: type distance %u m -> %u m", d0, *dist); }
}
/* replaces "call 0x64A600" at the two GenericLOD registrations: [esp+4] = name key, [esp+8] = record */
__attribute__((naked)) static void GenLodStub(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "pushl 0x28(%%esp)\n\t"
        "pushl 0x28(%%esp)\n\t"
        "call %P0\n\t"
        "add $8, %%esp\n\t"
        "popal\n\t"
        "push $0x64A600\n\t"
        "ret\n\t" :: "i"(GenLod));
}

static void ObjInit(void)
{
    float k = IniFloat("Graphics", "ObjectDistanceScale", 2.0f);
    if (k <= 1.001f) return;
    if (k > 4.f) k = 4.f;
    if (!Expect(0x477537, "F3 0F 10 64 86 18 F3 0F 5C A6 B0 01 00 00 F3 0F 5E A6 B4 01 00 00") ||
        !Expect(0x4A7237, "D8 A6 B0 01 00 00") || !Expect(0x4A77F7, "D8 A6 B0 01 00 00")) { Log("objects: code mismatch, skipped"); return; }
    g_objInv = 1.f / k;
    g_farScale = IniFloat("Graphics", "FarObjectDistanceScale", 1.5f);
    if (g_farScale > 3.f) g_farScale = 3.f;
    if (Expect(0x63AA1E, "BF 90 D1 7B 01 E8") && Expect(0x65F440, "BF 90 D1 7B 01 E8")) {
        g_genScale = k;
        WriteCall(0x63AA23, (void *)GenLodStub, 5);
        WriteCall(0x65F445, (void *)GenLodStub, 5);
    } else Log("objects: GenericLOD registration mismatch");
    WriteCall(0x477537, (void *)ObjFadeSSE, 22);
    WriteCall(0x4A7237, (void *)ObjFadeX87, 6);
    WriteCall(0x4A77F7, (void *)ObjFadeX87, 6);
    Log("objects: draw distance x%.2f, long-range types x%.2f", k, g_farScale);
}

/* Model detail levels. FUN_00470740 picks a model's LOD from distance / (model +0x84 x camera +0xE70):
 * level = log2 of that + 1, clamped to the model's range; the coarsest levels of many models (cars, poles,
 * props) hold few or no parts, so they thin out or vanish there. Camera +0xE70 is set to 0.672 for the scene
 * camera (0x8590B6, constant 0xBEB458); [Graphics] LodDistanceScale multiplies it, moving every level
 * switch (and the last, empty level) out by that factor. */
static float g_lodScale = 0.672015f;
static void LodScaleInit(void)
{
    float k = IniFloat("Graphics", "LodDistanceScale", 2.0f);
    if (k <= 1.001f) return;
    if (k > 4.f) k = 4.f;
    if (!Expect(0x8590B6, "F3 0F 10 0D 58 B4 BE 00")) { Log("lod: code mismatch, skipped"); return; }
    g_lodScale = *(float *)0xBEB458 * k;
    float *p = &g_lodScale;
    WriteMem(0x8590BA, &p, 4);
    Log("lod: model detail distance x%.2f", k);
}

/* Camera near and far planes. FUN_007140B0 builds a camera's parameters: +0x40 near plane, +0x44 far plane (the
 * camera's slot, copied into the frame's view at 0x85900E and turned into its projection at 0x859150). The far
 * plane is the atmosphere region's own value or, for regions left at the default 2400 m, ViewDistance (0xDFC348,
 * 0..100) x 0.01 x 2000 + 400 m; [Graphics] FarClipScale multiplies it.
 * The near plane is the camera controller's +0x5E0: 0.5 x clearance^2, lowered further while the camera's
 * collision pulls it in, clamped to 0.2..1 m (0x713E1F..0x713E3C). It drops to 0.2 m next to walls, doors and
 * plants - often with the character metres away - and the sky dome's distance haze (pixel shader with depthMap /
 * projectParams, drawn at the sky depth with the viewport's MinZ..MaxZ = 0.999..1 range at 0x47F7C0) then covers
 * the landscape from a few hundred metres instead of kilometres: mountains vanish into haze.
 * The near plane is now 1 m, or [Graphics] NearPlaneMargin short of the player character's body when the
 * drawn camera (taken from the frame's ViewProj, post.c; the controller's eye is placed before the camera
 * collision pulls it in, cam.c's rig before that) is closer than that (body: a vertical 1.9 m segment at its position, 0.4 m thick; also the camera's
 * own target - the vehicle being driven, the tank being hijacked), never below the game's 0.2 m: the camera
 * does not cut into the character, and the landscape keeps the depth precision of a 1 m near plane. Both changes are made at 0x714219, the function's single exit. */
static float g_farK = 1.f, g_margin = 0.3f;
extern volatile DWORD g_playerGuid;

static int TargetPos(DWORD guid, float *out)
{
    DWORD buf[8] = {0};
    DWORD g = guid, *b = buf;
    BYTE ok;
    __asm__ volatile (
        "push %%ebp\n\t"
        "push %%ebx\n\t"
        "mov $0x665AF0, %%eax\n\t"
        "call *%%eax\n\t"
        "pop %%ebx\n\t"
        "pop %%ebp\n\t"
        : "=a"(ok), "+D"(g), "+S"(b)
        :
        : "ecx", "edx", "memory", "cc");
    if (!ok) return 0;
    memcpy(out, buf, 12);
    return out[0] == out[0] && fabsf(out[0]) < 1e6f && fabsf(out[1]) < 1e6f && fabsf(out[2]) < 1e6f;
}
/* distance from the eye to a standing body at p */
static float BodyDist(const float *eye, const float *p)
{
    float y = eye[1] - p[1];
    if (y < 0.f) y = 0.f;
    if (y > 1.9f) y = 1.9f;
    float dx = eye[0] - p[0], dy = eye[1] - (p[1] + y), dz = eye[2] - p[2];
    return sqrtf(dx * dx + dy * dy + dz * dz) - 0.4f;
}
volatile float g_vpCam[3];   /* the scene camera position (post.c, from the frame's ViewProj) */
static void __cdecl NearPlane(float *blk, BYTE *ctrl)
{
    if (!ctrl) return;
    float eye[3] = {g_vpCam[0], g_vpCam[1], g_vpCam[2]};   /* last frame's drawn camera */
    if (!(eye[0] == eye[0]) || (eye[0] == 0.f && eye[1] == 0.f && eye[2] == 0.f)) return;   /* unknown: the game's own near plane */
    float d = 1e9f, p[3];
    DWORD tg = *(DWORD *)(ctrl + 0x20), pl = g_playerGuid;
    if (pl && TargetPos(pl, p)) { float e = BodyDist(eye, p); if (e < d) d = e; }
    if (tg && tg != pl && TargetPos(tg, p)) { float e = BodyDist(eye, p); if (e < d) d = e; }
    /* 1 m, or just short of the character's body when the camera is closer than that */
    float n = d - g_margin;
    if (n > 1.f) n = 1.f;
    if (n < 0.2f) n = 0.2f;
    blk[0x40 / 4] = n;
}
/* replaces the epilogue at 0x714219: pop edi / mov eax,esi / pop ebx / mov esp,ebp / pop ebp / ret 4 */
__attribute__((naked)) static void CamEpilogue(void)
{
    __asm__ volatile(
        "flds 0x44(%%esi)\n\t"
        "fmuls %0\n\t"
        "fstps 0x44(%%esi)\n\t"
        "push 8(%%ebp)\n\t"
        "push %%esi\n\t"
        "call %P1\n\t"
        "add $8, %%esp\n\t"
        "pop %%edi\n\t"
        "mov %%esi, %%eax\n\t"
        "pop %%ebx\n\t"
        "mov %%ebp, %%esp\n\t"
        "pop %%ebp\n\t"
        "ret $4\n\t" :: "m"(g_farK), "i"(NearPlane));
}
static void CamPlanesInit(void)
{
    float k = IniFloat("Graphics", "FarClipScale", 2.0f);
    if (k < 1.f) k = 1.f;
    if (k > 4.f) k = 4.f;
    g_margin = IniFloat("Graphics", "NearPlaneMargin", 0.3f);
    if (!Expect(0x714219, "5F 8B C6 5B 8B E5 5D C2 04 00")) { Log("camera planes: code mismatch, skipped"); return; }
    g_farK = k;
    WriteJmp(0x714219, (void *)CamEpilogue, 10);
    Log("camera planes: far x%.2f, near plane 1 m or %.2f m short of the character", k, g_margin);
}

/* Terrain detail. A full-detail terrain chunk (TerrainObject activation 0x66CAC0 -> 0x4AA8A0 -> constructor
 * 0x4AA640, vtable 0xBACD98) gets five distances from the terrain data: +0x1F0/+0x1F4 the detail-texture fade
 * (shader FadeDistances c240: blend from start to end), +0x200/+0x204 the second fade (c241), and its mesh level
 * is (camera distance - bounding radius) / +0x1F4, 0 or 1 (draw 0x4AAB60, shadow 0x4AAF26, depth 0x4AB19A), so
 * the coarse mesh replaces the full one exactly where the detail textures have faded out. [Graphics]
 * TerrainDetailScale multiplies the four distances on their way into the constructor: detail textures and the
 * full mesh reach that much farther. */
static float g_terrK = 1.f;
/* replaces push ebx / mov ebx,[esp+8] at 0x4AA640; args: [esp+4] owner, [esp+8] fade0 start, [esp+0xC] fade0 end
 * (= mesh level step), [esp+0x10] unused here, [esp+0x14] fade1 start, [esp+0x18] fade1 end */
__attribute__((naked)) static void TerrCtor(void)
{
    __asm__ volatile(
        "flds 0x08(%%esp)\n\t" "fmuls %0\n\t" "fstps 0x08(%%esp)\n\t"
        "flds 0x0C(%%esp)\n\t" "fmuls %0\n\t" "fstps 0x0C(%%esp)\n\t"
        "flds 0x14(%%esp)\n\t" "fmuls %0\n\t" "fstps 0x14(%%esp)\n\t"
        "flds 0x18(%%esp)\n\t" "fmuls %0\n\t" "fstps 0x18(%%esp)\n\t"
        "push %%ebx\n\t"
        "mov 8(%%esp), %%ebx\n\t"
        "push $0x4AA645\n\t"
        "ret\n\t" :: "m"(g_terrK));
}
static void TerrainInit(void)
{
    float k = IniFloat("Graphics", "TerrainDetailScale", 2.0f);
    if (k <= 1.001f) return;
    if (k > 6.f) k = 6.f;
    if (!Expect(0x4AA640, "53 8B 5C 24 08 56 57 8B F1") || !Expect(0x4AA8D1, "E8 6A FD FF FF")) { Log("terrain: code mismatch, skipped"); return; }
    g_terrK = k;
    WriteJmp(0x4AA640, (void *)TerrCtor, 5);
    Log("terrain: full-detail mesh and detail textures x%.2f", k);
}

void LodInit(void)
{
    TerrainInit();
    CamPlanesInit();
    LodScaleInit();
    ObjInit();
    WorldInit();
    TreeInit();
    g_scale = IniFloat("Graphics", "GrassDistanceScale", 3.0f);
    if (g_scale > 10.f) g_scale = 10.f;
    if (g_scale <= 1.01f) return;
    InitializeCriticalSection(&g_partLock);
    if (Expect(0x4A592C, "F3 0F 10 47 08 0F 2F C8 0F 87") && Expect(0x665CB0, "51 0F 57 C0 53 56 8B F1")) {
        WriteCall(0x4A592C, (void *)DrawCmp, 8);
        WriteJmp(0x665CB0, (void *)MaxDistStub, 5);
        Log("grass: scrub and vegetation draw distance x%.2f", g_scale);
    } else Log("grass: code mismatch, skipped");
}

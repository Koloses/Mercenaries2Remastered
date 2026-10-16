/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* m2r:no-retarget
 * Fire on burning bushes, trees and wrecks. Model-state scripts start fires with StartEmitter (1.1 0x4D2A50,
 * Xbox 0x8222FA68): the effect emits from the mesh surface under one model node (shape types 1/4/5 at
 * effect+0x770). The PC emitter measures that surface from CPU-side mesh copies (sub-mesh +0x154/+0x158/+0x15C)
 * that many meshes do not have (the Xbox reads the vertex buffers instead), and it skips hidden nodes. The surface
 * then comes out as 0, nothing is emitted and e.g. a dying jungle canopy never shows its flames.
 * 1) StartEmitter mesh emitters include hidden nodes (+0x7A5 bit0, normally taken from the template).
 * 2) A mesh emitter whose surface still counts 0 becomes a volume emitter: the bounding box of the meshes under
 *    its node (model node matrices x instance world matrix), particles born at random points inside it.
 * [Graphics] EmitterFix. */
#define COBJMACROS
#include "m2r.h"
#include <d3d9.h>
#include <math.h>

#ifdef M2R_GAME11
#define EF_AREACALL 0x4787D4u    /* submesh loader: call 0x85C070 (index copy), reached only for AREA submeshes */
#define EF_IDXCOPY 0x85C070u
#define EF_AREARET 0x4787D9u
#define EF_ALLOC   0x84AE70u
#define EF_SETNE   0x4D2C3Au     /* setne dl -> include-hidden flag passed to 0x48B540 */
#define EF_CALL    0x48ADBFu     /* call mesh area walker (count mode) in the emission count 0x48AD50 */
#define EF_WALKER  0x49BC90u
#define EF_ECALL   0x48FED8u     /* call emit-by-shape in the job emission 0x48F9D0 */
#define EF_EMIT    0x48AF50u
#else
#define EF_AREACALL 0x4786F4u
#define EF_IDXCOPY 0x85BE50u
#define EF_AREARET 0x4786F9u
#define EF_ALLOC   0x84AC20u
#define EF_SETNE   0x4D2AAAu
#define EF_CALL    0x48ACEFu
#define EF_WALKER  0x49BC30u
#define EF_ECALL   0x48FE08u
#define EF_EMIT    0x48AE80u
#endif

static DWORD g_walker = EF_WALKER, g_emit = EF_EMIT, g_idxCopy = EF_IDXCOPY, g_areaRet = EF_AREARET;
static volatile LONG g_areaMeshes, g_areaFixedPos;

/* ---- port of the Xbox emission surface (Xbox loader 0x821C9268 -> 0x82573EE0, walkers 0x821EA888/0x821EB2A8) ----
 * The PC loader copies the strip indices (+0x154) and FLOAT16 positions (+0x158) of every submesh that has an AREA
 * chunk, but never fills the per-triangle area array (+0x15C), so every mesh-surface emitter gets area 0.
 * Area of strip triangle i = |cross(b-a, c-a)| / 2 (0 for cuts / bad indices), stored as FLOAT16 per index.
 * The PC position copy also ignores the POSITION element offset (the Xbox uses it): rebuilt when it is not 0. */
static float HalfToFloat(WORD h)
{
    DWORD s = (DWORD)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, f;
    if (!e) { if (!m) f = s; else { int k = -1; do { k++; m <<= 1; } while (!(m & 1024)); m &= 1023; f = s | ((DWORD)(112 - k) << 23) | (m << 13); } }
    else if (e == 31) f = s | 0x7F800000 | (m << 13);
    else f = s | ((e + 112) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}
static WORD FloatToHalf(float v)
{
    if (!(v > 0)) return 0;
    if (v >= 65504.f) return 0x7BFF;
    DWORD f; memcpy(&f, &v, 4); int e = (int)((f >> 23) & 255) - 127 + 15; DWORD m = f & 0x7FFFFF;
    if (e <= 0) { if (e < -10) return 0; m |= 0x800000; return (WORD)(m >> (14 - e)); }
    return (WORD)((e << 10) | (m >> 13));
}
static void *GameAlloc(DWORD size)
{
    void *r; DWORD fn = EF_ALLOC;
    __asm__ volatile("push $1\n\tpush %2\n\tcall *%1\n\tadd $8, %%esp" : "=a"(r) : "r"(fn), "r"(size), "a"(0), "c"(0) : "edx", "memory", "cc");
    return r;
}
void __cdecl EfBuildArea(BYTE *E, const BYTE *strm, const BYTE *ibuf)
{
    (void)ibuf;
    if (IsBadReadPtr(E, 0x18)) return;
    int n = *(int *)E, nv = *(int *)(E + 4);
    const WORD *idx = *(const WORD **)(E + 0xC); WORD *pos = *(WORD **)(E + 0x10);
    InterlockedIncrement(&g_areaMeshes);
    if (!idx || !pos || n < 3 || nv <= 0 || *(void **)(E + 0x14)) return;
    BYTE *sub = E - 0x148;
    IDirect3DVertexDeclaration9 *decl = *(IDirect3DVertexDeclaration9 **)(sub + 0x118);
    BYTE *desc = *(BYTE **)(sub + 0x110);
    if (decl && desc && strm && !IsBadReadPtr(desc, 0x10)) {
        D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1]; UINT ne = MAXD3DDECLLENGTH + 1;
        if (SUCCEEDED(IDirect3DVertexDeclaration9_GetDeclaration(decl, el, &ne)))
            for (UINT k = 0; k < ne && el[k].Stream != 0xFF; k++)
                if (el[k].Usage == D3DDECLUSAGE_POSITION && el[k].UsageIndex == 0 && el[k].Type == D3DDECLTYPE_FLOAT16_4) {
                    DWORD stride = *(DWORD *)(desc + 8), off = el[k].Offset;
                    if (off && stride >= off + 6 && !IsBadReadPtr(strm, (nv - 1) * stride + off + 6)) {
                        for (int v = 0; v < nv; v++) memcpy(pos + v * 3, strm + v * stride + off, 6);
                        InterlockedIncrement(&g_areaFixedPos);
                    }
                    break;
                }
    }
    WORD *area = (WORD *)GameAlloc((DWORD)n * 2);
    if (!area) return;
    for (int i = 0; i < n; i++) {
        float a = 0;
        if (i + 2 < n) {
            WORD i0 = idx[i], i1 = idx[i + 1], i2 = idx[i + 2];
            if (i0 != 0xFFFF && i1 != 0xFFFF && i2 != 0xFFFF && i0 < nv && i1 < nv && i2 < nv) {
                float p[3][3]; WORD ii[3] = {i0, i1, i2};
                for (int t = 0; t < 3; t++) for (int j = 0; j < 3; j++) p[t][j] = HalfToFloat(pos[ii[t] * 3 + j]);
                float u[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
                float w[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
                float c0 = u[1] * w[2] - u[2] * w[1], c1 = u[2] * w[0] - u[0] * w[2], c2 = u[0] * w[1] - u[1] * w[0];
                float q = c0 * c0 + c1 * c1 + c2 * c2;
                if (q > 0) a = 0.5f * sqrtf(q);
            }
        }
        area[i] = FloatToHalf(a);
    }
    MemoryBarrier();
    *(WORD **)(E + 0x14) = area;
}
/* replaces the call to the index copier at the end of the submesh loader */
__attribute__((naked)) static void AreaStub(void)
{
    __asm__ volatile(
        "pop %%eax\n\t"
        "call *%0\n\t"                      /* original index copy (stdcall, 2 args): esp = loader esp again */
        "pushal\n\t"
        "pushfl\n\t"
        "pushl 0x40(%%esp)\n\t"             /* loader [esp+0x1C]: IBUF file data */
        "pushl 0x40(%%esp)\n\t"             /* loader [esp+0x18]: STRM file data */
        "push %%edi\n\t"                    /* E = submesh + 0x148 */
        "call _EfBuildArea\n\t"
        "add $12, %%esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%1"
        : : "m"(g_idxCopy), "m"(g_areaRet));
}
static volatile LONG g_converted, g_boxes;

typedef struct { BYTE *volatile shape; DWORD owner, def; float mn[3], mx[3]; } EfBox;
#define NBOX 128
static EfBox g_box[NBOX];
static volatile LONG g_boxNext;

static int Ok(float f) { return f == f && f > -1e6f && f < 1e6f; }

/* world AABB of the meshes under node `hash` of render instance `inst` (all meshes if none is under it) */
static int MeshBox(BYTE *inst, DWORD hash, float *mn, float *mx)
{
    if (!inst || IsBadReadPtr(inst, 0x2C0)) return 0;
    BYTE *model = *(BYTE **)(inst + 0x1E0), *mats = *(BYTE **)(inst + 0x2B8);
    if (!model || !mats || IsBadReadPtr(model, 0x64)) return 0;
    int nparts = *(int *)(model + 0x4C); BYTE *parts = *(BYTE **)(model + 0x50), *nodes = *(BYTE **)(model + 0x60);
    void **meshes = *(void ***)(model + 0x58);
    if (nparts <= 0 || nparts > 4096 || !parts || !nodes || !meshes || IsBadReadPtr(parts, nparts * 4)) return 0;
    const float *W = (const float *)(inst + 0x40);
    for (int pass = 0; pass < 2; pass++) {
        int n = 0;
        mn[0] = mn[1] = mn[2] = 1e30f; mx[0] = mx[1] = mx[2] = -1e30f;
        for (int i = 0; i < nparts; i++) {
            BYTE *p = parts + i * 4; int node = *(short *)p;
            if (node < 0) continue;
            if (!pass) {                                    /* node chain must reach `hash` */
                int k = node, hit = 0, guard = 0;
                while (k >= 0 && guard++ < 256) { BYTE *nd = nodes + k * 0xB0; if (IsBadReadPtr(nd, 8)) break; if (*(DWORD *)nd == hash) { hit = 1; break; } k = *(short *)(nd + 4); }
                if (!hit) continue;
            }
            void *mesh = meshes[p[2]];
            if (!mesh || IsBadReadPtr(mesh, 4)) continue;
            void **vt = *(void ***)mesh;
            if (IsBadReadPtr(vt, 0x2C)) continue;
            float *lo, *hi;
            { void *m = mesh; __asm__ volatile("call *%2" : "=a"(lo), "+c"(m) : "r"(vt[0x24 / 4]) : "edx", "memory", "cc"); }
            { void *m = mesh; __asm__ volatile("call *%2" : "=a"(hi), "+c"(m) : "r"(vt[0x28 / 4]) : "edx", "memory", "cc"); }
            if (IsBadReadPtr(lo, 12) || IsBadReadPtr(hi, 12)) continue;
            const float *N = (const float *)(mats + node * 0x40);
            if (IsBadReadPtr(N, 0x40)) continue;
            for (int c = 0; c < 8; c++) {
                float v[3] = {(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2]}, a[3], w[3];
                for (int j = 0; j < 3; j++) a[j] = v[0] * N[j] + v[1] * N[4 + j] + v[2] * N[8 + j] + N[12 + j];
                for (int j = 0; j < 3; j++) w[j] = a[0] * W[j] + a[1] * W[4 + j] + a[2] * W[8 + j] + W[12 + j];
                for (int j = 0; j < 3; j++) { if (!Ok(w[j])) goto bad; if (w[j] < mn[j]) mn[j] = w[j]; if (w[j] > mx[j]) mx[j] = w[j]; }
            }
            n++;
            continue;
        bad:;
        }
        if (n) return 1;
    }
    return 0;
}

/* count stage: the mesh surface is empty -> volume emitter (type 0 + box), or a plain point emitter */
int __cdecl EfConvert(BYTE *shape)
{
    BYTE *eff = shape - 0x770;
    float mn[3], mx[3];
    *(DWORD *)shape = 0;
    InterlockedIncrement(&g_converted);
    if (!MeshBox(*(BYTE **)(shape + 4), *(DWORD *)(shape + 0x10), mn, mx)) return 0;
    const float *at = (const float *)(eff + 0x7C4);
    float cx = (mn[0] + mx[0]) / 2 - at[0], cy = (mn[1] + mx[1]) / 2 - at[1], cz = (mn[2] + mx[2]) / 2 - at[2];
    if (cx * cx + cy * cy + cz * cz > 40.f * 40.f) return 0;           /* not around the effect: keep the point */
    for (int j = 0; j < 3; j++) {                                        /* stay inside the foliage, cap the size */
        float c = (mn[j] + mx[j]) / 2, h = (mx[j] - mn[j]) / 2 * 0.8f;
        if (h > 15.f) h = 15.f;
        mn[j] = c - h; mx[j] = c + h;
    }
    LONG i = (InterlockedIncrement(&g_boxNext) - 1) & (NBOX - 1);
    EfBox *b = &g_box[i];
    b->shape = NULL; MemoryBarrier();
    b->owner = *(DWORD *)(eff + 0x10); b->def = *(DWORD *)(eff + 0x7E4);
    for (int j = 0; j < 3; j++) { b->mn[j] = mn[j]; b->mx[j] = mx[j]; }
    MemoryBarrier(); b->shape = shape;
    InterlockedIncrement(&g_boxes);
    return 0;
}

/* emission stage: n new particles at pos[] (world) -> random points of the box */
void __cdecl EfSpread(int n, float *pos, BYTE *shape)
{
    if (n <= 0 || !pos || *(DWORD *)shape != 0) return;
    BYTE *eff = shape - 0x770;
    for (int i = 0; i < NBOX; i++) {
        EfBox *b = &g_box[i];
        if (b->shape != shape) continue;
        if (b->owner != *(DWORD *)(eff + 0x10) || b->def != *(DWORD *)(eff + 0x7E4)) { b->shape = NULL; return; }
        DWORD r = (DWORD)__builtin_ia32_rdtsc() * 2654435761u + (DWORD)(uintptr_t)pos;
        for (int k = 0; k < n && k < 4096; k++)
            for (int j = 0; j < 3; j++) {
                r = r * 1664525u + 1013904223u;
                float t = (r >> 8) * (1.0f / 16777216.0f);
                pos[k * 3 + j] = b->mn[j] + (b->mx[j] - b->mn[j]) * t;
            }
        return;
    }
}

/* replaces the walker call (count): eax = model instance, edx = 0, 5 stack args, ecx = shape */
__attribute__((naked)) static void CountStub(void)
{
    __asm__ volatile(
        "push %%ecx\n\t"
        "pushl 0x18(%%esp)\n\t" "pushl 0x18(%%esp)\n\t" "pushl 0x18(%%esp)\n\t" "pushl 0x18(%%esp)\n\t" "pushl 0x18(%%esp)\n\t"
        "call *%0\n\t"
        "add $0x14, %%esp\n\t"
        "pop %%ecx\n\t"
        "test %%eax, %%eax\n\t"
        "jg 1f\n\t"
        "mov (%%ecx), %%edx\n\t"
        "cmp $1, %%edx\n\t" "je 2f\n\t"
        "cmp $4, %%edx\n\t" "je 2f\n\t"
        "cmp $5, %%edx\n\t" "jne 1f\n\t"
        "2:\n\t"
        "push %%ecx\n\t"
        "call _EfConvert\n\t"
        "add $4, %%esp\n\t"
        "xor %%eax, %%eax\n\t"
        "1:\n\t"
        "ret"
        : : "m"(g_walker));
}

/* replaces the emit-by-shape call (stdcall, 7 args): eax = position output, ecx = shape */
__attribute__((naked)) static void EmitStub(void)
{
    __asm__ volatile(
        "cmpl $0, (%%ecx)\n\t"           /* a converted emitter needs the template's spawn shape list */
        "jne 3f\n\t"
        "mov 4(%%esp), %%edx\n\t"
        "test %%edx, %%edx\n\t"
        "je 4f\n\t"
        "cmpl $0, (%%edx)\n\t"
        "jne 3f\n\t"
        "4:\n\t"
        "xor %%eax, %%eax\n\t"
        "ret $0x1c\n\t"
        "3:\n\t"
        "push %%ecx\n\t"
        "push %%eax\n\t"
        "pushl 0x24(%%esp)\n\t" "pushl 0x24(%%esp)\n\t" "pushl 0x24(%%esp)\n\t" "pushl 0x24(%%esp)\n\t"
        "pushl 0x24(%%esp)\n\t" "pushl 0x24(%%esp)\n\t" "pushl 0x24(%%esp)\n\t"
        "mov 0x1c(%%esp), %%eax\n\t"
        "mov 0x20(%%esp), %%ecx\n\t"
        "call *%0\n\t"                     /* pops its 7 args */
        "push %%eax\n\t"
        "pushl 8(%%esp)\n\t"               /* shape */
        "pushl 8(%%esp)\n\t"               /* pos   */
        "push %%eax\n\t"                   /* n     */
        "call _EfSpread\n\t"
        "add $12, %%esp\n\t"
        "pop %%eax\n\t"
        "add $8, %%esp\n\t"
        "ret $0x1c"
        : : "m"(g_emit));
}

void EmitFixLog(void)
{
    LONG n = InterlockedExchange(&g_converted, 0), b = InterlockedExchange(&g_boxes, 0);
    if (n) Log("emitters: %ld mesh emitter(s) without surface, %ld turned into volume emitters", n, b);
    LONG m = InterlockedExchange(&g_areaMeshes, 0), fp = InterlockedExchange(&g_areaFixedPos, 0);
    if (m) Log("emitters: emission surfaces built for %ld submeshes (%ld with an offset position)", m, fp);
}

void EmitFixInit(void)
{
    if (!IniInt("Graphics", "EmitterFix", 1)) return;
    if (!Expect(EF_SETNE, "0F 95 C2 D9 1C 24 50 51") || !Expect(EF_CALL, "E8") || !Expect(EF_ECALL, "E8") ||
        (DWORD)(EF_CALL + 5 + *(LONG *)(EF_CALL + 1)) != EF_WALKER ||
        (DWORD)(EF_ECALL + 5 + *(LONG *)(EF_ECALL + 1)) != EF_EMIT) { Log("emitters: code mismatch, fix off"); return; }
    if (Expect(EF_AREACALL, "E8") && (DWORD)(EF_AREACALL + 5 + *(LONG *)(EF_AREACALL + 1)) == EF_IDXCOPY &&
        Expect(EF_AREARET, "5F 5E 5D 5B 83 C4 4C C2 08 00") && Expect(EF_AREACALL - 0x2A, "80 7C 24 60 00 74 28 8B 54 24 18"))
        WriteCall(EF_AREACALL, (void *)AreaStub, 5);
    else Log("emitters: submesh loader mismatch, emission surfaces not built");
    static const BYTE inc[3] = {0xB2, 0x01, 0x90};          /* mov dl,1 ; nop */
    WriteMem(EF_SETNE, inc, 3);
    WriteCall(EF_CALL, (void *)CountStub, 5);
    WriteCall(EF_ECALL, (void *)EmitStub, 5);
    Log("emitters: fire on bushes, trees and wrecks (mesh emitters without surface become volume emitters)");
}

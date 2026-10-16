/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* m2r:no-retarget
 * Sun shadows of far "modeled" trees, as on Xbox 360. Far trees are billboards; tree types that ship a low-poly
 * model (+0x6CC, loaded by the PC like on Xbox: un-indexed VB at +0x6B4/+0x6B8, material at +0x330) cast their
 * sun shadow with that model on Xbox (PgBillboardTreeShadowVP_modeled, one instanced draw per type). The PC shadow
 * pass (1.0 0x4A1AF0) lost that branch and draws the flat billboard card for every type.
 * 1) Detour at the material bind of that loop: a modeled type binds its model material and is drawn for layer 0
 *    only (Xbox draws it once); the draw {start, count, type} is noted.
 * 2) At the D3D side the noted card draw (PgBillboardTreeShadowVP_3, triangle strip, 6 vertices per tree) becomes
 *    one instanced draw: stream 0 = model vertices (INDEXEDDATA|trees), stream 1 = the billboard buffer, one
 *    record per tree (stride 96 = 6 x 16, INSTANCEDATA), with the Xbox shader math (rotation by the tree's random w).
 * 3) The type's trees in the fade zone between billboards and full trees (PC: no shadow at all) get the same
 *    instanced model draw from the fade buffer (stride 0x14, 6 vertices per tree), as the Xbox (FUN_821EF8F0).
 * If the D3D side cannot do it, the draw is dropped (the card would carry the model texture).
 * [Graphics] ModeledTreeShadows. */
#define COBJMACROS
#include "m2r.h"
#include <d3d9.h>
#include "d3dhook.h"
#include "treeshadow_shader.h"

#ifdef M2R_GAME11
#define TS_AT   0x4A1CF9u     /* lea ecx,[edi+0x10]; mov [cur],eax; call bind */
#define TS_BIND 0x854740u
#define TS_BACK 0x4A1D06u
#define TS_LOOP 0x4A1D42u     /* cmp eax,[edi+0x6B0] (next layer) */
#define TS_CUR  0x1163714u
#define TS_TYPES 0x1174A30u    /* tree type array (0x710 each) */
#define TS_NTYPES 0xE8F734u
#define TS_FADEVB 0xE8F714u    /* fade-zone billboard buffer: [0] view index, views of 0x18 at +4 {desc, VB, decl} */
#define TS_FADESTART 0xE8F878u /* per type: first vertex / vertex count (6 per tree) */
#define TS_FADECOUNT 0xE8F918u
#else
#define TS_AT   0x4A1C99u
#define TS_BIND 0x8544D0u
#define TS_BACK 0x4A1CA6u
#define TS_LOOP 0x4A1CE2u
#define TS_CUR  0x1164714u
#define TS_TYPES 0x1175A30u
#define TS_NTYPES 0xE90734u
#define TS_FADEVB 0xE90714u
#define TS_FADESTART 0xE90878u
#define TS_FADECOUNT 0xE90918u
#endif
#define TS_VP_LEN 1724u
#define TS_VP_CRC 0xD5FC1DD7u     /* PgBillboardTreeShadowVP_3 */

void *VtSwapPublic(void *obj, int idx, void *hook);
typedef HRESULT (STDMETHODCALLTYPE *CreateVS_t)(IDirect3DDevice9 *, const DWORD *, IDirect3DVertexShader9 **);
typedef HRESULT (STDMETHODCALLTYPE *SetVS_t)(IDirect3DDevice9 *, IDirect3DVertexShader9 *);
typedef HRESULT (STDMETHODCALLTYPE *DP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
static CreateVS_t o_CreateVS; static SetVS_t o_SetVS; static DP_t o_DP;
static int g_on;

/* ------------------------------------------------------------------ game side */
typedef struct { volatile DWORD start, count; BYTE *volatile obj; IDirect3DVertexBuffer9 *volatile fvb; volatile DWORD fstart, fcount; } TsRec;
#define NREC 64
static TsRec g_rec[NREC];
static volatile LONG g_recN;
static DWORD g_tsCur = TS_CUR, g_tsBind = TS_BIND, g_tsBack = TS_BACK, g_tsLoop = TS_LOOP;
static volatile int g_tsOk;
static volatile LONG g_types, g_noted;

static int TypeOk(BYTE *o)
{
    if (IsBadReadPtr(o + 0x6B4, 0x18) || IsBadReadPtr(o + 0x700, 8)) return 0;
    BYTE *desc = *(BYTE **)(o + 0x6B4);
    if (!desc || IsBadReadPtr(desc, 0x10) || !*(void **)(o + 0x6B8)) return 0;
    DWORD stride = *(DWORD *)(desc + 8), v = *(DWORD *)(desc + 0xC);
    return stride == 20 && v >= 3 && v % 3 == 0 && v <= 30000;
}
/* modeled type at layer `layer`: g_tsOk = 0 draw the card as usual, 1 draw layer 0 with the model material
   (noted for the D3D side), 2 skip this layer (the model draw covers the type) */
void __cdecl TsCheck(BYTE *o, DWORD layer)
{
    g_tsOk = 0;
    if (!TypeOk(o)) return;
    DWORD st = *(DWORD *)(o + 0x700), n = *(DWORD *)(o + 0x704);
    if (!n || n % 6 || st % 6) return;
    if (layer) { g_tsOk = 2; return; }
    LONG i = InterlockedIncrement(&g_recN) - 1;
    TsRec *r = &g_rec[i & (NREC - 1)];
    r->obj = NULL; r->start = st; r->count = n;
    /* the trees of this type in the near/far fade zone (Xbox draws their model shadow too, FUN_821EF8F0) */
    r->fvb = NULL; r->fstart = r->fcount = 0;
    BYTE *types = *(BYTE **)TS_TYPES; DWORD *fb = *(DWORD **)TS_FADEVB;
    if (types && o >= types && (DWORD)(o - types) % 0x710 == 0 && (DWORD)(o - types) / 0x710 < 40 && fb && !IsBadReadPtr(fb, 4 + 2 * 0x18)) {
        DWORD ti = (DWORD)(o - types) / 0x710, vi = fb[0];
        DWORD fs = ((DWORD *)TS_FADESTART)[ti], fc = ((DWORD *)TS_FADECOUNT)[ti];
        if (vi < 2) {
            DWORD *view = fb + 1 + vi * 6; BYTE *desc = (BYTE *)view[0];
            if (desc && !IsBadReadPtr(desc, 0x10) && *(DWORD *)(desc + 8) == 0x14 && view[1] && fc && fc % 6 == 0 && fs % 6 == 0 &&
                fs + fc <= *(DWORD *)(desc + 0xC)) { r->fvb = (IDirect3DVertexBuffer9 *)view[1]; r->fstart = fs; r->fcount = fc; }
        }
    }
    r->obj = o;
    InterlockedIncrement(&g_noted);
    g_tsOk = 1;
}
__attribute__((naked)) static void TsStub(void)
{
    __asm__ volatile(
        "mov %0, %%ecx\n\t"
        "mov %%eax, (%%ecx)\n\t"            /* original: mov [cur], eax */
        "lea 0x10(%%edi), %%ecx\n\t"        /* billboard material */
        "cmpb $0, 0x6CC(%%edi)\n\t"
        "je 1f\n\t"
        "pushal\n\t"
        "pushl 0x2C(%%esp)\n\t"             /* layer index (loop [esp+0xC]) */
        "push %%edi\n\t"
        "call _TsCheck\n\t"
        "add $8, %%esp\n\t"
        "popal\n\t"
        "cmpl $2, %4\n\t"
        "je 2f\n\t"
        "cmpl $1, %4\n\t"
        "jne 1f\n\t"
        "lea 0x330(%%edi), %%ecx\n\t"       /* model material (Xbox +0x2F0) */
        "1:\n\t"
        "call *%1\n\t"
        "jmp *%2\n\t"
        "2:\n\t"
        "mov 0xC(%%esp), %%eax\n\t"
        "inc %%eax\n\t"
        "jmp *%3"
        : : "m"(g_tsCur), "m"(g_tsBind), "m"(g_tsBack), "m"(g_tsLoop), "m"(g_tsOk));
}

/* ------------------------------------------------------------------ D3D side */
static IDirect3DVertexShader9 *g_bbVS[4]; static int g_nbbVS;
static IDirect3DVertexShader9 *g_curVS, *g_instVS;
static IDirect3DVertexDeclaration9 *g_decl;
static IDirect3DIndexBuffer9 *g_ib; static UINT g_ibN;
static LONG g_drawn, g_dropped, g_fadeDrawn;

static DWORD Crc32(const BYTE *p, DWORD n)
{
    DWORD c = 0xFFFFFFFF;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1)); }
    return ~c;
}
static DWORD CodeLen(const DWORD *c)
{
    DWORD i = 1;
    for (int g = 0; g < 100000; g++) {
        DWORD t = c[i];
        if (t == 0x0000FFFF) return (i + 1) * 4;
        if ((t & 0xFFFF) == 0xFFFE) i += 1 + (t >> 16);
        else i += 1 + ((t >> 24) & 0xF);
    }
    return 0;
}
static HRESULT STDMETHODCALLTYPE hk_CreateVS(IDirect3DDevice9 *d, const DWORD *c, IDirect3DVertexShader9 **o)
{
    HRESULT hr = o_CreateVS(d, c, o);
    if (SUCCEEDED(hr) && o && *o) {
        int bb = c && !IsBadReadPtr(c, 8) && CodeLen(c) == TS_VP_LEN && Crc32((const BYTE *)c, TS_VP_LEN) == TS_VP_CRC;
        for (int i = 0; i < g_nbbVS; i++) if (g_bbVS[i] == *o) g_bbVS[i] = NULL;   /* address of a freed shader reused */
        if (bb) {
            int i = 0;
            while (i < g_nbbVS && g_bbVS[i]) i++;
            if (i < 4) { g_bbVS[i] = *o; if (i == g_nbbVS) g_nbbVS++; }
        }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_SetVS(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) { g_curVS = s; return o_SetVS(d, s); }

static int Setup(IDirect3DDevice9 *d, UINT v)
{
    if (!g_instVS && FAILED(IDirect3DDevice9_CreateVertexShader(d, (const DWORD *)g_treeShadowModeledVS, &g_instVS))) { g_instVS = NULL; return 0; }
    if (!g_decl) {
        static const D3DVERTEXELEMENT9 e[] = {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
            {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            D3DDECL_END()};
        if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(d, e, &g_decl))) { g_decl = NULL; return 0; }
    }
    if (g_ibN < v) {
        UINT n = v < 1024 ? 1024 : v;
        IDirect3DIndexBuffer9 *ib = NULL; void *p;
        if (FAILED(IDirect3DDevice9_CreateIndexBuffer(d, n * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL))) return 0;
        if (FAILED(IDirect3DIndexBuffer9_Lock(ib, 0, 0, &p, 0))) { IDirect3DIndexBuffer9_Release(ib); return 0; }
        for (UINT i = 0; i < n; i++) ((WORD *)p)[i] = (WORD)i;
        IDirect3DIndexBuffer9_Unlock(ib);
        if (g_ib) IDirect3DIndexBuffer9_Release(g_ib);
        g_ib = ib; g_ibN = n;
    }
    return 1;
}
static int IsBbVS(IDirect3DVertexShader9 *s) { for (int i = 0; i < g_nbbVS; i++) if (s && g_bbVS[i] == s) return 1; return 0; }

static HRESULT STDMETHODCALLTYPE hk_DP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE pt, UINT sv, UINT pc)
{
    if (pt != D3DPT_TRIANGLESTRIP || !g_curVS || !IsBbVS(g_curVS)) return o_DP(d, pt, sv, pc);
    UINT cnt = pc + 2; BYTE *obj = NULL; IDirect3DVertexBuffer9 *fvb = NULL; UINT fs = 0, fc = 0;
    LONG n = g_recN;
    for (LONG k = 0; k < NREC && k < n; k++) {                 /* newest first */
        TsRec *r = &g_rec[(n - 1 - k) & (NREC - 1)];
        BYTE *o = r->obj;
        if (o && r->start == sv && r->count == cnt) { obj = o; fvb = r->fvb; fs = r->fstart; fc = r->fcount; break; }
    }
    if (!obj) return o_DP(d, pt, sv, pc);                       /* a billboard (card) shadow */
    {   /* the type must still exist (the forest is unloaded on travel) and the bound shader must really be ours:
           g_curVS misses state block applies, so the device is asked (and that reference is what is restored) */
        BYTE *types = *(BYTE **)TS_TYPES; DWORD nt = *(DWORD *)TS_NTYPES;
        if (!types || obj < types || obj >= types + nt * 0x710 || (DWORD)(obj - types) % 0x710 || !TypeOk(obj)) return o_DP(d, pt, sv, pc);
    }
    IDirect3DVertexShader9 *vs = NULL;
    if (FAILED(IDirect3DDevice9_GetVertexShader(d, &vs)) || !vs) return o_DP(d, pt, sv, pc);
    if (!IsBbVS(vs)) { IDirect3DVertexShader9_Release(vs); return o_DP(d, pt, sv, pc); }
    BYTE *desc = *(BYTE **)(obj + 0x6B4);
    UINT V = *(DWORD *)(desc + 0xC), N = cnt / 6;
    IDirect3DVertexBuffer9 *mvb = *(IDirect3DVertexBuffer9 **)(obj + 0x6B8);
    IDirect3DVertexBuffer9 *bb = NULL; UINT off = 0, stride = 0;
    if (V > 65535 || !Setup(d, V) || FAILED(IDirect3DDevice9_GetStreamSource(d, 0, &bb, &off, &stride)) || !bb || stride != 16) {
        if (bb) IDirect3DVertexBuffer9_Release(bb);
        IDirect3DVertexShader9_Release(vs);
        if (!g_dropped++) Log("tree shadows: modeled draw dropped (stride %u, V %u)", stride, V);
        return D3D_OK;
    }
    IDirect3DVertexDeclaration9 *decl = NULL; IDirect3DIndexBuffer9 *ib = NULL;
    IDirect3DVertexBuffer9 *s1 = NULL; UINT off1 = 0, st1 = 0, f0 = 1, f1 = 1;
    IDirect3DDevice9_GetVertexDeclaration(d, &decl);
    IDirect3DDevice9_GetIndices(d, &ib);
    IDirect3DDevice9_GetStreamSource(d, 1, &s1, &off1, &st1);
    IDirect3DDevice9_GetStreamSourceFreq(d, 0, &f0);
    IDirect3DDevice9_GetStreamSourceFreq(d, 1, &f1);

    IDirect3DDevice9_SetVertexDeclaration(d, g_decl);
    IDirect3DDevice9_SetVertexShader(d, g_instVS);
    IDirect3DDevice9_SetStreamSource(d, 0, mvb, 0, 20);
    IDirect3DDevice9_SetStreamSourceFreq(d, 0, D3DSTREAMSOURCE_INDEXEDDATA | N);
    IDirect3DDevice9_SetStreamSource(d, 1, bb, off + sv * 16, 96);
    IDirect3DDevice9_SetStreamSourceFreq(d, 1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
    IDirect3DDevice9_SetIndices(d, g_ib);
    HRESULT hr = IDirect3DDevice9_DrawIndexedPrimitive(d, D3DPT_TRIANGLELIST, 0, 0, V, 0, V / 3);
    if (SUCCEEDED(hr) && fvb && fc) {                           /* fade-zone trees: same model, their own records */
        IDirect3DDevice9_SetStreamSourceFreq(d, 0, D3DSTREAMSOURCE_INDEXEDDATA | (fc / 6));
        IDirect3DDevice9_SetStreamSource(d, 1, fvb, fs * 0x14, 6 * 0x14);
        if (SUCCEEDED(IDirect3DDevice9_DrawIndexedPrimitive(d, D3DPT_TRIANGLELIST, 0, 0, V, 0, V / 3))) g_fadeDrawn++;
    }
    IDirect3DDevice9_SetStreamSourceFreq(d, 0, f0);
    IDirect3DDevice9_SetStreamSourceFreq(d, 1, f1);
    IDirect3DDevice9_SetStreamSource(d, 1, s1, off1, st1);
    IDirect3DDevice9_SetStreamSource(d, 0, bb, off, stride);
    IDirect3DDevice9_SetIndices(d, ib);
    IDirect3DDevice9_SetVertexShader(d, vs);
    IDirect3DVertexShader9_Release(vs);
    IDirect3DDevice9_SetVertexDeclaration(d, decl);
    if (decl) IDirect3DVertexDeclaration9_Release(decl);
    if (ib) IDirect3DIndexBuffer9_Release(ib);
    if (s1) IDirect3DVertexBuffer9_Release(s1);
    IDirect3DVertexBuffer9_Release(bb);
    if (SUCCEEDED(hr)) { if (!g_drawn++) Log("tree shadows: first modeled tree shadow drawn (%u trees x %u vertices)", N, V); }
    else if (!g_dropped++) Log("tree shadows: instanced draw failed (%08lX)", hr);
    return D3D_OK;
}

static void OnFrame(IDirect3DDevice9 *d);
void TreeShadowDevice(IDirect3DDevice9 *d)
{
    if (!g_on || o_DP) return;
    void *p;
    if ((p = VtSwapPublic(d, 91, (void *)hk_CreateVS))) o_CreateVS = (CreateVS_t)p;
    if ((p = VtSwapPublic(d, 92, (void *)hk_SetVS)))    o_SetVS = (SetVS_t)p;
    if ((p = VtSwapPublic(d, 81, (void *)hk_DP)))       o_DP = (DP_t)p;
    D3DAddFrameCallback(OnFrame);
}
static void OnFrame(IDirect3DDevice9 *d)
{
    static DWORD t0; static LONG last = -1;
    (void)d;
    DWORD t = GetTickCount();
    if (t - t0 < 30000) return;
    t0 = t;
    if (g_noted != last) { last = g_noted; Log("tree shadows: %ld modeled draws noted, %ld drawn (+%ld fade-zone), %ld dropped", g_noted, g_drawn, g_fadeDrawn, g_dropped); }
}
void TreeShadowInit(void)
{
    if (!IniInt("Graphics", "ModeledTreeShadows", 1)) return;
    /* A1 [cur-src] | 8D 4F 10 | A3 [cur] | E8 rel -> bind */
    if (!Expect(TS_AT - 5, "A1") || !Expect(TS_AT, "8D 4F 10 A3") || *(DWORD *)(TS_AT + 4) != TS_CUR || !Expect(TS_AT + 8, "E8") ||
        (DWORD)(TS_AT + 13 + *(LONG *)(TS_AT + 9)) != TS_BIND || !Expect(TS_LOOP, "3B 87 B0 06 00 00") || !Expect(TS_BACK, "0F B7 0D")) {
        Log("tree shadows: code mismatch, off"); return;
    }
    WriteJmp(TS_AT, (void *)TsStub, 13);
    g_on = 1;
    Log("tree shadows: modeled far trees cast their model shadow (Xbox)");
}

/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Vehicle deformation ([Deformation] in Mercenaries2Remastered.ini).
 *
 * Game side (game thread, hook at the start of the physics event dispatch 0x404930, run once per frame
 * after the Havok step):
 *   Every _CarPhysicsV2 object is in the hash map at 0x17BF8A0 (count +4, capacity +8, element size +C,
 *   block shift +E, block mask +10, keys +1C (-1 = empty), block pointers +20; values are object pointers,
 *   car objects have vtable 0xBA94A0). Object +4C/+50/+54: body wrappers (array / count / indirect flag),
 *   wrapper +38 = hkpRigidBody. Rigid body: +10 shape, +E0 rotation (3 columns), +110 position,
 *   +1A0 linear velocity. hkpShape::getAabb is vtable +1C (thiscall: transform, tolerance, aabb out).
 *   A sudden change of a car's velocity (over ~0.1 s, beyond what driving can do) is an impact: the dent
 *   goes on the side the impulse came from, on the chassis box, with depth and size from the speed change.
 *
 * Render side (D3D9 hooks):
 *   Vertex shaders with an objectData.LocalToWorld constant get a patched twin (vspatch.c) that displaces
 *   object-space positions by the car's dents. A draw whose LocalToWorld matches a dented car (same
 *   rotation, origin within a few metres) is drawn with the twin and the dents in free constants; the
 *   game's shader and constants are restored right after.
 */
#include "m2r.h"
#define COBJMACROS
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3dhook.h"
#include "vspatch.h"
void TyreCar(int key, BYTE *obj, BYTE *rb, double now);
void TyreFrame(double now);

#define ND      12          /* dents per car (shader loop) */
#define MAXCAR  256
#define HIST    24

void *VtSwapPublic(void *obj, int idx, void *hook);

typedef struct { float x, y, z; } V3;
static V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 mul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float len(V3 a) { return sqrtf(dot(a, a)); }

typedef struct { V3 c; float r; V3 d; float seed; } Dent;          /* body space */

typedef struct {
    int used, key; void *obj, *rb;
    DWORD seen;
    V3 pos, vel, col[3];                                /* rotation columns (world = col * local) */
    float ht[HIST]; V3 hv[HIST]; int hn, hi;
    double lastHit;
    int boxOk; V3 bmin, bmax;
    Dent dent[ND]; int nd;
} Car;

/* game-thread state */
static Car g_car[MAXCAR];
static DWORD g_frame;

/* snapshot for the render thread */
typedef struct { int key; V3 pos, vel, col[3]; int nd; Dent dent[ND]; V3 oEst; int oOk; V3 bmin, bmax; int box; } RCar;
static RCar g_snap[MAXCAR]; static int g_nsnap;
static CRITICAL_SECTION g_cs;
static volatile LONG g_snapGen;

static int g_on;
static float g_thresh, g_strength, g_maxDepth, g_vert;

static double Now(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

/* --------------------------------------------------------------------------------------- game side */
#define T_BASE 0x17BF8A0
#define CAR_VT 0xBA94A0

typedef void (__attribute__((thiscall)) *GetAabb_t)(void *shape, const float *xf, float tol, float *aabb);

static int SafeRead(const void *p, size_t n) { return p && !IsBadReadPtr(p, n); }

static void BodyBox(Car *c)
{
    static float xf[16] __attribute__((aligned(16)));
    static float ab[8] __attribute__((aligned(16)));
    void *shape = *(void **)((BYTE *)c->rb + 0x10);
    c->boxOk = -1;
    if (!SafeRead(shape, 0x20)) return;
    void **vt = *(void ***)shape;
    if (!SafeRead(vt, 0x20)) return;
    memset(xf, 0, sizeof xf); xf[0] = xf[5] = xf[10] = 1.f;
    ab[0] = ab[1] = ab[2] = 1e9f; ab[4] = ab[5] = ab[6] = -1e9f;
    ((GetAabb_t)vt[7])(shape, xf, 0.f, ab);
    V3 mn = v3(ab[0], ab[1], ab[2]), mx = v3(ab[4], ab[5], ab[6]);
    V3 s = sub(mx, mn);
    if (!(s.x > 0.3f && s.y > 0.3f && s.z > 0.3f && s.x < 40.f && s.y < 40.f && s.z < 40.f)) return;
    c->bmin = mn; c->bmax = mx; c->boxOk = 1;
}

static V3 ToLocal(const Car *c, V3 w) { return v3(dot(w, c->col[0]), dot(w, c->col[1]), dot(w, c->col[2])); }

static DWORD g_rng = 0x2545F491;
static float Rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng & 0xFFFFFF) / 16777216.f; }

/* one dent: merge with a close one or add (when full, merge into the nearest) */
static void PutDent(Car *c, V3 S, V3 D, float r)
{
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < c->nd; i++) { float d = len(sub(c->dent[i].c, S)); if (d < bd) { bd = d; best = i; } }
    if (best >= 0 && (bd < 0.5f * fmaxf(r, c->dent[best].r) || c->nd == ND)) {
        Dent *e = &c->dent[best];
        V3 nd = add(e->d, D); float nl = len(nd), cap = g_maxDepth;
        if (nl > cap) nd = mul(nd, cap / nl);
        float w = len(D) / (len(D) + len(e->d) + 1e-4f);
        e->c = add(mul(e->c, 1.f - w), mul(S, w));
        e->d = nd; e->r = fminf(fmaxf(e->r, r), e->r + 0.3f);
    } else {
        Dent *e = &c->dent[c->nd++]; e->c = S; e->d = D; e->r = r; e->seed = Rnd() * 17.f;
    }
}

static void AddDent(Car *c, V3 dl, float mag)
{
    if (c->boxOk == 0) BodyBox(c);
    if (c->boxOk != 1) return;
    V3 ctr = mul(add(c->bmin, c->bmax), 0.5f), h = mul(sub(c->bmax, c->bmin), 0.5f);
    float l = len(dl); if (l < 1e-4f) return;
    V3 in = mul(dl, 1.f / l);                               /* impulse direction = inward at the hit side */
    V3 u = mul(in, -1.f);                                   /* from the centre towards the hit side */
    float t = 1e9f;
    if (fabsf(u.x) > 1e-4f) t = fminf(t, h.x / fabsf(u.x));
    if (fabsf(u.y) > 1e-4f) t = fminf(t, h.y / fabsf(u.y));
    if (fabsf(u.z) > 1e-4f) t = fminf(t, h.z / fabsf(u.z));
    V3 S = add(ctr, mul(u, t));
    /* contact height: bumpers / doors rather than the box middle, unless it's a roof or underside hit */
    if (fabsf(u.y) < 0.6f) S.y = c->bmin.y + (c->bmax.y - c->bmin.y) * (0.35f + 0.2f * Rnd());
    float hmax = fmaxf(h.x, fmaxf(h.y, h.z));
    float size = fminf(fmaxf(hmax / 2.2f, 0.7f), 1.4f);
    float e = (mag - g_thresh) * g_strength;
    float depth = fminf(0.06f + e * 0.028f, g_maxDepth);
    if (fabsf(u.x) > fabsf(u.z) && fabsf(u.x) > fabsf(u.y)) depth *= 0.75f;  /* doors: the occupants sit right behind */
    float r = fminf(fmaxf(0.5f + 0.045f * mag, 0.55f), 1.3f) * size;
    /* the impact is never perfectly flat: main dent a bit off the line, plus smaller ones around it */
    V3 side = fabsf(u.y) < 0.9f ? v3(-u.z, 0.f, u.x) : v3(1.f, 0.f, 0.f);
    float sl = len(side); side = mul(side, 1.f / sl);
    V3 up = v3(u.y * side.z - u.z * side.y, u.z * side.x - u.x * side.z, u.x * side.y - u.y * side.x);
    V3 in2 = add(in, add(mul(side, (Rnd() - 0.5f) * 0.4f), mul(up, (Rnd() - 0.5f) * 0.2f)));
    in2 = mul(in2, 1.f / len(in2));
    PutDent(c, add(S, mul(side, (Rnd() - 0.5f) * 0.3f * r)), mul(in2, depth), r);
    int nsat = e > 4.f ? (e > 12.f ? 3 : 2) : (e > 1.5f ? 1 : 0);
    for (int k = 0; k < nsat; k++) {
        float a = Rnd() * 6.2832f, d = (0.45f + 0.35f * Rnd()) * r;
        V3 o = add(mul(side, cosf(a) * d), mul(up, sinf(a) * d * 0.6f));
        PutDent(c, add(S, o), mul(in2, depth * (0.35f + 0.35f * Rnd())), r * (0.4f + 0.25f * Rnd()));
    }
}

static Car *FindCar(int key, void *obj)
{
    Car *fr = NULL;
    for (int i = 0; i < MAXCAR; i++) {
        if (g_car[i].used && g_car[i].key == key && g_car[i].obj == obj) return &g_car[i];
        if (!g_car[i].used && !fr) fr = &g_car[i];
    }
    if (fr) { memset(fr, 0, sizeof *fr); fr->used = 1; fr->key = key; fr->obj = obj; }
    return fr;
}

static void UpdateCar(Car *c, double now)
{
    BYTE *rb = (BYTE *)c->rb;
    c->pos = *(V3 *)(rb + 0x110);
    for (int i = 0; i < 3; i++) c->col[i] = *(V3 *)(rb + 0xE0 + 16 * i);
    c->vel = *(V3 *)(rb + 0x1A0);
    /* velocity history */
    c->hi = (c->hi + 1) % HIST; c->ht[c->hi] = (float)now; c->hv[c->hi] = c->vel; if (c->hn < HIST) c->hn++;
    /* sample ~0.1 s old */
    int ref = -1;
    for (int k = 1; k < c->hn; k++) {
        int j = (c->hi - k + HIST) % HIST;
        if (now - c->ht[j] > 0.25) break;
        ref = j;
        if (now - c->ht[j] >= 0.1) break;
    }
    if (ref < 0 || now - c->lastHit < 0.25) return;
    V3 dv = sub(c->vel, c->hv[ref]);
    V3 dl = ToLocal(c, dv);
    if (dl.y > 0.f) dl.y *= g_vert;                         /* landings: the underside, not seen */
    float mag = len(dl);
    if (mag < g_thresh) return;
    c->lastHit = now; c->hn = 1;
    AddDent(c, dl, mag);
}

/* motorcycles: the car's raycast vehicle (vtable 0xBAA360 at +0x158, see post.c) has 2 wheels (count +0x208) */
static int IsBike(BYTE *obj)
{
    BYTE *v = *(BYTE **)(obj + 0x158);
    if (!v || ((DWORD)v & 3) || IsBadReadPtr(v, 0x20C) || *(DWORD *)v != 0xBAA360) return 0;
    if (*(int *)(v + 0x208) != 2) return 0;
    static int logged; if (!logged) { logged = 1; Log("deform: motorcycles keep their shape"); }
    return 1;
}
/* Physics objects in the table say what they are through vtable +0xE0 (FUN_005D0760, Object.GetPhysicsType):
 * 0 prop, 1 human, 2 car, 3 tank, 4 helicopter, 5 terrain, 6 building, 7 usable, 8 jet, 9 winch, 10 boat, ...
 * Cars, helicopters, jets and boats dent (all share the body wrapper / rigid body layout); tanks don't. */
static int Dentable(BYTE *obj)
{
    void **vt = *(void ***)obj;
    if ((DWORD)vt < 0xB00000 || (DWORD)vt > 0xD00000) return 0;
    const BYTE *f = (const BYTE *)vt[0xE0 / 4];                 /* "mov eax, type; ret" in every physics class */
    if ((DWORD)f < 0x401000 || (DWORD)f > 0xB00000 || f[0] != 0xB8 || f[5] != 0xC3) return 0;
    int t = *(const int *)(f + 1);
    return t == 2 || t == 4 || t == 8 || t == 10;
}
static void ScanCars(void)
{
    static double last;
    double now = Now();
    if (now - last < 0.004) return;                         /* called more than once per frame */
    last = now;
    g_frame++;
    TyreFrame(now);
    BYTE *T = (BYTE *)T_BASE;
    int cap = *(int *)(T + 8), *keys = *(int **)(T + 0x1C);
    BYTE **blocks = *(BYTE ***)(T + 0x20);
    int esz = *(short *)(T + 0xC), sh = *(BYTE *)(T + 0xE), mask = *(int *)(T + 0x10);
    if (cap > 0 && keys && blocks) {
        for (int i = 0; i < cap; i++) {
            if (keys[i] == -1) continue;
            BYTE *blk = blocks[i >> sh];
            if (!blk) continue;
            BYTE *obj = *(BYTE **)(blk + ((mask - 1) & i) * esz);
            if (!obj || !Dentable(obj)) continue;
            if (*(int *)(obj + 0x50) < 1) continue;
            BYTE *w = *(BYTE **)(obj + 0x4C);
            if (*(int *)(obj + 0x54)) w = SafeRead(w, 4) ? *(BYTE **)w : NULL;
            BYTE *rb = SafeRead(w, 0x3C) ? *(BYTE **)(w + 0x38) : NULL;
            if (!SafeRead(rb, 0x1B0)) continue;
            /* every kind's body is a Havok rigid body: the same class as the cars' */
            static DWORD rbVt;
            if (*(DWORD *)obj == CAR_VT && !rbVt) rbVt = *(DWORD *)rb;
            if (*(DWORD *)obj != CAR_VT && (!rbVt || *(DWORD *)rb != rbVt)) continue;
            if (*(DWORD *)obj == CAR_VT) TyreCar(keys[i], obj, rb, now);
            if (!g_on || (*(DWORD *)obj == CAR_VT && IsBike(obj))) continue;
            Car *c = FindCar(keys[i], obj);
            if (!c) continue;
            if (c->rb != rb) { c->rb = rb; c->boxOk = 0; c->hn = 0; }
            c->seen = g_frame;
            UpdateCar(c, now);
        }
    }
    int n = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MAXCAR; i++) {
        Car *c = &g_car[i];
        if (!c->used) continue;
        if (g_frame - c->seen > 120) { c->used = 0; continue; }
        if (!c->nd) continue;
        RCar *r = &g_snap[n];
        if (r->key != c->key) { r->oOk = 0; }
        r->key = c->key; r->pos = c->pos; r->vel = c->vel;
        memcpy(r->col, c->col, sizeof r->col); r->nd = c->nd; memcpy(r->dent, c->dent, sizeof r->dent);
        r->box = c->boxOk == 1; r->bmin = c->bmin; r->bmax = c->bmax;
        n++;
    }
    g_nsnap = n;
    InterlockedIncrement(&g_snapGen);
    LeaveCriticalSection(&g_cs);
}

static void *g_scanPtr = (void *)ScanCars;
__attribute__((naked)) static void DispatchStub(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "call *%0\n\t"
        "popfl\n\t"
        "popal\n\t"
        "sub $0xE0, %%esp\n\t"
        "push $0x404936\n\t"
        "ret\n\t" :: "m"(g_scanPtr));
}

/* ------------------------------------------------------------------------------------- render side */
typedef HRESULT (STDMETHODCALLTYPE *CreateVS_t)(IDirect3DDevice9 *, const DWORD *, IDirect3DVertexShader9 **);
typedef HRESULT (STDMETHODCALLTYPE *SetVS_t)(IDirect3DDevice9 *, IDirect3DVertexShader9 *);
typedef HRESULT (STDMETHODCALLTYPE *SetVSCF_t)(IDirect3DDevice9 *, UINT, const float *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *SetVSCI_t)(IDirect3DDevice9 *, UINT, const int *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *CreatePS_t)(IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **);
typedef HRESULT (STDMETHODCALLTYPE *SetPS_t)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
typedef HRESULT (STDMETHODCALLTYPE *SetRS_t)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
static CreatePS_t o_CreatePS; static SetPS_t o_SetPS; static SetRS_t o_SetRS;
static CreateVS_t o_CreateVS; static SetVS_t o_SetVS; static SetVSCF_t o_SetF; static SetVSCI_t o_SetI;
static DrawIP_t o_DrawIP; static DrawP_t o_DrawP;

typedef struct {
    IDirect3DVertexShader9 *vs;
    DWORD *code;                    /* original bytecode */
    IDirect3DVertexShader9 *twin[2];/* patched shaders: [0] dents, [1] glass (no bending, breakage out) */
    int state[2];                   /* 0 = not tried, 1 = twin ok, -1 = can't patch */
    VsPatchInfo pi[2];
    int vcReg;                      /* viewContextData (ViewProj) register, -1 = none */
} VsEnt;
typedef struct {
    IDirect3DPixelShader9 *ps;
    DWORD *code;
    IDirect3DPixelShader9 *twin;    /* broken-glass pixel shader */
    int state, sem;
} PsEnt;
#define PSCAP 8192
static PsEnt g_psTab[PSCAP];
static PsEnt *g_curPs;
static DWORD g_blend;               /* D3DRS_ALPHABLENDENABLE */
#define VSCAP 8192
static VsEnt g_vsTab[VSCAP];
static VsEnt *g_cur;
static float g_cf[256][4];
static int g_ci[16][4];
static RCar g_rc[MAXCAR]; static int g_nrc; static LONG g_rcGen = -1;
static int g_patchOk;
static float g_lag;
static int g_rfr, g_minNd = ND;
/* render-side anchor per car: where this frame's (or the last frame's) car body draw put the car origin */
typedef struct { int key, fr; V3 p; double t; } Anchor;
static Anchor g_anc[MAXCAR];
static Anchor *AnchorFor(int key)
{
    Anchor *fr = NULL;
    for (int i = 0; i < MAXCAR; i++) { if (g_anc[i].key == key) return &g_anc[i]; if (!fr && (!g_anc[i].key || g_rfr - g_anc[i].fr > 60)) fr = &g_anc[i]; }
    if (!fr) fr = &g_anc[(DWORD)key % MAXCAR];
    memset(fr, 0, sizeof *fr); fr->key = key; fr->fr = -1000;
    return fr;
}

static VsEnt *VsFind(IDirect3DVertexShader9 *vs, int add)
{
    DWORD h = ((DWORD)(UINT_PTR)vs >> 4) * 2654435761u;
    for (int k = 0; k < VSCAP; k++) {
        VsEnt *e = &g_vsTab[(h + k) & (VSCAP - 1)];
        if (e->vs == vs) return e;
        if (!e->vs) { if (!add) return NULL; e->vs = vs; return e; }
    }
    return NULL;
}

static PsEnt *PsFind(IDirect3DPixelShader9 *ps, int add)
{
    DWORD h = ((DWORD)(UINT_PTR)ps >> 4) * 2654435761u;
    for (int k = 0; k < PSCAP; k++) {
        PsEnt *e = &g_psTab[(h + k) & (PSCAP - 1)];
        if (e->ps == ps) return e;
        if (!e->ps) { if (!add) return NULL; e->ps = ps; return e; }
    }
    return NULL;
}

static int CtabReg(const DWORD *code, const char *want)
{
    if ((code[1] & 0xFFFF) != 0xFFFE || code[2] != 0x42415443) return -1;
    const BYTE *b = (const BYTE *)(code + 3); UINT n = (((code[1] >> 16) & 0x7FFF) - 1) * 4;
    DWORD nc = *(const DWORD *)(b + 12), off = *(const DWORD *)(b + 16);
    for (DWORD k = 0; k < nc && off + 20 * (k + 1) <= n; k++) {
        const BYTE *e = b + off + 20 * k; DWORD nm = *(const DWORD *)e;
        if (nm < n && !strcmp((const char *)b + nm, want) && *(const WORD *)(e + 4) == 2) return *(const WORD *)(e + 6);
    }
    return -1;
}
extern float g_postVP[4][4]; extern volatile int g_postInScene;
void PostVoteVP(const float *m);

static int HasObjectData(const DWORD *code)
{
    if ((code[1] & 0xFFFF) != 0xFFFE || code[2] != 0x42415443) return 0;
    UINT n = (((code[1] >> 16) & 0x7FFF) - 1) * 4;
    const BYTE *b = (const BYTE *)(code + 3);
    for (UINT i = 0; i + 11 <= n; i++) if (!memcmp(b + i, "objectData", 11)) return 1;
    return 0;
}

static HRESULT STDMETHODCALLTYPE hk_CreateVS(IDirect3DDevice9 *d, const DWORD *code, IDirect3DVertexShader9 **out)
{
    HRESULT hr = o_CreateVS(d, code, out);
    if (SUCCEEDED(hr) && out && *out && !(code && (code[0] & 0xFFFF0000) == 0xFFFE0000 && HasObjectData(code))) {
        VsEnt *e = VsFind(*out, 0);                 /* a shader we do not patch at the address of an old one */
        if (e) { for (int k = 0; k < 2; k++) { if (e->twin[k]) IDirect3DVertexShader9_Release(e->twin[k]); e->twin[k] = NULL; } e->state[0] = e->state[1] = -1; e->vcReg = -1; }
    }
    if (SUCCEEDED(hr) && out && *out && code && (code[0] & 0xFFFF0000) == 0xFFFE0000 && HasObjectData(code)) {
        VsEnt *e = VsFind(*out, 1);
        if (e) {
            for (int k = 0; k < 2; k++) { if (e->twin[k]) IDirect3DVertexShader9_Release(e->twin[k]); e->twin[k] = NULL; }
            free(e->code);
            UINT n = VsCodeSize(code);
            e->code = n ? (DWORD *)malloc(n) : NULL;
            if (e->code) memcpy(e->code, code, n);
            e->state[0] = e->state[1] = e->code ? 0 : -1;
            e->vcReg = CtabReg(code, "viewContextData");
            if (!g_on) e->state[0] = e->state[1] = -1;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreatePS(IDirect3DDevice9 *d, const DWORD *code, IDirect3DPixelShader9 **out)
{
    HRESULT hr = o_CreatePS(d, code, out);
    if (SUCCEEDED(hr) && out && *out && !(code && code[0] == 0xFFFF0300)) {
        PsEnt *e = PsFind(*out, 0);                 /* a shader we do not patch at the address of an old one */
        if (e) { if (e->twin) IDirect3DPixelShader9_Release(e->twin); e->twin = NULL; e->state = -1; }
    }
    if (SUCCEEDED(hr) && out && *out && code && code[0] == 0xFFFF0300) {
        PsEnt *e = PsFind(*out, 1);
        if (e) {
            if (e->twin) IDirect3DPixelShader9_Release(e->twin);
            free(e->code);
            UINT n = VsCodeSize(code);
            e->code = n ? (DWORD *)malloc(n) : NULL;
            if (e->code) memcpy(e->code, code, n);
            e->twin = NULL; e->state = e->code ? 0 : -1;
        }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_SetPS(IDirect3DDevice9 *d, IDirect3DPixelShader9 *ps)
{
    g_curPs = ps ? PsFind(ps, 0) : NULL;
    return o_SetPS(d, ps);
}
static HRESULT STDMETHODCALLTYPE hk_SetRS(IDirect3DDevice9 *d, D3DRENDERSTATETYPE st, DWORD v)
{
    if (st == D3DRS_ALPHABLENDENABLE) g_blend = v;
    return o_SetRS(d, st, v);
}

static HRESULT STDMETHODCALLTYPE hk_SetVS(IDirect3DDevice9 *d, IDirect3DVertexShader9 *vs)
{
    g_cur = vs ? VsFind(vs, 0) : NULL;
    return o_SetVS(d, vs);
}
static HRESULT STDMETHODCALLTYPE hk_SetF(IDirect3DDevice9 *d, UINT r, const float *v, UINT n)
{
    if (v && r < 256) memcpy(g_cf[r], v, 16 * (r + n > 256 ? 256 - r : n));
    return o_SetF(d, r, v, n);
}
static HRESULT STDMETHODCALLTYPE hk_SetI(IDirect3DDevice9 *d, UINT r, const int *v, UINT n)
{
    if (v && r < 16) memcpy(g_ci[r], v, 16 * (r + n > 16 ? 16 - r : n));
    return o_SetI(d, r, v, n);
}

static void PullSnapshot(void)
{
    LONG g = g_snapGen;
    if (g == g_rcGen) return;
    EnterCriticalSection(&g_cs);
    /* keep the render-side model offset estimate per car */
    RCar old[MAXCAR]; int nold = g_nrc; memcpy(old, g_rc, sizeof(RCar) * nold);
    memcpy(g_rc, g_snap, sizeof(RCar) * g_nsnap); g_nrc = g_nsnap;
    g_rcGen = g_snapGen;
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < g_nrc; i++) {
        g_rc[i].oOk = 0;
        for (int j = 0; j < nold; j++) if (old[j].key == g_rc[i].key) { g_rc[i].oEst = old[j].oEst; g_rc[i].oOk = old[j].oOk; break; }
    }
}

/* draw with the deformation twin when the current object is a dented car; 1 = drawn */
static int Deformed(IDirect3DDevice9 *d, int indexed, D3DPRIMITIVETYPE pt, INT bv, UINT mi, UINT nv, UINT si, UINT pc)
{
    VsEnt *e = g_cur;
    int glass = g_blend != 0;
    if (!e || e->state[glass] < 0) return 0;
    PullSnapshot();
    if (!g_nrc) return 0;
    if (e->state[glass] == 0) {
        DWORD *pc2 = NULL; char why[256];
        e->state[glass] = -1;
        if (g_patchOk && VsPatch(e->code, ND, glass, &e->pi[glass], &pc2, why, sizeof why)) {
            if (SUCCEEDED(o_CreateVS(d, pc2, &e->twin[glass]))) { e->state[glass] = 1; if (e->pi[glass].nd < g_minNd) g_minNd = e->pi[glass].nd; }
        }
        free(pc2);
        if (e->state[glass] < 0) return 0;
    }
    PsEnt *pe = NULL;
    if (glass) {                                                    /* glass breaks instead of bending */
        pe = g_curPs;
        if (!pe || pe->state < 0) return 0;
        if (pe->state == 0 || pe->sem != e->pi[1].gsem) {
            DWORD *pc2 = NULL; char why[256];
            if (pe->twin) { IDirect3DPixelShader9_Release(pe->twin); pe->twin = NULL; }
            pe->state = -1; pe->sem = e->pi[1].gsem;
            if (PsPatch(pe->code, pe->sem, &pc2, why, sizeof why) && SUCCEEDED(o_CreatePS(d, pc2, &pe->twin))) pe->state = 1;
            free(pc2);
            if (pe->state < 0) return 0;
        }
    }
    VsPatchInfo *pi = &e->pi[glass];
    int o = pi->objReg;
    const float *W0 = g_cf[o], *W1 = g_cf[o + 1], *W2 = g_cf[o + 2];
    V3 t = v3(W0[3], W1[3], W2[3]);
    RCar *best = NULL, *rot = NULL; float bd = 1e9f, rd = 1e9f;
    for (int i = 0; i < g_nrc; i++) {
        RCar *c = &g_rc[i];
        V3 dp = sub(t, c->pos);
        float dist = len(dp);
        if (dist > 4.f + len(c->vel) * 0.05f || dist >= bd) continue;
        /* rotation: W row i = (col0[i], col1[i], col2[i]) */
        const float *W[3] = {W0, W1, W2}; int ok = 1;
        for (int r = 0; r < 3 && ok; r++) {
            float rx = ((float *)&c->col[0])[r], ry = ((float *)&c->col[1])[r], rz = ((float *)&c->col[2])[r];
            ok = fabsf(W[r][0] - rx) < 0.25f && fabsf(W[r][1] - ry) < 0.25f && fabsf(W[r][2] - rz) < 0.25f;
        }
        if (ok) { best = c; bd = dist; }
        else if (dist < rd && c->box) { rot = c; rd = dist; }
    }
    float A[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   /* part axes in car space (rows), / |axis|^2 */
    int rotated = 0;
    if (!best) {
        /* a part with its own orientation (hinged hood or boot lid, parts modelled turned): origin inside the
         * car's box -> dents turned into the part's space */
        if (!rot || pi->skinned) return 0;
        /* hinged: turned about the car's side axis only (characters in or next to the car turn about up) */
        V3 px = v3(W0[0], W1[0], W2[0]); float lx = len(px);
        if (lx < 0.5f || fabsf(dot(px, rot->col[0])) / lx < 0.995f) return 0;
        best = rot; rotated = 1;
    }
    /* The renderer draws the car where it was a moment ago (interpolation / a frame behind the physics):
     * the offset of the car body's own draws runs along the velocity. Learn that lag, and treat draws whose
     * origin is not the car origin after it (driver, passengers, debris) as someone else's. The car model's
     * origin is the rigid body's (logged offset 0), so dents go to the shader in body space unchanged. */
    V3 e2 = sub(t, best->pos);
    float sp = len(best->vel), v2 = dot(best->vel, best->vel);
    Anchor *an = AnchorFor(best->key);
    int fresh = g_rfr - an->fr <= 2;
    if (sp > 3.f && !rotated) {
        float along = dot(e2, best->vel) / v2;
        V3 perp = sub(e2, mul(best->vel, along));
        if (len(perp) < 0.15f && fabsf(along) < 0.15f) g_lag = g_lag * 0.95f + along * 0.05f;
    }
    V3 el = sub(e2, mul(best->vel, g_lag));                       /* physics estimate */
    V3 ew = el;
    int body = 0;
    if (fresh) {
        /* the car body draw of this or the last frame: same origin -> body, otherwise a part by its offset */
        V3 ref = an->fr == g_rfr ? an->p : add(an->p, mul(best->vel, (float)(Now() - an->t)));
        ew = sub(t, ref);
        body = len(ew) < 0.06f + (an->fr == g_rfr ? 0.f : sp * 0.01f);
    }
    if (!body && len(el) < 0.25f + sp * 0.01f) body = 1;
    if (rotated) body = 0;
    float res = body ? 0.f : len(ew);
    if (body) { an->p = t; an->fr = g_rfr; an->t = Now(); }
    if (!body && !fresh) res = len(el) > 0.45f + sp * 0.015f ? len(el) : 0.f;   /* no anchor yet: as before */
    V3 ob = v3(0.f, 0.f, 0.f);
    if (rotated || res > 0.f) {
        /* a separate part of the car (bumpers, plates, doors that can come off): its own origin inside the
         * car's box -> dents shifted into its space */
        if (!best->box) return 0;
        V3 l = v3(dot(ew, best->col[0]), dot(ew, best->col[1]), dot(ew, best->col[2]));
        const float m = rotated ? 0.25f : 0.5f;
        if (l.x < best->bmin.x - m || l.y < best->bmin.y - m || l.z < best->bmin.z - m ||
            l.x > best->bmax.x + m || l.y > best->bmax.y + m || l.z > best->bmax.z + m) return 0;
        ob = l;
    }
    if (rotated) {
        for (int j = 0; j < 3; j++) {
            V3 aw = v3(W0[j], W1[j], W2[j]);                         /* part axis j in world */
            float s2 = dot(aw, aw);
            if (s2 < 0.25f || s2 > 4.f) return 0;
            A[j][0] = dot(aw, best->col[0]) / s2; A[j][1] = dot(aw, best->col[1]) / s2; A[j][2] = dot(aw, best->col[2]) / s2;
        }
    }
    float cst[2 * ND][4];
    int n = best->nd < g_minNd ? best->nd : g_minNd;      /* the same dents in every pass (depth, shadow, colour) */
    if (n > pi->nd) n = pi->nd;
    for (int i = 0; i < n; i++) {
        Dent *dn = &best->dent[i];
        V3 cm = sub(dn->c, ob), dd = dn->d;
        if (rotated) {
            V3 a0 = v3(A[0][0], A[0][1], A[0][2]), a1 = v3(A[1][0], A[1][1], A[1][2]), a2 = v3(A[2][0], A[2][1], A[2][2]);
            cm = v3(dot(cm, a0), dot(cm, a1), dot(cm, a2)); dd = v3(dot(dd, a0), dot(dd, a1), dot(dd, a2));
        }
        cst[2 * i][0] = cm.x; cst[2 * i][1] = cm.y; cst[2 * i][2] = cm.z; cst[2 * i][3] = 1.f / (dn->r * dn->r);
        cst[2 * i + 1][0] = dd.x; cst[2 * i + 1][1] = dd.y; cst[2 * i + 1][2] = dd.z; cst[2 * i + 1][3] = dn->seed;
    }
    int b = pi->base, ir = pi->ireg;
    int li[4] = {n, 0, 2, 0};
    o_SetF(d, b, &cst[0][0], 2 * n);
    o_SetI(d, ir, li, 1);
    o_SetVS(d, e->twin[glass]);
    if (pe) o_SetPS(d, pe->twin);
    HRESULT hr = indexed ? o_DrawIP(d, pt, bv, mi, nv, si, pc) : o_DrawP(d, pt, si, pc);
    (void)hr;
    o_SetVS(d, e->vs);
    if (pe) o_SetPS(d, pe->ps);
    o_SetF(d, b, g_cf[b], 2 * n);
    o_SetI(d, ir, g_ci[ir], 1);
    return 1;
}

static HRESULT STDMETHODCALLTYPE hk_DrawIP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE pt, INT bv, UINT mi, UINT nv, UINT si, UINT pc)
{
    if (g_postInScene && g_cur && g_cur->vcReg >= 0 && g_cur->vcReg < 252) PostVoteVP(g_cf[g_cur->vcReg]);
    if (g_nrc || g_snapGen != g_rcGen) if (Deformed(d, 1, pt, bv, mi, nv, si, pc)) return D3D_OK;
    return o_DrawIP(d, pt, bv, mi, nv, si, pc);
}
static HRESULT STDMETHODCALLTYPE hk_DrawP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE pt, UINT sv, UINT pc)
{
    if (g_nrc || g_snapGen != g_rcGen) if (Deformed(d, 0, pt, 0, 0, 0, sv, pc)) return D3D_OK;
    return o_DrawP(d, pt, sv, pc);
}

static void OnFrame(IDirect3DDevice9 *d) { g_rfr++; }

void DeformDevice(IDirect3DDevice9 *d)
{
    if (o_CreateVS) return;
    void *p;
    if ((p = VtSwapPublic(d, 91, (void *)hk_CreateVS))) o_CreateVS = (CreateVS_t)p;
    if ((p = VtSwapPublic(d, 92, (void *)hk_SetVS)))    o_SetVS = (SetVS_t)p;
    if ((p = VtSwapPublic(d, 94, (void *)hk_SetF)))     o_SetF = (SetVSCF_t)p;
    if ((p = VtSwapPublic(d, 96, (void *)hk_SetI)))     o_SetI = (SetVSCI_t)p;
    if ((p = VtSwapPublic(d, 82, (void *)hk_DrawIP)))   o_DrawIP = (DrawIP_t)p;
    if ((p = VtSwapPublic(d, 81, (void *)hk_DrawP)))    o_DrawP = (DrawP_t)p;
    if ((p = VtSwapPublic(d, 106, (void *)hk_CreatePS))) o_CreatePS = (CreatePS_t)p;
    if ((p = VtSwapPublic(d, 107, (void *)hk_SetPS)))   o_SetPS = (SetPS_t)p;
    if ((p = VtSwapPublic(d, 57, (void *)hk_SetRS)))    o_SetRS = (SetRS_t)p;
    g_patchOk = VsPatchInit();
    VsPatchSetCap(g_maxDepth);
    Log("deform: device hooks on%s", g_patchOk ? "" : " (d3dx9_36 assembler missing: off)");
}

void DeformInit(void)
{
    g_on = IniInt("Deformation", "Enabled", 1);
    g_strength = IniFloat("Deformation", "Strength", 1.0f);
    g_thresh = IniFloat("Deformation", "Threshold", 4.0f);
    g_maxDepth = IniFloat("Deformation", "MaxDepth", 0.35f);
    g_vert = 0.35f;
    if (g_thresh < 1.f) g_thresh = 1.f;
    int TyreEnabled(void);
    if (!g_on && !TyreEnabled()) return;
    if (!Expect(0x404930, "81 EC E0 00 00 00 53 55")) { Log("deform: unexpected code at 0x404930, off"); g_on = 0; return; }
    InitializeCriticalSection(&g_cs);
    WriteJmp(0x404930, (void *)DispatchStub, 6);
    D3DAddFrameCallback(OnFrame);
    Log("deform: on (threshold %.1f m/s, strength %.2f, max depth %.2f m)", g_thresh, g_strength, g_maxDepth);
}

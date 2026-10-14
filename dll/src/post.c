/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Tyre marks ([TyreMarks]).
 *
 * The car's raycast vehicle (vtable 0xBAA360) hangs at car object +0x158; wheels at +0x54 (count +0x208),
 * 0x130 bytes each. Skid quads are collected on the game thread (TyreCar, called from deform.c ScanCars)
 * and drawn into the HDR scene target (A16B16G16R16F, depth-tested against the scene depth) at the first
 * switch to the half-size A16B16G16R16 bloom target, i.e. when the lit scene is finished.
 * The camera ViewProj is the one most scene draws use this frame (viewContextData, voted in deform.c).
 */
#include "m2r.h"
#define COBJMACROS
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "d3dhook.h"

void *VtSwapPublic(void *obj, int idx, void *hook);

float g_postVP[4][4];
static IDirect3DSurface9 *g_curDS, *g_curRT;     /* bound depth buffer / render target */
/* targets the camera-voted scene draws went into this frame (the game uses several full-size HDR targets;
 * the scene is the one most of those draws go to), with the depth buffer bound for them */
#define NSC 4
static struct { IDirect3DSurface9 *rt, *ds; int n; } g_sc[NSC]; static int g_nsc;
static IDirect3DSurface9 *g_sceneRT, *g_sceneDS;
/* the scene camera: the ViewProj most scene draws use this frame (sky, reflections and the like differ) */
static float g_vpc[4][16]; static int g_vpn[4], g_nvp;
extern IDirect3DDevice9 *g_dev;
static void CurDS(void)
{
    /* the device's own depth buffer is bound at creation / Reset without a SetDepthStencilSurface call */
    IDirect3DSurface9 *s = NULL;
    if (!g_curDS && g_dev && SUCCEEDED(IDirect3DDevice9_GetDepthStencilSurface(g_dev, &s)) && s) { g_curDS = s; IDirect3DSurface9_Release(s); }
}
static volatile int g_postInDepth;
static IDirect3DSurface9 *g_lastHdr;
void PostVoteVP(const float *m)
{
    int i;
    if (!g_postInDepth) {
        /* scene colour pass: remember the target and depth buffer the marks are drawn into */
        CurDS();
        for (i = 0; i < g_nsc && (g_sc[i].rt != g_curRT || g_sc[i].ds != g_curDS); i++) ;
        if (i < g_nsc) g_sc[i].n++;
        else if (g_nsc < NSC) { g_sc[g_nsc].rt = g_curRT; g_sc[g_nsc].ds = g_curDS; g_sc[g_nsc].n = 1; g_nsc++; }
        return;
    }
    /* depth pre-pass: the camera the depth texture the marks are projected with was drawn from. The scene
       pass also draws with other cameras (the near layer: far plane 500 m) that do not match it. */
    for (i = 0; i < g_nvp; i++) if (!memcmp(g_vpc[i], m, 64)) { g_vpn[i]++; return; }
    if (g_nvp < 4) { memcpy(g_vpc[g_nvp], m, 64); g_vpn[g_nvp++] = 1; return; }
    int lo = 0; for (i = 1; i < 4; i++) if (g_vpn[i] < g_vpn[lo]) lo = i;
    if (g_vpn[lo] <= 1) { memcpy(g_vpc[lo], m, 64); g_vpn[lo] = 1; }
}
static int PickVP(void)
{
    int best = -1;
    for (int i = 0; i < g_nvp; i++) if (best < 0 || g_vpn[i] > g_vpn[best]) best = i;
    static int nl;
    if (nl < 5 && g_nvp > 1) { nl++; Log("post: %d cameras in the depth pre-pass, using the one with %d of %d+%d+%d+%d draws", g_nvp, g_vpn[best], g_vpn[0], g_vpn[1], g_nvp > 2 ? g_vpn[2] : 0, g_nvp > 3 ? g_vpn[3] : 0); }
    static int have;
    if (best >= 0) { memcpy(g_postVP, g_vpc[best], 64); have = 1; }
    g_nvp = 0;
    return have;                                   /* no depth pre-pass draw this frame: last frame's camera */
}
volatile int g_postInScene;

static int g_tyre;
static float g_tyreLife = 120.f;
static float g_slipMin = 3.5f;          /* sliding speed (m/s) a tyre needs to leave a mark */

/* ------------------------------------------------------------------------------------- tyre data */
#define MAXQ 6000
typedef struct { float p[4][3]; float a; float t; } Quad;
static Quad g_q[MAXQ]; static int g_qn, g_qhead;
static CRITICAL_SECTION g_qcs;
#define MAXCARW 128
typedef struct {
    int key; BYTE *veh; DWORD seen;
    float last[10][3]; double lt[10]; int have[10];
    float axisErr[2];                   /* which wheel axis the tread rolls along (+0x6C or +0x60) */
    float pfwd, dirAcc, negZ[3]; double pt;
} TCar;
static TCar g_tc[MAXCARW];
static DWORD g_tcFrame;
static double g_t0 = -1.0, g_now;       /* seconds since the first call (float-safe) */

int TyreEnabled(void) { return g_tyre; }

/* The car's raycast vehicle (Pandemic's own action, vtable 0xBAA360, 0x284 bytes) hangs at car object +0x158
 * (created by FUN_00437080, set up by FUN_00449460). Wheels: pointers at +0x54, count +0x208, 0x130 bytes each
 * (FUN_0044DF60 init, FUN_0044E600 suspension, FUN_0044F680 drive/brake):
 *   +0x0C angular velocity (= tread speed / radius)   +0x1C contact normal   +0x28 contact point
 *   +0x34 ray hits (>= 1 = on the ground)              +0x60 / +0x6C wheel axes in the ground plane (steered
 *   +0x90 tread speed, m/s (drive, brake and handbrake    with the wheel; rear wheels: chassis columns 2 / 0)
 *         act on it; locked wheel = 0)                 +0x10C radius   +0x120 front axle
 * A tyre leaves a mark where it slides over the ground: sideways (contact point velocity across the wheel) or
 * along (ground speed under the wheel differs from its tread speed: locked brakes, handbrake, wheelspin). */
#define VEH_VT 0xBAA360
static int Rd(const void *p, int n) { return p && !((DWORD)p & 3) && !IsBadReadPtr(p, n); }

static void AddQuad(const float a[3], const float b[3], const float side[3], float w, float alpha)
{
    Quad *q = &g_q[g_qhead];
    for (int k = 0; k < 3; k++) {
        q->p[0][k] = a[k] - side[k] * w; q->p[1][k] = a[k] + side[k] * w;
        q->p[2][k] = b[k] - side[k] * w; q->p[3][k] = b[k] + side[k] * w;
    }
    q->a = alpha; q->t = (float)g_now;
    g_qhead = (g_qhead + 1) % MAXQ; if (g_qn < MAXQ) g_qn++;
}

/* once per game frame, before the cars (deform.c ScanCars) */
void TyreFrame(double now)
{
    if (g_t0 < 0) g_t0 = now;
    g_now = now - g_t0;
    g_tcFrame++;
}

static TCar *CarSlot(int key, BYTE *veh)
{
    TCar *c = NULL, *fr = NULL, *old = NULL;
    for (int i = 0; i < MAXCARW; i++) {
        TCar *t = &g_tc[i];
        if (t->key == key && t->veh == veh) { c = t; break; }
        if (!t->key || g_tcFrame - t->seen > 30) { if (!fr) fr = t; }          /* free, or car gone */
        else if (t->seen != g_tcFrame && (!old || t->seen < old->seen)) old = t; /* least recently seen */
    }
    if (!c) {
        c = fr ? fr : old;
        if (!c) return NULL;                                /* every slot is a car seen this frame */
        memset(c, 0, sizeof *c); c->key = key; c->veh = veh;
    }
    c->seen = g_tcFrame;
    return c;
}

static float Dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
/* v projected onto the plane with normal n, normalised; 0 = degenerate */
static int PlaneDir(const float *v, const float *n, float *o)
{
    float d = Dot3(v, n);
    for (int k = 0; k < 3; k++) o[k] = v[k] - n[k] * d;
    float l = sqrtf(Dot3(o, o));
    if (l < 0.2f) return 0;
    for (int k = 0; k < 3; k++) o[k] /= l;
    return 1;
}

/* game thread, every car every frame (deform.c ScanCars) */
static int g_marksAdded, g_marksDrawn, g_postRuns, g_postNoScene;
void TyreCar(int key, BYTE *obj, BYTE *rb, double now)
{
    (void)now;
    if (!g_tyre) return;
    BYTE *v = Rd(obj + 0x158, 4) ? *(BYTE **)(obj + 0x158) : NULL;
    if (!Rd(v, 0x284) || *(DWORD *)v != VEH_VT) return;
    int n = *(int *)(v + 0x208);
    if (n < 2 || n > 10) return;
    BYTE *wh[10];
    for (int i = 0; i < n; i++) { wh[i] = *(BYTE **)(v + 0x54 + 4 * i); if (!Rd(wh[i], 0x130)) return; }
    TCar *c = CarSlot(key, v);
    if (!c) return;
    float *vel = (float *)(rb + 0x1A0), *av = (float *)(rb + 0x1B0), *cp = (float *)(rb + 0x110);

    /* 1) slip from the chassis motion (works for every car): sideways speed at the rear wheels (front wheels
     *    steer, they follow a sliding rear), hard braking / wheelspin from the change of forward speed */
    float *ax = (float *)(rb + 0xE0), *az = (float *)(rb + 0x100);
    {   /* forward = the chassis axis the wheels are spread along most (wheelbase > track) */
        float mnx = 1e9f, mxx = -1e9f, mnz = 1e9f, mxz = -1e9f;
        for (int i = 0; i < n; i++) {
            float *p = (float *)(wh[i] + 0x48), q[3] = {p[0] - cp[0], p[1] - cp[1], p[2] - cp[2]};
            float x = Dot3(q, ax), z = Dot3(q, az);
            if (x < mnx) mnx = x;
            if (x > mxx) mxx = x;
            if (z < mnz) mnz = z;
            if (z > mxz) mxz = z;
        }
        if (mxx - mnx > mxz - mnz) { float *t = ax; ax = az; az = t; }
    }
    float fwd = Dot3(vel, az);
    if (fabsf(fwd) > 3.f) c->dirAcc += fwd > 0.f ? 1.f : -1.f;            /* cars mostly drive forwards */
    if (c->dirAcc < 0.f) { for (int k = 0; k < 3; k++) c->negZ[k] = -az[k]; az = c->negZ; }
    float acc = 0.f;
    if (c->pt > 0 && g_now - c->pt > 0.001 && g_now - c->pt < 0.2) acc = (float)((fwd - c->pfwd) / (g_now - c->pt));
    c->pfwd = fwd; c->pt = g_now;
    float lon = fabsf(acc) > 7.5f && fabsf(fwd) > 4.f ? (fabsf(acc) - 7.5f) * 0.6f + 2.5f : 0.f;
    float side[10], pvs[10][3], rear = 0.f;
    for (int i = 0; i < n; i++) {
        float *p = (float *)(wh[i] + 0x28);
        float r[3] = {p[0] - cp[0], p[1] - cp[1], p[2] - cp[2]};
        float *pv = pvs[i];
        pv[0] = vel[0] + av[1] * r[2] - av[2] * r[1]; pv[1] = vel[1] + av[2] * r[0] - av[0] * r[2]; pv[2] = vel[2] + av[0] * r[1] - av[1] * r[0];
        side[i] = fabsf(Dot3(pv, ax));
        if (Dot3(r, az) < 0.f && side[i] > rear) rear = side[i];
    }
    float sp = sqrtf(Dot3(vel, vel));

    EnterCriticalSection(&g_qcs);
    for (int i = 0; i < n; i++) {
        BYTE *w = wh[i];
        float *pos = (float *)(w + 0x28), *nrm = (float *)(w + 0x1C);
        float r[3] = {pos[0] - cp[0], pos[1] - cp[1], pos[2] - cp[2]};
        int contact = *(int *)(w + 0x34) >= 1 && nrm[1] > 0.3f && Dot3(r, r) < 225.f;
        if (!contact) { c->have[i] = 0; continue; }
        int front = Dot3(r, az) > 0.f;
        float slip = front ? (rear > 4.f ? rear * 0.8f : 0.f) : side[i];
        if (lon > slip) slip = lon;

        /* 2) the wheel itself: its tread speed (+0x90) against the ground speed under it along the axis it rolls
         *    on (locked brakes, handbrake, wheelspin), once that axis is known from free rolling */
        float ax2[2][3];
        if (PlaneDir((float *)(w + 0x6C), nrm, ax2[0]) && PlaneDir((float *)(w + 0x60), nrm, ax2[1])) {
            float tread = fabsf(*(float *)(w + 0x90));
            float va = Dot3(pvs[i], ax2[0]), vb = Dot3(pvs[i], ax2[1]);
            if (tread > 2.f && tread < 150.f) {
                c->axisErr[0] = c->axisErr[0] * 0.98f + fabsf(fabsf(va) - tread) * 0.02f;
                c->axisErr[1] = c->axisErr[1] * 0.98f + fabsf(fabsf(vb) - tread) * 0.02f;
            }
            if (fabsf(c->axisErr[0] - c->axisErr[1]) >= 1.f && (c->axisErr[0] < 1.f || c->axisErr[1] < 1.f)) {
                float along = c->axisErr[1] < c->axisErr[0] ? vb : va;
                float lslip = fabsf(fabsf(along) - tread);
                if (lslip > slip) slip = lslip;
            }
        }
        if (!(sp > 2.f && slip > g_slipMin)) { c->have[i] = 0; continue; }
        float p[3] = {pos[0] + nrm[0] * 0.03f, pos[1] + nrm[1] * 0.03f, pos[2] + nrm[2] * 0.03f};
        if (c->have[i]) {
            float d[3] = {p[0] - c->last[i][0], p[1] - c->last[i][1], p[2] - c->last[i][2]};
            float l = sqrtf(Dot3(d, d));
            if (l < 0.25f) continue;                                    /* wait for some length */
            if (l < 3.f && g_now - c->lt[i] < 0.3) {
                float s[3] = {nrm[1] * d[2] - nrm[2] * d[1], nrm[2] * d[0] - nrm[0] * d[2], nrm[0] * d[1] - nrm[1] * d[0]};
                float sl = sqrtf(Dot3(s, s));
                if (sl > 1e-4f) {
                    for (int k = 0; k < 3; k++) s[k] /= sl;
                    float a = (slip - g_slipMin) / 5.f; if (a > 1.f) a = 1.f; a = 0.35f + 0.55f * a;
                    AddQuad(c->last[i], p, s, 0.13f, a);
                    g_marksAdded++;
                }
            }
        }
        memcpy(c->last[i], p, sizeof p); c->lt[i] = g_now; c->have[i] = 1;
    }
    LeaveCriticalSection(&g_qcs);
}

/* ------------------------------------------------------------------------------------- rendering */
typedef HRESULT (STDMETHODCALLTYPE *SRT_t)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *SDS_t)(IDirect3DDevice9 *, IDirect3DSurface9 *);
static SRT_t o_SRT; static SDS_t o_SDS;
typedef HRESULT (STDMETHODCALLTYPE *SR_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
static SR_t o_SR; static IDirect3DSurface9 *g_copySrc, *g_copyDst; static RECT g_copyRa, g_copyRb;
static UINT g_bw, g_bh;
static int g_done, g_ready;
static IDirect3DVertexShader9 *g_vsB; static IDirect3DPixelShader9 *g_psB;
static IDirect3DTexture9 *g_depthTex; static IDirect3DSurface9 *g_depthSurf; static int g_depthThisFrame;
static IDirect3DVertexDeclaration9 *g_declB;
static IDirect3DVertexBuffer9 *g_cubeVB, *g_instVB; static IDirect3DIndexBuffer9 *g_cubeIB;
static IDirect3DStateBlock9 *g_sb;

/* Marks are projected decals: each skid segment is a thin box around the stretch of ground the wheel slid over
 * (its centre, half-length, half-width and a +-0.35 m thickness along the ground normal), drawn as instanced
 * boxes. For every screen pixel the box covers, the scene point behind it is rebuilt from the depth pre-pass
 * (z/w of the scene camera in .r) and coloured if it lies inside the box on a surface that faces up the way the
 * ground does. The mark therefore lies on whatever surface was drawn there - the rendered terrain is a lower
 * detail mesh than the physics ground the wheels touch, centimetres above or below it - from any viewing
 * angle. Only the far side of each box is shaded (the face normal points away from the camera), so every
 * pixel is coloured once, also with the camera inside a box. */
static const char g_src[] =
    "float4 vp0 : register(c0); float4 vp1 : register(c1); float4 vp2 : register(c2); float4 vp3 : register(c3);\n"
    "float4 px : register(c4);   /* 1/w, 1/h */\n"
    "float4 pr : register(c5);   /* a, b: w = b / (z/w - a) */\n"
    "float4 cam : register(c6);  /* camera position */\n"
    "float4 ivp[4] : register(c8); /* inverse ViewProj (rows) */\n"
    "sampler2D sDepth : register(s0);\n"
    "struct I { float4 w : TEXCOORD0; float4 c : TEXCOORD1; float4 u : TEXCOORD2; float4 v : TEXCOORD3; float4 n : TEXCOORD4; float face : TEXCOORD5; };\n"
    "struct V { float4 o : POSITION; I i; };\n"
    "V VSB(float4 p : POSITION, float4 f : NORMAL, float4 c : TEXCOORD1, float4 u : TEXCOORD2, float4 v : TEXCOORD3, float4 n : TEXCOORD4) {\n"
    "  V r; float3 W = c.xyz + u.xyz * p.x + v.xyz * p.y + n.xyz * p.z;\n"
    "  float4 P = float4(W, 1); r.o = float4(dot(P, vp0), dot(P, vp1), dot(P, vp2), dot(P, vp3));\n"
    "  r.i.w = float4(W, r.o.w); r.i.c = c;\n"
    "  r.i.u = float4(u.xyz / dot(u.xyz, u.xyz), u.w); r.i.v = float4(v.xyz / dot(v.xyz, v.xyz), v.w); r.i.n = float4(n.xyz / dot(n.xyz, n.xyz), n.w);\n"
    "  float3 fw = u.xyz * f.x + v.xyz * f.y + n.xyz * f.z;\n"
    "  r.i.face = dot(fw, W - cam.xyz); return r; }\n"
    "float4 PSB(I i, float2 vpos : VPOS) : COLOR {\n"
    "  clip(i.face);\n"
    "  float d = tex2Dlod(sDepth, float4((vpos + 0.5) * px.xy, 0, 0)).r;\n"
    "  clip(0.99999 - d);                                                 /* sky: nothing drawn there */\n"
    "  float2 ndc = float2(vpos.x * px.x * 2 - 1, 1 - vpos.y * px.y * 2);   /* D3D9: pixel centres on integer screen positions */\n"
    "  float4 H = float4(ndc, d, 1);\n"
    "  float4 P4 = float4(dot(ivp[0], H), dot(ivp[1], H), dot(ivp[2], H), dot(ivp[3], H));\n"
    "  float3 P = P4.xyz / P4.w;\n"
    "  float3 q = P - i.c.xyz;\n"
    "  float s = dot(q, i.u.xyz), t = dot(q, i.v.xyz), h = dot(q, i.n.xyz);\n"
    "  clip(1 - abs(s)); clip(1 - abs(t)); clip(1 - abs(h));\n"
    "  float3 sn = normalize(cross(ddx(P), ddy(P)));\n"
    "  float up = abs(dot(sn, normalize(i.n.xyz)));\n"
    "  float edge = saturate((1 - abs(t)) * 3.5);                         /* soft sides */\n"
    "  float ends = saturate((1 - abs(s)) * 10.5);                        /* cross-fade where segments overlap */\n"
    "  float2 w2 = abs(frac(t * float2(3.4, 1.3) + float2(0.2, 0.6)) - 0.5) * 4 - 1;   /* rubber streaks along the slide */\n"
    "  float tread = 0.72 + 0.28 * w2.x * w2.y;\n"
    "  float3 g3 = frac(floor(P.xzx * 30.0) * 0.1031); g3 += dot(g3, g3.yzx + 33.33);\n"
    "  float grain = frac((g3.x + g3.y) * g3.z);\n"
    "  float a = i.c.w * saturate((up - 0.55) * 4) * edge * ends * tread * (0.8 + 0.2 * grain);\n"
    "  return float4(lerp(1.0.xxx, float3(0.20, 0.19, 0.18), a * 0.75), 1); }\n";

typedef struct XBuf XBuf;
typedef struct { void *q, *a; ULONG (WINAPI *Release)(XBuf *); void *(WINAPI *Ptr)(XBuf *); } XBufVt;
struct XBuf { const XBufVt *vt; };
typedef HRESULT (WINAPI *CS_t)(LPCSTR, UINT, const void *, void *, LPCSTR, LPCSTR, DWORD, XBuf **, XBuf **, void **);

static int Comp(IDirect3DDevice9 *d, CS_t cs, const char *ep, const char *prof, void **out)
{
    XBuf *code = NULL, *err = NULL;
    HRESULT hr = cs(g_src, (UINT)sizeof g_src - 1, NULL, NULL, ep, prof, 0, &code, &err, NULL);
    if (FAILED(hr)) { Log("tyres: %s: %.300s", ep, err ? (const char *)err->vt->Ptr(err) : "?"); if (err) err->vt->Release(err); return 0; }
    if (err) err->vt->Release(err);
    hr = prof[0] == 'v' ? IDirect3DDevice9_CreateVertexShader(d, (const DWORD *)code->vt->Ptr(code), (IDirect3DVertexShader9 **)out)
                        : IDirect3DDevice9_CreatePixelShader(d, (const DWORD *)code->vt->Ptr(code), (IDirect3DPixelShader9 **)out);
    code->vt->Release(code);
    return SUCCEEDED(hr);
}

static int Init(IDirect3DDevice9 *d)
{
    HMODULE x = GetModuleHandleA("d3dx9_36.dll"); if (!x) x = LoadLibraryA("d3dx9_36.dll");
    CS_t cs = x ? (CS_t)GetProcAddress(x, "D3DXCompileShader") : NULL;
    if (!cs) return 0;
    if (!g_vsB && !Comp(d, cs, "VSB", "vs_3_0", (void **)&g_vsB)) return 0;
    if (!g_psB && !Comp(d, cs, "PSB", "ps_3_0", (void **)&g_psB)) return 0;
    static const D3DVERTEXELEMENT9 eb[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        {1, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
        {1, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3},
        {1, 48, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4}, D3DDECL_END()};
    if (!g_declB && FAILED(IDirect3DDevice9_CreateVertexDeclaration(d, eb, &g_declB))) return 0;
    if (!g_cubeVB) {
        /* 6 faces x 4 corners: position in [-1,1]^3 and the face's outward normal */
        if (FAILED(IDirect3DDevice9_CreateVertexBuffer(d, 24 * 32, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g_cubeVB, NULL))) return 0;
        float *v;
        if (FAILED(IDirect3DVertexBuffer9_Lock(g_cubeVB, 0, 0, (void **)&v, 0))) return 0;
        for (int f = 0; f < 6; f++) {
            int ax = f >> 1; float sg = (f & 1) ? 1.f : -1.f;
            int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
            for (int k = 0; k < 4; k++) {
                float *o = v + (f * 4 + k) * 8;
                o[0] = o[1] = o[2] = 0; o[3] = 1; o[4] = o[5] = o[6] = 0; o[7] = 0;
                o[ax] = sg; o[a1] = (k & 1) ? 1.f : -1.f; o[a2] = (k & 2) ? 1.f : -1.f;
                o[4 + ax] = sg;
            }
        }
        IDirect3DVertexBuffer9_Unlock(g_cubeVB);
    }
    if (!g_cubeIB) {
        if (FAILED(IDirect3DDevice9_CreateIndexBuffer(d, 36 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_cubeIB, NULL))) return 0;
        WORD *ix;
        if (FAILED(IDirect3DIndexBuffer9_Lock(g_cubeIB, 0, 0, (void **)&ix, 0))) return 0;
        for (int f = 0; f < 6; f++) {
            WORD b0 = (WORD)(f * 4);
            WORD t[6] = {b0, (WORD)(b0 + 1), (WORD)(b0 + 2), (WORD)(b0 + 2), (WORD)(b0 + 1), (WORD)(b0 + 3)};
            memcpy(ix + f * 6, t, sizeof t);
        }
        IDirect3DIndexBuffer9_Unlock(g_cubeIB);
    }
    if (!g_instVB && FAILED(IDirect3DDevice9_CreateVertexBuffer(d, MAXQ * 64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_instVB, NULL))) return 0;
    if (!g_sb && FAILED(IDirect3DDevice9_CreateStateBlock(d, D3DSBT_ALL, &g_sb))) return 0;
    Log("tyres: shaders ready");
    return 1;
}

void PostReset(void)
{
    if (g_sb) { IDirect3DStateBlock9_Release(g_sb); g_sb = NULL; }
    if (g_instVB) { IDirect3DVertexBuffer9_Release(g_instVB); g_instVB = NULL; }
    if (g_depthTex) { IDirect3DTexture9_Release(g_depthTex); g_depthTex = NULL; }
    g_depthSurf = NULL;
    g_sceneRT = g_sceneDS = g_curDS = g_curRT = NULL; g_nsc = 0; g_ready = 0;
}

/* camera position: the point where clip x, y and w are 0 (rows 0, 1, 3 of ViewProj) */
static int CamPos(float *o)
{
    const float *r0 = g_postVP[0], *r1 = g_postVP[1], *r3 = g_postVP[3];
    double m[3][3] = {{r0[0], r0[1], r0[2]}, {r1[0], r1[1], r1[2]}, {r3[0], r3[1], r3[2]}}, b[3] = {-r0[3], -r1[3], -r3[3]};
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (fabs(det) < 1e-12) return 0;
    for (int c = 0; c < 3; c++) {
        double t[3][3]; memcpy(t, m, sizeof t);
        for (int r = 0; r < 3; r++) t[r][c] = b[r];
        double dc = t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) + t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
        o[c] = (float)(dc / det);
    }
    return 1;
}

/* inverse of a 4x4 matrix (double precision), 0 = singular */
static int Inv4(const float m[4][4], float o[4][4])
{
    double a[4][8];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r][c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-15) return 0;
        if (p != c) for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        double iv = 1.0 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= iv;
        for (int r = 0; r < 4; r++) if (r != c) { double f = a[r][c]; for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[r][c] = (float)a[r][c + 4];
    return 1;
}

static void DrawMarks(IDirect3DDevice9 *d)
{
    if (!g_depthTex || !g_depthThisFrame) { g_marksDrawn = 0; return; }
    float cam[4] = {0, 0, 0, 1};
    if (!CamPos(cam)) { return; }
    float ivp[4][4];
    if (!Inv4(g_postVP, ivp)) { return; }
    /* z/w = a + b / w along a view ray */
    float *r2 = g_postVP[2], *r3 = g_postVP[3];
    float l3 = r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2];
    if (l3 < 1e-12f) return;
    float a = (r2[0] * r3[0] + r2[1] * r3[1] + r2[2] * r3[2]) / l3, bb = r2[3] - a * r3[3];
    if (fabsf(bb) < 1e-9f) return;
    float *inst;
    if (FAILED(IDirect3DVertexBuffer9_Lock(g_instVB, 0, 0, (void **)&inst, D3DLOCK_DISCARD))) return;
    int n = 0; float now = (float)g_now;
    EnterCriticalSection(&g_qcs);
    for (int i = 0; i < g_qn; i++) {
        Quad *q = &g_q[(g_qhead - 1 - i + MAXQ) % MAXQ];
        float age = now - q->t;
        if (age > g_tyreLife) break;
        float c[3], u[3], v[3];
        for (int k = 0; k < 3; k++) {
            c[k] = (q->p[0][k] + q->p[1][k] + q->p[2][k] + q->p[3][k]) * 0.25f;
            u[k] = ((q->p[2][k] + q->p[3][k]) - (q->p[0][k] + q->p[1][k])) * 0.25f;
            v[k] = (q->p[1][k] - q->p[0][k]) * 0.5f;
        }
        float dx = c[0] - cam[0], dy = c[1] - cam[1], dz = c[2] - cam[2];
        if (dx * dx + dy * dy + dz * dz > 250.f * 250.f) continue;
        float nn[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        float nl = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        if (nl < 1e-6f) continue;
        float *o = inst + n * 16;
        o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = q->a * (1.f - age / g_tyreLife);
        o[4] = u[0] * 1.05f; o[5] = u[1] * 1.05f; o[6] = u[2] * 1.05f; o[7] = 0;   /* segments overlap a little */
        o[8] = v[0]; o[9] = v[1]; o[10] = v[2]; o[11] = 0;
        o[12] = nn[0] / nl * 0.35f; o[13] = nn[1] / nl * 0.35f; o[14] = nn[2] / nl * 0.35f; o[15] = 0;
        n++;
    }
    LeaveCriticalSection(&g_qcs);
    IDirect3DVertexBuffer9_Unlock(g_instVB);
    g_marksDrawn = n;
    if (!n) return;
   
    float c[4][4] = {{1.f / g_bw, 1.f / g_bh, 0, 0}, {a, bb, 0, 0}, {cam[0], cam[1], cam[2], 1}, {0, 0, 0, 0}};
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetTexture(d, 0, (IDirect3DBaseTexture9 *)g_depthTex);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT); IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_SRGBTEXTURE, 0);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SRCBLEND, D3DBLEND_ZERO);
    IDirect3DDevice9_SetRenderState(d, D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    IDirect3DDevice9_SetRenderState(d, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    IDirect3DDevice9_SetVertexDeclaration(d, g_declB);
    IDirect3DDevice9_SetVertexShader(d, g_vsB); IDirect3DDevice9_SetPixelShader(d, g_psB);
    IDirect3DDevice9_SetVertexShaderConstantF(d, 0, &g_postVP[0][0], 4);
    IDirect3DDevice9_SetVertexShaderConstantF(d, 6, c[2], 1);
    IDirect3DDevice9_SetPixelShaderConstantF(d, 4, c[0], 4);
    IDirect3DDevice9_SetPixelShaderConstantF(d, 8, &ivp[0][0], 4);
    IDirect3DDevice9_SetStreamSource(d, 0, g_cubeVB, 0, 32);
    IDirect3DDevice9_SetStreamSource(d, 1, g_instVB, 0, 64);
    IDirect3DDevice9_SetStreamSourceFreq(d, 0, D3DSTREAMSOURCE_INDEXEDDATA | (UINT)n);
    IDirect3DDevice9_SetStreamSourceFreq(d, 1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
    IDirect3DDevice9_SetIndices(d, g_cubeIB);
    IDirect3DDevice9_DrawIndexedPrimitive(d, D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);
    IDirect3DDevice9_SetStreamSourceFreq(d, 0, 1); IDirect3DDevice9_SetStreamSourceFreq(d, 1, 1);
    IDirect3DDevice9_SetTexture(d, 0, NULL);
}

static void RunPost(IDirect3DDevice9 *d)
{
    int b = -1;
    for (int i = 0; i < g_nsc; i++) if (b < 0 || g_sc[i].n > g_sc[b].n) b = i;
    g_nsc = 0;
    g_postRuns++;
    if (b < 0 || !g_sc[b].rt || !g_sc[b].ds) { g_postNoScene++; return; }
    g_sceneRT = g_sc[b].rt; g_sceneDS = g_sc[b].ds;
    /* the lit scene ends up in the last full-size HDR target drawn before the bloom chain starts: with some
       effects on screen (water refraction, heat shimmer) the game copies the scene into a second target and
       finishes it there, and marks drawn into the first one never reach the screen */
    if (g_lastHdr && g_lastHdr != g_sceneRT) { g_sceneRT = g_lastHdr; }
    if (!g_ready) g_ready = Init(d) ? 1 : -1;
    if (g_ready < 0 || !PickVP()) { return; }
    { extern volatile float g_vpCam[3]; float c[3]; if (CamPos(c)) { g_vpCam[0] = c[0]; g_vpCam[1] = c[1]; g_vpCam[2] = c[2]; } }
    IDirect3DStateBlock9_Capture(g_sb);
    IDirect3DDevice9_SetRenderState(d, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE, 7);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_CLIPPLANEENABLE, 0);
    D3DVIEWPORT9 vpt = {0, 0, g_bw, g_bh, 0.f, 1.f};
    o_SRT(d, 0, g_sceneRT); o_SDS(d, g_sceneDS);
    for (int i = 1; i < 4; i++) o_SRT(d, i, NULL);
    IDirect3DDevice9_SetViewport(d, &vpt);
    UINT f0 = 1, f1 = 1;
    IDirect3DDevice9_GetStreamSourceFreq(d, 0, &f0); IDirect3DDevice9_GetStreamSourceFreq(d, 1, &f1);
    DrawMarks(d);
    /* the copy the rest of the frame reads was taken before the marks: take it again */
    if (g_copyDst && g_marksDrawn) o_SR(d, g_copySrc, &g_copyRa, g_copyDst, &g_copyRb, D3DTEXF_NONE);
    IDirect3DDevice9_SetStreamSourceFreq(d, 0, f0); IDirect3DDevice9_SetStreamSourceFreq(d, 1, f1);
    IDirect3DStateBlock9_Apply(g_sb);
    o_SDS(d, g_curDS);
    o_SRT(d, 0, g_curRT);
}


static HRESULT STDMETHODCALLTYPE hk_SR(IDirect3DDevice9 *d, IDirect3DSurface9 *a, const RECT *ra, IDirect3DSurface9 *b, const RECT *rb, D3DTEXTUREFILTERTYPE f)
{
    /* the game copies the finished HDR scene into a second target, and the bloom chain and the final composite
       read that copy: remember the last full-frame copy of an HDR scene target before the bloom chain */
    if (!g_done && a && b) {
        D3DSURFACE_DESC da, db;
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(a, &da)) && SUCCEEDED(IDirect3DSurface9_GetDesc(b, &db)) &&
            da.Format == D3DFMT_A16B16G16R16F && db.Format == da.Format && da.Width == g_bw && da.Height == g_bh &&
            db.Width == g_bw && db.Height == g_bh) { g_copySrc = a; g_copyDst = b; g_copyRa = ra ? *ra : (RECT){0, 0, (LONG)g_bw, (LONG)g_bh}; g_copyRb = rb ? *rb : g_copyRa; }
    }
    return o_SR(d, a, ra, b, rb, f);
}

static HRESULT STDMETHODCALLTYPE hk_SDS(IDirect3DDevice9 *d, IDirect3DSurface9 *s)
{
    g_curDS = s;
    return o_SDS(d, s);
}
static HRESULT STDMETHODCALLTYPE hk_SRT(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s)
{
    if (i == 0 && s) {
        D3DSURFACE_DESC sd;
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(s, &sd))) {
            g_postInScene = sd.Width == g_bw && sd.Height == g_bh && sd.Format == D3DFMT_A16B16G16R16F;
            if (g_postInScene && !g_done) g_lastHdr = s;
            g_postInDepth = sd.Width == g_bw && sd.Height == g_bh && sd.Format == D3DFMT_G32R32F;
            if (g_postInDepth) g_postInScene = 1;
            if (sd.Width == g_bw && sd.Height == g_bh && sd.Format == D3DFMT_G32R32F && (sd.Usage & D3DUSAGE_RENDERTARGET)) {
                /* the depth pre-pass target: the first full-size G32R32F target of the frame */
                if (!g_depthThisFrame && s != g_depthSurf) {
                    IDirect3DTexture9 *t = NULL;
                    if (SUCCEEDED(IDirect3DSurface9_GetContainer(s, &IID_IDirect3DTexture9, (void **)&t)) && t) {
                        if (g_depthTex) IDirect3DTexture9_Release(g_depthTex);
                        g_depthTex = t; g_depthSurf = s;
                    }
                }
                g_depthThisFrame = 1;
            }
            if (!g_done && g_nsc && sd.Format == D3DFMT_A16B16G16R16 && sd.Width == g_bw / 2) {
                g_done = 1; g_postInScene = 0; g_postInDepth = 0;
                RunPost(d);
            }
        }
        g_curRT = s;
    }
    return o_SRT(d, i, s);
}

static void BackBufferSize(IDirect3DDevice9 *d)
{
    IDirect3DSurface9 *bb = NULL;
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        D3DSURFACE_DESC bd; IDirect3DSurface9_GetDesc(bb, &bd); g_bw = bd.Width; g_bh = bd.Height;
        IDirect3DSurface9_Release(bb);
    }
}
static void OnFrame(IDirect3DDevice9 *d)
{
    static DWORD t0; static int lastAdded = -1;
    DWORD t = GetTickCount();
    if (t - t0 > 30000) {                       /* status every 30 s while something changes */
        t0 = t;
        { void DrawPoolStatus(void); DrawPoolStatus(); }
        if (g_marksAdded != lastAdded) {
            lastAdded = g_marksAdded;
            Log("tyres: %d mark segments made, %d on screen, scene found in %d of %d frames", g_marksAdded, g_marksDrawn, g_postRuns - g_postNoScene, g_postRuns);
        }
    }
   
    g_done = 0; g_nvp = 0; g_nsc = 0; g_depthThisFrame = 0; g_lastHdr = NULL; g_copySrc = g_copyDst = NULL;
    BackBufferSize(d);
}

void PostDevice(IDirect3DDevice9 *d)
{
    if (!g_tyre) return;
    void *p;
    if ((p = VtSwapPublic(d, 37, (void *)hk_SRT))) o_SRT = (SRT_t)p;
    if ((p = VtSwapPublic(d, 39, (void *)hk_SDS))) o_SDS = (SDS_t)p;
    if ((p = VtSwapPublic(d, 34, (void *)hk_SR))) o_SR = (SR_t)p;
    BackBufferSize(d);
    D3DAddFrameCallback(OnFrame);
    Log("tyres: device hooks on");
}

void PostInit(void)
{
    g_tyre = IniInt("TyreMarks", "Enabled", 1);
    g_tyreLife = IniFloat("TyreMarks", "Lifetime", 120.f);
    g_slipMin = IniFloat("TyreMarks", "SlipThreshold", 3.5f);
    if (g_slipMin < 0.5f) g_slipMin = 0.5f;
    if (g_tyreLife < 5.f) g_tyreLife = 5.f;
    InitializeCriticalSection(&g_qcs);
    Log("tyres: marks %d, lifetime %.0f s", g_tyre, g_tyreLife);
}

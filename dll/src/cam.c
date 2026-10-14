/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * On-foot camera rig ("human" camera controller, vtable 0xBD2100, update 0x71DC40)
 *
 * The controller writes its final eye position to this+0x24 (orientation
 * quaternion at +0x34) and then calls the base update 0x70FD40, which hands
 * the result to the engine. We hook that call (0x71E59A) and re-place the eye
 * relative to the character before it is consumed:
 *
 *   eye' = char + (eye - char) * Distance + up * Height + right * Side
 *
 * Moving along the char->eye line keeps the character at the same spot on
 * screen, just smaller (more of the body visible). Separate values are used
 * while aiming (aim flag this+0x7DD), blended over BlendTime seconds.
 * Values are re-read from Mercenaries2Remastered.ini every second, so they can be tuned
 * with the game running.
 */
#include "m2r.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HUMAN_VT   0xBD2100

static int   g_on;
static float g_pivot, g_drop, g_dist, g_height, g_fovN;      /* normal (GTA style orbit) */
static float g_aimBack, g_aimSide, g_aimUp, g_aimConv, g_aimFov; /* aiming: shoulder cam converging on the aim ray */
static float g_blendTime;
static float g_aimT;                 /* 0 = normal, 1 = aiming */
static BYTE *g_lastCtrl;
static LARGE_INTEGER g_qpf, g_prev;
static float g_saveEye[3], g_saveQ[4], g_saveFov;
static int   g_saved;
static int   g_viewSplit, g_autoConv;
float AimConvergeDist(const float *eye, const float *f, float dt, DWORD me);
extern float g_rEye[3], g_rQ[4], g_rFov;
extern volatile int g_rValid;
volatile DWORD g_playerGuid;
void AimUpdate(BYTE *ctrl, const float *eye, const float *f, float pivot, float dt, float *viewF);
float SightPitch(float fov);

static void LoadCfg(void)
{
    g_on        = IniInt("Camera", "Enable", 1);
    g_pivot     = IniFloat("Camera", "GameFocusDist", 3.0f);
    g_drop      = IniFloat("Camera", "FocusDrop", 0.6f);
    g_dist      = IniFloat("Camera", "Distance", 4.2f);
    g_height    = IniFloat("Camera", "Height", 0.35f);
    g_fovN      = IniFloat("Camera", "FovScale", 1.1f);
    g_aimBack   = IniFloat("Camera", "AimBack", 2.0f);
    g_aimSide   = IniFloat("Camera", "AimSide", 0.4f);
    g_aimUp     = IniFloat("Camera", "AimHeight", 0.0f);
    g_aimConv   = IniFloat("Camera", "AimConverge", 40.0f);
    if (g_aimConv < 2.f) g_aimConv = 2.f;
    g_aimFov    = IniFloat("Camera", "AimFovScale", 1.9f);
    g_autoConv  = IniInt("Camera", "AimConvergeAuto", 1);
    g_blendTime = IniFloat("Camera", "BlendTime", 0.25f);
    if (g_fovN < 0.3f || g_fovN > 2.5f) g_fovN = 1.f;
    if (g_aimFov < 0.3f || g_aimFov > 2.5f) g_aimFov = 1.f;
    if (g_dist < 0.5f) g_dist = 0.5f;
}

static void QuatFromDir(float fx, float fy, float fz, float *q)
{
    float yaw = atan2f(fx, fz);
    float h = sqrtf(fx * fx + fz * fz);
    float pitch = atan2f(-fy, h);                 /* positive = looking down */
    float sy = sinf(yaw * .5f), cy = cosf(yaw * .5f), sx = sinf(pitch * .5f), cx = cosf(pitch * .5f);
    q[0] = cy * sx; q[1] = sy * cx; q[2] = -sy * sx; q[3] = cy * cx;
}

static void Nlerp(const float *a, const float *b, float t, float *o)
{
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float sgn = d < 0 ? -1.f : 1.f, n = 0;
    for (int i = 0; i < 4; i++) { o[i] = a[i] * (1 - t) + b[i] * t * sgn; n += o[i] * o[i]; }
    n = 1.f / sqrtf(n);
    for (int i = 0; i < 4; i++) o[i] *= n;
}



static void __cdecl Adjust(BYTE *ctrl)
{
    static DWORD nextCfg;
    DWORD now = GetTickCount();
    if ((int)(now - nextCfg) >= 0) { nextCfg = now + 1000; LoadCfg(); }
    g_lastCtrl = ctrl;
    g_playerGuid = *(DWORD *)(ctrl + 0x20);     /* the on-foot camera follows the player character */

    LARGE_INTEGER tq; QueryPerformanceCounter(&tq);
    float dt = g_prev.QuadPart ? (float)(tq.QuadPart - g_prev.QuadPart) / g_qpf.QuadPart : 0.f;
    g_prev = tq;
    if (dt > 0.1f) dt = 0.1f;

    int aim = ctrl[0x7DD] != 0;
    float step = g_blendTime > 0.01f ? dt / g_blendTime : 1.f;
    g_aimT += aim ? step : -step;
    if (g_aimT < 0) g_aimT = 0;
    if (g_aimT > 1) g_aimT = 1;
    if (!g_on) return;

    float t = g_aimT * g_aimT * (3 - 2 * g_aimT);       /* smoothstep */

    float *q = (float *)(ctrl + 0x34), *eye = (float *)(ctrl + 0x24);
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float n = x * x + y * y + z * z + w * w;
    if (!(n > 0.9f && n < 1.1f)) return;
    float fx = 2 * (x * z + w * y), fy = 2 * (y * z - w * x), fz = 1 - 2 * (x * x + y * y);

    /* save the controller's own state (restored after the engine read it) */
    g_saveEye[0] = eye[0]; g_saveEye[1] = eye[1]; g_saveEye[2] = eye[2];
    memcpy(g_saveQ, q, 16);
    g_saveFov = *(float *)(ctrl + 0x5E8);
    g_saved = 1;

    /* aim assist may turn the game's aim away from the view (hip fire) */
    float E[3] = { eye[0], eye[1], eye[2] };
    {
        float gf[3] = { fx, fy, fz }, vf[3];
        AimUpdate(ctrl, E, gf, g_pivot, dt, vf);
        if (vf[0] != fx || vf[1] != fy || vf[2] != fz) {
            float P[3] = { E[0] + fx * g_pivot, E[1] + fy * g_pivot, E[2] + fz * g_pivot };
            fx = vf[0]; fy = vf[1]; fz = vf[2];
            E[0] = P[0] - fx * g_pivot; E[1] = P[1] - fy * g_pivot; E[2] = P[2] - fz * g_pivot;
            g_viewSplit = 1;
        } else g_viewSplit = 0;
    }

    /* normal: orbit a target dropped to chest height, same yaw/pitch as the game */
    float T[3] = { E[0] + fx * g_pivot, E[1] + fy * g_pivot - g_drop, E[2] + fz * g_pivot };
    float eN[3] = { T[0] - fx * g_dist, T[1] - fy * g_dist + g_height, T[2] - fz * g_dist };
    float qN[4];
    QuatFromDir(T[0] - eN[0], T[1] - eN[1], T[2] - eN[2], qN);

    /* aim: over-the-shoulder camera that looks at the point the game aims at
       (AimConverge metres down the game's aim ray), so the crosshair stays true */
    float rx = 1 - 2 * (y * y + z * z), rz = 2 * (x * z - w * y);
    if (g_viewSplit) { rx = fz; rz = -fx; }
    float rl = sqrtf(rx * rx + rz * rz);
    if (rl > 1e-3f) { rx /= rl; rz /= rl; }
    /* The game shoots through its reticle, which sits above the screen centre (aim.c, SightPitch): along the
       game's forward turned up by SightPitch(game fov). The aim camera converges on that line and is turned
       down so the reticle (same screen place, wider fov here) shows it. */
    float fovk = g_fovN + (g_aimFov - g_fovN) * t, fovIn = *(float *)(ctrl + 0x5E8), fovOut = fovIn * fovk;
    if (fovOut > 2.6f) fovOut = 2.6f;
    int fovOk = fovIn > 0.05f && fovIn < 3.f;
    float aY = atan2f(fx, fz), aP = asinf(fy < -1 ? -1 : fy > 1 ? 1 : fy) + (fovOk ? SightPitch(fovIn) : 0.f);
    float ad[3] = { cosf(aP) * sinf(aY), sinf(aP), cosf(aP) * cosf(aY) };
    float conv = g_aimConv;
    if (g_autoConv) { float c = AimConvergeDist(E, ad, dt, *(DWORD *)(ctrl + 0x20)); if (c > 0) conv = c; }
    float A[3] = { E[0] + ad[0] * conv, E[1] + ad[1] * conv, E[2] + ad[2] * conv };
    float eA[3] = { E[0] - fx * g_aimBack - rx * g_aimSide,
                    E[1] - fy * g_aimBack + g_aimUp,
                    E[2] - fz * g_aimBack - rz * g_aimSide };
    float qA[4];
    {
        float d[3] = { A[0] - eA[0], A[1] - eA[1], A[2] - eA[2] };
        float h = sqrtf(d[0] * d[0] + d[2] * d[2]), cP = atan2f(d[1], h);
        if (fovOk) cP -= SightPitch(fovOut);
        QuatFromDir(cosf(cP) * sinf(atan2f(d[0], d[2])), sinf(cP), cosf(cP) * cosf(atan2f(d[0], d[2])), qA);
    }

    for (int i = 0; i < 3; i++) eye[i] = eN[i] + (eA[i] - eN[i]) * t;
    Nlerp(qN, qA, t, q);

    float *fov = (float *)(ctrl + 0x5E8);
    if (*fov > 0.05f && *fov < 3.f) {
        float v = *fov * fovk;
        if (v > 2.6f) v = 2.6f;
        *fov = v;
    }
    memcpy(g_rEye, eye, 12); memcpy(g_rQ, q, 16); g_rFov = *fov; g_rValid = 1;
}

/* after the engine consumed the camera: give the controller its own state back,
   it uses last frame's eye/fov as the starting point of its smoothing */
static void __cdecl Restore(BYTE *ctrl)
{
    if (!g_saved) return;
    g_saved = 0;
    float *eye = (float *)(ctrl + 0x24);
    eye[0] = g_saveEye[0]; eye[1] = g_saveEye[1]; eye[2] = g_saveEye[2];
    memcpy(ctrl + 0x34, g_saveQ, 16);
    *(float *)(ctrl + 0x5E8) = g_saveFov;
}

static void *g_baseUpd = (void *)0x70FD40;

/* replaces `call 0x70FD40` at 0x71E59A (ecx = controller, [esp+4] = dt) */
__attribute__((naked)) static void CamThunk2(void)
{
    __asm__ volatile (
        "push %%ecx\n\t"              /* [esp]=this [esp+4]=ret [esp+8]=dt */
        "push %%ecx\n\t"
        "call %P0\n\t"
        "add $4, %%esp\n\t"
        "mov (%%esp), %%ecx\n\t"
        "push 8(%%esp)\n\t"
        "call *%1\n\t"               /* base update, ret 4 */
        "push (%%esp)\n\t"
        "call %P2\n\t"
        "add $4, %%esp\n\t"
        "pop %%ecx\n\t"
        "ret $4\n\t"
        :: "i"(Adjust), "m"(g_baseUpd), "i"(Restore));
}

static void CamFrame(void)
{
    static DWORD lastVt;
    DWORD base = *(volatile DWORD *)0x11761B0;
    if (base) {
        BYTE *ctrl = *(BYTE **)(base + 0x200);
        DWORD vt = ctrl ? *(DWORD *)ctrl : 0;
        if (vt != lastVt) { Log("cam: active controller vtable %08lX%s", vt, vt == HUMAN_VT ? " (human)" : ""); lastVt = vt; }
    }
}

#include "d3dhook.h"
static void CamFrameCb(IDirect3DDevice9 *d) { (void)d; CamFrame(); }

void CamInit(void)
{
    QueryPerformanceFrequency(&g_qpf);
    LoadCfg();
    /* Aim hold (FUN_0071D0A0, run by the camera update): when aiming starts or ends, and when the controller is
     * asked to face the aim point (+0x7DC), it stores the aim point (+0x978, the player's target - with hip
     * redirect the enemy marked by aim.c) and turns yaw and pitch toward it with weights +0x984 / +0x988 (1.0 at
     * first, fading out; pulls at 0x71D422 and 0x71D47D in the same update): the view snapped to enemies without
     * aiming, and on entering aim. Our camera keeps the crosshair on the aim line by itself; both weights are
     * now set to 0 there (0x71D2BE: movaps xmm0,xmm1 -> xorps xmm0,xmm0; 0x71D2EC stores xmm0 instead of xmm1). */
    if (Expect(0x71D2BE, "0F 28 C1") && Expect(0x71D2E1, "0F 57 C0 F3 0F 11 86 84 09 00 00 F3 0F 11 8E 88 09 00 00")) {
        static const BYTE x0[3] = {0x0F, 0x57, 0xC0}, st[1] = {0x86};
        WriteMem(0x71D2BE, x0, 3);
        WriteMem(0x71D2EF, st, 1);
        Log("cam: aim hold off");
    } else Log("cam: aim hold code mismatch");
    /* Target follow (camera update 0x71DECA..0x71DF5E): while the character is in its locked state (character
     * +0x2F4, set with a subdued enemy) and the stick doesn't turn the camera, yaw is turned every frame toward the
     * point at character +0x2FC (the enemy) from +0x308: the camera stuck to subdued enemies like a magnet and
     * circled with them. The turn is skipped (0x71DF2B: jbe -> jmp). */
    if (Expect(0x71DF24, "0F 2F 1D 74 C1 B9 00 76 40")) {
        static const BYTE jmp = 0xEB;
        WriteMem(0x71DF2B, &jmp, 1);
        Log("cam: target follow off");
    } else Log("cam: target follow code mismatch");
    /* Aim magnetism (FUN_0071D5B0, called at 0x71DC28 with the enemy under the crosshair): adds a yaw turn toward
     * that enemy (FUN_0070E830, state +0x784, weight +0x780) to the stick yaw - the view snapped to enemies near
     * the crosshair. Its result is dropped: the stick yaw is returned as is (0x71D776: movss xmm0,[esp+4] /
     * addss xmm0,[esp+0x38] -> movss xmm0,[esp+0x38]). */
    if (Expect(0x71D776, "F3 0F 10 44 24 04 F3 0F 58 44 24 38")) {
        static const BYTE ld[12] = {0xF3, 0x0F, 0x10, 0x44, 0x24, 0x38, 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x90};
        WriteMem(0x71D776, ld, 12);
        Log("cam: aim magnetism off");
    } else Log("cam: aim magnetism code mismatch");
    /* Mounted gun aim assist (turret controller update 0x723140 -> FUN_007239C0): picks a target (FUN_0070EC00,
     * state +0x654) and turns the stick yaw / pitch deltas toward it - mounted guns pulled onto vehicles. Its
     * result is not stored: the deltas stay as the stick gave them (0x723E0C / 0x723E10: two movss stores ->
     * nop). */
    if (Expect(0x723E0B, "5E F3 0F 11 19 F3 0F 11 02 5B")) {
        static const BYTE nop8[8] = {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
        WriteMem(0x723E0C, nop8, 8);
        Log("cam: mounted gun aim assist off");
    } else Log("cam: mounted gun aim assist code mismatch");
    if (Expect(0x71E59A, "E8 A1 17 FF FF")) {
        WriteCall(0x71E59A, (void *)CamThunk2, 5);
        D3DAddFrameCallback(CamFrameCb);
        Log("cam: on-foot camera installed (dist %.2f h %.2f drop %.2f fov x%.2f | aim back %.2f side %.2f up %.2f conv %.0f fov x%.2f)",
            g_dist, g_height, g_drop, g_fovN, g_aimBack, g_aimSide, g_aimUp, g_aimConv, g_aimFov);
    } else Log("cam: call site mismatch, camera rig not installed");
}

/* ------------------------------------------------------------------ turrets
 * "turret" controller (vtable 0xBD2200, update 0x723140): yaw/pitch are
 * integrated from rate inputs this+0x10 / this+0x14 (scaled by 0.25 and
 * clamped to +-1 in 0x722EB0) times the turret's YawSpeed (+0x648) /
 * PitchSpeed (+0x630). We feed the right stick straight into the rate input
 * and optionally scale the speeds for the duration of the update. */
#define TURRET_VT 0xBD2200
extern float g_padRX, g_padRY;
extern int   g_padRActive;
typedef void (__thiscall *TurretUpd_t)(void *self, float dt);
static TurretUpd_t o_TurretUpd;
static float g_tSens, g_tSpeed;
static int   g_tInvX, g_tInvY, g_tOn;

static void __fastcall TurretUpd(BYTE *self, void *edx, float dt)
{
    (void)edx;
    static DWORD nextCfg;
    DWORD now = GetTickCount();
    if ((int)(now - nextCfg) >= 0) {
        nextCfg = now + 1000;
        g_tOn    = IniInt("Turret", "Enable", 1);
        g_tSens  = IniFloat("Turret", "StickSensitivity", 1.0f);
        g_tSpeed = IniFloat("Turret", "SpeedScale", 2.0f);
        g_tInvX  = IniInt("Turret", "InvertX", 0);
        g_tInvY  = IniInt("Turret", "InvertY", 0);
    }
    if (!g_tOn) { o_TurretUpd(self, dt); return; }
    if (g_padRActive) {
        float x = g_padRX * g_tSens, y = g_padRY * g_tSens;
        if (x > 1) x = 1;
        if (x < -1) x = -1;
        if (y > 1) y = 1;
        if (y < -1) y = -1;
        *(float *)(self + 0x10) = (g_tInvX ? -x : x) * 4.f;
        *(float *)(self + 0x14) = (g_tInvY ? -y : y) * 4.f;
    }
    /* +0x648 / +0x630 are the turret's current turn velocities (ramped by the
       game); +0x668 / +0x66C are the top turn rates. Scale only the top rates,
       for this update only. */
    float *ry = (float *)(self + 0x668), *rp = (float *)(self + 0x66C);
    float ry0 = *ry, rp0 = *rp;
    BYTE save[0x30], ang[8];
    memcpy(save, self + 0x10, 0x30);              /* inputs, eye, orientation */
    memcpy(ang, self + 0x618, 8);
    if (ry0 > 0 && ry0 < 50) *ry = ry0 * g_tSpeed;
    if (rp0 > 0 && rp0 < 50) *rp = rp0 * g_tSpeed;
    o_TurretUpd(self, dt);
    *ry = ry0; *rp = rp0;
    /* never let a broken value through (NaN / runaway angles) */
    const float *f = (const float *)self;
    int bad = 0;
    for (int o = 0x24; o < 0x44; o += 4) if (!isfinite(f[o / 4]) || fabsf(f[o / 4]) > 1e6f) bad = 1;
    if (!isfinite(f[0x618 / 4]) || !isfinite(f[0x61C / 4]) || fabsf(f[0x618 / 4]) > 100.f || fabsf(f[0x61C / 4]) > 100.f) bad = 1;
    if (bad) {
        static int nbad;
        if (nbad++ < 5) Log("turret: bad state after update (yaw %f pitch %f q %f %f %f %f), restored", f[0x618 / 4], f[0x61C / 4], f[13], f[14], f[15], f[16]);
        memcpy(self + 0x10, save, 0x30);
        memcpy(self + 0x618, ang, 8);
        *(float *)(self + 0x10) = 0; *(float *)(self + 0x14) = 0;
    }
}

void TurretInit(void)
{
    void **slot = (void **)(TURRET_VT + 0x10);
    if (*slot != (void *)0x723140) { Log("turret: vtable mismatch"); return; }
    o_TurretUpd = (TurretUpd_t)*slot;
    void *p = (void *)TurretUpd;
    WriteMem((DWORD)slot, &p, 4);
    Log("turret: mounted gun stick input hooked");
}

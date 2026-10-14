/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * Aim assist + hip-fire reticle
 *
 * Hip fire ("Total Overdose" style): the nearest hostile near the centre of
 * the view is marked red and handed to the game as the aim point (Redirect),
 * so the character shoots at it; neither the camera nor the game's aim is
 * turned. The weapon reticle is replaced by a small grey dot.
 *
 * Aiming down the sight: the game reticle comes back; on entering aim the
 * view snaps to the hostile closest to the aim line, then a gentle magnet
 * keeps it on target while it stays inside a small cone.
 *
 * Game pieces used
 *   humans list         head node at 0x198D768, node = {obj, next}, obj+0x10 = guid
 *   transform           0x665AF0 (edi = guid, esi = out[8]) -> pos at out[0..2]
 *   health component    0x5857E0 (ecx = 0x17BEF78, eax = guid), +4 = health
 *   relation            0x5880D0 stdcall (guidA, guidB) -> xmm0, < 0 = enemies
 *   view-projection     *(0xDFC2F8) + vp*0xE80 + 0xB70, vp = *(WORD *)(.. + 0x2B92)
 *   human controller    +0x20 character guid, +0x618 yaw (radians),
 *                       +0x61C pitch (0..1), +0x7DD aiming
 */
#define COBJMACROS
#include "m2r.h"
#include "d3dhook.h"
#include "dxt.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int InputMode(void);
extern float g_padRX, g_padRY;
extern int g_padRActive;
static float g_stickNow;

#define HUMAN_HEAD   0x198D768
#define HEALTH_TABLE 0x17BEF78
#define PI 3.14159265f

/* ------------------------------------------------------------ settings */
static int   g_on, g_padOnly, g_hipOn, g_adsOn, g_dot, g_mark;
static float g_hipCone, g_hipRange;
static float g_snapCone, g_snapTime, g_range, g_magRadius, g_magMin, g_magStrength, g_magMaxRate;
static const float g_chest = 1.15f;     /* aim point above a character's position when its bones are not known */
static int   g_redirect, g_occOn;

static void LoadCfg(void)
{
    g_on         = IniInt("AimAssist", "Enable", 1);
    g_padOnly    = IniInt("AimAssist", "GamepadOnly", 1);
    g_hipOn      = IniInt("AimAssist", "HipFireAssist", 1);
    g_hipCone    = IniFloat("AimAssist", "HipCone", 14.f);
    g_hipRange   = IniFloat("AimAssist", "HipRange", 45.f);
    g_adsOn      = IniInt("AimAssist", "AimDownSightAssist", 1);
    g_snapCone   = IniFloat("AimAssist", "SnapCone", 20.f);
    g_snapTime   = IniFloat("AimAssist", "SnapTime", 0.08f);
    g_range      = IniFloat("AimAssist", "Range", 120.f);
    g_magRadius  = IniFloat("AimAssist", "MagnetRadius", 1.2f);
    g_magMin     = IniFloat("AimAssist", "MagnetMinAngle", 2.0f);
    g_magStrength= IniFloat("AimAssist", "MagnetStrength", 5.0f);
    g_magMaxRate = IniFloat("AimAssist", "MagnetMaxRate", 45.f);
    g_dot        = IniInt("AimAssist", "HipDot", 1);
    g_mark       = IniInt("AimAssist", "MarkTarget", 1);
    g_redirect   = IniInt("AimAssist", "HipRedirect", 1);
    g_occOn      = IniInt("AimAssist", "IgnoreHidden", 1);
}

/* ------------------------------------------------------------ game calls */
static int ValidPtr(const void *p) { return (DWORD)p >= 0x10000 && (DWORD)p < 0x7FFF0000; }

static int GamePos(DWORD guid, float *out)
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

static void *Component(DWORD table, DWORD guid)
{
    void *r;
    __asm__ volatile (
        "push %%ebx\n\t"
        "push %%esi\n\t"
        "push %%edi\n\t"
        "push %%ebp\n\t"
        "mov $0x5857E0, %%edx\n\t"
        "call *%%edx\n\t"
        "pop %%ebp\n\t"
        "pop %%edi\n\t"
        "pop %%esi\n\t"
        "pop %%ebx\n\t"
        : "=a"(r), "+c"(table)
        : "a"(guid)
        : "edx", "memory", "cc");
    return r;
}

static float Relation(DWORD a, DWORD b)
{
    float r;
    __asm__ volatile (
        "push %%ebx\n\t"
        "push %%esi\n\t"
        "push %%edi\n\t"
        "push %%ebp\n\t"
        "push %2\n\t"
        "push %1\n\t"
        "mov $0x5880D0, %%eax\n\t"
        "call *%%eax\n\t"
        "pop %%ebp\n\t"
        "pop %%edi\n\t"
        "pop %%esi\n\t"
        "pop %%ebx\n\t"
        "movss %%xmm0, %0\n\t"
        : "=m"(r)
        : "r"(a), "r"(b)
        : "eax", "ecx", "edx", "memory", "cc");
    return r;
}

static int Alive(DWORD guid)
{
    BYTE *h = (BYTE *)Component(HEALTH_TABLE, guid);
    return h && ValidPtr(h) && *(float *)(h + 4) > 0.01f;
}

typedef void *(__cdecl *GetPlayer_t)(int);
static const GetPlayer_t GetPlayer = (GetPlayer_t)0x6CDAF0;

/* ------------------------------------------------------------ hostile list */
#define MAXT 96
static DWORD g_tgt[MAXT];
static int   g_ntgt;

/* Subdued enemies (Lua Subdue, FUN_005BFCC0: character record 0x17BF9C8 +0x3C |= 0x80) are left alone: the
 * hip mark held on to them and turned the aim with the player circling them */
static int Subdued(DWORD g)
{
    BYTE *r;
    __asm__ volatile("call *%3" : "=a"(r) : "a"(g), "c"(0x17BF9C8), "r"(0x5857E0) : "edx", "memory", "cc");
    return r && (*(WORD *)(r + 0x3C) & 0x80);
}
static void ScanHostiles(DWORD me)
{
    int n = 0;
    DWORD node = *(volatile DWORD *)HUMAN_HEAD;
    for (int it = 0; node && it < 1024; it++) {
        if (!ValidPtr((void *)node)) break;
        DWORD obj = *(DWORD *)node, next = *(DWORD *)(node + 4);
        if (obj && ValidPtr((void *)obj)) {
            DWORD g = *(DWORD *)(obj + 0x10);
            if (g && g != me && Alive(g) && !Subdued(g)) {
                float r1 = Relation(g, me), r2 = Relation(me, g);
                float r = r1 < r2 ? r1 : r2;
                if (r < -0.01f && n < MAXT) g_tgt[n++] = g;
            }
        }
        node = next;
    }
    g_ntgt = n;
}

/* ------------------------------------------------------------ helpers */
static float Wrap(float a)
{
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

static void DirAngles(const float *d, float *yaw, float *pitch)
{
    float h = sqrtf(d[0] * d[0] + d[2] * d[2]);
    *yaw = atan2f(d[0], d[2]);
    *pitch = atan2f(d[1], h);
}

/* ------------------------------------------------------------ calibration
 * forward yaw = s * ctrl[0x618] + c, forward pitch = a * ctrl[0x61C] + b */
static float g_prevYawF, g_prevYawS, g_prevPitF, g_prevPitS;
static int   g_havePrev, g_yawVote, g_pitN[2];
static double g_sPP[2], g_sPT[2];

/* pitch is calibrated separately for the hip camera and the aim camera (about 1.4 and 1.0 radians per unit),
   and not while the camera moves between them (it turns by itself then) */
static void Calibrate(float yawF, float pitF, float yawS, float pitS, int m, int moving)
{
    if (g_havePrev) {
        float dyF = Wrap(yawF - g_prevYawF), dyS = Wrap(yawS - g_prevYawS);
        if (fabsf(dyS) > 0.003f && fabsf(dyS) < 0.4f && fabsf(dyF) > 0.002f) {
            float k = dyF / dyS;
            if (fabsf(fabsf(k) - 1) < 0.5f) { g_yawVote += k > 0 ? 1 : -1; if (g_yawVote > 200) g_yawVote = 200; if (g_yawVote < -200) g_yawVote = -200; }
        }
        float dpF = pitF - g_prevPitF, dpS = pitS - g_prevPitS;
        if (!moving && fabsf(dpS) > 0.002f && fabsf(dpS) < 0.3f && fabsf(dpF) < 0.5f) {
            g_sPP[m] += dpS * dpS; g_sPT[m] += dpS * dpF; if (g_pitN[m] < 100000) g_pitN[m]++;
        }
    }
    g_prevYawF = yawF; g_prevYawS = yawS; g_prevPitF = pitF; g_prevPitS = pitS; g_havePrev = 1;
}
static float YawSign(void)    { return g_yawVote < 0 ? -1.f : 1.f; }
static float PitchSlope(int m)
{
    if (g_pitN[m] >= 8 && g_sPP[m] > 1e-6) return (float)(g_sPT[m] / g_sPP[m]);
    if (g_pitN[!m] >= 8 && g_sPP[!m] > 1e-6) return (float)(g_sPT[!m] / g_sPP[!m]);
    return 0.f;
}

/* ------------------------------------------------------------ state */
static float g_offYaw, g_offPit;           /* game aim minus rendered view (radians) */
static DWORD g_lock;                       /* hip-fire target */
static DWORD g_adsTgt;
static float g_snapLeft;
/* entering aim the game camera moves to its aim position over about 0.35 s and turns by itself: the snap
   keeps pulling (by the current error each frame) until it is in place */
#define SNAP_HOLD 0.4f
/* The sight is not at the screen centre: the game puts the reticle at (0, 0.25) of the screen (Lua
   GetReticlePosition, FUN_005B2E70: 0, [0xBBC7EC] = 0.25) and shoots through it, i.e. along the camera's forward
   turned up by atan(0.25 * tan(fov / 2) / aspect) (fov is horizontal; 2.4 degrees at the aim fov 0.576, as the
   game's aim point showed). The aim assist puts the target on that line, not on the view centre (which left the
   reticle above the head); the aim camera (cam.c) is turned so the reticle shows that line. */
#define RETICLE_Y 0.25f
float g_aspect = 16.f / 9.f;
float SightPitch(float fov) { return atanf(RETICLE_Y * tanf(fov * .5f) / g_aspect); }           /* snap goal in the controller's yaw / pitch state */
static int   g_prevAim;
static volatile int   g_markOn, g_redirOn;
static void Redirect(void);
static volatile float g_markPos[3];
static volatile DWORD g_lastUpd;
static volatile int   g_hipMode = -1;      /* 1 = on foot, not aiming */
static DWORD g_nextScan, g_nextCfg;

/* ------------------------------------------------------------ visibility
 * line-of-sight rays through the game's own ray system (the one the reticle
 * uses, type 0x223F6FDA): submit 0x409560, results one frame later 0x409760 */
typedef struct { DWORD guid; DWORD seen, tested; } Vis;
static Vis g_vis[64];
static volatile LONG g_qIssued, g_qVis, g_qHid;
typedef struct { DWORD guid; float pos[3]; } Cand;
static Cand g_cand[16];
static volatile int g_ncand;
static float g_eyeLast[3];

static Vis *VisSlot(DWORD g, int create)
{
    int freeI = -1; DWORD oldest = 0xFFFFFFFF; int oldI = 0;
    for (int i = 0; i < 64; i++) {
        if (g_vis[i].guid == g) return &g_vis[i];
        if (!g_vis[i].guid && freeI < 0) freeI = i;
        if (g_vis[i].tested < oldest) { oldest = g_vis[i].tested; oldI = i; }
    }
    if (!create) return NULL;
    int i = freeI >= 0 ? freeI : oldI;
    memset(&g_vis[i], 0, sizeof g_vis[i]);
    g_vis[i].guid = g;
    return &g_vis[i];
}


static DWORD RaySubmit(const float *start, const float *end, DWORD ignore)
{
    DWORD h;
    const float *st = start, *en = end;
    __asm__ volatile (
        "mov %3, %%eax\n\t"
        "push %%ebx\n\t"
        "push %%esi\n\t"
        "push %%edi\n\t"
        "push %%ebp\n\t"
        "mov 0x1175034, %%edi\n\t"
        "push $0\n\t"
        "push $1\n\t"
        "push %%eax\n\t"
        "push $0x223F6FDA\n\t"
        "push $0\n\t"
        "mov $0x409560, %%eax\n\t"
        "call *%%eax\n\t"
        "pop %%ebp\n\t"
        "pop %%edi\n\t"
        "pop %%esi\n\t"
        "pop %%ebx\n\t"
        : "=a"(h), "+c"(en), "+d"(st)
        : "m"(ignore)
        : "memory", "cc");
    return h;
}

/* 1 = result copied into buf, 0 = gone, -1 = not ready yet (keep waiting).
   Results live in the ray system's slot array (sys+0x20004, 0x80 bytes per
   slot) while its frame counter == submit counter + 1 (same as 0x71FB6F). */
static int RayFetch(DWORD h, BYTE *buf)
{
    BYTE *sys = *(BYTE **)0x1175034;
    if (!h || !ValidPtr(sys) || sys[0x5C89C]) return 0;
    WORD cnt = *(WORD *)(sys + 0x5C860), hi = (WORD)(h >> 16);
    if (cnt == hi) return -1;
    if ((WORD)(hi + 1) != cnt) return 0;
    BYTE *arr = *(BYTE **)(sys + 0x20004);
    if (!ValidPtr(arr) || !(h & 0xFFFF)) return 0;
    memcpy(buf, arr + ((h & 0xFFFF) - 1) * 0x80, 0x80);
    return 1;
}

/* ------------------------------------------------------------ body bones
 * Characters keep the model-space positions of 12 body bones for hit locations (FUN_0043E350, bone names hashed
 * at 0x117CFA4: Hips, RThigh, RShin, LThigh, LShin, Spine2, Chest, ...) in their rig object: bone i at
 * +0x140 + i*0x18, rotation quaternion +0x270, position +0x260; world = rotate(bone) + position, as FUN_0043EB?
 * does (0x43EC16). The rig is +0x18 of the character's animation component (vtable 0xBCDB98, owner +0x14),
 * whose update (vtable +0x14, FUN_00691730) runs every frame; the component is noted there per character. */
#define ANIM_VT_UPD 0xBCDBAC
#define BONE_CHEST  6
#define BONE_HEAD   11
typedef struct { DWORD guid; BYTE *comp; } AnimRec;
static AnimRec g_anim[256];
static void *g_animUpd = (void *)0x691730;
static void __attribute__((used)) NoteAnim(BYTE *comp)
{
    DWORD g = *(DWORD *)(comp + 0x14);
    if (!g) return;
    AnimRec *r = &g_anim[(g * 2654435761u) >> 24];
    r->guid = g; r->comp = comp;
}
__attribute__((naked)) static void AnimUpdThunk(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "push %%ecx\n\t"
        "call _NoteAnim\n\t"
        "add $4, %%esp\n\t"
        "popal\n\t"
        "jmp *%0\n\t" :: "m"(g_animUpd));
}
static int Chest(DWORD g, float *out)
{
    AnimRec *r = &g_anim[(g * 2654435761u) >> 24];
    if (r->guid != g || !ValidPtr(r->comp) || IsBadReadPtr(r->comp, 0x1C) || *(DWORD *)r->comp != 0xBCDB98 ||
        *(DWORD *)(r->comp + 0x14) != g) return 0;
    BYTE *rig = *(BYTE **)(r->comp + 0x18);
    if (!ValidPtr(rig) || IsBadReadPtr(rig, 0x280)) return 0;
    /* Bone_Chest sits low (about 1.1 m above the feet, the belly): the aim point is 40% of the way up to the
       head bone, the upper chest */
    const float *c = (const float *)(rig + 0x140 + BONE_CHEST * 0x18), *hd = (const float *)(rig + 0x140 + BONE_HEAD * 0x18);
    float b[3] = { c[0] + (hd[0] - c[0]) * 0.4f, c[1] + (hd[1] - c[1]) * 0.4f, c[2] + (hd[2] - c[2]) * 0.4f };
    const float *t = (const float *)(rig + 0x260),
                *q = (const float *)(rig + 0x270);
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(n > 0.5f && n < 1.5f)) return 0;
    n = 1.f / sqrtf(n);
    float x = q[0] * n, y = q[1] * n, z = q[2] * n, w = q[3] * n;
    /* v * D3DXMatrixRotationQuaternion(q) */
    float vx = b[0], vy = b[1], vz = b[2];
    out[0] = vx * (1 - 2 * (y * y + z * z)) + vy * 2 * (x * y - w * z) + vz * 2 * (x * z + w * y) + t[0];
    out[1] = vx * 2 * (x * y + w * z) + vy * (1 - 2 * (x * x + z * z)) + vz * 2 * (y * z - w * x) + t[1];
    out[2] = vx * 2 * (x * z - w * y) + vy * 2 * (y * z + w * x) + vz * (1 - 2 * (x * x + y * y)) + t[2];
    float P[3];
    if (!GamePos(g, P)) return 0;
    float d[3] = { out[0] - P[0], out[1] - P[1], out[2] - P[2] };
    return out[0] == out[0] && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 9.f;
}
/* aim point on a target: its upper chest, else a fixed height above its position */
static int AimPoint(DWORD g, float *X)
{
    if (Chest(g, X)) return 1;
    if (!GamePos(g, X)) return 0;
    X[1] += g_chest;
    return 1;
}

typedef struct { DWORD h, guid; float start[3]; float dist; int age; } Ray;
static Ray g_rays[8];
static int g_nrays, g_rayNext;

static void RaysUpdate(const float *eye, DWORD me)
{
    DWORD now = GetTickCount();
    static BYTE buf[0x200];
    /* collect finished rays, keep the ones still in flight */
    int keep = 0;
    for (int i = 0; i < g_nrays; i++) {
        memset(buf, 0, sizeof buf);
        int r = RayFetch(g_rays[i].h, buf);
        if (r < 0) { if (++g_rays[i].age < 10) g_rays[keep++] = g_rays[i]; continue; }
        if (!r) continue;
        DWORD hit = *(DWORD *)(buf + 0x6C);
        const float *hp = (const float *)(buf + (hit ? 0x54 : 0x24));
        float d[3] = { hp[0] - g_rays[i].start[0], hp[1] - g_rays[i].start[1], hp[2] - g_rays[i].start[2] };
        float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        Vis *v = VisSlot(g_rays[i].guid, 1);
        v->tested = now;
        if (hit == g_rays[i].guid || L >= g_rays[i].dist - 0.6f) { v->seen = now; InterlockedIncrement(&g_qVis); }
        else InterlockedIncrement(&g_qHid);
    }
    g_nrays = keep;
    if (!g_occOn || g_nrays) return;        /* one batch in flight at a time */
    int n = g_ncand, sent = 0;
    for (int k = 0; k < n && g_nrays < 8; k++) {
        Cand *c = &g_cand[(g_rayNext + k) % n];
        float d[3] = { c->pos[0] - eye[0], c->pos[1] - eye[1], c->pos[2] - eye[2] };
        float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        sent++;
        if (L < 2.f) { Vis *v = VisSlot(c->guid, 1); v->tested = v->seen = now; continue; }
        /* start 1.5 m in front of the camera (past our own character), end 0.4 m past the target */
        float st[3], en[3];
        for (int j = 0; j < 3; j++) { st[j] = eye[j] + d[j] / L * 1.5f; en[j] = c->pos[j] + d[j] / L * 0.4f; }
        DWORD h = RaySubmit(st, en, me);
        if (!h) continue;
        Ray *r = &g_rays[g_nrays++];
        r->h = h; r->guid = c->guid; memcpy(r->start, st, 12); r->dist = L - 1.5f; r->age = 0;
        InterlockedIncrement(&g_qIssued);
    }
    if (n) g_rayNext = (g_rayNext + sent) % n;
}

static int Visible(DWORD g)
{
    if (!g_occOn) return 1;
    if (g_qIssued > 200 && g_qVis + g_qHid == 0) return 1;   /* ray system not answering: don't hide anything */
    Vis *v = VisSlot(g, 0);
    if (!v || !v->tested) return 0;                 /* not tested yet: assume hidden */
    return (int)(GetTickCount() - v->seen) < 400;
}

/* Hip fire: the game fires only while the target it is given lies within about 12 degrees of where the gun
 * points (the view); beyond that the trigger did nothing while the mark was still held (up to HipCone x 1.5).
 * Hip marks are kept within HIP_MAX degrees of the view. */
#define HIP_MAX 11.f
static float g_coneCap = 1e9f;
static int Pick(const float *P, float vy, float vp, float cone, float range, DWORD prefer,
                float *outYaw, float *outPit, float *outPos, float *outDist)
{
    float best = 1e9f; int found = 0;
    for (int i = 0; i < g_ntgt; i++) {
        float X[3];
        if (!Visible(g_tgt[i])) continue;
        if (!AimPoint(g_tgt[i], X)) continue;
        float d[3] = { X[0] - P[0], X[1] - P[1], X[2] - P[2] };
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist < 1.0f || dist > range) continue;
        float ty, tp; DirAngles(d, &ty, &tp);
        float ey = Wrap(ty - vy), ep = tp - vp;
        float ang = sqrtf(ey * ey + ep * ep) * (180.f / PI);
        float c = cone * (g_tgt[i] == prefer ? 1.5f : 1.f);
        if (c > g_coneCap) c = g_coneCap;
        if (ang > c) continue;
        float score = ang + dist * 0.05f - (g_tgt[i] == prefer ? 3.f : 0.f);
        if (score < best) {
            best = score; found = 1;
            *outYaw = ty; *outPit = tp; *outDist = dist;
            memcpy(outPos, X, 12);
            g_lock = g_tgt[i];        /* caller decides whether to keep it */
        }
    }
    return found;
}

/* Called from the camera hook with the game's own eye / forward for this frame.
   Writes the view direction to render (may differ from the game's aim). */

void AimUpdate(BYTE *ctrl, const float *eye, const float *f, float pivot, float dt, float *viewF)
{
    DWORD now = GetTickCount();
    if ((int)(now - g_nextCfg) >= 0) { g_nextCfg = now + 1000; LoadCfg(); }
    memcpy(viewF, f, 12);
    g_lastUpd = now;

    float yawF, pitF; DirAngles(f, &yawF, &pitF);
    float *yawS = (float *)(ctrl + 0x618), *pitS = (float *)(ctrl + 0x61C);
    int aim = ctrl[0x7DD] != 0;
    /* entering / leaving aim the game camera moves to its aim position over about 0.35 s and turns up by a few
       degrees by itself (pitch state unchanged) */
    static float moved;
    if (aim != g_prevAim) moved = 0;
    moved += dt;
    Calibrate(yawF, pitF, *yawS, *pitS, aim, moved < 0.4f);
    g_hipMode = !aim;
    int active = g_on && !(g_padOnly && InputMode() == 0);
    DWORD me = *(DWORD *)(ctrl + 0x20);

    if (active && (int)(now - g_nextScan) >= 0) { g_nextScan = now + 120; ScanHostiles(me); }

    /* targets to test for visibility this frame */
    {
        int n = 0;
        if (active)
            for (int i = 0; i < g_ntgt && n < 16; i++) {
                float X[3];
                if (!AimPoint(g_tgt[i], X)) continue;
                float d[3] = { X[0] - eye[0], X[1] - eye[1], X[2] - eye[2] };
                float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dist > g_range || d[0] * f[0] + d[1] * f[1] + d[2] * f[2] < 0) continue;
                g_cand[n].guid = g_tgt[i]; memcpy(g_cand[n].pos, X, 12); n++;
            }
        memcpy(g_eyeLast, eye, 12);
        g_ncand = n;
        RaysUpdate(eye, me);
    }

    /* rendered view = game aim minus the current offset */
    float vy = Wrap(yawF - g_offYaw), vp = pitF - g_offPit;
    float slope = PitchSlope(aim), s = YawSign();

    float wantY = 0, wantP = 0;
    g_markOn = 0;
    g_redirOn = 0;
    if (active && !aim && g_hipOn && g_ntgt) {
        DWORD prev = g_lock;
        float ty, tp, X[3], dist;
        g_coneCap = HIP_MAX;
        int got = Pick(eye, vy, vp, g_hipCone, g_hipRange, prev, &ty, &tp, X, &dist);
        g_coneCap = 1e9f;
        if (got) {
            /* hip fire only marks the target and hands it to the game as the aim point (Redirect): the camera
               is not turned toward it */
            memcpy((void *)g_markPos, X, 12);
            g_markOn = g_mark;
            g_redirOn = g_redirect;
        } else g_lock = 0;
    } else g_lock = 0;

    /* move the offset toward its goal; in aim mode it returns to zero */
    float k = dt * (aim ? 12.f : 10.f);
    if (k > 1) k = 1;
    float nY = g_offYaw + Wrap(wantY - g_offYaw) * k;
    float nP = g_offPit + (wantP - g_offPit) * k;
    if (fabsf(nY) < 1e-4f && !g_lock) nY = 0;
    if (fabsf(nP) < 1e-4f && !g_lock) nP = 0;

    /* ADS snap + magnet act on the game aim (and the view follows) */
    float addY = 0, addP = 0;
    if (active && aim && g_adsOn && g_ntgt) {
        float ty, tp, X[3], dist;
        float fov = *(float *)(ctrl + 0x5E8);
        float aimP = pitF + (fov > 0.05f && fov < 3.f ? SightPitch(fov) : 0.f);
        if (!g_prevAim) {
            DWORD keep = g_lock;
            if (Pick(eye, yawF, aimP, g_snapCone, g_range, 0, &ty, &tp, X, &dist)) {
                g_adsTgt = g_lock; g_snapLeft = SNAP_HOLD;
            } else g_adsTgt = 0;
            g_lock = keep;
        }
        DWORD keep = g_lock;
        float cone;
        /* the player moving the stick takes over: cancel the snap, weaken the magnet */
        float stick = g_padRActive ? sqrtf(g_padRX * g_padRX + g_padRY * g_padRY) : 0.f;
        if (stick > 1) stick = 1;
        if (g_snapLeft > 0 && stick > 0.35f) g_snapLeft = 0;
        g_stickNow = stick;
        if (g_snapLeft > 0) cone = g_snapCone * 1.5f;
        else cone = g_magMin;
        /* magnet cone grows for close targets: atan(radius / distance) */
        int got = 0;
        {
            DWORD best = 0; float by = 0, bp = 0, bd = 0;
            for (int pass = 0; pass < 2 && !got; pass++) {
                float c = cone;
                if (pass == 1) c = 30.f;      /* wide pass, filtered below by the radius rule */
                if (Pick(eye, yawF, aimP, c, g_range, g_adsTgt, &ty, &tp, X, &dist)) {
                    float ey = Wrap(ty - yawF), ep = tp - aimP;
                    float ang = sqrtf(ey * ey + ep * ep);
                    float lim = atanf(g_magRadius / dist);
                    if (lim < g_magMin * PI / 180.f) lim = g_magMin * PI / 180.f;
                    if (g_snapLeft > 0 || ang <= lim) { best = g_lock; by = ty; bp = tp; bd = dist; got = 1; }
                }
            }
            if (got) { g_adsTgt = best; ty = by; tp = bp; dist = bd; }
        }
        g_lock = keep;
        if (got) {
            float ey = Wrap(ty - yawF), ep = tp - aimP, fr;
            if (g_snapLeft > 0) {
                float st = g_snapTime > 0.01f ? g_snapTime : 0.01f;
                fr = dt / st; if (fr > 1) fr = 1;
                g_snapLeft -= dt;
            } else {
                fr = g_magStrength * dt * (1.f - 0.75f * g_stickNow); if (fr > 1) fr = 1;
                float maxStep = g_magMaxRate * PI / 180.f * dt;
                float m = sqrtf(ey * ey + ep * ep) * fr;
                if (m > maxStep && m > 0) fr *= maxStep / m;
            }
            addY = ey * fr;
            addP = slope != 0.f ? ep * fr : 0.f;
        } else g_snapLeft = 0;
    } else { g_snapLeft = 0; if (!aim) g_adsTgt = 0; }
    g_prevAim = aim;

    /* apply: game yaw/pitch state changes by (offset change + ADS pull) */
    float dY = (nY - g_offYaw) + addY, dP = (nP - g_offPit) + addP;
    if (dY != 0) *yawS = Wrap(*yawS + s * dY);
    if (dP != 0 && slope != 0) {
        float v = *pitS + dP / slope;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        *pitS = v;
    }
    if (g_redirOn) Redirect();

    /* render this frame with the old offset (the game's quat reflects it) */
    float ry = vy, rp = vp;
    viewF[0] = cosf(rp) * sinf(ry); viewF[1] = sinf(rp); viewF[2] = cosf(rp) * cosf(ry);
    g_offYaw = nY; g_offPit = nP;

}

/* ------------------------------------------------------------ hip redirect
 * player object (0x6CDAF0(0)): +0x11C target under the reticle, +0x124 aim point */
static void Redirect(void)
{
    BYTE *pl = (BYTE *)GetPlayer(0);
    if (!pl || !ValidPtr(pl) || !g_lock) return;
    memcpy(pl + 0x124, (const void *)g_markPos, 12);
    *(DWORD *)(pl + 0x11C) = g_lock;
}

/* ------------------------------------------------------------ reticle swap
 * hip mode: weapon reticles (64x64) become a grey dot, and the crosshair ticks
 * in the HUD atlas global_gui_hud02 (512x512, x 236..288, y 296..352) are cleared */
typedef struct { IDirect3DTexture9 *tex; IDirect3DBaseTexture9 *dst; BYTE *orig, *alt; int w, h, state; } RTex;
static RTex g_rt[24];
static int  g_nrt;
static BYTE g_dotData[4096];
static volatile int g_rSelf;
static CRITICAL_SECTION g_rLock;

static const unsigned g_retCrc[] = {
    0xeea0116e, 0x385aaa31, 0x32b8bce0, 0xc693d5ba, 0xf3f3843b, 0x0b9678a9, 0x4ba8659c, 0xc9f61064, 0x143b6092 };
/* artillery autocannon bullets/sniper grenade largebullet rockets shotgun tank tow */
#define HUD02_CRC 0xf8d506cfu

static unsigned Crc(const BYTE *p, int n)
{
    static unsigned tab[256];
    if (!tab[1]) for (int m = 255; m >= 0; m--) {                  /* tab[1] last: the "ready" mark */
        unsigned c = (unsigned)m; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); tab[m] = c;
    }
    unsigned c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void MakeDot(void)
{
    static BYTE rgba[64 * 64 * 4];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float dx = x + 0.5f - 32, dy = y + 0.5f - 32, r = sqrtf(dx * dx + dy * dy);
            float a = 2.6f - r; if (a < 0) a = 0; if (a > 1) a = 1;
            float o = 3.6f - r; if (o < 0) o = 0; if (o > 1) o = 1;     /* dark rim for contrast */
            BYTE *p = rgba + (y * 64 + x) * 4;
            float g = 0.75f * a + 0.15f * (1 - a);
            p[0] = p[1] = p[2] = (BYTE)(g * 255);
            p[3] = (BYTE)((a * 0.85f + (o - a) * 0.45f) * 255);
        }
    DxtEncode5(rgba, 64, 64, g_dotData);
}

static void RUpload(RTex *t, int alt)
{
    D3DLOCKED_RECT lr;
    g_rSelf = 1;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(t->tex, 0, &lr, NULL, 0))) {
        const BYTE *src = alt ? t->alt : t->orig;
        int rows = t->h / 4, rb = t->w * 4;
        for (int r = 0; r < rows; r++) memcpy((BYTE *)lr.pBits + r * lr.Pitch, src + r * rb, rb);
        IDirect3DTexture9_UnlockRect(t->tex, 0);
        if (t->dst) D3DUpdateTexture((IDirect3DBaseTexture9 *)t->tex, t->dst);
        t->state = alt;
    }
    g_rSelf = 0;
}

static void OnTex(IDirect3DTexture9 *tex, const D3DSURFACE_DESC *d, void *bits, int pitch)
{
    if (g_rSelf || d->Format != D3DFMT_DXT5) return;
    int kind = 0;
    if (d->Width == 64 && d->Height == 64 && pitch == 256) kind = 1;
    else if (d->Width == 512 && d->Height == 512 && pitch == 2048) kind = 2;
    if (!kind) return;
    int size = d->Width * d->Height;
    unsigned c = Crc((const BYTE *)bits, size);
    if (kind == 1) {
        int hit = 0;
        for (unsigned i = 0; i < sizeof g_retCrc / sizeof g_retCrc[0]; i++) if (g_retCrc[i] == c) hit = 1;
        if (!hit) return;
    } else if (c != HUD02_CRC) return;

    EnterCriticalSection(&g_rLock);
    int i;
    for (i = 0; i < g_nrt; i++) if (g_rt[i].tex == tex) break;
    if (i == g_nrt) {
        if (g_nrt >= 24) { LeaveCriticalSection(&g_rLock); return; }
        g_nrt++; IDirect3DTexture9_AddRef(tex);
        g_rt[i].orig = (BYTE *)malloc(size); g_rt[i].alt = (BYTE *)malloc(size);
        Log("aim: %s texture %08X -> %p", kind == 1 ? "reticle" : "crosshair atlas", c, tex);
    }
    RTex *t = &g_rt[i];
    t->tex = tex; t->w = d->Width; t->h = d->Height; t->state = 0;
    memcpy(t->orig, bits, size);
    if (kind == 1) memcpy(t->alt, g_dotData, 4096);
    else {
        memcpy(t->alt, bits, size);
        for (int by = 296 / 4; by < 352 / 4; by++)
            for (int bx = 236 / 4; bx < 288 / 4; bx++) {
                BYTE *blk = t->alt + (by * (512 / 4) + bx) * 16;
                memset(blk, 0, 8);                 /* alpha endpoints 0, indices 0: transparent */
            }
    }
    LeaveCriticalSection(&g_rLock);
}

static void OnUpd(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
{
    if (g_rSelf) return;
    EnterCriticalSection(&g_rLock);
    for (int i = 0; i < g_nrt; i++)
        if ((IDirect3DBaseTexture9 *)g_rt[i].tex == src && g_rt[i].dst != dst) {
            if (g_rt[i].dst) IDirect3DBaseTexture9_Release(g_rt[i].dst);
            g_rt[i].dst = dst; IDirect3DBaseTexture9_AddRef(dst);
            g_rt[i].state = 0;
        }
    LeaveCriticalSection(&g_rLock);
}

/* ------------------------------------------------------------ overlay */
typedef struct { float x, y, z, rhw; DWORD c; } V;

static void DrawMarker(IDirect3DDevice9 *dev, float sx, float sy, float size)
{
    V v[24]; int n = 0;
    DWORD col = 0xE8FF3A2A, sh = 0x90000000;
    /* four inward-pointing triangles around the target */
    for (int pass = 0; pass < 2; pass++) {
        DWORD c = pass ? col : sh;
        float o = pass ? 0 : 1.5f, s2 = size * 0.45f;
        float pts[4][6] = {
            { sx, sy - size, sx - s2, sy - size - s2, sx + s2, sy - size - s2 },
            { sx, sy + size, sx + s2, sy + size + s2, sx - s2, sy + size + s2 },
            { sx - size, sy, sx - size - s2, sy + s2, sx - size - s2, sy - s2 },
            { sx + size, sy, sx + size + s2, sy - s2, sx + size + s2, sy + s2 } };
        for (int t = 0; t < 4; t++)
            for (int k = 0; k < 3; k++) {
                V *p = &v[n++];
                p->x = pts[t][k * 2] + o; p->y = pts[t][k * 2 + 1] + o; p->z = 0; p->rhw = 1; p->c = c;
            }
    }
    IDirect3DStateBlock9 *sb = NULL;
    if (FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &sb)) || !sb) return;
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, n / 3, v, sizeof(V));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DStateBlock9_Apply(sb);
    IDirect3DStateBlock9_Release(sb);
}

/* distance from the game camera to what is under the game's crosshair
   (player aim point, player+0x124), smoothed; 0 = unknown */
/* Aim camera convergence: how far down the game's aim ray (eye E, direction f) the first surface is. The aim
 * point the game stores for the reticle (player +0x124) is traced from the camera that was drawn, i.e. from
 * our shoulder camera, which itself looks at the convergence point: when that ray met the ground or a wall
 * close by, the camera turned toward it, the next trace hit closer still, and the view dropped to the ground
 * and stuck there for a moment. Our own ray along the game's aim (the line the shots follow) does not
 * depend on the drawn camera. One ray per frame, result one frame later. */
float AimConvergeDist(const float *eye, const float *f, float dt, DWORD me)
{
    static float cur;
    static DWORD h;
    static float hs[3];
    static BYTE buf[0x200];
    float L = -1.f;
    if (h) {
        int r = RayFetch(h, buf);
        if (r != -1) {
            if (r == 1) {
                DWORD hit = *(DWORD *)(buf + 0x6C);
                const float *hp = (const float *)(buf + (hit ? 0x54 : 0x24));
                float d[3] = { hp[0] - hs[0], hp[1] - hs[1], hp[2] - hs[2] };
                L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1.5f;
            }
            h = 0;
        }
    }
    if (!h) {
        float st[3], en[3];
        for (int j = 0; j < 3; j++) { st[j] = eye[j] + f[j] * 1.5f; en[j] = eye[j] + f[j] * 300.f; }
        memcpy(hs, st, 12);
        h = RaySubmit(st, en, me);
    }
    if (!(L > 0.f && L < 5000.f)) return cur;
    if (L < 3.f) L = 3.f;
    if (L > 300.f) L = 300.f;
    if (cur <= 0) cur = L;
    float k = dt * 8.f; if (k > 1) k = 1;
    cur += (L - cur) * k;
    return cur;
}

/* render camera of the last frame (set by the camera hook) */
float g_rEye[3], g_rQ[4], g_rFov;
volatile int g_rValid;

static int ProjM(const float *M, int tr, const float *X, float *nx, float *ny)
{
    float o[4];
    for (int j = 0; j < 4; j++)
        o[j] = tr ? X[0] * M[j * 4] + X[1] * M[j * 4 + 1] + X[2] * M[j * 4 + 2] + M[j * 4 + 3]
                  : X[0] * M[j] + X[1] * M[4 + j] + X[2] * M[8 + j] + M[12 + j];
    if (!(o[3] > 0.01f)) return 0;
    *nx = o[0] / o[3]; *ny = o[1] / o[3];
    return 1;
}

static int g_projMethod = -1;

/* clip-space position through the engine matrix (needed for depth tests) */

static int Project(const float *X, float aspect, float *nx, float *ny)
{
    float x = g_rQ[0], y = g_rQ[1], z = g_rQ[2], w = g_rQ[3];
    float f[3] = { 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y) };
    float r[3] = { 1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y) };
    float u[3] = { 2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x) };
    static int logged;
    int method = g_projMethod;
    BYTE *base = *(BYTE **)0xDFC2F8;
    if (method < 0 && ValidPtr(base)) {
        /* find the engine's view-projection matrix: the view centre must land at 0,0 */
        float T[3] = { g_rEye[0] + f[0] * 20, g_rEye[1] + f[1] * 20, g_rEye[2] + f[2] * 20 };
        for (int vp = 0; vp < 4 && method < 0; vp++)
            for (int tr = 0; tr < 2 && method < 0; tr++) {
                float a, b;
                if (ProjM((const float *)(base + vp * 0xE80 + 0xB70), tr, T, &a, &b) && fabsf(a) < 0.03f && fabsf(b) < 0.03f)
                    method = vp * 2 + tr;
            }
        static int tries;
        if (method < 0) { if (++tries < 120) return 0; method = 100; }
        g_projMethod = method;
    }
    if (method >= 0 && !logged) { logged = 1; Log("aim: marker projection %s", method == 100 ? "own (camera fov)" : "engine matrix"); }
    if (method != 100) return ProjM((const float *)(base + (method / 2) * 0xE80 + 0xB70), method & 1, X, nx, ny);
    float d[3] = { X[0] - g_rEye[0], X[1] - g_rEye[1], X[2] - g_rEye[2] };
    float zc = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    if (zc < 0.1f) return 0;
    float t = tanf(g_rFov * 0.5f);
    *nx = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (zc * t * aspect);
    *ny = (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) / (zc * t);
    return 1;
}

static void OnFrame(IDirect3DDevice9 *dev)
{
    DWORD now = GetTickCount();
    int fresh = (int)(now - g_lastUpd) < 200;

    /* reticle: grey dot while on foot and not aiming */
    int wantDot = g_on && g_dot && fresh && g_hipMode == 1;
    EnterCriticalSection(&g_rLock);
    for (int i = 0; i < g_nrt; i++) if (g_rt[i].state != wantDot) RUpload(&g_rt[i], wantDot);
    LeaveCriticalSection(&g_rLock);

    if (!fresh || !g_rValid) return;
    D3DVIEWPORT9 v;
    if (FAILED(IDirect3DDevice9_GetViewport(dev, &v)) || !v.Height) return;
    g_aspect = (float)v.Width / v.Height;
    float nx, ny;
    if (g_projMethod < 0) {      /* find the engine matrix early (needed for depth tests) */
        float x = g_rQ[0], y = g_rQ[1], z = g_rQ[2], w = g_rQ[3];
        float T[3] = { g_rEye[0] + 30 * 2 * (x * z + w * y), g_rEye[1] + 30 * 2 * (y * z - w * x), g_rEye[2] + 30 * (1 - 2 * (x * x + y * y)) };
        Project(T, (float)v.Width / v.Height, &nx, &ny);
    }
    if (g_redirOn && g_markOn) Redirect();
    if (!g_markOn) return;
    float X[3] = { g_markPos[0], g_markPos[1] + 0.25f, g_markPos[2] };
    if (!Project(X, (float)v.Width / v.Height, &nx, &ny)) return;
    if (nx < -1 || nx > 1 || ny < -1 || ny > 1) return;
    float sx = v.X + (nx + 1) * 0.5f * v.Width, sy = v.Y + (1 - ny) * 0.5f * v.Height;
    float size = v.Height * 0.012f;
    if (size < 6) size = 6;
    DrawMarker(dev, sx, sy, size);
}

void AimInit(void)
{
    LoadCfg();
    if (*(DWORD *)ANIM_VT_UPD == 0x691730) {
        DWORD f = (DWORD)AnimUpdThunk;
        WriteMem(ANIM_VT_UPD, &f, 4);
    } else Log("aim: animation component code mismatch, chest bones off");
    InitializeCriticalSection(&g_rLock);
    MakeDot();
    D3DSetTextureCallback(OnTex);
    D3DSetUpdateCallback(OnUpd);
    D3DAddFrameCallback(OnFrame);
    Log("aim: assist %s (hip %d, ads %d, gamepad only %d), hip dot %d", g_on ? "on" : "off", g_hipOn, g_adsOn, g_padOnly, g_dot);
}

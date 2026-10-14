/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * Gamepad look speed
 *
 * The game converts every stick direction to a byte (0..255) in
 * FUN_00829130: value = rescaled(deflection) * JoystickSensitivity, clamped
 * to 255. The in-game slider (0.55x..1.5x) therefore only changes how early
 * full speed is reached; the top turn speed is fixed by the camera data
 * (YawSpeed/PitchSpeed), which is why "max sensitivity" still feels slow.
 *
 * The PC build also has a mouse-look path with no such cap. We drive it from
 * the right stick: after the pad is read (call at 0x4FB2F2) the bytes coming
 * from the right stick (XInput binding codes 4..7) are cleared, and after the
 * PC input merge (call at 0x4FA570 from 0x4FB4F9) the stick is added to the
 * per-player mouse delta (0x14A3880 / 0x14A3884 + player*0x80) with our own
 * curve, speed and dead zone, integrated over real frame time.
 */
#include "m2r.h"
#include <math.h>

#define REC_BASE   0x14A28BC          /* joystick records, stride 0x390 */
#define REC_STRIDE 0x390
#define PAD_BASE   0x14A3840          /* per-player merged input, stride 0x80 */

static int   g_on;
static float g_speed, g_speedY, g_curve, g_dead, g_mult;
static int   g_invert;
static float g_lx[4], g_ly[4];        /* pending look (normalised, curved) */
float g_padRX, g_padRY;              /* player 0 right stick, -1..1, shaped (for turrets) */
int   g_padRActive;
static int   g_have[4];
static float g_accX[4], g_accY[4];
static LARGE_INTEGER g_qpf, g_last[4];

typedef void (__stdcall *MergeFn)(int);
static const MergeFn o_Merge = (MergeFn)0x4FA570;

static float Shape(float v)
{
    float a = fabsf(v);
    if (a <= g_dead) return 0.f;
    a = (a - g_dead) / (1.f - g_dead);
    if (a > 1.f) a = 1.f;
    a = powf(a, g_curve);
    return v < 0 ? -a : a;
}

/* called after FUN_00829130(record, out) */
/* While the PDA is open the d-pad and the shoulder buttons give the console's button numbers, whatever actions
 * they are bound to: the PDA's cheat codes are sequences of PAD1 U/D/L/R (1..4), ALT1_2 (18, LB) and ALT2_2
 * (21, RB). On PC logical button i reads the pad button bound to the action that owns it (FUN_004FBD40:
 * SecondaryAttack -> 18, PrimarySwitch -> 21, Up/Down/Right/SecondarySwitch -> 1/2/4/3), so with changed
 * bindings the codes would need other buttons. Other logical buttons on those six pad buttons are cleared,
 * so each press is one ButtonPress. XInput button codes: d-pad U 0x13 D 0x12 L 0x10 R 0x11, LB 0x18, RB 0x19. */
static void PdaButtons(BYTE *rec, BYTE *out)
{
    static const BYTE logical[6] = {1, 2, 3, 4, 18, 21}, code[6] = {0x13, 0x12, 0x10, 0x11, 0x18, 0x19};
    static const WORD mask[6] = {0x0001, 0x0002, 0x0004, 0x0008, 0x0100, 0x0200};   /* XINPUT_GAMEPAD wButtons */
    const DWORD *tbl = (const DWORD *)0xB92A14;      /* logical button -> binding slot */
    WORD b = *(WORD *)(rec + 0x360);
    for (int i = 1; i <= 25; i++) {
        BYTE c = rec[0x374 + tbl[i - 1]];
        for (int k = 0; k < 6; k++) if (c == code[k]) out[i] = 0;
    }
    for (int k = 0; k < 6; k++) out[logical[k]] = (b & mask[k]) ? 0xFF : 0;
}

static void __cdecl JoyPost(BYTE *rec, BYTE *out)
{
    int idx = (int)((DWORD)rec - REC_BASE) / REC_STRIDE;
    if (idx < 0 || idx > 3 || !out) return;
    if (*(int *)(rec + 0x358) < 0) { g_have[idx] = 0; return; }     /* not an XInput pad */
    { int CheatsPdaOpen(void); if (CheatsPdaOpen()) PdaButtons(rec, out); }
    if (!g_on) return;

    const DWORD *tbl = (const DWORD *)0xB92A14;
    int rsBound = 0;
    for (int i = 1; i <= 25; i++) {
        BYTE code = rec[0x374 + tbl[i - 1]];
        if (code >= 4 && code <= 7) { out[i] = 0; rsBound = 1; }
    }
    if (!rsBound) { g_have[idx] = 0; if (idx == 0) g_padRActive = 0; return; }
    SHORT rx = *(SHORT *)(rec + 0x368), ry = *(SHORT *)(rec + 0x36A);
    float x = rx / 32767.f, y = ry / 32767.f;
    /* radial dead zone, then per-axis curve */
    float m = sqrtf(x * x + y * y);
    if (m <= g_dead) { x = y = 0; }
    g_lx[idx] = Shape(x);
    g_ly[idx] = Shape(y);
    if (idx == 0) { g_padRX = g_lx[0]; g_padRY = g_ly[0]; g_padRActive = (g_lx[0] != 0 || g_ly[0] != 0); }
    g_have[idx] = 1;
    float sens = *(float *)(rec + 0x33C);            /* in-game slider, 0.55..1.5 */
    if (sens > 0.1f && sens < 3.f) { g_lx[idx] *= sens; g_ly[idx] *= sens; }
}

/* replaces `call 0x829130` at 0x4FB2F2: eax = record, [esp+4] = out */
static void *g_joyTarget = (void *)0x829130;

__attribute__((naked)) static void JoyThunk2(void)
{
    __asm__ volatile (
        "push %%eax\n\t"
        "push 8(%%esp)\n\t"
        "mov 4(%%esp), %%eax\n\t"
        "call *%0\n\t"
        "pop %%ecx\n\t"
        "push %%eax\n\t"
        "push 8(%%esp)\n\t"
        "push %%ecx\n\t"
        "call %P1\n\t"
        "add $8, %%esp\n\t"
        "pop %%eax\n\t"
        "ret $4\n\t"
        :: "m"(g_joyTarget), "i"(JoyPost));
}

/* replaces `call 0x4FA570` at 0x4FB4F9 */
void PickupMerge(int idx);
static void __stdcall MergeWrap(int idx)
{
    o_Merge(idx);
    if (idx < 0 || idx > 3) return;
    PickupMerge(idx);
    if (!g_on) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float dt = g_last[idx].QuadPart ? (float)(now.QuadPart - g_last[idx].QuadPart) / g_qpf.QuadPart : 0.f;
    g_last[idx] = now;
    if (dt > 0.1f) dt = 0.1f;
    if (!g_have[idx]) { g_accX[idx] = g_accY[idx] = 0; return; }

    float sx = g_speed * g_mult, sy = g_speed * g_speedY * g_mult;
    g_accX[idx] += g_lx[idx] * sx * dt;
    g_accY[idx] += g_ly[idx] * sy * (g_invert ? -1.f : 1.f) * dt;
    int dx = (int)g_accX[idx], dy = (int)g_accY[idx];
    g_accX[idx] -= dx; g_accY[idx] -= dy;
    if (dx || dy) {
        int *pad = (int *)(PAD_BASE + idx * 0x80);
        pad[0x40 / 4] += dx;
        pad[0x44 / 4] += dy;
    }
}

void PadInit(void)
{
    QueryPerformanceFrequency(&g_qpf);
    float dz = IniFloat("Gamepad", "Deadzone", 10.f);
    if (dz >= 0.f && dz < 60.f) {
        /* game's own per-axis dead zone byte (default 0x28 = 15.7%) for all pad records */
        BYTE b = (BYTE)(dz * 255.f / 100.f + 0.5f);
        static const DWORD sites[] = {0x828646, 0x8289D2};       /* mov byte ptr [reg+0x344], 0x28 */
        for (int i = 0; i < 2; i++)
            if (Expect(sites[i] + 6, "28")) WriteMem(sites[i] + 6, &b, 1);
        Log("pad: stick dead zone %.1f%%", dz);
    }

    g_on = IniInt("Gamepad", "StickLook", 1);
    if (!g_on) {                                   /* the merge hook also serves the automatic pickup, the pad read the PDA */
        if (Expect(0x4FB4F9, "E8 72 F0 FF FF")) WriteCall(0x4FB4F9, (void *)MergeWrap, 5);
        if (Expect(0x4FB2F2, "E8 39 DE 32 00")) WriteCall(0x4FB2F2, (void *)JoyThunk2, 5);
        return;
    }
    g_speed  = IniFloat("Gamepad", "LookSpeed", 1500.f);
    g_speedY = IniFloat("Gamepad", "LookSpeedY", 0.65f);
    g_curve  = IniFloat("Gamepad", "LookCurve", 1.8f);
    g_mult   = IniFloat("Gamepad", "LookSensitivity", 1.0f);
    g_dead   = IniFloat("Gamepad", "LookDeadzone", 12.f) / 100.f;
    g_invert = IniInt("Gamepad", "LookInvertY", 0);
    if (g_curve < 0.5f) g_curve = 0.5f;
    if (g_dead < 0.f || g_dead > 0.6f) g_dead = 0.12f;

    if (Expect(0x4FB2F2, "E8 39 DE 32 00") && Expect(0x4FB4F9, "E8 72 F0 FF FF")) {
        WriteCall(0x4FB2F2, (void *)JoyThunk2, 5);
        WriteCall(0x4FB4F9, (void *)MergeWrap, 5);
        Log("pad: right-stick look via mouse path, speed %.0f (y %.2f) curve %.2f mult %.2f",
            g_speed, g_speedY, g_curve, g_mult);
    } else Log("pad: call sites mismatch, stick look not installed");
}

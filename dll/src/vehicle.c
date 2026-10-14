/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Vehicle handling assist.
 *
 * _CarPhysicsV2 templates (0x18C bytes) are registered through FUN_0064A600
 * (edi = table 0x17BC290, args: name id, template*). Two call sites:
 *   0x638FB9  binary database load
 *   0x659977  text schema load
 * We intercept both and adjust the steering-response fields only; mass,
 * engine torque, top speed, gearing, brakes and suspension stay untouched.
 *
 * Template layout (offsets from the schema loader at 0x658F60):
 *   +04 Mass  +08 SteerMaxAngle  +0C SteerMaxVisualAngle
 *   +10 SteerSpeed  +14 SteeringInertia  +18 SteerNoInputInertia  +1C SteerDiffSignInertia
 *   +20 LowSpeedSteerSpeed  +24 LowSpeedLimit
 *   +28 HighSpeedSteerLoss  +2C ..BrakeMulti  +30 ..NoThrottleMulti
 *   +34 LinearDampening  +38/3C/40 AngDampeningXYZ  +44 MaxSpeed  +48 EngineTorque
 *   +4C HiSpeedGrip  +6C FrontWheelFrictionSide  +94 RearWheelFrictionSide
 *   +130 LowSpeedSteerBoost
 */
#include "m2r.h"
#include <math.h>

#define REG_FN 0x64A600

static float g_steerSpeed, g_inertia, g_hsLoss;
static float g_bSteer, g_bInertia, g_bHsLoss;
static int   g_style;
static float g_yawDamp, g_frontGrip, g_rearGrip, g_hiGrip, g_comDrop;

#define F(p, o) (*(float *)((BYTE *)(p) + (o)))

/* binary _CarPhysicsV2 record (0x18C bytes):
 *   +00 Mass  +04 SteerMaxAngle  +08 SteerMaxVisualAngle  +0C SteerSpeed
 *   +10 SteeringInertia  +14 SteerNoInputInertia  +18 SteerDiffSignInertia
 *   +1C LowSpeedSteerSpeed  +20 LowSpeedLimit  +24 HighSpeedSteerLoss
 *   +34/38/3C AngDampening X/Y(yaw)/Z  +40 MaxSpeed  +44 EngineTorque  +48 HiSpeedGrip
 *   +68 FrontWheelFrictionSide  +90 RearWheelFrictionSide  +108 GravityFactor
 *   +114.. InertiaTensor  +120 CenterOfMassOffset  +130.. Bike* (nonzero = bike) */
/* A template registered again without being reloaded must not be tweaked twice: templates are remembered with
 * their tweaked steering speed (memory is reused, so a different value at a known address is a new template). */
static struct { BYTE *t; float v; } g_done[4096];
static void __cdecl CarTweak(DWORD name, BYTE *t)
{
    if (!t) return;
    DWORD slot = (((DWORD)(UINT_PTR)t >> 4) * 2654435761u) & 4095;
    if (g_done[slot].t == t && g_done[slot].v == F(t, 0x0C)) return;
    int bike = F(t, 0x130) > 0.f || F(t, 0x134) > 0.f || F(t, 0x140) > 0.f;
    int heavy = F(t, 0) > 10000.f;                  /* tanks, trucks */
    float sp = bike ? g_bSteer : g_steerSpeed, in = bike ? g_bInertia : g_inertia, hl = bike ? g_bHsLoss : g_hsLoss;
    F(t, 0x0C) *= sp;                                /* steering turn rate */
    for (int o = 0x10; o <= 0x18; o += 4) {          /* per-step smoothing (retention 0..1) */
        float r = F(t, o);
        if (r > 0.0f && r < 1.0f && in > 0.05f) F(t, o) = powf(r, 1.0f / in);
    }
    float h = F(t, 0x24);                            /* share of steering lost at top speed */
    if (h > 0.0f && h <= 1.0f) F(t, 0x24) = h * hl;

    /* arcade handling for cars: more grip, rear-biased so the car stays planted on asphalt
     * (more front than rear grip, less yaw damping and more steering at speed made cars oversteer on roads) */
    if (g_style && !bike && !heavy) {
        if (F(t, 0x38) > 0) F(t, 0x38) *= g_yawDamp;
        if (F(t, 0x68) > 0) F(t, 0x68) *= g_frontGrip;
        if (F(t, 0x90) > 0) F(t, 0x90) *= g_rearGrip;
        if (F(t, 0x48) > 0) F(t, 0x48) *= g_hiGrip;
        float c = F(t, 0x124) - g_comDrop;
        F(t, 0x124) = c;
    }
    g_done[slot].t = t; g_done[slot].v = F(t, 0x0C);
}

/* replaces "call 0x64A600": stack = [ret][name][tmpl], edi = table */
static void *g_tweakPtr;

__attribute__((naked)) static void CarRegStub2(void)
{
    __asm__ volatile(
        "cmp $0x17BC290, %%edi\n\t"
        "jne 1f\n\t"
        "pushal\n\t"
        "pushl 0x28(%%esp)\n\t"
        "pushl 0x28(%%esp)\n\t"
        "call *%0\n\t"
        "add $8, %%esp\n\t"
        "popal\n"
        "1:\n\t"
        "push $0x64A600\n\t"
        "ret\n\t"
        :: "m"(g_tweakPtr));
}

void VehicleInit(void)
{
    if (!IniInt("Vehicles", "Enable", 1)) return;
    g_steerSpeed = IniFloat("Vehicles", "SteerSpeed", 1.3f);
    g_inertia    = IniFloat("Vehicles", "SteerInertia", 0.7f);
    g_hsLoss     = IniFloat("Vehicles", "HighSpeedSteerLoss", 0.9f);
    g_bSteer     = IniFloat("Vehicles", "BikeSteerSpeed", 1.0f);
    g_bInertia   = IniFloat("Vehicles", "BikeSteerInertia", 1.0f);
    g_bHsLoss    = IniFloat("Vehicles", "BikeHighSpeedSteerLoss", 0.8f);
    g_style      = IniInt("Vehicles", "ArcadeHandling", 1);
    g_yawDamp    = IniFloat("Vehicles", "YawDamping", 1.1f);
    g_frontGrip  = IniFloat("Vehicles", "FrontGrip", 1.1f);
    g_rearGrip   = IniFloat("Vehicles", "RearGrip", 1.35f);
    g_hiGrip     = IniFloat("Vehicles", "HighSpeedGrip", 1.15f);
    g_comDrop    = IniFloat("Vehicles", "LowerCenterOfMass", 0.12f);
    g_tweakPtr   = (void *)CarTweak;

    int n = 0;
    if (Expect(0x638FB4, "BF 90 C2 7B 01 E8 42 16 01 00")) { WriteCall(0x638FB9, CarRegStub2, 5); n++; }
    if (Expect(0x659972, "BF 90 C2 7B 01 E8")) {
        DWORD rel = *(DWORD *)0x659978;
        if (0x65997C + rel == REG_FN) { WriteCall(0x659977, CarRegStub2, 5); n++; }
    }
    Log("Vehicles: %d hook(s), cars: steer x%.2f smoothing x%.2f hs-loss x%.2f | bikes: steer x%.2f smoothing x%.2f hs-loss x%.2f | arcade %d (yaw damp x%.2f, grip front x%.2f rear x%.2f, hs grip x%.2f, CoM -%.2f)",
        n, g_steerSpeed, g_inertia, g_hsLoss, g_bSteer, g_bInertia, g_bHsLoss, g_style, g_yawDamp, g_frontGrip, g_rearGrip, g_hiGrip, g_comDrop);
}

/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Automatic ammo pickup ([Gameplay] AutoAmmoPickup).
 *
 * Walking over a dropped gun whose ammo type matches one of the player's guns takes its ammo straight into that
 * gun's reserve and removes the dropped gun - no button, no animation. A gun at its reserve limit takes nothing
 * and the dropped gun stays; other dropped guns are left alone.
 *
 * The gun is found by the game's own context action logic: the on-foot context action is chosen every frame in
 * FUN_006A0770 and shown through FUN_006A0C40 (esi = the player's context controller, +0x88 action id, +0x8C
 * target object); 3 (TakeAmmo) is offered for a gun lying within reach that uses ammo of one of the player's guns.
 * Data:
 *   inventory 0x17BF3D8 [character]: gun in hand +0, second gun +4, holstered gun +0xC (see Guns)
 *   weapon instances 0x17BEC08 [gun]: +0x18 / +0x1C reserve upgrade, +0x24 reserve (u16), +0x26 kind (& 3: 0 gun),
 *     +0x30 rounds in the clip (u16)
 *   weapon templates 0x17BC778 [gun]: +0x08 AmmoTemplate, +0x1E MaxAmmoReserve, +0x26 rounds per reserve unit,
 *     +0x27 MaxAmmoReserveModifier
 *   reserve upgrades 0x17BCEF8 [instance +0x18]: +8 upgrade count
 * The reserve limit is computed as Weapon.GetMaxReserveAmmo does (FUN_005EA930), and the gun is removed as
 * Object.Remove does (FUN_005CDC00 -> FUN_004F2A00). */
#include "m2r.h"

#define ACT_AMMO 3
#define T_INV    0x17BF3D8
#define T_INST   0x17BEC08
#define T_TMPL   0x17BC778
#define T_UPG    0x17BCEF8

static int g_on;
static BYTE *volatile g_ctx;
extern volatile DWORD g_playerGuid;

static void *g_ctxFn = (void *)0x6A0C40;
__attribute__((naked)) static void CtxStub(void)
{
    __asm__ volatile(
        "mov %%esi, %0\n\t"
        "jmp *%1\n\t" : "=m"(g_ctx) : "m"(g_ctxFn));
}

/* FUN_005857E0: ecx = component table, eax = object -> its record or NULL */
static BYTE *Rec(DWORD table, DWORD obj)
{
    BYTE *r;
    if (!obj) return NULL;
    __asm__ volatile("call *%3" : "=a"(r) : "a"(obj), "c"(table), "r"(0x5857E0) : "edx", "memory", "cc");
    return r;
}
static void Remove(DWORD obj)
{
    __asm__ volatile("push $0\n\tcall *%1\n\tadd $4, %%esp"
                     : "+a"(obj) : "r"(0x4F2A00) : "ecx", "edx", "memory", "cc");
}
/* FUN_00526F30: eax = character; slot, mode, fallback -> gun object */
static DWORD Fetch(DWORD pl, int slot, int mode, DWORD fb)
{
    DWORD g;
    __asm__ volatile("push %2\n\tpush %3\n\tpush %4\n\tcall *%5\n\tadd $12, %%esp"
                     : "=a"(g) : "a"(pl), "r"(fb), "r"(mode), "r"(slot), "r"(0x526F30) : "ecx", "edx", "memory", "cc");
    return g;
}
/* the player's guns, as the ammo display (FUN_005EE1E0, every frame) finds them: in hand +0 (else
 * FUN_00526F30(0, 1, +0xC)), second gun +4 (else (1, 1, +0x10)), the holstered one +0xC (else (0, 0, +0)).
 * A gun being picked up is already one of them: it must not be emptied and removed. */
static int Guns(DWORD pl, const DWORD *inv, DWORD *out)
{
    DWORD c[3];
    c[0] = inv[0] ? inv[0] : Fetch(pl, 0, 1, inv[3]);
    c[1] = inv[1] ? inv[1] : Fetch(pl, 1, 1, inv[4]);
    c[2] = inv[3] ? inv[3] : Fetch(pl, 0, 0, inv[0]);
    int n = 0;
    for (int i = 0; i < 3; i++) {
        int dup = !c[i];
        for (int j = 0; j < n && !dup; j++) dup = out[j] == c[i];
        if (!dup) out[n++] = c[i];
    }
    return n;
}
static int MaxReserve(DWORD gun, BYTE *inst, BYTE *tmpl)
{
    int max = *(short *)(tmpl + 0x1E), mult = *(signed char *)(tmpl + 0x27);
    if (*(DWORD *)(inst + 0x1C)) {
        BYTE *u = Rec(T_UPG, *(DWORD *)(inst + 0x18));
        if (u) max += *(int *)(u + 8) * mult;
    }
    (void)gun;
    return max;
}

/* called after the input merge for player idx (pad.c), on the game thread */
void PickupMerge(int idx)
{
    static DWORD done;                                  /* the gun just emptied (its removal is deferred) */
    if (!g_on || idx != 0) return;
    BYTE *c = g_ctx;
    if (!c || IsBadReadPtr(c + 0x88, 8)) return;
    int act = *(int *)(c + 0x88);
    if (act != ACT_AMMO) return;
    DWORD gun = *(DWORD *)(c + 0x8C), pl = g_playerGuid;
    if (!gun || !pl || gun == done) return;
    BYTE *gi = Rec(T_INST, gun), *gt = Rec(T_TMPL, gun), *inv = Rec(T_INV, pl);
    if (!gi || !gt || !inv || (gi[0x26] & 3)) return;
    int gper = *(signed char *)(gt + 0x26) > 0 ? *(signed char *)(gt + 0x26) : 1;
    int rounds = *(WORD *)(gi + 0x24) * gper + *(WORD *)(gi + 0x30);       /* reserve units + loaded rounds */
    if (rounds <= 0) return;
    DWORD ammo = *(DWORD *)(gt + 8);
    DWORD guns[3]; int ng = Guns(pl, (const DWORD *)inv, guns);
    for (int s = 0; s < ng; s++) if (guns[s] == gun) return;     /* being picked up: it is the player's own now */
    for (int s = 0; s < ng; s++) {
        DWORD mine = guns[s];
        BYTE *mi = Rec(T_INST, mine), *mt = Rec(T_TMPL, mine);
        if (!mi || !mt || (mi[0x26] & 3) || *(DWORD *)(mt + 8) != ammo) continue;
        int res = *(WORD *)(mi + 0x24), max = MaxReserve(mine, mi, mt);
        if (res >= max) continue;                       /* full: nothing happens */
        int per = *(signed char *)(mt + 0x26) > 0 ? *(signed char *)(mt + 0x26) : 1;
        int have = (rounds + per / 2) / per;
        if (have < 1) have = 1;
        int add = have < max - res ? have : max - res;
        *(WORD *)(mi + 0x24) = (WORD)(res + add);
        static int logged;
        if (logged++ < 3) Log("pickup: %d reserve units from %08lX into %08lX (reserve %d -> %d of %d, ammo %08lX)", add, gun, mine, res, res + add, max, ammo);
        done = gun;
        Remove(gun);
        return;
    }
}

void PickupInit(void)
{
    g_on = IniInt("Gameplay", "AutoAmmoPickup", 1);
    if (!g_on) return;
    if (!Expect(0x6A099E, "E8 9D 02 00 00") || !Expect(0x4F2A00, "FF 25 D4 F3 45 02")) { Log("pickup: unexpected code, off"); g_on = 0; return; }
    WriteCall(0x6A099E, (void *)CtxStub, 5);
    Log("pickup: automatic ammo pickup on");
}

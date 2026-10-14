/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* PC cheat codes: typing a word while the PDA is open enters the matching console code.
 *
 * The ported TU4 PDA script (MrxGuiPda) records every ButtonPress of the PDA's ControllerInput events in
 * tControlHistory and compares it with its codes when the PDA closes (_tCheatEnable first, then the cheats once
 * Pg.WereCheatsUsed). With a keyboard the engine sends no ButtonPress events except one: the GUI mouse click,
 * sent as button 6 by the input event builder FUN_006AB290 (block at 0x6AB467, value read from the float
 * constant 0xDFDB60). That block is reused here to send the code's buttons one per input update, so the
 * script handles them exactly like a gamepad entering the code. Gamepad entry is unchanged.
 * PDA open/closed: the script calls _GuiInternal.RegisterForPdaUpdate(uId, true / false) on Open / Close
 * (binding 0x5BC730, table entry 0xB9A368).
 * Words: SASSY (enable cheats), NOTTODAYSOLANO, TRIGGERHAPPY, FILLERUP, AIRDIVA, MAKEITGLOW, CARGOCULT, THATSMYCAR,
 * FASHIONVICTIM, HOOKMEUP. */
#include "m2r.h"
#include "d3dhook.h"
#include <string.h>

enum { U = 1, D = 2, L = 3, R = 4, LB = 18, RB = 21 };
typedef struct { const char *key, *def; BYTE seq[12]; char word[24]; } Code;
static Code g_codes[] = {
    {"Enable",       "SASSY",    {LB, RB, RB, LB, RB, LB, LB, RB, RB, RB, LB}, {0}},
    {"Invincible",   "NOTTODAYSOLANO",   {U, D, L, D, R, R}, {0}},
    {"InfiniteAmmo", "TRIGGERHAPPY",      {U, D, L, R, L, L}, {0}},
    {"Fuel",         "FILLERUP",      {U, U, U, D, D, D}, {0}},
    {"Airstrikes",   "AIRDIVA", {R, L, D, U, R, L, D, U}, {0}},
    {"Nuke",         "MAKEITGLOW",      {U, U, D, D, L, R, L, R}, {0}},
    {"Supplies",     "CARGOCULT",  {L, R, R, L, U, U, L, U}, {0}},
    {"Vehicles",     "THATSMYCAR",  {U, D, L, R, R, L}, {0}},
    {"Costumes",     "FASHIONVICTIM",  {U, R, D, L, U}, {0}},
    {"Grapple",      "HOOKMEUP",   {U, L, D, R, U}, {0}},
};
#define NCODE (int)(sizeof g_codes / sizeof g_codes[0])

static volatile int g_pdaOpen;
static char g_typed[32]; static int g_nTyped;
static BYTE g_queue[12]; static volatile int g_qn, g_qi;

/* ---- the input event builder: one queued button per update ---- */
DWORD g_cheatBtn;                       /* float bits of the button to send, 0 = none */
float g_cheatVal = 6.f;                 /* the value the block sends (6 = mouse click, as the game) */
__attribute__((naked)) static void InjectStub(void)
{
    __asm__ volatile(
        "mov _g_cheatBtn, %eax\n\t"
        "test %eax, %eax\n\t"
        "jz 1f\n\t"
        "movl $0, _g_cheatBtn\n\t"
        "mov %eax, _g_cheatVal\n\t"
        "push $0x6AB47B\n\t"                    /* build the ButtonPress entry */
        "ret\n"
        "1:\n\t"
        "movl $0x40C00000, _g_cheatVal\n\t"     /* 6.0 */
        "cmp %ebp, 0x1176034\n\t"
        "je 2f\n\t"
        "push $0x6AB46F\n\t"
        "ret\n"
        "2:\n\t"
        "push $0x6AB4CF\n\t"
        "ret");
}

/* ---- PDA open / closed ---- */
typedef struct { DWORD v; DWORD tt; } TV;
#define LTOP(Ls)  (*(TV **)((BYTE *)(Ls) + 8))
#define LBASE(Ls) (*(TV **)((BYTE *)(Ls) + 0xC))
typedef int (__cdecl *LFn)(void *);
static LFn o_RegPda;
static int __cdecl RegPda(void *Ls)
{
    TV *b = LBASE(Ls), *t = LTOP(Ls);
    if (t - b >= 2) {
        int open = !(b[1].tt == 0 || (b[1].tt == 1 && b[1].v == 0));
        if (open != g_pdaOpen) { g_pdaOpen = open; g_nTyped = 0; if (!open) g_qn = g_qi = 0; }
        static int logged;
        if (open && !logged) { logged = 1; Log("cheats: PDA open (d-pad left %s it)", *(BYTE *)0xCFB963 ? "does not close" : "closes"); }
    }
    return o_RegPda(Ls);
}

int CheatsPdaOpen(void) { return g_pdaOpen; }

static int Foreground(void)
{
    DWORD pid = 0; HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static void OnFrame(IDirect3DDevice9 *d)
{
    (void)d;
    static BYTE down[26];
    if (g_qi < g_qn && !g_cheatBtn) {                   /* next button of the code */
        float f = (float)g_queue[g_qi++]; memcpy(&g_cheatBtn, &f, 4);
    }
    int fg = Foreground();
    for (int k = 0; k < 26; k++) {
        int dn = fg && (GetAsyncKeyState('A' + k) & 0x8000);
        if (dn && !down[k] && g_pdaOpen && g_qi >= g_qn) {
            if (g_nTyped == (int)sizeof g_typed) { memmove(g_typed, g_typed + 1, sizeof g_typed - 1); g_nTyped--; }
            g_typed[g_nTyped++] = (char)('A' + k);
            for (int c = 0; c < NCODE; c++) {
                int n = (int)strlen(g_codes[c].word);
                if (n && n <= g_nTyped && !memcmp(g_typed + g_nTyped - n, g_codes[c].word, n)) {
                    int m = 0; while (m < 12 && g_codes[c].seq[m]) m++;
                    memcpy(g_queue, g_codes[c].seq, m); g_qi = 0; g_qn = m; g_nTyped = 0;
                    Log("cheats: %s typed, close the PDA to apply", g_codes[c].key);
                    break;
                }
            }
        }
        down[k] = (BYTE)dn;
    }
}

void CheatsInit(void)
{
    for (int c = 0; c < NCODE; c++) strcpy(g_codes[c].word, g_codes[c].def);
    if (!Expect(0x6AB467, "39 2D 34 60 17 01 74 60 8B 0D 74 45 ED 00") || !Expect(0x6AB4AE, "F3 0F 10 05 60 DB DF 00") ||
        !Expect(0xB9A368, "F8 5C BB 00 30 C7 5B 00")) { Log("cheats: unexpected code, typed cheats off"); return; }
    WriteJmp(0x6AB467, (void *)InjectStub, 8);
    WritePtr(0x6AB4B2, &g_cheatVal);
    o_RegPda = (LFn)0x5BC730;
    WritePtr(0xB9A36C, (const void *)RegPda);
    D3DAddFrameCallback(OnFrame);
    Log("cheats: type a code word in the PDA, then close it");
}

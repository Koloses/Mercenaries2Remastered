/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Movies, and the game's own error log.
 *
 * d3d.log mirror: the game's error log (FUN_0074D310, printf-style; d3d.log is overwritten every run) is also
 * written to Mercenaries2Remastered.log, so its movie, texture and device messages are kept with the mod's (the
 * last five runs are kept). Repeats of the same line are counted instead of written again.
 *
 * Black videos with sound ([Fixes] MovieFix): the four movie player slots (0x17C64F0, 0x16C each) share the three
 * MovieBink texture resources the interface draws (0xF77EDC). Skipping a movie (FUN_00709740) sets the stop
 * request of every slot, also the idle ones, and the stop (FUN_0070A140, run from the player update at 0x7097C1)
 * returns the shared resources' textures to the pool and clears them. When the intro script starts the next movie
 * in slot 0 before the idle slots' stop requests are handled, those stops take the new movie's textures away: it
 * keeps decoding and playing its sound, but the interface has nothing to draw (the second intro after skipping
 * the first one). A stop request for a slot that is not playing anything (no Bink handle, no surfaces) now only
 * marks it stopped.
 *
 * Pooled textures of the wrong size: the texture pool (FUN_0085CBD0) files free textures under the power-of-two
 * exponents of their width and height, so a request for 1920x1080 could get back a free 1024x1024 texture of the
 * same format. The movie player then clears and decodes a full 1920x1080 frame into it and writes past the end
 * (crash when the main menu movie starts after leaving a DLC race). A pooled texture is now handed out only when
 * its size matches; otherwise a new one is created. */
#include "m2r.h"
#define COBJMACROS
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ---- d3d.log mirror ---- */
void __cdecl GameLog(const char *fmt, va_list ap)
{
    static char last[512]; static int rep;
    char b[512];
    if (!fmt || IsBadReadPtr(fmt, 1)) return;
    _vsnprintf(b, sizeof b - 1, fmt, ap); b[sizeof b - 1] = 0;
    size_t n = strlen(b);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    if (!strcmp(b, last)) { rep++; return; }
    if (rep) Log("game: (previous line %d more times)", rep);
    rep = 0;
    lstrcpynA(last, b, sizeof last);
    Log("game: %s", b);
}
__attribute__((naked)) static void GameLogStub(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "lea 40(%esp), %eax\n\t"          /* the arguments after the format */
        "push %eax\n\t"
        "pushl 40(%esp)\n\t"              /* the format */
        "call _GameLog\n\t"
        "add $8, %esp\n\t"
        "popal\n\t"
        "mov 4(%esp), %ecx\n\t"           /* the replaced instructions */
        "jmp *0x24554E8");
}

/* ---- stop requests of idle player slots ---- */
typedef void (__stdcall *Stop_t)(BYTE *player);
static Stop_t o_Stop = (Stop_t)0x70A140;
static void __stdcall StopSlot(BYTE *p)          /* p = slot + 4 (the player: +0x2C active, +0x34 Bink, +0x38 surfaces) */
{
    if (!*(DWORD *)(p + 0x34) && !*(DWORD *)(p + 0x38) && !*(DWORD *)(p + 0x3C) && !*(DWORD *)(p + 0x40)) {
        *(DWORD *)(p + 0x2C) = 0;
        return;
    }
    o_Stop(p);
}

/* ---- texture pool: exact sizes only ---- */
void *__cdecl PoolCheck(IDirect3DTexture9 *t, UINT w, UINT h)
{
    D3DSURFACE_DESC d;
    if (IsBadReadPtr(t, 4) || FAILED(IDirect3DTexture9_GetLevelDesc(t, 0, &d))) return t;
    if (d.Width == w && d.Height == h) return t;
    static int n;
    if (n++ < 8) Log("texture pool: free %ux%u texture not used for a %ux%u request", d.Width, d.Height, w, h);
    return NULL;                                     /* the caller creates one of the right size */
}
__attribute__((naked)) static void PoolFindStub(void)
{
    __asm__ volatile(
        "pushl 12(%esp)\n\t"
        "pushl 12(%esp)\n\t"
        "pushl 12(%esp)\n\t"
        "mov $0x85C970, %ecx\n\t"
        "call *%ecx\n\t"                         /* eax = format (in), free texture or 0 (out) */
        "add $12, %esp\n\t"
        "test %eax, %eax\n\t"
        "jz 1f\n\t"
        "pushl 44(%esp)\n\t"                     /* requested height (the caller's second argument) */
        "push %ebp\n\t"                          /* requested width */
        "push %eax\n\t"
        "call _PoolCheck\n\t"
        "add $12, %esp\n\t"
        "1:\n\t"
        "ret");
}

void MovieInit(void)
{
    if (Expect(0x85CC66, "8B C3 E8 03 FD FF FF 83 C4 0C")) { WriteCall(0x85CC68, (void *)PoolFindStub, 5); Log("texture pool: exact sizes"); }
    else Log("texture pool: site mismatch");
    if (Expect(0x74D310, "8B 4C 24 04 FF 25 E8 54 45 02")) WriteJmp(0x74D310, (void *)GameLogStub, 10);
    else Log("movie: game log site mismatch, d3d.log not mirrored");
    if (!IniInt("Fixes", "MovieFix", 1)) return;
    if (Expect(0x7097C1, "E8 7A 09 00 00")) { WriteCall(0x7097C1, (void *)StopSlot, 5); Log("movie: idle slot stop fix on"); }
    else Log("movie: stop request site mismatch, fix off");
}

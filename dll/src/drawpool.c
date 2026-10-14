/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Per-frame draw tables of the renderer, sized for the stock view distances.
 *
 * Every model part drawn in a frame gets a 64-byte matrix slot from a static pool of 2 x 0x1400 slots at
 * 0x18C71E0 (one half per frame in flight, 0xFF464C picks the half, the word counter 0xFF46CC hands out slots and
 * is reset to 2 at the frame start). Nothing checks the counter: past 5120 slots a frame writes into the other
 * frame's half (still being drawn) and past that into the object at 0x19671E0, so parts are drawn with other
 * parts' matrices: stretched triangles and pieces of the world repeated in the wrong place. Shader constants
 * (2 x 128 KB, offset 0xFF46C8) and the draw command words (2 x 320 KB, index 0xFF46D4) are not checked either.
 * The draw item list (2 x 0x2000 items of 0x58 bytes at 0xFF4700, sort keys 0x2000 x 8 bytes at 0x1154700)
 * is checked (FUN_008546A0) and silently drops every draw past 8192. With more trees and grass in view the
 * list fills, and whatever is submitted last is lost: a model whose depth pre-pass got in but whose colour
 * pass did not leaves its shape filled with whatever the scene target held from an earlier pass (pieces of
 * the world repeated inside tree outlines). The stock distances stay below all of these limits.
 *
 * The draw list and sort keys are moved to 0x8000 items (draw indices are 16-bit, 0xFFFF ends a sort chain):
 * The matrix pool is moved to a larger one (every code reference: 81 addresses, 31 slot strides, 3 half
 * strides), the two heap buffers are allocated 4x larger (FUN_00853C70), and the peak use is logged. */
#include "m2r.h"

#define OLD_BASE 0x18C71E0u
#define OLD_CAP  0x1400u
#define NEW_CAP  0x8000u                        /* slot indices are 16-bit words */

static const DWORD g_pool[][2] = {{0x46f86c,0x18c71e0},{0x472cf3,0x18c71e0},{0x472d72,0x18c71e0},{0x477924,0x18c71e0},{0x477e65,0x18c71e0},{0x477f2f,0x18c71e0},{0x477fef,0x18c71e0},{0x478765,0x18c71e0},{0x47f996,0x18c71e0},{0x47fce2,0x18c71e0},{0x4875fd,0x18c71e0},{0x487e70,0x18c71e0},{0x48d9ed,0x18c71e0},{0x493b80,0x18c71e0},{0x4954e1,0x18c71e0},{0x49a4fe,0x18c71e0},{0x49e4f3,0x18c71e0},{0x4a3dda,0x18c71e0},{0x4a5879,0x18c71e0},{0x4a8bb5,0x18c71e0},{0x4a8ce4,0x18c71e0},{0x4a8dd7,0x18c71e0},{0x4aaa2e,0x18c71e0},{0x4aaf75,0x18c71e0},{0x4ab1e9,0x18c71e0},{0x4ae821,0x18c71e0},{0x4ae99a,0x18c71e0},{0x4aeb67,0x18c71e0},{0x6887c2,0x18c71e0},{0x852f16,0x18c71e0},{0x85431a,0x18c723c},{0x85432a,0x18c7240},{0x85433a,0x18c7244},{0x85434a,0x18c7248},{0x85435a,0x18c724c},{0x85436a,0x18c7250},{0x85437a,0x18c7254},{0x85438a,0x18c7258},{0x85439a,0x18c7220},{0x8543a2,0x18c7224},{0x8543aa,0x18c7228},{0x8543b2,0x18c722c},{0x8543ba,0x18c7230},{0x8543c2,0x18c7234},{0x8543ca,0x18c7238},{0x8543d2,0x18c725c},{0x8543d8,0x18c7220},{0x8543de,0x18c71e0},{0x8543e4,0x18c7224},{0x8543ea,0x18c71e4},{0x8543f0,0x18c7228},{0x8543f6,0x18c71e8},{0x8543fc,0x18c722c},{0x854402,0x18c71ec},{0x854408,0x18c7230},{0x85440e,0x18c71f0},{0x854414,0x18c7234},{0x85441a,0x18c71f4},{0x854420,0x18c7238},{0x854426,0x18c71f8},{0x85442c,0x18c723c},{0x854432,0x18c71fc},{0x854439,0x18c7240},{0x854441,0x18c7200},{0x854448,0x18c7244},{0x854457,0x18c7204},{0x854463,0x18c7248},{0x85446f,0x18c7208},{0x854476,0x18c724c},{0x85447c,0x18c720c},{0x854482,0x18c7250},{0x854488,0x18c7210},{0x85448e,0x18c7254},{0x854494,0x18c7214},{0x85449a,0x18c7258},{0x8544a0,0x18c7218},{0x8544a6,0x18c725c},{0x8544ac,0x18c721c},{0x854bb9,0x18c71e0},{0x854c29,0x18c71e4},{0x854e48,0x18c71e4}};
static const DWORD g_stride[] = {0x46f73e,0x472ce6,0x472d5e,0x4778f0,0x477e4e,0x477f1d,0x477fdd,0x47875b,0x47f986,0x47fcc2,0x48759c,0x487e0f,0x48d9c3,0x493b6d,0x4954d4,0x49a4f1,0x49e4ab,0x4a3dcc,0x4a5869,0x4a8b9e,0x4a8cd4,0x4a8dbc,0x4aaa1e,0x4aaf68,0x4ab1dc,0x4ae814,0x4ae98d,0x4aeb5a,0x6887b5,0x852f05,0x854e3e};
static const DWORD g_half[] = {0x854312,0x854ba8,0x854c1e};

#define OLD_DRAWS   0xFF4700u
#define OLD_KEYS    0x1154700u
#define NEW_DRAWS_N 0x8000u
/* sort key references (disp32 = 0x1154700 + k) */
static const DWORD g_keys[] = {0x854976,0x8549f7,0x85496f,0x85497e,0x854a03,0x854a25,0x854a35,0x854a6b,0x854a97,0x854ac3,0x854aef,0x856124,0x854992};

static int DrawListInit(void)
{
    size_t i;
    for (i = 0; i < sizeof g_keys / 4; i++) { DWORD v = *(DWORD *)g_keys[i]; if (v < OLD_KEYS - 2 || v > OLD_KEYS + 6) return 0; }
    if (!Expect(0x8546A6, "81 F9 00 20 00 00") || !Expect(0x8546F5, "C1 E0 0D") || !Expect(0x854701, "05 00 47 FF 00") ||
        !Expect(0x854B96, "69 C9 00 00 0B 00") || !Expect(0x854BAC, "81 C1 00 47 FF 00")) return 0;
    BYTE *draws = VirtualAlloc(NULL, 2 * NEW_DRAWS_N * 0x58, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BYTE *keys = VirtualAlloc(NULL, NEW_DRAWS_N * 8 + 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!draws || !keys) return 0;
    for (i = 0; i < sizeof g_keys / 4; i++) { DWORD v = (DWORD)(UINT_PTR)keys + (*(DWORD *)g_keys[i] - OLD_KEYS); WriteMem(g_keys[i], &v, 4); }
    DWORD cap = NEW_DRAWS_N, half = NEW_DRAWS_N * 0x58, base = (DWORD)(UINT_PTR)draws;
    BYTE sh = 15;                               /* frame << 15 = frame * 0x8000 */
    WriteMem(0x8546A8, &cap, 4);
    WriteMem(0x8546F7, &sh, 1);
    WriteMem(0x854702, &base, 4);
    WriteMem(0x854B98, &half, 4);
    WriteMem(0x854BAE, &base, 4);
    return 1;
}

/* Objects drawn this frame (FUN_008544D0 registers an object once per frame: list 0x1164770, 0xC00 per frame,
 * counter word 0x1164704, slot kept in object +0x180, 0xFFFF = none; draw items refer to the slot). No check
 * either: past 3072 objects the list runs into the other frame's list and then over 0x116A770.. (the render
 * thread's per-frame table pointers), so draws pick other objects' data. */
#define OLD_OBJS   0x1164770u
#define NEW_OBJS_N 0x4000u
static int ObjListInit(void)
{
    if (!Expect(0x853ED1, "69 C0 00 30 00 00") || !Expect(0x853ED7, "05 70 47 16 01") || !Expect(0x8544E7, "69 D2 00 0C 00 00") ||
        !Expect(0x8544F3, "89 0C 95 70 47 16 01") || !Expect(0x854B9E, "69 D2 00 30 00 00") || !Expect(0x854BB2, "81 C2 70 47 16 01")) return 0;
    BYTE *l = VirtualAlloc(NULL, 2 * NEW_OBJS_N * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!l) return 0;
    DWORD base = (DWORD)(UINT_PTR)l, n = NEW_OBJS_N, half = NEW_OBJS_N * 4;
    WriteMem(0x853ED3, &half, 4); WriteMem(0x853ED8, &base, 4);
    WriteMem(0x8544E9, &n, 4);    WriteMem(0x8544F6, &base, 4);
    WriteMem(0x854BA0, &half, 4); WriteMem(0x854BB4, &base, 4);
    return 1;
}

/* Occlusion queries: a ring of 1024 (device +0x2E8C, created by FUN_0074A5xx, released at 0x74A83E, read
 * through 0x1164768). Every object tested in a frame takes the next one (0x46F924: (n + 1) & 0x3FF) and reads
 * its result when it is drawn again. With more objects tested per frame than the ring holds over the frames a
 * result is in flight, a query is handed to another object before it was read: objects get other objects'
 * pixel counts, are taken as hidden and skip their colour pass (their depth is already in), leaving holes that
 * keep the previous frame's image. The ring is moved to 16384 queries. */
#define NEW_QUERIES 16384u
static void *g_queries[NEW_QUERIES];
static int QueryRingInit(void)
{
    if (!Expect(0x74A5FB, "8D BE 8C 2E 00 00") || !Expect(0x74A601, "BB 00 04 00 00") || !Expect(0x74A83E, "81 C6 8C 2E 00 00") ||
        !Expect(0x74A844, "BF 00 04 00 00") || !Expect(0x853DE3, "81 C2 8C 2E 00 00") || !Expect(0x46F92D, "25 FF 03 00 80") ||
        !Expect(0x46F93C, "0D 00 FC FF FF")) return 0;
    DWORD a = (DWORD)(UINT_PTR)g_queries, n = NEW_QUERIES, mask = 0x80000000u | (NEW_QUERIES - 1), neg = ~(NEW_QUERIES - 1);
    BYTE c[6];
    c[0] = 0xBF; memcpy(c + 1, &a, 4); c[5] = 0x90; WriteMem(0x74A5FB, c, 6);    /* mov edi, g_queries */
    WriteMem(0x74A602, &n, 4);
    c[0] = 0xBE; WriteMem(0x74A83E, c, 6);                                       /* mov esi, g_queries */
    WriteMem(0x74A845, &n, 4);
    c[0] = 0xBA; WriteMem(0x853DE3, c, 6);                                       /* mov edx, g_queries */
    WriteMem(0x46F92E, &mask, 4);
    WriteMem(0x46F93D, &neg, 4);
    return 1;
}

/* Scene grid (FUN_00469490 builds it, FUN_0046A000 inserts, FUN_00469F10 walks a cell list): every placed
 * object is linked into each 1/0x10 cell it overlaps, up to 16, from a pool of 0x4000 nodes {object, next}.
 * Taking a node does not check for an empty pool: with more objects loaded (longer type distances) the free
 * list runs out, a node at index -1 is written and a cell list ends up with an empty node, which the gather
 * dereferences (crash at 0x469F25). The pool is made 16 times larger. */
static int SceneGridInit(void)
{
    if (!Expect(0x4694AD, "B8 00 40 00 00") || !Expect(0x4694E4, "C7 47 18 00 40 00 00")) return 0;
    DWORD n = 0x40000;
    WriteMem(0x4694AE, &n, 4); WriteMem(0x4694E7, &n, 4);
    return 1;
}

static volatile LONG g_pkQ;
static volatile LONG g_pkObj, g_pkInst, g_pkInst2;
static volatile LONG g_pkMat, g_pkDraw, g_pkConst, g_pkCmd;
static void __cdecl Record(void)
{
    LONG m = *(volatile WORD *)0xFF46CC, d = *(volatile LONG *)0xFF46F0, c = *(volatile LONG *)0xFF46C8, k = *(volatile LONG *)0xFF46D4;
    if (m > g_pkMat) g_pkMat = m;
    if (d > g_pkDraw) g_pkDraw = d;
    if (c > g_pkConst) g_pkConst = c;
    if (k > g_pkCmd) g_pkCmd = k;
    LONG o = *(volatile WORD *)0x1164704, i1 = *(volatile WORD *)0xFF46E0, i2 = *(volatile WORD *)0x116A770;
    if (o > g_pkObj) g_pkObj = o;
    if (i1 > g_pkInst) g_pkInst = i1;
    if (i2 > g_pkInst2) g_pkInst2 = i2;
    static LONG qPrev;
    LONG q = *(volatile LONG *)0x116A7C4, dq = (q - qPrev) & (NEW_QUERIES - 1);
    qPrev = q;
    if (dq > g_pkQ) g_pkQ = dq;
}
/* 0x853FBD, frame start, before the counters are reset: mov [0xFF46F0], esi */
__attribute__((naked)) static void FrameStub(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "call %P0\n\t"
        "popal\n\t"
        "movl %%esi, 0xFF46F0\n\t"
        "ret\n\t" :: "i"(Record));
}
void DrawPoolStatus(void)
{
    static LONG lm, ld, lc, lk, lo;
    if (g_pkMat == lm && g_pkDraw == ld && g_pkConst == lc && g_pkCmd == lk && g_pkObj == lo) return;
    lm = g_pkMat; ld = g_pkDraw; lc = g_pkConst; lk = g_pkCmd; lo = g_pkObj;
    Log("drawpool: peak per frame: %ld matrices (game limit %u), %ld draws (game limit 8192), %ld KB constants (game limit 128), %ld KB commands (game limit 320)",
        lm, OLD_CAP, ld, lc / 1024, lk * 4 / 1024);
    Log("drawpool: peak per frame: %ld objects (game limit 3072), instance slots %ld (game limit 3072), %ld occlusion queries (game ring 1024)", lo, (LONG)g_pkInst, (LONG)g_pkQ);
}

void DrawPoolInit(void)
{
    size_t i, n = sizeof g_pool / sizeof g_pool[0];
    for (i = 0; i < n; i++) if (*(DWORD *)g_pool[i][0] != g_pool[i][1]) break;
    int ok = i == n;
    for (i = 0; ok && i < sizeof g_stride / 4; i++) ok = *(DWORD *)g_stride[i] == OLD_CAP;
    for (i = 0; ok && i < sizeof g_half / 4; i++) ok = *(DWORD *)g_half[i] == OLD_CAP * 64;
    ok = ok && Expect(0x853CEE, "B8 00 00 05 00") && Expect(0x853D05, "68 00 00 05 00") && Expect(0x853D5E, "B8 00 00 02 00") &&
         Expect(0x853D75, "68 00 00 02 00") && Expect(0x853FBD, "89 35 F0 46 FF 00");
    if (!ok) { Log("drawpool: code mismatch, skipped"); return; }
    BYTE *pool = VirtualAlloc(NULL, 2 * NEW_CAP * 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pool) { Log("drawpool: no memory"); return; }
    for (i = 0; i < n; i++) { DWORD v = (DWORD)(UINT_PTR)pool + (g_pool[i][1] - OLD_BASE); WriteMem(g_pool[i][0], &v, 4); }
    DWORD cap = NEW_CAP, half = NEW_CAP * 64;
    for (i = 0; i < sizeof g_stride / 4; i++) WriteMem(g_stride[i], &cap, 4);
    for (i = 0; i < sizeof g_half / 4; i++) WriteMem(g_half[i], &half, 4);
    DWORD cmd = 0x50000 * 4, cst = 0x20000 * 4;
    WriteMem(0x853CEF, &cmd, 4); WriteMem(0x853D06, &cmd, 4);
    WriteMem(0x853D5F, &cst, 4); WriteMem(0x853D76, &cst, 4);
    WriteCall(0x853FBD, (void *)FrameStub, 6);
    Log("drawpool: matrix pool %u -> %u slots per frame, constants 512 KB, commands 1280 KB", OLD_CAP, NEW_CAP);
    if (DrawListInit()) Log("drawpool: draw list 8192 -> %u per frame", NEW_DRAWS_N);
    else Log("drawpool: draw list code mismatch, skipped");
    if (SceneGridInit()) Log("drawpool: scene grid nodes 16384 -> 262144");
    else Log("drawpool: scene grid code mismatch, skipped");
    if (QueryRingInit()) Log("drawpool: occlusion query ring 1024 -> %u", NEW_QUERIES);
    else Log("drawpool: occlusion query code mismatch, skipped");
    if (ObjListInit()) Log("drawpool: object list 3072 -> %u per frame", NEW_OBJS_N);
    else Log("drawpool: object list code mismatch, skipped");
}

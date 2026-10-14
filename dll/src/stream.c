/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * stream.c - resource streaming throughput.
 *
 * The streaming manager (*(0x1176630)) issues reads from its per-frame scheduler FUN_008739e0 only while
 *   in-flight requests  < byte  [mgr + 0x4c358]   (set to 1 at init, 0x872b55, never changed)
 *   in-flight bytes    <= dword [mgr + 0x4c350]   (2 MB at init; reloaded from DAT_01175c3c / DAT_01175c40
 *                                                 on state changes)
 * and completions are only collected in that same per-frame tick. With one request in flight the game
 * streams exactly one file request per frame (60 reads/s at 60 fps): a DVD-era policy. The single I/O
 * thread still serves requests in priority order; allowing several in flight only removes the idle frame
 * between them.
 *
 * [Performance] StreamConcurrent (default 16, 1 = game), StreamInFlightMB (default 64, 2 = game)
 */
#include <windows.h>
#include <d3d9.h>

void Log(const char *fmt, ...);
int IniInt(const char *sec, const char *key, int def);
void D3DAddFrameCallback(void (*cb)(IDirect3DDevice9 *));

#define STREAM_MGR      ((BYTE **)0x1176630)
#define OFF_MAXREQ      0x4c358
#define OFF_MAXBYTES    0x4c350
#define CFG_BYTES_A     ((DWORD *)0x1175c3c)
#define CFG_BYTES_B     ((DWORD *)0x1175c40)

static int g_req; static DWORD g_bytes;

static void OnFrame(IDirect3DDevice9 *d)
{
    (void)d;
    BYTE *m = *STREAM_MGR;
    if (!m || IsBadWritePtr(m + OFF_MAXBYTES, 12)) return;
    if (m[OFF_MAXREQ] < (BYTE)g_req) m[OFF_MAXREQ] = (BYTE)g_req;
    if (*(DWORD *)(m + OFF_MAXBYTES) < g_bytes) *(DWORD *)(m + OFF_MAXBYTES) = g_bytes;
    if (*CFG_BYTES_A && *CFG_BYTES_A < g_bytes) *CFG_BYTES_A = g_bytes;
    if (*CFG_BYTES_B && *CFG_BYTES_B < g_bytes) *CFG_BYTES_B = g_bytes;
}

void StreamInit(void)
{
    g_req = IniInt("Performance", "StreamConcurrent", 16);
    int mb = IniInt("Performance", "StreamInFlightMB", 64);
    if (g_req < 1) g_req = 1;
    if (g_req > 64) g_req = 64;
    if (mb < 2) mb = 2;
    if (mb > 256) mb = 256;
    g_bytes = (DWORD)mb << 20;
    if (g_req <= 1 && mb <= 2) return;
    D3DAddFrameCallback(OnFrame);
    Log("stream: up to %d reads / %d MB in flight", g_req, mb);
}

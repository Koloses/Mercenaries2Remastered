/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * CPU usage fixes
 *
 * 1) Job worker thread (CreateThread(0x876400) from 0x876834) polls its job
 *    queues forever and, when idle, calls Sleep(0) at 0x87650C. On modern
 *    Windows Sleep(0) returns immediately when nothing else is ready, so that
 *    thread burns a whole core even in the menus. It is also the render
 *    thread (the scene and all D3D calls run as its jobs), so it only sleeps
 *    once it has been idle for a while.
 *
 * 2) Spin-waits on Sleep(0) (GPU query, job results) yield, and sleep only once a wait is long.
 *
 * 3) Tree billboards (0x4A1990): six vertices per tree are written into a locked dynamic vertex buffer, and
 *    each vertex's height was then raised by reading it back from that buffer (addss xmm1,[eax+0xC]) -
 *    a read from write-combined memory, hundreds of cycles, for every vertex of every distant tree. The
 *    value is the one just copied from [esi+0xC], so it is read from there instead.
 */
#include "m2r.h"
#include <mmsystem.h>

static LARGE_INTEGER g_qpf;

/* Waiting threads keep yielding (SwitchToThread / pause) for the first [Performance] IdleSpinMs of a wait
 * (default 20) and only then sleep 1 ms per pass. The render job worker and the main thread hand work to each
 * other several times a frame; a Sleep(1) inside one of those hand-offs (it ends 1-2 ms later) delays the
 * whole frame. Waits longer than a frame are real idle time (pause, loading), where sleeping saves the core. */
static LONGLONG g_spinTicks;

/* replaces `call [Sleep]` at 0x87650C (argument is always 0) */
static void WINAPI WorkerIdle(DWORD ms)
{
    static LONGLONG idleStart, last;
    LARGE_INTEGER now;
    (void)ms;
    QueryPerformanceCounter(&now);
    /* more than 0.25 ms since the previous idle call -> the worker ran a job: a new wait */
    if ((now.QuadPart - last) * 4000 > g_qpf.QuadPart) idleStart = now.QuadPart;
    if (now.QuadPart - idleStart < g_spinTicks) { if (!SwitchToThread()) YieldProcessor(); }
    else Sleep(1);
    QueryPerformanceCounter(&now);
    last = now.QuadPart;
}

/* replaces Sleep(0) in two spin-waits:
 *  0x74A960  loop on IDirect3DQuery9::GetData == S_FALSE (waiting for the GPU)
 *  0x8766E9  main thread waiting for job results it cannot run itself */
static __thread LONGLONG t_waitStart, t_waitLast;
static void WINAPI SpinWait(DWORD ms)
{
    LARGE_INTEGER now;
    (void)ms;
    QueryPerformanceCounter(&now);
    if ((now.QuadPart - t_waitLast) * 5000 > g_qpf.QuadPart) t_waitStart = now.QuadPart;   /* >0.2 ms gap: new wait */
    if (now.QuadPart - t_waitStart >= g_spinTicks) Sleep(1);
    else if (!SwitchToThread()) YieldProcessor();
    QueryPerformanceCounter(&now);
    t_waitLast = now.QuadPart;
}
static void *g_spinWaitPtr = (void *)SpinWait;

void PerfInit(void)
{
    QueryPerformanceFrequency(&g_qpf);
    int spin = IniInt("Performance", "IdleSpinMs", 20);
    if (spin < 0) spin = 0;
    if (spin > 200) spin = 200;
    g_spinTicks = g_qpf.QuadPart * spin / 1000;

    if (IniInt("Performance", "WorkerIdleFix", 1)) {
        if (Expect(0x876500, "6A 00 C7 82 40 90 9F 01 00 00 00 00 FF 15 8C 51 B0 00")) {
            timeBeginPeriod(1);
            WriteCall(0x87650C, (void *)WorkerIdle, 6);
            Log("perf: job worker idle fix on");
        } else Log("perf: worker idle site mismatch, skipped");
    }
    if (Expect(0x4A1A82, "F3 0F 10 89 40 C8 BA 00 F3 0F 58 48 0C F3 0F 11 48 0C")) {
        static const BYTE m = 0x4E;                      /* addss xmm1,[eax+0xC] -> addss xmm1,[esi+0xC] */
        WriteMem(0x4A1A8D, &m, 1);
        Log("perf: tree billboard vertex read-back removed");
    } else Log("perf: tree billboard code mismatch, skipped");
    if (IniInt("Performance", "SpinWaitFix", 1)) {
        timeBeginPeriod(1);
        if (Expect(0x74A954, "8B 3D 8C 51 B0 00 8D 9B 00 00 00 00 6A 00 FF D7")) {
            WritePtr(0x74A956, &g_spinWaitPtr);              /* mov edi, [Sleep] -> mov edi, [&SpinWait] */
            Log("perf: GPU query wait fix on");
        } else Log("perf: GPU query wait site mismatch, skipped");
        if (Expect(0x8766E7, "6A 00 FF 15 8C 51 B0 00")) {
            WriteCall(0x8766E9, (void *)SpinWait, 6);
            Log("perf: main thread job wait fix on");
        } else Log("perf: job wait site mismatch, skipped");
    }
}

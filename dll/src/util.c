/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
#include "m2r.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

char g_gameDir[MAX_PATH];
static char g_iniPath[MAX_PATH];
/* The log is written by its own thread: Log() only appends to a memory buffer. A write to the log file can
 * block (another program holding the file, a network or shared folder), and the game thread that logged then
 * froze with it. When the writer falls behind the buffer drops lines and says how many. */
static CRITICAL_SECTION g_logLock, g_logIo;
static HANDLE g_logFile = INVALID_HANDLE_VALUE, g_logEvt;
#define LOG_BUF (1 << 20)
static char g_logBuf[LOG_BUF], g_logOut[LOG_BUF];
static int g_logLen, g_logDrop;

static const char g_defaultIni[] =
"; Mercenaries2Remastered settings (Beta 1)\r\n"
"; Most values are read at start; [Camera], [Turret] and [AimAssist] are re-read while the game runs.\r\n"
"\r\n"
"[Display]\r\n"
"; borderless window covering the monitor instead of exclusive fullscreen\r\n"
"Borderless=1\r\n"
"; no blurry stretching on scaled desktops\r\n"
"DpiAware=1\r\n"
"; FPS counter (FpsKey toggles it, 0x78 = F9)\r\n"
"FpsCounter=0\r\n"
"FpsKey=0x78\r\n"
#ifdef M2R_SHOT
"; screenshot key (0x7A = F11, Shift + key = comparison of the same frame without / with the post effects),\r\n"
"; saved in the Screenshots folder (developer builds)\r\n"
"ScreenshotKey=0x7A\r\n"
#endif
"\r\n"
"[Graphics]\r\n"
"; forced anisotropic filtering (0 = game default)\r\n"
"Anisotropic=16\r\n"
"; SMAA on the finished frame\r\n"
"SMAA=1\r\n"
"; Xbox 360 rendering: shader fixes (normal maps, shadow filtering on vegetation / decals / water, lighting\r\n"
"; without the PC clamps), linear HDR bloom, soft particles, fire on burning vegetation and wrecks, model\r\n"
"; shadows of far trees, the 1.1 paper animations\r\n"
"XboxShaders=1\r\n"
"LinearBloom=1\r\n"
"SoftParticles=1\r\n"
"EmitterFix=1\r\n"
"ModeledTreeShadows=1\r\n"
"PaperAnimations=1\r\n"
"; Xbox depth-aware edge anti-aliasing: -1 = only while the game's Anti-Aliasing is off (the Xbox has no MSAA),\r\n"
"; 1 = always, 0 = off\r\n"
"DepthAA=-1\r\n"
"; draw distances: the defaults are tested; higher values load more of the world at once and can run the game\r\n"
"; out of memory or into engine limits (crashes, freezes, missing objects) and cost frame rate\r\n"
"; draw distance of grass, bushes and leaves (x game)\r\n"
"GrassDistanceScale=3.0\r\n"
"; distance at which trees are 3D models instead of billboards (x game)\r\n"
"TreeDistanceScale=3.0\r\n"
"; draw distance of objects / of long-range object types (x game)\r\n"
"ObjectDistanceScale=2.0\r\n"
"FarObjectDistanceScale=1.5\r\n"
"; distance at which models switch to coarser detail levels (x game)\r\n"
"LodDistanceScale=2.0\r\n"
"; distance around the camera the world is streamed in (x game)\r\n"
"WorldDistanceScale=2.0\r\n"
"; terrain detail distance (x game)\r\n"
"TerrainDetailScale=2.0\r\n"
"; far clip plane (x game); how far the near plane stays in front of the character (metres)\r\n"
"FarClipScale=2.0\r\n"
"NearPlaneMargin=0.3\r\n"
"\r\n"
"[Color]\r\n"
"; colour correction on the finished frame (ToggleKey switches it on/off to compare, 0x79 = F10)\r\n"
"Enabled=1\r\n"
"ToggleKey=0x79\r\n"
"; brightness in stops (+1 = twice as bright)\r\n"
"Exposure=0.0\r\n"
"Contrast=1.08\r\n"
"Saturation=1.05\r\n"
"; extra saturation for dull colours only\r\n"
"Vibrance=0.15\r\n"
"; midtones (>1 brighter), shadows lift (0..0.1), highlights gain\r\n"
"Gamma=1.0\r\n"
"Lift=0.0\r\n"
"Gain=1.0\r\n"
"; white balance: Temperature + warmer / - cooler, Tint + magenta / - green (about -1..1)\r\n"
"Temperature=0.0\r\n"
"Tint=0.0\r\n"
"Sharpen=0.3\r\n"
"Vignette=0.15\r\n"
"\r\n"
"[Fixes]\r\n"
"; the game's Anti-Aliasing option works in the borderless window and on the 3D scene\r\n"
"AAFix=1\r\n"
"; movies stay visible after skipping one\r\n"
"MovieFix=1\r\n"
"; stowed weapons, holsters and pouches no longer twitch above 30 fps\r\n"
"TwitchFix=1\r\n"
"\r\n"
"[Performance]\r\n"
"; the job worker and spin-waits sleep instead of burning a core; spin time before sleeping (ms)\r\n"
"WorkerIdleFix=1\r\n"
"SpinWaitFix=1\r\n"
"IdleSpinMs=20\r\n"
"; let Windows cache game data reads\r\n"
"CachedReads=1\r\n"
"; streaming: requests / megabytes in flight at once (game: 1 / 2)\r\n"
"StreamConcurrent=16\r\n"
"StreamInFlightMB=64\r\n"
"; larger memory pools (4 GB exe only): main heap and small block heap (MB)\r\n"
"TunePools=1\r\n"
"MainHeapMB=1024\r\n"
"SmallBlockMB=192\r\n"
"; skip objects smaller than this share of the screen in the depth / shadow / reflection passes (0 = off)\r\n"
"DepthCull=0.0\r\n"
"ShadowCull=0.02\r\n"
"ReflectionCull=0.05\r\n"
"; occlusion test in the depth pass; draw reflections every Nth frame\r\n"
"DepthPassOcclusion=1\r\n"
"ReflectionRate=2\r\n"
"; faster forest decoding, SecuROM detours removed from hot code, redundant D3D states dropped\r\n"
"FastForest=1\r\n"
"SecuromFix=1\r\n"
"StateFilter=1\r\n"
"; pre-compile the shaders used before at start (Mercenaries2Remastered\\shaders.bin)\r\n"
"ShaderWarm=1\r\n"
"\r\n"
"[Gamepad]\r\n"
"; right stick turns the camera through the mouse-look path (the game's own stick look is capped)\r\n"
"StickLook=1\r\n"
"; turn speed at full deflection (mouse counts per second); the in-game sensitivity slider also applies\r\n"
"LookSpeed=1500\r\n"
"LookSensitivity=1.0\r\n"
"; vertical speed relative to horizontal\r\n"
"LookSpeedY=0.65\r\n"
"; response curve: 1 = linear, higher = finer aim near the centre\r\n"
"LookCurve=1.8\r\n"
"; right stick dead zone (percent)\r\n"
"LookDeadzone=12\r\n"
"LookInvertY=0\r\n"
"; dead zone for all stick directions (percent, game 15.7)\r\n"
"Deadzone=10\r\n"
"\r\n"
"[Prompts]\r\n"
"; Xbox controller buttons in on-screen prompts while a gamepad is used\r\n"
"XboxPrompts=1\r\n"
"\r\n"
"[Camera]\r\n"
"; on-foot camera\r\n"
"Enable=1\r\n"
"; distance from the look-at point / extra height (metres)\r\n"
"Distance=4.2\r\n"
"Height=0.35\r\n"
"; how far below the game's look-at point to aim (higher = character higher on screen)\r\n"
"FocusDrop=0.6\r\n"
"; distance from the game's camera to its look-at point\r\n"
"GameFocusDist=3.0\r\n"
"; field of view (x game)\r\n"
"FovScale=1.1\r\n"
"; aiming: over-the-shoulder camera, metres back / to the right (negative = left shoulder) / up\r\n"
"AimBack=2.0\r\n"
"AimSide=0.4\r\n"
"AimHeight=0\r\n"
"; field of view while aiming (1 = game zoom, higher = less zoom)\r\n"
"AimFovScale=1.9\r\n"
"; the shoulder camera looks at what is under the reticle (1) or this many metres down the aim line\r\n"
"AimConvergeAuto=1\r\n"
"AimConverge=40\r\n"
"; blend time between the normal and the aim camera (seconds)\r\n"
"BlendTime=0.25\r\n"
"\r\n"
"[Turret]\r\n"
"; mounted guns: right stick drives the gun directly\r\n"
"Enable=1\r\n"
"StickSensitivity=1.0\r\n"
"SpeedScale=2.0\r\n"
"InvertX=0\r\n"
"InvertY=0\r\n"
"\r\n"
"[AimAssist]\r\n"
"Enable=1\r\n"
"; only while the gamepad is the active input\r\n"
"GamepadOnly=1\r\n"
"; hip fire: mark the enemy nearest the view centre and send the shots to it (the view is not turned)\r\n"
"HipFireAssist=1\r\n"
"HipCone=14\r\n"
"HipRange=45\r\n"
"HipRedirect=1\r\n"
"; grey dot instead of the weapon reticle when not aiming / red mark over the target\r\n"
"HipDot=1\r\n"
"MarkTarget=1\r\n"
"; aiming down the sight: snap to the enemy nearest the reticle, then a gentle magnet\r\n"
"AimDownSightAssist=1\r\n"
"SnapCone=20\r\n"
"SnapTime=0.08\r\n"
"Range=120\r\n"
"MagnetRadius=1.2\r\n"
"MagnetMinAngle=2.0\r\n"
"MagnetStrength=5.0\r\n"
"MagnetMaxRate=45\r\n"
"; skip enemies hidden behind walls\r\n"
"IgnoreHidden=1\r\n"
"\r\n"
"[Vehicles]\r\n"
"; handling; top speed, engine power and mass are never touched\r\n"
"Enable=1\r\n"
"; cars: steering turn rate / smoothing time (lower = snappier) / share of the stock steering loss at top speed\r\n"
"SteerSpeed=1.3\r\n"
"SteerInertia=0.7\r\n"
"HighSpeedSteerLoss=0.9\r\n"
"; motorbikes and quads\r\n"
"BikeSteerSpeed=1.0\r\n"
"BikeSteerInertia=1.0\r\n"
"BikeHighSpeedSteerLoss=0.8\r\n"
"; arcade handling for cars: more tyre grip (rear > front keeps the car stable), lower centre of mass\r\n"
"ArcadeHandling=1\r\n"
"YawDamping=1.1\r\n"
"FrontGrip=1.1\r\n"
"RearGrip=1.35\r\n"
"HighSpeedGrip=1.15\r\n"
"LowerCenterOfMass=0.12\r\n"
"\r\n"
"[Deformation]\r\n"
"; cars dent on impacts; windows break where a dent reaches them\r\n"
"Enabled=1\r\n"
"; dent depth multiplier / speed change (m/s within ~0.1 s) that counts as an impact / deepest dent (metres)\r\n"
"Strength=1.0\r\n"
"Threshold=4.0\r\n"
"MaxDepth=0.35\r\n"
"\r\n"
"[TyreMarks]\r\n"
"; skid marks, seconds they stay, slide speed (m/s) that leaves a mark\r\n"
"Enabled=1\r\n"
"Lifetime=120\r\n"
"SlipThreshold=3.5\r\n"
"\r\n"
"[Gameplay]\r\n"
"; walking over a dropped weapon that uses your ammo takes the ammo\r\n"
"AutoAmmoPickup=1\r\n"
"; answer the 'play online?' question with offline (the servers are gone)\r\n"
"SkipOnlinePrompt=1\r\n";

static DWORD WINAPI LogThread(LPVOID u);

void UtilInit(void)
{
    InitializeCriticalSection(&g_logLock);
    GetModuleFileNameA(NULL, g_gameDir, MAX_PATH);
    char *s = strrchr(g_gameDir, '\\'); if (s) *s = 0;

    char lp[MAX_PATH];
    _snprintf(lp, sizeof lp, "%s\\Mercenaries2Remastered.log", g_gameDir);
    {   /* the logs of the last five runs are kept (Mercenaries2Remastered.1.log = the previous run) */
        char a[MAX_PATH], b[MAX_PATH];
        for (int i = 5; i >= 1; i--) {
            if (i > 1) _snprintf(a, sizeof a, "%s\\Mercenaries2Remastered.%d.log", g_gameDir, i - 1); else lstrcpynA(a, lp, sizeof a);
            _snprintf(b, sizeof b, "%s\\Mercenaries2Remastered.%d.log", g_gameDir, i);
            if (GetFileAttributesA(a) != INVALID_FILE_ATTRIBUTES) MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING);
        }
    }
    g_logFile = CreateFileA(lp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        InitializeCriticalSection(&g_logIo);
        g_logEvt = CreateEventA(NULL, FALSE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, LogThread, NULL, 0, NULL));
    }

    _snprintf(g_iniPath, sizeof g_iniPath, "%s\\Mercenaries2Remastered.ini", g_gameDir);
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileA(g_iniPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w; WriteFile(h, g_defaultIni, sizeof g_defaultIni - 1, &w, NULL); CloseHandle(h);
        }
    }
}

void Log(const char *fmt, ...)
{
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char line[4096]; va_list ap;
    SYSTEMTIME st; GetLocalTime(&st);
    int n = _snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt); int k = _vsnprintf(line + n, sizeof line - n - 2, fmt, ap); va_end(ap);
    n = k < 0 || k > (int)sizeof line - n - 2 ? (int)sizeof line - 2 : n + k;
    line[n++] = '\r'; line[n++] = '\n';
    EnterCriticalSection(&g_logLock);
    if (g_logLen + n <= LOG_BUF) { memcpy(g_logBuf + g_logLen, line, n); g_logLen += n; } else g_logDrop++;
    LeaveCriticalSection(&g_logLock);
    SetEvent(g_logEvt);
}

/* writes what has been logged so far (log thread; crash logger) */
void LogFlush(void)
{
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_logIo);
    EnterCriticalSection(&g_logLock);
    int n = g_logLen, drop = g_logDrop;
    memcpy(g_logOut, g_logBuf, n); g_logLen = 0; g_logDrop = 0;
    LeaveCriticalSection(&g_logLock);
    DWORD w;
    if (drop) { char m[64]; int k = _snprintf(m, sizeof m, "[log: %d lines dropped]\r\n", drop); WriteFile(g_logFile, m, k, &w, NULL); }
    if (n) WriteFile(g_logFile, g_logOut, n, &w, NULL);
    LeaveCriticalSection(&g_logIo);
}

/* process exit: the log thread is gone; write the rest unless it died in the middle of a write */
void LogFinal(void)
{
    if (g_logFile == INVALID_HANDLE_VALUE || !TryEnterCriticalSection(&g_logIo)) return;
    if (TryEnterCriticalSection(&g_logLock)) { LeaveCriticalSection(&g_logLock); LogFlush(); }
    LeaveCriticalSection(&g_logIo);
}

static DWORD WINAPI LogThread(LPVOID u)
{
    (void)u;
    for (;;) { WaitForSingleObject(g_logEvt, 250); LogFlush(); }
    return 0;
}

int IniInt(const char *sec, const char *key, int def)
{
    return (int)GetPrivateProfileIntA(sec, key, def, g_iniPath);
}

float IniFloat(const char *sec, const char *key, float def)
{
    char b[64];
    GetPrivateProfileStringA(sec, key, "", b, sizeof b, g_iniPath);
    if (!b[0]) return def;
    return (float)atof(b);
}

int PatchIAT(HMODULE mod, void *orig, void *hook)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress || !orig) return 0;
    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress); d->Name; d++) {
        for (void **slot = (void **)(base + d->FirstThunk); *slot; slot++) {
            if (*slot != orig) continue;
            DWORD old;
            if (VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
                *slot = hook;
                VirtualProtect(slot, sizeof *slot, old, &old);
                patched++;
            }
        }
    }
    return patched;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int Expect(DWORD addr, const char *hex)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || (DWORD)mbi.BaseAddress + mbi.RegionSize < addr + 32)
        return 0;
    const BYTE *p = (const BYTE *)addr;
    while (*hex) {
        if (*hex == ' ') { hex++; continue; }
        if (hex[0] == '?') { p++; hex += (hex[1] == '?') ? 2 : 1; continue; }
        int hi = hexval(hex[0]), lo = hexval(hex[1]);
        if (hi < 0 || lo < 0) return 0;
        if (*p != (BYTE)(hi << 4 | lo)) return 0;
        p++; hex += 2;
    }
    return 1;
}

#ifdef M2R_SHOT
/* developer builds: [Debug] PatchLo / PatchHi (hex) limit which exe addresses are patched, to find a bad patch */
static int PatchAllowed(DWORD addr)
{
    static DWORD lo = 1, hi; static int skipped;
    if (lo == 1) {
        char b[32];
        IniStr("Debug", "PatchLo", "0", b, sizeof b); lo = strtoul(b, NULL, 16);
        IniStr("Debug", "PatchHi", "FFFFFFFF", b, sizeof b); hi = strtoul(b, NULL, 16);
        if (lo == 1) lo = 0;
        if (lo || hi != 0xFFFFFFFF) Log("debug: only exe patches in %08lX-%08lX", lo, hi);
    }
    if (addr < 0x400000 || addr >= 0x3800000 || (addr >= lo && addr < hi)) return 1;
    if (++skipped <= 400) Log("debug: patch at %08lX skipped", addr);
    return 0;
}
#endif

void WriteMem(DWORD addr, const void *src, size_t n)
{
#ifdef M2R_SHOT
    if (!PatchAllowed(addr)) return;
#endif
    DWORD old;
    VirtualProtect((void *)addr, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)addr, src, n);
    VirtualProtect((void *)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, n);
}

static void WriteRel(BYTE op, DWORD at, void *to, size_t padTo)
{
    BYTE b[32];
    size_t n = padTo < 5 ? 5 : padTo;
    if (n > sizeof b) n = sizeof b;
    b[0] = op;
    *(DWORD *)(b + 1) = (DWORD)to - (at + 5);
    for (size_t i = 5; i < n; i++) b[i] = 0x90;
    WriteMem(at, b, n);
}

void WriteJmp(DWORD at, void *to, size_t padTo)  { WriteRel(0xE9, at, to, padTo); }
void WriteCall(DWORD at, void *to, size_t padTo) { WriteRel(0xE8, at, to, padTo); }
void WritePtr(DWORD addr, const void *p)         { WriteMem(addr, &p, 4); }

/* the Mercenaries2.exe build this DLL was compiled for (M2R_GAME) */
int GameVersionOK(void)
{
    return Expect(0x876400, "55 8B EC 83 E4 F8 83 EC 1C 64 A1 2C 00 00 00") &&     /* job worker */
           Expect(0x828B14, "F3 0F 2A C0 F3 0F 59 05 48 87 B9 00");                /* joy sensitivity */
}

void IniStr(const char *sec, const char *key, const char *def, char *out, int n)
{
    GetPrivateProfileStringA(sec, key, def, out, n, g_iniPath);
}

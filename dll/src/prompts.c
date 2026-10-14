/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/*
 * Xbox button prompts
 *
 * The PC build draws prompt tags ([jump], [sfire], [move] ...) with the icon
 * atlases pc\common_18_buttons / pc\common_20_buttons (action pictures). The
 * game also ships the Xbox 360 atlases (common_*_xbox_buttons,
 * font_16_xbox_buttons) in vz.wad but never uses them on PC.
 *
 * When a gamepad is the active input we repaint every action icon in the PC
 * atlases with the Xbox glyph of the pad button bound to that action in
 * Mercs2.ini ([Controller ...] section), and swap font_16_buttons for its
 * Xbox twin (identical layout). Keyboard/mouse input switches back.
 */
#include "m2r.h"
#define COBJMACROS
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "d3dhook.h"
#include "dxt.h"
#include "prompts_types.h"
#include "promptsgfx.h"

#define MAX_TEX 16

typedef struct {
    IDirect3DTexture9 *tex;
    IDirect3DBaseTexture9 *dst;   /* GPU copy when tex is a staging (sysmem) texture */
    int      w, h, size;
    int      atlas;          /* index in g_atlases, or -1 for font_16 */
    uint8_t *orig, *pad;     /* DXT5 level 0 */
    int      state;          /* 0 = original, 1 = pad */
} Tracked;

static Tracked g_tex[MAX_TEX];
static int g_ntex;
static CRITICAL_SECTION g_lock;
static volatile int g_selfLock;
static volatile int g_padMode = -1;          /* -1 unknown, 0 kb/mouse, 1 pad */
static int g_glyphFor[ACT_COUNT];   /* xbox glyph per action, -1 = keep icon */
static FILETIME g_iniTime;
static int g_enabled;
static int g_mouseOK;     /* game mouse object address verified */

/* ---------------------------------------------------------- bindings */
static int GlyphFromBinding(const char *v)
{
    int n;
    if (sscanf(v, "Button %d", &n) == 1) {
        static const int b[] = {G_A, G_B, G_X, G_Y, G_LB, G_RB, G_BACK, G_START, G_LS_CLICK, G_RS_CLICK};
        return (n >= 1 && n <= 10) ? b[n - 1] : -1;
    }
    if (!_stricmp(v, "DPAD Up"))    return G_DPAD_UP;
    if (!_stricmp(v, "DPAD Down"))  return G_DPAD_DOWN;
    if (!_stricmp(v, "DPAD Left"))  return G_DPAD_LEFT;
    if (!_stricmp(v, "DPAD Right")) return G_DPAD_RIGHT;
    if (!_stricmp(v, "Left Stick - Up"))     return G_LS_UP;
    if (!_stricmp(v, "Left Stick - Down"))   return G_LS_DOWN;
    if (!_stricmp(v, "Left Stick - Left"))   return G_LS_LEFT;
    if (!_stricmp(v, "Left Stick - Right"))  return G_LS_RIGHT;
    if (!_stricmp(v, "Right Stick - Up"))    return G_RS_UP;
    if (!_stricmp(v, "Right Stick - Down"))  return G_RS_DOWN;
    if (!_stricmp(v, "Right Stick - Left"))  return G_RS_LEFT;
    if (!_stricmp(v, "Right Stick - Right")) return G_RS_RIGHT;
    if (!_stricmp(v, "Other Left Stick - Left"))  return G_LT;   /* triggers share the Z axis */
    if (!_stricmp(v, "Other Left Stick - Right")) return G_RT;
    return -1;
}

static int StickGlyph(int g)
{
    if (g == G_LS_UP || g == G_LS_DOWN || g == G_LS_LEFT || g == G_LS_RIGHT) return G_LS;
    if (g == G_RS_UP || g == G_RS_DOWN || g == G_RS_LEFT || g == G_RS_RIGHT) return G_RS;
    if (g >= G_DPAD && g <= G_DPAD_LR) return G_DPAD;
    return g;
}

/* returns 1 if bindings changed */
static int LoadBindings(void)
{
    char ini[MAX_PATH], secs[4096], sec[128] = "";
    _snprintf(ini, sizeof ini, "%s\\Mercs2.ini", g_gameDir);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(ini, GetFileExInfoStandard, &fa)) return 0;
    if (!CompareFileTime(&fa.ftLastWriteTime, &g_iniTime)) return 0;
    g_iniTime = fa.ftLastWriteTime;

    GetPrivateProfileSectionNamesA(secs, sizeof secs, ini);
    for (char *s = secs; *s; s += strlen(s) + 1)
        if (!_strnicmp(s, "Controller", 10)) { lstrcpynA(sec, s, sizeof sec); break; }

    int g[ACT_COUNT];
    for (int a = 0; a < ACT_COUNT; a++) {
        char v[64] = "";
        g[a] = -1;
        if (sec[0] && a != ACT_NONE && a != ACT_MOVE && a != ACT_LOOK) {
            GetPrivateProfileStringA(sec, g_actName[a], "", v, sizeof v, ini);
            g[a] = GlyphFromBinding(v);
        }
    }
    if (!sec[0]) {  /* no pad section yet: Xbox defaults of the PC version */
        g[ACT_JUMP] = G_A; g[ACT_MELEEATTACK] = G_B; g[ACT_RELOAD] = G_X; g[ACT_USE] = G_Y;
        g[ACT_SECONDARYATTACK] = G_LB; g[ACT_PRIMARYSWITCH] = G_RB; g[ACT_PDA] = G_BACK; g[ACT_START] = G_START;
        g[ACT_SPRINT] = G_LS_CLICK; g[ACT_BINOCULARS] = G_RS_CLICK; g[ACT_PRIMARYATTACK] = G_RT; g[ACT_CROUCH] = G_LT;
        g[ACT_SECONDARYSWITCH] = G_DPAD_LEFT; g[ACT_UP] = G_DPAD_UP; g[ACT_DOWN] = G_DPAD_DOWN; g[ACT_RIGHT] = G_DPAD_RIGHT;
        g[ACT_FORWARD] = G_LS_UP; g[ACT_BACKWARD] = G_LS_DOWN; g[ACT_MOVELEFT] = G_LS_LEFT; g[ACT_MOVERIGHT] = G_LS_RIGHT;
        g[ACT_LOOKUP] = G_RS_UP; g[ACT_LOOKDOWN] = G_RS_DOWN; g[ACT_LOOKLEFT] = G_RS_LEFT; g[ACT_LOOKRIGHT] = G_RS_RIGHT;
    }
    g[ACT_MOVE] = g[ACT_FORWARD] >= 0 ? StickGlyph(g[ACT_FORWARD]) : -1;
    g[ACT_LOOK] = g[ACT_LOOKUP] >= 0 ? StickGlyph(g[ACT_LOOKUP]) : -1;

    int changed = memcmp(g, g_glyphFor, sizeof g) != 0;
    memcpy(g_glyphFor, g, sizeof g);
    Log("prompts: bindings from [%s]%s", sec[0] ? sec : "(defaults)", changed ? " (changed)" : "");
    return changed;
}

static void BuildPad(Tracked *t)
{
    if (t->atlas < 0) { memcpy(t->pad, g_f16xbox, (int)g_f16len < t->size ? (int)g_f16len : t->size); return; }
    ComposeAtlas(t->orig, t->w, t->h, t->atlas, g_glyphFor, t->pad);
}

static void Upload(Tracked *t, int pad)
{
    D3DLOCKED_RECT lr;
    g_selfLock = 1;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(t->tex, 0, &lr, NULL, 0))) {
        const uint8_t *src = pad ? t->pad : t->orig;
        int rows = t->h / 4, rowBytes = t->w * 4;   /* DXT5: 16 bytes per 4x4 block */
        for (int r = 0; r < rows; r++) memcpy((uint8_t *)lr.pBits + r * lr.Pitch, src + r * rowBytes, rowBytes);
        IDirect3DTexture9_UnlockRect(t->tex, 0);
        if (t->dst) D3DUpdateTexture((IDirect3DBaseTexture9 *)t->tex, t->dst);
        t->state = pad;
    } else Log("prompts: LockRect failed on %p", t->tex);
    g_selfLock = 0;
}

/* ------------------------------------------------- texture detection */
/* The game reuses texture objects: an atlas texture can later be refilled with another picture (faction icons
 * in tooltips, for one). A tracked texture that the game fills with anything but its atlas is dropped, so the
 * button atlas is never written over the new picture. */
static void Untrack(int i)
{
    Tracked *t = &g_tex[i];
    if (t->dst) IDirect3DBaseTexture9_Release(t->dst);
    IDirect3DTexture9_Release(t->tex);
    free(t->orig); free(t->pad);
    g_tex[i] = g_tex[--g_ntex];
}

static void OnTexture(IDirect3DTexture9 *tex, const D3DSURFACE_DESC *d, void *bits, int pitch)
{
    if (g_selfLock) return;
    int atlas = -2;
    if (d->Format == D3DFMT_DXT5 && pitch == (int)d->Width * 4) {
        if (d->Width == 256 && d->Height == 256) atlas = -3;
        else if (d->Width == 64 && d->Height == 128) atlas = -1;
    }
    int size = d->Width * d->Height;               /* DXT5 = 1 byte per pixel */
    if (atlas != -2) {
        static unsigned tab[256];
        if (!tab[1]) for (int n = 255; n >= 0; n--) {              /* tab[1] last: the "ready" mark */
            unsigned c = (unsigned)n; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); tab[n] = c;
        }
        const uint8_t *p = (const uint8_t *)bits; unsigned c = 0xFFFFFFFFu;
        for (int i = 0; i < size; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        unsigned crc = ~c;
        if (atlas == -3) {
            for (int i = 0; i < g_natlases; i++) if (g_atlases[i].crc == crc) atlas = i;
            if (atlas < 0) atlas = -2;
        } else if (crc != g_f16crc) atlas = -2;
    }
    EnterCriticalSection(&g_lock);
    int i;
    for (i = 0; i < g_ntex && g_tex[i].tex != tex; i++) ;
    if (i < g_ntex) {
        if (atlas != -2 && g_tex[i].atlas == atlas) {             /* the same atlas loaded again */
            if (g_padMode == 1) { memcpy(bits, g_tex[i].pad, size); g_tex[i].state = 1; } else g_tex[i].state = 0;
            LeaveCriticalSection(&g_lock); return;
        }
        Untrack(i);                                               /* the game put another picture there */
    }
    if (atlas == -2) { LeaveCriticalSection(&g_lock); return; }
    for (int k = g_ntex - 1; k >= 0 && g_ntex >= MAX_TEX; k--) {  /* full: drop textures the game has let go of */
        IDirect3DTexture9_AddRef(g_tex[k].tex);
        if (IDirect3DTexture9_Release(g_tex[k].tex) == 1) Untrack(k);
    }
    if (g_ntex < MAX_TEX) {
        Tracked *t = &g_tex[g_ntex++];
        memset(t, 0, sizeof *t);
        t->tex = tex; IDirect3DTexture9_AddRef(tex);
        t->w = d->Width; t->h = d->Height; t->size = size; t->atlas = atlas;
        t->orig = (uint8_t *)malloc(size); t->pad = (uint8_t *)malloc(size);
        memcpy(t->orig, bits, size);
        BuildPad(t);
        if (g_padMode == 1) { memcpy(bits, t->pad, size); t->state = 1; }
        Log("prompts: found %s (tex %p, pool %d)", atlas >= 0 ? g_atlases[atlas].name : "font_16_buttons", tex, d->Pool);
    }
    LeaveCriticalSection(&g_lock);
}

static void OnUpdate(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
{
    if (g_selfLock) return;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ntex; i++)
        if ((IDirect3DBaseTexture9 *)g_tex[i].tex == src && g_tex[i].dst != dst) {
            if (g_tex[i].dst) IDirect3DBaseTexture9_Release(g_tex[i].dst);
            g_tex[i].dst = dst; IDirect3DBaseTexture9_AddRef(dst);
            Log("prompts: %p is staged into %p", src, dst);
        }
    LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------- input device switch */
typedef struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XState;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XState *);
static XInputGetState_t pXInputGetState;

/* real keyboard / mouse activity (GetLastInputInfo also counts gamepad input
   on modern Windows, so it cannot be used to tell the devices apart) */
static int KbMouseActivity(void)
{
    static POINT lastPt; static int havePt;
    static int lastDx, lastDy, haveD;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return 0;

    for (int vk = 1; vk < 0xFF; vk++) {
        if (vk == VK_PACKET) continue;
        if (GetAsyncKeyState(vk) & 0x8000) return 1;
    }
    /* raw DirectInput mouse delta of the game's mouse device */
    DWORD mouse = *(volatile DWORD *)0xED4574;
    if (mouse) {
        int dx = *(volatile int *)(mouse + 0x28), dy = *(volatile int *)(mouse + 0x2C);
        int moved = haveD && (abs(dx - lastDx) > 2 || abs(dy - lastDy) > 2);
        lastDx = dx; lastDy = dy; haveD = 1;
        if (moved) return 1;
    }
    POINT pt;
    if (GetCursorPos(&pt)) {
        int moved = havePt && (abs(pt.x - lastPt.x) > 3 || abs(pt.y - lastPt.y) > 3);
        lastPt = pt; havePt = 1;
        if (moved) return 1;
    }
    return 0;
}

/* Input polling runs on its own thread: reading all 254 key states every frame and XInput on empty pad slots
 * (a device enumeration each time) cost the render thread close to a millisecond per frame. */
static DWORD WINAPI InputThread(LPVOID u)
{
    (void)u;
    int kbSeen = 0, tick = 0;
    DWORD padSeen[4] = {0}, nextProbe[4] = {0}, nextIni = 0;
    for (;; Sleep(25)) {
        if (g_mouseOK && KbMouseActivity()) kbSeen = 1;
        if (++tick & 1) continue;                            /* pads and the decision every 50 ms */
        DWORD now = GetTickCount();
        int padActive = 0, padPresent = 0;
        for (DWORD i = 0; i < 4 && pXInputGetState; i++) {
            if (!padSeen[i] && (int)(now - nextProbe[i]) < 0) continue;   /* empty slot: probe every 2 s */
            XState st;
            if (pXInputGetState(i, &st) != 0) { padSeen[i] = 0; nextProbe[i] = now + 2000; continue; }
            padSeen[i] = 1; padPresent = 1;
            if (st.buttons || st.lt > 40 || st.rt > 40 ||
                abs(st.lx) > 12000 || abs(st.ly) > 12000 || abs(st.rx) > 12000 || abs(st.ry) > 12000) padActive = 1;
        }
        int kbActive = kbSeen && !padActive;
        kbSeen = 0;
        int want = g_padMode;
        if (padActive) want = 1;
        else if (kbActive) want = 0;
        else if (g_padMode < 0) want = padPresent;
        if (want == 1 && (int)(now - nextIni) >= 0) {
            nextIni = now + 2000;
            if (LoadBindings()) {
                EnterCriticalSection(&g_lock);
                for (int i = 0; i < g_ntex; i++) { BuildPad(&g_tex[i]); if (g_tex[i].state) g_tex[i].state = -1; }
                LeaveCriticalSection(&g_lock);
            }
        }
        if (want != g_padMode) Log("prompts: input -> %s", want ? "gamepad" : "keyboard/mouse");
        g_padMode = want;
    }
    return 0;
}

static void OnFrame(IDirect3DDevice9 *dev)
{
    (void)dev;
    int mode = g_padMode == 1;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ntex; i++)
        if (g_tex[i].state != mode) Upload(&g_tex[i], mode);
    LeaveCriticalSection(&g_lock);
}

void PromptsInit(void)
{
    g_enabled = IniInt("Prompts", "XboxPrompts", 1);
    if (!g_enabled) return;
    /* glyph data from the player's own game files (written by the installer / tools\\prompts.py) */
    char path[MAX_PATH]; _snprintf(path, sizeof path, "%s\\Mercenaries2Remastered\\prompts.bin", g_gameDir); path[MAX_PATH - 1] = 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD sz = f != INVALID_HANDLE_VALUE ? GetFileSize(f, NULL) : 0, rd = 0;
    unsigned char *buf = sz && sz < (8u << 20) ? (unsigned char *)malloc(sz) : NULL;
    int ok = buf && ReadFile(f, buf, sz, &rd, NULL) && rd == sz && PromptsParse(buf, sz);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) { free(buf); g_enabled = 0; Log("prompts: %s missing or invalid - Xbox prompts off (run the installer once)", path); return; }
    InitializeCriticalSection(&g_lock);
    memset(g_glyphFor, 0xff, sizeof g_glyphFor);
    LoadBindings();
    HMODULE x = LoadLibraryA("xinput1_3.dll");
    if (x) pXInputGetState = (XInputGetState_t)GetProcAddress(x, "XInputGetState");
    g_mouseOK = GameVersionOK();
    D3DSetTextureCallback(OnTexture);
    D3DSetUpdateCallback(OnUpdate);
    D3DAddFrameCallback(OnFrame);
    CloseHandle(CreateThread(NULL, 0, InputThread, NULL, 0, NULL));
    Log("prompts: Xbox prompts enabled");
}

/* 1 = gamepad in use, 0 = keyboard/mouse, -1 = unknown */
int InputMode(void) { return g_padMode; }

/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Atlas compositing (platform independent) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dxt.h"
#include "prompts_types.h"
#include "promptsgfx.h"

/* ------------------------------------------------------------- data */
PAtlas g_atlases[MAX_ATLAS];
int g_natlases;
XGlyph g_xg[G_COUNT];
const unsigned char *g_xgPix;
unsigned g_f16crc, g_f16len;
const unsigned char *g_f16xbox;

static uint32_t Rd32(const unsigned char *b, size_t n, size_t *p, int *bad)
{
    uint32_t v = 0;
    if (*p + 4 > n) { *bad = 1; return 0; }
    memcpy(&v, b + *p, 4); *p += 4; return v;
}
int PromptsParse(const unsigned char *b, size_t n)
{
    size_t p = 8; int bad = 0;
#define NEED(k) do { if (p + (k) > n) return 0; } while (0)
#define U32() Rd32(b, n, &p, &bad)
    if (n < 8 || memcmp(b, "M2PR", 4) || b[4] != 1) return 0;
    uint32_t na = U32(); if (bad || na > MAX_ATLAS) return 0;
    for (uint32_t i = 0; i < na; i++) {
        PAtlas *a = &g_atlases[i];
        NEED(32); memcpy(a->name, b + p, 31); a->name[31] = 0; p += 32;
        a->crc = U32(); a->w = (int)U32(); a->h = (int)U32(); a->n = (int)U32();
        NEED((size_t)a->n * sizeof(PCell)); a->cells = (const PCell *)(b + p); p += (size_t)a->n * sizeof(PCell);
        for (int k = 0; k < a->n; k++) if (a->cells[k].act >= ACT_COUNT) return 0;
    }
    uint32_t ng = U32(); if (bad || ng != G_COUNT) return 0;
    for (uint32_t i = 0; i < ng; i++) { g_xg[i].off = U32(); NEED(4); g_xg[i].w = b[p]; g_xg[i].h = b[p + 1]; p += 4; }
    uint32_t np = U32(); if (bad) return 0; NEED(np); g_xgPix = b + p; p += np;
    for (uint32_t i = 0; i < ng; i++) if ((size_t)g_xg[i].off + (size_t)g_xg[i].w * g_xg[i].h * 4 > np) return 0;
    g_f16crc = U32(); g_f16len = U32(); if (bad) return 0; NEED(g_f16len); g_f16xbox = b + p; p += g_f16len;
    if (bad) return 0;
    g_natlases = (int)na;
    return 1;
#undef U32
#undef NEED
}

/* ---------------------------------------------------------- compositing */
static void DrawGlyph(uint8_t *img, int iw, int ih, const PCell *c, int gi)
{
    const XGlyph *g = &g_xg[gi];
    const uint8_t *src = g_xgPix + g->off;
    int x0 = c->x0, y0 = c->y0, x1 = c->x1, y1 = c->y1;
    /* clear the old icon (1px margin for its drop shadow) */
    for (int y = y0 - 1; y <= y1; y++)
        for (int x = x0 - 1; x <= x1; x++)
            if (x >= 0 && y >= 0 && x < iw && y < ih) memset(img + (y * iw + x) * 4, 0, 4);

    float cw = (float)(x1 - x0), ch = (float)(y1 - y0);
    float s = cw / g->w < ch / g->h ? cw / g->w : ch / g->h;
    if (s > 1.15f) s = 1.15f;
    int dw = (int)(g->w * s + 0.5f), dh = (int)(g->h * s + 0.5f);
    int ox = x0 + ((int)cw - dw) / 2, oy = y0 + ((int)ch - dh) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            /* bilinear sample, premultiplied */
            float fx = (x + 0.5f) / s - 0.5f, fy = (y + 0.5f) / s - 0.5f;
            int ix = (int)fx, iy = (int)fy; if (fx < 0) { fx = 0; ix = 0; } if (fy < 0) { fy = 0; iy = 0; }
            float ax = fx - ix, ay = fy - iy;
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; k++) {
                int sx = ix + (k & 1), sy = iy + (k >> 1);
                if (sx >= g->w) sx = g->w - 1;
                if (sy >= g->h) sy = g->h - 1;
                float wgt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
                const uint8_t *p = src + (sy * g->w + sx) * 4;
                float a = p[3] / 255.f;
                acc[0] += p[0] * a * wgt; acc[1] += p[1] * a * wgt; acc[2] += p[2] * a * wgt; acc[3] += a * wgt;
            }
            int tx = ox + x, ty = oy + y;
            if (tx < 0 || ty < 0 || tx >= iw || ty >= ih) continue;
            uint8_t *d = img + (ty * iw + tx) * 4;
            if (acc[3] > 0.001f) {
                d[0] = (uint8_t)(acc[0] / acc[3]); d[1] = (uint8_t)(acc[1] / acc[3]); d[2] = (uint8_t)(acc[2] / acc[3]);
            }
            d[3] = (uint8_t)(acc[3] * 255.f + 0.5f);
        }
}

void ComposeAtlas(const uint8_t *orig, int w, int h, int atlas, const int *glyphFor, uint8_t *out)
{
    const PAtlas *at = &g_atlases[atlas];
    uint8_t *img = (uint8_t *)malloc(w * h * 4);
    if (!img) { memcpy(out, orig, w * h); return; }
    DxtDecode5(orig, w, h, img);
    for (int i = 0; i < at->n; i++) {
        const PCell *c = &at->cells[i];
        int gi = c->act ? glyphFor[c->act] : -1;
        if (gi >= 0) DrawGlyph(img, w, h, c, gi);
    }
    DxtEncode5(img, w, h, out);
    free(img);
}


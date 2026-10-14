/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Minimal DXT5 (BC3) decoder / encoder for UI atlases */
#include <string.h>
#include <stdint.h>
#include "dxt.h"

static void rgb565(uint16_t c, int *r, int *g, int *b)
{
    *r = ((c >> 11) & 31) * 255 / 31;
    *g = ((c >> 5) & 63) * 255 / 63;
    *b = (c & 31) * 255 / 31;
}

void DxtDecode5(const uint8_t *src, int w, int h, uint8_t *rgba)
{
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4, src += 16) {
            int a[8]; a[0] = src[0]; a[1] = src[1];
            if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
            else { for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5; a[6] = 0; a[7] = 255; }
            uint64_t ab = 0; for (int i = 0; i < 6; i++) ab |= (uint64_t)src[2 + i] << (8 * i);
            uint16_t c0 = src[8] | src[9] << 8, c1 = src[10] | src[11] << 8;
            int c[4][3];
            rgb565(c0, &c[0][0], &c[0][1], &c[0][2]);
            rgb565(c1, &c[1][0], &c[1][1], &c[1][2]);
            for (int k = 0; k < 3; k++) {         /* BC3 always uses 4-colour mode */
                c[2][k] = (2 * c[0][k] + c[1][k]) / 3;
                c[3][k] = (c[0][k] + 2 * c[1][k]) / 3;
            }
            uint32_t ci = src[12] | src[13] << 8 | src[14] << 16 | (uint32_t)src[15] << 24;
            for (int p = 0; p < 16; p++) {
                int x = bx + (p & 3), y = by + (p >> 2);
                if (x >= w || y >= h) continue;
                uint8_t *d = rgba + (y * w + x) * 4;
                int idx = (ci >> (2 * p)) & 3;
                d[0] = c[idx][0]; d[1] = c[idx][1]; d[2] = c[idx][2];
                d[3] = a[(ab >> (3 * p)) & 7];
            }
        }
}

static uint16_t to565(int r, int g, int b)
{
    return (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

void DxtEncode5(const uint8_t *rgba, int w, int h, uint8_t *dst)
{
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4, dst += 16) {
            uint8_t px[16][4];
            for (int p = 0; p < 16; p++) {
                int x = bx + (p & 3), y = by + (p >> 2);
                if (x >= w) x = w - 1;
                if (y >= h) y = h - 1;
                memcpy(px[p], rgba + (y * w + x) * 4, 4);
            }
            /* alpha: 8-value mode between min and max */
            int amin = 255, amax = 0;
            for (int p = 0; p < 16; p++) { if (px[p][3] < amin) amin = px[p][3]; if (px[p][3] > amax) amax = px[p][3]; }
            dst[0] = (uint8_t)amax; dst[1] = (uint8_t)amin;
            uint64_t ab = 0;
            if (amax > amin) {
                int pal[8]; pal[0] = amax; pal[1] = amin;
                for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * amax + i * amin) / 7;
                for (int p = 0; p < 16; p++) {
                    int best = 0, bd = 1 << 30;
                    for (int i = 0; i < 8; i++) { int dd = abs(pal[i] - px[p][3]); if (dd < bd) { bd = dd; best = i; } }
                    ab |= (uint64_t)best << (3 * p);
                }
            }
            for (int i = 0; i < 6; i++) dst[2 + i] = (uint8_t)(ab >> (8 * i));

            /* colour: endpoints from the bounding box of visible pixels (principal-ish axis) */
            int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0}, n = 0;
            for (int p = 0; p < 16; p++) {
                if (px[p][3] < 8) continue;
                n++;
                for (int k = 0; k < 3; k++) { if (px[p][k] < mn[k]) mn[k] = px[p][k]; if (px[p][k] > mx[k]) mx[k] = px[p][k]; }
            }
            if (!n) { memset(dst + 8, 0, 8); continue; }
            /* pick diagonal of the box that best matches the colour correlation */
            int cov[3] = {0, 0, 0}, mean[3] = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
            for (int p = 0; p < 16; p++) {
                if (px[p][3] < 8) continue;
                int dr = px[p][0] - mean[0], dg = px[p][1] - mean[1], db = px[p][2] - mean[2];
                cov[0] += dr * dg; cov[1] += dr * db; cov[2] += dg * db;
            }
            int e0[3] = {mx[0], mx[1], mx[2]}, e1[3] = {mn[0], mn[1], mn[2]};
            if (cov[0] < 0) { int t = e0[1]; e0[1] = e1[1]; e1[1] = t; }       /* r vs g anti-correlated */
            if (cov[1] < 0) { int t = e0[2]; e0[2] = e1[2]; e1[2] = t; }       /* r vs b */
            uint16_t c0 = to565(e0[0], e0[1], e0[2]), c1 = to565(e1[0], e1[1], e1[2]);
            if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
            uint32_t ci = 0;
            if (c0 != c1) {
                int c[4][3];
                rgb565(c0, &c[0][0], &c[0][1], &c[0][2]);
                rgb565(c1, &c[1][0], &c[1][1], &c[1][2]);
                for (int k = 0; k < 3; k++) { c[2][k] = (2 * c[0][k] + c[1][k]) / 3; c[3][k] = (c[0][k] + 2 * c[1][k]) / 3; }
                for (int p = 0; p < 16; p++) {
                    int best = 0, bd = 1 << 30;
                    for (int i = 0; i < 4; i++) {
                        int dr = c[i][0] - px[p][0], dg = c[i][1] - px[p][1], db = c[i][2] - px[p][2];
                        int dd = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                        if (dd < bd) { bd = dd; best = i; }
                    }
                    ci |= (uint32_t)best << (2 * p);
                }
            }
            dst[8] = c0 & 255; dst[9] = c0 >> 8; dst[10] = c1 & 255; dst[11] = c1 >> 8;
            dst[12] = ci & 255; dst[13] = (ci >> 8) & 255; dst[14] = (ci >> 16) & 255; dst[15] = ci >> 24;
        }
}

/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
#include <stdint.h>
#include <string.h>
static const int steps[89]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const int idxt[16]={-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
/* mono MS-IMA ADPCM, block_align bytes per block (4 header + (block_align-4) data => 1+2*(block_align-4) samples) */
int ima_encode_mono(const int16_t *in, int n, uint8_t *out, int block_align) {
    int spb = 1 + 2 * (block_align - 4), o = 0, idx = 0;
    for (int s = 0; s < n; s += spb) {
        int pred = in[s];
        uint8_t *b = out + o;
        b[0] = pred & 255; b[1] = (pred >> 8) & 255; b[2] = idx; b[3] = 0;
        memset(b + 4, 0, block_align - 4);
        for (int k = 1; k < spb; k++) {
            int smp = (s + k < n) ? in[s + k] : pred;
            int step = steps[idx], diff = smp - pred, code = 0;
            if (diff < 0) { code = 8; diff = -diff; }
            int dq = step >> 3;
            if (diff >= step) { code |= 4; diff -= step; dq += step; }
            step >>= 1; if (diff >= step) { code |= 2; diff -= step; dq += step; }
            step >>= 1; if (diff >= step) { code |= 1; dq += step; }
            pred += (code & 8) ? -dq : dq;
            if (pred > 32767) pred = 32767; if (pred < -32768) pred = -32768;
            idx += idxt[code]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
            int bi = 4 + (k - 1) / 2;
            if ((k - 1) & 1) b[bi] |= code << 4; else b[bi] |= code;
        }
        o += block_align;
    }
    return o;
}

/* multi-channel MS-IMA ADPCM with 64 frames per block (block_align = 36*ch).
 * header predictor = frame s, nibbles = frames s+1..s+64, so a decoder that outputs either
 * the 64 nibble samples or header+63 nibbles advances exactly 64 frames per block (PC engine counts 64). */
static int enc1(int *pred, int *idx, int smp) {
    int step = steps[*idx], diff = smp - *pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int dq = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; dq += step; }
    step >>= 1; if (diff >= step) { code |= 2; diff -= step; dq += step; }
    step >>= 1; if (diff >= step) { code |= 1; dq += step; }
    *pred += (code & 8) ? -dq : dq;
    if (*pred > 32767) *pred = 32767; if (*pred < -32768) *pred = -32768;
    *idx += idxt[code]; if (*idx < 0) *idx = 0; if (*idx > 88) *idx = 88;
    return code;
}
int ima_encode64(const int16_t *in, int frames, int ch, uint8_t *out) {
    int idx[8] = {0}, o = 0, ba = 36 * ch;
    for (int s = 0; s < frames; s += 64) {
        uint8_t *b = out + o; memset(b, 0, ba);
        for (int c = 0; c < ch; c++) {
            int pred = in[s * ch + c];
            b[4 * c] = pred & 255; b[4 * c + 1] = (pred >> 8) & 255; b[4 * c + 2] = idx[c]; b[4 * c + 3] = 0;
            for (int k = 0; k < 64; k++) {
                int f = s + 1 + k; if (f >= frames) f = frames - 1;
                int code = enc1(&pred, &idx[c], in[f * ch + c]);
                int chunk = k / 8, within = k % 8;
                int bi = 4 * ch + chunk * 4 * ch + 4 * c + within / 2;
                if (within & 1) b[bi] |= code << 4; else b[bi] |= code;
            }
        }
        o += ba;
    }
    return o;
}

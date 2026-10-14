/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
/* Vertex shader patch for vehicle deformation.
 *
 * The game's vertex shaders (vs_3_0, compiled HLSL with constant tables) transform the input position by
 * BoneMatrixArray[index] (rigid skinning, optional) and objectData.LocalToWorld. A patched copy adds, before
 * any other code, a displacement of the object-space position by a list of dents:
 *
 *   m   = skinned ? Bone[idx] * v : v                       object (model) space position
 *   acc = sum_i  D_i * saturate(1 - |m - C_i|^2 / r_i^2)^2  C_i/r_i: dent centre/radius, D_i: push vector
 *   v'  = v + (skinned ? Bone[idx]^T * acc : acc)            back to bone space (rotation part)
 *
 * and every later read of the position input uses v'. Dents live in otherwise unused float constants
 * (c[base + 2i] = C.xyz, 1/r^2; c[base + 2i + 1] = D.xyz), the dent count in an unused integer constant
 * (loop count, start 0, step 2). The shader is patched as text: D3DXDisassembleShader, edit,
 * D3DXAssembleShader (the game's own d3dx9_36.dll).
 */
#include "vspatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

typedef struct XBuf XBuf;
typedef struct { HRESULT (WINAPI *QI)(XBuf *, const void *, void **); ULONG (WINAPI *AddRef)(XBuf *);
                 ULONG (WINAPI *Release)(XBuf *); void *(WINAPI *Ptr)(XBuf *); DWORD (WINAPI *Size)(XBuf *); } XBufVt;
struct XBuf { const XBufVt *vt; };
typedef HRESULT (WINAPI *Disasm_t)(const DWORD *, BOOL, LPCSTR, XBuf **);
typedef HRESULT (WINAPI *Asm_t)(LPCSTR, UINT, const void *, void *, DWORD, XBuf **, XBuf **);
static Disasm_t p_dis; static Asm_t p_asm;
static float g_cap = 0.35f;
void VsPatchSetCap(float m) { if (m > 0.01f) g_cap = m; }

int VsPatchInit(void)
{
    HMODULE x = GetModuleHandleA("d3dx9_36.dll"); if (!x) x = LoadLibraryA("d3dx9_36.dll");
    if (!x) return 0;
    p_dis = (Disasm_t)GetProcAddress(x, "D3DXDisassembleShader");
    p_asm = (Asm_t)GetProcAddress(x, "D3DXAssembleShader");
    return p_dis && p_asm;
}

UINT VsCodeSize(const DWORD *c)
{
    UINT i = 1;
    for (;;) {
        DWORD t = c[i];
        if (t == 0x0000FFFF) return (i + 1) * 4;
        if ((t & 0xFFFF) == 0xFFFE) i += 1 + ((t >> 16) & 0x7FFF);
        else i += 1 + ((t >> 24) & 0xF);
        if (i > 65536) return 0;
    }
}

/* constant table: fills used float/int registers and the registers of the named parameters */
static int Ctab(const DWORD *c, BYTE *fused, BYTE *iused, int *obj, int *bone, int *blendw)
{
    *obj = *bone = -1; *blendw = 0;
    DWORD t = c[1];
    if ((t & 0xFFFF) != 0xFFFE || c[2] != 0x42415443) return 0;     /* 'CTAB' */
    const BYTE *b = (const BYTE *)(c + 3); UINT n = (((t >> 16) & 0x7FFF) - 1) * 4;
    DWORD nc = *(const DWORD *)(b + 12), off = *(const DWORD *)(b + 16);
    for (DWORD k = 0; k < nc && off + 20 * (k + 1) <= n; k++) {
        const BYTE *e = b + off + 20 * k;
        DWORD name = *(const DWORD *)e; WORD rs = *(const WORD *)(e + 4), ri = *(const WORD *)(e + 6), rc = *(const WORD *)(e + 8);
        const char *s = name < n ? (const char *)b + name : "";
        if (rs == 2) for (int r = ri; r < ri + rc && r < 256; r++) fused[r] = 1;
        if (rs == 1) for (int r = ri; r < ri + rc && r < 16; r++) iused[r] = 1;
        if (rs == 2 && !strcmp(s, "objectData")) *obj = ri;
        if (rs == 2 && !strcmp(s, "BoneMatrixArray")) *bone = ri;
        if (!strcmp(s, "BlendWeight")) *blendw = 1;
    }
    return 1;
}

typedef struct { char *p; size_t n, cap; } Str;
static void Put(Str *s, const char *t)
{
    size_t l = strlen(t);
    if (s->n + l + 1 > s->cap) { s->cap = (s->n + l + 1) * 2 + 4096; s->p = (char *)realloc(s->p, s->cap); }
    memcpy(s->p + s->n, t, l + 1); s->n += l;
}
static void Putf(Str *s, const char *f, ...)
{
    char b[256]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); Put(s, b);
}
/* copy line replacing register token `from` (e.g. "v0") by `to` */
static void PutRepl(Str *s, const char *line, size_t len, const char *from, const char *to)
{
    size_t fl = strlen(from); char tmp[512]; size_t o = 0;
    for (size_t i = 0; i < len && o < sizeof tmp - 16; ) {
        if (!strncmp(line + i, from, fl) && (i == 0 || !isalnum((BYTE)line[i - 1])) && !isdigit((BYTE)line[i + fl])) {
            strcpy(tmp + o, to); o += strlen(to); i += fl;
        } else tmp[o++] = line[i++];
    }
    tmp[o] = 0; Put(s, tmp);
}

static int StartsWith(const char *l, const char *p) { return !strncmp(l, p, strlen(p)); }

int VsPatch(const DWORD *code, int nd, int glass, VsPatchInfo *pi, DWORD **out, char *why, int whyn)
{
    BYTE fused[256] = {0}, iused[16] = {0};
    int obj, bone, blendw;
    *out = NULL; memset(pi, 0, sizeof *pi);
#define FAIL(m) do { snprintf(why, whyn, "%s", m); goto fail; } while (0)
    XBuf *dis = NULL, *res = NULL, *err = NULL; Str s = {0};
    char *txt = NULL; char **L = NULL;
    if ((code[0] & 0xFFFF0000) != 0xFFFE0000 || (code[0] & 0xFFFF) < 0x0300) FAIL("not vs_3_0");
    if (!Ctab(code, fused, iused, &obj, &bone, &blendw)) FAIL("no constant table");
    if (obj < 0) FAIL("no objectData");
    (void)blendw;                                   /* morph (damage) shaders: deltas add to the position, ours too */
    if (!p_dis || FAILED(p_dis(code, FALSE, NULL, &dis))) FAIL("disassembly failed");
    {
        const char *d = (const char *)dis->vt->Ptr(dis); size_t dl = strlen(d);
        txt = (char *)malloc(dl + 1); memcpy(txt, d, dl + 1);
    }
    /* collect lines */
    int nl = 0, cap = 256; L = (char **)malloc(cap * sizeof *L);
    for (char *p = strtok(txt, "\n"); p; p = strtok(NULL, "\n")) {
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p); while (l && (p[l - 1] == '\r' || p[l - 1] == ' ')) p[--l] = 0;
        if (!*p || StartsWith(p, "//")) continue;
        if (nl == cap) { cap *= 2; L = (char **)realloc(L, cap * sizeof *L); }
        L[nl++] = p;
    }
    int pos = -1, bi = -1, nrm = -1, tan = -1, lastDcl = -1, maxr = -1, ver = -1;
    for (int i = 0; i < nl; i++) {
        char *l = L[i]; int r;
        if (StartsWith(l, "vs_3_0")) ver = i;
        if (StartsWith(l, "def c") && sscanf(l + 5, "%d", &r) == 1 && r < 256) fused[r] = 1;
        if (StartsWith(l, "defi i") && sscanf(l + 6, "%d", &r) == 1 && r < 16) iused[r] = 1;
        if (StartsWith(l, "dcl")) {
            lastDcl = i;
            const char *v = strstr(l, " v");
            if (v && sscanf(v + 2, "%d", &r) == 1) {
                if (StartsWith(l, "dcl_position ") && pos < 0) pos = r;
                if (StartsWith(l, "dcl_blendindices ") && bi < 0) bi = r;
                if (StartsWith(l, "dcl_normal ") && nrm < 0) nrm = r;
                if (StartsWith(l, "dcl_tangent ") && tan < 0) tan = r;
            }
        }
        for (const char *q = l; (q = strchr(q, 'r')); q++)
            if ((q == l || !isalnum((BYTE)q[-1])) && isdigit((BYTE)q[1]) && sscanf(q + 1, "%d", &r) == 1 && r > maxr) maxr = r;
        if (strstr(l, "aL") && !StartsWith(l, "loop")) { /* shader uses its own loops: fine, ours ends first */ }
    }
    if (ver != 0) FAIL("unexpected header");
    if (pos < 0) FAIL("no position input");
    if (maxr >= 16) FAIL("no free temporaries");
    int skin = bone >= 0, nb = 0, bw = -1;
    char idxLine[256] = "", movaLine[64] = "";
    if (skin) {
        if (bi < 0) FAIL("bones without blend indices");
        for (int i = 0; i < nl; i++)
            if (StartsWith(L[i], "dcl_blendweight ")) { const char *v = strstr(L[i], " v"); if (v) bw = atoi(v + 2); }
        /* first "mova a0..., rX..." and the single instruction that computed rX from inputs/constants */
        int m = -1, rx = -1;
        for (int i = lastDcl + 1; i < nl; i++)
            if (StartsWith(L[i], "mova a0") && sscanf(strchr(L[i], ',') + 1, " r%d", &rx) == 1) { m = i; break; }
        if (m <= lastDcl + 1) FAIL("no bone index computation");
        char dst[16]; snprintf(dst, sizeof dst, "r%d", rx);
        const char *l = L[m - 1], *sp = strchr(l, ' ');
        if (!sp || strncmp(sp + 1, dst, strlen(dst)) || isdigit((BYTE)sp[1 + strlen(dst)])) FAIL("bone index computed in several steps");
        for (const char *q = strchr(sp + 1, ','); q && *q; q++)
            if (*q == 'r' && isdigit((BYTE)q[1]) && !isalnum((BYTE)q[-1])) FAIL("bone index uses temporaries");
        Str t = {0}; PutRepl(&t, l, strlen(l), dst, "r29");
        snprintf(idxLine, sizeof idxLine, "%s", t.p); free(t.p);
        t.p = NULL; t.n = t.cap = 0; PutRepl(&t, L[m], strlen(L[m]), dst, "r29");
        snprintf(movaLine, sizeof movaLine, "%s", t.p); free(t.p);
        /* bones per vertex: index components the shader uses with the bone matrices */
        const char *cc = "xyzw";
        for (int k = 0; k < 4; k++) {
            char want[64]; snprintf(want, sizeof want, "c%d[a0.%c]", bone, cc[k]);
            int seen = 0; for (int i = m; i < nl && !seen; i++) seen = strstr(L[i], want) != NULL;
            if (seen) nb = k + 1; else break;
        }
        if (!nb) FAIL("unexpected bone matrix addressing");
        if (nb > 1) {
            if (bw < 0) FAIL("several bones without weights");
            for (int k = 0; k < nb; k++) {          /* weight k must be read as v<bw>.<k> */
                char want[16]; snprintf(want, sizeof want, "v%d.%c", bw, cc[k]);
                int seen = 0; for (int i = m; i < nl && !seen; i++) seen = strstr(L[i], want) != NULL;
                if (!seen) FAIL("unexpected bone weights");
            }
        }
    }
    /* free constants: 2 per dent + 3 for our literals, highest free block */
    int need, base = -1;
    for (; nd >= 6 && base < 0; nd--) {                       /* fewer dents if the shader is short of constants */
        need = 2 * nd + 4;
        for (int top = 255; top - need + 1 >= 0 && base < 0; top--) {
            int ok = 1; for (int r = top - need + 1; r <= top && ok; r++) ok = !fused[r];
            if (ok) base = top - need + 1;
        }
        if (base >= 0) break;
    }
    if (base < 0) FAIL("no free constants");
    int ireg = -1; for (int r = 0; r < 16 && ireg < 0; r++) if (!iused[r]) ireg = r;
    if (ireg < 0) FAIL("no free integer constant");
    int L0 = base + 2 * nd, L1 = L0 + 1, L2 = L0 + 2, L3 = L0 + 3;
    /* see-through layers (glass, paint gloss): bend like the body, and output (model position, breakage) as texcoord<gsem> for the pixel shader */
    int gout = -1, gsem = -1;
    if (glass) {
        BYTE ou[16] = {0}, su[16] = {0};
        for (int i = 1; i <= lastDcl; i++) {
            const char *o = strstr(L[i], " o"); int r, u;
            if (o && sscanf(o + 2, "%d", &r) == 1 && r < 16) ou[r] = 1;
            if (o && sscanf(L[i], "dcl_texcoord%d", &u) == 1 && u < 16) su[u] = 1;
            if (o && StartsWith(L[i], "dcl_texcoord ")) su[0] = 1;
        }
        for (int r = 11; r >= 0 && gout < 0; r--) if (!ou[r]) gout = r;
        for (int u = 9; u < 16 && gsem < 0; u++) if (!su[u]) gsem = u;
        if (gout < 0 || gsem < 0) FAIL("no free output");
    }

    /* registers: r31 position out, r30 displacement, r29/r24/r22/r21/r20 scratch, r28 model position,
     * r27/r26 dent, r25 model normal, r19 normal correction, r23 normal out, r22 tangent out */
    char P[8], N[8], Tn[8];
    snprintf(P, sizeof P, "v%d", pos); snprintf(N, sizeof N, "v%d", nrm); snprintf(Tn, sizeof Tn, "v%d", tan);
    int hasN = nrm >= 0, hasT = tan >= 0 && hasN;
    Put(&s, "vs_3_0\n");
    Putf(&s, "def c%d, 12, 0, 0, 0\n", L3);
    Putf(&s, "def c%d, 3, 0, 1, 1.5\n", L0);
    Putf(&s, "def c%d, 2, -1, %.4f, %.4f\n", L1, VSP_FREQ, VSP_WRINKLE);
    Putf(&s, "def c%d, -18, %.5f, 0.666667, %.4f\n", L2, 2.0 * VSP_FREQ / 1.5, g_cap);
    for (int i = 1; i <= lastDcl; i++) { Put(&s, L[i]); Put(&s, "\n"); }
    if (glass) Putf(&s, "dcl_texcoord%d o%d\n", gsem, gout);
    if (skin) {
        /* blended bone matrix rows -> r16..r18 */
        const char *cc = "xyzw";
        Put(&s, idxLine); Put(&s, "\n"); Put(&s, movaLine); Put(&s, "\n");
        for (int row = 0; row < 3; row++) {
            if (nb == 1) Putf(&s, "mov r%d, c%d[a0.x]\n", 16 + row, bone + row);
            else {
                Putf(&s, "mul r%d, c%d[a0.x], v%d.x\n", 16 + row, bone + row, bw);
                for (int k = 1; k < nb; k++) Putf(&s, "mad r%d, c%d[a0.%c], v%d.%c, r%d\n", 16 + row, bone + row, cc[k], bw, cc[k], 16 + row);
            }
        }
        Putf(&s, "dp4 r28.x, %s, r16\ndp4 r28.y, %s, r17\ndp4 r28.z, %s, r18\n", P, P, P);
        if (hasN) Putf(&s, "dp3 r25.x, %s, r16\ndp3 r25.y, %s, r17\ndp3 r25.z, %s, r18\n", N, N, N);
    } else {
        Putf(&s, "mov r28.xyz, %s\n", P);
        if (hasN) Putf(&s, "mov r25.xyz, %s\n", N);
    }
    Putf(&s, "mov r30.xyz, c%d.y\n", L0);
    if (hasN || glass) Putf(&s, "mov r19, c%d.y\n", L0);
    Putf(&s, "loop aL, i%d\n", ireg);
    /* relative and absolute constant addressing can't be mixed in one instruction: dents go to r27/r26 */
    Putf(&s, "mov r27, c%d[aL]\n", base);                    /* centre, 1/r^2 */
    Putf(&s, "mov r26, c%d[aL]\n", base + 1);                /* push vector, seed */
    Put(&s, "add r29.xyz, r28, -r27\n");
    Put(&s, "dp3 r29.w, r29, r29\n");
    Put(&s, "mul r29.w, r29.w, r27.w\n");                     /* q = |m-c|^2 / r^2 */
    Putf(&s, "add r29.w, c%d.z, -r29.w\n", L0);
    Putf(&s, "mul_sat r29.w, r29.w, c%d.w\n", L0);           /* a = sat(1.5 (1-q)) */
    Put(&s, "add r24.w, r29.w, r29.w\n");                   /* w = a^2 (3-2a): flat crushed centre, sharp rim */
    Putf(&s, "add r24.w, -r24.w, c%d.x\n", L0);
    Put(&s, "mul r24.w, r24.w, r29.w\nmul r24.w, r24.w, r29.w\n");
    Putf(&s, "add r20.w, c%d.z, -r29.w\n", L0);              /* grad w = -18 a(1-a)/r^2 (m-c) */
    Put(&s, "mul r20.w, r20.w, r29.w\nmul r20.w, r20.w, r27.w\n");
    Putf(&s, "mul r20.w, r20.w, c%d.x\n", L2);
    Put(&s, "mul r20.xyz, r29, r20.w\n");
    /* creases: triangle waves of the position, phase from the dent seed */
    Putf(&s, "mad r24.xyz, r28, c%d.z, r26.w\n", L1);
    Put(&s, "frc r24.xyz, r24\n");
    Putf(&s, "mad r24.xyz, r24, c%d.x, c%d.y\n", L1, L1);   /* s in [-1,1) */
    Putf(&s, "sge r21.xyz, r24, c%d.y\n", L0);
    Putf(&s, "mad r21.xyz, r21, c%d.x, c%d.y\n", L1, L1);   /* sign(s) */
    Put(&s, "abs r24.xyz, r24\n");
    Putf(&s, "dp3 r22.w, r24, c%d.z\n", L2);
    Putf(&s, "add r22.w, r22.w, c%d.y\n", L1);               /* T in [-1,1] */
    Putf(&s, "mad r22.w, r22.w, c%d.w, -c%d.y\n", L1, L1);  /* F = 1 + k T */
    Putf(&s, "mul r21.xyz, r21, c%d.y\n", L2);               /* grad T */
    Putf(&s, "mul r21.xyz, r21, c%d.w\n", L1);
    Put(&s, "mul r21.xyz, r21, r24.w\n");
    Put(&s, "mad r21.xyz, r20, r22.w, r21\n");                /* grad h = grad w F + w k grad T */
    Put(&s, "mul r24.w, r24.w, r22.w\n");                     /* h = w F */
    Put(&s, "mad r30.xyz, r26, r24.w, r30\n");                /* displacement += D h */
    if (glass) {                                              /* breakage += h |D| 12 */
        Put(&s, "dp3 r21.w, r26, r26\nrsq r21.w, r21.w\nrcp r21.w, r21.w\n");
        Putf(&s, "mul r21.w, r21.w, c%d.x\n", L3);
        Put(&s, "mad r19.w, r24.w, r21.w, r19.w\n");
    }
    if (hasN) {
        Put(&s, "dp3 r20.w, r26, r25\n");
        Put(&s, "mad r19.xyz, r21, -r20.w, r19\n");          /* normal: n - grad h (D.n) */
    }
    Put(&s, "endloop\n");
    /* overlapping dents: total push limited to the maximum depth (cabins stay clear of the occupants) */
    Put(&s, "dp3 r29.w, r30, r30\nrsq r29.w, r29.w\n");
    Putf(&s, "mul r29.w, r29.w, c%d.w\n", L2);
    Putf(&s, "min r29.w, r29.w, c%d.z\n", L0);
    Put(&s, "mul r30.xyz, r30, r29.w\n");
    if (hasN) Put(&s, "mul r19.xyz, r19, r29.w\n");
    Putf(&s, "mov r31, %s\n", P);
    if (glass) Putf(&s, "mov o%d.xyz, r28\nmov_sat o%d.w, r19.w\n", gout, gout);
    if (skin) {
        Put(&s, "mul r29.xyz, r30.x, r16\nmad r29.xyz, r30.y, r17, r29\nmad r29.xyz, r30.z, r18, r29\n");
        Put(&s, "add r31.xyz, r31, r29\n");
        if (hasN) {
            Put(&s, "mul r29.xyz, r19.x, r16\nmad r29.xyz, r19.y, r17, r29\nmad r29.xyz, r19.z, r18, r29\n");
            Put(&s, "mov r19.xyz, r29\n");
        }
    } else Put(&s, "add r31.xyz, r31, r30\n");
    if (hasN) {
        Putf(&s, "mov r23, %s\n", N);
        Put(&s, "add r23.xyz, r23, r19\n");
        Put(&s, "nrm r29.xyz, r23\nmov r23.xyz, r29\n");
    }
    if (hasT) {                                               /* re-orthogonalise the tangent */
        Putf(&s, "mov r22, %s\n", Tn);
        Put(&s, "dp3 r29.w, r22, r23\nmad r22.xyz, r23, -r29.w, r22\nnrm r29.xyz, r22\nmov r22.xyz, r29\n");
    }
    for (int i = lastDcl + 1; i < nl; i++) {
        Str a1 = {0}, a2 = {0};
        PutRepl(&a1, L[i], strlen(L[i]), P, "r31");
        if (hasN) PutRepl(&a2, a1.p, a1.n, N, "r23"); else Put(&a2, a1.p);
        if (hasT) PutRepl(&s, a2.p, a2.n, Tn, "r22"); else Put(&s, a2.p);
        Put(&s, "\n"); free(a1.p); free(a2.p);
    }
    /* vs_3_0 rule: one constant register per instruction (our lines; the game's are compiler output) */
    for (char *ln = s.p; ln && *ln; ) {
        char *e2 = strchr(ln, '\n'); size_t ll = e2 ? (size_t)(e2 - ln) : strlen(ln);
        if (strncmp(ln, "def", 3) && strncmp(ln, "dcl", 3)) {
            int regs[4], nr = 0;
            for (size_t k = 0; k < ll; k++)
                if (ln[k] == 'c' && isdigit((BYTE)ln[k + 1]) && (k == 0 || !isalnum((BYTE)ln[k - 1]))) {
                    int r = atoi(ln + k + 1), dup = 0;
                    for (int j = 0; j < nr; j++) dup |= regs[j] == r;
                    if (!dup && nr < 4) regs[nr++] = r;
                }
            if (nr > 1) { snprintf(why, whyn, "internal: two constants in '%.*s'", (int)ll, ln); goto fail; }
        }
        ln = e2 ? e2 + 1 : NULL;
    }
#ifdef VSP_TEST
    if (getenv("VSP_DUMP")) fputs(s.p, stdout);
#endif
    if (!p_asm || FAILED(p_asm(s.p, (UINT)s.n, NULL, NULL, 0, &res, &err))) {
        snprintf(why, whyn, "assembly failed: %.200s", err ? (const char *)err->vt->Ptr(err) : "?");
        goto fail;
    }
    {
        DWORD n = res->vt->Size(res);
        *out = (DWORD *)malloc(n); memcpy(*out, res->vt->Ptr(res), n);
    }
    pi->objReg = obj; pi->base = base; pi->ireg = ireg; pi->nd = nd; pi->skinned = skin; pi->gsem = gsem;
    free(L); free(txt); free(s.p);
    dis->vt->Release(dis); res->vt->Release(res); if (err) err->vt->Release(err);
    why[0] = 0;
    return 1;
fail:
    free(L); free(txt); free(s.p);
    if (dis) dis->vt->Release(dis);
    if (res) res->vt->Release(res);
    if (err) err->vt->Release(err);
    return 0;
#undef FAIL
}

/* Pixel shader twin for broken glass: the vertex twin passes (model position, breakage 0..1) in
 * texcoord<sem>; pixels where breakage plus a jagged pattern of the position passes 0.7 are discarded,
 * so the pane opens up in shards around the impact instead of bending. */
int PsPatch(const DWORD *code, int sem, DWORD **out, char *why, int whyn)
{
#define FAIL(m) do { snprintf(why, whyn, "%s", m); goto fail; } while (0)
    XBuf *dis = NULL, *res = NULL, *err = NULL; Str s = {0};
    char *txt = NULL; char **L = NULL;
    *out = NULL;
    if (code[0] != 0xFFFF0300) FAIL("not ps_3_0");
    if (!p_dis || FAILED(p_dis(code, FALSE, NULL, &dis))) FAIL("disassembly failed");
    {
        const char *d = (const char *)dis->vt->Ptr(dis); size_t dl = strlen(d);
        txt = (char *)malloc(dl + 1); memcpy(txt, d, dl + 1);
    }
    int nl = 0, cap = 256; L = (char **)malloc(cap * sizeof *L);
    for (char *p = strtok(txt, "\n"); p; p = strtok(NULL, "\n")) {
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p); while (l && (p[l - 1] == '\r' || p[l - 1] == ' ')) p[--l] = 0;
        if (!*p || StartsWith(p, "//")) continue;
        if (nl == cap) { cap *= 2; L = (char **)realloc(L, cap * sizeof *L); }
        L[nl++] = p;
    }
    if (!nl || !StartsWith(L[0], "ps_3_0")) FAIL("unexpected header");
    BYTE vu[10] = {0}, cu[224] = {0}; int maxr = -1, lastDcl = 0;
    for (int i = 1; i < nl; i++) {
        const char *l = L[i]; int r;
        if (StartsWith(l, "dcl") || StartsWith(l, "def")) lastDcl = i;
        if (StartsWith(l, "dcl")) {
            const char *v = strstr(l, " v");
            if (v && sscanf(v + 2, "%d", &r) == 1 && r < 10) vu[r] = 1;
            int u; if (sscanf(l, "dcl_texcoord%d", &u) == 1 && u == sem) FAIL("semantic in use");
        }
        for (const char *q = l; *q; q++) {
            if (q > l && isalnum((BYTE)q[-1])) continue;
            if (*q == 'c' && isdigit((BYTE)q[1]) && (r = atoi(q + 1)) < 224) cu[r] = 1;
            if (*q == 'r' && isdigit((BYTE)q[1]) && (r = atoi(q + 1)) > maxr) maxr = r;
        }
    }
    int vk = -1; for (int r = 9; r >= 0 && vk < 0; r--) if (!vu[r]) vk = r;
    int ca = -1; for (int r = 223; r > 0 && ca < 0; r--) if (!cu[r] && !cu[r - 1]) ca = r - 1;
    if (vk < 0 || ca < 0 || maxr >= 31) FAIL("no free registers");
    int t = maxr + 1, cb = ca + 1;
    Put(&s, "ps_3_0\n");
    Putf(&s, "def c%d, %.4f, 0.7, 0.35, 0\n", ca, VSP_FREQ * 4.0);
    Putf(&s, "def c%d, 2, -1, 0.333333, 0\n", cb);
    for (int i = 1; i <= lastDcl; i++) { Put(&s, L[i]); Put(&s, "\n"); }
    Putf(&s, "dcl_texcoord%d v%d\n", sem, vk);
    Putf(&s, "mul r%d.xyz, v%d, c%d.x\n", t, vk, ca);
    Putf(&s, "frc r%d.xyz, r%d\n", t, t);
    Putf(&s, "mad r%d.xyz, r%d, c%d.x, c%d.y\n", t, t, cb, cb);
    Putf(&s, "abs r%d.xyz, r%d\n", t, t);
    Putf(&s, "dp3 r%d.w, r%d, c%d.z\n", t, t, cb);           /* jagged pattern 0..1 */
    Putf(&s, "mad r%d.w, r%d.w, c%d.z, v%d.w\n", t, t, ca, vk);
    Putf(&s, "add r%d.x, -r%d.w, c%d.y\n", t, t, ca);
    Putf(&s, "mov r%d.yz, r%d.x\n", t, t);
    Putf(&s, "texkill r%d\n", t);
    for (int i = lastDcl + 1; i < nl; i++) { Put(&s, L[i]); Put(&s, "\n"); }
#ifdef VSP_TEST
    if (getenv("VSP_DUMP")) fputs(s.p, stdout);
#endif
    if (!p_asm || FAILED(p_asm(s.p, (UINT)s.n, NULL, NULL, 0, &res, &err))) {
        snprintf(why, whyn, "assembly failed: %.200s", err ? (const char *)err->vt->Ptr(err) : "?");
        goto fail;
    }
    {
        DWORD n = res->vt->Size(res);
        *out = (DWORD *)malloc(n); memcpy(*out, res->vt->Ptr(res), n);
    }
    free(L); free(txt); free(s.p);
    dis->vt->Release(dis); res->vt->Release(res); if (err) err->vt->Release(err);
    why[0] = 0;
    return 1;
fail:
    free(L); free(txt); free(s.p);
    if (dis) dis->vt->Release(dis);
    if (res) res->vt->Release(res);
    if (err) err->vt->Release(err);
    return 0;
#undef FAIL
}

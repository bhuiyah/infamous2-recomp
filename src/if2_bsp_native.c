/* src/if2_bsp_native.c -- the BSP polygon clipper of SPU job 216 (jobmod_9cb47b0cc0eb_e60c0, LS 0x68D8 and the three
 * functions it calls), natively.
 *
 * The job turns objects made of convex polygons into RSX command buffers of inline vertices, clipping them through
 * a BSP tree it builds in local store as it goes; most of its time is these four functions:
 *   0x68D8  push a list of polygons through the tree: at each node split the list by the node's plane, keep the
 *           negative side for child 0 (on a stack) and descend into child 1 with the positive side; at a missing
 *           child 0 the negative side is dropped (0x5A98), at a missing child 1 the positive side becomes new nodes
 *           (0x4A88) and the node links to them;
 *   0x60A0  split a polygon list by a plane into two lists;
 *   0x5A98  the same, keeping only the positive side;
 *   0x4A88  append the planes of a list as nodes.
 * Pure local store: everything the four functions store, in the same places, the stack frames included, and the
 * registers a caller can see as they leave them (r0, r1, r80..r127). SPU single precision = host SSE under
 * round-toward-zero, FTZ and DAZ with every zero made +0; an operand with exponent 255 or an overflow abandons the
 * run, which undoes its stores (journal), and the lifted code runs.
 *
 * Hooked with spu_registry_replace (tools/lift_jobs.sh SPU_JOBMOD_INDIRECT_CALLS makes the brsl to 0x68D8
 * replaceable). BSP_NATIVE=0: off; =2: the live check (the native on a copy of the local store and registers, the
 * lifted code on the real ones, the two compared).
 *
 * Adapted from MIT-licensed code:
 *   Copyright (c) 2026 the infamous2-recomp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of
 * the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <immintrin.h>
#include <intrin.h>

#include "spu_context.h"
#include "spu_helpers.h"

#if !defined(__SSE4_1__) || !defined(__FMA__)
#error "if2_bsp_native.c needs SSE4.1 and FMA (built like the lifted SPU sources)"
#endif
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#define J220_IMAGE 216   /* jobmod_9cb47b0cc0eb_e60c0 */
#define J220_BSP   0x68D8u
#define LSQ        0x3FFF0u
#define CSR_FAST   0xFFC0u               /* RZ | FZ | DAZ, all exceptions masked */
#define NOINL      __attribute__((noinline))

typedef __m128i V;                       /* a quad: lane i = SPU word i */

/* Copy-on-first-write journal of local-store quads, to undo an abandoned run */
typedef struct {
    uint64_t bm[SPU_LS_SIZE / 16 / 64];
    uint32_t n;
    uint16_t list[SPU_LS_SIZE / 16];
    uint8_t save[SPU_LS_SIZE];
} Journal;

typedef struct {
    uint8_t* ls;
    Journal* j;
    int bad;                             /* abandon: 1 float, 2 layout, 3 runaway */
} B;

#define BSW _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)
static inline V LQ(const B* b, uint32_t a) { return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(b->ls + (a & LSQ))), BSW); }
static inline void SQ(B* b, uint32_t a, V v)
{
    const uint32_t q = a & LSQ, i = q >> 4;
    Journal* j = b->j;
    if (!(j->bm[i >> 6] >> (i & 63) & 1)) {
        j->bm[i >> 6] |= 1ull << (i & 63);
        j->list[j->n++] = (uint16_t)i;
        memcpy(j->save + q, b->ls + q, 16);
    }
    _mm_storeu_si128((__m128i*)(b->ls + q), _mm_shuffle_epi8(v, BSW));
}
static void j_undo(Journal* j, uint8_t* ls)
{
    for (uint32_t k = j->n; k-- > 0;) {
        const uint32_t i = j->list[k];
        memcpy(ls + 16 * i, j->save + 16 * i, 16);
        j->bm[i >> 6] = 0;
    }
    j->n = 0;
}
static void j_clear(Journal* j)
{
    for (uint32_t k = 0; k < j->n; k++) j->bm[j->list[k] >> 6] = 0;
    j->n = 0;
}

#define W(v, i)      ((uint32_t)_mm_extract_epi32((v), (i)))
#define SETW(v, i, x) _mm_insert_epi32((v), (int)(x), (i))
#define ROT1(v)      _mm_shuffle_epi32((v), _MM_SHUFFLE(0, 3, 2, 1))   /* rotqbyi 4 */
#define ROT2(v)      _mm_shuffle_epi32((v), _MM_SHUFFLE(1, 0, 3, 2))   /* rotqbyi 8 */
#define ROT3(v)      _mm_shuffle_epi32((v), _MM_SHUFFLE(2, 1, 0, 3))   /* rotqbyi 12 */
#define SPLAT(v, i)  _mm_shuffle_epi32((v), _MM_SHUFFLE(i, i, i, i))
#define ADDI(v, k)   _mm_add_epi32((v), _mm_set1_epi32((int)(k)))
static inline V splat(uint32_t x) { return _mm_set1_epi32((int)x); }

/* replace word i of the quad at a (cwd + shufb + stqd) */
static inline void SW(B* b, uint32_t a, int i, uint32_t x)
{
    V q = LQ(b, a);
    switch (i) { case 0: q = SETW(q, 0, x); break; case 1: q = SETW(q, 1, x); break;
                 case 2: q = SETW(q, 2, x); break; default: q = SETW(q, 3, x); break; }
    SQ(b, a, q);
}

/* ------------------------------------------------------------------ SPU single precision */
#define FV(v) _mm_castsi128_ps(v)
#define IV(v) _mm_castps_si128(v)
/* The arithmetic is plain SSE: with MXCSR at CSR_FAST, x86 gives the SPU's
 * result unless an operand has exponent 255 (a number to the SPU, Inf or NaN
 * to x86) or the result overflows (the SPU goes on into exponent 255). Loads
 * that feed arithmetic are checked for exponent 255 (e255), and the run is
 * abandoned on the overflow flag (j220_bsp_run). A zero may come out -0,
 * which the SPU never gives: canon() where a sign could be seen. */
static inline __m128 fs(V x, V y) { return _mm_sub_ps(FV(x), FV(y)); }
static inline __m128 fm(V x, V y) { return _mm_mul_ps(FV(x), FV(y)); }
static inline __m128 fma_(V x, V y, V z) { return _mm_fmadd_ps(FV(x), FV(y), FV(z)); }
static inline __m128 fnms(V x, V y, V z) { return _mm_fnmadd_ps(FV(x), FV(y), FV(z)); }
static inline V canon(__m128 r)
{
    const V v = IV(r);
    return _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(v, splat(0x7F800000u)), _mm_setzero_si128()), v);
}
/* exponent 255 in a lane of lm */
static inline void e255(B* b, V v, int lm)
{
    const V e = _mm_cmpeq_epi32(_mm_and_si128(v, splat(0x7F800000u)), splat(0x7F800000u));
    if (__builtin_expect(_mm_movemask_ps(FV(e)) & lm, 0)) b->bad = 1;
}
#define XYZ 7
#define ALL 15
/* fcgt on two values that are results or constants (no exponent 0 but +0, no 255) */
static inline int fgt0(V x, V y) { return _mm_comigt_ss(FV(x), FV(y)); }
static inline V vabs(V x) { return _mm_and_si128(x, splat(0x7FFFFFFFu)); }
static inline V frest_fi(V x)
{
    u128 a, e; memcpy(&a, &x, 16);
    e = spu_frest(a);
    const u128 r = spu_fi(a, e);
    V o; memcpy(&o, &r, 16); return o;
}
/* cuflt(cfltu(x, 141), 123): x scaled to 32-bit fixed point and back */
static inline uint32_t fix32(uint32_t x)
{
    const uint32_t e = x >> 23 & 0xFF;
    if ((x & 0x80000000u) || e == 0) return 0;
    uint32_t u;
    if (e >= 127) u = 0xFFFFFFFFu;
    else {
        const uint32_t m = (x & 0x7FFFFF) | 0x800000;
        u = e >= 118 ? m << (e - 118) : (118 - e >= 32 ? 0 : m >> (118 - e));
    }
    if (!u) return 0;
    const int n = 31 - __builtin_clz(u);
    const uint32_t mant = n >= 23 ? u >> (n - 23) : u << (23 - n);
    return (uint32_t)(n + 127 - 32) << 23 | (mant & 0x7FFFFF);
}
static inline V vfix32(V x)
{
    return _mm_setr_epi32((int)fix32(W(x, 0)), (int)fix32(W(x, 1)), (int)fix32(W(x, 2)), (int)fix32(W(x, 3)));
}

/* (x*a + y*b) + z*c of two quads, as the SPU sums the products (lane 0; the
 * w lanes are left out, so they raise nothing) */
static inline V dot3(V x, V y)
{
    const __m128 p = fm(_mm_blend_epi32(x, _mm_setzero_si128(), 8), _mm_blend_epi32(y, _mm_setzero_si128(), 8));
    return IV(_mm_add_ss(_mm_add_ss(p, _mm_movehdup_ps(p)), _mm_movehl_ps(p, p)));
}
/* the plane's signed distance of v (lane 0); v's x, y, z checked */
static inline V pdist(B* b, V pl, V v, V dsp)
{
    e255(b, v, XYZ);
    return IV(_mm_sub_ss(FV(dot3(pl, v)), FV(dsp)));
}

/* ------------------------------------------------------------------ the four functions */
/* append entry quad e to the list at l (word 0 = count) */
static inline void l_append(B* b, uint32_t l, V e)
{
    const V h = LQ(b, l);
    const uint32_t c = W(h, 0);
    SQ(b, l, SETW(h, 0, c + 1));
    SQ(b, c * 16 + l + 16, e);
}

/* the bytes of a polygon of c vertices, lane-wise (the SPU takes c from the
 * polygon's header quad, whose other words go along in the other lanes) */
static inline V PSIZE(V c) { return ADDI(_mm_slli_epi32(ADDI(c, 2), 4), 32); }

/* copy n vertex-pointer quads */
static inline void qcopy(B* b, uint32_t dst, uint32_t src, int32_t n)
{
    for (int32_t k = 0; k < n; k++) SQ(b, dst + 16 * k, LQ(b, src + 16 * k));
}

/* Where the edges A -> C and B -> D cross the plane: the SPU computes the
 * four distances (C, A, D, B) in one quad, both parameters with one
 * reciprocal, and quantizes them to 32-bit fixed point (cfltu/cuflt). */
static inline void crossings(B* b, V pl, V A, V Bv, V C, V D, V* nv1, V* nv2)
{
    const V nx = SPLAT(pl, 0), ny = SPLAT(pl, 1), nz = SPLAT(pl, 2), dsp = SPLAT(pl, 3), one = splat(0x3F800000u);
    e255(b, A, ALL); e255(b, Bv, ALL); e255(b, C, ALL); e255(b, D, ALL);
    const V CA = IV(fs(C, A)), DB = IV(fs(D, Bv));
    const V X = _mm_setr_epi32((int)W(C, 0), (int)W(A, 0), (int)W(D, 0), (int)W(Bv, 0));
    const V Y = _mm_setr_epi32((int)W(C, 1), (int)W(A, 1), (int)W(D, 1), (int)W(Bv, 1));
    const V Z = _mm_setr_epi32((int)W(C, 2), (int)W(A, 2), (int)W(D, 2), (int)W(Bv, 2));
    const V dd = canon(fs(IV(fma_(Z, nz, IV(fma_(X, nx, IV(fm(Y, ny)))))), dsp));
    const V dr = ROT1(dd);
    /* lanes 1 and 3 are not used: 1.0 there, so they raise nothing */
    const V den = _mm_blend_epi32(canon(fs(dr, dd)), one, 0xA);
    const V e = frest_fi(den);
    e255(b, e, ALL);
    const V rcp = IV(fma_(IV(fnms(e, den, one)), e, e));
    const V t = vfix32(IV(fm(rcp, dr)));
    *nv1 = canon(fma_(SPLAT(t, 0), CA, A));
    *nv2 = canon(fma_(SPLAT(t, 2), DB, Bv));
}

/* 0x60A0 (both = 1: negative side to list ln, positive to lp) and 0x5A98
 * (both = 0: positive side only). sp, r1: 0x68D8's stack pointer and r1. */
static void split(B* b, int both, uint32_t S, V pl, uint32_t L, uint32_t ln, uint32_t lp, V r1)
{
    const uint32_t sp = W(r1, 0);
    SQ(b, sp - 0x50, r1);
    const int32_t cnt = (int32_t)W(LQ(b, L), 0);
    if (cnt <= 0) return;
    const V dsp = SPLAT(pl, 3);
    e255(b, pl, ALL);
    const V c99 = splat(0x3F7FBE76u), c01 = splat(0x3DCCCCCCu);
    uint32_t ea = L + 16;
    for (int32_t k = 0; k < cnt && !b->bad; k++, ea += 16) {
        const V E = LQ(b, ea);
        const uint32_t p = W(E, 0);
        if (p & 15) { b->bad = 2; return; }
        /* coplanar: parallel planes and vertex 0 within 0.1 */
        const V pp = LQ(b, p);
        const V v0 = LQ(b, W(LQ(b, p + 0x20), 0));
        e255(b, pp, XYZ);
        const V dn = dot3(pl, pp);
        const V d0 = pdist(b, pl, v0, dsp);
        if (fgt0(vabs(dn), c99) && fgt0(c01, vabs(d0))) {
            l_append(b, lp, E);
            if (both) l_append(b, ln, E);
            continue;
        }
        const int32_t n = (int32_t)W(LQ(b, p + 0x10), 1);
        if (n <= 0 || n > 4096) { b->bad = 2; return; }
        const uint32_t vb = p + 0x20;
        int32_t slot[3] = { -1, -1, -1 };
        int prev = fgt0(_mm_setzero_si128(), pdist(b, pl, LQ(b, W(LQ(b, vb + 16 * (n - 1)), 0)), dsp));
        int cur = prev;
        for (int32_t i = 0; i < n; i++) {
            cur = fgt0(_mm_setzero_si128(), pdist(b, pl, LQ(b, W(LQ(b, vb + 16 * i), 0)), dsp));
            slot[cur + (prev ^ 1)] = i;
            prev = cur;
        }
        SQ(b, sp - 0x30, splat((uint32_t)slot[0]));
        SQ(b, sp - 0x20, splat((uint32_t)slot[1]));
        SQ(b, sp - 0x10, splat((uint32_t)slot[2]));
        if (b->bad) return;
        const int32_t s0 = slot[0], s2 = slot[2];
        if (s0 < 0) {                                    /* all on one side */
            if (!cur) l_append(b, lp, E);
            else if (both) l_append(b, ln, E);
            continue;
        }
        /* the two crossing edges: A -> C enters the positive side at s0, B -> D leaves it at s2 */
        const int32_t a0 = (s0 + n - 1) % n, a2 = (s2 + n - 1) % n;
        const V sq = LQ(b, S + 0x50);
        const V t32 = ADDI(ROT3(sq), -32), t16 = ADDI(ROT3(sq), -16);
        if (W(t32, 0) & 15) { b->bad = 2; return; }
        SQ(b, S + 0x50, SETW(sq, 3, W(t32, 0)));
        const uint32_t ea0 = W(LQ(b, vb + 16 * a0), 0), ea2 = W(LQ(b, vb + 16 * a2), 0);
        const uint32_t ec = W(LQ(b, vb + 16 * s0), 0), ed = W(LQ(b, vb + 16 * s2), 0);
        const V A = LQ(b, ea0), Bv = LQ(b, ea2), C = LQ(b, ec), D = LQ(b, ed);
        V nv1, nv2;
        crossings(b, pl, A, Bv, C, D, &nv1, &nv2);
        if (b->bad) return;
        SQ(b, W(t32, 0), nv1);
        SQ(b, W(t16, 0), nv2);
        V P1, P2 = _mm_setzero_si128();
        if (s2 > s0) {                                   /* positive run s0..s2-1 */
            const int32_t c1 = s2 - s0;
            P1 = ADDI(t32, -(32 + 16 * (c1 + 2)));
            const uint32_t q1 = W(P1, 0);
            const V h = LQ(b, p + 0x10);
            SQ(b, S + 0x50, SETW(LQ(b, S + 0x50), 3, q1));
            SW(b, q1 + 0x10, 0, W(h, 0));
            SW(b, q1 + 0x10, 1, (uint32_t)(c1 + 2));
            qcopy(b, q1 + 0x20, vb + 16 * s0, c1);
            SQ(b, q1 + 16 * c1 + 0x30, t32);
            SQ(b, q1 + 16 * c1 + 0x20, t16);
            if (both) {                                  /* negative: 0..s0-1, new1, new2, s2..n-1 */
                const int32_t c2 = n - c1;
                const V h2 = LQ(b, p + 0x10), sq3 = LQ(b, S + 0x50);
                P2 = _mm_sub_epi32(ROT3(sq3), PSIZE(ADDI(ROT1(h2), -c1)));
                const uint32_t q2 = W(P2, 0);
                SQ(b, S + 0x50, SETW(sq3, 3, q2));
                SW(b, q2 + 0x10, 0, W(h2, 0));
                SW(b, q2 + 0x10, 1, (uint32_t)(c2 + 2));
                qcopy(b, q2 + 0x20, vb, s0);
                qcopy(b, q2 + 0x20 + 16 * (s0 + 2), vb + 16 * s2, n - s2);
                SQ(b, q2 + 16 * s0 + 0x30, t16);
                SQ(b, q2 + 16 * s0 + 0x20, t32);
            }
        } else {                                         /* positive run s0..n-1, 0..s2-1 */
            const int32_t c1 = n - (s0 - s2);
            const V h = LQ(b, p + 0x10);
            P1 = _mm_sub_epi32(t32, PSIZE(ADDI(ROT1(h), -(s0 - s2))));
            const uint32_t q1 = W(P1, 0);
            SQ(b, S + 0x50, SETW(LQ(b, S + 0x50), 3, q1));
            SW(b, q1 + 0x10, 0, W(h, 0));
            SW(b, q1 + 0x10, 1, (uint32_t)(c1 + 2));
            qcopy(b, q1 + 0x20, vb, s2);
            qcopy(b, q1 + 0x20 + 16 * (s2 + 2), vb + 16 * s0, n - s0);
            if (both) {
                SQ(b, q1 + 16 * s2 + 0x30, t32);
                SQ(b, q1 + 16 * s2 + 0x20, t16);
                const int32_t c2 = s0 - s2;              /* negative: s2..s0-1, new1, new2 */
                const V sq3 = LQ(b, S + 0x50), h2 = LQ(b, p + 0x10);
                P2 = ADDI(ROT3(sq3), -(32 + 16 * (c2 + 2)));
                const uint32_t q2 = W(P2, 0);
                SQ(b, S + 0x50, SETW(sq3, 3, q2));
                SW(b, q2 + 0x10, 0, W(h2, 0));
                SW(b, q2 + 0x10, 1, (uint32_t)(c2 + 2));
                qcopy(b, q2 + 0x20, vb + 16 * s2, c2);
                SQ(b, q2 + 16 * c2 + 0x30, t16);
                SQ(b, q2 + 16 * c2 + 0x20, t32);
            } else {
                SQ(b, q1 + 16 * s2 + 0x20, t16);
                SQ(b, q1 + 16 * s2 + 0x30, t32);
            }
        }
        SQ(b, W(P1, 0), LQ(b, p));
        l_append(b, lp, P1);
        if (both) {
            SQ(b, W(P2, 0), LQ(b, p));
            l_append(b, ln, P2);
        }
    }
}

/* 0x4A88: the planes of list l as nodes, last entry first; word 0 of each
 * node's second quad is 0 for the last one. 0: the table is full (the
 * overflow flag is set) */
static int add_nodes(B* b, uint32_t S, uint32_t l)
{
    int32_t i = (int32_t)W(LQ(b, l), 0) - 1;
    if (i < 0) return 1;
    const uint32_t base = W(LQ(b, S + 0x30), 1);
    if (base & 15) { b->bad = 2; return 0; }
    for (;; i--) {
        const uint32_t cap = W(LQ(b, S + 0x40), 1);
        const V s30 = LQ(b, S + 0x30);
        const V e = LQ(b, l + 16 + 16 * (uint32_t)i);
        if (!((int32_t)cap > (int32_t)W(s30, 2))) {        /* full: the overflow flag, byte 0 of S+0x30 */
            const V q = LQ(b, S + 0x30);
            SQ(b, S + 0x30, SETW(q, 0, (W(q, 0) & 0x00FFFFFFu) | 0x01000000u));
            return 0;
        }
        const uint32_t c = W(s30, 2);
        SQ(b, S + 0x30, SETW(s30, 2, c + 1));
        const uint32_t rec = c * 32 + base;
        SQ(b, rec, LQ(b, W(e, 0)));
        SW(b, rec + 0x10, 0, i != 0);
        SW(b, rec + 0x10, 1, 0);
        if (i == 0) return 1;
    }
}

/* 0x68D8 */
static NOINL void bsp(B* b, const u128* g)
{
    V r[128];
    memcpy(r, g, sizeof r);
    const uint32_t sp = W(r[1], 0);
    const uint32_t S = W(r[3], 0);
    SQ(b, sp - 0x30, r[82]); SQ(b, sp + 0x10, r[0]); SQ(b, sp - 0x10, r[80]); SQ(b, sp - 0x20, r[81]);
    SQ(b, sp - 0x40, r[83]); SQ(b, sp - 0x50, r[84]); SQ(b, sp - 0x60, r[85]); SQ(b, sp - 0x70, r[86]);
    SQ(b, sp - 0x80, r[87]); SQ(b, sp - 0xA0, r[1]);
    const V r1 = ADDI(r[1], -160);
    if (S & 15) { b->bad = 2; return; }
    V N = r[4], L = r[5];
    if (!W(N, 0)) return;
    unsigned steps = 0;
    while (!(W(LQ(b, S + 0x30), 0) >> 24)) {             /* until the overflow flag */
        if (++steps > 1u << 20) { b->bad = 3; return; }
        const uint32_t n = W(N, 0);
        if (n & 15) { b->bad = 2; return; }
        /* the positive-side list */
        const V lh = LQ(b, W(L, 0)), s50 = LQ(b, S + 0x50);
        const V Lp = _mm_sub_epi32(ROT3(s50), ADDI(_mm_slli_epi32(lh, 4), 16));
        SQ(b, S + 0x50, SETW(s50, 3, W(Lp, 0)));
        if (W(Lp, 0) & 15) { b->bad = 2; return; }
        SW(b, W(Lp, 0), 0, 0);
        const V kids = LQ(b, n + 0x10);
        if (!W(kids, 0)) {
            split(b, 0, S, LQ(b, n), W(L, 0), 0, W(Lp, 0), r1);
        } else {
            const V lh2 = LQ(b, W(L, 0)), s50b = LQ(b, S + 0x50);
            const V Ln = _mm_sub_epi32(Lp, ADDI(_mm_slli_epi32(lh2, 4), 16));
            SQ(b, S + 0x50, SETW(s50b, 3, W(Ln, 0)));
            if (W(Ln, 0) & 15) { b->bad = 2; return; }
            SW(b, W(Ln, 0), 0, 0);
            split(b, 1, S, LQ(b, n), W(L, 0), W(Ln, 0), W(Lp, 0), r1);
            if (b->bad) return;
            if ((int32_t)W(LQ(b, W(Ln, 0)), 0) > 0) {   /* push (child 0, negative list) */
                const V s40 = LQ(b, S + 0x40);
                const uint32_t c = W(s40, 3);
                SQ(b, S + 0x40, SETW(s40, 3, c + 1));
                const uint32_t rec = c * 32 + W(s40, 2);
                SQ(b, rec + 0x10, Ln);
                const V k = LQ(b, n + 0x10);
                const V z = _mm_cmpeq_epi32(k, _mm_setzero_si128());
                SQ(b, rec, _mm_andnot_si128(z, _mm_add_epi32(N, _mm_slli_epi32(k, 5))));
            }
        }
        if (b->bad) return;
        V next = _mm_setzero_si128();
        if ((int32_t)W(LQ(b, W(Lp, 0)), 0) > 0) {
            const V k = LQ(b, n + 0x10);
            if (!W(k, 1)) {                              /* no child 1: the list becomes nodes */
                const uint32_t before = W(LQ(b, S + 0x30), 2);
                if (add_nodes(b, S, W(Lp, 0))) {          /* the node now links to them */
                    const uint32_t base = W(LQ(b, S + 0x30), 1);
                    const V k2 = LQ(b, n + 0x10);
                    SQ(b, n + 0x10, SETW(k2, 1, before - (uint32_t)((int32_t)(n - base) >> 5)));
                }
            } else {
                next = _mm_add_epi32(N, _mm_slli_epi32(ROT1(k), 5));
                if (W(next, 0)) { N = next; L = Lp; continue; }
            }
        }
        /* pop, or stop */
        const V s40 = LQ(b, S + 0x40);
        if ((int32_t)W(s40, 3) > 0) {
            const uint32_t c = W(s40, 3);
            SQ(b, S + 0x40, SETW(s40, 3, c - 1));
            const uint32_t rec = c * 32 + W(s40, 2);
            L = LQ(b, rec - 0x10);
            N = LQ(b, rec - 0x20);
        } else N = next;
        if (!W(N, 0)) return;
    }
}

/* Run 0x68D8 on (gpr, ls). 1: done, the state is the function's result; 0:
 * nothing changed (the lifted function must run). */
static SPU_THREAD_LOCAL Journal* t_j;
static SPU_THREAD_LOCAL int t_why;
int j220_bsp_run(u128* gpr, uint8_t* ls)
{
    if (!t_j) { t_j = (Journal*)calloc(1, sizeof *t_j); if (!t_j) return 0; }
    B b = { ls, t_j, 0 };
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(CSR_FAST);
    bsp(&b, gpr);
    if (_mm_getcsr() & 0x9) b.bad = 1;          /* overflow or invalid */
    _mm_setcsr(csr);
    t_why = b.bad;
    if (b.bad) { j_undo(t_j, ls); return 0; }
    j_clear(t_j);
    return 1;
}
int j220_bsp_why(void) { return t_why; }


/* ---- the hook ---- */
typedef void (*spu_fn_t)(spu_context*);
spu_fn_t spu_registry_replace(int image_id, uint32_t addr, spu_fn_t fn);
static spu_fn_t s_bsp_orig;
static volatile long s_bsp_calls, s_bsp_fallback, s_bsp_cmp, s_bsp_bad;
static int s_bsp_mode = 1;   /* BSP_NATIVE: 0 off, 1 on, 2 live check */
static void bsp_run_orig(spu_context* ctx)
{
    const uint32_t r0 = ctx->gpr[0]._u32[0];
    ctx->host_depth++; s_bsp_orig(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
}
/* BSP_NATIVE=2: the native runs on a copy of the local store and registers, the lifted code on the real ones; then
 * the whole local store and the registers a caller can see (r0, r1, r80..r127) are compared. */
static void bsp_check(spu_context* ctx)
{
    static SPU_THREAD_LOCAL uint8_t* ls;
    static SPU_THREAD_LOCAL u128* gpr;
    if (!ls) { ls = (uint8_t*)malloc(SPU_LS_SIZE); gpr = (u128*)malloc(128 * sizeof(u128)); }
    memcpy(ls, ctx->ls, SPU_LS_SIZE); memcpy(gpr, ctx->gpr, 128 * sizeof(u128));
    const int ok = j220_bsp_run(gpr, ls);
    bsp_run_orig(ctx);
    if (!ok) { _InterlockedIncrement(&s_bsp_fallback); return; }
    const long n = _InterlockedIncrement(&s_bsp_cmp);
    const int bad = memcmp(ls, ctx->ls, SPU_LS_SIZE) != 0;
    int breg = -1;
    for (int r = 0; r < 128 && breg < 0; r++)
        if ((r <= 1 || r >= 80) && memcmp(&gpr[r], &ctx->gpr[r], 16)) breg = r;
    if ((bad || breg >= 0) && _InterlockedIncrement(&s_bsp_bad) <= 8) {
        uint32_t o = 0;
        while (o < SPU_LS_SIZE && ls[o] == ctx->ls[o]) o++;
        fprintf(stderr, "[BSP] differs: LS first at 0x%05X, register r%d\n", bad ? o : 0xFFFFFu, breg);
    }
    if ((n & 1023) == 0) fprintf(stderr, "[BSP] compared %ld calls, %ld differ, %ld declined\n", n, s_bsp_bad, s_bsp_fallback);
}
static void bsp_hook(spu_context* ctx)
{
    if (s_bsp_mode == 2) { bsp_check(ctx); return; }
    _InterlockedIncrement(&s_bsp_calls);
    if (j220_bsp_run(ctx->gpr, ctx->ls)) { SPU_RET(ctx); return; }
    _InterlockedIncrement(&s_bsp_fallback);
    bsp_run_orig(ctx);
}
void if2_bsp_configure(void)
{
    const char* e = getenv("BSP_NATIVE");
    if (e && e[0] == '0') { fprintf(stderr, "[BSP] native clipper off (BSP_NATIVE=0)\n"); return; }
    if (e && e[0] == '2') s_bsp_mode = 2;
    s_bsp_orig = spu_registry_replace(J220_IMAGE, J220_BSP, bsp_hook);
    if (!s_bsp_orig) { fprintf(stderr, "[BSP] 0x68D8 is not lifted as replaceable: native clipper off\n"); return; }
    fprintf(stderr, "[BSP] native clipper %s (image %d, 0x%X)\n", s_bsp_mode == 2 ? "in live-check mode" : "on",
            J220_IMAGE, J220_BSP);
}

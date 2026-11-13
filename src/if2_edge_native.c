/* if2_edge_native.c -- EG1: native replacements for stages of inFamous 2's Edge Geometry job (job module
 * d0d15ec83992, image 222), computing exactly what the SPU code computes. Plan and the decoded layout:
 * analysis/edgegeom/EG_PLAN.md (private), section 9.
 *
 * Built with the lifted SPU code's flags (ps3recomp_apply_spu_isa: the host ISA, no FP contraction,
 * SPU_HELPERS_FP_RZDAZ), so the float helpers below are the very ones the lifted code uses and the SPU
 * thread's MXCSR mode (round toward zero, DAZ, FTZ) applies. Every function works on a local store image
 * and returns 0 when its input is outside what it implements; the caller then runs the SPU code. */
#include <stdint.h>
#include <string.h>
#include "spu_context.h"
#include "spu_helpers.h"

#define LSM (SPU_LS_SIZE - 1u)

/* diagnostics (counters, per-phase TSC ticks) for the offline harness only: shared counters bumped from every
 * SPU thread would bounce their cache line between cores */
#ifdef EG_PHASE_TIMING
#  define EG_T(x) x
#else
#  define EG_T(x)
#endif

static inline uint32_t be32(const uint8_t* ls, uint32_t a)
{
    const uint8_t* p = ls + (a & LSM);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void st32(uint8_t* ls, uint32_t a, uint32_t v)
{
    uint8_t* p = ls + (a & LSM);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline u128 ldq(const uint8_t* ls, uint32_t a) { return spu_ls_load_be(ls + (a & (LSM & ~15u))); }
static inline void stq(uint8_t* ls, uint32_t a, u128 v) { spu_ls_store_be(ls + (a & (LSM & ~15u)), v); }
static inline u128 k4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    u128 r; r._u32[0] = a; r._u32[1] = b; r._u32[2] = c; r._u32[3] = d; return r;
}
static inline u128 splat(u128 v, int lane)
{
    const uint32_t b = 0x00010203u + 0x04040404u * (uint32_t)lane;
    return spu_shufb(v, v, k4(b, b, b, b));
}

/* ---- skinning (LS 0xD0A8: r3 Edge context, r4 matrices, r5 their count, r6 per-vertex blend data) ----
 * Implemented: skinning flavor 3 (4 influences, non-uniform scaling: normals by the cofactor of the
 * blended rows), matrix format 0 (3x4, 48 bytes), a normal table, at most one more table (the tangent),
 * transformed by the blended rotation. Then the normal-like tables are normalized. */
int if2_eg_skin_ref(uint8_t* ls, uint32_t ectx, uint32_t mats, uint32_t nmat, uint32_t blend)
{
    const uint8_t* c = ls + (ectx & LSM);
    if (!(c[0] & 0x80) || (c[0] & 0x10)) return 0;          /* the SPU code's own checks: skip / assert paths */
    if ((c[6] & 15) != 3 || c[7] != 0 || nmat == 0) return 0;
    const uint32_t nv = (uint32_t)c[8] << 8 | c[9];
    const uint32_t tpos = be32(ls, ectx + 0x5C), tnrm = be32(ls, ectx + 0x60);
    uint32_t ext[4], next = 0;
    for (int i = 0; i < 4; i++) { ext[i] = be32(ls, ectx + 0x64 + 4u * i); if (ext[i]) next++; }
    if (!tpos || !tnrm || next > 1 || (next == 1 && !ext[0])) return 0;
    for (int i = 0; i < 4; i++) if (be32(ls, ectx + 0x74 + 4u * i)) return 0;   /* attribute kinds: only 0 seen */
    const uint32_t ttan = ext[0];
    const uint32_t scratch = be32(ls, ectx + 0x10 + 4u * (c[0] & 15u));        /* the next free uniform table */

    /* 1. the matrices, in place: rows [R00 R11 R22 T0], [R01 R12 R20 T1], [R02 R10 R21 T2] */
    const u128 m_l1 = k4(0, 0xFFFFFFFFu, 0, 0), m_l2 = k4(0, 0, 0xFFFFFFFFu, 0);
    const u128 s_r1 = k4(0x14151617u, 0x08090A0Bu, 0x10111213u, 0x0C0D0E0Fu);   /* LS 0xEFB0 */
    const u128 s_r2 = k4(0x18191A1Bu, 0x10111213u, 0x04050607u, 0x0C0D0E0Fu);   /* LS 0xEFF0 */
    for (uint32_t m = 0; m < nmat; m++) {
        const uint32_t a = mats + 48u * m;
        const u128 r0 = ldq(ls, a), r1 = ldq(ls, a + 16), r2 = ldq(ls, a + 32);
        stq(ls, a, spu_selb(spu_selb(r0, r1, m_l1), r2, m_l2));
        stq(ls, a + 16, spu_shufb(r1, spu_selb(r2, r0, m_l1), s_r1));
        stq(ls, a + 32, spu_shufb(r2, spu_selb(r1, r0, m_l2), s_r2));
    }

    /* 2. per vertex, in the SPU code's batches (the blended rows of a batch go to the scratch table) */
    const u128 one = k4(0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u);
    const u128 zero = k4(0, 0, 0, 0);
    const u128 m_xyz = k4(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0);              /* fsmbi 0xFFF0 */
    const u128 k255 = k4(0x3B808080u, 0x3B808080u, 0x3B808080u, 0x3B808080u);   /* ilhu 0x3B80 + iohl 0x8080: 1/255, rounded down */
    const u128 s_yzx = k4(0x04050607u, 0x08090A0Bu, 0x00010203u, 0x0C0D0E0Fu);   /* LS 0xEFA0 */
    const u128 s_zxy = k4(0x08090A0Bu, 0x00010203u, 0x04050607u, 0x0C0D0E0Fu);
    const u128 s_t1 = k4(0x80808080u, 0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x80808080u);    /* LS 0xEEB0 */
    const u128 s_t2 = k4(0x1C1D1E1Fu, 0x04050607u, 0x08090A0Bu, 0x80808080u);    /* LS 0xF050 */
    const uint32_t n4 = (nv + 3u) & ~3u;
    uint32_t bsize = 4;
    if (n4 > 11u) bsize = (uint32_t)(((uint64_t)n4 * 0xAAAAAAABull) >> 33) & ~3u;
    uint32_t done = 0;
    while (done < n4) {
        const uint32_t len = (n4 - done) > bsize ? bsize : (n4 - done);
        for (uint32_t j = 0; j < len; j++) {
            const uint32_t v = done + j;
            const uint8_t* bd = ls + ((blend + 8u * v) & LSM);
            u128 wv = spu_fm(spu_cuflt(k4(bd[0], bd[2], bd[4], bd[6]), 155), k255);
            const u128 w0 = splat(wv, 0), w1 = splat(wv, 1), w2 = splat(wv, 2), w3 = splat(wv, 3);
            const uint32_t i0 = 48u * bd[1], i1 = 48u * bd[3], i2 = 48u * bd[5], i3 = 48u * bd[7];
            u128 row[3];
            for (int r = 0; r < 3; r++) {
                const uint32_t o = mats + 16u * (uint32_t)r;
                u128 acc = spu_fm(w0, ldq(ls, o + i0));
                acc = spu_fma(w1, ldq(ls, o + i1), acc);
                acc = spu_fma(w2, ldq(ls, o + i2), acc);
                acc = spu_fma(w3, ldq(ls, o + i3), acc);
                row[r] = acc;
            }
            const u128 R0 = spu_selb(one, row[0], m_xyz), R1 = spu_selb(m_xyz, row[1], m_xyz),
                       R2 = spu_selb(m_xyz, row[2], m_xyz);
            const u128 T = spu_shufb(spu_shufb(row[1], row[2], s_t1), row[0], s_t2);
            const uint32_t sa = scratch + 48u * j;
            stq(ls, sa, R0); stq(ls, sa + 16, R1); stq(ls, sa + 32, R2);
            /* position */
            const u128 p = ldq(ls, tpos + 16u * v);
            stq(ls, tpos + 16u * v,
                spu_fma(spu_shufb(p, zero, s_zxy), R2, spu_fma(spu_shufb(p, zero, s_yzx), R1, spu_fma(p, R0, T))));
            /* normal: the cofactor of the blended rows */
            {
                const u128 a2 = spu_shufb(R2, R2, s_yzx), b2 = spu_shufb(R2, R2, s_zxy);
                const u128 c1 = spu_shufb(R1, R1, s_zxy), e1 = spu_shufb(R1, R1, s_yzx);
                const u128 d0 = spu_shufb(R0, R0, s_yzx), f0 = spu_shufb(R0, R0, s_zxy);
                const u128 A = spu_fms(a2, b2, spu_fm(c1, d0));
                const u128 B = spu_fms(c1, e1, spu_fm(a2, f0));
                const u128 C = spu_fms(d0, f0, spu_fm(b2, e1));
                const u128 n = ldq(ls, tnrm + 16u * v);
                stq(ls, tnrm + 16u * v,
                    spu_fma(A, spu_shufb(n, zero, s_zxy), spu_fma(B, spu_shufb(n, zero, s_yzx), spu_fm(C, n))));
            }
            if (ttan) {
                const u128 t = ldq(ls, ttan + 16u * v);
                stq(ls, ttan + 16u * v,
                    spu_fma(spu_shufb(t, zero, s_zxy), R2, spu_fma(spu_shufb(t, zero, s_yzx), R1, spu_fm(t, R0))));
            }
        }
        done += len;
    }

    /* 3. normalize the tangent table, then the normal table: (nv + 7) & ~7 vectors, w kept */
    const uint32_t tabs[2] = { ttan, tnrm };
    const u128 s_a = k4(0x00010203u, 0x10111213u, 0x04050607u, 0x14151617u);   /* LS 0xEEE0 */
    const u128 s_b = k4(0x04050607u, 0x14151617u, 0x00010203u, 0x10111213u);   /* rotqbyi 8 */
    const u128 s_z = k4(0x08090A0Bu, 0x18191A1Bu, 0x08090A0Bu, 0x18191A1Bu);   /* LS 0xF000 */
    const u128 s_y = k4(0x18191A1Bu, 0x1C1D1E1Fu, 0x00010203u, 0x04050607u);   /* LS 0xF040 */
    const u128 m_hi = k4(0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);                       /* fsmbi 0xFF */
    const uint32_t n8 = (nv + 7u) & ~7u;
    for (int ti = 0; ti < 2; ti++) {
        const uint32_t tb = tabs[ti];
        if (!tb) continue;
        for (uint32_t v = 0; v < n8; v += 4) {
            const u128 v0 = ldq(ls, tb + 16u * v), v1 = ldq(ls, tb + 16u * v + 16),
                       v2 = ldq(ls, tb + 16u * v + 32), v3 = ldq(ls, tb + 16u * v + 48);
            const u128 Z = spu_selb(spu_shufb(v0, v1, s_z), spu_shufb(v2, v3, s_z), m_hi);
            const u128 t5 = spu_shufb(v2, v3, s_b), t6 = spu_shufb(v0, v1, s_a);
            const u128 Y = spu_shufb(t5, t6, s_y);
            const u128 X = spu_selb(t6, t5, m_hi);
            const u128 l2 = spu_fma(Z, Z, spu_fma(Y, Y, spu_fm(X, X)));
            const u128 rs = spu_fi(l2, spu_frsqest(l2));
            const u128 vv[4] = { v0, v1, v2, v3 };
            for (int k = 0; k < 4; k++) {
                const uint32_t b = 0x00010203u + 0x04040404u * (uint32_t)k;
                stq(ls, tb + 16u * (v + (uint32_t)k), spu_fm(vv[k], spu_shufb(rs, one, k4(b, b, b, 0x10111213u))));
            }
        }
    }

    /* 4. the Edge context's free pointer: the lower of the two inputs' LS addresses */
    st32(ls, ectx + 0x58, mats > blend ? blend : mats);
    return 1;
}

/* ---- skinning, fast form -------------------------------------------------------------------------------
 * The same operations in the same order as if2_eg_skin_ref, on plain SSE/FMA registers: lane moves are
 * register shuffles (no SPU shufb emulation) and each float op is the host op plus the +0 that turns a
 * flushed -0 into the SPU's +0 -- what the RZDAZ helpers do on their fast path. The helpers' other test
 * (a result past +-FLT_MAX, or an exponent-255 operand: the SPU has no infinities) cannot fire here: the
 * inputs are range-checked per vertex (rotations below 2^8, normals and tangents below 2^20, positions and
 * translations below 2^40; normalized groups below 2^60), and with those every product and sum stays far
 * inside the float range. A vertex or group outside them takes the exact helpers. */
#if defined(__clang__) || defined(__GNUC__)
#  define EG_OPQ(v) __asm__("" : "+x"(v))
#else
#  define EG_OPQ(v) ((void)0)
#endif
static inline __m128 ldf(const uint8_t* ls, uint32_t a) { return _mm_castsi128_ps(spu__v(ldq(ls, a))); }
static inline void stf(uint8_t* ls, uint32_t a, __m128 v) { stq(ls, a, spu__u(_mm_castps_si128(v))); }
static inline __m128 nz(__m128 f) { EG_OPQ(f); return _mm_add_ps(f, _mm_setzero_ps()); }
static inline __m128 FM(__m128 a, __m128 b) { EG_OPQ(a); return nz(_mm_mul_ps(a, b)); }
static inline __m128 FMA(__m128 a, __m128 b, __m128 c) { EG_OPQ(a); return nz(_mm_fmadd_ps(a, b, c)); }
static inline __m128 FMS(__m128 a, __m128 b, __m128 c) { EG_OPQ(a); return nz(_mm_fmsub_ps(a, b, c)); }
#define YZX(v) _mm_shuffle_ps((v), (v), _MM_SHUFFLE(3, 0, 2, 1))
#define ZXY(v) _mm_shuffle_ps((v), (v), _MM_SHUFFLE(3, 1, 0, 2))

/* every lane of the 16 bytes at a below 2^e (and no exponent-255 value) */
static inline int below(const uint8_t* ls, uint32_t a, int e_xyz, int e_w)
{
    const __m128i v = _mm_and_si128(spu__v(ldq(ls, a)), _mm_set1_epi32(0x7FFFFFFF));
    const __m128i lim = _mm_setr_epi32((127 + e_xyz) << 23, (127 + e_xyz) << 23, (127 + e_xyz) << 23, (127 + e_w) << 23);
    return _mm_movemask_epi8(_mm_cmplt_epi32(v, lim)) == 0xFFFF;
}

unsigned long long g_eg_skin_fast_v, g_eg_skin_exact_v, g_eg_norm_fast, g_eg_norm_exact;   /* diagnostics */
int if2_eg_skin(uint8_t* ls, uint32_t ectx, uint32_t mats, uint32_t nmat, uint32_t blend)
{
    const uint8_t* c = ls + (ectx & LSM);
    if (!(c[0] & 0x80) || (c[0] & 0x10)) return 0;
    if ((c[6] & 15) != 3 || c[7] != 0 || nmat == 0) return 0;
    const uint32_t nv = (uint32_t)c[8] << 8 | c[9];
    const uint32_t tpos = be32(ls, ectx + 0x5C), tnrm = be32(ls, ectx + 0x60);
    uint32_t ext[4], next = 0;
    for (int i = 0; i < 4; i++) { ext[i] = be32(ls, ectx + 0x64 + 4u * i); if (ext[i]) next++; }
    if (!tpos || !tnrm || next > 1 || (next == 1 && !ext[0])) return 0;
    for (int i = 0; i < 4; i++) if (be32(ls, ectx + 0x74 + 4u * i)) return 0;
    const uint32_t ttan = ext[0];
    const uint32_t scratch = be32(ls, ectx + 0x10 + 4u * (c[0] & 15u));
    const uint32_t n4 = (nv + 3u) & ~3u, n8 = (nv + 7u) & ~7u;

    /* which of the 256 matrix slots an index can name are in range (before the prep moves lanes:
     * the rotation in x y z, the translation in w -- the prep keeps each lane's kind) */
    uint8_t mok[256];   /* 0 out of range, 1 in range, 2 not checked yet (checked on first use) */
    memset(mok, 2, sizeof mok);
    for (uint32_t m = 0; m < nmat; m++)
        mok[m] = (uint8_t)(below(ls, mats + 48u * m, 8, 40) && below(ls, mats + 48u * m + 16, 8, 40) &&
                           below(ls, mats + 48u * m + 32, 8, 40));

    /* 1. the matrices into the diagonal layout (lane moves only):
     *    [R00 R11 R22 T0], [R01 R12 R20 T1], [R02 R10 R21 T2] */
    for (uint32_t m = 0; m < nmat; m++) {
        const uint32_t a = mats + 48u * m;
        const __m128 r0 = ldf(ls, a), r1 = ldf(ls, a + 16), r2 = ldf(ls, a + 32);
        const __m128 o0 = _mm_blend_ps(_mm_blend_ps(r0, r1, 2), r2, 4);
        const __m128 o1 = _mm_setr_ps(_mm_cvtss_f32(_mm_shuffle_ps(r0, r0, 1)), _mm_cvtss_f32(_mm_shuffle_ps(r1, r1, 2)),
                                      _mm_cvtss_f32(r2), _mm_cvtss_f32(_mm_shuffle_ps(r1, r1, 3)));
        const __m128 o2 = _mm_setr_ps(_mm_cvtss_f32(_mm_shuffle_ps(r0, r0, 2)), _mm_cvtss_f32(r1),
                                      _mm_cvtss_f32(_mm_shuffle_ps(r2, r2, 1)), _mm_cvtss_f32(_mm_shuffle_ps(r2, r2, 3)));
        stf(ls, a, o0); stf(ls, a + 16, o1); stf(ls, a + 32, o2);
    }

    /* 2. per vertex, batch by batch (the rows of a batch go to the scratch table). A vertex whose inputs
     *    are outside the ranges takes the exact helpers (the SPU code's semantics for any value). */
    const __m128 one = _mm_set1_ps(1.0f), zero = _mm_setzero_ps();
    const __m128 k255 = _mm_castsi128_ps(_mm_set1_epi32(0x3B808080));
    const u128 uone = k4(0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u), uzero = k4(0, 0, 0, 0);
    const u128 um_xyz = k4(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
    const u128 uk255 = k4(0x3B808080u, 0x3B808080u, 0x3B808080u, 0x3B808080u);
    const u128 s_yzx = k4(0x04050607u, 0x08090A0Bu, 0x00010203u, 0x0C0D0E0Fu);
    const u128 s_zxy = k4(0x08090A0Bu, 0x00010203u, 0x04050607u, 0x0C0D0E0Fu);
    const u128 s_t1 = k4(0x80808080u, 0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x80808080u);
    const u128 s_t2 = k4(0x1C1D1E1Fu, 0x04050607u, 0x08090A0Bu, 0x80808080u);
    uint32_t bsize = 4;
    if (n4 > 11u) bsize = (uint32_t)(((uint64_t)n4 * 0xAAAAAAABull) >> 33) & ~3u;
    uint32_t done = 0;
    while (done < n4) {
        const uint32_t len = (n4 - done) > bsize ? bsize : (n4 - done);
        for (uint32_t j = 0; j < len; j++) {
            const uint32_t v = done + j;
            const uint8_t* bd = ls + ((blend + 8u * v) & LSM);
            const uint32_t i0 = mats + 48u * bd[1], i1 = mats + 48u * bd[3], i2 = mats + 48u * bd[5],
                           i3 = mats + 48u * bd[7];
            const uint32_t sa = scratch + 48u * j;
            for (int k = 1; k < 8; k += 2)
                if (mok[bd[k]] == 2) {   /* past the list (padding vertices): read before the prep moved nothing there */
                    const uint32_t ma = mats + 48u * bd[k];
                    mok[bd[k]] = (uint8_t)(below(ls, ma, 8, 40) && below(ls, ma + 16, 8, 40) && below(ls, ma + 32, 8, 40));
                }
            const int fast = mok[bd[1]] && mok[bd[3]] && mok[bd[5]] && mok[bd[7]] &&
                             below(ls, tpos + 16u * v, 40, 40) && below(ls, tnrm + 16u * v, 20, 20) &&
                             (!ttan || below(ls, ttan + 16u * v, 20, 20));
            EG_T(if (fast) g_eg_skin_fast_v++; else g_eg_skin_exact_v++;)
            if (fast) {
                const __m128 wv = FM(_mm_cvtepi32_ps(_mm_setr_epi32(bd[0], bd[2], bd[4], bd[6])), k255);
                const __m128 w0 = _mm_shuffle_ps(wv, wv, 0x00), w1 = _mm_shuffle_ps(wv, wv, 0x55),
                             w2 = _mm_shuffle_ps(wv, wv, 0xAA), w3 = _mm_shuffle_ps(wv, wv, 0xFF);
                __m128 row[3];
                for (uint32_t r = 0; r < 3; r++) {
                    __m128 acc = FM(w0, ldf(ls, i0 + 16u * r));
                    acc = FMA(w1, ldf(ls, i1 + 16u * r), acc);
                    acc = FMA(w2, ldf(ls, i2 + 16u * r), acc);
                    row[r] = FMA(w3, ldf(ls, i3 + 16u * r), acc);
                }
                const __m128 R0 = _mm_blend_ps(row[0], one, 8), R1 = _mm_blend_ps(row[1], zero, 8),
                             R2 = _mm_blend_ps(row[2], zero, 8);
                const __m128 tw = _mm_shuffle_ps(row[0], row[1], _MM_SHUFFLE(3, 3, 3, 3));   /* r0.w r0.w r1.w r1.w */
                const __m128 T = _mm_blend_ps(_mm_shuffle_ps(tw, row[2], _MM_SHUFFLE(3, 3, 2, 0)), zero, 8);
                stf(ls, sa, R0); stf(ls, sa + 16, R1); stf(ls, sa + 32, R2);
                const __m128 p = ldf(ls, tpos + 16u * v);
                stf(ls, tpos + 16u * v, FMA(ZXY(p), R2, FMA(YZX(p), R1, FMA(p, R0, T))));
                {
                    const __m128 a2 = YZX(R2), b2 = ZXY(R2), c1 = ZXY(R1), e1 = YZX(R1), d0 = YZX(R0), f0 = ZXY(R0);
                    const __m128 A = FMS(a2, b2, FM(c1, d0));
                    const __m128 B = FMS(c1, e1, FM(a2, f0));
                    const __m128 C = FMS(d0, f0, FM(b2, e1));
                    const __m128 n = ldf(ls, tnrm + 16u * v);
                    stf(ls, tnrm + 16u * v, FMA(A, ZXY(n), FMA(B, YZX(n), FM(C, n))));
                }
                if (ttan) {
                    const __m128 t = ldf(ls, ttan + 16u * v);
                    stf(ls, ttan + 16u * v, FMA(ZXY(t), R2, FMA(YZX(t), R1, FM(t, R0))));
                }
            } else {
                const u128 wv = spu_fm(spu_cuflt(k4(bd[0], bd[2], bd[4], bd[6]), 155), uk255);
                const u128 w0 = splat(wv, 0), w1 = splat(wv, 1), w2 = splat(wv, 2), w3 = splat(wv, 3);
                u128 row[3];
                for (uint32_t r = 0; r < 3; r++) {
                    u128 acc = spu_fm(w0, ldq(ls, i0 + 16u * r));
                    acc = spu_fma(w1, ldq(ls, i1 + 16u * r), acc);
                    acc = spu_fma(w2, ldq(ls, i2 + 16u * r), acc);
                    row[r] = spu_fma(w3, ldq(ls, i3 + 16u * r), acc);
                }
                const u128 R0 = spu_selb(uone, row[0], um_xyz), R1 = spu_selb(um_xyz, row[1], um_xyz),
                           R2 = spu_selb(um_xyz, row[2], um_xyz);
                const u128 T = spu_shufb(spu_shufb(row[1], row[2], s_t1), row[0], s_t2);
                stq(ls, sa, R0); stq(ls, sa + 16, R1); stq(ls, sa + 32, R2);
                const u128 p = ldq(ls, tpos + 16u * v);
                stq(ls, tpos + 16u * v, spu_fma(spu_shufb(p, uzero, s_zxy), R2,
                                                spu_fma(spu_shufb(p, uzero, s_yzx), R1, spu_fma(p, R0, T))));
                const u128 a2 = spu_shufb(R2, R2, s_yzx), b2 = spu_shufb(R2, R2, s_zxy);
                const u128 c1 = spu_shufb(R1, R1, s_zxy), e1 = spu_shufb(R1, R1, s_yzx);
                const u128 d0 = spu_shufb(R0, R0, s_yzx), f0 = spu_shufb(R0, R0, s_zxy);
                const u128 A = spu_fms(a2, b2, spu_fm(c1, d0)), B = spu_fms(c1, e1, spu_fm(a2, f0)),
                           C = spu_fms(d0, f0, spu_fm(b2, e1));
                const u128 n = ldq(ls, tnrm + 16u * v);
                stq(ls, tnrm + 16u * v, spu_fma(A, spu_shufb(n, uzero, s_zxy),
                                                spu_fma(B, spu_shufb(n, uzero, s_yzx), spu_fm(C, n))));
                if (ttan) {
                    const u128 t = ldq(ls, ttan + 16u * v);
                    stq(ls, ttan + 16u * v, spu_fma(spu_shufb(t, uzero, s_zxy), R2,
                                                    spu_fma(spu_shufb(t, uzero, s_yzx), R1, spu_fm(t, R0))));
                }
            }
        }
        done += len;
    }

    /* 3. normalize the tangent table, then the normal table. A group of 4 with a lane past 2^60, or a
     *    zero-length vector (frsqest(0) is an exponent-255 number: a large value to the SPU, an infinity
     *    to the host), takes the exact helpers. */
    const uint32_t tabs[2] = { ttan, tnrm };
    const u128 s_a = k4(0x00010203u, 0x10111213u, 0x04050607u, 0x14151617u);
    const u128 s_b = k4(0x04050607u, 0x14151617u, 0x00010203u, 0x10111213u);
    const u128 s_z = k4(0x08090A0Bu, 0x18191A1Bu, 0x08090A0Bu, 0x18191A1Bu);
    const u128 s_y = k4(0x18191A1Bu, 0x1C1D1E1Fu, 0x00010203u, 0x04050607u);
    const u128 m_hi = k4(0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
    for (int ti = 0; ti < 2; ti++) {
        const uint32_t tb = tabs[ti];
        if (!tb) continue;
        for (uint32_t v = 0; v < n8; v += 4) {
            const uint32_t a = tb + 16u * v;
            int fast = below(ls, a, 60, 60) && below(ls, a + 16, 60, 60) && below(ls, a + 32, 60, 60) &&
                       below(ls, a + 48, 60, 60);
            __m128 rs = zero;
            const __m128 v0 = ldf(ls, a), v1 = ldf(ls, a + 16), v2 = ldf(ls, a + 32), v3 = ldf(ls, a + 48);
            if (fast) {
                __m128 X = v0, Y = v1, Z = v2, W = v3;
                _MM_TRANSPOSE4_PS(X, Y, Z, W);
                const __m128 l2 = FMA(Z, Z, FMA(Y, Y, FM(X, X)));
                const u128 l2u = spu__u(_mm_castps_si128(l2));
                rs = _mm_castsi128_ps(spu__v(spu_fi(l2u, spu_frsqest(l2u))));
                fast = !_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(rs), _mm_set1_epi32(0x7F800000)),
                                                          _mm_set1_epi32(0x7F800000)));
            }
            EG_T(if (fast) g_eg_norm_fast++; else g_eg_norm_exact++;)
            if (fast) {
                stf(ls, a,      FM(v0, _mm_blend_ps(_mm_shuffle_ps(rs, rs, 0x00), one, 8)));
                stf(ls, a + 16, FM(v1, _mm_blend_ps(_mm_shuffle_ps(rs, rs, 0x55), one, 8)));
                stf(ls, a + 32, FM(v2, _mm_blend_ps(_mm_shuffle_ps(rs, rs, 0xAA), one, 8)));
                stf(ls, a + 48, FM(v3, _mm_blend_ps(_mm_shuffle_ps(rs, rs, 0xFF), one, 8)));
                continue;
            }
            const u128 q0 = ldq(ls, a), q1 = ldq(ls, a + 16), q2 = ldq(ls, a + 32), q3 = ldq(ls, a + 48);
            const u128 Z = spu_selb(spu_shufb(q0, q1, s_z), spu_shufb(q2, q3, s_z), m_hi);
            const u128 t5 = spu_shufb(q2, q3, s_b), t6 = spu_shufb(q0, q1, s_a);
            const u128 Y = spu_shufb(t5, t6, s_y), X = spu_selb(t6, t5, m_hi);
            const u128 l2 = spu_fma(Z, Z, spu_fma(Y, Y, spu_fm(X, X)));
            const u128 r = spu_fi(l2, spu_frsqest(l2));
            const u128 qq[4] = { q0, q1, q2, q3 };
            for (int k = 0; k < 4; k++) {
                const uint32_t b = 0x00010203u + 0x04040404u * (uint32_t)k;
                stq(ls, a + 16u * (uint32_t)k, spu_fm(qq[k], spu_shufb(r, uone, k4(b, b, b, 0x10111213u))));
            }
        }
    }
    st32(ls, ectx + 0x58, mats > blend ? blend : mats);
    return 1;
}

/* ---- triangle culling (LS 0xA798: r3 Edge context, r4 the cull flavor; returns the kept index count) ----
 * First the per-vertex pass (LS 0xA3C8): each position (x, y, z, 1) goes through the object rows at ctx+0xB0
 * premultiplied by the view-projection rows at ctx+0xF0, is divided by |w| (frest, fi, one Newton step; x, y
 * and z keep their sign when w < 0), scaled and offset by ctx+0x130 / ctx+0x140, and gets an outcode
 * (bit 7 x > x1, 6 x < x0, 5 y > y1, 4 y < y0, 3 z > zmax, 2 z < zmin, 1 w > 0; the viewport at ctx+0xE0).
 * [x, y, outcode, 0] goes to the vertex scratch table (ctx+0x10 + 4 * (ctx[0] & 15)) for 4 * ceil(n / 4)
 * vertices. Then the triangles, 4 at a time: culled when all three outcodes share a plane bit (0xFC), or,
 * with all three w > 0 and none below zmin, when back-facing (flavors 2 and 3; 1 culls none) or when all
 * three x (or all three y) snap to the same pixel (cflts(v * s + 0.5), s = 2 for ctx[0x150] = 3 or 4).
 * The kept indices are compacted in place, 3 quadwords stored per group of 4 triangles exactly as the SPU
 * code's pipeline does (two leading all-culled groups, a read-merge at the write point, the second and
 * third quadword redirected to the stack past the end of the input), so the bytes after the kept list
 * match too. ctx+0x1A0 gets the kept index count, ctx+0x58 the 16-aligned end of the kept list; r3 = 3 * kept.
 *
 * The fast path computes the same operations with host instructions (see the skinning's note): a group of
 * 4 vertices does when its positions are below 2^20 and its |w| at least 2^-20 (the rows, scale and offset
 * below 2^20), which keeps every result far inside the float range; a vertex whose screen x or y is not
 * below 2^60 marks its triangles for the helpers. */
static inline uint32_t be16(const uint8_t* ls, uint32_t a)
{
    const uint8_t* p = ls + (a & LSM);
    return (uint32_t)p[0] << 8 | p[1];
}
static inline u128 k1(uint32_t a) { return k4(a, a, a, a); }
static inline uint32_t gt1(u128 m, int k) { return m._u32[k] & 1u; }

#define EG_MAXGROUPS 0x1800u      /* triangle groups (4 triangles) */
#define EG_MAXVG 0x1000u          /* vertex groups (4 vertices): the table's 16 bytes a vertex fill the LS */
typedef struct {
    u128 R[4][4], bx0, by0, bx1, by1, zmin, zmax, sx, sy, sz, ox, oy, oz, snap;
    uint32_t tbl, pos;
    int fastv;                    /* the rows, scale, offset below 2^20; bounds and snap finite */
} cull_setup;

static void cull_setup_init(const uint8_t* ls, uint32_t ectx, cull_setup* cs)
{
    const u128 B0 = ldq(ls, ectx + 0xB0), C0 = ldq(ls, ectx + 0xC0), D0 = ldq(ls, ectx + 0xD0);
    const u128 K = k4(0, 0, 0, 0x3F800000u);
    uint32_t big = 0;
    for (int i = 0; i < 4; i++) {
        const u128 F = ldq(ls, ectx + 0xF0 + 16u * (uint32_t)i);
        const u128 row = spu_fma(splat(F, 3), K, spu_fma(splat(F, 2), D0, spu_fma(splat(F, 1), C0, spu_fm(splat(F, 0), B0))));
        for (int j = 0; j < 4; j++) { cs->R[i][j] = splat(row, j); big |= (row._u32[j] & 0x7FFFFFFFu) >= (127u + 20u) << 23; }
    }
    const u128 vp = ldq(ls, ectx + 0xE0);
    const uint32_t hx0 = vp._u32[0] >> 16, hy0 = vp._u32[0] & 0xFFFFu;
    const uint32_t hx1 = ((vp._u32[1] >> 16) + hx0) & 0xFFFFu, hy1 = ((vp._u32[1] & 0xFFFFu) + hy0) & 0xFFFFu;
    const u128 bnd = spu_cuflt(k4(hx0, hy0, hx1, hy1), 155);
    cs->bx0 = splat(bnd, 0); cs->by0 = splat(bnd, 1); cs->bx1 = splat(bnd, 2); cs->by1 = splat(bnd, 3);
    const u128 za = splat(vp, 2), zb = splat(vp, 3), zgt = spu_fcgt(za, zb);
    cs->zmin = spu_selb(za, zb, zgt); cs->zmax = spu_selb(zb, za, zgt);
    const u128 S = ldq(ls, ectx + 0x130), O = ldq(ls, ectx + 0x140);
    cs->sx = splat(S, 0); cs->sy = splat(S, 1); cs->sz = splat(S, 2); cs->ox = splat(O, 0); cs->oy = splat(O, 1); cs->oz = splat(O, 2);
    for (int j = 0; j < 3; j++) big |= (S._u32[j] & 0x7FFFFFFFu) >= (127u + 20u) << 23 || (O._u32[j] & 0x7FFFFFFFu) >= (127u + 20u) << 23;
    big |= (za._u32[0] & 0x7F800000u) == 0x7F800000u || (zb._u32[0] & 0x7F800000u) == 0x7F800000u;
    const uint8_t b150 = ls[(ectx + 0x150) & LSM];
    cs->snap = k1(b150 == 3 || b150 == 4 ? 0x40000000u : 0x3F800000u);
    cs->fastv = !big;
}

/* native per-vertex copy for the triangle pass: [x, y, outcode, ix & 0xFFFF | iy << 16] (ix, iy: the snapped
 * x and y). Outcode bit 0 (never set by the SPU code) marks a vertex the fast triangle math cannot take: one
 * with w > 0 and z >= zmin (the only kind the area and snap tests read) whose x or y is not below 2^60 or
 * whose snapped x or y does not fit 16 bits. */
typedef struct { _Alignas(16) uint32_t v[EG_MAXVG * 4][4]; } cull_verts;

static inline int lt60(uint32_t f) { return (f & 0x7FFFFFFFu) < (127u + 60u) << 23; }
static inline int in16(uint32_t i) { return (int32_t)i == (int16_t)i; }

static void cull_vgroup_ref(uint8_t* ls, const cull_setup* cs, uint32_t g, cull_verts* nv)
{
    u128 P[4];
    for (int k = 0; k < 4; k++) P[k] = ldq(ls, cs->pos + 64u * g + 16u * (uint32_t)k);
    const u128 x = k4(P[0]._u32[0], P[1]._u32[0], P[2]._u32[0], P[3]._u32[0]);
    const u128 y = k4(P[0]._u32[1], P[1]._u32[1], P[2]._u32[1], P[3]._u32[1]);
    const u128 z = k4(P[0]._u32[2], P[1]._u32[2], P[2]._u32[2], P[3]._u32[2]);
    u128 c[4];
    for (int i = 0; i < 4; i++) c[i] = spu_fma(z, cs->R[i][2], spu_fma(y, cs->R[i][1], spu_fma(x, cs->R[i][0], cs->R[i][3])));
    const u128 one = k1(0x3F800000u), zero = k1(0), sgn = k1(0x80000000u), half = k1(0x3F000000u);
    const u128 w = c[3], e = spu_frest(w), f = spu_fi(w, e), r = spu_fma(spu_fnms(f, w, one), f, f);
    const u128 s = spu_and(w, sgn);
    const u128 X = spu_fma(spu_xor(spu_fm(c[0], r), s), cs->sx, cs->ox);
    const u128 Y = spu_fma(spu_xor(spu_fm(c[1], r), s), cs->sy, cs->oy);
    const u128 Z = spu_fma(spu_xor(spu_fm(c[2], r), s), cs->sz, cs->oz);
    const u128 m7 = spu_fcgt(X, cs->bx1), m6 = spu_fcgt(cs->bx0, X), m5 = spu_fcgt(Y, cs->by1), m4 = spu_fcgt(cs->by0, Y);
    const u128 m3 = spu_fcgt(Z, cs->zmax), m2 = spu_fcgt(cs->zmin, Z), m1 = spu_fcgt(w, zero);
    const u128 ix = spu_cflts(spu_fma(X, cs->snap, half), 173), iy = spu_cflts(spu_fma(Y, cs->snap, half), 173);
    for (int k = 0; k < 4; k++) {
        const uint32_t oc = gt1(m7, k) << 7 | gt1(m6, k) << 6 | gt1(m5, k) << 5 | gt1(m4, k) << 4 |
                            gt1(m3, k) << 3 | gt1(m2, k) << 2 | gt1(m1, k) << 1;
        stq(ls, cs->tbl + 64u * g + 16u * (uint32_t)k, k4(X._u32[k], Y._u32[k], oc, 0));
        if (nv) {
            const uint32_t j = 4u * g + (uint32_t)k;
            const int ok = lt60(X._u32[k]) && lt60(Y._u32[k]) && in16(ix._u32[k]) && in16(iy._u32[k]);
            nv->v[j][0] = X._u32[k]; nv->v[j][1] = Y._u32[k];
            nv->v[j][2] = oc | ((oc & 6u) == 2u && !ok);
            nv->v[j][3] = (ix._u32[k] & 0xFFFFu) | iy._u32[k] << 16;
        }
    }
}

/* the cull mask of 4 triangles (bit 3 = the first), indices idx[12], from the table in the local store */
static uint32_t cull_group_ref(const uint8_t* ls, uint32_t vt, const uint32_t* idx, uint32_t flip, int backface, u128 snap)
{
    u128 V[12];
    for (int j = 0; j < 12; j++) V[j] = ldq(ls, vt + 16u * idx[j]);
    u128 X[3], Y[3], iX[3], iY[3];
    uint32_t oc[3][4];
    const u128 half = k1(0x3F000000u);
    for (int v = 0; v < 3; v++) {
        X[v] = k4(V[v]._u32[0], V[3 + v]._u32[0], V[6 + v]._u32[0], V[9 + v]._u32[0]);
        Y[v] = k4(V[v]._u32[1], V[3 + v]._u32[1], V[6 + v]._u32[1], V[9 + v]._u32[1]);
        for (int t = 0; t < 4; t++) oc[v][t] = V[3 * t + v]._u32[2];
        iX[v] = spu_cflts(spu_fma(X[v], snap, half), 173);
        iY[v] = spu_cflts(spu_fma(Y[v], snap, half), 173);
    }
    const u128 dx1 = spu_fs(X[1], X[0]), dx2 = spu_fs(X[2], X[0]), dy1 = spu_fs(Y[1], Y[0]), dy2 = spu_fs(Y[2], Y[0]);
    const u128 area = spu_fms(dx1, dy2, spu_fm(dy1, dx2));
    const u128 bf = spu_fcgt(spu_xor(area, k1(flip)), k1(0));
    uint32_t mask = 0;
    for (int t = 0; t < 4; t++) {
        const uint32_t o = oc[0][t] | oc[1][t] | oc[2][t], a = oc[0][t] & oc[1][t] & oc[2][t];
        const int front = !(o & 4u) && (a & 2u);
        const int degx = iX[0]._u32[t] == iX[1]._u32[t] && iX[1]._u32[t] == iX[2]._u32[t];
        const int degy = iY[0]._u32[t] == iY[1]._u32[t] && iY[1]._u32[t] == iY[2]._u32[t];
        const int cull = (a & 0xFCu) || (front && backface && (bf._u32[t] & 1u)) || (front && (degx || degy));
        mask |= (uint32_t)cull << (3 - t);
    }
    return mask;
}

/* the compaction and its stores: groups g = -2 .. ngroups-1 (the two leading ones all culled) */
static uint32_t cull_store_ref(uint8_t* ls, uint32_t out, uint32_t ngroups, uint32_t ntris, const uint8_t* mask, const uint8_t* src)
{
    const int32_t end = (int32_t)(out + ((6u * ntris + 15u) & ~15u));
    uint32_t dst = out, a = 0, total = 0;
    for (int32_t g = -2; g < (int32_t)ngroups; g++) {
        uint8_t S[32] = { 0 };
        uint32_t m = 15, n = 0;
        if (g >= 0) {
            m = mask[g];
            for (int t = 0; t < 4; t++)
                if (!(m >> (3 - t) & 1u)) { memcpy(S + 6 * n, src + 24u * (uint32_t)g + 6u * (uint32_t)t, 6); n++; }
        }
        uint8_t* q0 = ls + (dst & (LSM & ~15u));
        for (uint32_t k = a; k < 16; k++) q0[k] = S[k - a];
        if (!((int32_t)dst > end - 17)) {
            uint8_t* q1 = ls + ((dst + 16u) & (LSM & ~15u));
            for (uint32_t k = 0; k < 16; k++) q1[k] = S[16 - a + k];
        }
        if (!((int32_t)dst > end - 33)) {
            uint8_t* q2 = ls + ((dst + 32u) & (LSM & ~15u));
            for (uint32_t k = 0; k < 16; k++) q2[k] = k < a ? S[32 - a + k] : S[16 + k - a];
        }
        dst += 6u * n; a = (a + 6u * n) & 15u; total += n;
    }
    return total;
}

typedef struct {
    uint32_t ectx, flavor, nv, tbl, pos, out, ntris, ngroups, tail, flip;
    int backface;
} cull_args;

static int cull_args_init(const uint8_t* ls, uint32_t ectx, uint32_t flavor, cull_args* ca)
{
    if (ectx & 15u) return 0;
    const uint8_t* c = ls + (ectx & LSM);
    if (!(c[0] & 0x80) || flavor < 1 || flavor > 3) return 0;
    if (be32(ls, ectx + 0x194)) return 0;                     /* a custom per-vertex callback */
    if (c[0x150] == 5) return 0;
    ca->ectx = ectx; ca->flavor = flavor;
    ca->nv = be16(ls, ectx + 8);
    ca->tbl = be32(ls, ectx + 0x10 + 4u * (c[0] & 15u)); ca->pos = be32(ls, ectx + 0x5C); ca->out = be32(ls, ectx + 0x50) & LSM;
    if (!ca->nv || (ca->out & 15u)) return 0;
    const uint32_t nidx = be16(ls, ectx + 0x1A0);
    ca->ntris = (uint32_t)(((uint64_t)nidx * 0xAAAAAAABull) >> 33) & 0xFFFFu;
    ca->ngroups = (ca->ntris + 3u) >> 2; ca->tail = ca->ntris & 3u;
    if (ca->ngroups > EG_MAXGROUPS || ca->out + 24u * ca->ngroups > SPU_LS_SIZE) return 0;
    const uint32_t nib = c[6] >> 4u;
    ca->flip = (flavor == 2 ? 0u : 0x80000000u) ^ ((nib == 2 || nib == 0) ? 0x80000000u : 0u);
    ca->backface = flavor != 1;
    return 1;
}

static void cull_finish(uint8_t* ls, const cull_args* ca, uint32_t kept, u128* r3)
{
    const uint32_t cnt = (3u * kept) & 0xFFFFu;
    uint8_t* h = ls + ((ca->ectx + 0x1A0) & LSM);
    h[0] = (uint8_t)(cnt >> 8); h[1] = (uint8_t)cnt;
    st32(ls, ca->ectx + 0x58, (ca->out + 6u * kept + 15u) & ~15u);
    *r3 = k1(3u * kept);                                     /* the path through 0xA9B0 returns 3 * kept, splat */
}

static __declspec(thread) uint8_t t_mask[EG_MAXGROUPS], t_src[24u * EG_MAXGROUPS + 32];
static __declspec(thread) _Alignas(16) uint16_t t_idx[12u * EG_MAXGROUPS + 16];

int if2_eg_cull_ref(uint8_t* ls, uint32_t ectx, uint32_t flavor, u128* r3)
{
    cull_args ca;
    if (!cull_args_init(ls, ectx, flavor, &ca)) return 0;
    cull_setup cs;
    cull_setup_init(ls, ectx, &cs);
    cs.tbl = ca.tbl; cs.pos = ca.pos;
    for (uint32_t g = 0; g < (ca.nv + 3u) >> 2; g++) cull_vgroup_ref(ls, &cs, g, NULL);
    memcpy(t_src, ls + ca.out, 24u * ca.ngroups);
    for (uint32_t g = 0; g < ca.ngroups; g++) {
        uint32_t idx[12];
        for (int j = 0; j < 12; j++) idx[j] = (uint32_t)t_src[24u * g + 2u * j] << 8 | t_src[24u * g + 2u * j + 1];
        t_mask[g] = (uint8_t)cull_group_ref(ls, ca.tbl, idx, ca.flip, ca.backface, cs.snap);
    }
    if (ca.ngroups && ca.tail) t_mask[ca.ngroups - 1] |= (uint8_t)(15u >> ca.tail);
    cull_finish(ls, &ca, cull_store_ref(ls, ca.out, ca.ngroups, ca.ntris, t_mask, t_src), r3);
    return 1;
}

/* ---- the fast path ---- */
static inline __m128 kf(u128 v) { return _mm_castsi128_ps(spu__v(v)); }
static inline __m128 FNMS(__m128 a, __m128 b, __m128 c) { EG_OPQ(a); return nz(_mm_fnmadd_ps(a, b, c)); }
static inline __m128 FS(__m128 a, __m128 b) { EG_OPQ(a); return nz(_mm_sub_ps(a, b)); }
static inline __m128i ABSI(__m128 v) { return _mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7FFFFFFF)); }
static inline __m128i cflts173(__m128 y)   /* cflts(y, 173) for |y| < 2^127: saturating truncation */
{
    return _mm_xor_si128(_mm_cvttps_epi32(y), _mm_castps_si128(_mm_cmpge_ps(y, _mm_set1_ps(2147483648.0f))));
}

unsigned long long g_eg_cull_t[4], g_eg_cull_vfast, g_eg_cull_vexact, g_eg_cull_tfast, g_eg_cull_texact;   /* diagnostics */

static int cull_vgroup_fast(uint8_t* ls, const cull_setup* cs, uint32_t g, cull_verts* nv)
{
    const uint32_t a = cs->pos + 64u * g;
    __m128 p0 = ldf(ls, a), p1 = ldf(ls, a + 16), p2 = ldf(ls, a + 32), p3 = ldf(ls, a + 48);
    _MM_TRANSPOSE4_PS(p0, p1, p2, p3);   /* p0 x, p1 y, p2 z */
    const __m128i lim = _mm_set1_epi32((127 + 20) << 23);
    const __m128i big = _mm_or_si128(_mm_cmpgt_epi32(ABSI(p0), lim), _mm_or_si128(_mm_cmpgt_epi32(ABSI(p1), lim), _mm_cmpgt_epi32(ABSI(p2), lim)));
    __m128 c[4];
    for (int i = 0; i < 4; i++)
        c[i] = FMA(p2, kf(cs->R[i][2]), FMA(p1, kf(cs->R[i][1]), FMA(p0, kf(cs->R[i][0]), kf(cs->R[i][3]))));
    const __m128 w = c[3];
    const __m128i small = _mm_cmplt_epi32(ABSI(w), _mm_set1_epi32((127 - 20) << 23));
    if (!_mm_testz_si128(_mm_or_si128(big, small), _mm_or_si128(big, small))) return 0;
    const __m128 e = kf(spu_frest(spu__u(_mm_castps_si128(w))));
    const __m128 f = kf(spu_fi(spu__u(_mm_castps_si128(w)), spu__u(_mm_castps_si128(e))));
    const __m128 one = _mm_set1_ps(1.0f), sgn = _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u));
    const __m128 r = FMA(FNMS(f, w, one), f, f), s = _mm_and_ps(w, sgn);
    const __m128 X = FMA(_mm_xor_ps(FM(c[0], r), s), kf(cs->sx), kf(cs->ox));
    const __m128 Y = FMA(_mm_xor_ps(FM(c[1], r), s), kf(cs->sy), kf(cs->oy));
    const __m128 Z = FMA(_mm_xor_ps(FM(c[2], r), s), kf(cs->sz), kf(cs->oz));
    __m128 Xo = X; EG_OPQ(Xo);
    const __m128i oc = _mm_or_si128(
        _mm_or_si128(_mm_or_si128(_mm_and_si128(_mm_castps_si128(_mm_cmpgt_ps(Xo, kf(cs->bx1))), _mm_set1_epi32(0x80)),
                                  _mm_and_si128(_mm_castps_si128(_mm_cmpgt_ps(kf(cs->bx0), Xo)), _mm_set1_epi32(0x40))),
                     _mm_or_si128(_mm_and_si128(_mm_castps_si128(_mm_cmpgt_ps(Y, kf(cs->by1))), _mm_set1_epi32(0x20)),
                                  _mm_and_si128(_mm_castps_si128(_mm_cmpgt_ps(kf(cs->by0), Y)), _mm_set1_epi32(0x10)))),
        _mm_or_si128(_mm_or_si128(_mm_and_si128(_mm_castps_si128(_mm_cmpgt_ps(Z, kf(cs->zmax))), _mm_set1_epi32(0x08)),
                                  _mm_and_si128(_mm_castps_si128(_mm_cmpgt_ps(kf(cs->zmin), Z)), _mm_set1_epi32(0x04))),
                     _mm_and_si128(_mm_castps_si128(_mm_cmpgt_ps(w, _mm_setzero_ps())), _mm_set1_epi32(0x02))));
    /* the table entries [x, y, oc, 0], big-endian */
    __m128 t0 = X, t1 = Y, t2 = _mm_castsi128_ps(oc), t3 = _mm_setzero_ps();
    _MM_TRANSPOSE4_PS(t0, t1, t2, t3);
    const __m128i bs = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const uint32_t ta = cs->tbl + 64u * g;
    _mm_storeu_si128((__m128i*)(ls + (ta & (LSM & ~15u))), _mm_shuffle_epi8(_mm_castps_si128(t0), bs));
    _mm_storeu_si128((__m128i*)(ls + ((ta + 16u) & (LSM & ~15u))), _mm_shuffle_epi8(_mm_castps_si128(t1), bs));
    _mm_storeu_si128((__m128i*)(ls + ((ta + 32u) & (LSM & ~15u))), _mm_shuffle_epi8(_mm_castps_si128(t2), bs));
    _mm_storeu_si128((__m128i*)(ls + ((ta + 48u) & (LSM & ~15u))), _mm_shuffle_epi8(_mm_castps_si128(t3), bs));
    /* the native copy */
    const __m128 half = _mm_set1_ps(0.5f);
    const __m128i ix = cflts173(FMA(X, kf(cs->snap), half)), iy = cflts173(FMA(Y, kf(cs->snap), half));
    const __m128i l60 = _mm_set1_epi32(((127 + 60) << 23) - 1);
    const __m128i bad = _mm_or_si128(
        _mm_or_si128(_mm_cmpgt_epi32(ABSI(X), l60), _mm_cmpgt_epi32(ABSI(Y), l60)),
        _mm_andnot_si128(_mm_and_si128(_mm_cmpeq_epi32(ix, _mm_srai_epi32(_mm_slli_epi32(ix, 16), 16)),
                                       _mm_cmpeq_epi32(iy, _mm_srai_epi32(_mm_slli_epi32(iy, 16), 16))), _mm_set1_epi32(-1)));
    const __m128i front = _mm_cmpeq_epi32(_mm_and_si128(oc, _mm_set1_epi32(6)), _mm_set1_epi32(2));
    const __m128i ocs = _mm_or_si128(oc, _mm_and_si128(_mm_and_si128(bad, front), _mm_set1_epi32(1)));
    const __m128i ixy = _mm_or_si128(_mm_and_si128(ix, _mm_set1_epi32(0xFFFF)), _mm_slli_epi32(iy, 16));
    __m128 n0 = X, n1 = Y, n2 = _mm_castsi128_ps(ocs), n3 = _mm_castsi128_ps(ixy);
    _MM_TRANSPOSE4_PS(n0, n1, n2, n3);
    __m128* np = (__m128*)nv->v[4u * g];
    np[0] = n0; np[1] = n1; np[2] = n2; np[3] = n3;
    return 1;
}

/* 8 vertices (groups g, g + 1) in 256-bit vectors: the same operations as cull_vgroup_fast, lane for lane */
static inline __m256 nz8(__m256 f) { EG_OPQ(f); return _mm256_add_ps(f, _mm256_setzero_ps()); }
static inline __m256 FM8(__m256 a, __m256 b) { EG_OPQ(a); return nz8(_mm256_mul_ps(a, b)); }
static inline __m256 FMA8(__m256 a, __m256 b, __m256 c) { EG_OPQ(a); return nz8(_mm256_fmadd_ps(a, b, c)); }
static inline __m256 FNMS8(__m256 a, __m256 b, __m256 c) { EG_OPQ(a); return nz8(_mm256_fnmadd_ps(a, b, c)); }
static inline __m256i ABSI8(__m256 v) { return _mm256_and_si256(_mm256_castps_si256(v), _mm256_set1_epi32(0x7FFFFFFF)); }
static inline __m256 kf8(u128 v) { const __m128 x = kf(v); return _mm256_set_m128(x, x); }
#define EG_TRANSPOSE8(r0, r1, r2, r3) do {                                                     \
        const __m256 t0_ = _mm256_unpacklo_ps(r0, r1), t1_ = _mm256_unpacklo_ps(r2, r3);       \
        const __m256 t2_ = _mm256_unpackhi_ps(r0, r1), t3_ = _mm256_unpackhi_ps(r2, r3);       \
        r0 = _mm256_shuffle_ps(t0_, t1_, 0x44); r1 = _mm256_shuffle_ps(t0_, t1_, 0xEE);        \
        r2 = _mm256_shuffle_ps(t2_, t3_, 0x44); r3 = _mm256_shuffle_ps(t2_, t3_, 0xEE); } while (0)
static inline __m256i cflts173_8(__m256 y)
{
    return _mm256_xor_si256(_mm256_cvttps_epi32(y), _mm256_castps_si256(_mm256_cmp_ps(y, _mm256_set1_ps(2147483648.0f), _CMP_GE_OQ)));
}

/* split in two loops (transform, then project and store) for shorter dependency chains per iteration */
static inline int cull_v2_transform(const uint8_t* ls, const cull_setup* cs, uint32_t g, __m256* c)
{
    const uint32_t a = cs->pos + 64u * g;
    __m256 p[4];
    for (int k = 0; k < 4; k++) p[k] = _mm256_set_m128(ldf(ls, a + 64u + 16u * (uint32_t)k), ldf(ls, a + 16u * (uint32_t)k));
    EG_TRANSPOSE8(p[0], p[1], p[2], p[3]);   /* x, y, z per lane: [group g | group g + 1] */
    const __m256i lim = _mm256_set1_epi32((127 + 20) << 23);
    const __m256i big = _mm256_or_si256(_mm256_cmpgt_epi32(ABSI8(p[0]), lim),
                                        _mm256_or_si256(_mm256_cmpgt_epi32(ABSI8(p[1]), lim), _mm256_cmpgt_epi32(ABSI8(p[2]), lim)));
    for (int i = 0; i < 4; i++)
        c[i] = FMA8(p[2], kf8(cs->R[i][2]), FMA8(p[1], kf8(cs->R[i][1]), FMA8(p[0], kf8(cs->R[i][0]), kf8(cs->R[i][3]))));
    const __m256i small = _mm256_cmpgt_epi32(_mm256_set1_epi32((127 - 20) << 23), ABSI8(c[3]));
    const __m256i chk = _mm256_or_si256(big, small);
    return _mm256_testz_si256(chk, chk);
}

static inline void cull_v2_project(uint8_t* ls, const cull_setup* cs, uint32_t g, const __m256* c, cull_verts* nv)
{
    const __m256 w = c[3];
    const __m128i wl = _mm256_castsi256_si128(_mm256_castps_si256(w)), wh = _mm256_extracti128_si256(_mm256_castps_si256(w), 1);
    const __m128i el = spu__v(spu_frest(spu__u(wl))), eh = spu__v(spu_frest(spu__u(wh)));
    const __m128i fl = spu__v(spu_fi(spu__u(wl), spu__u(el))), fh = spu__v(spu_fi(spu__u(wh), spu__u(eh)));
    const __m256 f = _mm256_castsi256_ps(_mm256_set_m128i(fh, fl));
    const __m256 one = _mm256_set1_ps(1.0f), sgn = _mm256_castsi256_ps(_mm256_set1_epi32((int)0x80000000u));
    const __m256 r = FMA8(FNMS8(f, w, one), f, f), s8 = _mm256_and_ps(w, sgn);
    const __m256 X = FMA8(_mm256_xor_ps(FM8(c[0], r), s8), kf8(cs->sx), kf8(cs->ox));
    const __m256 Y = FMA8(_mm256_xor_ps(FM8(c[1], r), s8), kf8(cs->sy), kf8(cs->oy));
    const __m256 Z = FMA8(_mm256_xor_ps(FM8(c[2], r), s8), kf8(cs->sz), kf8(cs->oz));
    __m256 Xo = X; EG_OPQ(Xo);
#define EG_B8(cmp, bit) _mm256_and_si256(_mm256_castps_si256(cmp), _mm256_set1_epi32(bit))
    const __m256i oc = _mm256_or_si256(
        _mm256_or_si256(_mm256_or_si256(EG_B8(_mm256_cmp_ps(Xo, kf8(cs->bx1), _CMP_GT_OQ), 0x80),
                                        EG_B8(_mm256_cmp_ps(kf8(cs->bx0), Xo, _CMP_GT_OQ), 0x40)),
                        _mm256_or_si256(EG_B8(_mm256_cmp_ps(Y, kf8(cs->by1), _CMP_GT_OQ), 0x20),
                                        EG_B8(_mm256_cmp_ps(kf8(cs->by0), Y, _CMP_GT_OQ), 0x10))),
        _mm256_or_si256(_mm256_or_si256(EG_B8(_mm256_cmp_ps(Z, kf8(cs->zmax), _CMP_GT_OQ), 0x08),
                                        EG_B8(_mm256_cmp_ps(kf8(cs->zmin), Z, _CMP_GT_OQ), 0x04)),
                        EG_B8(_mm256_cmp_ps(w, _mm256_setzero_ps(), _CMP_GT_OQ), 0x02)));
#undef EG_B8
    /* the table entries [x, y, oc, 0], big-endian */
    __m256 t0 = X, t1 = Y, t2 = _mm256_castsi256_ps(oc), t3 = _mm256_setzero_ps();
    EG_TRANSPOSE8(t0, t1, t2, t3);
    const __m256i bs = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m256i T[4] = { _mm256_shuffle_epi8(_mm256_castps_si256(t0), bs), _mm256_shuffle_epi8(_mm256_castps_si256(t1), bs),
                           _mm256_shuffle_epi8(_mm256_castps_si256(t2), bs), _mm256_shuffle_epi8(_mm256_castps_si256(t3), bs) };
    const uint32_t ta = cs->tbl + 64u * g;
    for (int k = 0; k < 4; k++) {
        _mm_storeu_si128((__m128i*)(ls + ((ta + 16u * (uint32_t)k) & (LSM & ~15u))), _mm256_castsi256_si128(T[k]));
        _mm_storeu_si128((__m128i*)(ls + ((ta + 64u + 16u * (uint32_t)k) & (LSM & ~15u))), _mm256_extracti128_si256(T[k], 1));
    }
    /* the native copy */
    const __m256 half = _mm256_set1_ps(0.5f);
    const __m256i ix = cflts173_8(FMA8(X, kf8(cs->snap), half)), iy = cflts173_8(FMA8(Y, kf8(cs->snap), half));
    const __m256i l60 = _mm256_set1_epi32(((127 + 60) << 23) - 1);
    const __m256i bad = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpgt_epi32(ABSI8(X), l60), _mm256_cmpgt_epi32(ABSI8(Y), l60)),
        _mm256_andnot_si256(_mm256_and_si256(_mm256_cmpeq_epi32(ix, _mm256_srai_epi32(_mm256_slli_epi32(ix, 16), 16)),
                                             _mm256_cmpeq_epi32(iy, _mm256_srai_epi32(_mm256_slli_epi32(iy, 16), 16))),
                            _mm256_set1_epi32(-1)));
    const __m256i front = _mm256_cmpeq_epi32(_mm256_and_si256(oc, _mm256_set1_epi32(6)), _mm256_set1_epi32(2));
    const __m256i ocs = _mm256_or_si256(oc, _mm256_and_si256(_mm256_and_si256(bad, front), _mm256_set1_epi32(1)));
    const __m256i ixy = _mm256_or_si256(_mm256_and_si256(ix, _mm256_set1_epi32(0xFFFF)), _mm256_slli_epi32(iy, 16));
    __m256 n0 = X, n1 = Y, n2 = _mm256_castsi256_ps(ocs), n3 = _mm256_castsi256_ps(ixy);
    EG_TRANSPOSE8(n0, n1, n2, n3);
    __m128* np = (__m128*)nv->v[4u * g];
    np[0] = _mm256_castps256_ps128(n0); np[1] = _mm256_castps256_ps128(n1);
    np[2] = _mm256_castps256_ps128(n2); np[3] = _mm256_castps256_ps128(n3);
    np[4] = _mm256_extractf128_ps(n0, 1); np[5] = _mm256_extractf128_ps(n1, 1);
    np[6] = _mm256_extractf128_ps(n2, 1); np[7] = _mm256_extractf128_ps(n3, 1);
}

/* 4 triangles from the native copy (indices in range); 0..15 = the mask, -1 = take the helpers */
static const uint8_t k_rev4[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
static inline int cull_group_fast(const cull_verts* nv, const uint16_t* idx, __m128 flip, __m128 bfon)
{
    __m128 v[3][4];
    for (int k = 0; k < 3; k++) {
        __m128 a0 = _mm_load_ps((const float*)nv->v[idx[k]]), a1 = _mm_load_ps((const float*)nv->v[idx[3 + k]]);
        __m128 a2 = _mm_load_ps((const float*)nv->v[idx[6 + k]]), a3 = _mm_load_ps((const float*)nv->v[idx[9 + k]]);
        _MM_TRANSPOSE4_PS(a0, a1, a2, a3);
        v[k][0] = a0; v[k][1] = a1; v[k][2] = a2; v[k][3] = a3;
    }
    const __m128i o0 = _mm_castps_si128(v[0][2]), o1 = _mm_castps_si128(v[1][2]), o2 = _mm_castps_si128(v[2][2]);
    const __m128i oo = _mm_or_si128(_mm_or_si128(o0, o1), o2), aa = _mm_and_si128(_mm_and_si128(o0, o1), o2);
    if (!_mm_testz_si128(oo, _mm_set1_epi32(1))) return -1;
    const __m128 dx1 = FS(v[1][0], v[0][0]), dx2 = FS(v[2][0], v[0][0]), dy1 = FS(v[1][1], v[0][1]), dy2 = FS(v[2][1], v[0][1]);
    __m128 ar = _mm_xor_ps(FMS(dx1, dy2, FM(dy1, dx2)), flip);
    EG_OPQ(ar);
    const __m128i bf = _mm_castps_si128(_mm_and_ps(_mm_cmpgt_ps(ar, _mm_setzero_ps()), bfon));
    const __m128i x0 = _mm_castps_si128(v[0][3]), x1 = _mm_castps_si128(v[1][3]), x2 = _mm_castps_si128(v[2][3]);
    const __m128i e = _mm_and_si128(_mm_cmpeq_epi16(x0, x1), _mm_cmpeq_epi16(x1, x2));
    const __m128i deg = _mm_or_si128(_mm_slli_epi32(e, 16), e);            /* sign bit: all x or all y equal */
    const __m128i z = _mm_setzero_si128();
    const __m128i frust = _mm_xor_si128(_mm_cmpeq_epi32(_mm_and_si128(aa, _mm_set1_epi32(0xFC)), z), _mm_set1_epi32(-1));
    const __m128i front = _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(aa, _mm_set1_epi32(2)), z),
                                           _mm_cmpeq_epi32(_mm_and_si128(oo, _mm_set1_epi32(4)), z));
    const __m128i cull = _mm_or_si128(frust, _mm_and_si128(front, _mm_or_si128(bf, _mm_srai_epi32(deg, 31))));
    return k_rev4[_mm_movemask_ps(_mm_castsi128_ps(cull))];
}

/* the store emulation with byte shuffles (raw local-store byte order) */
static __m128i k_t1a[16], k_t1b[16], k_t2[16], k_sh0[16], k_ge[16], k_q1a[16], k_q1b[16], k_rot[16];
static volatile int k_tables_ready;
static void cull_tables(void)
{
    if (k_tables_ready) return;
    for (int m = 0; m < 16; m++) {
        int8_t s[32], a[16], b[16], c[16];
        int n = 0;
        memset(s, -1, sizeof s);
        for (int t = 0; t < 4; t++) if (!(m >> (3 - t) & 1)) { for (int k = 0; k < 6; k++) s[6 * n + k] = (int8_t)(6 * t + k); n++; }
        /* S = kept bytes of the 24-byte group G = lo (0..15) | hi (16..23) */
        for (int k = 0; k < 16; k++) {
            a[k] = s[k] >= 0 && s[k] < 16 ? s[k] : -1;          /* from lo */
            b[k] = s[k] >= 16 ? (int8_t)(s[k] - 16) : -1;       /* from hi */
            c[k] = s[16 + k] >= 16 ? (int8_t)(s[16 + k] - 16) : -1;   /* S[16..31] only from hi (24 bytes at most) */
        }
        memcpy(&k_t1a[m], a, 16); memcpy(&k_t1b[m], b, 16); memcpy(&k_t2[m], c, 16);
    }
    for (int al = 0; al < 16; al++) {
        int8_t sh0[16], ge[16], q1a[16], q1b[16], rot[16];
        for (int k = 0; k < 16; k++) {
            sh0[k] = k >= al ? (int8_t)(k - al) : -1;             /* q0: S[k - a] */
            ge[k] = k >= al ? -1 : 0;
            q1a[k] = k < al ? (int8_t)(16 - al + k) : -1;         /* q1: S[16 - a + k]: lo for k < a */
            q1b[k] = k >= al ? (int8_t)(k - al) : -1;             /*                    hi for k >= a */
            rot[k] = (int8_t)((k + 16 - al) & 15);                /* q2: k < a: S[32 - a + k] = hi[16 - a + k]; else hi[k - a] */
        }
        memcpy(&k_sh0[al], sh0, 16); memcpy(&k_ge[al], ge, 16); memcpy(&k_q1a[al], q1a, 16); memcpy(&k_q1b[al], q1b, 16);
        memcpy(&k_rot[al], rot, 16);
    }
    k_tables_ready = 1;
}

static uint32_t cull_store_fast(uint8_t* ls, uint32_t out, uint32_t ngroups, uint32_t ntris, const uint8_t* mask, const uint8_t* src)
{
    const int32_t end = (int32_t)(out + ((6u * ntris + 15u) & ~15u));
    uint32_t dst = out, a = 0, total = 0;
    for (int32_t g = -2; g < (int32_t)ngroups; g++) {
        __m128i lo = _mm_setzero_si128(), hi = _mm_setzero_si128();
        uint32_t n = 0;
        if (g >= 0) {
            const uint32_t m = mask[g];
            const __m128i G0 = _mm_loadu_si128((const __m128i*)(src + 24u * (uint32_t)g));
            const __m128i G1 = _mm_loadl_epi64((const __m128i*)(src + 24u * (uint32_t)g + 16));
            lo = _mm_or_si128(_mm_shuffle_epi8(G0, k_t1a[m]), _mm_shuffle_epi8(G1, k_t1b[m]));
            hi = _mm_shuffle_epi8(G1, k_t2[m]);
            n = 4u - (uint32_t)__builtin_popcount(m);
        }
        __m128i* q0 = (__m128i*)(ls + (dst & (LSM & ~15u)));
        _mm_storeu_si128(q0, _mm_blendv_epi8(_mm_loadu_si128(q0), _mm_shuffle_epi8(lo, k_sh0[a]), k_ge[a]));
        if (!((int32_t)dst > end - 17))
            _mm_storeu_si128((__m128i*)(ls + ((dst + 16u) & (LSM & ~15u))), _mm_or_si128(_mm_shuffle_epi8(lo, k_q1a[a]), _mm_shuffle_epi8(hi, k_q1b[a])));
        if (!((int32_t)dst > end - 33))
            _mm_storeu_si128((__m128i*)(ls + ((dst + 32u) & (LSM & ~15u))), _mm_shuffle_epi8(hi, k_rot[a]));
        dst += 6u * n; a = (a + 6u * n) & 15u; total += n;
    }
    return total;
}

int if2_eg_cull(uint8_t* ls, uint32_t ectx, uint32_t flavor, u128* r3)
{
    EG_T(const unsigned long long TS = __rdtsc();)
    cull_args ca;
    if (!cull_args_init(ls, ectx, flavor, &ca)) return 0;
    const uint32_t nvg = (ca.nv + 3u) >> 2;
    if (nvg > EG_MAXVG) return if2_eg_cull_ref(ls, ectx, flavor, r3);
    cull_tables();
    static __declspec(thread) cull_verts* nv;
    if (!nv) { nv = (cull_verts*)_aligned_malloc(sizeof *nv, 64); if (!nv) return 0; }
    cull_setup cs;
    cull_setup_init(ls, ectx, &cs);
    cs.tbl = ca.tbl; cs.pos = ca.pos;
    EG_T(const unsigned long long T0 = __rdtsc();)
    {
        static __declspec(thread) __m256* tc;
        static __declspec(thread) uint8_t tok[EG_MAXVG / 2];
        if (!tc) { tc = (__m256*)_aligned_malloc(sizeof(__m256) * 4 * (EG_MAXVG / 2), 64); if (!tc) return 0; }
        const uint32_t npair = cs.fastv ? nvg / 2 : 0;
        for (uint32_t q = 0; q < npair; q++) tok[q] = (uint8_t)cull_v2_transform(ls, &cs, 2u * q, tc + 4u * q);
        for (uint32_t q = 0; q < npair; q++)
            if (tok[q]) { cull_v2_project(ls, &cs, 2u * q, tc + 4u * q, nv); EG_T(g_eg_cull_vfast += 2;) }
        for (uint32_t g = 0; g < nvg; g++) {
            if (g / 2 < npair && tok[g / 2]) continue;
            if (cs.fastv && cull_vgroup_fast(ls, &cs, g, nv)) { EG_T(g_eg_cull_vfast++;) continue; }
            cull_vgroup_ref(ls, &cs, g, nv); EG_T(g_eg_cull_vexact++;)
        }
    }
    EG_T(const unsigned long long T1 = __rdtsc();)
    memcpy(t_src, ls + ca.out, 24u * ca.ngroups);
    /* the indices, host order; the slots past the last triangle read vertex idx[0] (culled whatever they read) */
    const __m128i bs16 = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
    for (uint32_t o = 0; o < 24u * ca.ngroups; o += 16)
        _mm_store_si128((__m128i*)((uint8_t*)t_idx + o), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(t_src + o)), bs16));
    if (ca.ngroups && ca.tail)
        for (uint32_t j = 3u * ca.tail; j < 12; j++) t_idx[12u * (ca.ngroups - 1) + j] = t_idx[12u * (ca.ngroups - 1)];
    const __m128 flip = _mm_castsi128_ps(_mm_set1_epi32((int)ca.flip));
    const __m128 bfon = _mm_castsi128_ps(_mm_set1_epi32(ca.backface ? -1 : 0));
    const __m128i lim = _mm_set1_epi16((short)(4u * nvg - 1u));
    for (uint32_t g = 0; g < ca.ngroups; g++) {
        const uint16_t* ix = t_idx + 12u * g;
        const __m128i i0 = _mm_loadu_si128((const __m128i*)ix), i1 = _mm_loadl_epi64((const __m128i*)(ix + 8));
        const __m128i mx = _mm_max_epu16(i0, i1);   /* i1's upper half is 0 */
        int m = -1;
        if (_mm_movemask_epi8(_mm_cmpeq_epi16(_mm_max_epu16(mx, lim), lim)) == 0xFFFF) m = cull_group_fast(nv, ix, flip, bfon);
        if (m < 0) {
            uint32_t idx[12];
            for (int j = 0; j < 12; j++) idx[j] = ix[j];
            m = (int)cull_group_ref(ls, ca.tbl, idx, ca.flip, ca.backface, cs.snap); EG_T(g_eg_cull_texact++;)
        } else { EG_T(g_eg_cull_tfast++;) }
        t_mask[g] = (uint8_t)m;
    }
    if (ca.ngroups && ca.tail) t_mask[ca.ngroups - 1] |= (uint8_t)(15u >> ca.tail);
    EG_T(const unsigned long long T2 = __rdtsc(); g_eg_cull_t[0] += T1 - T0; g_eg_cull_t[1] += T2 - T1;)
    cull_finish(ls, &ca, cull_store_fast(ls, ca.out, ca.ngroups, ca.ntris, t_mask, t_src), r3);
    EG_T(g_eg_cull_t[2] += __rdtsc() - T2; g_eg_cull_t[3] += T0 - TS;)
    return 1;
}

/* ---- vertex decompression (LS 0x4448: r3 Edge context, r4 the segment config, r5 the segment's pointer table) ----
 * The job's specialized path for the one stream layout the game uses (anything else runs the SPU code): the
 * primary stream, 9 bytes a vertex, holds the position as 3 big-endian 16-bit fixed-point values (10 integer
 * bits, config byte 0x1B) and the normal as a 24-bit unit vector; an optional secondary stream, 3 bytes a
 * vertex, holds the tangent as a unit vector. Positions: (raw + offset[k]) / 2^6 (the per-mesh offsets at the
 * input block), w = 1. Unit vector raw24: a = bits 23..14, b = bits 13..4, selector = bits 3..0; A = fma(1+a*2^-23,
 * K1, K0) (the SPU's 1.0|a mantissa trick), same B, C = |1 - (A^2 + B^2)| ^ 0.5 by frsqest/fi/fm, and the selector's
 * 16-byte entry of the table at LS 0xF380 is a shuffle pattern (low 31 bits) and sign bits (bit 31) that place C
 * (with its sign), A and B; its last lane is w (the tangent's handedness). The SPU code works on vertex pairs,
 * so ceil-to-even vertices are written, and the normal and tangent loops' pipelines each write two zero
 * quadwords past their table first. Tables: ctx+0x10/0x14/0x18 (the first free slots of ctx+0x170, which gets
 * the attribute ids 1 2 3), recorded at ctx+0x5C/0x60/0x64; ctx+0x54 and ctx+0x58 get the input address. */
static const uint8_t k_dec_desc1[32] = {
    0x02, 0x09, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x03, 0x01, 0x06, 0x00, 0x10, 0x00,
    0x06, 0x09, 0x03, 0x02, 0x03, 0x00, 0x10, 0x00, 0x0A, 0x06, 0x0A, 0x06, 0x0A, 0x06, 0x00, 0x00 };
static const uint8_t k_dec_desc2[16] = {
    0x01, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x03, 0x03, 0x03, 0x00, 0x08, 0x00 };

static int ls_eq(const uint8_t* ls, uint32_t a, const uint8_t* k, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (ls[(a + i) & LSM] != k[i]) return 0;
    return 1;
}
static inline uint32_t be24(const uint8_t* ls, uint32_t a)
{
    return (uint32_t)ls[a & LSM] << 16 | (uint32_t)ls[(a + 1) & LSM] << 8 | ls[(a + 2) & LSM];
}

/* two unit vectors (raw24 each) -> two quadwords, the SPU's operations lane for lane */
static void uv_pair_ref(const uint8_t* ls, uint32_t r0, uint32_t r1, u128* n0, u128* n1)
{
    const u128 F = k4(0x3F800000u | (r0 >> 14 & 0x3FFu), 0x3F800000u | (r0 >> 4 & 0x3FFu),
                      0x3F800000u | (r1 >> 14 & 0x3FFu), 0x3F800000u | (r1 >> 4 & 0x3FFu));
    const u128 AB = spu_fma(k1(0x46352E7Cu), F, k1(0xC6353150u));
    const u128 sq = spu_fm(AB, AB), sh = k4(sq._u32[1], sq._u32[2], sq._u32[3], 0);
    const u128 T = spu_fs(k1(0x3F800000u), spu_fa(sq, sh));
    const u128 C = spu_fm(T, spu_fi(T, spu_frsqest(T)));
    const u128 L0 = ldq(ls, 0xF380u + 16u * (r0 & 15u)), L1 = ldq(ls, 0xF380u + 16u * (r1 & 15u));
    const u128 SGN = k4(0x80000000u, 0x80000000u, 0x80000000u, 0xFFFFFFFFu), M31 = k4(0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, 0);
    const u128 Z3 = k4(0, 0, 0, 0x0C0D0E0Fu);
    *n0 = spu_shufb(spu_selb(C, L0, SGN), AB, spu_selb(Z3, L0, M31));
    u128 P1 = spu_selb(Z3, L1, M31);
    for (int k = 0; k < 3; k++) P1._u32[k] += 0x08080808u;
    *n1 = spu_shufb(spu_selb(C, L1, SGN), AB, P1);
}

typedef struct { uint32_t ectx, ptab, nv, n2, in1, in2, ib, tP, tN, tT, w14; int tan; } dec_args;

static int dec_args_init(const uint8_t* ls, uint32_t ectx, uint32_t cfg, uint32_t ptab, dec_args* da)
{
    if (ectx & 15u) return 0;
    const uint32_t d1 = be32(ls, ptab + 0x2C), d2 = be32(ls, ptab + 0x30);
    if (!ls_eq(ls, d1, k_dec_desc1, 32)) return 0;
    if (d2 && !ls_eq(ls, d2, k_dec_desc2, 16)) return 0;
    if (ls[(cfg + 0x1B) & LSM] != 10) return 0;                       /* 10 integer bits: / 2^6 */
    for (int i = 0; i < 16; i++) if (ls[(ectx + 0x170 + (uint32_t)i) & LSM]) return 0;   /* table slots 0 1 2 */
    da->ectx = ectx; da->ptab = ptab; da->tan = d2 != 0;
    da->nv = be16(ls, ectx + 8);
    if (!da->nv) return 0;
    da->n2 = (da->nv + 1u) & ~1u;
    da->w14 = be32(ls, ptab + 0x14); da->in1 = da->w14; da->in2 = be32(ls, ptab + 0x18); da->ib = be32(ls, ptab + 0x28);
    da->tP = be32(ls, ectx + 0x10); da->tN = be32(ls, ectx + 0x14); da->tT = be32(ls, ectx + 0x18);
    return 1;
}

static void dec_ctx(uint8_t* ls, const dec_args* da)
{
    const uint32_t c = da->ectx;
    st32(ls, c + 0x54, da->w14);
    ls[(c + 0x170) & LSM] = 1; ls[(c + 0x171) & LSM] = 2;
    st32(ls, c + 0x5C, da->tP); st32(ls, c + 0x60, da->tN);
    if (da->tan) { ls[(c + 0x172) & LSM] = 3; st32(ls, c + 0x64, da->tT); }
    st32(ls, c + 0x58, da->w14);
}

int if2_eg_decomp_ref(uint8_t* ls, uint32_t ectx, uint32_t cfg, uint32_t ptab)
{
    dec_args da;
    if (!dec_args_init(ls, ectx, cfg, ptab, &da)) return 0;
    const u128 off = k4(be32(ls, da.ib), be32(ls, da.ib + 4), be32(ls, da.ib + 8), 0);
    stq(ls, da.tN + 16u * da.n2, k1(0)); stq(ls, da.tN + 16u * da.n2 + 16u, k1(0));
    for (uint32_t v = 0; v < da.n2; v++) {
        const uint32_t p = da.in1 + 9u * v;
        u128 val = k4(be16(ls, p) + off._u32[0], be16(ls, p + 2) + off._u32[1], be16(ls, p + 4) + off._u32[2], 0);
        const u128 ip = spu_csflt(k4((uint32_t)((int32_t)val._u32[0] >> 6), (uint32_t)((int32_t)val._u32[1] >> 6),
                                     (uint32_t)((int32_t)val._u32[2] >> 6), 0), 155);
        const u128 fp = spu_cuflt(k4(val._u32[0] << 26, val._u32[1] << 26, val._u32[2] << 26, 0), 123);
        u128 pos = spu_fa(ip, fp);
        pos._u32[3] = 0x3F800000u;
        stq(ls, da.tP + 16u * v, pos);
    }
    for (uint32_t v = 0; v < da.n2; v += 2) {
        u128 n0, n1;
        uv_pair_ref(ls, be24(ls, da.in1 + 9u * v + 6u), be24(ls, da.in1 + 9u * v + 15u), &n0, &n1);
        stq(ls, da.tN + 16u * v, n0); stq(ls, da.tN + 16u * v + 16u, n1);
    }
    if (da.tan) {
        stq(ls, da.tT + 16u * da.n2, k1(0)); stq(ls, da.tT + 16u * da.n2 + 16u, k1(0));
        for (uint32_t v = 0; v < da.n2; v += 2) {
            u128 t0, t1;
            uv_pair_ref(ls, be24(ls, da.in2 + 3u * v), be24(ls, da.in2 + 3u * v + 3u), &t0, &t1);
            stq(ls, da.tT + 16u * v, t0); stq(ls, da.tT + 16u * v + 16u, t1);
        }
    }
    dec_ctx(ls, &da);
    return 1;
}


/* the fast path: positions as (float)(raw + offset) * 2^-6 (exact: |offset| < 2^23 keeps the sum below 2^24);
 * unit vectors 4 at a time with host ops (A, B, A^2 + B^2 and 1 - that stay in [-2, 2]: no overflow, and the
 * RZ/DAZ/FTZ mode plus +0 gives the helpers' results) and the toolkit's frsqest/fi/fm helpers for C; the table
 * entries 0..11 (checked against the job's table) become byte shuffles. A selector 12..15 (the table's tail is
 * text) takes the helpers for its pair. */
static const uint8_t k_dec_lut[12][16] = {
    { 0x80, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13, 0x94, 0x15, 0x16, 0x17, 0xBF, 0x80, 0x00, 0x00 },
    { 0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0xBF, 0x80, 0x00, 0x00 },
    { 0x80, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13, 0x94, 0x15, 0x16, 0x17, 0x3F, 0x80, 0x00, 0x00 },
    { 0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x3F, 0x80, 0x00, 0x00 },
    { 0x90, 0x11, 0x12, 0x13, 0x00, 0x01, 0x02, 0x03, 0x94, 0x15, 0x16, 0x17, 0xBF, 0x80, 0x00, 0x00 },
    { 0x10, 0x11, 0x12, 0x13, 0x00, 0x01, 0x02, 0x03, 0x14, 0x15, 0x16, 0x17, 0xBF, 0x80, 0x00, 0x00 },
    { 0x90, 0x11, 0x12, 0x13, 0x00, 0x01, 0x02, 0x03, 0x94, 0x15, 0x16, 0x17, 0x3F, 0x80, 0x00, 0x00 },
    { 0x10, 0x11, 0x12, 0x13, 0x00, 0x01, 0x02, 0x03, 0x14, 0x15, 0x16, 0x17, 0x3F, 0x80, 0x00, 0x00 },
    { 0x90, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x80, 0x01, 0x02, 0x03, 0xBF, 0x80, 0x00, 0x00 },
    { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x00, 0x01, 0x02, 0x03, 0xBF, 0x80, 0x00, 0x00 },
    { 0x90, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x80, 0x01, 0x02, 0x03, 0x3F, 0x80, 0x00, 0x00 },
    { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x00, 0x01, 0x02, 0x03, 0x3F, 0x80, 0x00, 0x00 } };
/* per entry: pshufb of the host vector [|C|, A, B, 0] into the big-endian output, then OR the sign of C and w;
 * k_uvo[parity]: vertex 0 of a pair takes C's sign from the entry's lane 0, vertex 1 from lane 2 */
static __m128i k_uvp[12], k_uvo[2][12];
static volatile int k_uv_ready;
static void dec_tables(void)
{
    if (k_uv_ready) return;
    for (int e = 0; e < 12; e++) {
        const uint8_t* L = k_dec_lut[e];
        uint8_t p[16], o0[16], o1[16];
        memset(o0, 0, 16); memset(o1, 0, 16);
        for (int j = 0; j < 3; j++) {
            const uint32_t w = ((uint32_t)L[4 * j] << 24 | (uint32_t)L[4 * j + 1] << 16 | (uint32_t)L[4 * j + 2] << 8 | L[4 * j + 3]) & 0x7FFFFFFFu;
            const int s = w == 0x00010203u ? 0 : w == 0x10111213u ? 1 : 2;   /* the table above has only these three */
            for (int b = 0; b < 4; b++) p[4 * j + b] = (uint8_t)(4 * s + 3 - b);
            if (s == 0) { o0[4 * j] = L[0] & 0x80; o1[4 * j] = L[8] & 0x80; }
        }
        for (int b = 0; b < 4; b++) { p[12 + b] = 0x80; o0[12 + b] = o1[12 + b] = L[12 + b]; }
        memcpy(&k_uvp[e], p, 16); memcpy(&k_uvo[0][e], o0, 16); memcpy(&k_uvo[1][e], o1, 16);
    }
    k_uv_ready = 1;
}

/* 4 unit vectors (raw24, 2 pairs) -> 4 big-endian quadwords at out[0..3] (raw byte pointers) */
static inline void uv4_fast(const uint32_t* raw, uint8_t* const* out)
{
    const __m128i r = _mm_loadu_si128((const __m128i*)raw);
    const __m128i one = _mm_set1_epi32(0x3F800000), m10 = _mm_set1_epi32(0x3FF);
    const __m128 FA = _mm_castsi128_ps(_mm_or_si128(one, _mm_and_si128(_mm_srli_epi32(r, 14), m10)));
    const __m128 FB = _mm_castsi128_ps(_mm_or_si128(one, _mm_and_si128(_mm_srli_epi32(r, 4), m10)));
    const __m128 K1 = _mm_castsi128_ps(_mm_set1_epi32(0x46352E7C)), K0 = _mm_castsi128_ps(_mm_set1_epi32((int)0xC6353150u));
    const __m128 A = FMA(K1, FA, K0), B = FMA(K1, FB, K0);
    __m128 s = _mm_add_ps(FM(A, A), FM(B, B)); s = nz(s);
    const __m128 T = FS(_mm_set1_ps(1.0f), s);
    const u128 Tu = spu__u(_mm_castps_si128(T));
    const __m128 C = _mm_castsi128_ps(spu__v(spu_fm(Tu, spu_fi(Tu, spu_frsqest(Tu)))));
    __m128 v0 = _mm_and_ps(C, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))), v1 = A, v2 = B, v3 = _mm_setzero_ps();
    _MM_TRANSPOSE4_PS(v0, v1, v2, v3);
    const __m128 vv[4] = { v0, v1, v2, v3 };
    for (int k = 0; k < 4; k++) {
        const uint32_t e = raw[k] & 15u;
        _mm_storeu_si128((__m128i*)out[k], _mm_or_si128(_mm_shuffle_epi8(_mm_castps_si128(vv[k]), k_uvp[e]), k_uvo[k & 1][e]));
    }
}

/* a stream of unit vectors: n2 (even) vertices, stride bytes apart, to table t; 0 when a selector is 12..15 or
 * the ranges leave the local store (the caller then takes the helpers for the whole stream) */
static int uv_stream_fast(uint8_t* ls, uint32_t in, uint32_t stride, uint32_t n2, uint32_t t)
{
    uint32_t v = 0;
    for (; v + 4 <= n2; v += 4) {
        uint32_t raw[4]; uint8_t* out[4];
        for (int k = 0; k < 4; k++) {
            const uint8_t* p = ls + in + stride * (v + (uint32_t)k);
            raw[k] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            out[k] = ls + t + 16u * (v + (uint32_t)k);
        }
        if ((raw[0] & 12u) == 12u || (raw[1] & 12u) == 12u || (raw[2] & 12u) == 12u || (raw[3] & 12u) == 12u) {
            u128 a, b;
            uv_pair_ref(ls, raw[0], raw[1], &a, &b); stq(ls, t + 16u * v, a); stq(ls, t + 16u * v + 16u, b);
            uv_pair_ref(ls, raw[2], raw[3], &a, &b); stq(ls, t + 16u * v + 32u, a); stq(ls, t + 16u * v + 48u, b);
            continue;
        }
        uv4_fast(raw, out);
    }
    if (v < n2) {   /* the last pair */
        u128 a, b;
        uv_pair_ref(ls, be24(ls, in + stride * v), be24(ls, in + stride * (v + 1)), &a, &b);
        stq(ls, t + 16u * v, a); stq(ls, t + 16u * v + 16u, b);
    }
    return 1;
}

int if2_eg_decomp(uint8_t* ls, uint32_t ectx, uint32_t cfg, uint32_t ptab)
{
    dec_args da;
    if (!dec_args_init(ls, ectx, cfg, ptab, &da)) return 0;
    const int32_t o0 = (int32_t)be32(ls, da.ib), o1 = (int32_t)be32(ls, da.ib + 4), o2 = (int32_t)be32(ls, da.ib + 8);
    const uint32_t L = SPU_LS_SIZE;
    const int inrange = da.in1 + 9u * da.n2 + 16u <= L && da.tP + 16u * da.n2 <= L && da.tN + 16u * da.n2 + 32u <= L &&
                        (!da.tan || (da.in2 + 3u * da.n2 + 16u <= L && da.tT + 16u * da.n2 + 32u <= L)) &&
                        (da.tP | da.tN | da.tT) % 16u == 0 &&
                        o0 > -(1 << 23) && o0 < (1 << 23) && o1 > -(1 << 23) && o1 < (1 << 23) && o2 > -(1 << 23) && o2 < (1 << 23) &&
                        ls_eq(ls, 0xF380u, &k_dec_lut[0][0], sizeof k_dec_lut);
    if (!inrange) return if2_eg_decomp_ref(ls, ectx, cfg, ptab);
    dec_tables();
    stq(ls, da.tN + 16u * da.n2, k1(0)); stq(ls, da.tN + 16u * da.n2 + 16u, k1(0));
    {   /* positions */
        const __m128i sel = _mm_setr_epi8(1, 0, -1, -1, 3, 2, -1, -1, 5, 4, -1, -1, -1, -1, -1, -1);
        const __m128i off = _mm_setr_epi32(o0, o1, o2, 0);
        const __m128 sc = _mm_setr_ps(1.0f / 64.0f, 1.0f / 64.0f, 1.0f / 64.0f, 0.0f), w1 = _mm_setr_ps(0, 0, 0, 1.0f);
        const __m128i bs = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        const uint8_t* p = ls + da.in1;
        uint8_t* o = ls + da.tP;
        for (uint32_t v = 0; v < da.n2; v++, p += 9, o += 16) {
            const __m128i iv = _mm_add_epi32(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)p), sel), off);
            const __m128 f = _mm_add_ps(_mm_mul_ps(_mm_cvtepi32_ps(iv), sc), w1);
            _mm_storeu_si128((__m128i*)o, _mm_shuffle_epi8(_mm_castps_si128(f), bs));
        }
    }
    uv_stream_fast(ls, da.in1 + 6u, 9u, da.n2, da.tN);
    if (da.tan) {
        stq(ls, da.tT + 16u * da.n2, k1(0)); stq(ls, da.tT + 16u * da.n2 + 16u, k1(0));
        uv_stream_fast(ls, da.in2, 3u, da.n2, da.tT);
    }
    dec_ctx(ls, &da);
    return 1;
}

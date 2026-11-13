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
            if (fast) g_eg_skin_fast_v++; else g_eg_skin_exact_v++;
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
            if (fast) g_eg_norm_fast++; else g_eg_norm_exact++;
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

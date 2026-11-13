/* if2_edge.cpp -- EG: inFamous 2's Edge Geometry job (job module d0d15ec83992,
 * image 222) off the SPUs. Plan: analysis/edgegeom/EG_PLAN.md (private).
 *
 * The job's stage calls are lifted as calls through the dispatcher
 * (tools/lift_jobs.sh: SPU_JOBMOD_INDIRECT_CALLS), so this module can wrap or
 * replace them with spu_registry_replace. Nothing is replaced unless an EG_*
 * setting asks for it.
 *
 * EG0 diagnostics:
 *   EG_DUMP=<n>    the first n segments' arguments and the local-store bytes
 *                  they point at (stderr, [EG-DUMP]).
 *   EG_CENSUS=1    per segment config: calls, vertices, indexes, SPU time in
 *                  the segment and in each stage ([EG-CENSUS] every 5 s).
 *   EG_SPIN=<pct>  after each segment, spin for pct% of its own time (the
 *                  sensitivity test: 100 = the job costs twice as much). */
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_context.h"
#include "ps3emu/options.h"

extern "C" {
typedef void (*spu_fn_t)(spu_context*);
spu_fn_t spu_registry_replace(int image_id, uint32_t addr, spu_fn_t fn);
}

namespace {

const int kImage = 222;                 /* jobmod_d0d15ec83992 (generated/spu_jobs/jobmods_register.c) */
const uint32_t kSegment = 0x5740;       /* per segment: (work, config, ..., pointer table, ...) */
struct Stage { uint32_t addr; const char* name; };
const Stage kStages[] = {
    { 0x4448, "decomp-wrap" }, { 0xAD48, "decomp" }, { 0x5490, "matprep" }, { 0xD0A8, "skin" },
    { 0xD030, "idx-wrap" }, { 0xC7B8, "idx" }, { 0xA798, "cull" }, { 0xA3C8, "cull-inner" },
};
const int kNStages = (int)(sizeof kStages / sizeof kStages[0]);

spu_fn_t s_seg_orig;
spu_fn_t s_stage_orig[kNStages];
int s_dump_left, s_census, s_spin;

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline const uint8_t* lsp(spu_context* ctx, uint32_t a) { return ctx->ls + (a & (SPU_LS_SIZE - 1)); }
inline uint32_t r32(spu_context* ctx, int r) { return ctx->gpr[r]._u32[0]; }

/* Run the original to its return (the lifter's call bracket, so a tail
 * chain inside it finishes here, not in the caller's drain). */
inline void run_orig(spu_context* ctx, spu_fn_t f)
{
    const uint32_t ret = r32(ctx, 0);
    ctx->host_depth++;
    f(ctx);
    spu_drain_call(ctx, ret);
    ctx->host_depth--;
}

void hexline(char* o, size_t n, spu_context* ctx, uint32_t a, uint32_t len)
{
    size_t p = 0;
    for (uint32_t i = 0; i < len && p + 4 < n; i++)
        p += (size_t)snprintf(o + p, n - p, "%02X%s", lsp(ctx, a)[i], (i & 3) == 3 ? " " : "");
}

/* ---- census ---------------------------------------------------------------- */
struct Cfg { uint8_t key[16]; uint64_t calls, cyc, verts, idx, stage_cyc[kNStages]; };
const int kCfgMax = 256;
Cfg s_cfg[kCfgMax];
int s_ncfg;
SRWLOCK s_lock = SRWLOCK_INIT;
uint64_t s_tot_cyc, s_t0;
__declspec(thread) int t_cfg = -1;      /* the config of the segment this SPU thread runs */
__declspec(thread) uint64_t t_stage_cyc[kNStages];
__declspec(thread) uint32_t t_verts, t_idx;

void census_print(void)
{
    AcquireSRWLockExclusive(&s_lock);
    fprintf(stderr, "[EG-CENSUS] %d configs, %.1f Mcyc total%c", s_ncfg, s_tot_cyc / 1e6, 10);
    for (int i = 0; i < s_ncfg; i++) {
        const Cfg* c = &s_cfg[i];
        char k[64]; size_t p = 0;
        for (int j = 0; j < 16; j++) p += (size_t)snprintf(k + p, sizeof k - p, "%02X%s", c->key[j], (j & 3) == 3 ? " " : "");
        fprintf(stderr, "[EG-CENSUS] cfg %2d %s calls %llu Mcyc %.1f verts %llu idx %llu |", i, k,
                (unsigned long long)c->calls, c->cyc / 1e6, (unsigned long long)c->verts, (unsigned long long)c->idx);
        for (int s = 0; s < kNStages; s++)
            if (c->stage_cyc[s]) fprintf(stderr, " %s %.1f", kStages[s].name, c->stage_cyc[s] / 1e6);
        fprintf(stderr, "%c", 10);
    }
    ReleaseSRWLockExclusive(&s_lock);
}

int cfg_slot(const uint8_t* key)
{
    for (int i = 0; i < s_ncfg; i++) if (!memcmp(s_cfg[i].key, key, 16)) return i;
    if (s_ncfg >= kCfgMax) return -1;
    memcpy(s_cfg[s_ncfg].key, key, 16);
    return s_ncfg++;
}

/* ---- hooks ------------------------------------------------------------------- */
void segment_hook(spu_context* ctx)
{
    const uint32_t r3 = r32(ctx, 3), r4 = r32(ctx, 4), r5 = r32(ctx, 5), r6 = r32(ctx, 6), r7 = r32(ctx, 7),
                   r8 = r32(ctx, 8), r9 = r32(ctx, 9);
    if (s_dump_left > 0 && InterlockedDecrement((volatile LONG*)&s_dump_left) >= 0) {
        char a[200], b[200], c[200], d[200];
        fprintf(stderr, "[EG-DUMP] spu %u r3 %05X r4 %05X r5 %05X r6 %X r7 %05X r8 %05X r9 %05X%c",
                ctx->spu_id, r3, r4, r5, r6, r7, r8, r9, 10);
        for (uint32_t o = 0; o < 0x80; o += 0x20) {
            hexline(a, sizeof a, ctx, r4 + o, 32); hexline(b, sizeof b, ctx, r7 + o, 32);
            hexline(c, sizeof c, ctx, r3 + o, 32); hexline(d, sizeof d, ctx, r5 + o, 32);
            fprintf(stderr, "[EG-DUMP]  +%02X cfg %s| tbl %s| work %s| r5 %s%c", o, a, b, c, d, 10);
        }
        /* the pointer table's words, followed one level */
        for (uint32_t o = 0; o < 0x40; o += 4) {
            const uint32_t p = be32(lsp(ctx, r7 + o));
            if (p && p < SPU_LS_SIZE) { hexline(a, sizeof a, ctx, p, 32); fprintf(stderr, "[EG-DUMP]  tbl[%02X] -> %05X: %s%c", o, p, a, 10); }
        }
    }
    uint64_t t0 = 0;
    if (s_census || s_spin) {
        if (s_census) {
            /* key: the segment's Edge config header (r5: flags + uniform tables, hole size, the four
             * vertex format ids, index + skinning flavors, matrix format), then a hash of the custom
             * stream descriptions the pointer table names (r7 +0x00 output, +0x28 input, +0x30
             * secondary input; 32 bytes each). */
            uint8_t key[16];
            memset(key, 0, sizeof key);
            memcpy(key, lsp(ctx, r5), 8);
            key[1] = 0;   /* the hole size varies per mesh; not a format */
            uint32_t h = 2166136261u;
            /* stream descriptions: 8-byte header (attribute count, stride, ...) + 8 bytes per attribute */
            static const uint32_t kDesc[3] = { 0x00, 0x2C, 0x30 };
            for (int d = 0; d < 3; d++) {
                const uint32_t p = be32(lsp(ctx, r7 + kDesc[d]));
                const uint32_t n = p ? 8u + 8u * (lsp(ctx, p)[0] & 15u) : 0u;
                for (uint32_t i = 0; i < n; i++) h = (h ^ lsp(ctx, p)[i]) * 16777619u;
                h = (h ^ (n | (uint32_t)d << 8)) * 16777619u;
            }
            memcpy(key + 8, &h, 4);
            t_verts = (uint32_t)lsp(ctx, r5)[8] << 8 | lsp(ctx, r5)[9];
            t_idx = (uint32_t)lsp(ctx, r5)[10] << 8 | lsp(ctx, r5)[11];
            AcquireSRWLockExclusive(&s_lock);
            const int before = s_ncfg;
            t_cfg = cfg_slot(key);
            if (s_ncfg > before) {   /* a new config: its stream descriptions, once */
                fprintf(stderr, "[EG-FMT] cfg %d header %02X%02X%02X%02X %02X%02X%02X%02X%c", t_cfg,
                        key[0], lsp(ctx, r5)[1], key[2], key[3], key[4], key[5], key[6], key[7], 10);
                static const char* const kName[3] = { "out", "in", "in2" };
                for (int d = 0; d < 3; d++) {
                    const uint32_t p = be32(lsp(ctx, r7 + kDesc[d]));
                    if (!p) continue;
                    char a[400];
                    hexline(a, sizeof a, ctx, p, 8u + 8u * (lsp(ctx, p)[0] & 15u));
                    fprintf(stderr, "[EG-FMT]   %-3s %s%c", kName[d], a, 10);
                }
            }
            ReleaseSRWLockExclusive(&s_lock);
            memset(t_stage_cyc, 0, sizeof t_stage_cyc);
        }
        t0 = __rdtsc();
    }
    run_orig(ctx, s_seg_orig);
    if (s_census || s_spin) {
        const uint64_t dt = __rdtsc() - t0;
        if (s_census && t_cfg >= 0) {
            AcquireSRWLockExclusive(&s_lock);
            Cfg* c = &s_cfg[t_cfg];
            c->calls++; c->cyc += dt; s_tot_cyc += dt; c->verts += t_verts; c->idx += t_idx;
            for (int s = 0; s < kNStages; s++) c->stage_cyc[s] += t_stage_cyc[s];
            ReleaseSRWLockExclusive(&s_lock);
            const uint64_t now = GetTickCount64();
            if (!s_t0) s_t0 = now;
            if (now - s_t0 >= 5000 && InterlockedCompareExchange64((volatile LONG64*)&s_t0, (LONG64)now, (LONG64)s_t0) != (LONG64)now)
                census_print();
        }
        if (s_spin) {
            const uint64_t until = __rdtsc() + dt * (uint64_t)s_spin / 100u;
            while (__rdtsc() < until) YieldProcessor();
        }
    }
    t_cfg = -1;
}

/* ---- corpus (EG_CORPUS=<dir>,<n>[,<every>]) -----------------------------------
 * For each hooked stage, every <every>-th call (default 97) up to n records:
 * <dir>/<stage addr>_<k>.eg = header (magic "EGC1", stage addr, k), the 128
 * registers (2 KB) and the 256 KB local store before the call, then the 128
 * registers and the local store after it. Private development data. */
char s_corpus_dir[MAX_PATH];
int s_corpus_n, s_corpus_every = 97;
volatile LONG s_corpus_calls[kNStages], s_corpus_done[kNStages];

void corpus_write(int stage, const spu_context* before_ctx, const uint8_t* ls_before, spu_context* ctx, int k)
{
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s\\%04X_%03d.eg", s_corpus_dir, kStages[stage].addr, k);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const uint32_t hdr[4] = { 0x31434745u, kStages[stage].addr, (uint32_t)k, SPU_LS_SIZE };
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(before_ctx->gpr, sizeof ctx->gpr, 1, f);
    fwrite(ls_before, SPU_LS_SIZE, 1, f);
    fwrite(ctx->gpr, sizeof ctx->gpr, 1, f);
    fwrite(ctx->ls, SPU_LS_SIZE, 1, f);
    fclose(f);
}

/* ---- EG1 native stages (if2_edge_native.c) ------------------------------------------
 * EG_NATIVE=1: the skinning and triangle-culling stages run natively (each falls back to the SPU code per
 * call when its input is outside what the native code implements). EG_VERIFY=<n>: every n-th call of a
 * stage, the native code runs on a copy of the local store, the SPU code on the real one, and the two are
 * compared byte for byte outside the stack frame ([EG-VERIFY]). */
extern "C" int if2_eg_skin(uint8_t* ls, uint32_t ectx, uint32_t mats, uint32_t nmat, uint32_t blend);
extern "C" int if2_eg_cull(uint8_t* ls, uint32_t ectx, uint32_t flavor, u128* r3);
int s_native, s_verify_every;
volatile LONG s_v_calls, s_v_n, s_v_bad, s_v_unsup, s_nat_calls, s_nat_fallback;
volatile LONG s_vc_calls, s_natc_calls, s_natc_fallback;
spu_fn_t s_skin_orig, s_cull_orig;

void verify_compare(const char* stage, const uint8_t* a, const uint8_t* b, uint32_t sp)
{
    uint32_t nd = 0, first = 0xFFFFFFFFu, last = 0;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i += 16) {
        if (i + 16 > sp - 0x800u && i < sp + 0x40u) continue;   /* the SPU code's stack frames */
        if (memcmp(a + i, b + i, 16)) { nd++; if (first == 0xFFFFFFFFu) first = i; last = i; }
    }
    const LONG n = InterlockedIncrement(&s_v_n);
    if (nd) {
        const LONG bad = InterlockedIncrement(&s_v_bad);
        if (bad <= 12) {
            fprintf(stderr, "[EG-VERIFY] %s MISMATCH: %u quadwords differ, %05X..%05X; first: native", stage, nd, first, last);
            for (int k = 0; k < 16; k += 4)
                fprintf(stderr, " %02X%02X%02X%02X", a[first + k], a[first + k + 1], a[first + k + 2], a[first + k + 3]);
            fprintf(stderr, " spu");
            for (int k = 0; k < 16; k += 4)
                fprintf(stderr, " %02X%02X%02X%02X", b[first + k], b[first + k + 1], b[first + k + 2], b[first + k + 3]);
            fprintf(stderr, "%c", 10);
        }
    }
    if ((n & 255) == 0) fprintf(stderr, "[EG-VERIFY] %s: %ld compared, %ld mismatched, %ld unsupported%c", stage, n,
                                (long)s_v_bad, (long)s_v_unsup, 10);
}

void skin_hook(spu_context* ctx)
{
    const uint32_t ectx = r32(ctx, 3), mats = r32(ctx, 4), nmat = r32(ctx, 5), blend = r32(ctx, 6);
    if (s_verify_every && InterlockedIncrement(&s_v_calls) % s_verify_every == 0) {
        static __declspec(thread) uint8_t* copy;
        if (!copy) copy = (uint8_t*)malloc(SPU_LS_SIZE);
        memcpy(copy, ctx->ls, SPU_LS_SIZE);
        const int ok = if2_eg_skin(copy, ectx, mats, nmat, blend);
        const uint32_t sp = r32(ctx, 1);
        run_orig(ctx, s_skin_orig);
        if (ok) verify_compare("skin", copy, ctx->ls, sp);
        else InterlockedIncrement(&s_v_unsup);
        return;
    }
    if (s_native) {
        InterlockedIncrement(&s_nat_calls);
        if (if2_eg_skin(ctx->ls, ectx, mats, nmat, blend)) { SPU_RET(ctx); return; }
        InterlockedIncrement(&s_nat_fallback);
    }
    run_orig(ctx, s_skin_orig);
}

void cull_hook(spu_context* ctx)
{
    const uint32_t ectx = r32(ctx, 3), flavor = r32(ctx, 4);
    if (s_verify_every && InterlockedIncrement(&s_vc_calls) % s_verify_every == 0) {
        static __declspec(thread) uint8_t* copy;
        if (!copy) copy = (uint8_t*)malloc(SPU_LS_SIZE);
        memcpy(copy, ctx->ls, SPU_LS_SIZE);
        u128 r3;
        const int ok = if2_eg_cull(copy, ectx, flavor, &r3);
        const uint32_t sp = r32(ctx, 1);
        run_orig(ctx, s_cull_orig);
        if (ok) {
            if (memcmp(&r3, &ctx->gpr[3], sizeof r3)) {     /* the returned count: a mismatch in the LS compare too */
                const LONG bad = InterlockedIncrement(&s_v_bad);
                if (bad <= 12) fprintf(stderr, "[EG-VERIFY] cull r3 MISMATCH: native %08X spu %08X%c", r3._u32[0], ctx->gpr[3]._u32[0], 10);
            }
            verify_compare("cull", copy, ctx->ls, sp);
        } else InterlockedIncrement(&s_v_unsup);
        return;
    }
    if (s_native) {
        InterlockedIncrement(&s_natc_calls);
        u128 r3;
        if (if2_eg_cull(ctx->ls, ectx, flavor, &r3)) { ctx->gpr[3] = r3; SPU_RET(ctx); return; }
        InterlockedIncrement(&s_natc_fallback);
    }
    run_orig(ctx, s_cull_orig);
}

template <int I> void stage_hook(spu_context* ctx)
{
    if (s_corpus_n && s_corpus_done[I] < s_corpus_n &&
        InterlockedIncrement(&s_corpus_calls[I]) % s_corpus_every == 1) {
        const LONG k = InterlockedIncrement(&s_corpus_done[I]);
        if (k <= s_corpus_n) {
            static __declspec(thread) uint8_t* ls_before;
            static __declspec(thread) spu_context* ctx_before;
            if (!ls_before) { ls_before = (uint8_t*)malloc(SPU_LS_SIZE); ctx_before = (spu_context*)malloc(sizeof *ctx); }
            memcpy(ls_before, ctx->ls, SPU_LS_SIZE);
            memcpy(ctx_before->gpr, ctx->gpr, sizeof ctx->gpr);
            run_orig(ctx, s_stage_orig[I]);
            corpus_write(I, ctx_before, ls_before, ctx, (int)k);
            return;
        }
    }
    const uint64_t t0 = __rdtsc();
    run_orig(ctx, s_stage_orig[I]);
    t_stage_cyc[I] += __rdtsc() - t0;
}
const spu_fn_t kStageHooks[kNStages] = { stage_hook<0>, stage_hook<1>, stage_hook<2>, stage_hook<3>,
                                         stage_hook<4>, stage_hook<5>, stage_hook<6>, stage_hook<7> };

} // namespace

extern "C" void if2_edge_configure(void)
{
    const char* e;
    s_dump_left = (e = getenv("EG_DUMP")) ? atoi(e) : 0;
    s_census = (e = getenv("EG_CENSUS")) && *e && *e != '0';
    s_spin = (e = getenv("EG_SPIN")) ? atoi(e) : 0;
    if ((e = getenv("EG_CORPUS")) && *e) {
        char buf[MAX_PATH + 32];
        snprintf(buf, sizeof buf, "%s", e);
        char* c1 = strchr(buf, ',');
        if (c1) { *c1 = 0; s_corpus_n = atoi(c1 + 1); char* c2 = strchr(c1 + 1, ','); if (c2) s_corpus_every = atoi(c2 + 1); }
        else s_corpus_n = 50;
        if (s_corpus_every < 1) s_corpus_every = 1;
        snprintf(s_corpus_dir, sizeof s_corpus_dir, "%s", buf);
        CreateDirectoryA(s_corpus_dir, NULL);
        s_census = 1;   /* the stage hooks */
    }
    {   /* the native_geometry row (PC Settings > Frame > CPU, default On; EG_NATIVE=0/1 sets and locks it) */
        char ov[16] = "1";
        const int k = opt_find("native_geometry");
        if (k >= 0) opt_text(k, ov, sizeof ov);
        s_native = !(ov[0] == '0' || !strcmp(ov, "off") || !strcmp(ov, "false"));
    }
    s_verify_every = (e = getenv("EG_VERIFY")) ? atoi(e) : 0;
    if (s_native || s_verify_every) {
        s_skin_orig = spu_registry_replace(kImage, 0xD0A8, skin_hook);
        if (!s_skin_orig) fprintf(stderr, "[EG] the skinning stage is not replaceable: native off%c", 10);
        else fprintf(stderr, "[EG] native skinning %s%s%c", s_native ? "on" : "off",
                     s_verify_every ? " (EG_VERIFY: compared with the SPU code)" : "", 10);
        if (s_native) atexit([] { fprintf(stderr, "[EG] native skinning: %ld calls, %ld fell back to the SPU code%c",
                                          (long)s_nat_calls, (long)s_nat_fallback, 10); });
        const char* nc = getenv("EG_NO_CULL");   /* A/B: skinning only */
        if (!(nc && *nc == 0x31)) s_cull_orig = spu_registry_replace(kImage, 0xA798, cull_hook);
        if (!s_cull_orig) fprintf(stderr, "[EG] native culling off%c", 10);
        else if (s_native) atexit([] { fprintf(stderr, "[EG] native culling: %ld calls, %ld fell back to the SPU code%c",
                                                (long)s_natc_calls, (long)s_natc_fallback, 10); });
    }
    if (!s_dump_left && !s_census && !s_spin) return;
    s_seg_orig = spu_registry_replace(kImage, kSegment, segment_hook);
    if (!s_seg_orig) {
        fprintf(stderr, "[EG] the Edge job's segment call (image %d, LS 0x%X) is not lifted as replaceable: EG off%c",
                kImage, kSegment, 10);
        return;
    }
    if (s_census)
        for (int i = 0; i < kNStages; i++) s_stage_orig[i] = spu_registry_replace(kImage, kStages[i].addr, kStageHooks[i]);
    if (s_census) atexit(census_print);
    fprintf(stderr, "[EG] hooked: dump %d census %d spin %d%%%c", s_dump_left, s_census, s_spin, 10);
}

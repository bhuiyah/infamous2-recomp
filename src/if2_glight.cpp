/* if2_glight.cpp -- GL1: inFamous 2's SPU deferred lighting on the GPU.
 *
 * The lighting job (job module c6e7383dc013, image 218) lights the G-buffer
 * the walker copied to main memory, tile by tile (32x40 tiles, 720 a frame,
 * shared by the job's instances through an atomic counter; the instance that
 * finishes the last tile writes the RSX label 0x870 the GPU waits for). Its
 * per-tile kernel (LS 0x4AC8, reached by an indirect call) is replaced by a
 * stub: the tile counting, the completion and the label stay the job's own,
 * the tile's transfers into the two buffers move nothing (a DMA skip window),
 * and the stub hands the job's parameters to the renderer, which computes the
 * two images in a compute pass (libs/video/rsx_d3d12_glight.c).
 *
 * GLIGHT=0: the job runs on the SPUs as before. GLIGHT_VERIFY=1: the SPU
 * kernel runs, the GPU pass runs on the kernel's own input, and the renderer
 * compares the two every frame ([GLIGHT-VERIFY]). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_context.h"
#include "rsx_d3d12_glight.h"
#include "ps3emu/options.h"

extern "C" {
typedef void (*spu_fn_t)(spu_context*);
spu_fn_t spu_registry_replace(int image_id, uint32_t addr, spu_fn_t fn);
int spu_dma_skip_set(int image_id, uint32_t lo, uint32_t hi);
void spu_dma_skip_clear(void);
void spu_indirect_branch(spu_context* ctx);
extern uint8_t* vm_base;
}

namespace {

const int kImage = 218;                 /* jobmod_c6e7383dc013 (generated/spu_jobs/jobmods_register.c) */
const uint32_t kKernel = 0x4AC8;        /* the per-tile lighting kernel */
const uint32_t kC0 = 0x47400B80u;       /* normals in, diffuse out */
const uint32_t kC1 = 0x47784B80u;       /* depth in, specular out */
const uint32_t kPitch = 5120, kW = 1280, kH = 720, kPlane = 5120u * 720u;
const uint32_t kLightsCap = 256;
/* The SSAO job (jobmod_e6fe749031f8, image 224) runs five plug-ins (images
 * 160-164, lifted at the PIC base 0x20000, entry 0x20030): depth -> half-res
 * linear depth, AO, two bilateral blurs, a joint bilateral 2x upsample into the
 * full-res B8 at RSX local 0xCF800000. Its parameters sit at guest
 * 0x00A94980..0x00A94AB0. */
const int kAoImage = 224;
const int kAoPlug[5] = { 160, 161, 162, 163, 164 };
const uint32_t kAoEntry = 0x20030;
const uint32_t kAoOut = 0xCF800000u, kAoOutBytes = 1280u * 720u;
const uint32_t kAoParams = 0x00A94980u, kAoParamBytes = 0x130u;
const uint32_t kAux = 0x47B08B80u, kAuxEnd = 0x47C22480u;   /* the job's half-res planes */

spu_fn_t s_orig;
spu_fn_t s_ao_orig[5];
int s_ao;
uint8_t s_aoparams[0x130];
int s_verify;
volatile LONG s_seq;                    /* seqlock: odd while the snapshot is written */
uint8_t s_consts[256];
uint8_t s_lights[kLightsCap * 48];
uint32_t s_nl;
uint8_t* s_vin;                         /* verify: the kernel's input, both planes */

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline const uint8_t* lsp(spu_context* ctx, uint32_t a) { return ctx->ls + (a & (SPU_LS_SIZE - 1)); }

/* The job keeps its constant block's and light list's LS addresses at
 * LS 0x9C80 and 0x9CB0 (main 0x43D0 stores them; the kernel reads them). */
void snapshot(spu_context* ctx)
{
    const uint32_t cl = be32(lsp(ctx, 0x9C80)), ll = be32(lsp(ctx, 0x9CB0));
    uint32_t nl = be32(lsp(ctx, cl + 0x90));
    if (nl > kLightsCap) nl = kLightsCap;
    InterlockedIncrement(&s_seq);
    memcpy(s_consts, lsp(ctx, cl), 256);
    for (uint32_t i = 0; i < nl * 48u; i += 16) memcpy(s_lights + i, lsp(ctx, ll + i), 16);
    s_nl = nl;
    if (s_ao) memcpy(s_aoparams, vm_base + kAoParams, kAoParamBytes);   /* this frame's: the SSAO job ran just before */
    InterlockedIncrement(&s_seq);
}

void kernel_stub(spu_context* ctx)
{
    const uint32_t t = ctx->gpr[3]._u32[0];           /* the tile's context (LS) */
    const uint32_t tx = be32(lsp(ctx, t + 4)), ty = be32(lsp(ctx, t + 8));
    if (tx == 0 && ty == 0) snapshot(ctx);             /* one tile 0 a frame: its parameters */
    if (s_verify) {
        if (s_vin && tx < kW / 32 && ty < kH / 40)
            for (uint32_t y = 0; y < 40; y++) {
                const uint32_t o = (ty * 40 + y) * kPitch + tx * 128;
                memcpy(s_vin + o, vm_base + kC0 + o, 128);
                memcpy(s_vin + kPlane + o, vm_base + kC1 + o, 128);
            }
        s_orig(ctx);                                   /* the SPU's own lighting */
        return;
    }
    SPU_RET(ctx);
}

uint32_t params_seq() { const LONG s = s_seq; return (s & 1) ? 0 : (uint32_t)s / 2; }

int params_copy(uint8_t* consts, uint8_t* lights, uint32_t cap, uint8_t* aoparams, uint32_t* nlights, uint32_t* seq)
{
    for (int tries = 0; tries < 1000; tries++) {
        const LONG a = s_seq;
        if (a == 0) return 0;
        if (a & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        uint32_t nl = s_nl; if (nl > cap) nl = cap;
        memcpy(consts, s_consts, 256);
        memcpy(lights, s_lights, nl * 48u);
        memcpy(aoparams, s_aoparams, kAoParamBytes);
        MemoryBarrier();
        if (s_seq != a) continue;
        *nlights = nl; *seq = (uint32_t)a / 2;
        return 1;
    }
    return 0;
}

int verify_input(uint8_t* dst, uint32_t bytes, uint32_t seq)
{
    (void)seq;
    if (!s_vin || bytes != 2 * kPlane) return 0;
    memcpy(dst, s_vin, bytes);
    return 1;
}

/* The pipeline could not be built (at device init, before the first frame):
 * the job whole again -- its transfers move bytes, its kernel is the SPU's. */
void unavailable()
{
    spu_dma_skip_clear();
    if (s_orig) spu_registry_replace(kImage, kKernel, s_orig);
    for (int i = 0; i < 5; i++) if (s_ao_orig[i]) spu_registry_replace(kAoPlug[i], kAoEntry, s_ao_orig[i]);
}

/* An SSAO plug-in's per-tile entry: nothing (the GPU makes the images). */
void ao_stub(spu_context* ctx) { SPU_RET(ctx); }

} // namespace

extern "C" void if2_glight_configure(void)
{
    /* the gpu_lighting row (PC Settings > Frame > CPU, default On; GLIGHT=0/1 sets and locks it) */
    char ov[16] = "1";
    { const int k = opt_find("gpu_lighting"); if (k >= 0) opt_text(k, ov, sizeof ov); }
    const char* v = getenv("GLIGHT_VERIFY");
    const int on = !(ov[0] == '0' || !strcmp(ov, "off") || !strcmp(ov, "false"));
    s_verify = v && *v && *v != '0';
    if (!on && !s_verify) return;
    s_orig = spu_registry_replace(kImage, kKernel, kernel_stub);
    if (!s_orig) {
        fprintf(stderr, "[GLIGHT] the lighting job's kernel (image %d, LS 0x%X) is not lifted: GL1 off%c", kImage, kKernel, 10);
        return;
    }
    if (s_verify) s_vin = (uint8_t*)calloc(2, kPlane);
    else spu_dma_skip_set(kImage, kC0, kC0 + 2 * kPlane);   /* both planes (contiguous) */
    const char* ea = getenv("GLIGHT_AO");
    s_ao = !(ea && *ea == '0');
    if (s_ao && !s_verify) {
        for (int i = 0; i < 5; i++) s_ao_orig[i] = spu_registry_replace(kAoPlug[i], kAoEntry, ao_stub);
        int all = 1; for (int i = 0; i < 5; i++) all &= s_ao_orig[i] != NULL;
        if (!all) {
            for (int i = 0; i < 5; i++) if (s_ao_orig[i]) spu_registry_replace(kAoPlug[i], kAoEntry, s_ao_orig[i]);
            fprintf(stderr, "[GLIGHT] the SSAO job's plug-ins are not all lifted: SSAO stays on the SPUs%c", 10);
            s_ao = 0;
        } else {
            spu_dma_skip_set(kAoImage, kC0, kAuxEnd);              /* the G-buffer copies and its half-res planes */
            spu_dma_skip_set(kAoImage, kAoOut, kAoOut + kAoOutBytes);
        }
    }
    rsx_glight_cfg c;
    memset(&c, 0, sizeof c);
    c.ea_c0 = kC0; c.ea_c1 = kC1; c.w = kW; c.h = kH; c.pitch = kPitch;
    c.ea_ao = s_ao ? kAoOut : 0;
    c.params_seq = params_seq; c.params_copy = params_copy;
    c.verify_input = s_verify ? verify_input : NULL;
    c.unavailable = unavailable;
    rsx_glight_configure(&c);
    fprintf(stderr, "[GLIGHT] configured: the lighting job's kernel %s%c",
            s_verify ? "runs on the SPU and the GPU (GLIGHT_VERIFY)" : "runs on the GPU", 10);
}

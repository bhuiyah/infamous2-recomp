/*
 * inFamous 2 -- title-specific HLE overrides.
 *
 * main.cpp calls infamous2_install_overrides() BEFORE the runtime registers
 * its own handlers. ps3_hle_call() scans the ctx-handler table in registration
 * order and the first match wins, so anything registered here shadows the
 * generic ps3recomp implementation of that NID for this title only.
 *
 * Rules for this file (same discipline the other ps3recomp ports follow):
 *   - Guest pointers arrive as 32-bit EAs in r3..r10. Never dereference them as
 *     host pointers; go through the vm_read / vm_write helpers (big-endian).
 *   - Return values go in ctx->gpr[3].
 *   - If a fix is not inFamous-2-specific, it belongs upstream in ps3recomp,
 *     not here. Send it as a PR and delete the override once it lands.
 *
 * Every override should carry a comment saying what in the game needed it and
 * how it was confirmed (RPCS3 log, Ghidra, a trace), so it can be removed later.
 */

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ps3emu/nid.h"
#include "ppu_context.h"
#include "if2_version.h"
#include "display_cfg.h"   /* libs/video: aspect / bool parsing (header-only) */
#include "rsx_display.h"   /* libs/video: display.ini, the presented aspect */
#include "ps3emu/options.h" /* the settings store: this title's rows */
#define RSX_PFX_NO_D3D12           /* only the backend API, no D3D12 headers */
#include "rsx_postfx.h"            /* libs/video: the PC anti-aliasing / sharpening passes */
/* the save-state image (the TITL section), through the toolkit's runtime/ppu
 * include directory, as main.cpp reaches it */
#include "../savestate/ss_state.h"

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name,
                                     void (*fn)(ppu_context*));

/* Register by exported function name; the NID is SHA-1(name + suffix). */
#define IF2_OVERRIDE(name, fn) \
    ps3_hle_register_ctx(ps3_compute_nid(name), name, fn)

/* Guest memory, big-endian (runtime/ppu/ppu_loader.cpp; the lifted code's own
 * accessors, so writes break reservations and feed the write watch). */
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void     vm_write32(uint64_t addr, uint32_t val);

/* libs/video/cellGcmSys.c: `config` is a guest EA carried in a pointer. */
extern "C" int32_t cellGcmGetConfiguration(void* config);

/* ---------------------------------------------------------------------------
 * Example (disabled): PlayStation Move.
 *
 * inFamous 2 shipped with optional PS Move support. ps3recomp's cellGem is a
 * no-device stub, which is what we want -- the game should take its DualShock
 * path. If bring-up shows it stalling in a cellGem call instead, an override
 * that reports "not connected" goes here, e.g.:
 *
 *   static void if2_cellGemGetStatus(ppu_context* ctx)
 *   {
 *       ctx->gpr[3] = 0x80121801u;   // CELL_GEM_ERROR_UNINITIALIZED -- verify
 *   }                                // against RPCS3's cellGem.h before use
 *   ...
 *   IF2_OVERRIDE("cellGemGetStatus", if2_cellGemGetStatus);
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Ultrawide: a wider field of view (Hor+) with the HUD laid out for it.
 *
 * The game keeps its screen shape in one integer, the "aspect width" W at
 * 0x96EF88, beside the output height H at 0x96EF84. Video init (0x13211C,
 * called once, from 0x132978) writes W = 16H/9, or 4H/3 on a 4:3 TV -- with
 * the same 1280x720 buffers: the shipped SD output is already anamorphic.
 * Everything that has a screen shape takes it from W/H:
 *   - every camera, when it is built (ctor 0x440208, aspect = W/H at
 *     0x4403C0 via slot 0x948D90, its only reader); SetFov 0x43E370 makes
 *     tanX = tanY * aspect, so the view is Hor+ (vertical FOV unchanged), and
 *     the frustum (PPU, SPU vis/Edge), shadow fitting, picking/aim and the
 *     radial post effects follow the camera;
 *   - the UI canvas, W*720/H wide (0x42B3E0), anchored with the safe-area
 *     X margin at 0xA010B0 (= int(W*0.075) from video init); the reticle and
 *     world markers map NDC onto that canvas through W/H (0x425A08).
 * So W' = round(H * aspect) gives the wide view and the wide HUD, rendered
 * anamorphically into the same 1280x720 buffers; the backend then shows the
 * image at that aspect (rsx_display_set_content_aspect: window shape,
 * letter/pillarbox bars, fullscreen). Confirmed by the 2026-10-14 X1 run
 * (W poked to 1707: menu and gameplay wider, HUD on the wide canvas, same
 * fps) and the camera/HUD/culling studies.
 *
 * WHEN (static proof, generated PPU code): 0x132978 calls video init at
 * 0x1329A4, then 0x4EF18C at 0x1329DC (in between it reads only its config
 * struct); 0x4EF18C calls _cellGcmInitBody (HLE) and then
 * cellGcmGetConfiguration at 0x4EF20C -- its only caller in the EBOOT. So
 * the override below runs right after video init wrote W, before anything
 * read it; no camera exists yet (all are made by runtime factories once the
 * game world loads). W' is also written at install, after the ELF is loaded
 * and before the guest runs, for anything that might read W before video
 * init. Video init runs once, so nothing writes W back later.
 *
 * Also applied there:
 *   - Two render passes take W as if it were the buffer width: noise tiling
 *     (W/texW against renderH/texH, 0xE55A4) and a 0.5/W half-texel offset
 *     (0x2EB448). Their pool slots (0x93C67C, 0x944760; one reader each) are
 *     pointed at the render width at 0xA009DC (1280) instead, which is what
 *     they get at 16:9. IF2_TEXELFIX=0 leaves them.
 *   - hud = wide: safe X = int(W'*0.075) (128 at W'=1707) -- HUD in the
 *     corners of the wide screen; hud = 16:9: the margin of the central
 *     16:9 area, 96 + (W'-1280)/2 (309) -- HUD where it is on a 16:9 TV.
 *   - lod = 1: four screen-size thresholds compare against d*tanX, so at a
 *     wider aspect every object would count as smaller and lose detail (or
 *     be culled) nearer the camera, across the whole screen. Each constant
 *     has exactly one reader (pool-base resolver, lis-based and raw-pointer
 *     scans of the EBOOT agree) and is scaled by k = W'/(16H/9) so the
 *     decision for an object is the one 16:9 makes: 90000*k^2 at 0x93DE54
 *     ((d*tanX)^2 test, 0x147F30), 40*k at 0x945BE4 (0x343CEC), 0.2/k at
 *     0x949244 (r/(d*tanX), 0x44D364), 0.1/k at 0x9442CC (visprog mode 3,
 *     0x2D6368). 0.01 at 0x9491D8 is shared with a timer test (0x44C258)
 *     and is left alone. Cost: the per-object detail of 16:9, for the extra
 *     objects the wider view shows. Update 1.04 adds a dynamic-quality
 *     table whose levels 12..20 give the other visprog modes a threshold
 *     (c*tanX)^2 too (1.00: 0); its nine c values are scaled the same way.
 *   - fmv = fit: see if2_fmv_quad.
 *
 * Settings (env first, then display.ini): IF2_ASPECT / aspect =
 * auto|off|64:27|21:9|43:18|32:9|16:10|<w:h>|<number> (auto = the monitor's
 * aspect when wider than 16:9, else unchanged), IF2_HUD / hud = wide|16:9,
 * IF2_LOD / lod = 1|0, IF2_FMV / fmv = fit|fill. At 16:9 nothing is written
 * or registered.
 * -----------------------------------------------------------------------*/

namespace {

/* (tools/if2_items.py: disc EBOOT 0x96EF84 / 0x96EF88 / 0xA010B0 / 0xA009DC, slots 0x93C67C / 0x944760 /
 * 0x944600 / 0x948D90) */
constexpr uint32_t kH         = IF2_R(DISP_H);       /* output height (int) */
constexpr uint32_t kW         = IF2_R(DISP_W);       /* aspect width (int) */
constexpr uint32_t kSafeX     = IF2_R(SAFE_X);       /* UI safe-area X margin */
constexpr uint32_t kRenderW   = IF2_R(RENDER_W);     /* render width (int, 1280) */
constexpr uint32_t kSlotNoise = IF2_R(SLOT_NOISE);   /* -> W, read at 0xE55A4 only */
constexpr uint32_t kSlotTexel = IF2_R(SLOT_TEXEL);   /* -> W, read at 0x2EB448 only */
constexpr uint32_t kSlotFmv   = IF2_R(SLOT_FMV);     /* -> W, read at 0x2E7F7C only (FMV quad) */
constexpr uint32_t kSlotCam   = IF2_R(SLOT_CAM);     /* -> W, camera ctor only */

struct LodConst { uint32_t ea; uint32_t stock; int power; const char* what; };
/* power: the threshold scales by k^power */
constexpr LodConst kLodAll[] = {
#if IF2_HAVE_LOD
    /* (the reader named in each string is this EBOOT's) */
    { IF2_R(LOD_0), 0x47AFC800u,  2, IF2_KNOWN_STR("(d*tanX)^2 < 90000 (0x147F30)", "(d*tanX)^2 < 90000 (0x151D58)", "(d*tanX)^2 < 90000") },
    { IF2_R(LOD_1), 0x42200000u,  1, IF2_KNOWN_STR("d*0.005*tanX vs 40 (0x343CEC)", "d*0.005*tanX vs 40 (0x35D7CC)", "d*0.005*tanX vs 40") },
    { IF2_R(LOD_2), 0x3E4CCCCDu, -1, IF2_KNOWN_STR("r/(d*tanX) vs 0.2 (0x44D364)", "r/(d*tanX) vs 0.2 (0x48C97C)", "r/(d*tanX) vs 0.2") },
    { IF2_R(LOD_3), 0x3DCCCCCDu, -1, IF2_KNOWN_STR("visprog mode 3: 0.1 (0x2D6368)", "visprog mode 3: 0.1 (0x2EDAC0)", "visprog mode 3: 0.1") },
#endif
#if IF2_HAVE_LOD && IF2_HAVE_LOD_QUALITY
    /* 1.04's dynamic-quality table 0x9D7224 (21 x 20 bytes; level = [0xA6E1F0+0x6D0], 0..20, raised while the
     * scene's per-frame display data exceeds 5 MiB): column +0xC is c of the visprog threshold (c*tanX)^2 for
     * the modes other than 3 (0x2ED5DC..0x2ED5FC), the column's only reader; 0 below level 12. 1.00 had no
     * such table (that threshold was 0). Scaled like mode 3's 0.1. */
    { IF2_R(QUALITY_C12), 0x3D4CCCCDu, -1, IF2_KNOWN_STR("", "quality level 12: c 0.05 (0x2ED5DC)", "quality level 12: c 0.05") },
    { IF2_R(QUALITY_C13), 0x3DCCCCCDu, -1, IF2_KNOWN_STR("", "quality level 13: c 0.1 (0x2ED5DC)", "quality level 13: c 0.1") },
    { IF2_R(QUALITY_C14), 0x3E4CCCCDu, -1, IF2_KNOWN_STR("", "quality level 14: c 0.2 (0x2ED5DC)", "quality level 14: c 0.2") },
    { IF2_R(QUALITY_C15), 0x3E4CCCCDu, -1, IF2_KNOWN_STR("", "quality level 15: c 0.2 (0x2ED5DC)", "quality level 15: c 0.2") },
    { IF2_R(QUALITY_C16), 0x3E4CCCCDu, -1, IF2_KNOWN_STR("", "quality level 16: c 0.2 (0x2ED5DC)", "quality level 16: c 0.2") },
    { IF2_R(QUALITY_C17), 0x3E4CCCCDu, -1, IF2_KNOWN_STR("", "quality level 17: c 0.2 (0x2ED5DC)", "quality level 17: c 0.2") },
    { IF2_R(QUALITY_C18), 0x3E4CCCCDu, -1, IF2_KNOWN_STR("", "quality level 18: c 0.2 (0x2ED5DC)", "quality level 18: c 0.2") },
    { IF2_R(QUALITY_C19), 0x3E4CCCCDu, -1, IF2_KNOWN_STR("", "quality level 19: c 0.2 (0x2ED5DC)", "quality level 19: c 0.2") },
    { IF2_R(QUALITY_C20), 0x3E4CCCCDu, -1, IF2_KNOWN_STR("", "quality level 20: c 0.2 (0x2ED5DC)", "quality level 20: c 0.2") },
#endif
    { 0u, 0u, 0, nullptr },   /* (end: the LOD items may all be missing on another EBOOT) */
};
const LodConst* const kLod = kLodAll;

double g_aspect    = 0.0;   /* effective aspect; 0 = stock (nothing applied) */
int    g_hud_169   = 0;
int    g_lod       = 1;
int    g_texelfix  = 1;
int    g_fmv_fit   = 0;     /* set when the aspect is active and fmv = fit */
int    g_applied   = 0;
/* Detail distance (settings store row "detail", 1.0 = the PS3's): the LOD
 * thresholds scale by k = (lod ? W'/(16H/9) : 1) * detail. g_k_wide is
 * W'/(16H/9) once video init applied the aspect (1 at 16:9). */
double   g_detail  = 1.0;
double   g_k_wide  = 1.0;
constexpr int kNLod = (int)(sizeof kLodAll / sizeof kLodAll[0]) - 1;
constexpr int kNLodA = kNLod ? kNLod : 1;   /* (array sizes) */
uint32_t g_lod_cur[kNLodA];      /* what we last left in each (kLod[].stock until the first apply) */
int      g_lod_foreign[kNLodA];
bool     g_lod_init = false;

uint32_t f2u(float f)    { uint32_t u; memcpy(&u, &f, 4); return u; }
float    u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* Write the LOD thresholds for the current k. A constant that does not
 * hold what we last left there (the stock value at first) is someone else's:
 * left alone. video_init: the aspect's own log lines, as before the store. */
void if2_lod_apply(bool video_init)
{
    if (!g_lod_init) {
        for (int i = 0; i < kNLod; i++) g_lod_cur[i] = kLod[i].stock;
        g_lod_init = true;
    }
    const double k = (g_lod ? g_k_wide : 1.0) * g_detail;
    for (int i = 0; i < kNLod; i++) {
        const LodConst& c = kLod[i];
        const uint32_t cur = vm_read32(c.ea);
        if (cur != g_lod_cur[i]) {
            if (!g_lod_foreign[i]++)
                fprintf(stderr, "[if2-aspect] LOD 0x%X holds 0x%08X, not the stock 0x%08X: left\n",
                        c.ea, cur, c.stock);
            continue;
        }
        const float nv = (float)((double)u2f(c.stock) * std::pow(k, (double)c.power));
        if (f2u(nv) == cur) continue;
        vm_write32(c.ea, f2u(nv));
        g_lod_cur[i] = f2u(nv);
        if (video_init)
            fprintf(stderr, "[if2-aspect] LOD k=%.4f: 0x%X %g -> %g  %s\n",
                    k, c.ea, (double)u2f(c.stock), (double)nv, c.what);
        else
            fprintf(stderr, "[if2-lod] detail %.2f, lod %d: k=%.4f: 0x%X %g -> %g  %s\n",
                    g_detail, g_lod, k, c.ea, (double)u2f(c.stock), (double)nv, c.what);
    }
    fflush(stderr);
}

const char* if2_opt(const char* env, const char* key, const char** src)
{
    const char* e = getenv(env);
    if (e && *e) { *src = env; return e; }
    *src = "display.ini";
    return rsx_display_cfg_get(key);
}

/* safe X as video init computes it: (int)(x * 0.075f) */
uint32_t safe_margin(uint32_t w) { return (uint32_t)(int)((float)w * 0.075f); }

void if2_aspect_apply(void)
{
    if (g_applied++ || g_aspect <= 0.0) return;   /* one guest call site; once */
    const uint32_t H = vm_read32(kH), W0 = vm_read32(kW);
    if (H < 240 || H > 2160) {
        /* Video init's own W stays, so the image is its stock shape again:
         * take back the wide window shape announced at install, or the
         * 16:9 picture would be stretched across it. */
        rsx_display_set_content_aspect(0.0);
        fprintf(stderr, "[if2-aspect] H = %u at video init is not a video height: nothing applied\n", H);
        return;
    }
    const uint32_t W = (uint32_t)std::lround((double)H * g_aspect);
    const uint32_t w169 = (H << 4) / 9;             /* video init's 16:9 W */
    vm_write32(kW, W);
    rsx_display_set_content_aspect((double)W / (double)H);

    const uint32_t sx0 = vm_read32(kSafeX);
    const uint32_t sx = (g_hud_169 && W > w169) ? safe_margin(w169) + (W - w169) / 2 : safe_margin(W);
    vm_write32(kSafeX, sx);

    char slots[96] = "texel slots left (IF2_TEXELFIX=0)";
    if (g_texelfix) {
        int moved = 0;
        for (uint32_t s : { kSlotNoise, kSlotTexel })
            if (vm_read32(s) == kW) { vm_write32(s, kRenderW); moved++; }
        snprintf(slots, sizeof slots, "texel slots 0x%X/0x%X -> render width 0x%X (%d of 2)",
                 kSlotNoise, kSlotTexel, kRenderW, moved);
    }

    fprintf(stderr, "[if2-aspect] aspect %.4f: W %u -> %u (H %u), safe X %u -> %u (hud %s), %s\n",
            g_aspect, W0, W, H, sx0, sx, g_hud_169 ? "16:9" : "wide", slots);

    g_k_wide = (double)W / (double)w169;  /* tanX' / tanX */
    if (!g_lod) fprintf(stderr, "[if2-aspect] LOD thresholds left at their 16:9 values (lod = 0)\n");
    if (g_lod || g_detail != 1.0) if2_lod_apply(g_lod != 0);   /* k = (lod ? W'/(16H/9) : 1) * detail */
    fprintf(stderr, "[if2-aspect] full-screen movies: %s\n",
            g_fmv_fit ? "fit -- pillarboxed at 16:9 (needs tools/ppu_postlift.py in the build; "
                        "\"[if2-fmv]\" confirms it on the first movie frame)"
                      : "fill (stock: cropped top and bottom)");
    fflush(stderr);
}

/* The title's only cellGcmGetConfiguration call (0x4EF20C) comes right after
 * video init: the stock handler, then the aspect. */
void if2_cellGcmGetConfiguration(ppu_context* ctx)
{
    const int32_t rc = cellGcmGetConfiguration((void*)(uintptr_t)(uint32_t)ctx->gpr[3]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    if2_aspect_apply();
}

int if2_aspect_install(void)
{
    /* Load display.ini now, on the main thread before any guest code runs:
     * that also makes the process per-monitor DPI aware (dpi_aware), which
     * belongs before the first window and the first monitor query. With
     * every IF2_* set in the environment nothing below would ask for the
     * file, leaving it to the frame clock thread while the guest already
     * runs. */
    (void)rsx_display_cfg_get(NULL);
    const char* src = "";
    const char* v = if2_opt("IF2_ASPECT", "aspect", &src);
    double a = dcfg_parse_aspect(v ? v : "auto");
    if (a == DCFG_ASPECT_BAD) {
        fprintf(stderr, "[if2-aspect] %s aspect = %s: expected auto, off, 64:27, 21:9, 32:9, "
                        "16:10, W:H or a number; using auto\n", src, v);
        a = DCFG_ASPECT_AUTO;
    }
    if (a == DCFG_ASPECT_OFF) {
        fprintf(stderr, "[if2-aspect] off: stock 16:9\n");
        return 0;
    }
    char why[96];
    if (a == DCFG_ASPECT_AUTO) {
        int mw = 0, mh = 0;
        const double m = rsx_display_target_aspect(&mw, &mh);
        a = (m > 16.0 / 9.0 + 0.01) ? m : 16.0 / 9.0;
        snprintf(why, sizeof why, "auto: monitor %dx%d", mw, mh);
    } else {
        snprintf(why, sizeof why, "%s = %s", src, v);
    }
    const double lo = 4.0 / 3.0, hi = 32.0 / 9.0;
    if (a < lo || a > hi) {
        fprintf(stderr, "[if2-aspect] %.4f (%s) is outside 4:3..32:9, clamped\n", a, why);
        a = a < lo ? lo : hi;
    }
    if (std::fabs(a - 16.0 / 9.0) < 0.002) {
        fprintf(stderr, "[if2-aspect] 16:9 (%s): stock, nothing changed\n", why);
        return 0;
    }
    if (!IF2_HAVE(ASPECT)) {
        if2_feature_off("aspect ratio", "the display width / height, safe area or their pool slots");
        return 0;
    }
    /* Only this EBOOT: every location still holds its stock value. */
    const uint32_t W0 = vm_read32(kW), H0 = vm_read32(kH);
    if (W0 != 1280 || H0 != 720 || vm_read32(kSlotNoise) != kW || vm_read32(kSlotTexel) != kW ||
        vm_read32(kSlotFmv) != kW || vm_read32(kSlotCam) != kW) {
        fprintf(stderr, "[if2-aspect] W/H or their pool slots are not the values of %s "
                        "(W=%u H=%u): aspect %.4f NOT applied\n", IF2_EBOOT_NAME, W0, H0, a);
        return 0;
    }

    const char* hs = if2_opt("IF2_HUD", "hud", &src);
    if (hs) {
        char n[16];
        dcfg_norm(hs, n, sizeof n);
        if (!strcmp(n, "16:9") || !strcmp(n, "169") || !strcmp(n, "center") || !strcmp(n, "centre"))
            g_hud_169 = 1;
        else if (strcmp(n, "wide") && n[0])
            fprintf(stderr, "[if2-aspect] %s hud = %s: expected wide or 16:9, using wide\n", src, hs);
    }
    const char* ls = if2_opt("IF2_LOD", "lod", &src);
    if (ls) {
        const int b = dcfg_parse_bool(ls);
        if (b < 0) fprintf(stderr, "[if2-aspect] %s lod = %s: expected 1 or 0, using 1\n", src, ls);
        else g_lod = b;
    }
    g_fmv_fit = 1;
    const char* fs = if2_opt("IF2_FMV", "fmv", &src);
    if (fs) {
        char n[16];
        dcfg_norm(fs, n, sizeof n);
        if (!strcmp(n, "fill") || !strcmp(n, "crop") || !strcmp(n, "stock")) g_fmv_fit = 0;
        else if (strcmp(n, "fit") && n[0])
            fprintf(stderr, "[if2-aspect] %s fmv = %s: expected fit or fill, using fit\n", src, fs);
    }
    { const char* e = getenv("IF2_TEXELFIX"); if (e && *e == '0') g_texelfix = 0; }

    g_aspect = a;
    const uint32_t W = (uint32_t)std::lround(720.0 * a);
    vm_write32(kW, W);                                  /* before video init too */
    rsx_display_set_content_aspect((double)W / 720.0);  /* window shape */
    fprintf(stderr, "[if2-aspect] aspect %.4f (%s): W' = %u at video init\n", a, why, W);
    IF2_OVERRIDE("cellGcmGetConfiguration", if2_cellGcmGetConfiguration);
    return 1;
}

/* ---------------------------------------------------------------------------
 * This title's rows in the settings store (include/ps3emu/options.h). The
 * display.ini keys above keep their parsers (env first, then display.ini with
 * options.ini's [display] layered in by the backend); the rows give a menu
 * their type and choices, and make lod / fmv / detail live: applied at the
 * store's render point (the window thread, right after a Present). Defaults =
 * today: detail 1.0 writes nothing at 16:9 and the aspect's own values wide.
 * -----------------------------------------------------------------------*/
int chk_aspect(const char* v) { return dcfg_parse_aspect(v) != DCFG_ASPECT_BAD; }

#define IF2_NOPRESET { nullptr, nullptr, nullptr, nullptr, nullptr }
const opt_def kIf2Rows[] = {
    { "aspect", nullptr, "display", "display", "Display", "Screen", "Aspect ratio (field of view)",
      "auto: the monitor's shape when it is wider than 16:9; off: the stock 16:9 game; 64:27, 43:18, 21:9, "
      "32:9, 16:10 or W:H. Wider = more at the sides, same vertical view (Hor+), HUD spread out.",
      OPT_TEXT, OPT_RESTART, nullptr, nullptr, 0, 0, 0, "auto", "IF2_ASPECT", "DISPLAY_CFG", IF2_NOPRESET,
      OPT_F_ENHANCEMENT, "if2", chk_aspect },
    { "hud", nullptr, "display", "display", "Display", "Screen", "HUD layout",
      "wide: the screen's corners; 16:9: where a 16:9 TV shows it.",
      OPT_ENUM, OPT_RESTART, "wide|16:9", "Wide|16:9 (PS3)", 0, 0, 0, "wide", "IF2_HUD", "DISPLAY_CFG",
      IF2_NOPRESET, OPT_F_ENHANCEMENT, "if2", nullptr },
    { "lod", nullptr, "display", "display", "Display", "Screen", "Keep 16:9 level of detail",
      "On a wide screen, objects keep the detail and draw distance they have at 16:9.",
      OPT_BOOL, OPT_LIVE, nullptr, "Off|On", 0, 0, 0, "1", "IF2_LOD", "DISPLAY_CFG", IF2_NOPRESET, 0,
      "if2", nullptr },
    { "fmv", nullptr, "display", "display", "Display", "Screen", "Full-screen movies",
      "fit: the whole movie at 16:9 with bars at the sides; fill: the stock crop (top and bottom cut).",
      OPT_ENUM, OPT_LIVE, "fit|fill", "Fit|Fill (PS3)", 0, 0, 0, "fit", "IF2_FMV", "DISPLAY_CFG",
      IF2_NOPRESET, 0, "if2", nullptr },
    { "detail", "detail_distance", "options", "graphics", "Graphics", "Detail", "Detail distance",
      "Scales the distances at which objects keep their detail or are culled (the game's screen-size "
      "thresholds). 1.00 = the PS3; lower = fewer objects and more frames a second, higher = more detail "
      "further away at a CPU cost.",
      OPT_FLOAT, OPT_LIVE, nullptr, nullptr, 0.5, 2.0, 0.05, "1.0", "IF2_DETAIL", nullptr,
      { "1.0", "0.75", "1.0", "1.0", "1.25" }, OPT_F_ENHANCEMENT, "if2", nullptr },
};

opt_id g_opt_lod = -1, g_opt_fmv = -1, g_opt_detail = -1;

void if2_opt_changed(opt_id id, const char* old_v, const char* new_v, void* user)
{
    (void)old_v; (void)user;
    if (id == g_opt_detail) {
        g_detail = opt_f(id);
        if2_lod_apply(false);
    } else if (id == g_opt_lod) {
        g_lod = opt_i(id) == 1;
        if (g_applied && g_aspect > 0.0) if2_lod_apply(false);   /* at 16:9 lod changes nothing */
    } else if (id == g_opt_fmv) {
        g_fmv_fit = g_aspect > 0.0 && opt_i(id) == 0;
        fprintf(stderr, "[if2-fmv] full-screen movies: %s\n", g_fmv_fit ? "fit" : g_aspect > 0.0 ? "fill" :
                "stock (16:9: nothing to fit)");
        fflush(stderr);
    }
}

/* ---------------------------------------------------------------------------
 * Language. `language` is the PS3 system language the game is told (cellSysutilGetSystemParamInt LANG,
 * libs/system/cellSysutil.c), read once at boot (0x9E7C8). The game picks its language from it within its
 * edition's list: 0x9DFF8 matches TITLE_ID against the game's SKU list (the strings at 0x744B02) -> the
 * edition word (+0 of the platform object); 0x9D9DC looks the system language up in that edition's list
 * (kLangLists: {count, list} per edition), equal or one of the pairs at 0x7449E0 (british~english,
 * latino~spanish, brazilian~portuguese, chinese~chinese_s); no match = the list's first entry; a system
 * language over 16 (Portuguese (Brazil), English (UK), Turkish) counts as none. BCUS98125's list (edition 0):
 * english, french, latino, brazilian.
 *
 * `language_edition = europe` (beyond a US PS3, opt-in) puts the game's own European list (edition 1, the
 * BCES01143 one: british, french, italian, german, spanish, dutch, polish, portuguese, russian) into the US
 * edition's slot of that table, once, before the game runs. The table is read by 0x9D9DC alone (its only
 * reference, the TOC slot 0x9A10F4); the edition word itself, TITLE_ID and everything derived from them
 * (ratings, splash, DLC, saves, trophies) stay US. docs/NOTES.md 2026-11-11 / 2026-11-12.
 * -----------------------------------------------------------------------*/
constexpr uint32_t kLangLists = IF2_R(LANG_LISTS);     /* {count, list} per edition (disc 0x8D09D0) */
constexpr uint32_t kLangListUS = IF2_R(LANG_LIST_US);   /* english french latino brazilian (disc 0x6ECE14) */
constexpr uint32_t kLangListEU = IF2_R(LANG_LIST_EU);   /* british french ... portuguese russian (disc 0x6ECE24) */

const opt_def kLangEditionRow =
    { "language_edition", "edition", "options", "system", "System", "Language", "Language edition",
      "Which edition's languages the game offers. US (original): this disc's edition, BCUS98125, as on a PS3 -- "
      "English, French, Latin American Spanish and Brazilian Portuguese. Europe (beyond a US PS3): the game's own "
      "European-edition list, whose packs this disc also holds -- English (UK), French, Italian, German, Spanish "
      "(Spain), Dutch, Polish, Portuguese (Portugal), Russian. Only the language list changes: the game stays the US "
      "edition (saves, title screens, DLC, trophies). Takes effect at the next launch.",
      OPT_ENUM, OPT_RESTART, "us|europe", "US (original)|Europe (adds Russian, German, Italian, Polish, ...)",
      0, 0, 0, "us", "IF2_LANGUAGE_EDITION", nullptr, IF2_NOPRESET, OPT_F_ENHANCEMENT, "if2", nullptr };

const opt_def kLanguageUS =
    { "language", "system_language|lang", "options", "system", "System", "Language", "Language",
      "The PS3 system language the game sees: menus, subtitles and voices follow it, as on a PS3 set to that "
      "language. This edition (BCUS98125) has English, French, Latin American Spanish and Brazilian Portuguese; "
      "on a PS3 set to any other language it plays in English (Language edition: Europe offers more). Auto: the "
      "Windows display language when it is one of these, else English. Takes effect at the next launch (the game "
      "reads it once, at start).",
      OPT_ENUM, OPT_RESTART, "auto|english_us|french|spanish|portuguese",
      "Auto (Windows language)|English|French|Spanish (Latin America)|Portuguese (Brazil)",
      0, 0, 0, "auto", "IF2_LANGUAGE", nullptr, IF2_NOPRESET, 0, "cellSysutil", nullptr };

/* English (UK) is the system language english_uk (18): the game reads it as none and takes the list's first
 * entry, british -- what a UK PS3 gets. Spanish and Portuguese are the European ones here. */
const opt_def kLanguageEU =
    { "language", "system_language|lang", "options", "system", "System", "Language", "Language",
      "The PS3 system language the game sees, with Language edition: Europe (beyond a US PS3): menus, subtitles "
      "and voices follow it. English (UK) has British text and the English voices. Auto: the Windows display "
      "language when it is one of these (US English -> English (UK), Brazilian -> Portuguese), else English (UK). "
      "Takes effect at the next launch (the game reads it once, at start).",
      OPT_ENUM, OPT_RESTART, "auto|english_uk|french|italian|german|spanish|dutch|polish|portuguese|russian",
      "Auto (Windows language)|English (UK)|French|Italian|German|Spanish (Spain)|Dutch|Polish|Portuguese (Portugal)|"
      "Russian",
      0, 0, 0, "auto", "IF2_LANGUAGE", nullptr, IF2_NOPRESET, 0, "cellSysutil", nullptr };

/* A European disc (BCES01143, beta): the game is the European edition itself -- it picks the European list from
 * its own TITLE_ID -- so there is no Language edition row and the language row offers that list. */
const opt_def kLanguageEUDisc =
    { "language", "system_language|lang", "options", "system", "System", "Language", "Language",
      "The PS3 system language the game sees: menus, subtitles and voices follow it, as on a PS3 set to that "
      "language. This edition (BCES01143, Europe) has English (UK), French, Italian, German, Spanish, Dutch, Polish, "
      "Portuguese and Russian; on a PS3 set to another language it plays in English (UK). Auto: the Windows display "
      "language when it is one of these (US English -> English (UK), Brazilian -> Portuguese), else English (UK). "
      "Takes effect at the next launch (the game reads it once, at start).",
      OPT_ENUM, OPT_RESTART, "auto|english_uk|french|italian|german|spanish|dutch|polish|portuguese|russian",
      "Auto (Windows language)|English (UK)|French|Italian|German|Spanish (Spain)|Dutch|Polish|Portuguese (Portugal)|"
      "Russian",
      0, 0, 0, "auto", "IF2_LANGUAGE", nullptr, IF2_NOPRESET, 0, "cellSysutil", nullptr };

/* The UK disc (BCES01144, beta): its edition's list is British English alone, whatever the PS3's language. */
const opt_def kLanguageUKDisc =
    { "language", "system_language|lang", "options", "system", "System", "Language", "Language",
      "This edition (BCES01144, UK) has one language, English (UK): the game plays in it whatever the PS3's "
      "language is.",
      OPT_ENUM, OPT_RESTART, "auto|english_uk", "Auto|English (UK)",
      0, 0, 0, "auto", "IF2_LANGUAGE", nullptr, IF2_NOPRESET, OPT_F_HIDDEN, "cellSysutil", nullptr };

bool g_lang_europe = false;

/* The two rows: the edition first, then `language` with that edition's list (both restart rows, so the list
 * is fixed for the session). A saved language the edition does not offer is not one of the row's values: the
 * store logs it ("is not a value of this setting: ignored") and the row stays at Auto. Only the US disc has the
 * edition row: a European disc is that edition already. */
void if2_language_rows(void)
{
    if (!IF2_EDITION_US) {
        const bool uk = !strcmp(IF2_TITLE_ID, "BCES01144");
        opt_register(uk ? &kLanguageUKDisc : &kLanguageEUDisc);
        fprintf(stderr, "[if2-lang] %s (%s): the game's own %s language list; no Language edition setting\n",
                IF2_TITLE_ID, IF2_EDITION, uk ? "UK" : "European");
        return;
    }
    if (!IF2_HAVE(LANGUAGE_EDITION)) {
        if2_feature_off("Language edition: Europe", "the language-list table");
        opt_register(&kLanguageUS);
        return;
    }
    opt_register(&kLangEditionRow);
    const opt_id e = opt_find("language_edition");
    g_lang_europe = e >= 0 && opt_i(e) == 1;
    opt_register(g_lang_europe ? &kLanguageEU : &kLanguageUS);
}

/* After the EBOOT is in guest memory, before the game runs: with Europe, the US slot of the language-list
 * table gets the European entry. Checked first: anything else there (another EBOOT) and nothing is written. */
void if2_language_edition_apply(void)
{
    if (!g_lang_europe) return;
    const uint32_t c0 = vm_read32(kLangLists), l0 = vm_read32(kLangLists + 4);
    const uint32_t c1 = vm_read32(kLangLists + 8), l1 = vm_read32(kLangLists + 12);
    if (c0 != 4 || l0 != kLangListUS || c1 != 9 || l1 != kLangListEU) {
        fprintf(stderr, "[if2-lang] language edition Europe: the language table at 0x%08X is not this EBOOT's "
                "({%u, 0x%08X}, {%u, 0x%08X}): left as it is (US languages)\n", kLangLists, c0, l0, c1, l1);
        return;
    }
    vm_write32(kLangLists, c1);
    vm_write32(kLangLists + 4, l1);
    fprintf(stderr, "[if2-lang] language edition Europe: the US edition's language list (0x%08X) is the game's "
            "European one now (%u languages); the edition, title id and saves stay US\n", kLangLists, c1);
    fflush(stderr);
}

extern "C" void if2_achievements_rows(void);   /* src/if2_achievements.cpp */

/* Before if2_aspect_install: registers the rows and logs the store's summary. */
int if2_options_install(void)
{
    for (const opt_def& r : kIf2Rows) opt_register(&r);
    if2_language_rows();
    if2_achievements_rows();     /* src/if2_achievements.cpp: Recomp Achievements (off by default) */
    g_opt_lod = opt_find("lod");
    g_opt_fmv = opt_find("fmv");
    g_opt_detail = opt_find("detail");
    if (g_opt_detail >= 0) g_detail = opt_f(g_opt_detail);
    opt_log_summary();
    return 0;
}

/* After if2_aspect_install: live changes, and a detail other than 1.0 at
 * 16:9 (with a wide aspect video init applies it). */
void if2_options_watch(void)
{
    for (opt_id id : { g_opt_lod, g_opt_fmv, g_opt_detail })
        if (id >= 0) opt_watch(id, if2_opt_changed, nullptr, OPT_ON_RENDER);
    if (g_aspect <= 0.0 && g_detail != 1.0) if2_lod_apply(false);
}

/* ---------------------------------------------------------------------------
 * PC anti-aliasing and sharpening (libs/video/rsx_postfx.h; Graphics rows aa
 * and sharpen): where this title's 3D picture ends, so the passes take it
 * before the HUD. The final composite -- scene * (1 - bloom.a) + 2 * bloom +
 * glow (docs/GROUND_TRUTH.md section 8), a full-screen triangle with blending
 * off into the display buffer (its redirected surface 0x734000 / 0x3B0000)
 * that reads the distortion offsets (unit 0, 320x180), the lit scene (unit 1,
 * 0x11C0000), the particles (unit 5, 512x288) and the bloom (unit 6) -- is
 * E933D344 in play and in-engine cutscenes, and F4BBE02F (the same plus a
 * colour grade) under the pause menu and in some cutscenes. Everything drawn
 * into the display buffer after it is HUD or menu: the minimap (8C3E6603,
 * 0B356610, EFCF73BE), text (A426D9C6), the pause menu. Found in RSX_CAP
 * manifests (every pier gameplay capture: E933D344 the one full-screen
 * blend-off draw into 0x734000, followed only by HUD draws; the boat
 * cutscene: E933D344; the pause menu and a Kuo cutscene: F4BBE02F into
 * 0x3B0000); of the 322 programs in RPCS3's shader cache for
 * this title these two are the only ones with the composite's shape (they
 * read exactly units 0, 1, 5 and 6; the 2.0 bloom gain; the luma weights). A composite variant
 * not listed here costs only the effect: such frames keep the PS3 picture
 * ([postfx] counts them as "without the scene-end draw"). 75552C13 is the
 * Bink YUV->RGB pass: movie frames get no pass. The ids are microcode
 * hashes (the shaders are disc data): the same for both EBOOTs.
 * -----------------------------------------------------------------------*/
const unsigned kIf2SceneEnd[] = { 0xE933D344u, 0xF4BBE02Fu };
const unsigned kIf2Movie[] = { 0x75552C13u };

void if2_postfx_install(void)
{
    rsx_postfx_set_scene_end(kIf2SceneEnd, (int)(sizeof kIf2SceneEnd / sizeof kIf2SceneEnd[0]));
    rsx_postfx_set_movie(kIf2Movie, (int)(sizeof kIf2Movie / sizeof kIf2Movie[0]));
}

}  /* namespace */

/* ---------------------------------------------------------------------------
 * Full-screen movies at a wide W: fit (pillarbox) instead of fill (crop).
 *
 * The full-screen Bink quad (0x2E7EDC; called from 0x1CE248) draws through
 * 0x2D043C with a rectangle at sp+0x74 (x0,y0,x1,y1 = -1,-1,1,1) and texture
 * coordinates at sp+0x84 (u0,v0,u1,v1), v0/v1 = 0.5 -/+ 0.5*r with
 * r = 16H/(9W) (0x2E7F7C..0x2E802C). At W > 16H/9 that keeps the screen
 * full and crops the movie top and bottom (W'=1707: v 0.125..0.875, a
 * quarter of the picture gone). Fit instead: x -r..r, v 0..1 -- the whole
 * movie at its 16:9 shape in the middle of the wide screen. At 16:9 r is 1
 * and nothing changes; for a narrower screen (r > 1) the stock letterbox
 * stays.
 *
 * Called by the lifted code just before that draw: tools/ppu_postlift.py
 * inserts the call into generated/ppu (tools/relift.sh runs it after every
 * PPU lift). It reads the values the game just stored, so it does nothing
 * unless they are exactly that layout.
 * -----------------------------------------------------------------------*/
extern "C" void if2_fmv_quad(ppu_context* ctx)
{
    if (!g_fmv_fit) return;
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    float q[8];
    for (int i = 0; i < 8; i++) q[i] = u2f(vm_read32(sp + 0x74 + 4u * (uint32_t)i));
    /* x0 y0 x1 y1 u0 v0 u1 v1 */
    if (q[0] != -1.0f || q[1] != -1.0f || q[2] != 1.0f || q[3] != 1.0f || q[4] != 0.0f || q[6] != 1.0f)
        return;
    const float r = q[7] - q[5];
    if (!(r > 0.05f && r < 0.999f)) return;
    vm_write32(sp + 0x74, f2u(-r));    /* x0 */
    vm_write32(sp + 0x7C, f2u(r));     /* x1 */
    vm_write32(sp + 0x88, f2u(0.0f));  /* v0 */
    vm_write32(sp + 0x90, f2u(1.0f));  /* v1 */
    static int said = 0;
    if (!said++) {
        fprintf(stderr, "[if2-fmv] movie quad fitted: x -%.4f..%.4f, v 0..1 (was v %.4f..%.4f)\n",
                (double)r, (double)r, (double)q[5], (double)q[7]);
        fflush(stderr);
    }
}

/* ---------------------------------------------------------------------------
 * IF2_PHYSFB=1 (diagnostic, read-only): why the physics step runs its PPU
 * narrowphase for a body instead of using the SPU job's contacts.
 *
 * The step (0x4796E0, called from the main loop) submits one narrowphase job
 * per moving body (jobmod_e094d9aca2b5_ef1f0, image 223; command built at
 * 0x47B908) and reads back the job's 16-byte result header (+8 status byte,
 * +0xA record count, +0xC records). It runs the same narrowphase on the PPU
 * instead (0x47D438, the bl at 0x479D5C) when
 *   hdr == 0  the PPU could not take that header from the per-step result
 *             ring (mgr+0x40154 base, +0x40158 cursor, +0x4015C limit; the
 *             lwarx/stwcx at 0x47A420), so no job was submitted for the body;
 *   st != 0   the job asked for it: 4 = its records did not fit the ring (its
 *             atomic add on the same cursor, job LS 0xD808 -> 0x13338),
 *             1 = the query helper at LS 0x10178 returned < 0 (set at LS
 *             0xDCA8), anything else = a collider's return code (LS 0xDC14).
 * The ring is reset every step (0x47A0F4) and is shared by the headers and
 * every job's records, so a burst of debris -- many small bodies in contact
 * -- is expected to overflow it (static analysis; this log is the run-time
 * check); the fallback is then the game's own overflow path. It is also the
 * only way into the mesh contact code at 0x47618C, whose mis-lifted
 * `ld r31,0x448(r1)` (ps3recomp ppu_lifter callee-save snapshot) built
 * contacts on the physics manager object 0x00A332C0 and hung the game (see
 * if2_physfb_side_a below).
 *
 * tools/ppu_postlift.py calls this right before that bl: mgr = r24, the body
 * = r31, the body's 12-byte work item = *(r1+0xD0) + r20 (+4 body, +8
 * header). Prints the first 32 fallbacks, then every 256th, with running
 * totals per reason ("st0" = forced by IF2_PHYSFB_FORCE below).
 *
 * IF2_PHYSFB_FORCE=1 (test only): if2_physfb_force, called where the step has
 * just loaded the work item's header into r0 (before 0x479D00), zeroes it, so
 * every body takes the fallback -- the game's own path for a full ring; the
 * submitted job's results are simply not read. That puts the mesh contact
 * code above on every touching pair of convex bodies, in any scene, instead
 * of only during a debris burst: a quick A/B for the lifter fix.
 * -----------------------------------------------------------------------*/
static int if2_env_on(const char* name)
{
    const char* e = getenv(name);
    return (e && *e && *e != '0') ? 1 : 0;
}

extern "C" void if2_physfb_force(ppu_context* ctx)
{
    static int s_on = -1;
    if (s_on < 0) {
        s_on = if2_env_on("IF2_PHYSFB_FORCE");
        if (s_on) {
            fprintf(stderr, "[if2-physfb] IF2_PHYSFB_FORCE: every body takes the PPU narrowphase\n");
            fflush(stderr);
        }
    }
    if (s_on) ctx->gpr[0] = 0;   /* the header the step tests next */
}

extern "C" void if2_physfb_diag(ppu_context* ctx)
{
    static int s_on = -1;
    if (s_on < 0) s_on = if2_env_on("IF2_PHYSFB");
    if (!s_on) return;
    /* The physics step runs on the main thread only: plain counters do. */
    static unsigned long s_n, s_nohdr, s_ovf, s_neg, s_forced, s_other;
    const uint32_t mgr  = (uint32_t)ctx->gpr[24];
    const uint32_t body = (uint32_t)ctx->gpr[31];
    const uint32_t item = vm_read32((uint32_t)ctx->gpr[1] + 0xD0u) + (uint32_t)ctx->gpr[20];
    const uint32_t hdr  = vm_read32(item + 8u);
    const uint32_t w2   = hdr ? vm_read32(hdr + 8u) : 0u;   /* status byte, pad, count */
    const uint32_t st = w2 >> 24, cnt = w2 & 0xFFFFu;
    ++s_n;
    if (!hdr) ++s_nohdr;
    else if (st == 4) ++s_ovf;
    else if (st == 1) ++s_neg;
    else if (st == 0) ++s_forced;
    else ++s_other;
    if (s_n > 32 && (s_n & 255u)) return;
    const uint32_t rb = vm_read32(mgr + 0x40154u), rc = vm_read32(mgr + 0x40158u),
                   rl = vm_read32(mgr + 0x4015Cu);
    fprintf(stderr, "[if2-physfb] #%lu body=0x%08X hdr=0x%08X st=%u cnt=%u ring 0x%X of 0x%X "
            "(base 0x%08X cur 0x%08X lim 0x%08X) | no-hdr %lu, st4 %lu, st1 %lu, st0 %lu, other %lu\n",
            s_n, body, hdr, st, cnt, rc - rb, rl - rb, rb, rc, rl,
            s_nohdr, s_ovf, s_neg, s_forced, s_other);
    fflush(stderr);
}

/* IF2_PHYSFB=1, the direct check of the lifter fix: called in func_004761F4
 * (the body of the mesh contact generator 0x47618C, reached only from the PPU
 * narrowphase above) right after `ld r31,0x448(r1)` (EBOOT 0x4761FC), which
 * reloads side A's collision primitive that the prologue chunk spilled there
 * (`std r4,0x448(r1)` at 0x476194). r31 must equal that slot. The old lift
 * made the load `r31 = _cs_31` -- the CALLER's r31, the physics manager
 * 0x00A332C0 (shape word +0xC = 0) -- so a build without the ps3recomp lifter
 * fix prints MISLIFT here on the first convex pair the PPU narrowphase meets,
 * long before the NULL reads and the hang ~27 s later. Prints the first 16
 * entries, every MISLIFT (up to 64), then every 4096th entry. */
extern "C" void if2_physfb_side_a(ppu_context* ctx)
{
    static int s_on = -1;
    if (s_on < 0) s_on = if2_env_on("IF2_PHYSFB");
    if (!s_on) return;
    static std::atomic<unsigned long> s_n, s_bad;   /* zero-initialised (static) */
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    const uint32_t slot_hi = vm_read32(sp + 0x448u), slot = vm_read32(sp + 0x44Cu);
    const uint32_t a_hi = (uint32_t)(ctx->gpr[31] >> 32), a = (uint32_t)ctx->gpr[31];
    const bool bad = a != slot || a_hi != slot_hi;
    const unsigned long n = s_n.fetch_add(1) + 1;
    const unsigned long nb = bad ? s_bad.fetch_add(1) + 1 : s_bad.load();
    if (!(n <= 16 || (bad && nb <= 64) || (n & 4095u) == 0)) return;
    const uint32_t shape = a ? vm_read32(a + 0xCu) : 0u;
    fprintf(stderr, "[if2-physfb] mesh contacts #%lu: side A r31=0x%08X, slot r1+0x448=0x%08X, "
            "shape 0x%08X -- %s (MISLIFT total %lu)\n", n, a, slot, shape,
            bad ? "MISLIFT (r31 is not the spilled argument)" : "ok", nb);
    fflush(stderr);
}

/* Test starts (both opt-in, inert unless their variables are set):
 *   src/if2_checkpoint.cpp -- boot from a stored save (CHECKPOINT), drive the
 *     title flow on game events to READY, keep the game's saves as checkpoints
 *     (CHECKPOINT_AUTOSNAP). Wraps the title's four cellSaveData imports, and
 *     cellPadGetData while it drives input.
 *   src/if2_taskgraph.cpp -- watch the game's task graph (IF2_TASKLOG) and jump
 *     a mission to one of its steps (IF2_TASKJUMP): the prologue, which never
 *     saves. Wraps cellSysutilCheckCallback. */
extern "C" int if2_checkpoint_install(void);
extern "C" int if2_taskgraph_install(void);
extern "C" int if2_clock_install(void);   /* src/if2_clock.cpp: IF2_CLOCKLOG, the game's frame clock */
extern "C" int if2_waitev_install(void);  /* src/if2_waitev.cpp: WAIT_EVENTS poll-loop sites */
extern "C" int if2_menu_install(void);    /* src/if2_menu.cpp: native entries in the game's menus (NATIVE_MENU) */
extern "C" int if2_achievements_install(void);   /* src/if2_achievements.cpp: trophies -> RecompLauncher */

/* ---------------------------------------------------------------------------
 * The online step after a user-made mission, without a connection (GitHub #21).
 *
 * After a user-made (UGC) mission the game's UGC manager starts an online step
 * by itself: the network start dialog, two PSN account flags, the PSN terms
 * check, then HTTP to the UGC server -- which no longer exists. Before this
 * the flags and the terms check were not implemented and "Please wait..."
 * stayed up forever; answered, the step ends in "Communication error" (the
 * server). The manager's idle state starts that step only while its online
 * mode (G+0x8F8) is still undecided (-1); with the mode "offline" (0) it takes
 * its offline state instead, which needs no network and shows nothing.
 * if2_ugc_offline / if2_ugc_offline2 (called by the lifted idle state right
 * before it starts the step, from either of its two ways in: tools/ppu_postlift.py)
 * turn that start into the offline state. Every online moment of the game goes
 * through this step: its only online features are the UGC server's (login,
 * ratings, playlists, uploads, reports), and a later request to go online
 * (the scripts' TRY_ENABLE_ONLINE) only puts the mode back to undecided.
 * PS3_NET_ONLINE=1 leaves the game's own choice.
 *
 * The PSN answers below are for the online step when a player starts it from
 * a menu: an unrestricted account that has accepted the terms, so it reaches
 * the server question instead of waiting forever.
 * -----------------------------------------------------------------------*/
extern "C" void if2_ugc_offline(ppu_context* ctx)
{
    /* r4 = the state the idle state is about to enter: the online step's first state, 7 in the 1.04 EBOOT
     * and 5 in the disc one; their offline states are 21 and 18 */
    static int s_on = -1, s_said;
    if (s_on < 0) { const char* e = getenv("IF2_UGC_OFFLINE"); s_on = getenv("PS3_NET_ONLINE") || (e && *e == '0') ? 0 : 1; }
    if (!s_on) return;
    const uint32_t to = (uint32_t)ctx->gpr[4] == 7u ? 21u : (uint32_t)ctx->gpr[4] == 5u ? 18u : 0u;
    if (!to) return;
    if (s_said++ < 8) fprintf(stderr, "[infamous2] UGC: the game's online step is skipped (offline; #21)\n");
    ctx->gpr[4] = to;
}
/* the idle state's other way into the online step (the online mode already on): the same */
extern "C" void if2_ugc_offline2(ppu_context* ctx) { if2_ugc_offline(ctx); }

extern "C" int32_t cellSysutilQueueGuestCallbackArgs(uint32_t opd, const uint64_t args[8]);   /* libs/system */
static const uint32_t kNpEulaAlreadyAccepted = 2;   /* SceNpEulaStatus: SCE_NP_EULA_ALREADY_ACCEPTED */

/* sceNpManagerGetContentRatingFlag(s32* isRestricted, s32* age) */
static void if2_NpContentRatingFlag(ppu_context* ctx)
{
    const uint32_t restricted = (uint32_t)ctx->gpr[3], age = (uint32_t)ctx->gpr[4];
    if (restricted) vm_write32(restricted, 0);
    if (age) vm_write32(age, 18);
    ctx->gpr[3] = 0;
}
/* sceNpManagerGetChatRestrictionFlag(s32* isRestricted) */
static void if2_NpChatRestrictionFlag(ppu_context* ctx)
{
    const uint32_t restricted = (uint32_t)ctx->gpr[3];
    if (restricted) vm_write32(restricted, 0);
    ctx->gpr[3] = 0;
}
/* sceNpEulaCheckEulaStatus(commId, arg2, arg3, callback, cbArg): callback(status, errorCode, cbArg) */
static void if2_NpEulaCheckEulaStatus(ppu_context* ctx)
{
    const uint32_t cb = (uint32_t)ctx->gpr[6], arg = (uint32_t)ctx->gpr[7];
    const uint64_t args[8] = { kNpEulaAlreadyAccepted, 0, arg, 0, 0, 0, 0, 0 };
    if (cb) cellSysutilQueueGuestCallbackArgs(cb, args);
    fprintf(stderr, "[infamous2] PSN terms check: already accepted (no connection needed; #21)\n");
    ctx->gpr[3] = 0;
}

static int if2_psn_bypass_install(void)
{
    IF2_OVERRIDE("sceNpManagerGetContentRatingFlag", if2_NpContentRatingFlag);
    IF2_OVERRIDE("sceNpManagerGetChatRestrictionFlag", if2_NpChatRestrictionFlag);
    IF2_OVERRIDE("sceNpEulaCheckEulaStatus", if2_NpEulaCheckEulaStatus);
    return 3;
}

extern "C" void infamous2_install_overrides(void)
{
    int n = 0;
    n += if2_psn_bypass_install();   /* #21: the PSN checks after a UGC mission pass offline */
    /* What this build was made from (tools/if2_resolve.py), and what is off because it was not found */
    fprintf(stderr, "[if2] EBOOT %s (SHA-1 %.12s..., title %s, %s)%s%s\n", IF2_EBOOT_NAME, IF2_EBOOT_SHA1,
            IF2_TITLE_ID, IF2_EBOOT_KNOWN ? "every title address cross-checked" : "title addresses found by pattern: BETA",
            IF2_FEATURES_OFF[0] ? "; off on this EBOOT: " : "", IF2_FEATURES_OFF);

    /* Overrides go here as bring-up finds them, e.g.
     *   IF2_OVERRIDE("cellGemGetStatus", if2_cellGemGetStatus); n++;
     */
    n += if2_options_install();  /* settings store: this title's rows, the [options] summary */
    if2_language_edition_apply();   /* language_edition = europe: the game's European language list */
    n += if2_aspect_install();   /* ultrawide: cellGcmGetConfiguration */
    if2_options_watch();         /* live lod / fmv / detail */
    if2_postfx_install();        /* anti-aliasing / sharpening: the scene-end and movie programs */
    n += if2_checkpoint_install();   /* CHECKPOINT*, IF2_SIGLOG: save-data imports, cellPadGetData */
    n += if2_taskgraph_install();    /* IF2_TASKLOG, IF2_TASKJUMP: cellSysutilCheckCallback */
    n += if2_clock_install();        /* IF2_CLOCKLOG: game frame rate / dt / game-time speed; IF2_DTCLAMP */
    n += if2_waitev_install();       /* WAIT_EVENTS: event-driven kicker / WWS barrier waits (M6a) */
    n += if2_menu_install();         /* NATIVE_MENU: Quit Game in the pause menu (the menus' input handler) */
    n += if2_achievements_install(); /* achievements = 1: trophy unlocks to RecompLauncher (RL_SESSION) */

    if (getenv("IF2_VERBOSE"))
        fprintf(stderr, "[infamous2] %d title override(s) installed\n", n);
}

/* ---------------------------------------------------------------------------
 * Save states: the TITL section (runtime/savestate/ss_state.h; engine design
 * 5.2, 5.6, 6.2) -- the display values this file applied.
 *
 * The values themselves are guest memory (W, the safe-area X, the texel
 * slots, the LOD thresholds: GMEM has them). What a load needs besides is
 * how they were made, so that it adopts the snapshot's shape (design 5.6:
 * the content aspect set from it, these values taken over, the player told)
 * instead of deriving them again from this session's settings:
 *   title  the hook's state: the effective aspect (0 = stock, nothing
 *          applied), hud, lod, texel fix, fmv fit, applied, the detail
 *          distance, the wide factor k, and the LOD constants it last wrote
 *          (g_lod_cur: if2_lod_apply compares guest memory with them, so
 *          they must match the restored memory, or every constant reads as
 *          someone else's and a live detail change stops working)
 *   guest  the same values as guest memory holds them at capture -- H, W,
 *          safe X, render width, the four pool slots, each LOD constant --
 *          for inspection and the adopt decision; never written back
 * Captured on the frame-clock thread with every guest thread parked; that
 * thread is also the window thread that applies a live lod / detail change,
 * so nothing else writes these meanwhile. Guest memory is read straight from
 * vm_base: vm_read32's slow path can log. The diagnostics (said, s_on, the
 * physfb counters) and the option ids are not state.
 *
 * A load (M4c) ADOPTS the snapshot's shape (design 5.6): its aspect, HUD,
 * texel fix, wide factor, the applied flag and the LOD constants it left
 * are what the restored guest memory was made with, so they are taken over,
 * the presented aspect follows its W/H, and the player is told when that is
 * not this session's shape (it stays until a restart: the cameras in memory
 * have it). This session's LIVE settings -- lod, detail, fmv -- are kept and
 * applied to the adopted state (if2_lod_apply writes the constants only
 * where they differ); they are host-flagged, the fidelity check skips them.
 * The other title patch in guest memory, IF2_DTCLAMP's dt clamp (critique
 * H11), is recorded (guest.dt_clamp) and set to this run's IF2_DTCLAMP: it
 * is a run setting, nothing is derived from it. The title's host hooks get
 * their part too: WAIT_EVENTS' watches cleared at the teardown
 * (src/if2_waitev.cpp), and when a load put back ANOTHER moment than the
 * stop's (not a round trip), the checkpoint harness counts READY as reached
 * and drops its autosnap baseline (src/if2_checkpoint.cpp).
 * -----------------------------------------------------------------------*/
extern "C" uint8_t* vm_base;
extern "C" uint64_t ppu_timebase_now(void);                    /* runtime/syscalls/sys_timer.c */
extern "C" void     ss_ui_notify(unsigned ms, const char* fmt, ...);   /* runtime/savestate/ss_ui.c */
extern "C" void     if2_waitev_ss_reset(void);                 /* src/if2_waitev.cpp */
extern "C" void     if2_checkpoint_ss_loaded(void);            /* src/if2_checkpoint.cpp */

namespace {

/* IF2_DTCLAMP's word and the one beside it that proves this EBOOT (if2_clock.cpp) */
constexpr uint32_t kClamp15 = IF2_R(CLAMP15);   /* 1/15 */
constexpr uint32_t kClamp30 = IF2_R(CLAMP30);   /* 1/30, or 1/<IF2_DTCLAMP> */

struct TitlGuest {
    uint32_t h, w, safe_x, render_w;
    uint32_t slot_noise, slot_texel, slot_fmv, slot_cam;
    uint32_t lod[kNLodA];
    uint32_t dt_clamp;
};

const ss_field kTitlFields[] = {
    SS_FIELD(g_aspect), SS_FIELD(g_hud_169), SS_FIELD_F(g_lod, SS_FF_HOST), SS_FIELD(g_texelfix),
    SS_FIELD_F(g_fmv_fit, SS_FF_HOST), SS_FIELD(g_applied), SS_FIELD_F(g_detail, SS_FF_HOST), SS_FIELD(g_k_wide),
    SS_FIELD(g_lod_cur), SS_FIELD(g_lod_init),
};
const ss_list kTitlList = SS_LIST("title", kTitlFields);
/* The same record read into a struct (a load decides what it adopts). */
struct TitlState {
    double   g_aspect;
    int      g_hud_169, g_lod, g_texelfix, g_fmv_fit, g_applied;
    double   g_detail, g_k_wide;
    uint32_t g_lod_cur[kNLodA];
    bool     g_lod_init;
};
static_assert(sizeof(TitlState::g_aspect) == sizeof g_aspect && sizeof(TitlState::g_lod) == sizeof g_lod &&
              sizeof(TitlState::g_detail) == sizeof g_detail && sizeof(TitlState::g_lod_cur) == sizeof g_lod_cur &&
              sizeof(TitlState::g_lod_init) == sizeof g_lod_init, "TitlState mirrors kTitlFields");
const ss_field kTitlStateFields[] = {
    SS_MEMBER(TitlState, g_aspect), SS_MEMBER(TitlState, g_hud_169), SS_MEMBER(TitlState, g_lod),
    SS_MEMBER(TitlState, g_texelfix), SS_MEMBER(TitlState, g_fmv_fit), SS_MEMBER(TitlState, g_applied),
    SS_MEMBER(TitlState, g_detail), SS_MEMBER(TitlState, g_k_wide), SS_MEMBER(TitlState, g_lod_cur),
    SS_MEMBER(TitlState, g_lod_init),
};
const ss_list kTitlStateList = SS_LIST("title", kTitlStateFields);
const ss_field kTitlGuestFields[] = {
    SS_MEMBER_F(TitlGuest, h, SS_FF_NO_RESTORE), SS_MEMBER_F(TitlGuest, w, SS_FF_NO_RESTORE),
    SS_MEMBER_F(TitlGuest, safe_x, SS_FF_NO_RESTORE), SS_MEMBER_F(TitlGuest, render_w, SS_FF_NO_RESTORE),
    SS_MEMBER_F(TitlGuest, slot_noise, SS_FF_NO_RESTORE), SS_MEMBER_F(TitlGuest, slot_texel, SS_FF_NO_RESTORE),
    SS_MEMBER_F(TitlGuest, slot_fmv, SS_FF_NO_RESTORE), SS_MEMBER_F(TitlGuest, slot_cam, SS_FF_NO_RESTORE),
    SS_MEMBER_F(TitlGuest, lod, SS_FF_NO_RESTORE), SS_MEMBER_F(TitlGuest, dt_clamp, SS_FF_NO_RESTORE),
};
const ss_list kTitlGuestList = SS_LIST("guest", kTitlGuestFields);
const ss_list* const kTitlLists[] = { &kTitlList, &kTitlGuestList, nullptr };

uint32_t titl_guest32(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}

int titl_capture(ss_writer* w)
{
    TitlGuest g;
    memset(&g, 0, sizeof g);
    if (vm_base && IF2_HAVE(ASPECT)) {
        g.h = titl_guest32(kH);
        g.w = titl_guest32(kW);
        g.safe_x = titl_guest32(kSafeX);
        g.render_w = titl_guest32(kRenderW);
        g.slot_noise = titl_guest32(kSlotNoise);
        g.slot_texel = titl_guest32(kSlotTexel);
        g.slot_fmv = titl_guest32(kSlotFmv);
        g.slot_cam = titl_guest32(kSlotCam);
    }
    if (vm_base) {
        for (int i = 0; i < kNLod; i++) g.lod[i] = titl_guest32(kLod[i].ea);
        if (IF2_HAVE(DTCLAMP)) g.dt_clamp = titl_guest32(kClamp30);
    }
    ss_write_fields(w, &kTitlList);
    ss_write_fields_at(w, &kTitlGuestList, &g);
    return ss_writer_error(w);
}

/* ---- the load ---- */
struct TitlSession { bool valid; double aspect; int texelfix; };
TitlSession g_ss_session;       /* this session's own shape, before a load adopted a state's */
uint64_t    g_ss_tb_stop;       /* the guest time base at the load's stop (teardown) */
bool        g_ss_other_moment;  /* the load put back another moment than the stop's */
bool        g_ss_shape_note;    /* ... of another shape than this session's: told at the release */

/* A shape the hook can have made: stock (0), or an aspect install accepts
 * (4:3..32:9), with a wide factor to match. */
bool titl_state_ok(const TitlState& s)
{
    const bool aspect_ok = s.g_aspect == 0.0 || (s.g_aspect >= 4.0 / 3.0 - 0.01 && s.g_aspect <= 32.0 / 9.0 + 0.01);
    return aspect_ok && s.g_k_wide > 0.0 && s.g_k_wide < 4.0;
}

/* "16:9" for the stock shape, else the ratio */
const char* titl_aspect_name(double a, char* b, size_t n)
{
    if (a <= 0.0) snprintf(b, n, "16:9");
    else snprintf(b, n, "%.2f:1", a);
    return b;
}

int titl_check_image(ss_reader* r)
{
    TitlState s;
    memset(&s, 0, sizeof s);
    const int e = ss_read_fields_at(r, &kTitlStateList, &s);
    if (e) return e;
    if (!titl_state_ok(s))
        return ss_reader_fail(r, SS_E_CORRUPT, "title.g_aspect %g / g_k_wide %g: not a shape this hook makes",
                              s.g_aspect, s.g_k_wide);
    return SS_OK;
}

/* Every guest thread gone, the guest clock frozen at the stop: this
 * session's own shape kept the first time (later loads may have adopted
 * others), the stop's instant noted, WAIT_EVENTS' watches cleared. */
void titl_teardown(void)
{
    if (!g_ss_session.valid) {
        g_ss_session.aspect = g_aspect;
        g_ss_session.texelfix = g_texelfix;
        g_ss_session.valid = true;
    }
    g_ss_tb_stop = ppu_timebase_now();
    if2_waitev_ss_reset();
}

int titl_restore(ss_reader* r)
{
    TitlState s;
    memset(&s, 0, sizeof s);
    const int e = ss_read_fields_at(r, &kTitlStateList, &s);
    if (e) return e;
    if (!titl_state_ok(s))
        return ss_reader_fail(r, SS_E_CORRUPT, "title.g_aspect %g / g_k_wide %g: not a shape this hook makes",
                              s.g_aspect, s.g_k_wide);
    /* The guest clock now reads the snapshot's instant (TIME restored it):
     * a round trip puts back the stop's own (within the time base's
     * rounding), any other load another moment. 1 ms of time base. */
    const uint64_t tb = ppu_timebase_now();
    g_ss_other_moment = (tb > g_ss_tb_stop ? tb - g_ss_tb_stop : g_ss_tb_stop - tb) > 79800u;

    /* adopt the snapshot's shape (design 5.6) */
    const double before = g_aspect;
    const int fmv_choice = before > 0.0 ? g_fmv_fit : (g_opt_fmv >= 0 ? opt_i(g_opt_fmv) == 0 : 1);
    g_aspect = s.g_aspect;
    g_hud_169 = s.g_hud_169;
    g_texelfix = s.g_texelfix;
    g_applied = s.g_applied;
    g_k_wide = s.g_k_wide;
    memcpy(g_lod_cur, s.g_lod_cur, sizeof g_lod_cur);
    g_lod_init = s.g_lod_init;
    g_fmv_fit = g_aspect > 0.0 && fmv_choice;   /* this session's choice, on the adopted shape */
    if (std::fabs(g_aspect - before) > 1e-9 && IF2_HAVE(ASPECT)) {
        uint32_t H = titl_guest32(kH);
        const uint32_t W = titl_guest32(kW);
        if (H < 240 || H > 2160) H = 720;
        rsx_display_set_content_aspect(g_aspect > 0.0 && W ? (double)W / (double)H : 0.0);
    }
    g_ss_shape_note = g_ss_session.valid && std::fabs(g_aspect - g_ss_session.aspect) > 0.002;
    if (g_ss_shape_note && g_applied && g_aspect > 0.0 && g_texelfix != g_ss_session.texelfix)
        fprintf(stderr, "[if2-aspect] save state: texel slots as the saving run left them (IF2_TEXELFIX=%d "
                        "there, %d here)\n", g_texelfix, g_ss_session.texelfix);

    /* this session's lod / detail on the adopted constants (written only
     * where they differ: nothing on a round trip) */
    const double k = (g_lod ? g_k_wide : 1.0) * g_detail;
    if (g_lod_init || k != 1.0) if2_lod_apply(false);

    /* IF2_DTCLAMP: this run's, whatever the saving run's was */
    if (IF2_HAVE(DTCLAMP) && titl_guest32(kClamp15) == 0x3D888889u) {
        const char* dc = getenv("IF2_DTCLAMP");
        const int hz = (dc && *dc) ? atoi(dc) : 0;
        const uint32_t want = (hz >= 10 && hz <= 30) ? f2u(1.0f / (float)hz) : 0x3D088889u;
        const uint32_t have = titl_guest32(kClamp30);
        if (have != want) {
            vm_write32(kClamp30, want);
            fprintf(stderr, "[if2-clock] save state: max frame dt %.2f ms in the loaded state, this run's %.2f ms "
                            "(IF2_DTCLAMP) set\n", 1000.0 * (double)u2f(have), 1000.0 * (double)u2f(want));
        }
    }
    fflush(stderr);
    return SS_OK;
}

/* The world runs again: the player told of an adopted shape; the checkpoint
 * harness told of another moment. */
void titl_after_release(void)
{
    if (g_ss_shape_note) {
        char a[32];
        titl_aspect_name(g_aspect, a, sizeof a);
        ss_ui_notify(6000, "This state was saved at %s: the picture stays %s until you restart", a, a);
        g_ss_shape_note = false;
    }
    if (g_ss_other_moment) {
        if2_checkpoint_ss_loaded();
        g_ss_other_moment = false;
    }
}

const ss_section_ops kTitlOps = {
    SS_FOURCC('T', 'I', 'T', 'L'), 1, 0, SS_ORDER_TITLE,
    nullptr, titl_capture, titl_teardown, titl_restore, titl_after_release,
    "inFamous 2 display hooks", kTitlLists, nullptr,
    nullptr, titl_check_image,
};

}  /* namespace */
SS_SECTION_REGISTER(kTitlOps)

/* The display configuration in force, for a save-state slot's header
 * (src/if2_ss_title.cpp, engine design 5.6): the aspect (0 = stock 16:9),
 * the HUD kept at 16:9, the LOD factor on, full-screen movies fitted. */
extern "C" void if2_display_config(double* aspect, uint8_t* hud, uint8_t* lod, uint8_t* fmv)
{
    if (aspect) *aspect = g_aspect;
    if (hud) *hud = (uint8_t)(g_hud_169 != 0);
    if (lod) *lod = (uint8_t)(g_lod != 0);
    if (fmv) *fmv = (uint8_t)(g_fmv_fit != 0);
}

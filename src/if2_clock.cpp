/*
 * inFamous 2 -- the game's own frame clock, observed (IF2_CLOCKLOG=1).
 *
 * The title times every frame itself. func_00085014, called once per
 * main-loop iteration (0x1DB888 in the loop at 0x1DB544), reads mftb and
 * stores, in the clock object at 0x9BFC70:
 *
 *   +0x00  dt = (mftb - stamp) / timebase, clamped to [0, 1/30 s]
 *          (1/15 s when the byte at 0xA115D6 is set; it is never written)
 *   +0x04 / +0x14 / +0x24  three time domains' integer 1440 Hz tick counters
 *   +0x08 / +0x18 / +0x28  each domain's dt = dt * rate (rates at +0x10/+0x20/+0x30);
 *          the remainder below one tick is carried (+0x0C/+0x1C/+0x2C)
 *   +0xE0  the mftb stamp of this update (u64)
 *
 * Domain 0 (the game clock: 130+ readers of +0x08, 600+ of +0x04) is only
 * advanced when the call's second argument is 0, which the main loop always
 * passes. The main loop's iteration counter is the word at 0xA08F90.
 *
 * Nothing in the title sets a minimum dt or steps the world at a fixed rate
 * from this clock, so above 30 fps game time is real time and below 30 fps it
 * runs slow by the clamp (each frame longer than 1/30 s only advances 1/30 s).
 *
 * This file only OBSERVES it: a host thread polls the stamp every ~0.5 ms;
 * every new stamp is one game frame, with its real length (stamp delta,
 * unclamped) and the dt the game used (clamped). Once a second:
 *
 *   [if2-clock] t=<s> loops=<n> flips=<n> pres=<n> ft avg/p50/p99/max=<ms>
 *               clamped=<n> lost=<ms> game/real d0=<r> d1=<r> d2=<r> [rates a/b/c]
 *
 * loops = game main-loop iterations (the real game frame rate, which the
 * [fps] line -- presents, capped by the display's vsync -- does not show);
 * flips = the guest's flip requests (cellGcm_flip_request_count; one per loop
 * iteration while the title renders); pres = present_guest_frame() calls, the
 * unit of the [if2-sig] f= field and of CHECKPOINT_SCRIPT / EXIT_AFTER -- more
 * than flips (main.cpp's "fc != last_flip" branch does not consume the pending
 * flag, so many flips are counted twice); clamped = frames longer than the
 * clamp; lost = the game time those frames did not advance; game/real =
 * domain ticks / 1440 per real second (1.000 = correct speed).
 *
 * The thread checks that 0x93ACE0/0x93ACE8 hold the clamp constants (1/15,
 * 1/30) of this EBOOT (BCUS98125 v02.00) and stays off otherwise.
 * IF2_CLOCKLOG=2 also prints every frame ([if2-frame] real/dt ms).
 *
 * IF2_DTCLAMP=<hz> (needs no IF2_CLOCKLOG): the one game-side change here.
 * Writes 1/<hz> over the 1/30 constant at 0x93ACE8, which only the clamp at
 * 0x8522C reads, so a frame up to 1/<hz> s long advances game time by its real
 * length instead of 1/30 s (the title's own alternative, the 0xA115D6 switch,
 * selects 1/15). 15 = what that switch gives; 10..30 accepted; default off.
 * For sub-30 fps stretches (hitches, today's gate) only; at >= 30 fps nothing
 * changes.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "if2_version.h"

extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void     vm_write32(uint64_t addr, uint32_t val);
extern "C" unsigned ppu_boot_frames_presented(void);   /* src/main.cpp */
extern "C" uint32_t cellGcm_flip_request_count(void);  /* libs/video/cellGcmSys.c */

namespace {

constexpr uint32_t kClock     = IF2_R(CLOCK_OBJ);    /* the clock object (disc 0x9BFC70) */
constexpr uint32_t kStampLo   = kClock + 0xE4u; /* low word of the u64 stamp at +0xE0 */
constexpr uint32_t kLoopCount = IF2_R(LOOP_COUNT);   /* main-loop iterations (disc 0xA08F90) */
constexpr uint32_t kClamp15   = IF2_R(CLAMP15);      /* 1/15 (disc 0x93ACE0) */
constexpr uint32_t kClamp30   = IF2_R(CLAMP30);      /* 1/30 (disc 0x93ACE8) */
constexpr double   kTimebase  = 79800000.0;

float f32(uint32_t ea) { const uint32_t u = vm_read32(ea); float f; memcpy(&f, &u, 4); return f; }

int g_mode = 0;

DWORD WINAPI clock_thread(LPVOID)
{
    HANDLE tmr = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
    if (!tmr) tmr = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER qf, q0, qn;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    const LONGLONG t_start = q0.QuadPart;

    /* wait for the game to start its clock: a nonzero stamp */
    uint32_t prev = 0;
    for (;;) {
        Sleep(50);
        prev = vm_read32(kStampLo);
        if (prev) break;
    }
    uint32_t t0[3], loops0 = vm_read32(kLoopCount);
    for (int d = 0; d < 3; d++) t0[d] = vm_read32(kClock + 4u + 0x10u * (uint32_t)d);
    unsigned flips0 = cellGcm_flip_request_count(), pres0 = ppu_boot_frames_presented();
    QueryPerformanceCounter(&q0);

    std::vector<float> ft;      /* real frame lengths this second, ms */
    ft.reserve(1024);
    unsigned clamped = 0;
    double lost_ms = 0.0;

    for (;;) {
        if (tmr) {
            LARGE_INTEGER due; due.QuadPart = -5000;      /* 0.5 ms */
            SetWaitableTimer(tmr, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(tmr, 5);
        } else {
            Sleep(1);
        }
        const uint32_t lo = vm_read32(kStampLo);
        if (lo != prev) {
            /* The update stores dt (+0x00) before the stamp (+0xE0); x86
             * stores are seen in order, so dt belongs to this stamp. */
            const double real_ms = (double)(uint32_t)(lo - prev) * 1000.0 / kTimebase;
            const double dt_ms = (double)f32(kClock) * 1000.0;
            prev = lo;
            ft.push_back((float)real_ms);
            if (real_ms > dt_ms + 0.05) { clamped++; lost_ms += real_ms - dt_ms; }
            if (g_mode >= 2)
                fprintf(stderr, "[if2-frame] real=%.3f dt=%.3f ms%c", real_ms, dt_ms, 10);
        }
        QueryPerformanceCounter(&qn);
        const double el = (double)(qn.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
        if (el < 1.0) continue;

        uint32_t t1[3];
        for (int d = 0; d < 3; d++) t1[d] = vm_read32(kClock + 4u + 0x10u * (uint32_t)d);
        const uint32_t loops1 = vm_read32(kLoopCount);
        const unsigned flips1 = cellGcm_flip_request_count(), pres1 = ppu_boot_frames_presented();
        double avg = 0.0, p50 = 0.0, p99 = 0.0, mx = 0.0;
        if (!ft.empty()) {
            for (float v : ft) avg += v;
            avg /= (double)ft.size();
            std::sort(ft.begin(), ft.end());
            p50 = ft[ft.size() / 2];
            p99 = ft[std::min(ft.size() - 1, (size_t)((double)ft.size() * 0.99))];
            mx  = ft.back();
        }
        double r[3];
        for (int d = 0; d < 3; d++) r[d] = (double)(int32_t)(t1[d] - t0[d]) / 1440.0 / el;
        const float ra = f32(kClock + 0x10u), rb = f32(kClock + 0x20u), rc = f32(kClock + 0x30u);
        char rates[64] = "";
        if (ra != 1.0f || rb != 1.0f || rc != 1.0f)
            snprintf(rates, sizeof rates, " rates %.3f/%.3f/%.3f", ra, rb, rc);
        fprintf(stderr, "[if2-clock] t=%.0fs loops=%u flips=%u pres=%u ft avg/p50/p99/max=%.2f/%.2f/%.2f/%.2f ms"
                        " clamped=%u lost=%.0fms game/real d0=%.3f d1=%.3f d2=%.3f%s%c",
                (double)(qn.QuadPart - t_start) / (double)qf.QuadPart,
                (unsigned)(loops1 - loops0), flips1 - flips0, pres1 - pres0, avg, p50, p99, mx,
                clamped, lost_ms, r[0], r[1], r[2], rates, 10);
        memcpy(t0, t1, sizeof t0);
        loops0 = loops1; flips0 = flips1; pres0 = pres1; q0 = qn;
        ft.clear(); clamped = 0; lost_ms = 0.0;
    }
    return 0;
}

} /* namespace */

/* Called from infamous2_install_overrides(); returns the number of HLE
 * overrides installed (none: this only starts a thread). */
extern "C" int if2_clock_install(void)
{
    const char* e = getenv("IF2_CLOCKLOG");
    g_mode = (e && *e) ? atoi(e) : 0;
    const char* dc = getenv("IF2_DTCLAMP");
    const int clamp_hz = (dc && *dc) ? atoi(dc) : 0;
    if (g_mode <= 0 && clamp_hz <= 0) return 0;
    if (!IF2_HAVE(DTCLAMP) || (g_mode > 0 && !IF2_HAVE(CLOCKLOG))) {
        if2_feature_off(IF2_HAVE(DTCLAMP) ? "IF2_CLOCKLOG" : "IF2_DTCLAMP / IF2_CLOCKLOG", "the clock or its clamp");
        return 0;
    }
    /* The EBOOT's data is mapped before the overrides are installed. */
    if (vm_read32(kClamp15) != 0x3D888889u || vm_read32(kClamp30) != 0x3D088889u) {
        fprintf(stderr, "[if2-clock] clamp constants not found at 0x%X/0x%X: not %s, off%c",
                kClamp15, kClamp30, IF2_EBOOT_NAME, 10);
        return 0;
    }
    if (clamp_hz >= 10 && clamp_hz <= 30) {
        const float c = 1.0f / (float)clamp_hz;
        uint32_t u; memcpy(&u, &c, 4);
        vm_write32(kClamp30, u);
        fprintf(stderr, "[if2-clock] IF2_DTCLAMP=%d: the game's max frame dt is %.2f ms (was 33.33)%c",
                clamp_hz, 1000.0 / clamp_hz, 10);
    } else if (clamp_hz) {
        fprintf(stderr, "[if2-clock] IF2_DTCLAMP=%d ignored (10..30)%c", clamp_hz, 10);
    }
    if (g_mode <= 0) return 0;
    if (HANDLE h = CreateThread(nullptr, 0, clock_thread, nullptr, 0, nullptr)) CloseHandle(h);
    fprintf(stderr, "[if2-clock] watching the game clock at 0x%08X (IF2_CLOCKLOG=%d)%c", kClock, g_mode, 10);
    return 0;
}

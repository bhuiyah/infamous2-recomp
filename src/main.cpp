/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

/* Save-state core (runtime/savestate/ss_core.h), reached through the
 * toolkit's runtime/ppu include directory: a new include path would change
 * the compile command of every lifted TU and rebuild all of them. */
#include "../savestate/ss_core.h"
/* ... and its user-facing side: save / load requests from the window menu
 * and hotkeys, which the frame clock hands to the coordinator. */
#include "../savestate/ss_ui.h"
/* ... and the image a save captures (M4b): the guest arena and clock it
 * reads are declared from here. */
#include "../savestate/ss_state.h"
/* ... and the slots behind the window menu's Save / Load (M5): files on disk,
 * F5 / F9, SS_LOAD at launch. */
#include "../savestate/ss_slots.h"

#include <stdint.h>
#include <stdio.h>
#include "if2_version.h"   /* the EBOOT this build lifted (generated/if2_resolved.h) */
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#include <tlhelp32.h>   /* HOST_SAMPLE_ALL: thread enumeration */
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
void     cellGcm_fifo_kick_wait(unsigned ms); /* idle wait, woken early by a FIFO kick */
unsigned cellGcm_flip_request_count(void);
void*    cellGcm_fifo_kick_event(void);       /* the walker's idle-wait wake event */
void     cellGcm_ss_walker_info(ss_walker_info* w);    /* save-state census */
void     cellGcm_ss_walker_rebase(unsigned long long ms);
/* runtime/syscalls/sys_timer.h: the guest time base (mftb), PS3_TIMEBASE_FREQ */
uint64_t ppu_timebase_now(void);

/* libs/input/pad_host.h: physical controllers -- motors off, threads joined. */
void     padhost_shutdown(void);
/* include/ps3emu/options.h: the settings store's pending writes, if any; a row's text. */
int      opt_save(void);
int      opt_find(const char* key);
int      opt_text(int id, char* buf, size_t n);
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" int  rsx_d3d12_backend_ss_quiesce(void);   /* save states: the GPU idle before a capture */
extern "C" int  rsx_d3d12_backend_ss_reset(void);     /* save states: a load drops the renderer's caches */
extern "C" int  rsx_d3d12_backend_in_modal(void);     /* save states: no load inside a modal loop */
extern "C" int  rsx_d3d12_backend_grab_frame(int max_w, void (*cb)(const unsigned char*, int, int, int, void*),
                                             void* user);   /* save states: a slot's thumbnail */
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static void present_guest_frame(void)
{
    rsx_backend_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

/* HOST_SAMPLE=<start s>[:<seconds>]: sample the frame-clock thread (FIFO
 * walker + renderer) every ~1 ms for <seconds> (default 20) from <start s>,
 * unwinding each sample with the x64 unwind tables (no PDB needed), and write
 * host_samples.txt: per-RVA self and inclusive counts. Resolve the RVAs with
 * build/infamous2.map (tools/host_profile.py). Answers "where does a 10 s
 * menu frame go" without a profiler installed. */
static HANDLE s_clock_thread;
/* HOST_SAMPLE_ALL=1: every thread of the process instead (2 ms period). The
 * aggregate goes to host_samples.txt as before; host_threads.txt adds, per
 * host thread, how often its innermost exe frame was each function -- a
 * thread blocked in the OS has no exe leaf, so this ranks the BUSY threads
 * (which SPU/PPU host thread is burning the time, e.g. during a movie). */
static int host_unwind(HANDLE th, ULONG64* st, int max)
{
    CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &c)) return 0;
    int ns = 0;
    for (; ns < max; ns++) {
        st[ns] = c.Rip;
        ULONG64 ib = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &ib, NULL);
        if (!f) {   /* leaf: return address at [rsp] */
            if (!c.Rsp) break;
            c.Rip = *(ULONG64*)c.Rsp; c.Rsp += 8;
        } else {
            PVOID hd = NULL; ULONG64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, f, &c, &hd, &ef, NULL);
        }
        if (!c.Rip) { ns++; break; }
    }
    return ns;
}
static DWORD WINAPI host_sampler(LPVOID)
{
    const char* e = getenv("HOST_SAMPLE");
    double at = 0, dur = 20;
    if (sscanf(e, "%lf:%lf", &at, &dur) < 1) return 0;
    const int all = getenv("HOST_SAMPLE_ALL") ? 1 : 0;
    /* HOST_TIMELINE=1 (with HOST_SAMPLE_ALL): host_timeline.txt, one record per
     * tick and sampled thread -- "<tid> <B|I> <exe frames...>" after "t <us>". */
    FILE* tl = (all && getenv("HOST_TIMELINE")) ? fopen("host_timeline.txt", "w") : NULL;
    LARGE_INTEGER tl_f; QueryPerformanceFrequency(&tl_f);
    Sleep((DWORD)(at * 1000));
    timeBeginPeriod(1);
    ULONG64 base = (ULONG64)GetModuleHandleA(NULL);
    /* NTF: per-thread histogram slots, keyed by the leaf's FUNCTION start (its
     * unwind entry), not the instruction -- 64 instruction-keyed slots caught
     * ~13% of a busy Bink thread's samples. */
    enum { NB = 1 << 16, NT = 128, NTF = 1024 };
    static ULONG64 rva[NB]; static unsigned self_n[NB], incl_n[NB];
    /* per thread: tid, total samples, exe-leaf histogram */
    static DWORD t_id[NT]; static HANDLE t_h[NT]; static unsigned t_n[NT], t_busy[NT];
    static ULONG64 t_rva[NT][NTF]; static unsigned t_cnt[NT][NTF]; static int nt = 0;
    unsigned samples = 0;
    const DWORD self_tid = GetCurrentThreadId(), pid = GetCurrentProcessId();
    ULONGLONG t_end = GetTickCount64() + (ULONGLONG)(dur * 1000);
    while (GetTickCount64() < t_end) {
        Sleep(all ? 2 : 1);
        int ti_list[NT]; int nl = 0;
        /* HOST_SAMPLE_SNAP_MS (default 500): re-enumerate the threads that often,
         * not every round. A Toolhelp snapshot lists every thread in the SYSTEM
         * (~4000 here, 30-37 ms), so it was the whole round -- ~31 samples/s per
         * thread -- and it slowed every compute thread of this process ~13-25%
         * (the gate: 30-33 fps sampled vs 37-41 unsampled). In between, the known
         * handles are reused; one whose thread exited fails SuspendThread and is
         * skipped. 0 = snapshot every round (the old behaviour). */
        static ULONGLONG s_snap_at; static int s_snap_ms = -1, s_list[NT], s_nlist;
        if (s_snap_ms < 0) { const char* se = getenv("HOST_SAMPLE_SNAP_MS"); s_snap_ms = se ? atoi(se) : 500; }
        if (all && s_nlist && GetTickCount64() - s_snap_at < (ULONGLONG)s_snap_ms) {
            for (int i = 0; i < s_nlist; i++) ti_list[nl++] = s_list[i];
        } else if (all) {
            s_snap_at = GetTickCount64();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te; te.dwSize = sizeof te;
            if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) do {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self_tid) continue;
                int k = -1;
                for (int i = 0; i < nt; i++) if (t_id[i] == te.th32ThreadID) { k = i; break; }
                if (k < 0 && nt < NT) {
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                          THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                    if (h) { k = nt++; t_id[k] = te.th32ThreadID; t_h[k] = h; }
                }
                if (k >= 0 && nl < NT) ti_list[nl++] = k;
            } while (Thread32Next(snap, &te));
            if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
            for (int i = 0; i < nl; i++) s_list[i] = ti_list[i];
            s_nlist = nl;
        } else {
            if (!nt) { t_id[0] = GetThreadId(s_clock_thread); t_h[0] = s_clock_thread; nt = 1; }
            ti_list[nl++] = 0;
        }
        if (tl) {
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            const unsigned long long f = (unsigned long long)tl_f.QuadPart, v = (unsigned long long)q.QuadPart;
            fprintf(tl, "t %llu\n", (v / f) * 1000000ull + (v % f) * 1000000ull / f);
        }
        for (int li = 0; li < nl; li++) {
            int k = ti_list[li];
            if (SuspendThread(t_h[k]) == (DWORD)-1) continue;
            ULONG64 st[24];
            int ns = host_unwind(t_h[k], st, 24);
            ResumeThread(t_h[k]);
            if (tl) {
                const int b = ns > 0 && st[0] >= base && st[0] - base <= 0x7FFFFFFF;
                fprintf(tl, "%lu %c", (unsigned long)t_id[k], b ? 'B' : 'I');
                for (int j = 0, w = 0; j < ns && w < 4; j++)
                    if (st[j] >= base && st[j] - base <= 0x7FFFFFFF) {
                        fprintf(tl, " %llX", (unsigned long long)(st[j] - base)); w++; }
                fputc('\n', tl);
            }
            samples++; t_n[k]++;
            int leaf_done = 0;
            for (int j = 0; j < ns; j++) {
                if (st[j] < base || st[j] - base > 0x7FFFFFFF) continue;   /* outside the exe */
                ULONG64 r = st[j] - base;
                unsigned h = (unsigned)((r * 0x9E3779B97F4A7C15ull) >> 48) & (NB - 1);
                while (rva[h] && rva[h] != r) h = (h + 1) & (NB - 1);
                rva[h] = r;
                if (j == 0) self_n[h]++;
                int dup = 0;
                for (int q = 0; q < j; q++) if (st[q] == st[j]) { dup = 1; break; }
                if (!dup) incl_n[h]++;
                if (!leaf_done && j == 0) {   /* busy in the exe right now */
                    leaf_done = 1; t_busy[k]++;
                    ULONG64 ib = 0;
                    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(st[0], &ib, NULL);
                    ULONG64 key = (rf && ib == base) ? (ULONG64)rf->BeginAddress : r;
                    unsigned q = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 40) & (NTF - 1);
                    for (int probe = 0; probe < NTF; probe++, q = (q + 1) & (NTF - 1))
                        if (!t_cnt[k][q] || t_rva[k][q] == key) {
                            t_rva[k][q] = key; t_cnt[k][q]++; break; }
                }
            }
        }
    }
    if (tl) fclose(tl);
    FILE* f = fopen("host_samples.txt", "w");
    if (f) {
        fprintf(f, "samples %u base 0x%llX\n", samples, (unsigned long long)base);
        for (int h = 0; h < NB; h++)
            if (rva[h]) fprintf(f, "%llX %u %u\n", (unsigned long long)rva[h], self_n[h], incl_n[h]);
        fclose(f);
    }
    if (all && (f = fopen("host_threads.txt", "w")) != NULL) {
        for (int k = 0; k < nt; k++) {
            if (!t_busy[k]) continue;
            fprintf(f, "thread %lu samples %u busy %u\n", (unsigned long)t_id[k], t_n[k], t_busy[k]);
            for (int q = 0; q < NTF; q++)
                if (t_cnt[k][q]) fprintf(f, "  %llX %u\n", (unsigned long long)t_rva[k][q], t_cnt[k][q]);
        }
        fclose(f);
    }
    fprintf(stderr, "[host-sample] %u samples (%d thread(s)) -> host_samples.txt%s\n",
            samples, nt, all ? " + host_threads.txt" : "");
    return 0;
}

/* The frame clock's state, at file scope so modal_tick can run the clock too. */
static int       s_rsx_ok;
static unsigned  s_last_flip;
static ULONGLONG s_next_tick;

#if defined(_WIN32)
/* Dragging the window's border or title bar (or opening its system menu)
 * runs a modal loop inside DispatchMessage, i.e. inside rsx_backend_pump()
 * below, and the loop in frame_clock stops until the mouse is released --
 * vblanks, flips, the FIFO drain and so the whole game with it. The backend
 * calls this from a window timer (~10 ms) inside such a loop instead: the
 * loop body's tick, present and drain work, without the pump (we are inside
 * it). Keep it in step with frame_clock. */
extern "C" void rsx_d3d12_backend_set_modal_tick(void (*tick)(void));
static void modal_tick(void)
{
    if (!s_rsx_ok) return;
    ULONGLONG now = GetTickCount64();
    int fired = 0;
    while ((long long)(now - s_next_tick) >= 0 && fired < 240) {
        cellGcmTickVBlank();
        cellGcmTickFlip();
        if (cellGcm_take_flip_pending()) {
            present_guest_frame();
            s_last_flip = cellGcm_flip_request_count();
        }
        cellGcm_rsx_process_fifo();
        s_next_tick += 16;
        fired++;
    }
    if (fired >= 240) s_next_tick = now;
    if (cellGcm_take_flip_pending()) {
        present_guest_frame();
        s_last_flip = cellGcm_flip_request_count();
    }
    cellGcm_rsx_process_fifo();
    unsigned fc = cellGcm_flip_request_count();
    if (fc != s_last_flip) {
        present_guest_frame();
        s_last_flip = fc;
    } else if (fc == 0) {
        rsx_backend_present();
    }
    if (g_ss_armed || g_ss_ui_pending) ss_frame_tick(fc);   /* save states: see frame_clock */
}
#endif

/* Save-state stops (runtime/savestate/ss_core.h): a request -- a census
 * (SS_CENSUS=<sec>), SS_SAVE_AT, the window menu -- is coordinated from this
 * thread, the FIFO walker's own, by ss_frame_tick, once per frame-clock
 * iteration and right after a flip. These are the frame clock's parts of it:
 * one walker pass (a pending flip presented first, as the loop does), and
 * ending the idle wait early when the last thread parks. */
static int ss_walker_pass(void)
{
    if (!s_rsx_ok) return 0;
    if (cellGcm_take_flip_pending()) {
        present_guest_frame();
        s_last_flip = cellGcm_flip_request_count();
    }
    cellGcm_rsx_process_fifo();
    return 1;
}
static void ss_wake_clock(void)
{
#if defined(_WIN32)
    HANDLE e = (HANDLE)cellGcm_fifo_kick_event();
    if (e) SetEvent(e);
#endif
}
/* A save's capture (M4b), the walker drained: the recorded batch executed
 * without a present, pending reports delivered, the GPU waited for (< 0: it
 * did not go idle, and the save refuses that stop). */
static int ss_gpu_idle(void)
{
#if defined(_WIN32)
    return s_rsx_ok ? rsx_d3d12_backend_ss_quiesce() : 0;
#else
    return 0;
#endif
}

/* A save-state LOAD (M4c): its teardown drops the renderer's caches derived
 * from guest memory; its release resyncs the frame clock to the restored
 * guest -- the flip count the walker reports now is the snapshot's, so the
 * next present waits for the guest's next flip, and the vblank ticks start
 * from now (no burst of the ticks the pause would owe). A load waits out the
 * window's modal loop (critique #4). */
static void ss_gpu_reset(void)
{
#if defined(_WIN32)
    if (s_rsx_ok) (void)rsx_d3d12_backend_ss_reset();
#endif
}
static void ss_resync_clock(unsigned flip_count)
{
    s_last_flip = flip_count;
    s_next_tick = GetTickCount64();
}
static int ss_in_modal(void)
{
#if defined(_WIN32)
    return s_rsx_ok ? rsx_d3d12_backend_in_modal() : 0;
#else
    return 0;
#endif
}
/* A save's thumbnail (M5): the next presented frame, downscaled by the
 * backend, on this thread. */
static int ss_thumb_grab(int max_w, ss_thumb_cb cb, void* user)
{
#if defined(_WIN32)
    return s_rsx_ok ? rsx_d3d12_backend_grab_frame(max_w, cb, user) : 0;
#else
    (void)max_w; (void)cb; (void)user;
    return 0;
#endif
}

/* What the save-state slots need of the port (runtime/savestate/ss_slots.h):
 * the game and build a slot file is checked against, the moment's text, the
 * thumbnail. */
extern "C" void     if2_ss_describe(char* mission, size_t n, uint32_t* game_mode);      /* src/if2_ss_title.cpp */
extern "C" void     if2_display_config(double* aspect, uint8_t* hud, uint8_t* lod, uint8_t* fmv);   /* hooks */
extern "C" int      if2_png_encode_rgba(const uint8_t* rgba, int w, int h, int stride, void** png, size_t* n);
extern "C" uint64_t ppu_cont_hash(void);                  /* runtime/ppu/ppu_resume.cpp */
extern "C" uint64_t spu_channels_ss_image_table(void);    /* runtime/spu/spu_channels.c */
extern "C" double   ps3_guest_time_scale(void);           /* runtime/syscalls/sys_timer.c */

/* the play-session issues log (runtime/diag/play_issues.c, include/ps3emu/play_issues.h) */
extern "C" int  pi_start(const char* path, const char* build);
extern "C" int  pi_active(void);
extern "C" void pi_set_log(const char* path);
extern "C" void pi_end(const char* how);
extern "C" int  ps3_savedata_busy(void);     /* cellSaveData: a save or load in progress */

/* HOST_PLACEMENT (runtime/syscalls/lv2_register.c; NOTES 2026-10-27, PF1): the
 * frame clock -- the FIFO walker and the renderer -- and the PPU main thread
 * ask for cores of their own; the SPU workers ask from their own threads. */
extern "C" int ps3_host_place_thread(int role, int idx);
extern "C" void ps3_host_placement_configure(int mode);
/* SPU_TASKSET_LLE (runtime/spu/spu_channels.c; NOTES 2026-10-27, PF4): this title's
 * taskset task -- the psarc decompressor, waiting for requests all game long --
 * runs its syscalls through Sony's policy module, so its waits give the SPU back
 * to the SPURS kernel as on a PS3 and the WWS jobs run on all five SPUs.
 * SPU_TASKSET_LLE=0 is the kill switch (the HLE park). */
extern "C" void spu_taskset_lle_configure(int on);
/* GL1 (src/if2_glight.cpp): the SPU lighting job on the GPU (GLIGHT, GLIGHT_VERIFY). */
extern "C" void if2_glight_configure(void);
extern "C" void if2_edge_configure(void);
extern "C" void if2_bsp_configure(void);
extern "C" void if2_rlist_configure(void);

static DWORD WINAPI frame_clock(LPVOID)
{
    ps3_host_place_thread(0, 0);
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "inFamous 2 (ps3recomp)";

    int& rsx_ok = s_rsx_ok;
    rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned&  last_flip = s_last_flip;
    ULONGLONG& next_tick = s_next_tick;
    last_flip = 0;
    next_tick = GetTickCount64();
#if defined(_WIN32)
    if (rsx_ok) rsx_d3d12_backend_set_modal_tick(modal_tick);
#endif
    {   static const ss_host_ops ss_ops = { ss_walker_pass, cellGcm_ss_walker_info,
                                            cellGcm_ss_walker_rebase, ss_wake_clock, ss_gpu_idle,
                                            ss_resync_clock, ss_in_modal, cellGcm_flip_request_count,
                                            ss_gpu_reset };
        ss_set_host_ops(&ss_ops); }

    for (;;) {
        /* Was Sleep(4): the FIFO kick event (cellGcmSys.c) is signalled when a
         * game thread polls a fence the walker has not published yet, and it
         * was waited on by nobody here, so every such round trip paid the full
         * idle tick. Same 4 ms timeout, woken at once by a kick. */
        /* FIFO_IDLE_MS=<n> (default 4): how long the walker sleeps when nothing
         * wakes it. A guest waiting on the walker (ref-register polls, the FIFO
         * watch on put / label waits in cellGcmSys.c) wakes it at once; the
         * timeout only paces the idle drain. 1 ms measured slower. */
        { static int idle = -1;
          if (idle < 0) { const char* e = getenv("FIFO_IDLE_MS"); idle = e ? atoi(e) : 4; if (idle < 0) idle = 4; }
          cellGcm_fifo_kick_wait((unsigned)idle); }
        ULONGLONG now = GetTickCount64();

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += 16;             /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();

            if (rsx_backend_pump() != 0) {
                /* The window was closed (close button or Alt+F4; Esc is the
                 * pad's START in the keyboard bindings). Quit: without a
                 * window nothing drains the FIFO, so the title would freeze on
                 * its next RSX label and keep running invisibly until killed.
                 * WINDOW_CLOSE_KEEP=1 keeps the old headless behaviour. */
                static int keep = -1;
                if (keep < 0) { const char* e = getenv("WINDOW_CLOSE_KEEP"); keep = (e && *e && *e != '0') ? 1 : 0; }
                if (!keep) {
                    fprintf(stderr, "[rsx] window closed -- exiting\n");
                    /* A save the title is writing finishes first (its own thread writes it; up to 5 s --
                     * a save list left open on screen keeps the flag up, and is not worth more). */
                    for (int w = 0; w < 500 && ps3_savedata_busy(); w++) {
                        if (!w) fprintf(stderr, "[rsx] a save is being written: waiting for it\n");
                        Sleep(10);
                    }
                    /* So does a save-state slot file (F5 just before Quit): ~0.4 s, more on a slow disk;
                     * without it the slot would keep only a .tmp. */
                    ss_slots_wait_written(30000);
                    pi_end("the game was closed (Quit game, the window's close button or Alt+F4)");
                    /* The settings store saves on every change; this writes
                     * anything still pending (a batch) before ExitProcess. */
                    opt_save();
                    /* Stop the controllers' rumble first: ExitProcess skips
                     * atexit, and by its end XInput is already shut down
                     * (pad_host's exit hook only reaches HID pads). */
                    padhost_shutdown();
                    ss_request_summary("exit");   /* save states (ExitProcess skips atexit) */
                    fflush(stderr); fflush(stdout);
                    ExitProcess(0);
                }
                rsx_ok = 0;
                continue;
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                rsx_backend_present();
            }
        }
        /* Save states (armed unless SAVESTATES=0): one step of the stop state
         * machine per iteration -- also with no window, so a stop always
         * finishes; armed with no request it returns at once. A request's
         * stop starts right after a flip; the walker, vblank ticks and
         * presents above keep running while the guest threads park.
         * A save / load request from the window menu or a hotkey
         * (g_ss_ui_pending) is handed to the save-state engine here too: the
         * window procedure only queues it. */
        if (g_ss_armed || g_ss_ui_pending) ss_frame_tick(cellGcm_flip_request_count());
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf and USRDIR */
    for (int i = 0; i < 2; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    const char* above = strrchr(s_vfs_root, '/');
    above = above ? above + 1 : s_vfs_root;
#ifdef _WIN32
    if (_stricmp(above, "PS3_GAME") != 0) {
        /* A title update's EBOOT (<hdd0>/game/<ID>/USRDIR/EBOOT.elf): that hdd0 is /dev_hdd0, and the
         * disc is the project's game/disc, found by climbing from the EBOOT. */
        static char s_hdd0[1024];
        snprintf(s_hdd0, sizeof s_hdd0, "%s", s_vfs_root);
        for (int i = 0; i < 2; i++) { char* s = strrchr(s_hdd0, '/'); if (s) *s = 0; }
        if (!getenv("PS3_HDD0_ROOT") && s_hdd0[0]) _putenv_s("PS3_HDD0_ROOT", s_hdd0);
        char up[1024], probe[1100];
        snprintf(up, sizeof up, "%s", s_hdd0);
        for (;;) {
            snprintf(probe, sizeof probe, "%s/game/disc/PS3_GAME", up);
            if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
                snprintf(s_vfs_root, sizeof s_vfs_root, "%s/game/disc", up);
                fprintf(stderr, "[boot] update EBOOT: /dev_bdvd = %s, /dev_hdd0 = %s\n", s_vfs_root,
                        getenv("PS3_HDD0_ROOT"));
                break;
            }
            char* s = strrchr(up, '/');
            if (!s) break;
            *s = 0;
        }
    } else
#endif
    {
        (void)above;
        char* s = strrchr(s_vfs_root, '/');   /* strip PS3_GAME */
        if (s) *s = 0;
    }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    /* MEM_WRITE_WATCH: the D3D12 backend's texture cache asks the OS which
     * guest pages were written since it uploaded a texture, instead of
     * re-reading every bound texture every batch (libs/video/rsx_tex_cache.h).
     * A page costs nothing until the cache resets it (pages under and between
     * resident textures, rationed -- see that header); the first write after a
     * reset takes one soft fault (~0.45 us). VM_WRITE_WATCH=0 reserves without
     * it, and the cache falls back to checksumming every texture every batch. */
    { const char* e = getenv("VM_WRITE_WATCH");
      if (!(e && *e == '0'))
          vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE); }
    if (!vm_base)
        vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is committed up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left. */
    /* mmap, not calloc: vm_commit() mprotects page ranges of this arena, and
     * glibc's calloc returns a 16-byte-offset pointer for huge blocks, so
     * every mprotect failed with EINVAL and the first sys_memory_allocate
     * reported ENOMEM (inFamous 2's malloc then returned NULL). */
    void* p = mmap(nullptr, 0xE0000000u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
    ppu_vm_size = 0xE0000000u;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

extern "C" void infamous2_install_overrides(void);
#if IF2_EBOOT_KNOWN && IF2_EBOOT_V104
#define IF2_BUILD_TEXT "inFamous 2 recomp (BCUS98125 update 1.04), built " __DATE__ " " __TIME__
#elif IF2_EBOOT_KNOWN
#define IF2_BUILD_TEXT "inFamous 2 recomp (BCUS98125 disc), built " __DATE__ " " __TIME__
#else
#define IF2_BUILD_TEXT "inFamous 2 recomp (" IF2_EBOOT_NAME "), built " __DATE__ " " __TIME__
#endif

#ifdef _WIN32
#include <io.h>
/* Started with no arguments (double-clicked, or a shortcut): play mode. Work from the project folder (the exe's
 * folder or its parent, whichever holds game\disc), set the defaults tools\run.ps1 sets, run the EBOOT this build
 * lifted (IF2_DEFAULT_EBOOT, from analysis\inputs.json), and -- when Windows made a console just for this process --
 * hide that console and keep the log in logs\play-<date>-<time>.log. Returns the EBOOT path, or NULL. */
static const char* play_mode_setup(void)
{
    static char root[MAX_PATH], eboot[MAX_PATH * 2];
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return NULL;
    char* s = strrchr(exe, '\\');
    if (!s) return NULL;
    *s = 0;                                                    /* exe folder */
    const char* cands[2] = { exe, NULL };
    char parent[MAX_PATH];
    snprintf(parent, sizeof parent, "%s", exe);
    char* p = strrchr(parent, '\\');
    if (p) { *p = 0; cands[1] = parent; }
    root[0] = 0;
    for (int i = 0; i < 2 && !root[0]; i++) {
        if (!cands[i]) continue;
        char t[MAX_PATH * 2];
        snprintf(t, sizeof t, "%s\\game\\disc", cands[i]);
        if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) snprintf(root, sizeof root, "%s", cands[i]);
    }
    if (!root[0]) return NULL;
    SetCurrentDirectoryA(root);                                /* config\, gamedata\, logs\ resolve from here */
    char v[MAX_PATH * 2];
    snprintf(v, sizeof v, "%s\\game\\disc", root);
    if (!getenv("PS3_VFS_ROOT")) _putenv_s("PS3_VFS_ROOT", v);
#if IF2_EBOOT_V104   /* (the layout the resolver saw: a disc-layout EBOOT runs without the update, as on a PS3) */
    /* game\hdd0 holds the title update (and the HDD install beside it); a disc build runs without it, as
     * the disc EBOOT never runs with an update installed on a PS3 (its patch archives are 1.04 data). */
    snprintf(v, sizeof v, "%s\\game\\hdd0", root);
    if (!getenv("PS3_HDD0_ROOT") && GetFileAttributesA(v) != INVALID_FILE_ATTRIBUTES) _putenv_s("PS3_HDD0_ROOT", v);
#endif
    if (!getenv("PS3_TITLE")) _putenv_s("PS3_TITLE", "inFamous 2");
    /* The play log is a redirected stderr, which makes the runtime's per-wait lines verbose by default
     * (runtime/ps3_log.h); in play they cost ~8% at the prologue gate (34.4 -> 37.2 fps, 3 runs each,
     * 2026-10-21). Errors and the title's own status lines are not behind this switch. */
    if (!getenv("PS3_VERBOSE")) _putenv_s("PS3_VERBOSE", "0");
    if (!getenv("SPU_DMA_REPEAT_LIMIT")) _putenv_s("SPU_DMA_REPEAT_LIMIT", "100000000");
    if (!getenv("SPU_INTERP_UNLIFTED")) _putenv_s("SPU_INTERP_UNLIFTED", "1");
    if (!getenv("PS3_NULL_READ_MAX")) _putenv_s("PS3_NULL_READ_MAX", "400");
    if (!getenv("PS3_NULL_READ_STACK")) _putenv_s("PS3_NULL_READ_STACK", "40");
    snprintf(eboot, sizeof eboot, "%s\\%s", root, IF2_DEFAULT_EBOOT);
    for (char* c = eboot; *c; c++) if (*c == '/') *c = '\\';
    /* A console that exists only for us (double-click): hide it, log to a file instead. */
    SYSTEMTIME st; GetLocalTime(&st);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    char log[MAX_PATH] = "";
    DWORD pids[2];
    if (GetConsoleWindow() && GetConsoleProcessList(pids, 2) == 1) {
        CreateDirectoryA("logs", NULL);
        snprintf(log, sizeof log, "logs\\play-%s.log", stamp);
        if (freopen(log, "w", stdout)) { setvbuf(stdout, NULL, _IONBF, 0); _dup2(_fileno(stdout), _fileno(stderr)); }
        else log[0] = 0;
        ShowWindow(GetConsoleWindow(), SW_HIDE);
    }
    /* The play-session issues log beside it: stutters and their causes, slow stretches, stalls, log errors,
     * crashes, F8 marks (runtime/diag/play_issues.c; ~0.1 us a frame, one background thread). PLAY_ISSUES=0
     * turns it off. tools/play_triage.py reads it. */
    const char* pe = getenv("PLAY_ISSUES");
    if (!pe || strcmp(pe, "0")) {
        CreateDirectoryA("logs", NULL);
        char ip[MAX_PATH];
        snprintf(ip, sizeof ip, "logs\\play-%s-issues.txt", stamp);
        if (pi_start(pe && *pe ? pe : ip, IF2_BUILD_TEXT) && log[0]) pi_set_log(log);
    }
    return eboot;
}
#endif

#ifdef _WIN32
/* The options menu, FPS counter and notices are drawn into the game's own frames (the overlay's backbuffer
 * host), not on a separate DirectComposition layer above the window. That layer failed three ways on
 * players' PCs: RivaTuner's hook made its Present remove the game's device (the picture froze, issue #10);
 * AMD's driver crashed in amdxc64.dll the first time it presented in fullscreen (issues #15, #9); and in
 * borderless fullscreen, once gameplay frames were on screen, the layer came out nearly see-through or
 * not at all -- F1 paused the game with no menu visible (issue #3; reproduced here on an NVIDIA card: solid
 * over the boot screen, see-through over gameplay). The backbuffer host has none of these (the game keeps
 * presenting while paused). OVL_HOST=composition still picks the separate layer. */
static void overlay_host_for_hooks(void)
{
    if (getenv("OVL_HOST")) return;
    _putenv_s("OVL_HOST", "backbuffer");
}

/* The first start copies about 2.5 GB from the disc into game\hdd0\game\<title>_INSTALL (as the PS3
 * does); the screen stays dark until the splash movie, and the full copy takes a few minutes. Players
 * took that for a black-screen hang and closed the game (issues #9, #13). While the install's two
 * archives are missing or still growing, a notice says what is going on. Normal starts: nothing. */
extern "C" void ovl_notice(const char* utf8, unsigned ms);   /* include/ps3emu/overlay.h */

static char s_install_dir[MAX_PATH * 2];

static long long install_bytes(int* complete)
{
    static const char* const k_parts[] = { "install1\\infamous1.psarc_s", "install2\\infamous2.psarc_s" };
    long long total = 0;
    *complete = 1;
    for (const char* part : k_parts) {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\USRDIR\\cache\\psarc\\%s", s_install_dir, part);
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a)) { *complete = 0; continue; }
        total += ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    }
    return total;
}

static DWORD WINAPI first_install_notice_thread(LPVOID)
{
    /* one line (notices do not wrap): keep it short */
    const char* msg = "First start: installing game data (2.5 GB). This takes a few minutes.";
    /* ovl_notice is ignored until the overlay is installed, which the renderer does as it opens the
     * game window (rsx_d3d12_backend.c): wait for that window (this process's), then a moment more. */
    for (int i = 0; i < 600; i++) {                            /* at most 60 s */
        HWND w = FindWindowA("ps3recomp_d3d12", NULL);
        DWORD pid = 0;
        if (w) GetWindowThreadProcessId(w, &pid);
        if (w && pid == GetCurrentProcessId()) break;
        Sleep(100);
    }
    Sleep(1000);
    long long last = -1;
    int stable = 0;
    for (int t = 0; t < 20 * 60; t += 2) {                    /* at most 20 minutes */
        int complete;
        const long long now = install_bytes(&complete);
        stable = (complete && now == last) ? stable + 1 : 0;
        last = now;
        if (stable >= 3) {                                     /* both there, unchanged for 6 s */
            fprintf(stderr, "[install] first-start install looks complete (%.2f GB in the two archives)\n", now / 1e9);
            ovl_notice("First-start install finished.", 4000);
            return 0;
        }
        if (t % 20 == 0) ovl_notice(msg, 22000);               /* refreshed before it fades */
        Sleep(2000);
    }
    return 0;
}

/* An install interrupted on an earlier start (issue #13: a slow drive, the game closed during the dark first
 * start) leaves <title>_INSTALL behind with the intro movie and the game's ne.bv but not the archives. The game
 * decides "installed" from the folder alone (cellGameDataCheck finds it), so every later start skipped the copy
 * and stayed black after the intro. Every file of a complete install is a copy of the disc's file at the same
 * path under PS3_GAME\USRDIR (48 of 48 on a US copy; ne.bv is the game's own and has no disc counterpart): a
 * missing archive, or any file whose size differs from the disc's, means it was cut off. Such a folder is moved
 * aside (never deleted) to <title>_INSTALL.incomplete-<time>, and the game installs again from the start. */
static int install_cut_off(const char* inst, const char* disc_usr, const char* rel, char* why, size_t nwhy)
{
    char dir[MAX_PATH * 2], pat[MAX_PATH * 2];
    snprintf(dir, sizeof dir, "%s\\USRDIR%s%s", inst, rel[0] ? "\\" : "", rel);
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int cut = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char sub[MAX_PATH * 2];
        snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "\\" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { cut = install_cut_off(inst, disc_usr, sub, why, nwhy); continue; }
        char dp[MAX_PATH * 2];
        snprintf(dp, sizeof dp, "%s\\%s", disc_usr, sub);
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(dp, GetFileExInfoStandard, &a)) continue;   /* the game's own file (ne.bv) */
        const long long have = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        const long long want = ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        if (have != want) {
            snprintf(why, nwhy, "%s is %lld of %lld bytes", sub, have, want);
            cut = 1;
        }
    } while (!cut && FindNextFileA(h, &fd));
    FindClose(h);
    return cut;
}

static void install_repair(const char* disc_usr)
{
    if (GetFileAttributesA(s_install_dir) == INVALID_FILE_ATTRIBUTES) return;        /* not made yet */
    if (GetFileAttributesA(disc_usr) == INVALID_FILE_ATTRIBUTES) return;             /* no disc to compare with */
    char why[MAX_PATH * 2] = "";
    static const char* const k_parts[] = { "install1\\infamous1.psarc_s", "install2\\infamous2.psarc_s" };
    for (const char* part : k_parts) {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\USRDIR\\cache\\psarc\\%s", s_install_dir, part);
        if (!why[0] && GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) snprintf(why, sizeof why, "cache\\psarc\\%s is missing", part);
    }
    if (!why[0] && !install_cut_off(s_install_dir, disc_usr, "", why, sizeof why)) return;   /* complete */
    SYSTEMTIME st; GetLocalTime(&st);
    char aside[MAX_PATH * 2];
    snprintf(aside, sizeof aside, "%s.incomplete-%04u%02u%02u-%02u%02u%02u", s_install_dir, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    if (MoveFileA(s_install_dir, aside))
        fprintf(stderr, "[install] the HDD install was cut off on an earlier start (%s): moved aside to %s; "
                        "the game installs again now\n", why, aside);
    else
        fprintf(stderr, "[install] the HDD install was cut off on an earlier start (%s), and moving it aside failed "
                        "(error %lu): the game may stay black after the intro -- delete %s and start again\n",
                why, (unsigned long)GetLastError(), s_install_dir);
}

static void first_install_notice(void)
{
    const char* hdd0 = getenv("PS3_HDD0_ROOT");
    char base[MAX_PATH * 2];
    if (hdd0 && *hdd0) snprintf(base, sizeof base, "%s", hdd0);
    else if (!GetFullPathNameA("game\\hdd0", sizeof base, base, NULL)) return;
    /* the edition's own folder: BCUS98125_INSTALL, BCES01143_INSTALL (Europe)... */
    snprintf(s_install_dir, sizeof s_install_dir, "%s\\game\\%s_INSTALL", base, IF2_TITLE_ID);
    for (char* c = s_install_dir; *c; c++) if (*c == '/') *c = '\\';
    {
        const char* vfs = getenv("PS3_VFS_ROOT");
        char disc_usr[MAX_PATH * 2];
        if (vfs && *vfs) snprintf(disc_usr, sizeof disc_usr, "%s\\PS3_GAME\\USRDIR", vfs);
        else if (!GetFullPathNameA("game\\disc\\PS3_GAME\\USRDIR", sizeof disc_usr, disc_usr, NULL)) disc_usr[0] = 0;
        for (char* c = disc_usr; *c; c++) if (*c == '/') *c = '\\';
        if (disc_usr[0]) install_repair(disc_usr);
    }
    int complete;
    install_bytes(&complete);
    if (complete) return;
    fprintf(stderr, "[install] first start: the HDD install (%s) is not complete yet -- the game builds it now; "
                    "a notice says so on screen\n", s_install_dir);
    HANDLE h = CreateThread(NULL, 0, first_install_notice_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
#endif

int main(int argc, char** argv)
{
    const char* eboot_arg = argc >= 2 ? argv[1] : NULL;
#ifdef _WIN32
    if (!eboot_arg) eboot_arg = play_mode_setup();
    /* any other run: PLAY_ISSUES=<file> starts the issues log (harness tests of it) */
    else if (const char* pe = getenv("PLAY_ISSUES"))
        if (*pe && strcmp(pe, "0")) pi_start(pe, IF2_BUILD_TEXT);
#endif
    if (!eboot_arg) {
        printf("usage: %s <game>/PS3_GAME/USRDIR/EBOOT.elf\n"
               "       (decrypted EBOOT from YOUR copy -- see README)\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
    overlay_host_for_hooks();           /* before the overlay first shows (it reads OVL_HOST then) */
    first_install_notice();             /* first start: say the dark screen is the HDD install */
#endif
    /* Save states: armed by default (SAVESTATES=0 turns them off: no slicing,
     * no thread roots, every request refused); SS_CENSUS and SS_SAVE_AT add
     * request sources. (Already run by ss_core.c's static constructor.) */
    ss_init();
    {   /* The SPU host waits a strict save accepts besides drain hops: this
         * title's, audited in the SPU study (census 2026-10-15, engine design
         * 1.2; tools/ss_census.py's SPU_AUDITED is the same list). Image ids
         * are this title's lifted SPU images in registration order. To
         * extend, see ss_set_spu_audited in ss_core.h. */
        static const ss_spu_audit k_spu_audited[] = {
            { 41, SS_SPU_W_CHANNEL, 0x01178 },   /* SPURS kernel, channel wait: idempotent prefix, resv re-taken */
            { 41, SS_SPU_W_SIGNAL,  0x00A70 },   /* SPURS kernel, taskset WAIT_SIGNAL: exact */
            {  2, SS_SPU_W_EQUEUE,  0x0FA08 },   /* Bink decoder, stop-receive (0x110): idempotent */
        };
        ss_set_spu_audited(k_spu_audited, (int)(sizeof k_spu_audited / sizeof k_spu_audited[0]));
    }

    printf("=== inFamous 2 -- ps3recomp static recompilation ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }
    /* What a save captures (runtime/savestate/ss_state.h): the 4 GB guest
     * arena (not the 64K guard past it) and the guest time base. */
    ss_state_set_arena(vm_base, 0x100000000ull);
    ss_time_set_guest_clock(ppu_timebase_now, 79800000ull /* PS3_TIMEBASE_FREQ */);

    uint32_t entry = ppu_load_elf(eboot_arg);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", eboot_arg);
        return 1;
    }

    derive_vfs_root(eboot_arg);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    {   /* Save-state slots (M5): the window menu's Save / Load and F5 / F9 on
         * files in savestates\BCUS98125 (SS_DIR), SS_LOAD at launch,
         * SS_SAVE_AT + SS_SAVE_SLOT. The working directory is the project's
         * by now (run.ps1, or play mode's chdir). */
        static char eboot_full[1024];
        if (!_fullpath(eboot_full, eboot_arg, sizeof eboot_full)) snprintf(eboot_full, sizeof eboot_full, "%s", eboot_arg);
        ss_slots_host sh;
        memset(&sh, 0, sizeof sh);
        sh.title_id = IF2_TITLE_ID;   /* (savestates\<title id>) */
#if IF2_EBOOT_KNOWN && IF2_EBOOT_V104
        sh.disc_ver = "01.04";
        sh.build_text = "1.04 " __DATE__ " " __TIME__;
#elif IF2_EBOOT_KNOWN
        sh.disc_ver = "02.00";
        sh.build_text = "disc " __DATE__ " " __TIME__;
#elif IF2_EBOOT_V104
        sh.disc_ver = "01.04";
        sh.build_text = "beta " __DATE__ " " __TIME__;
#else
        sh.disc_ver = "disc";
        sh.build_text = "beta disc " __DATE__ " " __TIME__;
#endif
        sh.eboot_path = eboot_full;
        sh.cont_hash = ppu_cont_hash;
        sh.spu_table = spu_channels_ss_image_table;
        sh.describe = if2_ss_describe;
        sh.display = if2_display_config;
        sh.thumb_grab = ss_thumb_grab;
        sh.png_encode = if2_png_encode_rgba;
        sh.guest_time_scale = ps3_guest_time_scale();
        ss_slots_init(&sh);
    }

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    /* Title overrides go FIRST: ps3_hle_call scans the ctx-handler table in
     * registration order and the first match wins, so anything registered
     * after this (ppu_fs, sysprx) cannot shadow an inFamous 2 override. */
    infamous2_install_overrides();
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    /* HOST_PLACEMENT: this title opts in -- the cpu_placement row (PC Settings > Frame, advanced: auto | off;
     * restart), decided before the frame clock places itself (runtime/syscalls/lv2_register.c). */
    {
        char pv[16] = "auto";
        const int k = opt_find("cpu_placement");
        if (k >= 0) opt_text(k, pv, sizeof pv);
        ps3_host_placement_configure(strcmp(pv, "off") ? -1 : 0);
    }
    spu_taskset_lle_configure(1);   /* before the first SPU thread */
    if2_glight_configure();         /* before the first SPU thread and the device */
    if2_edge_configure();           /* EG: the Edge Geometry job (native skinning) */
    if2_bsp_configure();            /* job 216's BSP clipper, natively (src/if2_bsp_native.c) */
    if2_rlist_configure();          /* job 225's render-list builder, natively (src/if2_rlist_native.c) */
    s_clock_thread = CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
    if (getenv("HOST_SAMPLE")) CreateThread(NULL, 0, host_sampler, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    /* PS3_SAMPLE=<ms>: guest-PC sampler (where each thread spends its time),
     * started from the boot thread as the toolkit's own runner does. Inert
     * unless the variable is set. */
    { extern void ps3_sampler_start(void); ps3_sampler_start(); }

    ps3_host_place_thread(2, 0);   /* this thread runs the PPU main thread (HOST_PLACEMENT=3) */
    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}

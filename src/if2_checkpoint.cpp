/*
 * inFamous 2 -- checkpoints: test "breakpoints" in the game.
 *
 *  1. Boot a run from a stored save (CHECKPOINT=<name>). The save is copied into
 *     a sandbox save-data directory (PS3_SAVEDATA_DIR, libs/system/cellSaveData.c)
 *     before any guest code runs, so a test never reads or overwrites the
 *     player's own saves.
 *  2. Drive the title flow on GAME EVENTS, not on wall-clock seconds: the
 *     title's boot-time save load (cellSaveDataListAutoLoad), its own signals
 *     (MenuActivate, PackLoaded, WarpedToCheckpoint, ...) and guest frames.
 *     With a save the title continues it by itself -- no button (logs
 *     run-20261014-131107 / -131644: ListAutoLoad -> splash -> loading screen
 *     -> AutoSave2 x2 -> New Marais, no input). For a new game (@new) CROSS is
 *     pressed while a menu is up and nothing is loading. With IF2_TASKJUMP
 *     (src/if2_taskgraph.cpp) READY also waits for the jump to its mission step.
 *  3. Say when gameplay is READY (a [checkpoint] READY log line and
 *     CHECKPOINT_READY_FILE), then play a frame-timed pad script from there
 *     (CHECKPOINT_SCRIPT) and optionally quit (CHECKPOINT_EXIT_AFTER).
 *  4. Opt-in (CHECKPOINT_AUTOSNAP=1): keep every save the game writes as a
 *     numbered checkpoint (auto-NNNN: the save, its SUB_TITLE/DETAIL text, a
 *     screenshot, meta.json), so play sessions build the library.
 * With neither CHECKPOINT nor CHECKPOINT_AUTOSNAP nor IF2_SIGLOG set, nothing
 * here is registered: a normal run takes the runtime's own paths.
 *
 * Library: CHECKPOINT_DIR, else <project>\checkpoints (found from the exe's
 * folder), else .\checkpoints. User data -- copies of saves -- kept out of git
 * (.gitignore: checkpoints/). tools\checkpoint.ps1 lists / saves / loads /
 * renames / deletes entries; tools\run.ps1 -Checkpoint <name> boots one.
 *   <name>\meta.json            name, description, created, source, save dir,
 *                               the save's SUB_TITLE / DETAIL, frame, ...
 *   <name>\screen.png           the game window when the save was kept
 *   <name>\savedata\<DIR>\...   the save directory as cellSaveData keeps it
 *   <name>\PARAM.SFO, ...       a "flat" entry (a save directory's files copied
 *                               straight out of RPCS3's or a PS3's savedata)
 *   _runs\<name>-<time>-<pid>\savedata   sandboxes (newest CHECKPOINT_KEEP_RUNS kept)
 *
 * Environment:
 *   CHECKPOINT=<name>|@new          boot from that checkpoint / with no save
 *   IF2_BOOT=continue|newgame|off   the title-flow driver (default: newgame for
 *                                   @new, continue for any other checkpoint)
 *   CHECKPOINT_READY_FILE=<path>    key=value: status=booting|ready|fail|timeout,
 *                                   confidence, reason, timeline (written at boot
 *                                   and when the outcome is known)
 *   CHECKPOINT_READY_ON=<sig>[,..]  READY signals (default WarpedToCheckpoint)
 *   CHECKPOINT_READY_SHOT=<file>    screenshot of the game window at READY (.png)
 *   CHECKPOINT_TIMEOUT=<s>          give up (status=timeout) after this (600)
 *   CHECKPOINT_SCRIPT="<f>:<mask>[/<hold>]|<f1>-<f2>:<lx>,<ly>,<rx>,<ry>|..."
 *                                   pad port 0, f = guest frames after READY;
 *                                   masks as PAD_SCRIPT (CROSS 0x4000, START 0x0008,
 *                                   UP 0x0010, DOWN 0x0040); sticks 0-255, 128 = centre
 *   CHECKPOINT_EXIT_AFTER=<frames>  quit that many frames after READY (exit code 0);
 *                                   on fail / timeout quit at once (4 / 5)
 *   CHECKPOINT_SKIP_MOVIES=all|fmv|0  in driven runs: skip the story FMVs (movies/
 *                                   cutscene/v_*) and the logo splash (all, default),
 *                                   the FMVs only (fmv), none (0)
 *   CHECKPOINT_AUTOSNAP=1           keep the game's saves as auto-NNNN (off by default)
 *   CHECKPOINT_DEDUP_BYTES=<n>      a save whose own files differ from the last one
 *                                   loaded or kept by <= n bytes, same save title,
 *                                   is not kept again (16; -1 keeps every save)
 *   CHECKPOINT_KEEP_RUNS=<n>        sandboxes kept in _runs (8)
 *   IF2_SIGLOG=1|all|<sig>[,...]    log the game's signals ([if2-sig] lines;
 *                                   1 = the first 8 of each, then every 100th)
 *
 * The signal hook: every game signal goes through one dispatcher,
 * 0x2F26B8(source r3, id r4, args r5) -- 0x2F305C(source, id) and 0x2F27D8
 * (source, id, args) end in it. The ids index the game's reflection name table
 * at 0x983910 (AmbientSpeech .. XpChanged, 109 names); each constant id matches
 * its only sender (SubtitleStart 93 / SubtitleFinish 94 in the subtitle player
 * 0x358E14 / 0x358E58, MenuActivate 48 / MenuDeactivate 49 at 0x1BA960 /
 * 0x1BA420, MovieStarted 60 / MovieFinished 61 at 0x1D0C60 / 0x1D0D34, the
 * Mission* ids in the mission manager 0x1C4BB8..0x1C56A0, WarpedToCheckpoint
 * 105 at 0x369A14 / 0x372AAC). Which of them fire when is still to be logged on
 * a real run (IF2_SIGLOG=all); the READY default is the likeliest, not a proven one.
 * tools/ppu_postlift.py puts if2_signal_hook at the top of the lifted
 * func_002F26B8; the names are read from guest memory at install and the
 * feature turns itself off when the table is not this EBOOT's.
 *
 * Save data: the title imports cellSaveDataListAutoLoad (boot), AutoSave2
 * (autosaves), ListSave2 / ListLoad2 (the pause menu). When needed they are
 * wrapped here (ctx handlers, registered before the runtime's): each calls the
 * runtime's own entry, then asks cellSaveData which directory that call loaded
 * or wrote (ps3_savedata_last_op) and reads it from disk.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "ps3emu/nid.h"
#include "ppu_context.h"
#include "if2_version.h"

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
/* runtime/ppu/ppu_loader.cpp: the lifted code's own big-endian accessors */
extern "C" uint8_t  vm_read8(uint64_t addr);
extern "C" uint16_t vm_read16(uint64_t addr);
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void     vm_write16(uint64_t addr, uint16_t val);
extern "C" unsigned ppu_boot_frames_presented(void);       /* src/main.cpp: guest flips */
extern "C" void     padhost_shutdown(void);                /* libs/input: motors off */
extern "C" void     ss_request_summary(const char* when);  /* runtime/savestate: requests at exit */
extern "C" long long    g_ftr_ready_qpc;                   /* libs/video/cellGcmSys.c: FRAME_TRACE marks */
extern "C" volatile int g_ftr_pad_rx;
/* libs/input/cellPad.c: `data` is a guest EA carried in a pointer */
extern "C" int32_t  cellPadGetData(uint32_t port_no, void* data);
extern "C" int      options_intercepted(void);             /* libs/system/options.c */
/* include/ps3emu/overlay.h: the options overlay / FPS counter, hidden for a grab */
extern "C" int      ovl_capture_begin(unsigned timeout_ms);
extern "C" void     ovl_capture_end(void);
/* libs/system/cellSaveData.c: guest EAs carried in pointers, as the generic
 * HLE table passes them; ListAutoLoad has a 9th (stack) argument, so its ctx
 * entry is called instead. */
extern "C" void     ps3_savedata_list_auto_load(ppu_context* ctx);
extern "C" int32_t  cellSaveDataAutoSave2(uint32_t version, const char* dirName, uint32_t errDialog,
                                          void* setBuf, void* funcStat, void* funcFile,
                                          uint32_t container, void* userdata);
extern "C" int32_t  cellSaveDataListSave2(uint32_t version, void* setList, void* setBuf, void* funcList,
                                          void* funcStat, void* funcFile, uint32_t container, void* userdata);
extern "C" int32_t  cellSaveDataListLoad2(uint32_t version, void* setList, void* setBuf, void* funcList,
                                          void* funcStat, void* funcFile, uint32_t container, void* userdata);
/* The directory cellSaveData uses (PS3_SAVEDATA_DIR or its default), and the
 * save the last completed call loaded or wrote (cellSaveData.h). */
extern "C" const char* ps3_savedata_root(void);
extern "C" uint32_t    ps3_savedata_last_op(char* dirName, int* wrote);
/* src/if2_taskgraph.cpp: 0 no IF2_TASKJUMP, 1 pending, 2 at its step, 3 gave up */
extern "C" int         if2_taskjump_status(char* what, size_t cap);
/* Called by src/if2_taskgraph.cpp on the game's main thread right before the
 * jump acts (its WarpToCheckpoint can post WarpedToCheckpoint at once): READY
 * counts the game's signals from that moment. */
extern "C" void        if2_checkpoint_taskjump_mark(void);

extern "C" void if2_signal_hook(ppu_context* ctx);
/* the play-session issues log (runtime/diag/play_issues.c): the game's events as context, F8 screenshots */
extern "C" int  pi_active(void);
extern "C" void pi_signal(const char* name);
extern "C" void pi_set_mark_hook(void (*hook)(int n, const char* shot_path));
extern "C" int  if2_checkpoint_install(void);
/* src/if2_png.cpp: the port's PNG writer (WIC) -- screenshots here, save-state thumbnails in main.cpp */
extern "C" int  if2_png_write(const char* path, int w, int h, const void* bgr24, unsigned stride);
/* runtime/savestate/ss_slots.h: SS_LOAD's launch load is done (ok) or given up */
extern "C" void ss_slots_set_launch_hook(void (*fn)(int ok, const char* what));

namespace {

/* ------------------------------------------------------------------ util */

unsigned frames() { return ppu_boot_frames_presented(); }
ULONGLONG g_t0 = 0;
double uptime() { return (double)(GetTickCount64() - g_t0) / 1000.0; }

const char* env(const char* k) { const char* v = getenv(k); return (v && *v) ? v : nullptr; }

#define CKLOG(...) do { fprintf(stderr, "[checkpoint] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

bool is_dir(const std::string& p)
{
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool is_file(const std::string& p)
{
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
std::string join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    char e = a.back();
    return (e == '\\' || e == '/') ? a + b : a + "\\" + b;
}
void mkdirs(const std::string& p)
{
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        const char c = p[i] == '/' ? '\\' : p[i];
        cur += c;
        if ((c == '\\' && i > 2) || i + 1 == p.size()) CreateDirectoryA(cur.c_str(), nullptr);
    }
}
std::string full_path(const std::string& p)
{
    char buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameA(p.c_str(), sizeof buf, buf, nullptr);
    return (n && n < sizeof buf) ? std::string(buf) : p;
}
/* Entries of `dir` that are directories (dirs) or files. Names starting with
 * '.' -- cellSaveData's ".NAME.tmp" / ".NAME.old" work directories -- only
 * when `dots` is set. */
std::vector<std::string> list_dir(const std::string& dir, bool dirs, bool dots = false)
{
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(join(dir, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.cFileName[0] == '.' && !dots) continue;
        const bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (d == dirs) out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}
/* A copied save file must stay writable: CopyFile keeps the read-only
 * attribute (a checkpoint folder marked read-only to protect it), and the
 * title's next save into the sandbox would then fail. */
bool copy_file_rw(const std::string& from, const std::string& to)
{
    if (!CopyFileA(from.c_str(), to.c_str(), FALSE)) return false;
    const DWORD a = GetFileAttributesA(to.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesA(to.c_str(), a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    return true;
}
int copy_tree(const std::string& src, const std::string& dst)
{
    mkdirs(dst);
    int n = 0;
    for (auto& f : list_dir(src, false)) {
        if (copy_file_rw(join(src, f), join(dst, f))) n++;
        else CKLOG("copy failed: %s -> %s (error %lu)", join(src, f).c_str(), dst.c_str(), GetLastError());
    }
    for (auto& d : list_dir(src, true)) n += copy_tree(join(src, d), join(dst, d));
    return n;
}
/* Everything, the dot-named work directories a run's cellSaveData left
 * included: skipping them left every pruned sandbox behind, non-empty. */
void remove_tree(const std::string& dir)
{
    for (auto& d : list_dir(dir, true, true)) remove_tree(join(dir, d));
    for (auto& f : list_dir(dir, false, true)) {
        const std::string p = join(dir, f);
        const DWORD a = GetFileAttributesA(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
            SetFileAttributesA(p.c_str(), a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
        DeleteFileA(p.c_str());
    }
    RemoveDirectoryA(dir.c_str());
}
bool read_file(const std::string& p, std::string& out)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
std::string now_stamp(bool iso)
{
    time_t t = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &t);
    char b[32];
    strftime(b, sizeof b, iso ? "%Y-%m-%dT%H:%M:%S" : "%Y%m%d-%H%M%S", &lt);
    return b;
}
std::string json_str(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}
/* one line, for key=value files and the log */
std::string flat(std::string s)
{
    for (auto& c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}
std::string first_line(const std::string& s) { return s.substr(0, s.find('\n')); }
std::string guest_str(uint32_t ea, size_t max)
{
    std::string s;
    if (!ea) return s;
    for (size_t i = 0; i < max; i++) {
        const char c = (char)vm_read8(ea + i);
        if (!c) break;
        s += c;
    }
    return s;
}

/* PARAM.SFO: the binary PSF format ("\0PSF", little-endian index) that RPCS3 and
 * the PS3 write, or the "KEY=value" lines older cellSaveData builds wrote. */
std::string sfo_get(const std::string& sfo, const char* key)
{
    if (sfo.size() >= 20 && !memcmp(sfo.data(), "\0PSF", 4)) {
        auto u16 = [&](size_t o) { return o + 2 <= sfo.size() ? (unsigned)(uint8_t)sfo[o] | ((unsigned)(uint8_t)sfo[o + 1] << 8) : 0u; };
        auto u32 = [&](size_t o) { return o + 4 <= sfo.size() ? u16(o) | (u16(o + 2) << 16) : 0u; };
        const unsigned keys = u32(8), data = u32(12), n = u32(16);
        for (unsigned i = 0; i < n && i < 256; i++) {
            const size_t e = 20 + 16u * i;
            const size_t ko = keys + u16(e);
            if (e + 16 > sfo.size() || ko >= sfo.size()) break;
            if (strncmp(sfo.c_str() + ko, key, sfo.size() - ko) != 0) continue;
            const unsigned fmt = u16(e + 2), len = u32(e + 4);
            const size_t off = (size_t)data + u32(e + 12);
            if (off + len > sfo.size()) return "";
            if (fmt == 0x0404) { char b[16]; snprintf(b, sizeof b, "%u", u32(off)); return b; }
            std::string v = sfo.substr(off, len);
            v.resize(strnlen(v.c_str(), v.size()));
            return v;
        }
        return "";
    }
    const std::string k = std::string(key) + "=";
    size_t p = 0;
    while (p < sfo.size()) {
        size_t e = sfo.find('\n', p);
        if (e == std::string::npos) e = sfo.size();
        if (sfo.compare(p, k.size(), k) == 0) {
            std::string v = sfo.substr(p + k.size(), e - p - k.size());
            if (!v.empty() && v.back() == '\r') v.pop_back();
            return v;
        }
        p = e + 1;
    }
    return "";
}

/* ---------------------------------------------------------- screenshots */

/* The game window's client area into <path>: PNG through WIC, or a BMP when
 * the name ends in .bmp or WIC fails. Read from the screen (what the player
 * sees, as tools\kbm_inject.ps1 "shot" does), without sending the window a
 * message -- its thread may be busy for seconds. At most `maxw` wide. */
bool write_png(const std::string& path, int w, int h, const void* bits, UINT stride)
{
    return if2_png_write(path.c_str(), w, h, bits, stride) != 0;   /* src/if2_png.cpp */
}
bool write_bmp(const std::string& path, int w, int h, const void* bits, UINT stride)
{
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER bh = {};
    bh.biSize = sizeof bh; bh.biWidth = w; bh.biHeight = -h;       /* top-down rows */
    bh.biPlanes = 1; bh.biBitCount = 24; bh.biCompression = BI_RGB; bh.biSizeImage = stride * (DWORD)h;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof bh; fh.bfSize = fh.bfOffBits + bh.biSizeImage;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&bh, sizeof bh, 1, f); fwrite(bits, 1, bh.biSizeImage, f);
    fclose(f);
    return true;
}
HWND game_view()
{
    HWND top = nullptr;
    const DWORD pid = GetCurrentProcessId();
    while ((top = FindWindowExA(nullptr, top, "ps3recomp_d3d12", nullptr)) != nullptr) {
        DWORD wp = 0;
        GetWindowThreadProcessId(top, &wp);
        if (wp == pid) break;
    }
    if (!top || !IsWindowVisible(top) || IsIconic(top)) return nullptr;
    HWND v = FindWindowExA(top, nullptr, "ps3recomp_d3d12_view", nullptr);
    return v ? v : top;
}
/* Returns the file written ("" when there is no visible window). */
std::string screenshot(const std::string& path, int maxw)
{
    HWND w = game_view();
    if (!w) return "";
    RECT r; GetClientRect(w, &r);
    POINT o = {0, 0}; ClientToScreen(w, &o);
    const int sw = r.right - r.left, sh = r.bottom - r.top;
    if (sw <= 16 || sh <= 16) return "";
    const int dw = std::min(sw, maxw), dh = std::max(1, (int)((long long)sh * dw / sw));
    std::string out;
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = dw; bi.bmiHeader.biHeight = -dh;          /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        SetStretchBltMode(mem, HALFTONE);
        SetBrushOrgEx(mem, 0, 0, nullptr);
        /* The overlay (menu, FPS counter, notices) is composed by the desktop,
         * so a grab of the screen would hold it: hidden for the grab. */
        ovl_capture_begin(500);
        StretchBlt(mem, 0, 0, dw, dh, scr, o.x, o.y, sw, sh, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        ovl_capture_end();
        const UINT stride = ((UINT)dw * 3 + 3) & ~3u;
        const bool want_bmp = path.size() > 4 && !_stricmp(path.c_str() + path.size() - 4, ".bmp");
        if (!want_bmp && write_png(path, dw, dh, bits, stride)) out = path;
        else {
            std::string b = want_bmp ? path : path.substr(0, path.find_last_of('.')) + ".bmp";
            if (write_bmp(b, dw, dh, bits, stride)) out = b;
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem); ReleaseDC(nullptr, scr);
    return out;
}
struct ShotJob { std::string path; };
DWORD WINAPI shot_thread(LPVOID p)
{
    ShotJob* job = (ShotJob*)p;
    const std::string got = screenshot(job->path, 960);
    if (got.empty()) CKLOG("no screenshot (%s): the game window is not visible", job->path.c_str());
    delete job;
    return 0;
}
void screenshot_async(const std::string& path)
{
    ShotJob* job = new ShotJob{path};
    if (HANDLE h = CreateThread(nullptr, 0, shot_thread, job, 0, nullptr)) CloseHandle(h);
    else delete job;
}

/* ---------------------------------------------------------------- config */

enum Boot { BOOT_OFF, BOOT_CONTINUE, BOOT_NEWGAME };
Boot        g_boot = BOOT_OFF;
std::string g_lib;              /* library root (absolute) */
std::string g_cp;               /* CHECKPOINT value ("" = none) */
std::string g_sandbox;          /* the sandbox save-data directory of this run */
std::string g_ready_file, g_ready_shot;
int         g_autosnap = 0;
long        g_dedup = 16;
double      g_timeout = 600.0;
long        g_exit_after = -1;  /* frames after READY; -1 = keep running */
long        g_exit_after_ms = -1;  /* CHECKPOINT_EXIT_AFTER=<n>ms|<n>s: wall time after READY */

/* the save-data directory cellSaveData uses (libs/system/cellSaveData.c) */
std::string savedata_root() { return full_path(ps3_savedata_root()); }

/* The save a save-data call just loaded or wrote: cellSaveData counts the
 * operations that complete, so a count that moved during the call names it. */
struct SaveOp { bool done = false; std::string dir; bool wrote = false; };
uint32_t save_ops() { return ps3_savedata_last_op(nullptr, nullptr); }
SaveOp save_op_since(uint32_t before)
{
    char d[64] = "";
    int w = 0;
    SaveOp op;
    if (ps3_savedata_last_op(d, &w) == before) return op;
    op.done = true; op.dir = d; op.wrote = w != 0;
    return op;
}

/* --------------------------------------------------------------- signals */

constexpr uint32_t kSigNames = IF2_R(SIG_NAMES);   /* reflection table: name pointer per signal id (disc 0x983910) */
constexpr int      kMaxSig   = 128;
int         g_nsig = 0;
std::string g_sig_name[kMaxSig];
std::atomic<int>      g_sig_on{0};
std::atomic<uint32_t> g_sig_count[kMaxSig];
std::atomic<uint32_t> g_sig_frame[kMaxSig];
int  g_siglog = 0;              /* 0 off, 1 rate-limited, 2 all, 3 selected */
bool g_sig_issues = false;      /* the play-session issues log takes each signal's name */
bool g_sig_sel[kMaxSig];

int sig_id(const char* name)
{
    for (int i = 0; i < g_nsig; i++) if (!_stricmp(g_sig_name[i].c_str(), name)) return i;
    return -1;
}
std::string image_cstr(uint32_t ea, size_t max)      /* a printable string in the EBOOT image */
{
    std::string s;
    if (ea < 0x10000 || ea >= 0x01000000) return s;
    for (size_t i = 0; i < max; i++) {
        const char c = (char)vm_read8(ea + i);
        if (!c) return s;
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return std::string();
        s += c;
    }
    return std::string();
}
bool load_signal_names()
{
    int n = 0;
    if (!IF2_HAVE(SIGNALS)) {
        if2_feature_off("game signals (checkpoint boot driver)", "the signal name table");
        g_nsig = 0;
        return false;
    }
    for (; n < kMaxSig; n++) {
        std::string s = image_cstr(vm_read32(kSigNames + 4u * (uint32_t)n), 48);
        if (s.empty() || s == "Sphere") break;          /* "Sphere" starts the next enum */
        g_sig_name[n] = s;
    }
    /* 1.04 appends one id (109, ContmodekChanged); ids 0..108 are the disc EBOOT's. Another build: the same
     * first ids, maybe more after them. */
    if ((IF2_EBOOT_KNOWN ? n != (IF2_EBOOT_V104 ? 110 : 109) : n < 109) || g_sig_name[0] != "AmbientSpeech" || g_sig_name[105] != "WarpedToCheckpoint" ||
        g_sig_name[48] != "MenuActivate") {
        CKLOG("the signal name table at 0x%08X is not that of %s (%d names) -- game "
              "signals off; the boot driver falls back to frame counts", kSigNames, IF2_EBOOT_NAME, n);
        g_nsig = 0;
        return false;
    }
    g_nsig = n;
    return true;
}

/* ids the driver watches (-1 when unknown) */
int S_MENU_ON = -1, S_MENU_OFF = -1, S_MOVIE_ON = -1, S_MOVIE_OFF = -1, S_WARP = -1,
    S_RESET = -1, S_PACK = -1, S_MISSION = -1;
std::vector<int> g_ready_ids;    /* CHECKPOINT_READY_ON */

std::vector<int> sig_list(const char* var, const char* dflt)
{
    std::vector<int> out;
    const std::string s = env(var) ? env(var) : dflt;
    for (size_t p = 0; p <= s.size();) {
        size_t e = s.find_first_of(",| ", p);
        if (e == std::string::npos) e = s.size();
        const std::string w = s.substr(p, e - p);
        const int id = w.empty() ? -1 : sig_id(w.c_str());
        if (id >= 0) out.push_back(id);
        else if (!w.empty()) CKLOG("%s: no signal named '%s'", var, w.c_str());
        p = e + 1;
    }
    return out;
}

uint32_t cnt(int id) { return id >= 0 ? g_sig_count[id].load(std::memory_order_relaxed) : 0; }
unsigned lastf(int id) { return id >= 0 ? g_sig_frame[id].load(std::memory_order_relaxed) : 0; }

} /* namespace */

/* Inserted by tools/ppu_postlift.py at the top of the lifted func_002F26B8:
 * r3 = the signal source object, r4 = signal id, r5 = the argument list. */
extern "C" void if2_signal_hook(ppu_context* ctx)
{
    if (!g_sig_on.load(std::memory_order_relaxed)) return;
    const uint32_t id = (uint32_t)ctx->gpr[4];
    if (id >= (uint32_t)g_nsig) return;
    const unsigned f = frames();
    const uint32_t n = g_sig_count[id].fetch_add(1, std::memory_order_relaxed) + 1;
    g_sig_frame[id].store(f, std::memory_order_relaxed);
    if (g_sig_issues) pi_signal(g_sig_name[id].c_str());
    if (!g_siglog) return;
    const bool show = g_siglog == 2 || (g_siglog == 3 && g_sig_sel[id]) ||
                      (g_siglog == 1 && (n <= 8 || n % 100 == 0));
    if (show)
        fprintf(stderr, "[if2-sig] f=%u t=%.1fs %s #%u src=0x%08X args=0x%08X\n", f, uptime(),
                g_sig_name[id].c_str(), n, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[5]);
}

namespace {

/* ------------------------------------------------------------ pad input */

std::atomic<uint32_t> g_press_mask{0}, g_press_from{0}, g_press_until{0};
std::atomic<uint32_t> g_ready_frame{0};          /* 0 = not ready yet */
double g_ready_uptime = 0;                       /* uptime() at READY, for ms script items */

struct ScriptItem { uint32_t f0, f1; int kind; uint32_t mask; uint8_t st[4]; bool ms; };  /* ms: f0/f1 in ms after READY */
std::vector<ScriptItem> g_script;                 /* frames relative to READY */
uint32_t g_script_end = 0;

void press(uint32_t mask, uint32_t hold, const char* why)
{
    const unsigned f = frames();
    g_press_mask.store(0);
    g_press_from.store(f + 1);
    g_press_until.store(f + 1 + hold);
    g_press_mask.store(mask);
    CKLOG("press 0x%04X for %u frames at frame %u (t=%.1fs): %s", mask, hold, f, uptime(), why);
}

bool parse_script(const char* s)
{
    while (s && *s) {
        while (*s == ' ' || *s == '|' || *s == ',' || *s == ';') s++;
        if (!*s) break;
        char* e;
        ScriptItem it{};
        const unsigned long a = strtoul(s, &e, 0);
        if (e == s) return false;
        if (*e == '-') {                                    /* f1-f2:lx,ly,rx,ry */
            const unsigned long b = strtoul(e + 1, &e, 0);
            if (e[0] == 'm' && e[1] == 's') { it.ms = true; e += 2; }
            if (*e != ':' || b <= a) return false;
            e++;
            for (int c = 0; c < 4; c++) {
                char* q;
                const long v = strtol(e, &q, 0);
                if (q == e) return false;
                it.st[c] = (uint8_t)std::min(255L, std::max(0L, v));
                e = q;
                if (c < 3) { if (*e != ',') return false; e++; }
            }
            it.kind = 1; it.f0 = (uint32_t)a; it.f1 = (uint32_t)b;
        } else if (*e == ':' || (e[0] == 'm' && e[1] == 's' && e[2] == ':')) {   /* f[ms]:mask[/hold] */
            if (*e == 'm') { it.ms = true; e += 2; }
            const unsigned long m = strtoul(e + 1, &e, 0);
            unsigned long hold = it.ms ? 100 : 6;
            if (*e == '/') hold = strtoul(e + 1, &e, 0);
            it.kind = 0; it.f0 = (uint32_t)a; it.f1 = (uint32_t)(a + (hold ? hold : 1)); it.mask = (uint32_t)m;
        } else return false;
        g_script.push_back(it);
        if (!it.ms) g_script_end = std::max(g_script_end, it.f1);
        s = e;
    }
    return true;
}

/* CELL_PAD_BTN_OFFSET_*: digital bit -> pressure word (the title sets port
 * setting 0x6, PRESS_ON, so the pressure words are live). */
const struct { uint8_t word, bit, press; } kPress[] = {
    {2, 0x20, 8}, {2, 0x80, 9}, {2, 0x10, 10}, {2, 0x40, 11},      /* RIGHT LEFT UP DOWN   */
    {3, 0x10, 12}, {3, 0x20, 13}, {3, 0x40, 14}, {3, 0x80, 15},    /* TRI CIRCLE CROSS SQ  */
    {3, 0x04, 16}, {3, 0x08, 17}, {3, 0x01, 18}, {3, 0x02, 19} };  /* L1 R1 L2 R2          */

void inject(uint32_t ea, uint32_t mask, const uint8_t* st)
{
    auto word = [&](int i) { return (uint64_t)ea + 4u + 2u * (uint32_t)i; };
    if (mask) {
        vm_write16(word(2), (uint16_t)(vm_read16(word(2)) | (mask & 0xFF)));
        vm_write16(word(3), (uint16_t)(vm_read16(word(3)) | ((mask >> 8) & 0xFF)));
        const int32_t len = (int32_t)vm_read32(ea);
        for (auto& p : kPress) {     /* len counts the valid u16 words (8, 20 or 24) */
            const uint32_t m = p.word == 2 ? p.bit : (uint32_t)p.bit << 8;
            if ((mask & m) && len > p.press) vm_write16(word(p.press), 255);
        }
    }
    if (st) {
        vm_write16(word(6), st[0]); vm_write16(word(7), st[1]);    /* left x, y  */
        vm_write16(word(4), st[2]); vm_write16(word(5), st[3]);    /* right x, y */
    }
}

/* cellPadGetData, then the driver's press and the READY script on port 0. A
 * packet with len 0 ("nothing new", cellPad.c) is left alone: the title
 * ignores its contents, and presses are timed in frames, not in packets. */
void if2_cellPadGetData(ppu_context* ctx)
{
    const uint32_t port = (uint32_t)ctx->gpr[3], ea = (uint32_t)ctx->gpr[4];
    const int32_t rc = cellPadGetData(port, (void*)(uintptr_t)ea);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    if (rc != 0 || port != 0 || !ea || (int32_t)vm_read32(ea) <= 0) return;
    if (options_intercepted()) return;   /* the system has the pad (options.h): neutral, scripts too */
    const unsigned f = frames();
    uint32_t mask = 0;
    const uint8_t* st = nullptr;
    const uint32_t pm = g_press_mask.load();
    if (pm && f >= g_press_from.load() && f < g_press_until.load()) mask |= pm;
    const uint32_t rf = g_ready_frame.load();
    if (rf && !g_script.empty()) {
        const uint32_t rel = f - rf;
        const double rel_ms = (uptime() - g_ready_uptime) * 1000.0;   /* set before g_ready_frame */
        for (auto& it : g_script) {
            if (it.ms ? (rel_ms < it.f0 || rel_ms >= it.f1) : (rel < it.f0 || rel >= it.f1)) continue;
            if (it.kind == 0) mask |= it.mask; else st = it.st;
        }
    }
    g_ftr_pad_rx = st ? (int)st[2] : (rf ? 128 : -1);   /* FRAME_TRACE: the scripted right stick */
    if (mask || st) inject(ea, mask, st);
}

/* ------------------------------------------------------------- boot driver */

enum Phase { P_BOOT, P_RUN, P_SETTLE, P_DONE };
std::atomic<int>      g_phase{P_BOOT};
std::atomic<int>      g_bootload{-1};    /* -1 not yet, 0 no save, 1 loaded, 2 failed, 3 not the sandbox */
std::atomic<unsigned> g_bootload_frame{0};
int32_t     g_bootload_rc = 0;           /* what ListAutoLoad returned */
std::string g_loaded_dir;                /* written before g_bootload is published */
double      g_t_bootload = -1, g_t_menu = -1, g_t_took = -1, g_t_ready = -1;
uint32_t    g_base[kMaxSig];
unsigned    g_presses = 0, g_last_press = 0, g_settle_from = 0, g_settle_start = 0;
unsigned    g_tj_frame = 0;              /* IF2_TASKJUMP reached its step at this frame */
std::string g_tj_what;
/* if2_checkpoint_taskjump_mark: the signal counts when the jump acted */
std::atomic<uint32_t> g_tj_mark_count[kMaxSig];
std::atomic<unsigned> g_tj_mark_frame{0};
bool        g_hooked = false;            /* a game signal has been seen since the boot load */
std::string g_ready_why, g_confidence;
std::atomic<int> g_exit_code{-1};
/* SS_LOAD (toolsun.ps1 -LoadState): the run starts from a save state loaded at
 * the game's first frame -- READY is that load's completion, the title flow is
 * not driven (no presses, no READY signal waited for). */
bool        g_ssload = false;

std::string signal_summary()
{
    std::string sum;
    for (int id : {S_MENU_ON, S_MENU_OFF, S_MOVIE_ON, S_MOVIE_OFF, S_RESET, S_PACK, S_MISSION, S_WARP})
        if (id >= 0) { char b[64]; snprintf(b, sizeof b, " %s=%u", g_sig_name[id].c_str(), cnt(id) - g_base[id]); sum += b; }
    return sum;
}
void write_ready_file(const char* status, const std::string& why)
{
    if (g_ready_file.empty()) return;
    const std::string tmp = g_ready_file + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { CKLOG("cannot write %s", g_ready_file.c_str()); return; }
    fprintf(f, "status=%s\n", status);
    if (!strcmp(status, "ready")) fprintf(f, "confidence=%s\n", g_confidence.c_str());
    fprintf(f, "checkpoint=%s\nboot=%s\nreason=%s\npid=%lu\nframe=%u\nuptime_s=%.1f\n", g_cp.c_str(),
            g_boot == BOOT_NEWGAME ? "newgame" : g_boot == BOOT_CONTINUE ? "continue" : "off",
            flat(why).c_str(), (unsigned long)GetCurrentProcessId(), frames(), uptime());
    fprintf(f, "save_dir=%s\nsandbox=%s\npresses=%u\n", g_loaded_dir.c_str(), g_sandbox.c_str(), g_presses);
    fprintf(f, "t_bootload_s=%.1f\nt_menu_s=%.1f\nt_took_s=%.1f\nt_ready_s=%.1f\n",
            g_t_bootload, g_t_menu, g_t_took, g_t_ready);
    if (g_nsig) fprintf(f, "signals=%s\n", signal_summary().c_str());
    {
        char tj[200] = "";
        if (if2_taskjump_status(tj, sizeof tj)) fprintf(f, "taskjump=%s\n", flat(tj).c_str());
    }
    if (!g_ready_shot.empty() && !strcmp(status, "ready")) fprintf(f, "screenshot=%s\n", g_ready_shot.c_str());
    fclose(f);
    MoveFileExA(tmp.c_str(), g_ready_file.c_str(), MOVEFILE_REPLACE_EXISTING);
}
void finish(const char* status, const std::string& why, const char* confidence = "")
{
    g_phase.store(P_DONE);
    g_confidence = confidence;
    if (!strcmp(status, "ready")) {
        g_t_ready = uptime();
        g_ready_uptime = g_t_ready;
        { LARGE_INTEGER q; QueryPerformanceCounter(&q); g_ftr_ready_qpc = q.QuadPart; }   /* FRAME_TRACE */
        g_ready_frame.store(std::max(1u, frames()));
        CKLOG("READY '%s' at frame %u, t=%.1fs (%s): %s; boot load t=%.1fs, presses %u;%s%s%s%s", g_cp.c_str(),
              frames(), uptime(), confidence, why.c_str(), g_t_bootload, g_presses,
              g_nsig ? (g_tj_frame ? " signals since the task jump:" : " signals since the boot load:") : "",
              signal_summary().c_str(), g_tj_frame ? "; IF2_TASKJUMP: " : "", g_tj_what.c_str());
        if (!g_ready_shot.empty()) screenshot_async(g_ready_shot);
        if (!g_script.empty()) CKLOG("CHECKPOINT_SCRIPT starts now (%zu item(s), last frame item ends at +%u frames;"
                                     " ms items are timed from t=%.3fs)", g_script.size(), g_script_end, g_t_ready);
    } else {
        CKLOG("%s '%s' at frame %u, t=%.1fs: %s;%s", status, g_cp.c_str(), frames(), uptime(), why.c_str(),
              signal_summary().c_str());
        g_exit_code.store(!strcmp(status, "fail") ? 4 : 5);
    }
    write_ready_file(status, why);
}
void snap_base() { for (int i = 0; i < g_nsig; i++) g_base[i] = cnt(i); }
bool since(int id) { return id >= 0 && cnt(id) > g_base[id]; }
bool any_signal_since() { for (int i = 0; i < g_nsig; i++) if (cnt(i) > g_base[i]) return true; return false; }
unsigned latest(std::initializer_list<int> ids)
{
    unsigned m = 0;
    for (int id : ids) if (since(id)) m = std::max(m, lastf(id));
    return m;
}

/* frames; the title runs 30-75 fps here, menus ~30-75, loading screens ~30 */
constexpr unsigned kMenuSettle = 45;     /* a menu up this long before CROSS            */
constexpr unsigned kConfirm    = 240;    /* at least this long between presses          */
constexpr unsigned kMaxPresses = 8;
constexpr unsigned kMenuStuck  = 900;    /* continue: a menu idle this long gets CROSS  */
constexpr unsigned kBlind      = 1200;   /* newgame: no menu signal, nothing loading    */
constexpr unsigned kSettle     = 60;     /* frames after a READY signal                 */
constexpr unsigned kSettleMax  = 3600;   /* ... however long a movie claims to run      */
constexpr unsigned kFallback   = 2400;   /* READY by frame count without signals        */
constexpr unsigned kQuiet      = 1200;   /* world loaded, nothing new this long         */

void tick()
{
    const unsigned f = frames();
    const int phase = g_phase.load();
    if (phase == P_DONE) return;
    if (uptime() > g_timeout) {
        char tj[200] = "";
        const int tjs = if2_taskjump_status(tj, sizeof tj);
        finish("timeout", phase == P_BOOT ? "the title never ran its boot-time save load"
                        : g_boot == BOOT_NEWGAME && !g_presses ? "no main menu was seen (no MenuActivate, CROSS never pressed)"
                        : tjs == 1 ? std::string("IF2_TASKJUMP never reached its step: ") + tj
                        : "no READY signal (CHECKPOINT_READY_ON) arrived");
        return;
    }
    if (g_ssload) return;                   /* READY comes from the save state's load (or its failure) */
    switch (phase) {
    case P_BOOT: {
        const int bl = g_bootload.load();
        if (bl < 0) return;
        snap_base();
        const unsigned bf = g_bootload_frame.load();
        if (bl == 3) {
            finish("fail", "cellSaveData did not use the sandbox: this build ignores PS3_SAVEDATA_DIR "
                           "(toolkit patch), so the run may have read the player's own saves");
            return;
        }
        if (g_boot == BOOT_CONTINUE && bl == 0) {
            finish("fail", "no save was loaded at boot: the checkpoint has no save directory the title "
                           "accepts, or this build's cellSaveData ignores PS3_SAVEDATA_DIR");
            return;
        }
        if (bl == 2) {
            char b[96];
            snprintf(b, sizeof b, "the title's boot-time save load failed (cellSaveDataListAutoLoad 0x%08X)",
                     (unsigned)g_bootload_rc);
            finish("fail", b);
            return;
        }
        if (bl == 1)
            CKLOG("save '%s' loaded at boot (frame %u, t=%.1fs); the title continues it by itself -- waiting for %s",
                  g_loaded_dir.c_str(), bf, uptime(), g_boot == BOOT_NEWGAME ? "gameplay (newgame asked, a save was loaded)"
                                                                            : "gameplay");
        else
            CKLOG("no save at boot (frame %u, t=%.1fs); waiting for the main menu", bf, uptime());
        g_phase.store(P_RUN);
        return;
    }
    case P_RUN: {
        /* IF2_TASKJUMP (src/if2_taskgraph.cpp): READY means "at its step", so
         * nothing before the jump counts. */
        char tj[200] = "";
        const int tjs = if2_taskjump_status(tj, sizeof tj);
        if (tjs == 3) { finish("fail", std::string("IF2_TASKJUMP gave up: ") + tj); return; }
        if (tjs == 2 && !g_tj_frame) {
            g_tj_what = tj;
            /* the counts from the moment it acted (its warp's WarpedToCheckpoint
             * may come before this thread looks), else from now */
            const unsigned mf = g_tj_mark_frame.load();
            if (mf) {
                g_tj_frame = mf;
                for (int i = 0; i < g_nsig; i++) g_base[i] = g_tj_mark_count[i].load();
            } else {
                g_tj_frame = std::max(1u, f);
                snap_base();
            }
            CKLOG("IF2_TASKJUMP at frame %u (t=%.1fs): %s; READY counts the game's signals from frame %u",
                  f, uptime(), tj, g_tj_frame);
        }
        const bool tj_wait = tjs == 1;
        for (int id : g_ready_ids)
            if (!tj_wait && since(id)) {
                g_ready_why = g_sig_name[id];
                g_settle_from = g_settle_start = std::max(lastf(id), f);
                g_phase.store(P_SETTLE);
                CKLOG("%s at frame %u (t=%.1fs); settling", g_ready_why.c_str(), lastf(id), uptime());
                return;
            }
        /* frames are counted from the boot load, or from the jump */
        const unsigned bf = std::max(g_bootload_frame.load(), g_tj_frame);
        if (g_nsig && any_signal_since()) g_hooked = true;
        const bool hooked = g_hooked;
        /* a new game only when no save was loaded: with one the title continues it */
        const bool newgame = g_boot == BOOT_NEWGAME && g_bootload.load() == 0;
        auto do_press = [&](const char* why) {
            g_last_press = f;
            g_presses++;
            press(0x4000, 6, why);
            if (g_t_took < 0) g_t_took = uptime();
        };
        /* No game signal at all since the boot load: another EBOOT, or this build
         * lacks the post-lift hook. Frame counts only -- the weakest READY. */
        if (!hooked) {
            if (newgame && g_presses < 3) {
                if (f > bf + kBlind && f >= g_last_press + kConfirm)
                    do_press("CROSS, blind: no game signals to find the main menu by");
                return;
            }
            if (!tj_wait && f > std::max(newgame ? g_last_press : 0u, bf) + kFallback)
                finish("ready", g_nsig ? "no game signals since the boot load (is if2_signal_hook in this build? "
                                         "tools/ppu_postlift.py) -- READY by frame count"
                                       : "game signals unavailable -- READY by frame count", "frames");
            return;
        }
        if (since(S_MENU_ON) && g_t_menu < 0) g_t_menu = uptime();
        /* The world loaded (packs / a world reset, or the task jump) and nothing
         * has happened since -- no pack, reset, movie or menu change -- yet no
         * READY signal. Not for a new game before its menu was answered: that is
         * the idle main menu. */
        const unsigned load_busy = latest({S_PACK, S_RESET, S_MOVIE_ON, S_MOVIE_OFF, S_WARP});
        const unsigned busy = std::max({load_busy, latest({S_MENU_ON, S_MENU_OFF}), g_tj_frame});
        if ((since(S_PACK) || since(S_RESET) || g_tj_frame) && !tj_wait && (!newgame || g_presses) &&
            f > busy + kQuiet && f > g_last_press + kQuiet) {
            char b[200];
            snprintf(b, sizeof b, "world loaded and quiet for %u frames, no %s", kQuiet,
                     g_ready_ids.empty() ? "READY signal" : g_sig_name[g_ready_ids[0]].c_str());
            finish("ready", b, "quiet");
            return;
        }
        /* A menu waiting for input: activated after the last deactivation, and
         * nothing loading since it came up. */
        if (g_presses >= kMaxPresses || (g_presses && f < g_last_press + kConfirm)) return;
        const unsigned mf = lastf(S_MENU_ON);
        const bool menu_active = since(S_MENU_ON) && (!since(S_MENU_OFF) || mf >= lastf(S_MENU_OFF));
        const bool menu_up = menu_active && latest({S_PACK, S_RESET, S_MOVIE_ON, S_WARP}) <= mf;
        if (newgame) {
            /* The title menu streams its background world in after it activates
             * (15 PackLoaded after MenuActivate in run-20261014-175308), so
             * "nothing loaded since the menu came up" never held and the run timed
             * out at the menu. For a new game: the menu is active and the packs
             * have been quiet for kMenuSettle frames. Movies are not counted --
             * a skipped FMV signals MovieStarted with no MovieFinished. */
            const unsigned packs = latest({S_PACK, S_RESET, S_WARP});
            if (menu_active && f >= std::max(mf, packs) + kMenuSettle)
                do_press(g_presses ? "CROSS: a menu is up again (difficulty / confirmation)"
                                   : "CROSS on the main menu (new game)");
            else if (!g_tj_frame && !since(S_MENU_ON) && g_presses < 3 && f > std::max(bf, load_busy) + kBlind &&
                     f >= g_last_press + kBlind)
                do_press("CROSS, blind: no menu signal, and nothing has loaded for a while");
        } else if (menu_up && f >= mf + kMenuStuck && f >= g_last_press + kMenuStuck && g_presses < 2) {
            do_press("CROSS: a menu has been up with nothing loading (the title waits for input)");
        }
        return;
    }
    case P_SETTLE: {
        /* not while a movie plays (started more often than finished), but not forever */
        if (S_MOVIE_ON >= 0 && S_MOVIE_OFF >= 0 && f < g_settle_start + kSettleMax &&
            cnt(S_MOVIE_ON) - g_base[S_MOVIE_ON] > cnt(S_MOVIE_OFF) - g_base[S_MOVIE_OFF]) {
            g_settle_from = f;
            return;
        }
        if (f >= g_settle_from + kSettle) finish("ready", g_ready_why, "signal");
        return;
    }
    default:
        return;
    }
}

[[noreturn]] void quit(int code, const char* why)
{
    CKLOG("exiting (%s), code %d", why, code);
    padhost_shutdown();
    ss_request_summary("exit");   /* (ExitProcess skips its atexit) */
    fflush(stdout); fflush(stderr);
    ExitProcess((UINT)code);
}

DWORD WINAPI driver_thread(LPVOID)
{
    for (;;) {
        tick();
        if (g_phase.load() == P_DONE) {
            const int ec = g_exit_code.load();
            if (g_exit_after < 0) break;
            if (ec >= 0) quit(ec, "CHECKPOINT_EXIT_AFTER: the boot did not reach READY");
            const uint32_t rf = g_ready_frame.load();
            if (rf && (g_exit_after_ms >= 0 ? uptime() * 1000.0 >= g_t_ready * 1000.0 + (double)g_exit_after_ms
                                            : frames() >= rf + (unsigned)g_exit_after)) {
                Sleep(300);                     /* let a READY screenshot finish */
                quit(0, "CHECKPOINT_EXIT_AFTER frames after READY");
            }
            if (rf && uptime() > g_t_ready + g_timeout)
                quit(5, "CHECKPOINT_EXIT_AFTER: the frames after READY never came (CHECKPOINT_TIMEOUT)");
        }
        Sleep(10);
    }
    return 0;
}

/* -------------------------------------------------------- save snapshots */

struct SaveImage {
    bool valid = false;
    std::string dir, subtitle, detail, title;
    std::vector<std::pair<std::string, std::string>> files;   /* name, bytes */
};
SaveImage g_ref;                 /* the save last loaded or kept */
SaveImage g_boot_save;           /* the save loaded at boot */
CRITICAL_SECTION g_snap_lock;

SaveImage read_save(const std::string& host, const std::string& dir)
{
    SaveImage s;
    s.dir = dir;
    for (auto& f : list_dir(host, false)) {
        std::string b;
        if (read_file(join(host, f), b)) s.files.push_back({f, b});
    }
    for (auto& f : s.files)
        if (!_stricmp(f.first.c_str(), "PARAM.SFO")) {
            s.subtitle = sfo_get(f.second, "SUB_TITLE");
            s.detail = sfo_get(f.second, "DETAIL");
            s.title = sfo_get(f.second, "TITLE");
        }
    s.valid = !s.files.empty();
    return s;
}
/* Bytes that differ between two saves' own files, or -1 when they are not the
 * same file set. PARAM.SFO / PARAM.PFD belong to the save-data service: a save
 * from RPCS3 and its re-save by this runtime can differ there for the same
 * point in the game. */
long save_diff(const SaveImage& a, const SaveImage& b)
{
    if (!a.valid || !b.valid || _stricmp(a.dir.c_str(), b.dir.c_str())) return -1;
    auto own = [](const SaveImage& s) {
        std::vector<const std::pair<std::string, std::string>*> v;
        for (auto& f : s.files)
            if (_stricmp(f.first.c_str(), "PARAM.SFO") && _stricmp(f.first.c_str(), "PARAM.PFD")) v.push_back(&f);
        return v;
    };
    const auto x = own(a), y = own(b);
    if (x.size() != y.size() || x.empty()) return -1;
    long d = 0;
    for (size_t i = 0; i < x.size(); i++) {
        if (_stricmp(x[i]->first.c_str(), y[i]->first.c_str()) || x[i]->second.size() != y[i]->second.size()) return -1;
        for (size_t k = 0; k < x[i]->second.size(); k++) d += x[i]->second[k] != y[i]->second[k];
    }
    return d;
}
bool same_title(const SaveImage& a, const SaveImage& b)
{
    return a.valid && b.valid && a.subtitle == b.subtitle && first_line(a.detail) == first_line(b.detail);
}

constexpr unsigned kBootGrace = 1800;    /* frames after the boot load: "continue" saves */

void write_meta(const std::string& root, const std::string& name, const std::string& source,
                const std::string& api, const SaveImage& s, const std::string& screen)
{
    std::string desc = s.subtitle;
    const std::string d1 = first_line(s.detail);
    if (!d1.empty()) desc += (desc.empty() ? "" : " | ") + d1;
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    FILE* m = fopen(join(root, "meta.json").c_str(), "wb");
    if (!m) return;
    fprintf(m, "{\n");
    fprintf(m, "  \"name\": %s,\n", json_str(name).c_str());
    fprintf(m, "  \"description\": %s,\n", json_str(desc).c_str());
    fprintf(m, "  \"created\": %s,\n", json_str(now_stamp(true)).c_str());
    fprintf(m, "  \"source\": %s,\n", json_str(source).c_str());
    fprintf(m, "  \"api\": %s,\n", json_str(api).c_str());
    fprintf(m, "  \"dir\": %s,\n", json_str(s.dir).c_str());
    fprintf(m, "  \"title\": %s,\n", json_str(s.title).c_str());
    fprintf(m, "  \"subtitle\": %s,\n", json_str(s.subtitle).c_str());
    fprintf(m, "  \"detail\": %s,\n", json_str(s.detail).c_str());
    fprintf(m, "  \"frame\": %u,\n", frames());
    fprintf(m, "  \"uptime_s\": %.1f,\n", uptime());
    fprintf(m, "  \"booted_from\": %s,\n", json_str(g_cp).c_str());
    fprintf(m, "  \"exe\": %s,\n", json_str(exe).c_str());
    fprintf(m, "  \"screen\": %s\n", json_str(screen).c_str());
    fprintf(m, "}\n");
    fclose(m);
}

/* auto-NNNN: one past the highest number in the library; the directory is
 * created here, so two runs saving at once cannot take the same one. */
std::string new_auto_entry(std::string& root)
{
    int mx = 0;
    for (auto& d : list_dir(g_lib, true))
        if (!_strnicmp(d.c_str(), "auto-", 5)) mx = std::max(mx, atoi(d.c_str() + 5));
    mkdirs(g_lib);
    for (int n = mx + 1; n < mx + 1000; n++) {
        char name[32];
        snprintf(name, sizeof name, "auto-%04d", n);
        root = join(g_lib, name);
        if (CreateDirectoryA(root.c_str(), nullptr)) return name;
    }
    return "";
}

void keep_save(const std::string& api, const std::string& dir, const std::string& host)
{
    SaveImage cur = read_save(host, dir);
    if (!cur.valid) { CKLOG("%s: '%s' has no files on disk (%s) -- not kept", api.c_str(), dir.c_str(), host.c_str()); return; }
    const char* skip = nullptr;
    char why[160] = "";
    const unsigned bf = g_bootload_frame.load();
    if (g_boot != BOOT_OFF && !g_ready_frame.load()) {
        skip = "saved during the title flow: the same point as the save it booted from";
    } else if (g_bootload.load() == 1 && frames() < bf + kBootGrace && same_title(g_boot_save, cur)) {
        skip = "the title's save on continuing the save it loaded at boot";
    } else {
        const long d = save_diff(g_ref, cur);
        if (g_dedup >= 0 && d >= 0 && d <= g_dedup && same_title(g_ref, cur)) {
            snprintf(why, sizeof why, "%ld byte(s) differ from the last save loaded or kept, same title", d);
            skip = why;
        }
    }
    g_ref = cur;
    if (skip) { CKLOG("%s '%s' (%s) -- not kept: %s", api.c_str(), dir.c_str(), cur.subtitle.c_str(), skip); return; }

    std::string root;
    const std::string name = new_auto_entry(root);
    if (name.empty()) { CKLOG("cannot create an auto-NNNN entry in %s", g_lib.c_str()); return; }
    const std::string dst = join(join(root, "savedata"), dir);
    mkdirs(dst);
    int nfiles = 0;
    for (auto& f : cur.files)
        if (FILE* o = fopen(join(dst, f.first).c_str(), "wb")) {
            nfiles += fwrite(f.second.data(), 1, f.second.size(), o) == f.second.size();
            fclose(o);
        }
    write_meta(root, name, "autosave", api, cur, "screen.png");
    screenshot_async(join(root, "screen.png"));
    CKLOG("kept the game's save as '%s': %s | %s  (%d file(s), %s)", name.c_str(), cur.subtitle.c_str(),
          first_line(cur.detail).c_str(), nfiles, root.c_str());
}

/* --- the title's save-data imports, wrapped -------------------------------- */

void if2_ListAutoLoad(ppu_context* ctx)
{
    /* CellSaveDataSetList: sortType, sortOrder, dirNamePrefix (guest pointers, BE) */
    const uint32_t setList = (uint32_t)ctx->gpr[5];
    const std::string prefix = setList ? guest_str(vm_read32(setList + 8), 64) : "";
    const uint32_t before = save_ops();
    ps3_savedata_list_auto_load(ctx);
    const int32_t rc = (int32_t)ctx->gpr[3];
    const SaveOp op = save_op_since(before);
    /* where cellSaveData looked (fixed once it has run) -- a checkpoint run
     * whose cellSaveData ignored PS3_SAVEDATA_DIR read the player's saves */
    const std::string root = savedata_root();
    const bool wrong_root = !g_sandbox.empty() && _stricmp(root.c_str(), g_sandbox.c_str()) != 0;
    const bool loaded = rc == 0 && op.done;
    EnterCriticalSection(&g_snap_lock);
    if (loaded) g_boot_save = g_ref = read_save(join(root, op.dir), op.dir);
    g_loaded_dir = op.done ? op.dir : "";
    g_t_bootload = uptime();
    g_bootload_frame.store(frames());
    g_bootload_rc = rc;
    g_bootload.store(wrong_root ? 3 : rc != 0 ? 2 : loaded ? 1 : 0);
    LeaveCriticalSection(&g_snap_lock);
    CKLOG("boot-time save load (ListAutoLoad, prefix '%s') -> 0x%08X: %s%s%s  [%s]", prefix.c_str(), (unsigned)rc,
          rc != 0 ? "failed" : loaded ? "loaded '" : "no save loaded", loaded ? op.dir.c_str() : "",
          loaded ? "'" : "", root.c_str());
    if (wrong_root)
        CKLOG("ERROR: cellSaveData read %s, not this run's sandbox %s -- PS3_SAVEDATA_DIR was ignored",
              root.c_str(), g_sandbox.c_str());
}

/* A save the title just wrote: keep it (CHECKPOINT_AUTOSNAP) or remember it as
 * the reference the next one is compared with. */
void after_save(const char* api, int32_t rc, const SaveOp& op)
{
    if (rc != 0 || !op.done) return;
    if (!op.wrote) { CKLOG("%s '%s': nothing written (unchanged, or PS3_SAVEDATA_READONLY)", api, op.dir.c_str()); return; }
    const std::string root = savedata_root();
    EnterCriticalSection(&g_snap_lock);
    if (g_autosnap && !g_lib.empty()) keep_save(api, op.dir, join(root, op.dir));
    else g_ref = read_save(join(root, op.dir), op.dir);
    LeaveCriticalSection(&g_snap_lock);
}

#define GP(i) ((void*)(uintptr_t)(uint32_t)ctx->gpr[i])

void if2_AutoSave2(ppu_context* ctx)
{
    const uint32_t before = save_ops();
    const int32_t rc = cellSaveDataAutoSave2((uint32_t)ctx->gpr[3], (const char*)GP(4), (uint32_t)ctx->gpr[5],
                                             GP(6), GP(7), GP(8), (uint32_t)ctx->gpr[9], GP(10));
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    after_save("AutoSave2", rc, save_op_since(before));
}
void if2_ListSave2(ppu_context* ctx)
{
    const uint32_t before = save_ops();
    const int32_t rc = cellSaveDataListSave2((uint32_t)ctx->gpr[3], GP(4), GP(5), GP(6), GP(7), GP(8),
                                             (uint32_t)ctx->gpr[9], GP(10));
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    after_save("ListSave2", rc, save_op_since(before));
}
void if2_ListLoad2(ppu_context* ctx)
{
    const uint32_t before = save_ops();
    const int32_t rc = cellSaveDataListLoad2((uint32_t)ctx->gpr[3], GP(4), GP(5), GP(6), GP(7), GP(8),
                                             (uint32_t)ctx->gpr[9], GP(10));
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    const SaveOp op = save_op_since(before);
    if (rc != 0 || !op.done) return;
    const std::string root = savedata_root();
    EnterCriticalSection(&g_snap_lock);
    g_ref = read_save(join(root, op.dir), op.dir);       /* loaded from the menu: the new reference */
    LeaveCriticalSection(&g_snap_lock);
    CKLOG("the title loaded '%s' from its menu (%s)", op.dir.c_str(), g_ref.subtitle.c_str());
}

#undef GP

/* ------------------------------------------------------------ the sandbox */

/* Copy checkpoint <name> into a fresh sandbox directory and point cellSaveData
 * at it. "@new" makes an empty one: no save, so the title offers a new game. */
bool make_sandbox(const std::string& name)
{
    const bool fresh = name == "@new";
    const std::string src = join(g_lib, name);
    if (!fresh && !is_dir(src)) {
        CKLOG("FATAL: no checkpoint '%s' in %s (tools\\checkpoint.ps1 list)", name.c_str(), g_lib.c_str());
        return false;
    }
    const std::string runs = join(g_lib, "_runs");
    {   /* keep the newest CHECKPOINT_KEEP_RUNS sandboxes made by earlier runs */
        long keep = env("CHECKPOINT_KEEP_RUNS") ? strtol(env("CHECKPOINT_KEEP_RUNS"), nullptr, 0) : 8;
        if (keep < 1) keep = 1;
        std::vector<std::pair<ULONGLONG, std::string>> v;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(join(runs, "*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == '.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                v.push_back({((ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime, fd.cFileName});
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::sort(v.begin(), v.end());
        for (size_t i = 0; i + (size_t)keep <= v.size(); i++) remove_tree(join(runs, v[i].second));
    }
    std::string tag = fresh ? "new" : name;
    for (auto& c : tag) if (c == '\\' || c == '/' || c == ':') c = '_';
    /* the save paths under it must stay inside MAX_PATH (ANSI file APIs) */
    if (tag.size() > 40) tag.resize(40);
    /* a directory of its own: never one an earlier run left files in */
    mkdirs(runs);
    std::string run;
    for (int k = 0; k < 100; k++) {
        char sfx[80];
        snprintf(sfx, sizeof sfx, k ? "-%s-%lu-%d" : "-%s-%lu", now_stamp(false).c_str(),
                 (unsigned long)GetCurrentProcessId(), k);
        run = join(runs, tag + sfx);
        if (CreateDirectoryA(run.c_str(), nullptr)) break;
        run.clear();
    }
    if (run.empty()) { CKLOG("FATAL: cannot create a sandbox in %s", runs.c_str()); return false; }
    const std::string root = join(run, "savedata");
    mkdirs(root);
    int n = 0;
    std::string how;
    if (fresh) {
        how = "empty (new game)";
    } else if (is_dir(join(src, "savedata"))) {
        n = copy_tree(join(src, "savedata"), root);
        how = "savedata\\*";
    } else if (is_file(join(src, "PARAM.SFO"))) {
        /* flat: the save directory's own files; its name from meta.json "dir",
         * the binary SFO's SAVEDATA_DIRECTORY, or the title's one save slot */
        std::string dir, sfo, meta;
        if (read_file(join(src, "meta.json"), meta)) {
            const size_t k = meta.find("\"dir\"");
            if (k != std::string::npos) {
                const size_t a = meta.find('"', meta.find(':', k) + 1), b = a == std::string::npos ? a : meta.find('"', a + 1);
                if (a != std::string::npos && b != std::string::npos) dir = meta.substr(a + 1, b - a - 1);
            }
        }
        if (dir.empty() && read_file(join(src, "PARAM.SFO"), sfo)) dir = sfo_get(sfo, "SAVEDATA_DIRECTORY");
        if (dir.empty()) dir = IF2_TITLE_ID "_INFAMOUS2_0";     /* the game names its saves after its TITLE_ID */
        const std::string d = join(root, dir);
        mkdirs(d);
        for (auto& f : list_dir(src, false)) {
            if (!_stricmp(f.c_str(), "meta.json") || !_strnicmp(f.c_str(), "screen.", 7)) continue;
            n += copy_file_rw(join(src, f), join(d, f)) ? 1 : 0;
        }
        how = "flat -> " + dir;
    } else {
        CKLOG("FATAL: checkpoint '%s' (%s) has neither savedata\\ nor PARAM.SFO", name.c_str(), src.c_str());
        return false;
    }
    _putenv_s("PS3_SAVEDATA_DIR", root.c_str());
    g_sandbox = full_path(root);
    CKLOG("booting from '%s': %d file(s) copied (%s) into the sandbox %s",
          name.c_str(), n, how.c_str(), g_sandbox.c_str());
    return true;
}

/* <project>\checkpoints: walk up from the exe (build\ or build\Release\) to the
 * folder holding tools\checkpoint.ps1 or src\infamous2_hooks.cpp. */
std::string default_library()
{
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    std::string d = exe;
    for (int up = 0; up < 4; up++) {
        const size_t s = d.find_last_of("\\/");
        if (s == std::string::npos) break;
        d.resize(s);
        if (is_file(join(d, "tools\\checkpoint.ps1")) || is_file(join(d, "src\\infamous2_hooks.cpp")))
            return join(d, "checkpoints");
    }
    return full_path("checkpoints");
}

} /* namespace */

/* What the game is doing now, from its signals (src/if2_ss_title.cpp: a
 * save-state slot's game mode; P3SS_MODE_* in runtime/savestate/ss_file.h):
 * 0 unknown (signals off: no checkpoint run, IF2_SIGLOG or play mode),
 * 1 gameplay, 2 pause menu, 3 title menu, 4 movie, 5 cutscene, 6 loading. */
extern "C" uint32_t if2_checkpoint_game_mode(void)
{
    if (!g_sig_on.load() || !g_nsig) return 0;
    const int s_poly_on = sig_id("PlayPolycut"), s_poly_off = sig_id("StopPolycut");
    const int s_world = sig_id("WarpedToCheckpoint"), s_msn = sig_id("MissionStart");
    const int mon = sig_id("MovieStarted"), moff = sig_id("MovieFinished");
    const int menu_on = sig_id("MenuActivate"), menu_off = sig_id("MenuDeactivate");
    if (mon >= 0 && moff >= 0 && cnt(mon) > cnt(moff)) return 4;
    if (s_poly_on >= 0 && s_poly_off >= 0 && cnt(s_poly_on) > cnt(s_poly_off)) return 5;
    if (menu_on >= 0 && cnt(menu_on) && (menu_off < 0 || lastf(menu_on) >= lastf(menu_off)))
        return (cnt(s_world) || cnt(s_msn)) ? 2 : 3;
    const int reset = sig_id("ResetWorldPre"), pack = sig_id("PackLoaded");
    if (reset >= 0 && cnt(reset) && lastf(reset) + 120 > frames() && !(s_world >= 0 && lastf(s_world) >= lastf(reset)))
        return 6;
    (void)pack;
    return 1;
}

extern "C" void if2_checkpoint_taskjump_mark(void)
{
    if (g_tj_mark_frame.load()) return;
    for (int i = 0; i < g_nsig; i++) g_tj_mark_count[i].store(cnt(i));
    g_tj_mark_frame.store(std::max(1u, frames()));
}

/* Save states (engine design 6.2, title hooks): a load put back ANOTHER
 * moment of the game than the one it stopped at -- an A-B selftest's A, a
 * slot; a round trip puts back the same moment and does not come here (the
 * TITL section in infamous2_hooks.cpp tells them apart, and calls this once
 * the world runs again). What the boot driver was waiting for no longer
 * describes the game: READY counts as reached (the script and
 * CHECKPOINT_EXIT_AFTER count from now). The autosnap baseline -- the save
 * last loaded or kept, and the boot save's -- belonged to the other moment:
 * the next save the game writes is compared with nothing, so it is kept.
 * The signal counters continue. */
extern "C" void if2_checkpoint_ss_loaded(void)
{
    EnterCriticalSection(&g_snap_lock);
    g_ref = SaveImage();
    g_boot_save = SaveImage();
    LeaveCriticalSection(&g_snap_lock);
    if (g_boot != BOOT_OFF && g_phase.load() != P_DONE)
        finish("ready", "a save state of another moment was loaded", "load");
}

/* Called from infamous2_install_overrides(): after the EBOOT is loaded, before
 * any guest code runs and before the runtime registers its own handlers (the
 * first registration of a NID wins). */
extern "C" int if2_checkpoint_install(void)
{
    g_t0 = GetTickCount64();
    InitializeCriticalSection(&g_snap_lock);
    if (const char* v = env("CHECKPOINT")) g_cp = v;
    if (const char* v = env("CHECKPOINT_DEDUP_BYTES")) g_dedup = strtol(v, nullptr, 0);
    if (const char* v = env("CHECKPOINT_TIMEOUT")) g_timeout = atof(v);
    if (const char* v = env("CHECKPOINT_READY_FILE")) g_ready_file = full_path(v);
    if (const char* v = env("CHECKPOINT_READY_SHOT")) g_ready_shot = full_path(v);
    if (const char* v = env("CHECKPOINT_EXIT_AFTER")) {
        char* q = nullptr;
        g_exit_after = strtol(v, &q, 0);
        if (q && q[0] == 'm' && q[1] == 's') g_exit_after_ms = g_exit_after;
        else if (q && q[0] == 's') g_exit_after_ms = g_exit_after * 1000;
    }

    g_boot = g_cp.empty() ? BOOT_OFF : g_cp == "@new" ? BOOT_NEWGAME : BOOT_CONTINUE;
    if (const char* v = env("IF2_BOOT")) {
        if (!_stricmp(v, "continue")) g_boot = BOOT_CONTINUE;
        else if (!_stricmp(v, "newgame") || !_stricmp(v, "new")) g_boot = BOOT_NEWGAME;
        else if (!_stricmp(v, "off") || !strcmp(v, "0")) g_boot = BOOT_OFF;
        else CKLOG("IF2_BOOT=%s: expected continue, newgame or off", v);
    }
    /* keeping the game's saves is opt-in, in every kind of run */
    if (const char* v = env("CHECKPOINT_AUTOSNAP")) g_autosnap = *v != '0';
    const char* sl = env("IF2_SIGLOG");
    const bool issues = pi_active() != 0;     /* play mode: the game's events for the issues log */
    if (issues) pi_set_mark_hook([](int, const char* shot) { screenshot_async(shot); });
    if (g_boot == BOOT_OFF && !g_autosnap && !sl && g_cp.empty() && !issues)
        return 0;                       /* nothing asked for: the runtime's own paths */
    g_lib = full_path(env("CHECKPOINT_DIR") ? env("CHECKPOINT_DIR") : default_library());

    if (!g_cp.empty()) {
        write_ready_file("booting", "");
        if (!make_sandbox(g_cp)) {
            write_ready_file("fail", "no such checkpoint: " + g_cp + " in " + g_lib);
            fflush(stdout); fflush(stderr);
            ExitProcess(3);
        }
        /* cellSaveData fixes its directory at the first call: this is it */
        const std::string used = savedata_root();
        if (_stricmp(used.c_str(), g_sandbox.c_str()) != 0) {
            const std::string why = "cellSaveData uses " + used + ", not the sandbox " + g_sandbox +
                                    " (was it called before the install?)";
            CKLOG("FATAL: %s", why.c_str());
            write_ready_file("fail", why);
            fflush(stdout); fflush(stderr);
            ExitProcess(4);
        }
    }

    /* The story FMVs and the logo splash: nothing a driven test needs. The
     * title treats a movie it cannot open as finished (MOVIE_SKIP in
     * runtime/ppu/ppu_fs.cpp; v_00_intro_montage the same way). The splash
     * (splash_<region>.bik, 352 frames = 11.7 s of logos) plays before the
     * world load starts, so skipping it moves READY 11.5 s earlier (New Marais
     * 46.2 -> 34.7 s, run-20261021-222114); the world loads and plays the same. */
    if (g_boot != BOOT_OFF) {
        const char* sm = env("CHECKPOINT_SKIP_MOVIES");
        std::string add;
        if (!sm || !_stricmp(sm, "fmv") || !_stricmp(sm, "1") || !_stricmp(sm, "all")) add = "movies/cutscene/v_";
        if (!sm || !_stricmp(sm, "all")) add += ",movies/cutscene/splash_";
        if (!add.empty()) {
            const std::string ms = env("MOVIE_SKIP") ? std::string(env("MOVIE_SKIP")) + "," + add : add;
            _putenv_s("MOVIE_SKIP", ms.c_str());
            CKLOG("MOVIE_SKIP=%s", ms.c_str());
        }
    }

    /* game signals */
    if ((g_boot != BOOT_OFF || sl || issues) && load_signal_names()) {
        g_sig_issues = issues;
        if (sl) {
            if (!_stricmp(sl, "all")) g_siglog = 2;
            else if (!strcmp(sl, "1")) g_siglog = 1;
            else {
                g_siglog = 3;
                const std::string s = sl;
                for (size_t p = 0; p <= s.size();) {
                    size_t e = s.find_first_of(",| ", p);
                    if (e == std::string::npos) e = s.size();
                    const std::string w = s.substr(p, e - p);
                    const int id = w.empty() ? -1 : sig_id(w.c_str());
                    if (id >= 0) g_sig_sel[id] = true;
                    else if (!w.empty()) CKLOG("IF2_SIGLOG: no signal named '%s'", w.c_str());
                    p = e + 1;
                }
            }
        }
        S_MENU_ON = sig_id("MenuActivate");    S_MENU_OFF = sig_id("MenuDeactivate");
        S_MOVIE_ON = sig_id("MovieStarted");   S_MOVIE_OFF = sig_id("MovieFinished");
        S_WARP = sig_id("WarpedToCheckpoint"); S_RESET = sig_id("ResetWorldPre");
        S_PACK = sig_id("PackLoaded");         S_MISSION = sig_id("MissionStart");
        g_ready_ids = sig_list("CHECKPOINT_READY_ON", "WarpedToCheckpoint");
        g_sig_on.store(1);
        CKLOG("game signals on (%d names from 0x%08X)%s", g_nsig, kSigNames, g_siglog ? "; IF2_SIGLOG logging" : "");
    }

    if (const char* sc = env("CHECKPOINT_SCRIPT")) {
        if (!parse_script(sc)) { CKLOG("CHECKPOINT_SCRIPT: cannot parse '%s' -- ignored", sc); g_script.clear(); }
        else CKLOG("CHECKPOINT_SCRIPT: %zu item(s), frames counted from READY", g_script.size());
        if (g_boot == BOOT_OFF && !g_script.empty()) CKLOG("CHECKPOINT_SCRIPT needs CHECKPOINT (READY starts it)");
    }

    int n = 0;
    if (g_boot != BOOT_OFF || g_autosnap) {
        ps3_hle_register_ctx(ps3_compute_nid("cellSaveDataListAutoLoad"), "cellSaveDataListAutoLoad", if2_ListAutoLoad);
        ps3_hle_register_ctx(ps3_compute_nid("cellSaveDataAutoSave2"), "cellSaveDataAutoSave2", if2_AutoSave2);
        ps3_hle_register_ctx(ps3_compute_nid("cellSaveDataListSave2"), "cellSaveDataListSave2", if2_ListSave2);
        ps3_hle_register_ctx(ps3_compute_nid("cellSaveDataListLoad2"), "cellSaveDataListLoad2", if2_ListLoad2);
        n += 4;
        CKLOG("the game's saves are %s (CHECKPOINT_AUTOSNAP=%d)",
              g_autosnap ? ("kept as auto-NNNN in " + g_lib).c_str() : "not kept", g_autosnap);
    }
    if (g_boot != BOOT_OFF && env("SS_LOAD")) {
        g_ssload = true;
        ss_slots_set_launch_hook([](int ok, const char* what) {
            if (g_phase.load() == P_DONE) return;
            if (ok) finish("ready", std::string("the save state ") + (what ? what : "") + " was loaded", "load");
            else finish("fail", std::string("the save state was not loaded: ") + (what ? what : "?"));
        });
        CKLOG("SS_LOAD=%s: READY is the save state's load at the game's first frame (the title flow is not driven)",
              env("SS_LOAD"));
    }
    if (g_boot != BOOT_OFF) {
        ps3_hle_register_ctx(ps3_compute_nid("cellPadGetData"), "cellPadGetData", if2_cellPadGetData);
        n++;
        if (HANDLE h = CreateThread(nullptr, 0, driver_thread, nullptr, 0, nullptr)) CloseHandle(h);
        CKLOG("boot driver: %s, checkpoint '%s', READY on %s, timeout %.0fs, library %s",
              g_boot == BOOT_NEWGAME ? "newgame" : "continue", g_cp.c_str(),
              env("CHECKPOINT_READY_ON") ? env("CHECKPOINT_READY_ON") : "WarpedToCheckpoint", g_timeout, g_lib.c_str());
    }
    return n;
}

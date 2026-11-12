/*
 * inFamous 2 -- the game's own trophies as Recomp Achievements (RecompLauncher). OFF by default.
 *
 * The game unlocks its trophies through sceNpTrophyUnlockTrophy (52 trophies, ids 0-51; 0 is the platinum, which
 * a PS3 awards by itself and the game never asks for). The toolkit's HLE (libs/network/sceNpTrophy.c) records an
 * unlock in gamedata/trophies/<communication id>.json and logs it. With this on, each unlock the HLE accepted is
 * also passed to RecompLauncher as a "native" achievement over the channel its Recomp Achievements documentation
 * describes (github.com/elripalda/RecompAchievements, docs/SPEC.md, channel version 1): RecompLauncher starts the
 * game with RL_SESSION = a pipe name; one JSON object per line each way. This file is our own code speaking that
 * documented format -- nothing of theirs is copied or linked in -- and it sends only:
 *   {"t":"hello","sdk":"if2-native-1","console":"ps3","memory":false,"game":"infamous2-recomp"}   on connect
 *   {"t":"native","id":<trophy id>,"earlier":true}   for each trophy the trophy file already has, when the game
 *                                                     registers its trophy context (the player had it before)
 *   {"t":"native","id":<trophy id>}                  for each unlock the HLE accepts while playing
 * Lines from the launcher (quiet, list) are read and logged; there is no toast of our own to hide, and the game's
 * trophies need no memory rules.
 *
 * Switched by the settings store's row `achievements` ([system], options.ini; env IF2_ACHIEVEMENTS=1 locks it on).
 * Off: nothing here runs and no HLE entry is replaced -- the trophy calls are exactly as before. On but started
 * without RecompLauncher (no RL_SESSION): one log line, nothing replaced. Read once at start.
 * Log lines: [rl-ach].
 */
#include <windows.h>

#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "ps3emu/nid.h"
#include "ps3emu/options.h"
#include "ppu_context.h"

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
/* The HLE handlers (libs/network/sceNpTrophy.c), called the way the HLE dispatcher calls a plain handler
 * (runtime/ppu/ppu_hle.cpp: eight GPR arguments, the result into r3), so the wrapper changes nothing for the game. */
extern "C" void sceNpTrophyRegisterContext(void);
extern "C" void sceNpTrophyUnlockTrophy(void);
typedef uint64_t (*if2_hle_generic)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static const char* const kGameId = "infamous2-recomp";   /* this port's RecompLauncher id (recomplauncher.com/game/...) */

static const opt_def kAchRow =
    { "achievements", "recomp_achievements", "options", "system", "System", "RecompLauncher", "Recomp Achievements",
      "Passes the game's own trophy unlocks to RecompLauncher as achievements, when RecompLauncher started the game "
      "(otherwise it does nothing). The trophies are still kept in gamedata\\trophies as before. Takes effect at the "
      "next launch.",
      OPT_BOOL, OPT_RESTART, nullptr, "Off|On", 0, 0, 0, "0", "IF2_ACHIEVEMENTS", nullptr,
      { nullptr, nullptr, nullptr, nullptr, nullptr }, OPT_F_ADVANCED, "if2", nullptr };

namespace {

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<std::string> g_queue;        /* lines to send; "" = read the trophy files and send the earlier unlocks */
std::wstring g_pipe;
HANDLE g_h = INVALID_HANDLE_VALUE;
bool g_earlier_sent = false;

void logf(const char* fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[rl-ach] %s\n", b);
    fflush(stderr);
}

void push(std::string line)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_queue.push_back(std::move(line));
    }
    g_cv.notify_one();
}

/* One overlapped write of the whole buffer (the handle is overlapped so the reader's pending read does not
 * hold writes back). */
bool write_all(HANDLE h, const std::string& s)
{
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n = 0;
    BOOL ok = WriteFile(h, s.data(), (DWORD)s.size(), nullptr, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = TRUE;
    if (ok) ok = GetOverlappedResult(h, &ov, &n, TRUE);
    CloseHandle(ov.hEvent);
    return ok && n == s.size();
}

void reader(HANDLE h)
{
    std::string acc;
    char buf[1024];
    for (;;) {
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        BOOL ok = ReadFile(h, buf, sizeof buf, nullptr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = TRUE;
        if (ok) ok = GetOverlappedResult(h, &ov, &n, TRUE);
        CloseHandle(ov.hEvent);
        if (!ok || n == 0) break;
        acc.append(buf, n);
        size_t e;
        while ((e = acc.find('\n')) != std::string::npos) {
            std::string l = acc.substr(0, e);
            acc.erase(0, e + 1);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l.size() > 160) l = l.substr(0, 160) + "...";
            logf("launcher: %s", l.c_str());
        }
    }
}

bool connect_pipe()
{
    for (int tries = 0; tries < 10; tries++) {
        HANDLE h = CreateFileW(g_pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            /* the reader first: a pipe without buffers blocks a write until the other side reads, and the
             * launcher may write to us ("quiet") before it reads our hello */
            std::thread(reader, h).detach();
            char hello[256];
            snprintf(hello, sizeof hello, "{\"t\":\"hello\",\"sdk\":\"if2-native-1\",\"console\":\"ps3\",\"memory\":false,"
                     "\"game\":\"%s\"}\n", kGameId);
            if (!write_all(h, hello)) {
                logf("the pipe refused our hello (error %lu)", GetLastError());
                CloseHandle(h);            /* also ends the reader's pending read */
                return false;
            }
            g_h = h;
            logf("connected to RecompLauncher");
            return true;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) WaitNamedPipeW(g_pipe.c_str(), 2000);
        else Sleep(500);
    }
    logf("could not connect to RecompLauncher's pipe (error %lu): unlocks are kept in gamedata\\trophies only",
         GetLastError());
    return false;
}

/* The trophy files the HLE keeps (gamedata/trophies/<id>.json: {"trophies": [{"id": N, "timestamp": T}, ...]}). */
std::string earlier_lines()
{
    std::string out;
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA("gamedata\\trophies\\*.json", &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    int count = 0;
    do {
        std::string p = std::string("gamedata\\trophies\\") + fd.cFileName;
        FILE* fp = fopen(p.c_str(), "rb");
        if (!fp) continue;
        std::string t;
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, fp)) > 0) t.append(b, n);
        fclose(fp);
        for (size_t at = 0; (at = t.find("\"id\":", at)) != std::string::npos; ) {
            at += 5;
            int id = atoi(t.c_str() + at);
            if (id < 0 || id > 255) continue;
            char l[96];
            snprintf(l, sizeof l, "{\"t\":\"native\",\"id\":%d,\"earlier\":true}\n", id);
            out += l;
            count++;
        }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    if (count) logf("%d trophy unlock(s) from before this session sent as earlier", count);
    return out;
}

void worker()
{
    bool up = connect_pipe();
    for (;;) {
        std::string line;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return !g_queue.empty(); });
            line = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (line.empty()) {                      /* the trophy context is registered: the earlier unlocks, once */
            if (g_earlier_sent) continue;
            g_earlier_sent = true;
            line = earlier_lines();
            if (line.empty()) continue;
        }
        if (!up) up = connect_pipe();           /* once more per message: the launcher may have been busy */
        if (!up) continue;
        if (!write_all(g_h, line)) {
            logf("the pipe closed (error %lu): reconnecting", GetLastError());
            CloseHandle(g_h);
            g_h = INVALID_HANDLE_VALUE;
            up = connect_pipe() && write_all(g_h, line);
        }
    }
}

void if2_RegisterContext(ppu_context* ctx)
{
    const uint64_t r = ((if2_hle_generic)(void*)sceNpTrophyRegisterContext)(
        ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8], ctx->gpr[9], ctx->gpr[10]);
    ctx->gpr[3] = r;
    if ((int32_t)r == 0) push(std::string());
}

void if2_UnlockTrophy(ppu_context* ctx)
{
    const int32_t id = (int32_t)ctx->gpr[5];
    const uint64_t r = ((if2_hle_generic)(void*)sceNpTrophyUnlockTrophy)(
        ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8], ctx->gpr[9], ctx->gpr[10]);
    ctx->gpr[3] = r;
    if ((int32_t)r != 0) return;                 /* refused (already unlocked, bad id): nothing new */
    char l[64];
    snprintf(l, sizeof l, "{\"t\":\"native\",\"id\":%d}\n", id);
    logf("trophy %d unlocked: sent to RecompLauncher", id);
    push(l);
}

}  // namespace

/* if2_options_install, before the store's summary: the row. */
extern "C" void if2_achievements_rows(void)
{
    opt_register(&kAchRow);
}

/* infamous2_install_overrides: with the row on and RecompLauncher's pipe named, the two trophy calls are wrapped
 * and the channel's thread starts. Returns the overrides installed. */
extern "C" int if2_achievements_install(void)
{
    const opt_id id = opt_find("achievements");
    if (id < 0 || !opt_i(id)) return 0;
    const char* s = getenv("RL_SESSION");
    if (!s || !*s) {
        logf("on, but the game was not started by RecompLauncher (no RL_SESSION): nothing to send");
        return 0;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return 0;
    g_pipe.assign((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &g_pipe[0], n);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyRegisterContext"), "sceNpTrophyRegisterContext", if2_RegisterContext);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyUnlockTrophy"), "sceNpTrophyUnlockTrophy", if2_UnlockTrophy);
    std::thread(worker).detach();
    logf("on: trophy unlocks go to RecompLauncher as native achievements of '%s'", kGameId);
    return 2;
}

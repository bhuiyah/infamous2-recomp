/* Host test for src/if2_achievements.cpp (Recomp Achievements): includes the file, stubs the settings store and
 * the two trophy HLE handlers, plays RecompLauncher's side of the channel on a named pipe of its own, and checks:
 *   - off (the default): nothing installed, no HLE entry replaced;
 *   - on without RL_SESSION: nothing installed;
 *   - on with RL_SESSION: both trophy calls wrapped; hello first; at RegisterContext the trophy file's unlocks as
 *     "earlier" (once); each unlock the HLE accepts as "native"; a refused unlock (already unlocked) sends nothing;
 *     the wrappers hand the HLE's own r3 back unchanged (also an error code), with every argument passed through;
 *     lines from the launcher are read.
 * Synthetic data only (a temporary folder with gamedata\trophies\NPWR01730.json); no game files, no game build.
 * From the project folder (Windows):
 *
 *   clang-cl /nologo /EHsc /Od -std:c++20 -MD -Wno-everything
 *     -Iexternal\ps3recomp\include -Iexternal\ps3recomp\runtime\ppu
 *     tests\test_if2_achievements.cpp /Fe%TEMP%\test_if2_achievements.exe /Fo%TEMP%\
 *     && %TEMP%\test_if2_achievements.exe
 *
 * Prints "if2_achievements host checks passed". */
#include "../src/if2_achievements.cpp"

#include <direct.h>
#include <string>
#include <vector>

#define CHECK(c) do { if (!(c)) { fprintf(stdout, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); \
                                  exit(1); } } while (0)

/* ---- the settings store: one row ---- */
static const opt_def* g_row = nullptr;
static int g_row_on = 0;
extern "C" opt_id opt_register(const opt_def* d) { g_row = d; return 0; }
extern "C" opt_id opt_find(const char* key) { return g_row && !strcmp(key, g_row->key) ? 0 : -1; }
extern "C" int opt_i(opt_id id) { return id == 0 ? g_row_on : 0; }

/* ---- the HLE: what got replaced, and the two handlers ---- */
struct Reg { uint32_t nid; std::string name; void (*fn)(ppu_context*); };
static std::vector<Reg> g_regs;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    g_regs.push_back({ nid, name, fn });
}
static uint64_t g_args[8];
static uint64_t g_ret_register = 0, g_ret_unlock = 0;
extern "C" uint64_t sceNpTrophyRegisterContext_impl(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                                                    uint64_t f, uint64_t g, uint64_t h)
{
    uint64_t v[8] = { a, b, c, d, e, f, g, h }; memcpy(g_args, v, sizeof v); return g_ret_register;
}
extern "C" uint64_t sceNpTrophyUnlockTrophy_impl(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                                                 uint64_t f, uint64_t g, uint64_t h)
{
    uint64_t v[8] = { a, b, c, d, e, f, g, h }; memcpy(g_args, v, sizeof v); return g_ret_unlock;
}
/* the file under test declares them as void(void) and calls them through the dispatcher's 8-argument type */
#pragma comment(linker, "/alternatename:sceNpTrophyRegisterContext=sceNpTrophyRegisterContext_impl")
#pragma comment(linker, "/alternatename:sceNpTrophyUnlockTrophy=sceNpTrophyUnlockTrophy_impl")

static void (*find_fn(const char* name))(ppu_context*)
{
    for (auto& r : g_regs) if (r.name == name) { CHECK(r.nid == ps3_compute_nid(name)); return r.fn; }
    return nullptr;
}

/* ---- RecompLauncher's side: a byte-mode pipe server ---- */
static HANDLE g_srv = INVALID_HANDLE_VALUE;
static std::string g_rx;
static std::string read_line(DWORD timeout_ms)
{
    const DWORD t0 = GetTickCount();
    for (;;) {
        size_t e = g_rx.find('\n');
        if (e != std::string::npos) { std::string l = g_rx.substr(0, e); g_rx.erase(0, e + 1); return l; }
        DWORD avail = 0;
        if (PeekNamedPipe(g_srv, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            char b[512]; DWORD n = 0;
            if (ReadFile(g_srv, b, avail < sizeof b ? avail : sizeof b, &n, nullptr)) g_rx.append(b, n);
            continue;
        }
        if (GetTickCount() - t0 > timeout_ms) return "<timeout>";
        Sleep(10);
    }
}

static ppu_context ctx_with(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    ppu_context c0;
    memset(&c0, 0, sizeof c0);
    c0.gpr[3] = a; c0.gpr[4] = b; c0.gpr[5] = c; c0.gpr[6] = d; c0.gpr[7] = 0x77; c0.gpr[10] = 0xAA;
    return c0;
}

int main()
{
    /* a scratch folder as the working directory, with the HLE's trophy file: two earlier unlocks */
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string dir = std::string(tmp) + "if2_ach_test_" + std::to_string(GetCurrentProcessId());
    CHECK(_mkdir(dir.c_str()) == 0);
    CHECK(_chdir(dir.c_str()) == 0);
    _mkdir("gamedata");
    _mkdir("gamedata\\trophies");
    FILE* f = fopen("gamedata\\trophies\\NPWR01730.json", "wb");
    CHECK(f);
    fputs("{\n  \"trophies\": [\n    { \"id\": 44, \"timestamp\": 1793072926 },\n    { \"id\": 7, \"timestamp\": 1793072999 }\n  ]\n}\n", f);
    fclose(f);

    /* 1. off: nothing */
    if2_achievements_rows();
    CHECK(g_row && !strcmp(g_row->key, "achievements") && !strcmp(g_row->dflt, "0") && g_row->type == OPT_BOOL);
    CHECK(!strcmp(g_row->env, "IF2_ACHIEVEMENTS"));
    g_row_on = 0;
    _putenv("RL_SESSION=\\\\.\\pipe\\if2-ach-unused");
    CHECK(if2_achievements_install() == 0 && g_regs.empty());

    /* 2. on, not started by RecompLauncher: nothing */
    g_row_on = 1;
    _putenv("RL_SESSION=");
    CHECK(if2_achievements_install() == 0 && g_regs.empty());

    /* 3. on, with RecompLauncher's pipe */
    char name[128];
    snprintf(name, sizeof name, "\\\\.\\pipe\\RecompLauncher-if2test-%lu", GetCurrentProcessId());
    /* no buffers (as .NET's NamedPipeServerStream makes by default): every write waits for the other side's read */
    g_srv = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 0, 0, nullptr);
    CHECK(g_srv != INVALID_HANDLE_VALUE);
    std::string env = std::string("RL_SESSION=") + name;
    _putenv(env.c_str());
    CHECK(if2_achievements_install() == 2);
    CHECK(g_regs.size() == 2);
    auto reg = find_fn("sceNpTrophyRegisterContext");
    auto unl = find_fn("sceNpTrophyUnlockTrophy");
    CHECK(reg && unl);
    CHECK(ConnectNamedPipe(g_srv, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED);
    /* the launcher talks first: this write returns only once the game side reads it */
    DWORD n = 0;
    const char* quiet = "{\"t\":\"quiet\"}\n";
    CHECK(WriteFile(g_srv, quiet, (DWORD)strlen(quiet), &n, nullptr));
    std::string l = read_line(5000);
    printf("  < %s\n", l.c_str());
    CHECK(l == "{\"t\":\"hello\",\"sdk\":\"if2-native-1\",\"console\":\"ps3\",\"memory\":false,\"game\":\"infamous2-recomp\"}");

    /* RegisterContext: the HLE's result and arguments pass through; the earlier unlocks follow, once */
    g_ret_register = 0;
    ppu_context c = ctx_with(1, 1, 0x00123450, 0x00ABCDE0);
    reg(&c);
    CHECK(c.gpr[3] == 0 && g_args[0] == 1 && g_args[2] == 0x00123450 && g_args[3] == 0x00ABCDE0 && g_args[4] == 0x77 && g_args[7] == 0xAA);
    l = read_line(5000); printf("  < %s\n", l.c_str()); CHECK(l == "{\"t\":\"native\",\"id\":44,\"earlier\":true}");
    l = read_line(5000); printf("  < %s\n", l.c_str()); CHECK(l == "{\"t\":\"native\",\"id\":7,\"earlier\":true}");
    reg(&c);   /* a second registration (a reload) does not send them again */

    /* UnlockTrophy accepted: native */
    g_ret_unlock = 0;
    c = ctx_with(1, 1, 12, 0x00200000);
    unl(&c);
    CHECK(c.gpr[3] == 0 && g_args[2] == 12 && g_args[3] == 0x00200000);
    l = read_line(5000); printf("  < %s\n", l.c_str()); CHECK(l == "{\"t\":\"native\",\"id\":12}");

    /* UnlockTrophy refused (SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED as the dispatcher returns it): r3 kept, nothing sent */
    g_ret_unlock = 0x8002290Bull;
    c = ctx_with(1, 1, 12, 0);
    unl(&c);
    CHECK(c.gpr[3] == 0x8002290Bull);
    g_ret_unlock = 0;
    c = ctx_with(1, 1, 33, 0);
    unl(&c);
    l = read_line(5000); printf("  < %s\n", l.c_str()); CHECK(l == "{\"t\":\"native\",\"id\":33}");
    CHECK(read_line(300) == "<timeout>");   /* nothing else: not the refused one, not the earlier ones again */

    printf("if2_achievements host checks passed\n");
    fflush(stdout);
    ExitProcess(0);   /* the channel's threads are detached and blocked on the pipe */
}

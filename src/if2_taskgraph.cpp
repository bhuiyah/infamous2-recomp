/*
 * inFamous 2 -- the game's task graph: watch it (IF2_TASKLOG), list a
 * mission's steps live (IF2_TASKJUMP=list) and, for test starts with no save
 * to start from, jump a mission to one of its steps (IF2_TASKJUMP=<n>).
 * Opt-in; with neither variable set nothing is registered.
 *
 * Why this exists. The prologue (Empire City, up to the Beast fights) never
 * saves: an autosave needs the "saving enabled" flag and the world "i2" (New
 * Marais) or game mode 8 (0x125804), so there is no save to put a test run at
 * the gate, the boss or the bridge -- src/if2_checkpoint.cpp covers everything
 * from New Marais on. The shipped EBOOT has no developer start either: main
 * (0x1DC0A8) stores argc/argv at 0xA08F98/0xA08F9C (via 0x1DB4B4) and nothing
 * reads them back, no config file name appears in it, and DEBUG_MENU_COMMAND /
 * MENU (type ids 284/285) are data-driven classes whose menus are not in it.
 * What it has is the machinery its own checkpoints use: every mission, its
 * steps and their checkpoints are nodes of one task graph
 * (i2_task_graph.sprig.xpps, 968 nodes; 986 in update 1.04's patch archives; a save's STATE file stores the node
 * states). BCUS98125 v02.00, static analysis:
 *   - the graph object is at 0xA94E10 (the r3 of every task-graph call):
 *     +0x08 -> node* array, +0x0C s16 node count (0x3748FC reads 0xA94E1C),
 *     +0x10 oid hash (0x37255C), +0x7C -> the current mission object (its
 *     +0x14 -> +0x10 is the mission's oid; 0x37919C); the word at 0xA94E00
 *     points at a u16 index table the link lists use;
 *   - a node (0x2C bytes for a step): +0x00 oid, +0x04 kind (u8: 3 mission,
 *     1 task, 4 mission step, 5 checkpoint inside a step), +0x06 u16 parent
 *     index, +0x08 state (s8: 0 inactive, 1 active, 2/3 complete; the
 *     checkpoint natives write 3 or 2, 0x1C453C), +0x0A u16 its own index,
 *     +0x0C successors and +0x10 predecessors (u16 count, u16 slot of the
 *     first index in the u16 table), +0x1C children descriptor (+0x08 s16
 *     count, +0x12 slot; the children are consecutive node indices,
 *     0x371A98), +0x20 a flag word (printed by the step list);
 *   - a mission's steps are its kind-4 children, chained by successor links;
 *     the active one has state 1. RESPAWN_CHECKPOINT (0x1C43F4) is the active
 *     step's oid (0x371FEC); CURRENT_CHECKPOINT (0x1C423C) is the last
 *     completed kind-5 child of the active step, else the step itself;
 *   - SetNodeState(graph, node, state) = 0x37A2E8: queues the change and runs
 *     the game's own propagation (0x376064: a completed step activates its
 *     successors) and notifications (0x3798F4).
 * The prologue is msn_beast_intro (oid 0x459, node 0), active from boot: a
 * linear chain of 26 steps (kPrologueSteps below). 1-6 are the title flow, 7
 * starts with New Game.
 *
 * 2026-10-14, run-20261014-180859 ("-Checkpoint @new -Step 23 -TaskLog 1"
 * timed out with "IF2_TASKJUMP never reached its step: waiting for mission
 * 459", and not one [if2-task] line): the graph was never looked at. Every
 * guest pointer was checked against main memory (0x10000..0x10000000), but
 * the graph pack -- node table, nodes, link table -- lives in the game's heap,
 * which this runtime hands out from 0x40000000 (sys_memory_allocate /
 * sys_mmapper, runtime/syscalls/sys_memory.c; that run's 121 MB heap block was
 * at 0x40400000). The host harness the first version was tested with had its
 * fake graph at 0x01000000. guest_ok() now also accepts that window, up to the
 * allocator's high-water mark g_sys_mem_bump_ptr (its pages are committed when
 * they are handed out and never decommitted, so nothing below it faults).
 *
 * The jump, the way the game's own load / respawn path makes it:
 *   1. graph: complete each active step on the way to the target with the
 *      game's SetNodeState(graph, step, 3), as play would, until the target is
 *      the active step -- the mission's respawn checkpoint;
 *   2. warp (IF2_TASKJUMP_MODE=warp, the default): TaskGraph::
 *      WarpToCheckpoint(graph, mission oid, step oid, flags, 1, -1) = 0x378EA0.
 *      It is what the game calls after it loaded a save's STATE into the graph
 *      (0x379440 -> 0x379610, flags 0x12, then 1, -1) and what scripts reach
 *      through a native (0x4B72D8 -> 0x4CFFC). It finds the step (0x377DD0 ->
 *      0x37255C: a child of the mission's node), builds the destination from
 *      the step's properties (0x378D50) and submits a world warp to the warp
 *      manager 0xA26C80 (0x36BA08), which is busy while its +0x40 is non-zero
 *      (0x369B80 and 0x379440 test that first; so do we). The flags are the
 *      game's warp flags (named by the switch at 0x4C7788: 0x01 ReloadWorld,
 *      0x02 RestartMsn, 0x04 WantLetterbox, 0x08 ResurrectLock, 0x10 Wait, ...).
 *      With 0x10 the warp's completion callback is 0x372A90 (descriptor
 *      0x9589E0), which sends WarpedToCheckpoint -- the checkpoint boot
 *      driver's READY. With 0x02, when the target mission is the current one,
 *      it sets the restart flag 0xA08F1D, which the world (re)start consumes
 *      and clears (0x1DB164 -> 0x1D9A7C -> 0x373FB8 walks the graph): the
 *      missions are started again from the graph, as after a Continue. Without
 *      0x01 the warp manager resets the world in place (it sends ResetWorldPre,
 *      0x36CFC0) instead of reloading it -- and in place, from our call between
 *      two frames, the flag is never consumed: the mission is abandoned and
 *      drops back to step 1. So the default is 0x13 (ReloadWorld too): the
 *      flag is consumed 5 frames later, the mission script begins at
 *      RESPAWN_CHECKPOINT, so the target step's own setup runs (enemies,
 *      objectives, cutscene state) and Cole is put at the step's destination
 *      (seen 2026-10-14: step 11 on the pier with "Head towards the
 *      disturbance", step 23 after the boss with "Return to the boat"; READY
 *      ~35 s after launch). Known: the warp's event (0x378F14 -> 0x3793B8)
 *      runs with the game's current-context stack 0x9BF760 empty, so a few
 *      handlers read NULL+0x6C/0x78/0x84 once ([null-read] by 0x1012D0,
 *      0x100318, 0x37A0C, 0x381FC, 0x3847C); play is unaffected.
 *      After the warp the watcher follows the restart flag and
 *      the warp manager for kWatchPolls polls and logs whether the world
 *      consumed the flag (the missions were restarted) or left it pending.
 *      WarpToCheckpoint returns 1 when the warp was taken; when it refuses a
 *      step, the nearest earlier step is tried (logged).
 *   IF2_TASKJUMP_MODE=graph stops after 1 (the first version): the running
 *   mission script is not rebuilt, so what the skipped steps would have done
 *   (cutscenes, spawns, Cole's position) is missing and the script stays in
 *   the step it was running. Neither mode re-creates world changes the
 *   skipped steps made outside the target step's own setup.
 *
 * Environment:
 *   IF2_TASKLOG=1|2|3        read-only: log every node whose state changes
 *                            ([if2-task]); 2 also lists a node's children when it
 *                            turns active; 3 also dumps the whole graph once
 *   IF2_TASKJUMP=<n>|0x<oid>|list
 *                            list: the mission's steps with their live states when
 *                            it becomes active, every change of one of them, and a
 *                            [beat] line every 15 s saying what the watcher sees.
 *                            With a step (the n-th step, or the step with that
 *                            oid): the same, and once play has reached step
 *                            IF2_TASKJUMP_AFTER, the jump above ([if2-taskjump]).
 *   IF2_TASKJUMP_MODE=warp|graph   warp (default) or the graph alone
 *   IF2_TASKJUMP_MISSION=<hex oid>  the mission (459 = msn_beast_intro)
 *   IF2_TASKJUMP_AFTER=<n>   wait for play to reach this step first (prologue 7:
 *                            the title flow is never touched; otherwise 1)
 *   IF2_TASKJUMP_DELAY=<polls>  and this many frames more (0); a warp also waits
 *                            for the warp manager to be idle for 30 frames
 *   IF2_TASKJUMP_WARP_FLAGS=<hex>  WarpToCheckpoint's flags (default 0x13
 *                            ReloadWorld|RestartMsn|Wait; 0x12 resets in place)
 * FABRICATED progress -- for test starts only; a save written afterwards
 * carries it, so without CHECKPOINT (whose sandbox keeps the player's saves out
 * of reach) a jump sets PS3_SAVEDATA_READONLY=1. Status for the boot driver:
 * if2_taskjump_status (0 off, 1 pending + what it waits for, 2 done, 3 gave up).
 *
 * Both run from a cellSysutilCheckCallback override: the main loop (0x1DB544)
 * calls it once per frame between two frames, on the main thread (lr
 * 0x1DB824); its only other caller is the fatal-error screen's thread
 * (0x108ED4). Only main-loop polls act on the game; until one has been seen,
 * the first thread that polls is watched from (read-only for its first 1800
 * polls: a build whose main loop returns elsewhere still jumps, later).
 */

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "ps3emu/nid.h"
#include "ppu_context.h"
#include "if2_version.h"

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" int32_t  cellSysutilCheckCallback(void);
extern "C" uint8_t  vm_read8(uint64_t addr);
extern "C" uint16_t vm_read16(uint64_t addr);
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" uint32_t g_sys_mem_bump_ptr;                         /* runtime/syscalls/sys_memory.c */
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;          /* runtime/ppu/ppu_loader.cpp */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);     /* lifted tail calls */
extern "C" void if2_checkpoint_taskjump_mark(void);             /* src/if2_checkpoint.cpp */
void IF2_F(FN_SET_NODE_STATE)(ppu_context* ctx);       /* SetNodeState (generated/ppu; disc 0x37A2E8) */
void IF2_F(FN_WARP_TO_CHECKPOINT)(ppu_context* ctx);   /* TaskGraph::WarpToCheckpoint (disc 0x378EA0) */

extern "C" int if2_taskgraph_install(void);
extern "C" int if2_taskjump_status(char* what, size_t cap);

namespace {

constexpr uint32_t kTg          = IF2_R(TG);              /* task graph object (disc 0xA94E10) */
constexpr uint32_t kTgNodes     = kTg + 0x08;   /* -> node* [count] */
constexpr uint32_t kTgCount     = kTg + 0x0C;   /* s16 */
constexpr uint32_t kTgMission   = kTg + 0x7C;   /* -> current mission object */
constexpr uint32_t kTgIndex     = IF2_R(TG_INDEX);        /* -> u16 index table (disc 0xA94E00) */
constexpr uint32_t kWarpMgr     = IF2_R(WARP_MGR);        /* the world warp manager (disc 0xA26C80) */
constexpr uint32_t kWarpBusy    = kWarpMgr + 0x40;
constexpr uint32_t kWarpLock    = IF2_R(WARP_LOCK);       /* u8: while set, 0x36BA08 refuses warps without ResurrectLock (0x08) */
constexpr uint32_t kRestartFlag = IF2_R(RESTART_FLAG);    /* u8: restart the missions at the next world start */
constexpr uint32_t kToc         = IF2_R(TOC);             /* the EBOOT's TOC (every lifted r2 load) */
constexpr uint32_t kMainLoopRet = IF2_R(MAIN_LOOP_RET);   /* lr of the main loop's call (disc 0x1DB824) */
constexpr uint32_t kMsnPrologue = 0x00000459;   /* msn_beast_intro */
constexpr int      kPrologueAfter = 7;          /* its first step past the title flow */
/* ReloadWorld | RestartMsn | Wait (its end sends WarpedToCheckpoint). 0x12, the load path's flags
 * (0x379440), resets the world in place from our between-frames call: the restart flag is never
 * consumed, the mission is abandoned (MissionAbandoned) and falls back to step 1 (2026-10-14 runs
 * 220038 / 220217). With ReloadWorld the world restart consumes it 5 frames later and the target
 * step's own script runs (step 11: "Head towards the disturbance"; 23: "Return to the boat"). */
constexpr uint32_t kWarpFlags   = 0x13;
constexpr int      kKindStep = 4, kKindCheckpoint = 5;
constexpr int      kMaxNodes = 4096, kMaxSteps = 64;
constexpr int      kWarpSettle  = 30;           /* polls with the warp manager idle before a jump */
constexpr int      kWarpBusyMax = 3600;         /* polls with it busy before giving up */
constexpr int      kWarpTries   = 5, kWarpRetry = 60;
constexpr double   kBeat = 15.0;                /* seconds between [beat] lines */
constexpr int      kOtherActAfter = 1800;       /* polls from another lr before they may act */
constexpr int      kWatchPolls  = 1800;         /* polls the warp's outcome is followed for */

/* The game's warp flag names, from its value -> name switch (0x4C7788; the strings at 0x94B89C are not in
 * bit order). 0x80 has no name on the disc; update 1.04 names it SuppressText. No value moved; All = 0x77F.
 * ("Autoplay", among those strings, belongs to another enum: 0x4C7884.) */
struct WarpFlagName { uint32_t bit; const char* name; };
const WarpFlagName kWarpFlagNames[] = {
    { 0x001, "ReloadWorld" }, { 0x002, "RestartMsn" }, { 0x004, "WantLetterbox" }, { 0x008, "ResurrectLock" },
    { 0x010, "Wait" }, { 0x020, "HideScreen" }, { 0x040, "NoFade" },
#if IF2_EBOOT_V104
    { 0x080, "SuppressText" },
#endif
    { 0x100, "NoPauseSound" }, { 0x200, "TriggerUgcMode" }, { 0x400, "DevAlwaysQueue" } };

/* msn_beast_intro's steps in graph order (names from the game's graph pack). */
const char* const kPrologueSteps[26] = {
    "set_karma_cap", "continue_i1_game", "temp_intro_montage", "pre_press_start", "loop_press_start",
    "post_press_start", "intro", "city_falls", "engage", "cole_lands", "approach", "bus", "approach_2",
    "emerge", "reveal", "battle", "lift", "battle_2", "head_explodes", "battle_3", "call_lightning",
    "beast_sinks", "return", "cutscene_cole_jumps", "cole_struggle", "finale" };

/* settings (if2_taskgraph_install) */
int      g_tasklog = 0;        /* IF2_TASKLOG level */
int      g_jmode   = 0;        /* IF2_TASKJUMP: 0 off, 1 list, 2 step number, 3 step oid */
int      g_jnum    = 0;
uint32_t g_joid    = 0;
uint32_t g_jmsn    = kMsnPrologue;
int      g_jafter  = 0;        /* IF2_TASKJUMP_AFTER (1-based step), 0 = default */
int      g_jdelay  = 0;
bool     g_warp    = true;     /* IF2_TASKJUMP_MODE */
uint32_t g_wflags  = kWarpFlags;

/* if2_taskjump_status: 0 off, 1 pending, 2 done (the target is active or past), 3 gave up */
std::atomic<int> g_status{0};
std::mutex       g_status_mx;
char             g_status_what[200];

/* what the watcher sees */
uint32_t g_nodes_ea = 0;       /* the node table the snapshot belongs to */
int      g_nodes_n  = 0;
int8_t   g_state[kMaxNodes];
int      g_msn = -1;           /* node index of the mission, -1 unknown */
bool     g_msn_active = false; /* it was active at the last poll */
char     g_wait[200];          /* what the jump waits for (pending status, [beat]) */

/* the jump */
bool     g_jdone  = false;
int      g_calls  = 0;         /* steps completed here */
int      g_polls  = 0;         /* polls since the AFTER step was reached */
int      g_idle   = 0;         /* consecutive polls with the warp manager idle */
int      g_busy   = 0;         /* polls with it busy */
int      g_tries  = 0;         /* warp attempts that were refused */
uint64_t g_retry_at = 0;
bool     g_marked = false;

/* after a taken warp: its outcome (watch_tick) */
bool     g_watch    = false;
uint64_t g_warp_at  = 0;       /* g_npoll of the WarpToCheckpoint call */
bool     g_flag_set = false;   /* the restart flag, right after the call */
int      g_wbusy    = 0;       /* 0 not seen busy yet, 1 busy, 2 idle again */
char     g_done_what[200];     /* the status text the jump finished with */

/* polls */
std::atomic<int>      g_main_seen{0};
std::atomic<uint64_t> g_first_tid{~0ull};
std::mutex            g_tick_mx;
int      g_other_polls = 0;    /* polls from the first thread while no main-loop poll was seen */
uint64_t g_npoll = 0;
double   g_next_beat = 0.0;
std::chrono::steady_clock::time_point g_t0;

double secs(void)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
}

void set_status(int s, const char* fmt, ...)
{
    char b[sizeof g_status_what];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_status_mx);
    memcpy(g_status_what, b, sizeof b);
    g_status.store(s);
}

/* What a pending jump waits for: the boot driver's status, and the [beat]. */
void note_wait(const char* fmt, ...)
{
    char b[sizeof g_wait];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (!strcmp(b, g_wait)) return;
    memcpy(g_wait, b, sizeof b);
    if (g_status.load() == 1) set_status(1, "%s", b);
}

/* A guest range that is backed: main memory (the ELF, its BSS, the PPU
 * stacks), or the window sys_memory_allocate / sys_mmapper hand out from
 * 0x40000000 (the game's heap), up to the allocator's high-water mark. */
bool guest_ok(uint32_t ea, uint32_t len)
{
    const uint64_t end = (uint64_t)ea + len;
    if (ea >= 0x10000u && end <= 0x10000000u) return true;
    const uint32_t top = g_sys_mem_bump_ptr;
    return ea >= 0x40000000u && top <= 0x50000000u && end <= top;
}

uint32_t node_at(int i)        { return vm_read32(g_nodes_ea + 4u * (uint32_t)i); }
int      node_kind(uint32_t n)  { return guest_ok(n, 0x2C) ? vm_read8(n + 4) : -1; }
int      node_state(uint32_t n) { return guest_ok(n, 0x2C) ? (int8_t)vm_read8(n + 8) : -1; }
uint32_t node_oid(uint32_t n)   { return guest_ok(n, 0x2C) ? vm_read32(n) : 0; }
uint32_t node_flags(uint32_t n) { return guest_ok(n, 0x2C) ? vm_read32(n + 0x20) : 0; }

const char* state_name(int s)
{
    return s == 0 ? "inactive" : s == 1 ? "ACTIVE" : s == 2 ? "complete(2)" : s == 3 ? "complete" : "?";
}

/* "RestartMsn|Wait" for the log. */
const char* flag_names(uint32_t f, char* b, size_t cap)
{
    std::string s;
    uint32_t known = 0;
    for (const WarpFlagName& n : kWarpFlagNames) {
        known |= n.bit;
        if (f & n.bit) { if (!s.empty()) s += '|'; s += n.name; }
    }
    if (const uint32_t rest = f & ~known) {          /* unnamed bits in hex */
        char x[16];
        snprintf(x, sizeof x, "0x%X", rest);
        if (!s.empty()) s += '|';
        s += x;
    }
    snprintf(b, cap, "%s", s.empty() ? "none" : s.c_str());
    return b;
}

/* A run of `count` node indices in the u16 table from `slot`. */
int index_list(uint32_t slot, int count, int* out, int max)
{
    const uint32_t tab = vm_read32(kTgIndex);
    int n = 0;
    if (count <= 0 || !guest_ok(tab, 2u * (slot + (uint32_t)count))) return 0;
    for (int i = 0; i < count && n < max; i++) {
        const int v = (int16_t)vm_read16(tab + 2u * (slot + (uint32_t)i));
        if (v >= 0 && v < g_nodes_n) out[n++] = v;
    }
    return n;
}

/* The children of node n: [*first, *first + *count) in the node table. */
bool node_children(uint32_t n, int* first, int* count)
{
    *first = *count = 0;
    if (!guest_ok(n, 0x2C)) return false;
    const uint32_t desc = vm_read32(n + 0x1C);
    if (!guest_ok(desc, 0x14)) return false;
    const int cnt = (int16_t)vm_read16(desc + 0x08);
    int lo = -1;
    if (cnt <= 0 || index_list(vm_read16(desc + 0x12), 1, &lo, 1) != 1 || lo + cnt > g_nodes_n) return false;
    *first = lo; *count = cnt;
    return true;
}

/* Predecessors of node n (its +0x10 link list). */
int node_preds(uint32_t n, int* out, int max)
{
    if (!guest_ok(n, 0x2C)) return 0;
    return index_list(vm_read16(n + 0x12), (int16_t)vm_read16(n + 0x10), out, max);
}

int find_oid(uint32_t oid)
{
    for (int i = 0; i < g_nodes_n; i++)
        if (node_oid(node_at(i)) == oid) return i;
    return -1;
}

/* The mission's steps (kind-4 children), in graph order. */
int mission_steps(int msn, int* steps, int max)
{
    int lo, cnt, n = 0;
    if (msn >= 0 && node_children(node_at(msn), &lo, &cnt))
        for (int c = lo; c < lo + cnt && n < max; c++)
            if (node_kind(node_at(c)) == kKindStep) steps[n++] = c;
    return n;
}

const char* step_name(int j, int ns)
{
    return (g_jmsn == kMsnPrologue && ns == 26 && j >= 0 && j < 26) ? kPrologueSteps[j] : "";
}

/* The 1-based number of the mission's active step, 0 if none. */
int active_step(const int* steps, int ns)
{
    for (int j = 0; j < ns; j++) if (node_state(node_at(steps[j])) == 1) return j + 1;
    return 0;
}

/* The oid of the mission object the graph calls current, ~0 if none. */
uint32_t current_mission_oid(void)
{
    const uint32_t obj = vm_read32(kTgMission);
    if (!guest_ok(obj, 0x18)) return ~0u;
    const uint32_t d = vm_read32(obj + 0x14);
    return guest_ok(d, 0x14) ? vm_read32(d + 0x10) : ~0u;
}

void print_node(const char* pfx, int i)
{
    const uint32_t n = node_at(i);
    const int k = node_kind(n);
    fprintf(stderr, "%s#%d oid %08X kind %d state %d", pfx, i, node_oid(n), k, node_state(n));
    if (k == kKindCheckpoint) fprintf(stderr, " +28 %08X", vm_read32(n + 0x28));
    fputc('\n', stderr);
}

void print_children(int i)
{
    int lo, cnt;
    if (!node_children(node_at(i), &lo, &cnt)) return;
    for (int c = lo; c < lo + cnt; c++) print_node("[if2-task]     child ", c);
}

/* The mission's steps with their live states. */
void print_steps(int msn, const int* steps, int ns)
{
    fprintf(stderr, "[if2-taskjump] %.1fs mission %08X (#%d, state %d): %d step(s), active step %d; "
                    "current mission object oid %08X\n",
            secs(), g_jmsn, msn, node_state(node_at(msn)), ns, active_step(steps, ns), current_mission_oid());
    for (int j = 0; j < ns; j++) {
        const uint32_t n = node_at(steps[j]);
        fprintf(stderr, "[if2-taskjump]   step %2d %-20s #%d oid %08X flags %08X %s\n", j + 1, step_name(j, ns),
                steps[j], node_oid(n), node_flags(n), state_name(node_state(n)));
        int lo, cnt;
        if (node_children(n, &lo, &cnt))
            for (int c = lo; c < lo + cnt; c++)
                if (node_kind(node_at(c)) == kKindCheckpoint) print_node("[if2-taskjump]       checkpoint ", c);
    }
    fflush(stderr);
}

/* Run a lifted guest function on the calling guest thread: its registers, TLS
 * and thread id, a fresh frame 4 KB below the caller's stack pointer (the
 * guest stack grows down, and nothing of the caller's lives below its SP
 * across a call), the EBOOT's TOC. Not ppu_guest_call: that one runs every
 * callback at the same fixed scratch stack (0xCFFE0000), which the vblank
 * handlers it pumps from inside any HLE call would share. */
uint64_t guest_call(ppu_context* caller, void (*fn)(ppu_context*), uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0)
{
    ppu_context c;
    memcpy(&c, caller, sizeof c);
    c.gpr[1] = (caller->gpr[1] - 0x1000u) & ~0xFull;
    c.gpr[2] = kToc;
    c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3; c.gpr[7] = a4; c.gpr[8] = a5;
    c.lr = 0;
    c.reserve_addr = 0; c.reserve_value = 0; c.reserve_valid = 0;
    ppu_context* const saved = g_active_ctx;
    g_active_ctx = &c;
    fn(&c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&c); }
    g_active_ctx = saved;
    return c.gpr[3];
}

/* Step 2 of the jump: WarpToCheckpoint to the target step (index tj in
 * steps), else to the nearest earlier step it accepts, down to step `after`.
 * The 0-based index of the step warped to, or -1. */
int warp(ppu_context* ctx, const int* steps, int ns, int tj, int after)
{
    if (!g_marked) { g_marked = true; if2_checkpoint_taskjump_mark(); }
    char fn[160];
    flag_names(g_wflags, fn, sizeof fn);
    for (int j = tj; j >= 0 && j >= after - 1; j--) {
        const uint32_t oid = node_oid(node_at(steps[j]));
        const unsigned before = vm_read8(kRestartFlag);
        const uint64_t r = guest_call(ctx, IF2_F(FN_WARP_TO_CHECKPOINT), kTg,
                                      (uint64_t)(int64_t)(int32_t)g_jmsn, (uint64_t)(int64_t)(int32_t)oid,
                                      g_wflags, 1, ~0ull);
        const bool took = (uint8_t)r != 0;
        const unsigned lock = vm_read8(kWarpLock);
        fprintf(stderr, "[if2-taskjump] %.1fs WarpToCheckpoint(mission %08X, step %d %s oid %08X, flags 0x%X %s) "
                        "-> %d%s\n", secs(), g_jmsn, j + 1, step_name(j, ns), oid, g_wflags, fn, took ? 1 : 0,
                took ? "" : lock && !(g_wflags & 8) ? ": refused (the warp lock is set: without ResurrectLock "
                                                       "the warp manager refuses every warp)"
                                                    : ": refused (no such step or no destination for it, or the "
                                                      "warp manager would not queue it)");
        if (took) {
            const unsigned flag = vm_read8(kRestartFlag);
            fprintf(stderr, "[if2-taskjump] %.1fs warp taken: restart-missions flag 0x%08X %u -> %u, current mission "
                            "object oid %08X%s\n", secs(), kRestartFlag, before, flag, current_mission_oid(),
                    flag ? "; watching for the world to consume it (the missions restart from the graph)"
                    : (g_wflags & 2) ? " -- the mission is NOT restarted (it is not the current one)"
                                     : " (no RestartMsn in the flags)");
            fflush(stderr);
            return j;
        }
        if (vm_read32(kWarpBusy)) break;   /* refused because a warp started: try again later */
    }
    fflush(stderr);
    return -1;
}

/* Every poll while the mission is active. */
void jump_tick(ppu_context* ctx, int msn, const int* steps, int ns, bool activated, bool can_act)
{
    if (activated) {
        print_steps(msn, steps, ns);
        if (g_jmode >= 2 && !g_jdone) g_polls = 0;
    }
    if (g_jmode < 2 || g_jdone) return;
    int tj = -1;                                   /* the target, 0-based in steps */
    if (g_jmode == 2 && g_jnum <= ns) tj = g_jnum - 1;
    if (g_jmode == 3)
        for (int j = 0; j < ns; j++) if (node_oid(node_at(steps[j])) == g_joid) tj = j;
    if (tj < 0) {
        fprintf(stderr, "[if2-taskjump] no such step (IF2_TASKJUMP) in a mission of %d steps: nothing changed\n", ns);
        fflush(stderr);
        set_status(3, "no step %s in mission %X (%d steps)", g_jmode == 2 ? "number" : "oid", g_jmsn, ns);
        g_jdone = true;
        return;
    }
    const int target = steps[tj];
    const int after = g_jafter ? g_jafter : (g_jmsn == kMsnPrologue ? kPrologueAfter : 1);
    if (activated)
        fprintf(stderr, "[if2-taskjump] target: step %d %s (#%d oid %08X), %s, once play has reached step %d\n",
                tj + 1, step_name(tj, ns), target, node_oid(node_at(target)),
                g_warp ? "graph + WarpToCheckpoint" : "graph only", after);
    if (after > ns) {
        set_status(3, "IF2_TASKJUMP_AFTER %d: mission %X has %d steps", after, g_jmsn, ns);
        g_jdone = true;
        return;
    }
    const uint32_t tnode = node_at(target);
    if (node_state(node_at(steps[after - 1])) < 1) {
        g_polls = 0;
        const int a = active_step(steps, ns);
        note_wait("waiting for play to reach step %d %s (active: step %d %s)", after, step_name(after - 1, ns),
                  a, a ? step_name(a - 1, ns) : "-");
        return;
    }
    if (node_state(tnode) != 0 && g_calls == 0) {  /* play got there by itself */
        fprintf(stderr, "[if2-taskjump] %.1fs step %d %s is %s by play itself: nothing to do\n", secs(), tj + 1,
                step_name(tj, ns), state_name(node_state(tnode)));
        fflush(stderr);
        if (!g_marked) { g_marked = true; if2_checkpoint_taskjump_mark(); }
        set_status(2, "step %d %s %s by play itself", tj + 1, step_name(tj, ns),
                   node_state(tnode) == 1 ? "active" : "past");
        g_jdone = true;
        return;
    }
    if (g_polls < g_jdelay) {
        g_polls++;
        note_wait("step %d reached; jumping in %d frame(s) (IF2_TASKJUMP_DELAY)", after, g_jdelay - g_polls + 1);
        return;
    }
    if (!can_act) {
        note_wait("at step %d, but these polls are not the main loop's: not acting", after);
        return;
    }
    if (g_warp) {
        const uint32_t busy = vm_read32(kWarpBusy);
        if (busy) {
            g_idle = 0;
            if (++g_busy > kWarpBusyMax) {
                set_status(3, "the warp manager stayed busy for %d frames (0x%08X = 0x%08X)", g_busy, kWarpBusy, busy);
                g_jdone = true;
                return;
            }
            note_wait("warp manager busy (0x%08X = 0x%08X): waiting", kWarpBusy, busy);
            return;
        }
        if (g_idle < kWarpSettle) {
            g_idle++;
            note_wait("warp manager idle %d/%d frames", g_idle, kWarpSettle);
            return;
        }
    }

    /* 1. the graph */
    if (node_state(tnode) == 0) {
        /* The target's ancestors: everything reachable backwards over the
         * predecessor links, inside this mission. */
        static uint8_t anc[kMaxNodes];
        memset(anc, 0, sizeof anc);
        int stack[256], sp = 0;
        stack[sp++] = target;
        while (sp) {
            int preds[16];
            const int np = node_preds(node_at(stack[--sp]), preds, 16);
            for (int k = 0; k < np; k++)
                if (!anc[preds[k]]) { anc[preds[k]] = 1; if (sp < 256) stack[sp++] = preds[k]; }
        }
        for (int guard = 0; guard < kMaxSteps && g_calls < kMaxSteps; guard++) {
            int act = -1, aj = -1;
            for (int j = 0; j < ns && act < 0; j++)
                if (anc[steps[j]] && node_state(node_at(steps[j])) == 1) { act = steps[j]; aj = j; }
            if (act < 0) break;                     /* nothing to advance yet: next poll */
            const uint32_t an = node_at(act);
            guest_call(ctx, IF2_F(FN_SET_NODE_STATE), kTg, an, 3);
            g_calls++;
            fprintf(stderr, "[if2-taskjump] %.1fs completed step %d %s (#%d oid %08X): state now %d\n",
                    secs(), aj + 1, step_name(aj, ns), act, node_oid(an), node_state(an));
            if (node_state(an) == 1) {
                fprintf(stderr, "[if2-taskjump] SetNodeState left it active: giving up\n");
                fflush(stderr);
                set_status(3, "SetNodeState left step %d active", aj + 1);
                g_jdone = true;
                return;
            }
            if (node_state(tnode) != 0) break;
        }
        fflush(stderr);
        if (node_state(tnode) == 0) {
            if (g_calls >= kMaxSteps) {
                set_status(3, "%d steps completed and step %d is still inactive", g_calls, tj + 1);
                g_jdone = true;
            } else {
                note_wait("%d step(s) completed; step %d is not active yet", g_calls, tj + 1);
            }
            return;
        }
        fprintf(stderr, "[if2-taskjump] %.1fs step %d %s is %s after %d step(s) completed here. FABRICATED "
                        "progress\n", secs(), tj + 1, step_name(tj, ns), state_name(node_state(tnode)), g_calls);
        fflush(stderr);
    }
    if (!g_warp) {
        if (!g_marked) { g_marked = true; if2_checkpoint_taskjump_mark(); }
        set_status(2, "step %d %s active, %d step(s) completed (graph only: the mission script was not rebuilt)",
                   tj + 1, step_name(tj, ns), g_calls);
        g_jdone = true;
        return;
    }

    /* 2. the warp */
    if (g_npoll < g_retry_at) return;
    const int wj = warp(ctx, steps, ns, tj, after);
    if (wj >= 0) {
        snprintf(g_done_what, sizeof g_done_what,
                 "step %d %s active, %d step(s) completed, WarpToCheckpoint to step %d%s (restart flag %u)",
                 tj + 1, step_name(tj, ns), g_calls, wj + 1, wj == tj ? "" : " (the target was refused)",
                 vm_read8(kRestartFlag));
        set_status(2, "%s", g_done_what);
        g_jdone = true;
        g_watch = true; g_warp_at = g_npoll; g_flag_set = vm_read8(kRestartFlag) != 0; g_wbusy = 0;
        g_next_beat = secs() + kBeat;
        return;
    }
    if (++g_tries >= kWarpTries) {
        set_status(3, "WarpToCheckpoint refused steps %d..%d %d times (the graph is at step %d; warp lock "
                      "0x%08X = %u)", tj + 1, after, g_tries, tj + 1, kWarpLock, vm_read8(kWarpLock));
        g_jdone = true;
        return;
    }
    g_retry_at = g_npoll + kWarpRetry;
    g_idle = 0;
    note_wait("WarpToCheckpoint refused (%d of %d tries); again in %d frames", g_tries, kWarpTries, kWarpRetry);
}

/* After a taken warp, every poll for kWatchPolls polls: when the warp manager
 * took the request and finished it, and whether the world consumed the
 * restart flag (0x1DB164 clears it when it restarts the missions from the
 * graph). Reads only the EBOOT's BSS, so it runs while the graph is away. */
void watch_tick(void)
{
    if (!g_watch) return;
    const unsigned long long n = g_npoll - g_warp_at;
    const uint32_t busy = vm_read32(kWarpBusy);
    if (busy && g_wbusy == 0) {
        g_wbusy = 1;
        fprintf(stderr, "[if2-taskjump] %.1fs the warp manager took the warp, %llu frame(s) after the call\n", secs(), n);
    } else if (!busy && g_wbusy == 1) {
        g_wbusy = 2;
        fprintf(stderr, "[if2-taskjump] %.1fs the warp manager is idle again, %llu frames after the call: the warp "
                        "is over\n", secs(), n);
    }
    if (g_flag_set && !vm_read8(kRestartFlag)) {
        g_flag_set = false;
        fprintf(stderr, "[if2-taskjump] %.1fs restart flag consumed %llu frames after the warp: the world restarted "
                        "the missions from the task graph\n", secs(), n);
        set_status(2, "%s; missions restarted %llu frames later", g_done_what, n);
    }
    if (!g_flag_set && g_wbusy == 2) { g_watch = false; fflush(stderr); return; }
    if (n < (unsigned long long)kWatchPolls) return;
    if (g_flag_set) {
        fprintf(stderr, "[if2-taskjump] %.1fs restart flag 0x%08X still set %llu frames after the warp: the world "
                        "has not restarted the missions, so the mission script was not rebuilt (a warp that "
                        "reloads the world: IF2_TASKJUMP_WARP_FLAGS=0x13, the default)\n", secs(), kRestartFlag, n);
        set_status(2, "%s; restart flag still pending %llu frames later", g_done_what, n);
    }
    if (g_wbusy != 2)
        fprintf(stderr, "[if2-taskjump] %.1fs the warp manager %s %llu frames after the call\n", secs(),
                g_wbusy ? "is still busy" : "never showed the warp as running", n);
    fflush(stderr);
    g_watch = false;
}

/* A [beat] line: what the watcher sees. */
void beat(bool have_graph, uint32_t nodes, int count, int msn, const int* steps, int ns)
{
    char what[200];
    {
        std::lock_guard<std::mutex> lk(g_status_mx);
        snprintf(what, sizeof what, "%s", g_status_what);
    }
    const int st = g_status.load();
    if (!have_graph) {
        fprintf(stderr, "[if2-taskjump] %.1fs [beat] no task graph%s: node table *0x%08X = 0x%08X, count %d "
                        "(guest heap top 0x%08X); warp manager %s, restart flag %u%s%s\n", secs(),
                g_nodes_n ? " now" : " yet", kTgNodes, nodes, count, g_sys_mem_bump_ptr,
                vm_read32(kWarpBusy) ? "busy" : "idle", vm_read8(kRestartFlag), st ? "; " : "", st ? what : "");
    } else if (msn < 0) {
        fprintf(stderr, "[if2-taskjump] %.1fs [beat] graph of %d nodes at 0x%08X: no mission %X in it\n", secs(),
                g_nodes_n, g_nodes_ea, g_jmsn);
    } else {
        const int a = active_step(steps, ns);
        int done = 0;
        for (int j = 0; j < ns; j++) done += node_state(node_at(steps[j])) >= 2;
        fprintf(stderr, "[if2-taskjump] %.1fs [beat] mission %X #%d %s; active step %d %s, %d of %d complete; "
                        "warp manager %s, restart flag %u; current mission object oid %08X%s%s\n", secs(), g_jmsn,
                msn, state_name(node_state(node_at(msn))), a, a ? step_name(a - 1, ns) : "-", done, ns,
                vm_read32(kWarpBusy) ? "busy" : "idle", vm_read8(kRestartFlag), current_mission_oid(),
                st == 1 ? "; jump: " : st == 2 ? "; jump done: " : st == 3 ? "; jump gave up: " : "",
                st ? what : "");
    }
    fflush(stderr);
}

void task_tick(ppu_context* ctx, bool can_act)
{
    g_npoll++;
    if (can_act) watch_tick();
    const uint32_t nodes = vm_read32(kTgNodes);
    const int count = (int16_t)vm_read16(kTgCount);
    const bool beat_now = g_jmode && (g_jmode == 1 || g_status.load() == 1 || g_watch) && secs() >= g_next_beat;
    if (beat_now) g_next_beat = secs() + kBeat;
    if (count <= 0 || count > kMaxNodes || !guest_ok(nodes, 4u * (uint32_t)count)) {
        note_wait("no task graph yet (node table *0x%08X = 0x%08X, count %d)", kTgNodes, nodes, count);
        if (beat_now) beat(false, nodes, count, -1, nullptr, 0);
        return;                                     /* no graph loaded (yet) */
    }
    const bool fresh = nodes != g_nodes_ea || count != g_nodes_n;
    if (fresh) {
        g_nodes_ea = nodes; g_nodes_n = count;
        for (int i = 0; i < count; i++) g_state[i] = (int8_t)node_state(node_at(i));
        fprintf(stderr, "[if2-task] %.1fs task graph: %d nodes, table 0x%08X%s\n", secs(), count, nodes,
                !IF2_EBOOT_KNOWN || count == (IF2_EBOOT_V104 ? 986 : 968) ? "" : " (not the node count of this EBOOT's graph pack: the layout may differ)");
        if (g_tasklog >= 3)
            for (int i = 0; i < count; i++) print_node("[if2-task]   ", i);
    }
    int msn = -1;
    if (g_jmode) {
        if (fresh || g_msn < 0 || g_msn >= count || node_oid(node_at(g_msn)) != g_jmsn) {
            const int was = g_msn;
            g_msn = find_oid(g_jmsn);
            if (fresh || g_msn != was) {
                if (g_msn >= 0)
                    fprintf(stderr, "[if2-taskjump] %.1fs mission %08X is node #%d (state %d)\n", secs(), g_jmsn,
                            g_msn, node_state(node_at(g_msn)));
                else
                    fprintf(stderr, "[if2-taskjump] %.1fs mission %08X is not in this graph (%d nodes)\n", secs(),
                            g_jmsn, count);
                fflush(stderr);
            }
        }
        msn = g_msn;
    }
    int steps[kMaxSteps];
    const int ns = msn >= 0 ? mission_steps(msn, steps, kMaxSteps) : 0;

    int shown = 0, stepshown = 0;
    for (int i = 0; i < count; i++) {
        const int st = node_state(node_at(i));
        if (st == g_state[i]) continue;
        if (g_tasklog && shown < 64) {
            const uint32_t n = node_at(i);
            fprintf(stderr, "[if2-task] %.1fs #%d oid %08X kind %d: state %d -> %d\n",
                    secs(), i, node_oid(n), node_kind(n), g_state[i], st);
            if (g_tasklog >= 2 && st == 1) print_children(i);
        }
        shown++;
        if (g_jmode && ns && stepshown < 32)
            for (int j = 0; j < ns; j++)
                if (steps[j] == i) {
                    fprintf(stderr, "[if2-taskjump] %.1fs step %d %s (#%d oid %08X): %s -> %s\n", secs(), j + 1,
                            step_name(j, ns), i, node_oid(node_at(i)), state_name(g_state[i]), state_name(st));
                    stepshown++;
                }
        g_state[i] = (int8_t)st;
    }
    if (g_tasklog && shown > 64)
        fprintf(stderr, "[if2-task] ... %d more state changes this frame not shown\n", shown - 64);
    if (shown && (g_tasklog || stepshown)) fflush(stderr);

    if (g_jmode) {
        if (msn < 0) {
            note_wait("mission %X is not in the task graph (%d nodes)", g_jmsn, count);
            g_msn_active = false;
        } else if (g_state[msn] != 1) {
            note_wait("mission %X (#%d) is %s", g_jmsn, msn, state_name(g_state[msn]));
            g_msn_active = false;
        } else {
            const bool activated = !g_msn_active;
            g_msn_active = true;
            jump_tick(ctx, msn, steps, ns, activated, can_act);
        }
        if (beat_now) beat(true, nodes, count, msn, steps, ns);
    }
}

/* Every cellSysutilCheckCallback: the main loop's (lr 0x1DB824) act; until
 * one has been seen, the first polling thread is watched from. */
void poll(ppu_context* ctx)
{
    const uint32_t lr = (uint32_t)ctx->lr;
    if (lr == kMainLoopRet) {
        if (!g_main_seen.exchange(1)) {
            fprintf(stderr, "[if2-task] %.1fs first main-loop poll (thread %llu): watching the task graph at "
                            "0x%08X\n", secs(), (unsigned long long)ctx->thread_id, kTg);
            fflush(stderr);
        }
        std::lock_guard<std::mutex> lk(g_tick_mx);
        task_tick(ctx, true);
        return;
    }
    if (g_main_seen.load()) return;
    uint64_t want = ~0ull;
    if (!g_first_tid.compare_exchange_strong(want, ctx->thread_id)) {
        if (want != ctx->thread_id) return;
    } else {
        fprintf(stderr, "[if2-task] %.1fs cellSysutilCheckCallback polled from lr 0x%08X (thread %llu), not the "
                        "main loop's 0x%08X: watching from it until the main loop polls; no jump from there "
                        "for %d polls\n", secs(), lr, (unsigned long long)ctx->thread_id, kMainLoopRet,
                kOtherActAfter);
        fflush(stderr);
    }
    std::unique_lock<std::mutex> lk(g_tick_mx, std::try_to_lock);
    if (!lk.owns_lock()) return;
    /* A build whose main loop calls through another return address (a
     * relift): the only thread that ever polls is the main one after all. */
    const bool act = ++g_other_polls > kOtherActAfter;
    if (g_other_polls == kOtherActAfter + 1) {
        fprintf(stderr, "[if2-task] %.1fs no main-loop poll in %d polls: acting from thread %llu (lr 0x%08X)\n",
                secs(), kOtherActAfter, (unsigned long long)ctx->thread_id, lr);
        fflush(stderr);
    }
    task_tick(ctx, act);
}

/* The stock handler, then the task graph. */
void if2_cellSysutilCheckCallback(ppu_context* ctx)
{
    const int32_t rc = cellSysutilCheckCallback();
    poll(ctx);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

void reset(void)
{
    g_tasklog = 0; g_jmode = 0; g_jnum = 0; g_joid = 0; g_jmsn = kMsnPrologue; g_jafter = 0; g_jdelay = 0;
    g_warp = true; g_wflags = kWarpFlags;
    g_status.store(0);
    g_status_what[0] = 0;
    g_nodes_ea = 0; g_nodes_n = 0; g_msn = -1; g_msn_active = false; g_wait[0] = 0;
    g_jdone = false; g_calls = 0; g_polls = 0; g_idle = 0; g_busy = 0; g_tries = 0; g_retry_at = 0;
    g_marked = false;
    g_watch = false; g_warp_at = 0; g_flag_set = false; g_wbusy = 0; g_done_what[0] = 0;
    g_main_seen.store(0); g_first_tid.store(~0ull); g_other_polls = 0;
    g_npoll = 0; g_next_beat = 0.0;
}

}  /* namespace */

/* For src/if2_checkpoint.cpp's boot driver: a READY that should mean "at the
 * target step" waits for 2 and fails on 3. */
extern "C" int if2_taskjump_status(char* what, size_t cap)
{
    std::lock_guard<std::mutex> lk(g_status_mx);
    const int s = g_status.load();
    if (what && cap) snprintf(what, cap, "%s", s ? g_status_what : "");
    return s;
}

/* The prologue's active step (src/if2_ss_title.cpp: a save-state slot's
 * description -- the prologue never saves, so the game has no text of its
 * own for it): 1..26 with its name, or 0 when msn_beast_intro is not the
 * active mission. Read-only and independent of the watcher (any thread; the
 * reads are of the graph pack in guest memory, each one range-checked). */
extern "C" int if2_taskgraph_prologue_step(char* name, size_t cap)
{
    if (name && cap) name[0] = 0;
    const uint32_t nodes = vm_read32(kTgNodes);
    const int count = (int16_t)vm_read16(kTgCount);
    if (count <= 0 || count > kMaxNodes || !guest_ok(nodes, 4u * (uint32_t)count)) return 0;
    const uint32_t m = vm_read32(nodes);                  /* node 0: msn_beast_intro */
    if (!guest_ok(m, 0x2C) || vm_read32(m) != kMsnPrologue || (int8_t)vm_read8(m + 8) != 1) return 0;
    const uint32_t desc = vm_read32(m + 0x1C);            /* its children (node_children) */
    if (!guest_ok(desc, 0x14)) return 0;
    const int cnt = (int16_t)vm_read16(desc + 0x08);
    const uint32_t tab = vm_read32(kTgIndex), slot = vm_read16(desc + 0x12);
    if (cnt <= 0 || cnt > kMaxNodes || !guest_ok(tab, 2u * (slot + 1u))) return 0;
    const int lo = (int16_t)vm_read16(tab + 2u * slot);
    if (lo < 0 || lo + cnt > count) return 0;
    int j = 0;
    for (int c = lo; c < lo + cnt; c++) {                 /* its steps in graph order (mission_steps) */
        const uint32_t n = vm_read32(nodes + 4u * (uint32_t)c);
        if (!guest_ok(n, 0x2C) || vm_read8(n + 4) != kKindStep) continue;
        if ((int8_t)vm_read8(n + 8) == 1) {
            if (name && cap) snprintf(name, cap, "%s", j < 26 ? kPrologueSteps[j] : "");
            return j + 1;
        }
        j++;
    }
    return 0;
}

extern "C" int if2_taskgraph_install(void)
{
    reset();
    const char* e = getenv("IF2_TASKLOG");
    if (e && *e && *e != '0') g_tasklog = (*e >= '1' && *e <= '9') ? *e - '0' : 1;
    e = getenv("IF2_TASKJUMP");
    if (e && *e) {
        char* end = nullptr;
        if (!strcmp(e, "list")) g_jmode = 1;
        else if (e[0] == '0' && (e[1] == 'x' || e[1] == 'X')) {
            g_joid = (uint32_t)strtoul(e, &end, 16);
            if (end != e + 2 && !*end) g_jmode = 3;
        } else {
            const long v = strtol(e, &end, 10);
            if (end != e && !*end && v >= 1 && v <= kMaxSteps) { g_jmode = 2; g_jnum = (int)v; }
        }
        if (!g_jmode)
            fprintf(stderr, "[if2-taskjump] IF2_TASKJUMP=%s: expected a step number (1..%d), "
                            "0x<step oid> or \"list\"; ignored\n", e, kMaxSteps);
    }
    e = getenv("IF2_TASKJUMP_MISSION");
    if (e && *e) g_jmsn = (uint32_t)strtoul(e, nullptr, 16);
    e = getenv("IF2_TASKJUMP_AFTER");
    if (e && *e) g_jafter = atoi(e) > 0 && atoi(e) <= kMaxSteps ? atoi(e) : 0;
    e = getenv("IF2_TASKJUMP_DELAY");
    if (e && *e) g_jdelay = atoi(e) > 0 ? atoi(e) : 0;
    e = getenv("IF2_TASKJUMP_MODE");
    if (e && *e) {
        if (!strcmp(e, "graph")) g_warp = false;
        else if (strcmp(e, "warp"))
            fprintf(stderr, "[if2-taskjump] IF2_TASKJUMP_MODE=%s: expected warp or graph; using warp\n", e);
    }
    e = getenv("IF2_TASKJUMP_WARP_FLAGS");
    if (e && *e) g_wflags = (uint32_t)strtoul(e, nullptr, 16);
    if (!g_tasklog && !g_jmode) return 0;
    if (!IF2_HAVE(TASKJUMP)) {
        if2_feature_off("IF2_TASKJUMP / IF2_TASKLOG", "the task graph, its warp manager or the main loop's call");
        g_tasklog = 0; g_jmode = 0;
        return 0;
    }
    g_t0 = std::chrono::steady_clock::now();
    if (g_jmode >= 2) set_status(1, "waiting for the first main-loop poll");
    /* A jump fabricates progress, and the title autosaves it (any mission after
     * the prologue). Outside a CHECKPOINT run's sandbox the saves are the
     * player's own: keep them as they are (libs/system/cellSaveData.c reads
     * PS3_SAVEDATA_READONLY at every save). */
    const char* cp = getenv("CHECKPOINT");
    if (g_jmode >= 2 && !(cp && *cp)) {
        _putenv_s("PS3_SAVEDATA_READONLY", "1");
        fprintf(stderr, "[if2-taskjump] no CHECKPOINT sandbox: PS3_SAVEDATA_READONLY=1, so the fabricated "
                        "progress is never written over the player's saves\n");
    }
    if (g_jmode == 1)
        fprintf(stderr, "[if2-taskjump] mission %08X: list its steps live when it is active\n", g_jmsn);
    else if (g_jmode && g_warp) {
        char fn[160];
        fprintf(stderr, "[if2-taskjump] mission %08X: when it is active, advance it to the IF2_TASKJUMP step, "
                        "then WarpToCheckpoint there (flags 0x%X %s)\n", g_jmsn, g_wflags,
                flag_names(g_wflags, fn, sizeof fn));
    }
    else if (g_jmode)
        fprintf(stderr, "[if2-taskjump] mission %08X: when it is active, advance it to the IF2_TASKJUMP step "
                        "(graph only)\n", g_jmsn);
    ps3_hle_register_ctx(ps3_compute_nid("cellSysutilCheckCallback"), "cellSysutilCheckCallback",
                         if2_cellSysutilCheckCallback);
    return 1;
}

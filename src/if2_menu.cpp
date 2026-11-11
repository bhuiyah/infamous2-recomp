/*
 * inFamous 2 -- native entries in the game's own menus.
 *   N1: QUIT GAME in the pause menu (the game's own Yes/No message box).
 *   N2: PC SETTINGS in the pause menu: the game's own Options screen, filled with the settings store's rows
 *       (include/ps3emu/options.h) in the game's font and look.
 *
 * How the game's menus work (1.04 addresses; the research is in docs/NOTES.md, N1 and N2). A menu is a scene
 * object driven by data scripts that call reflected natives (ADD_WIDGET_ITEM, ADD_NAV_ACTION, PUSH_MENU, ...):
 *   menu+0x324  its items {symbol, widget, -} (12 bytes, sorted by symbol; a row's symbol is its label's string id)
 *   menu+0x334  its actions {kind, button mask, item, target} (16 bytes): kind 0 = Up / Down from item to target,
 *               2 = trigger (the button sends the action id to the menu's script), 3 = trigger on any item (Back,
 *               Start: item -1)
 *   menu+0x344  the focused item; +0x354 the menu that pushed it, +0x358 the one it pushed; +0x31C its
 *               input-listener interface (the per-frame input handler 0x1CD0B8, through the adjustor thunk 0x67AE18)
 * Scene objects: +0x14 -> instance data (+0x10 = the symbol id), +0x1C parent, +0x20 next sibling, +0x110 first
 * child. A text box shows the string id at +0x184 (a record "name\0text" in the language dictionary) and is drawn
 * while it is in the world (+0x58 = 0). Strings live in the dictionary's id -> record hash table (dict+0x30:
 * {buckets, table}; nodes {id, id, record, next}); a record is {id, -> "name\0text", 0, flags}.
 * The Options screen (menu symbol 0x9D86) is built by the pause menu's script on every open: PUSH_MENU, then per
 * row ADD_WIDGET_BY_PROXY on its widget list (symbol 0xF9C; it lays the rows out, at most 32) from the row proxy
 * (symbol 0xA0B2: a "list" widget -- a label in two text boxes, 0xA0E1 / 0xA0E2, and a value Left / Right steps
 * through with the game's arrows, slide and sound; at most 64 values, the current one at +0x580), ADD_ITEM per
 * value, ADD_WIDGET_ITEM with the label's string id as the symbol, Up / Down links. Back (an any-item action to
 * the script) clears the menu's items, pops it and clears the widget list.
 *
 * What this does, all through the game's own functions on the game's main thread, from a wrapper around the
 * menus' input handler (ppu_register_function on the thunk 0x67AE18; the lifted thunk and handler run inside):
 *  - strings: our texts get NEW string ids (0xF2000 up), put into the dictionary with the game's own insert under
 *    its lock (records copy u_menu_options' flags; one per text, found again after a load);
 *  - the top-level pause menu (the one with "Saved Data") gets PC SETTINGS and QUIT GAME after the game's
 *    entries (QUIT GAME alone when one slot is free): labels in the free slots' text boxes, Up / Down links (the
 *    wrap-around links re-targeted, the game's ADD_NAV_ACTION for the new ones), no trigger -- the script never
 *    sees them; Select on them is read from the menu's own joystick with its handler's gating;
 *  - QUIT GAME: the game's Yes/No message box (as Abandon Mission); Yes posts WM_CLOSE (main.cpp's close path);
 *  - PC SETTINGS: pushes the Options menu with PUSH_MENU's C++ and makes its rows (CATEGORY + 10, once per open:
 *    rows made later, while the menu is up, would stay invisible). The push makes the game's script fill the menu
 *    as for its own Options; three natives are wrapped for that fill while our screen owns the menu: its
 *    ADD_WIDGET_ITEMs and ADD_NAV_ACTIONs are not given to the menu, and at its last link its rows leave the widget
 *    list (REMOVE_WIDGET, into the pool) before the frame is drawn; the FOCUS setter keeps the focus on our rows.
 *    CATEGORY steps through the overlay's tabs (then ADVANCED: the troubleshooting rows; a tab over 10 settings is
 *    split), as do L1 / R1 from any row; the other rows are that category's settings, their values the overlay's
 *    own choices / slider stops / presets (ovl_menu_row_positions: one model, two front ends), unused rows out of
 *    the world and out of the Up / Down ring. Left / Right on a row (the game's own list widget: arrows, slide,
 *    sound) sets the setting through ovl_menu_row_choose; an env-locked row has its value alone; RESTART rows are
 *    marked *; a change made anywhere else (the F1 overlay, a preset, a load) moves the rows. The footer is a help
 *    line for the focused row (its tip, locked / next-launch notes) above the button hints. Back or Start clear
 *    the screen the way the game's Back does and pop it.
 * Everything it changes lives in guest memory, so a save state carries it: the screen's own state is a guest
 * block anchored in the dictionary (string id 0xF1FFF); the host keeps caches only, re-checked against guest
 * memory every frame (a load into the open screen re-reads its rows from the store).
 *
 * Environment:
 *   NATIVE_MENU=0    off (the game's menus stay as they are; a PC Settings screen a loaded state brings is closed)
 *   MENU_DIAG=1      log every menu the game opens: its items and actions, with the string names
 *   MENU_TRACE=1     log every call of the menu natives the scripts make (this, arguments, result);
 *                    MENU_TRACE=2 also the property accessors (CURRENT_ITEM, FOCUS, text, ...)
 *   MENU_TEST_SKIP=n test switch: put the entries n slots further down (the layout's later slots)
 * Log lines: [menu].
 * Only the 1.04 EBOOT's addresses are known: on another EBOOT the feature says so and stays off.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "ppu_context.h"
#include "if2_version.h"
#include "ovl_menu.h"        /* the overlay's menu model (libs/video): tabs, rows, choices, setters */
#include "../savestate/ss_state.h"   /* the PCS2 layout section (as infamous2_hooks.cpp) */

extern "C" uint8_t  vm_read8(uint64_t addr);
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void     vm_write8(uint64_t addr, uint8_t val);
extern "C" void     vm_write32(uint64_t addr, uint32_t val);
extern "C" void     ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;          /* runtime/ppu/ppu_loader.cpp */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);     /* lifted tail calls */
extern "C" void     ps3_indirect_call(ppu_context* ctx);     /* runtime/ppu/ppu_loader.cpp: a bctrl to ctx->ctr */

extern "C" int if2_menu_install(void);

#if IF2_HAVE_MENU
#define IF2_MENU_KNOWN 1
/* the lifted functions this calls (generated/ppu) */
void IF2_F(MF_INPUT_THUNK)(ppu_context* ctx);   /* the menus' per-frame input handler, as the input system calls it: the
                                           * adjustor thunk `addi r3,r3,-0x31C; b 0x1CD0B8` (OPD 0x9BA628) */
void IF2_F(MF_ADD_NAV_ACTION)(ppu_context* ctx);   /* ADD_NAV_ACTION(menu, mask, from, to) */
void IF2_F(MF_DICT_LOOKUP)(ppu_context* ctx);   /* string dictionary lookup (dict, id, 0) -> {id, "name\0text"} */
void IF2_F(MF_TB_SET_STRING)(ppu_context* ctx);   /* text box: set string id (tb, id) */
void IF2_F(MF_NEW)(ppu_context* ctx);   /* operator new (size) */
void IF2_F(MF_OBJ_ADD)(ppu_context* ctx);   /* scene object ADD (the script's ADD): back into the world, if out (+0x58) */
void IF2_F(MF_MENU_JOYSTICK)(ppu_context* ctx);   /* the menu's joystick (menu) -> joystick object, 0 = none */
void IF2_F(MF_UI_INPUT)(ppu_context* ctx);   /* the UI that has the input (ui mgr, 1, menu's screen id, menu) */
void IF2_F(MF_MSGBOX)(ppu_context* ctx);   /* message box (title, text, buttons, 0, 0, 0, callback OPD, 0) */
void IF2_F(MF_MENU_BY_SYMBOL)(ppu_context* ctx);   /* the menu with this symbol (id) -> menu, 0 */
void IF2_F(MF_PUSH_MENU)(ppu_context* ctx);   /* PUSH_MENU's C++ (menu, id) -> the menu pushed */
void IF2_F(MF_POP_MENU)(ppu_context* ctx);   /* POP_MENU's C++ (menu) */
void IF2_F(MF_CLEAR_ITEMS)(ppu_context* ctx);   /* CLEAR_ITEMS(menu): its items and the actions bound to them */
void IF2_F(MF_ADD_WIDGET_ITEM)(ppu_context* ctx);   /* ADD_WIDGET_ITEM(menu, widget, symbol) */
void IF2_F(MF_FOCUS)(ppu_context* ctx);   /* focus (menu, symbol) */
void IF2_F(MF_ADD_WIDGET_BY_PROXY)(ppu_context* ctx);   /* ADD_WIDGET_BY_PROXY's C++ (widget list, proxy) -> the new row */
void IF2_F(MF_WLIST_CLEAR)(ppu_context* ctx);   /* CLEAR(widget list): its rows go back to its pool */
void IF2_F(MF_ADD_ITEM)(ppu_context* ctx);   /* ADD_ITEM's C++ (list widget, key, label id, 0, colour) */
void IF2_F(MF_SET_CURRENT_INDEX)(ppu_context* ctx);   /* CURRENT_INDEX's setter (list widget, index) */
void IF2_F(MF_MULTIMAP_INSERT)(ppu_context* ctx);   /* hash multimap insert (table, &key, &value) */
void IF2_F(MF_LWMUTEX_LOCK)(ppu_context* ctx);   /* sys_lwmutex_lock (import stub) */
void IF2_F(MF_LWMUTEX_UNLOCK)(ppu_context* ctx);   /* sys_lwmutex_unlock (import stub) */
void IF2_F(MF_DICT_LOCK_MAKE)(ppu_context* ctx);   /* the dictionary's lock: made on first use */
void IF2_F(MF_REMOVE_WIDGET)(ppu_context* ctx);   /* REMOVE_WIDGET's C++ (widget list, row): out of the list, into its pool */
void IF2_F(MF_SET_HANDLER)(ppu_context* ctx);   /* SET_HANDLER (widget, [handler]) -- the native */
void IF2_F(MF_OBJ_REMOVE)(ppu_context* ctx);   /* scene object REMOVE (the script's REMOVE): out of the world (+0x58) */
void IF2_F(MF_NAT_ADD_WIDGET_ITEM)(ppu_context* ctx);   /* the ADD_WIDGET_ITEM native */
void IF2_F(MF_NAT_ADD_NAV_ACTION)(ppu_context* ctx);    /* the ADD_NAV_ACTION native */
/* MENU_TRACE: the menu natives the scripts call, and (MENU_TRACE=2) the property accessors: the ones the
 * resolver found in this EBOOT, as IF2_TRACE_NATIVES / IF2_TRACE_ACCESSORS (generated/if2_resolved.h) */
#define IF2_DECL_NATIVE(a, n) void func_##a(ppu_context* ctx);
IF2_TRACE_NATIVES(IF2_DECL_NATIVE)
IF2_TRACE_ACCESSORS(IF2_DECL_NATIVE)
#else
#define IF2_MENU_KNOWN 0
#endif

namespace {

#if IF2_MENU_KNOWN
/* (1.04: 0x9DB730, 0x67AE18, 0x9ACB10, 0x9A5DD0, 0x9A5E14; the vtables 0x940018, 0x954890, 0x954120, 0x943760,
 * 0x9489E0) */
constexpr uint32_t kToc       = IF2_R(TOC);              /* the EBOOT's TOC */
constexpr uint32_t kMenuInput = IF2_R(MF_INPUT_THUNK);   /* key of the menus' input-handler thunk in the indirect-call table */
constexpr uint32_t kStrMgr    = IF2_R(MD_STR_MGR);       /* -> string manager; its dictionary at +8 (func_0039C67C) */
constexpr uint32_t kUiMgr     = IF2_R(MD_UI_MGR);        /* -> the UI manager the menu handler asks who has the input */
constexpr uint32_t kRepeatT   = IF2_R(MD_REPEAT_T);      /* f32: the menu handler's button-query argument (0.15) */
constexpr uint32_t kCbKey     = 0x00000400;   /* our message-box callback's "code address": never guest code */
constexpr uint32_t kYes       = 7;            /* a Yes/No box's Yes (the Abandon Mission callback 0x1C78C0) */
constexpr int      kMaxItems  = 32;
constexpr int      kMaxActs   = 96;

/* menu fields */
constexpr uint32_t M_ITEMS = 0x324, M_ACTS = 0x334, M_FOCUS = 0x344, M_TB0 = 0x110;
constexpr uint32_t M_SCREEN = 0x14, M_BUSY = 0x364;    /* -> its screen (+0x248: id); > 0: not taking input */
/* text box fields */
constexpr uint32_t T_NEXT = 0x38, T_LABEL = 0x184;
/* scene object: 0 while it is in the world (drawn); the script REMOVEs the text boxes of the slots it does not use */
constexpr uint32_t O_OUT = 0x58;
/* scene tree: instance data (+0x10 its symbol), parent, next sibling, first child */
constexpr uint32_t O_PROTO = 0x14, O_PARENT = 0x1C, O_NEXT = 0x20, O_CHILD = 0x110;
/* list widget: its current value, its colour (ADD_ITEM's default), its ITEMS; widget list: its row count */
constexpr uint32_t L_INDEX = 0x580, L_COLOR = 0x998, W_ROWS = 0x370, W_COUNT = 0x374, L_HANDLER = 0x358;
/* the dictionary: its lock made (byte), its id -> record table */
constexpr uint32_t D_LOCKED = 0x18, D_TABLE = 0x30;

/* the Options screen and its parts (classes by vtable, parts by symbol) */
constexpr uint32_t kOptMenuId = 0x9D86;       /* the Options menu (PUSH_MENU's argument in the pause menu's script) */
constexpr uint32_t kMenuVt = IF2_R(MD_VT_MENU), kListVt = IF2_R(MD_VT_LIST), kWListVt = IF2_R(MD_VT_WLIST),
                   kProxyVt = IF2_R(MD_VT_PROXY), kTextBoxVt = IF2_R(MD_VT_TEXTBOX);
constexpr uint32_t kWListId = 0xF9C, kRowProxyId = 0xA0B2, kLabelTbA = 0xA0E1, kLabelTbB = 0xA0E2;
constexpr uint32_t kNavDpad = 0x5936;         /* u_menu_nav_dpad: "Navigate / Select / Back", the screen's footer */
constexpr uint32_t kTmplStr = 0x5939;         /* u_menu_options: our records copy its flags */
constexpr uint32_t kBtnUp = 1, kBtnDown = 2, kBtnBack = 0x10000000, kBtnStart = 0x1000;
constexpr uint32_t kBtnL1 = 0x100, kBtnR1 = 0x400;   /* the menus' joystick bits for L1 / R1 (seen with MENU_DIAG) */
constexpr int      kMaxRows = 32;             /* the widget list holds 32 rows (CATEGORY + 31 settings) */
constexpr int      kPageRows = 10;            /* settings per page: the screen shows 11 rows above its footer */
constexpr size_t   kValueLen = 24;            /* a longer value label loses its "(...)" (the value column's width) */
constexpr int      kMaxPos = 64;              /* a list widget holds 64 values */

/* our string ids (the game's go up to 0x81671) */
constexpr uint32_t kStateId = 0x000F1FFF, kIdBase = 0x000F2000, kIdEnd = 0x000FFFF0;

/* the screen's state in guest memory: a block after the name of the record kStateId ("u_pc_state\0\0" + pad) */
constexpr uint32_t kBlkMagic = 0x50435332;   /* 'PCS2' */
constexpr uint32_t B_MAGIC = 0x00, B_SERIAL = 0x04, B_OPEN = 0x08, B_MENU = 0x0C, B_PAUSE = 0x10, B_WLIST = 0x14,
                   B_PAGE = 0x18, B_NROWS = 0x1C, B_FOOT = 0x20, B_FOOT_ID = 0x24, B_NVIS = 0x28, B_ROWS = 0x40, B_ROW = 0x40,
                   B_SIZE = 0x40 + kMaxRows * 0x40;
/* a row: its widget, symbol (= label id), the value we last saw it on, its values' hash, its setting's key */
constexpr uint32_t R_WIDGET = 0x00, R_SYM = 0x04, R_LAST = 0x08, R_HASH = 0x0C, R_KEY = 0x10, R_KEYLEN = 0x30;
/* (Every B_* / R_* above is in kPcsFields, the PCS2 section's layout hash: a change refuses older states.) */

/* string names that identify a menu */
const char* const kPauseMark   = "u_menu_saved_data";
const char* const kPcText      = "PC Settings";
const char* const kQuitText    = "Quit Game";
const char* const kConfirmText = "Are you sure you want to quit? Any unsaved progress will be lost.";

constexpr uint32_t kYesNo     = 0x0000054D;   /* u_msgbox_yesno, as 0x1C6DE8 passes it */
#endif /* IF2_MENU_KNOWN */

bool g_on = false;      /* NATIVE_MENU */
int  g_diag = 0;        /* MENU_DIAG */
int  g_skip = 0;        /* MENU_TEST_SKIP: test the layout's later slots (leave that many free slots before ours) */

#if IF2_MENU_KNOWN
/* the last menu looked at (a cache of guest memory, re-validated every frame) */
uint32_t g_menu = 0;            /* menu object */
uint32_t g_menu_sig = 0;        /* its action list's signature when classified */
int      g_menu_kind = 0;       /* 0 not ours, 1 the pause menu */
struct Entry { uint32_t slot, tb[2], label; };
Entry    g_pc = {}, g_quit = {}; /* our entries in that pause menu (slot 0: none) */
uint32_t g_sel_mask = 0;        /* the pause menu's Select button mask (from its trigger actions) */
uint32_t g_said = 0;            /* log limiter: the last state signature reported */
bool     g_sel_prev = false;    /* Select was down on one of our entries last frame (an edge opens it) */
int      g_retry = 0;           /* frames until a pause menu we could not add to is tried again */
uint32_t g_diag_focus = 0;      /* MENU_DIAG: the pause menu's focus last logged */

/* our strings: text -> id, re-checked against the dictionary on every use (a load can put back another one) */
struct Str { std::string text; uint32_t id; };
std::vector<Str> g_str;
uint32_t g_str_next = kIdBase;  /* the next id to try */

/* the settings screen's host side: the overlay's menu model (its own instance) and the pages built from it */
ovl_menu g_model;
bool     g_model_ready = false;
struct PRow { opt_id id; std::string key, label; };
struct Page { std::string name; std::vector<PRow> rows; };
std::vector<Page> g_pages;
int      g_pages_rows = -1;     /* opt_count() when the pages were built */
unsigned g_gen_seen = 0;        /* opt_generation() the rows were last synced to */
uint32_t g_serial_seen = 0;     /* the guest block's serial last seen (another one: a load -- re-sync) */
uint32_t g_serial_next = 0;     /* the next serial to write (from a per-process random start: never one written before) */
int      g_page_pref = 0;       /* the category the screen opens on (the last one shown) */
uint32_t g_foot_sig = 0;        /* what the footer was last made for (focus, page, store generation, block serial) */
bool     g_str_scanned = false; /* our strings already in the dictionary (a state loaded into this process) were read */
int      g_fill_rows = 0;       /* rows of the game's own fill taken off our screen (log) */
uint32_t g_fill_first = 0;      /* the symbol of the fill's first row (its last link leads Down to it) */
/* the interceptors' next function: the lifted native, or MENU_TRACE's wrapper of it */
void (*g_next_awi)(ppu_context*) = nullptr, (*g_next_nav)(ppu_context*) = nullptr;

/* Run a lifted guest function on the calling guest thread (as src/if2_taskgraph.cpp's guest_call):
 * its registers and TLS, a fresh frame 4 KB below the caller's stack pointer, the EBOOT's TOC. */
uint64_t guest_call(ppu_context* caller, void (*fn)(ppu_context*), uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0,
                    uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0, uint64_t a7 = 0)
{
    ppu_context c;
    memcpy(&c, caller, sizeof c);
    c.gpr[1] = (caller->gpr[1] - 0x1000u) & ~0xFull;
    c.gpr[2] = kToc;
    c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3;
    c.gpr[7] = a4; c.gpr[8] = a5; c.gpr[9] = a6; c.gpr[10] = a7;
    c.lr = 0;
    c.reserve_addr = 0; c.reserve_value = 0; c.reserve_valid = 0;
    ppu_context* const saved = g_active_ctx;
    g_active_ctx = &c;
    fn(&c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&c); }
    g_active_ctx = saved;
    return c.gpr[3];
}

bool plausible(uint32_t ea) { return ea >= 0x00010000u && ea < 0xD0000000u && !(ea & 3); }

/* A virtual call: the OPD in slot `slot` of the object's vtable, through the indirect-call dispatcher. */
uint64_t guest_vcall(ppu_context* caller, uint32_t obj, uint32_t slot, uint64_t a1 = 0, double f1 = 0.0)
{
    const uint32_t opd = vm_read32(vm_read32(obj) + slot);
    if (!plausible(opd)) return 0;
    ppu_context c;
    memcpy(&c, caller, sizeof c);
    c.gpr[1] = (caller->gpr[1] - 0x1000u) & ~0xFull;
    c.gpr[2] = vm_read32(opd + 4);
    c.gpr[3] = obj; c.gpr[4] = a1;
    c.fpr[1] = f1;
    c.ctr = vm_read32(opd);
    c.lr = 0;
    c.reserve_addr = 0; c.reserve_value = 0; c.reserve_valid = 0;
    ppu_context* const saved = g_active_ctx;
    g_active_ctx = &c;
    ps3_indirect_call(&c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&c); }
    g_active_ctx = saved;
    return c.gpr[3];
}

uint64_t sx(int32_t v) { return (uint64_t)(int64_t)v; }

/* A script native (this, its argument words, a mask of the ones given), with the argument array 2 KB below the
 * caller's stack pointer (dead space for it; guest_call's frame starts 4 KB below). */
uint64_t call_native(ppu_context* ctx, void (*fn)(ppu_context*), uint32_t self, std::initializer_list<uint32_t> args)
{
    const uint32_t a = (uint32_t)((ctx->gpr[1] - 0x800u) & ~0xFull);
    uint32_t k = 0;
    for (uint32_t v : args) vm_write32(a + 4 * k++, v);
    return guest_call(ctx, fn, self, a, (1u << k) - 1);
}

/* ---- the menu's joystick, with its input handler's gating (func_001CD0B8): the menu has a joystick that is
 * active, it is not busy (+0x364), and no other UI (a message box) has the input. Queries are the joystick's
 * "pressed within the repeat time" (vtable slot 0); a press is used up with slot 4 (as the handler does). */
uint32_t input_joy(ppu_context* ctx, uint32_t menu)
{
    const uint32_t joy = (uint32_t)guest_call(ctx, IF2_F(MF_MENU_JOYSTICK), menu);
    if (!plausible(joy)) return 0;
    if (!(guest_vcall(ctx, joy, 0x24) & 0xFF)) return 0;
    if ((int32_t)vm_read32(menu + M_BUSY) > 0) return 0;
    const uint32_t scr = vm_read32(menu + M_SCREEN);
    if (!plausible(scr)) return 0;
    const uint32_t ui = (uint32_t)guest_call(ctx, IF2_F(MF_UI_INPUT), vm_read32(kUiMgr), 1,
                                             sx((int32_t)vm_read32(scr + 0x248)), menu);
    if (ui && plausible(ui) && vm_read32(ui + 0x190) != 2) return 0;
    return joy;
}
bool joy_pressed(ppu_context* ctx, uint32_t joy, uint32_t mask)
{
    uint32_t tb = vm_read32(kRepeatT);
    float t;
    memcpy(&t, &tb, 4);
    return guest_vcall(ctx, joy, 0, sx((int32_t)mask), (double)t) != 0;
}
void joy_use(ppu_context* ctx, uint32_t joy, uint32_t mask) { guest_vcall(ctx, joy, 4, sx((int32_t)mask)); }

bool select_down(ppu_context* ctx, uint32_t menu, uint32_t mask)
{
    const uint32_t joy = input_joy(ctx, menu);
    return joy && joy_pressed(ctx, joy, mask);
}

std::string guest_str(uint32_t ea, size_t max = 160)
{
    std::string s;
    if (!ea || ea >= 0xD0000000u) return s;
    for (size_t i = 0; i < max; i++) {
        const char c = (char)vm_read8(ea + i);
        if (!c) break;
        s += c;
    }
    return s;
}

/* scene tree */
uint32_t obj_id(uint32_t o)
{
    const uint32_t p = vm_read32(o + O_PROTO);
    return plausible(p) ? vm_read32(p + 0x10) : 0xFFFFFFFFu;
}
/* a child of parent with this vtable and symbol (id -1: any), 0 = none */
uint32_t find_child(uint32_t parent, uint32_t vt, uint32_t id)
{
    if (!plausible(parent)) return 0;
    uint32_t c = vm_read32(parent + O_CHILD);
    for (int k = 0; k < 512 && plausible(c); k++, c = vm_read32(c + O_NEXT)) {
        if (vm_read32(c + O_PARENT) != parent) return 0;
        if (vm_read32(c) == vt && (id == 0xFFFFFFFFu || obj_id(c) == id)) return c;
    }
    return 0;
}

/* ---- strings */
uint32_t dict() { const uint32_t mgr = vm_read32(kStrMgr); return plausible(mgr) ? mgr + 8 : 0; }

/* The dictionary record of a string id: {id, -> "name\0text"} (0: none). */
uint32_t str_rec(ppu_context* ctx, uint32_t id)
{
    if (id == 0xFFFFFFFFu) return 0;
    const uint32_t d = dict();
    if (!d) return 0;
    const uint32_t rec = (uint32_t)guest_call(ctx, IF2_F(MF_DICT_LOOKUP), d, id, 0);
    return plausible(rec) && vm_read32(rec) == id ? rec : 0;
}
std::string str_name(ppu_context* ctx, uint32_t id)
{
    const uint32_t rec = str_rec(ctx, id);
    return rec ? guest_str(vm_read32(rec + 4), 80) : std::string();
}
/* guest memory from the game's allocator, zeroed */
uint32_t guest_new(ppu_context* ctx, uint32_t size)
{
    size = (size + 15) & ~15u;
    const uint32_t p = (uint32_t)guest_call(ctx, IF2_F(MF_NEW), size);
    if (!plausible(p)) return 0;
    for (uint32_t k = 0; k < size; k += 4) vm_write32(p + k, 0);
    return p;
}

/* A new record {id, buf, the flags of u_menu_options' record} in the dictionary: the game's own insert into its
 * id -> record table, under the dictionary's lock (as func_0039A118 takes it). The record's last word is the
 * value the insert copies into its node. Returns the record, 0 on failure. */
uint32_t dict_insert(ppu_context* ctx, uint32_t id, uint32_t buf)
{
    const uint32_t d = dict(), tmpl = str_rec(ctx, kTmplStr);
    if (!d || !tmpl || guest_str(vm_read32(tmpl + 4), 40) != "u_menu_options") return 0;
    const uint32_t rec = guest_new(ctx, 0x20);
    if (!rec) return 0;
    vm_write32(rec, id);
    vm_write32(rec + 4, buf);
    vm_write32(rec + 8, vm_read32(tmpl + 8));
    vm_write32(rec + 12, vm_read32(tmpl + 12));
    vm_write32(rec + 16, rec);
    if (!vm_read8(d + D_LOCKED)) guest_call(ctx, IF2_F(MF_DICT_LOCK_MAKE), d);
    guest_call(ctx, IF2_F(MF_LWMUTEX_LOCK), d, 0);
    guest_call(ctx, IF2_F(MF_MULTIMAP_INSERT), d + D_TABLE, rec, rec + 16);
    guest_call(ctx, IF2_F(MF_LWMUTEX_UNLOCK), d);
    return str_rec(ctx, id) == rec ? rec : 0;
}

/* A new string id with this text ("u_pc_<id>\0text"); 0xFFFFFFFF on failure. */
uint32_t str_new(ppu_context* ctx, const std::string& text)
{
    for (int tries = 0; tries < 256; tries++) {
        const uint32_t id = g_str_next++;
        if (g_str_next >= kIdEnd) g_str_next = kIdBase;
        if (str_rec(ctx, id)) continue;               /* in use (a string of a state loaded since) */
        char name[24];
        snprintf(name, sizeof name, "u_pc_%X", id);
        const uint32_t n = (uint32_t)strlen(name), buf = guest_new(ctx, n + 1 + (uint32_t)text.size() + 1);
        if (!buf) return 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; i++) vm_write8(buf + i, (uint8_t)name[i]);
        for (uint32_t i = 0; i < text.size(); i++) vm_write8(buf + n + 1 + i, (uint8_t)text[i]);
        if (!dict_insert(ctx, id, buf)) return 0xFFFFFFFFu;
        return id;
    }
    return 0xFFFFFFFFu;
}

/* Our strings a loaded state brought along (made by an earlier run): read once, so they are not made twice.
 * Ours are consecutive from kIdBase; the scan stops after 64 ids in a row with no record. */
void str_scan(ppu_context* ctx)
{
    g_str_scanned = true;
    int gap = 0, found = 0;
    for (uint32_t id = kIdBase; id < kIdEnd && gap < 64; id++) {
        const uint32_t rec = str_rec(ctx, id);
        if (!rec) { gap++; continue; }
        gap = 0;
        const uint32_t t = vm_read32(rec + 4);
        const std::string nm = guest_str(t, 40);
        if (nm.compare(0, 5, "u_pc_") != 0) continue;
        const std::string text = guest_str(t + (uint32_t)nm.size() + 1, 400);
        bool have = false;
        for (const Str& s : g_str) if (s.text == text) have = true;
        if (!have) { g_str.push_back(Str{ text, id }); found++; }
        if (id >= g_str_next) g_str_next = id + 1;
    }
    if (found) {
        fprintf(stderr, "[menu] strings: %d of ours already in the dictionary (a loaded state's)\n", found);
        fflush(stderr);
    }
}

/* The string id of a text (made the first time; re-made when the dictionary lost it). */
uint32_t str_id(ppu_context* ctx, const std::string& text)
{
    if (!g_str_scanned) str_scan(ctx);
    for (Str& s : g_str)
        if (s.text == text) {
            if (const uint32_t rec = str_rec(ctx, s.id)) {        /* still ours, with this text? (one lookup) */
                const uint32_t t = vm_read32(rec + 4);
                const std::string nm = guest_str(t, 40);
                if (nm.compare(0, 5, "u_pc_") == 0 && guest_str(t + (uint32_t)nm.size() + 1, 400) == text) return s.id;
            }
            s.id = str_new(ctx, text);
            return s.id;
        }
    const uint32_t id = str_new(ctx, text);
    if (id != 0xFFFFFFFFu) g_str.push_back(Str{ text, id });
    return id;
}

/* The game's font: plain ASCII (x for the multiplication sign, '-' for the middle dot and dashes). */
std::string ascii(const char* s)
{
    std::string o;
    for (const unsigned char* p = (const unsigned char*)s; p && *p; p++) {
        if (*p < 0x80) { o += (char)*p; continue; }
        if (p[0] == 0xC3 && p[1] == 0x97) { o += 'x'; p++; continue; }
        if (p[0] == 0xC2 && p[1] == 0xB7) { o += '-'; p++; continue; }
        if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x93 || p[2] == 0x94)) { o += '-'; p += 2; continue; }
        while ((p[1] & 0xC0) == 0x80) p++;            /* anything else: left out */
    }
    return o;
}

struct Act { uint32_t kind, mask, item, target, ea; };

int read_acts(uint32_t menu, Act* a, int cap)
{
    const uint32_t base = vm_read32(menu + M_ACTS);
    const int n = (int)vm_read32(menu + M_ACTS + 4);
    if (n < 0 || n > cap || (n && !plausible(base))) return -1;
    for (int i = 0; i < n; i++) {
        const uint32_t e = base + 16u * (uint32_t)i;
        a[i] = Act{ vm_read32(e), vm_read32(e + 4), vm_read32(e + 8), vm_read32(e + 12), e };
    }
    return n;
}
int read_items(uint32_t menu, uint32_t* key, uint32_t* widget, int cap)
{
    const uint32_t base = vm_read32(menu + M_ITEMS);
    const int n = (int)vm_read32(menu + M_ITEMS + 4);
    if (n < 0 || n > cap || (n && !plausible(base))) return -1;
    for (int i = 0; i < n; i++) {   /* {symbol, widget, item object}: the pause menu's slots are item objects */
        const uint32_t e = base + 12u * (uint32_t)i;
        key[i] = vm_read32(e);
        widget[i] = vm_read32(e + 8) ? vm_read32(e + 8) : vm_read32(e + 4);
    }
    return n;
}
/* a cheap signature of the action list: changes when the script rebuilds it */
uint32_t act_sig(uint32_t menu)
{
    const uint32_t base = vm_read32(menu + M_ACTS), n = vm_read32(menu + M_ACTS + 4);
    uint32_t h = base * 2654435761u ^ n;
    if (n && n <= (uint32_t)kMaxActs && plausible(base))
        for (uint32_t i = 0; i < n; i++) h = (h ^ vm_read32(base + 16u * i + 12)) * 16777619u;
    return h;
}

void diag_menu(ppu_context* ctx, uint32_t menu)
{
    Act a[kMaxActs];
    uint32_t key[kMaxItems], wid[kMaxItems];
    const int na = read_acts(menu, a, kMaxActs), ni = read_items(menu, key, wid, kMaxItems);
    fprintf(stderr, "[menu] menu 0x%08X vtable 0x%08X: %d item(s), %d action(s), focus 0x%X\n", menu,
            vm_read32(menu), ni, na, vm_read32(menu + M_FOCUS));
    for (int i = 0; i < ni; i++)
        fprintf(stderr, "[menu]   item 0x%X widget 0x%08X (vtable 0x%08X)\n", key[i], wid[i],
                plausible(wid[i]) ? vm_read32(wid[i]) : 0);
    for (int i = 0; i < na; i++)
        fprintf(stderr, "[menu]   action kind %u mask 0x%08X item 0x%X -> 0x%X %s\n", a[i].kind, a[i].mask, a[i].item,
                a[i].target, a[i].kind >= 2 ? str_name(ctx, a[i].target).c_str() : "");
    uint32_t tb = vm_read32(menu + M_TB0);
    for (int k = 0; k < 24 && plausible(tb); k++, tb = vm_read32(tb + T_NEXT))
        fprintf(stderr, "[menu]   text box 0x%08X vtable 0x%08X string 0x%X %s\n", tb, vm_read32(tb),
                vm_read32(tb + T_LABEL), str_name(ctx, vm_read32(tb + T_LABEL)).c_str());
    fflush(stderr);
}

/* Is this the top-level pause menu? (an entry named "u_menu_saved_data") */
bool is_pause_menu(ppu_context* ctx, uint32_t menu)
{
    Act a[kMaxActs];
    const int n = read_acts(menu, a, kMaxActs);
    for (int i = 0; i < n; i++)
        if (a[i].kind == 2 && str_name(ctx, a[i].target) == kPauseMark) return true;
    return false;
}

/* An entry is in when its slot is a navigation target. */
bool entry_present(uint32_t menu, uint32_t slot)
{
    Act a[kMaxActs];
    const int n = read_acts(menu, a, kMaxActs);
    for (int i = 0; i < n; i++)
        if (a[i].kind == 0 && a[i].target == slot) return true;
    return false;
}

void say_once(uint32_t sig, const char* fmt, ...)
{
    if (sig == g_said) return;
    g_said = sig;
    va_list ap;
    va_start(ap, fmt);
    fputs("[menu] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputs("\n", stderr);
    va_end(ap);
    fflush(stderr);
}

/* An entry's label in its slot's text boxes, and the text boxes in the world (drawn): the script REMOVEs the
 * text boxes of the slots it does not use when the menu opens, and sets their strings when it uses them. */
void show_label(ppu_context* ctx, const Entry& e)
{
    if (!e.slot) return;
    for (uint32_t tb : e.tb) {
        if (!plausible(tb)) continue;
        if (vm_read32(tb + T_LABEL) != e.label) guest_call(ctx, IF2_F(MF_TB_SET_STRING), tb, e.label);
        if (vm_read8(tb + O_OUT)) guest_call(ctx, IF2_F(MF_OBJ_ADD), tb);
    }
}
bool label_shown(const Entry& e)
{
    return !e.slot || (vm_read32(e.tb[0] + T_LABEL) == e.label && !vm_read8(e.tb[0] + O_OUT) &&
                       vm_read32(e.tb[1] + T_LABEL) == e.label && !vm_read8(e.tb[1] + O_OUT));
}

/* Add PC SETTINGS and QUIT GAME (QUIT GAME alone when one slot is free) to the pause menu just (re)built by its
 * script, after its entries. Fills g_pc / g_quit; true when at least QUIT GAME is in. */
bool add_entries(ppu_context* ctx, uint32_t menu)
{
    g_pc = Entry{}; g_quit = Entry{};
    Act a[kMaxActs];
    uint32_t key[kMaxItems], wid[kMaxItems];
    const int na = read_acts(menu, a, kMaxActs), ni = read_items(menu, key, wid, kMaxItems);
    if (na <= 0 || ni <= 0) return false;
    /* the entries the script shows: their order in the item list, their label (= action) ids */
    int used = 0;
    uint32_t sel = 0, label[kMaxItems];
    for (int i = 0; i < ni; i++) {
        label[i] = 0xFFFFFFFFu;
        for (int j = 0; j < na; j++)
            if (a[j].kind == 2 && a[j].item == key[i]) { label[i] = a[j].target; sel = a[j].mask; }
    }
    while (used < ni && label[used] != 0xFFFFFFFFu) used++;
    for (int i = used; i < ni; i++)
        if (label[i] != 0xFFFFFFFFu) { say_once(0x51000000u ^ (uint32_t)used, "pause menu: its entries are not the first slots -- no entries of ours here"); return false; }
    const int at = used + g_skip;          /* our first slot: the first free one (MENU_TEST_SKIP: a later one) */
    /* (3 entries at least: with 2, both of the first entry's links lead to the last, and the wrap-around pair
     * below could not be told from the plain Up / Down pair) */
    if (used < 3 || at >= ni || !sel) { say_once(0x52000000u ^ (uint32_t)used, "pause menu: %d entries, no free slot -- no entries of ours here", used); return false; }
    const int n = ni - at >= 2 ? 2 : 1;    /* PC SETTINGS + QUIT GAME, or QUIT GAME alone (it stays the last row) */
    for (int k = 0; k < n; k++)
        if (!plausible(wid[at + k]) || !plausible(wid[0]) || vm_read32(wid[at + k]) != vm_read32(wid[0])) {
            say_once(0x53000000u ^ key[at + k], "pause menu: item 0x%X after the entries is not an entry slot -- no entries of ours here", key[at + k]);
            return false;
        }
    /* their text boxes: the scene list holds two per slot, in slot order (checked on the used ones) */
    uint32_t tbs[2 * kMaxItems];
    int nt = 0;
    const uint32_t tb0 = vm_read32(menu + M_TB0);
    const uint32_t tvt = plausible(tb0) ? vm_read32(tb0) : 0;
    for (uint32_t tb = tb0; plausible(tb) && nt < 2 * (at + n); tb = vm_read32(tb + T_NEXT))
        if (vm_read32(tb) == tvt) tbs[nt++] = tb;
    if (nt < 2 * (at + n)) { say_once(0x54000000u ^ (uint32_t)nt, "pause menu: %d text boxes -- no entries of ours here", nt); return false; }
    for (int i = 0; i < used; i++)
        if (vm_read32(tbs[2 * i] + T_LABEL) != label[i] || vm_read32(tbs[2 * i + 1] + T_LABEL) != label[i]) {
            say_once(0x55000000u ^ (uint32_t)i, "pause menu: text boxes do not match its entries (slot %d) -- no entries of ours here", i);
            return false;
        }
    const uint32_t pc_id = n == 2 ? str_id(ctx, kPcText) : 0, quit_id = str_id(ctx, kQuitText);
    if (quit_id == 0xFFFFFFFFu || pc_id == 0xFFFFFFFFu) {
        say_once(0x58000000u, "pause menu: our strings could not be added to the game's dictionary -- no entries of ours");
        return false;
    }
    Entry e[2];
    for (int k = 0; k < n; k++)
        e[k] = Entry{ key[at + k], { tbs[2 * (at + k)], tbs[2 * (at + k) + 1] }, (n == 2 && k == 0) ? pc_id : quit_id };
    /* already in (the list was re-read after we added them): the slots that are navigation targets */
    if (entry_present(menu, e[0].slot)) {
        g_sel_mask = sel;
        if (n == 2 && entry_present(menu, e[1].slot)) { g_pc = e[0]; g_quit = e[1]; }
        else { g_quit = e[0]; g_quit.label = quit_id; }
        return true;
    }
    for (int k = 0; k < n; k++) {
        const uint32_t la = vm_read32(e[k].tb[0] + T_LABEL), lb = vm_read32(e[k].tb[1] + T_LABEL);
        if ((la != 0xFFFFFFFFu && la != e[k].label) || (lb != 0xFFFFFFFFu && lb != e[k].label)) {
            say_once(0x56000000u ^ la, "pause menu: a free slot's text boxes are in use (0x%X) -- no entries of ours here", la);
            return false;
        }
    }
    /* the Up / Down links: the wrap-around pair from the last entry to the first */
    const uint32_t first = key[0], last = key[used - 1];
    int down = -1, up = -1;
    for (int j = 0; j < na; j++) {
        if (a[j].kind != 0) continue;
        if (a[j].item == last && a[j].target == first) down = j;
        if (a[j].item == first && a[j].target == last) up = j;
    }
    if (down < 0 || up < 0) { say_once(0x57000000u, "pause menu: no wrap-around navigation -- no entries of ours here"); return false; }
    for (int k = 0; k < n; k++) show_label(ctx, e[k]);
    const uint32_t mdown = a[down].mask, mup = a[up].mask;
    vm_write32(a[down].ea + 12, e[0].slot);      /* last --Down--> our first */
    vm_write32(a[up].ea + 12, e[n - 1].slot);    /* first --Up--> our last */
    guest_call(ctx, IF2_F(MF_ADD_NAV_ACTION), menu, mup, e[0].slot, last);              /* our first --Up--> last */
    for (int k = 0; k + 1 < n; k++) {
        guest_call(ctx, IF2_F(MF_ADD_NAV_ACTION), menu, mdown, e[k].slot, e[k + 1].slot);
        guest_call(ctx, IF2_F(MF_ADD_NAV_ACTION), menu, mup, e[k + 1].slot, e[k].slot);
    }
    guest_call(ctx, IF2_F(MF_ADD_NAV_ACTION), menu, mdown, e[n - 1].slot, first);       /* our last --Down--> first */
    g_sel_mask = sel;
    if (n == 2) { g_pc = e[0]; g_quit = e[1]; } else g_quit = e[0];
    if (n == 2) fprintf(stderr, "[menu] pause menu 0x%08X: PC Settings and Quit Game added as entries %d-%d (items", menu,
                        at + 1, at + 2);
    else fprintf(stderr, "[menu] pause menu 0x%08X: Quit Game added as entry %d (item", menu, at + 1);
    for (int k = 0; k < n; k++) fprintf(stderr, " 0x%X", e[k].slot);
    fprintf(stderr, ", strings");
    for (int k = 0; k < n; k++) fprintf(stderr, " 0x%X", e[k].label);
    fprintf(stderr, "; Up 0x%X Down 0x%X Select 0x%X)\n", mup, mdown, sel);
    fflush(stderr);
    return entry_present(menu, e[n - 1].slot);
}

/* The game's Yes/No message box over the pause menu. */
void confirm_quit(ppu_context* ctx)
{
    const uint32_t title = str_id(ctx, kQuitText), text = str_id(ctx, kConfirmText);
    if (title == 0xFFFFFFFFu || text == 0xFFFFFFFFu) { fprintf(stderr, "[menu] Quit Game: no strings for the message box\n"); return; }
    /* the callback's OPD {key, TOC}: guest memory from the game's allocator */
    const uint32_t opd = (uint32_t)guest_call(ctx, IF2_F(MF_NEW), 16);
    if (!plausible(opd)) { fprintf(stderr, "[menu] Quit Game: no memory for the message box callback\n"); return; }
    vm_write32(opd, kCbKey);
    vm_write32(opd + 4, kToc);
    const uint32_t h = (uint32_t)guest_call(ctx, IF2_F(MF_MSGBOX), title, text, kYesNo, 0, 0, 0, opd, 0);
    if (!plausible(h)) {
        fprintf(stderr, "[menu] Quit Game selected: the game's message box did not open (0x%08X)\n", h);
        fflush(stderr);
        return;
    }
    vm_write32(h, vm_read32(h) + 1);          /* hold a reference, as the game's callers do (never released: 12 bytes) */
    fprintf(stderr, "[menu] Quit Game selected: the game's Yes/No message box is up (handle 0x%08X)\n", h);
    fflush(stderr);
}

/* ================================================================ PC SETTINGS: the game's Options screen, ours */

/* The screen's state block in guest memory (0: none). make: create it the first time. */
uint32_t blk_find(ppu_context* ctx)
{
    const uint32_t rec = str_rec(ctx, kStateId);
    if (!rec) return 0;
    const uint32_t buf = vm_read32(rec + 4);
    if (!plausible(buf) || guest_str(buf, 16) != "u_pc_state") return 0;
    return vm_read32(buf + 0x10 + B_MAGIC) == kBlkMagic ? buf + 0x10 : 0;
}
uint32_t blk_get(ppu_context* ctx)
{
    if (const uint32_t b = blk_find(ctx)) return b;
    if (str_rec(ctx, kStateId)) return 0;             /* the id holds something else */
    const uint32_t buf = guest_new(ctx, 0x10 + B_SIZE);
    if (!buf) return 0;
    const char* nm = "u_pc_state";
    for (uint32_t i = 0; nm[i]; i++) vm_write8(buf + i, (uint8_t)nm[i]);
    vm_write32(buf + 0x10 + B_MAGIC, kBlkMagic);
    return dict_insert(ctx, kStateId, buf) ? buf + 0x10 : 0;
}
/* A new serial: unique to this process (a random start, then +1), so one a load puts back -- a state of another
 * session, or of this one before the last change -- is never the one last written, and the rows are re-read. */
void blk_touch(uint32_t b)
{
    if (!g_serial_next) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        g_serial_next = (uint32_t)(((uint64_t)q.QuadPart * 0x9E3779B97F4A7C15ull) >> 32) ^ (GetCurrentProcessId() << 16);
    }
    if (!++g_serial_next) g_serial_next = 1;
    vm_write32(b + B_SERIAL, g_serial_next);
    g_serial_seen = g_serial_next;
}
uint32_t row_ea(uint32_t b, int r) { return b + B_ROWS + B_ROW * (uint32_t)r; }
std::string row_key(uint32_t b, int r) { return guest_str(row_ea(b, r) + R_KEY, R_KEYLEN - 1); }
void row_set_key(uint32_t b, int r, const std::string& k)
{
    const uint32_t e = row_ea(b, r) + R_KEY;
    for (uint32_t i = 0; i < R_KEYLEN; i++) vm_write8(e + i, i < k.size() && i + 1 < R_KEYLEN ? (uint8_t)k[i] : 0);
}

/* The pages: the overlay's tabs with their settings (and the Graphics tab's preset row), then "Advanced" with the
 * troubleshooting (OPT_F_ADVANCED) rows of every tab. Hidden rows are never shown. */
void build_pages()
{
    if (!g_model_ready) { ovl_menu_init(&g_model); g_model_ready = true; }
    ovl_menu_env_now(&g_model.env);
    ovl_menu_opened(&g_model);
    g_pages.clear();
    Page adv{ "Advanced", {} };
    for (int t = 0; t < g_model.ntab; t++) {
        ovl_menu_select_tab(&g_model, t);
        Page p{ g_model.tabs[t], {} };
        for (int i = 0; i < g_model.nitems; i++) {
            const ovl_item& it = g_model.items[i];
            if (it.kind == OVL_IT_PRESET) { p.rows.push_back(PRow{ OVL_ROW_PRESET, "@preset", it.label }); continue; }
            if (it.kind != OVL_IT_SETTING) continue;
            const opt_def* d = opt_def_of(it.id);
            if (!d || (d->flags & OPT_F_HIDDEN)) continue;
            ((d->flags & OPT_F_ADVANCED) ? adv : p).rows.push_back(PRow{ it.id, d->key, it.label });
        }
        if (!p.rows.empty()) g_pages.push_back(p);
    }
    if (!adv.rows.empty()) g_pages.push_back(adv);
    /* a tab with more settings than the screen shows: "Tab 1/2", "Tab 2/2" */
    std::vector<Page> out;
    for (const Page& p : g_pages) {
        const int parts = ((int)p.rows.size() + kPageRows - 1) / kPageRows;
        for (int k = 0; k < parts; k++) {
            Page q{ p.name, {} };
            if (parts > 1) q.name += " " + std::to_string(k + 1) + "/" + std::to_string(parts);
            for (int i = k * kPageRows; i < (int)p.rows.size() && i < (k + 1) * kPageRows; i++) q.rows.push_back(p.rows[i]);
            out.push_back(q);
        }
    }
    g_pages.swap(out);
    g_pages_rows = opt_count();
}

const PRow* page_row(int page, const std::string& key)
{
    if (page < 0 || page >= (int)g_pages.size()) return nullptr;
    for (const PRow& r : g_pages[page].rows) if (r.key == key) return &r;
    return nullptr;
}
opt_id row_id(const std::string& key) { return key == "@preset" ? OVL_ROW_PRESET : opt_find(key.c_str()); }

/* A row's values (the labels its list steps through) and the current one. The CATEGORY row: the pages.
 * An env-locked row: its current value alone (it cannot change). */
std::vector<std::string> row_values(int page, const std::string& key, int* cur)
{
    std::vector<std::string> v;
    *cur = 0;
    if (key == "@page") {
        for (const Page& p : g_pages) v.push_back(p.name);
        *cur = page;
        return v;
    }
    const opt_id id = row_id(key);
    std::vector<char> buf((size_t)kMaxPos * 80);
    char (*lab)[80] = reinterpret_cast<char (*)[80]>(buf.data());
    int c = -1;
    const int n = ovl_menu_row_positions(&g_model, id, lab, kMaxPos, &c);
    const bool locked = id >= 0 && opt_locked(id, nullptr);
    for (int i = 0; i < n; i++)
        if (!locked || i == c) {
            std::string t = ascii(lab[i]);
            const size_t par = t.find(" (");
            if (t.size() > kValueLen && par != std::string::npos && par > 0) t.resize(par);
            v.push_back(t);
        }
    if (v.empty()) v.push_back("-");
    *cur = locked ? 0 : (c < 0 ? 0 : c);
    if (id == OVL_ROW_PRESET && c < 0) { v.push_back("Custom"); *cur = (int)v.size() - 1; }
    return v;
}
uint32_t values_hash(const std::vector<std::string>& v)
{
    uint32_t h = 2166136261u;
    for (const std::string& s : v) { for (char ch : s) h = (h ^ (uint8_t)ch) * 16777619u; h = (h ^ 0xFFu) * 16777619u; }
    return h;
}
/* the label a row shows: RESTART rows marked * (their change takes effect at the next launch) */
std::string row_label(const PRow& r)
{
    std::string s = ascii(r.label.c_str());
    const opt_def* d = r.id >= 0 ? opt_def_of(r.id) : nullptr;
    if (d && d->apply == OPT_RESTART) s += " *";
    return s + ":";
}

/* A list widget's values: its old ones out, one item per label (the label's string id as key and label, the
 * widget's own colour), the current one selected. False when a label got no string id (no guest memory, no ids
 * left): its item is missing, so the list's indices no longer match the values -- the row takes no change then. */
bool fill_row(ppu_context* ctx, uint32_t w, const std::vector<std::string>& vals, int cur)
{
    guest_vcall(ctx, w, 0x160, sx(-1));                       /* CLEAR_ITEMS(list) */
    const uint32_t color = vm_read32(w + L_COLOR);
    std::vector<uint32_t> ids;
    bool all = true;
    for (const std::string& s : vals) {
        uint32_t id = str_id(ctx, s);
        for (uint32_t k : ids) if (k == id) { id = str_new(ctx, s); break; }   /* keys are unique per list */
        if (id == 0xFFFFFFFFu) { all = false; continue; }
        ids.push_back(id);
        guest_call(ctx, IF2_F(MF_ADD_ITEM), w, id, id, 0, color);
    }
    guest_call(ctx, IF2_F(MF_SET_CURRENT_INDEX), w, sx(cur < 0 ? 0 : cur));
    if (!all) fprintf(stderr, "[menu] PC Settings: a value got no string -- that row takes no change\n");
    return all;
}
void set_row_label(ppu_context* ctx, uint32_t w, uint32_t sym)
{
    for (uint32_t id : { kLabelTbA, kLabelTbB })
        if (const uint32_t tb = find_child(w, kTextBoxVt, id))
            if (vm_read32(tb + T_LABEL) != sym) guest_call(ctx, IF2_F(MF_TB_SET_STRING), tb, sym);
}

/* Empty the screen and pop it: the menu's items and their links, the pop, the rows' values, the widget list (its
 * rows go back to its pool) -- the order the game's Back uses. */
void clear_screen(ppu_context* ctx, uint32_t b)
{
    const uint32_t menu = vm_read32(b + B_MENU), wl = vm_read32(b + B_WLIST);
    const int n = (int)vm_read32(b + B_NROWS);
    guest_call(ctx, IF2_F(MF_CLEAR_ITEMS), menu);
    guest_call(ctx, IF2_F(MF_POP_MENU), menu);
    for (int r = 0; r < n && r < kMaxRows; r++) {
        const uint32_t w = vm_read32(row_ea(b, r) + R_WIDGET);
        if (plausible(w) && vm_read32(w) == kListVt) guest_vcall(ctx, w, 0x160, sx(-1));
    }
    if (plausible(wl) && vm_read32(wl) == kWListVt) guest_call(ctx, IF2_F(MF_WLIST_CLEAR), wl);
    vm_write32(b + B_NROWS, 0);
}

/* our row symbols: a string id per row slot (never shown -- the label text boxes show the labels) */
uint32_t row_sym(ppu_context* ctx, int k) { return str_id(ctx, "#pc-settings-row-" + std::to_string(k)); }

/* a row in the world (drawn) or out of it, as the scripts' ADD / REMOVE */
void row_show(ppu_context* ctx, uint32_t w, bool on)
{
    if (on && vm_read8(w + O_OUT)) guest_call(ctx, IF2_F(MF_OBJ_ADD), w);
    else if (!on && !vm_read8(w + O_OUT)) guest_call(ctx, IF2_F(MF_OBJ_REMOVE), w);
}

/* The screen's rows, once per open (rows made while the menu is up stay invisible until its next activation, so a
 * category change re-fills these): CATEGORY + kPageRows list rows from the game's row proxy, no handler, items of
 * the menu under our row symbols, one Up and one Down action each (show_page aims them). Returns the rows made. */
int make_rows(ppu_context* ctx, uint32_t b)
{
    const uint32_t menu = vm_read32(b + B_MENU), wl = vm_read32(b + B_WLIST);
    const uint32_t px = find_child(wl, kProxyVt, kRowProxyId);
    const int want = kPageRows + 1;
    int n = 0;
    uint32_t syms[kMaxRows];
    for (int k = 0; k < want && px; k++) {
        const uint32_t sym = row_sym(ctx, k);
        if (sym == 0xFFFFFFFFu) break;
        const uint32_t w = (uint32_t)guest_call(ctx, IF2_F(MF_ADD_WIDGET_BY_PROXY), wl, px);
        if (!plausible(w) || vm_read32(w) != kListVt) {
            fprintf(stderr, "[menu] PC Settings: the game made no row %d (0x%08X)\n", k, w);
            break;
        }
        if (vm_read32(w + L_HANDLER)) call_native(ctx, IF2_F(MF_SET_HANDLER), w, { 0 });   /* SET_HANDLER(row, none) */
        guest_call(ctx, IF2_F(MF_ADD_WIDGET_ITEM), menu, w, sym);
        const uint32_t e = row_ea(b, n);
        vm_write32(e + R_WIDGET, w);
        vm_write32(e + R_SYM, sym);
        vm_write32(e + R_LAST, 0);
        vm_write32(e + R_HASH, 0);
        row_set_key(b, n, "");
        syms[n++] = sym;
    }
    for (int k = 0; k < n; k++) {
        guest_call(ctx, IF2_F(MF_ADD_NAV_ACTION), menu, kBtnUp, syms[k], syms[(k + n - 1) % n]);
        guest_call(ctx, IF2_F(MF_ADD_NAV_ACTION), menu, kBtnDown, syms[k], syms[(k + 1) % n]);
    }
    vm_write32(b + B_NROWS, (uint32_t)n);
    vm_write32(b + B_NVIS, 0);
    return n;
}

/* Rows a page does not use: out of the world (an activation of the menu may have put them back). */
void hide_spare_rows(ppu_context* ctx, uint32_t b)
{
    const int n = (int)vm_read32(b + B_NROWS), nvis = (int)vm_read32(b + B_NVIS);
    for (int k = nvis; k < n && k < kMaxRows; k++) {
        const uint32_t w = vm_read32(row_ea(b, k) + R_WIDGET);
        if (plausible(w) && vm_read32(w) == kListVt) row_show(ctx, w, false);
    }
}

/* Show category `page` in the rows: CATEGORY, then its settings (label, values, the current one); the rows after
 * them out of the world; the Up / Down ring over the rows shown (our actions re-aimed in place). */
void show_page(ppu_context* ctx, uint32_t b, int page)
{
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    const uint32_t menu = vm_read32(b + B_MENU);
    const int n = (int)vm_read32(b + B_NROWS);
    if (page < 0 || page >= (int)g_pages.size() || n < 1) return;
    std::vector<std::pair<std::string, std::string>> rows;    /* key, label */
    rows.emplace_back("@page", "Category:");
    for (const PRow& r : g_pages[page].rows)
        if ((int)rows.size() < n) rows.emplace_back(r.key, row_label(r));
    const int nvis = (int)rows.size();
    for (int k = 0; k < n; k++) {
        const uint32_t e = row_ea(b, k), w = vm_read32(e + R_WIDGET);
        if (!plausible(w) || vm_read32(w) != kListVt) continue;
        if (k >= nvis) { row_show(ctx, w, false); row_set_key(b, k, ""); continue; }
        const uint32_t label = str_id(ctx, rows[k].second);
        if (label != 0xFFFFFFFFu) set_row_label(ctx, w, label);
        int cur = 0;
        const std::vector<std::string> vals = row_values(page, rows[k].first, &cur);
        const bool all = fill_row(ctx, w, vals, cur);
        vm_write32(e + R_LAST, vm_read32(w + L_INDEX));
        vm_write32(e + R_HASH, all ? values_hash(vals) : 0);     /* 0: incomplete (re-filled at the next re-sync) */
        row_set_key(b, k, rows[k].first);
        row_show(ctx, w, true);
    }
    /* the ring: Up / Down of each shown row to its neighbours among the shown rows */
    Act a[kMaxActs];
    const int na = read_acts(menu, a, kMaxActs);
    for (int k = 0; k < nvis; k++) {
        const uint32_t sym = vm_read32(row_ea(b, k) + R_SYM);
        const uint32_t up = vm_read32(row_ea(b, (k + nvis - 1) % nvis) + R_SYM);
        const uint32_t down = vm_read32(row_ea(b, (k + 1) % nvis) + R_SYM);
        for (int i = 0; i < na; i++) {
            if (a[i].kind != 0 || a[i].item != sym) continue;
            if (a[i].mask == kBtnUp && a[i].target != up) vm_write32(a[i].ea + 12, up);
            if (a[i].mask == kBtnDown && a[i].target != down) vm_write32(a[i].ea + 12, down);
        }
    }
    vm_write32(b + B_NVIS, (uint32_t)nvis);
    vm_write32(b + B_PAGE, (uint32_t)page);
    g_page_pref = page;
    blk_touch(b);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    fprintf(stderr, "[menu] PC Settings: category %d/%d \"%s\", %d row(s) (%.1f ms)\n", page + 1, (int)g_pages.size(),
            g_pages[page].name.c_str(), nvis - 1, 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)fq.QuadPart);
    fflush(stderr);
}

/* Our footer: a help line for the focused row above the button hints (the screen's "Navigate / Select / Back"). */
const char* const kNavLine = "\xEF\x82\x90 Navigate   \xEF\x82\x93\xEF\x82\x91 Change   "
                             "\xEF\x82\x84\xEF\x82\x86 Category   \xEF\x82\x8F Back";
constexpr size_t kHelpLen = 96;              /* the help line's length (the footer's width) */

/* a tip's first sentence, cut at a word to kHelpLen */
std::string first_sentence(const char* tip)
{
    std::string t = ascii(tip ? tip : "");
    for (size_t i = 0; i + 1 < t.size(); i++)
        if (t[i] == '.' && t[i + 1] == ' ') { t.resize(i + 1); break; }
    if (t.size() > kHelpLen) {
        size_t cut = t.rfind(' ', kHelpLen - 3);
        t.resize(cut == std::string::npos ? kHelpLen - 3 : cut);
        t += "...";
    }
    return t;
}

std::string help_line(uint32_t b, int row)
{
    if (row < 0) return "";
    const std::string key = row_key(b, row);
    if (key == "@page") return "Changes are saved at once. * = takes effect at the next launch.";
    if (key == "@preset") return "Sets the graphics settings below at once (PS3 Original = the PS3's picture).";
    const opt_id id = row_id(key);
    const opt_def* d = id >= 0 ? opt_def_of(id) : nullptr;
    if (!d) return "";
    const char* env = nullptr;
    if (opt_locked(id, &env)) {
        std::string t = "Locked: set by the environment variable ";
        return t + (env ? env : "?") + ".";
    }
    std::string t = first_sentence(d->tip);
    if (d->apply == OPT_RESTART)
        t = (opt_restart_pending_row(id) ? "Changed: takes effect at the next launch. "
                                         : "Takes effect at the next launch. ") + t;
    if (t.size() > kHelpLen) {
        size_t cut = t.rfind(' ', kHelpLen - 3);
        t.resize(cut == std::string::npos ? kHelpLen - 3 : cut);
        t += "...";
    }
    return t;
}

/* the footer for the focused row, when it changed (the focus, the page, the store, a load) */
void update_footer(ppu_context* ctx, uint32_t b)
{
    const uint32_t foot = vm_read32(b + B_FOOT), menu = vm_read32(b + B_MENU);
    if (!plausible(foot) || vm_read32(foot) != kTextBoxVt) return;
    const uint32_t focus = vm_read32(menu + M_FOCUS);
    const uint32_t sig = (focus * 2654435761u) ^ (vm_read32(b + B_PAGE) * 40503u) ^ (opt_generation() * 97u) ^
                         vm_read32(b + B_SERIAL) ^ vm_read32(foot + T_LABEL);
    if (sig == g_foot_sig) return;
    int row = -1;
    const int nvis = (int)vm_read32(b + B_NVIS);
    for (int k = 0; k < nvis; k++) if (vm_read32(row_ea(b, k) + R_SYM) == focus) row = k;
    const std::string help = help_line(b, row);
    const uint32_t id = str_id(ctx, help.empty() ? std::string(kNavLine) : help + "\n" + kNavLine);
    if (id != 0xFFFFFFFFu && vm_read32(foot + T_LABEL) != id) guest_call(ctx, IF2_F(MF_TB_SET_STRING), foot, id);
    g_foot_sig = (focus * 2654435761u) ^ (vm_read32(b + B_PAGE) * 40503u) ^ (opt_generation() * 97u) ^
                 vm_read32(b + B_SERIAL) ^ vm_read32(foot + T_LABEL);
}

void close_screen(ppu_context* ctx, uint32_t b, const char* why);

/* PC SETTINGS selected in the pause menu: push the Options menu and fill it with the rows of category `page`. */
void open_screen(ppu_context* ctx, uint32_t pause, int page)
{
    const uint32_t opts = (uint32_t)guest_call(ctx, IF2_F(MF_MENU_BY_SYMBOL), kOptMenuId);
    if (!plausible(opts) || vm_read32(opts) != kMenuVt) {
        fprintf(stderr, "[menu] PC Settings: no Options menu (0x%08X) -- not opened\n", opts);
        return;
    }
    const uint32_t wl = find_child(opts, kWListVt, kWListId);
    const uint32_t px = wl ? find_child(wl, kProxyVt, kRowProxyId) : 0;
    if (vm_read32(opts + M_ITEMS + 4) != 0 || !wl || !px || vm_read32(wl + W_COUNT) != 0) {
        fprintf(stderr, "[menu] PC Settings: the Options screen is in use or not as expected (items %u, widget list "
                        "0x%08X, row proxy 0x%08X) -- not opened\n", vm_read32(opts + M_ITEMS + 4), wl, px);
        return;
    }
    const uint32_t b = blk_get(ctx);
    if (!b) { fprintf(stderr, "[menu] PC Settings: no state block -- not opened\n"); return; }
    build_pages();
    if (g_pages.empty()) { fprintf(stderr, "[menu] PC Settings: no settings to show\n"); return; }
    vm_write32(b + B_OPEN, 1);
    vm_write32(b + B_MENU, opts);
    vm_write32(b + B_PAUSE, pause);
    vm_write32(b + B_WLIST, wl);
    vm_write32(b + B_NROWS, 0);
    g_fill_rows = 0;
    g_fill_first = 0;
    const uint32_t pushed = (uint32_t)guest_call(ctx, IF2_F(MF_PUSH_MENU), pause, kOptMenuId);
    if (pushed != opts) {
        vm_write32(b + B_OPEN, 0);
        fprintf(stderr, "[menu] PC Settings: PUSH_MENU gave 0x%08X, not the Options menu 0x%08X\n", pushed, opts);
        return;
    }
    /* the footer: ours while the screen is up */
    uint32_t foot = 0;
    for (uint32_t c = vm_read32(opts + O_CHILD); plausible(c) && vm_read32(c + O_PARENT) == opts; c = vm_read32(c + O_NEXT))
        if (vm_read32(c) == kTextBoxVt && vm_read32(c + T_LABEL) == kNavDpad) { foot = c; break; }
    vm_write32(b + B_FOOT, foot);
    vm_write32(b + B_FOOT_ID, kNavDpad);
    g_foot_sig = 0;
    if (page < 0 || page >= (int)g_pages.size()) page = 0;
    fprintf(stderr, "[menu] PC Settings opened: the Options menu 0x%08X pushed over the pause menu 0x%08X, %d categor%s\n",
            opts, pause, (int)g_pages.size(), g_pages.size() == 1 ? "y" : "ies");
    if (make_rows(ctx, b) < 2) {
        fprintf(stderr, "[menu] PC Settings: no rows could be made -- closed again\n");
        close_screen(ctx, b, "no rows");
        return;
    }
    show_page(ctx, b, page);
    guest_call(ctx, IF2_F(MF_FOCUS), opts, vm_read32(row_ea(b, 0) + R_SYM));
    update_footer(ctx, b);
    g_gen_seen = opt_generation();
}

/* the footer back to the game's button hints (when it still shows one of our strings) */
void restore_footer(ppu_context* ctx, uint32_t b)
{
    const uint32_t foot = vm_read32(b + B_FOOT);
    if (!plausible(foot) || vm_read32(foot) != kTextBoxVt) return;
    const uint32_t l = vm_read32(foot + T_LABEL);
    if (l >= kIdBase && l < kIdEnd) guest_call(ctx, IF2_F(MF_TB_SET_STRING), foot, vm_read32(b + B_FOOT_ID));
}

/* Back / Start on the screen: as the game's Back -- the items out, the menu popped, the rows cleared. */
void close_screen(ppu_context* ctx, uint32_t b, const char* why)
{
    restore_footer(ctx, b);
    clear_screen(ctx, b);
    vm_write32(b + B_OPEN, 0);
    blk_touch(b);
    fprintf(stderr, "[menu] PC Settings closed (%s): back to the pause menu 0x%08X\n", why, vm_read32(b + B_PAUSE));
    fflush(stderr);
}

/* Is the block's screen the one in guest memory? (its menu, and each row's symbol -> its widget) */
bool screen_valid(uint32_t b, uint32_t menu)
{
    if (vm_read32(b + B_OPEN) != 1 || vm_read32(b + B_MENU) != menu || vm_read32(menu) != kMenuVt) return false;
    const int n = (int)vm_read32(b + B_NROWS);
    uint32_t key[kMaxItems], wid[kMaxItems];
    const int ni = read_items(menu, key, wid, kMaxItems);
    if (n < 1 || n > kMaxRows || ni != n) return false;
    for (int r = 0; r < n; r++) {
        const uint32_t w = vm_read32(row_ea(b, r) + R_WIDGET), s = vm_read32(row_ea(b, r) + R_SYM);
        bool found = false;
        for (int i = 0; i < ni; i++) if (key[i] == s && vm_read32(vm_read32(menu + M_ITEMS) + 12u * (uint32_t)i + 4) == w) found = true;
        if (!found || !plausible(w) || vm_read32(w) != kListVt) return false;
    }
    return true;
}

/* Every row from the store (a change made elsewhere, or a load): its values when they differ, its index. */
void resync_rows(ppu_context* ctx, uint32_t b)
{
    const int page = (int)vm_read32(b + B_PAGE), n = (int)vm_read32(b + B_NVIS);
    for (int r = 0; r < n; r++) {
        const uint32_t e = row_ea(b, r), w = vm_read32(e + R_WIDGET);
        int cur = 0;
        const std::vector<std::string> vals = row_values(page, row_key(b, r), &cur);
        const uint32_t h = values_hash(vals);
        if (h != vm_read32(e + R_HASH)) {
            vm_write32(e + R_HASH, fill_row(ctx, w, vals, cur) ? h : 0);
        } else if ((int)vm_read32(w + L_INDEX) != cur) {
            guest_call(ctx, IF2_F(MF_SET_CURRENT_INDEX), w, sx(cur));
        }
        vm_write32(e + R_LAST, vm_read32(w + L_INDEX));
    }
    blk_touch(b);
}

/* before the menu's own input handler: Back and Start are ours */
bool screen_pre(ppu_context* ctx, uint32_t menu, uint32_t b)
{
    const uint32_t joy = input_joy(ctx, menu);
    if (!joy) return true;
    const bool back = joy_pressed(ctx, joy, kBtnBack), start = !back && joy_pressed(ctx, joy, kBtnStart);
    if (back || start) {
        joy_use(ctx, joy, back ? kBtnBack : kBtnStart);
        close_screen(ctx, b, back ? "Back" : "Start");
        return false;
    }
    /* L1 / R1: the category before / after, from any row (the focus stays on its row when the new one has it) */
    const bool l1 = joy_pressed(ctx, joy, kBtnL1), r1 = !l1 && joy_pressed(ctx, joy, kBtnR1);
    if ((l1 || r1) && !g_pages.empty()) {
        joy_use(ctx, joy, l1 ? kBtnL1 : kBtnR1);
        const int np = (int)g_pages.size();
        const int page = ((int)vm_read32(b + B_PAGE) + (l1 ? np - 1 : 1)) % np;
        const uint32_t focus = vm_read32(menu + M_FOCUS);
        show_page(ctx, b, page);
        const int nvis = (int)vm_read32(b + B_NVIS);
        int row = 0;
        for (int k = 0; k < nvis; k++) if (vm_read32(row_ea(b, k) + R_SYM) == focus) row = k;
        guest_call(ctx, IF2_F(MF_FOCUS), menu, vm_read32(row_ea(b, row) + R_SYM));
    }
    return true;
}

void purge_fill(ppu_context* ctx, uint32_t b);

/* after it: the rows the player stepped set their settings; the store's changes move the rows */
void screen_post(ppu_context* ctx, uint32_t menu, uint32_t b)
{
    (void)menu;
    purge_fill(ctx, b);                                      /* (normally done at the fill's end already) */
    if (g_fill_rows) {
        fprintf(stderr, "[menu] PC Settings: the game's own fill of the Options menu (%d row(s)) taken off the screen\n",
                g_fill_rows);
        g_fill_rows = 0;
    }
    if (g_pages.empty() || g_pages_rows != opt_count()) build_pages();
    const unsigned gen = opt_generation();
    if (gen != g_gen_seen || vm_read32(b + B_SERIAL) != g_serial_seen) {
        if (vm_read32(b + B_SERIAL) != g_serial_seen)
            fprintf(stderr, "[menu] PC Settings: the screen in guest memory is another one (a load) -- its rows re-read\n");
        g_gen_seen = gen;
        resync_rows(ctx, b);
    }
    hide_spare_rows(ctx, b);
    update_footer(ctx, b);
    const int page = (int)vm_read32(b + B_PAGE), n = (int)vm_read32(b + B_NVIS);
    for (int r = 0; r < n; r++) {
        const uint32_t e = row_ea(b, r), w = vm_read32(e + R_WIDGET);
        const int idx = (int)vm_read32(w + L_INDEX), last = (int)vm_read32(e + R_LAST);
        if (idx == last) continue;
        if (!vm_read32(e + R_HASH)) {                 /* its values are incomplete: an index there is no value */
            guest_call(ctx, IF2_F(MF_SET_CURRENT_INDEX), w, sx(last));
            continue;
        }
        const std::string key = row_key(b, r);
        if (key == "@page") {                         /* another category: the rows re-filled */
            if (idx < 0 || idx >= (int)g_pages.size()) continue;
            show_page(ctx, b, idx);
            return;
        }
        vm_write32(e + R_LAST, (uint32_t)idx);
        blk_touch(b);
        const PRow* pr = page_row(page, key);
        const opt_id id = row_id(key);
        g_model.msg[0] = 0;
        const int res = ovl_menu_row_choose(&g_model, id, idx, kMaxPos);
        char now[96] = "";
        if (id >= 0) opt_text(id, now, sizeof now);
        fprintf(stderr, "[menu] PC Settings: %s (%s) -> value %d: %s%s%s%s\n", pr ? pr->label.c_str() : key.c_str(),
                key.c_str(), idx, res == OPT_SET_CHANGED ? "set, now " : res == OPT_SET_SAME ? "unchanged, " : "refused, ",
                id == OVL_ROW_PRESET ? "preset" : now, g_model.msg[0] ? " -- " : "", g_model.msg);
        fflush(stderr);
        if (res == OPT_SET_REFUSED) g_gen_seen = ~gen;     /* the row goes back to the store's value next frame */
    }
}

/* the menus' per-frame input handler, wrapped */
void menu_input(ppu_context* ctx)
{
    /* this = the menu's listener interface: the thunk's own addi says where the menu starts */
    const int32_t adj = (int16_t)(vm_read32(kMenuInput) & 0xFFFF);
    const uint32_t menu = (uint32_t)ctx->gpr[3] + (uint32_t)adj;
    uint32_t focus0 = 0, scr = 0;
    bool ours = false;
    if (plausible(menu) && vm_read32(menu) == kMenuVt && obj_id(menu) == kOptMenuId) {
        /* the Options menu: our screen? (its state is in guest memory) */
        const uint32_t b = blk_find(ctx);
        if (b && vm_read32(b + B_OPEN) == 1 && vm_read32(b + B_MENU) == menu) {
            if (!g_on) {                              /* NATIVE_MENU=0 and a state with our screen open: closed */
                if (screen_valid(b, menu)) close_screen(ctx, b, "NATIVE_MENU=0");
                else { restore_footer(ctx, b); vm_write32(b + B_OPEN, 0); blk_touch(b); }
            } else if (screen_valid(b, menu)) scr = b;
            else {
                restore_footer(ctx, b);
                vm_write32(b + B_OPEN, 0);
                blk_touch(b);
                fprintf(stderr, "[menu] PC Settings: the Options menu no longer holds our rows -- the screen is left to the game\n");
                fflush(stderr);
            }
        }
    }
    if (!g_on) {
        IF2_F(MF_INPUT_THUNK)(ctx);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(ctx); }
        return;
    }
    if (!scr && plausible(menu)) {
        const uint32_t sig = act_sig(menu);
        /* (a pause menu we could not add to is tried again every 30 frames: the script rebuilds the same list
         * on every open, so a one-off failure -- its text boxes not labelled yet, no memory -- would stick) */
        const bool retry = g_menu_kind == 1 && !g_quit.slot && --g_retry <= 0;
        if (menu != g_menu || sig != g_menu_sig || retry) {
            if (g_diag) diag_menu(ctx, menu);
            g_menu = menu;
            g_menu_sig = sig;
            g_menu_kind = is_pause_menu(ctx, menu) ? 1 : 0;
            g_pc = Entry{}; g_quit = Entry{};
            if (g_menu_kind == 1) {
                add_entries(ctx, menu);
                g_retry = 30;
                g_menu_sig = act_sig(menu);       /* our own links are part of what was classified */
                if (g_diag && g_quit.slot) diag_menu(ctx, menu);
            }
        }
        ours = g_menu_kind == 1 && g_quit.slot;
        if (ours) {
            /* the slots are still free (no trigger of the script's on them): keep our labels up */
            if (!label_shown(g_pc)) show_label(ctx, g_pc);
            if (!label_shown(g_quit)) show_label(ctx, g_quit);
            focus0 = vm_read32(menu + M_FOCUS);
        }
    }
    if (scr && !screen_pre(ctx, menu, scr)) scr = 0;
    IF2_F(MF_INPUT_THUNK)(ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(ctx); }
    if (scr) { screen_post(ctx, menu, scr); return; }
    if (ours && g_diag) {
        const uint32_t f = vm_read32(menu + M_FOCUS);
        if (f != g_diag_focus) {
            fprintf(stderr, "[menu] diag: focus 0x%X -> 0x%X\n", focus0, f);
            g_diag_focus = f;
        }
    }
    if (ours && focus0 && (focus0 == g_pc.slot || focus0 == g_quit.slot)) {
        const bool down = select_down(ctx, menu, g_sel_mask);
        if (down && !g_sel_prev) {
            if (focus0 == g_quit.slot) confirm_quit(ctx);
            else {
                if (const uint32_t joy = input_joy(ctx, menu)) joy_use(ctx, joy, g_sel_mask);
                open_screen(ctx, menu, g_page_pref);
            }
        }
        g_sel_prev = down;
    } else {
        g_sel_prev = false;
    }
}

/* ---- the Options script's own fill, while our screen owns the Options menu. Pushing the Options menu makes the
 * game's script fill it (its reaction to the push, later in the same frame). Its rows are made in the widget list
 * as usual, but their items, Up / Down links and focus are not given to the menu; at the fill's end (its last link,
 * Down to its first row) they leave the widget list into its pool (REMOVE_WIDGET: out of the world), before the
 * frame is drawn. The script runs its usual code; the screen keeps our rows only. ---- */
bool our_row(uint32_t b, uint32_t w)
{
    const int n = (int)vm_read32(b + B_NROWS);
    for (int r = 0; r < n && r < kMaxRows; r++)
        if (vm_read32(row_ea(b, r) + R_WIDGET) == w) return true;
    return false;
}
bool our_sym(uint32_t b, uint32_t sym)
{
    const int n = (int)vm_read32(b + B_NROWS);
    for (int r = 0; r < n && r < kMaxRows; r++)
        if (vm_read32(row_ea(b, r) + R_SYM) == sym) return true;
    return false;
}
/* our open screen's block if this is its menu, else 0. Once our rows are made, the menu holds exactly them: a menu
 * left without a close (its items gone) is the game's again, and its own fill goes through. */
uint32_t our_menu(ppu_context* ctx, uint32_t obj)
{
    if (!plausible(obj) || vm_read32(obj) != kMenuVt || obj_id(obj) != kOptMenuId) return 0;
    const uint32_t b = blk_find(ctx);
    if (!b || vm_read32(b + B_OPEN) != 1 || vm_read32(b + B_MENU) != obj) return 0;
    const uint32_t n = vm_read32(b + B_NROWS);
    return n == 0 || vm_read32(obj + M_ITEMS + 4) == n ? b : 0;
}
/* every row of the widget list that is not ours: no handler, into the pool; focus on one of ours */
void purge_fill(ppu_context* ctx, uint32_t b)
{
    const uint32_t wl = vm_read32(b + B_WLIST), menu = vm_read32(b + B_MENU);
    if (!plausible(wl) || vm_read32(wl) != kWListVt) return;
    uint32_t foreign[kMaxRows + 1];
    int nf = 0;
    const uint32_t arr = vm_read32(wl + W_ROWS);
    const int n = (int)vm_read32(wl + W_COUNT);
    for (int i = 0; i < n && i <= kMaxRows && plausible(arr); i++) {
        const uint32_t w = vm_read32(arr + 4u * (uint32_t)i);
        if (plausible(w) && !our_row(b, w) && nf <= kMaxRows) foreign[nf++] = w;
    }
    for (int i = 0; i < nf; i++) {
        if (vm_read32(foreign[i] + L_HANDLER)) call_native(ctx, IF2_F(MF_SET_HANDLER), foreign[i], { 0 });
        guest_call(ctx, IF2_F(MF_REMOVE_WIDGET), wl, foreign[i]);
    }
    g_fill_rows += nf;
    hide_spare_rows(ctx, b);
    if (!our_sym(b, vm_read32(menu + M_FOCUS)) && vm_read32(b + B_NROWS))
        guest_call(ctx, IF2_F(MF_FOCUS), menu, vm_read32(row_ea(b, 0) + R_SYM));
}
void icpt_add_widget_item(ppu_context* ctx)        /* ADD_WIDGET_ITEM(menu, [row, symbol]) */
{
    const uint32_t self = (uint32_t)ctx->gpr[3], args = (uint32_t)ctx->gpr[4];
    if (plausible(args))
        if (const uint32_t b = our_menu(ctx, self))
            if (!our_row(b, vm_read32(args))) {
                if (!g_fill_first) g_fill_first = vm_read32(args + 4);
                return;
            }
    g_next_awi(ctx);
}
void icpt_add_nav_action(ppu_context* ctx)         /* ADD_NAV_ACTION(menu, [mask, from, to]) */
{
    const uint32_t self = (uint32_t)ctx->gpr[3], args = (uint32_t)ctx->gpr[4];
    if (const uint32_t b = our_menu(ctx, self)) {   /* ours are made directly: this is the game's fill */
        if (plausible(args) && vm_read32(args) == kBtnDown && g_fill_first && vm_read32(args + 8) == g_fill_first) {
            purge_fill(ctx, b);                     /* its last link: the fill is done */
            g_fill_first = 0;
        }
        return;
    }
    g_next_nav(ctx);
}
void icpt_focus(ppu_context* ctx)                  /* the FOCUS property's setter (menu, symbol), through its OPD */
{
    if (const uint32_t b = our_menu(ctx, (uint32_t)ctx->gpr[3]))
        if (!our_sym(b, (uint32_t)ctx->gpr[4])) return;
    IF2_F(MF_FOCUS)(ctx);
}

/* the message box's callback: r3 = the button */
void msgbox_done(ppu_context* ctx)
{
    const uint32_t r = (uint32_t)ctx->gpr[3];
    fprintf(stderr, "[menu] Quit Game message box: %s (result %u)\n", r == kYes ? "Yes -- closing the game" : "No", r);
    fflush(stderr);
    if (r != kYes) return;
    const DWORD pid = GetCurrentProcessId();
    for (HWND w = nullptr; (w = FindWindowExA(nullptr, w, "ps3recomp_d3d12", nullptr)) != nullptr;) {
        DWORD wp = 0;
        GetWindowThreadProcessId(w, &wp);
        if (wp == pid) {
            PostMessageW(w, WM_CLOSE, 0, 0);   /* main.cpp's close path: a save finishes, then the process ends */
            return;
        }
    }
    fprintf(stderr, "[menu] no game window to close\n");
}

/* ---- MENU_TRACE=1 (diag): every call of the menu natives the scripts make, with this / args / result ---- */
struct TraceNat { uint32_t addr; const char* name; void (*fn)(ppu_context*); };
#define IF2_TRACE_ROW(a, n) { 0x##a, n, func_##a },
const TraceNat kTrace[] = { IF2_TRACE_NATIVES(IF2_TRACE_ROW) };
constexpr int kNTrace = (int)(sizeof kTrace / sizeof kTrace[0]);

std::string trace_word(ppu_context* ctx, uint32_t w)
{
    char b[160];
    if (plausible(w) && w >= 0x00800000u) {
        snprintf(b, sizeof b, "%08X{vt %08X id %X}", w, vm_read32(w), obj_id(w));
        return b;
    }
    if (w && w < 0x00100000u) {
        const std::string nm = str_name(ctx, w);
        if (!nm.empty()) { snprintf(b, sizeof b, "%X(%s)", w, nm.c_str()); return b; }
    }
    snprintf(b, sizeof b, "%X", w);
    return b;
}

template <int K>
void trace_nat(ppu_context* ctx)
{
    const TraceNat& t = kTrace[K];
    const uint32_t self = (uint32_t)ctx->gpr[3], args = (uint32_t)ctx->gpr[4], mask = (uint32_t)ctx->gpr[5];
    std::string line = "[menu] T " + std::string(t.name) + " this=" + trace_word(ctx, self);
    char b[64];
    snprintf(b, sizeof b, " mask=%X a=[", mask);
    line += b;
    uint32_t w[6] = {};
    if (plausible(args))
        for (int i = 0; i < 6; i++) w[i] = vm_read32(args + 4u * (uint32_t)i);
    for (int i = 0; i < 6; i++) { if (i) line += ' '; line += trace_word(ctx, w[i]); }
    line += ']';
    t.fn(ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(ctx); }
    if (plausible(args) && vm_read32(args) != w[0]) line += " -> " + trace_word(ctx, vm_read32(args));
    fprintf(stderr, "%s\n", line.c_str());
    fflush(stderr);
}

template <int K>
void trace_install()
{
    if constexpr (K < kNTrace) {
        ppu_register_function(kTrace[K].addr, trace_nat<K>);
        if (kTrace[K].addr == IF2_R(MF_NAT_ADD_WIDGET_ITEM)) g_next_awi = trace_nat<K>;
        if (kTrace[K].addr == IF2_R(MF_NAT_ADD_NAV_ACTION)) g_next_nav = trace_nat<K>;
        trace_install<K + 1>();
    }
}

const TraceNat kTraceAcc[] = { IF2_TRACE_ACCESSORS(IF2_TRACE_ROW) };
constexpr int kNTraceAcc = (int)(sizeof kTraceAcc / sizeof kTraceAcc[0]);

template <int K>
void trace_acc(ppu_context* ctx)
{
    const TraceNat& t = kTraceAcc[K];
    const uint32_t self = (uint32_t)ctx->gpr[3], a1 = (uint32_t)ctx->gpr[4];
    std::string line = "[menu] T " + std::string(t.name) + " this=" + trace_word(ctx, self) + " r4=" + trace_word(ctx, a1);
    t.fn(ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(ctx); }
    line += " -> " + trace_word(ctx, (uint32_t)ctx->gpr[3]);
    fprintf(stderr, "%s\n", line.c_str());
    fflush(stderr);
}

template <int K>
void trace_acc_install()
{
    if constexpr (K < kNTraceAcc) {
        ppu_register_function(kTraceAcc[K].addr, trace_acc<K>);
        trace_acc_install<K + 1>();
    }
}

/* PCS2: the screen's guest block as a layout. GMEM saves the block as raw pages, so a build that moved a field
 * (or grew the block: the old, smaller one would be written past) would misread another build's block; this
 * section's layout hash -- every constant of the block at its value -- refuses such a state before anything
 * stops. OPTIONAL: a state from before it loads unchecked. */
#define PCS_F(c) { #c, nullptr, (uint32_t)(c), 0u }
const ss_field kPcsFields[] = {
    PCS_F(kBlkMagic), PCS_F(kStateId), PCS_F(kIdBase), PCS_F(kIdEnd), PCS_F(kMaxRows), PCS_F(B_MAGIC),
    PCS_F(B_SERIAL), PCS_F(B_OPEN), PCS_F(B_MENU), PCS_F(B_PAUSE), PCS_F(B_WLIST), PCS_F(B_PAGE), PCS_F(B_NROWS),
    PCS_F(B_FOOT), PCS_F(B_FOOT_ID), PCS_F(B_NVIS), PCS_F(B_ROWS), PCS_F(B_ROW), PCS_F(B_SIZE), PCS_F(R_WIDGET),
    PCS_F(R_SYM), PCS_F(R_LAST), PCS_F(R_HASH), PCS_F(R_KEY), PCS_F(R_KEYLEN),
};
#undef PCS_F
const ss_list kPcsList = SS_LIST("pc settings block", kPcsFields);
const ss_list* const kPcsLists[] = { &kPcsList, nullptr };
int pcs_capture(ss_writer* w)
{
    ss_write_u64(w, "magic", kBlkMagic);
    return ss_writer_error(w);
}
int pcs_restore(ss_reader* r) { (void)r; return SS_OK; }   /* (its layout hash is the point) */
const ss_section_ops kPcsOps = {
    SS_FOURCC('P', 'C', 'S', '2'), 1, SS_SEC_OPTIONAL | SS_SEC_NO_VERIFY, SS_ORDER_TITLE + 10,
    nullptr, pcs_capture, nullptr, pcs_restore, nullptr,
    "PC Settings screen layout", kPcsLists, nullptr, nullptr, nullptr,
};
#endif /* IF2_MENU_KNOWN */

} /* namespace */

#if IF2_MENU_KNOWN
SS_SECTION_REGISTER(kPcsOps)
#endif

extern "C" int if2_menu_install(void)
{
    const char* e = getenv("NATIVE_MENU");
    g_on = !(e && *e == '0');
    const char* d = getenv("MENU_DIAG");
    g_diag = d && *d && *d != '0';
    if (const char* k = getenv("MENU_TEST_SKIP")) g_skip = atoi(k) > 0 ? atoi(k) : 0;
#if IF2_MENU_KNOWN
    /* with the entries off too: the box's callback (a save state made with the box open still answers) and the
     * menus' input wrapper, which then only closes a PC Settings screen a loaded save state brings */
    ppu_register_function(kCbKey, msgbox_done);
    if (!g_on) ppu_register_function(kMenuInput, menu_input);
#endif
    if (!g_on) { fprintf(stderr, "[menu] NATIVE_MENU=0: the game's menus are left as they are\n"); return 0; }
#if IF2_MENU_KNOWN
    ppu_register_function(kMenuInput, menu_input);
    if (const char* tr = getenv("MENU_TRACE"))
        if (*tr && *tr != '0') {
            trace_install<0>();
            if (*tr == '2') trace_acc_install<0>();
            fprintf(stderr, "[menu] MENU_TRACE: %d menu natives traced%s\n", kNTrace, *tr == '2' ? ", and the property accessors" : "");
        }
    if (!g_next_awi) g_next_awi = IF2_F(MF_NAT_ADD_WIDGET_ITEM);
    if (!g_next_nav) g_next_nav = IF2_F(MF_NAT_ADD_NAV_ACTION);
    ppu_register_function(IF2_R(MF_NAT_ADD_WIDGET_ITEM), icpt_add_widget_item);   /* the game's fill on our screen (above) */
    ppu_register_function(IF2_R(MF_NAT_ADD_NAV_ACTION), icpt_add_nav_action);
    ppu_register_function(IF2_R(MF_FOCUS), icpt_focus);
    fprintf(stderr, "[menu] native menu entries on: PC Settings and Quit Game in the pause menu (NATIVE_MENU=0 turns "
                    "them off)%s\n", g_diag ? "; MENU_DIAG logs every menu" : "");
    return 1;
#else
#if IF2_EBOOT_KNOWN
    fprintf(stderr, "[menu] native menu entries: not mapped for %s -- off\n", IF2_EBOOT_NAME);
#else
    if2_feature_off("native menu entries (PC Settings / Quit Game in the pause menu)", "the menu functions");
#endif
    return 0;
#endif
}

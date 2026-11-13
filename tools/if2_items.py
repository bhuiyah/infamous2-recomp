"""if2_items.py -- every EBOOT-specific value the inFamous 2 title code uses, with the US builds' values.

tools/if2_resolve.py finds each of these in the EBOOT being lifted (by code patterns: config/if2_sigs.json) and
writes generated/if2_resolved.h, which src/ uses instead of hard-coded addresses. The values below are the two
US builds' (BCUS98125 disc EBOOT "us100", APP_VER 01.00; title update 1.04 "us104"): they are the cross-check
-- on those EBOOTs (known by SHA-1) the resolver must reproduce every one exactly, or the build stops -- and
the source the signatures are derived from (if2_resolve.py derive). None = the build has no such thing.

Kinds:
  func    a function entry point (a lifted func_XXXXXXXX)
  code    an instruction address (a call site, a return address, a patched or checked instruction)
  data    a data address, found through the instruction(s) that materialise it (a TOC load, lis + addi, ...)
  toc     the TOC (r2) of the EBOOT (from its entry point's descriptor)
  import  a firmware import stub, by library + NID (analysis/EBOOT.imports.json)
  callee  the target of the `bl` at another (code) item
  rel     another item + a constant (fields of one object, rows of one table)
  deref   the 32-bit word stored at another item + a constant (a pointer in a table)
  tocdisp the signed 16-bit displacement of the r2-relative load at another (code) item
  strtab  a table of string pointers, by the strings its first entries point to
  pm      the Wws_Job policy module (the known bytes, else a well-formed module where its registering code says)

Each item names the features it gates (FEATURES): a feature whose items do not all resolve is off in that
build, and the game runs without it (its own behaviour).
"""

# feature -> what it is (shown in the resolver report and the game's log)
FEATURES = {
    "aspect":     "ultrawide / other aspect ratios (display W, H, safe area, the pool slots of W)",
    "lod":        "LOD thresholds kept at 16:9 detail on a wide screen, and the Detail distance setting",
    "lod_quality": "Detail distance on 1.04's dynamic-quality table (levels 12..20)",
    "fmv_fit":    "full-screen movies fitted (pillarboxed) on a wide screen (post-lift hook)",
    "dtclamp":    "IF2_DTCLAMP: the game's max frame dt",
    "clocklog":   "IF2_CLOCKLOG: the game-clock monitor",
    "taskjump":   "IF2_TASKJUMP / checkpoint jumps (task graph)",
    "signals":    "game signals for the checkpoint boot driver and the play-session issues log (post-lift hook)",
    "wev_main_kicker":  "WAIT_EVENTS: the main thread's SPURS kicker wait",
    "wev_main_wws":     "WAIT_EVENTS: the main thread's WWS job-queue wait (1)",
    "wev_main_wws2":    "WAIT_EVENTS: the main thread's WWS job-queue wait (2)",
    "wev_light_kicker": "WAIT_EVENTS: the lighting kicker's wait",
    "wev_edge_kicker":  "WAIT_EVENTS: the Edge kicker's wait",
    "menu":       "PC Settings and Quit Game in the game's own pause menu",
    "menu_trace": "MENU_TRACE diagnostics (a missing native is just not traced)",
    "language_edition": "Language edition: Europe on the US edition (the language-list table)",
    "physfb":     "IF2_PHYSFB physics diagnostics (post-lift hooks)",
    "ugc_offline": "user-made missions: the online step after one is skipped offline (post-lift hook, #21)",
    "wws_pm":     "the game's Wws_Job SPURS policy module, lifted (else interpreted: slower)",
}


def F(name, us100, us104, gates, desc, **kw):
    return dict(name=name, kind="func", us100=us100, us104=us104, gates=gates, desc=desc, **kw)


def C(name, us100, us104, gates, desc, **kw):
    return dict(name=name, kind="code", us100=us100, us104=us104, gates=gates, desc=desc, **kw)


def D(name, us100, us104, gates, desc, **kw):
    return dict(name=name, kind="data", us100=us100, us104=us104, gates=gates, desc=desc, **kw)


def R(name, base, off, gates, desc, kind="rel", **kw):
    return dict(name=name, kind=kind, base=base, off=off, gates=gates, desc=desc, **kw)


ITEMS = [
    # ---- the EBOOT's TOC -------------------------------------------------------------------------------------
    dict(name="toc", kind="toc", us100=0x00973528, us104=0x009DB730, gates=["taskjump", "menu"],
         desc="the TOC (r2) every lifted function loads"),

    # ---- display (src/infamous2_hooks.cpp) -------------------------------------------------------------------
    D("disp_h", 0x0096EF84, 0x009D7214, ["aspect"], "output height (int)"),
    D("disp_w", 0x0096EF88, 0x009D7218, ["aspect"], "aspect width (int)"),
    D("safe_x", 0x00A010B0, 0x00A6E8D0, ["aspect"], "UI safe-area X margin"),
    D("render_w", 0x00A009DC, 0x00A6E1FC, ["aspect"], "render width (int, 1280)"),
    D("slot_noise", 0x0093C67C, 0x009A2494, ["aspect"], "TOC slot -> W, read by the noise pass only"),
    D("slot_texel", 0x00944760, 0x009AA990, ["aspect"], "TOC slot -> W, read by the texel-offset pass only"),
    D("slot_fmv", 0x00944600, 0x009AA82C, ["aspect"], "TOC slot -> W, read by the FMV quad only"),
    D("slot_cam", 0x00948D90, 0x009AF3EC, ["aspect"], "TOC slot -> W, read by the camera constructor only"),
    D("lod_0", 0x0093DE54, 0x009A3D00, ["lod"], "LOD: (d*tanX)^2 < 90000", expect=0x47AFC800),
    D("lod_1", 0x00945BE4, 0x009ABE28, ["lod"], "LOD: d*0.005*tanX vs 40", expect=0x42200000),
    D("lod_2", 0x00949244, 0x009AF8A4, ["lod"], "LOD: r/(d*tanX) vs 0.2", expect=0x3E4CCCCD),
    D("lod_3", 0x009442CC, 0x009AA4F4, ["lod"], "LOD: visprog mode 3: 0.1", expect=0x3DCCCCCD),
    D("quality_table", None, 0x009D7224, ["lod_quality"],
      "1.04's dynamic-quality table (21 x 20 bytes; column +0xC is c of the visprog threshold)"),
    # (rows 12..20, column +0xC; the stock values are checked at run time before each write)
    *[R("quality_c%d" % lv, "quality_table", 0xC + 20 * lv, ["lod_quality"], "quality level %d: c" % lv)
      for lv in range(12, 21)],

    # ---- the clock (src/if2_clock.cpp, IF2_DTCLAMP in src/infamous2_hooks.cpp) --------------------------------
    D("clock_obj", 0x009BFC70, 0x00A2C500, ["clocklog"], "the game clock object"),
    D("loop_count", 0x00A08F90, 0x00A76F50, ["clocklog"], "main-loop iterations"),
    D("clamp15", 0x0093ACE0, 0x009A0AD4, ["dtclamp", "clocklog"], "frame dt clamp 1/15", expect=0x3D888889),
    # (1/30 is the word after 1/15's neighbour: no instruction addresses it on its own; its stock value is checked)
    R("clamp30", "clamp15", 8, ["dtclamp", "clocklog"], "frame dt clamp 1/30", expect=0x3D088889,
      us100=0x0093ACE8, us104=0x009A0ADC),

    # ---- the task graph (src/if2_taskgraph.cpp) ---------------------------------------------------------------
    F("fn_set_node_state", 0x0037A2E8, 0x00394CC8, ["taskjump"], "SetNodeState(graph, node, state)"),
    F("fn_warp_to_checkpoint", 0x00378EA0, 0x003933A8, ["taskjump"], "TaskGraph::WarpToCheckpoint"),
    D("tg", 0x00A94E10, 0x00B03210, ["taskjump"], "the task graph object"),
    D("tg_index", 0x00A94E00, 0x00B03200, ["taskjump"], "-> the graph's u16 index table"),
    D("warp_mgr", 0x00A26C80, 0x00A94E80, ["taskjump"], "the world warp manager"),
    D("warp_lock", 0x00A32EB0, 0x00AA1290, ["taskjump"], "u8: warps refused while set"),
    D("restart_flag", 0x00A08F1D, 0x00A76EDD, ["taskjump"], "u8: restart the missions at the next world start"),
    C("main_loop_ret", 0x001DB824, 0x001EF2E8, ["taskjump"], "return address of the main loop's cellSysutilCheckCallback"),

    # ---- signals (src/if2_checkpoint.cpp; the dispatcher hook: tools/ppu_postlift.py) -------------------------
    # (a data table: found by the names its first entries point to)
    dict(name="sig_names", kind="strtab", strings=["AmbientSpeech", "AmmoFired", "AttackerChangeTarget"],
         us100=0x00983910, us104=0x009EBFAC, gates=["signals"], desc="reflection table: name pointer per signal id"),
    F("pl_signal_fn", 0x002F26B8, 0x0030A038, ["signals"], "the game's signal dispatcher (post-lift hook)"),

    # ---- full-screen movie fit (tools/ppu_postlift.py) --------------------------------------------------------
    F("pl_fmv_fn", 0x002E7EDC, 0x002FE8F8, ["fmv_fit"], "the FMV quad's function"),
    C("pl_fmv_call", 0x002E8030, 0x002FEA4C, ["fmv_fit"], "its bl the hook goes before"),
    dict(name="pl_fmv_callee", kind="callee", base="pl_fmv_call", us100=0x002D043C, us104=0x002E785C,
         gates=["fmv_fit"], desc="that bl's target"),

    # ---- physics diagnostics (tools/ppu_postlift.py, IF2_PHYSFB) ----------------------------------------------
    F("pl_phys_fn", 0x004796E0, 0x004B90AC, ["physfb"], "the physics step"),
    C("pl_phys_call", 0x00479D5C, 0x004B9728, ["physfb"], "its bl to the PPU narrowphase fallback"),
    dict(name="pl_phys_callee", kind="callee", base="pl_phys_call", us100=0x0047D438, us104=0x004BCE44,
         gates=["physfb"], desc="the PPU narrowphase fallback"),
    F("pl_mesh_fn", 0x004761F4, 0x004B5BA0, ["physfb"], "the mesh contact generator's body"),
    C("pl_mesh_lrsave", 0x00476200, 0x004B5BAC, ["physfb"], "its LR save (std r0, frame(r1))"),

    # ---- user-made missions offline (tools/ppu_postlift.py, GitHub #21) ----------------------------------------
    F("pl_ugc_fn", 0x003D2A04, 0x00400F6C, ["ugc_offline"], "the UGC manager's per-frame update"),
    C("pl_ugc_call", 0x003D2BE4, 0x00401158, ["ugc_offline"], "its idle state's bl that starts the online step"),
    dict(name="pl_ugc_callee", kind="callee", base="pl_ugc_call", us100=0x003D270C, us104=0x00400BB4,
         gates=["ugc_offline"], desc="the UGC manager's state change"),

    # ---- language edition (src/infamous2_hooks.cpp) -----------------------------------------------------------
    D("lang_lists", 0x008D09D0, 0x00930A00, ["language_edition"], "{count, list} per edition (the US one first)"),
    R("lang_list_us", "lang_lists", 4, ["language_edition"], "the US edition's list", kind="deref",
      us100=0x006ECE14, us104=0x00744E44),
    R("lang_list_eu", "lang_lists", 12, ["language_edition"], "the European edition's list", kind="deref",
      us100=0x006ECE24, us104=0x00744E54),

    # ---- WAIT_EVENTS (src/if2_waitev.cpp): each wait site and the return addresses it is reached from ---------
    C("wev_mk_sc", 0x003374F4, 0x00350D18, ["wev_main_kicker"], "main kicker: the instruction after its usleep"),
    C("wev_mk_c1", 0x001DB7DC, 0x001EF2A0, ["wev_main_kicker"], "main kicker: caller 1"),
    C("wev_mk_c2", 0x00337554, 0x00350D78, ["wev_main_kicker"], "main kicker: caller 2"),
    C("wev_mk_c3", 0x00337928, 0x0035114C, ["wev_main_kicker"], "main kicker: caller 3"),
    C("wev_mw_sc", 0x00370634, 0x0038AA70, ["wev_main_wws"], "main WWS wait: after its first usleep"),
    C("wev_mw_sc2", 0x0037065C, 0x0038AA98, ["wev_main_wws"], "main WWS wait: after its second usleep"),
    C("wev_mw_c1", 0x00370600, 0x0038AA3C, ["wev_main_wws"], "main WWS wait: caller 1"),
    C("wev_mw_c2", 0x00370640, 0x0038AA7C, ["wev_main_wws"], "main WWS wait: caller 2"),
    C("wev_mw_c3", 0x00370668, 0x0038AAA4, ["wev_main_wws"], "main WWS wait: caller 3"),
    C("wev_m2_sc", 0x003710A8, 0x0038B4E4, ["wev_main_wws2"], "main WWS wait 2: after its first usleep"),
    C("wev_m2_sc2", 0x003710D4, 0x0038B510, ["wev_main_wws2"], "main WWS wait 2: after its second usleep"),
    C("wev_m2_c1", 0x0037108C, 0x0038B4C8, ["wev_main_wws2"], "main WWS wait 2: caller 1"),
    C("wev_m2_c2", 0x003710B8, 0x0038B4F4, ["wev_main_wws2"], "main WWS wait 2: caller 2"),
    C("wev_m2_c3", 0x003710E4, 0x0038B520, ["wev_main_wws2"], "main WWS wait 2: caller 3"),
    C("wev_lk_sc", 0x0033745C, 0x00350C80, ["wev_light_kicker"], "light kicker: after its usleep"),
    C("wev_lk_c1", 0x00337430, 0x00350C54, ["wev_light_kicker"], "light kicker: caller 1"),
    C("wev_lk_c2", 0x00337464, 0x00350C88, ["wev_light_kicker"], "light kicker: caller 2"),
    D("wev_lk_periods", 0x006F2360, 0x0074A8B0, ["wev_light_kicker"], "light kicker: poll periods per state",
      expect=0x00000384),
    C("wev_ek_sc", 0x00338D08, 0x0035252C, ["wev_edge_kicker"], "Edge kicker: after its usleep"),
    C("wev_ek_c1", 0x00338CE8, 0x0035250C, ["wev_edge_kicker"], "Edge kicker: caller 1"),
    C("wev_ek_c2", 0x00338D10, 0x00352534, ["wev_edge_kicker"], "Edge kicker: caller 2"),
    C("wev_ek_glob", 0x00338AEC, 0x00352310, ["wev_edge_kicker"], "Edge kicker's step: globals = *(r2 + d)"),
    dict(name="wev_ek_glob_disp", kind="tocdisp", base="wev_ek_glob", us100=-0x7AD0, us104=-0x7AC8,
         gates=["wev_edge_kicker"], desc="that load's r2 displacement"),
    C("wev_ek_row0", 0x003707D4, 0x0038AC10, ["wev_edge_kicker"], "the row-0 id read (q+0xB8)"),
    C("wev_ek_row1", 0x00371138, 0x0038B574, ["wev_edge_kicker"], "the row-1 id read (q+0xD4)"),

    # ---- the game's SPURS policy module (tools/extract_policy_modules.py) -------------------------------------
    dict(name="wws_pm", kind="pm", us100=0x00887080, us104=0x008E5B00, gates=["wws_pm"],
         desc="the Wws_Job policy module (SPU code, 0x2E80 bytes)"),
]

# ---- the native menu entries (src/if2_menu.cpp; update 1.04 only) ----------------------------------------------
_MENU_FUNCS = [
    ("mf_input_thunk", 0x0067AE18, "the menus' per-frame input handler (the adjustor thunk the input system calls)"),
    ("mf_add_nav_action", 0x001CC36C, "ADD_NAV_ACTION(menu, mask, from, to)"),
    ("mf_dict_lookup", 0x0039A430, "string dictionary lookup (dict, id, 0)"),
    ("mf_tb_set_string", 0x0039A788, "text box: set string id (tb, id)"),
    ("mf_new", 0x004CF4A0, "operator new (size)"),
    ("mf_obj_add", 0x001BE00C, "scene object ADD"),
    ("mf_menu_joystick", 0x001CC78C, "the menu's joystick (menu)"),
    ("mf_ui_input", 0x0010F31C, "the UI that has the input"),
    ("mf_msgbox", 0x000C7908, "message box (title, text, buttons, ..., callback OPD, 0)"),
    ("mf_menu_by_symbol", 0x001CB868, "the menu with this symbol (id)"),
    ("mf_push_menu", 0x001CE3E0, "PUSH_MENU's C++ (menu, id)"),
    ("mf_pop_menu", 0x001CE83C, "POP_MENU's C++ (menu)"),
    ("mf_clear_items", 0x001CDCF0, "CLEAR_ITEMS(menu)"),
    ("mf_add_widget_item", 0x001CDBC8, "ADD_WIDGET_ITEM(menu, widget, symbol)"),
    ("mf_focus", 0x001CCF68, "focus (menu, symbol): the FOCUS setter"),
    ("mf_add_widget_by_proxy", 0x00495658, "ADD_WIDGET_BY_PROXY's C++ (widget list, proxy)"),
    ("mf_wlist_clear", 0x0049384C, "CLEAR(widget list)"),
    ("mf_add_item", 0x00491D7C, "ADD_ITEM's C++ (list widget, key, label id, 0, colour)"),
    ("mf_set_current_index", 0x00491A34, "CURRENT_INDEX's setter (list widget, index)"),
    ("mf_multimap_insert", 0x006B6F5C, "hash multimap insert (table, &key, &value)"),
    ("mf_dict_lock_make", 0x004E3DA4, "the dictionary's lock: made on first use"),
    ("mf_remove_widget", 0x0049374C, "REMOVE_WIDGET's C++ (widget list, row)"),
    ("mf_set_handler", 0x005302F8, "SET_HANDLER (widget, [handler]) -- the native"),
    ("mf_obj_remove", 0x001BDF74, "scene object REMOVE"),
    ("mf_nat_add_widget_item", 0x00520B94, "the ADD_WIDGET_ITEM native (intercepted on our screen)"),
    ("mf_nat_add_nav_action", 0x005273CC, "the ADD_NAV_ACTION native (intercepted on our screen)"),
]
for _n, _a, _d in _MENU_FUNCS:
    # the thunk is two instructions: found as a code address (it is a lifted function all the same)
    ITEMS.append(F(_n, None, _a, ["menu"], _d))
ITEMS += [
    dict(name="mf_lwmutex_lock", kind="import", lib="sysPrxForUser", nid=0x1573DC3F, us100=None, us104=0x007428F8,
         gates=["menu"], desc="sys_lwmutex_lock (import stub)"),
    dict(name="mf_lwmutex_unlock", kind="import", lib="sysPrxForUser", nid=0x1BC200F4, us100=None, us104=0x00742918,
         gates=["menu"], desc="sys_lwmutex_unlock (import stub)"),
    D("md_str_mgr", None, 0x009ACB10, ["menu"], "-> the string manager"),
    D("md_ui_mgr", None, 0x009A5DD0, ["menu"], "-> the UI manager"),
    D("md_repeat_t", None, 0x009A5E14, ["menu"], "f32: the menu handler's button-query argument (0.15)",
      expect=0x3E19999A),
    D("md_vt_menu", None, 0x00940018, ["menu"], "vtable: menu"),
    D("md_vt_list", None, 0x00954890, ["menu"], "vtable: list widget"),
    D("md_vt_wlist", None, 0x00954120, ["menu"], "vtable: widget list"),
    D("md_vt_proxy", None, 0x00943760, ["menu"], "vtable: widget proxy"),
    D("md_vt_textbox", None, 0x009489E0, ["menu"], "vtable: text box"),
]

# MENU_TRACE natives (diagnostics; a native not found is left out of the trace): (address, label, accessor?)
MENU_TRACE = [
    (0x00531520, "PUSH_MENU", 0), (0x00531FB4, "POP_MENU", 0), (0x00531F24, "SWITCH_TO_MENU", 0),
    (0x00520B94, "ADD_WIDGET_ITEM", 0), (0x00527408, "CLEAR_ITEMS(menu)", 0), (0x00531D80, "ADD_WIDGET_BY_PROXY", 0),
    (0x00531490, "ADD_WIDGET_BY_CLASS", 0), (0x00528758, "REMOVE_WIDGET", 0), (0x00528734, "CLEAR(widget list)", 0),
    (0x005302F8, "SET_HANDLER", 0), (0x0052B1C8, "SET_LABEL", 0), (0x0052089C, "SET_LABEL(widget list)", 0),
    (0x0052B148, "SET_ACTION_SOUND", 0), (0x0052E9FC, "SET_INCREMENTS", 0), (0x00520AC0, "SET_VALUE_LABEL", 0),
    (0x00520A60, "SET_TEXT_COLOR(text)", 0), (0x005209D4, "SET_TEXT_COLOR(list)", 0), (0x00520A34, "ADD_SLOT", 0),
    (0x0052B288, "SET_SELECT_LIMITS", 0), (0x0052B270, "SET_NAV_BUTTONS", 0), (0x0052B25C, "SET_SCROLL_INDICATORS", 0),
    (0x0052B214, "CLEAR_ITEMS(list)", 0), (0x0052CE54, "SET_SLOT_TEXT_BOX", 0), (0x0052D0D8, "ADD_ITEM", 0),
    (0x00520974, "HANDLE_ACTION", 0), (0x00520BC4, "SET_VALUES", 0), (0x005273CC, "ADD_NAV_ACTION", 0),
    (0x00527340, "ADD_TRIGGER_ACTION", 0), (0x005272F4, "REMOVE_ACTIONS", 0), (0x00520C70, "SET_TEXT_BOX", 0),
    (0x00411D80, "ADD", 0), (0x00411DA4, "REMOVE", 0), (0x0052B00C, "UPDATE_TEXT_BOX", 0),
    (0x00491424, "CURRENT_ITEM=", 1), (0x006F00D4, "CURRENT_ITEM?", 1), (0x00491A34, "CURRENT_INDEX=", 1),
    (0x0070119C, "CURRENT_VALUE=", 1), (0x006F00C4, "CURRENT_VALUE?", 1), (0x006F0A88, "FOCUS?", 1),
    (0x0039A788, "text=", 1), (0x001CC124, "STATE=", 1), (0x006F0A7C, "STATE?", 1),
]
for _i, (_a, _l, _acc) in enumerate(MENU_TRACE):
    ITEMS.append(F("mt_%02d" % _i, None, _a, ["menu_trace"], "MENU_TRACE: " + _l, trace=_l, accessor=_acc))

BY_NAME = {it["name"]: it for it in ITEMS}
assert len(BY_NAME) == len(ITEMS), "duplicate item names"
for _it in ITEMS:
    for _g in _it["gates"]:
        assert _g in FEATURES, (_it["name"], _g)
    _it.setdefault("us100", None)
    _it.setdefault("us104", None)

# The EBOOTs whose values are above, by SHA-1 of the decrypted ELF.
KNOWN = {
    "f1c20717d6532dfb27e292a5db6cdf5a3218760b": "us100",
    "7814ff1a87d7d3d10bd0e9627dc05ac353ed4162": "us104",
}

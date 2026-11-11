/* if2_resolved_test.h -- for the host tests only (tests/test_if2_*.cpp include title sources without a lift):
 * tools/if2_resolve.py's header for the US disc EBOOT (1.00), whose values the title sources were written
 * against. A build always uses generated/if2_resolved.h (the EBOOT being lifted). */
#pragma once
#define IF2_RESOLVED 1
#define IF2_EBOOT_SHA1 "f1c20717d6532dfb27e292a5db6cdf5a3218760b"
#define IF2_EBOOT_KNOWN 1   /* 1: a US build whose values are cross-checked */
#define IF2_EBOOT_V104 0   /* the layout family: 1.04's (1) or the disc EBOOT's (0) */
#define IF2_TITLE_ID "BCUS98125"
#define IF2_EDITION "North America"
#define IF2_EDITION_US 1
#define IF2_EBOOT_FROM_UPDATE 0   /* lifted from the title update (game/hdd0), not the disc */
#define IF2_EDITION_BETA 0
#define IF2_EBOOT_NAME "BCUS98125 disc (v02.00)"

#define IF2_R_TOC                        0x00973528u   /* the TOC (r2) every lifted function loads */
#define IF2_OK_TOC                       1
#define IF2_R_DISP_H                     0x0096EF84u   /* output height (int) */
#define IF2_OK_DISP_H                    1
#define IF2_R_DISP_W                     0x0096EF88u   /* aspect width (int) */
#define IF2_OK_DISP_W                    1
#define IF2_R_SAFE_X                     0x00A010B0u   /* UI safe-area X margin */
#define IF2_OK_SAFE_X                    1
#define IF2_R_RENDER_W                   0x00A009DCu   /* render width (int, 1280) */
#define IF2_OK_RENDER_W                  1
#define IF2_R_SLOT_NOISE                 0x0093C67Cu   /* TOC slot -> W, read by the noise pass only */
#define IF2_OK_SLOT_NOISE                1
#define IF2_R_SLOT_TEXEL                 0x00944760u   /* TOC slot -> W, read by the texel-offset pass only */
#define IF2_OK_SLOT_TEXEL                1
#define IF2_R_SLOT_FMV                   0x00944600u   /* TOC slot -> W, read by the FMV quad only */
#define IF2_OK_SLOT_FMV                  1
#define IF2_R_SLOT_CAM                   0x00948D90u   /* TOC slot -> W, read by the camera constructor only */
#define IF2_OK_SLOT_CAM                  1
#define IF2_R_LOD_0                      0x0093DE54u   /* LOD: (d*tanX)^2 < 90000 */
#define IF2_OK_LOD_0                     1
#define IF2_R_LOD_1                      0x00945BE4u   /* LOD: d*0.005*tanX vs 40 */
#define IF2_OK_LOD_1                     1
#define IF2_R_LOD_2                      0x00949244u   /* LOD: r/(d*tanX) vs 0.2 */
#define IF2_OK_LOD_2                     1
#define IF2_R_LOD_3                      0x009442CCu   /* LOD: visprog mode 3: 0.1 */
#define IF2_OK_LOD_3                     1
#define IF2_R_QUALITY_TABLE              0x00000000u   /* 1.04's dynamic-quality table (21 x 20 bytes; column +0xC is c of the v */
#define IF2_OK_QUALITY_TABLE             0
#define IF2_R_QUALITY_C12                0x00000000u   /* quality level 12: c */
#define IF2_OK_QUALITY_C12               0
#define IF2_R_QUALITY_C13                0x00000000u   /* quality level 13: c */
#define IF2_OK_QUALITY_C13               0
#define IF2_R_QUALITY_C14                0x00000000u   /* quality level 14: c */
#define IF2_OK_QUALITY_C14               0
#define IF2_R_QUALITY_C15                0x00000000u   /* quality level 15: c */
#define IF2_OK_QUALITY_C15               0
#define IF2_R_QUALITY_C16                0x00000000u   /* quality level 16: c */
#define IF2_OK_QUALITY_C16               0
#define IF2_R_QUALITY_C17                0x00000000u   /* quality level 17: c */
#define IF2_OK_QUALITY_C17               0
#define IF2_R_QUALITY_C18                0x00000000u   /* quality level 18: c */
#define IF2_OK_QUALITY_C18               0
#define IF2_R_QUALITY_C19                0x00000000u   /* quality level 19: c */
#define IF2_OK_QUALITY_C19               0
#define IF2_R_QUALITY_C20                0x00000000u   /* quality level 20: c */
#define IF2_OK_QUALITY_C20               0
#define IF2_R_CLOCK_OBJ                  0x009BFC70u   /* the game clock object */
#define IF2_OK_CLOCK_OBJ                 1
#define IF2_R_LOOP_COUNT                 0x00A08F90u   /* main-loop iterations */
#define IF2_OK_LOOP_COUNT                1
#define IF2_R_CLAMP15                    0x0093ACE0u   /* frame dt clamp 1/15 */
#define IF2_OK_CLAMP15                   1
#define IF2_R_CLAMP30                    0x0093ACE8u   /* frame dt clamp 1/30 */
#define IF2_OK_CLAMP30                   1
#define IF2_R_FN_SET_NODE_STATE          0x0037A2E8u   /* SetNodeState(graph, node, state) */
#define IF2_OK_FN_SET_NODE_STATE         1
#define IF2_FN_FN_SET_NODE_STATE         func_0037A2E8
#define IF2_R_FN_WARP_TO_CHECKPOINT      0x00378EA0u   /* TaskGraph::WarpToCheckpoint */
#define IF2_OK_FN_WARP_TO_CHECKPOINT     1
#define IF2_FN_FN_WARP_TO_CHECKPOINT     func_00378EA0
#define IF2_R_TG                         0x00A94E10u   /* the task graph object */
#define IF2_OK_TG                        1
#define IF2_R_TG_INDEX                   0x00A94E00u   /* -> the graph's u16 index table */
#define IF2_OK_TG_INDEX                  1
#define IF2_R_WARP_MGR                   0x00A26C80u   /* the world warp manager */
#define IF2_OK_WARP_MGR                  1
#define IF2_R_WARP_LOCK                  0x00A32EB0u   /* u8: warps refused while set */
#define IF2_OK_WARP_LOCK                 1
#define IF2_R_RESTART_FLAG               0x00A08F1Du   /* u8: restart the missions at the next world start */
#define IF2_OK_RESTART_FLAG              1
#define IF2_R_MAIN_LOOP_RET              0x001DB824u   /* return address of the main loop's cellSysutilCheckCallback */
#define IF2_OK_MAIN_LOOP_RET             1
#define IF2_R_SIG_NAMES                  0x00983910u   /* reflection table: name pointer per signal id */
#define IF2_OK_SIG_NAMES                 1
#define IF2_R_PL_SIGNAL_FN               0x002F26B8u   /* the game's signal dispatcher (post-lift hook) */
#define IF2_OK_PL_SIGNAL_FN              1
#define IF2_FN_PL_SIGNAL_FN              func_002F26B8
#define IF2_R_PL_FMV_FN                  0x002E7EDCu   /* the FMV quad's function */
#define IF2_OK_PL_FMV_FN                 1
#define IF2_FN_PL_FMV_FN                 func_002E7EDC
#define IF2_R_PL_FMV_CALL                0x002E8030u   /* its bl the hook goes before */
#define IF2_OK_PL_FMV_CALL               1
#define IF2_R_PL_FMV_CALLEE              0x002D043Cu   /* that bl's target */
#define IF2_OK_PL_FMV_CALLEE             1
#define IF2_FN_PL_FMV_CALLEE             func_002D043C
#define IF2_R_PL_PHYS_FN                 0x004796E0u   /* the physics step */
#define IF2_OK_PL_PHYS_FN                1
#define IF2_FN_PL_PHYS_FN                func_004796E0
#define IF2_R_PL_PHYS_CALL               0x00479D5Cu   /* its bl to the PPU narrowphase fallback */
#define IF2_OK_PL_PHYS_CALL              1
#define IF2_R_PL_PHYS_CALLEE             0x0047D438u   /* the PPU narrowphase fallback */
#define IF2_OK_PL_PHYS_CALLEE            1
#define IF2_FN_PL_PHYS_CALLEE            func_0047D438
#define IF2_R_PL_MESH_FN                 0x004761F4u   /* the mesh contact generator's body */
#define IF2_OK_PL_MESH_FN                1
#define IF2_FN_PL_MESH_FN                func_004761F4
#define IF2_R_PL_MESH_LRSAVE             0x00476200u   /* its LR save (std r0, frame(r1)) */
#define IF2_OK_PL_MESH_LRSAVE            1
#define IF2_R_LANG_LISTS                 0x008D09D0u   /* {count, list} per edition (the US one first) */
#define IF2_OK_LANG_LISTS                1
#define IF2_R_LANG_LIST_US               0x006ECE14u   /* the US edition's list */
#define IF2_OK_LANG_LIST_US              1
#define IF2_R_LANG_LIST_EU               0x006ECE24u   /* the European edition's list */
#define IF2_OK_LANG_LIST_EU              1
#define IF2_R_WEV_MK_SC                  0x003374F4u   /* main kicker: the instruction after its usleep */
#define IF2_OK_WEV_MK_SC                 1
#define IF2_R_WEV_MK_C1                  0x001DB7DCu   /* main kicker: caller 1 */
#define IF2_OK_WEV_MK_C1                 1
#define IF2_R_WEV_MK_C2                  0x00337554u   /* main kicker: caller 2 */
#define IF2_OK_WEV_MK_C2                 1
#define IF2_R_WEV_MK_C3                  0x00337928u   /* main kicker: caller 3 */
#define IF2_OK_WEV_MK_C3                 1
#define IF2_R_WEV_MW_SC                  0x00370634u   /* main WWS wait: after its first usleep */
#define IF2_OK_WEV_MW_SC                 1
#define IF2_R_WEV_MW_SC2                 0x0037065Cu   /* main WWS wait: after its second usleep */
#define IF2_OK_WEV_MW_SC2                1
#define IF2_R_WEV_MW_C1                  0x00370600u   /* main WWS wait: caller 1 */
#define IF2_OK_WEV_MW_C1                 1
#define IF2_R_WEV_MW_C2                  0x00370640u   /* main WWS wait: caller 2 */
#define IF2_OK_WEV_MW_C2                 1
#define IF2_R_WEV_MW_C3                  0x00370668u   /* main WWS wait: caller 3 */
#define IF2_OK_WEV_MW_C3                 1
#define IF2_R_WEV_M2_SC                  0x003710A8u   /* main WWS wait 2: after its first usleep */
#define IF2_OK_WEV_M2_SC                 1
#define IF2_R_WEV_M2_SC2                 0x003710D4u   /* main WWS wait 2: after its second usleep */
#define IF2_OK_WEV_M2_SC2                1
#define IF2_R_WEV_M2_C1                  0x0037108Cu   /* main WWS wait 2: caller 1 */
#define IF2_OK_WEV_M2_C1                 1
#define IF2_R_WEV_M2_C2                  0x003710B8u   /* main WWS wait 2: caller 2 */
#define IF2_OK_WEV_M2_C2                 1
#define IF2_R_WEV_M2_C3                  0x003710E4u   /* main WWS wait 2: caller 3 */
#define IF2_OK_WEV_M2_C3                 1
#define IF2_R_WEV_LK_SC                  0x0033745Cu   /* light kicker: after its usleep */
#define IF2_OK_WEV_LK_SC                 1
#define IF2_R_WEV_LK_C1                  0x00337430u   /* light kicker: caller 1 */
#define IF2_OK_WEV_LK_C1                 1
#define IF2_R_WEV_LK_C2                  0x00337464u   /* light kicker: caller 2 */
#define IF2_OK_WEV_LK_C2                 1
#define IF2_R_WEV_LK_PERIODS             0x006F2360u   /* light kicker: poll periods per state */
#define IF2_OK_WEV_LK_PERIODS            1
#define IF2_R_WEV_EK_SC                  0x00338D08u   /* Edge kicker: after its usleep */
#define IF2_OK_WEV_EK_SC                 1
#define IF2_R_WEV_EK_C1                  0x00338CE8u   /* Edge kicker: caller 1 */
#define IF2_OK_WEV_EK_C1                 1
#define IF2_R_WEV_EK_C2                  0x00338D10u   /* Edge kicker: caller 2 */
#define IF2_OK_WEV_EK_C2                 1
#define IF2_R_WEV_EK_GLOB                0x00338AECu   /* Edge kicker's step: globals = *(r2 + d) */
#define IF2_OK_WEV_EK_GLOB               1
#define IF2_R_WEV_EK_GLOB_DISP           (-31440)
#define IF2_OK_WEV_EK_GLOB_DISP          1
#define IF2_R_WEV_EK_ROW0                0x003707D4u   /* the row-0 id read (q+0xB8) */
#define IF2_OK_WEV_EK_ROW0               1
#define IF2_R_WEV_EK_ROW1                0x00371138u   /* the row-1 id read (q+0xD4) */
#define IF2_OK_WEV_EK_ROW1               1
#define IF2_R_WWS_PM                     0x00887080u   /* the Wws_Job policy module (SPU code, 0x2E80 bytes) */
#define IF2_OK_WWS_PM                    1
#define IF2_R_MF_INPUT_THUNK             0x00000000u   /* the menus' per-frame input handler (the adjustor thunk the input syste */
#define IF2_OK_MF_INPUT_THUNK            0
#define IF2_FN_MF_INPUT_THUNK            if2_unresolved_fn
#define IF2_R_MF_ADD_NAV_ACTION          0x00000000u   /* ADD_NAV_ACTION(menu, mask, from, to) */
#define IF2_OK_MF_ADD_NAV_ACTION         0
#define IF2_FN_MF_ADD_NAV_ACTION         if2_unresolved_fn
#define IF2_R_MF_DICT_LOOKUP             0x00000000u   /* string dictionary lookup (dict, id, 0) */
#define IF2_OK_MF_DICT_LOOKUP            0
#define IF2_FN_MF_DICT_LOOKUP            if2_unresolved_fn
#define IF2_R_MF_TB_SET_STRING           0x00000000u   /* text box: set string id (tb, id) */
#define IF2_OK_MF_TB_SET_STRING          0
#define IF2_FN_MF_TB_SET_STRING          if2_unresolved_fn
#define IF2_R_MF_NEW                     0x00000000u   /* operator new (size) */
#define IF2_OK_MF_NEW                    0
#define IF2_FN_MF_NEW                    if2_unresolved_fn
#define IF2_R_MF_OBJ_ADD                 0x00000000u   /* scene object ADD */
#define IF2_OK_MF_OBJ_ADD                0
#define IF2_FN_MF_OBJ_ADD                if2_unresolved_fn
#define IF2_R_MF_MENU_JOYSTICK           0x00000000u   /* the menu's joystick (menu) */
#define IF2_OK_MF_MENU_JOYSTICK          0
#define IF2_FN_MF_MENU_JOYSTICK          if2_unresolved_fn
#define IF2_R_MF_UI_INPUT                0x00000000u   /* the UI that has the input */
#define IF2_OK_MF_UI_INPUT               0
#define IF2_FN_MF_UI_INPUT               if2_unresolved_fn
#define IF2_R_MF_MSGBOX                  0x00000000u   /* message box (title, text, buttons, ..., callback OPD, 0) */
#define IF2_OK_MF_MSGBOX                 0
#define IF2_FN_MF_MSGBOX                 if2_unresolved_fn
#define IF2_R_MF_MENU_BY_SYMBOL          0x00000000u   /* the menu with this symbol (id) */
#define IF2_OK_MF_MENU_BY_SYMBOL         0
#define IF2_FN_MF_MENU_BY_SYMBOL         if2_unresolved_fn
#define IF2_R_MF_PUSH_MENU               0x00000000u   /* PUSH_MENU's C++ (menu, id) */
#define IF2_OK_MF_PUSH_MENU              0
#define IF2_FN_MF_PUSH_MENU              if2_unresolved_fn
#define IF2_R_MF_POP_MENU                0x00000000u   /* POP_MENU's C++ (menu) */
#define IF2_OK_MF_POP_MENU               0
#define IF2_FN_MF_POP_MENU               if2_unresolved_fn
#define IF2_R_MF_CLEAR_ITEMS             0x00000000u   /* CLEAR_ITEMS(menu) */
#define IF2_OK_MF_CLEAR_ITEMS            0
#define IF2_FN_MF_CLEAR_ITEMS            if2_unresolved_fn
#define IF2_R_MF_ADD_WIDGET_ITEM         0x00000000u   /* ADD_WIDGET_ITEM(menu, widget, symbol) */
#define IF2_OK_MF_ADD_WIDGET_ITEM        0
#define IF2_FN_MF_ADD_WIDGET_ITEM        if2_unresolved_fn
#define IF2_R_MF_FOCUS                   0x00000000u   /* focus (menu, symbol): the FOCUS setter */
#define IF2_OK_MF_FOCUS                  0
#define IF2_FN_MF_FOCUS                  if2_unresolved_fn
#define IF2_R_MF_ADD_WIDGET_BY_PROXY     0x00000000u   /* ADD_WIDGET_BY_PROXY's C++ (widget list, proxy) */
#define IF2_OK_MF_ADD_WIDGET_BY_PROXY    0
#define IF2_FN_MF_ADD_WIDGET_BY_PROXY    if2_unresolved_fn
#define IF2_R_MF_WLIST_CLEAR             0x00000000u   /* CLEAR(widget list) */
#define IF2_OK_MF_WLIST_CLEAR            0
#define IF2_FN_MF_WLIST_CLEAR            if2_unresolved_fn
#define IF2_R_MF_ADD_ITEM                0x00000000u   /* ADD_ITEM's C++ (list widget, key, label id, 0, colour) */
#define IF2_OK_MF_ADD_ITEM               0
#define IF2_FN_MF_ADD_ITEM               if2_unresolved_fn
#define IF2_R_MF_SET_CURRENT_INDEX       0x00000000u   /* CURRENT_INDEX's setter (list widget, index) */
#define IF2_OK_MF_SET_CURRENT_INDEX      0
#define IF2_FN_MF_SET_CURRENT_INDEX      if2_unresolved_fn
#define IF2_R_MF_MULTIMAP_INSERT         0x00000000u   /* hash multimap insert (table, &key, &value) */
#define IF2_OK_MF_MULTIMAP_INSERT        0
#define IF2_FN_MF_MULTIMAP_INSERT        if2_unresolved_fn
#define IF2_R_MF_DICT_LOCK_MAKE          0x00000000u   /* the dictionary's lock: made on first use */
#define IF2_OK_MF_DICT_LOCK_MAKE         0
#define IF2_FN_MF_DICT_LOCK_MAKE         if2_unresolved_fn
#define IF2_R_MF_REMOVE_WIDGET           0x00000000u   /* REMOVE_WIDGET's C++ (widget list, row) */
#define IF2_OK_MF_REMOVE_WIDGET          0
#define IF2_FN_MF_REMOVE_WIDGET          if2_unresolved_fn
#define IF2_R_MF_SET_HANDLER             0x00000000u   /* SET_HANDLER (widget, [handler]) -- the native */
#define IF2_OK_MF_SET_HANDLER            0
#define IF2_FN_MF_SET_HANDLER            if2_unresolved_fn
#define IF2_R_MF_OBJ_REMOVE              0x00000000u   /* scene object REMOVE */
#define IF2_OK_MF_OBJ_REMOVE             0
#define IF2_FN_MF_OBJ_REMOVE             if2_unresolved_fn
#define IF2_R_MF_NAT_ADD_WIDGET_ITEM     0x00000000u   /* the ADD_WIDGET_ITEM native (intercepted on our screen) */
#define IF2_OK_MF_NAT_ADD_WIDGET_ITEM    0
#define IF2_FN_MF_NAT_ADD_WIDGET_ITEM    if2_unresolved_fn
#define IF2_R_MF_NAT_ADD_NAV_ACTION      0x00000000u   /* the ADD_NAV_ACTION native (intercepted on our screen) */
#define IF2_OK_MF_NAT_ADD_NAV_ACTION     0
#define IF2_FN_MF_NAT_ADD_NAV_ACTION     if2_unresolved_fn
#define IF2_R_MF_LWMUTEX_LOCK            0x00000000u   /* sys_lwmutex_lock (import stub) */
#define IF2_OK_MF_LWMUTEX_LOCK           0
#define IF2_FN_MF_LWMUTEX_LOCK           if2_unresolved_fn
#define IF2_R_MF_LWMUTEX_UNLOCK          0x00000000u   /* sys_lwmutex_unlock (import stub) */
#define IF2_OK_MF_LWMUTEX_UNLOCK         0
#define IF2_FN_MF_LWMUTEX_UNLOCK         if2_unresolved_fn
#define IF2_R_MD_STR_MGR                 0x00000000u   /* -> the string manager */
#define IF2_OK_MD_STR_MGR                0
#define IF2_R_MD_UI_MGR                  0x00000000u   /* -> the UI manager */
#define IF2_OK_MD_UI_MGR                 0
#define IF2_R_MD_REPEAT_T                0x00000000u   /* f32: the menu handler's button-query argument (0.15) */
#define IF2_OK_MD_REPEAT_T               0
#define IF2_R_MD_VT_MENU                 0x00000000u   /* vtable: menu */
#define IF2_OK_MD_VT_MENU                0
#define IF2_R_MD_VT_LIST                 0x00000000u   /* vtable: list widget */
#define IF2_OK_MD_VT_LIST                0
#define IF2_R_MD_VT_WLIST                0x00000000u   /* vtable: widget list */
#define IF2_OK_MD_VT_WLIST               0
#define IF2_R_MD_VT_PROXY                0x00000000u   /* vtable: widget proxy */
#define IF2_OK_MD_VT_PROXY               0
#define IF2_R_MD_VT_TEXTBOX              0x00000000u   /* vtable: text box */
#define IF2_OK_MD_VT_TEXTBOX             0

#define IF2_HAVE_ASPECT                  1
#define IF2_HAVE_LOD                     1
#define IF2_HAVE_LOD_QUALITY             0   /* not found: quality_table, quality_c12, quality_c13, quality_c14, quality_c15, quality_c16, quality_c17, quality_c18, quality_c19, quality_c20 */
#define IF2_HAVE_FMV_FIT                 1
#define IF2_HAVE_DTCLAMP                 1
#define IF2_HAVE_CLOCKLOG                1
#define IF2_HAVE_TASKJUMP                1
#define IF2_HAVE_SIGNALS                 1
#define IF2_HAVE_WEV_MAIN_KICKER         1
#define IF2_HAVE_WEV_MAIN_WWS            1
#define IF2_HAVE_WEV_MAIN_WWS2           1
#define IF2_HAVE_WEV_LIGHT_KICKER        1
#define IF2_HAVE_WEV_EDGE_KICKER         1
#define IF2_HAVE_MENU                    0   /* not found: mf_input_thunk, mf_add_nav_action, mf_dict_lookup, mf_tb_set_string, mf_new, mf_obj_add, mf_menu_joystick, mf_ui_input, mf_msgbox, mf_menu_by_symbol,  */
#define IF2_HAVE_MENU_TRACE              1
#define IF2_HAVE_LANGUAGE_EDITION        1
#define IF2_HAVE_PHYSFB                  1
#define IF2_HAVE_WWS_PM                  1
#define IF2_FEATURES_OFF "lod_quality, menu"

#define IF2_TRACE_NATIVES(X) \
    /* end */
#define IF2_TRACE_ACCESSORS(X) \
    /* end */


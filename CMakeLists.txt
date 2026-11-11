cmake_minimum_required(VERSION 3.20)
# inFamous 2 (Sucker Punch, 2011) -- static recompilation onto ps3recomp.
# Built from the ps3recomp starter template; game data is never in this tree.
project(infamous2
    VERSION 0.0.1
    LANGUAGES C CXX
    DESCRIPTION "inFamous 2 (PS3) statically recompiled to a native executable"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: the pinned checkout tools/setup.sh clones into external/.
    set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ps3recomp")
endif()
if(NOT EXISTS "${PS3RECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "No ps3recomp checkout at ${PS3RECOMP_DIR}. "
                        "Run tools/setup.sh (or setup.ps1), or pass -DPS3RECOMP_DIR=.")
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/ppu" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run tools/relift.sh against your own "
        "decrypted EBOOT first (see README), or point -DRECOMP_DIR= at it.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
    # Save-state continuations (table, chain validation, resume dispatcher).
    # A lift made with --cont-list registers its continuations here; without
    # one it is idle (PPU_RESUME_CHECK_EVERY is its only, diagnostic, switch).
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_resume.cpp"
    # Save states P0 (SS_SELFRESUME): per-thread self-resume, the PPU half of
    # runtime/savestate/ss_core.c's protocol. Idle unless the switch is set.
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_selfresume.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Real firmware SPURS (libsre), lifted from the PLAYER'S OWN decrypted libsre.prx
# by tools/lift_libsre.sh into generated/prx/ (git-ignored, never distributed).
# Optional: without it the game falls back to ps3recomp's HLE cellSpurs, which
# lacks _cellSpursLFQueuePushBody and stalls inFamous 2's psarc decompressor.
# ---------------------------------------------------------------------------
set(PRX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/prx")
set(PRX_SOURCES "")
if(EXISTS "${PRX_DIR}/libsre_ns/libsre_exports.c" AND EXISTS "${PRX_DIR}/libsre.linked.bin")
    file(GLOB LIBSRE_LIFTED CONFIGURE_DEPENDS "${PRX_DIR}/libsre_ns/ppu_recomp_*.cpp")
    list(APPEND PRX_SOURCES ${LIBSRE_LIFTED}
         "${PRX_DIR}/libsre_ns/libsre_exports.c"
         "${PS3RECOMP_DIR}/runtime/prx/prx_loader.c")
    # libsre's own SPU programs (the SPURS kernel), lifted by lift_libsre.sh.
    file(GLOB LIBSRE_SPU CONFIGURE_DEPENDS
         "${PRX_DIR}/spu_lifted/*/spu_recomp.c" "${PRX_DIR}/spu_lifted/spu_workloads.c"
         # SPURS policy modules captured at LS 0xA00 (tools/lift_policy.py)
         "${CMAKE_CURRENT_SOURCE_DIR}/generated/spu_pm/*/spu_recomp.c"
         "${CMAKE_CURRENT_SOURCE_DIR}/generated/spu_pm/spu_pm_register.c"
         # WWS job modules from the EBOOT (tools/lift_jobs.sh)
         "${CMAKE_CURRENT_SOURCE_DIR}/generated/spu_jobs/*/spu_recomp.c"
         "${CMAKE_CURRENT_SOURCE_DIR}/generated/spu_jobs/jobmods_register.c"
         # SPURS JobChain job binaries, position-independent (tools/lift_jobchain.py)
         "${CMAKE_CURRENT_SOURCE_DIR}/generated/spu_jobchain/*/spu_recomp.c"
         "${CMAKE_CURRENT_SOURCE_DIR}/generated/spu_jobchain/jobchain_register.c")
    message(STATUS "libsre (LLE):    ${PRX_DIR}")
else()
    message(STATUS "libsre (LLE):    not lifted -- HLE cellSpurs (run tools/lift_libsre.sh)")
endif()

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/stubs.cpp
    src/infamous2_hooks.cpp
    src/if2_checkpoint.cpp
    src/if2_taskgraph.cpp
    src/if2_menu.cpp
    src/if2_clock.cpp
    src/if2_waitev.cpp
    src/if2_ss_title.cpp
    src/if2_png.cpp
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
    ${PRX_SOURCES}
)
# Linker map: resolves HOST_SAMPLE's addresses (tools/host_profile.py).
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE "/MAP:${CMAKE_BINARY_DIR}/infamous2.map")
endif()
if(PRX_SOURCES)
    # C/C++ only: rc.exe (app.rc) gets the target's defines too, and a quoted path with a space in it
    # ("C:/Users/First Last/...") is "RC1107: invalid usage" there (issue #2).
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        IF2_HAVE_LIBSRE=1 "$<$<COMPILE_LANGUAGE:C,CXX>:IF2_PRX_DIR=\"${PRX_DIR}\">")
    target_include_directories(${PROJECT_NAME} PRIVATE "${PS3RECOMP_DIR}/runtime/prx")
    if(LIBSRE_SPU)
        target_sources(${PROJECT_NAME} PRIVATE ${LIBSRE_SPU})
        set_source_files_properties(${LIBSRE_SPU} PROPERTIES
            INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
            COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT
            COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
        ps3recomp_apply_spu_isa(${LIBSRE_SPU})
        if(NOT SPU_SOURCES)   # dispatch_mt is otherwise added with the game's SPU set
            target_sources(${PROJECT_NAME} PRIVATE "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        endif()
    endif()
    set_source_files_properties(${LIBSRE_LIFTED} PROPERTIES
        COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w;/bigobj,-w;-ffp-contract=off>")
endif()

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
#   PS3RECOMP_SPU_LIFTED_FLAGS -- host ISA for the SPU helpers (x86-64-v3 by
#                          default: AVX2/FMA3, ~10x faster float/shuffle helpers),
#                          no FP contraction, /GS- /Zc:tlsGuards-, and
#                          -DSPU_HELPERS_FP_RZDAZ (PS3RECOMP_SPU_FP_RZDAZ: helpers
#                          for the RZ+DAZ+FTZ mode the runtime then forces on SPU
#                          host threads; same results, ~2x faster float code);
#                          set by the SDK's CMakeLists.txt and applied with
#                          ps3recomp_apply_spu_isa(), which also tells the
#                          runtime (CPU check, FP mode). Also on LIBSRE_SPU above.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(MSVC)
        # Generated code: warnings are noise here (clang-cl emitted ~2,000
        # -Wunsafe-buffer-usage lines per build). /O2 = the -O2 below.
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;/O2")
    else()
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-O2")
    endif()
    ps3recomp_apply_spu_isa(${SPU_SOURCES})
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc")
    # The icon: the disc's own PS3_GAME/ICON0.PNG, converted into the build folder (game artwork: never committed).
    set(_if2_icon_src "${CMAKE_CURRENT_SOURCE_DIR}/game/disc/PS3_GAME/ICON0.PNG")
    set(_if2_icon "${CMAKE_BINARY_DIR}/infamous2.ico")
    if(EXISTS "${_if2_icon_src}")
        if(NOT EXISTS "${_if2_icon}" OR "${_if2_icon_src}" IS_NEWER_THAN "${_if2_icon}")
            execute_process(COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                                    -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_icon.ps1"
                                    -In "${_if2_icon_src}" -Out "${_if2_icon}"
                            RESULT_VARIABLE _if2_icon_rc OUTPUT_QUIET)
        endif()
    endif()
    if(EXISTS "${_if2_icon}")
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc" PROPERTIES
            COMPILE_DEFINITIONS IF2_HAVE_ICON
            INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}"
            OBJECT_DEPENDS "${_if2_icon}")
        message(STATUS "infamous2.exe icon: ${_if2_icon} (from the disc's ICON0.PNG)")
    else()
        message(STATUS "infamous2.exe icon: none (no game/disc/PS3_GAME/ICON0.PNG)")
    endif()
endif()

# The EBOOT this build lifted (analysis/inputs.json), relative to the project root: infamous2.exe started with no
# arguments (double-clicked) runs it from there.
set(_if2_eboot_rel "game/disc/PS3_GAME/USRDIR/EBOOT.elf")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/analysis/inputs.json")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/analysis/inputs.json" _if2_inputs)
    string(JSON _if2_eboot ERROR_VARIABLE _if2_jerr GET "${_if2_inputs}" eboot)
    if(NOT _if2_jerr AND _if2_eboot)
        file(TO_CMAKE_PATH "${_if2_eboot}" _if2_eboot)
        file(RELATIVE_PATH _if2_eboot_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_if2_eboot}")
    endif()
endif()
# Where the title hooks' addresses are in that EBOOT: tools/if2_resolve.py (the lift step) found them by code
# pattern and wrote generated/if2_resolved.h (src/if2_version.h includes it); its report is
# analysis/resolve_report.txt. No header = a lift from before the resolver: run the lift step again.
set(_if2_resolved "${CMAKE_CURRENT_SOURCE_DIR}/generated/if2_resolved.h")
if(NOT EXISTS "${_if2_resolved}")
    message(FATAL_ERROR "generated/if2_resolved.h is missing: run the lift step again "
                        "(powershell -File tools\\build.ps1 -Step lift), then build")
endif()
file(STRINGS "${_if2_resolved}" _if2_name REGEX "^#define IF2_EBOOT_NAME ")
string(REGEX REPLACE "^#define IF2_EBOOT_NAME \"(.*)\"$" "\\1" _if2_name "${_if2_name}")
message(STATUS "infamous2 hooks: ${_if2_name} (generated/if2_resolved.h)")
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/generated")
# C/C++ only, as IF2_PRX_DIR above: string defines never reach rc.exe.
target_compile_definitions(${PROJECT_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:IF2_DEFAULT_EBOOT=\"${_if2_eboot_rel}\">")

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
        # The D3D12 backend uses IID_IDXGIFactory6 & co. but only pulls in
        # d3d12/dxgi/d3dcompiler via #pragma; the IIDs live in dxguid.lib
        # (lld-link: undefined symbol IID_IDXGIFactory6).
        dxguid
    )
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
if(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()

# ---------------------------------------------------------------------------
# PPU code speed (fps plan M5). Every switch here is output-exact -- the same
# guest behaviour and pixels, only different host code -- and OFF by default.
# tools\build.ps1 sets them (-Pgo, -PgoScope, -Define NAME=VALUE); like any
# cache option they stay set for later builds of this build directory.
#
#   IF2_PPU_VM_INLINE      OFF | reads | all: the lifted PPU code (game and
#                          libsre) inlines the guest memory accessors' fast path
#                          -- a gate test, a range compare (+ the SPU reserved-
#                          line test on stores), the byte-swapped access
#                          (runtime/ppu/ppu_vm_inline.h, force-included) -- for
#                          loads only (reads: nearly all of the gain at a
#                          quarter of the code growth) or for loads and stores.
#                          Every other access still calls the out-of-line
#                          accessor, whose full path is unchanged. Inline reads
#                          skip the HOTREAD spin detector; PPU_HOTREAD=1 (or
#                          STALL_DUMP / PPU_SPINBT) routes them back through it.
#   IF2_PPU_NO_TLS_GUARDS  /Zc:tlsGuards- on the lifted PPU code, as the lifted
#                          SPU code already has: drops the on-demand TLS-init
#                          check in front of every g_trampoline_fn store. An
#                          EXE's TLS is initialized for each thread before the
#                          thread runs, so the check never fires.
#   IF2_PPU_ARCH_V3        x86-64-v3 (/arch:AVX2: BMI2, MOVBE, AVX2) for the
#                          lifted PPU code, FP contraction off. Measured no
#                          faster; for A/B only.
#   IF2_PGO                OFF | instrument | use: clang profile-guided
#                          optimization over IF2_PGO_SCOPE. instrument: counters,
#                          plus src/if2_pgo.cpp, which writes each run's profile
#                          to <exe dir>\pgo-raw\if2-<pid>.profraw. use: optimize
#                          with IF2_PGO_PROFILE (llvm-profdata merge of those).
#   IF2_PGO_SCOPE          ppu: the lifted PPU code (game and libsre) and the
#                          PPU scaffold. all: every source of the game and of
#                          the ps3recomp runtime library (SPU code, RSX ...).
#                          host: all but the SPU code (the lifted SPU programs
#                          and the runtime's runtime/spu/ sources). all measured
#                          walker -8%, PPU main -8% per frame but SPU +11%: the
#                          short training runs leave SPU code cold.
# ---------------------------------------------------------------------------
set(IF2_PPU_VM_INLINE "OFF" CACHE STRING
    "Inline the guest memory accessors' fast path into the lifted PPU code: OFF, reads or all")
set_property(CACHE IF2_PPU_VM_INLINE PROPERTY STRINGS OFF reads all)
option(IF2_PPU_NO_TLS_GUARDS "/Zc:tlsGuards- on the lifted PPU code" OFF)
option(IF2_PPU_ARCH_V3       "x86-64-v3 for the lifted PPU code (FP contraction off)" OFF)
set(IF2_PGO "OFF" CACHE STRING "Profile-guided optimization: OFF, instrument or use")
set_property(CACHE IF2_PGO PROPERTY STRINGS OFF instrument use)
set(IF2_PGO_SCOPE "ppu" CACHE STRING "What IF2_PGO covers: ppu, host or all")
set_property(CACHE IF2_PGO_SCOPE PROPERTY STRINGS ppu host all)
set(IF2_PGO_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/pgo/infamous2.profdata" CACHE FILEPATH
    "Merged profile (llvm-profdata merge) for IF2_PGO=use")

set(IF2_PPU_LIFTED ${RECOMP_SOURCES} ${LIBSRE_LIFTED})
string(TOUPPER "${IF2_PPU_VM_INLINE}" _if2_vmi)
if(_if2_vmi MATCHES "^(OFF|0|NO|FALSE|)$")
    set(_if2_vmi "")
elseif(_if2_vmi MATCHES "^(ALL|ON|1|YES|TRUE)$")
    set(_if2_vmi "all")
elseif(_if2_vmi STREQUAL "READS")
    set(_if2_vmi "reads")
else()
    message(FATAL_ERROR "IF2_PPU_VM_INLINE must be OFF, reads or all (is '${IF2_PPU_VM_INLINE}')")
endif()
if((_if2_vmi OR IF2_PPU_NO_TLS_GUARDS OR IF2_PPU_ARCH_V3 OR NOT IF2_PGO STREQUAL "OFF")
   AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "IF2_PPU_* and IF2_PGO need clang / clang-cl")
endif()
if(_if2_vmi)
    if(NOT EXISTS "${PS3RECOMP_DIR}/runtime/ppu/ppu_vm_inline.h")
        message(FATAL_ERROR "IF2_PPU_VM_INLINE=${IF2_PPU_VM_INLINE} but ${PS3RECOMP_DIR}/runtime/"
                            "ppu/ppu_vm_inline.h is missing (the toolkit was reset without the M5 "
                            "change): re-apply it, or -DIF2_PPU_VM_INLINE=OFF")
    endif()
    set_property(SOURCE ${IF2_PPU_LIFTED} APPEND PROPERTY COMPILE_DEFINITIONS PPU_VM_INLINE=1)
    if(_if2_vmi STREQUAL "reads")
        set_property(SOURCE ${IF2_PPU_LIFTED} APPEND PROPERTY COMPILE_DEFINITIONS PPU_VM_INLINE_WRITES=0)
    endif()
    if(MSVC)
        set_property(SOURCE ${IF2_PPU_LIFTED} APPEND PROPERTY COMPILE_OPTIONS
            "/FI${PS3RECOMP_DIR}/runtime/ppu/ppu_vm_inline.h")
    else()
        set_property(SOURCE ${IF2_PPU_LIFTED} APPEND PROPERTY COMPILE_OPTIONS
            "SHELL:-include \"${PS3RECOMP_DIR}/runtime/ppu/ppu_vm_inline.h\"")
    endif()
    # The out-of-line side: PPU_HOTREAD / STALL_DUMP / PPU_SPINBT arm the read gate.
    set_property(SOURCE "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp" APPEND PROPERTY
        COMPILE_DEFINITIONS PPU_VM_INLINE_LIFTED=1)
endif()
if(IF2_PPU_NO_TLS_GUARDS AND MSVC)
    set_property(SOURCE ${IF2_PPU_LIFTED} APPEND PROPERTY COMPILE_OPTIONS "/Zc:tlsGuards-")
endif()
if(IF2_PPU_ARCH_V3)
    if(MSVC)
        set_property(SOURCE ${IF2_PPU_LIFTED} APPEND PROPERTY COMPILE_OPTIONS
            "/arch:AVX2" "/clang:-ffp-contract=off")
    else()
        set_property(SOURCE ${IF2_PPU_LIFTED} APPEND PROPERTY COMPILE_OPTIONS
            "-march=x86-64-v3" "-ffp-contract=off")
    endif()
endif()
if(NOT IF2_PGO STREQUAL "OFF")
    if(IF2_PGO STREQUAL "instrument")
        set(_if2_pgo_flags "-fprofile-instr-generate")
        # The profile writer: the port leaves through ExitProcess, which skips
        # the profile runtime's own exit-time write.
        target_sources(${PROJECT_NAME} PRIVATE src/if2_pgo.cpp)
        set_property(SOURCE src/if2_pgo.cpp APPEND PROPERTY COMPILE_DEFINITIONS IF2_PGO_INSTRUMENT=1)
        # Link THIS compiler's profile runtime, by full path. The objects only
        # name it (/DEFAULTLIB:clang_rt.profile-x86_64.lib), and the MSVC tools
        # ship an older clang_rt.profile-x86_64.lib (raw format v10) that this
        # LLVM's llvm-profdata refuses to merge ("raw profile version
        # mismatch"). Which one the bare name finds depends on the library
        # search order: lld-link run by CMake under vcvars finds LLVM's, a
        # clang-cl-driven link finds MSVC's. A full path settles it.
        if(MSVC)
            execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-file-name=clang_rt.profile-x86_64.lib
                            OUTPUT_VARIABLE _if2_prt OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            file(TO_CMAKE_PATH "${_if2_prt}" _if2_prt)
            if(NOT IS_ABSOLUTE "${_if2_prt}" OR NOT EXISTS "${_if2_prt}")
                message(FATAL_ERROR "IF2_PGO=instrument: no clang_rt.profile-x86_64.lib beside "
                                    "${CMAKE_CXX_COMPILER} (got '${_if2_prt}')")
            endif()
            target_link_libraries(${PROJECT_NAME} PRIVATE "${_if2_prt}")
            message(STATUS "PGO runtime:     ${_if2_prt}")
        else()
            target_link_options(${PROJECT_NAME} PRIVATE -fprofile-instr-generate)
        endif()
    elseif(IF2_PGO STREQUAL "use")
        if(NOT EXISTS "${IF2_PGO_PROFILE}")
            message(FATAL_ERROR "IF2_PGO=use: no profile at ${IF2_PGO_PROFILE} "
                                "(tools\\build.ps1 -Pgo use merges build\\pgo-raw into it)")
        endif()
        # The profile's hash on the command line: a re-merged profile at the
        # same path then rebuilds what it covers (ninja does not track the file).
        file(MD5 "${IF2_PGO_PROFILE}" _if2_pgo_md5)
        set(_if2_pgo_flags "-fprofile-instr-use=${IF2_PGO_PROFILE}" "-DIF2_PGO_PROFILE_MD5=${_if2_pgo_md5}"
            -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing)
    else()
        message(FATAL_ERROR "IF2_PGO must be OFF, instrument or use (is '${IF2_PGO}')")
    endif()
    if(IF2_PGO_SCOPE STREQUAL "all" OR IF2_PGO_SCOPE STREQUAL "host")
        target_compile_options(${PROJECT_NAME} PRIVATE ${_if2_pgo_flags})
        target_compile_options(ps3recomp_runtime PRIVATE ${_if2_pgo_flags})
        if(IF2_PGO_SCOPE STREQUAL "host")
            # Per-source options follow the target's on the command line, so
            # the -fno- form turns the profile off for these files only.
            if(IF2_PGO STREQUAL "use")
                set(_if2_pgo_off "-fno-profile-instr-use")
            else()
                set(_if2_pgo_off "-fno-profile-instr-generate")
            endif()
            set_property(SOURCE ${SPU_SOURCES} ${LIBSRE_SPU} APPEND PROPERTY
                COMPILE_OPTIONS ${_if2_pgo_off})
            get_target_property(_if2_rt_srcs ps3recomp_runtime SOURCES)
            list(FILTER _if2_rt_srcs INCLUDE REGEX "runtime/spu/")
            set_property(SOURCE ${_if2_rt_srcs} TARGET_DIRECTORY ps3recomp_runtime APPEND PROPERTY
                COMPILE_OPTIONS ${_if2_pgo_off})
            list(LENGTH _if2_rt_srcs _if2_n_rt)
            message(STATUS "PGO host scope:  SPU programs and ${_if2_n_rt} runtime/spu source(s) left unprofiled")
        endif()
    elseif(IF2_PGO_SCOPE STREQUAL "ppu")
        set_property(SOURCE ${IF2_PPU_LIFTED} ${PS3RECOMP_SCAFFOLD} APPEND PROPERTY
            COMPILE_OPTIONS ${_if2_pgo_flags})
    else()
        message(FATAL_ERROR "IF2_PGO_SCOPE must be ppu, host or all (is '${IF2_PGO_SCOPE}')")
    endif()
endif()
message(STATUS "PPU code speed:  inline accessors ${IF2_PPU_VM_INLINE}, no TLS guards "
               "${IF2_PPU_NO_TLS_GUARDS}, x86-64-v3 ${IF2_PPU_ARCH_V3}, PGO ${IF2_PGO} (${IF2_PGO_SCOPE})")

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

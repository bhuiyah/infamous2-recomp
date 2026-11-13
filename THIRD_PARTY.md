# Third-party code and credits

This repository's own code is MIT-licensed (see `LICENSE`, "bhuiyah and infamous2-recomp contributors"). It contains
**no code, data or assets from inFamous 2, from Sony's firmware or from any PS3 SDK**. Everything below
is either fetched by the build from its own source or merely referred to.

## ps3recomp (fetched at build time, patched)

- **What:** the static recompilation toolkit this port is built on -- the PowerPC and SPU lifters, the
  ELF/SELF/PRX tools, the HLE runtime (the PS3 OS and libraries) and the RSX -> Direct3D 12 renderer.
- **Where:** <https://github.com/sp00nznet/ps3recomp>, fetched by `tools/setup.ps1` into `external/ps3recomp`
  at the commit in `ps3recomp.pin`. Not copied into this repository.
- **Licence:** MIT, `Copyright (c) 2026 Ned Heller / sp00nznet`.
- **Our changes to it:** `patches/ps3recomp/0001-entry-envp-and-lwcond-layout.patch` (applied by `tools/setup.ps1`)
  is a derivative of ps3recomp and is distributed under the same MIT licence.
- **Files derived from its project template:** `src/main.cpp`, `src/stubs.cpp` and `CMakeLists.txt` (MIT, same
  copyright line; noted in `LICENSE`).
- ps3recomp itself bundles third-party code under its own licences (for example `third_party/at3_standalone`,
  an ATRAC3 decoder under the LGPL 2.1, and the stb_image / stb_image_write single-file libraries in its image
  codecs). They are part of the toolkit checkout and of the executable you build, not of this repository; see
  the toolkit's own files for their terms.

## Native SPU job rewrites

- **What:** `src/if2_rlist_native.c` (the render-list builder of SPU job 225) and `src/if2_bsp_native.c` (the BSP
  polygon clipper of SPU job 216): C rewrites of what those jobs compute, adapted from MIT-licensed code and checked
  against the translated SPU code with their live checks (`RLIST_NATIVE=2`, `BSP_NATIVE=2`).
- **Licence:** MIT; the copyright and permission notices are at the top of each file.

## Referred to, not included

- **RPCS3** (<https://rpcs3.net>, GPL-2.0): used by players to decrypt their own EBOOT and firmware, and as a
  reference for hardware behaviour in comments and notes. No RPCS3 code is included here.
- **bethington/ghidra-mcp, Ghidra, Temurin JDK:** downloaded only if a contributor runs the optional
  `tools/setup_ghidra_mcp.ps1` (reverse-engineering setup, `docs/MCP_SETUP.md`).
- **Real-ESRGAN ncnn-vulkan** (BSD-3 / MIT): an optional external upscaler `tools/texpack` can call; never
  shipped here.
- The player's own build tools (Git for Windows, Python, CMake, Ninja, Visual Studio Build Tools, LLVM) are
  installed by the player; the launcher only offers to run `winget` for them after asking.

## Thanks

- Ned Heller (sp00nznet) and the ps3recomp contributors (`external/ps3recomp/CONTRIBUTORS.md` after setup).
- The RPCS3 team, whose emulator lets players decrypt their own dumps and whose research documents much of
  the PS3's behaviour.
- The people who captured real-PS3 footage of inFamous 2 that the image-quality work was measured against
  (`docs/GROUND_TRUTH.md`, `tools/ground_truth/targets.json`).
- Sucker Punch Productions, for the game.

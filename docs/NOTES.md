# Bring-up notes

Dated log of what was tried and where each run stopped. Newest first.
## 2026-11-13 — EG1 skinning 30% cheaper (6 threads +1%); EG3 gated out

- **Skinning** (`if2_eg_skin`): the prepared matrices are kept in a host-order copy (no byte swap per row load),
  range checks run on the vectors already loaded, vertices go two at a time in 256-bit registers (one per half),
  the blended rows go to the scratch table only for the slots no later batch rewrites, and the normalization does
  two groups at a time. Same operations, same order: offline 80/80 byte-identical, in game EG_VERIFY 1.9M compares,
  0 mismatches. Harness 33.7k -> 23.5k cycles a call. A/B (gate fight, 3 reps, EG_SKIN_V1=1 = the old loops):
  6 threads 21.04 -> 20.84 ms, 4 threads unchanged (28.21 / 28.23 ms).
- **EG3** (the whole Edge job as compute shaders) was gated by measurement and not built: the job is still ~13% of
  CPU at 4 threads (gate 1 passes: ~75k cycles a segment, skinning the largest stage), but the RSX cull state on
  the ring's draws is mixed -- back, front, and 12-30% with culling off (mostly the position-only shadow draws) --
  so dropping Edge's CPU culling, which a GPU path would do, changes what is drawn (gate 2 fails).
- **Index decompression** (0xC7B8) was made native and bit-exact but showed no gain; not shipped (EG_PLAN 9.3).

## 2026-11-13 — EG2: the renderer's Edge-ring draws take a lean stream path (4 threads +1.6%, 6 +1.2%)

- **Census** (VEMIT_STATS ring split, gate fight, per 2 s of the renderer thread): draws reading the Edge job's
  output ring cost ~780 of ~2740 Mcyc of vertex emission. Half are position-only (shadow-type, every attribute
  in the ring), half mix ring position/normal/tangent with static color and UV streams, so the plan's
  single-staged-buffer EG2 does not apply as is. Upper bound (EG2_SKIP_RING=1 drops those draws, wrong picture):
  6 threads 21.65 -> 20.37 ms, 4 threads 29.0 -> 27.3 ms, partly GPU work.
- **What changed** (toolkit `rsx_vertex_fetch.c`): `rsx_vemit_set_stream_range(lo, hi)` registers a stream-only
  range of RSX local memory (the title sets the Edge ring, 0x015E0000..0x01D60000). A draw with an attribute in it
  goes straight to the stream path: no density count, plan signatures or cache probe (ring data never repeats,
  so the cache always held it back), and its dedupe uses the index itself as the table slot (Edge indices are a
  compact 0..1023 range: sequential lines, no collisions). Same vertices, same picture.
- **Measured** (ring draws, Mcyc per 2 s): position-only 284 -> 192, mixed 480 -> 384. A/B (gate fight, 3 reps,
  EG2_STREAM=0/1): 6 threads 21.37 -> 21.12 ms (vertex time 5.81 -> 5.45 ms), 4 threads 28.53 -> 28.06 ms (6.60 ->
  6.07 ms).
- Not done: pre-converting the ring attributes on the SPU threads (would remove part of the remaining ~300 Mcyc of
  conversion, at the cost of a second vertex stream or a per-vertex merge in the backend).

## 2026-11-13 — EG1: Edge Geometry vertex decompression as native code, bit-exact (+1.2% / +1.6%)

- **What it is.** The Edge job's vertex decompression (0x4448, 15% of the job), specialized path for the one
  stream layout the game uses: positions as 3 x 16-bit fixed point (10.6, plus per-mesh offsets), normals and
  tangents as 24-bit unit vectors (two 10-bit components, the third as sqrt(1 - a^2 - b^2) via frsqest/fi, a
  4-bit selector into a 16-entry table that places the components, their sign and the tangent's handedness).
- **Native** (`if2_eg_decomp`): the same operations under the SPU threads' float mode; positions reduce exactly
  to (raw + offset) / 64; unit vectors 4 at a time with the toolkit's frsqest/fi helpers and the table turned
  into byte shuffles; it also writes the two zero quadwords the SPU loops' pipelines leave past each table.
  Any other stream layout, or a selector naming the table's text tail, uses the SPU code / the exact helpers.
- **Verification.** Offline 80 recorded calls byte-identical; in game EG_VERIFY=2 in both test states, ~1.9M
  compares over the three native stages, 0 mismatches. Harness ~10k cycles a call (exact reference 25k).
- **Gain** (gate fight, 2 reps, frame interval): 4 threads 29.45 -> 29.10 ms (+1.2%), 6 threads 21.85 -> 21.50 ms
  (+1.6%). EG_NO_DECOMP=1 keeps this stage on the SPU code (A/B).

## 2026-11-13 — EG1: Edge Geometry triangle culling as native code, bit-exact (4 threads +5.9%)

- **What it is.** The Edge job's second-largest stage (0xA798 with its per-vertex pass 0xA3C8; 26% of the job):
  each vertex is transformed by the object and view-projection rows, divided by |w| (frest, fi, one Newton step),
  mapped to the viewport and given an outcode (x/y/z bounds, w > 0); then per triangle: culled when all three
  outcodes share an outside plane, or (all in front) when back-facing (flavors 2/3) or when all three corners
  snap to one pixel column or row. The kept indices are compacted in place.
- **Native** (src/if2_edge_native.c `if2_eg_cull`): the same operations in the same order under the SPU threads'
  RZ/DAZ/FTZ mode. The in-place compaction reproduces the SPU code's software pipeline store for store (two leading
  all-culled groups, the read-merge at the write point, the stores past the input redirected to the stack), so
  the bytes after the kept list are identical too. Fast path: vertices 8 at a time (AVX2) within range bounds,
  triangles 4 at a time from a native copy [x, y, outcode, snapped x|y]; anything out of range takes the toolkit's
  exact SPU float helpers. A custom per-vertex callback, flavor 0 and the no-cull mode run the SPU code.
- **Verification.** Offline 80 recorded calls byte-identical (local store outside the stack frame, r3); in game
  EG_VERIFY=2 in both test states: ~1.26M compares (culling and skinning), 0 mismatches.
- **Cost.** Harness (TSC, warm): ~14-16k per call for ~550 vertices / ~850 triangles; the lifted code, at the
  census rates, ~54k.
- **Gain** (gate fight, 2 reps, frame interval): 4 threads 30.3 -> 28.6 ms (33.0 -> 34.9 fps, +5.9%), 6 threads
  22.0 -> 21.7 ms (+1.5%). Same option row (Frame > CPU > Native geometry processing); EG_NO_CULL=1 keeps
  culling on the SPU code (A/B).
- Also: the skinning's per-vertex diagnostic counters (v0.1.29) were shared globals bumped from every SPU thread;
  all such counters are compiled into the offline harness only now.

## 2026-11-13 — EG1: Edge Geometry skinning as native code, bit-exact (4 threads +4.5%)

- **What the job is.** SPU job module d0d15ec83992 (image 222, 22% of the SPU threads' samples, the largest) is
  Sony's Edge Geometry library in Sucker Punch's batch wrapper: ~940 mesh segments a frame (decompress, skin,
  cull, compress into a ring in RSX local memory, fill the command-buffer holes job 225 left). Measured stage
  split: skinning 36%, triangle culling 26%, vertex decompression 15%, index decompression 7%. Doubling the job's
  cost (EG_SPIN=100) costs 19% fps on 4 threads and 11% on 6: worth making cheaper.
- **Hook (toolkit).** Lifted `brsl` calls are direct C calls, so spu_registry_replace cannot reach them.
  spu_lifter.py --indirect-calls <addr,...> emits the listed calls through the dispatcher (as a `bisl`);
  lift_jobmods.py takes them per module from SPU_JOBMOD_INDIRECT_CALLS, which tools/lift_jobs.sh sets for the Edge
  module's stage entries. Only those 10 call sites change.
- **Native skinning (src/if2_edge_native.c, 0xD0A8).** Flavor 3 (4 influences, non-uniform scaling), matrix format
  0: the matrices rearranged in place into a diagonal layout, per vertex the 4 weights (byte * 0x3B808080, the
  job's own 1/255) blend 3 rows (fm + 3 fma), position by the translation-seeded fma chain, normal by the
  cofactor of the blended rows, tangent by the rotation; then normal and tangent tables normalized with the SPU's
  fi(frsqest). Same operations in the same order with the RZ/DAZ/FTZ mode the SPU threads run in; the blended rows
  go to the same scratch table in the same batches. Inputs are range-checked per vertex (and per normalization
  group); anything outside (and zero-length vectors, whose frsqest is an exponent-255 value) uses the toolkit's
  exact SPU float helpers. Unsupported layouts run the SPU code.
- **Verification.** Offline: 80 recorded calls (local store before/after, two save states), byte-identical outside
  the stack frame (analysis/edgegeom/harness, private). In game: EG_VERIFY=<n> runs native code on a copy and the
  SPU code on the real local store and compares: 543k calls, 0 mismatches.
- **Gain** (gate fight, 2 reps): 4 threads 31.3 -> 32.8 fps (interval 32.3 -> 30.9 ms); 6 threads unchanged
  within noise (the walker limits there). Option row Frame > CPU > Native geometry processing (default On,
  EG_NATIVE=0/1).
- Diagnostics: EG_DUMP, EG_CENSUS (per config and stage cycles), EG_SPIN, EG_CORPUS (recording), EG_VERIFY.
- Tried and dropped: converting dense never-cached vertex ranges straight into the stream (VC_DENSE_STREAM): the
  walker's cost on those draws is the conversion itself, not the per-index dedupe (32 cycles an index either way).

## 2026-11-12 — LE1: lower-end CPUs: G-buffer copies stay on the GPU, Auto pacing never below 30, sparse meshes cached (6 threads 39 -> 44 fps at the gate)

- **G-buffer capture on the GPU (default on; GLIGHT_GBUF_GPU=0 = the readback path).** The title copies each G-buffer surface to main memory for the lighting job as two NV3089 blits (1024 + 256 columns), so the whole-surface capture of GL1 never matched. The capture now takes any 1:1 rectangle of a colour or zeta surface into its place in the lighting input (box copy, RowPitch = the plane's pitch), and a plane is current once its columns add up to the width. The 4.6 ms readback and 1.7 ms of GPU waits a frame are gone at 6 threads (39 -> 43 fps); the picture matches GLIGHT_VERIFY.
- **frame_pacing Auto never locks below 30 fps.** Auto could choose every 3rd vblank, which is 20 fps on a 60 Hz display, in heavy scenes the game ran at 25-29 (a hub report: a 7800X3D at 20 fps in particle fights). Plain Auto now takes at most refresh/30 vblanks a frame (60 or 75 Hz: every 2nd; 120 Hz and up: every 3rd), and a frame that misses is shown a vblank later.
- **VC_SPARSE (default 16; 0 = the old rule).** VEMIT_STATS=1 (new) counts entries / indices / conversions per path: a third of the indices took the per-batch dedupe + convert path every frame, because the title's 256-index chunks span up to ~1700 vertices and the cache only took spans within n + n/4 + 16 of their distinct count. A span up to 16x its distinct vertices now goes to the cache (the unused vertices of the span converted too, once): hits 10.7k -> 13.3k entries a frame, vertex work 7.7 -> 6.2 ms (6 threads), 9.1 -> 7.1 ms (4 threads). Hits are still validated by content hash; frames identical. VC_SPAN=<n> (default 4096) caps the span.
- Measured and not taken: the content hash on a hit costs nothing measurable (an unsafe skip-the-hash probe: no change); a wider span cap: no change. VDEFER=1 (prototype, default off, NOT safe: it reads guest memory at the replay) moves vertex emission to the render thread: the walker's vertex time 7.0 -> 0.7 ms but only +2 fps at 6 threads, as the render thread and the guest's threads then compete for the same cores -- what helps a 6-thread CPU is less work, not moved work.

## 2026-11-12 — GL1: the SPU lighting and SSAO jobs run on the GPU (issue #19; gate +17% on 16 threads, +21% on 12, +37% on 6)
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001 | runtime/spu/spu_channels.c (spu_registry_replace, spu_dma_skip_set/clear), runtime/spu/spu_dma.h (skip windows), libs/video/rsx_d3d12_glight.{c,h} (new), libs/video/rsx_d3d12_backend.c, libs/video/cellGcmSys.c, libs/system/options.c (gpu_lighting row); src/if2_glight.cpp (new), src/main.cpp
- **Goal:** issue #19 (Ryzen 5 2600: 12-16 fps, CPU-bound). The gate's frame was set by the SPU lighting chain (label B -> 0x870, ~7.35 ms on five workers busy ~88%): about 30 ms of CPU a frame that the PS3 spent on its SPUs. Move it to the GPU, keep the picture.
- **What the chain is** (LJ_TRACE / LJ_DUMP research diag, private tree only): two job modules a frame, one after the other.
  - **SSAO**, jobmod_e6fe749031f8 (image 224): five plug-ins (images 160-164, lifted PIC at 0x20000): 0x87D500 depth -> half-res linear depth (even pixels, 1/(d24*a*2^-24+b)); 0x87F880 12-sample AO at 640x360 (kernel offsets scaled by 1/clamp(z,1289,2578), sample positions moved along a view-space normal from a 3x3 matrix, range falloff, sum * 1/(140.86*z/zc), byte = sat(1.2*v-0.2)*255); 0x87D880 / 0x87E480 5-tap bilateral blurs (x then y; weights 1, 0.6, 0.1 times max(0, 1-|dz|*30/z)); 0x87EC80 joint bilateral 2x upsample (9/3/3/1 times the depth weight with 40/z) into the full-res B8 at RSX local 0xCF800000. Parameters at guest 0x00A94980..0x00A94AB0.
  - **Tiled deferred lighting**, jobmod_c6e7383dc013 (image 218): 720 tiles of 32x40 shared through an atomic counter; per tile its 4 frustum planes and the depth box of its non-sky pixels, every light (48-byte records: position, half-float colour, type point/spot, inner/outer radius, cone, direction, near cut) culled against them, then per pixel: position from depth through the constant block's matrix, N from the normal bytes, gloss 2^(8A/255-0.889); diffuse max(N.L,0)*att^2 (att = sat((outer-d)/(outer-inner)), spots times the cone and near cut), specular (1+0.926g)/(1+g(1-N.H))^8; output packed in place over the G-buffer copies as sqrt-encoded RGB with a shared scale in A (0x47400B80) and specular in the high byte (0x47784B80).
  - Decoded with a symbolic SPU evaluator (scratch symspu.py: it unrolls a software-pipelined loop and prints each store as an expression of what was loaded), then C models checked against per-pass SPU dumps: lighting 98.5% of pixels byte-identical, AO passes 72-100% identical and every other pixel +-1 (the SPU's frest/fi estimates and round-toward-zero conversions).
- **Change (default on: PC Settings > Frame > CPU, "Lighting on the graphics card"; GLIGHT=0/1 sets it):**
  - SPU side (title, src/if2_glight.cpp): the lighting job's per-tile kernel (LS 0x4AC8) and the five SSAO plug-in entries are replaced through the registry (`spu_registry_replace`) by stubs; the jobs' tile counters, barriers and labels 0x870 / 0x880 / 0x890 stay the jobs' own. Their transfers into the G-buffer copies, the SSAO half-res planes and the AO buffer move nothing (`spu_dma_skip_set`, checked at the top of mfc_do_transfer_impl only while a window is set). The lighting stub snapshots the job's constants and light list from LS (tile 0, seqlock) and the SSAO parameters from guest memory.
  - GPU side (libs/video/rsx_d3d12_glight.c): at the prepass of the first draw that samples 0x47400B80 / 0x47784B80 / 0xCF800000 after a new snapshot, the G-buffer copies and the parameters go into an upload slot; the replay of that draw records one lighting dispatch (a 256-thread group per tile) and the five SSAO dispatches into one UAV buffer, copied into two RGBA8 textures and one R8, which every draw of the frame binds in place of the guest upload (the views the texture cache would make: RGBA8 / R8 with the crossbar). Such batches replay on the walker (RTW_PLANS). Shader build failure at device init puts the SPU jobs back (`unavailable`).
- **Verify (GLIGHT_VERIFY=1):** the SPU code runs, the GPU pass runs on the kernel's own input (copied per tile before the SPU writes), and each frame's outputs are compared ([GLIGHT-VERIFY]): gate lighting 98.5% px exact, >4 in ~50 px per channel; pier 99.7%; checkpoint 03 boot + polycut + docks 97.5-98%, one frame with one tile's scale byte different (a light right at a culling boundary); SSAO 53-62% exact, 30-40% +-1, >4 in 6-300 px. Final frames compared by eye on gate, pier and the checkpoint 03 boot: same picture.
- **Measured** (gate, save state, uncapped, `[if2-clock]` loops/s; 9800X3D): 16 threads 55 -> 64-65 fps, CPU per game frame 114 -> 83-103 ms; process affinity 12 threads 42 -> 51 (+21%), 6 threads 30 -> 40-41 (+37%). The SSAO move adds little on 16 threads (the frame is the walker / main thread there) and is the larger share of the freed SPU time.
- **Not default:** `GLIGHT_GBUF_GPU=1` captures the two G-buffer copies on the GPU (the walker's NV3089 copy -> the module's buffer, no readback, no CPU stores, no guest copy; cellGcmSys g_gcm_rt_capture). Same picture, but no fps difference measured at 6, 12 or 16 threads, so the readback path stays the default.
- **Known:** the frame clock calls present twice per guest flip above ~60 fps (both present paths in main.cpp's loop fire when a flip lands during the FIFO drain; the backend still presents once) -- CHECKPOINT_EXIT_AFTER counts those calls. Light lists over 256 lights are cut (the job's LS holds fewer).
## 2026-11-12 — CP1: core placement for 6-core and hybrid Intel CPUs (issues #3, #12)
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001 | runtime/syscalls/lv2_register.c, libs/system/options.c (the row's text)
Players on an i5-11400F (6C/12T, #3: ~25 fps) and a 12700K / 14700F (hybrid, #12 / #13) got no placement: auto
needed 8+ cores of one class sharing one cache, with SMT, and picked cores by CoreIndex (on a hybrid CPU, maybe E-cores).
- **Change.** The cores are the process's allowed logical processors (its affinity mask), grouped by core. "Fast"
  cores = the highest efficiency class (P-cores; every core of a uniform CPU), of those the ones on the last-level
  cache holding most of them. Placed threads take fast cores from the highest CoreIndex down; the process default
  (every other thread) = every allowed core not reserved (on a hybrid CPU: the P-cores left and the E-cores). On a
  hybrid CPU in mode 1-2 the PPU main thread is kept to the P-cores left (shared, never an E-core). Auto: 2 with
  6+ fast cores and SMT on one cache, or a hybrid CPU with 6+ P-cores; else off. Mode 3 now needs 7 fast cores (6
  left none for the rest). `HOST_PLACEMENT_SIM=<n>:<class>[x<smt>],...` prints auto's plan for a CPU this PC is not.
- **Harness** (scratch cp/): bench.ps1 = tools\run.ps1 -LoadState savestates\test\<state>.p3ss with FPS_LOG=1,
  CHECKPOINT_EXIT_AFTER=40s, the HDD install the states were made over, the process started with `start /affinity`
  (FFF = 6 cores / 12 threads of the 9800X3D, FF = 4 / 8); mean of the one-second [fps] samples from 3 s after the
  load (the first sample with over 3000 draws) to the last but one; campaign.ps1 interleaves arms.
- **Measured** (gate_fight, no input; street = fm_after_boat + left stick forward, right stick 150), fps mean (min):
  | CPUs | off | mode 1 | mode 2 |
  |---|---|---|---|
  | 6C/12T gate (3 rounds) | 35.2 | 38.0 | 39.1 |
  | 6C/12T gate (3 rounds, 2nd campaign) | 35.6 (34.0) | | 40.1 (37.0-38.0) |
  | 6C/12T street walk (3 rounds; off A/A 48.6) | 48.2 (46-47) | | 48.0 (46-47) |
  | 4C/8T gate (3 rounds) | 31.3 | 30.9 | |
  | 8C/16T gate (2 rounds, earlier campaign) | 41.4 | | 46.9 |
  Absolute levels moved ~10% between campaigns (the machine), so only arms within one campaign compare. Final exe,
  auto: 6C 41.7 vs off 39.1; 8C 47.1; 4C picks off (33.2). Hybrid CPUs are not measurable here: the decision was
  checked with HOST_PLACEMENT_SIM (12700K, 14700F, 13400, Core Ultra 9 285K, Ryzen 5 5600, Ryzen 7 5800X).

## 2026-11-12 — RL1: RecompLauncher's follow-ups (PR #7): a launcher without its window, real INI defaults, the toolkit download asked first, Recomp Achievements (off)
launcher (v0.1.8) | recomplauncher.json, tools/setup.ps1, tools/build.ps1, config/display.ini, config/input.ini,
config/savestates.ini, src/if2_achievements.cpp, src/infamous2_hooks.cpp, CMakeLists.txt, tests/test_if2_achievements.cpp,
README.md
- **Asked:** the four suggestions in PR #7 (RecompLauncher support): Easy Setup through a prepare mode, settings on the
  game's page through real INI defaults, a question before the first-start toolkit download, Recomp Achievements.
- **What the manifest can say** (recomplauncher.com/schema/recomplauncher.json, the same file as RecompLauncher's
  docs/recomplauncher.schema.json; docs/DEVELOPERS.md, docs/SETUP-INPUTS.md): `inputs` (named disc / update / dlc /
  game inputs, a file or a folder each, passed to programs as `{input:id}`; optional ones as `--x={input:x}`, left out
  when not given), `prepare` (a program run without a window after the inputs are picked; `progress <0..1> <text>`
  lines; exit 0 = done; `timeout` in s), `config` (ini / toml / json files, keys as Section/Key) with `settings`
  (choices, sliders, bindings, groups). Not there: firmware / BIOS inputs (SETUP-INPUTS.md rules them out on purpose)
  and any free-text notes field. So libsre stays manual, said in the prepare step's error and the README.
- **Without the window** (Headless in the launcher): `--prepare` (the files dialog's check and "Use these files" import,
  shared code: GameSetup.Import), `--build` (the stale steps through `tools\build.ps1 -Step`, the same state.ini
  fingerprints, root.* records and logs\launcher-*.log as the window), `--play` (run.ps1 as the window's Play; waits).
  `--game <folder>` finds the disc, the update, EBOOT.elf and libsre in a folder; `--disc/--update/--eboot/--libsre`
  win; libsre is also looked for in the RPCS3 folder the update sits in (`..\..\..\dev_flash\sys\external`), then an
  earlier game\firmware\libsre.prx. Lines: `PROGRESS <step> <n>/<total>`, `progress <f> <text>`, INFO / WARN /
  `ERROR <msg>` / `OK <what>` / `EXIT <code>`; exit codes 0-8 (README). A Windows-subsystem exe: a pipe it is given is
  used, otherwise it attaches to the parent console. The window and every old switch are as before (--detect-out of
  v0.1.7 and v0.1.8 byte-identical on a fresh and on a built folder; --report-out differs only in the version line).
- **The manifest:** inputs disc + update (folders) + eboot (optional .elf); prepare = `--prepare --disc {input:disc}
  --update {input:update} --eboot={input:eboot}`, 900 s; start = the launcher's window as before (it builds on the
  first Play and asks before the toolkit download). config = config\options.ini (the in-game PC Settings' file: it wins
  over display.ini / input.ini and an update never replaces it, while the shipped INIs are release files an update
  overwrites); 21 settings in 6 groups. The language rows are left out (their values differ per edition), and so is
  variable refresh (its menu change also sets vsync, tearing, fps cap and pacing, which a plain key write would not).
  Validated against the fetched schema with a small draft-07 checker (scratch validate.py; jsonschema is not installed
  and nothing was downloaded): 0 errors; it also passes RecompLauncher's three examples and catches a planted error set.
- **INI defaults:** display.ini 11, input.ini 73, savestates.ini 9 lines uncommented, comments kept (map.* examples and
  the "not yet" save-state lines stay comments). The store (libs/system/options.c resolve): a home-file key that parses
  is layer 2 (OPT_SRC_HOME) under options.ini (3) and the env (4); its value is the default, so every getter, opt_reset
  (back to home_val = default), the save (a value equal to the layers below goes back to its "# key = default" line)
  and the summary ("non-default" compares with the built-in default) behave as before. One visible difference: the PC
  Settings detail line "Value from" says "display.ini, line N" instead of "Built-in default". The home parsers replace
  a binding's keys (pad_kbm.h kbm_parse_line), so a written default binding is the same binding.
  A/B on one build (instA, display/input/savestates via DISPLAY_CONFIG / PAD_CONFIG / SAVESTATES_CONFIG, OPTIONS_CONFIG
  scratch, 40 s from boot): the [options] lines, the bindings table, hotkeys, mouse / keyboard / controller lines,
  [ss-slots] policy, [if2-aspect], language and trophy lines are identical; the one settings line that differs is
  "[display] settings from display.ini: (none)" -> the eleven keys listed (log text only). With a player's options.ini
  (aspect 21:9, hud 16:9, fmv fill, aa fast, mouse 1.5, rumble 50) both builds took all six from options.ini
  ("non-default: ... (options.ini:N)") and left the file byte-identical.
- **The toolkit question:** the window asks before step 1 when external\ps3recomp has no .git (ToolkitAskForm:
  what, ~15 MB / ~35 MB on disk, from where, why; Continue / Cancel); Cancel returns before anything is written.
  Continue passes IF2_TOOLKIT_DOWNLOAD=yes to that step. `--build` needs `--yes` (else EXIT 5, nothing changed).
  build.ps1 asks in its own console (setup.ps1's question would not show through its pipe), `-Yes` skips; setup.ps1
  run by hand asks the same; without a console and without consent both stop with exit 3. IF2_TOOLKIT_URL overrides
  the clone source (tests); the default stays GitHub.
- **Recomp Achievements (github.com/elripalda/RecompAchievements):** a C99 library (rl_achievements, with rcheevos and
  cJSON) that talks to RecompLauncher over a local pipe (RL_SESSION; one JSON object per line: hello, native, event,
  unlock, ...; docs/SPEC.md). Three sources: native (the game's own achievements; Xbox 360 today, names read by the
  launcher), memory (rcheevos rules, hosted lists), events. Licence: own terms (Recomp Achievements License 1.0): free
  for any game, launchers need written permission. The trophies: TROPHY.TRP (NPWR01730_00) has 52, ids 0-51: 1
  platinum (0, awarded by the PS3 itself), 2 gold, 9 silver, 40 bronze; 13, 18 and 19 hidden. The HLE
  (sceNpTrophy.c) really unlocks: it records gamedata/trophies/NPWR01730.json and logs "TROPHY UNLOCKED"; it never
  awards the platinum (no grade data); GetGameInfo returns placeholder counts. Built: src/if2_achievements.cpp, our own
  code speaking the documented channel (nothing vendored or downloaded): with options `achievements = 1` ([system];
  env IF2_ACHIEVEMENTS) and RL_SESSION set, sceNpTrophyRegisterContext / UnlockTrophy are wrapped (called exactly as
  the dispatcher calls them, r3 kept) and the bridge sends hello (console "ps3", game "infamous2-recomp"), the trophy
  file's unlocks as native/earlier at registration, and each accepted unlock as native. Off: no row action, no
  wrapper. Not done: save_loaded / casual (no save-load or save-state-load hook here; save states would need one for
  Ranked), the platinum. RecompLauncher has no "ps3" console or native list for this game yet: theirs to add.
- **Tests** (scratch installs made with `git archive` of the working tree; the disc linked read-only as the window does,
  a junction to an existing game\disc\PS3_GAME; update and libsre copied from another install's game\; the junctions removed
  with rmdir after; PLAY_ISSUES=0 and OPTIONS_CONFIG in scratch for every game start):
  - headless: --help 0; an unknown switch 1; --build before any files 2; --prepare with a wrong disc / update / no
    libsre 2 with one ERROR line each and nothing written; --prepare --disc/--update/--libsre= 0 (junction + copies,
    state.ini files.*); --prepare --game <folder> 0; libsre found in an RPCS3-style dev_flash beside the update 0;
    --build without --yes 5 (no file in the folder changed); --build --yes with IF2_TOOLKIT_URL = the local checkout:
    all 7 steps, 3:47, EXIT 0, 108 progress lines; v0.1.7's --detect-out then says every step is up to date; --build
    again: "up to date"; after a src change: only step 7 (1:34); --play started and waited for the game (6 runs).
  - the window: --detect-out of v0.1.7 and v0.1.8 identical (with --disc/--update/--libsre, with --pretend-missing, on
    a built folder); the question (UI Automation + BM_CLICK, no SendInput): Cancel left the folder's files, state.ini
    and logs exactly as they were and logged the decline; Continue cloned (from the local checkout) and went on to the
    next steps; closing mid-build asked and stopped. build.ps1 / setup.ps1 without a console and without -Yes: exit 3,
    nothing cloned; build.ps1 -Yes cloned.
  - INI A/B and options.ini: above.
  - tests/test_if2_achievements.cpp (host): off -> nothing replaced; on without RL_SESSION -> nothing; on -> hello,
    the trophy file's 44 and 7 as earlier (once), unlocks 12 and 33 as native, a refused unlock (0x8002290B kept in r3)
    sends nothing, every argument passed through; also with an unbuffered pipe whose server writes first (the first
    in-game run deadlocked on exactly that: the reader now starts before the hello). In the game with a fake launcher
    pipe: hello at boot, trophy 44 (seeded in gamedata\trophies) as earlier after RegisterContext, the trophy calls'
    log lines as with it off; options.ini achievements = 1 without RL_SESSION: one line, nothing replaced. No unlock
    was triggered in-game (44, the one an earlier test here unlocked, needs climbing a chain-link fence).
- **Open (the user decides):** start RecompLauncher's Play with our window (as now) or with `--build --yes --play`
  (no window; a first build of ~4+ min shows only RecompLauncher's spinner, and missing build tools would fail instead
  of being offered); the achievements bridge, which waits for RecompLauncher to add a PS3 / native list for this game
  (their side; their licence's launcher clause does not apply to a game).
## 2026-11-12 — EU1: European copies as a beta (issue #3): every title address found in the EBOOT by pattern; issues #4 and #5
EBOOT: BCUS98125 update 1.04 (and the disc EBOOT 1.00) | tools/if2_resolve.py, tools/if2_items.py, config/if2_sigs.json,
config/if2_contdb.json, tests/test_if2_resolve.py, tests/if2_resolved_test.h, src/if2_version.h and every title source,
tools/relift.sh, tools/ppu_postlift.py, tools/extract_policy_modules.py, tools/check_inputs.py, tools/run.ps1,
tools/build.ps1, CMakeLists.txt, launcher (v0.1.6), README.md, game/README.md
- **Asked:** issue #3's player has BCES01143 with update 1.04 (PARAM.SFO disc 01.01, app 01.00; decrypted "update" EBOOT
  SHA-1 4bfff469..., 10,159,776 bytes; libsre 41e3e15a..., another firmware) and failed at extract_policy_modules
  ("Wws_Job module: the bytes at 0x008E5B00 are not the known module"). Nobody here has that disc: make it a beta that
  works from the player's EBOOT itself and diagnoses from their log.
- **Finding first:** 10,159,776 bytes is exactly the US *disc* EBOOT's size (1.04's is 10,605,256), and the game picks
  its edition at run time from TITLE_ID (one EBOOT for every region). So the player's "update EBOOT" is very likely a
  disc-layout build (the EU disc EBOOT, decrypted from the update folder by mistake, or the disc's own); the step that
  failed looked for 1.04's module address because PARAM.SFO said 01.04. check_inputs now says which layout an EBOOT's
  code has, and a disc-layout EBOOT runs as the disc game (no update files mounted: run.ps1 / play mode follow the
  layout, not APP_VER).
- **Inventory (tools/if2_items.py, 152 items, each with the US 1.00 / 1.04 values as the cross-check and the features it
  gates):** the TOC; display W/H, safe X, render W and the four pool slots of W (aspect); the four LOD constants (lod) and
  1.04's dynamic-quality table, rows 12..20 (lod_quality); the clock object, loop counter and the 1/15 / 1/30 dt clamps
  (clocklog, dtclamp); SetNodeState, WarpToCheckpoint, the task graph, its index, the warp manager, warp lock, restart
  flag and the main loop's return address (taskjump); the signal-name table and the dispatcher (signals); the FMV quad
  function, its bl and callee (fmv_fit); the physics step, its fallback bl/callee, the mesh generator and its LR save
  (physfb); the language-list table and its US / EU lists (language_edition); the 25 WAIT_EVENTS addresses (5 sites,
  their callers, the Edge kicker's r2 displacement, row reads, the light kicker's poll table); the Wws_Job policy module;
  the native menu's 26 functions, 2 import stubs, 8 globals / vtables (menu, 1.04 only); 42 MENU_TRACE natives
  (diagnostics). Also keyed by US values before, now handled: KNOWN_EBOOTS gating (check_inputs, launcher), the
  ppu_postlift tables (built from the resolved items; on the US builds they must equal HOOKS / HOOKS_V104), the
  save-state continuation lists (config/ss_cont_list*.txt: kept for the US builds; another EBOOT gets both lists carried
  over function by function, analysis/ss_cont_list.resolved.txt), IF2_EBOOT_V104 (now the layout the resolver saw),
  the save-state title id / disc_ver and the checkpoint save directory (IF2_TITLE_ID). Not EBOOT-keyed, left as they
  are: the shader hashes (disc data), menu symbol ids (script data, checked at run time), libsre's SPU kernel LS
  addresses and ss_cont_list_libsre.txt (firmware: a player's other libsre may lose save-state resume points in libsre
  frames), the Bink image's continuation LS addresses (by image). The toolkit patch has no title addresses.
- **The resolver (tools/if2_resolve.py, standard library):** a pattern = up to 6 windows of 8 instructions around an
  anchor, normalised two ways -- loose (every load/store/addi displacement, lis/ori immediates, branch displacements
  masked) and tight (only what is address-like: r2-relative, and bases set by lis or loaded from r2 / another such base
  since the last blr) -- each window kept as its blake2b hash + its rarest normalised word (no game code in the repo;
  the Wws_Job module is found by the hash of its first 64 bytes + SHA-1). Windows vote for the anchor (tolerance 0x100);
  >= 2 must agree, a second place with as many is "ambiguous"; the anchor's own 5 words then confirm it (an insertion
  between window and anchor: searched within 8 instructions, never further). Values are decoded from THIS EBOOT: the
  address itself, the effective address of the access (its base traced back through the function: r2, a pool pointer
  loaded from the TOC, lis/addi/ori, mr; epilogue restores skipped; a piece find_functions split off a large frame's
  prologue counts as its function), the word it loads, a bl target, a table found by its strings (signal names), the
  import table (lwmutex stubs). Data items without a reader of their own: rel (1/30 = 1/15 + 8, stock value checked;
  the quality rows) or deref (the lang lists). Patterns are derived from both US builds (`derive`), validated by
  matching on the reference; functions too small to single out are found through their callers. Output:
  generated/if2_resolved.h (IF2_R/IF2_OK/IF2_F per item, IF2_HAVE per feature, the trace X-lists), analysis/resolve.json,
  analysis/resolve_report.txt (every item: found / ambiguous / missing, by which pattern and where). It runs in the lift
  step right after find_functions (the report lands in the launcher's log); on a US EBOOT any difference from
  if2_items stops the lift, and items a US build was never mapped for stay off there ("n/a": 1.00's code has most of
  the native menu's functions, found by the 1.04 patterns -- never tried, so off as before).
- **Results.** US 1.04: 152/152 found, every value the old constant (MENU_TRACE's SET_TEXT_COLOR(list) included, from
  a one-window pattern); US 1.00: the 64 items it has, all equal, 88 n/a. Cross-build honesty check (patterns of one
  build only, on the other): 1.04's on 1.00: 58 right, 0 wrong, 6 missing (lod_3, the mesh generator and its LR save --
  its frame grew --, the language table and its two lists: code that changed); 1.00's on 1.04: 57 right, 0 wrong,
  5 missing, 1 ambiguous. tests/test_if2_resolve.py on both: every pattern window unique; SHIFTED copy (padding at five
  function boundaries, 0x80..0x4000 bytes, data segment +0x20000, branches / descriptors / pointers / lis pairs
  rewritten): 150 of 152 items moved, all found at the new addresses (1.00: 63 of 64 moved, all found); CHANGED copy
  (two windows of every item get one instruction changed, 108 / 78 instructions): all found. The check-step probe on
  those copies: 61 of 64 core items (the 64 both builds have) without the lift's function list, 0 ambiguous.
- **Runtime safety:** every feature checks IF2_HAVE and, when off, says once `[if2] <feature>: off -- not found in this
  EBOOT (...): the game's own behaviour`; the one-line summary `[if2] EBOOT <name> (SHA-1, title, cross-checked | BETA);
  off on this EBOOT: ...` opens every log. The waitev sites keep their code-word checks (now relative to the found
  sites), aspect / LOD / language keep their stock-value checks. Test: a build with a header where nothing was found
  (every item 0, every feature off, title BCES01143) booted @new to READY by frame count (signals off), all eight
  features logged off, no FATAL / WATCHDOG. Title sources compile with no error and no warning against the 1.04, 1.00,
  nothing-found and UK headers (clang-cl -fsyntax-only, the build's flags).
- **Wws_Job module:** the lifter needs no exact bytes (lift_policy lifts what is there; the runtime keys a module by
  its first 16 bytes): the known module anywhere in the EBOOT (128-aligned, by hash + SHA-1), else where its
  registering code says (a pattern), accepted only when well-formed (aligned, 0x2E80 bytes, >= 97% SPU-shaped words)
  and reported as "not the known bytes". Without it, extract_policy_modules says the module is interpreted and goes on
  for a non-US EBOOT (the US ones still stop).
- **Editions:** check_inputs: BCES01143 / BCES01144 are beta; an EBOOT not known by SHA-1 passes only when the probe
  finds >= 75% of the core items with none ambiguous (prints the probe), and warns when an update folder's EBOOT has a
  disc layout; 4bfff469... is named "Europe, beta"; every other unknown EBOOT still stops (IF2_ALLOW_UNKNOWN_EBOOT=1 as
  before). With a European disc the language row is the European list and there is no Language edition row (UK: British
  English, hidden). Tried with scratch fake SFO / EBOOTs (the shifted copies under BCES01143 / BCES01144, a BCAS20181,
  a non-EBOOT, a 1.00-sized one): check_inputs ok / ok / stop / stop / ok + layout warning; the launcher's
  --detect-out [ok] / [!] / [x] the same.
- **Diagnostics:** the launcher's Save report writes logs\eu-report-<date>.txt (report-<date> for US copies;
  --report-out for tests): disc / update SFO, EBOOT path, size, SHA-1, libsre SHA-1, inputs.json (the probe included),
  resolve_report.txt, the steps' state, the newest launcher log's last 80 lines and the first game log's first 60 / last
  120 (UTF-16 read), with %USERPROFILE%, %USERNAME% and %COMPUTERNAME% in place of the user folder (also in / and
  JSON-escaped spellings), user name and machine name. Checked on a scratch install: 0 occurrences of the user name.
- **Issue #4:** the libsre firmware note is `[ok] libsre.prx: decrypted (firmware build differs from the tested one: this
  is fine, it does not stop the build)` in the launcher and check_inputs. A failed step's dialog and status line now say
  `Step N of 7 (id: title) failed: <its first error line>` -- from that step's own output only, [ok] / [i] / [!] lines
  never taken; the log says the same.
- **Issue #5 (with v0.1.5's build.ps1 reconfigure):** the launcher's file identity is relative to the folder, so a
  moved / copied install keeps its fingerprints; check (inputs.json's paths) and build (CMakeCache's home) run again
  when their output names another folder (Pipeline.MovedFrom, plus root.<step> in state.ini), without re-running the
  steps between. Tried: the repo copied to scratch A (built there: "the folder moved: configuring the build directory
  again", 2 min), --mark-built, A copied to `...\mv B\a deeper folder\inFamous Copy` (144 characters): --detect-out says
  check and build to run ("its output names another folder"), the rest up to date; check + build in B as the
  launcher runs them: "the folder moved: configuring the build directory again", CMakeCache's home = B, built in 2 min,
  and the exe booted @new to READY from B (VFS root and libsre from B). The scratch junctions were removed with rmdir.
  Not run: the launcher's GUI itself in B (its decisions were checked through --detect-out; --mark-built is a new
  test switch that records a build as the launcher would).
- **v0.1.4 vs this, US 1.04, default settings, @new to READY + 900 frames (PLAY_ISSUES=0, scratch options.ini):** the same
  [if2*] / [menu] / [checkpoint] / [options] / [cellSysutil] lines apart from frame-count noise (READY 2520 / 2525) and
  the new [if2] EBOOT line; 0 FATAL / WATCHDOG / null-read in both. ppu_postlift inserted the same 7 calls; the lift used
  the same ss_cont_list.104.txt.
- **Tests:** test_if2_resolve PASS on 1.04 and 1.00; test_if2_taskgraph passed; test_if2_checkpoint compiles, but its
  link fails with the same 10 undefined symbols at v0.1.4 (pi_signal, g_ftr_*, ...: it predates the play-issues code;
  not this change); test_sysutil_lang 83 checks 0 failed; test_savedata_flow passed; manifest gate 4082 statics,
  0 unclassified. No toolkit change: the patch is v0.1.5's.
- **What could still fail on the EU build, and how the report shows it:** a function the EU code changed (a feature
  `off`, its items `missing` with "best k/6 windows"); two look-alike places (`ambiguous` with both addresses); a
  1.00-layout EBOOT gets no menu entries or quality rows (1.04 only) and the check step's layout warning; the policy
  module not the known bytes (`[!] ... not the known one (SHA-1 ...)`); save states that park in code the continuation
  lists did not carry (the resolve report's continuation count); their libsre (another firmware) in save-state resume.

## 2026-11-12 — Language edition: Europe (opt-in, issue #1: Russian), and a non-US copy refused at the file check (issue #3)
EBOOT: BCUS98125 update 1.04 | src/infamous2_hooks.cpp (rows language_edition / language, the table write), config/ss_manifest.txt,
ps3recomp libs/system/tests/test_sysutil_lang.c, tools/check_inputs.py, tools/extract_policy_modules.py, tools/build.ps1, launcher, README.md
- **Asked:** after 2026-11-11 (a US PS3 set to Russian plays this disc in English), the user chose an opt-in way to the
  game's European languages on the US copy.
- **Where, as narrow as it goes:** the game picks its language in 0x9D9DC from the list of its edition in the table at
  0x930A00 ({count, list} per edition; disc EBOOT 0x8D09D0). That table has exactly one reader: 0x9D9DC, through the TOC
  slot 0x9A10F4 (every lifted function scanned for loads of it and of the table itself: one hit). So with
  `language_edition = europe` the port copies the table's Europe entry ({9, 0x744E54}: british, french, italian,
  german, spanish, dutch, polish, portuguese, russian) over the US entry ({4, 0x744E44}), once, after the EBOOT is
  loaded and before the game runs -- checked first, and nothing is written when the four words are not this EBOOT's.
  The edition word (+0 of the platform object, from TITLE_ID in 0x9DFF8), TITLE_ID itself and what is derived from
  them (+4 = 0x9D91C(edition), +0x50 = 0x9D828(edition) -> the per-edition strings at 0x744C52 "UP9000-BCUS98125_00",
  the content ids of DLC / PSN; the splash movie; save directory names from TITLE_ID; trophies through sceNpTrophy) are
  not touched. Seen in the Europe runs: the DLC checks still ask for UP9000-BCUS98125_00-..., the splash is still
  splash_america.bik, the saves are BCUS98125_INFAMOUS2_0, and apart from the language lines the logs match a US run.
  `us` (the default) writes nothing: the v0.1.3 path (a default run against a v0.1.3 one: the same packs and the same
  options / cellSysutil / cellGame / cellSaveData / file lines, apart from lines other threads cut into).
- **Rows:** `language_edition = us | europe` (System > Language, restart, OPT_F_ENHANCEMENT, env IF2_LANGUAGE_EDITION,
  alias edition; "US (original) / Europe (adds Russian, German, Italian, Polish, ...)"). It is registered first and
  picks the `language` row's list for the session: US auto | english_us | french | spanish | portuguese (as before);
  Europe auto | english_uk | french | italian | german | spanish | dutch | polish | portuguese | russian. English (UK)
  is the system language 18, which the game reads as none: the list's first entry, british_text with the English
  voices -- what a UK PS3 gets. A saved language the edition does not offer is not one of the row's values: the store
  logs "options.ini:N: language = russian is not a value of this setting: ignored" and the row is Auto. Auto with
  Europe: Russian Windows -> russian, en-US -> english_uk (the sibling), pt-BR -> portuguese, Turkish / Japanese ->
  english_us, which the game pairs with british. Turkish is not in the European list (its own edition, BCES01229, has
  it alone, and system Turkish counts as none), nor are Japanese, Korean, Chinese: not offered.
- **In game (checkpoint 03 boat polycut, then the pause menu and PC Settings > System; scratch options.ini, caches, states;
  PLAY_ISSUES=0):** every European language boots with its packs: russian / russian_text, english / british_text,
  french, italian, german, spanish (the European pack, not latino), dutch, polish, portuguese (not brazilian), each with
  its _text from the update; one GetSystemParamInt(LANG) per run. Frame dumps: the polycut's subtitles and the pause
  menu in each language in the game's font, Cyrillic (СПОСОБНОСТИ, ПАРАМЕТРЫ ...; "По-твоему, было бы "проще" сжечь кучу
  людей заживо?!") and Polish (ę, ż, ł) included, no missing-glyph boxes; PC Settings shows LANGUAGE EDITION * EUROPE
  and LANGUAGE * RUSSIAN. Russian voice: russian.psarc (8656 /dialogue/*.ogg) is the only voice pack the run opens.
  Env IF2_LANGUAGE_EDITION=europe with Auto on an en-US Windows: english_uk -> british_text.
- **Saves both ways:** a save the game wrote in a Russian / Europe session (BCUS98125_INFAMOUS2_0, its DETAIL in
  Russian) boots in a US session (English), a US-session save boots in a Europe one (German); a save state made in a
  Europe / Russian session loads in a US session, one made in a US session loads in a Europe / Polish one (the loaded
  moment keeps its language: guest memory; the table write is guest memory too and harmless there -- 0x9D9DC runs once,
  at boot).
- **Tests:** test_sysutil_lang 83 checks (11 new: auto against the European list); manifest gate 4082 statics, 0
  unclassified (g_lang_europe BOOT).
- **Issue #3 (a European disc got to step 6 and failed in extract_policy_modules: "Wws_Job module: the bytes at
  0x008E5B00 are not the known module"):** check_inputs.py warned only, for a title id other than BCUS98125 and for an
  unknown EBOOT SHA-1. Both are errors now, naming the edition found (the game's own SKU list: BCES01143 Europe,
  BCES01144 UK, BCES01229 Turkey, BCAS20181 Asia, BCJS30075 Japan, the PSN demos), saying that only BCUS98125 with
  update 1.04 (or its 1.00 disc EBOOT) is supported and pointing to Language edition: Europe for the European
  languages; IF2_ALLOW_UNKNOWN_EBOOT=1 turns them back into warnings (development). The launcher's game-files check
  says the same before any step ([x], or [!] with the variable; also in --detect-out with --disc / --update).
  extract_policy_modules.py ends a failure with what it means (not a BCUS98125 1.00 / 1.04 EBOOT, US only) and the jobs
  step's error points at those lines. Tried: a copy of the 1.04 EBOOT with one byte flipped and a PARAM.SFO saying
  BCES01143 (both in scratch): check_inputs exit 1 with the message, exit 0 with the override; the launcher's check
  [x] / [!] the same; extract_policy_modules on the disc EBOOT posing as 1.04 prints the user's line and then the new
  explanation, exit 1; the real US files still pass, analysis/inputs.json byte-identical.

## 2026-11-11 — Issue #2: the build and the game from a folder with spaces (and a deep or non-ASCII folder)
CMakeLists.txt, tools/build.ps1, tools/run.ps1, src/stubs.cpp, ps3recomp libs/system/cellGame.c, launcher, README.md
- **Reported:** "Compile the game" failed with `RC1107: invalid usage` when the folder had a space. Cause: the target's
  string defines with paths (`IF2_PRX_DIR="C:/Users/First Last/..."`, `IF2_DEFAULT_EBOOT`) went to rc.exe too
  (app.rc), which cannot take them. Fix: both are `$<$<COMPILE_LANGUAGE:C,CXX>:...>`; app.rc gets IF2_EBOOT_V104,
  IF2_HAVE_LIBSRE, IF2_HAVE_ICON only, and the icon still builds.
- **Tested for real:** a `git archive` of the working tree in `...\space test\Földer Name\inFamous2Recomp`
  (149 characters), game files set up as the launcher's Import does (a junction `game\disc\PS3_GAME` to the disc,
  read only; copies of the update folder and libsre.prx), the launcher built there with tools\build_launcher.cmd,
  then every step the way the launcher runs it (`powershell -File tools\build.ps1 -Step <id>`, cwd = the folder):
  setup (clone from GitHub + apply), check, analyze, lift, libsre, jobs, build -- all exit 0 -- and the game through
  tools\run.ps1. Three more problems turned up and are fixed:
  - the game's content root (cellGame, host /dev_hdd0/game) was built in a 128-byte buffer (the guest's
    CELL_GAME_PATH_MAX): a deep folder's HDD install went to a folder named after the first 127 bytes
    (`...\space test\Földe`) and the game spun at boot. Host-side buffers are CELL_GAME_HOST_PATH_MAX (1024) now;
    guest paths keep 128. (Not save-state data: those statics are KEEP / BOOT.)
  - libsre was looked up at the build's absolute path, which CMake writes in UTF-8 while fopen reads the ANSI code
    page: with "ö" in the folder it was not found and the game ran without libsre (cellSpurs HLE: no gameplay).
    stubs.cpp now tries `generated/prx` below the working directory first (run.ps1, the launcher and play mode all
    run from the project folder), the baked path last.
  - run.ps1 (and build.ps1) wrote paths into an ASCII batch file: "ö" became "?" (shown with a test batch: the
    old form does not find the file, the new one does). Paths now reach the batch files as environment variables.
  After the fixes, in that folder: libsre loaded, the content root whole, the first-boot HDD install done, READY
  (checkpoint 03, Spanish, subtitles in the frame dumps).
- **Guards:** the launcher checks its folder at startup (before anything builds a path below it) and in
  `--detect-out`: letters the ANSI code page cannot spell (the runtime opens files through it -- e.g. a Cyrillic
  folder on an English Windows) and a path over 150 characters (the build's deepest file is ~102 characters below
  the folder, build\ps3recomp_sdk\CMakeFiles\...\atrac3plusdsp.cpp.obj, against Windows' 260) get a warning with a
  suggested folder (`<drive>\Games\inFamous2Recomp`) and a Continue / quit choice. Tried: the spaced folder "ok", a
  Cyrillic one and a 159-character one warned. README: a note on the folder. Not handled: a folder so deep that the
  launcher exe itself is near 260 characters -- .NET fails to start it ("Fatal error") before any code runs.
- Not run: the launcher's update installer from such a folder (it needs a newer release); read through: every path
  it passes is quoted, and it works through .NET file APIs.

## 2026-11-11 — Language: the PS3 system language as a setting (issue #1); the US edition plays in four languages, Russian is not one of them
EBOOT: BCUS98125 update 1.04 | ps3recomp cellSysutil.{c,h} (+ libs/system/tests/test_sysutil_lang.c), src/infamous2_hooks.cpp
(row `language`), config/ss_manifest.txt, README.md
- **Asked:** issue #1 "add in game rus" (Russian text and voice). The disc has every language pack
  (`USRDIR\cache\psarc\<lang>.psarc`, the voices: 8656 `/dialogue/*.ogg` each; `<lang>_text.psarc` on the disc and,
  newer, in the 1.04 update's `patch\psarc`: `/subtitle/*.bsub` and the UI text), and a PS3 picks the language from
  its system language. Our cellSysutilGetSystemParamInt hard-coded LANG = English (US).
- **How the game picks (1.04, read in the lifted code):** 0x9E7C8 (one call, at boot, from 0x1EFB1C) reads TITLE_ID
  (cellGameGetParamString 100), matches it against the game's SKU list (0x9DFF8; the strings at 0x744B02: BCUS98125,
  BCES01143, BCES01144, BCES01229, BCAS20181, BCJS30075, demos...) -> its edition (+0), then reads the system
  parameters LANG (-> +8), ENTER_BUTTON_ASSIGN, DATE / TIME_FORMAT and 0x121 in one loop, then 0x9D9DC picks the
  language (+0xC): a system language over 16 (Portuguese (Brazil) 17, English (UK) 18, Turkish 19) counts as none;
  it is looked up in the edition's list (0x930A00 + 8 x edition: {count, list}), equal or one of four pairs (0x7449E0:
  british~english, latino~spanish, brazilian~portuguese, chinese~chinese_s); no match = the list's first entry.
  The names (0x931244, game index = PS3 index for 0..16, then 17 british, 18 latino, 19 brazilian, 20 turkish) give
  `<name>_text.psarc` and, unless 0x1B0DF8 says the language has no voices (korean, chinese, british), `<name>.psarc`
  (0x9DDC4; a flag at +0x954 of another object can force English voices).
  Edition lists: US (BCUS98125) english, french, latino, brazilian; Europe (BCES01143) british, french, italian,
  german, spanish, dutch, polish, portuguese, russian; BCES01144 british; BCES01229 turkish; Asia english, korean,
  chinese; Japan japanese, english (and a Europe list with turkish for one special case).
  **So a US PS3 set to Russian runs this disc in English**, and so does every other language outside the four.
- **Built:** the HLE answers LANG with `cellSysutil_system_language()`: a settings-store row `language` (registered by
  the title; no row = English (US), as before), resolved once at the first query and logged
  (`[cellSysutil] system language: spanish (language = spanish, <file>:2)`, `GetSystemParamInt(LANG) = 3 (spanish)`).
  Toolkit API: `cellSysutil_lang_key` / `_from_key` (the PS3 languages 0..19; there is no 20+ on a PS3),
  `_from_windows` (a LANGID -> PS3 language: en-GB/AU/NZ/IE/ZA/IN -> English (UK), pt-BR -> Portuguese (Brazil), zh-TW/HK/MO
  -> Chinese (T), ...), `_lang_auto` (that language if the row offers it, else its sibling -- Portuguese PT/BR, English
  US/UK, Chinese T/S --, else English (US)). The inFamous 2 row (System > Language, OPT_RESTART, env IF2_LANGUAGE,
  alias system_language / lang): auto (default) | english_us | french | spanish | portuguese, labelled Auto (Windows
  language), English, French, Spanish (Latin America), Portuguese (Brazil) -- the four system languages that change
  something in this edition (Portuguese (Brazil) as a system language gives English here: the > 16 rule; Auto maps a
  pt-BR Windows to Portuguese, which gives the Brazilian pack). Restart: the game reads it once (one call site; one
  `GetSystemParamInt(LANG)` line per run), so the row says "takes effect at the next launch"; no live switching.
- Not language-dependent here: GetSystemParamString (the game asks NICKNAME / CURRENT_USERNAME only), PARAM.SFO
  (no TITLE_xx keys: one title in every language), the enter button (a separate PS3 setting: unchanged, Cross), the
  save data (the game writes its own titles in the current language; loading does not depend on them -- below). The
  story FMVs carry 19 audio tracks and the Bink player is the game's own lifted code: it picks the track.
- In game (checkpoint 03 boat polycut, scratch options.ini / caches / save states, PLAY_ISSUES=0):
  - each offered language, a fresh boot with `language = X` in a scratch options.ini (runs m_*, p_*): english_us ->
    english.psarc + english_text.psarc; french -> french + french_text; spanish -> latino + latino_text; portuguese ->
    brazilian + brazilian_text; one `GetSystemParamInt(LANG)` per run. Frame dumps: the boat polycut's subtitles
    ("Zeke, déplace ce bateau au moindre problème.", "Zeke, mueve el bote ante cualquier problema.", "Zeke, faz o barco
    andar no primeiro sinal de problema.") and the pause menu (POUVOIRS / PODERES / PODERES ... with our PC SETTINGS and
    QUIT GAME rows in English) in the game's font, accents right, no missing glyphs. The voices: the only voice pack
    opened is the chosen one (8656 dialogue .ogg files; english.psarc is not opened), and the subtitle timing follows
    each language's lines.
  - the system languages this edition does not offer, before the list was cut (runs ru1, ru_new, p_*): russian (with the
    checkpoint save and with no save), japanese, portuguese_br and english_uk all open english.psarc + english_text.psarc
    and show English subtitles -- the game's own fallback, so they are not offered.
  - PC Settings > System shows "LANGUAGE *" (4th row); Right on it: Spanish (Latin America) -> Portuguese (Brazil), the
    footer "Changed: takes effect at the next launch.", `[options] language = portuguese (was spanish)`, the scratch file
    `language = portuguese`; the next launch with that file: brazilian packs (c_change2, c_next). Auto with an empty
    file: "auto (default): Windows UI language 0x0409 = english_us" (c_auto). IF2_LANGUAGE=french: locked, French
    (c_env). `language = russian` in the file: "not a value of this setting: ignored".
  - Saves: the checkpoint save (written in English) boots and loads in French, Spanish and Portuguese; a save the game
    wrote in a French session ("(Sauvegarde automatique) 3. Élan en avant") boots in Spanish and in English
    (d_frsave_*). Save states: one made in a French session loads in an English session and in a French one (and across
    a rebuild, with the usual warning); the loaded moment keeps its French text and voice pack (the language lives in
    guest memory and the open files), as expected -- the next launch is English again.
- Tests: test_sysutil_lang (new, 72 checks + 4 child processes: keys, LANGIDs, auto against this title's list and
  others, the row: none / env / a value not offered / auto), test_options_store 366, test_ovl_menu 1371, test_ss_libs
  PASS (it links cellSysutil.c); manifest gate 4081 statics, 0 unclassified (s_sys_lang BOOT).
- **Russian (issue #1) -- not done, a decision:** the data works: an experiment (scratch only, not kept) set the
  edition word to Europe at the LANG query, and Russian booted with russian.psarc + russian_text.psarc and Cyrillic
  subtitles in the game's font ("Зик, чуть что - сразу уводи лодку."). Offering it means telling the game it is the
  European edition -- not what a US PS3 does -- so it is left out.

## 2026-11-03 — S2: the lighting job's generated SPU code, line by line; no exact codegen lever reaches 3% -- the one systematic waste (zero-lane Newton steps: ~11 M exact-fallback calls a second) is worth ~1-2%, and removing it on the fast path cost 4-5% (measured, nothing kept)
EBOOT: BCUS98125 update 1.04 | no source change kept (probe builds -- line tables for the job TUs, window / fallback
counters -- and four lever variants were made in local work files and reverted; build\infamous2.exe is the committed
6364fd24); docs/NOTES.md
- **Goal:** S1 left the gate's SPU lighting chain (~7.35 ms of ~19 ms) as ~70% lifted job code (jobmod_c6e7383dc013
  ~60%, ed956f3d8d84 ~23%). Profile that code at instruction level, remove avoidable costs in the code generator
  (exactly: bit-identical SPU results, unchanged channel / DMA order), keep a change only at >= ~3% at the gate.
- **Harness** (local work files): S1's sampler (HOST_SAMPLE_RAW) plus CodeView line tables for the generated/spu_jobs TUs
  (-gline-tables-only, a PDB in scratch): every sample maps to a line of the generated C, i.e. one SPU instruction
  (linehot.py); counters force-included into the job TUs (/FI): window attempts and failures per site, float
  fallbacks per call site (return address -> line; cnt.py); relift.sh lifts spu_jobs / spu_jobchain into scratch (an
  unmodified lifter reproduces both trees byte for byte); fpsab.sh with a rebuild-of-base arm (rb) besides the
  committed exe, 3-4 interleaved rounds.
- **Where c6e7's time goes (gate):** 55% plain statements, 37% window fast copies, 8% write-through statements; by
  inlined helper: own code 25%, spu_fi 14%, spu_fm 6%, the window check 5%, fcgt / normalization / result test ~4%
  each, LS loads 3%. The hot loops are tight AVX-512: no register-file traffic per instruction, one vpshufb per LS
  access, no dispatch in the loops, a 3-load back-edge check; their cost is the guest work's own latency chains. Region
  0x7170 (loop 0x7300: rsqrt Newton steps, y = fi(x, frsqest(x)), x*y) is 20% of c6e7 and runs on its statements: its
  window fails on zero lanes and, after an invocation's 4 attempts, every iteration makes four spu_fm_slow calls
  (~96 M each in 35 s; 404 M fm fallbacks in c6e7, 115 M at one fnms by il(-1) in d0d15ec83992, ~4 M per site in
  ed956's divide). The SPU has no infinity: frest / frsqest of 0 is an exponent-255 number, times the zero lane +0 on
  the SPU, inf * 0 = NaN on the host.
- **Tried (all exact):**
  | variant | what | gate | street | pan |
  |---|---|---|---|---|
  | z3 | spu_<op>_z: lanes with an exponent-0 multiplicand masked to a zero product on the fast path and in the window forms; the lifter's reaching-definitions pass puts them where a multiplicand may hold an estimate / exponent-255 immediate (330 sites in c6e7) | -5.2 / -3.4% (A/A 1.8%); -3.6% vs rb | -5.5 / -4.4%; -4.0% vs rb | -2.7 / -2.4% |
  | z1 | z3 with a register-level site rule (716 sites in c6e7) | 5% below z3 | 5% below z3 | |
  | z4 | the masked form only as an inline second chance after the plain fast path rejects (windows plain) | +0.4%; +1.3% vs rb | -1.3%; -0.9% vs rb | |
  | fl | -O3 -mtune=znver5 on every lifted SPU TU | +0.5%; +0.1% vs rb | -0.8%; +0.5% vs rb | |
  z3 cut c6e7's fm fallbacks 404 M -> 17 M and let the 0x7300 window run (attempts 24 M -> 160 M, 0.7% failing), but
  the masks cost on every hot multiply of jobs and plug-ins that never see a zero lane. Exactness of the forms:
  a new unit test (32 M checks, four MXCSR modes, AVX2 / AVX-512, with and without SPU_HELPERS_FP_RZDAZ) found no
  mismatch in the lifted code's mode; local s2_review.diff keeps z3 and the test for a later attempt. A first
  2-round A/B had shown z1 +6% at the gate: its base arm ran at 45 fps against 52-53 in every later round (a disturbed
  arm) -- gate A/Bs need the rebuild-of-base arm and 3+ rounds.
- **Not built:** SPU-only PGO; an LS image in host word order (drops one 1-cycle vpshufb per LS access, <= ~3% of
  c6e7); a cheaper window check (a latency the predictor already hides).
- **Next lever:** not the generated code of these programs -- it is near the cost of the guest work. Less SPU work per
  frame or more of it in parallel (the walker's segments around the chain remain the larger levers, R1 / S1).
## 2026-11-03 — S1: where the gate's SPU lighting chain goes; no exact SPU-side lever reaches 3% -- the chain is the lifted job code (~70% of the workers' time), the MFC emulation's removable bookkeeping is worth at most +1.0-1.5% at the gate (measured as an upper bound); the fifth worker's "kernel time" is an accounting split, not work (measured, nothing kept)
EBOOT: BCUS98125 update 1.04 | no source change kept (a probe build -- S1_PROBE counters, a fixed HOST_SAMPLE unwinder
with a raw per-sample dump, the parks' wake latency -- and an upper-bound build, S1_UB, were made in scratch and
reverted; build\infamous2.exe is the committed R1 exe, 6364fd24); docs/NOTES.md
- **Goal:** PF7 / R1 left the gate fight set by the SPU lighting chain (label B -> 0x870, ~7.35 ms; five workers ~88%
  busy; the walker waits at its acquire). Measure where the chain's time goes (dispatch, the lifted code, DMA / MFC,
  lock-line reservations, waits, the fifth worker's ~35% kernel time), build the largest exact lever behind a switch,
  keep it only at >= ~3% at the gate.
- **Harness** (local work files: r.sh / rgn.sh / fpsab.sh from r1 with per-arm env, prof.sh = r.sh + threadcpu.ps1,
  s1p.py ([S1P] rows), hsraw.py / hscat.py / hsocc.py (raw samples: per thread, by leaf category, the five workers'
  occupancy per sampler round split by whether the walker is waiting at a label); every run its own scratch shader
  cache, exe copies at their own paths).
- **The sampler works again.** PF7's HOST_SAMPLE_ALL died 4 s in at the gate, and it died again here ~1 s in (exit
  0xC0000005): host_unwind guessed a return address at [rsp] for any frame without an unwind entry and then unwound
  from it, reading through garbage rsp / rbp. The probe's fix (local p_probe.py + p_probe3.py): the walk stays
  inside the sampled thread's stack (its TEB's DeallocationStack..StackBase), the [rsp] guess only for the innermost
  frame, the walk under SEH; plus HOST_SAMPLE_RAW (a line per sample: the leaf as module+offset, 16 exe frames) and
  HOST_SAMPLE_BUSY (sample only threads that ran that many Mcycles in the last snapshot). 20 s at the gate and the
  street without a fault, ~2-3% fps cost.
- **Per worker per frame at the gate** (S1_PROBE: rdtsc brackets on the MFC command paths, channel waits, wakes and
  the lock-line lock; ~18.9 ms frames, each worker ~17 ms of CPU):
  | path | count a frame | each | ms a frame |
  |---|---|---|---|
  | GETLLAR / PUTLLC ok / PUTLLC fail / PUTLLUC | 2.2k / 1.5k / 110 / 180 | 37 / 95 / 210 / 65 ns | 0.08 / 0.14 / 0.02 / 0.01 |
  | DMA GET / list / PUT (3.0 MB a frame) | 18.9k / 1.7k / 1.0k | 20 / 420 / 100 ns | 0.36 / 0.72 / 0.10 |
  | lock-line lock, contended | 340 (0 yields) | 180 ns | 0.06 |
  | parked (channel waits, LR parks) | 40 | 50 us | 2.0 (idle) |
  | wakes issued | 500 | 55 ns | 0.03 |
  The parks (S1W, all workers, per 5 s): ~51.5k, half shorter than 2 us, 12.8k of 10-50 us, ~3.2k over 200 us; the
  wake latency (the spu_ch_wake that made the channel ready -> the waiter running) 2.8 us mean, 98% under 10 us:
  ~0.1 ms a worker a frame -- not a lever.
- **The lifted job code is the chain** (clean sampler runs, S1_PROBE off, 20 s, ~6900 rounds at the gate). All workers,
  whole run: guest code 70.5% (job modules 56.4, plug-ins 11.5, policy modules 2.5), the SPU runtime 13.5% + memcpy
  2.4%, parked 12.5%, exception / unwind machinery (spu_irq_regs_maybe_restore's longjmps) 0.6%. Street: guest 61.5%,
  runtime 14.4% + 2.8%, parked 19.8%. In the rounds where the walker waits at a label (933 of 6860: ~2.6 ms a frame)
  -- the lighting chain: 5 workers busy in 86.6% of rounds, 1-2 busy in 9.7% (a serial tail, jobmod_cd7019d4687a,
  94% its own code); the workers' samples: guest 69.5% (jobmod_c6e7383dc013 ~60% and jobmod_ed956f3d8d84 ~23%
  inclusive -- the lighting), runtime 16.1% + memcpy 2.7%, parked 10.8%. The runtime's share, largest first:
  mfc_do_transfer_impl 2.1, spu_fm_slow 2.1 (the lighting's x * frsqest(x) on zero lanes: exponent-255 lanes take
  the vector fallback), gcm_note_spu_put 1.6 (every PUT walks the 16 IO mappings for the pushbuffer finder),
  spu_overlay_note_get 1.2, spu_lockline_lock 0.9, spu_drain_call 0.8, spu_wrch_impl 0.8, mfc_for 0.7 (a linear
  owner scan on every MFC channel write), spu_wrch 0.7, wev_poke 0.7, spu_indirect_branch 0.6. jobmod_ed956f3d8d84 is
  the DMA-heavy one (many small GETs: ~28% of its time in the channel-write / transfer path); c6e7383dc013 15%.
  The lifted code is already the helper-level optimum this toolkit has (x86-64-v4, RZDAZ, register-cached, inlined
  SIMD float fast paths): no exact codegen change was in reach for this milestone.
- **Upper bound for the MFC path's bookkeeping** (`S1_UB=1`, measurement only, not exact as a whole; scratch
  s1/p_ub.py, exe 5c623d52 in s1/bin_ub1): mfc_for served from a per-thread cache (generation-checked), no
  pushbuffer-block bookkeeping in gcm_note_spu_put (its label wake kept), no overlay note for GETs under 64 bytes, no
  POISON-DMA scan, no repeated-transfer guard. Same exe, 3 interleaved rounds (READY+4..40 s, warm, each arm its own
  path and cache):
  | scene | S1_UB=0 / again (A/A) | S1_UB=1 / again | gain |
  |---|---|---|---|
  | gate fight | 52.96 / 53.19 (0.4%) | 53.57 / 53.73 | +0.7..+1.5% |
  | busy street | 66.64 / 65.92 (1.1%) | 67.47 / 67.35 | +1.1..+2.4% |
  Any exact version of it can only take less: below the bar, not built. The other runtime items are smaller still
  (spu_fm_slow's vector fallback is 13-17 ticks already; the lock-line lock ~0.9% of the chain; the wake latency
  ~0.6% of the workers' time).
- **The fifth worker's kernel time is an accounting split, not work.** One SPU worker per run shows 9-32% kernel
  time in GetThreadTimes / threadcpu (0x2004 in 6 of 7 runs, once 0x2001 -- placed on core 5, with 0x2004 at 1%).
  That worker's thread cycles (QueryThreadCycleTime, 83.4-83.6 G over 18 s against 84-85.6 G), its total CPU time,
  its work (GETLLAR / PUTLLC / DMA counts within ~5% of its peers'), its context switches (1089 vs 842-931 a second)
  and its sampled profile (13.4% outside the exe vs 12.5-13.7%, the same waits, no extra syscall) all match the
  other workers', so the "kernel" ms replace user ms in the split rather than adding work. VM_WRITE_WATCH=0 keeps it
  (9-16%), so it is not the texture cache's write-watch faults; the CPUs' DPC / ISR time is ~6% and only on CPU 0.
  Every thread on CPUs 0-5 (the shared set) shows it too (PPU main 35-41%, 0x2005 28-42%, the render thread 24-32%).
  It costs nothing measurable; PF7's placement experiment (no gain with the fifth on a core of its own) agrees.
- **Not built:** an exact MFC fast path (bounded above at +1.0-1.5% at the gate); lock-line lock striping (~0.9% of
  the chain); a spin-before-park (the wake latency is 2.8 us); the fifth worker's placement (PF4 / PF7). Verification
  of a kept change does not apply; the reverted sources are byte-identical to their pre-S1 copies, build\infamous2.exe
  is the committed 6364fd24 (a rebuild from the reverted sources differs only in main.cpp's build stamp), manifest
  4080 statics 0 unclassified, a gate run on it clean.
- **Next lever:** not the SPU runtime. The gate is ~70% lifted lighting job code on five workers: a faster chain needs
  faster generated SPU code (e.g. the lifter emitting a DMA's channel sequence as one runtime call -- ed956f3d8d84's
  ~28% -- or cheaper exponent-255 handling where frsqest meets zero lanes), each worth low single digits. The walker
  segments around the chain (~9 + ~3.4 ms) and the street's walker remain the larger levers (R1's next: the prepass's
  host-only part, ~1.8 ms on the street). The probe's sampler fix (local p_probe.py, p_probe3.py) is worth
  landing on its own with the next diagnostic milestone.
## 2026-11-02 — R1: the replay of a batch on a render thread -- the walker hands each batch it does not wait for to a second thread after its prepass (the same D3D12 call stream as before; final exe: gate +1.6-2.5%, busy street +1.2-2.0%, pier pan +0.9-1.0%, earlier builds up to +3.9% on the street; the walker's render_frame 4.9-5.0 -> 3.3 ms a frame on the street, which now waits ~1 ms a frame for the guest)
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video/rsx_d3d12_backend.c (RENDER_THREAD: render_frame_impl ends with
rf_handoff; rf_replay = the old replay through move_to_next_frame, reading the batch from s_rpc; s_d3d.draws one of
s_draws_buf[2]; drains at the walker's entry points; the graveyards under a lock; ssc_vsb; RT_VERIFY; RT_STREAM_LOG's
hooks; TEX_FENCE_RSX_COPY in tex_fence_capture), rsx_rthread.h (new: the thread and its hand-off), rsx_rt_oracle.h + rsx_rt_oracle_gen.h (new: the command-stream
oracle, the second generated from the SDK's vtable), tests/test_rthread.c (new); config/ss_manifest.txt
- **Goal:** PF7's largest lever -- take the replay's D3D12 recording off the walker (the frame clock) without moving any
  read of guest memory or anything the guest can observe; a locked 75 fps on a 2560x1080 56-75 Hz test display.
- **Measured first** (local work files: a diagnostic build -- R1_PROBE, render_frame cut into sections and every
  command-list call timed through a pass-through proxy generated from the SDK's vtable, r1/gen/gen_probe.py; 30 s from
  the scenes' states, rows from READY+4 s; the probe's own brackets add ~0.5 ms to the replay loop):
  | scene | frame | render_frame | prepass (texture validation + uploads, PSO lookup) | replay loop | list driver time | descriptor calls | Close + Execute | Present | batches (readback / report / capacity / present) |
  |---|---|---|---|---|---|---|---|---|---|
  | busy street | 16.82 | 6.30 | 3.37 (1.12, 0.42) | 2.23 | 0.72 | 0.09 | 0.16 | 0.12 | 1 / 0 / 3 / 1 |
  | gate fight | 20.87 | 7.19 | 4.02 (1.37, 0.44) | 2.24 | 0.73 | 0.07 | 0.20 | 0.14 | 1 / 1 / 3 / 1 (empty) |
  | pier pan | 14.26 | 3.73 | 1.95 (0.84, 0.22) | 1.20 | 0.42 | 0.03 | 0.12 | 0.12 | 1 / 0 / 1.2 / 1 |
  | pier still | 16.50 | 6.13 | 3.18 (0.97, 0.42) | 2.22 | 0.75 | 0.09 | 0.17 | 0.13 | 1 / 0 / 3 / 1 |
  - NVIDIA's driver records for almost nothing: net of the bracket RSSetViewports 0.4 ns a call, root CBV 1.0,
    descriptor table 2.4, DrawIndexedInstanced 17.5, SetPipelineState 21.9, OMSetRenderTargets ~190, a barrier ~500,
    a clear 0.8-1.3 us, CopyTextureRegion 2.2 us, Reset 4.3 us, Close 4.9 us, ExecuteCommandLists ~27 us: ~71k list
    calls cost 0.72 ms a frame. Deferring only the driver calls (a serialised call stream) would have netted ~0.5 ms
    (3%); the replay's own bookkeeping (target / zeta binds, transitions, sampler sets, raster replay, PF2 copies,
    ZPASS queries) is the larger part, and it reads no guest memory either.
  - (a) stays on the walker: the prepass (3.2-4.0 ms: every guest read -- validation, hashing, upload staging, TX1's
    fence copies, PF6's plan binds -- and the RT / zeta / PSO / VS caches the record path shares), vertex conversion,
    the readback's and the report flush's GPU waits. (b) moves: the replay from the frame barrier to
    move_to_next_frame -- ~2.1 ms a frame on the street, of which the readback flush's share (14% of the records,
    the walker waits for its GPU result right after) stays synchronous. Estimated: street ~1.7 ms, gate ~0.7 ms on its
    critical path, pan ~1.0 ms -- over the 3% bar: built.
- **Design** (local design.md; the shared state from a static analysis of the replay's call closure against the
  record path's and the window procedure's, r1/gen/cshare.py):
  - render_frame_impl keeps its walker part (the GPU wait, collects, graveyards, the list's Reset, the atlas, DREDIR,
    the RT / zeta pre-pass, the texture / PSO prepass with the upload copies it records) and ends with rf_handoff: the
    packet (s_rpc: the records, their count, frame index, flush / present / reason, parity, vbatch, frame_count, the
    clear colour, the legacy MVP, the pipelines' readiness, cellGcmSys's flipped display, the thumbnail request), the
    walker's bookkeeping (the batch's accounting, the guest-memory peeks, the record buffers' swap, parity, vbatch,
    the dedupe reset, frame_count, the title bar / [fps], PF6's plans_replayed), then rf_replay -- the old replay
    through move_to_next_frame -- on the render thread or on the walker. Every replay input the walker can change
    before its next drain comes from s_rpc; the replay's helpers that read those globals (ssc_update, rt_selfsample,
    rtsnap_census, ssc_pso, zdbg_want, rs_frame_end, gpu_time_mark, pfx_frame, rs_window_present, rsx_capture_frame)
    read s_rpc in both modes.
  - Asynchronous (on the thread) only for a batch the walker does not wait for: a present, a capacity flush, a TX1
    fence split. On the walker as before: RT readback / report-measure / save-state flushes; a record bound to a PF6
    composed texture (the compose reads the walker's plans); legacy records or no VP pipeline yet; a pending
    RES_SCALE switch or the census; texture packs; PERF, PASSLOG, a thumbnail grab, GPUP_PROBE, the overlay's
    backbuffer fallback and the replay-side diagnostics (RTT_*, RSX_CAP, GCM_GUEST_FB, SCREENCOPY_DUMP, D3D12_IQ, ...).
  - One batch in flight. The walker drains (rsx_rthread.h: a short spin, then the done event -- auto-reset, set after
    the job's release, so a late one is one spurious wake; past 50 ms with the job still in flight the drain also
    dispatches SENT messages only, MsgWaitForMultipleObjectsEx + PeekMessage PM_QS_SENDMESSAGE, so a Present that
    SendMessages to the walker's window cannot deadlock it, while no posted message -- input, hotkeys, menu commands,
    the modal tick that drives the frame clock -- is dispatched inside a drain) at render_frame's
    start, at every GPU wait (wait_for_gpu: the render thread's list is on the queue before the fence), at the RT
    readback, the report measure (the replayed list's ZPASS queries are resolved before they are waited for), the
    save-state quiesce / reset / GPUP capture / restore, WM_CLOSE (before the window goes) and the shutdown (the
    thread is stopped and joined; no batch starts it again). So the
    command list, the queue, the fence, the swap chain and the replay's state (the targets' resource states, zetas,
    PF2's copies, the sampler sets, the ZPASS list, GPU_TIME, post-process state, the dumps) are the walker's again
    before it touches them; the TX1 fence walk reads only the targets' identity fields, which only the prepass
    writes. The records: s_d3d.draws is one of two arrays, swapped at the hand-off while the thread is on; the
    replay reads only its own. PF6's plans are marked replayed by the hand-off only (before the kick, or after a
    replay on the walker) -- never by the render thread, which would race the walker planning the next batch.
  - Lifetimes are the existing ones: every graveyard drains at render_frame's start after the drain and the GPU wait;
    both threads bury now (one lock), and a VS-cache eviction's blob waits there too (the PF2 coverage redraw's VS is
    resolved in the prepass, ssc_vsb, for a draw with a guest pipeline and no user clip planes -- the only ones
    rtsnap_wrote asks for -- so the replay never reads the VS cache; RENDER_THREAD=0 looks it up at the replay as
    before). A depth clear's zeta on an offscreen target with no surface is sized in the prepass at the replay's
    host size (RES_SCALE >= 2), so no zeta grows during a replay. Upload memory (the texture
    ring, the vertex / index / constant halves, the vertex cache's aged slabs) is written for a batch only after the
    walker's drain and GPU wait, as before.
  - Present, ExecuteCommandLists and move_to_next_frame run on the render thread; FRAME_TRACE's present times reach
    the walker's row through the next drain (its counters are the walker's: the render thread adds none);
    opt_render_point (live settings) after an asynchronous present runs at the next render_frame's start, after its
    drain -- between two batches, never at a drain mid-batch (a fence, a report measure, a readback, a save state).
    The overlay's backbuffer request is read at the hand-off (s_rpc.ovl_bb).
  - (The walker no longer drains at a TX1 fence: it did not close the fence gap below; TEX_FENCE_RSX_COPY does.)
  - `RENDER_THREAD=0`: no thread; every batch replays on the walker through the same code (the stream oracle below
    compares it with the pre-R1 exe). `RT_VERIFY=1`: the records hashed at the hand-off and after the replay, the
    replay's state hashed at its end and again at the walker's drain ([RTVERIFY]); [RT] every 600 frames (batches
    on the thread / on the walker by reason, the walker's waits and pumped drains, the thread's replay time; written
    while the thread is idle, its counts diffed, never reset under it).
- **Exactness:**
  - Stream oracle (`RT_STREAM_LOG=<file>[,<n>]`, new, diagnostic): the frame command list through a hashing proxy
    (generated from the SDK's vtable, r1/gen/gen_hash.py) -- every call and its arguments, the replay's and the
    prepass's descriptor writes by the recording thread -- a hash per batch for the first n batches after the state
    load. Objects are named by a number kept in their own private data (an address reused by a new object is a new
    name), descriptor handles and GPU addresses by first sighting; the batch's vertex parity and frame index are
    logged, since which half / backbuffer the load meets differs run to run, and only runs that start alike are
    compared. The pre-R1 source with only this oracle is the reference exe (local bin_ref). 4 rounds x 3 runs
    (reference, RENDER_THREAD=0, =1) per scene, every comparable pair (r1/slrun.sh, slsum.py): the leading identical
    batches -- pier: reference vs reference 11, vs RT=0 11-16, vs RT=1 11-16; gate 15 / 13-19 / 13-15 (the window
    includes the readback and the report flush with the sun-glow composes); street 11 / 11-14 / 11-16 -- the same
    windows as the A/A: the streams agree batch for batch until the game's own timing parts the runs (the
    batches' call counts differ from there on).
  - The existing oracles at 0 on the first stage-1 exe (f635b3ca; r1/ver.sh; 7 scenes -- pier still, pier pan, gate fight, busy street,
    2x render scale pan, AA fast + sharpen 25 pan, a cold boot of checkpoint 03 with the boat polycut and the walk):
    PF3_VERIFY 50.8M raster records / 51.0M sampler-set draws 0 bytes differing, 4.9M clean binds 0 stale, 6.5M binds
    before their TX1 fence 0 differing; REPORT_GPU_VERIFY (gate) 31,220 rows / 31,200 values 0 differing;
    TEX_PLACED_VERIFY 46,957 textures, 155,440 subresources (72 GB of rows) 0 differing; VCACHE_VERIFY 23.0M served draws (52 GB)
    0 bytes differing, 1.48M region writes 0 unsafe; RT_VERIFY 24,179 batches on the thread, records changed under the replay 0,
    the replay's state changed before the drain 0. No D3D12 error, removal, FATAL or WATCHDOG. A second cold boot
    (the review's rerun) then found one: PF3_VERIFY's fence check, 1 bind of 1.84M before its fence differing --
    texture 0x47784B80 (1280x720), one of the two NV3089 copy targets SPU jobs also rewrite in place, in the 3D title
    scene before the boat polycut ("RSX copies after the fence 0": not the walker's own copy). TX1 left those two
    out of the fence's copies (over the 256 KB cap, RSX-kept: "read at the replay", 0 in its runs); they are read at
    the prepass, at its FIFO point, and an SPU write after the reference that lands before that prepass reaches the
    draws before the fence. A drain wait at render_frame's start can widen that time, so the walker now drains at
    the fence instead -- it did not remove the hits (the GPU wait at render_frame's start, later now that the thread
    submits, can widen it too). Cold boots after it: 7 of 8 with 0, one with 1 bind (2.40M compared, the same texture); the
    committed exe 0 in 3 (and TX1's own runs). So the gap is TX1's, its exposure is higher with the thread (2 hits in
    10 cold boots against 0 in 3 + TX1's), only before gameplay.
  - **Fixed (`TEX_FENCE_RSX_COPY`, default 1; acts only while the render thread is on, so RENDER_THREAD=0 is
    unchanged; `=0` = the read at the prepass):** a copy target over the cap that the fence's walk finds dirty and
    sampled by a pending record is copied at the fence like the smaller slots (tex_fence_copy; the prepass
    validates and uploads from the copy). On the street that is the two 1280x720 targets every frame, ~7 MB:
    the fence's walk + copies 33 -> 148 us a frame. Measured (r1/fpsab.sh fc1, 4 interleaved rounds; old =
    0be4eb2b, new / new2 = the fixed exe 833cac0f, nc = it with TEX_FENCE_RSX_COPY=0): gate 51.18 / 52.34 /
    52.79 / nc 52.61 (A/A 0.9%), street 65.25 / 66.65 / 66.30 / nc 66.46 (A/A 0.5%) -- the copy's cost is under
    the A/A on both; against the committed exe gate +2.3% / +3.2%, street +2.1% / +1.6%. Cold boots with
    PF3_VERIFY on the fixed exe: 10 of 10 with 0 differing (1.57-1.65M binds before their fence each), RT_VERIFY
    0; the committed exe 3 of 3 with 0 (2.35-2.37M). Gameplay scenes on it (street, pan, gate): PF3_VERIFY,
    REPORT_GPU_VERIFY, TEX_PLACED_VERIFY, VCACHE_VERIFY, RT_VERIFY all 0. Manifest gate 4080 statics, 0
    unclassified (rt_fence_rsx_copy.en BOOT).
  - Re-verified on the final exe (a61defe1; final.sh): six scenes (still, pan, gate, street, 2x scale, AA + sharpen)
    PF3_VERIFY 43.5M raster records / 43.7M sampler-set draws 0 differing, 4.1M clean binds 0 stale, 6.6M binds
    before their fence 0 differing; REPORT_GPU_VERIFY 31,220 rows 0; TEX_PLACED_VERIFY 36,385 textures / 122,377
    subresources 0; VCACHE_VERIFY 18.4M draws (42 GB) 0, 1.37M region writes 0 unsafe; RT_VERIFY 51,773 batches 0 /
    0 (with the four cold boots); no D3D12 error, FATAL or WATCHDOG.
  - CELLMARK identity (r1/idall.sh = st1/idall.sh's: every arm its own exe copy; base = 0be4eb2b x2, new x2, new with
    RENDER_THREAD=0; on f635b3ca and again on the final exe; compared by content, r1/idcontent.py -- ss1/idpost.py's log alignment slipped twice on a dump
    line cut by another thread's output): post-load frames 0-3 byte-identical in every arm on the gate and the pier
    (first differing 4 / 4 / 4 / 4); the street 3-4 in the first pass (new2 at 3: one pixel, delta 31) and in two
    more street passes base vs base 2 and 4, new 2-4, RENDER_THREAD=0 3 -- the street's known early drift. Final
    exe: gate and street 0-3 identical in every arm, the pier 0-2 in every arm including base vs base.
- **Measured** (r1/fpsab.sh, 4 interleaved rounds, warm, each arm its own exe path and cache copy, READY+4..40 s;
  old = 0be4eb2b, new / new2 = the candidate (A/A), rt0 = it with RENDER_THREAD=0). First campaign, exe f635b3ca:
  | scene | old | new | new2 | rt0 | present p95 old / new | walker: render_frame / idle (waiting for the guest) old -> new |
  |---|---|---|---|---|---|---|
  | gate fight | 51.72 | 52.94 (+2.4%) | 53.05 (+2.6%) | 50.72 | 20.02 / 19.48 | 5.51 / 1.88 -> 4.33 / 2.29 ms |
  | busy street | 64.10 | 66.59 (+3.9%) | 66.21 (+3.3%) | 64.31 | 17.69 / 15.86 | 5.02 / 0.09 -> 3.35 / 1.03 ms |
  | pier pan | 72.50 | 73.11 (+0.8%) | 73.30 (+1.1%) | 72.22 | 16.40 / 15.57 | 2.92 / 1.85 -> 2.14 / 2.03 ms |
  A/A 0.2% / 0.6% / 0.3%. Final exe a61defe1 (the fence drain, 4 rounds again): gate 51.89 / 52.77 / 52.94 / 51.03
  (+1.7% / +2.0%, A/A 0.3%), street 64.81 / 65.31 / 67.15 / 63.83 (+0.8% / +3.6%; new's round 2 ran at 61.20 with
  5.16 batches a frame -- without it new is 66.68, +2.9%), pan 72.45 / 73.29 / 73.08 / 72.15 (+1.2% / +0.9%);
  present p95 street 16.11 -> 15.82 (new2), pan 16.43 -> 15.45. The walker's render_frame fell by the predicted 1.2-1.7 ms, but the frame fell by less: on
  the street the walker now waits ~1.0-1.2 ms a frame for the guest's commands (it was ~0.1) -- the PPU's side of
  the reference / flip chain is the clock in about half the time; at the gate the SPU lighting chain was already
  ~half of the frame. Moving the walker's wait to the fence cost nothing measurable. The thread replays 2.4-5.3 ms a frame (more than the walker saved: the wall time of its own
  Present and fence waits); the walker waited for it 0.00-0.08 ms a frame. RENDER_THREAD=0 is the old path's speed
  only roughly: first campaign street +0.3%, pan -0.4%, gate -1.9%; final street -1.5%, pan -0.4%, gate -1.7% --
  the kill switch was ~1-2% slower than the committed exe at the gate. After the review (exe 985dc730: no buffer
  swap and the VS lookup at the replay with the thread off, among the fixes below; r1/fpsab.sh fb2, 4 rounds):
  | scene | old 0be4eb2b | new | new2 | rt0 | A/A |
  |---|---|---|---|---|---|
  | gate fight | 51.63 | 52.91 (+2.5%) | 52.43 (+1.6%) | 51.53 (-0.2%) | 0.9% |
  | pier pan | 72.29 | 73.02 (+1.0%) | 72.96 (+0.9%) | 72.29 (0.0%) | 0.1% |
  | busy street | 64.86 | 65.66 (+1.2%) | 66.18 (+2.0%) | 64.71 (-0.2%) | 0.8% |
  RENDER_THREAD=0 is now within the A/A of the committed exe everywhere; no frame over 25 ms in any measured run.
- **Review fixes (2026-11-03, exe 985dc730):** PF6's plans_replayed off the render thread (it wrote rsx_report.c's
  plan_oldest while the walker planned the next batch; "[REPORT-GPU] BUG" never printed in any run before or after,
  0 of 561 logs); the TX1-fence drain removed; opt_render_point only at render_frame's start; the drain's sent-message
  pump past 50 ms (0 drains reached it in 138 runs) and WM_CLOSE's drain; the done event auto-reset (a late SetEvent
  can no longer leave it signalled for the next job); [RT]'s counts diffed, not reset under the thread; the overlay's
  request in the packet; no restart after rt_stop; the kill switch's buffer swap and VS lookup as before; ssc_vsb only
  where PF2 asks; the prepass sizes a surface-less target's depth-clear zeta at the replay's host size. Not done:
  gating TEX_FENCE_RSX_COPY on the batch going async -- that is decided after the prepass, the copy must be at the
  fence, and the copy (not the drain order) is what closed the gap; its cost is under the A/A. Re-verified on
  985dc730 (r1/final2.sh): 10 of 10 cold boots 0 at the fence; the six scenes' oracles 0; CELLMARK frames 0-3
  identical in every arm on gate, street and pier (as base vs base); save / load 2 / 2, 5 / 5 walking, 5 / 5
  mid-pan, both launch loads; the stream oracle's windows as before; alltests rc 0; manifest 0 unclassified.
- **Hitch census** (on f635b3ca; r1/campaign.sh = st1/campaign.sh: span / tour / gate / boot, 2 interleaved rounds, a fresh exe
  path per run pair (c = cold driver cache, w = warm), old = 0be4eb2b, new / new2 = the final exe): no new hitches -- frames over 25 / 33 ms a run, old / new / new2 (c, w): span 0 / 0 / 0.5, 0 / 0 / 0.5; tour 1 / 1 / 1 both; gate 0.5 / 0 / 2.5, 0 / 2 / 0; the boot's load 251 / 248.5 / 248 and 249.5 / 249 / 248 (the frontend's paced frames), its polycut 1.5 / 1 / 1.5 and 0.5 / 0.5 / 1, the walk after it 0 everywhere. Longest frame (mean of 2, worst): span c 21.8 / 21.2 / 26.3 (22.1 / 21.9 / 31.1), w 21.8 / 21.4 / 26.3 (22.1 / 21.5 / 32.1); tour c 29.5 / 28.1 / 28.9, w 28.5 / 29.3 / 28.6; gate c 24.5 / 22.2 / 26.6, w 22.9 / 26.3 / 21.7; the world-load frame unchanged (265-268 ms). Frames over 20 ms at the gate 163 -> 48-55 (c), 114 -> 34-58 (w); p99 lower or equal everywhere. Two shapes seen in the new arms only, in 2 of 8 span runs and 3 of 8 gate runs: the span's walk-to-pan frame at ~10.2 s at 31-32 ms, the walker waiting 12 ms for the guest's commands (old: that frame 21-22 ms); and gate frames of 25-28.5 ms where every walker block is ~1.5-2 ms slower with no wait (CPU-side; not the render thread's CPU sets -- it runs on the process's default cores -- nor its priority: RT_PRIO=0 / 2 measured within 0.5%).
- **Save / load** (r1/ssio.sh, the pf5/rev/reverify.sh step 6 pattern, scratch cache): the pier's F5 / F9 / menu save
  + load (2 / 2), five F5 / F9 cycles walking the street (5 / 5), five F5 / F9 cycles mid-pan (5 / 5, a second between
  them), that quick slot loaded at launch with RENDER_THREAD=0 and at the defaults; no SPU fault, D3D12 error, FATAL
  or WATCHDOG; load pauses 200-228 ms. The same again on the final exe (r1/ssio.sh fin2): all saved and loaded.
- **Tests:** tests/test_rthread.c (new; m4c/gcm/test_gcm_ss.sh runs it): the backend's pattern -- a packet and one of
  two record buffers written, a kick, the producer writing the other buffer while the job hashes its own twice, extra
  drains, drains with nothing in flight, a stop and a new start -- 200,000 rounds: 0 result mismatches, 0 overlapping
  jobs, 0 stale packets, 0 buffers changed under a job; planted faults caught (a drain that does not wait: 65,904
  mismatches; the producer writing the job's buffer: 199,532 stale packets, 31 changed under a job). A window test:
  a job SendMessages to the producer's window -- answered inside the drain on the producer's thread, no posted
  message taken; the old wait-only drain deadlocks there (its watchdog fails it). alltests rc 0
  in every group on the final exe (unit's "strict-pass share" line is the known non-gating print).
  Manifest gate 4079 statics, 0 unclassified (render_frame_impl's function-local statics that moved keep their
  class under rf_replay / rf_handoff; the new ones: the record buffers, the packet, the thread, the graveyards' lock,
  the VS graveyard KEEP, the stream oracle DIAG).
- **Risks to review first:** (0) the fence copies of the two copy targets (closed the gap above; ~7 MB a frame on
  the walker before the reference, measured under the A/A); (1) a walker path that reads the replay's state without a drain -- the entry points
  were found by analysis, not by construction (RT_VERIFY's state hash covers the targets, zetas, sampler sets,
  s_bb_keep, the fence; not PF2's copies or the post-process state); (2) the deferred opt_render_point -- a live
  setting after an asynchronous present applies at the next render_frame's start (after the guest recorded part of
  the next batch), not right after the Present; (2b) the drain's sent-message pump: a sent message's handler runs
  nested in the walker's drain (never seen: 0 pumped drains); (3) FRAME_TRACE rows: the present times arrive one drain later and the render thread's GPU waits are not counted
  (the walker's are); (4) PF6 composes, legacy records and the diagnostics replay on the walker -- correct, but
  silent: [RT]'s reasons show it.
- **Next:** the walker now waits for the guest on the street: the PPU's reference / flip chain (PF7's chain) and the
  gate's SPU lighting set the frame; on the walker, the prepass's host-only part (descriptor writes and PSO lookups,
  ~1.8 ms on the street, with the VS / PSO caches split from the record path) is the next movable block; PF6's
  composes on the thread (hand the plans' rows over at the hand-off).
## 2026-10-28 (night) — ST1: the hitch frames when new content comes into view -- textures placed in heaps made ahead, pipelines made at launch from a persistent list, vertex programs keyed by their own code, the shader-list prewarm off the walker (exact; the street pan's longest frame 35-43 -> 22 ms, the gate's 33 -> 23-25 ms, the cold polycut's 47 -> 26 ms, the world-load frame 650-1000 -> 265 ms)
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video/rsx_d3d12_backend.c (TEX_PLACED tph_*, TEX_UPCHUNK, VS_KEY_PROGRAM
vs_key_*, PSO_PREWARM's walker side, SHADER_PREWARM_THREAD sl_thread / sl_one / s_dec_lk, FRAME_TRACE columns),
rsx_pso_list.h (new: the pipeline list, its launch prewarm, PSO_PREWARM_VERIFY), rsx_tex_heap.h (new: the heaps' range
allocator), rsx_shader_async.h (sc_mem: DXBC held in memory), tests/test_tex_heap.c (new); config/ss_manifest.txt,
docs/NOTES.md
- **Goal:** remove the frames that run long when new content comes into view (PF7: 26-35 ms frames where the street
  walk turns into a pan) without changing anything the game computes or shows: measure first, then fix the largest
  causes exactly, each behind a kill switch.
- **Harness** (local work files): r.sh -- routes span (walk 10 s, then pan), street (walk turning), tour (60 s: three
  walks, three fast pans), gate (the fight), pier, boot (a cold boot of checkpoint 03: title, world load, the boat
  polycut, the walk; READY on PlayPolycut), all timed in ms after READY (below), FRAME_TRACE on; tools/run_exe.ps1 runs
  an exe copy at a new path (a cold driver cache); census.py (every frame over 1.5x the median or over 20 ms: each
  FRAME_TRACE component's excess over its own median, largest first, "unattributed" the rest), campaign.sh + csum.py,
  fpsab.sh + fps.py, ver.sh (oracles), idall.sh + idpost2.py + artset.py (identity without swapping build\
  infamous2.exe), ssio.sh (reverify step 6), final.sh, mkvlog.sh. Every run used its own scratch copy of
  build\shadercache (st1/sc_base); the player's cache was never written.
- **Measured first** (a diagnostic build = d14f02fb + FRAME_TRACE columns tex_create / upbuf / rt_create / pagefaults;
  "c" = the first run of an exe copy at a new path, "w" = its second / third; spikes = frames over 1.5x the median):
  | route | cond | >20 / >25 / >33 ms | spikes (ms over the median) | p99 / p99.9 ms | longest ms |
  |---|---|---|---|---|---|
  | span (40 s) | c / w | 17 / 5 / 1 -- 19 / 3 / 1 | 12 (147) -- 14 (136) | 19.0 / 27.3 -- 19.3 / 24.8 | 42.4 -- 33.4 |
  | street (40 s) | c / w | 0 / 0 / 0 -- 3 / 0.5 / 0 | 0 -- 1 (9) | 18.4 / 19.1 -- 18.4 / 20.4 | 19.8 -- 25.5 |
  | gate (40 s) | c / w | 492 / 2 / 0 -- 486 / 2 / 0.5 | 1 (13) -- 1 (13) | 22.3 / 24.7 -- 21.9 / 24.5 | 32.9 -- 33.0 |
  | boat polycut (37 s) | c / w / all cold | 10 / 6 / 3 -- 3 / 1.5 / 1 -- 21 / 20 / 19 | 10 (170) -- 3 (46) -- 21 (2229) | 15.1 / 30.3 -- 14.7 / 19.9 -- 14.6 / 244 | 50.9 -- 38.0 -- 285 |
  | load frame | FAST BOOT load w / c; world load w / c / all cold | 612-691 / 1051 ms; 627-638 / 1007 / 4506 ms |
  Causes of the spikes' excess: warm span / street / gate (317 ms over 6 runs) -- the walker's replay 26%, vertex
  emission 23% (both mostly the pan's turns back to the 9k-draw city view: 20-22 ms frames of throughput, PF7), texture
  creation 18%, unattributed 10%, pipeline creation 5%, the guest 4%, texture validation 4%, DXBC disk reads 4%; cold
  (160 ms over 3 runs) -- pipeline creation 39%, texture creation 20%, replay 13%, vertex emission 12%; the boat
  polycut warm (92 ms) -- GPU waits 32%, pipeline creation 21%, the guest 18%, texture creation 13%; cold (170 ms) --
  pipeline creation 83%; everything cold (2229 ms) -- D3DCompile 89% (waits for a worker's compile 60%, compiles on the
  walker 29%). The gate's ~490 frames over 20 ms are its 19.5 ms median (PF7's throughput), not new content.
  - **Texture creation:** a committed texture cost ~0.23 ms to create whatever its size (23 new 5.3 ms, 35 new 7.4 ms,
    86 in the polycut's camera cut 18-19 ms); their bytes' decode and upload ~0.5 ms.
  - **Pipelines:** 0.3-0.7 ms each when the driver's disk cache has them, 3-9 ms when not (span: 5 in one frame
    14 ms; the polycut: 16 in 19 ms; the load frame: 160-177 in 420-437 ms). The NVIDIA cache keys an application by
    its exe's path: a copy at a new path was cold, the same copy touched (new timestamp, same bytes) stayed warm.
  - **The gate's pipeline storm:** every 10-30 s a 33 ms frame (+13 ms): 118 vertex programs "new" and 140 pipelines
    missed at once. The per-draw VS cache keyed a program by the FNV of its start to the end of the loaded microcode
    store, so loading a program into the store re-keyed every program below it.
  - **DXBC disk reads:** 0.05-0.15 ms each, two reads of 8 and 12 ms (one file each) in the census.
  - **The load frame:** ~670 new textures (creation 140-165 ms), ~300 upload buffers of their own when the 16 MB ring
    overflowed (115-170 ms), pipelines, the decode of 83 MB (120-180 ms).
  - **Everything cold** (an empty DXBC cache: a first launch on a new machine): D3DCompile -- 260-350 ms frames in the
    polycut, a 4.5 s world-load frame. Not exactly removable (the DXBC is derived from game data; nothing ships).
  - **Two harness effects, removed:** n1/rgn.sh read the whole UTF-16 log (iconv + greps) every 15 s; frames of 20-27
    ms with no new content appeared ~15 s apart in its runs and not in st1/rgn.sh's (no live read). CHECKPOINT_SCRIPT
    frames are present_guest_frame calls, which count some flips twice at a rate set by the walker loop's timing
    (if2_clock.cpp): with the prewarm off the walker the span's pan began at 5.4 s instead of 10.4 s (IF2_CLOCKLOG:
    pres = 2 x flips against 1 x; loops = flips in both) -- every ST1 route is timed in ms (`<a>-<b>ms:` items).
- **Fixes** (each default on; `=0` = the old path exactly):
  - `TEX_PLACED`: a guest texture up to 8 MB is a placed resource in a 32 MB default heap (ALLOW_ONLY_NON_RT_DS_
    TEXTURES; rsx_tex_heap.h: first fit over each heap's offset-sorted free ranges, coalesced on free; the 4 KB small
    alignment when the driver grants it, else 64 KB), activated with an aliasing barrier before its copy; a helper
    thread keeps two heaps made ahead. The resource has the committed one's description, every subresource is written by
    the upload's copy before any draw samples it (the rule a reused slot's resource already depended on: the heap's
    old bytes are never read), and a block is freed only in a prepass for a slot not bound in the batch, after
    render_frame's GPU wait. Creation 0.23 -> ~0.01 ms a texture (44 new in one frame: 0.4 ms).
  - `PSO_PREWARM`: every guest pipeline the walker makes is described field by field (rsx_pso_list.h PslDesc: the
    whole D3D12_GRAPHICS_PIPELINE_STATE_DESC, the shaders named by their DXBC cache files and pinned by size + FNV-1a 64,
    the depth-bounds flag) and appended to <cache dir>\psolist.bin; at device init the newest 6144 distinct records are
    made on two below-normal threads (their DXBC kept in an in-memory map sc_compile reads before the disk) into a
    table (warm: ~250 pipelines in 0.06-0.10 s). On a miss of its own cache the walker builds the description as
    before and takes the table's pipeline only for the same record (memcmp) and the same VS and PS bytecode (memcmp);
    else it creates as before and the pipeline enters the table and the list. A pipeline made earlier but outside the
    window is appended again (recency); a list with many repeats is compacted at launch.
  - `VS_KEY_PROGRAM`: the VS cache keys a program by its own code to its END instruction (the decompiler reads nothing
    past it: both of its scans stop there and the D3D12 path sets no branch base); no END: the old key.
  - `TEX_UPCHUNK`: textures that overflow the upload ring share 32 MB upload chunks, not a buffer each.
  - `SHADER_PREWARM_THREAD`: the shader list's prewarm (HLSL regeneration, missing compiles queued) runs on a
    below-normal thread instead of ~1 ms slices of the walker's idle time; every decompile (walker and thread) holds one
    SRW lock (s_dec_lk: the decompilers keep process-wide state); the walker starts it at its first idle call (the
    raster and clip settings set first). The thread finishes the 14,900 records in a few seconds.
- **Exactness:**
  - Oracles (st1/ver.sh on the final exe; gate / tour / boot, each after a seed run that wrote the pipeline list):
    `TEX_PLACED_VERIFY=1` (every upload into a placed texture copied back -- the texture and the uploaded bytes by GPU
    copies into one readback buffer, compared after the batch): 29,910 textures, 92,660 subresources, 45.9 GB of rows,
    0 differing. `VS_KEY_VERIFY=1` (a program found under its own key with another store tail decompiled again, each
    pair once): 577 checked, 0 different HLSL. `PSO_PREWARM_VERIFY=1` (every table hit's description rebuilt from the
    record and compared with the walker's own byte for byte, input elements by text): 594 hits, 0 different. Its second
    check -- the pipeline created twice more and NVIDIA's driver blobs compared -- found 85% byte-identical and the rest
    differing from byte 832 on, at the same rate as two of the walker's own creations of one description (the
    control): the blob carries a per-creation part, so it decides nothing either way.
  - Identity (st1/idall.sh: d14f02fb x2, the final exe x2 with a warm pipeline list, the final exe with every switch
    off; each exe path warmed first): the first differing post-load frame (st1/idpost2.py, aligned on the load,
    contiguous) gate 4 / 4 / 4 / 4 (base2 / new1 / new2 / off0), street 3 / 4 / 4 / 4, pier 5 / 4 / 4 / 5; frames 0-3
    after the load -- the load frame made ~670 placed textures and took ~150 pipelines from the table -- byte-identical
    in every arm. (ss1/idpost.py by log order slipped twice on the pier: a "dumped" line printed before the [ss] load
    line from another thread.) The pier's frame 4 differs from the base in both new runs (143k pixels; new1 vs new2
    15k) and not in off0: the game's own time, measured (st1/pierclk.sh: the same arms again with IF2_CLOCKLOG=2;
    base vs base 5 of 5 identical, base vs new 4 of 6 again). The game clamps a frame's dt (66.7 ms in these runs);
    in the base the load's frame (493-538 ms) and the next (76-77 ms) are both over it, so both advance 66.7 ms; in
    the new runs the load's frame is 228-247 ms and the next 46-47 ms, under the clamp, so it advances 46-47 ms: from
    there the game is ~20 ms of game time behind the base's, and from frame 4 the picture shows it (the water).
    Artefact descriptions as sets (st1/artset.py; RSX_RES_ARTLOG's pipeline hash covers the whole
    description and both shaders' bytecode): gate 150, street 191, pier 191 pipeline descriptions, every one in both
    arms; 18 / 16 / 16 samplers the same.
- **Hitch census after** (st1/campaign.sh: 3 interleaved rounds; each arm on a fresh exe path per route and round -- c
  its first run, w its second; old = d14f02fb, new / new2 = the final exe, the A/A; the pipeline list seeded by one run
  per route, as a player's earlier sessions leave it; means over the rounds, "worst" = the longest frame of all three):
  | route | cond | old: >20 / >25 / >33, spikes (ms) | new | new2 | p99.9 old / new / new2 | longest old / new / new2 (worst) |
  |---|---|---|---|---|---|---|
  | span | c | 25 / 5.7 / 1.0, 20.7 (222) | 6.7 / 0 / 0, 2.7 (20) | 8.0 / 0 / 0, 3.7 (29) | 27.2 / 20.4 / 20.7 | 43.0 / 22.0 / 22.3 (43.7 / 22.2 / 22.4) |
  | span | w | 17.3 / 2.7 / 0.7, 11.7 (119) | 8.0 / 0 / 0, 5.3 (39) | 7.7 / 0 / 0, 5.0 (40) | 24.3 / 20.7 / 20.8 | 35.3 / 22.1 / 22.9 (37.5 / 22.5 / 24.1) |
  | tour | c | 20.3 / 8.7 / 2.0, 11.3 (206) | 7.3 / 1.0 / 0, 1.3 (16) | 7.7 / 1.0 / 0, 2.7 (29) | 28.0 / 20.8 / 20.9 | 92.2 / 28.3 / 29.6 (116.5 / 28.7 / 30.9) |
  | tour | w | 15.3 / 3.3 / 1.0, 5.7 (71) | 5.3 / 1.0 / 0, 2.0 (22) | 4.7 / 1.0 / 0, 1.7 (19) | 23.3 / 20.4 / 20.3 | 35.1 / 29.2 / 29.3 (35.8 / 29.4 / 29.9) |
  | gate | c | 711 / 1.7 / 0, 0.7 (9) | 381 / 0 / 0, 0 | 337 / 0 / 0, 0 | 24.4 / 21.8 / 22.1 | 29.9 / 22.7 / 23.3 (32.8 / 23.1 / 24.6) |
  | gate | w | 628 / 1.3 / 0.3, 1.0 (13) | 455 / 0.3 / 0, 0 | 190 / 0 / 0, 0 | 23.2 / 22.7 / 21.7 | 32.9 / 24.1 / 22.5 (33.0 / 25.4 / 23.1) |
  | boat polycut | c | 9.3 / 6.7 / 3.0, 9.3 (167) | 0.7 / 0.7 / 0, 0.7 (11) | 0.7 / 0.7 / 0, 0.7 (10) | 31.1 / 16.7 / 16.1 | 47.1 / 26.0 / 24.9 (47.5 / 31.8 / 30.1) |
  | boat polycut | w | 1.3 / 1.0 / 0.3, 1.3 (20) | 1.3 / 1.3 / 0.3, 1.3 (23) | 0.3 / 0.3 / 0, 0.3 (6) | 18.8 / 17.1 / 16.1 | 29.2 / 32.4 / 22.4 (37.2 / 35.1 / 32.6) |
  | walk after it | c / w | ~0 | ~0 | ~0 | 19.7-19.6 / 20.0-18.7 / 19.5-19.8 | 20.9-20.3 / 22.4-19.4 / 20.4-20.9 |
  | world-load frame | c / w | 1005 / 646 ms (worst 1022 / 655) | 262 / 266 ms | 267 / 263 ms | | |
  What is left over 25 ms in the new arms is not new-content work: the tour's frame at ~47 s and the polycut's camera
  cut at ~36 s each open with a 28-34 ms frame in which the walker waits 16-22 ms for the game's commands (the PPU /
  SPU side; the same frames in the old arm: 27-31 ms), and the camera cut's next frame (86 new textures, 7 pipelines
  from the table) is 26-30 ms: the decode of 14 MB of texels (3-6 ms) and the new geometry's vertex conversion (+4 ms)
  remain, creation 0.8-1.3 ms. The gate's frames over 20 ms are its median (19.5 ms). Spike causes in the new arms:
  span -- the replay and vertex emission of the pan's turns to the city (throughput); tour -- the guest; polycut --
  GPU waits and the guest.
- **Average fps** (st1/fpsab.sh, 4 interleaved rounds, warm, READY + 4 s to 40 s): gate 50.73 (old) / 51.15 (new) /
  50.85 (new2) = +0.8% / +0.2% (A/A 0.6%); street 62.25 / 63.78 / 64.17 = +2.5% / +3.1% (A/A 0.6%) -- the prewarm no
  longer takes the walker's idle time in the first ~21 s after the launch-time load. After it (READY + 25 to 40 s):
  gate 51.22 / 51.19 / 50.90; street 64.41 / 63.95 / 64.02. Four more street rounds (fb): 62.80 / 64.48 / 64.49
  (+2.7% / +2.7%, A/A 0.0%), after it 64.77 / 64.57 / 64.53. That late street window follows the GPU's power state:
  GPU_TIME's busy time is 1.12-1.14 ms a frame at P0 (memory 15 GHz) and 1.27-1.31 ms at P3 (7 GHz; nvidia-smi once
  a second, st1/clk.sh), and every new / new2 run ended at P3, the old ones in 4 of 8 (st1/clkfps.py). At P3 in both
  arms: old 64.43 (n = 4, 64.17-64.84), new 64.26 / new2 64.27 (n = 8 each), -0.3%, inside the runs' spread; old at
  P0 64.75. Which runs held P0 follows the texture path (st1/gpux.sh, the bisect): the committed path in 7 of 12 runs
  (d14f02fb 5 of 9, every switch off 1 of 2, TEX_PLACED=0 TEX_UPCHUNK=0 1 of 1), runs with either switch on in 0 of
  32 (placing at 64 KB alignment, or only textures up to 64 KB -- scratch knobs, reverted -- the same). The driver's
  choice is not visible; the likely reading is that without a committed creation and an upload buffer per new
  texture the GPU looks idle enough to lower its memory clock. The frames are not longer: present p99 street 17.79 /
  17.02 / 16.81 ms (fa), 17.47 / 16.66 / 16.79 (fb); gate 21.74 / 21.19 / 21.20.
- **Save / load** (reverify step 6, st1/ssio.sh, scratch cache): the pier's F5 / F9 / menu save and load (2 / 2), five
  F5 / F9 cycles walking (5 / 5; 2735 textures placed, 1971 blocks freed and reused), that quick slot loaded at launch
  with every ST1 switch off and at the defaults; no D3D12 error, FATAL or WATCHDOG.
- **Tests:** libs/video/tests/test_tex_heap.c (new: 200,000 random takes / gives over 16 seeds against a model -- in
  the heap, aligned, no overlap, free ranges sorted and coalesced, free + live = the heap, a refused take had no
  aligned gap -- plus the split / merge cases; three mutants caught: no merge with the next range, no alignment, an
  overrun); alltests rc 0 in every group (unit's "strict-pass share" line is the known non-gating print); manifest gate
  4045 statics, 0 unclassified (17 added).
- **Review (fixes 1-10; the final exe 0be4eb2b, built from the source above with them; st1/rev/p_rev1..5.py,
  p_rev4_manifest.py):**
  1. (HIGH) A fresh placed texture's aliasing barrier was recorded in tph_create, before its upload memory was
     found; when tc_upload_alloc then failed, the resource was released and its block freed with the barrier still in
     the open command list (a later texture of the batch could be placed on the block). Now tph_activate records the
     barrier right before the first copy, and a fresh resource given up in its batch (tph_bury) goes to the resource
     graveyard, its block to a deferred list (rsx_tex_heap.h TphDefer) given back at the next batch's start, after the
     GPU wait (tph_drain). test_tex_heap: a third of the random gives deferred and drained every 40 operations (no
     take may overlap a deferred block; used = live + deferred), a two-heap deferral case, a cleared heap; three
     more mutants caught (the drain gives nothing, half a block, only the first block).
  2. TEX_UPCHUNK: a 32 MB chunk that cannot be created or mapped falls back to a buffer of the texture's own size, not
     made the open chunk (as before ST1); TEX_UPCHUNK=0 unchanged.
  3. The pipeline list is 'IF2PSL02' ('IF2PSL01' read and rewritten): each launch is a session; a description
     carries the session it was written in, and 16-byte marks record a key used (a table hit or a create, once a
     session) or, from the prewarm, dead (a shader the cache lacks, a description that does not round-trip, a failed
     create). Kept at launch: not dead, used within PSO_LIST_SESSIONS (16) sessions, at most 2 x PSO_PREWARM_MAX
     (12,288) by last use; the prewarm window is the newest 6144 of those. The file is rewritten to the kept records
     (each with its last session) when it has a bad tail or the old format, 256+ records are dropped, it is over 16 MB,
     or over 1 MB with more than half of it going -- so appends never reach PSL_FILE_MAX in use. When the walker's
     cache evicts a pipeline, the table lets go of that record (VPFPEntry.pslk, psl_evict; the walker's own
     reference goes to the pipeline graveyard as before). shaderlist.bin has not this growth: its records are program
     inputs (a VS's microcode to END, an FP's code with its key), not decompiler output, so a build adds none --
     229 VS + 14,634 FP records, 18 MB of its 64 MB cap, regenerated on the prewarm thread in 1.3-1.5 s, all of them
     cached (0 compiles); left as it is.
  4. s_dec_lk: the shader-list prewarm thread (below normal) runs above normal while it holds the lock, and yields
     between records while another thread waits for it (s_dec_wait): the walker's record-time decompiles go first.
  5. TEX_PLACED_VERIFY copies only the upload's own bytes (up_total), not up to the 512-rounded end.
  6. The list is rewritten only after it was read and parsed whole. One that exists but cannot be read (open, size,
     memory, an unknown format) is left as it is and not written that session; a failed rewrite of a file with a bad
     tail or the old format leaves it unwritten too; the appended length is the file's real one.
  7. st1_shutdown (in rsx_d3d12_backend_shutdown, the GPU idle): the shader-list prewarm thread, the pipeline prewarm
     threads and the heap helper are stopped and joined; the table, the DXBC map (sc_mem_clear), the slots' textures,
     the heaps and spares are released and the statics reset (a later init starts from nothing); the prewarm keeps
     its own device and root-signature references; tph_on refuses another device. (The game never calls the backend
     shutdown -- only the harnesses do: compiled, not exercised.)
  8. Video memory (s_vram, IDXGIAdapter3::QueryVideoMemoryInfo): a heap -- a spare or not -- is made only while the
     process's local use stays 96 MB under its budget, else the texture is committed; at a batch start a heap nothing
     is placed in is released after 600 batches empty (one kept), and at once, with the spares, over the budget.
     [VRAM] lines, FRAME_TRACE vram_hw_mb; TEX_PLACED_BUDGET_MB=<n> (diagnostic) sets the budget.
  9. psl_ser requires the stream-output strides, buffer strides and rasterized stream unset (as NumEntries).
  10. The list's read is O(n) (PslKey.ent: each key's entry); the walker hashes a shader's bytecode once per DXBC name
      and blob (s_pslh), not on every miss.
  - **List tests** (st1/rev/listtest.sh, gate, 12 s a run): a new list (158 descriptions), the same cache again (158
    prewarmed, 150 used marks), an 'IF2PSL01' list (191 -> rewritten as v2, 191 prewarmed), a record naming a missing
    VS (stale in the prewarm, marked dead, dropped at the next launch) and one last used in session 0
    (PSO_LIST_SESSIONS=2: dropped), 1500 valid variants added with PSO_PREWARM_MAX=600 (1660 -> 1200 kept, rewritten;
    600 prewarmed), a directory where the list goes and a file in an unknown format (both left as they were, nothing
    appended), a bad tail (rewritten: 158 records, 143 marks). No D3D12 error in any.
  - **Low memory** (st1/rev/lowmem.sh, the final exe, TEX_PLACED_VERIFY on; boot and tour): TEX_FAULT_UPLOAD=50 (every
    50th upload allocation fails: 211 / 197 failures, 19 / 22 fresh placed textures buried and their blocks given back
    a batch later), TEX_FAULT_CHUNK=1 (every 32 MB chunk refused: 376 / 306 buffers of the texture's own size instead,
    nothing lost), TEX_PLACED_BUDGET_MB=1000 (91 / 351 textures committed for want of a heap under the budget, a spare
    released, the high-water mark 923 / 914 MB against 988 / 976): 0 differing rows in 11,557-12,642 placed textures a
    run, no D3D12 error, no device removal, every run to its end. Pixels on the pier against d14f02fb: the chunk and
    budget runs as the default (frames 0-3 identical, frame 4 the game's clock); with TEX_FAULT_UPLOAD=50 on both the
    new path and the old one (TEX_PLACED=0 TEX_UPCHUNK=0) the same uploads are lost: frames 0-2 identical, frame 3 73
    pixels within 2 levels (frames 0-3 identical in the first run on 2290ed21), frame 4 the clock.
  - **Video memory** (st1/rev/vram.sh, 2 rounds + seeds, warm; the process's high-water by QueryVideoMemoryInfo, the
    GPU's memory.used over idle by nvidia-smi): the process's high-water mark on the tour 976 / 976 MB against 935 /
    933 MB on the old texture path (TEX_PLACED=0 TEX_UPCHUNK=0), on the boot 989 / 989 against 934 / 934 (+42-55 MB:
    the two spare heaps and the heaps' unused tails); the GPU's memory.used over idle 999 / 993 and 1001 / 1008 MB
    against d14f02fb's 952 / 952 and 957 / 947.
  - **Re-verified on the final exe:** oracles (st1/ver.sh fin2) 29,919 placed textures / 92,787 subresources 0
    differing, 448 VS-key checks 0 different, 595 table hits 0 different descriptions; identity (st1/idall.sh,
    idpost2.py) gate 4 / 4 / 4 / 4 (base2 / new1 / new2 / off0), street 4 / 4 / 4 / 4, pier 5 / 4 / 4 / 5, artefact
    sets equal (150 / 191 / 191 pipelines, 18 / 16 / 16 samplers); save / load (ssio fin2) 2 / 2 pier, 5 / 5 walking,
    the launch loads off / default, no D3D12 error, FATAL or WATCHDOG; alltests rc 0 in every group; manifest gate
    4053 statics, 0 unclassified (8 added). Hitch census (campaign fc2, 3 rounds): longest frame old / new / new2
    (worst of 3 rounds): street pan cold 68.0 / 21.5 / 21.6 ms (116.4 / 21.7 / 22.0), warm 34.0 / 22.2 / 23.2; tour
    cold 42.8 / 27.8 / 28.5, warm 33.7 / 28.5 / 29.1; gate cold 33.3 / 23.3 / 24.6, warm 33.2 / 23.0 / 22.7; boat
    polycut cold 49.7 / 26.2 / 35.2, warm 40.0 / 35.4 / 26.7; the world-load frame cold 1006 / 266 / 267 ms, warm 644
    / 261 / 264. Frames over 25 ms a run: span cold 6.0 / 0 / 0, tour cold 7.3 / 1.0 / 1.0, polycut cold 6.3 / 0.7 /
    1.0; p99.9 span cold 28.2 / 20.6 / 20.6, tour cold 26.9 / 20.3 / 20.3. As in the first campaign but two frames:
    the polycut's worst (27.8-35.6 ms in the 11 new runs whose polycut window holds it; the first campaign's worst
    31.8 / 35.1 was the same frame) is the first frame after the load, whose walker waits 22-24 ms for the GPU to
    finish the load's copies of ~670 textures (the old path's 650-1000 ms load frame absorbed them; csum's polycut
    phase starts 0.4-0.5 s before READY, so it counts the frame in some runs and not others); and new2's gate round 2
    ran with a 22.9 ms median (both runs on its exe path, the walker idle 5.3 ms a frame against 1.6 -- the guest side
    slower, its own work and the GPU's equal: an A/A outlier). Fps A/B (fa2, 4 rounds): gate 50.48 (old) / 51.39 (new)
    / 51.45 (new2) = +1.8% / +1.9% (A/A 0.1%), street 62.84 / 64.87 / 64.49 = +3.2% / +2.6% (A/A 0.6%); after the
    first 25 s gate 50.96 / 51.52 / 51.49, street 64.53 / 65.02 / 64.69: no loss.
- **Next** (by what is left): (1) new content's conversion work on the walker -- the texel decode and the vertex
  conversion of a camera cut's new geometry (~8-10 ms of its 26-30 ms frame): decode textures known at record time on a
  worker before their replay (TX1's fence rules decide when their bytes are final), or the replay off the walker
  (PF7's lever 1); (2) the game-side waits at the camera cut / the tour's 47 s (16-22 ms on the PPU / SPU side); (3) a
  first launch with an empty cache: the VS compile on the record path (an async VS slot the replay waits for) and the
  first session's PS waits; (4) the load frame's remaining ~260 ms (the 83 MB decode, the state's restore,
  render-target creation); (5) the placed path's video memory: +42-55 MB at the high-water mark (two spare heaps
  and the heaps' unused tails) -- one spare, or smaller heaps, if a small GPU's budget is ever reached; (6) the first
  frame after a load (28-36 ms): its walker waits 22-24 ms for the GPU to finish the copies of the load's ~670
  textures (the old path's longer load frame hid that wait) -- the load frame could wait for them itself.
## 2026-10-28 (evening) — PF7: the critical path measured per scene; no exact lever reaches 3% -- the gate is a walker / SPU-lighting / walker chain, the street the walker end to end (measured, not built)
EBOOT: BCUS98125 update 1.04 | no source change kept (a timeline diagnostic, the SEMA_FLUSH prototype and a placement
experiment were built in scratch and reverted); docs/NOTES.md
- **Goal:** on the committed exe (d14f02fb) establish per heavy scene what sets the frame -- PPU main (P + B + K), the
  walker, the SPU workers or the GPU --, size every named lever, and build the single largest exactly achievable one;
  if none reaches ~3% in a heavy scene, report instead.
- **Harness** (local work files): rgs.sh (pf4's scenes plus `pan` = the pier pan and `span` = the street walk for 600
  frames, then a pan), r.sh / prof.sh / campaign.sh (pf6's with outputs in pf7, threadcpu), ftblk.py (the walker's
  split from FRAME_TRACE), ftwin.py (fps after the shader-list prewarm), tl.py / tlspu.py (the PF7_TL timeline below).
- **Per scene** (30 s after READY; threadcpu, WALKER_STATS, PS3_POLLTIME, FRAME_TRACE, GPU_TIME, IF2_CLOCKLOG):
  | scene | fps (READY+4..30 s / after the prewarm) | frame p50 / p95 / p99 ms | walker | SPU workers 0x2000-0x2004 | PPU main | GPU |
  |---|---|---|---|---|---|---|
  | gate fight | 50.08 / 50.5 | 19.68 / 21.32 / 22.35 | 90.3% busy; 1.9 ms a frame on SEMAPHORE_ACQUIRE 0x870 | 86.7 ms a frame, 5 x 88% | 9.2 ms | 1.43 ms |
  | busy street | 62.09 / 64.7 | 16.35 / 16.97 / 17.70 | 99.4% busy | 59.6 ms, 5 x 74% | 9.4 ms | 1.13 ms |
  | pier still | 64.11 / 66.8 | 15.78 / 16.52 / 17.12 | 99.6% busy | 59.5 ms | 8.5 ms | |
  | pier pan | 72.25 | 13.42 / 16.69 / 18.12 | 81-89%; the acquire 1.1-1.9 ms | 49.0 ms | 7.6 ms | |
  | street pan | 68.36 | 13.70 / 17.38 / 19.43 | 77-96% | 50.6 ms | 7.7 ms | |
  The pans by view: city frames (draws > 8000) 16.80 ms on the pier pan, 16.85 on the street pan, walker-bound;
  water frames (< 4000 draws) 13.25 ms, at the 75 Hz cap (their readbacks wait 3 ms for the GPU).
  The walker's split (FRAME_TRACE, ms a frame): gate render_frame 5.65 (GPU waits 1.27), readback 2.11, vertex
  emission 4.81, texture validation 0.97, uploads 0.42, the rest (FIFO decode, state, constants, records) 7.86; street
  4.96 (0.53), 1.86, 3.90, 0.78, 0.37, 7.21. HOST_SAMPLE on the frame clock, street: vnew_draw 59.8% inclusive
  (rsx_vemit_run 21.2: self 11.5, conversion 4.8, cache lookup 4.5 of which the content hash 2.9 = 0.48 ms),
  render_frame 30.2%, the readback 12.1%, vp_record_cb 10.8%; gate: the reference's report flush (deliver_n_ex ->
  d3d12_report_measure) 9.2%, the content hash 2.2% (0.44 ms). [RBT]: gate 2 readbacks a frame, their flush 1.30 ms,
  GPU wait 0.61, stores 0.18; street 1.11 / 0.52 / 0.16.
- **The frame, stamped** (PF7_TL, a diagnostic build only: QPC stamps of every batch, readback, GPU wait, back-end label,
  acquire, SPU put into the label window, reference reached / delivered / published, flip; medians of 1037 gate / 769
  street frames after the prewarm):
  - gate, 19.75 ms: record 2046 G-buffer draws to 2.63 ms, replay 1.47; record 1856 more; the readback 6.67-8.93
    (its batch's replay 1.29, GPU wait, stores, the depth copy); **label B (0x860) at 8.95**; the cascades (5.4 ms of
    recording, two capacity flushes 0.75 / 0.51 ms) to **the acquire of 0x870 at 14.36, released by an SPU at 16.32**
    (1.97 ms of waiting); sun pass, post passes, lamps to the reference at 17.50; the report flush (1591 records:
    replay 1.28, GPU wait 0.62) -> published 19.43 = the flip; the present (an empty batch) 0.27. The SPU writes after
    label B: 0x880 +0.32 ms, 0x890 (the lighting buffer) +2.87, **0x870 +7.35** (p10 / p90 7.09 / 7.58; any of the five
    workers). So **gate = walker to label B 8.95 + SPU lighting chain 7.35 + walker after the acquire 3.42**; the
    walker's cascades run under the lighting with ~1.95 ms to spare.
  - street, 15.27 ms: label B at 7.47; 0x890 +2.74, 0x870 +4.78; the walker reaches the acquire at +5.17 (it waits in 49
    of 769 frames); reference 13.84, published 14.00; the present's replay (1797 records) 1.21 -> 15.27. **Walker-bound
    end to end**; the lighting chain ends 0.39 ms before the walker needs it.
- **The shader-list prewarm runs on the walker for ~21 s after a launch-time state load** at the street / pier (9 s at
  the gate): its idle hook takes ~1 ms slices whenever the walker would wait for `put`, so the PPU's next commands wait;
  street 60-61 fps while it runs, 64-66 after (pier 62-63 -> 66-68). In play it finishes on the title screen; it does
  dilute every campaign's READY+4..30 s window (both arms alike). `SHADER_PREWARM=0` matches the post-prewarm window.
- **Probes** (wrong output, measurement only; 2 interleaved rounds; all / after the prewarm):
  | | gate all / post | street all / post |
  |---|---|---|
  | defaults | 50.33 / 50.65 | 62.68 / 64.61 |
  | REPORT_EXACT=0 | 52.70 / 53.06 (+4.8%) | 62.69 / 64.69 (0) |
  | RT_READBACK=0 | 54.30 / 54.75 (+8.1%) | 66.37 / 67.98 (+5.2%) |
  | SHADER_PREWARM=0 | 51.31 / 51.31 | 64.37 / 64.37 |
  REPORT_EXACT=0's gate gain is the walker's GPU wait at the reference (1.23 -> 0.54 ms a frame) and one batch less
  (render_frame -0.25 ms); RT_READBACK=0's is the readback batch's replay (moved into the cascades' slack), its GPU
  waits and stores.
- **Levers, ranked by what an exact version could take:**
  1. **A render thread for the replay's D3D12 recording** -- the replay is 5.0-5.7 ms of walker time a frame (28-30%
     of its samples), all of it on the street's path and 4 ms of it (the first G-buffer batch 1.47, the readback's
     1.29, the reference's 1.28) on the gate's two critical walker segments; the readback and the reference wait on
     it. Exact only if every guest read of a replay (texture validation and uploads, TX1's fences) stays at its FIFO
     point on the walker and only the command recording moves, with the shared renderer state (RT table, PSO and
     sampler caches, descriptor heaps, report plans, self-sample copies) handed over per batch. Ceiling ~2.5-3 ms at
     the street (-15-20%), ~2 ms at the gate. XL; the highest risk in the renderer.
  2. **SPU-side speed at the gate** -- the lighting chain (label B -> 0x870) is 7.35 ms, 37% of the gate's frame, with
     all five workers at 88% (street 4.78 ms at 74%); its length does not depend on when label B comes (correlation
     0.08 over 1037 frames): SPU execution. Every 10% of SPU time is up to ~0.7 ms at the gate. No profile this time
     (HOST_SAMPLE_ALL died); leads: 0x2004's kernel time, the one global reservation lock (spu_lockline_lock) under
     five busy workers, the SPU runtime's ~12% (PF1).
  3. **Async readback** (PF2's route b; SAMP_KEEP sets would need fences too) -- the exact part is the copies' GPU waits
     (gate 0.61, street 0.52 ms) and stores (0.16-0.18): ~0 at the gate (label B, and the lighting behind it, wait for
     the stores anyway), at most the 0.39 ms of lighting slack at the street (~2.5%). L, high risk.
  4. **The gate's report wait at the reference** -- REPORT_EXACT=0's +4.8% is the GPU wait (0.72 ms) plus a batch: the
     boxes must run on the GPU before the reference is visible, so an exact path can only take the batch's head out
     (SEMA_FLUSH, below) or end the waited-for list at the last lamp (the post passes' GPU time, est. <= 0.2-0.3 ms).
  5. **The vertex cache's content hash** -- 0.44 ms at the gate, 0.48 on the street (2.2-2.9% of the walker); exact only
     with write tracking that sees every writer up to the draw, which a write watch harvested per FIFO segment does
     not (PF3); at the gate only its share in the critical segments counts.
  6. **The shader-list prewarm on the walker** -- 6% on the street for ~21 s after a launch-time load, ~0 in play.
- **Built, measured, not kept:**
  - *SEMA_FLUSH* (local p_sf1.py; exe fba75704 in pf7/bin_sf1): when a FIFO pass stops at a SEMAPHORE_ACQUIRE
    whose label has not arrived (cellGcm_fifo_kick_wait; s_wake GCM_WAKE_EQ) and at least 32 records are pending, the
    walker replays and submits them (a mid-frame flush, as a capacity flush's) instead of sleeping, so that replay and
    its GPU work leave the batch the reference's report flush waits for. Kill switch SEMA_FLUSH=0; oracle
    SEMA_FLUSH_VERIFY=1 (every guest-memory texture slot the flushed batch bound, hashed after it and again at the next
    batch, where the old path replayed those records): gate 1399 batches, 20,985 slots, **0 differing**, no fenced
    slot, no non-VP record. At the gate: one flush a frame of ~688 cascade records (0.34 ms), the reference's batch
    1591 -> 899 records, its replay 1.28 -> 1.09 ms and GPU wait 0.62 -> 0.57, the walker's acquire wait 1.7 -> 1.35
    ms. 4 interleaved rounds (EXITS=40): **gate 51.22 / 51.27 (A/A) vs 50.47 = +1.5% / +1.6%** (after the prewarm
    51.65 / 51.71 vs 50.84), present p99 21.7 vs 22.6 ms; the street pan 69.23 / 69.11 vs 69.08 (0; the label arrived
    during 15% of its flushes); the busy street's walker finds the label already there in 94% of frames (not A/B'd).
    An extra batch costs ~0.25 ms (render_frame +0.15, texture re-uploads +0.11): most of what it moves. Below the
    bar: reverted.
  - *Placement* (local p_exp_spu5.py, exe be7ac874 = pf7/bin_spu5; 3 interleaved rounds against the committed
    exe): the fifth SPURS worker 0x2004 on a core of its own too (`HOST_PLACEMENT_SPU5=1`, every other thread then on
    two cores): gate 50.55 vs 50.85, street 62.28 vs 62.79 (the experiment exe without it 50.80 / 62.35) -- nothing;
    0x2004 keeps its 35% kernel time on a core of its own, so the time is something that worker does, not core
    sharing. The walker and the four placed workers on the cores with the highest CPPC scheduling class instead of
    the highest CoreIndex (`HOST_PLACEMENT_BEST=1`; the test PC's 9800X3D ranks cores 4/10 first, 14 last -- the walker's
    core today; that order also puts an SPU worker on core 0): gate 48.48 (-4.7%), street 58.68 (-6.5%). Reverted.
- **Seen, not examined:** HOST_SAMPLE_ALL at the gate died with an access violation 4 s into sampling (base exe) -- no
  SPU split this time; SPU 0x2004, the fifth worker, spends 22-35% of its CPU in the kernel, placed or not (the four
  placed ones 0-1%); in moving city views vertex emission rises to 4.4-5.4 ms (1-2.4k first sightings held and 0.4-1.3k
  admissions a frame; 3.8 ms still); the street pan's switch from walking to panning makes 26-35 ms frames (14-35 new
  textures a frame, 8-9 MB of uploads, new PSOs, a 9.2 ms vertex-shader compile).
- **Next lever:** the replay off the walker. A render thread that records each batch's D3D12 commands while the
  walker records the next one -- the guest reads of a replay (texture validation and uploads, TX1's fences) kept at
  their FIFO points on the walker, the rest of the renderer's state handed over per batch -- is the one lever left that
  is several ms on every heavy scene (the street's 5 ms of replay, the gate's 4 ms on its critical segments); then SPU
  speed for the gate's lighting chain, starting with a working SPU profile (HOST_SAMPLE_ALL, or ETW) and 0x2004's
  kernel time. SEMA_FLUSH (+1.6% at the gate, exact) is ready in scratch if a smaller step is wanted.
## 2026-10-28 (afternoon) — PF6: the sun glow's report records are composed on the GPU in the consumer's own command list -- no flush and GPU wait per lamp (gate fight +4.5%, A/A 0.4%; 242k composed rows equal to the exact path's texels)
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video/rsx_report.c / .h (REPORT_GPU: plans, their rows, owed delivery, the
2D-engine sync, GET numbers, REPORT_GPU_VERIFY's value tables; plan_ok appended to rsx_report_backend),
rsx_d3d12_report.c / .h (the compose pipeline: rsx_rgpu_*), rsx_d3d12_backend.c (records carry their plans; the prepass
binds a planned unit to its composed texture; the replay composes before the draw; the list hooks; plan_ok; draw_reads
scans a program the memo misses), cellGcmSys.c (an NV3089 blit / NV308A word lets owed reports land first),
tests/test_rsx_report.c (five cases), tests/test_report_d3d12.c (case C, WARP); config/ss_manifest.txt
- **Goal:** the gate fight's ~10 report deliveries a frame (each a mid-frame flush: render_frame + wait_for_gpu) cost
  +11.3% (the ceiling on d7f70f10: 47.37 fps, 52.72 with REPORT_EXACT=0, whose values are wrong); make them cheap without
  changing any value the game reads. The busy street has no reports (62.18 / 62.18).
- **Measured first** (the committed exe; local work files: r.sh, campaign.sh, ftrep.py -- batches, flush reasons,
  render_frame and GPU-wait ms a frame from FRAME_TRACE):
  - REPORT_STAT / REPORT_WHY at the gate: 10 "texture read" deliveries a frame, all the sun glow (unit 0, the 4x2
    Y16_X16 pair at IO 0x14080, format 0x0001F52A, pitch 16); 9 box-2 draws a frame skipped by the precise rule; 9
    draws a frame the rule "could not judge": the consumers (fp B5BB952A) -- the scan memo is one entry a bucket and the
    box program (CA60465E) evicts theirs, so the old rule took them as reading every unit (they read unit 0 anyway).
  - The ten lamps are back to back: GETs ~145 us apart (the first two ~1.04 ms: the first flush replays everything
    since the previous batch), at ops 1512-1541 of the frame's last batch of 1566 (PASSLOG), then six post passes, the
    display composite, and SET_REFERENCE right before the flip.
  - Per delivery: render_frame of a 3-record batch ~60 us plus the GPU wait ~100 us (default vs REPORT_EXACT=0:
    render_frame 5.96 vs 5.36 ms, GPU waits 1.70 vs 0.57 ms a frame; 15 vs 5 batches).
  - 4 interleaved rounds: default 48.45 fps, REPORT_TEXREAD_DEFER=1 (slice B's timeline, wrong values) 50.85
    (+5.0%), REPORT_EXACT=0 53.46 (+10.3%). Under DEFER every report lands at the frame's reference (REPORT_STAT:
    "reference" 51/s, no walker stop in between): one flush a frame, render_frame 5.69 ms, GPU waits 1.19 ms.
- **Design, by the numbers:**
  - (b) a cheaper flush -- submit without the wait, deliver at the next render_frame's collect, before its prepass --
    gains nothing here: the next lamp's consumer comes 3 records later, its render_frame waits for the same list, and
    the batch overhead stays. Not built.
  - (a) slice B (rptgpu/plan.md, simplified): the DEFER timeline, about half the ceiling. The other half is the
    reference: the reports must be in guest memory before the PPU sees SET_REFERENCE (it waits for it before it puts
    the flip; TX1 measured that chain 1:1), and their boxes are in the frame's last batch, so the walker replays that
    batch and waits for the GPU there instead of after the flip. No exact design moves that (the counts need the batch
    executed); an early submit after the last lamp would save only the 25-op tail (estimated ~0.05 ms). Built.
- **REPORT_GPU (default 1; `=0` = the path before, the kill switch):**
  - **Plans** (rsx_report.c): at SET_BEGIN_END, a draw whose live units (the precise rule's) read pending reports and
    whose route the backend vouches for (draw_reads) gets a plan per such unit instead of a delivery when each is a 4 x
    1..4 Y16_X16 linear texture, pitch 16, one level, 2D, 16-aligned, every pending report over it a counted ZPASS one
    exactly at a 16-byte row, fewer than 128 plans live, and `plan_ok` (the backend) agrees: no diagnostic that
    reorders, drops or rewrites records or texture sources; the CPU path would upload the format; no texture pack or
    dump could see it (the packs veto UN textures: the gate's). A row is the latest report pending at it (GET number,
    timer, segments) or, none pending, the four words memory holds at the draw. Anything else delivers as before
    (REPORT_STAT counts plans and the fallbacks by reason: route / shape / row / window / backend).
  - **Owed:** the old rule delivered every pending report at that draw, so they are owed (REPORT_PRECISE's owed_n):
    written at the next walker stop (before GET moves), flip, user command or -- new -- 2D-engine operation (an NV3089
    blit, an NV308A / NV309E inline word: they read and write guest memory as the walker passes them), and as before at
    every label, semaphore release, reference, RENDER_ENABLE test, queue-full and save-state quiesce: by deliver_n, the
    FIFO prefix in order, values by its own formula.
  - **The compose** (rsx_d3d12_report.c; at the replay, before the planned draw, once a list): this list's occlusion
    queries are resolved into a buffer mid-list, a 4x4-thread compute pass writes each row's texels -- the timer words,
    value = sat32(base + the counts of the row's queries in this list, each normalised as rsx_zpass_collect does for
    RES_SCALE), 0 -- as (W << 16 | W >> 16) into a staging footprint, and CopyTextureRegion moves it into a pool texture
    with the CPU upload's descriptor (R16G16_UNORM, 4 x h, one level); the prepass bound the unit to it with the CPU
    path's view (the crossbar's mapping). The base is the report's folded samples plus its segments' samples collected
    so far: every earlier list was collected at this render_frame's start; the row's segments closed at GET_REPORT,
    before the consumer's BEGIN, and records replay in record order with segments that only grow, so this list's
    queries of them are one run [qa, qb) before the draw (checked: a bug counter). A plan whose report was delivered
    first composes the written value; a record carried into the next list composes again from the collected samples:
    the same value. Plans die with the replay of their records (the latest BEGIN's live on for a carry); a load drops
    them all. GPU: ~10 us a compose (a resolve, three barrier calls, one 16-thread dispatch, one 32-byte copy;
    GPU_TIME 1.45 vs 1.35 ms a frame against DEFER).
  - draw_reads scans a program the memo does not hold (REPORT_GPU only; the function fp_scan_memo's miss path calls,
    into scratch): the consumers are judged ("could not judge" 9 -> 0 a frame).
- **Exactness argument:** a planned draw samples rsx_texture_decode of the 16h bytes the exact path's prepass would
  read for it -- the records as the FIFO's deliveries up to that draw leave them: per row the latest pending report's
  encode(ts, value) (the same ts, the same sum of the same query counts, the same normalisation and saturation) or
  memory's words. Guest memory gets the same FIFO writes, in order, before every point the PPU or an SPU can observe
  (GET, flip, labels, semaphores, the reference); only the time between the consumer and the next such point differs.
  Residual, as in the exact path but with a longer window: a PPU / SPU read of the records with no observation point
  between (racy on hardware too); a vertex or index fetch from them (unhooked before too). The exact path's own
  write-after-read case (a later GET with no counted draw, written before an earlier reader's prepass) is RSX-correct
  under a plan: its rows are the draw's moment.
- **Oracle** (`REPORT_GPU_VERIFY=1`, diagnostic; pf6/ver.sh): every compose's footprint is copied back and, after the
  list ran, each row is compared with the exact path's texels (the report's value now -- deliver_n's formula over its
  completed segments, or the value written -- encoded and run through the CPU upload's decoder; a raw row's words), and
  each composed value with the value the report's delivery writes ([RPTVER]). The final exe (5c3b50d6): gate 62,420
  rows (31,210 composes) / 62,400 delivered values, 2x render scale (RES_SCALE's per-query divisor) 57,620 / 57,600,
  texture packs on 60,020 / 60,000, five F5 / F9 cycles at the gate 60,020 / 60,000 -- **240,080 rows and 240,000
  values, 0 differing**, 0 bugs, 0 fallbacks, 0 violations, 0 D3D12 errors. The street, the pier (still, pan) and a
  cold boot of checkpoint 03 (title, load, boat polycut, walk) have no ZPASS report: no plan, no delivery, no error.
- **Picture** (pf6/idall.sh: the committed exe d7f70f10 x2 against the final x2 and with REPORT_GPU=0, aligned on
  the load by ss1/idpost.py): post-load frames identical until the scene's own drift -- gate 4 / 4 / 4 / 4 (base2 /
  new1 / new2 / new0), street 4 / 5 / 5 / 5, pier 5 / 5 / 5 / 5; artefact descriptions 0 different (street new1 lacks
  one PSO line, as before). The gate's first four post-load frames, lamp coronas included, are byte-identical with the
  committed exe's. Coronas over 60 dumps a run (every 4th present; pf6/coronas.py: five lamp regions and a sky
  control), REPORT_GPU=1 vs =0, two runs each, mean luma: A 164.8 / 166.1 vs 167.9 / 166.7, B 143.4 / 144.1 vs 144.4
  / 144.1, C 127.6 / 127.2 vs 127.2 / 127.2, D 163.1 / 162.0 vs 164.3 / 165.9, E 110.7 / 109.4 vs 111.2 / 110.7, sky
  165.5 / 161.8 vs 171.2 / 167.3: within the runs' spread (the fire and smoke animate).
- **Measured** (pf6/campaign.sh, 4 interleaved rounds, the final exe; new2 = defaults again, the A/A): gate 49.85 (new)
  / 49.94 (new2) / 47.62 (old, REPORT_GPU=0) fps = **+4.7% / +4.9%**, present p50 19.74 vs 20.71 ms, p95 21.49 vs
  22.51; batches a frame 6 vs 15, render_frame 5.64 vs 5.96 ms, GPU waits 1.32 vs 1.75 ms. Street 60.99 / 61.13 /
  61.01 (no reports: -0.03% / +0.2%, the A/A 0.2%). The earlier quick A/B (3 rounds): new 50.00, DEFER 50.51,
  REPORT_EXACT=0 52.97, old 47.84 -- the compose costs ~1% of DEFER's timeline. Batching the compose's transitions
  (four barrier calls -> two) was within noise (3 rounds 49.31 vs 49.76; GPU waits 1.24 vs 1.30 ms); kept.
- **Save / load:** the pier's F5 / F9 / menu (2 saved, 2 loaded), five F5 / F9 cycles walking (5 / 5), five at the gate
  with plans and the oracle on (5 / 5; the rows above), that gate slot loaded at launch with REPORT_GPU=0 and at the
  defaults (plans resume, 527/s); no D3D12 error, FATAL or WATCHDOG. A save's quiesce delivers every pending report
  (as before); no GPU-side report state outlives a list (rsx_rgpu_reset at the renderer's load reset, the plans at
  GCM_'s teardown / restore).
- **Tests:** tests/test_rsx_report.c 147 (new: the ten lamps planned with no measurement, the compose's values equal to
  the old delivery's, then owed to one walker-stop delivery; a row delivered before its compose and a raw row; plan
  lifetime over replays; five fallbacks and REPORT_GPU=0; the 2D-engine delivery and a load dropping plans; VERIFY's
  tables), tests/test_report_d3d12.c 28 on WARP (case C: three lamps in one list -- 256/512, 192/256 and 85/512 with
  a RES_SCALE divisor of 3 -- composed, copied out and equal to the CPU decode of the records the exact path then
  writes; composed again from the collected samples and from the written value; the D3D12 debug layer is not installed
  on the test PC); alltests rc 0 (unit's "strict-pass share" line is the known non-gating print), manifest gate 4026
  statics, 0 unclassified.
- **Review** (the main review: no high or medium finding; the LOWs fixed -- local p_rev_*.py, the
  pre-fix copies pf6/rev/pre; final exe d14f02fb):
  - *#1 resource states:* s_rg_q and the staging buffer are buffers -- created in COMMON, and COMMON again at every
    list's start (a buffer decays to COMMON when the list that used it has run) -- so each transition names the state
    they are really in. Before, a list whose first compose resolved no new query (a carried consumer, boxes in an
    earlier list) transitioned s_rg_q from COPY_DEST while it was COMMON: a debug-layer error some drivers would not
    tolerate. No transition at a list's end any more. The pool textures (not simultaneous-access) keep the states
    their explicit transitions leave; the upload and readback buffers never change state.
  - *#2 violations:* a violation only stops NEW plans (REPORT_STAT "stopped"); a live plan still learns its report's
    value at the delivery and owed reports still land at the 2D engine. plan_ok now gets the unit and the texture's
    address and checks the texture the backend will bind there (cur_texs), and rsx_rgpu_texture checks that the plan's
    rows can be had, so the prepass binds only plans whose compose cannot fail. If a bind fails anyway, the reports
    whose draws are all collected are delivered without a measurement (rsx_report_deliver_complete: a measurement
    inside the replay would flush recursively) before the CPU upload, and a report over the texture still waiting for
    a draw of the same batch is logged as an inexact read (never seen).
  - *#3 no order assumed:* each query's segment is uploaded beside its divisor and the compose sums the queries whose
    segment is in the row's range ([qa, qb) only bounds the loop). WARP case D: segment t, an unrelated query, t
    again -> 512, the collect's sum (the old one-run rule gave 256; a mutant without the segment test composes 4608 and
    fails the test and the oracle).
  - *#4 plan lifetime:* a record carried into the next batch from an earlier SET_BEGIN_END drops its plans (its 1-2
    dangling vertices draw nothing); the save-state reset drops the plans of the records it still holds before its
    quiesce.
  - *#5 the first-lamp hitch:* the pipeline and its ~140 resources are now made on a thread of their own at device
    init (rsx_rgpu_prewarm); until they are ready plan_ok says no (the old delivery). The gate from its state (FRAME_TRACE,
    3 runs each, pf6/rev/hitch.py): the second frame after the load took 63 / 63 ms with REPORT_GPU (38-39 ms
    without), once 1244 + 234 ms (the first run after a rebuild: a cold driver shader cache); now 31-33 ms (38-39
    without). The init takes 49-51 ms on its thread (180 ms with a cold driver cache, 14 ms on WARP); READY 0.7-0.8 s
    either way; the load's frame 610 vs 622 ms (both arms within their run-to-run spread).
  - *#6 (optional) not built:* this backend samples no vertex texture (only the experimental draw engine binds
    them), and a draw's vertex / index fetch ranges are known only inside vnew_draw, the per-draw path; that gap
    predates PF6 (the exact path has it between a GET and its consumer).
  - Re-verified (pf6/final.sh, T=rfin): the oracle 242,480 rows / 242,400 values, 0 differing (gate 60,020, 2x render
    scale 60,020, texture packs 62,420, five F5 / F9 cycles at the gate 60,020), 0 bugs / violations / D3D12 errors; the
    street, pier still / pan and the cold boot: no report, no error. A/B, 4 rounds: gate 50.62 (new) / 50.44 (new2) /
    48.43 (old) = +4.5% / +4.1% (A/A 0.4%), present p95 21.15 vs 22.10 ms; street 62.02 / 61.95 / 61.93 (A/A 0.1%).
    Identity against d7f70f10: gate 4 / 4 / 4 / 4, street 4 / 3 / 4 / 5 (the street's known drift, 3..6), pier 5 / 5
    / 5 / 5; coronas within the runs' spread (lamp means 167.4 / 168.3 vs 169.1 / 167.3, ...). Save / load as above
    (pier, five walking, five at the gate with the oracle on, that slot loaded at launch both ways). Tests:
    test_rsx_report 154 (case 23: the violation, deliver_complete, plan_ok's arguments), test_report_d3d12 32 (case D,
    the prewarmed pipeline), alltests rc 0, manifest gate 4028 statics, 0 unclassified.
## 2026-10-28 (morning) — TX1: the draws before a reference read the textures as they were at it -- the water job's rewrite no longer reaches them; PF3_VERIFY's "stale" binds were the right bytes (too-new binds 0-216 a run before, 0 after)
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video/rsx_d3d12_backend.c (TEX_FENCE: tex_fence_hook, tex_fence_capture
(+ _new, tex_fence_copy, tex_fence_rsx_note), tex_fence_split, tc_round_ensure, tc_alloc_slot, the fence's copy in
vp_upload_tex_slot (tc_fz_src / _srcmap / _newsrc) and its watched / hashed checks, the texture packs' bytes, FRAME_TRACE's
fl_texfence / texfences; PF3_VERIFY's fence oracle: tfv_*), rsx_tex_cache.h (the rule; tc_wt_fence_harvest / _current,
tc_wt_span_rsx / _hot, tc_wt_written_now, tc_wt_reset_span; the fence's state tc_fence, tc_fz_*), rsx_commands.c / .h
(g_rsx_tex_fence_hook at SET_REFERENCE, the back-end label, the texture-read label), cellGcmSys.c (the resync rescue
calls it), tests/test_tex_cache_rsx.c (five fence cases); config/ss_manifest.txt
- **Goal:** PF3_VERIFY found a few "clean but different" binds of two 64x64 main-memory textures (0x47C72900 /
  0x47C77E80; ~10 in 1.7M binds on the pan, 1 on the pier, 3 in a cold boot): a write after the round's harvest, before
  the bind. Find the writer and how the game orders it against the draws; make a bind hold the bytes the RSX reads.
- **The writer** (local work files: a diagnostic build, TX1_TRACE -- a QPC timeline of the SPU PUTs into the range and the
  label window, the walker's labels / acquires / references, the reference's publication and polls, the flip, and the
  backend's records (with a hash of the texture as the walker passed the draw), harvests and binds; tl.py / cyc.py):
  a SPURS job (image 41, the kernel; LS pc 0x048C0 / 0x049B0 / 0x04AE0) PUTs a 64x64 Y16_X16 map with its seven levels
  (16384 + 5456 + 4 bytes) into one of the two buffers each frame, alternating: a double-buffered water height map. 1127
  bursts in 40 s of the pan, 15-33 us each, on the five SPURS workers.
- **The ordering:** every burst comes 1.17 ms (min; p10 1.21, p50 1.48, p90 2.00) after the walker passed the frame's
  NV406E SET_REFERENCE, which the PPU's main thread polls (it sees the value 3 us after the walker publishes it) before
  it puts the flip and starts the next frame. Probe (TX1_REFDELAY_US=4000: each reference published 4 ms late): the
  bursts moved to 5.2-6.1 ms after the walker's reference and stayed 1.2-2.1 ms after the publication (fps 66 -> 53:
  the reference is on the frame's chain). So the game rewrites buffer A for frame N+2 only once the RSX is past frame
  N's reference -- after every draw of frame N that samples A. On a PS3 the reference is written after the commands
  before it are done: libgcm's cellGcmFinish is SetReferenceCommand + a poll of the reference (T2); nouveau's nv10-nv40
  fences are the same REF_CNT, polled before a buffer is reused (T4). The write is ordered after the draws: no race on
  hardware; frame N's water reads the old map.
- **What went wrong:** the walker records a frame's draws and passes its reference long before render_frame replays the
  last batch -- at the flip, which the PPU puts only after it has seen the reference -- and the textures are validated
  at the replay. The job's PUT lands ~1 ms after that replay starts: before its harvest in ~90% of frames, and the
  frame's last water draws got the map of frame N+2 (record-time oracle: 300 uploads + 2114 binds sharing them, of
  ~14k binds of the two textures in 40 s of the pan, held bytes written after their draw); after it in the rest, and
  they got the old map -- the PF3_VERIFY "stale" binds: all 35 / 42 of two traced pans held exactly the bytes the
  texture had when the walker passed the draw. The cache's "known race" was the case it got right; the deviation was
  the opposite one, which the oracle (bytes at the bind) could not see. Within a frame the first water pass (an
  earlier batch) and the last one used maps two frames apart.
- **Census** (TX1_TRACE: every re-validation of a written main-memory texture; pan, gate, street): six textures -- the
  two NV3089 copy targets 0x47400B80 / 0x47784B80 (the walker's copies; SPU jobs rewrite them in place too, ~8M PUTs in
  a cold boot), the SPU lighting buffer 0xCF800000 (written after the labels 0x850 / 0x860, acquired through 0x890
  before the sun pass), the two water maps, 0x4738FA80 (a hot page, never changed). None is rewritten after a
  texture-read label (0x1D74: ~75 a frame on 0x800-0x840, ring markers) while its draws are pending.
- **Fix (TEX_FENCE, default 1; `=0` = the old path, no hook):**
  - At a completion point the PPU or an SPU can wait on -- SET_REFERENCE, a back-end label (0x1D70) -- with records
    pending, after the reports' delivery and before the point is visible (rsx_commands.c, g_rsx_tex_fence_hook), the
    backend harvests the write watch and keeps the round.
  - The next record starts a new batch (tex_fence_split, a mid-frame flush), so every record of a fenced batch precedes
    its fence; its replay begins no round of its own while that harvest is the latest (tc_round_ensure): each slot is
    judged by the writes up to the fence; a write after it is the next batch's harvest's, where the draws after the
    fence are.
  - A slot the replay must read again (a write since it was last read) could also be rewritten after the fence, before
    the replay. Found by a walk over the pending records' sampled units (tex_fence_capture): a slot up to
    TEX_FENCE_COPY_MAX (256 KB) or over a page the hot rule leaves dirty has its bytes copied at the fence and is
    validated and uploaded from the copy (tc_fz_src); a larger one is watched -- the fence's harvest reset its pages,
    so at the replay the write watch says whether anything wrote it since (tc_wt_written_now): nothing, the guest
    memory is the bytes at the fence; something, counted ([TEXFENCE] "rewritten after the fence"; 0 in every run).
    Without this the first build still let a texture written on both sides of the fence reach the draws before it:
    the water in the boat polycut, whose only water draws are in the frame's last batch, and after the world load,
    where the maps are rewritten every loading frame and their pages are hot (96 / 38 / 24 binds in three cold boots).
  - Copying every such slot was exact too, but the street's 0.9 MB lighting buffer copied every frame (34 us, on the
    PPU's path: it waits for the reference) cost ~0.4% fps (street, 6 rounds: 62.16 / 62.30 with the copy, 62.53
    without, 62.44 old); watched it costs a GetWriteWatch at the replay (street, 6 rounds: 62.88 / 62.82 (A/A) / 62.91
    without the copy / 62.89 old). TEX_FENCE_COPY=0: no copy, no watch (the first build, for the A/B).
  - In the game's frames the reference comes right before the flip: the fence's harvest replaces the replay's (no extra
    harvest, no split; 113-143 us, now before the PPU sees the reference); the walk takes 11-22 us (1 us at the gate);
    the copies are the water maps (22 KB) when written since their last read.
  - Not fences: texture-read labels (0x1D74, ~75 a frame between draws: a harvest and a split at each would cost
    milliseconds a frame; the census above), NV406E semaphore releases (the front end: not after the draws on hardware).
- **Exactness** (PF3_VERIFY now also hashes, at a batch's fence, every texture its records can sample and compares each
  bind of a record before the fence with those bytes; tx1/ver.sh, the final exe, the seven scenes, defaults vs
  TEX_FENCE=0 interleaved):
  | scene | binds before a fence compared | differ: TEX_FENCE=1 / =0 | clean, memory changed after the fence (PF3's old "stale") =1 / =0 |
  |---|---|---|---|
  | pier still | 2.57M | **0** / 0 | 1 / 1 |
  | pier pan | 2.23M | **0** / 216 | 7 / 29 |
  | gate fight | 56k | **0** / 0 | 0 / 0 |
  | busy street | 2.60M | **0** / 0 | 1 / 1 |
  | 2x render scale, pan | 2.25M | **0** / 100 | 9 / 17 |
  | AA fast + sharpen 25, pan | 2.23M | **0** / 168 | 12 / 15 |
  | cold boot (checkpoint 03, title, load, polycut, walk) | 2.25M | **0** / 31 | 1 / 0 |
  14.2M binds, 0 differing; the clean binds of unfenced batches 0 stale in every run; no table miss or span mismatch;
  0 splits, 0 fenced batches that harvested after all; 2433 watched slots in the cold boot, 0 rewritten after their
  fence; no D3D12 error. The old rule's differences are the water maps (and one copy target in the cold boot); its run
  of the first build had 8 on the still, 183 on the pan. The last column is PF3's old metric: with the fence those are
  the binds that correctly do not see a write released by the fence.
- **Measured** (tx1/campaign.sh = pf5's, 4 interleaved rounds, gate fight and busy street; new2 = defaults again, the
  A/A): gate 48.70 (new) / 48.42 (new2, the A/A) / 48.34 (old) fps, street 62.03 / 62.22 / 62.27: new and new2 against old
  gate +0.7% / +0.2%, street -0.4% / -0.1%, within the A/A's 0.6% / 0.3%; present p95 21.87 / 22.09 / 22.01 and
  16.99 / 16.91 / 16.87 ms. The street over 6 rounds (tx1hy): 62.88 / 62.82 (A/A) / 62.89 (old), 62.91 without the
  copy (TEX_FENCE_COPY=0). The first build (no copy, no watch): gate 48.65 / 48.51 / 48.54, street 62.19 / 62.15 / 61.74.
- **Identity** (tx1/idall.sh: the committed exe a427c8d7 x2 against the final x2 and the final with TEX_FENCE=0, gate /
  street / pier, rs2/idcmp.py + ss1/idpost.py aligned on the load): the first differing post-load frame is 4 / 4 / 5
  (gate / street / pier) base1 vs base2, 4 / 4 / 5 for the final x2 (one pier run's dump line was cut by another
  thread's output, so its alignment slipped; re-run: 5), 4 / 4 / 2 with TEX_FENCE=0 (the pier's re-run: 5 -- the
  scenes' drift); artefact descriptions 181 / 223 / 222, 0 different (the gate's TEX_FENCE=0 run logged 154 more PSO
  lines, none different).
- **Save / load:** the pier's F5 / F9 / menu (saved, loaded, saved, loaded), five F5 / F9 cycles walking (5 / 5), that
  quick slot loaded at launch with TEX_FENCE=0 and at the defaults; no D3D12 error, FATAL or WATCHDOG.
- **Tests:** tests/test_tex_cache_rsx.c, with a writer THREAD released by the fence: the plain case (400 frames: 0 binds
  holding later bytes with the fence's harvest, 323 without), the copy (a write on both sides of the fence, 200 frames
  on normal pages and 200 with hot loading rounds: 0 with the copy, 200 reading the guest memory), the watched case
  (a 1 MB slot, a write after the fence every other frame: tc_wt_written_now right in 200 of 200, the memory the bytes
  at the fence in the 100 without); no write missed by the next batch. alltests rc 0 (unit's "strict-pass share" line
  is the known non-gating print), manifest gate 3981 statics, 0 unclassified.
- **Still read at the replay** (as reviewed, below): a copy target (RSX-kept, over the cap) that changed before its
  fence -- hashed at the fence and compared, so a write after the fence is counted, not undone (0 in every run); a
  texture with no slot at the fence that the no-slot table could not take (64 entries, 1 MB a fence: 61-153 a run,
  all in the first frame after a load or of the 3D title scene); a watched slot rewritten after the fence (counted:
  0 in the seven scenes; 2-3 in a cold boot with its movies, the FMV's luma plane when not hot -- page-granular: the
  oracle found those binds' bytes the fence's); a fence over the per-frame budget (the old rule, counted: 0).
- **Review** (the main TX1 review; tx1/rev/p_rev1-5.py, p_test4.py; final exe a8fc8498):
  - *Eviction (#1):* a slot the fence copied, watches or hashed is kept through its batch's prepass (tc_fz_protected
    in tc_alloc_slot), taken only when nothing else is free (counted "evicted": 0 in every run).
  - *A fence that lapses (#3):* the fence's state is rsx_tex_cache.h's tc_fence (tc_fz_open / _judge / _mark /
    _active / _protected / _forget, the backend's and the unit test's). tc_fz_judge records the batch it holds; one
    that lapses (a record after it, a harvest since) moves its serial on, so its copies, watches and hashes count no
    more, and the replay reads them only for the batch the fence holds (tc_fz_active, after tc_round_ensure; "unused"
    0 in every run).
  - *RSX-kept slots (#2):* copied like any other up to the cap. The larger ones (the two copy targets, 2 a fence on
    the pier and the street) can be hashed at the fence (TEX_FENCE_RSX_HASH=1): at the replay one whose hash at
    the fence is the slot's own needs no read (the fence's hash stands in; 3-86% of them), another is hashed again
    and compared (counted "rewritten after the fence": 0 in every run). The RSX's own copies into one before its
    replay are also counted from cellGcmSys's write notes (free; 0). The watch path would reset the pages the RSX
    copies rewrite every frame (a soft fault a page); not taken. Cost: walk + copies 22 -> 85 us a fence on the
    street (harvest ~129 us as before); street A/B, 4 rounds (tx1rh): hash on 62.16 / 62.38 (A/A), off 62.44, old
    62.39. The hash only counts (a change is not undone) and counted 0 everywhere, so after the review it is a
    diagnostic, default off: the copy targets are read at the replay, as before TX1, and their RSX copies counted.
    The final exe (d7f70f10, the hash off): the seven scenes 0 binds differing from their fence (TEX_FENCE=0: pan 84,
    2x scale 100, AA 168, cold boot 30); A/B 4 rounds gate 48.64 / 48.39 (A/A) / 48.77 (old), street 62.39 / 62.44 /
    62.47 -- within the A/A; identity against a427c8d7 as base vs base (gate 4, street 5, pier 5; a street dump that
    lost a frame line re-run twice: 5, 5); save / load in play, alltests rc 0, manifest gate 3987 statics, 0
    unclassified.
  - *Fence storms (#4), measured first* (pre-review exe, tx1/rev/storm.sh: a cold boot with every movie, then the
    pier's pause menu 20 s): one fence a guest frame everywhere -- the title (75/s), the splash Bink and the v_02 FMV
    (30/s; 2-4 copies a frame, 1.2-1.8 MB: the movie's planes), the loading screen, the 3D title scene, gameplay, the
    paused game (60/s, 1 watched); one split in the whole boot. FRAME_TRACE (final exe): fences a frame max 3, mean
    1.00 (3 frames with more than one); tex-fence splits 1 a cold boot, 0 in the pause run. fps per phase, TEX_FENCE=1
    / =0, two interleaved rounds, final exe: title 75 / 75, splash Bink 29.96-30.01 all, loading 31.35 / 31.31 vs
    31.50 / 31.39, 3D title scene 73.14 / 72.71 vs 73.57 / 73.43, v_02 FMV 29.97 all, paused 65.63 / 64.89 vs 66.26 /
    66.26 (pre-review exe: 66.79 / 69.16 vs 67.47 / 69.37; single 19 s windows -- three more interleaved rounds of
    the pause alone: 69.95 TEX_FENCE=1 / 69.33 with TEX_FENCE_RSX_HASH=0 / 69.53 TEX_FENCE=0). Then: at most
    `TEX_FENCE_MAX` fences (default 8) open a guest frame, over it the old rule (counted "budget": 0 except the
    TEX_FENCE_TEXREAD smoke); FRAME_TRACE columns fl_texfence (tex-fence splits, no longer in fl_capacity) and
    texfences.
  - *Hot slots over the cap (#4):* first watched after a reset of their pages, as asked -- on that build (c4c48662)
    the v_02 FMV's 900 KB luma plane (0x407C2C00 / 0x40CC2C00, hot) was rewritten after the fence at every other
    fence (723 in a cold boot; the pre-review copy: 0). So a hot slot over the cap is copied while the fence's such
    copies fit `TEX_FENCE_HOT_MAX` (KB, default 4096; the movie ~1.3 MB a frame), above that reset and watched
    (counted "hot": 0); the cold boot with its movies: 1297 copied, 0 differing binds (PF3_VERIFY, below).
  - *#5:* a texture with no slot at the fence (first bound in the fenced batch; not a render target's) is copied by
    key (tc_fznew, up to the cap; 64 entries, 1 MB a fence) and the replay that creates its slot reads the copy:
    1-74 a run, all read (one of 59 in the cold boot went to a batch that never validated).
  - *#6:* a watched slot is asked before and after the validation's hash and after the conversion. *#7:* the per-unit
    dedupe compares the whole key (levels, pitch, cube). *#8:* the texture packs hash the bytes the slot was read
    from (tp_note_upload, tp_bind_unit). *#9:* cellGcmSys's resync rescue calls the hook before it publishes a
    rescued reference; `TEX_FENCE_TEXREAD=1` (default 0) makes texture-read labels fences: on the street 8 a frame
    then the budget (8975 fences, 91396 over it), every one a split, PF3_VERIFY 0 differing of 2.23M, no error.
    *#10:* tests/test_tex_cache_rsx.c drives tc_fz_* (the lapse by a record and by a harvest, the eviction victim, the
    serial, a hot slot reset and watched).
  - *Re-verified (final exe a8fc8498):* PF3_VERIFY + the fence oracle, TEX_FENCE=1 / =0: pier still 2.57M binds, 0 /
    8 differing; pan 2.25M, 0 / 84; gate 56k, 0 / 0; street 2.60M, 0 / 8; 2x render scale 2.24M, 0 / 76; AA fast +
    sharpen 25 2.23M, 0 / 24; cold boot 2.21M, 0 / 30; the cold boot with its movies 344k, 0 / 13 -- 14.5M, 0; no
    D3D12 error. A/B (tx1rv, 4 interleaved rounds): gate 48.73 (new) / 48.82 (new2, A/A) / 48.71 (old), street 62.48 /
    62.27 / 62.68 (-0.3% / -0.65%, A/A 0.34%; the copy-target hash is ~0.3% of it -- now off by default, above).
    Identity against a427c8d7 (post-load frames identical before the first difference; base2 / new1 / new2 / new0):
    gate 2 / 2 / 2 / 2, street 5 / 5 / 5 / 5, pier 5 / 5 / 5 / 5 (re-run: three runs whose dump line another
    thread's output cut, and street new1 at 4); artefact descriptions 0 different (street: both TEX_FENCE=0 runs and
    one of the three new1 runs lack one PSO line base1 has). Save / load in play as before, 0 errors; alltests rc 0;
    manifest gate 3987 statics, 0 unclassified.
- **Seen, not examined:** an RSX copy (NV3089 / NV309E / NV308A) writes guest memory as the walker passes it (it reads
  back a pending source first, not a pending reader of its destination), so a draw before it in the same batch that
  sampled its destination would read the copy at its replay -- the same shape in the FIFO's own order; not looked for.
## 2026-10-28 (early morning) — SS1: a save state loaded at launch no longer fails -- two boot-time races in the save-state engine (forced at their interleavings: 120 of 120 launch loads failed before, 0 of 120 after)
EBOOT: BCUS98125 update 1.04 | ps3recomp runtime/savestate/ss_state.c / .h (the capture order built once under a lock,
into a copy published whole; ss_state_order_check), ss_core.c (load_checks: the order whole before the point of no
return), runtime/ppu/ppu_fs.cpp (its FSFD part added at static init), tests/test_ss_state.c (the order under
concurrent first use), tests/test_ss_fsfd.cpp (FSFD's layout whole before boot); config/ss_manifest.txt
- **Goal:** a save state loaded at launch (`tools\run.ps1 -LoadState`, the test scripts' FAST BOOT) sometimes failed
  past its point of no return, exit 3, with one of two messages: `the image has no park record for the main thread`
  (pf5's identity run street_new0d) or `SPU threads: SPU 0x2000 runs with no lifted context (a PPU-fallback run): not
  resumable` (1 of 116 PF5 loads). Loads during play never had. Find the cause from evidence; a launch load must never
  die and must give the state a normal load gives.
- **The failing log** (street_new0d against a good load of the same state): the restore printed TIME's re-anchor and
  GPUP's surface pool, but not FSFD's `[ss] load: open files` line, and MISC's trophy merge ran **twice**
  (`[sceNpTrophy] Loaded trophy data` twice after the teardown, once in every good load); the SPU threads came back on
  the boot world's contexts (the same ctx pointers as at boot; a good load hands out the image's) and 0x2006 not at
  all. ss_state_restore walks the capture order (s_ord): it held MISC twice and lacked PPUT, FSFD and the SPU sections.
  The SPU form is the same with SPUG restored and SPUC not (SPUG leaves every thread without a context, SPUC hands
  them out).
- **Cause 1, the exit 3s:** the capture order was sorted on its first use (sort_sections: s_ord filled in registration
  order, insertion-sorted in place, then s_nord set) with no lock. At a launch with SS_LOAD that first use comes from
  two threads at once: the state's reader (load_reader -> ss_file_load -> ss_file_check_compat_ex ->
  ss_section_count) and the frame clock building the window's menu bar (rsx_winmenu_create -> fill_slots ->
  ss_slots_read_info -> ss_file_check_compat_ex), which checks the header of each slot file that exists. Both wait in
  ss_slots_identity for the EBOOT hash (id_thread, ~12 ms) and are released by the same event. A second sorter
  entering while the first is mid-insertion finishes the order; the first's pending shift then lands in it: one
  section twice, the one it overwrote gone -- for the whole session (s_nord >= 0 is never sorted again): teardown,
  restore and every later capture use it. Only a launch with SS_LOAD and a slot file present has two first users (the
  scripts' n1/slots has quick + undo); in play the first use is the menu at boot or the first F5, alone, so a load
  during play cannot meet the race -- but a session whose launch load broke the order without dying (below) saved
  and loaded on it.
- **Measured** (local work files: loop.sh, launch loads in a loop alternating fm_after_boat / gate_fight, ExitAfter 30
  frames, ~4 s each; classified died / loaded / not loaded, and a loaded run checked for FSFD's line once and one
  trophy merge):
  - the base exe (48410080), no slot file: 300 of 300 loaded.
  - the base exe, a slot folder with quick + undo (copies of the two test states): 300 loaded, 0 died, **1 loaded on a
    broken order** (run 152, gate_fight: MISC restored twice, so some section that logs nothing was skipped).
  - a diagnostic build (who sorts and when, the order load_run runs on, what the restore restored; 200 runs): the
    menu (frame clock) sorts first in every run; the reader's first call comes 4.7-194 us later (median 63, p10 21):
    inside the few us of the sort rarely (0 of 200 here, 1 of 300 above; PF5's rate was higher on a busier machine).
  - the race at its interleaving (diagnostic builds only, local p_wide.py: the first thread into the sort waits
    400 us in its last insertion -- the registry's last section, TIME, moving to the front -- before shifting position
    J, as a thread preempted there would): the original code at J=4 ran on `TIME TIME MAPS GMEM KMEM KOBJ SPUG ...`
    (PPUT lost) and **50 of 50 launch loads died: "the image has no park record for the main thread"**; at J=6 (SPUC
    lost) **50 of 50: "SPU threads: SPU 0x2000 runs with no lifted context"**. The fix with the same wait: **100 of 100
    loaded**, every section restored once. (A wait at the same point in both threads overlapped the two sorts in 35 of
    35 runs and broke nothing: the damage needs the one-sided interleaving.)
  - the unit test (tests/test_ss_state.c, its new last phase: 40 registrations, 8 threads released at once to the first
    use after each): the original sort 8 of 8 runs FAIL (every thread handed a broken order, e.g. "RC39 twice"), the fix
    20 of 20 PASS.
- **Fix 1** (ss_state.c): the order is built under a lock (s_ord_lk; registration takes it too) into a private array,
  copied to s_ord and only then published (s_nord, an interlocked store); a caller that finds it built reads it after a
  full barrier. Whenever the old code did not race the order, and so every save and load, is what it was. Before the
  point of no return (ss_core.c load_checks), ss_state_order_check -- every registered section in the order once, by
  `order` -- refuses a load otherwise (not-resumable, "the save-state engine": nothing discarded, the game goes on; a
  launch load ends "SS_LOAD NOT loaded") instead of tearing down a world it cannot rebuild. It cannot fire now; it
  would name a regression. The post-discard checks (main's park record, SPU contexts) stay as the last word.
- **Cause 2, found in the after runs** (1 launch load of ~1400 this session, on the first fix exe, independent of
  it): `SS_LOAD NOT loaded: made by another build: section FSFD (open files) is laid out differently` -- a clean
  refusal, no exit 3, but the launch load did not happen. FSFD's layout hash covers the field lists of its fd-table
  parts, and ppu_fs added its part at boot (ppu_fs_register, main, after ss_slots_init started the reader); in that
  run the EBOOT hash finished before main reached ppu_fs_register (the hash line before the guest dispatch: 1 of
  ~1200 runs) and the reader checked a layout without ppu_fs's fields. Forced (diagnostic only, ss1/p_fsdelay.py:
  main held 100 ms before ppu_fs_register): **20 of 20 refused** with that message; with the part added at static
  init **20 of 20 loaded**. **Fix 2** (ppu_fs.cpp): the part is added at static init (a C++ namespace-scope
  initializer, as cellFs.c's part and every SS_SECTION_REGISTER); ppu_fs_register keeps the write-open check.
  test_ss_fsfd.cpp checks the layout holds ppu_fs's fields before ppu_fs_register and that registering does not move
  the hash (2 FAILED on the old ppu_fs.cpp, passes).
- **After** (exe c97f4821, both fixes): **300 of 300 launch loads with the slot files present, 300 of 300 without**
  (before: 300 + 300 loaded, 1 of them on a broken order), every one restoring every section once (FSFD's line once,
  one trophy merge). Identity (pf5/idrun.sh: the first 16 presents dumped, RSX_RES_ARTLOG, n1/slots; the base exe x2,
  the fix x2, on gate / street / pier): the dumps compared by their place after the load (local idpost.py -- the
  load comes after 7-10 boot frames, so frame numbers do not line up): the first 3 frames after the load
  byte-identical in all 9 pairs; the first differing one is post-load frame 3-5 base vs fix and 4-5 base vs base
  (PF5's own two street runs, before SS1: 3) -- the scenes' run-to-run drift; the artifact fingerprints identical in
  every pair but those with gate base1 (the first run of the batch: 154 more PSO lines, 1-2 descriptions differing,
  against base2 as against the fix). In play: the pier (pf1/steps/ssio.txt) F5 saved, F9 loaded, the menu's
  Quick Save / Quick Load saved and loaded; five F5 / F9 cycles while walking (pf4/ssio_walk.txt) 5 saved, 5 loaded;
  that quick slot loaded at launch on the walk route; no FATAL, WATCHDOG or NOT. alltests rc 0 everywhere (test_ss_state
  with the race phase, test_ss_fsfd with the layout checks); manifest gate 3970 statics, 0 unclassified.
- **Review** (one read-only pass): the publication sound (InterlockedExchange / CompareExchange are full barriers
  under clang-cl; s_ord written once, under the lock, before s_nord; the SRW lock constant-initialised, no init-order
  hazard), the order unchanged when no race happens (same stable sort, same key, every section's `order` distinct),
  fix 2 leaves FSFD's lists `{file, dir, ppu_fs}` either way; four small points, fixed:
  - A section registered after the order was built rebuilds it in place under walkers that read s_nord unlocked
    (unsafe; none does today -- all 24 are static init, the tests' late ones single-threaded): the header now says
    static init only, and a late registration is logged once.
  - The order check ran only inside the stop, where NOT_RESUMABLE is retried until give_up_s (60 s of stops at a
    launch): it now runs first in rq_precheck for every request (a save too: it would write an image without the
    missing sections) and ends the request at once -- a broken order does not get better; load_checks keeps it.
  - Its refusal path had no test: ss_state_order_check_list (the check over a given order) and a test_ss_state phase
    -- a section missing, twice, unknown, two swapped -- each refused, the restored order accepted.
  - Older than SS1: ss_slots_identity copied the identity even when the EBOOT hash had not finished in 5 s (a cold
    disk, a scan), possibly mid-write. id_thread now hashes into a local and publishes it whole; a copy made before
    it is ready has no SHA-1 (have_sha1 0, logged once) and the menu does not cache a verdict made with it; a load
    waits up to 60 s for it.
  Re-verified (exe a427c8d7): 150 of 150 launch loads with slot files present and 150 of 150 without, every section
  restored once; identity from the load on against SS1's base1 of the committed exe: the first 3-5 post-load frames
  byte-identical (gate 4 / 4, street 5 / 3, pier 5 / 5 -- the scenes' drift); in play the pier's F5 / F9 / menu, 5
  F5 / F9 cycles walking, that slot loaded at launch; the unit tests (the new phase: four refusals), alltests rc 0,
  manifest gate 3972 statics, 0 unclassified.
- **Seen, not changed:** the menu bar is built on the frame clock at boot and waits there for the EBOOT hash (~12 ms)
  when a slot file exists; crc_init (ss_state.c) is also first-use initialised without a lock, but two builders write
  the same table and SSE4.2 machines never read it.

## 2026-10-27 (late night) — PF5: the walker's converted-vertex cache admits a range on its second sighting and empties the slabs a few hot ranges keep (gate fight +3.7%, +6.0% at the HLE timing, busy street +8.5% over a 3-minute walk, +1.4% over 12 rounds; the overflow gone: 0 of 26 vs 20 of 42 runs)
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video/rsx_vertex_fetch.c / .h (VCACHE_ADMIT2: the first-sighting filter and the
table's recency victim; VCACHE_EVAC: slab emptying; VCACHE_VERIFY; VCACHE_STATS), rsx_d3d12_backend.c (the switches,
[VCSTAT], [VCVERIFY], the vc_* FRAME_TRACE columns, the load reset), tests/test_vcache_admit2.c (new);
config/ss_manifest.txt
- **Goal:** PF4's next lever: the walker's converted-vertex cache "overflows" in some runs (gate 9 of 10 HLE runs, 4 of 8
  with the fifth SPU at the street), converting hundreds to thousands of draws a frame that would fit, and with the fifth
  SPU the walker sets the frame. Fix it exactly (nothing the game computes or shows changes) and measure it.
- **Harness** (local work files): PF4's scenes from their states (pf4/rgs.sh, unchanged: gate_fight.p3ss; fm_after_boat.p3ss
  + the marais2 walk), pf5/r.sh (FRAME_TRACE now with vc_hits / vc_admits / vc_nofit / vc_bypassed / vc_recycled /
  vc_held / vc_moved per frame), pf5/campaign.sh (interleaved, rotating), pf4/ftsum.py, pf5/ovsum.py (per run: vertex
  emission, nofit / held / hits per frame; "overflow" by PF4's vertex-emission threshold, "frozen" by nofit >= 20 a
  frame, "diverged" when 30+ frames draw under 3/4 of the run's median).
- **Diagnosis** (`VCACHE_STATS=<n>`, new, diagnostic: a census of the cache every n presents -- per pinned slab the
  ranges a frame still hit in it, the bytes in hot / cold / never-hit / orphaned copies, the table's live and hot
  entries, admissions by cause, and a set of every range key seen with its sighting count; the old path, 3 runs a scene):
  | | gate fight | busy street |
  |---|---|---|
  | region full, every slab pinned | 5-9 s after the load (census 5-7 of every 60 presents); no admission from 9-20 s on | 4-5 s after the load; no admission from 5-6 s on |
  | after that | admits 0, nofit 1200-4500 draws a frame (1053-3729 mean): recurring ranges converted every frame | admits 0, nofit 134-184 a frame |
  | what the 256 MB held | hot (a frame hit it) 4-10 MB, cold live 43-47 (never hit 39-43), **orphaned 198-206** | hot 14-22 MB, cold live 78-86 (never hit 67-76), **orphaned 153-155** |
  | pinned slabs by the hot ranges in them (1 / 2-4 / 5+) | 57-75 / 61-70 / 116-134; hot KB per slab p50 5-7, p90 35-69 | 12-18 / 27-31 / 207-217; p50 23-34, p90 77-109 |
  | table (32768 entries) | 31.7-32.1k live, 2.3-4.1k hot; during the fill **36-69k live entries forgotten** per 60 presents | 25.8-26.4k live, 4.7-6.4k hot; 52-59k forgotten per 60 presents |
  | range keys in 30 s | 1.04M, ~950k seen once (2.25 GB of ranges) vs 123-194 MB of recurring ranges | 340-347k, 245-251k seen once (1.16-1.18 GB) vs 405-413 MB |
  What pins the slabs: inFamous 2 draws ~1000 ranges a frame at the gate (~90 on the street) that it never draws again
  -- geometry written to new addresses every frame. The old path admitted every range on its first sighting: their copies
  filled the slabs around the few ranges the frames still hit (each hit re-stamps its whole slab, and a slab is recycled
  only when no range in it was used for 16 batches), and their live entries filled the table, whose full probe windows
  then forgot slot 0 -- hot ranges among them -- at every admission; a forgotten hot range was admitted again into the
  slab being filled, which it then kept. Within 4-9 s of the load every slab held a hot range and nothing could be
  admitted for the rest of the run; which hot ranges were inside by then decides the run's cost (PF4's "timing-
  dependent" overflow; VCACHE_MB 512 / 1024 only delay it, the table being the same 32768 entries).
- **Built** (both default on, each with a kill switch; `VCACHE_ADMIT2=0 VCACHE_EVAC=0` = the old path):
  - `VCACHE_ADMIT2` (rsx_vertex_fetch.c, rsx_vc_get): a range is admitted on its second sighting. A key the table does
    not hold goes to a direct-mapped filter of first sightings beside the table (2^17 64-bit keys, 1 MB): not there, it
    is written there and the draw takes the stream path (the path a range that does not fit always took: no hash, no
    slab, no entry); there, the range is admitted. And an admission whose probe window holds only live entries takes the
    least recently used one no draw used for min_age batches (each entry's `last`: admission, hits, moves, bypassed
    sightings -- in what was the entry's padding, 48 bytes either way); with none it is refused (the stream path)
    instead of forgetting slot 0.
  - `VCACHE_EVAC`: when fewer than 1/8 of the slabs (at least 2) are free or recyclable as the allocator opens one, or
    none is, the slab whose live copies hold the fewest bytes (slab_live, per admission / move / recycling) is emptied:
    a hit on a copy in it converts the range again into the current slab (as a miss would) or takes the stream path when
    there is no room, so no draw reads the slab any more and it is recycled by the usual min_age rule. One slab at a
    time: at most 1 MB of conversions per 16 batches.
  - `VCACHE_VERIFY=1` (diagnostic): every draw the cache serves (a hit, an admission, a move) also converts its range
    afresh from the guest bytes (the stream path's converter, into cached memory) and compares it with the copy its
    indices name in the arena -- the bytes the GPU reads, read back with streaming loads -- and every write into the
    region is checked against per-16-byte stamps of the batches whose draws read those bytes: none of this batch's or
    the previous one's (render_frame waits for the GPU before it submits a batch, not before the walker records the
    next, so batch B-1 may still run while B is recorded). `[VCVERIFY]` every 100 presents; ~6-7 fps (the arena is an
    upload heap: reading it back is slow).
  - `VCACHE_STATS=<n>` (diagnostic, above); FRAME_TRACE's vc_* columns; [VPERF] counts held sightings.
  - tests/test_vcache_admit2.c (new; m4c/gcm/test_gcm_ss.sh runs it): the real cache code on a fake guest memory, 160
    frames of 15 batches drawing hot, one-shot, changing, walk (4 frames, then gone) and late (in view from some frame
    on) ranges through an 8 MB region and a 512-entry table, all four switch combinations: every served copy compared
    with the reference conversion and every draw of batches B and B-1 re-hashed at the end of B (0 differing, 0
    changed in all four); the old path freezes (3860 nofit in the last 20 frames, the walk ranges never served), admit2
    alone freezes on the late ranges' pins (2073), evac alone works but admits every one-shot (19154 admissions vs 3925),
    both: 0 nofit, every late draw and 99%+ of the walk ranges' 3rd / 4th draws served, no one-shot range ever served
    from the cache; and the oracle catches two planted faults (a corrupted byte in a copy: 1 draw / 1 byte differing;
    slabs recycled under a running batch: 15 unsafe writes, and the test's own re-hash sees the 15 draws change).
- **GPU safety** (what PF5 depends on, written down at the cache's comment): unchanged -- every write into the region is
  an admission or a move into a slab the allocator opened under min_age (>= 2: render_frame's full wait_for_gpu before
  each batch finishes B-2 before B is recorded), and every draw the cache serves stamps the slab it reads. The filter,
  the table's victim and the slab chosen for emptying are bookkeeping that never writes the arena; an emptied slab is
  not read (a hit there moves the range or takes the stream path) and ages out like any other. The state-load reset
  (after its wait_for_gpu) clears the filter, slab_live and the slab being emptied with the table.
- **Exactness** (nothing the game computes or shows changes):
  - `VCACHE_VERIFY=1` on the final exe (pf5/ver.sh, pf3/ver.sh's seven scenes on PF5's): served draws compared / bytes
    differing / region writes checked / unsafe: gate 0.97M / 0 / 8.6k / 0, street 1.01M / 0 / 37.5k / 0, 2x render
    scale 0.98M / 0 and 2.18M / 0, AA fast + sharpen 25 0.97M / 0 and 1.00M / 0, a cold boot of checkpoint 03 (the title,
    the world load, the flyover, the boat polycut, a walk) 7.34M / 0 / 260k / 0 with 695 slabs recycled; and two stress
    scenes with a 16 MB cache, gate 0.98M / 0 (16 recycled) and street 1.91M / 0 / 30.6k / 0 with 84 slabs recycled, 59
    emptied and 11410 copies moved. **17.3M served draws (39 GB), 0 differing bytes; 435k writes into the region, none
    over bytes a draw of the batch or the previous one read.** No D3D12 error or device removal. (A report every 100
    presents: the last <100 presents of each run are compared but not counted.)
  - CELLMARK identity (pf5/idall.sh: the state's load, 16 dumps; the base exe = PF4's final, md5 8b783482, swapped into
    build\ for its runs; base x2, final x2, final at the kill switch): **frames 0-3 byte-identical in all 12 pairs**
    (gate, street, pier); the first difference at frame 4-12 (gate), 5-6 (street), 5 (pier) -- base vs base too (12 / 6
    / 5); artefact descriptions 0 different (181 / 223 / 222 lines).
  - By construction: a copy is served only for the same key (plan signature with the resolved addresses, range,
    stride) and the same content hash, as before; a held first sighting and a refused admission take the stream path,
    the per-record conversion every nofit or sparse draw has always taken; a move converts the range afresh. The stream
    half cannot overflow from the extra held draws: batch_capacity_check flushes before any draw that might not fit (the
    gate's batches stay 15 a frame: 10 report deliveries, 3 capacity, 1 readback, both ways).
- **Measured** (pf5/campaign.sh: interleaved rounds, rotating order, the scene's state, 30 s after READY, FRAME_TRACE
  from READY+4 s, pf4/ftsum.py + pf5/ovsum.py; old / old2 = `VCACHE_ADMIT2=0 VCACHE_EVAC=0` twice, the A/A; the final
  exe, the fifth SPU on), 8 rounds:
  | scene | arm | fps (rounds) | present p50 / p95 / p99 ms | shown 1 / 2+ refreshes | vertex emission ms | overflow / frozen runs |
  |---|---|---|---|---|---|---|
  | gate | old | 46.22 (47.70 46.09 48.50 46.33 45.07 45.76 44.68 45.64) | 21.47 / 23.16 / 24.34 | 37.7 / 62.3 % | 5.94 | 6 / 8 of 8 |
  | gate | old2 (A/A) | 46.20 (46.70 46.38 48.48 46.79 46.21 47.08 43.90 44.04) | 21.37 / 23.64 / 25.25 | 37.8 / 62.2 % | 5.81 | 6 / 8 of 8 |
  | gate | **new** | **47.91** (48.45 48.43 48.25 48.32 48.21 47.68 47.22 46.75) = **+3.7%** | 20.53 / 22.56 / 24.39 | 43.5 / 56.5 % | 4.87 | **0 / 0** |
  | street | old | 60.61 (61.11 61.69 61.66 61.94 60.90 57.00 60.36 60.21) | 16.60 / 17.62 / 18.75 | 76.2 / 23.8 % | 4.14 | 1 / 8 of 8 |
  | street | old2 (A/A) | 59.73 (62.08 62.13 56.05 61.91 61.51 60.77 54.64 58.74) | 16.87 / 18.14 / 19.48 | 74.2 / 25.8 % | 4.34 | 2 / 8 of 8 |
  | street | new | 60.52 (61.83 61.77 61.76 61.89 61.16 59.68 55.62 60.41) | 16.58 / 18.03 / 19.97 | 74.9 / 25.1 % | 4.01 | **0 / 0** |
  A/A: gate 0.04%, street 1.5%. ("Overflow": PF4's vertex-emission threshold, gate > 5.3 ms, street > 4.4; "frozen":
  nofit >= 20 draws a frame -- every old run.) The gate: no old run reached the new arm's 47.9 but two "cheap" frozen
  runs (48.50 / 48.48: ~1200 nofit a frame instead of 2500-5700); every new run 892 first sightings held, 10741 hits,
  0 nofit a frame. GPU 1.48 ms both ways. The street: the old path's frozen state is usually cheap there (135-180
  nofit a frame, vertex emission 3.85-4.05 ms, the same as the new arm's 3.86-4.04); its overflowing runs (3 of 16:
  54.64 / 56.05 / 57.00, 2700-3900 nofit) are what PF5 removes, a mean effect below this campaign's resolution. The
  new arm's 55.62 (round 7) and 59.68 had no cache event out of the ordinary (held ~110, nofit 0) and every walker
  block ~10% slower (render_frame 5.76 vs 5.18 ms, vertex emission 4.32 vs 3.88, readback 2.06 vs 1.89): the machine;
  round 7 was the slowest round in every arm. 4 more interleaved street rounds (new / old only, run after a pause):
  new **62.07** (62.04 / 62.20 / 61.78 / 62.27; p50 / p95 / p99 16.31 / 17.06 / 17.91 ms), old 60.24 (61.43 / 62.19 /
  62.11 / 55.21, the last overflowing; 16.83 / 17.63 / 18.77): no slow new run. Pooled over the 12 street rounds the
  new arm 61.04 vs the old 60.18 (+1.4%); overflowing runs 0 of 12 vs 4 of 20.
  HLE timing (SPU_TASKSET_LLE=0 both arms, 3 rounds): gate old 42.52 (42.42 / 42.41 / 42.72, all overflowing) -> new
  **45.09** (45.15 / 45.50 / 44.64) = **+6.0%**, present p50 23.44 -> 21.90 ms, p99 26.10 -> 25.29; street old
  57.73 (58.70 / 57.92 / 56.56 -- the third re-run after its first try failed at the state load, below; it overflowed)
  vs new 57.10 (58.10 / 57.82 / 55.38, the last another slow-machine run): within the noise.
  Overflowing runs over everything above (census off, the final exe): gate 0 of 11 new vs 15 of 19 old; street 0 of
  15 vs 5 of 23; every old run "frozen" (all 42), no new one.
- **Stability / save states** (the final exe): 3-minute street walks at the defaults and at `VCACHE_ADMIT2=0
  VCACHE_EVAC=0`: exit 0, no SPU fault, D3D12 error, device removal or watchdog line; **63.6 vs 58.6 fps (+8.5%),
  lowest second 59.1 vs 55.0** -- the kill switch's walk overflowed all the way (3119 draws a frame nofit, vertex
  emission 5.23 ms), the defaults' never (110 first sightings held, 0 nofit, 3.85 ms); no error-like line only in
  either (pf1/errdiff.py: the offline cellNetCtl notice and a [SEMA] statistics line). F5 / F9 and the menu's Quick
  Save / Quick Load at the pier (pf1/steps/ssio.txt): saved, loaded, saved, loaded; five F5 / F9 cycles while walking
  the street (pf4/ssio_walk.txt): 5 saved, 5 loaded; that quick slot loaded at the kill switch (63.0 fps over 30 s of
  walking) and at the defaults (64.2), load pause 193-194 ms. A load clears the cache with its filter, slab_live and
  the slab being emptied (s_vc stays RESET in the manifest); the two test states load in every campaign run.
- **Tests:** m4c/integ/alltests.sh: unit, kernel, spu_host, spu_load, fsfd, gcm (now with test_vcache_admit2, all
  passed), libs rc 0 (unit's "strict-pass share" and "save #38 ... ppu-timeout" lines are the known non-gating prints,
  as in PF4's run); manifest gate 3967 statics, 0 unclassified (the six new ones classified: vc_stats_every.v and
  vc_verify_on.on BOOT, s_vcst, vc_stats_report.n, vc_ftr_events.last, vc_verify_report.n DIAG; s_vc's line names
  what PF5 added to it). The unit test also passes under AddressSanitizer. The cache header's comment was corrected
  after the verified build (the fill time and the orphaned range above, same lines): rsx_vertex_fetch.c compiles to
  the same object against both versions (pf5/cmtcheck.py), so the verified exe stands for the final sources.
- **Rejected / measured** (each on the street / gate census, then the unit test):
  - Markers in the table (the first build: a first sighting leaves its key in an entry with no live copy): the street's
    table filled with live entries of ranges drawn a few frames and passed (32768 live, 3082 hot): no room for a marker,
    nothing admitted, nothing recycled -- 4200 draws a frame held, 54.4 fps (61.3 before).
  - Markers that may take a cold entry (slab stamp): 61.1 fps, nofit 0, but ~330 sightings a frame held for ~90 one-shot
    ranges (one-shot markers overwrote recurring ranges' fresh markers before their second sighting), and pinned slabs
    grew 62 -> 93 in 100 s of walking.
  - The separate filter with a slab-stamp victim: the cold entries sit in slabs hot ranges keep, so full windows still
    forgot slot 0 ~130 times a frame on the street; 115 -> 0 recyclable slabs in 25 s, then nofit again.
  - PF4's "recycle the oldest GPU-safe slab (age >= 2) at a bounded rate": a new GPU-safety margin (2 batches instead of
    16) and, with ~1000 one-shot admissions a frame at the gate, every forced recycle re-converts the hot ranges in the
    slab; emptying under min_age compacts the same way without either.
  - Emptying alone (VCACHE_ADMIT2=0 VCACHE_EVAC=1): works in the unit test but admits every one-shot range (a hash and
    a whole-range conversion each, ~1000 a frame at the gate) and keeps the table full.
- **Review** (one read-only pass over the diff): no stale-copy, GPU-safety, kill-switch, robustness or threading
  defect; four small ones, fixed:
  - Evac's slab, once aged, was one free slab among others: opened later, a draw of a copy it still held stamped it
    again and the emptying was wasted. rsx_vc_next_free now returns it first once it has aged (the unit test's
    evac-alone run: 120 slabs emptied, 458 moved, 34852 draws served vs 119 / 472 / 34805; the other three runs
    unchanged).
  - admit2: a new key whose probe window holds only ranges the frames still use is refused before the allocator check
    and the content hash (it was refused after paying for both); nothing between changes the table.
  - [VPERF] (PERF=1) zeroed the cache's counters, so the census, FRAME_TRACE's vc_* columns and [VCVERIFY] miscounted
    with PERF on (none of the campaign runs had it): [VPERF] now prints from a baseline of its own
    (render_frame_impl.vcb, DIAG) and the counters only grow.
  - VCACHE_VERIFY stamps a served copy before comparing it, so a comparison skipped for want of memory still guards
    the copy; `_Static_assert` pins the entry at 48 bytes.
  - Left as is: evac also picks a slab when every pinned slab is densely hot -- churn only when the hot set nears 7/8
    of the region (a small VCACHE_MB; 4-22 MB of 256 here).
  Re-verified on the fixed exe (md5 48410080): the unit test (all four switch combinations, both planted faults);
  `VCACHE_VERIFY` over the nine scenes **22.6M served draws, 0 differing bytes; 398k region writes, 0 unsafe** (the 16 MB
  stress runs: 6 + 71 slabs emptied, 520 + 2545 copies moved); identity frames 0-3 byte-identical in 11 of 12 pairs and
  in 3 re-runs of the 12th (the street at the kill switch first differed at frame 3 once: <= 2 levels on 3.3% of the
  pixels, in the box base vs base differs in from frame 6 -- its re-runs first differ at 5 and 5); 4 interleaved rounds:
  gate 47.02 vs 45.44 at the kill switch (+3.5%; overflowing 0 of 4 vs 4 of 4), street 62.44 vs 60.62 (+3.0%; one old
  run overflowed, 56.14; p99 17.66 vs 18.45 ms); a 3-minute street walk 64.1 fps, lowest second 60.0; F5 / F9 / menu at
  the pier, 5 F5 / F9 cycles while walking, that slot loaded both ways; alltests rc 0; manifest gate 3968 statics, 0
  unclassified.
- **Seen, not PF5's:**
  - 1 of 116 state loads this session (0 of PF4's) failed at the game's first frame: `[ss] FATAL: load request #1
    failed past its point of no return: SPU threads: SPU 0x2000 runs with no lifted context (a PPU-fallback run): not
    resumable` (exit 3; SPU_TASKSET_LLE=0 and the old cache path, nothing drawn yet). A save-state load racing an SPU
    thread in a PPU-fallback run -- to look at in the save-state engine. In the review's re-runs a second form, also at
    a load right after launch (the kill switch's cache path): `the image has no park record for the main thread`
    (exit 3, 1 of 4). Loads during play (F9, the menu) have not failed.
  - With `VCACHE_STATS` on (the census scans the 96 MB key set every 60 presents: a walker stall of tens of ms every
    second), 2 of 3 gate runs on two interim builds left the fight's usual view after 5-7 s (draws 9550 -> 2200-4000
    a second); without the census 0 of 4 on the same build, 0 of 11 new and 0 of 19 old gate runs on the final exe,
    0 of 38 of PF4's; on the final exe with the census on, 0 of 6 (3 new, 3 old; pf5/divcheck.sh). Not reproduced
    and not explained -- the census is a diagnostic; the gate fight is unscripted (no input), and a stall of that size
    may change what its AI does.
- **Next lever:** the walker without the overflow: vertex emission ~4.8 ms a frame at the gate (it idles ~1.5 ms on the SPU
  lighting) and ~4.0 on the street, where the cache's own remaining cost is the content hash of every hit (~10.7k /
  11.7k a frame), which only write tracking harvested per FIFO segment could skip (PF3); ~890 / ~110 first sightings a
  frame take the stream path, as they must. Then PF4's list: the gate's 10 report deliveries a frame (deliver_n +
  d3d12_report_measure), the street's render_frame and readback.

## 2026-10-27 (late night) — PF4: the fifth SPU -- the psarc task gives its SPU back while it waits (gate fight +7.4%; busy street +4.3% over a 3-minute walk, the walker's vertex cache decides it)
EBOOT: BCUS98125 update 1.04 | ps3recomp runtime/spu/spu_channels.c (spu_taskset_lle_on / _configure,
spu_taskset_pm_call: a task's syscall into the policy is a call; a task taken up from the policy drops the streamed
overlay; TASK_PUTLOG), spu_drain.c (a warning line for an interrupt taken in task code), spu_dma.h (TASK_PUTLOG),
tests/test_spu_task_residency.c (new); src/main.cpp (the title opts in); config/ss_manifest.txt
- **Goal:** raise the frame rate of the two heaviest known scenes, the prologue's gate fight and New Marais's busy
  street, without changing anything the game computes or shows. Last measured 2026-10-21: gate ~37-39, docks ~74, busy
  street ~54 loops/s; since then M5, M6a, P2, PF1-PF3.
- **Harness** (local work files): both scenes start from save states in savestates\test (FAST BOOT pattern, READY 0.7 s,
  a run ~33 s): `gate_fight.p3ss` (pf4/mkgate.sh: the old perf2 gate route -- a new game, prologue step 11, the
  scripted walk 0.8-14.5 s after READY -- saved at READY+15 s, where the old window R+15..R+40 starts; Cole at the
  gate, ~9580 draws, no input after the load) and `fm_after_boat.p3ss` + the old marais2 walk (left stick forward,
  right stick 150) for the busy street (~9220 draws; the old window's 9270-9330). Both reproduce the old scenes'
  draw counts; the gate from its state runs at the old route's rate within ~5% (46 in the route run at R+15..40, 43-44
  loaded: the walker's vertex cache, below). pf4/rgs.sh / r.sh / prof.sh / ct.sh; quit by wall time
  (CHECKPOINT_EXIT_AFTER=30s).
- **Profile, today's build** (PF3 final; threadcpu per thread over 18 s, WALKER_STATS, FRAME_TRACE, GPU_TIME,
  HOST_SAMPLE_ALL + HOST_TIMELINE at the gate; per frame):
  | | gate (43.8 fps, 22.8 ms) | busy street (59.6 fps, 16.8 ms) |
  |---|---|---|
  | SPU workers 0x2000-0x2004 | four at 20.1-20.3 ms (88%), **the fifth 0** | four at 14.1-14.6 ms (85%), the fifth 0 |
  | the fifth SPU | parked all run in the psarc decompressor task's WAIT_SIGNAL (20% of the five's samples) | the same (0x2003 in one run: the busy 0x2004 then ran on the shared cores) |
  | walker (frame clock) | 18.0-19.0 ms CPU, busy 87-91%: **2.1-3.0 ms idle on the SPU lighting semaphore**, GPU waits 1.64 | 14.8-15.1 ms CPU, **busy 93%**: 0.56 ms on the lighting semaphore, 0.58 on the PPU's put, GPU waits 0.51 |
  | walker's blocks | render_frame 6.0 (15 batches: 10 report deliveries, 3 capacity, the readback, the present), vertex emission 4.9-6.4, readbacks 2.05, texture validation 1.04 | render_frame 5.07 (5 batches), vertex emission 3.9, readbacks 1.83, texture validation 0.88 |
  | PPU main | 10.1-11.0 ms CPU; **56% of its time in the WWS barrier waits** (CHAIN_TRACE accumulators), 9% in the 0x13C280 poll, ~0 in the light-kicker wait: game code ~8 ms | 7.3-9.0 ms CPU |
  | Edge Geom Task Kicker | 4.1-5.2 ms CPU (51-61% kernel), shared cores | 1.4 ms |
  | GPU | 1.5 ms (15 lists) | 1.3 ms (5 lists) |
  | **sets the frame** | **the SPU job pool**: the main thread waits on SPU job barriers more than half its frame, the walker waits for the SPU lighting 2-3 ms; the pool has four of the game's five SPURS SPUs | **the walker** (93% busy), the SPU lighting and the PPU's put within ~1 ms |
  - The fifth SPU: inFamous 2's only taskset task, the psarc decompressor (taskset 0x47D7E380), waits for requests in
    WAIT_SIGNAL. The runtime's HLE syscall parks that SPU's host thread until a signal; Sony's taskset policy module
    saves the waiting task and gives the SPU back to the SPURS kernel, so on a PS3 the WWS job queues (max contention 5)
    run on five SPUs. `SPU_TASKSET_LLE=1` (the lifted policy module, opt-in, "default off until verified") does that:
    single runs, gate 48.6-48.7 fps (+11%; five workers at 16.1-16.5 ms, the walker 92.5% busy, its lighting wait
    1.5 ms), busy street 58.9-62.7 (the walker 99.8% busy). On 10-21 it lost 9% at the gate: the kicker then spun ~96%
    of a core and the CPU was oversubscribed (M6a and PF1 since).
  - The walker's converted-vertex cache: vertex emission varies by run, 4.9 vs 5.5-6.4 ms at the gate and 3.9 vs 5.8
    at the street: in some runs the cache overflows ("nofit": 280-380 draws a frame at the gate, converted every
    frame, VPERF) though the frame's ranges would fit -- every 1 MB slab keeps a range some frame still hits. VCACHE_MB
    512 / 1024 removes the gate's nofit but not the street's (548 / 984 / 1741 nofit per 20 frames at 256 / 512 /
    1024 MB), and 70-130 ranges a frame miss at the gate whatever the size. Also the main source of run-to-run noise in
    walker-bound views.
  - CHAIN_TRACE writes a row per main-thread light-kicker wait; at the gate (and at the pier after a state load) that
    wait no longer happens -- the lighting is done before the main thread asks --, so rows stop after the load and only
    the accumulated barrier / poll times are left (above).
  - Levers in order: (1) the fifth SPU (SPU_TASKSET_LLE), the PS3's own scheduling; (2) the vertex cache's overflow
    (walker, exact: a content-hashed cache); (3) a core of its own for the fifth worker once it is busy (PF1 placed
    0x2000-0x2003); (4) the gate's 10 report deliveries a frame (deliver_n + d3d12_report_measure ~10% of the walker's
    samples).
- **The policy path was not safe as it stood.** An oracle first: `TASK_PUTLOG=<file>` (new, diagnostic) logs every MFC
  PUT the task's own code makes -- the decompressor's output -- with an FNV-1a 64 of the bytes; a cold boot of
  checkpoint 03 (the world load, the city flyover, the boat polycut's start: the decompressor's busiest time, READY on
  PlayPolycut). HLE twice: 29838 PUTs, 276.2 MB, the same contents. SPU_TASKSET_LLE=1: every boot stalled at the end
  of the world load (5 with an SPU FAULT line, one silently): one SPU branched to LS 0 ("[SPU] tid=0x2004 stopped ... FAULT", ran out at pc 0 with r1 = 0xC0), the task
  stayed "running" in its taskset (running / ready bits set) on a dead SPU, its last 568-572 outputs (9.1 MB) never
  came, the city-intro movie slot never finished and the boot sat in the flyover: READY "quiet" at 32 s, no polycut.
  Gameplay from a state did not show it in 30 s runs. Two residency bugs the HLE never exercised, both found with a
  per-SPU control-transfer ring (SPU_FLOWRING), a context log (a temporary TASK_CTXLOG of the taskset's DMA, syscalls
  and resumes; dropped again) and an LS watch on the saved-context line (SPU_LS_WATCH=0x2C80):
  1. **A policy-answered syscall's return lost the task's residency.** The task's POLL (syscall 3; wrapper at 0x95D0
     called from 0x32D0) tail-branches to the policy's entry at 0xA70; the policy answers and returns to the task's
     link (0xB60: restore the registers, `bi $lr`) -- correct, the watch shows 0x32D4 stored and read back. But
     spu_indirect_branch clears resident_task for every dispatch below the task region (0x3000), and the return came
     back through the task's bracket drain (pc == its return pc), so the task ran on with resident_task 0. Its next
     computed branch at depth > 0 then matched the "the policy restored a saved task" test (`ti && !resident_task &&
     host_depth`) and longjmp'd to depth 0 over its own live frames; a few returns later the guest stack ran out (r1
     0xC0) and it branched to LS 0. Fixed first in spu_drain_call (a drain returning into the task region with the
     residency it was entered with cleared gave it back); the review found that blind to the bisl-shaped WAIT_SIGNAL
     (below). **The fix: spu_taskset_pm_call** (spu_channels.c): a task's syscall into the policy runs as a nested
     call -- the policy runs with no task resident; when it returns to the task's link the residency comes back; when
     it switches away (0x808, the stack reset) or resumes a task it longjmps past the call. Whatever the call's shape
     (a tail branch from a wrapper, as POLL; a bisl straight to 0xA70, as WAIT_SIGNAL).
  2. **A policy resume left a WWS job module as the resident overlay.** With the fix above, 1 boot in 3 still stalled
     silently: the task spun in the dispatcher on job-module code (jobmod_fe4956b0b503 at 0x6D80) with task 1
     resident -- under the policy the SPUs that run WWS jobs also run the task, the dispatcher resolves a task address
     the task's lift has no entry for (a computed branch into a function) through resident_ovl next, and the job
     module that ran before was still named there though the task's ELF had replaced it. **Fixed at the TaskInfo
     resume** (the policy restored a task and branches into it): resident_ovl = 0. A miss then falls to the base image
     and the interpreter (SPU_INTERP_UNLIFTED, on in play and test runs), as with the HLE, where the task's SPU never
     held an overlay. The same where a task is taken up at host depth 0 (review). It assumes a task never runs with a
     streamed overlay of its own (inFamous 2's one task has none).
  Both apply only with the policy module (spu_taskset_lle_on); SPU_TASKSET_LLE=0 runs the old path unchanged.
- **Built** (default on for inFamous 2; `SPU_TASKSET_LLE=0` = the old HLE path, the kill switch):
  - `spu_taskset_lle_on()` / `spu_taskset_lle_configure()` (spu_channels.c): the decision, once, at the first query;
    the toolkit's default stays the HLE, a title opts in; inFamous 2 does in main.cpp before the frame clock starts.
    `SPU_TASKSET_LLE=1 | 0` overrides (anything else is logged and ignored). `[spu] taskset tasks: ...` says which.
  - the two residency rules above (spu_channels.c: spu_taskset_pm_call; the overlay drop at both resume paths);
  - `[irq] taken in taskset task code: ...` (spu_drain.c, at most 4 lines): an interrupt taken with a task resident
    would come back through the false-resume path (review; pre-existing, never seen);
  - `TASK_PUTLOG=<file>` (diagnostic; routes every DMA to spu_dma.h's diagnostic path);
  - tests/test_spu_task_residency.c (new; m4c/spu/spu_tests.sh runs it in both modes; the "policy" a co-resident
    region at 0xA00 as pm_30023680 is): A a POLL-shaped call (a wrapper's tail branch to 0xA70) and C a WAIT_SIGNAL-
    shaped one (a bisl to 0xA70) -- in the policy the residency is 0, back in the task 9 (policy module) / 0 (=0); B
    no task resident; D a call that stays in the task region; E C then a computed call into the task region: a call,
    not a resume; F / G a policy resume at depth > 0 / 0 with a job module left as the overlay: the base image's code
    runs (policy module) / the job module's (=0, the old path).
- **Exactness** (the game's own results unchanged):
  - The task's output: 13 policy-module cold boots of checkpoint 03 after the fixes (6 and 3 on two interim builds, 4 on
    the final): no SPU fault, the polycut reached, **29838 PUTs each, the content multiset of every PUT
    of 1 KB or more identical to the HLE's** (the HLE's own two boots identical too; addresses and small bookkeeping
    PUTs order differently between any two boots). The gate route's cold boot (a new game, prologue step 11 by
    IF2_TASKJUMP: a second world load): 25151 PUTs in each of 4 policy-module runs and the HLE run, the same contents.
    The street walk from its state (30 s, before the fixes): 390 PUTs / 4.0 MB, the same contents both ways. Nothing the task computes changes; where and when it
    runs does -- on any of the five SPUs, scheduled by Sony's own policy and kernel code, as on a PS3.
  - Scheduling only otherwise: the WWS jobs now run on five SPUs (max contention 5, the game's own setting); every
    ordering between them, the PPU, the RSX and the task is the game's own synchronisation (WWS barriers, the kicker
    labels, the taskset's signals and LFQueue), unchanged.
  - CELLMARK identity (pf4/idall.sh: the state's load, 16 dumps; the base exe = PF3 final swapped into build\ for its
    runs): pier 7 runs (base x4, the final x2, its kill switch), gate 5 (base x2, final x2, kill switch): **frames 0-3
    byte-identical in all 12**; frame 4 differs base vs base (one pair) and in one of the two policy runs per scene --
    a step of a fade on a near-black boot frame (at most 2/255, inside (394,210)-(884,508), the same difference two
    consecutive frames of one run show); frames 5+ timing-dependent as before. Artefact descriptions: 0 different
    (pier 222 lines; gate: every 181-line run alike, base1 logged 154 more PSO lines, every shared key the same).
- **Measured** (pf4/campaign.sh: interleaved rounds, the scene's state, 30 s after READY, FRAME_TRACE from READY+4 s,
  pf4/ftsum.py; off / off2 = SPU_TASKSET_LLE=0 twice, the A/A). c1 on an interim build (on = the policy module with
  the fifth worker placed too, on4 = with PF1's four: the final's defaults), 3 rounds; c2 on the final exe, 2 rounds:
  | scene | arm | fps (rounds) | present p50 / p95 / p99 ms | shown 1 / 2+ refreshes | GPU ms |
  |---|---|---|---|---|---|
  | gate | c1 off | 42.86 (42.23 / 43.66 / 42.68) | 23.11 / 24.91 / 26.80 | 25.0 / 75.0 % | 1.51 |
  | gate | c1 off2 (A/A) | 43.11 (43.20 / 42.63 / 43.50) | 23.27 / 24.65 / 26.72 | 26.1 / 73.9 % | 1.51 |
  | gate | c1 on | 47.66 (46.75 / 48.38 / 47.84) | 20.68 / 22.55 / 24.74 | 42.6 / 57.4 % | 1.51 |
  | gate | c1 **on4** | **47.77** (47.89 / 48.33 / 47.10) | 20.59 / 22.56 / 24.65 | 43.1 / 56.9 % | 1.51 |
  | gate | c2 off / off2 (A/A) | 42.99 (43.01 / 42.97) / 43.89 (45.59 / 42.19) | off: 23.34 / 24.75 / 26.80 | off: 25.5 / 74.5 % | 1.51 |
  | gate | c2 **on (final)** | **46.07** (45.44 / 46.70) | 21.56 / 22.73 / 24.80 | 37.3 / 62.7 % | 1.51 |
  | street | c1 off | 59.22 (59.25 / 59.17 / 59.23) | 16.69 / 17.93 / 18.62 | 73.4 / 26.6 % | 1.31 |
  | street | c1 off2 (A/A) | 59.27 (59.34 / 59.18 / 59.29) | 16.67 / 17.94 / 18.63 | 73.5 / 26.5 % | 1.30 |
  | street | c1 on | 60.24 (62.05 / 60.26 / 58.40) | 16.60 / 17.60 / 18.61 | 75.4 / 24.6 % | 1.30 |
  | street | c1 **on4** | **61.88** (61.83 / 61.91 / 61.90) | 16.34 / 17.07 / 17.83 | 78.8 / 21.2 % | 1.31 |
  | street | c2 off / off2 (A/A) | 59.07 (58.95 / 59.19) / 59.18 (59.14 / 59.22) | off: 16.80 / 17.78 / 18.63 | off: 73.0 / 27.0 % | 1.31 |
  | street | c2 **on (final)** | **60.21** (58.26 / 62.15) | 16.70 / 17.78 / 18.83 | 75.4 / 24.6 % | 1.30 |
  A/A: gate +0.6% (c1), +2.1% (c2: one round without the overflow below), street +0.1 / +0.2%. All runs pooled (c1
  ran an interim build; the final exe's own A/B is after the review, below): gate 43.17 (HLE, 10 runs) -> 47.30
  (policy, 8) = +9.6%; street 59.20 -> 60.85 = +2.8%. The spread in the policy
  arms is the walker's converted-vertex cache (pf4/matched.py: a run "overflows" when its median vertex emission is
  above 5.3 ms at the gate / 4.4 ms at the street): at the gate 9 of 10 HLE runs and 4 of 8 policy runs overflowed
  (policy without it 47.10-48.38, with it 45.44-47.84); at the street 0 of 10 HLE and 2 of 8 policy runs (58.26 /
  58.40; without it 60.26-62.15, mean 61.68 = **+4.2%**). With the fifth SPU the walker sets the frame again (gate:
  busy 92.5%, its wait for the SPU lighting 1.5 ms a frame, was 2.1-3.0; street: 99.8%, no lighting wait left), so
  the cache's overflow now costs frames directly. Per thread at the gate: five workers at 16.1-16.7 ms a frame each
  (were four at 20.1-20.3), the walker 16.9-17.0 ms CPU, PPU main 10.3-10.5 ms. Longer: a 180 s street walk at the
  defaults 63.3 fps (lowest second 58), at SPU_TASKSET_LLE=0 60.1 (58), +5.3%. The boots are shorter too: checkpoint
  03's polycut starts at 24.0-24.2 s (13 boots; HLE 27.2-27.5 s, 3 boots), the gate route's READY at 20.7-21.0 s (HLE
  22.5).
- **The fifth worker's core** (the interim build's default, c1's `on`: walker + five SPU workers on cores of their own, every other
  thread on 2 cores): gate 47.66 vs 47.77 with PF1's four, street 60.24 vs 61.88 (its 58.40 round overflowed): no
  gain to see; HLE with five placed (HOST_PLACEMENT_SPUS=5, 2 rounds) +0.5% street / +1.1% gate, noise. Not kept: PF1's
  placement stands (the fifth, unplaced worker shares cores 0-2).
- **Stability / save states** (the final exe): 180 s street walks at the defaults and at SPU_TASKSET_LLE=0 -- exit 0,
  no SPU fault, D3D12 error, device removal or watchdog line, no error-like line only in the default run
  (pf1/errdiff.py). F5 / F9 and the menu's Quick Save / Quick Load at the pier (pf1/steps/ssio.txt): saved, loaded;
  five F5 / F9 cycles while walking the street (pf4/ssio_walk.txt: saves with the task anywhere -- waiting in the
  policy, running on some SPU): 5 saved, 5 loaded; the quick slot that left (saved under the policy module) loaded at
  SPU_TASKSET_LLE=0 and at the defaults, 30 s of walking each: 63.4 fps, no fault. The two test states (saved under
  the HLE, the task parked on its SPU) load at the defaults in every campaign run; the parked task's WAIT_SIGNAL is
  re-issued at the load and goes to the policy module.
- **Tests:** m4c/integ/alltests.sh: unit, kernel, spu_host (now with test_spu_task_residency, both modes), spu_load,
  fsfd, gcm, libs rc 0 (unit's "strict-pass share" line is the known non-gating print); manifest gate 3959 statics,
  0 unclassified (the new ones classified; spu_indirect_branch.s_lle became the file-scope s_taskset_lle).
- **Rejected / measured:**
  - The fifth worker's core: above.
  - VCACHE_MB 512 / 1024 (single runs, PERF): the gate's nofit goes (0) but 70-130 ranges a frame still miss; the
    street's nofit rises (548 / 984 / 1741 per 20 frames at 256 / 512 / 1024). Not a fix for the overflow.
  - CHAIN_TRACE for the profile: its rows are written at the main thread's light-kicker waits, which a loaded state's
    gate and pier no longer have; the barrier / poll accumulators were used instead.
- **Review** (one read-only reviewer; probes in pf4/review). **High, fixed:** the drain rule (spu_drain_call gave the
  residency back on a return into the task region) could not see the task's WAIT_SIGNAL: a `bisl` straight to 0xA70
  (0x9C7C; reached from the LFQueue pop's wait callback, `bisl $r80` at 0xA66C), whose dispatch clears the residency
  before its bracket's drain records it. A WAIT_SIGNAL the policy answers at once -- the signal already latched, which
  Sony's policy returns from without a switch -- would have left the task unresident and its next computed call
  (`bisl $r6` at 0xA28C) would take the false resume: the stream decompressor dead, a hang. Not seen in 17 boots (a
  latent race); probe_bisl.c showed the frame lost. Fixed by making the syscall a call (spu_taskset_pm_call, above);
  the drain rule went (spu_drain.c is its pre-PF4 self but for the warning below). **Low, fixed:** the overlay drop
  was only on the longjmp resume; a task taken up at depth 0 (a save state's run parked inside the policy) kept a
  job module named (probe_ovl.c ran the job module's code) -- now dropped there too. **Low, watched:** an interrupt
  taken in task code would come back through the iret's dispatch unresident (pre-existing; the task now shares the
  WWS jobs' SPUs): a capped `[irq] taken in taskset task code` line -- 0 in every run below. **Low, accepted:** a
  task the policy saved, loaded with SPU_TASKSET_LLE=0, is resumed once by the real policy with the overlay drop off
  (kill switch only; the HLE then parks it). Docs: the headline pooled c1's interim build; the HLE-saved test states'
  parked task re-issues its WAIT_SIGNAL at the load and it goes to the policy module (not "the HLE until its next
  wait"). Cleared: the policy gives the SPU back through 0x808, a stack reset (a longjmp to depth 0), so job modules
  never run inside the task's frames and host depth stays bounded; the task's other syscalls (POLL, YIELD) are
  brsl-plus-tail, EXIT never returns; SPU_INTERP_UNLIFTED is on in play (main.cpp) as the overlay drop needs;
  TASK_PUTLOG is only on spu_dma.h's diagnostic path; spu_taskset_lle_on's CAS; the manifest.
- **After the review** (rebuilt; pf4/rev/reverify.sh, exe 8b783482): the unit test 15 / 15 (policy module) and 13 / 13
  (=0); against PF4's pre-review runtime it fails C, E and G (the WAIT_SIGNAL shape, the false resume after it, the
  depth-0 overlay drop), =0 unchanged. Task output: 4 cold boots of checkpoint 03, 29838 PUTs each, contents identical
  to the HLE's, polycut at 24.0-24.1 s; 2 gate-route boots, 25151 PUTs, identical; 0 SPU faults, 0 interrupts in task
  code. CELLMARK identity (base x2, final x2, final at =0, per scene): frames 0-3 byte-identical in all, the first
  difference at frame 4 (pier, base vs base too) / 5 (gate); artefact descriptions 0 different (the final's pier runs
  logged 1-2 fewer PSO lines: compile order). A/B, 3 rounds interleaved, the final exe:
  | scene | off / off2 (=0, the A/A) | policy module | vertex emission (policy) |
  |---|---|---|---|
  | gate | 42.18 / 42.55 | **45.52** (45.87 / 45.26 / 45.41) = **+7.4%** | 5.6-5.7 ms (all runs overflow, as 9 of 10 HLE runs) |
  | street | 59.29 / 58.81 | 56.77 (55.93 / 57.45 / 56.94) = -3.8% | 4.9-5.5 ms: **all 3 overflowed** (HLE: 3.8-3.9) |
  The street depends on the converted-vertex cache: pooled over every campaign (pf4/matched.py), 16 of 16 HLE runs
  never overflow (59.1-59.4 fps); policy-module runs at the final defaults that do not overflow run 60.3-62.2 (+3 to
  +5%), those that do 55.9-58.4 (-1 to -6%), 4 of 8 overflowed (c1 0 of 3, c2 1 of 2, this campaign 3 of 3; the
  logs of an overflowing and a clean run differ in nothing else: overlays, syscalls, no interpreter). 3-minute street
  walks (the defaults vs =0): 60.3 vs 57.8 fps (+4.3%; lowest second 58 vs 55), 0 faults, errdiff clean. Save / load:
  F5 / F9 / the menu at the pier saved and loaded; five F5 / F9 cycles while walking: 5 saved, 5 loaded; that slot
  loaded at =0 (63.6 fps) and at the defaults (62.5). alltests rc 0 (the new test both modes), gate 3961 statics, 0
  unclassified.
- **Next lever:** the converted-vertex cache's overflow -- with the fifth SPU it decides the busy street. Its slabs are recycled only when no range in them was hit for
  16 batches (~1 frame at the gate's 15 batches), so ranges whose content changes or that are seen once (the gate
  converts 70-130 a frame whatever the size) leave dead copies in slabs that other, hot ranges keep alive; once every
  slab holds one hot range the overflow converts its draws every frame (gate 4.9 -> 6.5-7.0 ms of the walker's
  frame, street 3.9 -> 5.5). Exact candidates: admit a range on its second sighting (a one-shot range never takes
  space), or recycle the oldest GPU-safe slab (age >= 2 batches) when none is old enough, at a bounded rate (compacts a
  fragmented cache, bounded thrash otherwise). Then the walker at the street (99.8% busy with the fifth SPU: vertex
  emission, render_frame, the readback) and the gate's 10 report deliveries a frame.

## 2026-10-27 (night) — PF3: the walker's per-draw work; the RSX's own copies stop faulting on write-watched pages (+7.9%)
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video/rsx_d3d12_backend.c (RASTER_MEMO, SAMP_KEEP, the RSX write note,
PF3_VERIFY), rsx_tex_cache.h (TEX_WW_RSX, TC_HARVEST_LOG; tests/test_tex_cache_rsx.c, new); config/ss_manifest.txt
- **Goal:** cut the FIFO walker's per-draw CPU cost at the heavy pier still view (~58 fps, walker ~95% busy: the frame
  clock) without changing anything the game computes or shows.
- **Profile, still view, base build** (HOST_SAMPLE on the frame clock, 25 s, 17.9k samples at 61 fps = 16.4 ms
  frames; each RVA resolved to its source line: pf3/dbgobj.sh compiles the file with the build's own flags plus DWARF
  line tables -- debug info only, the instruction sequence of every profiled function compared identical with the
  exe's -- and pf3/lines.py maps self samples and call-site samples to lines). Per frame:
  | walker work | ms | of it |
  |---|---|---|
  | vertex emission (rsx_vemit_run) | 3.5 | conversion 0.81, dedupe 0.57, cache lookup + content hash 0.56, distinct count 0.34, index swap 0.26, emit 0.15, plan 0.15 |
  | replay, 5 batches (render_frame) | 4.5 | texture validation 1.12 (**GetWriteWatch harvests 0.55**), D3D calls ~0.75 (SetPipelineState 0.25, DrawIndexed 0.18, Execute 0.15), **sampler sets 0.52 (CreateSampler 0.27)**, RT lookups 0.27, PSO lookup 0.14 |
  | G-buffer readback + 4 NV3089 copies | 2.8 | the flush 0.92, GPU waits 0.48, stores 0.07, **copy memcpy 1.02** |
  | constants and record (vp_record_cb) | 2.0 | FP constants (vp_record_fp + scan memo) 0.71, the bank / reuse logic 0.43, **RASTER_LOG's per-draw hash 0.29 + raster 0.15**, ROP plan 0.07, bank copy 0.09 |
  | FIFO decode + state methods | 2.0 | the dispatch, transform constants 0.40 |
  | record fill (16 units, VS, blend, target) | 0.6 | |
  | idle | 1.05 | ~1 ms a frame on the SPU lighting's semaphore |
  The copy: 2 surfaces (1280x720, 7.4 MB) a frame to main memory for the SPU jobs; with TEX_WW=0 its memcpy is 0.27 ms.
  The 0.75 ms difference is soft faults: the RSX samples those main-memory copies too, so the texture cache tracks
  their pages and resets their write watch after each frame's report (written once a frame: never "hot"), and the
  walker's next copy faults on each of ~1800 pages -- before the label the SPU lighting waits for, i.e. on the
  critical path. CHAIN_TRACE: label -> lighting done 5.8 ms; the walker's shadow cascades run under it with ~1 ms to
  spare, so per-draw savings there are hidden; the G-buffer pass, the readback and the passes after the lighting
  are the critical path.
- **Built** (all default on, each with a kill switch; `RASTER_MEMO=0 SAMP_KEEP=0 TEX_WW_RSX=0` = the old path):
  - `TEX_WW_RSX` (rsx_tex_cache.h, tc_wt_rsx_write; the backend's rsx_write_note_hook on cellGcmSys's
    g_gcm_rsx_write_note, which the texture packs used alone): a page the RSX's own copies (NV3089 / NV309E / NV308A)
    wrote within the last 16 rounds is left dirty at the harvest. Dirty, it is reported by every harvest, so a write by
    any other thread is still seen; the slots over it are re-hashed at each bind (re-uploaded every frame anyway:
    "re-hashed same" stayed 6 per 20 batches). Copy memcpy 1.02 -> 0.19 ms.
  - `SAMP_KEEP`: sampler sets survive across batches. A set's descriptors are a pure function of its 16 codes
    (append-only border palette; pack shifts and the store's anisotropy are in the codes), so a kept set holds what
    building it again would write. A new set takes a free slot, else the least recently bound set no draw of this
    batch binds (the GPU finished earlier batches at render_frame's wait); the heap is "full" only when this batch
    binds all 127 -- when the per-batch heap was -- and the stand-in then searches only this batch's sets.
    samp_set_for 0.52 -> 0.22 ms, no CreateSampler in steady state; a load drops the sets (s_samp_use).
  - `RASTER_MEMO`: raster_record_draw keeps the previous record's outputs (rs_vp, rs_k, rs_own, the route, RASTER_LOG's
    xd / od / em) with every input byte (guest registers, frame, viewport rect); identical inputs copy them. Logging
    unchanged (a repeat of the last key never prints; the statistics count every record). 0.45 -> 0.12 ms.
  - `PF3_VERIFY=1` (diagnostic): each shortcut also runs what it replaces on the same draw and compares what reaches
    the GPU -- the raster bytes (viewport, epilogue lanes, scissor) and route; the sampler codes of the bound set
    against the set the per-batch heap would have bound (simulated beside it); every texture the write watch calls
    clean is hashed against its slot's hash. `[PF3VERIFY]` every 600 presents. `TC_HARVEST_LOG=1`: what a harvest
    scans against the tracked pages.
- **Exactness:**
  - PF3_VERIFY on the final build: per scene, raster records / sampler-set draws compared (bytes differing) and clean texture binds
    hashed (stale): pier still 21.5M (0) / 21.6M (0), 2.29M (1); pan 13.7M (0) / 13.8M (0), 1.73M (10); 2x render
    scale still 16.1M (0) / 16.2M (0), 1.72M (0) and pan 13.6M (0) / 13.7M (0), 1.73M (8); AA fast + sharpen 25 still
    16.1M (0) / 16.2M (0), 1.72M (1) and pan 13.7M (0) / 13.8M (0), 1.73M (8); a cold boot through the title, the
    loading and the boat cutscene with a walk 23.6M (0) / 23.8M (0), 2.77M (0). **118M raster records and 119M
    sampler-set binds, 0 differing bytes**; the old per-batch heap was never full. No D3D12 error or device removal.
  - The "clean but different" textures are two 64x64 main-memory textures (0x47C72900 / 0x47C77E80) a concurrent writer
    rewrites every other frame around frames 107-115 of the pan, and one at the load (frame 8, 0x47C77E80): a write
    after the round's harvest, caught next round, under the old rule as well (TEX_WW_RSX=0 pan: 6 in 2.07M, the same two textures and the one at the load) -- the cache's known race
    with concurrent writers, not PF3's; none of them is on a page the RSX copies.
  - tests/test_tex_cache_rsx.c (new, real write-watched arena): 0 stale clean validations in 350 with the rule on and
    off (a copy a frame + random other writes); resets while copying 75 -> 2; the pages are reset again after the copies
    stop.
  - CELLMARK boot identity (pf3/idall.sh; base exe swapped into build\ for its runs): frames 0-4 byte-identical in all
    10 pairs of 5 runs (base x2, the defaults x2, PF3 off), frames 5+ timing-dependent as before (new vs new differ at
    5 too); 222 artefact descriptions, 0 different.
- **Measured** (pf3/campaign.sh = pf2's, 3 interleaved rounds, FAST BOOT pier, ftsum7; off / off2 = PF3 off twice,
  the A/A):
  | scene | arm | fps (rounds) | present p50 / p95 / p99 ms | shown 1 / 2+ refreshes | GPU ms |
  |---|---|---|---|---|---|
  | still | off | 57.81 (57.80 / 57.78 / 57.84) | 16.93 / 18.61 / 18.99 | 70.2 / 29.8 % | 1.27 |
  | still | off2 (A/A) | 57.43 (57.83 / 57.56 / 56.89) | 17.04 / 18.75 / 19.07 | 69.4 / 30.6 % | 1.27 |
  | still | **on** | **62.35** (62.05 / 62.46 / 62.54), +7.9% / +8.6% | 15.76 / 17.14 / 17.84 | 79.7 / 20.3 % | 1.27 |
  | pan | off | 68.64 (68.47 / 68.74 / 68.71) | 13.70 / 18.32 / 20.15 | 90.6 / 9.4 % | 0.87 |
  | pan | off2 (A/A) | 68.50 (68.47 / 68.64 / 68.37) | 13.66 / 18.33 / 20.07 | 90.4 / 9.6 % | 0.87 |
  | pan | **on** | **70.16** (70.14 / 70.07 / 70.27), +2.2% / +2.4% | 13.52 / 17.34 / 18.95 | 93.1 / 6.9 % | 0.81 |
  A/A: still -0.7%, pan -0.2%. One lever at a time (still, one round each; off 57.81): TEX_WW_RSX alone
  60.56, RASTER_MEMO alone 58.30, SAMP_KEEP alone 58.05 fps -- together +4.5 fps, more than the +3.5 of the three
  apart: with the copies' faults gone the per-draw savings fall on the critical path. [RBT]: the readback flush 1.03 -> 0.94 ms (its GPU wait 0.53
  ms, unchanged). WALKER_STATS at the defaults (the HOST_SAMPLE run, 65 fps): busy 91-93%, idle ~50 ms/s on the
  lighting's semaphore and ~34 ms/s on the PPU's put (base: 93%, 60 / 7 ms/s): the walker now waits on both of its
  producers; at the still view it is no longer the only clock.
- **Stability:** a 177 s steady pan at the defaults: exit 0, 70.4 fps mean, 63 the lowest second after the
  first; a 102 s pan at the defaults and a 107 s one with PF3 off (70.2 / 68.6 fps): no error-like log line only in
  either (pf1/errdiff.py). F5 / F9 and the menu's Quick Save / Quick Load in one run (pf1/steps/ssio.txt): saved,
  loaded (pause 129 ms), saved, loaded, a shot after each; the fast boot's own load: pause 206 ms. No D3D12 error,
  device removal or watchdog line in any run.
- **Tests:** m4c/integ/alltests.sh: unit, kernel, spu_host, spu_load, fsfd, gcm (test_gcm_ss_load 47,
  test_rsx_report 117), libs rc 0 (unit's "strict-pass share" line is the known non-gating print); manifest gate
  3955 statics, 0 unclassified (the new ones classified; raster_record_draw's and samp_set_for's moved to
  raster_record_draw_full / samp_pick); tests/test_tex_cache_rsx.c (build line in its header) all passed.
  After the review fixes (rebuilt): see the review bullet.
- **Rejected / measured:**
  - A general hot-page rule (any tracked page dirty 3 times within 16 harvests is left dirty): 0.67 ms better than none,
    0.38 ms worse than TEX_WW_RSX -- textures bound in every batch over once-a-frame pages are then re-hashed at every
    bind (PERF + FRAME_TRACE, single runs: 18.90 / 18.23 / 17.85 ms).
  - Narrower harvest spans: a harvest scans 210 MB in 5 64-MB regions for 86.5 MB of tracked pages (TC_HARVEST_LOG).
    GetWriteWatch costs ~5 us a call plus ~0.5 us per MB scanned (pf3/wwprobe.c on a guest-shaped arena), so runs over
    gaps <= 1 MB (23 calls, 125 MB) or <= 4 MB (14, 140 MB) cost the same or more. Not built.
  - The vertex cache's content hash (0.42 ms) by write watch: a cached range is read at record time, between harvests;
    a write by a concurrent producer after the last harvest would not be seen. Not exact; not built.
  - An off-RT lookup index (0.28 ms): every writer of off_rt[] identity fields would have to bump a generation; many
    sites hold OffRT pointers. Not worth the risk for ~0.15 ms on the critical path.
  - Bigger batches: PF1 measured them worse (the readback flush grows).
- **Review** (one read-only reviewer; probes in pf3/review: samp_eq.c runs the live samp_pick against the per-batch
  heap over 200k batches / 60M draws with 14k full-heap batches, 6M LRU refills and random loads -- 0 binds that
  differ, no set rewritten after a draw of its batch bound it; ww_race2.c puts a writer thread against the harvest,
  300k rounds -- 0 misses after a quiescent point, rule on and off; the unit test caught all three planted mutants).
  No high or medium finding: SAMP_KEEP's rewrites sit behind render_frame's full GPU wait as the per-batch heap's did
  (one queue, the wait signals and waits a fresh fence; the sampler tables are bound only inside render_frame);
  samp_write reads only the codes and the append-only border palette, so a kept set is what a rebuild would write
  (anisotropy reaches the codes; render scale never does); RASTER_MEMO's key holds every non-config input (the
  registers' 64 bytes, no padding, the frame size, the viewport rect; RES_SCALE applies later, at replay); the harvest
  marks a page's slots dirty before it resets it, so a write between GetWriteWatch and ResetWriteWatch is not lost,
  kept or not. Fixed (all low): the PERF line's "hot kept" counted the pages kept for the RSX's copies too (now apart);
  stale comments (the sampler cache "restarts with every render_frame"; tc_wt_rsx_write "any thread" -- the walker
  only; the manifest's hook "at startup" -- each batch); SAMP_KEEP's dependency on that full wait is written down at
  samp_frame_start (an async readback must make a set wait for the fence of the last batch that bound it); the NV3089
  linear note covered `out_h` rows of the pitch, short of the last row's right edge when the rectangle overhangs the
  pitch, and was sent when the clip let nothing through (both perf only: an unnoted page takes the old reset); the
  unit test's header says it is single-threaded. test_tex_cache_rsx now runs in m4c/gcm/test_gcm_ss.sh.
  After the fixes (rebuilt; pf3/rev/reverify.sh): PF3_VERIFY over the 7 scenes, **0 differing bytes** (raster records
  still 21.5M, pan 13.8M, 2x scale 21.5M / 13.7M, AA + sharpen 16.1M / 13.7M, cold boot 23.5M; the sampler binds
  alike, the old heap never full); stale clean binds only 0x47C72900 / 0x47C77E80 (the cold boot's 3 all 0x47C77E80);
  no D3D12 error. F5 / F9 / menu Quick Save and Load: saved, loaded. alltests rc 0 (test_tex_cache_rsx with them),
  gate 0 unclassified. A/B, 2 rounds: still 62.27 vs 57.85 fps (+7.6%), pan 70.18 vs 68.63 (+2.3%).
- **Seen, not PF3's (queued):** the "clean but different" binds above are the texture cache missing, for one round, a
  concurrent writer's write that lands after the round's harvest and before the bind (two 64x64 main-memory textures,
  a few a minute on the pan). On the PS3 the RSX reads memory when it draws. To look at: who writes them, whether the
  game orders that write before the draw (a label or semaphore the walker has passed), and a harvest at that point.
- **Next lever:** the walker is no longer alone on the still view's critical path (8% idle: the
  lighting semaphore and the PPU's put). Its own largest blocks: vertex emission 3.5 ms (tight; the vertex cache's
  content hash, 0.4 ms, could go only with a write watch harvested per FIFO segment), the 5 harvests 0.5 ms (one per
  batch; a pre-scan could harvest only the regions of the batch's bound slots), vp_record_fp's per-unit texture work
  ~0.3 ms (a texture-state generation, bumped in d3d12_bind_texture), off-RT lookups 0.28 ms. Off the walker: the SPU
  lighting chain (5.8 ms from the label) and the async readback (PF2's design, the ~0.5 ms GPU wait).

## 2026-10-27 (evening) — PF2: the readback stall; a written self-sample copy brought up to date over its writers' coverage (+4.9%)
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video/rsx_d3d12_backend.c (copy reuse, coverage update, census,
verify), rsx_report.{c,h} (rsx_selfsample_reuse_level, rsx_selfsample_writes, the rsx_ss_* copy state;
tests/test_rsx_report.c); config/ss_manifest.txt
- **Goal:** cut the walker's time at the G-buffer readback (PF1: 3.75 ms of the heavy pier still view's ~18 ms frame,
  1.5 ms of it waiting for the GPU to finish the copy-on-sample copies) without changing anything the game computes or
  shows.
- **Census first** (`RT_SELFSAMPLE_CENSUS=1`, [SSREUSE], still view, per frame): 150 units sample a G-buffer plane they
  are bound to, 150 full 1280x720 copies = 524 MB a frame: 5 the first in the replay, 145 after the plane was written
  since the previous copy (142 by one draw, 3 by 9+). Per idea, what it removes:
  - copy only the planes the draw samples: already so (PREPASS_LIVE: a unit outside the program's read mask gets no
    surface; 0 units of programs without a read mask) -- 0;
  - one copy per (op, plane) when two units read one plane: 0 here (each unit a different plane);
  - reuse a copy while nothing has written its plane: 0 -- each decal-like draw writes every plane it samples (write
    masks 0x487: A RGB, B alpha, C blue; 0x77, 0xC87...);
  - a dirty rect from the writers' scissors / viewports: they cover 100% of the plane -- 0 bytes;
  - one copy per run of draws that cannot touch each other's texels: not provable from what a record knows
    (full-target scissors and viewports; the positions exist only on the GPU) -- 0;
  - a smaller format view: R8G8B8A8 planes, every channel may be read -- 0;
  - the reader's scissor-limited rect: full target, and a sampler is not bounded by the scissor -- 0;
  - built, **the coverage update**: 142 of the 150 copies (499 MB) become 142 redraws of the previous draw's geometry
    (20.3k vertices a frame); 8 copies (25 MB) stay. Pan: 65-69 units a frame, 58-62 covered, 6.5-6.8 copies.
- **Sizing** (GPU_TIME + CHAIN_TRACE, per list, single runs): the readback flush's list is 1.14 ms of GPU time; with
  each copy cut to a 64x64 corner but every barrier kept (a throw-away experiment) 0.53 ms; with no copies
  (RT_SELFSAMPLE=0, wrong) 0.11 ms: the bytes cost ~0.6 ms, the 600 barrier calls ~0.4 ms. So fewer bytes and fewer
  barrier calls both matter.
- **Route (b), async readback -- the ordering design, not built.** The readback is synchronous: cellGcmSys's NV3089
  handler calls rsx_d3d12_readback_surface before its CPU copy (local -> main); it flushes the batch, copies the
  surface to a readback buffer, waits for the GPU and stores into guest memory; then the blit runs and the FIFO goes
  on: label B (the back-end semaphore the SPU light kicker waits for), the 4789 shadow-cascade draws (whose records
  read guest memory on the CPU: vertex arrays, indices, texture uploads), ZPASS reports, the semaphore acquire on the
  lighting before the sun pass, the zeta readback, the flip. Exact async needs (1) the blit and every later
  guest-visible effect -- semaphore / label releases, reports, notifies, REF, NV0039 / NV3089 transfers, flips and
  their events, and the GET the PPU reads (held at the first queued command) -- in one in-order queue published once
  the readback's fence has passed and its stores are done; (2) every record-time guest read checked against the
  pending write ranges (the surface, the blit destinations), blocking on overlap; (3) a FIFO semaphore acquire that
  drains the queue while it waits (the walker would otherwise wait on the lighting, which waits on label B); (4)
  render_frame's opening wait_for_gpu and the shared upload rings changed so the next batch can record while the
  readback list runs; (5) a save-state stop that drains the queue in rsx_d3d12_backend_ss_quiesce (no save holds a
  pending queue; a load starts empty). The SPU lighting still starts at label B, so only the walker's own wait moves:
  after route (a) that wait is 0.53 ms (was 1.4 ms), the most (b) could take back, at L effort and the highest risk in
  the renderer. Not built.
- **Built, route (a): `RT_SELFSAMPLE_REUSE`** (rsx_report.c rsx_selfsample_reuse_level; default 2, `=1` clean reuse
  only, `=0` the old copy per unit = kill switch; RT_SELFSAMPLE mode 2 only). Mode 2's rule stays: a draw that samples
  a plane it is bound to reads what the plane holds as the draw starts.
  - Clean reuse: the copy this replay (one render_frame op loop, `s_rtsnap_loop`) took is reused while nothing has
    written its plane. The loop records into one command list in op order, so every write between two draws is
    recorded by the loop between them, and the loop reports each: a draw writes the bound targets whose write-mask
    nibble is set (rsx_selfsample_writes, from the PSO's mask: rsx_rop_fill_blend); every bound target without the
    guest pipeline, after an MRT gap or under CMASK_FORCE; a clear and a display snapshot (a copy into a surface)
    write planes outside the draws, and they -- conservatively also a target rebind, the screen-copy capture and
    the post-fx scene hook, which only read -- end every reuse of every copy taken before them, clean or written
    (rtsnap_dirty_all, rsx_ss_barrier; as first written, clean copies only: review H1 below).
  - Coverage update (ssc_update, before a reader's units bind): a draw writes a plane only inside its rasterised
    coverage, so the copy differs from the plane only inside its writers' coverage. Each writer since the copy (s_ssw:
    the op, its viewport and scissor) is drawn again: its guest pipeline's vertex shader (vp_get_fp_pso_impl's
    resolution), constants, vertices and draw arguments; no culling, no depth clip, no depth / stencil test,
    conservative rasterisation (every pixel the triangles touch: a superset of the writer's coverage that also absorbs
    any rounding difference between two pipelines on one vertex shader); a pixel shader that loads the plane at its
    pixel and writes the copy. One barrier call each way for all the reader's planes (plane RT -> PSR, copy PSR -> RT
    and back), then the op's targets, pipeline, viewport and scissor are put back. Only for UNORM8 planes (a UNORM8
    load and store is the identity), at most 8 writers, writers with the guest pipeline; otherwise the full copy.
    Pipelines are cached per (VS blob -- referenced, so the key stays unique --, vertex layout, format); a 16-wide SRV
    table per RT slot (SSC_DESC_BASE, the heap grows by 1024 descriptors); the copies are created render-target
    capable with an RTV heap of their own. The per-copy state and the decision are rsx_report.c's rsx_ss_*
    (unit-tested); the pipeline cache holds 64, the least recently used replaced (review M1).
  - Diagnostics: `RT_SELFSAMPLE_VERIFY=1` -- at every reuse the reused copy and a new copy of the plane go to two
    buffers and a compute pass counts their differing 32-bit words ([SSVERIFY]; a GPU wait per list);
    `RT_SELFSAMPLE_CENSUS=1` ([SSREUSE]). The bookkeeping returns at once until the loop has taken a copy (it runs for
    each of ~9000 ops).
- **Exactness:**
  - Verify mode (the final build): a 40 s still run 275k compares (3.5 MB a side each), a 40 s pan 125k: **0
    differing 32-bit words**, every reuse of both scenes (and 209k in a still run of the build before the bookkeeping's fast path).
  - RSX_CAP at frame 400 of the still view (every surface, alpha too): the same frame is not reachable -- REUSE=0 vs
    REUSE=0 already differs in 129 of 284 images and in the op list (1727 vs 1741 draws), the default twice in 114,
    off vs on in 143; the display-buffer parity (0x3B0000 / 0x734000 as the third plane) also follows the run's
    timing. Hence the verify mode above: the old and the new copy compared in the same frame at every use.
  - CELLMARK boot identity (pf2/idall.sh, base exe = the build before PF2): frames 0-4 byte-identical in all 5 runs
    (base x2, the default x2, REUSE=0), frames 5+ timing-dependent as before (base vs base 6 of 16 differ); 222
    artefact descriptions, 0 different.
- **Measured** (pf2/campaign.sh = pf1's, 3 interleaved rounds, FAST BOOT pier, HOST_PLACEMENT auto, ftsum7;
  off / off2 = RT_SELFSAMPLE_REUSE=0 twice, the A/A):
  | scene | arm | fps (rounds) | present p50 / p95 / p99 ms | shown 1 / 2+ refreshes | GPU ms |
  |---|---|---|---|---|---|
  | still | off | 55.47 (55.53 / 55.40 / 55.47) | 17.77 / 19.24 / 19.65 | 64.8 / 35.2 % | 2.13 |
  | still | off2 (A/A) | 55.30 (55.13 / 55.20 / 55.59) | 17.75 / 19.38 / 19.78 | 64.4 / 35.6 % | 2.12 |
  | still | **on (2)** | **58.20** (57.69 / 58.49 / 58.44), +4.9% / +5.2% | 16.87 / 18.39 / 18.73 | 71.2 / 28.8 % | 1.26 |
  | pan | off | 68.09 (68.18 / 68.00 / 68.10) | 13.65 / 18.91 / 20.34 | 89.7 / 10.3 % | 1.10 |
  | pan | off2 (A/A) | 67.99 (67.93 / 68.08 / 67.95) | 13.63 / 18.96 / 20.41 | 89.5 / 10.5 % | 1.12 |
  | pan | **on (2)** | **68.76** (68.80 / 68.77 / 68.71), +1.0% / +1.1% | 13.61 / 18.26 / 19.88 | 90.8 / 9.2 % | 0.87 |
  A/A: still -0.3%, pan -0.15%. CHAIN_TRACE (single runs): the readback flush's list 1.13 -> 0.27 ms of GPU time,
  the walker's GPU wait at the readback 1.39 -> 0.53 ms, its flush 1.11 -> 1.04 ms, readback-begin -> label 3.58 ->
  2.65 ms; the light chain after the label unchanged (5.85 ms). The still view is now above the 56 Hz VRR floor.
  WALKER_STATS at the still view: the walker 95.3% busy at REUSE=0 and 94.9% at the default (it now builds ~5% more
  frames in the same share): still the frame clock.
- **Stability:** the fast boot loads its state (pause 205 ms); F5 / F9 and the menu's Quick Save / Quick Load in one
  run (pf1/steps/ssio.txt): saved, loaded (pause 120 ms), saved, loaded, the picture right after each load. A 178 s
  steady pan at the defaults: exit 0, 69.4 fps mean, 60 the lowest second; a 107 s pan at the defaults and one at
  REUSE=0 (68.7 / 68.3 fps): no error-like log line only in the default runs (pf1/errdiff.py: the one difference is
  a cellNetCtl line another thread's print split). The census and verify runs logged no D3D12 error or device
  removal.
- **Tests:** test_rsx_report 117 passed (new: the targets a draw writes -- masks, gaps, the backbuffer as target A,
  no pipeline --, the default level, and the rsx_ss_* copy state through event sequences); m4c/integ/alltests.sh: unit, kernel, spu_host, spu_load, fsfd, gcm, libs
  rc 0 (unit's "strict-pass share" line is the known non-gating print); manifest gate 3942 statics (35 new), 0
  unclassified.
- **Review** (one read-only reviewer, with a model probe of the bookkeeping: pf2/review/probe_dirty.c). Fixed:
  - H1 (high, the picture): rtsnap_dirty_all barred only clean copies. A plane written by a draw after its copy,
    then cleared (the whole-view or the rect clear) or overwritten by the display snapshot (DB_SNAP: the third
    G-buffer plane is the display alias), then sampled with 1-8 writers since: the coverage update redrew only the
    draw's coverage, and the reader saw the pre-clear texels everywhere else. The pier never clears between decals,
    so the verify runs could not catch it. Now every copy of the loop is barred, clean or written; the per-copy state
    and its decision moved into rsx_report.c (rsx_ss_copied / _barrier / _takes_writer / _written / _covered /
    _decide) with 21 sequence checks in test_rsx_report, the H1 sequence among them.
  - M1: the coverage pipelines (64, keyed by the VS blob) were never evicted: a long session that met 64 (VS, layout,
    format) keys fell back to full copies for good, silently. Now the least recently used one (not one of this
    frame) is replaced, its pipeline through the PSO graveyard; logged once.
  - L1: a writer drawn with user clip planes (USER_CLIP_CONTROL; the record's new `uclip`) is not drawn again (wbad):
    its clip distances need not cover conservatively. L2: the screen-copy capture binds only the backbuffer, so it
    now sets cur_rt -3 and a rebind follows (under DEPTH_MODEL=0 the coverage update's restore re-bound MRT planes
    the old path had left unbound). L3: RT_SELFSAMPLE_REUSE outside 0-2 is the kill switch's 0, logged (it was 2).
  - M3 (evidence): the census counts plane updates over 2+ writers; the verify runs widened (below).
  Not changed: no cost cap on the redraws (up to 8 writers of any size; 1024 writer records a loop, then wbad):
  performance only.
  After the fixes (rebuilt; pf2/rev/reverify.sh), verify mode, **0 differing 32-bit words everywhere**: pier still
  285k compares, pan 127k (0.3 plane updates a frame over 2+ writers), 2x render scale still 223k and pan 107k, AA
  fast + sharpen 25 still 285k and pan 128k, a cold boot through the title, the loading and the opening scene with a
  walk 285k (5 to 151 self-sampling units a frame; before the fixes 291k there too and 271k on a long walk). The
  census: one full copy a frame more (a barrier now ends a written copy: 141-143 updates, 8-9 copies). A/B, 2
  rounds: still 58.1 vs 55.7 fps (+4.4%), pan 68.8 vs 68.2 (+0.9%). F5 / F9 / menu quick save and load: saved,
  loaded, the picture right. alltests.sh rc 0 (test_rsx_report 117), manifest gate 0 unclassified.
- **Next lever:** the walker still sets the frame (95% busy; the still frame fell 0.85 ms, the readback's 0.93 ms).
  At the readback it now waits 0.53 ms for the GPU (the decal phase's barrier calls: two per reader, each a pipeline
  drain; the 8 copies left are first-in-replay or have 9+ writers) and records the flush in 1.04 ms. In order: the
  walker's own per-draw work (vnew_draw 56% in PF1: vertex emission, constants, the draws-full flushes); one MRT
  coverage draw per writer for all of a reader's planes (fewer pipeline / target switches, CPU); async readback (b) for
  the last ~0.5 ms of GPU wait, with the ordering design above.

## 2026-10-27 (afternoon) — PF1: the heavy pier view's critical path; the walker and the SPU workers on cores of their own (+4.5%)
EBOOT: BCUS98125 update 1.04 | ps3recomp runtime/syscalls/lv2_register.c (HOST_PLACEMENT), runtime/platform/host_placement.h
and chain_trace.h (new), runtime/savestate/ss_file.c, runtime/syscalls/sys_timer.c (CHAIN_TRACE),
libs/video/rsx_d3d12_backend.c, rsx_commands.c, cellGcmSys.c (CHAIN_TRACE stamps); src/main.cpp; config/ss_manifest.txt
- **Goal:** the New Marais pier "still" city view (~8970 draws) ran ~52 fps: below the 56 Hz VRR floor, and on the fixed
  75 Hz display 41% of frames stay up for 2 refreshes. Target +10% (57+) without changing what the game computes or shows.
- **Profile, still view, base build** (FAST BOOT, quiet machine; threadcpu, FRAME_TRACE, PERF / VPERF, WALKER_STATS,
  PS3_POLLTIME, HOST_SAMPLE and HOST_SAMPLE_ALL, and CHAIN_TRACE below). 18.7 ms frames (53.4 fps in that run):
  - **walker** (frame clock): 16.7 ms CPU + 1.4 ms GPU waits + 0.5 ms idle -- 97% occupied (WALKER_STATS busy 99.6%).
    Inclusive samples: vnew_draw 56% (vertex emission 20%, constants vp_record_cb 10%, and the three draws-full
    capacity flushes 17%), render_frame 29% (5 batches: 3 draws-full, the readback flush, the present), the G-buffer
    readback 17% (NV3089 blits 22% with the CPU copies to main memory), wait_for_gpu 9%. VPERF: 2.1M vertices drawn a
    frame, 205k converted, 11.3k cache hits / 100 misses.
  - **SPU workers** 0x2000-0x2003: 15.2-15.4 ms each (82%), 61 ms of SPU time a frame -- the same per frame at 49 and
    53 fps, so real job work, not spinning (HOST_SAMPLE_ALL: lifted job code, the lighting plugs plug_0087f880 /
    0087ec80 the largest; the SPU runtime -- DMA, channels, overlay and GCM PUT notes -- ~12% of the busy time).
  - **PPU main**: 7.6 ms CPU (vm_read32 / vm_write64 / ... ~38% of its exe samples); per frame (CHAIN_TRACE) 7.5 ms
    game code (1.2 ms of it in a 16k/s usleep poll at lr 0x13C280), 4.3 ms in the WWS barrier waits, 6.6 ms waiting for
    the light kicker. The kicker thread: 1.6 ms a frame (event waits; it spun ~96% of a core at C0).
  - **The chain** (CHAIN_TRACE, medians): label A (just before the G-buffer copies) -> the walker's readback 3.75 ms
    (its flush 1.3, GPU wait 1.5, stores 0.2, the NV3089 copies ~0.8) -> label B -> the SPU lighting jobs 3.3 ms
    (kicker state 2) -> the tail 2.6 ms (state 3) -> the main thread wakes. The lighting runs under the walker's
    shadow cascades (4789 draws after the readback, before the sun pass), so the walker almost never waits on the
    lighting's semaphore; the main thread's 6.6 ms kicker wait is slack.
  - **The GPU wait at the readback** is the copy-on-sample snapshots (RT_SELFSAMPLE mode 2: ~150 full 1280x720 copies
    a frame, 3 G-buffer planes for each of ~50 decal-like draws at the end of the G-buffer pass, all with full-screen
    scissors): the readback flush's list is 1.17 ms of GPU time with 256 draws vs 0.39 ms for the 2972 before it
    (GPU_TIME per list); RT_SELFSAMPLE=0 (wrong, sizing only) 0.06 ms.
  - **Critical path: the walker.** Its frame is the frame; the SPU lighting chain behind its readback is within ~0.5 ms
    of binding (moving walker work around just moves the idle: see the batch shape below). In the pan (66 fps) the
    walker idles 2.7 ms a frame and the main thread (game code 5.9 + barrier 1.7) plus the lighting chain (5.2) set
    the pace.
  - **Since C0** (2026-10-15, the gate at 40 fps: walker 91%, SPU 86%, main 24.8 ms = game code 9.5 + waiting for SPU
    lighting ~10 + barrier 4.7, the kicker spinning 96% of a core): the spins are gone (M6a), the walker's per-draw
    work shrank (VCACHE 256, CB_COMPACT, REPORT_PRECISE, PREPASS_LIVE), and at the heaviest view the walker itself is
    the bottleneck again, with the SPU pool at 82%; game code ~7.5 ms.
- **Levers, estimated then measured** (single runs unless said; base ~18.7 ms):
  - batch capacity (MAX_DRAWS 2048 -> 8192, so no draws-full flushes): est. 1-2.5 ms; measured walker CPU -0.7 ms but
    the readback flush grows +1.6 ms (the whole G-buffer replayed and run on the GPU at the copy) and the walker then
    waits: 19.1-19.3 ms, worse. Pre-readback batches of 1024: 19.9. A split m records before the last frame's readback
    point (m 256-1600): 19.2-19.5. Not kept.
  - thread placement (SMT siblings of the critical threads): est. 0-8%, unknown sharing. Walker alone -0.35 ms, walker
    + SPU workers -0.9 ms, + the PPU main thread no better. **Built (below).**
  - RT_SELFSAMPLE=1 (one snapshot per texture-cache epoch, ~1/3 of the copies): a further -0.7 ms (17.2 vs 17.9 ms) --
    but decals then read the plane as of the epoch's start, not as the draw starts: not exact; a ground-truth question
    (real-PS3 footage of the pier decals), not taken.
  - async readback (record past the copies while the GPU finishes; the label after them published when the data has
    landed): up to -1.5 ms of walker time (the GPU wait), L effort, every guest-visible write after the copies must wait
    for it. Next lever.
  - PPU code (M5 inline vm accessors): the main thread is not critical at the still view; the pan's lever. Spin-waits:
    none left on the critical path (the 0x13C280 poll is the main thread's own time).
- **Built: HOST_PLACEMENT** (lv2_register.c ps3_host_place_thread; main.cpp asks for the frame clock and the PPU main
  thread, the SPU threads 0x2000-0x2003 ask when they start, recreated threads after a load ask again). With CPU sets:
  the frame clock on the highest core of group 0, the first four of the SPURS kernel's five SPU workers (0x2004 is
  ~0.5% busy) on the next four, every other thread of the
  process on the rest (the process's default CPU sets); the save-state codec pools (ss_file.c) opt out
  (host_placement.h: own CPU sets = every core), so a load reads as fast as before (SS_LOAD 141 ms; 209 ms confined).
  Modes: 1 walker, 2 + SPU workers, 3 + PPU main; **default auto = 2** when group 0 has 8+ cores of one efficiency
  class and one last-level cache, all with SMT (the test PC's 9800X3D: 8 cores, one CCD; the 7+ / no-SMT rule as first
  written was narrowed by the review below), else 0; the title opts in from the `cpu_placement` row (auto | off);
  `HOST_PLACEMENT=0` = the old scheduler placement. Placement only: nothing the game computes depends on it. `[placement]` lines name what was done.
- **Measured** (pf1/campaign.sh = iq1's, 3 interleaved rounds, FAST BOOT pier, ftsum7; off / off2 = HOST_PLACEMENT=0 twice, the A/A):
  | scene | arm | fps (rounds) | present p50 / p95 / p99 ms | shown 1 / 2+ refreshes | GPU ms |
  |---|---|---|---|---|---|
  | still | off | 52.97 (52.75 / 52.98 / 53.18) | 18.81 / 20.74 / 22.10 | 58.5 / 41.5 % | 2.13 |
  | still | off2 (A/A) | 52.55 (51.88 / 52.75 / 53.02) | 18.94 / 21.17 / 23.98 | 57.3 / 42.7 % | 2.13 |
  | still | **auto (2)** | **55.33** (55.16 / 55.14 / 55.70), +4.5% / +5.3% | 17.85 / 19.49 / 20.59 | 64.4 / 35.6 % | 2.13 |
  | still | 3 | 55.24 (55.01 / 55.44 / 55.27) | 18.04 / 19.46 / 20.73 | 64.2 / 35.8 % | 2.12 |
  | pan | off | 66.37 (65.83 / 66.81 / 66.47) | 14.08 / 19.69 / 21.73 | 86.9 / 13.1 % | 1.17 |
  | pan | off2 (A/A) | 66.61 (66.52 / 66.55 / 66.76) | 14.02 / 19.56 / 21.83 | 87.3 / 12.7 % | 1.15 |
  | pan | **auto (2)** | **67.98** (68.14 / 67.85 / 67.93), +2.4% / +2.1% | 13.72 / 18.94 / 20.52 | 89.5 / 10.5 % | 1.20 |
  | pan | 3 | 67.88 (67.86 / 68.03 / 67.76) | 13.75 / 18.94 / 20.57 | 89.4 / 10.6 % | 1.13 |
  A/A: still -0.8%, pan +0.4% (rounds within +-1%). The final build (CHAIN_TRACE's hooks moved to a header after the
  campaign; the default path unchanged), one more round: still 52.09 -> 54.62, pan 66.14 -> 67.70 fps.
  The +10% target is not reached: the still view is at 55.3 fps,
  still below the 56 Hz VRR floor by a hair; what is left is the walker's own work (async readback next, ~-1.5 ms est.)
  and the copy-on-sample policy (a picture decision).
- **Correctness:** CELLMARK fast-boot identity (pf1/idall.sh, rs2/idcmp.py; the base exe swapped into build\ for its
  runs): boot frames 0-4 byte-identical in all 6 runs (base x2; new x4: auto x2, HOST_PLACEMENT=0, the final build),
  frames 5+ timing-dependent as always (base vs base 6 of 16 identical); 220 artefact descriptions, 0 different.
  Save states: the fast boot loads its state (pause 190-243 ms, base 187-218); F5 / F9 and the menu's Quick Save /
  Quick Load in one run (pf1/steps/ssio.txt): saved, loaded (pause 124 ms), saved, loaded; the quick slot read 155-187
  ms on 8 codec threads. A 181 s steady pan at the defaults: exit 0, 68.7 fps mean, and a 93 s pair at the defaults /
  HOST_PLACEMENT=0: no error-like log line only in the placed run.
- **CHAIN_TRACE=<file.csv>** (sys_timer.c; diagnostic, one load when off): one row per main-thread kicker wait: the
  previous wait's end, the barrier waits (CHAIN_LR_B) and other polls (CHAIN_LR_S) since, the wait itself (CHAIN_LR_K),
  the walker's first readback (begin / end) and the back-end label after it, the light kicker's states 0-3 (CHAIN_LR_L
  poll sites, state word r31+0x318), the walker's present before the readback and its idle in between by kind. With it
  on, [RBT] lines split the readback (its flush, the GPU waits, the stores) and [GPUTIME] adds GPU ms per list class.
  The walker files stamp through runtime/platform/chain_trace.h (header only). inFamous 2 1.04: `CHAIN_LR_K=1EF2A0
  CHAIN_LR_B=38AA3C,38AA7C,38AAA4,38B4C8,38B4F4,38B520 CHAIN_LR_S=13C280 CHAIN_LR_L=350C54,350C88`; scratch
  pf1/chainsum.py summarises. (KICKER_TRACE's sites are still the 1.00 addresses.)
- **Tests:** m4c/integ/alltests.sh: unit, kernel, spu_host, spu_load, fsfd, gcm, libs rc 0 (unit's "strict-pass share"
  line is the known non-gating print); manifest gate 3907 statics, 0 unclassified. The walker files reach CHAIN_TRACE
  through a header-only selectany hook block (runtime/platform/chain_trace.h, as guest_clock.h), so the unit tests that
  build rsx_commands.c / cellGcmSys.c alone link as before (test_gcm_ss_load 47, test_rsx_report_fifo 10 passed).
- Review (one read-only reviewer with probes on the test PC: the CPU-set mechanism, every thread's fate, topology detection
  on other CPUs, thread identity, CHAIN_TRACE, save states, these docs). CPU sets act as hard confinement here: the
  placed threads never leave their core and every other thread of the process (the other PPU threads, SPU 0x2004-
  0x2006, cellAudio's mixer, the driver, the compile workers, the overlay) shares cores 0-2 -- ~0.65 of a logical CPU
  busy at the still view, so headroom is ample. Probe (pf1/review/cpuprobe.c): under light / moderate foreign load
  the placed core does better (Windows keeps other load off it: 99% vs 81-92% of an unloaded run), near full
  saturation worse (37-43% vs 48-59%); a soft placement (own core + shared cores, ideal processor) ran on its own core
  only ~45% of the time, losing the gain. Fixed (medium):
  - Placement is a title opt-in: the toolkit's default is off (ps3_host_placement_configure, before the frame clock
    starts); inFamous 2 opts in from a new settings row `cpu_placement = auto | off` (PC Settings > Frame, CPU,
    advanced; restart), so players can switch it off from the menu. HOST_PLACEMENT=auto | on | off | 0-3 stays as a
    debug override (a typo is logged and ignored; before, atoi turned "on" into off).
  - Auto now needs 8+ cores of one efficiency class and one last-level cache, all with SMT: an 8C/8T part (an
    i7-9700K, SMT off, an 8-vCPU VM) would have left every other thread 3 logical CPUs.
  - A failing CPU-set call turns placement off (SetProcessDefaultCpuSets: nothing placed; SetThreadSelectedCpuSets:
    the process default is cleared so no core stays reserved), and a failure to read the CPU sets is logged.
  - cellAudio's mixer and the shader-compile workers run on every core (ps3_host_unplace_thread): the mixer never
    shares cores 0-2 with the game's busy threads, and a cold shader cache's compile bursts do not crowd the threads
    the walker waits on; the background F5 write pool now stays on the shared cores (only the load's read pool and a
    foreground write opt out: the load pause, where it was measured).
  In game (pf1/rev): defaults -> mode 2 on the test PC's 9800X3D, the walker on core 7 and the SPU workers on 6-3 as before;
  cpu_placement = off in an options.ini -> "[placement] off"; HOST_PLACEMENT=on -> auto; =bogus -> logged, the
  setting's choice. test_options_store 366, test_ovl_menu 1371. Not changed: the walker sits on the CPU's
  least-preferred CPPC core (Windows keeps foreign load off it; an order by SchedulingClass is an experiment for
  later); the forced debug modes pick cores by CoreIndex (wrong ones on hybrid / two-cache CPUs); only processor
  group 0 is considered; CHAIN_TRACE's idle columns need FRAME_TRACE without WALKER_STATS and its CSV can lose its
  last rows at an ExitProcess quit (a diagnostic). Rebuilt; alltests.sh rc 0 everywhere, manifest gate 0
  unclassified.

## 2026-10-27 — P8a: Shadow filtering (PS3 / Soft / Softer) in PC Settings
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video/rsx_fp_decompiler.{c,h} (rsx_ztexs; tests/test_fp_decompiler.c,
tests/test_ztex_pcf.c), rsx_d3d12_backend.c, libs/system/options.c (+ tests/test_options_store.c),
libs/video/tests/test_ovl_menu.c; config/ss_manifest.txt
- **What:** a Graphics row `shadow_filter` (group Shadows, "Shadow filtering": `ps3` = PS3 | `soft` = Soft | `softer` =
  Softer; OPT_ENUM, OPT_RESTART, OPT_F_ENHANCEMENT; env `GFX_SHADOW_FILTER` locks it through the store's grammar, so
  both menus show what runs; presets PS3 / Low / Medium ps3, High soft, Ultra softer). The title resolves every
  shadow map with one projective depth fetch per tap: on NV40/RSX one 2x2 bilinear PCF (our rsx_ztex, exact since
  ZTEX_PCF), so a close-up spreads one shadow texel over 6-7 pixels and the edge is a staircase (the "blocky neck
  shadow", design image-quality 2) -- at Internal resolution 2x even more visibly. Soft and Softer give such a tap a
  wider kernel, in texels of the bound map (GetDimensions), so it holds at any shadow-map size and any internal
  resolution.
- **Kernels (rsx_ztexs, rsx_fp_decompiler.c; CPU twin rsx_fp_shadow_weights):** around k = round(p), d = p - k
  (p = uv * size - 0.5), per axis -- Soft: texels k-1..k+1 with (1/2 - d)^2 / 2, 3/4 - d^2, (1/2 + d)^2 / 2: a tent
  of 1-texel radius integrated over each texel's square (bilinear PCF is the 1-texel box over the same squares, so
  Soft is that box once more: the quadratic B-spline), 9 texels per tap; Softer: texels k-2..k+2 with (1/2 - d)^2 / 8,
  (1 - d) / 4, 7/16 - d^2 / 4, (1 + d) / 4, (1/2 + d)^2 / 8: a tent of 2-texel radius, 25 texels per tap. Weights sum to
  1 exactly, are >= 0 (a step in the map ramps monotonically over 1 / 2 / 4 texels: PS3 / Soft / Softer), and are
  read in 2x2 blocks each gathered at its own centre (i + 1) / size -- the exact-PCF rule, so the sampler's CLAMP /
  BORDER / WRAP still applies and no seam band comes back: 4 blocks (12 gathers for the packed alias) for Soft, 9
  (27) for Softer. The compare (ZFUNC 1-7) is rsx_ztex's. A pixel whose unit has ZFUNC 0 (a depth read), NEAREST, or
  a ZTEX_PCF diagnostic footprint takes rsx_ztex (the PS3 kernel) per draw.
  Softer is the 5x5 tent, not a 12-tap rotated Poisson disc of 2-texel radius (built as ZTEX_SOFTER=poisson): without
  a temporal filter the rotation shows as grain along every penumbra (p8a/crops_poisson.png), it costs more fetches
  (36 gathers), and its sum on a sloped receiver is noisier (test_ztex_pcf: min 0.71 against the tent's 0.91).
- **Mechanism: shader variants, a restart row.** The PS3 value must leave the decompiled text byte-identical (every
  cached shader, the shader list's records and every pipeline stay valid), so the kernel cannot be a branch in
  today's helper: it is a decompile-time setting (rsx_fp_set_shadow_filter, process-wide, set once before the first
  decompile from raster_cfg, like FP_WPOS) that makes a depth unit the program samples ONCE call rsx_ztexs, appended
  after the unchanged rsx_ztex. The caches need no new key: the disk cache is keyed by the HLSL text (Soft / Softer
  texts get their own entries next to the PS3 ones), the in-memory PS / PSO caches live one session (the setting
  never changes inside it), and the shader list records program inputs, not text -- its prewarm regenerates the
  session's variant. Live switching would recompile ~24 shadow programs on the render thread the moment their draws
  come (the renderer keeps no old pipeline while a replacement builds), hence OPT_RESTART: the next launch's prewarm
  workers and first-use compiles build the new variants once (25-35 ms each, off the frame where the workers get
  there first), later launches read them from the cache. No per-draw CPU work: no new constants, no new key fields.
- **Multi-tap exclusion:** rsx_fp_multi_fetch_units (the rsx_fp_txl_units walk) marks units with two or more texture
  instructions; their taps keep rsx_ztex, so the title's own 5-tap resolves (a centre TXP + four TEX at +-0.7 texel:
  94BF0862 box, 4EC41EA2 cascade, E168CEAC biased cascade) are not blurred twice -- checked on the real programs
  (local mtcheck.c over the RPCS3 raw cache copy, not committed): all three byte-identical to their PS3 HLSL at
  Soft and Softer, the single-tap C2C03CA4 / 604335F9 changed. A tap inside a flow-control block also keeps the PS3
  kernel (none of the title's do), as do cube units.
- **PS3 = today's output** (p8a/base = build\infamous2.exe + .map before the first build):
  - every fragment program the renderer ever compiled: the 11,136 FP records of build\shadercache\shaderlist.bin
    decompiled exactly as the backend does (unit modes, alpha test, sRGB, ROP epilogue) by the original and the new
    decompiler at PS3 give the same 11,136 text hashes (p8a/slcensus.c); at Soft / Softer / Poisson all 1,398 records
    with a depth unit take rsx_ztexs and all compile (D3DCompile ps_5_0, 0 failures). 24 programs: the cascade
    resolves 2FBEC818 / CC3DA20A, the close-up box C2C03CA4, the key light 604335F9, the forward character draws
    DF1DE7FE / B147C6C0 / 0AF67207 / 42AEA2A6, the texture-space passes, 30538599 / EAF437D5 / 09AAB32E ...
  - a pier gameplay run with a pan (FP_DUMP_ALL, fast boot): 194 of 194 programs' HLSL identical old vs new at the
    defaults (the 4 depth-sampling ones included; the dump's address comment and first-upload constant values set
    aside);
  - identity runs (rs2/idcmp.py; base exe 3 runs, final build 2 at the defaults + an options.ini `shadow_filter = ps3`
    + `GFX_SHADOW_FILTER=ps3`): CELLMARK boot frames 0-4 byte-identical in all 12 base / new pairs (5+ differ base vs
    base too), 219-220 artefact keys (PSOs with their PS bytecode hash, samplers, surfaces, swap chain) in both, 0
    different descriptions in every pair, no `[shadow]` line.
- **Acne / light leaking (the receiver plane, measured, not shipped):** a wider kernel compares the receiver with
  texels up to 1.5 / 2.5 texels away. Built as an A/B: a receiver-plane depth bias per texel from the screen-space
  derivatives of (u, v, r) (Isidoro 2006; two-sided, or lowering only; a slope limit with the PS3 kernel as fallback).
  On a clean analytic slope (test_ztex_pcf, a receiver over its own depth with just the PS3's one-texel bias) it
  works: without it Soft loses up to 1.2% and Softer 8.6% of the light, with it nothing. In the game it does the
  opposite: the resolves rebuild the receiver from the depth buffer, and the plane solved from those derivatives is
  noisy -- speckles on posts, plank piles and Cole (p8a/crops_acne_mask.png: the shadow mask after the last cascade
  resolve, snapped with RSX_CAP_OPSNAP=3B0000) -- and its slope limit, calibrated on the wide cascade's stored
  surfaces (slopes p50 1.7e-3, p99 4.3e-3 depth / texel: p8a/zslope.py), cannot tell a steep wall from a quad across
  two surfaces. Without it the wider kernels shadow nothing the PS3 kernel leaves lit: on the mask (p8a/maskcmp.py,
  pixels lit in PS3 and >= 20 px from any PS3 shadow), PS3 vs PS3 mean loss 0.002-0.005/255, Soft 0.004-0.008,
  Softer 0.005-0.006; 0 pixels darker by >= 24/255 in either view, at most 14 by >= 4/255 (Softer, pier). Where the
  title itself has acne (its PS3 2x2 already stripes the post and crate faces grazed by the sun in the mask; not
  visible in the final picture), Soft keeps the stripes and Softer averages them into a smooth grey. Cole's skin in the cutscene close-ups:
  no acne at either kernel (crops_kuo_*). So the default has no plane (pl = 0: every texel against the pixel's r,
  as the PS3's 2x2 does); ZTEX_SOFT_PLANE=on|lower brings it back for A/B.
- **Picture** (host pixels, the same camera per view; PS3 | Soft | Softer, *_1x.png the presented 1280x720 picture x3,
  *_x2.png RES_SCALE=2 2560x1440 x2; p8a/crops_*): the Kuo cutscene close-up of Cole's neck (READY on PlayPolycut
  + 33.6 s: the design's neck shadow -- PS3's staircase under the ear becomes a smooth edge, crops_kuo_neck), his face
  (+15 s, crops_kuo_face), Cole's shadow on the pier planks (crops_cole_shadow: the stair-steps at x2 gone), Cole at
  gameplay distance (crops_cole_self), the far facades in cascade 2 and the warehouse (crops_walls), the dock pallets
  (crops_pallets). The far cascade's texels are a few pixels on screen, so its edges change least.
- **Cost** (p8a/campaign.sh = iq1's: fast-boot pier, 1280x720 window, vsync 75 Hz, GPU_TIME=1, FRAME_TRACE, 3
  interleaved rounds, the arm order rotating; p8a/ftsum.py -> p8a/cost_summary.txt; A/A spread +-0.4-1%):
  | scene | arm | fps (min..max) | vs PS3 | present p50 / p99 ms | GPU busy ms |
  |---|---|---|---|---|---|
  | still | PS3 | 52.78 (52.17..53.10) | -- | 18.85 / 22.61 | 2.11 |
  | still | Soft | 52.89 (52.69..53.07) | +0.2% | 18.89 / 22.18 | 2.13 |
  | still | Softer | 53.04 (52.82..53.32) | +0.5% | 18.78 / 22.05 | 2.16 |
  | still | 2x PS3 | 47.76 (47.47..47.94) | -9.5% | 20.79 / 24.15 | 4.79 |
  | still | 2x Softer | 47.67 (47.47..48.01) | -9.7% (-0.2% vs 2x PS3) | 20.84 / 24.29 | 4.93 |
  | pan | PS3 | 66.59 (66.30..66.74) | -- | 14.03 / 21.58 | 1.17 |
  | pan | Soft | 66.77 (66.67..66.83) | +0.3% | 13.97 / 21.48 | 1.14 |
  | pan | Softer | 66.67 (66.61..66.74) | +0.1% | 14.04 / 21.41 | 1.19 |
  | pan | 2x PS3 | 64.52 (64.31..64.67) | -3.1% | 14.42 / 22.76 | 2.18 |
  | pan | 2x Softer | 64.43 (64.28..64.71) | -3.2% (-0.1% vs 2x PS3) | 14.42 / 22.90 | 2.34 |
  No measurable fps or frame-time cost: every delta is inside the A/A spread. GPU busy +0.02-0.05 ms a frame at 1x,
  +0.14-0.16 ms with Softer at 2x (27 gathers per resolve pixel against 3; the resolves are full-screen but
  depth-bounded per cascade). The CPU side is unchanged: no new per-draw constant or key, the kernel lives in the text.
- **Menus** (p8a/shots): the native PC Settings Graphics page now has 10 setting rows (one page under the category row: preset,
  brightness, anti-aliasing, sharpening, AF, texture packs, Shadow filtering *, internal resolution, upscaling,
  detail distance); Right moves it PS3 -> Soft -> Softer, the preset reads Custom, the help line "Changed: takes effect
  at the next launch. How shadow edges are smoothed." The F1 Graphics tab: group SHADOWS, "Shadow filtering
  [RESTART] Softer", "1 change applies after a restart"; with GFX_SHADOW_FILTER=soft the row is greyed with ENV +
  RESTART badges and shows Soft. Log: `[options] shadow_filter = soft (was ps3)`.
- **Knobs:** GFX_SHADOW_FILTER (locks the row); debug: ZTEX_SOFT_PLANE=on|lower, ZTEX_SOFT_PLANE_MAX=<depth/texel>
  (0 = no limit), ZTEX_SOFTER=poisson. Log: `[shadow] shadow filtering soft: ...` once at start, and per program
  (first 48) `[shadow] fp=... id=...: N depth tap(s) with the soft kernel, M with the PS3 2x2`.
- **Tests:** test_fp_decompiler 352 (Test 24: PS3 byte-identical to a decompile that never set the filter and again
  after Soft; a unit sampled twice, the 5-tap shape, a tap in an IFE block -> the PS3 text byte for byte; unit 1 once
  + unit 2 twice -> only unit 1 soft; not on cube, unbuffered or colour units; the weights: sum 1, >= 0, symmetric,
  = the tents' areas numerically (1e-12), PS3 = the bilinear pair; the edge ramps 1 / 2 / 4 texels, monotone),
  test_ztex_pcf 678 on WARP and the RTX 5080 (the soft kernels against a double reference from rsx_fp_shadow_weights:
  max |err| 0.000000 over ZFUNC 1-7, CLAMP and BORDER edges, both depth modes, both maps; NEAREST keeps the PS3 kernel (a degenerate
  receiver plane too, with the ZTEX_SOFT_PLANE A/B); all-lit = 1 within 2.4e-7 (exactly 1 after the review's snap), all-shadowed = 0; the edge monotone over 0.996 / 1.996 /
  3.988 texels; the receiver-plane A/B on sloped receivers incl. an occluder square), test_fp_warp 66,
  test_options_store 365 (the row, presets, the env lock in the store's grammar: GFX_SHADOW_FILTER=Soft runs and shows
  soft, `1` is refused and ps3 runs and shows), test_ovl_menu 1351 (Shadows group after Textures, three choices,
  Ultra -> Softer + restart pending); ss_manifest gate 0 unclassified (s_fp_zfilt / s_fp_zplane_max / s_fp_zsoft_hlsl
  BOOT, s_zfilt BOOT); m4c alltests.sh: every suite rc 0 (unit, kernel, spu_host 833 + 62, spu_load 79, fsfd, gcm 47 + 88, libs; the known non-gating strict-pass-share print).
- **Not done / open:** shadow-map RESOLUTION (P8b) is the other half; the title's own multi-tap switch (answering its
  CMultiTapShadowEnable query, NOTES 2026-10-20) is not offered; the restart row's first launch compiles the new
  variants once (not measured as a hitch: every variant was in the cache by the time of the timed runs). Softer
  widens penumbrae to 4 texels everywhere, contact shadows included (a matter of taste: Soft is the High preset's).
- Review (one read-only reviewer: the PS3 path and cache keys, the latch order, the "sampled once" rule, the kernel
  HLSL, buffer sizes, the row / presets / menus, save states, this entry; test_fp_decompiler 352 and test_ztex_pcf 678
  rebuilt and passing on WARP and the RTX 5080; nothing high or medium). Fixed: the soft kernels' fp32 weight sum is
  1 +- 2.4e-7, so a fully lit tap could read 1.0000002 (the PS3 kernel gives exactly 1): a program computing 1 - s and
  feeding POW / LG2 would get NaN and the output guard a black pixel. rsx_ztexs now returns exactly 1 / 0 within 1e-6
  of them (all three variants; partial values move by < 1e-6); test_ztex_pcf: all lit -> 1 with max|err| 0. Wording:
  the degenerate receiver plane keeps the PS3 kernel only with the ZTEX_SOFT_PLANE A/B (test header, entry above);
  the control-byte comment in rsx_fp_decompiler.h. In game after the rebuild: soft and softer runs, 4 soft programs
  each, 0 decompile / compile failures. Not changed (documented): the "sampled once" rule cannot see one shadow map
  bound to several units or a multi-pass filter (another title could get a double blur; this title's three
  multi-tap resolves sample one unit, and its maps are separate 1024x1024 textures); a soft variant that failed to
  decompile would drop its draw rather than fall back to the PS3 kernel (all 1,398 depth programs of the shader
  list decompile and compile at soft / softer); a player who matched High / Ultra before reads "Custom" until a
  preset is picked again (shadow_filter stays ps3). alltests.sh rc 0 everywhere, manifest gate 0 unclassified.

## 2026-10-27 (morning) — P7: a window-sized picture at every factor (Upscaling: bilinear / bicubic / Lanczos-2), a Variable refresh row, the presentation mode from software
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video/rsx_upscale.h (new; tests/test_upscale.c), rsx_res_pass.{c,h} (+ tests/test_res_pass_warp.c),
rsx_d3d12_backend.c, ovl_menu.c (+ test), libs/system/options.c (+ test); config/ss_manifest.txt
- **What:** two rows. `upscale` (Graphics > Resolution, "Upscaling": `off` = Off / Windows (PS3) | `bilinear` | `bicubic`
  (Catmull-Rom) | `lanczos` (Lanczos-2); OPT_RESTART; env `GFX_UPSCALE`; preset PS3 = off, the others leave it). Off is
  today's 1280x720 swap chain stretched by Windows. The others give the swap chain the view's size at 1x too and run
  the RES_SCALE window pass (rs_window_present / rsx_rs_blit, generalised: `rsx_rs_blit_opts.filter` picks a second
  pixel shader; filter 0 runs the old shader unchanged) from the guest backbuffer -- after the aa / sharpen stage 0,
  the gamma and every guest-visible copy. At RES_SCALE >= 2 the same row picks the downsample kernel (off = today's
  box / area average). `vrr` (Frame > Frame rate, "Variable refresh (G-Sync / FreeSync)"; OPT_RESTART; env
  `PRESENT_VRR`): a window-sized (bilinear when upscale is off) and tearing-capable swap chain at start; switching it
  in a menu also sets, through the store (so both menus show what runs), vsync 0, tearing 1, fps_cap = the cap
  (rsx_vrr_cap_hz: min(refresh - 3, refresh - refresh^2 / 3600) = 72 at 75 Hz; a lower limit stays), upscale bilinear
  when off and frame_pacing off (FRAME_LIMIT presents every k-th refresh: with vrr it ran at interval 3, 25 fps);
  off puts vsync 1 / tearing 0 / no limit back where they still hold VRR's values. The fps limit's choices gain
  "72 fps (variable refresh: just under 75 Hz)". Restart rows because the swap chain's size class and flags are set
  where it is created (init_d3d12: window-sized or 1280x720, tearing / waitable flags, render_targets[] swapped for
  private guest backbuffers by rs_window_setup); a live switch would re-create the swap chain on the view with the GPU
  idle and re-wire both RTV heaps -- the res-scale window pass has the same rule.
- **Kernels (rsx_upscale.h, tested CPU twin rsx_up_sample):** per axis ratio = S / D, the kernel stretched by
  max(1, ratio) (a downsample filters what the smaller picture cannot hold), every texel within radius * scale of the
  pixel's centre a tap (<= 16 per axis), exact texels (Load, edges clamped), weights normalised. Tent (r 1),
  Catmull-Rom (B 0, C 1/2, r 2), Lanczos-2 (r 2). Anti-ringing (bicubic, Lanczos; `RSX_UPSCALE_ANTIRING=0` off):
  clamp to the min / max of the main-lobe taps (|x| < 1 on both axes: the 2x2 texels when upscaling). Linear light
  only when an axis shrinks; upscaling interpolates the encoded values like the compositor. 1:1 = the texel itself.
- **Default = today's output and presentation:** up_cfg returns 0 at upscale off / vrr 0 / RES_SCALE unset, so the
  swap chain, its flags and every resource are what they were; the kernel shader is compiled only when a blit asks for
  a filter. Evidence (p7/id, rs2/idcmp.py; base = build\infamous2.exe copied to p7/base before the first build, 3
  runs; new exe 2 + 2 final at defaults, one with an options.ini `upscale = off, vrr = 0`, one with
  `GFX_UPSCALE=off PRESENT_VRR=0`): 16 CELLMARK fast-boot dumps each, the deterministic boot frames 0-4
  byte-identical in every base / new pair (5-8 identical per pair; 5+ are the fading warning screen, base vs base
  differs there too), 220 artefact keys (PSOs, samplers, surfaces, swap chain) in both, 0 different descriptions, no
  [RSCALE] / [upscale] line. New at the defaults: the `[present]` lines below (a QPC compare per present, one sample
  a second on the render thread, <= ~110 us).
- **The presentation mode from software (`[present]`, PRESENT_MODE_LOG=0 off):** IDXGISwapChainMedia::
  GetFrameStatisticsMedia is E_NOINTERFACE on a D3D12 swap chain, and DwmGetCompositionTimingInfo's counters advance
  once per call on this Windows build (p7/dwmprobe.c: +1/s with a 60 Hz GDI animation running). What works,
  unelevated: the kernel's count of the frames THIS process put on the monitor's VidPN source itself
  (D3DKMTQueryStatistics, D3DKMT_QUERYSTATISTICS_PROCESS_VIDPNSOURCE, gdi32 loaded at run time). About one per
  present = INDEPENDENT FLIP (our buffers go to the screen: a full-screen flip or a hardware plane), none = COMPOSED.
  Logged on a change and once a minute with the DXGI statistics' display refreshes per shown frame (SyncRefreshCount /
  PresentCount). Findings on the test PC (2560x1080, RTX 5080):
  - today's default is composed only for the first second and after a PrintWindow screen grab; then INDEPENDENT FLIP,
    windowed (1707x720, the 1280x720 chain stretched: a scaling hardware plane) and in borderless fullscreen
    (`[present] now INDEPENDENT FLIP ...: 50 presents/s, 50 of our frames/s flipped to the display by us, 1.50 display
    refreshes per shown frame -- 1280x720 swap chain 1280x720, windowed, swap interval 1, vrr off`). So "DWM composes
    the stretched picture" was not true here in steady state (critique C8 was right: an MPO scaler). Cross-checked from
    another process (p7/vidpnprobe.c): the game's frames on the source = its present rate, the source's global count
    the same (DWM idle); after a screen grab the process had none and DWM flipped at 75 Hz.
  - window-sized (bilinear) and VRR arms, fullscreen: INDEPENDENT FLIP the same way (the VRR arm with swap interval
    0 + ALLOW_TEARING; its frames are the source's only flips).
  - variable refresh: NOT active on this monitor as set up. With vrr on, independent flip, interval 0 and the 72 cap,
    99.9-100% of the display intervals stay on the 75 Hz grid and the monitor shows 1.09 refreshes per frame at 69 fps
    (a display following the frames shows 1.00). The new check (rsx_vrr_verdict as first written: 6 of 8 seconds at 80-94% of the
    refresh) logged `[present] variable refresh NOT ACTIVE: the display keeps its fixed 75 Hz (1.09 refreshes per shown
    frame at 69 fps ...) so vsync 0 tears. Turn G-SYNC / FreeSync on ...` and a 10 s notice. G-SYNC Compatible
    (NVIDIA Control Panel) and FreeSync (the monitor's menu) must be switched on by the player; when they are, the same
    lines say ACTIVE.
  - the minute line (windowed VRR rows, p7/runs/p7_vrrlong3): `[present] last 60 s: composed 2 s, independent flip 57 s,
    unclear 1 s; 3869 presents, 3736 flipped by us; 1.15 display refreshes per shown frame; vsync-time split (kernel,
    ms): on 0, off keeping phase 0, off no phase 0; variable refresh NOT active (fixed refresh); the sample took <= 109
    us -- window-sized swap chain 1707x720, windowed, swap interval 0 + ALLOW_TEARING, vrr on` (the kernel's WDDM 2.7
    vsync-time fields stay 0 with this driver).
- **Frame time** (p7/campaign.sh: the 64:27 fast-boot state p7/fm_wide.p3ss, borderless fullscreen 2560x1080, 75 Hz,
  3 interleaved rounds, FRAME_TRACE; p7/ftsum7.py; arms: today's defaults / upscale bilinear (vsync 1) / the VRR
  rows as the menu writes them (opt_vrr.ini: vrr 1, vsync 0, tearing 1, fps_cap 72, upscale bilinear)):
  | scene | arm | fps | present p50 / p95 / p99 ms | sd | judder | shown 1 / 2+ refreshes | on the 75 Hz grid | GPU ms | window pass |
  |---|---|---|---|---|---|---|---|---|---|
  | still | today | 49.04 | 20.39 / 22.59 / 24.17 | 1.13 | 0.74 | 47.1 / 52.9 % | 100 % | 2.22 | -- |
  | still | window-sized | 49.45 | 20.28 / 21.91 / 23.49 | 0.98 | 0.68 | 48.3 / 51.7 % | 100 % | 2.24 | 0.053 |
  | still | VRR rows | 49.63 | 20.16 / 21.84 / 23.52 | 1.01 | 0.67 | 48.7 / 51.3 % | 100 % | 1.99 | 0.042 |
  | pan | today | 63.84 | 14.56 / 20.74 / 22.74 | 2.78 | 0.75 | 82.2 / 17.8 % | 100 % | 1.14 | -- |
  | pan | window-sized | 64.39 | 14.53 / 20.57 / 22.76 | 2.81 | 0.70 | 83.0 / 17.0 % | 100 % | 1.18 | 0.049 |
  | pan | VRR rows | 63.41 | 14.54 / 20.48 / 22.03 | 2.50 | 0.65 | 81.8 / 18.1 % | 99.9 % | 1.22 | 0.042 |
  (judder = mean |interval[i] - interval[i-1]|; display intervals 13.3 / 26.7 ms with an sd of 5-6.7 ms in every
  arm.) No arm costs fps (spread within +-1.2%, A/A +-0.4-1%); nothing changes on screen until VRR is active -- and
  the still view (49 fps) is below this monitor's 56 Hz VRR floor anyway (no LFC: 75 / 56 < 2).
- **Cost of the filters** (fullscreen 2560x1080, still, 2 rounds, k1): off 49.16 fps, bilinear 49.06 (window pass
  0.061 ms GPU), bicubic 49.20 (0.177 ms), Lanczos-2 49.17 (0.175 ms). Windowed 1707x720 Lanczos 0.07-0.10 ms;
  RES_SCALE 2 into 1707x720: box 0.018 ms, Lanczos 0.20 ms (6 x 8 taps). The GPU ends the frame long before the next
  render_frame's wait: no fps cost.
- **Picture** (p7/crops_p7_hud.png, crops_p7_scene.png: 2560x1080 output pixels x3; one frame through every kernel via
  RSX_CAP_UPSCALE_ALL, next to the 1280x720 frame's own pixels and a PrintWindow grab of the default run): bicubic and
  Lanczos-2 keep the subtitles' and the call box's strokes crisper than bilinear / Windows' stretch; railing, lamp post
  and roof edges less blurred. Halos (p7/wincmp.py, the frame's 2x2 source range per output pixel in four HUD
  regions): bilinear, clamped bicubic and clamped Lanczos 0 values outside it; Lanczos unclamped 2006-4434 values,
  up to 27/255 (WARP: a 26 | 230 step gives 9..247 unclamped, 26..230 clamped). The presented buffer is
  byte-identical to the capture path's Lanczos re-run.
- **Works with:** present = fit (the chain is the picture's view; bars outside it) and stretch (the whole client:
  `window-sized swap chain 1600x900 (present = stretch: the whole window)`, Lanczos 0.09 ms);
  window resize smaller / wider (bars) and minimise (p7/steps/winops.txt: `swap chain resized 1707x720 -> 1284x541 ->
  1520x641`, 0x0 skipped), F11 / the window menu's fullscreen and back (-> 2560x1080 -> 1520x641), the menu bar, the
  F1 overlay (its own DirectComposition layer), aa Fast + sharpen 50 at 1x (stage 0 into the private backbuffer, the
  window pass reads it) and at x2 (stage 1, the kernel reads s_pfx.out). FRAME_LIMIT=auto + PRESENT_VRR=1 resized
  too: ResizeBuffers now repeats the creation flags (s_sc_flags; it passed only ALLOW_TEARING, so a waitable swap
  chain at RES_SCALE >= 2 could not resize -- an older bug).
- **Knobs:** GFX_UPSCALE, PRESENT_VRR (lock the rows, store grammar); RSX_UPSCALE_ANTIRING=0; RSX_CAP adds
  window_WxH.bmp (the presented buffer), RSX_CAP_UPSCALE_ALL=1 the same frame through each kernel (1x);
  GPU_TIME=1 adds "window pass X ms"; PRESENT_MODE_LOG=0.
- **Tests:** test_upscale 514 (kernels: K(0) 1, zeros at the radius, partition of unity for the tent and Catmull-Rom,
  Lanczos-2 within 2.5%; taps cover the support at 19 axis pairs; normalised 2-D sums 1; 1:1 identity; flat colour;
  a HUD-like stroke and step: 96 halo pixels unclamped, 0 clamped, nothing outside the 2x2 range; the linear rule; the
  bars at 9 client sizes / aspects + stretch, edge-to-edge mapping; rsx_pm_decide incl. the defaults; the [present]
  classifier; the VRR cap, plan and verdict), test_res_pass_warp 97 (16 + up.twin: GPU vs the CPU twin within 1/255
  for 3 kernels x clamp x 5 ratios, 1:1 exact; up.flat; up.ring; up.gamma), test_options_store 350 (+ the rows, env
  locks in the store's grammar: GFX_UPSCALE=Bicubic runs and shows bicubic, a spelling the store refuses keeps off),
  test_ovl_menu 1323 (the 72 choice), test_res_scale 172, test_postfx 61, test_postfx_warp 39, test_display_cfg 72;
  ss_manifest gate 0 unclassified (s_sc_flags BOOT, s_gt_win DIAG, s_pmon KEEP); m4c alltests.sh: every suite rc 0 (unit, kernel, spu_host 833 + 62, spu_load 79, fsfd, gcm 47 + 88,
  libs; the known non-gating strict-pass-share print).
- **Menus** (p7/shots): the native PC Settings Graphics page (Upscaling *, changed to Lanczos-2) and Frame page
  (Variable refresh *), the F1 Graphics tab (Upscaling, RESTART) and Frame tab: switching Variable refresh on shows
  the limit 72 fps (variable refresh: just under 75 Hz), Vertical sync Off, Allow tearing On, Even frame pacing Off.
- **Not done / open:** VRR could not be seen working here (the monitor stayed at 75 Hz); the ACTIVE branch of the
  verdict is untested in game. The [present] counter cannot tell a full-screen flip from a hardware overlay plane
  (both are the process's own frames); PresentMon would (a download; ETW needs elevation). Sharpening still runs
  before the upscale (design 3.4 puts it after). No "edge-adaptive" upscaler. CELLMARK dumps and thumbnails stay the
  1280x720 frame (before the window pass).
- Review (one read-only reviewer: the default path, the 1x window-sized chain and what dumps / thumbnails / captures /
  the overlay see, the vrr row's side effects, kernel math, swap-chain flags, save states, these docs; test_upscale,
  test_res_pass_warp, test_options_store, test_ovl_menu rebuilt and passing; nothing high), three medium and five
  low findings. Fixed:
  - The NOT ACTIVE check could not decide at the row's own 72 cap (it only judged 80% of the refresh to refresh /
    1.06 = 70.75 fps at 75 Hz), so a VRR-less display tore at 72 fps with no word. rsx_vrr_verdict now compares the
    display's refreshes a second (refreshes per frame x fps) with fps (follows) and hz (fixed), for 80% of the
    refresh <= fps <= refresh - 2.5: 72 fps on 75 Hz decides (test: +-1 refresh of counting error still splits).
  - Switching vrr on and off was not a round trip: off reset vsync 1 / tearing 0 even when VRR had not set them (a
    a player's own uncapped vsync 0 + tearing 1 was lost) and never gave back upscale or even frame pacing; at 2x-4x it
    also swapped the downscale from the box average to a tent. Now on records exactly the rows it changed, from what,
    in a hidden row (`vrr_restore = fps_cap=0>72,upscale=off>bilinear,frame_pacing=auto>off`; it outlives the
    session), off puts back each recorded row that still holds VRR's value, and upscale is only touched at the PS3
    resolution. In game (p7/rev/vrrchk.sh, a scratch options.ini with vsync 0, tearing 1, frame_pacing auto): on set
    the limit 72, upscale bilinear, pacing off and recorded them; the next launch's off restored limit 0, upscale off,
    pacing auto and left vsync 0 / tearing 1.
  - The menus could disagree with what runs: the PS3 preset now switches vrr off (its PS3 value is 0), and an
    explicit PRESENT_TEARING=0 is honoured under vrr (the tearing row shows Off and the chain is not
    tearing-capable). With Upscaling set to Off by hand while vrr is on, the picture is still window-sized bilinear
    (VRR needs it): said in the row's help.
  - The `[present]` sample now runs after the frame-time notes (FRAMETIME_LOG / FRAME_TRACE / the issues log no longer
    count it in "Present held"); an adapter open that fails (Remote Desktop, a basic display driver, a monitor on
    another GPU) is retried every 30 s instead of every second; a new monitor resets the VRR verdict; "so vsync 0
    tears" now reads "frames tear with vsync 0"; the vrr help names everything it sets and that frames tear below
    the monitor's range; the s_pmon manifest reason is right.
  Not changed: the monitor still samples at the defaults (one D3DKMTQueryStatistics a second, 5 us mean here; the
  issues log gains the presentation mode, PRESENT_MODE_LOG=0 turns it off); a window resize whose ResizeBuffers or
  RTV setup fails leaves the window pass off (a still picture; the flags fix removed the common cause; existed at
  RES_SCALE before); a full-screen flip and a hardware overlay plane read the same. New at the defaults is also the
  fps-limit list's refresh-3 entry (72 here; 57 / 116 / 138 on 60 / 120 / 144 Hz). Rebuilt; test_upscale 520,
  test_options_store 352; alltests.sh rc 0 everywhere, manifest gate 0 unclassified.

## 2026-10-27 (morning) — IQ-1: anti-aliasing (Fast) and sharpening in PC Settings, before the HUD
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video/rsx_postfx.h + rsx_postfx.c (new; tests/test_postfx.c,
tests/test_postfx_warp.c), rsx_d3d12_backend.c, ovl_menu.c (slider hint), libs/system/options.c (+ test); src/infamous2_hooks.cpp;
config/ss_manifest.txt
- **What:** two Graphics rows (design image-quality 3.2 / 3.4, critique B6). `aa = off | fast` (Off (PS3) | Fast; env
  `GFX_AA`) and `sharpen = 0..100` % (slider, step 5; env `GFX_SHARPEN`); both OPT_FRAME (latched at the render point:
  the next frame), OPT_F_ENHANCEMENT; presets PS3 off/0, Low off/0, Medium fast/0, High and Ultra fast/25. All
  shader code is ours (no vendored code). **Fast** (FXAA-class, one full-screen pass): Rec.601 luma of the centre and
  its 4 neighbours, skipped below a local contrast of max(1/16, 1/8 of the brightest); the 4 corners give a sub-pixel
  factor (smoothstep, squared, <= 3/4); the edge's direction from second differences across rows / columns, its side
  from the larger step; bilinear pair taps walk both ways along the edge (10 taps a side, <= 23.5 px) until the pair
  luma leaves the start's by 1/4 of the step; the nearer end, when the luma turns the right way there, gives
  1/2 - d / length; one bilinear tap across the edge by the larger of the two. **Sharpen** (CAS-like): per channel
  min / max of the 3x3 cross, amount = sqrt(saturate(min(mn, 1 - mx) / mx)), weight -amount * setting * 0.2, out =
  (c + w (n + s + e + w)) / (1 + 4 w) (normalised: flat colour unchanged, 0 = identity; rsx_pfx_cas_px is the CPU twin).
- **Before the HUD, invisible to the guest (critique B6):** the title hook (src/infamous2_hooks.cpp, if2_postfx_install ->
  `rsx_postfx_set_scene_end` / `_set_movie`, toolkit API in rsx_postfx.h) names the final composite `scene*(1-bloom.a) +
  2*bloom + glow` (GROUND_TRUTH 8): **E933D344** (play, in-engine cutscenes) and **F4BBE02F** (the same + a colour grade:
  under the pause menu, a Kuo cutscene); 75552C13 (Bink) is the movie program. Found in the RSX_CAP manifests: in every
  pier capture (27, rs2/cap) E933D344 is the one full-screen blend-off triangle into the display surface (0x734000 /
  0x3B0000, reading units 0, 1 = scene 0x11C0000, 5 = particles, 6 = bloom), followed only by HUD draws (minimap
  8C3E6603 / 0B356610 / EFCF73BE, text A426D9C6). Our hash over RPCS3's 322-program cache for the title
  (iq1/fpcensus.py): these two are the only programs reading exactly units 0, 1, 5, 6 with the 2.0 gain and the luma
  weights. In the replay (only while a pass is asked for) a 64-bit filter of the named ids is tested per draw (one
  shift-and-test; at the PS3 values not even that: one flag); after a scene-end draw into a display-size RGBA8 surface
  the surface is copied (pfx_snap: the scene). At the end of a presented frame, after the screen copy and the display
  write-through and before the video-out gamma (stage 0, unscaled) or before the RES_SCALE window pass (stage 1,
  scaled), the passes filter that copy and write the presented picture -- the backbuffer, or a host-size texture the
  window pass resamples instead of the display surface -- keeping the finished frame's pixel wherever it differs from
  the copy (HUD, subtitles, menus: exact). The display surfaces, the readback / pick paths (0xE3C000, 0x10000) and
  every guest-visible copy are never written. No copy this frame (menus, loading, boot screens) or a movie drawn into
  the display: no pass. Without a hook (RSX_PFX_SCENE_END=frame, or another title) the whole frame is filtered.
- **PS3 values = today's output:** nothing is created (no pipeline, texture or heap; `[RSARTIFACT] postfx-*` lines only
  when a pass runs) and nothing recorded. Evidence (iq1/id, rs2/idcmp.py): base exe (4 runs) vs the new exe at defaults
  (2 + 2 final), with an options.ini `aa = off, sharpen = 0` (3) and with `GFX_AA=off GFX_SHARPEN=0` (1); 16 CELLMARK
  fast-boot dumps each: the deterministic boot frames byte-identical (3-8 per pair; base vs base 3-4), 220 artefact
  keys in both with 0 different descriptions (one run lacked one timing-dependent guest PSO, present in its two reruns
  and in base), 0 `[postfx]` lines. In play (a pier frame, RSX_CAP, VIDEOOUT_GAMMA=0, twice): the presented backbuffer
  is byte-identical to the display surface it was copied from, 921,600 of 921,600 pixels, no post textures.
- **HUD untouched (iq1/hudcheck.py on captures with the passes on; F = the finished display surface, S = the scene
  copy):** every pixel with F != S is the game's own in the output: 1x with VIDEOOUT_GAMMA=0, Fast + sharpen 50:
  65,951 pixels, max difference 0; 1x with the gamma, three arms: ~66,000, +-1 (the gamma modelled); x2 (host
  2560x1440), three arms: 258,823-259,021, max 0; Hor+ 64:27 cutscene subtitles 1x / x2: 2,774 / 9,822, 0 / 0.
  RSX_PFX_MASKVIS=1 tints the kept pixels: health / energy gauge, call box, minimap, subtitles; the pause menu + native
  PC Settings screen dim the whole frame, so all of it is kept (0 pixels filtered). The F1 overlay is its own
  DirectComposition layer. Movie frames: a checkpoint boot with the movies kept logged (build 4, before the display-only rule below) "movie
  program 75552C13 drew: movie frames get no pass" and "[postfx] last 60 s: 2263 presented frames, 451 with the passes, 797 without the scene-end
  draw (menus, loading), 1015 movie" (the city-intro movie draws 75552C13 into display surface 0x3B0000 at 1280x720).
  Only a movie drawn into the display counts: in-world video into a texture would not switch the passes off (that
  final rule has not met a movie in a run yet; an older capture, defect4, has the Bink pass drawing into 0x3B0000 at
  1280x720, which it matches; and with the HUD mask a movie's pixels differ from the scene copy, so they are kept).
- **Picture (iq1/crops_*.png, host pixels, same camera; the same-frame sheets compare the scene copy with the
  output):** the pier railing, Cole's poles / backpack / arm and the building lose their stair steps with Fast;
  sharpen 50 lifts plank and wall texture contrast; Fast changes 13.9% of the scene's pixels at 1x, Fast + sharpen
  42%. WARP (synthetic 1/5 and 1/7 slanted hard edges): the largest step along the edge 200 -> 60 / 46, in-between
  pixels 0 -> 251 / 270, mean error against the exact coverage 8.37 -> 3.92 / 8.36 -> 3.57; flat colour exact; a
  one-pixel checker keeps 18% of its contrast (why the HUD must be masked). Works at RES_SCALE 2 (the passes at
  2560x1440, then the window pass), in a 1600x675 and a 1707x720 window, Hor+ 64:27 (checkpoint boot: the fast boot's
  state adopts its own 16:9 shape).
- **Cost** (fast boot pier, 1280x720 window, vsync 75 Hz, 3 interleaved rounds, iq1/campaign.sh, iq1/ftsum.py; A/A
  +-0.4%): still (heavy city view): off 52.37 fps, p50 / p99 19.05 / 22.52 ms, GPU busy 2.13 ms; Fast 52.22 (-0.3%),
  19.09 / 22.75, 2.17; sharpen 50 52.46 (+0.2%), 18.99 / 22.52, 2.15; Fast + sharpen 52.38 (0.0%), 19.07 / 22.51, 2.17;
  x2 47.37 (-9.6%), 20.99 / 24.32, 4.81; x2 + Fast 47.35 (0.0% vs x2), 21.02 / 24.01, 4.91. pan: off 66.50, 14.05 /
  21.50, 1.18; Fast 66.54 (+0.1%), 1.24; sharpen 66.62 (+0.2%), 1.20; both 66.64 (+0.2%), 14.02 / 21.38, 1.25; x2 64.30
  (-3.3%), 2.23; x2 + Fast 64.26 (-0.1% vs x2), 14.44 / 22.92, 2.32. No measurable fps cost; about +0.05 ms GPU at 1x
  and +0.1 ms at x2.
- **Menus (screenshots iq1/cap/m_menu, v_ovl):** the native PC Settings Graphics page (8 of 10 rows: preset,
  brightness, anti-aliasing, sharpening, AF, texture packs, internal resolution, detail) and the F1 Graphics tab
  (no scroll) show both rows; Left / Right on the native rows and the overlay's selector / slider change them; each
  change logs `[options] aa = ...` and `[postfx] anti-aliasing ..., sharpening ...%` at the next render point.
- **Knobs:** GFX_AA, GFX_SHARPEN (lock the rows); debug: RSX_PFX_SCENE_END=<hex ids>|frame|hook (overrides the title's
  list; frame = whole frame), RSX_PFX_MASKVIS=1; RSX_CAP writes postfx_scene_WxH.bmp (the scene copy) and, scaled,
  postfx_out_WxH.bmp; RSX_RES_ARTLOG adds `postfx-tex` / `postfx-pso` lines. Log: `[postfx]` (settings, first
  textures / pipelines / pass, each scene-end program once, a minute line while on: frames with / without the pass).
- **Tests:** test_postfx 61 (parsers, CAS weight range / monotonicity / normalisation, the pass plan: none at the PS3
  values), test_postfx_warp 39 (WARP: flat exact, edges smoother, sharpen 0 identity, shader == CPU twin within
  1/255, HUD mask exact, the aa -> sharpen chain), test_options_store 327, test_ovl_menu 1281, test_res_scale 172,
  test_res_pass_warp 16; ss_manifest gate 0 unclassified (s_pfx_ids BOOT, s_pfx KEEP, s_pfxf RESET -- also cleared by
  rsx_d3d12_backend_ss_reset --, s_pfx_stat DIAG, rsx_postfx.c s_pfx_gpu KEEP); m4c alltests.sh: every suite rc 0
  (unit, kernel, spu_host 833 + 62, spu_load 79, fsfd, gcm 47 + 88, libs; the known non-gating strict-pass-share print).
- **Not done / open:** "High" (SMAA-class with generated area tables) was not started; Fast is the only AA. Sharpening
  runs at the internal resolution before the RES_SCALE window resample (design 3.4 puts it after an upscale filter,
  which does not exist yet). Composite variants not in the list (later missions, powers) keep the PS3 picture: the
  minute line counts them as "without the scene-end draw" -- check play logs. Under translucent full-screen overlays
  (pause menu dimming, fades drawn after the composite) the frame keeps the PS3 pixels. The "Off (PS3)" label
  follows GROUND_TRUTH ("no anti-aliasing"); the image-quality critique's C15 (an SPU MLAA on PS3?) is still open.
- Review (one read-only reviewer: the PS3 path, guest isolation, the HUD mask, GPU resources, shader math, the id
  filter, the rows and both menus, save states, these docs; test_postfx 61 and test_postfx_warp 39 rebuilt and
  passing; nothing high or medium), eight low findings. Fixed:
  - GFX_AA / GFX_SHARPEN were read by the backend with a wider grammar (1, fxaa, ps3, none, 50%) than the settings
    store's: GFX_AA=1 ran Fast while both menus showed "Off (PS3)". The backend now takes the store's values only
    (the variables lock the rows there; a spelling the store refuses keeps the lower layer's value, which runs).
    In game (iq1/rev/chk.sh): GFX_AA=fast runs Fast; GFX_AA=1 is refused by the store and nothing runs; an
    options.ini aa = fast, sharpen = 50 runs both passes.
  - A pass that can never run kept copying the display surface every frame (3.7 MB at 1x, 59 MB at x4): after a
    compile / pipeline failure (rsx_pfx_failed) and with RES_SCALE + RSX_RES_PRESENT=guest the passes are now
    switched off for the session (s_pfx.dead: no watch, no copies, textures freed).
  - The scene copy now also requires a display buffer (cellGcmOffsetIsDisplay), so a composite program drawn into
    another 1280x720 surface cannot replace the display's copy (that frame would have lost its pass).
  Not changed (documented here): at RES_SCALE the slot thumbnails, CELLMARK dumps and GPUP probes show the frame
  without the passes (they read the backbuffer before stage 1; at 1x they include them); a HUD pixel exactly equal
  to the scene pixel under it is filtered like the scene (rare, an edge-adjacent speck at most); after the passes
  have been on once the root signature, PSOs (<= 4) and descriptor heaps stay for the session (textures are freed);
  the first pass compiles its shader on the render thread (a one-time hitch, normally while the menu that switched
  it on has the game paused) and dragging Sharpening across 0 frees / remakes its textures; the help text's "the
  HUD stays" needs a title hook naming the scene end (another title gets whole-frame filtering);
  rsx_postfx_set_scene_end is meant to be called at startup (s_pfx_ids BOOT, not synchronised with the render
  thread); players on the old High preset now read "Custom" (presets are derived from the rows). Rebuilt; alltests.sh
  rc 0 everywhere, manifest gate 0 unclassified (pfx_latch.env_a / env_s gone).

## 2026-10-27 (morning) — resolution scaling stage 2: the "grid" on Cole's face was Edge's small-triangle culling
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video/rsx_res_scale.h (+ tests/test_res_scale.c), rsx_d3d12_backend.c,
runtime/spu/spu_dma.h + spu_channels.c + spurs_job.c (an SPU GET hook), libs/system/options.c (render_scale row);
config/ss_manifest.txt
- **Symptom:** RES_SCALE=2, Cole seen from the front on the New Marais pier: a speckled grid of dark holes over his
  face (mouth, chin, cheeks, eyes); also at x3 / x4, never at 1x. Local captures (in cap/, crops *.png).
- **Not a 1x consumer, not depth.** The holes are already in all three G-buffer planes (RSX_CAP of the G-buffer
  batch) and stay with the face's depth test forced to ALWAYS (new debug knob `RSX_DBG_ZFUNC=<fp id>:<0-7>`, checked
  with NEVER: the face vanishes), with `NO_ALPHATEST=1`, `USER_CLIP=0`, `VCOMPACT=0`, and in three consecutive frames.
  Op snapshots of the five face draws (fp 39FA852B; new `RSX_CAP_OPSNAP_OPS=<a>-<b>` limits RSX_CAP_OPSNAP to an op
  range) show each draw leaving 1-2 host-pixel slivers uncovered, with the Amp's depth (farther) still under them:
  the face's triangles do not cover those pixels. The face draws' vertex counts change every frame (2220 / 2391 /
  2445 / 2307 for the first one): the mesh is culled on the SPU each frame.
- **Cause:** the title runs Edge Geometry (SPURS workload "EdgeGeomJobs"). Edge culls, besides back faces and the
  frustum, every triangle that covers no SAMPLE POSITION of the target, from the EdgeGeomViewportInfo the PPU fills
  (128 bytes: u16 scissor[4], depth range, view-projection matrix, viewport scales +0x50, offsets +0x60, sampleFlavor
  +0x70). The PPU's copies hold scissor (0,0,1280,720), scale (640,-360), offset (640,360), flavor 0 (found in a raw
  save-state image, `SS_DUMP`); every Edge job GETs the 128 bytes with one DMA (SPU_DMA_RANGE: cmd 0x40, size 128).
  At x2 the rasteriser samples 2x2 host centres per guest pixel, so a sliver dropped for missing the ONE guest centre
  would have covered host centres: a hole. Faces, lips, eyes and distant meshes are dense with such slivers.
- **Fix, `RSX_RES_EDGECULL=auto|flavor|scale|off` (default auto = flavor):** while the latched factor is >= 2 the
  renderer installs an SPU GET hook (`g_spu_get_hook`, spu_channels.c; called after every GET and GET-list element
  in both spu_dma.h paths; NULL otherwise: one load per GET) that rewrites the job's LOCAL-STORE copy of a viewport
  of the display (|scale| * 2 == 1280x720, offset == |scale|, scissor inside, flavor 0); guest memory is never
  written (rsx_rs_edge_vp_patch, pure). `flavor`: for an even factor the guest grid with libgcm's SQUARE_CENTERED_4
  pattern (samples at +-1/4 px = the x2 host centres), scaled by k/2 (x4 = x2 scale + the pattern); odd factors
  scale. `scale`: scissor, scales.xy, offsets.xy times k. Both remove the holes at x2 and x4 (static pier view, far
  depth spikes in Cole's region 436 -> 207 / 217 at x2, 1088 -> 112 / 107 at x4; what is left is alpha-tested
  foliage and real gaps, equal in every arm); auto keeps Edge's screen coordinates in the 1x range at x2 and within
  2560 at x4. Shadow cascades (1024^2), the 256^2 reflection and the 512x288 particles keep their own viewports.
  Holds under Hor+ 64:27 (the viewport stays 1280x720). 47-59 rewrites per guest frame on the pier. Crops:
  rs2/crop_face_off_x2before_x2_x3.png (host pixels), crop_face_window1280.png (the 1280x720 window).
- **Also checked at x2 / x3 (host-size captures against off, same view):** the four 1x passes that read scaled
  surfaces in a gameplay frame -- B9D13BD0 (DOF 4:1, one LINEAR tap) on 0xE3C000 and 0x11C0000, E95881C2 (glow, 4
  NEAREST taps) on 0x3B0000, 7DAE6222 (packed depth, NEAREST) on 0x10000 -- produce 1280x180 / 320x180 buffers that
  match off; the LINEAR tap's footprint is half the PS3's per axis at x2 (a third at x3), invisible after the
  title's own blurs, so no resolve pass was built (design P4 stays open). The SPU light / AO buffers stay 1x and are
  magnified per guest pixel (daylight pier: few local lights, no fringe seen). Shadows (1x maps, PCF per host pixel),
  self-sample copies (host-size), HUD, subtitles, the pause menu and the native PC Settings screen are clean; the
  blocky DOF and particle edges are the title's 1/4-res buffers (the same at 1x). F5 / F9 at x2: 232 MB slot (image
  529 MB), loaded, next frame correct. Box-downsampled x2 vs off over the whole city view: edges and animation only.
- **Option `render_scale`** (Graphics > Resolution, "Internal resolution": PS3 (1280x720) | 2x | 3x | 4x; OPT_ENUM,
  OPT_RESTART, enhancement; preset PS3 = ps3, the other presets leave it alone; env RES_SCALE locks it). rs_cfg reads
  it once at start when RES_SCALE is unset: the window-sized swap chain and the surface pool are set up for the factor
  there, and the live path (rsx_res_scale_request) has no caller and no window pass for a 0 -> 2 switch, hence a
  restart row. The F1 overlay and the native PC Settings screen list it ("Internal resolution *"). Default PS3.
- **RES_SCALE off is unchanged:** the hook is installed only by the latch at a factor >= 2; the row's default parses
  to 0. Evidence (rs2/id, idcmp.py): base exe vs new exe, RES_SCALE unset, 16 CELLMARK boot dumps + RSX_RES_ARTLOG:
  the deterministic boot frames 0-4 byte-identical in every run (5+ are the fading warning screen, timing-dependent:
  base vs base differs there too), 220 artefact keys (PSOs, samplers, surfaces, swap chain) in both, 0 different
  descriptions, no [RSCALE] / Edge line. RES_SCALE=1 + RSX_RES_VERIFY: 10,694,068 checks, 0 mismatches.
- **fps / frame time** (fast boot, New Marais pier, 1280x720 window, vsync on (75 Hz), 3 interleaved rounds, rs2/ft via
  rs2/campaign.sh, p2/ab.py, tools/frametrace.py; A/A spread +-0.4%):
  - still (the heavy city view, ~9000 draws): off 52.2 fps, x2 47.1 (-9.8%), x3 40.6 (-22%); x2 without the Edge fix
    47.2. Frame time p50 / p99: 19.2 / 22.7 ms, 21.2 / 24.8, 24.6 / 28.2. GPU busy 2.1, 4.9, 9.0 ms/frame: at this
    serial frame (critique C4) 70-80% of the extra GPU time lands on the frame.
  - pan (steady right stick): off 66.5, x2 64.1 (-3.6%), x3 60.7 (-8.6%), x2 no fix 64.5; p95 20.0 / 21.7 / 24.8 ms,
    frames > 1.5x median per run 39 / 81 / 189, presents held 2 vblanks 209 / 280 / 372 of ~1600.
- **Knobs:** RSX_RES_EDGECULL (above); debug: RSX_RES_EDGECULL_LOG (review, below), RSX_CAP_OPSNAP_OPS, RSX_DBG_ZFUNC; the RSX_CAP manifest prints a draw's
  polygon offset (`pofs=`): only the 1x shadow cascades use one ((1, 2000)), so the main class needs no depth-bias
  scaling.
- **Tests:** test_res_scale 172 (151 + 21 edge.*), test_res_pass_warp 16, test_options_store 315 checks,
  test_ovl_menu 1241 checks, m4c alltests.sh all suites rc 0 (unit, kernel, spu_host 833 + 62, spu_load 79, fsfd, gcm 47 + 88, libs); ss_manifest gate: 0 unclassified.
- **Open:** the 1x-consumer resolve (box for LINEAR, pick for NEAREST / packed data), the depth-aware upsample of the
  SPU light / AO buffers (P5), x2's +0.25 px SPU inputs (C5); P3 (native 2560x1080 = 2.0 x 1.5) needs the record-time
  host map for fractional factors, per-axis Edge viewports and a factor that follows the window.
- Review (one read-only reviewer: the default path, the hook's placement, the matcher, save states, the option row,
  the knobs, this entry; nothing in the default path -- RES_SCALE unset installs no hook, nothing exports render_scale
  to the environment, s_rs changes only at the latch), one medium and three low findings:
  - The hook sees every SPU program's 128-byte GETs, not only Edge's: a job that shared the struct (a lighting job)
    would be handed a scaled viewport at x3 / x4. The hook now gets the SPU's image; each image a block is rewritten
    for is logged once, and `RSX_RES_EDGECULL_LOG=1` keeps the rewritten blocks' EAs and reports a later GET of one
    that no longer matches. In game (rs2/rev/diag.sh: x3 pan, x2 still, x4 pan): one image only (41, the SPURS kernel,
    which issues every job's input DMAs -- so the image cannot single out Edge), first block 0x492EA3F0, 48-60
    rewrites per guest frame, and 0 GETs of a rewritten block that no longer matched in about 187,000 rewrites: no
    PUT writes the patched bytes back. The SPU light buffers (main-memory textures 0x47400B80 / 0x47784B80, the
    G-buffer pass's t4 / t5; the first holds the pier's few local lights) are byte-identical between EDGECULL off,
    flavor and scale at x2 and between off and flavor (= x2 viewport + pattern) at x4 (rs2/rev/lightcmp.py over the
    rs2 captures): no lighting job reads the rewritten block. Default run: no [RSCALE] line.
  - Host-staged SPURS jobs (runtime/spu/spurs_job.c copies a job's cache and input lists straight into LS) bypassed
    the hook: those copies now report to it like a GET (this title's Edge jobs use MFC GETs).
  - A raw SPU's local store is guest memory: the hook is not called for it (spu->ls != ls_store), so the "never
    writes guest memory" rule holds there too.
  Not changed: a RES_SCALE spelling the settings store does not know (x2, 1, " 2") locks the row showing the lower
  layer's value (the renderer's own parse decides; the variable is a test knob). Rebuilt; alltests.sh rc 0
  everywhere, manifest gate 0 unclassified (s_rs_edge_img / _ea / _back DIAG, s_rs_edge_log BOOT); test_ss_core
  phase 9's 2 s give-up read 2013 ms once (timing under load, as before), its re-run passes.

## 2026-10-27 (early morning) — M7b part 1: save states survive rebuilds (build policy) + Undo last load
EBOOT: BCUS98125 update 1.04 | ps3recomp runtime/savestate (ss_file, ss_slots, ss_ui + tests), libs/input/pad_hotkeys.h,
libs/video/rsx_winmenu.c (+ tests), runtime/syscalls/sys_ppu_thread.c (SS_FAULT=cont_miss); config/savestates.ini,
input.ini, ss_manifest.txt; docs/SAVESTATES.md
- **Why:** every rebuild made all of the player's slots "(made by another build)" (build id = build text + PE link
  stamp), so F5 states died with each fix shipped. The fast boot had loaded one state across many rebuilds with the
  `build_check = 0` experiment.
- **Build policy (design 5.4 / decision 8), now the default (`build_check = 0`).** A state of another build id for the
  same EBOOT loads when every other guard passes, with a warning. Refused, before anything stops
  (ss_file_check_compat_ex; short reason for the menu, the full one in the log): title / disc version / EBOOT SHA-1
  (`another game`, `another game version`); a section's version or layout hash (`another build: layout changed`); an
  unknown mandatory section (`another build: unknown data`); a mandatory section the file lacks (`another build: data
  missing`); the SPU program table (`another build: SPU code changed`). Then the LOAD's own pre-check, still before
  its stop (sys_ppu_thread.c): a return address with no continuation, or a wait kind with no resumer -> "it was made
  by another build of the port, which this build cannot resume (tid ...)", and the menu greys that slot `(another
  build: cannot resume it)` until the file changes. A refusal never stops the game; only a failure past DISCARD (the
  existing point of no return) is fatal. `build_check = 1` keeps the strict rule ("... and build_check = 1
  (savestates.ini) loads only this build's").
- **The warning:** SS_COMPAT_OTHER_BUILD (ss_file_check_compat_ex's *warn, ss_file_image.compat_warn). Menu tag
  `(older build)` / `(newer build)` / `(another build)` from the PE link stamps in the two build ids
  (ss_file_build_order; the build text's __DATE__ __TIME__ is main.cpp's compile time and stays the same across most
  rebuilds), a log line, and an 8 s notice "Loaded slot 3 -- a state made by another build of the port: something fixed
  since then may behave differently" (launch loads too).
- **One rule:** every pre-check uses ss_slots_identity + ss_file_check_compat_ex: the menu's labels and greying
  (ss_slots_read_info), F9 and the slot keys (ss_ui's request reads the label), the load itself (ss_file_load in
  load_reader), and SS_LOAD / -LoadState (the in-process load at the title: the same reader). No thumbnail pre-check
  exists yet (thumbnails are not in the menu).
- **Undo last load (`backup_before_load = 1`, default).** load_reader, after the file is read and accepted, before the
  LOAD: a SAVE request (1 s give-up) whose done() on the frame-clock thread fills the metadata (describe hook) and
  holds other stops (s_writing); the image is written to `undo.p3ss` (xpress, normal priority; keep_bak moves the
  previous one to `undo.p3ss.bak`); then the LOAD; then ss_slots_undo_settle: loaded -> the .bak is dropped; refused ->
  the .bak goes back (none: undo.p3ss is removed). A fatal load leaves undo.p3ss = the pre-load moment (relaunch +
  Undo). "Undo" is a load of SS_UI_UNDO (slot 11 in ss_ui's mailbox; ss_ui_request_undo; request_save / _load refuse
  it), with its own undo point first, so two undos swap. Launch loads make none. A moment that cannot be saved: the
  load goes ahead, logged, notice "(no undo point: ...)", the old undo point stays.
- **Menu and key:** Load > separator > `&Undo Last Load  -  today 02:59  -  <mission>` (greyed `(nothing to undo)`),
  Ctrl+Shift+Z after the review (HK_UNDO_LOAD, input.ini `undo_load`; a load takes two modifiers): Z drives nothing
  in the default game bindings; the F-keys are taken or kept (F1 overlay, F5/F9 quick, F8 mark, F10 mouse release, F11 fullscreen, F12
  debuggers, Ctrl[+Shift]+F1..F10 slots, no Alt combinations).
- **Measured** (FAST BOOT pier, 6 F9 loads of an older-build quick slot per run, same binary; m7b/runs/m_on2, m_off2):
  key to loaded 0.54-0.57 s (first 0.66) with the undo point, 0.27-0.29 s without: +0.28 s. The undo point's pause
  63-68 ms (117 ms for the first save of the session: the staging buffer grows), written in 0.18 s (237 MB, 6 threads),
  0.28-0.29 s from its request; the load's own pause is unchanged (116-143 ms). With the slots' xpress_huff the write
  took 0.30-0.31 s (204 MB) and loads 0.68-0.73 s (m_on, m_xp): the undo point uses xpress. Reading it back is faster
  too (83 ms vs 152 ms).
- **In game** (final binary, m7b/runs/u2; slots m7b/slots: n1's quick.p3ss from build 6AE05CDA, and a copy of n2's
  screen_open_final.p3ss with TIME's layout hash changed in its header, m7b/craft_layout.py): the launch state
  fm_after_boat.p3ss (build 6AE06214) loads with the warning; Load menu: Quick Load `(older build)`, Slot 2 GREY
  `(another build: layout changed)`, Undo GREY `(nothing to undo)`. Pause menu open, cursor on HELP (u1_before_f9.png)
  -> F9: the older build's Options screen, with the warning notice (u2_after_f9.png) -> Undo: the pause menu on HELP
  again (u3_after_undo.png: mean absolute difference to u1 0.01/255 vs 35.7 to u2) -> Undo again: the Options screen
  (u4_undo_of_undo.png) -> Load > Slot 2: "Slot 2 cannot be loaded: another build: layout changed", the game as it was
  (u5_after_slot2.png). Strict run (m7b/runs/s1, SAVESTATES_CFG=build_check=1): SS_LOAD refused "made by another
  build ... build_check = 1", READY fail, the slots `(made by another build)`.
- **A refusal at the LOAD's pre-check** (m7b/runs/f1, checkpoint boot; SS_FAULT=cont_miss, a new test switch in
  pput_image_check that refuses every load there as a return address with no continuation): F9 of the older quick
  slot -> undo point taken (pause 122 ms) -> LOAD refused before its stop (attempts 0, stopped 0.0 ms: the game never
  stopped, the boat cutscene plays on, f2/f3 png) -> notice "Load of the quick slot failed: it was made by another
  build of the port, which this build cannot resume (...)"; the menu greys the slot `(another build: cannot resume
  it)`; undo.p3ss is the previous undo point again, byte for byte (SHA-1 dff739e9... before and after).
- **Tests:** test_ss_file 197 checks ok (the policy: another build id with agreeing layouts loads with
  SS_COMPAT_OTHER_BUILD, strict refuses it; a changed layout or version, an unknown or a missing section, another SPU
  table and another EBOOT are refused for another build too, each with its reason and no warning; ss_file_build_order;
  the reader's older / newer / another tags and a crafted layout greyed; the undo rotation: a first undo point, a
  refused load putting the previous one back, a refused first load leaving nothing, a done load dropping the previous,
  an undo of an undo swapping), test_ss_ui 146 (labels, the undo requests, notices, refusals, _complete_ms),
  test_winmenu 99 (the Undo item greyed / enabled, Ctrl+Z, the menu command, a plain Z ignored), test_pad_hotkeys 312,
  test_input_ini 21 against the shipped input.ini (local unit7.sh). m4c/integ/alltests.sh: unit, kernel,
  spu_host, spu_load, fsfd, gcm, libs all rc 0 (the census "strict-pass share" line is the known non-gating metric);
  manifest gate --strict --map: 3843 statics in 161 files, 0 unclassified (new: ss_slots.c s_backup BOOT).
- **Not done (the rest of M7b):** the relaunch fallback (SS_LOAD_AT_BOOT), remaining-time payloads, the precise-pc SPU
  relift, bridge resume. The menu cannot know continuation coverage before a load (the header carries no return
  addresses): such a slot shows `(older build)` until its first load is refused.
- Review (one read-only reviewer; the cross-build guards, the undo point's ordering and failures, threads, the key, the
  manifest, the tests; nothing found in the file handling or the threading), three medium and two low findings, fixed:
  - Undo without its own undo point was a one-way load: the undo point is a strict SAVE (refused while a thread is in
    a bridge, a trampoline, a chain without a continuation) and the load that follows is lenient, so Undo went ahead
    without one and everything since the last load was gone (with backup_before_load = 0, always). Undo is now
    refused when its moment could not be kept ("the game could not be saved first, which Undo needs to keep the way
    back -- try again in a moment" / "Undo last load is off"). Its key is Ctrl+Shift+Z: a load, so two modifiers
    (the project's rule; Ctrl+Z alone is not bound, test_winmenu checks it).
  - Guest memory the port itself lays out was invisible to the guards (GMEM is raw pages): the PC Settings block
    (if2_menu.cpp B_* / R_*) of a build that moved a field would be misread, or a smaller one written past. New
    OPTIONAL section PCS2 (src/if2_menu.cpp): it saves only its magic; its layout hash is every block constant at its
    value, so such a state is refused before anything stops.
  - Field lists hash names and sizes, not meanings: a renumbered wait kind would pass and run the wrong resumer after
    the point of no return. New OPTIONAL section KNDS (ss_core.c): its layout hash is the park `at` values, the wait
    kinds (a _Static_assert on SS_W__COUNT makes a new kind update it) and the SPU park points at their numbers.
    States from before both sections load unchecked (OPTIONAL), as today. The SPU side the review raised is covered
    already: the SPU table hash is every lifted SPU function's LS address and image (s_registry), so a relift that
    moves an entry refuses the state.
  - A slot is greyed "(another build: cannot resume it)" only when its own image failed the pre-check (blocker "the
    image"), not for a sys_cond wait of the running session.
  - The undo point's rename / delete after the load retries for ~2 s like ss_file's (an indexer or antivirus holding a
    fresh file).
  Not changed: the undo point is taken before the load's image pre-check, so a load refused there (cannot resume, a
  file changed on disk) has paid ~0.3 s; SS_FAULT=cont_miss refuses every load (a test switch). test_ss_file 197,
  test_ss_ui 146, test_winmenu 100, test_pad_hotkeys 313, test_input_ini 21 ok; manifest gate 3844 statics, 0
  unclassified (ss_section_reg_kPcsOps BOOT); alltests.sh rc 0 everywhere (test_ss_core phase 9's give-up window,
  waited < 2000 ms, read 2054 ms once right after the game runs -- a timing tolerance under load; two re-runs pass).
  In game (m7b/runs/fx1, fx2): an older build's launch state (no KNDS / PCS2) loads with the warning; F5 writes both
  sections; F9 loads it (same build, no warning) and Undo returns; the same file with PCS2's or KNDS's layout hash
  changed (craft_layout.py) is refused before anything stops ("section KNDS (value numbering) is laid out
  differently"); with backup_before_load = 0 Undo is refused, not done one way.

## 2026-10-27 (early morning) — the "second view every 2.52 s" was our present gate: stream-ordered flips present every frame
EBOOT: BCUS98125 update 1.04 | ps3recomp libs/video: rsx_present_gate.h, rsx_d3d12_backend.c, cellGcmSys.c (PRESENT_ORDERED=0 off, PASSLOG=<n>[:<every>])
- **There is no second view.** The 2026-10-26 trace (one present every 2.52 s with 4 readbacks, ~3900 draws, 8 uploads /
  15.9 MB) is two halves of two ordinary guest frames in one present. PASSLOG (new diagnostic, below) on the FAST BOOT
  pier with the same steady pan (right stick rx=200): that present holds frame N's tail (post chain + HUD into display
  0x734000), the walker's FLIP marker for N, then frame N+1's display clear, G-buffer (MRT 0xAB8000/0xE3C000/0x3B0000),
  its colour + Z readbacks (0xE3C000, 0x10000), shadow maps (0xF2F0000) and lighting, and is presented at a display clear in
  the middle of N+1. The display clear right after N's flip was HELD by the present gate: CB-small, 2152 records since the
  last present < a quarter of the reference (8600+). The reference is the largest frame recently seen: the pan turns the
  camera from the city (8600 records a frame) to the water and sunset (2000-2200) once per 2.52 s (FRAME_TRACE draws
  oscillate with exactly that period, the merged present sits at each trough), so each heavy-to-light turn held one whole
  frame (PRESENT_ADAPT's "one merged pair, then single frames"; [present-gate] "frames got small, re-based ... 8763 ->
  1027"). With the camera still on the city view: no 4-readback present at all. In real play the same thing happens at
  irregular times (the issues log's "every ~2.5-5 s").
- **Who asks for it / the period / what it renders into:** nobody in the title -- the period is the scripted pan's turn;
  the "extra" G-buffer, readbacks and SPU-light uploads (4x 1280x720 0x47400B80 / 0x47784B80 fA5 3600 KB, 0xCF800000 fA1
  900 KB, a 64x64 one) are frame N+1's own. The guest's flip interval at that present is normal (13.3 ms, flip_dt), so
  the game did no extra work.
- **Real PS3:** the RSX flips at the flip command's place in the stream, so each frame is scanned out once at its own
  vblank and nothing is doubled; at the game's ~30 fps budget there was no extra frame to fit. The cost was ours.
- **What a viewer got (old rule):** that present ~11 ms late (interval 24-28 ms), then one guest frame never presented
  (flips=2 two presents later): the dumped frames of a turn move 14 px a present and 28 px at that one (a skipped
  frame of motion); the DXGI queue drained 2 -> 1 (it absorbed the late present here; with less queued it is a refresh
  shown twice). And a
  hold can lock for good: from the save-state load on, the still city view presented every frame at a mid-frame display
  clear of the next one (PASSLOG: FLIP @654 -> CB-small 653 records -> CB-present @1053 of the next frame, every frame):
  each frame shown a frame late, flip to present 37 ms instead of 20.
- **Change (toolkit, default on):** PRESENT_ORDERED. cellGcmSys.c counts flips counted at call time (cellGcmSetFlip /
  cellGcmSetFlipCommand called directly, a _cellGcmSetFlipCommand with no room for its marker, cellGcmSetPrepareFlip;
  not the walker's own 0xFEAD words): cellGcm_flip_unordered_count(). When none happened since the gate's last present,
  every flip since was counted by the walker at its place in the command stream, so the display clear after it IS the
  frame boundary: rsx_pgate_decide_ordered presents the batch whatever its size and takes that frame's per-flip size as
  the reference. The size rule (HOLD_SMALL, the adaptive steps) stays for flips that may run ahead of the drain, the
  case it was written for; PRESENT_ORDERED=0 restores it for every flip. Rendering is untouched: only which display clear
  presents. inFamous 2's flips are all ordered (the GCM_FLIP_SMALLCTX marker).
- **PASSLOG=<min readbacks>[:<every>]** (diagnostic, off = nothing): per presented frame, its passes in order (runs of
  ops with one target / MRT / zeta / size: draws, clears, viewport, first FP; op index), texture uploads (offset, size,
  format, KB), readbacks, batch ends with their reason, the walker's FLIP markers and every display-clear present decision
  (CB-present / CB-noflip / CB-small / CB-nodisp, flip count, records) at the op they came before; printed for frames
  with >= min readbacks and every <every>-th frame.
- **Measured** (FAST BOOT pier, 2560x1080 75 Hz, vsync, fps_cap 0; same binary, PRESENT_ORDERED=0 vs default, 3
  interleaved rounds each; local campaign.sh + p2/ab.py; traces p2/ft/c1_*.csv, runs n1/runs/c1_*):
  steady pan: presents 66.60 -> 66.65 fps (66.64/66.46/66.71 vs 66.57/66.60/66.78), 4-readback presents 23.3 -> 0 /min,
  guest frames never shown 23.3 -> 0 /min, light-part present intervals > 20 ms 23.3 -> 0 /min, longest 27.6 -> 15.7 ms;
  still city view: 51.89 -> 52.69 fps (51.87/51.89/51.91 vs 52.57/52.61/52.89), flip to present 37.3 -> 20.5 ms.
  Checkpoint-03 boot (frontend, loading, boat polycut): same present pattern in both arms (2-record loading-screen
  presents 75 vs 76), 1 -> 0 unshown, no present-gate re-base.
- **Picture:** CELLMARK_DUMP of 24 presents across one turn in each arm (p2/dump_old, p2/dump_new; sheets
  p2/sheet_turn.png, p2/pair_full.png, p2/pair_crop.png): every present a complete frame (HUD, lit scene); the old merged
  present and the new run's frame at the same view (best match within 8 px, residual 7.2 vs ~12 between neighbours) look
  the same. Frame-to-frame pan: old 14,12,14,**28**,13 px around the merged present; new 14-15 px throughout.
- Tests: test_present_gate 2450 checks ok (new: the city turn -- 945 presents over 945 frames, 0 merged, vs 940 / 5 with
  the size rule --, a lighter view, the movie after the menu, DeferredShading's two clears per flip -- one whole frame per
  present --, a call-time flip falling back to the size rule); test_rsx_report 88/88, test_rsx_report_fifo 10/10.
  test_rsx_fifo does not link on Windows (runtime symbols) and test_gcm_user_command needs pthread: not run.
  ss_manifest_check --strict: 3843 statics, 0 unclassified (s_flip_unordered / s_flip_in_walker / s_pgate_unord KEEP,
  PRESENT_ORDERED / PASSLOG switches BOOT, PASSLOG buffers DIAG). The first run after a rebuild ran at 37 fps (walker
  idle 14 ms a frame, i.e. a slow guest; later runs of both arms normal): discard that run in A/Bs.
- Review (one read-only reviewer; every flip-counting path, the gate logic, partial-frame cases, PASSLOG, the manifest
  and the tests; for inFamous 2 the ordered path holds: its flips are walker markers and the walker and the display
  clear run on one thread under the FIFO lock), one medium-low and six low findings, fixed:
  - s_flip_in_walker was a process-wide flag held over the whole flip request (pacing sleeps, the guest flip handler):
    a call-time flip from another thread in that window would count as ordered. Whether a flip is ordered is now
    gcm_flip_request's argument (gcm_flip_walker for the walker's flip words); the flag is gone.
  - Counter order: s_flip_unordered now moves before s_flip_request_count, and the backend reads the request count
    first, so a call-time flip in fc is always seen as one.
  - The walker's FLIP_HEAD method (0xE920 / 0xE924) is in-stream too: ordered, and noted to PASSLOG.
  - A save-state load: s_pgate is restored or dropped while the call-time count is the process's, so the first
    decision after a load now uses the size rule (ss_reset sets the snapshot one behind).
  - PASSLOG=0 switched PASSLOG on; now off, like its neighbours.
  - PRESENT_ADAPT=0 is the original gate only with PRESENT_ORDERED=0 as well (ordered presents set the reference):
    said so in rsx_present_gate.h.
  - Tests: the fallback case passed ordered = 1 at a clear the backend cannot reach. The snapshot check moved into the
    header (rsx_pgate_decide_counted) and the simulator counts call-time flips: a call-time flip keeps the size rule
    until a present, a load for one decision, PRESENT_ORDERED=0 throughout. DeferredShading's claim corrected: the
    same sizes, presented one clear earlier than the size rule (the whole frame, HUD included, the frame the RSX scans
    out at the flip). test_present_gate 2460 checks ok, test_rsx_report 88/88, test_rsx_report_fifo 10/10; manifest
    gate 3842 statics, 0 unclassified.
  Re-run on the fixed build (p2/campaign.sh rv, 2 interleaved rounds; traces p2/ft/rv_*): steady pan merged presents
  and unshown frames 9 -> 0 per 23 s run, longest light-part interval 25.4-27.8 -> 14.8-17.9 ms, 65.62 / 66.93 ->
  66.69 / 67.17 fps; still city view 52.53 / 52.60 -> 53.48 / 53.21 fps. The one 3-vblank present in a new-arm run is a
  heavy city frame (8756 records, 28.6 ms), as in both arms: throughput, not a merge.

## 2026-10-27 (night) — N2: "PC Settings" in the game's own pause menu (the game's Options screen, our rows)
EBOOT: BCUS98125 update 1.04 | src/if2_menu.cpp + ps3recomp ovl_menu.{c,h} / ovl.c (NATIVE_MENU=0 off, MENU_DIAG=1, MENU_TRACE=1|2)
- The Options screen (menu symbol 0x9D86, the same class as the pause menu) under the microscope (MENU_TRACE logs every
  call of 33 menu natives the scripts make, =2 also the property accessors called through their OPDs; a raw SS_DUMP read
  with gmem.py). A native is (this r3, its argument words r4, a mask of the given ones r5; a result goes to args[0]); the
  VM skips a native on a null object (0x101564, an error flag on the script thread). The pause menu's script answers
  u_menu_options with PUSH_MENU only; the push itself makes the game's script fill the Options menu (later in the same
  frame): per row ADD_WIDGET_BY_PROXY on the widget list (child 0xF9C of the menu; lays rows out 40 px apart, no clipping,
  32 rows at most, a pool of 32 at +0x400) from the row proxy 0xA0B2 (a "list" widget, vtable 0x954890; the gamma row is a
  slider from proxy 0xA0B4), text= on its label text boxes 0xA0E1 / 0xA0E2, ADD_ITEM(key, label id) per value,
  CURRENT_ITEM=, ADD_WIDGET_ITEM(menu, row, label id), SET_HANDLER(row, menu), ADD_NAV_ACTION Up / Down and a ring. A Left /
  Right on a row is the row's own (the handler passes unclaimed input to the focused item, 0x48F178): it slides to the
  next value with the game's arrows and sound and tells its handler, whose script reads CURRENT_ITEM and applies the game
  option at once. Back (any-item action -> 0xDEB) clears the menu's items (CLEAR_ITEMS: items and the actions bound to
  them) and pops it; the pause menu's script, on its re-activation, clears every row (SET_HANDLER 0, CLEAR_ITEMS) and the
  widget list (CLEAR -> pool) and rebuilds the pause menu (focus back on Powers). Start on the Options screen -> 0x3DBD.
- C++ under the natives: PUSH 0x1CE3E0(menu, id) -> menu, POP 0x1CE83C(menu), the menu of a symbol 0x1CB868, CLEAR_ITEMS
  0x1CDCF0, ADD_WIDGET_ITEM 0x1CDBC8(menu, widget, symbol), focus 0x1CCF68(menu, symbol), ADD_WIDGET_BY_PROXY
  0x495658(list, proxy) (reuses a pooled row of that proxy), REMOVE_WIDGET 0x49374C, CLEAR 0x49384C, ADD_ITEM 0x491D7C(row,
  key, label, 0, colour = row+0x998; keys unique, 64 at most), CURRENT_INDEX= 0x491A34 (index at row+0x580), CLEAR_ITEMS(list)
  = row vtable +0x160(-1), SET_HANDLER 0x5302F8 (row+0x358). Scene tree: +0x14 -> instance (+0x10 symbol), +0x1C parent,
  +0x20 next sibling, +0x110 first child; ADD 0x1BE00C / REMOVE 0x1BDF74 (+0x58). Joystick (menu 0x1CC78C): vtable +0 =
  pressed within t (mask, 0.15 s: press times per bit at +0x24), +4 = use the press up, +0x24 = active; bits Up 1, Down 2,
  Left 4, Right 8, Triangle 0x10, Square 0x40, Circle 0x80 + 0x10000000 (Back), L1 0x100, L2 0x200, R1 0x400, R2 0x800,
  Start 0x1000, Cross 0x08000000. Glyphs (font private use): U+F090 D-pad, F091 / F093 right / left, F084 L1, F086 R1,
  F08E Cross, F08F Circle. Language dictionary: dict+0x30 = {buckets, table} of nodes {id, id, record, next}, insert
  0x6B6F5C(table, &id, &record) under the dictionary's lwmutex (0x7428F8 / 0x742918); a record {id, -> "name\0text", 0,
  flags} (+0xE: its dictionary). Rows made after the menu's activation (frames after the push) stay invisible (laid out,
  in the world, state 2) until the next activation -- also rows a CLEAR put into the pool and the list re-used.
- Built (one model, two front ends): the overlay's menu model gets an API for another front end (ovl_menu.h:
  ovl_menu_select_tab, ovl_menu_row_positions -- a row's choices / slider stops (word stop, lo..hi by the step, thinned to
  64, the current value always one of them) / presets with the overlay's own labels --, ovl_menu_row_choose -- set through
  the overlay's setter: locked rows refused, RESTART noted --, ovl_menu_env_now from ovl.c for the refresh-rate / window /
  monitor choices). if2_menu.cpp: strings are NEW ids (0xF2000 up, "u_pc_<id>") put into the dictionary with the game's
  insert (N1's two re-pointed demo strings are gone: Quit Game and its box text are new strings too); found again after a
  load into a fresh process (one scan). The pause menu gets PC SETTINGS + QUIT GAME after its entries (QUIT GAME alone when
  one slot is free; it stays the last row). Select on PC SETTINGS: the Options menu pushed with 0x1CE3E0, 11 rows made at
  once (CATEGORY + 10, symbols "#pc-settings-row-<k>"), the category's settings in them (labels in the rows' text boxes,
  values = ADD_ITEMs of our strings, index = the store's), spare rows REMOVEd and out of the Up / Down ring (our actions
  re-aimed in place). The game's own fill is let run but kept off: ADD_WIDGET_ITEM and ADD_NAV_ACTION are wrapped (not given
  to our menu), its rows leave the widget list at its last link (the Down link to its first row) -- before the frame is
  drawn -- and the FOCUS setter keeps the focus on our rows. Categories: the overlay's tabs, then Advanced (every
  OPT_F_ADVANCED row), a tab over 10 settings split ("Tab 1/2"); CATEGORY's Left / Right and L1 / R1 from any row re-fill
  the rows in place. Left / Right on a row is the game's list widget; each frame a moved index is set through
  ovl_menu_row_choose, and a store change (F1 overlay, a preset, a load) re-fills / re-aims the rows. Env-locked rows show
  their value alone; RESTART rows are marked *; the footer (the screen's Navigate / Select / Back text box) is a help line
  for the focused row -- the tip's first sentence, "Locked: set by the environment variable X.", "(Changed:) takes effect at
  the next launch." -- above the hints (D-pad Navigate, Left/Right Change, L1/R1 Category, Circle Back). Back or Start: the
  game's Back order (CLEAR_ITEMS, POP, rows cleared, CLEAR). State: a guest block after the record of string id 0xF1FFF
  ("u_pc_state": open, menu, widget list, category, rows {widget, symbol, last index, values hash, key}, a serial); every
  frame the block is checked against the menu's items (else the screen is left to the game); a serial the host did not
  write (a load) re-reads every row from the store.
- In game (fast state, New Marais pier; OPTIONS_CONFIG = a scratch file): the pause menu shows PC SETTINGS and QUIT GAME as
  rows 6-7 in the game's font; PC SETTINGS opens the Options screen with our rows ("the game's own fill of the Options
  menu (11 row(s)) taken off the screen"), CATEGORY + R1 through Display, Graphics (preset row), Frame, Controls,
  Audio, System, Advanced (Picture scaling *, DPI awareness *, Dump textures, Allow tearing *) and back, 0.3-1.8 ms a
  re-fill; Keep 16:9 LOD On -> Off (bool) and Full-screen movies Fit -> Fill (enum) with Left / Right: "[options] lod =
  0 (was 1)", "[options] fmv = fill (was fit)", options.ini [display] lod = 0 / fmv = fill, the F1 overlay showing Off
  and Fill; the overlay's preset Low shows as PRESET LOW / DETAIL DISTANCE 0.75X in the native screen. Footer help:
  "Locked: set by the environment variable IF2_ASPECT." on the env-locked aspect row (one value), "Changed: takes effect
  at the next launch. File / Save / Load / View above..." on Menu bar *. Back returns to the pause menu (focus on
  Powers, as after the game's Options); the game's own Options afterwards shows and works as before; QUIT GAME -> Yes
  exits (exit 0). Save states: F5 with Graphics open, Frame + fps_cap 30, F9 -> Graphics again ("a load -- its rows
  re-read"), Left / Right / Back still work; a fresh process loading that slot comes up in the screen (123 of our
  strings found again), changes a value, switches category, Back, Quit. NATIVE_MENU=0: the stock five entries; with
  that slot loaded, the screen is closed back to the stock pause menu at the first frame.
- Tests: test_ovl_menu 1221 checks (68 new: toggle / selector / slider / word stop / thinning / off-stop value /
  locked / RESTART / presets / no such row), test_ovl_host 16; every save-state suite rc 0; manifest gate 3830 statics,
  0 unclassified (18 new lines for if2_menu.cpp; ovl.c's fill_env statics renamed).
- Not done / limits: TEXT rows with the overlay's own choice lists (window size, monitor, aspect) are changeable like in
  F1; the System tab's action / info rows (reset all, open folder, quit, About) are not shown (the pause menu has Quit
  Game); a slider over 64 stops is thinned (mouse sensitivity steps 0.1 here, 0.05 in F1); a locked row still shows the
  list's arrows (Left / Right do nothing; the help line says why); text is English in every language; leaving PC Settings
  puts the focus back on Powers (the pause script's rebuild, as after its own Options); the title has no settings entry (no
  multi-entry main menu there, as for N1).
- Review (one read-only reviewer; the three wrapped natives, the fill purge, the dictionary insert and its lock, threading,
  the store mapping and the manifest checked against the game's own call sites; nothing found there), one medium and
  three low findings, fixed:
  - Medium: a load into the open screen was told by the block's serial alone, and serials repeated (every session
    counted 1, 2, ... the same way for the same steps): a state saved on Graphics, loaded at the same count in another
    session after the store changed (options.ini, F1), kept the state's rows, and a Left / Right then set a value from
    the wrong list (positions recomputed from the store). Serials now start at a per-process random value and are never
    reused (g_serial_next). Run fx_x2 (AF 16x before the screen opened, F9 into a state with AF at Game): "another one
    (a load) -- its rows re-read", 16X shown, Left 16x -> 8x.
  - Low: a fallback that leaves the screen to the game (its rows gone from the menu) now gives the footer its button
    hints back; the interceptors also want the menu to hold exactly our rows once made, so a menu left without a close
    is the game's again and its own fill goes through.
  - Low: a value that got no string id (no guest memory or ids left) was skipped, so the list's indices no longer
    matched the values; such a row now takes no change (its index is put back) until the next re-sync.
  - Low: the manifest's reason for g_fill_first was wrong (a lenient load can land mid-fill); the stale value is
    harmless (the same menu's first row, reset on open).
  - Not changed: str_scan stops after 64 free ids in a row, so a state with shifted ids makes a few strings twice (a
    small guest-heap leak only; ids are never duplicated).
  Re-run on the fixed build: x1 / x2 (above), a fresh process loading the open screen (123 strings found, AF Left,
  Frame, Back, Quit), the same with NATIVE_MENU=0 (closed to the stock pause menu at the first frame), the full
  scenario, F5 / F9 inside the screen, NATIVE_MENU=0.

## 2026-10-26 (late night) — N1: "Quit Game" in the game's own pause menu (native entry, the game's own Yes/No box)
EBOOT: BCUS98125 update 1.04 | src/if2_menu.cpp (NATIVE_MENU=0 off, MENU_DIAG=1 logs every menu, [menu] lines)
- How the game builds menus (1.04 addresses): menus are C++ objects driven by data scripts (a bytecode VM calling reflected
  natives through OPDs; the method table entry is {name, sig, symbol id, ..., OPD}, the OPD 0x20 bytes BEFORE its name:
  ADD_TRIGGER_ACTION 0x527340 -> 0x1CD6A4, ADD_NAV_ACTION 0x5273CC -> 0x1CC36C, ADD_WIDGET_ITEM 0x520B94, CLEAR_ITEMS
  0x527408, PUSH_MENU 0x531520, POP_MENU 0x531FB4). A menu: items at +0x324 {symbol, widget, item object}, actions at
  +0x334 {kind, mask, item, target} (0 Up/Down link, 2 trigger, 3 trigger on any item), focus +0x344, first text box +0x110.
  Its per-frame input handler 0x1CD0B8 is reached only through a vtable (the adjustor thunk 0x67AE18 of the listener
  interface at +0x31C); a trigger sends event 0x7D9 (action, item) to the menu's script. Labels are string symbol ids
  (text box +0x184, set by 0x39A788) looked up in the language dictionary (0x39A430; "name\0text" records from
  lang_<language>.xpps); a text box is drawn while it is in the world (+0x58 = 0: the script's ADD 0x1BE00C / REMOVE).
  The pause menu's script rebuilds it on every open (REMOVE_ACTIONS, one ADD_TRIGGER_ACTION with the entry's label id as
  the action id, two ADD_NAV_ACTION per entry, ADD/REMOVE of the slots' text boxes). Layout: seven slots (items
  0xA037..0xA03C, 0xA100, two text boxes each); the game shows five in every scene seen. The retail data keeps the demo
  build's "u_menu_quit_demo" (Quit Demo) among the script's candidate entries. The game's confirm dialog is a message box
  API, 0xC7908(title, text, buttons, 0, 0, 0, callback OPD, 0) (Abandon Mission: 0x1C6B10, u_msgbox_yesno; the callback
  gets 7 = Yes, 8 = No).
- Built: ppu_register_function on the thunk 0x67AE18 (lifted thunk + handler run inside). On the top-level pause menu
  (the one with "u_menu_saved_data"), after the script's rebuild: the first free slot gets the label (the dormant
  u_menu_quit_demo record re-pointed at "Quit Game" in a buffer from the game's allocator 0x4CF4A0; the text boxes set and
  ADDed), the wrap-around links re-targeted and two links added with the game's ADD_NAV_ACTION; no trigger, so the script
  never sees it. Select on it (the menu's own joystick query, with the handler's gating: active, not busy, no other UI
  holding the input) opens the game's message box, title "Quit Game", text "Are you sure you want to quit? Any unsaved
  progress will be lost." (u_focus_test_complete re-pointed); its callback is a host function at the fixed key 0x400 in
  the indirect-call table. Yes posts WM_CLOSE (main.cpp's close path). Everything is guest memory, re-checked every frame.
- In game (fast state, New Marais pier): the entry in the game's font and highlight bar as row 6; Up from Powers / Down
  from Watch Movies reach it, Down from it wraps to Powers; No closes the box and leaves the menu as it was; Options opens
  and returns, Back resumes, the menu is rebuilt with the entry on every open; Yes -> "[rsx] window closed -- exiting",
  exit code 0. Save states: a save in the pause menu (and with the box up) loads back to the same screen and still works; a
  pause-menu state saved with NATIVE_MENU=0 gets the entry after the load. MENU_TEST_SKIP=1 put it in row 7 (item 0xA100),
  drawn correctly, so a scene where the game shows six entries still has room. NATIVE_MENU=0: the stock five entries.
- Not done: the title has no multi-entry main menu (a save continues by itself; without one it is a single "Start a new
  game" screen), so no entry there. Text is English in every language (the game has no "Quit Game" string). The 1.00 disc
  EBOOT is not mapped (the feature says so and stays off). Seven entries in use would leave no slot (logged, no entry).
- Review (one read-only reviewer; message box, ADD_NAV_ACTION, allocator and text-box calls checked against the game's own
  call sites; no high or medium findings), three low ones fixed: a pause menu with only two entries could not tell the
  wrap-around links from the plain Up / Down pair (now 3 entries at least); an add that failed once was never retried
  while the script rebuilt the same list (now retried every 30 frames); the box's callback key 0x400 is registered also
  with NATIVE_MENU=0 (a save state made with the box open still answers). The reviewer's two final runs (full1, ss1)
  repeated on the fixed build.

## 2026-10-26 (late night) — save states M5: slot files, F5 / F9 and the menu work; -LoadState is in gameplay 0.7 s after the start
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
- Slot files (runtime/savestate/ss_file.c, P3SS v1): a 4 KB header the menu reads (game, EBOOT SHA-1, build id, per-section
  layout hashes, SPU table hash, continuation hash, display config, created / play time / guest time / flips / game mode /
  mission, a section table), the image's own tables, then every section as 1 MB chunks (XPRESS_HUFF via cabinet.dll's raw
  Compression API, stored as is when it would not save 2%) each with the CRC32C of its raw bytes; THMB (a 320-wide PNG) as a
  file-only section. Written after the release on worker threads (below normal): .tmp, flush, old -> .bak, MoveFileEx;
  read by mapping the file and decompressing on 8 threads straight into the raw image the M4c LOAD takes (its CRC pass is
  not repeated: SS_RQ_CRC_CHECKED). Refused before anything stops, each with its reason: not a save state / damaged (magic,
  any CRC, bounds, cut short), another game / EBOOT / build id, an unknown mandatory section, a section laid out
  differently, another SPU table, and (sys_ppu_thread.c's image check, new) a return address with no continuation in this
  build, named ("tid 1 'main': return address 0x001EFC24 (frame 1) has no continuation in this build").
- Slots (runtime/savestate/ss_slots.c): savestates\BCUS98125\slotNN.p3ss / quick.p3ss (+ .png, .bak); the window menu's
  engine, slot reader (labels "Slot 6 - today 20:09 - 3. Forward Momentum - 1:11", cached by size and time) and
  availability (greyed before the first frame, during a save or load, with SAVESTATES=0 -- with the reason on top); while a
  file is written every other stop waits (ss_add_check "writing"). config\savestates.ini (enabled, dir, slots, compression,
  thumbnail, give_up_s, launch_give_up_s, build_check; later keys documented as not yet). Mission text:
  src/if2_ss_title.cpp (the game's save subtitle, "Prologue - <step>" from the task graph, else the game mode from the
  signals). Thumbnail: the backend grabs the next presented frame (rsx_d3d12_backend_grab_frame); PNG via src/if2_png.cpp
  (the WIC writer moved out of if2_checkpoint.cpp).
- Across a restart: TIME's restore now puts the snapshot's guest-QPC epoch back (ss_guest_clock_ops.set_qpc, sys_timer.c)
  and re-anchors the time base to it, so absolute guest-clock values in the image (report / flip times, s_audio_start_us,
  deadlines in guest memory) agree in the new process ("[ss] guest clock: the snapshot's epoch is another process's").
- SS_LOAD=<slot|file> (tools\run.ps1 -LoadState): read while the game boots, loaded at its first frame; READY = the load
  (if2_checkpoint: no title flow driven). SS_SAVE_AT + SS_SAVE_SLOT=<n>[,<n>..] (run.ps1 -SaveAt / -SaveSlot).
  S\perf2\runarm.ps1 -View marais2s: marais2 booted from savestates\perf\marais2s.p3ss (the boat polycut's start).
- Measured (New Marais, RES_SCALE 1): save pause p50 65 / p95 72 / max 117 ms, request -> "Saved" 0.43 s, file 205 MB
  (image 397 MB, 51.5%; uncompressed 397 MB in 0.16 s); load: read + decompress 155 ms while the game runs, pause p50 130 /
  p95 141 ms, request -> "Loaded" 0.30 s (20 + 20 in one run, alternating slots: back at the saved spot 1 s after each
  load, screenshots 0.4-1.5 apart against 14-16 for the walked-to spot; again with a running and jumping route that
  takes Cole off the pier onto the buildings across the street: 20 + 20, 0.5-2.0 against 18-33; GPUP_PROBE: no load
  darker than its save; audio 183-188 blocks/s, no silent second). RES_SCALE 2: image 529 MB (GPUP 195 MB), file 232 MB, save pause 135-194 ms,
  load pause 204-215 ms, "Loaded" 0.46 s. -LoadState: gameplay 0.7 s after the start (first frame 0.5 s, load pause
  ~190 ms) against READY at 26.9 s for the checkpoint boot.
- Matrix (save in gameplay / pause menu / boat polycut / logo Bink x load from gameplay / title menu / loading screen):
  12 of 12 loaded at the right place; also loads out of a polycut, the pause menu and a movie. Refusals seen in game:
  another build (menu "(made by another build)", -LoadState fails with both build ids), empty, damaged (cut short),
  missing continuation (a crafted file), SAVESTATES=0 (greyed, "save states are turned off").
- Known: every rebuild makes the old slots "made by another build" (the first slice's rule; build_check = 0 is the
  experiment for perf arms). Loading a Bink state into a run that MOVIE_SKIPs that movie shows broken movie frames.
- Review (one read-only reviewer over the M5 change set; all five fixed, each checked against the code first):
  (1) quitting did not wait for a slot file being written -- ExitProcess killed the writer, leaving a ~200 MB .tmp and,
  between the two renames, no slot file at all: the exit path now waits for it (ss_slots_wait_written, up to 30 s, the
  thumbnail wait cut short); in game: F5, WM_CLOSE 7 ms after the capture with 1 codec thread -> "waiting for it", the
  file whole after 1.43 s, then the exit. (2) a failed final rename deleted the new file after the old one had moved to
  .bak (the slot showed empty): both renames are retried for ~2 s (a scanner or sync client holding the file), a failed
  .tmp -> slot puts the old file back, and only if that fails too is the complete .tmp kept, never deleted; ss_slots_sweep
  (at init) makes a complete .tmp whose slot file is missing the slot again and removes other .tmp files older than
  2 min (fault-injected MoveFileEx: both outcomes as designed; in game: a planted complete .tmp promoted, an old
  half one removed). (3) a file missing a mandatory section this build registers was accepted with build_check = 0
  (restore skipped it silently): ss_file_check_compat and ss_image_validate_ex now refuse it. (4) the continuation
  pre-check passed a chain the capture's walk had cut at 48 frames while a resume follows 64: PPUT_CHAIN_MAX is now 64
  and a chain still unchecked at that depth is refused. (5) a slot file replaced between the header read and the mapping
  was read past its end: the mapped size must equal the header's. Also load_reader no longer reads s_job after job_end.
  Rename errors name the file, not the path (the notice was cut off before its reason). test_ss_file 138/138 (sweep,
  missing section, quit with nothing written); every suite rc 0; manifest 3802 statics, 0 unclassified.

## 2026-10-26 (night) — play-session issues log, Quit game, F8 mark
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
- Every play session (the exe started with no arguments) writes logs\play-<stamp>-issues.txt next to its log:
  a header (CPU, GPU through DXGI, memory, OS, display, power, the settings that are not default -- no user or
  PC names; paths under the profile and the game folder are written relative), then STUTTER / HITCH / FREEZE
  (bursts of slow frames, each with what the frames spent their time on: shader compiles, pipeline creation,
  GPU readbacks, GPU wait, present, the game thread, texture streaming, new render targets), SLOW (10 s stretches
  under the frame-rate target), STALL (no frame for 3 s / 15 s), MINUTE (fps, 1% low, cpu, ram, vram), LOG
  (problem lines of the game's log, deduplicated), MARK (F8), CRASH (with a .dmp), END (the summary).
  runtime/diag/play_issues.c; tools/play_triage.py ranks one or many files (other players' too).
- Cost: the render thread hands one record per presented frame to a ring (126 ns); FRAME_TRACE's counters run in
  a light mode (s_ftr_on 2: no per-draw timing) to fill it. One writer thread at background priority with EcoQoS
  wakes every 2 s. PLAY_ISSUES=0 turns it off; PLAY_ISSUES=<file> turns it on outside play mode.
- Quit: the options overlay has a "Quit game" button (System tab) and a footer shortcut on every tab -- Triangle /
  Y, or Ctrl+Q -- pressed twice within 4 s; it closes the window the way Alt+F4 does. A save the game is writing
  finishes first (up to 5 s), then the issues log ends with END.
- F8 (HK_MARK; also View > Mark a Problem Here) marks the moment in the issues file and saves a screenshot beside it.
- In game (New Marais, harness): the file had the header, MINUTE lines, MARK #1 with play-test-mark1.png, and END
  after a WM_CLOSE (exit 0); no names in it. Unit tests: play_issues, ovl_menu 1153, pad_hotkeys 303, winmenu 92.
  Cost, issues log on vs off (New Marais, mean fps after READY): 60.24 vs 60.30 over 3 rounds on all 16 threads,
  29.56 vs 29.43 over 2 rounds pinned to 4 threads (a weaker CPU) -- identical medians, within noise.
- Found by it on the first run: a 25 ms frame (normal 13 ms, uncapped) every ~2.5-5 s with 4 GPU readbacks and
  8 texture uploads (15.9 MB) -- to triage.

## 2026-10-26 (night) — save states: review of M4c + GPUP, three fixes
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
One read-only review worker over the whole M4c + GPUP change (60 toolkit files); its findings checked by hand.
- Fixed: a LOAD's GPU flush (ss_core.c, stop_converged) ran without the watchdog's capture grace and without a
  stop_intact check, and s_loading was set only inside load_run -- a flush over ~3.3 s could let the watchdog
  release every thread and the load then restore over a running game. Now: grace during the flush, stop_intact
  after it, and s_loading set together with an intact check under s_lk, after the census hook (s_loading makes
  ppu_park_of read the load's park records; setting it before the hook failed test_ss_resumers).
- Fixed: SPUC / SPUH check_image test the running session (async runner, raw SPU, debug switches) but ran once
  per request; ss_set_load_session_check (lv2_register.c) re-reads them in every stop a load runs in.
- Fixed: GPUP bounds -- s_rd_n / s_memw_n from the image out of range fall back to relearning; a target's resource
  state must be read states or one write state.
- For M5: GPUP's save cost grows with RES_SCALE squared (pool x4 at 2, x16 at 4; the readback buffer stays
  allocated at pool x1.25) -- measure before F5/F9 ship.
- Checked clean by the review: normal play with saves armed (one volatile load per guest-clock read, one TLS store
  per HLE call, no new wait slicing, two compares per frame for GPUP_PROBE), GPUP barriers/footprints/states,
  discard / gate / recreate ordering, every restorer's image bounds, no leaks across loads.

## 2026-10-26 (evening) — save states: no black frame after a load (GPUP, the renderer's surface pool)
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
- The flash: a load dropped the renderer's offscreen targets and zeta surfaces (rsx_d3d12_backend_ss_reset), which
  hold what only the GPU drew, and reset the present gate. The first presented frame after most loads was fully
  black (8 of 11 loads in the control run, GPUP_RESTORE=0), sometimes the next one half drawn, and exposure
  jumped (98 -> 134 mean) while the targets were rebuilt from stale guest bytes.
- GPUP (engine design M6, rsx_d3d12_backend.c after rsx_d3d12_backend_ss_reset): a capture reads every live target
  and zeta back (depth and stencil planes; tight rows, so a re-capture is byte-identical) with the present gate,
  the display-redirect table and the colour/depth record (memw); a load makes each surface again at its slot and
  uploads its pixels in one list. A surface whose host size differs under this session's RES_SCALE falls back to
  the drop. GPUP_RESTORE=0 restores the old drop for comparison.
- In New Marais: 25 targets + 22 zeta surfaces, 63.6 MB, restored in 51-66 ms; the fidelity check now covers 16
  sections, all equal. Save capture +5.6 ms (p50 26.1 ms); load pause p50 195 ms (was 134).
- GPUP_PROBE=<n>: after each load and after each save, the mean brightness and near-black share of the next n
  presented frames ([gpup-probe] lines; the black-frame check). Round trips every 10 s: 16 loads x 90 frames, 0
  bad. A-to-B loads with Cole moving and dying (SS_SELFTEST_AB=15 twice, =8 once; 69 loads): the frames after
  each load match those after its save (116/1% vs 116/1% ...); in the 55 loads probed both ways, the 6 that came
  back dark were as dark right after the save (Cole's death fade), and the 2 dark loads of the first run (no save
  probe yet) show the same fade. None of the 55 made a frame darker than its save. Control (GPUP_RESTORE=0): 8 of 11
  loads presented a fully black frame.

## 2026-10-26 — save states M4c: an in-process load restores the whole game in ~135 ms
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
Built in parts: a core worker (runtime/savestate/ss_core.c load pipeline: discard -> teardown -> restore -> verify
-> recreate -> release, with check_image refusals before the stop), five section workers (kernel, SPU, files, GCM,
libs: teardown/restore/after_release per section) and an integration worker. Its review is still to come.
- A LOAD stops the game strictly, discards every guest thread (longjmp to its root), tears each section down, writes
  the image back, re-captures and compares it section by section (SS_VERIFY=1), recreates the 25 PPU threads, main and
  7 SPU threads at their parked points and releases. Nothing is relaunched: a failure after the point of no return is
  FATAL (exit 3).
- Selftests: SS_SELFTEST_EVERY=<s> (capture, then load the same state) and SS_SELFTEST_AB=<s> (save A, load it <s>
  later). 263 round trips (New Marais every 10 s and every 2 s, prologue, Bink logo and city-intro FMV, title menu) and
  28 A-to-B loads: every verify equal, no FATAL / watchdog / hang, audio 187.5 blocks/s with no silent second, fps
  after = before, pause p50 134 / p95 144 / max 212 ms (stop 37, teardown 16, restore 23, verify 34 ms). A-to-B puts
  Cole back at A's spot alive (screenshots 0.4-1.6 apart against 22-38 for B) and a polycut caught at A replays with
  the same timing.
- Armed with no load: 60.20 against 60.44 fps with SAVESTATES=0 over 4 alternating rounds (ranges overlap: noise).
- Known: the frames in the first ~second after a load are black or half drawn (the renderer drops its surface pool,
  which only the GPU had) -- fixed next by saving the surface pool with the state (design M6 GPUP). A load can still
  interrupt a game save being written; sys_vm, cellSpurs HLE tables and interrupt bindings are refused, not restored.
- Harness: a seconds-based CHECKPOINT_EXIT_AFTER longer than CHECKPOINT_TIMEOUT (600 s) quits with code 5; pass
  -Timeout for long runs.
## 2026-10-22 (afternoon) — save states M4b: a save captures the whole game into RAM in ~60-110 ms
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
Built in parts: a foundation worker (runtime/savestate/ss_state.{c,h}: section registry, field lists with
per-field name hashes and per-section layout hashes, writer/reader, staging buffer, CRC32C chunks; MAPS, GMEM, TIME,
CHNS; tools/ss_inspect.py, tools/ss_manifest_check.py), five subsystem workers in parallel (kernel: PPUT KOBJ KMEM;
SPU: SPUG SPUC SPUH; files: FSFD over all three fd tables; GCM_ incl. the walker's rsx_state and the report
registers; libs: AUDI PAD_ SUTL MISC TITL), an integration worker, three adversarial reviews (8 confirmed, all fixed).
- A strict SAVE now flushes the renderer and waits for the GPU (rsx_d3d12_backend_ss_quiesce), runs every
  section's check(), then capture() into the preallocated staging buffer (no allocation, no lock, no logging inside
  the pause), releases, and seals the CRCs after the release. SS_DUMP=<s>,.. also writes the raw image
  (.p3ssraw, 256-byte header; the P3SS v1 container is M5) with size / committed / non-zero MB and an XPRESS_HUFF
  estimate. A stop the watchdog released is refused, never captured (SS_CAPTURE_GRACE_MS).
- Guest memory (New Marais): 390-397 MB committed, 331-346 MB non-zero (RSX local 206 MB, sys_memory 113 MB);
  image 335-350 MB; XPRESS_HUFF 54-56% -> ~185-195 MB a slot. Gate: 278 MB non-zero, ~180 MB a slot.
- Pause (stop + GPU + capture): New Marais 57-110 ms (the first save of a run is the slowest), gate 56-109 ms --
  target < 300 ms. Capture itself 21-32 ms (MAPS scan ~14 ms, GMEM copy ~14 ms).
- ss_inspect on every dump: park kinds in PPUT == CHNS == the stop's census line; 26 parked threads (lwcond 10,
  equeue 8, sem 4, usleep 2, spu_join 2), 15 event queues, 11 ports, 2 event flags, 40 semaphores, 132 lwmutexes,
  22 lwconds, 7 SPU contexts, 10 open files (9 psarc + the Bink TV feed), 29-32 allocations; every chunk CRC ok.
- Soak: 105 saves (one every 10 s) captured, no new log errors, no watchdog; ~1-2% fps while saving every 10 s
  (the pauses themselves).
- Manifest gate (tools/ss_manifest_check.py --strict --map build/infamous2.map; config/ss_manifest.txt): 3670
  statics in 157 linked files classified SAVE 233 / RESET 325 / KEEP 689 / BOOT 1139 / DIAG 1284, 0 unclassified.
- Not yet: guest time is not frozen across a stop (each save moves guest time ~60 ms) -- M4c's single guest clock;
  every teardown/restore is NULL (M4c).

## 2026-10-22 — save states M4a: armed by default, stops on request, strict/lenient eligibility
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
Implemented in stages (implementer -> 3 adversarial reviews, each finding verified -> fixes; 13 confirmed, all
addressed), measured here.
- **Always on** (ss_enable from ss_init): armed at startup with no periodic stop; roots record themselves; host waits
  sliced. SAVESTATES=0 (off/no/false) keeps every old path and wins over SS_CENSUS / SS_SAVE_AT / SS_SELFRESUME.
- **Requests** (ss_request_submit, one at a time from any thread; CENSUS / SAVE / LOAD / HOLD / SELFTEST): the
  coordinator stops at the next flip and checks eligibility. Strict (save): every PPU thread parked with no callback,
  bridge or pending trampoline and its strict walk OK; every SPU host thread at a drain hop or an audited host wait
  (the title registers its table from main.cpp: 0x1178, 0xA70, 0xFA08), no foreign SPU context; mixer parked; walker
  idle; no PPU thread or SPU group in a create/exit transition (new counters); the game not inside cellSaveData (busy
  count, ps3_savedata_busy); no guest file open for writing (all three fd tables). Lenient (load): parked anywhere
  discardable. Blockers that need no stop are checked with the game running; refused stops are spaced
  (SS_RETRY_GAP, default 4x the stop) up to give_up_s (3 s); reasons with the design's user texts. SS_CENSUS is a
  periodic request source (same output); SS_SAVE_AT=<s>,.. submits saves (strict check only until M4b).
- **Event-driven waits stay on while armed** (wait_events.inc: a wait that reaches them is at most one slice long and
  parked at its start). Sliced CV waits return a spurious wake instead of sleeping again (a lost wake otherwise),
  timed equeue / event-flag waits keep one deadline across slices.
- Thread-slot generations in sys_ppu_thread.c (an exiting thread touches its slot only while it is still its own).
**Cost** (campaign.sh, armed vs SAVESTATES=0): docks +0.8%, street +0.3% (noise); gate 3 rounds armed -4%, then 5
rounds armed +2.6% (the gate's ±5% noise both ways): no measurable cost.
**Stops** (census through requests): New Marais 30/30 converged, 30/30 strict. SS_SAVE_AT 70/90/110/130: all
eligible at the first stop, 29-42 ms each.
**Per mode** (local modes.sh):
- Boot with the movies kept (checkpoint 03, -KeepMovies; SS_SAVE_AT 1.5..90 s): frontend, logo splash (Bink),
  city flyover, city-intro FMV (Bink), boat polycut, around the game's autosaves: 14/16 eligible at the first stop
  (23-104 ms). Loading screen (t=20, 24 s): refused after 3 s, "bridge" -- the fios media threads park inside a
  libsre call (cellSpursEventFlagWait); bridge resume is M7b in the design; t=27 on the same screen passed after
  0.7 s. Before the movie entries below, every save during a Bink movie was refused: main (0x001E492C) and the SP
  Movie Player (0x001E5718) waited on the movie through chains with no continuation (census during the movies:
  36.6% strict-pass, 131/131 converged); now listed with their parked owners (3725 continuations).
- Prologue gate (SS_SAVE_AT 20..88 s): the pause menu open (START at +20 s, closed at +32 s), gameplay and the gate
  fight: 10/10 eligible at the first stop.
- No fatal / watchdog lines in any run. Stops converge in 20-40 ms in every mode (one 142 ms on the loading screen).

## 2026-10-22 — save states M4a-0 on 1.04: every census stop would pass a strict save
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
The save-state engine (local design.md: in-process load, first working slot = M4a + M4b + M4c
+ M5) starts from M4a-0, done in a sandbox on the 1.00 build on 2026-10-20/21 and ported here:
- **libsre relift** (tools/lift_libsre.sh): the lift takes config/ss_cont_list.txt and the new
  config/ss_cont_list_libsre.txt together, so libsre (the real SPURS library) has the return-address stores (177
  `sc`, 28 indirect calls) and 119 continuations in 11 functions -- the SPURS handler threads park in it for good
  and could never pass a strict save before. Reproduced byte-identical from the firmware PRX (sandbox evidence).
- **The census's strict walk** (ss_core.c / ppu_selfresume.cpp, ss_sr_ops.walk): each parked thread's chain is
  walked the way a save would (every frame has a continuation and agrees with the stack, up to the root) and
  "  strict <verdict>" lines go into SS_CHAINS; roots record themselves whenever a census is armed;
  tools/ss_census.py --strict reports the strict-pass share and the uncovered return addresses (--missing) and
  the owners of parked sites (--seed). SS_SELFRESUME_ATROOT=1 runs threads that park for good through P0.
- tools/ss_leafscan.py (+ tests/test_ss_leafscan.py): callers of frameless syscall leaves, whose return address is
  in LR and on no stack frame, so the census cannot name them. tools/relift.sh --ppu-only: relift the PPU code only.
- **1.04 continuation list** (config/ss_cont_list.104.txt): the 1.00 M4a-0 sections translated by eboot_map.py (68
  mapped, 7 dropped), the 1.04 leaf scan, the 1.04-only threads (Motion Controller Update 0x001C4988/0x001C4CF4,
  Transport Lookup Thread 0x004C5DCC: the only uncovered parks in the first 1.04 census), main's 0x0032E870, and
  the owners of every parked site of the 1.04 censuses listed whole. 3624 continuations in 1389 functions.
**Census on 1.04** (SS_CENSUS, local census.sh): New Marais 30/30 stops converged (PPU p95 10.3 ms, stop
p95 39.7 ms), 30/30 strict-pass; prologue gate 66/66 converged (PPU p95 10.2 ms, stop p95 41.1 ms), 66/66
strict-pass (before the 1.04-only entries: 0%). Tests: test_ss_core, test_ss_selfresume (incl. at-root),
test_ss_census, test_ss_leafscan, test_ppu_cont all pass.
**Next: M4a** -- armed by default at no cost, stops on request (strict / lenient eligibility, reasons), and the
event-driven waits kept while armed.

## 2026-10-22 (late morning) — walker: prepass live units; where the street's walker time really goes
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
**PREPASS_LIVE** (default on, toolkit): the prepass settles the units a record's program does not sample (and
those at or above TEX_UNITS) before its unit loop -- no surface, and the null view written only where the window
slot does not already hold it, exactly what tc_copy_desc skipped anyway -- and the loop visits only live units.
PREPASS_LIVE=0 and the RSX_RES_DRYRUN census keep the old loop. PERF on the busy street: srv 14.6% -> 14.2% of
the walker (~0.1 ms/frame): the dead units were cheap after all.
**Walker profile, busy street** (HOST_SAMPLE on the frame clock, 14 s): vertex conversion ~27% inclusive
(rsx_vemit_run 11.5% self, rsx_ve_convert 6.2%, vc_get/hash), vp_record_cb 8.3% self, render_frame_impl 7.3% self,
**nv3089_blit 20.9% inclusive = the G-buffer readback** (rsx_d3d12_readback_surface 15.6%, of it wait_for_gpu
7.2%). off_rt_find_dims 0.4%, memw_is_zeta not seen: the planned off-RT/memw indexes (walker plan #3 part 2) are not
worth building. FRAME_TRACE at the docks: 2 readbacks a frame (colour + depth) 2.45 ms of a 16.9 ms frame, GPU
waits 1.02 ms.
**The readback is on the frame's critical path by design.** RB_SEQ=<n> (new diagnostic, rsx_report.c): every frame
4 NV3089 copies out of local memory (colour and Z24S8, 1024 + 256 columns each), then a back-end label with 0 draws
and 0 walker stops in between, ~1.1 ms after the first copy: the PPU waits on that label to start the SPU lighting
jobs (the SPU light buffers the GPU samples later in the frame). So a readback completed later than its copy has
nothing to overlap with. RT_READBACK=0 (stale lighting input, wrong picture; sizing only): gate +16.6%, docks +6.4%,
street +7.1% -- part of that is SPU light work that depends on the data (gate SPU 82 -> 71 ms/frame), part the
walker's wait for the GPU. What is left to win there: submitting batches earlier so less GPU work remains at the
copy (walker plan #16, chunked submission), and a cheaper copy. The bigger walker lever is vertex conversion
(GPU vertex pull, plan #14, XL).

## 2026-10-22 (morning) — GPU-side report delivery: measured, not built; the gate is SPU-bound now
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
**Go/no-go for "slice B"** (write the sun glow's record texels on the GPU so the 9 consumer flushes a frame go
too; ~10 days). Probe `REPORT_TEXREAD_DEFER=1` (WRONG values, A/B only): no texture read delivers, so reports
reach memory only at the observers -- at the gate all of them land at the frame's SET_REFERENCE, one delivery a
frame (REPORT_STAT: reference 44.7/s at ~44.5 loops/s), the best timeline slice B could have. 3 interleaved
rounds at the gate: precise rule (slice A, shipped) 42.60 loops/s, probe 43.84 (+2.9%, walker 18.8 -> 17.7 ms),
ceiling REPORT_EXACT=0 44.04 (+3.4%). 0.66 ms per loop, under the 1 ms bar set before building: not built. The
session's baseline is higher than yesterday's 40.2 (drift between sessions), and with the walker faster the
gate's busiest SPU worker is at 90-93%: the gate is SPU-bound now, like the docks.

## 2026-10-22 — sun-glow reports: half the flushes, no harvest for the record texture (gate +4.7%)
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
**What the gate was waiting on.** Every ZPASS-report flush at the prologue gate was a "texture read"
(REPORT_LOG: 19 per frame). REPORT_WHY (new, rsx_report.c) showed why: the sun glow (func_00131A80, two
36-vertex boxes per light, depth-tested into IO 0x14080 and untested into 0x14090) is read back on the GPU as a
4x2 Y16_X16 texture over exactly those two records (format 0x0001F52A, pitch 16), so the reads are real. Per
light the old rule flushed twice: before box 2 (unit 0 still bound to the record texture from the previous
light, but box 2's program samples no unit) and before the consumer. Ceiling A/B (REPORT_EXACT=0: no waits,
WRONG values): gate 39.6 -> 45.6 loops/s (+15.3%); New Marais unchanged.
**Design** (read-only review: 3 designs x 2 adversarial critiques + synthesis, local plan.md): an
exact CPU-only slice first, a GPU-compose slice (write the record texels on the GPU, ~10 days) only if a DEFER
measurement shows it would add >= ~1 ms.
**Slice A (done, default on, toolkit):**
- REPORT_PRECISE (A1): only units the draw's program can fetch trigger a delivery. The backend answers
  (rsx_report_backend.draw_reads, appended member; d3d12_report_draw_reads) from a read-only peek at the
  fragment-program scan memo -- the same entry vp_record_fp will use -- and says "unknown" (old rule) for any draw
  that may not take the VP route, may end with fp_shash == 0, or runs under a unit/source-rewriting diagnostic.
  Reports the old rule would have written and this one did not are owed as a FIFO prefix and written at the
  next walker stop (before GET is published), flip word/method or user command (rsx_report_idle), so no point
  the PPU can observe sees them later than before; later GETs stay pending exactly as before. Each record keeps
  the skipped units (rpt_skip); the prepass checks before every upload that none of them reads guest memory, and
  a hit logs and turns the rule off for the run.
- REPORT_TEX_VOLATILE (A2): a small texture (<= 4 KB bound) over an address a report record was written to is
  re-read at every bind (once per batch) without a write-watch round and stays untracked (slot.vol): every
  delivery rewrites it and starts a new batch, so validating it cost one harvest (~0.12 ms) per flush.
- REPORT_STAT=1: deliveries/s by reason, draws with a skipped unit, owed deliveries, draws the rule could not judge.
**Measured** (gate, 1.04): texture-read deliveries 19 -> 9 per frame, 0 owed, 0 violations; harvests per 20
batches 18-19 -> 5-10. A/B, 3 interleaved rounds: gate 38.40 -> 38.64 (A1 only, +0.6%) -> 40.20 (A1+A2, +4.7%;
walker 20.3 -> 19.6, PPU main 11.9 -> 11.0 ms/frame); docks 74.1 -> 74.5, busy street 54.9 -> 55.5 (noise). The
harvest, not the flush itself, was most of a flush's cost. Picture: the lamp coronas at READY match across arms
(mean RGB of both corona regions within run-to-run variation; frames covered by another window excluded).
**Review** (read-only review, 3 lenses, each finding verified): fixed -- owed reports were delivered with the
whole queue (now the FIFO prefix the old rule wrote); the volatile decision ran before the per-batch dedup (now
after it); stats labels; tests strengthened. Accepted as designed: the prepass check is a tripwire after the fact.
Tests: test_rsx_report 70/70 (new: the sun-glow sequence 19 vs 10 measurements with identical record values, the
unknown/REPORT_PRECISE=0 fallbacks, owed prefix delivery at a walker stop, the violation latch),
test_rsx_report_fifo 10/10.
**Next:** the remaining 9 flushes per frame are the consumers; slice B (GPU compose) is gated on a DEFER A/B.

## 2026-10-21 (late night) — test boots 11.5 s shorter; PGO measured and dropped
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
**Where a driven boot's time goes** (checkpoint 03, per-thread CPU + HOST_SAMPLE_ALL timeline + IF2_SIGLOG,
PS3_POLLTIME): frontend 0-5.3 s (paced by time: FPS_CAP 0 and 150 both reach PlayerActive #1 at 5.2-5.3 s);
the logo splash `splash_america.bik` (352 frames at 29.97 = 11.7 s; the 'SP Movie Player' thread 0.5 core) --
the world load only starts after it; the world load ~8 s (ResetWorldPre -> PackLoaded #8); then the city
flyover and the city-intro FMV slot (MovieStarted 15.0 -> MovieFinished 26.5 s, the FMV itself skipped) and
the boat polycut (PlayPolycut 26.5 -> PolycutFinished 63.2 s), which is the measured "docks" scene.
**The world load** is a hand-off, neither side saturated: the main thread's per-frame, time-sliced pump
(func_000B13B0: a deadline from a frame-budget constant, then func_000B31B0 + usleep(3) at lr 0x000B14BC,
~200k calls/s, until the deadline or until +0x3D0/+0x3D8/+0x3E0 are all zero) is 41% game code, 55% polling;
the busy SPU runs the EBOOT's SPU image 0, the Edge Zlib inflate task (edgezlib_inflate_task.cpp, zlib
1.2.3-PS3-SPU-EDGE): 47% inflating, 43% waiting for the next request; the fios threads ~5% each. A native
inflate would shorten loading screens (~3-4 s of 8 by the main thread's share) but not frames: at the docks
image 0 is not among the SPU workloads (jobmod d0d15ec83992 leads). Not done; queued.
**Changes:**
- Driven runs (CHECKPOINT=...) skip the logo splash by default: `CHECKPOINT_SKIP_MOVIES` is all | fmv | 0
  (all = FMVs + splash, the new default; fmv = the old default). READY 11.5 s sooner (New Marais 46.2 ->
  34.7 s), the world loads and plays the same. Player mode is unchanged (the driver is off there).
- Checkpoint 03 has no WarpedToCheckpoint, so READY fell back to "quiet for 1200 frames", which fires ~8 s
  into the boat polycut and sooner on a faster arm, moving the measured windows within the scene. The perf
  route now uses `CHECKPOINT_READY_ON=PlayPolycut` (scene-locked, 25.6-27.0 s) with everything after READY moved
  by the measured 7.6 s. Gate route unchanged after READY. A run: gate ~67 s (was ~79), New Marais ~82 s (~95).
- A/A (the same exe twice, 2 rounds): docks +0.1%, gate fight -5.0%, busy street -4.8%. Gate and street need
  3+ rounds and deltas above ~5% before they mean anything; the docks window is good to ~1%.
**PGO** (clang IR PGO, profile from two instrumented training runs, 3 interleaved rounds each):
- scope all: gate -4.4%, docks -10.1%, busy street -4.4%. Per frame the walker -1.6 ms and PPU main -0.9 ms,
  but the SPU workers +10..+11 ms.
- scope host (new IF2_PGO_SCOPE: all but the lifted SPU programs and runtime/spu/, which get
  -fno-profile-instr-use): gate -5.1%, docks -12.2%, busy street -7.5%; SPU still +8..+11 ms per frame, so
  it is not the SPU code's own codegen. Likeliest: this 8-core / 16-thread CPU is oversubscribed (4 SPU
  workers, walker, PPU main, the kicker spinning ~96% of a core), and faster PPU spin-polls take more of the
  cores the SPU workers share. PGO stays off. The next lever is fewer spin-waits, not faster code.
**Rebuilt:** build\infamous2.exe from the current source (the CB_COMPACT byte counter, the splash default).
Real save states (an instant boot into gameplay) are still the M4 engine, not built.

## 2026-10-21 (night) — perf: quiet play logs, a 256 MB vertex cache, compact constant slots
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
**Protocol** (the sandbox perf harness, now on the live tree: runarm.ps1 / campaign.sh /
perf_report4.py). Wall-clock routes: gate = prologue step 11 with a scripted walk, exit 42 s after READY;
New Marais = checkpoint 03 (boat polycut + docks walk, then the busy street), exit 45 s after READY.
16:9, PRESENT_SYNC=0, PAD_NO_HOST. Metric: median `[if2-clock]` loops/s over fixed windows (gate R+15..40,
docks "ml", busy street "ms" >= 8500 draws), per arm the median of 3 interleaved rounds, plus per-thread CPU
(QueryThreadCycleTime). A quiet machine matters: identical arms moved +-4% between sessions (36.2 vs 39.3).
**Baseline (1.04):** gate 34.4, docks 72.8, busy street 42.8 loops/s. Walker 23.6 ms/frame at the gate (85%
busy) and ~100% busy on the street; the docks are SPU-bound (4 workers, busiest 91%).
**Results:**
- `PS3_VERBOSE=0`: gate +8.4%. The play log is a redirected stderr, which made the runtime's per-wait lines
  verbose by default (runtime/ps3_log.h). Play mode (no-argument exe) now sets PS3_VERBOSE=0 unless given.
- `SPU_TASKSET_LLE=1` (the 5th SPU as on hardware): gate -9.1% (busiest SPU 99%). Stays off.
- per-thread DMA repeat guard + semaphore getenv hoist (ported from the earlier perf sandbox): gate +1.8%,
  New Marais within noise. On (the guard is also a correctness fix: another SPU's DMAs reset the count).
- `VCACHE_MB` 64 -> 256 (default now 256): busy street 41.6 -> 49.5 (+19%), walker 23.6 -> 19.3 ms; 1024
  within noise of 256; gate and docks unchanged. At 64 MB the street overflowed the converted-vertex cache
  (2.9k hits vs 45k "nofit" per window): those draws converted every frame. `VCACHE_AGE=48`: +0.5% (noise).
- `CB_COMPACT` (new, default on): a record whose VP constants changed stored all 512 registers (8 KB) in the
  write-combined upload heap; now only the registers its VS reads (scanned from the decompiled HLSL; full
  copies for relative addressing, VPC_NORM, and while a CPU-side slot reader is armed). Busy street +5.8%,
  walker -2 ms at the gate and on the street; gate loops/s neutral over 6 rounds (session drift). Checked with
  `CB_COMPACT_POISON=1` (every unread register NaN): gate, docks and busy street render intact.
**Walker study** (read-only design study, a local walker_plan.md): the next exact items are prepass
live units + O(1) off-RT lookups (~0.5 ms), report-round triggers, harvest narrowing; GPU vertex pull is XL.
**Now:** gate ~37-39, docks ~74, busy street ~54 loops/s (16:9). Next: PGO over the whole binary.

## 2026-10-21 — DLC works: the game finds, checks and decrypts its unlock files with the player's licence
EBOOT: BCUS98125 update 1.04 | ps3recomp a679051 + patch 0001
**How inFamous 2 takes DLC** (both EBOOTs; 1.00 knows 10 unlock files, 1.04 28): every item is one small EDAT,
/dev_hdd0/game/BCUS98125/USRDIR/dlc/ulf/unlock_<item>.edat (NPD v3, licence type 2, content id
UP9000-BCUS98125_00-PRMALLUS00000000 for the "all DLC" pack). It lists dlc/ with cellFsGetDirectoryEntries
(one 0x138-byte entry per call: CellFsStat 0x34 + CellFsDirent; d_type at +0x34, name at +0x36), then per
item cellFsStat, sceNpDrmIsAvailable(klicensee built in its own code, path), and cellFsOpen(path, RDONLY,
&fd, arg=2, 8) -- the EDAT open -- and reads the plaintext. 1.04's sceNpDrmIsAvailable2 sites are elsewhere.
**Runtime (generic).** cellFsGetDirectoryEntries was unregistered: it "succeeded" with nothing written, so
no DLC could ever appear. Now implemented over the open-dir table. sceNpDrmIsAvailable/2 were unregistered
too ("succeeded" untested): now edat_license_check -- the NPD header must authenticate under the title's
klicensee (CMAC(klic ^ NP_OMAC_KEY_2) over 0x60 bytes) and the RIF key for the content id must be supplied,
else 0x80029521 (licence not found), what a PS3 without the licence answers. RIF keys may now also live one
per licence as <exdata>/<content id>.rifkey (exdata = $PS3_EXDATA_DIR or
gamedata/dev_hdd0/home/00000001/exdata), beside $PS3_RIF_KEY and <file>.rifkey.
**Checked:** with the pack's 10 files in game\hdd0\game\BCUS98125\USRDIR\dlc\ulf, the game lists them,
every header authenticates under its klicensee, and each check answers "no RIF key" -- the boot carries on
with the items locked, as on a PS3 without the licence. Probes for the 1.04-only skins get ENOENT.
**With the player's licence** (the RIF key made locally from their own .rap -- the conversion is not in this
tree -- and placed as the exdata .rifkey above): every check answers 0, each unlock file decrypts (block hashes
pass; the plaintext is a one-line "Sentinel file for <item>"), and the items are in the game: the Powers menu
at the start of New Marais lists Sniper Blast and Lightning Hook under Blasts and Stalker / Electrocution
Grenade under Grenades. A/B with the key hidden (PS3_EXDATA_DIR=empty): Blasts shows only Alpha Blast.

## 2026-10-21 — title update 1.04 is the default build; the hooks carry both EBOOTs' values
EBOOT: BCUS98125 update 1.04 (and the disc v02.00, build-v100) | ps3recomp a679051 + patch 0001
**Result.** The lift of update 1.04 boots, loads its patch archives and plays: prologue step 8, the gate,
the Beast battle (task jumps through the 1.04 WarpToCheckpoint) and New Marais from the player's 1.00 save
all reach READY (35-49 s, as on 1.00) and exit cleanly; gate ~29 fps, Marais median 40 fps. The disc build
(kept as build-v100\infamous2.exe, or tools\build.ps1 -Disc) passes the same scenes.
**Per-version hooks.** src/if2_version.h: every guest value the title code uses is IF2_EA(1.00, 1.04) /
IF2_V / IF2_FN (lifted function names); CMake sets IF2_EBOOT_V104 from analysis/inputs.json and refuses
updates 1.01-1.03. Comments keep the disc EBOOT's addresses. ppu_postlift.py has HOOKS_V104; relift.sh
takes config/ss_cont_list.104.txt (eboot_map.py translate of the disc list: 2369 of 2506 entries carried
over, the rest are in code 1.04 changed; listed at the end of the file).
**What 1.04 changed under the hooks** (every address mapped, each risky one checked through the code that
reads it; then an independent audit: 484 assumptions checked by 7 auditors, every suspected break given
to an adversarial verifier -- 7 confirmed, all fixed below, 6 refuted; if2_waitev clean):
- three constant-pool slots of the aspect hack moved by one word (the update inserted pool words): noise
  0x9A2494, FMV 0x9AA82C, camera 0x9AF3EC -- a guess by the neighbours' shift lands on the next slot;
- a new dynamic-quality system: a level 0..20 rises while the scene's display data per frame exceeds 5 MiB;
  levels 12..20 give the visprog modes other than 3 a screen-size threshold (c*tanX)^2 from a table at
  0x9D7224 (1.00: 0). lod=1 / the detail row now scale its nine c values too (else wide screens lose detail
  exactly in heavy scenes); the mode-3 0.1 moved to 0x9AA4F4 (multiply now in a shared block);
- the signal-name table gains id 109 (ContmodekChanged); the warp-flag switch names 0x80 SuppressText
  (unnamed in 1.00). Our flag names were bit-indexed and wrong from 0x80 up on both builds ("Autoplay" is
  another enum): now keyed by value. The task graph has 986 nodes (disc 968; the old "722" was wrong);
- if2_waitev's edge kicker loads its globals from TOC-0x7AC8 (was -0x7AD0); queue fields unchanged;
- the mesh-contact function's frame grew by 0x10 (postlift anchor: LR save at r1+0x610; spill slot r1+0x448);
- PlayStation Move support (libgem, cellCamera): no device; cellGemGetMemorySize and
  cellCameraSetNotifyEventQueue2 are unresolved (faked CELL_OK) and harmless without a camera.
**Three runtime/pipeline faults the update exposed (all generic):**
- cellGame answered every PARAM.SFO query with placeholders (TITLE_ID "BLES00000", APP_VER "01.00"):
  cellGame_init_from_paramsfo was never called. 1.04 builds its patch path from GetParamString(TITLE_ID)
  and hung at boot looking in /dev_hdd0/game/BLES00000. cellGame now reads the boot content's SFO on first
  use, and an installed update's SFO (CATEGORY GD) for APP_VER, as the PS3 does.
- the SPU side was address-keyed and only partly redone: generated/spu_pm registered the decompressor task
  ELF (0x7AB900) and the Wws_Job policy module (0x887080) at 1.00 addresses, so the psarc decompressor task
  faulted (1.04: 0x80A100, 0x8E5B00). lift_policy.py now locates EBOOT-resident modules and task ELFs by
  content in the EBOOT being lifted; tools\build.ps1 runs lift_jobs.sh in the full pipeline (1.04 rebuilt
  16 of the 27 job modules); analyze.sh / relift.sh / extract_eboot_jobs.py start from clean folders (images
  and job modules of a previous EBOOT were lifted and registered beside the new ones); relift.sh finds the
  Bink image by index (its name carries the EBOOT offset).
- the HDD install lives in /dev_hdd0/game/BCUS98125_INSTALL beside the update: game\hdd0\game holds the
  update (BCUS98125) and a junction to the existing install. A disc build runs without game\hdd0
  (run.ps1/run.sh/play mode set PS3_HDD0_ROOT only for an update lift). 1.04 adds empty ugc/missions/
  dl, temp, tp folders to the install and changes nothing else in it.
**eboot_map.py.** GPR bookkeeping knows which instructions write no GPR (VMX, FP, compares, stores) and which
write rA (VMX ops naming v30 used to drop the r30 pool base); votes accept a relinked field offset; hand-
matched functions seed the votes; a 16-instruction window fallback places code outside known functions;
query/rewrite never "content-correct" hand entries; new translate command. 1.00 -> 1.04: 27,297 of 30,786
functions, 19,098 data addresses by vote.
**For any title update (playbook):** map with eboot_map.py; check every address in changed code through its
reader (readers.py / poolref.py / xcheck.py in the work notes); grep the hooks for TOC/pool offsets and
code words, not just addresses; rerun the WHOLE lift (analysis, PPU, SPU images, lift_jobs) from clean
folders; check what the title asks the system (cellGame params) and where its install/patch data live.

## 2026-10-21 — double-click launcher with the game's icon; eboot_map.py (1.00 -> 1.04 address mapping)
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Play mode.** build\infamous2.exe started with no arguments (a double-click, or the "inFamous 2" shortcut in the
project folder) finds the project folder (its own folder or the parent holding game\disc), works from there, sets
tools\run.ps1's defaults (PS3_VFS_ROOT, PS3_HDD0_ROOT when game\hdd0 exists, SPU_DMA_REPEAT_LIMIT,
SPU_INTERP_UNLIFTED, NULL-read reporting) and runs the EBOOT this build lifted (analysis\inputs.json, baked in as
IF2_DEFAULT_EBOOT). When Windows made a console just for it, the console is hidden and the log goes to
logs\play-<date>-<time>.log. Explicit arguments (run.ps1) behave exactly as before.
**Icon.** tools\make_icon.ps1 turns the disc's own PS3_GAME\ICON0.PNG into build\infamous2.ico (256..16 px) at CMake
configure time; src\app.rc embeds it (and a version block) when it exists; the game window and the taskbar use it.
The icon is the game's artwork, so it is only ever generated into the build folder. Checked: double-click start
into the player's save, inFamous icon on the exe and window, no console, play log written.
**tools/eboot_map.py (toolkit, generic).** Maps code and data addresses between two builds of one executable from
the ELFs alone: functions by a relink-proof instruction signature (unique anchors, then address order between them,
then similarity, then call sites); data by votes wherever matched code materialises an address (lis pairs, TOC
slots, fields of TOC-loaded table pointers, pointer slots in .data), changed functions through an instruction
alignment, content confirmation for constants and pointer slots. 1.00 -> 1.04: 27,293 of 30,786 functions,
~13,000 data addresses; of the ~205 addresses our hooks use, ~170 map or are confirmed automatically; the rest sit
in functions the update changed and are being mapped by hand (the 1.04 port, in progress).

## 2026-10-20 — texture dump / replacement packs (P0-P2) and tools\texpack
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**What (an enhancement, off by default = the PS3 picture).** libs/video/rsx_texpack.h (pure: identity, names,
eligibility, DDS/PNG headers, sampler LOD shift, mips, BC decode), rsx_texpack_io.h (WIC PNG, our DDS, bounded
reads, atomic writes), rsx_texpack_rt.h (runtime: loader / dump / watcher threads, a COPY queue); hooks in
rsx_d3d12_backend.c (upload, prepass bind, sampler, batch begin, readbacks), cellGcmSys.c (RSX copy destinations),
rsx_fp_decompiler.c (TXL units). With TEXPACK=0 and TEXDUMP=0 every hook is one test of g_tp_on (0): no thread,
allocation or device object, sampler codes unchanged (bits 59-63 are set only for a bound replacement of another size); the only
always-on work is one TXL scan per new fragment program, kept with the program. Knobs in settings_manifest.txt
(TEXPACK, TEXDUMP, TEXPACK_DIR, TEXDUMP_DIR, TEXPACK_HOTRELOAD, TEXPACK_VRAM_MB, advanced, debug); live setters
rsx_texpack_set_packs/_dump/_vram_mb, rsx_texpack_reload for the options menu.
**Identity (design 3, critique M1/M2/M5).** pack hash = tc_hash64_v2 of the canonical guest texel stream (every
guest level and face; LN pitch padding, cube gaps and X bits removed); the cache's own hash is reused when the
layout is contiguous; a level-0 hash (index.jsonl) is the fallback match for an unstable mip tail. Never dumped or
replaced: RTs / zeta aliases / snapshots (intercepted before the uploader), depth, UN, 3D and border format words,
bytes an RSX copy or readback wrote since the slot's previous upload, a one-colour level 0, debug layout
overrides. Content changes count on the canonical hash; 2 changes gate the dump only (still looked up).
**Dump (P1, TEXDUMP=1).** Settle rule (8 batches + 2 frames unchanged), render-thread copy (8 MB per frame), dump
thread: re-hash (a racing write retries), WIC PNG (exact round trip) or our DDS (cubes, floats; TEXDUMP_RAW=1 = the
uploaded blocks of every level), temp + rename, once per (hash, decoder version) across runs, TEXDUMP_MAX_MB cap;
index.jsonl: texture records (size, format, levels, h0, first run / frame / time / "checkpoint @new step 11") and
usage records (fp, unit, crossbar, gamma / expand / signed, TXL, alpha test, wrap, filters). Fixed on the way: the
dump thread took one usage record at a time with a memmove under the lock the render thread pushes with (the first
dump run had a 1.66 s render-thread stall in [PERF] srv at the gate's settle burst); now one hand-over per batch
and whole-queue swaps. s_d3d.frame_count is a once-a-second fps counter: the runtime keeps its own monotonic frame
for the settle rule, the RSX-write window, eviction and TEXPACK_AB.
**Replace (P2, TEXPACK=1).** Pack scan (each folder a pack, "_name" off, DDS beats PNG, TEXPACK_ORDER), loader
thread: PNG (WIC) / DDS (BC1-5, BC7, RGBA8/BGRA8, R8, R8G8, R16, R16G16, floats; _SRGB read as UNORM), class and
shape checks (power-of-two scale 1/4..16, 8192 / 320 MB caps; BC sizes that are not whole blocks, e.g. the walk's
256x1 / 128x1 DXT1, only where the device takes them, D3D12 OPTIONS8, as the backend's own uploads already do),
gamma-space box mips (TEXPACK_MIPGEN=linear), upload on its own COPY queue + fence; the render thread takes an
upload once the copy fence has passed, binds the replacement's SRV with the original's crossbar mapping (gamma /
expand / signed stay shader constants), keeps the original on TXL units unless same size and levels, shifts
min/max LOD by log2(scale) and ends the view at the guest chain + s (critique C3), gives a non-mip guest filter
levels [0, s] (TEXPACK_MINIFY=guest: level 0). VRAM budget (auto 35% of the adapter budget): a new replacement is
loaded only if it fits beside the resident ones and the loads in flight (sizes estimated at scan time from the PNG
header / DDS size; the original stays meanwhile), least recently bound ones are evicted when over the budget or to
make room for a held-back load, never one the last 2 frames bound; an upload that lands after packs were switched
off is released. A resource is released only when the direct fence of its last batch AND its copy fence have
passed; fence waits are bounded and check device removal. Hot reload (TEXPACK_HOTRELOAD=1): ReadDirectoryChangesW,
300 ms debounce; the old replacement stays bound until the new one is usable.
**tools\texpack (C, WIC only; README there).** list (where seen, class), upscale (DXT seam deblock, Lanczos-3 in
linear light with anti-ringing, edge-aware unsharp mask in gamma space clamped to the local range; wrap / mirror /
clamp borders as sampled; cutout alpha coverage kept; normal maps found by usage or texels, renormalised, never
sharpened; data skipped; --scale 1 identity / --tint test packs), compress (WIC BC1/2/3 DDS + mips), selftest.
--method external drives Real-ESRGAN ncnn-vulkan (the approved download, kept outside the repo; the zip carries no
licence file, the two repos' licences were checked: Real-ESRGAN BSD-3, the ncnn-vulkan port MIT) on a
wrap-bordered copy; alpha goes through our resampler, normal maps and data never go to it. Comparison (2x crops of
dumped planks / facade / cloth / rust / UI art): Real-ESRGAN is far sharper on clean hard surfaces (planks, facade
windows) but smooths fine grain (rust, concrete) and fabric weave into a flat painted look and draws crack lines
into the UI logo; the built-in one never invents detail and is modestly crisper than bilinear. Use Real-ESRGAN per
texture for architecture, the built-in one for organic / noisy / UI textures and always for normal maps.
**Tests.** test_texpack 124688 (identity against a slow reference over 3036 layouts, contiguous reuse, X bits /
padding / cube gaps, names, 200k mutated DDS files ending at a guard page, SFO, index lines, sampler shift, mips,
BC decode, WIC round trips, refusals); test_texpack_d3d12 80 on WARP (copy-queue upload read back bit-exact, PNG
mips, LOD levels with a 4x replacement in 7 sampler cases, both-fence release, fence-gated publish, an upload
landing after packs off is released, TEXPACK_VERIFY, eviction order + 2-frame protection + eviction for room, the
budget fit gate, scan-time size estimates, the slot path identify -> index -> loader -> bind incl. the level-0
match and the uniform / UN / RSX-written / 3D / border vetoes, dynamic = dump only, live setters called before
startup kept). The D3D12 debug layer is not installed here (the test says so; GBV is switched on when it is).
texpack selftest 19 (seamless wrap 2x/4x, clamp edges, no halo, alpha coverage, unit normals, deblock, PNG exact,
WIC BC1-3).
**In game (prologue step 11 gate walk, 16:9, PAD_NO_HOST; _evidence of the sandbox).**
- Dump: 835 textures (133 MB PNG, 1550 usage records) in the first run; a second run of the same walk (raw DDS,
  paused at the end) wrote all 835 same names again plus 9 first seen at the pause (the pause menu's art).
- Identity: all 844 raw DDS of that walk as a pack, TEXPACK_VERIFY=1, TEXPACK_AB=40: 838 loaded, 838 identical to
  the guest texture at every level, 0 differ; in the pause menu (a static scene) ~13k replacement binds per 20
  batches, and frames with packs on equal frames with packs off to the 1-LSB menu animation that consecutive
  same-phase frames also show (on/off pairs: <= 133 pixels differ by 1, several pairs byte-identical).
- Final exe, the same identity pack with TEXPACK_VERIFY and no A/B (R9): 835 loaded, 835 identical, 0 differ, 0
  refused -- incl. the four 256x1 / 128x1 DXT1 textures the loader refused before (now accepted where the device
  takes BC sizes that are not whole blocks, as the backend's own uploads of them already need).
- Coverage: all dumped colour textures tinted (A/B in one process): the replaced surfaces turn magenta, nothing
  else changes. The gate floor is f8a4c2a2990804e8 (a solid-green probe file showed it).
- Test pack (gamedata\textures\packs\BCUS98125\testpack): 2x built-in PNG (planks tinted red, dark planks, rust,
  Cole's clothing atlas, the planks' normal map), 4x Real-ESRGAN -> BC1 DDS (facade), 2x Real-ESRGAN -> BC3 DDS
  (skyline, alpha): the floor shows red-tinted, crisper boards in the on frames; TXL kept-original 0; sampler sets
  peak 34-55/128 (critique M3: no pressure).
- Hot reload: two probe files replaced while playing -> "pack index: 9 files (0 new, 2 changed)", both reloaded at
  2x (4096x2048 RGBA8, decode 50-70 ms on the loader) and swapped in; that second's worst frame was 53.9 ms against
  38-45 ms around it (frame dumps on in all of them): one frame, two 43 MB uncompressed uploads.
- VRAM budget (TEXPACK_VRAM_MB=96, the test pack): before the fit gate 7 replacements were resident (192 MB, all
  bound every frame, so none could go; 3 evicted after the camera turned, reloaded when seen again); with the
  final gate 2 are resident (85.3/96 MB) and the other visible ones stay original ("held back" 36-84 binds per
  20 batches), no eviction churn. Intermediate gates on the way: file-size guesses 149 MB, request-while-under
  128 MB; eviction to make room for a held-back load is covered by the WARP test (G2).
- Cost in one process (TEXPACK_AB=240, standing at the gate, 2 s after each toggle skipped; PERF render-thread
  shares): every texture of the scene replaced (identity pack, ~11k replacement binds per 20 batches)
  render_frame 27.7% on vs 27.3% off (prepass 18.2 vs 18.0, srv 15.5 vs 15.2), fps 24.9 vs 24.7; the test pack
  28.6 vs 28.3% (srv 15.9 vs 15.8), fps 23.8 vs 23.2. Worst frames after the scene start: 107-160 ms at t+7 s
  in every run of every arm, packs or not (the scene's own; one sbbase-exe run also had a 1.08 s frame); the
  520-790 ms scene-start frame likewise.
- fps (gate walk, READY+15..40 s, arms interleaved, 3 runs each; other builds ran meanwhile): sbbase
  exe 24.0 (23.6 / 24.0 / 24.6), defaults 22.7 (21.3 / 25.8 / 22.7), test pack 25.6 (25.6 / 25.6 / 22.6), dump
  (all 835 written during the walk) 22.6 / 34.5: every delta is inside the run-to-run spread; the main
  quiet re-measure decides. Final exe at defaults: READY, the walk at 23.6-26 fps, no [TEXPACK] output.
**Verification pass (final exe).** Fixed on the way: the sampler-code bits moved from 54-58 to 59-63 (the
shadow sandbox's TEX_BORDER palette index uses 54-58: a collision a textual merge would not have shown); a live
setter called before tp_rt_startup was reset by it (now 0 = no change, value + 1 otherwise); no upload once
shutdown began; the hooks in rsx_d3d12_backend.c sit one unchanged line away from where the options and resscale
sandboxes insert (5 keep-both adjacency conflicts -> 1: samp_write, next to resscale's rs_art_samp; keep both,
the texpack block first). Runs, 16:9, PAD_NO_HOST:
- New Marais (03_forward_momentum, boat cutscene then the docks): a raw dump wrote 1194 textures (108 MB DDS); that
  dump as a pack with TEXPACK_VERIFY, a 256 MB budget and TEXPACK_AB=150: 1108 loaded, 1108 identical, 0 differ,
  0 failed (104 MB resident).
- Eviction + hot reload stress (the same 1194 files, 48 MB budget = about half of what the scene's replacements
  need, packs toggled every 100 frames, TEXPACK_HOTRELOAD=1, pack folders renamed away and back while bound): 1421
  loads, 571 evictions, 0 failed, VRAM never above 48.0 MB; DXT1 folder away -> "989 removed", 46.6 -> 12.5 MB;
  back -> "989 new", reloaded as they were bound again; DXT45 away -> 192 removed, 47.8 -> 34.3 MB. 59 fps median
  at the docks against 60 at defaults in the same scene.
- Beast fight (step 16): an early pair of runs read test pack 27 vs defaults 43 fps; interleaved re-runs on the
  final exe: defaults 37.6 / 35.6, test pack 35.4 / 37.3 (mean, t = 40..60 s), worst frames 41-94 ms in both arms;
  same-process A/B: packs on 32.8 vs off 31.8 fps (exe 17:03), 32.3 vs 39.7 (final exe) -- the fight's own phases
  dominate, no difference beyond noise.
- Gate, final exe: the test pack with TEXPACK_AB=75 shows the red-tinted 2x floor in every on frame and the
  original in every off frame (_evidence\final_gate); defaults: READY, 28.5 fps, no [TEXPACK] line.
- sbapply dry-run against the live tree: everything merges except docs/NOTES.md (two top-of-file entries: keep
  both).
**Open.** A persistent staging ring for the loader (one committed upload buffer per texture today); the hot-reload
frame spike with big PNGs (play from DDS); "where seen" from the title (mission / step) instead of the run
harness's checkpoint name; manifest.ini overrides, a RAM cache and compiled packs (design P3/P5); BC7 encoding
(not in WIC); the built-in upscaler's gain is small at 720p until resolution scaling lands (critique M15); the
D3D12 debug layer / GBV run of the WARP test on a machine with the Graphics Tools.

## 2026-10-21 — camera smoothness: no shader-compile hitches; even frame pacing as an option; what is left
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Measured** (FRAME_TRACE=<csv>: one row per presented frame -- render-thread split, compiles, PSO creates, texture
uploads, readbacks, vblanks each frame stayed on screen; tools/frametrace.py summarises, per "camera still" / "camera
panning" phase from the scripted right stick). Pier, step 11, 54 s of scripted pans (still / full right / back / again
over the same views / partial deflections), 64:27 window, quiet machine:
- old build: panning 54.0 fps, max frame 64.8 ms -- a 45 ms D3DCompile (shader-cache miss) plus a 59 ms PSO miss path
  while the camera turned onto new geometry. That was the visible hitch.
- new build: panning 54.7 fps, max 32.6 ms, 0 compiles on the render thread.
- what still reads as judder: the game's frame time is bimodal (present-interval p50 16 ms, p95 28-29 ms), so on the
  75 Hz panel frames alternate 1 and 2 vblanks (1x1183 2x618 3x29 while panning). That is throughput, not a hitch.
**Shader compiles off the render thread (default on).** SHADER_ASYNC: 4 below-normal compile workers (SHADER_THREADS);
the render thread waits only for a shader a worker is already building. SHADER_KICK: a new fragment program's shader is
queued to the workers when its first draw is RECORDED, before replay needs it. SHADER_PREWARM: every program seen is
listed in shadercache\shaderlist.bin and compiled by the workers at the next start (from the walker's idle time),
so a second session never compiles in a frame. =0 on any of them is the old path. Warm-session PSO creation from the
driver cache costs 0.1-0.2 ms, so no PSO library was added.
**Even frame pacing (opt-in; options menu Frame > "Even frame pacing", env FRAME_LIMIT).** The guest flip is released
on the display's own vblank grid (DwmGetCompositionTimingInfo) every k vblanks, with a frame-latency waitable swap chain
(one queued frame) and present interval k: every frame stays on screen exactly k refreshes. auto = the steadiest k
(1..3) the scene holds, stepping up at once when 2 of 10 frames miss and down after 150 that fit; auto:30 never below
30 fps; a fixed rate that is refresh/k (75, 37.5, 25 here) locks k. Measured at the pier: auto locks 25 fps (3 vblanks,
807 of 837 panning frames), perfectly even; 37.5 misses often (2x/4x mix, 30 fps) because ~10% of frames take 28 ms.
So a smooth 37.5 or 75 here needs the frame-time work (perf), not pacing. Default off (today's behaviour).
**Also:** GCM_FIFO_CATCHUP_RUN=<k> (default 1 = old): the FIFO catch-up that occasionally put two guest frames into one
present; k=3 changed nothing measurable, so it stays 1.
**Next:** the frame-time work -- cut the 28 ms frames (SPU barrier/kicker waits, walker) so 37.5 locks, then 75.

## 2026-10-21 — controllers: the physical pad is player 1 next to a remapper's virtual copy; controller vs mouse arbitration
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Report: "the controller does nothing once I click the game window."** Two causes, neither in the title. (1) The test PC runs
DS4Windows 3.3.3 (Xbox 360 output, exclusive mode off) with ViGEmBus and HidHide: a virtual Xbox 360 pad next to the USB
DualSense. XInput pads took the first ports, so port 0 -- the only one the single-player title reads -- was the copy
(padprobe --list --readonly at sbbase: port 0 = XInput pad 0 045E:028E, port 1 = DualSense, plus the "seen twice" hint;
_evidence/padprobe_before.txt). Player 1 had no gyro, lightbar, pressure or PS button, and whenever DS4Windows stopped
feeding the copy the controller looked dead. (2) With the window clicked the mouse is captured, and any mouse count
(smoothed 50 ms, lifted to the 0.20 anti-deadzone) replaced the controller's right stick for that report: a resting or
jittering mouse pulled the camera. Many game windows today were PAD_NO_HOST=1 harness runs (controllers off by design).
**Virtual duplicates (pad_host_win.c "Virtual duplicates"; prefer_physical = 1, input.ini [controller], env
PAD_PREFER_PHYSICAL; PAD_BACKENDS unchanged).** A device is a remapper's copy when its ancestry (CM_Get_Parent walk over
instance id, hardware ids and service; no new deps) reaches the ViGEm bus (Nefarius\ViGEmBus\Gen1 / service ViGEmBus),
ScpVBus, or a root-enumerated software bus before any USB hub, Bluetooth radio, PCI or ACPI node, or carries Steam's
28DE:11FF. An XInput slot is matched to its XUSB device through the LED state xusb22 keeps for it (IOCTL 0x8000E008,
read-only; LED 6..9 = players 1..4 = slots 0..3, checked on the test PC with DS4Windows running: LED 6 = slot 0); caps VID
0x28DE = Steam; an unmatched slot is virtual only when every XUSB device is virtual and no XInput pad is on Bluetooth.
As many copies as there are physical PlayStation / Nintendo / HID gamepads are **hidden** (no port, listed as
"hidden"); Steam's also count physical XInput pads (Steam can copy an Xbox pad). Hidden rather than moved behind the
physical pads: the copy is the same controller, and on a port of its own it is a phantom player 2 mirroring player 1.
A hidden copy is a standby: the physical pad gone (unplug, HidHide) -> the copy takes its port; the physical pad back
(a replug where DS4Windows' copy is quicker than our open) -> it takes that port back from the copy. At startup copies
get ports after the physical pads. Real Xbox pads are on none of these buses: an Xbox pad and a DualSense keep ports 0
and 1. Everything is re-decided on every connect / disconnect (ph_dup_reeval) and on HID / XUSB interface arrival or
removal (XInput slots re-classified after the slot poll; a slot matched once keeps its answer while its device is
unreadable, i.e. leaving). prefer_physical = 0 restores the old order exactly (no startup hold, copies ported at connect).
Found by the new test and fixed in the same code: moving the physical pad onto the hidden copy's port first unbound its
old port, and ph_unbind's hand-out gave that port straight back to the pad (portless for that moment) -- one pad on two
ports, and a stale s_port[] entry after its unplug.
**Who is player 1.** The device list ends "Controller: DualSense (USB) on player 1" ("Controller: none on player 1
(port 0)" without one), a hidden copy is listed with "virtual: ViGEm bus" and one explanatory line, and the hide itself
is logged ("XInput pad 0 (045E:028E) is a virtual pad (ViGEm bus: DS4Windows, DualSenseX, BetterJoy, x360ce, ...), the
same controller as a physical one here: hidden while that one is connected ..."). cellPad.c shows the same sentence in
the window's notice box (ss_ui_notify, which also logs "[ss-ui] notice: ..."; /alternatename falls back to an empty stub
where ss_ui.c is not linked) once the window is up and player 1's pad has held for 300 ms (a hand-over shows once), and
again when it changes ("Player 1's controller disconnected" when it goes). padprobe marks hidden / virtual devices and
prints player 1.
**Arbitration (pad_kbm.h kbm_arbitrate; input.ini [options] last_device_wins = 1, mouse_takeover = 40).** Buttons still
combine. Right stick: a controller button press, its stick leaving rest (radius 0.15 after the host deadzone) or held out
while the mouse is still gives the camera to the controller; the mouse takes it back when its leaky displacement sum
(tau 500 ms) reaches mouse_takeover counts at mouse_sensitivity 1 (scaled by the sensitivity, so a turned-down high-DPI
mouse needs the same hand motion), or at once on a mouse-button or camera-key press. Displacement, not path length:
the first version summed |motion|, and 2-count diagonal noise on every report took the camera (test). Camera keys held
while the controller has the camera still turn it when its stick rests. Left stick: move-key press vs the stick leaving
rest, whichever came last; held keys still move while the stick rests. A controller that appears starts with both
sticks. last_device_wins = 0 is the old override; mouse_takeover = 0 lets any mouse motion take the camera.
**Tests.** test_pad_host 244/0 (was 193): the ancestry rules on the test PC's real device chains (_evidence/pnp_chains.txt),
LED -> slot, and 11 port cases on a four-slot fake XInput + stub pads -- DS4Windows' X360 + a DualSense at startup
(DualSense 0, copy hidden and listed), DS4Windows stopped / started while running (no port change), the DualSense
unplugged (copy takes 0) and replugged (DualSense takes 0 back, one port), prefer_physical toggled at run time, a real
Xbox pad + DualSense (0 / 1, with DS4Windows' copy of the DualSense hidden), Steam copies of an Xbox pad and of a
DualSense (hidden; a third with nothing left to copy gets a port), DS4Windows' virtual DUALSHOCK 4 (HID) hidden and
taking over, two DualSenses + three copies, prefer_physical = 0 = the old order; PAD_PREFER_PHYSICAL beats config_set.
test_pad_kbm 791/0 (was 737): resting-mouse noise (1-6 counts, every report or every 3rd, at rest or with the stick
held) 0 stolen reports in 125 (125 of 125 with last_device_wins = 0); real motion takes the camera in 5 reports; stick
or button takes it back; hybrid sweeps; mouse button; camera keys; keys vs left stick; slow drift; sensitivity scaling;
switches; parsing. test_cellpad_host 167/0 after the merge with the options overlay's tests (was 104): the notice text, once, nothing before it; the "Controller:" log
lines. test_input_ini 21/0: the new documented lines are the defaults.
**In game** (2026-10-21, live build, controllers on, DS4Windows not running at the time): the device list ends
"[padhost] Controller: DualSense (USB) on player 1" and the notice box shows the same line once the window is up.

## 2026-10-20 — in-game options menu (F1 / PS button): DirectComposition overlay, live settings, FPS counter
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**The menu** (`libs/video/ovl.c` host / thread / input, `ovl_menu.c` model + layout + widgets, `ovl_draw.h` +
`ovl_gpu.h` draw list and D3D12 renderer, `ovl_font_gdi.h` glyph atlas, `ovl_dcomp.cpp`; public API
`include/ps3emu/overlay.h`, with `ovl_open_options()` for a menu bar) is built from the settings store's rows:
tabs Display / Graphics / Frame / Controls / Audio / System, groups and labels from each row, the widget from its
type (selector with a choice list, slider, toggle, action button), badges RESTART (filled while pending) and ENV
(locked; the tooltip names the variable), a help pane (what it does, default, when it applies, where the value
came from, where it is saved), PlayStation / Xbox / Switch button glyphs for the pad in use, key caps otherwise.
Rows nobody registered (render scale, AA, texture packs) are not shown. Shadow filtering is a slot: ZTEX_PCF is
the shadow work's fidelity A/B, not a menu option, so nothing is registered. New rows: `fps_counter`
(off / on / detailed), `fps_counter_corner`, `menu_pad_combo` (L3+R3 held 0.5 s / none), `ui_scale`, and in
cellAudio `volume` (master, 100 = untouched) and `mute_unfocused`. The Frame tab's fps list holds the monitor's
refresh and half of it ("37.5 fps (half of 75 Hz)", "75 fps (the monitor's refresh)").
**Rendering (critique C7 / M7):** its own composition swap chain at the client size (B8G8R8A8, premultiplied,
FLIP_SEQUENTIAL, FRAME_LATENCY_WAITABLE_OBJECT, repeated in every ResizeBuffers; minimised sizes skipped) on its
own queue and thread, shown by one DirectComposition topmost target on the game's top-level window, above the
view child that owns the game's flip chain. Nothing exists until something is first shown (no thread, no swap
chain, no DComp: a run that never opens it creates none); device / target / visual are released whenever nothing
shows, so the game's chain can go back to independent flip. Fallback `OVL_HOST=backbuffer` (or no dcomp.dll):
drawn into the 1280x720 backbuffer after the CELLMARK copy. Text: GDI Segoe UI drawn 1:1 at integer pixels.
Faces under 22 px are now rasterised at 4x and box-filtered with fractional advances: GDI's hinting at 15 px gives
Segoe UI's 'm' 10 px of ink in a 13 px advance (TextOut shows the same), so at a 1707x720 window (UI scale 0.8)
every body line read "m any", "gam e"; partial coverage follows GDI's own white-on-dark curve (about sqrt).
**Input (critique M3-M5, C8):** F1 (exactly; removed from the keyboard bindings), the PS / Guide / Home button,
L3+R3 held (both masked from the game while held), `ovl_open_options()`. While open: keys and mouse go to the
menu, F11 / Alt+Enter / Alt+F4 still reach the window (F11 now also updates the store's `fullscreen` row), the
mouse is released and not re-captured by WM_ACTIVATE / WM_SIZE, every port reads neutral, motors off (last
request replayed at close), keys and pad buttons held at close ignored until released; the controller navigates
only while the window is in front. The game pauses the PS3 way (stage A's intercept); `pause_mode = off` keeps it
running.
**Captures (C3):** the READY / AutoSnap screenshots, `kbm_inject.ps1 shot` and now `checkpoint.ps1`'s screenshot
hide the overlay for the grab (`ovl_capture_begin/_end`, from another process the registered message
`ps3recomp_overlay_capture`; `-WithOverlay` keeps it). PrintWindow(PW_RENDERFULLCONTENT) does include it.
**Bugs found in game and fixed:** (1) the overlay thread read the open state before its wait, so the close that
woke it was seen only at the next wake -- with nothing else to show there was none, and the last menu frame
stayed on screen after F1 / Esc (`Q2_6427/q2_11`, `Q4_pauseoff/q4_04`); now read after the wait, guarded by
`test_ovl_host.c`. (2) A tooltip's 0.5 s delay and a footer message's end never redrew (the deadline was dropped
once passed). (3) ENV + RESTART badges ran into the control on narrow rows; the label now gives way (menu test
checks badges end before the control at 10 window sizes). (4) The About row's host text was cut at 47 bytes.
**In game** (pier, PAD_NO_HOST, IF2_CLOCKLOG, OVL_LOG; `_evidence/Q*`, `R_counter`):
- First open (F1 posted to the window): `[ovl] host: DirectComposition over the game window`, glyph atlas
  512x512 at scale 0.80 built in 53-61 ms on the worker, composition attached; the menu is composed over the
  live picture (game dimmed behind it) at 16:9 1280x720 (`Q1_tour`), 64:27 1707x720 and borderless 2560x1080
  after F11 pressed while the menu was open (`Q2b_6427`: surface 2560x1080 -> atlas 1.00 -> back to 1707x720 ->
  atlas 0.80; the env-locked aspect row shows ENV + RESTART and its tooltip after 0.5 s of hover).
- Pause (pause_mode = system): open -> `rates 0.000/0.000/0.000`, d0 = d1 = d2 = 0.000 every second, flips go on
  (29-35/s), the draw count frozen (10767), the mix -27 -> -64 -> -127 -> -180 dBFS; close -> d0 0.986-1.016 the
  next second, sound back (-37 dBFS). Twice in one run, also after Esc (`Q1_tour`).
- Live changes from the menu, each logged `[options] key = value (was ...)` and applied at once: brightness
  1.00 -> 1.30 in 6 slider steps (`[videoout-gamma] exponent 0.873016 ... 0.705128, PC brightness 1.30`, visibly
  brighter after close), anisotropy game -> 16x through the choice list (`[SAMP] at least 16x`), detail 1.00 ->
  0.50 in 10 steps (`[if2-lod]` thresholds 90000 -> 22500 ...), fps limit Off -> 30 (`[gcm] fps_cap 30: flips
  paced to 33.333 ms`; 30.00 fps every second after close, the counter shows "30 FPS"), FPS counter on; reset
  with R -> `fps_cap = 0 (was 30; reset)`, `flips not paced`. The fps list showed "37.5 fps (half of 75 Hz)" and
  "75 fps (the monitor's refresh)" (`Q3a_write/q3a_02`).
- options.ini (`Q3a_write`, OPTIONS_CONFIG = an evidence file): AF 16x, detail 0.75 and 37.5 chosen in the menu
  -> the file written from the template with exactly those 3 lines uncommented (`Q3_ini/options.ini`); next run
  (`Q3b_reload`, menu never opened): `[options] non-default: anisotropy=16 (options.ini:26) fps_cap=37.5
  (options.ini:30) detail=0.75 (options.ini:27)`, `[gcm] fps_cap 37.5: flips paced to 26.667 ms`, 37.4 fps from
  the first second, `[SAMP] at least 16x`, `[if2-lod] detail 0.75`.
- Input isolation with pause_mode = off (`Q4b_pauseoff`, kbm_inject SendInput, mouse captured first): F1 ->
  `[cellPad] mouse released`, intercept on without the flag, the clock runs on (d0 ~1.0); W held 2 s + 800
  counts of mouse + Space while open: the picture outside the panel changes by mean |d| 0.5-2.5 (ambient only:
  no walk, no camera turn; Space went to the menu); F1 -> `mouse captured` again; W 1.5 s after close: mean |d|
  19-38, 92-95 % of pixels (Cole walks).
- Screen grabs (`R_counter`, FPS_COUNTER=on from the start): the READY screenshot has no counter, a desktop grab
  has it ("31 FPS"), `kbm_inject shot` hides it (also the open menu in `Q1_tour/q1_03`), `-WithOverlay` keeps
  it; PrintWindow(PW_RENDERFULLCONTENT) includes counter and menu (`sheet_counter_corner.png`,
  `r_06_printwindow_menu.png`).
- Fallback host (`Q5_backbuffer`, OVL_HOST=backbuffer): the menu drawn in the game's frame (game paused, frames
  still presented), hidden from `kbm_inject shot` after two frames without it.
- Defaults, the menu never opened (final exe, `S_dump_*`): the 4 CELLMARK boot-frame dumps byte-identical to the
  sbbase exe's (md5 613aea7031, as stage A), the 22 startup / config lines identical, and the only `[ovl]` line
  is the install note (no thread, swap chain or DirectComposition object). Pier-walk fps, interleaved on a busy machine, first 24 s after READY: sbbase 35.0 / 34.2 vs
  new 34.7 / 34.0 (median; -0.3 / -0.2, noise).
**Tests:** `libs/video/tests/test_ovl_menu.c` 1064 checks (layout at 10 window sizes / DPIs incl. badges ending before
the control, tabs from the table, navigation, every widget, choice lists, env-locked rows, reset / reset-all,
presets, mouse hit tests and drags); `test_ovl_warp.c` 120 (WARP: text copied 1:1 from the atlas, the
supersampled small faces -- fractional advances, ink spanning the advance, integer placement --, shapes, the
whole menu at 4 sizes; renders in `_evidence/O2_warp_ss`); new `test_ovl_host.c` 16 (the host in a game-shaped
window: nothing before the first open, open / 2560 wide and back / minimise / capture / close / counter on and
off; it failed on the stale open read before the fix); `test_cellpad_host.c` 160 (+ the system's buttons: the
PS button and the held L3+R3 reach the hook once, L3+R3 masked while both down, a lone L3 passes; the
end-of-intercept hold now released and pressed again on both pads -- two checks still expected the old
behaviour); `test_options_store.c` 283 (fps_cap is FLOAT now: 37.5 accepted, an INT row refuses 12.5);
`test_videoout_gamma` 52 + d3d12 PASS. Build: tools\build.ps1 -BuildOnly OK.
**Open:** no physical controller in these runs (PAD_NO_HOST): the PS / Guide button, L3+R3, pad navigation and the rumble
mute are covered by the unit tests only; on Windows 11 the Xbox Guide button may also open the Game Bar.
PrintWindow-based grabs (grabwin / tour) include the overlay (test runs keep it off). Third-party overlays
(Steam, RTSS) over the second swap chain untested. No first-run hint, no key-rebinding page yet. While the FPS
counter shows, DWM composes the window (no independent flip / VRR). `[ovl]` lines can interleave with other
threads' output. A window menu bar can call `ovl_open_options()` (built separately).

## 2026-10-20 — settings store, the PS3 pause (CELL_PAD_INFO_INTERCEPTED), live frame pacing
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**One settings store** (`include/ps3emu/options.h`, `libs/system/options.c`, comment-preserving editor
`libs/system/opt_ini.h`): a row per setting (key, type, range or choices, default, env variable, preset
values, live / next-frame / restart), change callbacks (on the setter's thread, or queued to the render
point), and `config/options.ini` (git-ignored) layered over `display.ini` / `input.ini` and under the
environment; an env-set row is locked and never saved. Every change is saved at once (tmp + MoveFileEx),
because the window-close path ends in ExitProcess; main.cpp also flushes there. The home parsers read
options.ini's `[display]` / input sections between their file and their env overrides (`opt_layer_text`),
so unregistered keys pass through; the store loads itself on first use, so if2_aspect_install's early
display.ini read already sees it. `tools/run.ps1 -Checkpoint` runs set `OPTIONS_CONFIG=none` unless
`-Options`. The startup log names the file and every non-default row with its source (`[options]
non-default: aspect=off (env IF2_ASPECT)`). The render point (`opt_render_point`) runs in render_frame
after a presented frame only (flush and unpresented batches skip it): vsync, AF, brightness, window and
the LOD constants change between two frames. Rows: display (fullscreen / window / monitor live, aspect /
hud / present / dpi_aware restart, lod / fmv live), `brightness` (PC brightness: the video-out exponent /
b; 1.0 = bit-identical; it lands in CELLMARK dumps, which is why test runs ignore options.ini), `anisotropy`
(game | 2-16x: at least that on trilinear mipmapped units, the game's own value when higher; the per-unit
sampler memo restarts; env `TEX_AF` -- first registered as TEX_ANISO, which already is the CONTROL0 A/B
switch: renamed, a unit test guards it), `detail` (detail distance: k = (lod ? W'/(16H/9) : 1) * detail on
the four LOD thresholds), `fps_cap` (env FPS_CAP's old parse first; live, re-bases at the next flip),
`vsync` (PRESENT_SYNC first), `tearing` (restart), `pause_mode`, mouse and controller rows (live through
`cellPad_kbm_set` / `padhost_config_set`). List: `settings_manifest.txt`.
**PS3-style pause (T3: the title's own code).** func_0018C2D8 runs once per main-loop iteration, calls
cellPadGetInfo2 and on the rising edge of system_info bit 0 (CELL_PAD_INFO_INTERCEPTED) sets its clock's
slot-0 rate to 0 (func_00084A90(0x9BFC70, 0, 0.0)), and calls func_00316C78(0xA251E0, 8) (a per-category
counter that on 0 -> 1 posts an event through func_0032175C: the sound pause) and the pause group
0xA08760; the falling edge undoes all three. Our cellPadGetInfo2 hard-coded system_info = 0.
`options_set_intercepted(on)` -> `cellPad_host_intercept`: the flag on GetInfo / GetInfo2 / PeriphGetInfo,
every port neutral (scripted input and the checkpoint driver's presses too; packets keep their cadence),
taps / mouse motion dropped, keys held at the end ignored until released, motors off and the last request
re-sent, no mouse capture. `pause_mode = off` or `PAD_INTERCEPT_FLAG=0` (A/B): neutral input, no flag.
**In game (16:9, PAD_NO_HOST, IF2_CLOCKLOG, AUDIO_LEVEL_LOG = new mix-level diagnostic):**
- Gameplay, pier walk (final exe, two cycles): `[if2-clock] rates 0.000/0.000/0.000`, d0 = d1 = d2 = 0.000
  each second, loops and flips go on (25-30/s), the draw count frozen (11715), grabs 3 s apart identical
  (mean |d| 0.01), the mix fades -24 -> -180 dBFS in ~4 s; off -> d0 0.995 the next second, sound back
  (-37 dBFS). `_evidence/I_pause`, earlier build `A_pause`.
- Bink movie (the boot movie): the title runs rates 0/0/1 while it plays; intercept -> 0/0/0, the picture
  frozen (three grabs over 7 s identical), its sound -> -180 dBFS; off -> 0/0/1, picture and sound go on
  from where they stopped. `B_movie`.
- The game's own pause menu (START): 0/0/1 -> intercept 0/0/0 -> off 0/0/1, still in the menu -> START
  closes it -> 1/1/1. The intercept neither opens nor closes the game's menu. `B_menu`.
- Cutscene (New Marais boat polycut): 1/1/1 -> 0/0/0 for 10 s (draws frozen, sound -180 dBFS) -> runs on to
  PolycutFinished and the docks. `B_cut`.
- A/B `PAD_INTERCEPT_FLAG=0`: no rate change, d0 ~1.0 throughout, Cole stops mid-walk (neutral input)
  while the world goes on, walks on after. `G_flag0`.
**Live settings in game** (OPTIONS_TEST_FILE, applied at the render point): fps_cap 20 -> 20.00 fps and
20 loops/s at game speed 1.000, 0 -> free again; vsync 0/1; brightness 1.3 -> exponent 0.705128 (mean
luma 68 -> 88) and back to 0.916667; fullscreen 1/0; window 1280x720 / auto; mouse_sensitivity, rumble,
deadzone and their reset; `vsync=banana`, `fps_cap=abc` refused (`C_live`). Detail distance at the New
Marais docks, still camera: draws ~9040 (1.0) -> 8840 (0.5, -2.2%) -> 9285 (2.0, +2.7%) -> 9035 (1.0) ->
8840 (0.5), the step in the first second each time; at the pier gate +-1%. Anisotropy 16x at the docks,
two cycles: the grabs of one state are identical to each other, game vs 16x differ by mean |d| 0.47 on the
grazing planks; the new counter: 16x raised all 558842 trilinear mipmapped sampler codes built meanwhile,
18.5% had no anisotropy from the game (the rest asked for less) (`H_docks`, `af_game_vs_16x.png`).
**options.ini in game** (`C_file`, -Options, a user's file): the values load with their sources
(`fps_cap=30 (options.ini:3)` ...); set back to 0 the fps_cap line is commented again with its trailing
comment kept, brightness 1.1 uncommented in place, detail / anisotropy reset commented; CRLF kept.
**Defaults unchanged:** CELLMARK boot-frame dumps (4 per run) byte-identical from the sbbase exe, the new
exe, and the new exe reading an options.ini with all 23 rows uncommented at their defaults (`F_*`); the 22
startup / config lines identical (`[display]`, `[cellPad]` bindings, `[videoout-gamma] exponent 0.916667`,
`[if2-aspect]`); pier fps medians, interleaved, busy machine: sbbase 35.0 / 37.4 / 29.0, new 34.5 / 34.0 /
37.0 -- no measurable cost. Final exe (after the alias fix below), again: boot dumps byte-identical to sbbase
(`J_dump_*`, `K_dump_new`, also with the all-defaults options.ini); gameplay frames at the pier gate (standing,
3 dumps per run, base / new interleaved x2, `N_g_*`): not in sync between runs, so compared by statistics --
mean B/G/R within the spread of two sbbase runs (B 52.0-57.9 vs 53.5-57.5), luma histogram L1 to base run 1
0.058-0.137 new vs 0.071-0.112 sbbase; fps 26.0 new vs 25.6 / 26.6 sbbase (contested machine).
**Alias in game** (`L_alias`, -Options): `frame_limit = 30 # mine` loads as fps_cap 30 (`[gcm] fps_cap 30`),
a reset uncaps at once and leaves `# frame_limit = 0  # mine`, 40 re-caps, a reset uncaps; CRLF kept.
**Tests:** `libs/system/tests/test_options_store.c` (277 checks + 7 child processes: layers, sources, env
locks single / multi, TEX_ANISO not a lock, saving one line per change, set-back byte-identical, reset to
the lower layers, presets, callbacks once per change, late registration, restart-pending, a new file from
the template, OPTIONS_CONFIG=none, a UTF-16 file stays UTF-16, a key the user spelt with an alias
(`frame_limit = 30`) is the line set and reset -- the editor first matched the key only, so a reset left
the alias line active and the old value came back at the next launch -- and the 72 commented keys of
display.ini / input.ini set and reset byte-identical with the real parsers agreeing); `libs/input/tests/
test_cellpad_host.c` + the intercept (137 checks); `test_videoout_gamma.c` + the PC brightness (52).
test_gcm_user_command.c stubs opt_find / opt_i; test_cellpad_host now links options.c.
**Open:** F11 / Alt+Enter do not update the store's fullscreen row; whether a real PS3 returns neutral
packets or len = 0 while intercepted (neutral chosen: no stuck input either way); the overlay (stage B)
calls options_set_intercepted and must hide itself from READY / AutoSnap screenshots.

## 2026-10-20 — resolution scaling P0-P2: surface census, x1 plumbing, RES_SCALE=2/3/4 (experimental)
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
Design and critique: local res-scale (design.md, critique.md; the critique's corrections applied).
**P0 census (RSX_RES_DRYRUN=1, logging only; rsx_res_census.c + the pure classifier rsx_res_class.h).** Every
op's colour target, MRT planes and zeta (keyed by the size it is bound with) are unioned into one class group;
only units the program READS count (fp_texmask, critique C1). Scenes: pier (step 11 walk), gate (13), pause menu
(13 + START), Beast (16), New Marais boat cutscene -> docks -> street (03_forward_momentum), and the boot path
(warning / loading screens, main menu, splash and intro-montage movies). Last-window numbers per scene:
- Main group (one factor): colour 0xAB8000, 0xE3C000, 0x3B0000, 0x734000, 0x11C0000 and zetas 0x10000,
  0xE3C000, 0x3B0000, all 1280x720. Shadow 0xF2F0000 1024^2 (own rule "aux", joined to shadow through its
  zeta's depth-compare reads). Scene: particles 0xB9E000 + zeta 0xF770000 at 512x288; the colour 0xF770000
  512x288 (written only by 7DAE6222) is "post" alone and joins scene through the shared zeta (C8 confirmed).
  Aux: reflection 0xF6F0000 + zeta 0xF770000 at 256^2; its blur target 0xF730000 256^2 stays "other" (no
  shared binding: P4 needs a read-edge rule or a title table). Post 1280x180 / 320x180 / 160x90; tiny <= 80x45.
  47-54 keys; 57 MB of surfaces at 1x, 190 MB with the main class at x2, 148 MB at native 2.0x1.5 with all classes.
- New Marais adds a 256x256 view drawn INTO 0x11C0000 with zeta 0x10000 bound at 256x256 (aux), and
  0xF730000 at 128x128.
- Zeta aliasing (design Q1): 0xF770000 is bound at 256x256 and 512x288 twice per frame in every scene; the
  first depth access after each size change is a CLEAR (256^2) or 7DAE6222's ALWAYS + write (512x288), and
  0x10000's 256x256 use in New Marais is cleared first too. No depth hand-off between the sizes: separate
  host instances suffice, no migration is needed. 0xAB8000 / 0xE3C000 are bound as zetas at 1280x720,
  1280x180, 320x180 and 512x288 with no depth op at all.
- Reads: every packed-depth read (zeta as A8R8G8B8) is NEAREST (0x10000 47-59/frame; 0xF770000 at 512x288
  98/frame in play, 2619/frame under the pause menu's particles); PCF reads of 0xF2F0000 are LINEAR depth
  compares; no UN texture on any render target. 1x passes reading main surfaces: the DOF / bloom / glow chain
  (LINEAR) and 7DAE6222 (packed 0x10000).
- NV3089 readbacks: exactly one colour (0xE3C000) and one zeta (0x10000) per gameplay frame, every scene.
- Self-sample copies (RT_SELFSAMPLE=2): pier 56/frame (0.19 GB/frame), New Marais 58 (0.20 GB), pause menu
  51, gate 19 on average (67 in some frames), Beast 3 on average (94 peak); each is a distinct (op, plane)
  (critique C2: 57 estimated). x2 copies 4x the bytes (0.76-0.78 GB/frame).
- POINTS / LINES: none in any scene (design R8 / Q5 closed for these scenes).
- Frame copies: no SCREENTEX (s_screen_copy) and no DB_SNAP read in any scene: the eye adaptation's reads of
  the display buffers are reads of their redirected surfaces 0x3B0000 / 0x734000 (main class). The boot
  screens, main menu and splash movie draw straight into the backbuffer (4.5-5.3 draws/frame plus a 0x10000
  clear; one depth-tested backbuffer draw at the hand-over into play); the intro-montage movie is one quad
  per frame into the display alias.
- GPU (GPU_TIME, gameplay medians): pier 1.6 ms/frame, gate 1.7, Beast 2.9, New Marais 1.6, pause menu 4.7;
  3-24 render_frame lists per frame (readback and report flushes, C4).
**P1: x1 plumbing (rsx_res_scale.h, pure; default off).** OffRT and ZetaSurf carry a host size (hw, hh; the
guest w, h stay every key, pitch and readback size); zetas are grown to the host size the bound target needs;
ztex aliases are keyed by size AND space (host texels for samplers, a guest-size pick for the readback; C9).
At replay a draw's viewport / scissor and every clear rect scale by the bound target's integer factor (the
record-time raster map and the baked VS epilogue stay valid: rsx_rs_scale_vp, CPU-tested for both routes).
The factor is latched after Present, in the presenting branch (C3), so every batch of a guest frame sees one
table. MAX_ZETA and its LRU are unchanged. Copy and readback sites decide from the resource (hw != w), the
mode only gates the plumbing. RES_SCALE=1 runs all of it at factor 1. Evidence, gate step 13, 3 arms (the
sbbase exe, the new exe with RES_SCALE unset, the new exe at RES_SCALE=1 + RSX_RES_VERIFY): 40 boot
CELLMARK dumps per arm -- frame 0 and the static boot screen (28 indices) byte-identical in all three; the
11-12 animated fade / loading frames differ by timing, equally between the two new-exe arms. RSX_RES_ARTLOG
fingerprints unset vs =1: 768 PSO descriptions (shader bytecode, input layout, all state), 31 samplers, 23
surface descriptions and the swap chain (1280x720, 3 buffers, flip-discard, stretch) identical; 288 DXBC
blobs compiled fresh into two shader caches byte-identical; VERIFY 7,105,080 new-vs-old comparisons of
viewports, scissors, clear rects and target-bind viewports, 0 mismatches. Window shots equivalent.
**P2 slice: RES_SCALE=2|3|4 (experimental, default 0).** Main class only (surfaces of the display size and
their zetas) at k x 1280x720; every other class stays 1x and 1x passes read scaled surfaces directly
(effects_input "direct"). NV3089 readbacks of a scaled surface go through a pick (colour: a compute pass into a
guest-size staging texture; zeta: the guest-size alias with a pick shader), so the SPU still gets 1280x720
normals and depth: exact at odd factors, +0.25 guest px at x2 (C5; one rule for colour and depth). Seeds of a
new scaled surface are spread from guest memory (nearest); DB_SNAP into a scaled alias likewise. ZPASS counts
of draws into a scaled target are divided by kx*ky (rounded, non-zero stays >= 1; RSX_RES_REPORTNORM=0 raw).
The display surface is box-downsampled (linear light, RSX_RES_DOWNSAMPLE) into the guest backbuffer, which
keeps every guest-size copy (screen copy, gamma pass, dumps) as before; with RES_SCALE >= 2 at start the swap
chain is the view's size and a last pass resamples the scaled surface (plus the video-out exponent) into it,
following window resizes (RSX_RES_RESIZE, debounced). The shared legacy DSV is sized for the largest scaled
target (C7) and grows on a live switch. Gate x2 and x3 and New Marais x2: no errors, readback pick logged
(2560x1440 or 3840x2160 -> 1280x720), lighting / shadows / particles / HUD in place; crops in
_evidence/p2_gate_crops.png (1280x720 window: 2x2 supersampled vs PS3) and p2_big_crops.png (1600x900 window:
2560x1440 resampled vs 1280x720 DXGI-stretched); guest-size dumps' whole-frame means within 1-4 % of off
(animated fire); sun-query totals 110-119 at x2 vs 111-117 off (normalised). With RSX_RES_REPORTNORM=0 the
same total reads 441-475 (4x): the query's draws land in a scaled target, and the normalisation gives the 1x
count back.
**New Marais docks (checkpoint 03, no pad input, 1280x720 window; _evidence/p3_docks_off / _x2):** the
daylight docks after the boat cutscene, same subtitle line and nearly the same camera in both arms. x2 resolves
the far buildings' balcony rails and window frames, and the railing, the life ring and Cole's straps lose their
stair steps; HUD and subtitles are unchanged (drawn into the 2560x1440 display surface, then boxed). No errors;
readback pick of 0xE3C000 2560x1440 -> 1280x720 logged; guest-size dumps' whole-frame means 67.4/63.3/55.1 off
vs 68.6/64.8/57.2 x2 (the camera differs slightly). Crops: p3_docks_crops.png, dumps: p3_docks_dumps.png. (The
gate crops' bottom row differs mostly by the animated smoke drifting over the tables.)
**fps (pier/gate walk, 16:9, 1280x720 window, 2 interleaved runs per arm, other builds and runs
on the machine):** off 25.0 (24.0 / 26.0), RES_SCALE=2 25.6 (25.0 / 26.2) -- no measurable cost; GPU_TIME busy
1.7 ms/frame off (the quiet run) vs 2.6-2.9 ms at x2. To re-measure on a quiet machine.
**Fixed on the way:** a partial depth clear now scales by the op's colour-target factor (a 1x op may bind a
scaled zeta: New Marais' 256x256 view into 0x11C0000 against 0x10000), not the zeta's.
**Limits / open:** x2 SPU inputs are approximate (+0.25 px; x3 exact; a half-host-pixel raster shift would
make even factors exact). The boot screens, main menu and splash movie draw into the backbuffer and stay 1x
(the window pass upsamples them). No resolve for 1x consumers yet (DOF / bloom / exposure read the 2x
surfaces with their 1x taps), no other classes, no native / non-uniform / percent modes (P3-P4). One zeta
resource per offset (grown): a 1x op on a scaled zeta uses its top-left region -- safe for the title's
256x256 uses only because they clear first. A live 0 -> 2 switch has no window pass (DXGI stretch) and remakes
surfaces from guest memory (one frame of stale content). GCM_COMPOSITE_RT and DISPLAY_WRITETHRU are refused
with a log under scaling. The zeta pick shader is exercised in game only (the colour pick has the WARP test).
**Sandbox-only harness finding:** in the worker sandboxes every New Marais autosave failed ("There was an
error while saving.", the title dims the game under the dialog; at defaults too): the save stage directory
`<sandbox>\checkpoints\_runs\03_forward_momentum_rpcs3-<time>-<pid>\savedata\.BCUS98125_INFAMOUS2_0.tmp` is
248 characters, CreateDirectory's limit (cellSaveData "cannot stage"). The live tree's root is short enough;
a sandbox run can use a short CHECKPOINT_DIR copy (the p3_docks pair used short local copy). The census's
New Marais numbers were taken under the dimmed dialog.
**Tests:** test_res_scale 151 (identity vs rsx_raster.h over 19k rects, ownership and routes at 10 factor
sets, replay scaling == record-time host map for 5106 transforms, pick / guest_of / clamp / parse / norm, the
classifier on a synthetic gate frame, the census module), test_res_pass_warp 16 (WARP: pick x2/x3 RGBA8 and
RGBA16F exact, spread, blit 1:1 / box2 / box3 / gamma), test_report_d3d12 12 (unchanged). Knobs:
settings_manifest.txt.
**Verifier pass (adversarial review, same day).** Whole diff re-read; tests rebuilt and rerun (151 / 16 / 12, 0
failed); the 3-way merge of the backend onto the live tree (which had moved: shadow D3 + the window menu bar)
compiles against the live headers. One fix: a partial depth clear of a backbuffer op (cur_rt < 0: a redirected
display alias without a surface) was scaled by the zeta's own factor although its draws run a 1x viewport on the
zeta's top-left region; it is now never scaled (no effect at defaults or RES_SCALE=1). Runs on the final exe
(_evidence/v): defaults at the gate and the Beast reach READY with no [RSCALE] / [RSCENSUS] / [RSARTIFACT] line;
RES_SCALE=1 + RSX_RES_VERIFY at the Beast 5,758,509 checks, 0 mismatches; RES_SCALE=2 at the Beast (pick logged,
HUD / objective text in place), RES_SCALE=4 at the gate (5120x2880 main class, READY, no errors),
RSX_RES_PRESENT=guest at x2; window resizes at x2 (1600x900, 1024x576, maximised 2560x1009, restored) each
followed by one debounced ResizeBuffers (1531x861, 955x537, 1794x1009, 955x537) and correct pillarboxed output.
The title's 512x288 particle composite keeps its blocky silhouettes at every factor (also at 1x); against
x4 geometry they stand out more (x4_smoke_crops.png) -- the scene class is P3+ work.

## 2026-10-20 — shadows (D3): exact depth-compare PCF; the 5-tap resolves are the title's per-polycut switch
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Exact PCF (ZTEX_PCF, default exact; fidelity fix).** rsx_ztex took its 2x2 footprint from a Gather at uv and
its bilinear weights from an fp32 frac(uv*size - 0.5). The RTX 5080's Gather snaps uv to its 8-bit sub-texel
grid, so in a ~1/512-texel band before every texel boundary it already returns the next footprint while frac()
still says ~1: nearly full weight on a texel two columns over, i.e. single lit or shadowed pixels along every
shadow edge -- the dotted lines on Cole's neck and shirt in the Kuo cutscene. The footprint is now gathered at
its own centre (floor(p) + 1) / size and weighted by p - floor(p) of the same p (one footprint per sample,
ARB_shadow, T2); the sampler still applies CLAMP / BORDER / WRAP, the compare and the lerp are unchanged, and
it costs the same three Gathers. fp_texctl depth byte bits 5-6 select it per draw (no shader recompile):
exact | gather (the code before today) | diff (diagnostic: 1 where the two differ). =hw (SampleCmpLevelZero)
not built: it needs comparison samplers in the root signature and has 8-bit weights (below).
Mechanism (tests/test_ztex_pcf.c, the decompiler's own rsx_ztex HLSL against a double-precision ARB_shadow
reference, 1024² maps, x/y/diagonal sweeps in 1/1024-texel steps, ZFUNC 1-7, linear/nearest, CLAMP/BORDER, packed
and R32 depth): exact max |err| 0.000000 on WARP and on the RTX 5080 (0 of 39.3M samples > 1/64 each); gather 0
on WARP but 26808 of 39.3M samples off by up to 1.0 on the RTX 5080; diff marks exactly those samples; the
GPU's own SampleCmpLevelZero is 0.0066-0.0071 off. 400 checks pass.
In game (checkpoint 03_forward_momentum_rpcs3, 16:9). Same frame: an RSX capture at READY+26 s with
ZTEX_PCF=diff and the shadow mask snapped after every resolve marks 209 isolated seam pixels from the close-up
box resolve C2C03CA4, +70 from cascade CC3DA20A, +25 and +69 from the two 2FBEC818 cascades
(seamcount.py; _evidence/neck/diff_mode_seam_map_n_d1.png, whose two white blocks are the far
cascade's distance fade, not seams). Window grabs over the neck close-ups, isolated one-pixel dots on smooth
skin (skindots.py, T=6): final build, five shots READY+20-23 s, exact 27, gather 62; earlier build,
nine shots, two runs per arm, exact 63 and 55, gather 142 and 97. edgeprof spikes on the neck-shadow box:
gather 2.3 per 1000 edge px, exact 0.34 (T=6). The stair-steps stay in both arms (jumps): they are the
title's single 2x2 tap of a 1024² map spread over ~6 px per texel on the close-up. The cutscene runs on the
real-time clock, so the two arms' grabs are 0.2-0.3 s apart. Crops: _evidence/neck/neck_gather_left_exact_right.png,
_evidence/final/neck_x3_gather22.8_exact22.5.png.
**The 5-tap resolves are a title switch per polycut, not a bug of ours (T3, lifted code + runtime).**
MULTI_TAP_SHADOW_ENABLE / CMultiTapShadowEnable is entity type 532: factory func_004BF1B0 (factory record
0x980400, key 0x2214 = 0x2000 | 532) -> func_0004C588 allocates the 0x14-byte object and increments the shadow
manager's counter (singleton 0xA20CD0, +0x100 = 0xA20DD0); its destructors (0x69FA40 / 0x69FAA0) decrement it
(clamped at 0). func_002D7554 = count > 0 is read by the view setup (0xC782C -> view +0x1D3; inferred: the
switch to the 5-tap box / cascade / biased-cascade programs 94BF0862 / 4EC41EA2 / E168CEAC for C2C03CA4 /
CC3DA20A / 2FBEC818) and by the caster passes (0x2D90E8: polygon offset only with multi-tap; 0x2DBF9C: flagged
casters take the offset path only without it). PPU_WWATCH=A20DD0 + IF2_SIGLOG: booting the "2. Breaking into
New Marais" autosave (sandbox checkpoint 02_arrival, written by a -Step 26 run) the counter goes 0 -> 1 from
func_0004C588 one frame before PlayPolycut (the boat arrival, Zeke hands over the Amp), the three 5-tap
programs draw every frame (1, 1 and 2 per frame), and at StopPolycut the destructor takes it back to 0 and
2FBEC818 returns -- RPCS3's cache timing (5-tap first used ~10 min into play, at the arrival). ~200 s of
scripted dock / swamp walking after it: no 5-tap draw. The 03_forward_momentum_rpcs3 boot (MissionStart, the
v_02_city_intro_g FMV skipped, then the Kuo polycut) never writes the counter ([ZTEXID]/[ZTEXCNT] with
ZTEX_LOG=300: only the single-tap programs, final build too). Nothing in our boot paths skips it: the enable
lives and dies with the arrival polycut's data, the Kuo polycut has none, and MOVIE_SKIP only shortens the FMV
before it. So the blocky, title-resolution neck shadow in the Kuo cutscene is the title's own single tap, and
no checkpoint state needs restoring. With the counter held at 1 (PPU_POKE_ADDR=A20DD0 PPU_POKE_VAL=1,
fabricated state, diagnostic only) the Kuo polycut draws the 5-tap programs and the neck edge is visibly soft
(_evidence/neck/forced_5tap_vs_default_*.png): the natural "soft shadows" enhancement is to answer
func_002D7554 with 1 (title-authored kernel, title's own polygon-offset rules), off under PS3 -- not built.
Static details and the run lines: _evidence/multitap_re.txt.
**TEXTURE_BORDER_COLOR (TEX_BORDER, default 1).** Decoded but never used: every BORDER unit sampled D3D's zero
border. Logged ([TEXBORDER], always on, first 24): in the Marais and prologue boots the BORDER units are the
Bink YUV->RGB passes (75552C13 full-screen, 251E6BD4 at 256x192) whose Cr/Cb planes carry 0x80808080 --
neutral chroma -- A426D9C6 / 91FD0398 with border 0, and five programs on 1024² depth units with 0x00FFFFFF
(the cutscene key light 604335F9, 09AAB32E, C99FA614, 7E91513D, 16912544). =1 applies the guest colour to colour units when its four bytes are
equal (GL CLAMP_TO_BORDER / libgcm BORDER_COLOR, T2; the title's own values, T3); a colour whose channel order
would matter keeps the zero border (said once: whether D3D swizzles a border colour with the view's component
mapping is not pinned down). =2 also applies those as R,G,B,A and gives depth units the word's low 24 bits
(0x00FFFFFF -> 1.0, lit outside the map; a D24S8-texel reading would give 0.0039): no hardware test for either,
so opt-in by the two-evidence rule. In the Kuo close-ups TEX_BORDER=2 shows no change I could separate from the
run-to-run offset (best matched pair READY+24.0 / 24.3 s: mean |luma| 6.2, faces and neck alike by eye).
=0 is the old output. Unset or empty = 1; any other value is logged ([TEXBORDER] TEX_BORDER=x is not
0|1|2) and taken as 1 (it used to be atoi(), so a typo silently turned the fix off).
**Polygon offset (not changed).** libgcm writes BIAS = 2 x units (func_00501EF0) and we apply DepthBias = BIAS.
New: both call sites are behind the multi-tap switch (0x2D9124 only with it, 0x2DC0F8 only without it), so a
2x constant bias, if it is one, only touches the arrival polycut's casters and the flagged casters elsewhere.
Still no second evidence source (nouveau's offset_units * 2 not checked in source); left as is.
**Regressions / fps** (final build, defaults against ZTEX_PCF=gather TEX_BORDER=0, interleaved, other workers'
jobs on the machine): mean fps from READY+10 s, two runs per arm: pier walk 24.35 against 24.26, gate 25.79
against 24.79, Marais cutscene + docks (one run each) 34.55 against 34.30 -- no measurable cost. Pier, gate and
the Marais docks gameplay (Cole's shadow, dock cascades) look the same in both arms (_evidence/final/*_default_*);
the character mask bias is untouched.
**Tests:** test_ztex_pcf (new) 400/400 on WARP + RTX 5080, test_fp_decompiler 275/275 (ztex checks updated),
test_fp_warp 66/66.
**Verify (adversarial pass, 16:40 build).** Rebuilt; test_fp_decompiler 275/275, test_fp_warp 66/66,
test_ztex_pcf 400/400 (WARP + RTX 5080; gather's seam band reproduced). Beast fight (-Step 16, not in the
runs above), defaults vs ZTEX_PCF=gather TEX_BORDER=0, two interleaved runs each: 27.63 vs 27.58 fps, same
look (_evidence/verify/beast3_default_left_old_right.png). Kuo cutscene, one run per arm: one-pixel skin
dots on the neck close-ups (READY+20-23.5 s, skindots T=6) 24 default vs 65 gather
(_evidence/verify/neck_x2_default_top_gather_bottom_22.5.png); ZTEX_PCF=diff lights a dotted seam line on
the neck and single pixels on the dock planks and the life ring. ZTEX_PCF=foo / TEX_BORDER=abc are logged
and run at the defaults; TEX_BORDER=2 boots and plays the cutscene.
**Open:** footage of the Kuo docks polycut (edge width in texels against a known-hard edge) to close "single
tap is what the PS3 shows there"; =hw; whether the RSX reads BORDER_COLOR's low 24 bits for depth formats;
the 02_arrival checkpoint exists only in the shadow sandbox (checkpoints/ is not merged; recreate it from a
-Step 26 run's "2. Breaking into New Marais" autosave).

## 2026-10-20 (morning) — window menu bar with Save / Load slots (engine pending)
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Menu bar (libs/video/rsx_winmenu.c, runtime/savestate/ss_ui.{c,h}, libs/input/pad_hotkeys.h).** The game
window has a native bar: File (Open Save State Folder, Exit) | Save (Quick Save F5, Slot 1-10 Ctrl+F1..F10) |
Load (Quick Load F9, Slot 1-10 Ctrl+Shift+F1..F10; empty slots greyed) | View (Fullscreen F11, Show FPS,
Options... -- greyed until the options overlay registers). Slot labels come from savestates\slotN.meta (time +
note); savestates/ is git-ignored. Keys are configurable in input.ini [hotkeys]; a hotkey's keys never reach
the game; Alt / F10 / Alt+letter never open the menu (they keep releasing the mouse). The client area and the
picture keep their size (the frame grows by the bar); borderless fullscreen hides the bar. display.ini
menu_bar = 0 hides it. Requests go through a one-slot mailbox to the save-state coordinator (ss_frame_tick on
the frame-clock thread, never the window procedure); with no engine registered it shows "Save states are not
available yet". Checked in game: bar drawn, F5 and Ctrl+F3 reach the coordinator ([ss-ui] lines).
Tests: test_pad_hotkeys 297, test_input_ini 19, test_ss_ui 111+ (incl. a threaded hand-over race test),
test_winmenu 91.
**Save-state engine plan** (local design.md + critique): load inside the running process
(stop, unwind every guest thread, tear down, restore memory/kernel/SPU/RSX/audio/fs, recreate threads; keep the
window, device, audio device and pads), relaunch only as a fallback. Blockers found: libsre_ns was lifted before
M1/M3 (no sc_ra, no continuations) so its 4 SpursHdlr threads can never pass a strict save -- relift libsre
first; the movie threads are not tolerant of spurious lwcond wakes -- restore lwcond waiters exactly; the stop
watchdog (3.3 s) must heartbeat during long saves/loads. Estimate: first working slot ~5-6 weeks, all ~8-10.

## 2026-10-20 — tour after the raster / ROP / reports patches: the ghost Cole was the signed blend
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Ghost Cole beside the Beast (fixed).** The heat-haze pass (fp A643D140) adds screen offsets into a
320x180 target cleared to (0.502, 0.502) with SetBlendEquation 0xF006 ADD_SIGNED; the composite (fp
E933D344) samples the scene at uv + (D.xy - 0.5) * 0.0398. Added unsigned, D ran far from 0.5 and the
scene was fetched up to ~25 px away: a displaced second Cole. BLEND_SIGNED=0 brings it back, defaults and
RT_SELFSAMPLE=0 do not (A/B window captures at step 16, 16:9).
**"Hold L1" prompt box olive over lava: the title's own blend.** The box multiplies the composite by
0.25 per channel (measured 0.251/0.252/0.254); over the clipped yellow-white lava core (255,250,95) that is
(64,63,24). Whether that lava is as yellow on hardware is a footage question (our boss frame R/G 2.66 vs
LACRY 1.76-2.08, levels otherwise in range).
**Stair-stepped silhouettes against smoke/fire: title-side (T3).** One NEAREST depth tap per 512x288 texel
(fp 7DAE6222, depth exported with ALWAYS from the Z24S8 surface read as A8R8G8B8), particles KILled or
LEQUAL-tested per low-res texel (16912544, 7E91513D, C99FA614, E0DDEC8D), and the composite samples the
512x288 particle buffer LINEAR with no depth input. Our output equals a re-computation of that composite
from the captured targets to 0.41-0.55/255 (nearest model 1.35-2.11). Footage check open (LACRY 5:45-7:00).
**New Marais 1-px light rims on characters:** the additive character pass (fp 64058C39) on grazing
silhouettes without AA, multiplied by the NEAREST screen-space sun-shadow mask; not the low-res family.
**Side finding:** CONTROL0 8x anisotropy on the two non-mipmapped DOF downsample units (B9D13BD0, E95881C2)
is dropped by samp_lod_bits (implementation-defined without mips; open question, no change).
**fps cost of the three patches** (gate walk, 16:9, 2 interleaved runs per arm, other jobs on the CPU):
defaults 36.3, REPORT_EXACT=0 37.2, RT_SELFSAMPLE=1 36.2, both 37.2, all three patches' switches off 38.2 --
about 5%, inside the noise; kept on. The exact-report flush + GPU wait per frame and the 24-57 self-sample
copies per frame are the next sync points to make cheaper (submit at the producer, wait at the consumer).

## 2026-10-15 (evening) — exact occlusion reports, copy-on-sample, cellGcmBindZcull's 12 parameters
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**What the title does with its reports (T3, EBOOT).** SET_CONTEXT_DMA_REPORT = 0xFEED0001 (HOST_BUFFER,
0x4EF5DC and 0x1326B0), so "indices 5128/5129" are IO offsets 0x14080/0x14090 in cellGcmInit's map (EA
0x40414080), not REPORT_LOCATION_MAIN. func_00131924 builds a 4x2 Y16_X16 texture over the 0x80-byte record
buffer; func_000D066C draws a box twice between CLEAR_REPORT_VALUE/ZPASS enable and GET_REPORT (depth test on,
then off); the GET_REPORT and CLEAR writers (0x501300, 0x501380) have that one caller each, and the EBOOT holds
no 0x1E98 (RENDER_ENABLE) immediate. func_000E43AC samples the records with FP 97197763A2494DB3: glow =
visible/total / (1 + 15833.8 r^2) * (0.169, 0.149, 0.135), alpha 0 -- a small additive sun glow (at most
(43,38,34)/255, half at ~6 px). No getter is imported. Our RSX never wrote the records, so the title read
whatever its allocator left there (not measured; if zeros, no glow ever). Exact counts add the glow only
around a visible sun.
**Reports (P1-9, rsx_report.c + rsx_d3d12_report.c).** ZPASS enable/CLEAR/GET cut the draws into counter
segments; each counted draw gets a D3D12 occlusion query (exact samples), a report sums its segments, and the
big-endian {timer, count, 0} lands in guest memory before anything can read it: the first draw whose bound
texture covers it, a RENDER_ENABLE test of it, a label / semaphore / reference write. RENDER_ENABLE (0x1E98)
is implemented. Sinks: REPORT_LOCAL (2048-record area), REPORT_MAIN (IO 0x0E000000 + off), HOST_BUFFER,
FRAME_BUFFER. ZCULL_STATS reports write 0 (zcull not modelled). Toggles REPORT_EXACT=0 (old host array),
REPORT_SYNC=0 (measure at GET_REPORT), RENDER_COND=0, REPORT_LOG=<n> (default 8 lines; each prints what the
memory held before). Cost: one mid-frame flush + GPU wait per frame with the sun query (not measured yet).
**Copy-on-sample (P1-8).** A draw sampling one of its own colour targets got an SRV on the live RTV (D3D12
read/write hazard). It now samples a copy taken right before the draw (RT_SELFSAMPLE=2, default: memory as the
draw starts, the rule every other texture here follows). =1 keeps one copy per texture-cache epoch
(INVALIDATE_TEXTURE_CACHE 0x1FD8, decoded now) and frame -- the cache hypothesis, opt-in by the two-evidence
rule (no hardware test shows the cache keeping a surface); =0 old. [SELFSAMPLE] lines name the draws and
count the copies per frame.
**cellGcmBindZcull** (NID 0x9DC04436) takes its 12 parameters (the generated ELFv1 thunk reads 9-12 from
SP+0x70): binding stored, logged, values outside libgcm's rules flagged. build\ppu_hle_nids.cpp must be
regenerated (its custom command does not depend on the .c files; the apply script moves the stale one aside).
**Verify pass.** RT_SELFSAMPLE's default moved from 1 (per-epoch copy, which also outlived frames) to 2; a
non-ZPASS report writes 0 explicitly; a backend without a sample counter (not D3D12) writes the logged
0xFFFF instead of a silent 0; NID comments fixed; the zcull "changes nothing" claim marked an inference.
**Tests:** test_rsx_report 48, test_rsx_report_fifo 10, test_report_d3d12 12 (WARP: the title's query pair
counts 256 of 512 samples behind a half-screen occluder; two copy-then-sample passes = 192), test_rsx_imm
PASSED (its build line now lists ../rsx_report.c; since the WAIT_EVENTS edit of rsx_commands.c it also
needs g_wev_on / g_wev_any / wev_poke stubs, as before this change). Also after merging the raster and ROP
changes, in either order.
**Not checked in game yet:** the [REPORT] lines in the prologue (what the records held, the new counts), the
sun glow on SUNSET/PIER against LACRY 2:30-3:46, fps cost of the flush (RF_DBG reason=zpass-report, PERF
[GPUWAIT]), and whether any draw logs [SELFSAMPLE] (A/B RT_SELFSAMPLE=0/1/2).

## 2026-10-15 (evening) — fps: event-driven waits (M6a) on by default; PPU inline reads (M5) opt-in
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**M6a, event-driven waits (WAIT_EVENTS, default on; runtime/syscalls/wait_events.h/.inc,
src/if2_waitev.cpp).** The title's hot guest poll loops -- `while (!cond) usleep(N)`, every sub-ms
usleep being a SwitchToThread + QPC spin here -- now block on a host event: the main thread's wait
for the light-task kicker (func_003374BC, ~10 ms of a 25 ms gate frame), the WWS barrier waits
(func_003705D8 / func_00371000, ~4.7 ms) and the SPU Light Task Kicker's own polls (~96% of a core).
Each site names the guest words its loop re-checks; the writers (an SPU PUT/PUTLLC/PUTLLUC landing
there, the RSX walker writing a label, a PPU thread entering lv2) evaluate the loop's condition and
wake it. The sleep still ends after the original period, so a missed wake costs one poll, never a
hang, and the guest re-reads memory itself as before.
WAIT_EVENTS=0 = the old spin path; WAIT_EVENTS_STATS=1 prints `[wev]` per site (pokes, wakes,
missed, poke->run latency).
**M5, PPU codegen (CMake, all default OFF; tools\build.ps1 -Define / -Pgo).** IF2_PPU_VM_INLINE
(OFF|reads|all: guest loads/stores inlined into the lifted PPU code, runtime/ppu/ppu_vm_inline.h),
IF2_PPU_NO_TLS_GUARDS (/Zc:tlsGuards- on the lifted code), IF2_PGO (instrument|use, clang PGO over
IF2_PGO_SCOPE ppu|all, src/if2_pgo.cpp flushes the profile on exit).
**Measured at the gate** (prologue step 11 walk, READY+15..40 s, 2 runs each, PAD_NO_HOST, 16:9):
WAIT_EVENTS=0 37.2 fps (frametime p99 31-32 ms); M6a 40.7 (+9%, p99 28-31); M6a + M5 reads +
no TLS guards 40.2 (no gain: the gate is SPU-bound, not PPU-bound). `[wev]`: 0.0% missed wakes at every
site, poke->run 5.7-7.3 us. M6a is on; M5 stays opt-in until it shows a gain where game code dominates.
**Tour (2026-10-20, default ultrawide window, 1280x720 internal):** New Marais cutscene + docks 67 fps
median (min 30 on the busy street), city-falls intro 66, call-lightning 49, Beast 46, gate 45, bridge 44,
pier 40. Seen: a ghost copy of Cole beside the Beast's lava body, the "Hold L1" prompt box green over
lava, a hard blocky self-shadow on Cole's neck in the Marais cutscene, stair-stepped edges on the lava
overlay -- next.

## 2026-10-15 (evening) — raster geometry: the guest viewport transform, the scissor, clear rects (P0-6)
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Change** (libs/video/rsx_raster.h, rsx_d3d12_backend.c, rsx_commands.c, rsx_fp_decompiler.c;
GROUND_TRUTH 5.2). The guest VIEWPORT_SCALE/OFFSET x/y are applied exactly (`RASTER_XFORM`): an
integer window rect (every libgcm transform) is the D3D viewport itself, past the target included; a
fractional one goes through the VS epilogue on a frame-sized viewport (`rsx_raster_host_map`). Draws
are clipped to SET_SCISSOR & surface clip & the transform's NDC square (`RASTER_SCISSOR`,
`RASTER_VPCLIP`). CLEAR_SURFACE is clipped to scissor & surface clip (`RASTER_CLEAR`). SHADER_WINDOW
0x1D88 and WINDOW_OFFSET 0x02B8 are decoded and logged. f[WPOS] in SHADER_WINDOW's convention is
behind `FP_WPOS=1`: no title FP reads WPOS. All default on except FP_WPOS; each `=0` restores the old
path.
**Evidence.** T3 (EBOOT): the viewport writer 0x501C90 builds offset/scale from an unclamped integer
rect; the scissor writer 0x501DF0 (via 0x20B16C) takes the same rect clamped to the surface, zero-sized
off it. So ordinary draws keep the old D3D state bit for bit (test "libgcm_equals_old_path"). What
changes: (a) rects reaching past the surface: before, squeezed into the full target; now placed and
clipped; (b) zero-sized scissors: before, drawn over the full target; now nothing; (c) clears under a
sub-rect scissor: before, the whole surface; (d) a depth/stencil clear of a zeta resource larger than
the guest surface stops at the surface clip (before: the whole resource). T4: Mesa nouveau
nv30_validate_viewport/_scissor, nv30_clear.c, nvfx_fragprog.c (MIT). T2: GL 2.1 4.1.2/4.2.3; PSL1GHT
rsxSetViewport/rsxSetSurface.
**Tests.** test_raster_warp (new, D3D12 WARP) 70/70. Verify pass: the first cut's frame-sized epilogue
for every draw was not bit-identical to the old viewport on 1280x720 (76 edge pixels in 192 triangles),
and a fractional D3D viewport is not the pixel-centre footprint on WARP, hence the two routes.
test_fp_decompiler 273 and test_fp_warp 66 unchanged. All 322 title FPs decompile byte-identically
(both export modes, FP_WPOS on or off).
**Not run in game.** Picture plan: PIER hoses/lifebuoy and BOAT machinery rims; lamp halo (halo.py
against targets.json scene 6). Read the `[RASTER] draw ... old: viewport` lines first. If PIER/BOAT print
none, the rims are not raster geometry.

## 2026-10-15 (afternoon) — the green gate pillar was a missing saturate; TEX8 is on again
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Cause.** The deferred sun pass (fp 6B8AE6FE) saturates N.L with the RSX source modifier 4: op 40
`MAD_H H7.w, H1, H0, H5.z[sat]` (sun weight sat(-N.L)*k + sat(N.L)) and op 43 `MUL_H H1.w,
H5.z[sat], 129` (specular gate). Without the modifier, a sun-visible face turned away from the sun
(the gate pillar's +X face, N.L = -0.85) got NEGATIVE sun light (0.89,0.62,0.54)*-0.85: red and blue
went below zero, green stayed. The modifier landed with P0-4 (FP_SRCPREC default 0x10 = saturate).
**TEX8/TEX9.** TEX8 = VP o15 (nouveau DEST_TC(n) = 7+n; 20 title VPs write o15: the third colour of
the 3-colour tint materials, or a fill light; 30 FPs read it), so it is on by default again: the gate
lanterns are cream as on hardware and the "EMPIRE CITY" sign is blue-grey (TEX8 off: green/magenta).
No title VP writes o6/o16 and no FP reads TEX9, whose source is disputed (RPCS3 o6, nouveau o16):
FP_TC89=2 to route it, 0 = both off. Checked at the gate (2 frames, per-normal-class bins): defaults 0
green-cast pixels; FP_SRCPREC=0 control 1.3-1.55% of the "visible, facing away" class; FP_TC89=0
0.26-0.57% and wrong sign colours. Tests: test_fp_decompiler 273 (incl. the two pillar instructions).

## 2026-10-15 (midday) — NV40 shader rules, float HLE arguments, video-out gamma; fps measurements
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Shader semantics (P0-4, rsx_fp/vp_decompiler.c, docs/GROUND_TRUTH.md sections 3, 6, 11):**
source precision modifiers (saturate before abs, fp16/fx12/fx9 after abs, negate last; COL0/COL1/SSA
exempt), SRC2 bit-31 perspective correction (TEXn * rcp(SV_Position.w)), destination fx12/fx9 clamps,
fp16 rounding of H-register writes, the NV_fp 3.11.4.4 condition-code rule (CC from the stored value),
COL0/COL1 saturate at the VS output, spec-certain VP fixes. Branch words are no longer decoded as
modifiers. Toggles FP_SRCPREC (0x10 default), FP_PERSP, FP_DSTPREC (0x04), FP_HROUND, FP_CCRULE,
VP_COLSAT, VP_SPEC; every switch off = the old HLSL byte for byte. Tests: test_fp_decompiler 261,
test_fp_warp 66, new spec_audit_test 32 pass / 15 known-out-of-scope.
**Float HLE arguments (P0-3).** Handlers were called through an integer-only thunk, so float/double
arguments were garbage and arguments 9+ were never read. gen_hle_nids.py now emits ELFv1 thunks (f1-f13
interleaved with the GPR slots, 9th+ from SP+0x70, float results in f1; T2 ELFv1 3.2.3/3.2.4 + T3 the
title's own calls) for the 15 handlers that need them (cellVideoOutSetGamma, cellMsgDialogClose,
cellFont*, cellResc*, cellVoiceSetVolume; cellGcmSetZcull, cellSpursWorkloadAttributeInitialize,
cellSaveDataListAutoLoad). PS3_HLE_ABI=0 = old. Found on the way: cellGcmBindZcull is declared with 1
argument but the title passes 12 (its zcull entry was never set) -- open.
**cellVideoOutSetGamma** is real now: the title asks for gamma 1.0909 (12/11, brightness option at
settings +0x6A4); the present pass applies out = in^(1/g) (VIDEOOUT_GAMMA=0/inv, _FORCE=<g>). The curve
direction is convention, not a Sony document (GROUND_TRUTH open question 12).
**Checked in game** (gate walk, real-PS3 target floor mean (65,55,43) p95 81): white-out build
(145,120,109)/210 -> texture fix (63,46,33)/65 -> with these fixes (67,51,40)/76. Faces natural.
**fps measurements** (no in-process sampler; local runs): gate 40 fps = 4 SPU job
workers ~86% busy each (85.7 thread-ms/frame), render walker 22.8 ms (91%), PPU main 24.8 ms = game code
~9.5 + waiting for SPU lighting ~10 + SPU barrier ~4.7; the light-task kicker thread spins ~96% of a core.
New Marais light walk 73 fps: SPU ~88% busy, game code ~7.8 + barrier ~5.7 ms. A/B at the gate:
SPU_TASKSET_LLE (5th SPU) 33.7 fps vs 40.5 baseline (slower: the CPU is oversubscribed), +SPURS_SPUS=7
stalls, USLEEP_CLAMP no gain. The monitor here is 56-75 Hz. The measurement build (all env-gated:
PS3_POLLTIME, KICKER_TRACE, WALKER_STATS, FRAMETIME_LOG, GPU_TIME, VRAW_PROBE, HOST_TIMELINE,
HOST_SAMPLE_SNAP_MS, FPS_CAP, PRESENT_TEARING, IF2_DTCLAMP/IF2_CLOCKLOG, SPURS_SPUS, SPU_TASKSET_LLE,
USLEEP_CLAMP; run.ps1 -Script accepts ms) is in. Next: event-driven waits instead of spin-polling, PPU
codegen (inlined vm accessors, PGO).

## 2026-10-15 (morning) — the white-out and the blue faces were dropped texture binds
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Cause.** d3d12_bind_texture recorded only a whitelist of formats (B8, A8R8G8B8, G8B8, DXT1/23/45,
W16 float); any other bind was dropped and the unit kept its PREVIOUS texture. inFamous 2 keeps its
normalised Blinn-Phong specular lookup in an X16 texture (1024x8, local 0x0F8F6000, remap 0xAAEE,
read from our own guest memory: row 7 is 0 below N.H 0.989), its water normal maps in Y16_X16 and
its sun shadow map in DEPTH24_D8. So the sun pass read a stale 8x8 ambient cube as "specular"
(~0.3-0.4 everywhere): the glow mask 0.3*sat(spec-0.1) reached 35/255 on the pier floor and the
game's own x2-per-level glow pyramid (its data, checked on RPCS3's CPU side) blew it out to white.
The face skin shader (632599C5) read a stale DXT1 as its lookup -> cyan faces. Same bug.
**Fix (libs/video, texfmt v2).** Every libgcm texture format is bound, uploaded and decoded
(new: X16, Y16_X16, R6G5B5, both 4:2:2, HILO8/HILO_S8 at their real codes 0x98/0x99, depth
formats incl. the float ones, X32/Y16_X16/W16/W32 float); per-format presentation + the 16-bit
remap rewrite (X16 0xAAEE = X everywhere); depth textures do the TEXTURE_ADDRESS zfunc compare
with 2x2 PCF (GREATER = "texel > r", envytools RCOMP + PSL1GHT); an unknown format clears the
unit instead of leaving it stale (TEX_DROPLOG). Toggles: TEX_FMTALL=0 (old path), TEX_DEPTH=0,
TEX_B8_XBAR=0, TEX_UNNORM=0, TEX_ZFLIP=1, TEX_SIGNED=1 (opt-in: the signed-bit order is
disputed and the title sets none). Also SET_SHADER_PACKER (0x1FEC, the fragment program's
linear->sRGB output encode the title enables for its light passes and water) is now decoded
(FB_SRGB=0 = old): without it the correctly lit floor is near-black.
**Checked in game** against real-PS3 footage (Lacry longplay 3:39-3:46, tools/ground_truth): gate
floor dim warm wood, no white, railing shadows; floor glow mask 35 -> 0/255; every affected unit
binds the right texture (verify_cap 0 problems; TEX_FMTALL=0 brings back 18 stale units and the
white); Cole's and Kuo's faces skin-toned. Tests: test_texture_layout, test_fp_decompiler 209,
test_fp_srgb_output 23.
**TEX8/TEX9 (FP_TC89) is now opt-in.** Routing TEX8 = VP o15 / TEX9 = VP o6 did not fix the faces
and turned the gate pillar green; gating it with SET_VERTEX_ATTRIB_OUTPUT_MASK bits 12/13 (PSL1GHT;
vp_posoffset.w, VP_OUTMASK) fixes the lanterns (near-white as on hardware) but the pillar is still
green, so o6/o15 contents or the TEX9 mapping are still wrong. Default off (lanterns a saturated
yellow) until that is found; capture for it: local tc89cap.
**Also:** tools/relift.sh now passes config/ss_cont_list.txt (save-state continuations).

## 2026-10-15 — save states M2/M3/P0 work in game; ground truth is real hardware, not RPCS3
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Ground-truth policy** (project policy: RPCS3 rates inFamous 2 only "Playable" with workarounds,
so it is not the target). docs/GROUND_TRUTH.md: evidence tiers (hardware measurements, real-PS3
footage, published specs, the game's own code/data, hardware-tested RE; RPCS3 behaviour is a
labelled side-by-side only), the two-evidence rule for RSX-only semantics, an A/B env toggle per
semantic change, no silent fallbacks, standard scenes with real-PS3 footage targets
(tools/ground_truth/targets.json + pngstat.py/halo.py/bandstep.py). It also lists what we must do
better than RPCS3 (shader precision modifiers, perspective correction, colour clamps, occlusion
reports written to guest memory, signed blend, scissor, float HLE arguments, video-out gamma...).
**Save states, M2 (stop the world).** runtime/savestate/ss_core.c: SS_CENSUS=<s> parks every PPU
thread (blocking waits first, any syscall/HLE entry after 150 ms), the cellAudio mixer, the RSX
FIFO walker and the 7 SPU contexts, records each thread's guest call chain (ss_chains.txt), then
releases; a watchdog can never leave the game stopped. Unarmed, every hook is the plain old path.
**M3 (continuations).** ppu_lifter.py --cont-list config/ss_cont_list.txt emits, per listed return
address, a function that runs the rest of its owner from there (callee-saved state from the real
guest stack slots, tail fragments, alloca exits; ppu_recomp_cont.cpp), and runtime/ppu/ppu_resume.cpp
validates a parked chain against them. **P0 (self-resume):** SS_SELFRESUME=<ms> makes a thread, at a
safe syscall/HLE entry, snapshot itself, longjmp to its thread root, reset the host thread-locals
and rebuild its own call chain from continuations -- the core of restoring a saved state.
**Checked in game** (New Marais, 600 s scripted route, vs a matched baseline): main-thread
self-resume every 2 s 301 resumes; all threads every 500 ms 17698 resumes; census every 5 s 118/118
stops converged < 500 ms; chain probe 1.19M frames walked, 606k checked against the stack -- in every
run 0 refused chains, 0 context changes, 0 host-frame drift, no watchdog/fatal, fps 106-113% of the
baseline (noise). The continuation list grew from 1475 to 3718 continuations (1427 functions) from
the runs' missing-address reports. Tools: tools/ss_selfresume.py, tools/ss_census.py. Next: M4 (the
state writer: guest memory, kernel objects, SPU/RSX state) and a load in a fresh process.
## 2026-10-14 (midnight) — movies at 30 fps; no more reinstall on every boot; glow ground truth
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Movies at 0.2 fps were the present gate, not Bink.** d3d12_clear_impl presents at the display
clear after a flip only when the batch holds s_frame_draws_max/4 draws -- an all-time maximum.
The 7517-draw title menu set it to 1879, so a full-screen movie (8-16 draws a frame) was held
~120 guest frames per present; the game, Bink and audio ran at 30 fps all along. Now
(libs/video/rsx_present_gate.h) the reference is per flip, and it re-bases only when a batch
was already held across a flip (safe with flip counters that run ahead of the drain);
PRESENT_ADAPT=0 is the old gate. Test libs/video/tests/test_present_gate.c (307 checks). In
game: v_02_city_intro_g presents at a median 30.0 fps, 9 draws a present, guest flips 30/s
(was 0.22 fps). Light gameplay views are no longer shown as merged frame pairs either.
**The 2.5 GB install re-copy on every boot.** The game's install stamp
/dev_hdd0/game/BCUS98125_INSTALL/USRDIR/ne.bv is read and written through libc and raw lv2
sys_fs (syscall 801), and sys_fs_translate_path flattened any "USRDIR/" path to
<root>/USRDIR (an old hack), a folder that does not exist: the write failed, so every boot
looked like the first. Now /dev_hdd0/<rest> resolves to the cellFs tree when the file or its
folder exists there (only with PS3_VFS_ROOT set and no truncation). Checked: the next boot
wrote the 63-byte stamp (byte-identical to RPCS3's), the one after copied 0 of 47 files.
Saves ~5 GB of disk I/O per boot; wall time to READY is unchanged (the copy hid inside the
loading screen). Known, not changed: sys_fs.h CELL_FS_O_CREAT/EXCL/APPEND values are wrong
(not on inFamous 2's paths).
**Glow white-out, ground truth.** RPCS3 running the same title uploads the same pyramid blur
constants: vertical passes sum to 2.0, horizontal to 1.0 (c[462..466].z, 22+22 sets). So the
2x vertical gain is the game's own, and halving it (VPC_NORM, an opt-in experiment now in the
backend) would be wrong; something upstream (the glow mask in scene alpha, likely raised by the
10-13 normal-map decode) must be brighter than on hardware. Still open; the white water by the
ship is reproduced in ~45 s with `-Step 11 -Script "30-520:128,0,128,128"`. New diagnostics:
RSX_CAP_ALPHA=1 (alpha planes of every capture), RSX_CAP_VPC=462-466:62949BCC (per-draw VP
constants). New tool: tools/rpcs3_probe/rpcs3_mem.py reads a running RPCS3's guest memory from
its process (guest base found from the EBOOT's text; 121 MB in 0.5 s, against ~100 KB/s and
a dropped connection through the GDB stub); `tconst_scan.py --proc --elf ... --range
30400000-37D00000` (RPCS3 puts the 121 MB heap at 0x30400000; ours is 0x40400000).
## 2026-10-14 (late night) — the debris / bridge freeze was a lifter bug; prologue step jumps work
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**The freeze after breaking things** (the gate, the falling bridge after the prologue boss, an
idle new game ~9.5 min in) was one PPU lifter bug, not an SPU or physics fault. The mesh contact
generator's prologue chunk 0x47618C spills side A's primitive (`std r4,0x448(r1)`); the body
func_004761F4, split off at the `mflr`, saves r31 and then loads it from that slot
(`std r31,0x5E8(r1); ld r31,0x448(r1)`). The callee-save snapshot pass took that load for a
tail-entry restore (the body has no `stdu`) and made it `_cs_31`; the paired epilogue restore
later turned `_cs_31` into a snapshot of the CALLER's r31 -- the physics manager 0x00A332C0. So
every PPU-fallback mesh contact (convex debris pairs) was built with the manager as a primitive
(shape NULL), a contact edge got a NULL body, and ~27 s later the unlink loop 0x690C5C spun on
NULL+8 forever. Fix (tools/ppu_lifter.py): registers that have a paired save/restore in the body
are found first and never get the tail-entry rewrite. It changes exactly this one function in
the whole EBOOT (test: tools/test_ppu_cs_restore_order.py). The morning's NOTES claim that the SPU
re-lift had fixed the gate hang was a coincidence.
**Checked in game:** `-Step 23` with forward held: the bridge collapses (Broken/Demolish x100),
the mesh path runs 16 times with side A correct, Cole falls, dies and retries twice, and play
goes on 70 s past the collapse -- no NULL reads in physics, no spin, exit 0. Diagnostics stay in
(src/infamous2_hooks.cpp, inserted by tools/ppu_postlift.py): IF2_PHYSFB=1 logs why bodies take
the PPU fallback (ring/status) and checks side A of each mesh contact ("ok" / MISLIFT);
IF2_PHYSFB_FORCE=1 sends every body down the fallback (stress; 128k fallbacks in New Marais).
**Prologue step jumps work** (`-Step <n>`): the watcher never saw the task graph because guest
pointers were checked against main memory only and the graph lives in the heap (0x40000000+,
up to g_sys_mem_bump_ptr). With that fixed, WarpToCheckpoint with the load path's flags 0x12
reset the world in place: the restart flag was never consumed, the mission was abandoned
(MissionAbandoned) and fell back to step 1. The default is now 0x13 (ReloadWorld too): the
flag is consumed 5 frames later and the step's own script runs -- step 11 starts on the pier
with "Head towards the disturbance", step 23 after the boss with "Return to the boat", READY
~35 s after launch (the bridge test above took 37 s to set up, against ~10 min of play).
Known: the warp's event runs with the game's current-context stack (0x9BF760) empty, so a few
handlers read/write NULL+0x0..0x84 once (dropped); the game's own retries afterwards are clean.
## 2026-10-14 (night) — save states M1: exact return addresses; guest stack slots verified
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Why.** True save states (snapshot anywhere, restore in a fresh process) are feasible with
conditions (study: a local design note; milestones M0-M7, ~7-9 engineer-weeks). A
restore rebuilds each PPU thread's call chain from the GUEST stack, so the guest stack must be
exact. M1 makes it so and measures it.
**Lifter (tools/ppu_lifter.py).** Indirect calls (bctrl/bcctrl/blrl and conditional forms) now
store the hardware return address in ctx->lr before ps3_indirect_call (8071 sites; callees used
to save a stale LR, so back chains named the wrong return site); `sc` records its return site in
ctx->sc_ra (286 sites; a uint32 in former tail padding, sizeof(ppu_context) stays 1104).
--legacy-indirect-lr / PPU_LIFT_LEGACY_INDIRECT_LR=1 gives the old output byte for byte.
**SLOTCHECK.** --slotcheck[=cs|all] / PPU_LIFT_SLOTCHECK emits a check after every call that the
saved LR, callee-save slots, back chain and r1 still hold what the lifted code expects (reporter
runtime/ppu/ppu_slotcheck.h, env SLOTCHECK_*). Measured on the `all` build, 7 min of scripted
New Marais play from a checkpoint: 2.2e9 checks over 8077 functions, 0 mismatches -- the guest
stack slots are trustworthy, the premise every resume design needs.
**Also:** the SPU SMC microstepper writes the ISA link value ({pc+4,0,0,0}) for brsl/bisl.
Tests: tools/test_ppu_indirect_lr.py, test_ppu_slotcheck.py, runtime/ppu/tests/test_slotcheck.cpp.
**Next:** M2 (stop-the-world census) and M3 (continuations), then P0 in-game self-resume.
**Benchmark note:** with a save present the title continues it by itself (and plays
v_02_city_intro_g.bik at ~0.2 fps for a minute) -- fps comparisons need an empty
PS3_SAVEDATA_DIR.

## 2026-10-14 (evening) — saves work; checkpoints boot straight into a save in 48 s
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Result.** The game's saves load and save like on a PS3, and a test run can start from a stored
save: `tools\run.ps1 -Checkpoint 03_forward_momentum_rpcs3` reached New Marais gameplay (READY)
48 s after launch, against ~3 min through the title flow. A save written by our runtime loads
back, and an RPCS3 autosave ("(Autosave) 3. Forward Momentum") loads unchanged.
**Saves (toolkit cellSaveData.c).** inFamous 2 uses only ListAutoLoad (boot), AutoSave2,
ListLoad2 and ListSave2, all from its "Game State Thread". Slot 0 is the autosave, 1-9 manual.
The prologue never autosaves by design (saving is enabled only in world "i2"). Fixed: a real
binary PARAM.SFO with RPCS3's keys/sizes (was a private text file that dropped a detail line);
save info handed back on load (title/detail/sizes, RPCS3's 35 KB overhead); list sorted as the
game asks (Continue = newest); recreate modes honoured; offset writes truncate; atomic saves
(built in .NAME.tmp, swapped in; a stranded .NAME.old is restored); menus with nothing to pick
return cancel. Test: libs/system/tests/test_savedata_flow.c. Env: PS3_SAVEDATA_DIR,
PS3_SAVEDATA_PICK=<dir>, PS3_SAVEDATA_READONLY=1.
**Checkpoints (project).** src/if2_checkpoint.cpp boots a run from checkpoints\<name> (or @new)
in a sandbox copy (the player's saves are never touched), skips the story FMVs, drives the title
flow from the game's own signals (tools/ppu_postlift.py hooks the signal dispatcher 0x2F26B8 --
109 named signals; IF2_SIGLOG prints the timeline) and reports READY in logs\run-*.ready with a
screenshot; -Script / -ExitAfter count frames after READY; -AutoSnap keeps every save the game
writes as checkpoints\auto-NNNN. tools/checkpoint.ps1: list/save/load/show/rename/delete/prune/
wait. checkpoints/ and gamedata/ are git-ignored (saves are user data). Fixed here: a new game
pressed CROSS only if nothing loaded after the menu activated, but the title menu streams its
background world afterwards -- now it waits for the packs to go quiet.
**Experimental: prologue step jump** (-Step <n>, src/if2_taskgraph.cpp; the prologue is one
mission, msn_beast_intro 0x459, 26 linear steps decoded from i2_task_graph.sprig.xpps). Not
working yet: the watcher never saw mission 0x459 active (fix in progress).
**Freeze data.** The new-game run left idle hit the debris-contact NULL reads ~9.5 min in (after
Damage/Broken signals); with the stack logging now on by default in run.ps1 it shows the bad
primitive is the STATIC object 0x00A332C0 (shape 0, owner 0x40000) reached through the PPU
fallback narrowphase (func_004796E0+0x680 -> 0x47D084 -> 0x477608 -> 0x4761F4 -> 0x473CBC).
**Boot-time findings (not fixed yet):** the HDD install re-copies ~2.5 GB of TV movies on every
boot (missing *.xpps files suspected); v_02_city_intro_g.bik runs at ~0.2 fps for ~60 s
(skipped in checkpoint runs).

## 2026-10-14 (afternoon) — true ultrawide (Hor+), resizable window, borderless fullscreen
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Result.** On the test PC's 2560x1080 monitor the game now opens at 64:27 with a genuinely wider
view: same vertical field of view, more on the sides, nothing stretched, HUD laid out for the
wide screen (window 1707x720, gameplay ~40 fps at ~7800 draws; menu 63, cutscene 36).
IF2_ASPECT=off gives exactly the old 16:9 path (window 1280x720, menu 63 / cutscene 38).
**How: the game's own anamorphic path.** Video init (0x13211C) stores a virtual width
W = 16H/9 at 0x96EF88 (4H/3 for 4:3 TVs) that is independent of the 1280x720 buffers; the
camera constructor derives its aspect from W/H and SetFov builds tanX = tanY*aspect (Hor+ by
construction); the UI canvas, safe area, reticle, world markers and SPU culling all follow.
src/infamous2_hooks.cpp writes W' = round(720*a) at install and again in a
cellGcmGetConfiguration override (its single call site runs right after video init, before any
camera exists), sets safe X = int(W'*0.075) (hud=wide) or the 16:9 margin (hud=16:9),
redirects the two pool slots that misuse W as a buffer width (0x93C67C, 0x944760 ->
render width 0xA009DC), and with lod=1 scales the four single-use screen-size thresholds
(0x93DE54, 0x945BE4, 0x949244, 0x9442CC) by k = W'/(16H/9) so objects keep their 16:9 detail
decisions (0x9491D8 is shared and left alone). Each write checks the v02.00 stock value first.
**Movies.** The FMV quad (0x2E7EDC) crops to fill when W > 1280; tools/ppu_postlift.py (run by
relift.sh after every PPU lift) inserts a call that rewrites the quad to the movie's own 16:9
inside the frame ("[if2-fmv] movie quad fitted: x -0.7499..0.7499"). IF2_FMV=fill keeps the crop.
**Presentation (rsx_d3d12_backend.c).** The 1280x720 swap chain is never resized: it lives in a
child "view" window placed on the largest rectangle of the image aspect inside the top-level
window, whose black brush paints the bars. Resize / maximise / DPI change only move the child,
on the window thread, with no GPU wait. Borderless fullscreen: F11 or Alt+Enter, or
fullscreen=1. Per-monitor DPI v2. Dragging the title bar no longer freezes the game: a timer
inside the modal move loop runs the frame clock's work. Settings: config/display.ini
(aspect, fullscreen, window, hud, lod, fmv, present) or IF2_ASPECT / DISPLAY_CFG env.
**Checked in game:** auto -> 64:27, every guest value peeked as expected; movie fit; 2 rounds of
the winstress driver (5 window shapes, minimise/restore, maximise, F11 from maximised, a modal
move loop) with correct letter/pillarboxing and the FPS title updating throughout, no hang.
The earlier uw1 menu hang (external SetWindowPos on the old single-window swap chain) did not
reproduce. **Known:** leaving fullscreen after starting with fullscreen=1 restores a window
16x39 px larger than before (cosmetic). Fps comparisons must use IF2_ASPECT=off now.

## 2026-10-14 (midday) — USB/Bluetooth controllers
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Result.** A native Windows controller layer (no new dependencies): libs/input/pad_host.h +
pad_host_win.c (device manager: SetupDi/HID enumeration, CM_Register_Notification hotplug,
one reader thread per HID device, XInput on its own 250 Hz thread, 7 stable ports, rumble/LED
writer, ExitProcess-safe shutdown) and drivers pad_hid_sony.c (DS3 raw/sixaxis.sys/DsHidMini
SDF+SXS+GPJ, DS4 v1/v2/adapter, DualSense/Edge; USB + Bluetooth incl. CRC, calibration,
motion in SIXAXIS units, pressure), pad_hid_switch.c (Switch Pro, USB+BT, SPI calibration,
HD rumble), pad_hid_generic.c (HidP usages, per-device map lines). cellPad.c reads all 7 ports
from snapshots, reports real pressure/sensor words, ASSIGN_CHANGES once per change, rumble via
cellPadSetActDirect; keyboard/mouse still merge on port 0. In game: a test DualSense is
found on port 0 (USB, firmware 6.30, calibrated, battery 100 %) and keyboard/mouse still drive
the tutorial on top of it. Tests: ~1600 checks across 8 test programs in libs/input/tests
(stub drivers, synthesized and 500 captured real DualSense reports, ASan hotplug stress).
Only the USB DualSense was tried on hardware (read-only); XInput, DS3/DS4, Switch Pro and
every Bluetooth path rely on synthesized tests.
**Behaviour to know:** a connected pad drives port 0 in every run, scripted ones included, and
inFamous 2 enables SENSOR_ON, so motion jitter reaches the game: benchmark runs must set
PAD_NO_HOST=1 (perfrun.sh now does). XInput deadzone is radial 24 % (was per-axis), 8 % for
the others ("deadzone = auto").

## 2026-10-14 (morning) — the camera "spike" and the hang after blasting the gate
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Result.** The diagonal garbage spike (screen centre -> top-right corner, visible while
moving or turning the camera) is gone, and the game no longer hangs ~27 s after the
fairground gate is blasted: played past the gate with keyboard/mouse, no NULL reads, still
running a minute later (it hung in 3 of 3 earlier runs that blasted the gate).
**The spike: immediate-mode vertex data.** The depth-of-field composite (fp 501D327C) is
drawn as 12 immediate-mode vertices (SET_VERTEX_DATA* between BEGIN/END, a 4-triangle fan
round the screen centre). Its per-vertex blur weight (attribute 8) is pushed with
SET_VERTEX_DATA1F_M (0x1E60). rsx_commands.c decoded only 4F/2F/2S/4UB/4S: 1F, 3F and
SCALED4S fell through as unknown methods, and rsx_imm_flush left every attribute the
primitive had not pushed on whatever vertex ARRAY the guest still had enabled, so the fan
read the previous mesh's texcoords as weights. Weights > 1 make the shader's unclamped
lerp overshoot into the saturated wedge. Fix (IMM_FIX, default 3; 0 = old): decode
1F/3F/SCALED4S, and during an immediate draw read un-pushed attributes from the register
(RPCS3/hardware behaviour, never the arrays). Confirmed in game with IMM_DIAG=501D327C:
pushed=0x0101, all a8 = 0, "verdict inputs clean". Test: libs/video/tests/test_rsx_imm.c.
Note: the VS cache evicts slot 0 and shifts every slot once 1024 programs are cached, so
rsxcap "vs=" numbers drift from VS_DUMP_ALL file names in long sessions.
**The hang.** The main thread spun in the guest's own list-unlink loop
(0x690C5C, ICF-merged with 0x668B68): a physics contact edge built with a NULL body after
the gate broke (debris contacts from a primitive with a NULL shape and a junk owner, first
seen as NULL reads in 0x473CBC; real hardware would have faulted there). Our VM reads the
NULL page as zeros, so it became a hang 27 s later. It no longer reproduces after closing
the SPU translation gaps below; the most likely link is the Edge animation job module
(77f8b0e2e976, every frame), whose conditional `bihze` interrupt-enable idiom left
0x15280/0x155EC untranslated -- they ran in the interpreter in 298 of 320 runs.
**SPU translation gaps closed** (re-lift; generated/ is not committed, run the lift):
- tools/lift_jobchain.py seeds every JobChain job/plugin with lift_jobmods'
  switch_table_targets/manual_link_targets: PC-relative switch tables whose table address
  is a link-time offset below the lift base were missed (jc_008a0900 +0xED4/+0xEE0/+0xF64/
  +0x1050 -- an audio job, hit in 142 runs -- and 14 cases in jc_008c3300/c4f80/c6f00).
- toolkit find_spu_functions.py collect_duff_targets: also the conditional biz/binz/
  bihz/bihnz form of `brsl .+4; ai; bi` (jobmod 77f8 +0x14D70/0x15280/0x155EC,
  spu_0000 0x3990). Test: tools/test_find_spu_functions_getter.py.
- spu_lifter.py always stores drain_ret_pc before a bisl (only matters for lifts without
  --regions; inFamous 2 output byte-identical). Test: runtime/spu/tests/
  test_spu_interp_unlifted_call.c.
**Diagnostics added:** PS3_NULL_READ_MAX / PS3_NULL_READ_STACK (bounded NULL-read
reports with guest stacks and register peeks; 16-bit loads now reported; the counter no
longer wraps -- the hang wrote a 650 MB log), RSX_CAP_FILE=<path> (arm RSX_CAP, VS_TRACE_ID
and VB_CLIP_ID when that file appears: capture the exact moment a glitch is on screen),
RSX_AALOG=1 (anti-aliasing control / surface AA modes -- the game uses neither),
DRAW_SKIP_FP accepts fragment-program hashes as well as addresses, IMM_DIAG=<fp|*>.
**Still open:** a blown-out white area on the water/floor near the ship at the pier start;
the 13 JobChain job starts entered with no position-independent slot (lr=0x2570, whole job
interpreted -- residency, not translation).

## 2026-10-13 (late night) — keyboard + mouse controls; the gate is blasted open by hand
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Result.** The game is playable with keyboard and mouse. Driven in game with SendInput
(`tools\kbm_inject.ps1`), every check passed: the mouse turns and tilts the camera, WASD
runs, Space jumps, RMB aims (the tutorial advances to "Press R1 to fire"), LMB fires a
bolt, and a bolt blasted open the fairground gate -- the first time play went past the
scripted route's end. Esc opens the pause menu within 0.4 s, the arrows move through it,
and Esc closes it. Gameplay ran at 43-49 fps (median per 30 s), with no change to the
scripted benchmark route.
**Design** (`libs/input/pad_kbm.h` header-only parser/filter; `cellPad.c`):
- Keys are read with GetAsyncKeyState only while a window of this process is in front.
  Every non-repeat WM_KEYDOWN and mouse-button press is also latched, so a tap shorter
  than the gap between pad reports (~16-70 ms) is not lost. A key held when the window
  gains focus, or during an Alt/Win chord, is ignored until released, so Alt+Tab back in
  does not press Tab = SELECT.
- Mouse: raw input (WM_INPUT) on a message-only window with its own thread, registered
  only while the mouse is captured, so camera motion does not wait for the FIFO thread's
  message pump. Counts accumulate with interlocked adds; at each pad report the speed
  over an exact 50 ms window becomes right-stick deflection (sensitivity x speed / 1500
  counts/s, anti-deadzone 0.20, unit-circle clamp). The total stick-time equals
  distance / 1500, however the reports fall.
- Capture on a click in the window (that click is not a game input): the cursor is hidden
  and clipped to the client area, re-clipped every 500 ms. Alt, F10, focus loss, close or
  destroy release it. SC_KEYMENU is swallowed (Alt/F10 alone would enter a modal menu
  loop on the FIFO thread and freeze the game).
- Merge: with an XInput controller, buttons are ORed and a stick the keyboard/mouse is
  moving overrides the controller's. Scripted input (PAD_SCRIPT/STICK(S)/FILE/AUTOPRESS,
  YDKJ) is applied on top, unchanged (400-report digest identical old vs new).
- Esc no longer closes the window; it is START. The close button and Alt+F4 still quit.
**Config.** `config\input.ini` (all lines commented, showing the defaults), found from
the working directory; `PAD_CONFIG=<file>|none`, `PAD_KBM="a=b|c=d"` overrides,
`PAD_NO_KEYBOARD`, `PAD_NO_MOUSE`. UTF-8 (with or without BOM) and UTF-16 files parse.
The table is printed once at startup as one write (it used to interleave mid-line with
other threads' output). Defaults follow the inFamous 2 PS3 manual's control page; the
table is in docs\CONTROLLERS.md.
**Not checked yet:** SELECT (Tab/M) showed no map in the prologue, which seems to have none;
mouse feel and sensitivity on real hands; L3/R3 functions (the manual lists none).
**Tests:** `libs/input/tests/test_pad_kbm.c` (737 checks: parser, key names, mouse filter
integral, encodings), plus the reviewer's host harness over the real cellPad.c with
faked Win32 calls (latching, focus blocking, swallow bits, merge, scripted input).

## 2026-10-13 (night) — characters render: the green heads were dropped pack/unpack shader ops
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Result.** Cole (and every character material) renders with the right colours: skin,
hair, the jacket, the collar (`screenshots\cole_fixed_49fps_2026-10-13.png`). Frame rate
unchanged (gameplay 35 / menu 67 / cutscene 42 fps).
**Cause.** rsx_fp_decompiler.c had no cases for the RSX pack/unpack opcodes PK4/UP4,
PK2/UP2, PKB/UPB, PK16/UP16, PKG/UPG: it emitted "TODO: unhandled FP opcode" and wrote
nothing. inFamous 2's character materials gamma-encode their albedo through them (PKB/UPG
sRGB -> linear, PKG/UPB back to 8-bit sRGB into H0, the albedo export), so H0 kept a mask
texel's .y -- pure green on the head, a stale red on the collar, near-black on the jacket.
The NOTES 2026-10-06 lead (FP constants shifted by one word) was a red herring: those
slots are read .xxxx only and our constant slots match RPCS3's byte for byte.
**Also fixed in the decoder:** TEX_bx2 (OPDEST bit 21: 2c-1 expansion of normal-map
texels; 160 of the title's 322 programs, so normal mapping was off everywhere), SRC2 abs,
DDX/DDY. All 322 programs now decompile with no unhandled opcode. Switches: `FP_PACK=0`,
`FP_BX2=0`; `[FPPACK]` lines list programs using them. Tests use a hand-assembled program
with the same shape (no game microcode in the tree).
**Texture mip levels (new).** Guest mip chains are uploaded (level count from the format
register, RSX level layout incl. LN pitch on every level and 128-byte cube alignment),
SRVs cover all levels, sampler min/max LOD and bias come from CONTROL0/FILTER
(`TEX_MIPS=0` = level 0 only, as before; `TEX_MIPDBG`, `TEX_MIPDUMP=<off>[:WxH]`).
**Not fixed: the fairground gate strip.** Its texture (1024x8 DXT1 at local 0x08151A00) is
sampled with NEAREST min/mag -- real hardware also reads only level 0 -- so the aliasing
theory of NOTES 2026-10-13 is wrong; the texture data or its decode is the next suspect
(`TEX_MIPDUMP=0x08151A00:1024x8` dumps it).
**Closing the window now quits** (close button or Esc): without a window nothing drains the
FIFO and the title froze invisibly until killed. `WINDOW_CLOSE_KEEP=1` keeps it running.

## 2026-10-13 (evening) — gameplay 25 -> 34-35 fps: triple buffering, texture/vertex work, SPU "windows"
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Result** (quiet machine, same route, the test PC's x86-64-v4 build; two runs): gameplay
median 34.0 / 35.4 fps (p10 19-24, p90 40-50), menu 65-66 fps, opening cutscene 42 fps
(from 25 / 39 / 32.5). The fairground gate (~9700 draws) runs at ~39-42 fps
(`screenshots\fairground_gate_39fps_2026-10-13.png`). No stalls. Render-thread changes
alone: 30 / 61 / 39; SPU windows on top: 34-35 / 65-66 / 42.
**Changes** (each verified adversarially; all compose cleanly):
1. Frame (`rsx_d3d12_backend.c`): 3 swap-chain buffers instead of 2. With 2, a readback
   flush shortly after a present waited for the display's vblank before the walker could
   go on, and everything behind its labels (the light-task kicker, the main thread)
   waited with it: walker-blocked time per flush+readback 5.35 -> 0.43 ms (mean) in a
   harness on the test PC's 74 Hz display.
2. Texture re-validation (`rsx_tex_cache.h`, `vp_upload_tex_slot`): tc_hash64 ~13x cheaper
   (4.7% -> ~0.4% of the render thread); cheaper per-binding layout/heap work.
3. Vertex emit (`rsx_vertex_fetch.c`): the time was memory stalls, not work -- prefetch the
   index span from vnew_draw (`rsx_vemit_prefetch`) and ahead in the dedupe/convert loops,
   plan only the attributes the shader reads, one-pass plan signatures, smaller loops;
   output byte-identical (`VEMIT_FAST=0` = old code, `VPREFETCH=0` = no prefetch).
4. SPU "speculative windows" (`spu_lifter.py` + `spu_helpers.h`, lifted job/jobchain code):
   straight runs of float ops run on the plain host fast path with one combined check of
   every lane at the end; a run whose check fails is replayed exactly through the
   existing helpers. plug_00821000 0x204A8 -25%, plug_00820400 0x202C0 -23% per iteration
   in the harness. Bit-exact (full differential harness, new tests
   runtime/spu/tests/test_spu_helpers_win.c and tools/test_spu_lifter_windows.py). Note:
   `SPU_WINDOWS=0` at run time is NOT a speed baseline (the merged loop runs slower);
   build with `-DSPU_WIN_ENABLE=0` for an A/B. Lift-time knobs: SPU_LIFT_WINDOWS,
   SPU_LIFT_WIN_MIN. Data that keeps failing a window is ~5% slower in those regions.

## 2026-10-13 (afternoon) — SPU round 3: exact SIMD float fallbacks, branch cache, constant shuffles, AVX-512 option
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Result** (quiet machine, same route): with the default x86-64-v3 build gameplay median
25 fps (p90 30), menu 37 (mean 40), opening cutscene 28 (from 23-25 / 37 / 26). With
`-DPS3RECOMP_SPU_HOST_ISA=x86-64-v4` (AVX-512; the test PC's build cache is set to it) gameplay
25 (p90 32.5), menu 39 (mean 44), cutscene 32.5. Gameplay's median no longer moves with
SPU speed alone -- profile it next. No stalls.
**Changes** (four areas, each verified adversarially; all 24 application orders give one
tree; re-lift with tools/lift_jobs.sh):
1. Float fallbacks (`spu_helpers.h`): the vectors the fast path rejects -- in game almost
   always one lane of NORMAL x exponent-255, i.e. a value times frest/fi of a zero lane --
   run a 4-lane AVX2 exact path (`spu_<op>_vx`: SPU singles decoded to double with exponent
   255 as a number, exact op, SPU truncation/saturation/flush) instead of the scalar loop:
   13-17 vs 39-57 ticks per call. Proof sketch in the header; new test
   tests/test_spu_helpers_vx.c (40M vector checks).
2. Computed-branch cache (spu_channels.c/spu_drain.c/spu_context.h, lifter): a 512-entry
   per-context (pc, residency generation, image) -> function cache in front of the resolver
   (`SPU_IBFAST=0` off, `SPU_IBFAST=check` compares every hit with the full resolve), and
   760 `ila;bid` interrupt-window sites in job modules hop inline (register write-back,
   then re-entry through the target's own case). Residency writers bump `res_gen`.
3. Constant shuffles (lifter + helpers): shufb with a control loaded from the image's
   constant pool uses a precomputed table form with a guard (`spu_shufb_k`, 3587 sites);
   a guard miss takes `spu_shufb_any` (`SPU_HELPERS_SLOW_HOOK("shufb_k")` -- should be ~0%
   in a profile; an all-miss region is ~25% slower). Byte rotates/shifts use tables.
4. x86-64-v4 option (CMake + runtime CPU check incl. ZMM state): 11-34% faster per hot
   job region than v3 in the harness, bit-exact against v3 (2.2M cross-ISA helper
   records, differential runs over the lifted code). Default stays x86-64-v3.

## 2026-10-13 — render thread + SPU register caching: gameplay 14 -> 23-25 fps (31 at the fairground gate)
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Result** (quiet machine, same route as 2026-10-07): gameplay median 14 -> 20 fps from
the render-thread work, -> 23-25 fps with SPU register caching; menu 25 -> 37 fps, opening
cutscene 18 -> 26 fps. The full pier (~8300 draws) renders at ~26 fps and the fairground gate (~8900 draws)
at ~31 fps (`screenshots\fairground_gate_31fps_2026-10-13.png`). The route now reaches the
next objective, "Blast open the gate". No stalls.

**What limited it after 2026-10-07:** the heavy-view frame is a serial chain (PPU -> SPU ->
RSX G-buffer -> labels -> SPU lighting -> RSX) and the render thread was on it:
- `vp_record_cb` called getenv("VP_NOFIXPROJ") and getenv("VP_FIXPROJ") for EVERY draw
  record (~1 us each walking the environment) and read c[0] back from the write-combined
  slot it had just written: ~29% of the render thread's wall time, ~20 ms of a 71 ms
  frame. (HOST_SAMPLE counts a sample as busy only when the innermost frame is in the exe,
  so time inside ucrtbase was invisible: the thread was ~64% busy, not 41%.)
- CPU blits (NV3089 per-texel vm_write32 with a coherence test per word) 2.8-6 ms/frame,
  readback conversion ~1 ms, per-draw shader/PSO lookups and fragment-program scans
  ~3 ms, the FIFO decode ~6 ms per 746k-word frame.
- Latency: a guest waiting on an RSX label was only noticed at the walker's next 4 ms tick.

**Changes** (each adversarially verified; the combination reviewed in 3 dimensions --
dispatch/merge, renderer combination, codegen -- with no defects in the first two):
1. Constants (`rsx_d3d12_backend.c`): env switches read once; no read-back; a record whose
   vertex-constant bank and epilogue are unchanged reuses the previous record's slot
   (`rsx_state.vertex_constants_gen`, bumped only when stored bits change; `CB_REUSE=0`,
   `CB_REUSE_VERIFY=1` compares a CPU shadow before every reuse; `[CBREUSE]` with PERF=1).
2. Blits (`cellGcmSys.c`): NV3089/NV309E/NV308A as row copies with one coherence check per
   range (`NV309E_BLIT=0`), SIMD readback conversion: blits 16-18x faster.
3. Draw path: memoized vp_get_vs / vp_get_fp_pso (index), fragment-program scan memo,
   faster vertex emit/hash (`VHASH_AVX2=0`), fewer null-descriptor copies.
4. FIFO decode: a tight walker loop and a switch dispatch in rsx_process_method
   (`rsx_transform_constant_body` shared with the packet loop; every vertex-constant
   writer bumps the generation), and a FIFO "watch" that wakes the frame-clock walker as
   soon as a guest waits on put or a label (`GCM_FIFO_WATCH=0` restores the old waits;
   label hand-off 8 ms -> 0.33 ms per round trip in a harness).
5. SPU register caching (`tools/spu_lifter.py --reg-cache`, on for spu_jobs and
   spu_jobchain via `SPU_LIFT_RCACHE=1` in tools/lift_jobs.sh / lift_jobchain.py; forced
   off for spu_pm, generated/spu and generated/prx): SPU registers live in C locals within
   a region and are written back wherever the runtime can see ctx->gpr (exits, channel
   ops, nested calls, stops; r0 written through); local-store access through one gate
   (`spu_ls_gate`) instead of three probe tests per access. Bit-exact in 450k
   differential cases incl. interrupts mid-region and LS-watch/WWS probes. Cost: +49%
   .text for those 62 TUs. Diagnostic caveat: `SPU_LS_LOWREAD` and the STALL_DUMP register
   snapshot show r1-r4 as of the last sync point inside cached loops.
   **Re-lifting:** after any change to spu_lifter.py or spu_helpers.h also re-run
   `tools/lift_jobs.sh` (spu_jobs + spu_jobchain register-cached, spu_pm not), not only
   relift.sh / lift_libsre.sh.
6. `FIFO_IDLE_MS` (src/main.cpp, default 4; 1 ms measured slower).

**Measuring:** other benchmarks running on the same machine moved the same binary's
menu median between 18 and 35 fps; only quiet-machine runs are comparable.

**Open:** a diagonal strip of garbled black/white/colour blocks across the fairground
gate (caution tape?) -- the same with VCOMPACT=0 and CB_REUSE=0 and with or without
register caching; no build before these changes reached the gate, so whether it is new is
unknown. rsxcap at the gate (`RSX_CAP=1:8:50`, `RSX_CAP_AFTER_MS=398000`) puts it in a
G-buffer draw (batch 1, fp 0x00AC2B02 / fphash 4E45701E, 6 vertices) whose t0 is a
1024x8 DXT1 texture at local 0x08151A00. Likely cause: the D3D12 backend creates every
guest texture with MipLevels = 1 and never uploads the guest's mip chain, so a long thin
high-contrast texture minified ~100x aliases into noise (distant detail everywhere
shimmers for the same reason). Next step: upload mip levels (rsx_texture_mip_chain
already computes RSX's level offsets) and compare the strip.

**Next:** the 4 SPU threads are ~80% busy again; the main thread waits ~27% of gameplay on
the SPU Light Task Kicker (was ~50%). Biggest SPU items: plug_00821000 region 0x204A8
(10%), plug_00820400 0x202C0 (5%), jobmod c6e7 0x7448/0x7170 (~8%), the float slow paths
(spu_fm_slow + spu_fnms_slow ~2.4% self: frest(0) = 0x7FFFFFFF lanes -- a SIMD exact slow
path), spu_indirect_branch + spu_lookup ~3.8%, and an AVX-512 (x86-64-v4) build of the
lifted code (measured -9..-26% on the hot regions with 32 registers; needs a bit-exactness
pass and a CPU check).

## 2026-10-07 — frame rate ~1 -> 14 fps in the first area (30+ in light views); the post-intro freeze fixed
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Result.** Same scripted route as before (re-timed, see "How measured"): gameplay on the
pier from a median of 0.93 fps (p10 0.86, p90 1.09) to 14.0 fps (p90 30.6); the menu
fly-through ~1.5 -> 25 fps; the opening cutscene ~1 -> 18 fps. The frame rate now depends
on the view: the full pier (~8200 draws) ~14 fps, looking out over the harbour (~2400
draws) 30-32 fps (`screenshots\pier_14fps_2026-10-07.png`, `harbor_32fps_2026-10-07.png`).
The freeze ~13 s after the intro montage (Zeke and Cole) is gone; the lifted DA7ABA5E
plugins are now the default. Rendering unchanged (screenshots compared). The route now
reaches the "Hold L1 to aim" prompt.

**Where the time went** (HOST_SAMPLE_ALL, all threads):
- Before: SPU host threads ~78% busy, ~45% of that in the SPU float/shuffle helpers,
  which were real calls (u128 through memory, a CRT fmaf per lane, a 16-byte shufb loop:
  the lifted TUs had no SSE4/AVX flags); the default config also interpreted the
  DA7ABA5E plugins (3.6-7.5 s of interpreter time per 2 s). PPU main thread: ~81% of its
  busy time in the guest memory accessors' debug hooks. Renderer: ~7000 texture
  bindings/frame through a linear 1024-slot scan + sparse checksum + eviction cascade,
  ~1.7M vertices/frame expanded to 256-byte records.
- Now, heavy pier view: no thread saturated (4 SPU threads ~47%, render thread ~41%,
  PPU main ~14%) and the busy times add up to the frame: the frame is a serial chain
  (PPU -> SPU jobs -> RSX G-buffer -> labels -> SPU lighting -> RSX). PS3_POLLTOP: the
  main thread sleeps ~50% of gameplay at lr 0x1DB7DC waiting for the SPU Light Task
  Kicker (state +0x318 == 4), which itself sleeps ~85% polling RSX labels.

**Changes** (each A/B-measured in game and reviewed adversarially before merging):
1. `runtime/spu/spu_helpers.h`: always-inline SIMD fast paths for every hot helper --
   fa/fs/fm/fma/fms/fnms (one SIMD op + an integer lane check; a lane with exponent 255,
   overflow or a special case goes to the unchanged out-of-line reference), shufb (3
   pshufb + blend), the byte rotate/shift families, conversions, compares, cntb, frest/
   frsqest (gather), gb*/rot*/mpy*/clz/sumb/xs*. SPU host threads run round-toward-zero +
   DAZ + FTZ (`PS3RECOMP_SPU_FP_RZDAZ`, default on with clang/gcc): exactly the SPU's
   single precision, so denormal handling moves into hardware; DP helpers clear DAZ/FTZ
   around themselves; `spu_host_fp_enter/leave` bracket every lifted run (also HLE jobs
   on PPU threads, whose float results now truncate like an SPU's). Empty-asm barriers
   (`SPU__OPAQUE`, `SPU__RZ_OPAQUE`) stop clang from constant-folding SPU float ops with
   IEEE rounding/denormals. Bit-exact: the fxisa ISA harness byte-identical in all MXCSR
   modes at AVX2/SSE4.1/SSE2; ~3.1 billion differential vector checks; exhaustive 2^32
   sweeps of the integer helpers; a constant-operand test (tests/
   test_spu_helpers_constfold.c) that the first RZDAZ draft failed on 12-20k of 1.43M
   cases and the merged header passes with 0. CMake `PS3RECOMP_SPU_HOST_ISA` (default
   x86-64-v3 = AVX2/FMA3/BMI2; `x86-64-v2`, `baseline`); a port applies it with
   `ps3recomp_apply_spu_isa(<lifted SPU sources>)`, which also gives spu_drain.c the
   matching CPU check (exits with a message instead of an illegal-instruction fault;
   checked before every entry into lifted code) and FP mode. /GS- and /Zc:tlsGuards-
   on the lifted TUs. Hottest lifted loop (jobmod c6e7 region 0x9048): 2694 -> 267 TSC
   ticks per iteration; plug_00821000 region 0x204A8 a further -42..-65% from RZDAZ.
2. SPU runtime bookkeeping (~11% of SPU-thread time -> ~1.3%): inline drain-call fast
   path (the depth-guard ring was one cache line bounced between 4 cores on every guest
   call: 55 -> 2.5 ns), overlay-note pre-filters (73 -> 8 ns per GET), a per-thread
   spu_lookup cache, gated dispatch diagnostics.
3. MFC/channel fast paths (~2x per DMA/channel op): one gate for ~30 per-DMA debug
   checks, small frames, byte-wise coherence tests, cache-line isolation of hot globals.
4. PPU memory accessors (`ppu_loader.cpp`): ~20-instruction fast path, diagnostics gated
   once (`vm_diag_init`), reserved-line bitmap inline (`spu_coh_bitmap.h`); 3-5 ns -> ~1 ns
   per guest load/store.
5. SPU writes break PPU lwarx reservations line-wide (PUTLLC/PUTLLUC/DMA PUT ->
   `spu_coh_break_ppu`), stwcx./stdcx. re-check under the lock-line lock, atomic SPURS
   bitset updates, the PPU lifter emits `PPU_RESERVE` (locked exchange) before the lwarx
   load (`PPU_RESV_SPU_BREAK=0`, `PPU_STWCX_DELAY_US`, `RESV_DIAG`). A real race (libsre's
   LFQueue push compares one word while the task's pop registers through PUTLLC on the same
   line) -- but not the freeze. NOTE for an existing generated/ tree: re-lift, or apply the
   same rewrite to generated/ppu and generated/prx/libsre_ns (96 + 150 sites).
6. **The freeze** (`cellGcmSys.c`): the FIFO walker's park-grace timer (`s_park_off/t0`)
   was cleared only by a resync, never when the title patched its self-jump and the
   walker moved on. inFamous 2 parks at the same side-buffer slot (IO 0x14BC0) frame after
   frame, so 3 s after the first park any fresh park there was resynced on sight, dropping
   label 0x850; the Light Task Kicker then waited forever and main spun on 0xA25A18. Found
   by three independent analyses of the logs (every resync came 0.01-0.8 s after a
   normally presented frame, not after a 3 s stall). Now cleared whenever `get` leaves the
   park; `GCM_PARKLOG=1` prints each park's duration (max 16 ms in game). This was also the
   "SPURS stall" of 2026-10-05 item 7 / 2026-10-06 -- the lifted plugins only changed timing.
7. Renderer: texture cache keyed by (offset, size, format, cube, levels) with OS write
   tracking of the guest arena (`MEM_WRITE_WATCH` in src/main.cpp, `VM_WRITE_WATCH=0`),
   LRU over 2048 slots, one upload ring, prebuilt SRVs, 16x faster swizzle decode
   (`[TEXCACHE]` with PERF=1); VCOMPACT vertex path (only the attributes the VS reads,
   from its DXBC input signature; per-draw fetch plans; dedupe + indexed draws; a cross-
   frame vertex cache, `VCACHE_MB`, `VCACHE_AGE`; `VCOMPACT=0` = old path, `VFETCH_CHECK=1`
   compares against it; `[VPERF]`); an on-disk DXBC cache for guest shaders
   (`build\shadercache`, `SHADER_CACHE=0`, `SHADER_CACHE_DIR`): a scene cut's ~300 new
   pipelines cost 2.3 s cold, 0.08 s warm. Fixed on the way: DXT3/5 cube-face stride
   (8-byte blocks assumed).
8. `src/main.cpp`: the frame clock waits on the FIFO kick event (was Sleep(4));
   `FIFO_IDLE_MS` (default 4; 1 measured slower). Lifted DA7ABA5E plugins default on
   (`SPU_LIFTED_PLUGINS=0` = interpreter; tools/lift_jobchain.py).
9. Diagnostics: `FPS_LOG=1` ([fps] once a second), `PRESENT_SYNC=<n>`, `STALL_DUMP=<hex
   ea>` (dump only on a spin at that address; small values still mean "on"), the [stall]
   dump lists tasks parked in WAIT_SIGNAL with their signal bits and wait object, the 10 s
   WAIT_SIGNAL dump fires even when other signals keep waking the wait.

**Ruled out** (A/B, no change or worse): vsync on present (`PRESENT_SYNC=0`), log volume
(`PS3_VERBOSE=0`), the FIFO kick wait alone (0.93 -> 0.97), a 1 ms FIFO idle tick (menu
25 -> 20 fps).

**Review.** A 4-dimension adversarial review of the whole delta (concurrency, renderer,
build/defaults, diagnostics/regressions) found 12 real issues, all fixed: a VCOMPACT
stream-path overflow when dedupe collisions re-add vertices, the DXT3/5 cube stride, the
coherency unit test no longer linking, SPURS bitset updates bypassing the HLE write-watch,
VCOMPACT dropping draws whose fragment program failed (now the fallback PS draws them),
the ISA check's scope/race/CMake coupling, STALL_DUMP parsing, stale docs.

**How measured.** a local `perfrun.sh TAG SECS` = runphase + `FPS_LOG=1` + `fpsreport.py`
(medians: menu 30-105 s, cutscene 115-180 s, gameplay t >= 300 s; hang check). Route,
re-timed for the faster game: `MOVIE_SKIP=v_00_intro_montage;PAD_SCRIPT=110:0x4000;
PAD_STICKS=195-198:128,128,128,0|198-201:128,128,128,255|201-204:128,128,0,128|204-207:128,128,255,128|207-225:128,0,128,128;
PAD_STICKS_REPEAT=195,30`. The gameplay median depends on where the camera points
(30 fps over the water, 14 on the pier): compare menu/cutscene medians for A/B, and do
not benchmark while other heavy processes run (the game needs ~8 cores).

**Next.** The heavy-view frame is a serial chain, so both SPU code and render-thread time
count: (a) lifted-SPU codegen -- keep SPU registers in C locals within a region, fold the
LS access checks (in progress); (b) render thread -- reuse unchanged 8 KB vertex-constant
slots (vp_record_cb copies them per draw), CPU blits (nv3089), FIFO decode, per-draw
lookups; kick the walker on PUT/label waits (the kicker chain above).

## 2026-10-06 (17:30) — the player stays on the pier and walks to the first objective: SPU floats are not IEEE
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Result.** After the PIER 13 tutorial Cole stands on the deck (z = 12.2498, steady), is
drawn (he was invisible before), gets "Head towards the disturbance" and walks/runs ~25 m
along the pier toward the fairground arch with the scripted stick input
(`screenshots\pier_tutorial_cole_2026-10-06.png`, `first_objective_walk_2026-10-06.png`,
`pier_running_2026-10-06.png`; ~0.3-1.6 fps).

**What it was.** The SPU's single-precision format has no infinity or NaN: exponent 255 is
an ordinary exponent (magnitudes up to 0x7FFFFFFF ~ 6.8e38), denormal operands are zero,
results saturate and truncate. Every SPU float helper (runtime/spu/spu_helpers.h, used by
both the lifted code and the interpreter) computed in host IEEE floats, so the SPU FLT_MAX
0x7FFFFFFF -- which the title's SPU code builds 68 times (and IEEE's 0x7F7FFFFF never) as
"no hit yet" / bounding-box seed -- and frest(0) (= 1/0 for an axis-parallel ray) were
NaNs: every compare against them false, every product NaN. A straight-down ground probe
has dir.x = dir.z = 0, so its slab test got NaN reciprocals; the closest-hit loops
(jobmod 4ccdc05864ab at LS 0x6FA8, jobmod 512689a33f01 at LS 0xC378: query, then
`fcgt $r34,$r82,$r25` against the 0x7FFFFFFF seed) never recorded a hit. The world had
no floor: the player landed, sank ~25 cm/s of game time, and fell once the probe started
below the planks. The per-character visibility boxes were NaN too, so characters were
culled from the main view (their shadows, drawn without that test, still showed).
Now: fceq/fcgt/fcmeq/fcmgt order SPU values exactly (sign-magnitude on the bits,
denormals = 0); fa/fs/fm/fma/fms/fnms keep the host fast path for normal operands and
results and redo a lane that touches exponent 255, the denormal range or an IEEE FLT_MAX
result in double with SPU saturation/flush/truncation; cflts/cfltu/fesd/fi decode
exponent-255 inputs as numbers; zero results are +0; csflt/cuflt truncate; fesd stays
IEEE (ISA 9.2.1). SPU host threads now run with a round-toward-zero MXCSR by default
(`SPU_FPMODE`, `ieee` = the host's round-to-nearest; `ftz`/`rzftz` add DAZ/FTZ): with it
the helpers are bit-exact against an exact ISA model on 3M vectors (fa/fs/fm/fma/fms/fnms,
compares, conversions, fi, the frest/frsqest+fi sequences); with round-to-nearest ~1/3 of
fast-path results were an ulp high and the reciprocal sequence overshot 1/x. An
independent audit also reproduced the old failure (a vertical ray against a box).

**How it was found** (each step ruled something in or out):
1. `SPU_JOB_VERIFY` -- a differential verifier for the WWS job modules (images 200-226):
   the real lifted job runs under a recording sandbox (every channel op, DMA element and
   bytes, atomic, PM interrupt delta); a clone is then interpreted from the same entry
   state replaying that journal; first divergence, end registers, LS and PUTs are
   compared. `SPU_JOB_VERIFY=<ids|lo-hi>[:N]`, `_AFTER=<s>`, `_EVERY=<k>`,
   `_JOURNAL=<dir>` (binary journal per call; scratch jnl.py reads it), `_NOCLONE=1`,
   `_LINK`, `_STEPS`, `_MS`, `_MAXMB`, `_LSX`, `_STRICT`, `_DUMP`, `_SELFTEST`. Interrupts
   are replayed at the real resume pc (srr0), not the op boundary (that gave false
   divergences). In gameplay 65/66 sampled calls matched exactly.
2. It found the interpreter's `ilh` splatting words instead of halfwords (every
   interpreted shufb mask built with ilh was garbage). A per-instruction harness (every
   SPU word the title ships, 208k words x 256 states, lifted vs interpreted) then found
   mpyhha/mpyhhau writing the RRR destination (r70/r78), rchcnt splatting the count,
   mfspr/fscrrd leaving RT unchanged, bisled missing -- all fixed in spu_interp.c. Both
   sides share the float helpers, so neither tool could see the real bug.
3. Cole's draws: the main view had no draw reading Edge's skinned buffers except a carried
   prop; the shadow pass had the whole character. `VS_TRACE_ID` + tools/vp_eval.py and
   `c[460]` (inFamous 2's negated eye position) put the camera 2.4 m above, 1 m behind
   Cole -- a normal camera. Edge's outputs (journal) showed Cole's body in world space
   falling straight down with the camera: the player fell, not the camera.
4. A/B runs that did NOT change it: JobChain images interpreted (`SPU_INTERP_IMG=130-159`),
   SPU rounding/denormals (`SPU_FPMODE=rzftz`), guest time dilated from before landing
   (`GUEST_TIME_SCALE=0.05`, `_AFTER=215`: he still sank, slowly -- no timestep issue), a
   1.2 s sleep per flip (`FLIP_SLEEP_MS`).
5. `MEMSCAN=<x>,<y>,<tol>,<t1>,<t2>[,<period>]` found the position's copies in guest memory
   (a .bss snapshot at 0x00A33470, written by func_002846D0 via memcpy);
   `PPU_GUARD_EA` + `PPU_GUARD_AT_MS` (line mode) gave the z history. The verifier's one
   mismatch -- jobs leaving 0x7FF50000-class values in LS -- pointed at the float format.

**PPU lifter fixes** (independent ISA-model audit of every FP/VMX/integer word in the
EBOOT, 60k words, local work files): `vexptefp`/`vlogefp` had no lowering (33 sites, the
audio mixer's dB <-> gain: vD stayed stale) -- now exp2f/log2f with NJ flush;
`vsl`/`vsr`/`vslo`/`vsro` were undecoded and lifted to nothing (a 128-bit bitset's
masks) -- now decoded and lowered. Found but left: vmaddfp/vnmsubfp are not fused (1 ulp;
fmaf per lane at ~23k sites needs -mfma first), VMX ignores NJ (denormals), frsp/lfs/stfs
NaN payloads, fsqrt of a negative, no FPSCR status bits -- none reachable in normal play.

**Reviewed adversarially** (5 lenses over the day's diff, every finding verified; plus
ground-truth audits of the SPU helpers and the MFC engine, and of the PPU lifter): all
fixed except the noted perf item -- -0 zero results, a 1-ulp slow-path double rounding
(now exact truncation via TwoSum), +-Smin from the fast path, fesd, csflt/cuflt; the
verifier applying an interrupt one instruction early, g_spu_interp_irq_break surviving a
longjmp, jv_init racing, a silent journal overflow; the GUEST_FRAME_MS experiment
(ring-wrap and race bugs) dropped; diagnostics' output formats. Open: the per-lane SPU
float checks cost float-heavy lifted SPU code ~2-4x on those ops (an SSE fast path is the
next step); the MFC deviations found (event producers, fences, LS wrap, RdEventStat
masking...) cannot change computed data and are listed in local work files.

**Other fixes:** tools/setup.ps1 and setup.sh also `git clean -fd` the toolkit before
applying the patch (a file the patch adds survived the reset and made `git apply`
refuse it); the SPU decrementer wrapped every 231 s (u64 overflow, split divide);
sys_time_get_current_time returns the Unix epoch; SPU_INTERP_UNLIFTED stops at the
bracket's return pc; sys_time_get_timezone (syscall 144) registered (KNOWN ISSUE: writing
any timezone value hangs boot at the splash -- default writes nothing, `TZ_WRITE=1`).

**Diagnostics added:** the verifier above; `SPU_JOBHIST=1`; `SPU_INTERP_IMG=<ids>`;
`GUEST_TIME_SCALE` (+`_AFTER`); `FLIP_SLEEP_MS` (+`_AFTER_MS`);
`MEMSCAN`; `PPU_GUARD_AT_MS` line mode (+`PPU_GUARD_MAX`; prints the 32-byte line after
each write and the writer's host RIP -- scratch rva2sym.py names it from build\infamous2.map);
`PPU_WWATCH_AFTER_MS`; `CAM_TRACE=<frames>`; `VS_TRACE_ID=<ids>` (+`_AFTER_MS`, vp_eval
trace format); `VPOS_STATS=<fp addrs>` (indexed draws' attribute-0 bbox/NaN counts);
`DEPTH_OFF_FP=<ids>`; `FP_RED` takes a list; `LWC_LONG=1`; manifest `va0=`;
`RSX_CAP_OPSNAP` up to 256.

**Still open:** characters' heads render solid green (Cole's head draw: program 6004A381,
2283 skinned vertices, G-buffer pass; the albedo target is already green, hands are
fine). Lead: its FP constants k1/k10/k18 hold the same run of material values
(...0.2810, 0.2727, 0.8000, 0.5526, 0.0500...) each shifted by one word, and k1 =
(0.27, 0.80, 0.55) is that teal-green -- per-material constant patching into the FP looks
misaligned. Also: the menu sky is still black; the intermittent SPURS stall (PPU spins on
0xA25A18, one SPU in the taskset PM at 0xA70 with an Lr event pending, the other kernels
parked) hit 2 of ~25 runs; ~0.3-1.6 fps in the pier area.

## 2026-10-06 (08:20) — guest samplers: the blocky depth-of-field blur
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
The boat interior and the tutorial's out-of-focus areas were blocks ~40 px wide. The
composite (E933D344) reads its 320x180 depth-of-field blur on texture unit 0 with
TEXTURE_FILTER min/mag LINEAR -- and every guest fragment program on the D3D12 path sampled
through 16 fixed static samplers: s0 point/clamp (kept for the dbgfont atlas PS), s1-s15
linear/wrap, whatever the title asked for. So unit 0 was point-sampled everywhere (the
blocks; a build with s0 linear made them smooth), clamped render-target reads
wrapped at the edges, and the G-buffer's NEAREST reads were filtered.
Now the VP root signature has a sampler table (s0-s15) instead of static samplers, and each
guest-FP draw binds 16 samplers built from its units' TEXTURE_FILTER (min 16-23: NEAREST /
LINEAR / the four MIPMAP modes, no-mip modes clamp to level 0; mag 24-27) and
TEXTURE_ADDRESS (s/t/r in bits 0-3 / 8-11 / 16-19: WRAP, MIRROR, CLAMP(_TO_EDGE), BORDER,
MIRROR_ONCE*). Tables are cached by content in one shader-visible heap (128 sets of 16,
restarted after the GPU wait at the start of every render_frame; `[SAMP]` says if a batch
ever runs out); set 0 is the old fixed set, still used by draws without a guest
FP. `GUEST_SAMPLERS=0` binds set 0 everywhere. Not modelled yet: LOD bias, anisotropy,
the border colour (transparent black), depth-compare.
Reviewed adversarially (2 lenses): one finding (the cache restarted only when nearly full,
so a batch could start with 16 free sets), fixed.

## 2026-10-06 (07:40) — the black menu sky, traced; RSX user clip planes
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001
**Menu sky (still black -- cause found, not fixed).** The title screen's sky is drawn
in the G-buffer pass: two cloud layers (CC0050DA x2, depth ~0.99986; 9379ADDD, ~0.9997)
write their colour straight into the scene buffer 0x11C0000 as MRT plane D, and a
full-screen dome (91FD0398, z = w: depth 1.0, LEQUAL) fills the rest in the forward pass.
All three fail the depth test because one draw before them, program A930EE20 with 30
vertices, covers the whole screen at window depth ~0.9985. Its vertices are a building box
(walls and roof, no floor: (+-0.8, +-0.766, -1..1)), and with the matrix it is drawn with
(c259..c262) the menu camera sits INSIDE it (camera at box-space (-0.39, -0.07, -0.99));
its x = +0.8 wall lies entirely in front of the camera, faces it and spans the screen.
Every GPU rule draws that wall -- culling (BACK, CCW) and winding were checked (the global
convention is right: `CULL_FLIP=1` blacks out the whole menu), the matrix is not mirrored,
and RPCS3's vertex epilogue does nothing special for w <= 0 -- so on the PS3 this draw is
not issued with these inputs: the menu camera, or the LOD/visibility choice that selects
this proxy box, differs upstream (game or SPU side; the SPU job that reads the depth copies
already returns all zeros). `VB_SKIP_WNEG=A930EE20` (experiment: drop that program's draws
with a vertex behind the camera) shows the intended screen -- stormy clouds, the city
skyline, lightning, the menu (`screenshots/menu_box_skipped_2026-10-06.png`). Not a fix.

**User clip planes (new).** SET_USER_CLIP_PLANE_CONTROL (0x1478) was not decoded at all;
the title toggles plane 0 (mode 2, GE) around its planar water-reflection pass (target
0x0F6F0000, vertex programs writing o[5].y = dot(pos, c467)). Now the decompiler (opt-in
flag `rsx_vp_clip_distances`, set by the D3D12 backend) emits SV_ClipDistance0/1 from
o[5].yzw / o[6].yzw with per-draw factor/bias in b0 (`vp_uc[4]` after vp_posoffset):
GE keeps d >= 0, LT keeps d < 0, disabled keeps everything (RPCS3's mapping).
`USER_CLIP=0` restores the old behaviour. Checked at the PIER 13 tutorial (reflection
target snapshots, clip off vs on): off, the pier pilings' underwater parts rose through
the reflection as two tall dark posts; on, they stop at the water line. (The menu's
reflection pass renders nothing visible either way.) Reviewed adversarially (3 lenses,
every finding verified): a disabled plane now goes through the legacy multiply (factor 0
gives exactly its bias even when the VP left INF/NaN in that lane), and unwritten clip
lanes start at 0 rather than the output default w = 1 (which would have clipped whole
draws under an LT plane 2 or 5).

**Diagnostics added:** `VB_CLIP_ID=<fp id>[:<base>]` (each draw's NDC extent from the
uploaded vertices, at record time and again at replay from exactly the vertex range and
constant slot the GPU gets, plus the matrix and vertices of the first wild ones),
`VB_SKIP_WNEG`, `FP_VALVIS` with one expression per MRT target (`~`-separated, t0..t3),
`RSX_CAP_OPSNAP` also snapshots an MRT plane, `CONSTDBG_FULL=1` (c456..c467),
`VTX_CLIP_ID` prints every enabled attribute with its array format, the manifest prints
each draw's cull key, `QUERY_LOG=<n>` (occlusion-query / conditional-render / user-clip
methods: the title uses none of the first two in the menu).

## 2026-10-06 (06:00) — depth model, display buffers as surfaces, texel decode: the opening plays right
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | verified by run gpg (400 s)
Where it was: in game the G-buffer held the world but the lit scene showed the inside of a
sky box; long white / pink / red / flat orange-olive stretches through the boat and Beast
cutscenes and a red wash over the tutorial; the menu looked right only because every depth
test ran against a freshly cleared buffer.
Where it is: the boat cutscene (stormy purple sky, sea, boat), the Beast attacking the city,
the boat interior and the PIER 13 tutorial render with their real lighting and colours --
`screenshots/boat_cutscene_2026-10-06.png`, `beast_cutscene_2026-10-06.png`,
`boat_interior_2026-10-06.png`, `pier_tutorial_2026-10-06.png` (earlier in the day:
`tutorial_pier_2026-10-06.png`, `tutorial_lightning_2026-10-06.png`). ~0.8-2 fps.

Root causes, in the order found (all D3D12 backend / RSX decoder / GCM walker, patch 0001):
1. **FP constants baked into cached shaders** (the 21:50 review's first defect). The D3D12
   path decompiled each fragment program with its inline constants as literals but keyed
   the PSO on code only, so a program re-patched in place kept its first-seen constants,
   and a program re-uploaded to a new address (the composite, 67 addresses a run) was
   recompiled. Now programs are scanned at record time (`rsx_fp_scan_program`: size,
   structural hash, constants, sampled-unit mask), decompiled once per structure with
   `rsx_fp_decompile_buffered_ex`, and each draw's constants go to b1
   (`PSConstants { fp_constants[]; fp_alpha; fp_texctl; }`) from a per-parity linear ring
   (`[FPCB]`, 2 x 8 MB). PS cache keyed (structure, variant, address only for debug
   rewrites); PSO cache adds VS, blend, targets, formats, write mask, cull; the prepass's
   PSO choice is reused by the replay; evicted PSOs are released after the GPU wait.
   97 programs through the opening, each compiled once.
2. **Alpha test and cube maps never applied on the D3D12 path.** Alpha test is now a
   shader variant whose func/ref live in `fp_alpha` (ref decoded per surface format; 8-bit
   targets compare integers 0..255 like the hardware; `NO_ALPHATEST=1` off). Units bound
   with a cube view and sampled by the program declare TextureCube (the variant key holds
   the mask, from the views actually bound).
3. **Depth.** One shared depth buffer, cleared at every render-target switch, and every VP
   pipeline forced depth test + write LESS_EQUAL. RSX depth is a surface at
   SET_SURFACE_ZETA_OFFSET that lasts until CLEAR_SURFACE clears it; depth/stencil state is
   per draw. Now (`DEPTH_MODEL`, default on; `=0` restores the old rule): one D24S8 surface
   per zeta offset (24 slots, grown/evicted, deferred release), bound with each op's
   targets; per-draw depth test/write/func, stencil (front, two-sided, read and write
   masks, ref), polygon offset, depth range (CLIP_MIN/MAX), depth clip (ZMIN_MAX_CONTROL),
   depth bounds. `DS_STENCIL=0`, `DEPTH_CLIP=0`, `DEPTH_BOUNDS=0`.
4. **CLEAR_SURFACE ignored its mask**: every offscreen clear cleared colour, and nearly all
   of the title's clears are Z/S only (0x3). Now Z/S clear the zeta, colour bits clear
   colour (partial colour masks are skipped and reported).
5. **Depth read as colour.** The title samples its depth buffer (0x10000) as A8R8G8B8 and
   rebuilds 24-bit depth from A,R,G. Each zeta gets an RGBA8 alias per sampled size,
   refreshed by a compute pass before an op that samples it after a depth/stencil write
   (`ZETA_TEX=0` off). Colour vs depth at an offset = whatever wrote it last.
6. **Depth export** (SHADER_CONTROL 0xE): SV_Depth = saturate(r1.z) (RPCS3).
7. **Depth bounds** (0x380-0x388): ID3D12Device2 pipeline stream + OMSetDepthBounds.
8. **Batches overflowing the vertex buffer were truncated** (the G-buffer passes hit
   MAX_VERTICES): the batch now flushes early (`BATCH_SPLIT=0` off), carrying the partial
   triangle of a BEGIN/END stream across.
9. **Flips fired at call time** (4 KB per-frame flip contexts were rejected by the FIFO
   marker path's ring reserve): small contexts now take the marker (`GCM_FLIP_SMALLCTX=0`).
10. Vertex type CMP (11:11:10 normals) read as float32; FP facing register (SSA) was 0;
   indexed triangle strips kept one winding.
11. **Display buffers are the G-buffer's third plane.** The title registers display buffers
   at 0x3B0000 / 0x734000 and uses the back one as MRT C of its G-buffer, then -- with
   display-target (rt 0) draws -- accumulates its cascaded-shadow mask into that plane's red
   channel (2FBEC818, CC3DA20A, additive, red-only), a light term into green (6BFEFCA8),
   the water into it (7FA49ECB, later copied out by 2A36B210), and colour-clears it black
   (mask 0xF0). This backend kept two copies of a display buffer -- the swap-chain
   backbuffer for rt-0 draws and an offscreen RT at its offset for MRT use -- so the plane
   never received those passes (its red channel held stale data the sun pass multiplies by
   129 * N.L: the white and pink frames), they landed on the image being presented instead
   (red and green casts), and DB_SNAP pasted the backbuffer over the plane mid-frame. Now
   (`DISPLAY_REDIRECT`, default on): once a display buffer has been written as an
   offscreen surface it IS that surface -- rt-0 draws and colour clears aimed at it go there
   in order, samplers read it, and a present of it (the flipped buffer, from
   `cellGcm_flipped_display_offset`) copies it to the backbuffer. Titles that never alias a
   display buffer are unaffected.
12. **Texels were never decoded.** TEXTURE_ADDRESS unsigned remap BIASED (bits 12-15 = 1:
    [0,1] -> [-1,1]) and the gamma bits (20-23, sRGB per channel) were ignored. The G-buffer
    normals are stored n/2+0.5 and sampled BIASED -- undecoded, every normal leaned into the
    +++ octant, and the cleared background (0.502) decoded to a lit normal instead of 0, so
    the sun lit the sky; most colour textures are sRGB. Now every texel a buffered program
    samples goes through `rsx_texproc` with per-draw control bits (`fp_texctl`, after
    fp_alpha in b1), per channel in the hardware's precedence signed > gamma > BIASED (as
    RPCS3's process_texel); BIASED only where the CONTROL1 crossbar reads the texture (a
    forced 0/1 stays 0/1). Signed channels (TEXTURE_FILTER bits 28-31) are not converted
    yet -- this title sets none; a `[TEXCTL]` line says so if one appears. The other b1
    producers (draw engine, Metal, live draw) reserve a zeroed slot: texels raw there.
    `TEX_EXPAND=0`, `TEX_GAMMA=0`.
13. **SPU semaphores.** The RSX acquires labels 0x870/0x890 (NV406E SEMAPHORE_ACQUIRE) that
    SPU jobs release every frame; the walker skipped acquires. Now it waits, bounded
    (`GCM_SEMA_ACQUIRE=<ms>`, default 1500; 0 = never, -1 = forever): the drain stops and
    re-reads the label on later passes, and the stuck-get watchdog leaves a semaphore wait
    alone (it had resynced past the frame's flip and labels, wedging the title at boot).
    Measured: 0x890 is released ~420 ms after the RSX gets there, 0x870 within 16 ms.
14. **Depth copies to main memory.** Each frame the title blits its depth buffer (and a
    G-buffer plane) to main memory with NV3089 for its SPU job; readback only knew colour
    RTs. Zeta surfaces are now read back too (the RGBA8 alias, in RSX byte order;
    `ZETA_READBACK=0` off).
15. **256 texture slots per batch ran out** in the open world (a G-buffer batch binds
    hundreds of materials): everything after the last free slot sampled a null view. 1024
    now, with a log line if it ever runs out again.
16. Small render targets (the 20x11 bloom level) were forced to window size (a 16-pixel
    floor in current_rt_off): any size 1..4096 now.

Reviews: two adversarial reviews on the depth model (4 lenses each, every finding
verified) and one on everything after it (display redirect, texel decode, zeta readback,
semaphores, tools: 4 lenses, 20 confirmed findings, all fixed but two noted below). The
fixes from that one: flush accounting and readback-pending checks follow a redirected
display draw (by its recorded display target); the present copy runs for VP-less batches
too; a display buffer is redirected only when bound at window size, RGBA8, and an MRT pass
targeting it keeps its other planes; a zeta created or grown is not read back until the
guest writes it, and a readback with no zeta write falls through to the colour surface;
every zeta is sized before the replay records a conversion (3 aliases per zeta); the
semaphore wait's state is forgotten whenever `get` jumps, and the ring is not recycled
under it; `ps3_ms_now` is a real clock off Windows (it returned 0: every timeout on it was
infinite); STALL_DUMP is opt-in and copies SPU state under the lock-line lock; DMA put
scans stay inside local store; OPSNAP keeps each snapshot's own size; VTX_CLIP_ID indexes
its matrix from c[0].

Known limitations: a partial-mask colour clear of a redirected display buffer is still
skipped (as for every offscreen surface); one zeta sampled at more than three sizes in a
batch rebuilds aliases (log line); signed texture channels are sampled raw.

Still open: **regression -- the menu sky is black** (it was a cloudy sky at 21:50,
`screenshots/menu_2026-10-05_21h.png`; now lightning over black): the menu's G-buffer puts a
full-screen mesh at depth ~0.9985 and no light pass shades it, so the sky dome (LEQUAL at
1.0) fails everywhere (the old menu looked right only through the shared, cleared depth
buffer) -- whether that mesh's depth, the dome's, or a missing pass is wrong is not known
yet; the SPU job that
reads the depth/G-buffer copies writes all-zero results back (unverified whether that is
right); depth-of-field blur looks blocky (point-sampled downsample?); bright lightning
bloom; after the tutorial the player can end up below the pier (collision, not
rendering); ~0.8-2 fps, SPU-bound (the 0x890 wait alone is ~420 ms a frame).

Tools added: FRAMESTAT=<n> (per guest frame, per target: VP/fallback/clear/vertex counts --
how the empty open world was found), PSOSTAT, PSOMISSDBG, CUBEKEYDBG, FP_LIST, FP_DUMP_ALL,
FP_CACHE_BY_ADDR; RSX_CAP captures every flush batch (frame_NN_bK) with per-draw FP constants,
texture ADDRESS/FILTER words, the bytes of tiny textures, depth-bounds state;
`RSX_CAP_OPSNAP=<rt>` (that target after every draw), `RSX_CAP_MEMRT=1` (guest memory under
each RT), main-memory texture dumps; ZETADBG, FP_KILVIS, FP_VALVIS (ids | HLSL expr),
VS_DUMP_ALL, VTX_CLIP_ID, CONSTDBG_ID, DRAW_SKIP_ID, ZSCALE_DBG, TCONST_PKT, SEMA_TRACE,
NV3089_COPYLOG, SPU_DMA_RANGE_PUTS (+`SPU_DMA_RANGE_AFTER_MS`, non-zero-put stats),
STALL_DUMP=1 (dumps every SPU context when the PPU spins ~4M reads),
GCM_FLIPCOUNT flip entry points.

How to get there: the 2026-10-05 (21:50) env (MOVIE_SKIP, PAD_SCRIPT, PAD_STICKS,
PAD_STICKS_REPEAT); the boat cutscene starts ~t150 s, the Beast ~t210, the boat interior
~t230, the tutorial ~t300. The cutscene opens on a white -> grey fade (t151-158 in every
run), not a fault.

Next: the menu sky (regression above); then whether the SPU job reading the depth copies
should write zeros; the blocky DOF; the post-tutorial fall; a host profile of the SPU
path (0x890 latency).

## 2026-10-05 (21:50) — into gameplay: new game -> tutorial -> first objective; green cast fixed
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | PC runs 18:39..21:50
Where it was: main menu at ~1-1.8 fps with a green cast; START A NEW GAME led to the
intro montage Bink, which ran at ~0.02 fps (3273 frames -- hours).
Where it is: START A NEW GAME -> (intro movie skipped) -> in-engine "city falls"
cutscene -> the playable tutorial ("Use the right stick to look up", "...look left";
the stick input moves the camera and advances the prompts) -> the first objective,
"Head towards the disturbance". The menu, tutorial and Beast scene render with
correct colours (glow/bloom and water reflections work). ~1.1-2 fps in game.
Still wrong in game: a flat orange/olive full-screen colour with unlit red blocks
right after the Beast scene (looks like fog at full strength), a red full-screen layer
over "look left", and the open-world view is mostly black; menu: flat yellow strips on
the water, magenta/mint building tints.

How to get there (a local runphase.ps1 wrapper or tools\run.ps1 with these env vars):
`MOVIE_SKIP=v_00_intro_montage` (ppu_fs open -> ENOENT; the game skips the movie),
`PAD_SCRIPT=110:0x4000` (CROSS on START A NEW GAME), then
`PAD_STICKS=240-250:128,128,128,0|250-258:128,128,128,255|258-268:128,128,0,128|268-278:128,128,255,128|278-296:128,0,128,128`
with `PAD_STICKS_REPEAT=240,56` (right stick up/down/left/right, then left stick
forward, repeated: the prompts appear at run-dependent times). Run ONE instance at a
time: two started ~25 s apart both died in the first minute (shared data files).

Fixed / added, in the order found:
1. **Intro movie.** `MOVIE_SKIP=<substr>[,...]` in ppu_fs: matching opens fail with
   ENOENT; the game goes straight to the in-engine cutscene.
2. **Stick input.** `PAD_STICKS="<from>-<until>:lx,ly,rx,ry|..."` (16 timed segments,
   `|` or `;`; runphase splits -Env on `;`), `PAD_STICKS_REPEAT=<start>,<period>`.
3. **SPURS kernels spun a host core each** (2 adversarial
   verifiers: sound). libsre's system-service idle handler (PM 0x30021480 @0x11A8) waits
   with `rdch SPU_RdEventStat`, mask exactly Lr; the runtime returned at once unless
   SPU_CH_BLOCK, so it committed an identical line (PUTLLC succeeds) and looped,
   killing its peers' reservations: ~6.5M GETLLAR per 2 s. Now an Lr-only wait with a
   live reservation parks (`spu_ch_lr_wait_parks`; registered, non-policy, non-stack
   contexts; resv_ea != 0) and every hooked writer wakes it; `spu_ch_wake` takes the
   waiter's SRW lock first (lost wake-up); the 10 ms poll backs up unhooked writers
   (SPU_IDLEPROBE reports how often). GETLLAR 6.5M -> 100-300k per 2 s, EventStat
   reads 6.5M -> 2-5k. `SPU_LR_PARK=0` disables. Contexts now leave the notify registry
   before lv2 frees them, before a restart/PM reset memsets them, and at thread end.
4. **The green cast** -- root cause found. A display buffer is two resources here: the
   swap-chain backbuffer (draws with rt_off 0) and an offscreen RT aliasing its offset.
   The game uses the current back buffer's memory as its G-buffer's third plane
   (depth-like value in G, R masked), then draws the lit scene into the display buffer
   (a full-screen copy, op "#192" in `FRAMELIST=0F2DEB41`) and immediately downsamples
   it for the glow (op "#193"). That downsample read the alias -- the G-buffer plane --
   and the composite added it (green) to every frame. The water reflection read it too.
   Now the texture prepass tracks per display buffer which copy was written last
   (db_kind); a sampler of one last drawn through the backbuffer gets the backbuffer
   copied into the alias right before that op (`[DBSNAP]`), then samples the alias.
   `DB_SNAP=0` restores the old rule. End-of-frame write-through (DISPLAY_WRITETHRU)
   was the wrong model -- the glow is not frame feedback -- and stays off.
5. **MRT exports** (RPCS3 FragmentProgramDecompiler BuildCode): every BOUND target is
   written from its export register, 0 if never written (D3D left unwritten targets
   untouched); and RSX's one register file: an export lane last written through the
   other file is gathered (h(2n).xy = halves of r(n).x, low first; .zw of r(n).y;
   per 32-bit lane, suppressed writes excluded). `FP_MRT_WRITTEN_ONLY=1` reverts the
   first part. Unit tests 6/7 updated (36/36 pass).
6. **SET_BLEND_COLOR** was never decoded and OMSetBlendFactor never called (constant
   blend factors ran with D3D's default). Now recorded per draw (8-bit surfaces; fp16
   BLEND_COLOR2 not handled). This title does not use constant factors (BLENDDBG).
7. **The 0x6C00 plugin blob** (~7 s of interpreter time per 2 s in game) is one of five
   "DA7ABA5E" plugins in the EBOOT (4 x ila, magic, code offset 0x30, size; EA
   0x81EC80/0x81F000/0x81FC00/0x820400/0x821000). `lift_jobchain.py` lifts them PIC as
   images 160-164. `SPU_PIC_VERIFY=160-164[:N]` runs each call twice (interpreter on a
   cloned context, then lifted) and diffs registers + LS: 80/80 identical. Yet with
   them registered 3 of 6 runs stalled ~130-170 s in (PPU waits on 0xA25A18 forever;
   every main-instance kernel parked idle while workload 3 shows contention 1 -- it is
   "running" but nobody runs it), 0 of 5 without. Cause not found; they are opt-in:
   `SPU_LIFTED_PLUGINS=1`. [2026-10-07: the stall was the FIFO park-grace timer; the
   plugins are now the default.] Frame rate is the same either way (the SPUs are no longer
   the bottleneck).
8. **PIC slots.** Dispatch into a --pic slot checks the LS bytes at the target against
   the image's original bytes (`spu_pic_slot_intact`, content_ea; a mismatch drops the
   slot -- seen when a job loads over an older one); with that guard, slots survive
   DMAs that land inside them without covering their base (jobs' own data GETs used to
   drop them to the interpreter).
9. Diagnostics: `LASTDRAWS=<n>[:every]` (+`LASTDRAWS_AFTER_MS`), FRAMELIST prints MRT
   targets/display buffer/units 1,5,6, `FP_ZERO_UNIT=<fphash>:<unit>`, `SPU_PIC_SKIP`,
   `SPU_PIC_VERIFY`, SPU_MISS_DIAG prints an enclosing DA7ABA5E header
   (+`SPU_MISS_DUMP_DIR`), HOST_SAMPLE_ALL per-thread histogram keyed by function.

Reviewed (4 reviewers + adversarial verifiers):
fixed -- prepass slot eviction under live SRVs, frame-copy fallback rule, wrong-size
alias, per-lane export tracking, POSIX guard, registry lifetime, resv_ea 0, intact
window clamp, fast-path mismatch, PAD_STICKS init race, SPU_LIFTED_PLUGINS=0.
Refuted -- stale-slot shadowing by identical entry bytes, overlapped-slot pile-up.
Known, not fixed: a parked SPU has no exit on group terminate (as before: lifted SPU
threads never stopped on terminate); RT readback of a display buffer drawn through the
backbuffer reads the alias; the snapshot copies the whole backbuffer (partial writes,
two display buffers in one frame); IDLEPROBE 'blocked' counts every empty read.

**Pre-existing renderer defects found by that review (next):**
- The D3D12 path decompiles FPs with inline constants as literals, but the PSO cache key
  (rsx_fp_code_hash) skips the constants: a program re-patched in place keeps its
  first-seen constants (material tints, animated parameters). The composite E933D344
  is uploaded to a new address most frames (67 addresses in one run) and recompiled.
  Fix: rsx_fp_decompile_buffered_ex + the b1 constant layout, keyed on code only.
- Alpha test (rsx_fp_apply_alpha_test) and cube-map declarations are never applied on
  the D3D12 path (Texture2D declared for TEXTURECUBE SRVs; alpha-tested materials draw
  opaque).

Next: those two; then the full-strength fog colour in game (VP fog output / fog
params), the black open-world view, and a host profile (`HOST_SAMPLE_ALL`) of where the
1-2 fps goes now that the SPUs are idle.

## 2026-10-05 (17:35) — the main menu renders (green cast); five renderer/SPU fixes
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | PC runs 15:50..17:35
Where it was: menu phase ran but the frame was olive/black at ~0.1 fps.
Where it is: title screen + main menu draw ("START A NEW GAME", "Press ⓧ to continue",
storm sky with lightning and sparks, the New Marais skyline) at ~1-1.8 fps, stable for
200 s (no hangs, faults or pipeline failures). screenshots/main_menu_2026-10-05.png is a
correct-colour frame (~170 s; building tints still off). The green cast is intermittent:
frames with the skyline in view are right, the sky-only fly-through frames go green.
Perf next: a WWS job calls a blob at LS 0x6C00 (+0x30) that is still interpreted -- up to
5 s of interpreter time per 2 s late in the menu (SPU_INTERP_STATS: 0x6C44/0x6C54).

In the order found (each was the gate for the next):
1. **SPU MLAA read stale memory.** Per menu frame the game NV3089-blits RT 0xE3C000
   (1280x720, two blits: 1024 + 256 columns) and local 0x10000 (the Z24S8 zeta -- not
   read back, the backend has no depth readback) to main memory for SPU MLAA. D3D12 RTs
   never reached guest memory. `rsx_d3d12_readback_surface` (cellGcmSys hook
   `g_gcm_rt_readback`, called before the guest copy): flush pending records
   (render_frame without present; the backbuffer is kept for the rest of the frame),
   copy the RT to a readback buffer, write it in RSX byte order. Local sources only,
   32-bit/float surface formats only, surface pitch must equal the reader's.
   RT_READBACK=0 disables, RT_READBACK_DBG=n logs. Reviewed adversarially:
   depth after a flush, keep-backbuffer only if the flush drew display, `used` marks
   across the split, pitch-based surface choice -- all fixed.
2. **JobChain jobs interpreted** (~1.5 s of interpreter time per 2 s). `SPU_INTERP_STATS`
   + `SPU_MISS_DIAG` showed every miss with lr=0x2570 (the JobChain PM) at 20+ LS
   addresses: SPURS job binaries load wherever the PM's buffer lands. 30 binaries in the
   EBOOT (EA 0x88A180..0x8C6F00, 4 x ila header + `xor; br +8`). `spu_lifter.py --pic`
   lifts at 0x20000 and emits every pc-derived address as SPU_PIC(x) (+ctx->pic_bias);
   the runtime's resident slot carries pic_base and sets the bias on dispatch (also in the
   clang fast path, which used to ignore slots). Registered by EA and by 16-byte header.
   Interpreter time dropped to ~3 ms/2 s; one WWS job still calls a 0x6C00 blob (+0x30)
   interpreted (~1.6 s per frame burst) -- `SPU_MISS_DIAG` now prints its header.
3. **Frames took 10 s on the RSX thread.** Host profile (`HOST_SAMPLE`, tools/host_profile.py):
   58% in vp_get_fp_pso, 14% vp_get_vs. VS cache 16 (FIFO eviction shifted slots, so PSOs
   keyed by slot missed too) -> 1024; PSO cache 256 -> 4096; failures negative-cached
   (`PSOSTAT=1` showed ~95% of misses were the same failing programs). 0.08 -> 0.8 fps.
4. **The 3D scene never drew.** Those failures: E_INVALIDARG -- G-buffer materials sample
   texture units 4..6, the root signature had t0-t3 / s0-s3. Draw records, the per-draw
   descriptor windows, root signature and bind path now carry all 16 units. Also: the
   legacy texture-slot SRVs (heap 1+slot) overlapped the RT SRVs and the first draws'
   windows -- moved to their own range.
5. **GPU hang** right after (DXGI DEVICE_HUNG). DRED (now built in, `D3D12_DRED=0` off)
   + a "first copy of the frame list" note: the hung op was the seed copy of 0xAB8000
   recreated at 1280x180. The game uses 0xAB8000 as 1280x720 and 1280x180 in one frame;
   the offset-keyed pool released the other-size resource while the open command list
   still used it. Pool keyed by (offset, size, format), 32 slots, texture lookups match
   dims, records resolve their target by dims.
6. SET_COLOR_MASK_MRT (0x370) is now decoded (nibble per target B..D, A,R,G,B order) and
   applied per target (IndependentBlendEnable). The game sets it.

**The green cast (open).** RSX_CAP (now `RSX_CAP_AFTER_MS`-gated) + `FP_DUMP_ALL`:
- 0x11C0000 (the lit scene) is correct: dark blue storm, lightning.
- The final pass (fphash E933D344) is `scene*(1-bloom.a) + 2*bloom(0xB9E000) + glow`,
  glow = 0xE3C000 at 320x180 = weighted taps of **0x734000** (fp 0F2DEB41, weights in
  tc*.z). 0x734000 is display buffer 1, used as the 3rd G-buffer target and never cleared
  by the game; ours is G=1 over the sky, so glow adds full green.
- Tried: sampling a display buffer before this frame drew to it -> previous frame copy
  (kept: correct by construction); black seed for display-buffer surfaces (kept);
  display write-through (backbuffer -> the aliasing RT at frame end): correct hardware
  model but the glow then feeds back with gain > 1 and blows out to white in seconds, so
  DISPLAY_WRITETHRU defaults off. Conclusion: some input of the glow chain is wrong --
  most likely the G-buffer pass's third output or the tap weights (tc*.z from VS 163/164).
- Next: dump the G-buffer FP (`FP_DUMP_ALL`, op rt=0xAB8000 mrt=0xE3C000/0x734000) and
  check what h6/r3 holds; print the VS outputs tc1..tc4.z for the glow taps (VP_DUMP); compare
  with RPCS3 if available.

Also: write-through, `SCREEN_AS_TEX` now only for display buffers not yet drawn this frame
(=2 restores the old any-source rule); `RSX_CAP` manifest lists all 16 units and fphash.

## 2026-10-05 (late) — past loading: the menu phase runs; FIFO park fix; SPU regions
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | PC runs 14:30..15:30
Where it was: after loading, flips stopped at ~45 s; sometimes resumed ~100 s
later at 0.1-0.3 fps, sometimes never.
New probes (all opt-in):
- SPU_DUMP_AT_MS=<ms> (+SPU_FLOWRING=1): every SPU thread dumps its state,
  resident code slots and last 160 transfers at its next transfer after <ms>
  (counted from the first SPU transfer -- ps3_ms_now is GetTickCount64);
  SPU_SPIN_DUMP ranges are printed once with it.
- PPU_PEEK=<ea>:<len>[,...] (+PPU_PEEK_MS): background thread printing guest
  ranges whenever they change -- time series of counters (job lanes, SPURS
  ready/contention, a state word).
- PPU_THREADS_AT=<s> (+PPU_THREADS_EVERY=<s>): guest thread census (name, last
  syscall site, cia/lr, back chain). src/main.cpp now calls ps3_sampler_start,
  which starts these (and PS3_SAMPLE).
- CELLMARK_DUMP_AFTER_MS=<ms>: backbuffer dumps by time instead of present
  count (the present rate collapses from 33 to 2 fps at the menu).
- GCM_FLIPCOUNT=1 and PS3_POLLTOP=<s> (existing) were the most useful.
What the waits were (main thread, per frame):
- 0x504D90 / 0x505490: WWS job-queue barriers -- min of up to 7 per-SPU u16
  "lanes" at queue+0x40+16*row must reach the batch ticket (the LBP_SYNC_ACK
  structure; "Wws" is Sony WWS's SPURS job library, not Wwise). Queues:
  0xA93900 "General Jobs" (wkl0), 0xA94C80 "EdgeGeomJobs" (wkl1), 0xA94B00
  "SpuLightJobs" (wkl2).
- 0x3374BC: "SPU Light Task Kicker" (tid 9, obj 0xA25700) state +0x318 == 4.
  The kicker (0x337408) waits on semaphore obj+0x310, then steps 0x337040
  until state 4: state 0 waits for RSX label 0x20000850 >= the frame counter
  at obj+0x190/+0x194, state 1 for label 0x20000860, 2 polls 0x37056C, 3
  releases items.
Stall: the RSX FIFO walker met a JUMP-to-self in a side command buffer (IO
0x529BF0) with `put` back in the main ring and resynced to put AT ONCE
("FIFO resync (jump-park)") -- dropping the rest of the frame, including the
label 0x850 write. Kicker state 0 then waits forever, main waits for state 4,
no more flips. That rule was written for Twisted Metal (which abandons parks);
inFamous 2 patches its park a little later. Fix (cellGcmSys.c): stay parked
(re-reading the word each pass) for GCM_PARK_GRACE_MS (default 3000) before
resyncing, and keep the stuck-get watchdog off while a park is within grace.
Result: flips continue at ~2/s through 122 s with zero resyncs (every earlier
run froze at 45-70 s).
SPU code speed (spu_lifter.py --regions / SPU_LIFT_REGIONS=1, spu_drain.c):
find_spu_functions seeds a function at EVERY branch target, so each basic
block was a C function and every branch went through the dispatcher (~5 M hops
/s per SPU). Regions group blocks between real call targets (brsl/brasl, entry,
symbols, --extra-funcs) into one C function with labels; in-region branches are
gotos, backward ones behind SPU_BACKEDGE_EXIT (pending SPU interrupt, or
SPU_NO_GOTO=1); every block keeps a registered wrapper entry; LS 0, the entry
and --extra-funcs stay real transfers. Job modules: 11,062 direct trampolines
-> 3,870 (3,791 of them only on the safepoint exit); 8,514 functions -> 666
regions. Re-lift: SPU_LIFT_REGIONS=1 for tools/lift_jobs.sh, relift.sh's SPU
step and lift_libsre.sh's SPU step. Measured: General Jobs ~6 -> ~12 jobs/s,
no faults in ~15 min of runs -- but the post-load phase was NOT compute-bound:
PS3_SAMPLE showed the SPUs ~idle (system-service idle loop) while the main
thread polled job barriers / the kicker, and the kicker polled RSX labels.
Now: menu phase at ~2 flips/s; the window presents ~29/s frames carrying ONE
draw (a full-screen fill -- one dump was solid olive). Next: what the menu
frames actually draw (DRAWUNIQ, dumps after 50 s), then frame-rate (per-frame
CPU<->RSX label round trips through a 4 ms drain loop that also presents).

## 2026-10-05 — splash screens render; load starvation and WWS-queue stall fixed
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | PC runs 11:00..13:50
Renderer (patch 0001, rsx_d3d12_backend.c / rsx_commands.c):
- The fragment-program decompiler was given RSX_FP_CTRL_AUTO, which infers the
  export register from "wrote r0" -- programs that touch only r0.w but write
  their colour to h0 came out black (all text, Bink's YUV->RGB). It now gets the
  real SET_SHADER_CONTROL 32-bit-exports bit (exp32 -> 0x40). FP_CTRL_AUTO=1
  restores the heuristic.
- SCREEN_AS_TEX swapped any display-sized sampler source for the frame copy --
  including Bink's 1280x720 B8 luma plane. B8 is now excluded.
- Immediate mode (SET_VERTEX_DATA2F/2S/4UB/4S between BEGIN_END(prim) and
  BEGIN_END(0)) is turned into ordinary draws via a scratch block at local
  0x0FFF0000. The UI's full-screen black triangle is drawn this way.
- Result (backbuffer dumps, CELLMARK_DUMP): the WARNING screen, the Sony /
  Sucker Punch movie and the menu loading screen (black + inFamous 2 logo) all
  render correctly.
- Process lesson: two hours went into an "invisible movie" judged from window
  captures taken after the ~11 s movie had ended (and partly covered by other
  windows). Judge only backbuffer dumps (CELLMARK_DUMP*) or captures whose
  title draw count is known, and confirm a debug switch is live before trusting
  its absence.
Load starvation (spu_workload.c): the psarc decompressor task (taskset
0x47D7E380) waits in WAIT_SIGNAL after every 64 KB block. With real libsre,
cellSpursSendSignal only sets the bit at taskset+0x40; nothing signals the host
condition variable, so every wake came from the 1 s timeout -- one block per
second (~70 s at boot, ~100 min for the menu). spu_taskset_wait_signal now
re-tests the bit every SPU_WS_POLL_MS (default 1; 1000 = old behaviour).
Result: 2000 waits in 90 s (was ~170 in 3 min); 2.5 GB read by 90 s.
WWS job queues created with max contention 0 (ppu_hle.cpp): loading then
stopped at ~90 s with 4 of 5 SPURS SPUs idle in the system service. The new
SPU spin report (instance header + kernel context, SPU_SPIN_DUMP for more)
showed workload 0 = a WWS job queue (PM 0x887080) with readyCount 5 and
wklMaxContention 0 -- no SPU may take it. PPU_WWATCH=9D3ED0 showed libsre's own
add-workload writing max 0, 0, 8 for the three WWS queues (min 1). Cause: the
HLE->libsre bridge drops r1 by 0x1000 before calling into libsre (a YDKJ
workaround, default on) on the theory that "args are in regs" -- true for 8
arguments only. _cellSpursWorkloadAttributeInitialize has 9; its maxContention
is read from the caller's parameter save area (r1+0x70) and got stack garbage.
The bridge now copies the caller's 0x100-byte frame header + parameter area
into the new slice.
Result: add-workload now writes max contention 5, 5, 5 (and 8 for the taskset);
the queues' jobs are taken (readyCount back to 0). Loading then stops EARLIER,
at ~38 s / 4540 reads: a PPU thread loops in 0x52D63C..0x52D6AC (event-flag
clear + wait, then 0x58DFD8) because the 2048-byte command queue at 0x4879B3A0
is full and its busy byte +0x20 stays 1. The queue belongs to Audiokinetic
Wwise (thread "AK::EventManager", arg 0x4879B418) -- so "Wws_Job" (PM 0x887080)
is most likely Wwise's SPU job manager and workloads 0..2 its job queues.
At the same time one SPU logs a drain-resume at LS 0x221C (return_pc 0xA50,
img 41, depth 3) ~7000 times a second from boot -- never seen while queues 0/1
could not run. (That log cap failed because its static counter was rolled back
by the drain's longjmp; it is volatile now.)
Unlifted JobChain PM (tools/lift_policy.py, spu_channels.c): the drain flood is
not Wwise's PM at all -- 0xA4C/0xA50/0x221C are libsre's SYSTEM SERVICE PM
(0x30021480, image 121: 0xA4C `brsl 0x21A8`, 0x2218 `brsl 0x28D0` -> 0x221C).
Thirteen lines before the flood the kernel DMAs a PM we had never seen,
0x3002A180 (16000 bytes) -- libsre's JobChain PM, which Wwise's audio job chain
needs (cellSpursCreateJobChainWithAttribute / RunJobChain from the AK code).
It had no lift, and the resident-code slot of the system service is "sticky",
so the SPU kept running system-service lifted code over JobChain bytes: its
preempted-workload path (wklCurrentId != 32) hit `stopd` on the pending
SIGNAL, spu_stop() does not halt, and the drain re-entered 0x221C forever.
Wwise never rendered, AK::EventManager never drained its command queue, and
the loader stopped. Fixes: lift_policy.py now lifts all four captured PMs
(0x3002A180 = image 123; the other three are byte-identical to before), and
a code-sized GET of a different EA landing on a sticky slot's base now
evicts it, with "[spu-ovl] UNLIFTED code EA ..." when nothing lifted matches.
Result (14:09 run): no flood, reads reach 4680 (the pre-fix plateau) at 38 s.
Next stop: SPURS SPU 0 FAULTs in WWS job module 210 (EBOOT 0x82E780 at LS
0x4000) branching to LS 0x15280 -- runtime-loaded code (a Wwise DSP plugin?)
with no lift; wkl0 stays at contention 1 on the dead SPU.
Runtime-loaded Wwise DSP code (tools/run.ps1, spu_interp.c): the toolkit's
SPU_INTERP_UNLIFTED=1 (written for YDKJ's FMOD plugin) interprets such code
and rejoins lifted code on return; run.ps1 now sets it by default. With it
(14:11 run) reads pass 4680 and reach 6274 by 38 s and the screen changes
phase (1 draw/frame). Then Wwise's audio job chain (second SPURS instance,
workload [00] JobChain "Ak", its single SPU 0x2005) hit `dfm` at LS 0x6C54:
the interpreter had no double-precision ops at all, the SPU faulted and
libsre asserted ("SDK INTERNAL ASSERTION FAILURE ... spurs_handler_entry").
Added dfa/dfs/dfm/dfma/dfms/dfnms/dfnma/dfceq/dfcgt/dfcmeq/dfcmgt/dftsv (the
lifter's spu_helpers.h already had them) and rotmah; every decoded op now has
an interpreter case except bisled.
Interpreter rejoin at depth 0 (spu_channels.c): then a SPURS SPU thread just
ENDED ("stopped status=0x1 ... FAULT", status still RUNNING) at LS 0x43A8 in
WWS job module 225 and the frame loop froze. A drain at depth 2 met the job's
mid-function return site 0x4388 (no lifted entry) and restarted dispatch at
depth 0; the SMC microstepper stepped 0x4388.. and handed `rotqbyi` to the
interpreter, which stopped on reaching lifted 0x43A8 -- and that hand-off never
set g_spu_trampoline_fn (every branch exit of the microstepper does). Inside a
drain nobody noticed (the drain re-resolves pc); at depth 0 the driver loop
ended. Now both interpreter hand-offs (microstep and SPU_INTERP_UNLIFTED)
re-arm the dispatcher when they stop on lifted code. The thread-end FAULT now
prints pc/lr, LS words, the lookup result per image, the resident-code slots,
and (SPU_FLOWRING=1) the transfer ring.
orbi/xorhi/xorbi (tools/spu_disasm.py): next, job queue 0 (arg 0xA93900)
stopped draining -- main thread in func_00505490, HOTREAD on 0x00A93980 --
while job module d0d15ec83992 logged "UNSUPPORTED .word ... executed, result is
wrong" at six sites: 0x06xxxxxx (orbi) and 0x46xxxxxx (xorbi). The lifter maps
both to helpers, but the disassembler's RI10 table never decoded op8 0x06,
0x45 or 0x46. Added; re-lifted the job modules (tools/lift_jobs.sh) and the
game SPU images (relift.sh SPU step). The remaining `.word`s are ASCII data.

## 2026-10-05 — the splash movie plays (decoder side); screen still black
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | PC runs 10:12..11:00
Why Bink wrote to EA 0, traced backwards:
- PPU_WWATCH on its frame-buffer table (0xA08BC0): func_006D8D40 =
  BinkGetFrameBuffersInfo fills sizes/pitches; the movie player
  (func_001CF988) then converts its buffer EA (movie object 0xA087F0, field
  0x4C) to an IO offset through cellGcmGetOffsetTable's ioAddress table and
  back to EAs for each plane through eaAddress. 0x4C was 0 in the bad runs
  (and 0x40671400 / 0x40B71400 in good ones) -- a race.
- The movie thread sets a request state (0x48 = 1), sys_lwcond_wait()s, and on
  waking only checks state != 0. The main thread fills 0x48 = 2 and 0x4C under
  the lwmutex and sys_lwcond_signal()s. Our sys_lwcond_wait was poll-style
  (release, Sleep(1), reacquire, return) with no-op signals: every wait was a
  spurious wakeup 1 ms later, so the movie went on with a NULL buffer.
Fix 1 (patch 0001, ppu_sysprx.cpp): real lwcond -- per-cond counting
semaphore + waiter count; waiters register before releasing the lwmutex,
signal/signal_all/signal_to hand out tokens, timed waits return ETIMEDOUT.
Infinite waits keep a safety net (PS3_LWCOND_SAFETY_MS, default 500 ms, 0 =
none) that logs "[lwcond] ... no signal" and returns -- the FIOS media
threads hit it while idle, which is harmless.
Result: the buffer is valid (0x40B71400 in IO memory), Bink writes no more
rejected/null-page PUTs.
Fix 2 (patch 0001, lv2_register.c): spu_deliver_user_event routed lv2 SPU
thread user events only through the GROUP's ports
(connect_event_all_threads). Bink binds its completion queue per THREAD
(sys_spu_thread_connect_event(id, q11, USER, spup 0x35)) and signals with
sys_spu_thread_throw_event (intr mbox 0x75xxxxxx = throw, port 0x35), so every
"frame decoded" event was dropped and the movie player waited on q11 forever.
Now the thread's evt_bind ports are consulted too.
Got (10:5x run): Bink loops command -> decode -> event (hundreds of q11
events), the frame loop changes to the movie's: ~33 FPS, 7 draws/frame
(fullscreen quads prim=8 count=4 plus the four 256-vertex batches), no
faults. Screen still BLACK (window capture).
Remaining small thing: 8 rejected zero-size GETs from Bink (cmd 0x40, size 0,
ea 0x4897DB00) -- a size-0 DMA is legal (no-op) on hardware; the runtime
should accept it silently rather than call it malformed.
Next: the renderer. The draws log is capped before the movie phase, so first
raise/scope the D3D12 draw/bind_texture logging, then check: (a) the movie's
textures are in MAIN memory (IO-mapped, location 1) -- does the D3D12 backend
resolve location-1 texture offsets through the IO map; (b) Bink's YUV->RGB
fragment program; (c) whether the fullscreen quads' render target is the one
that gets flipped.

## 2026-10-05 — SPURS depth leak fixed; game loop runs; Bink gets its commands
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | PC runs 09:21..10:05
1. SPURS kernel host-depth leak (`[spu-depth] host_depth=2000`). The kernel
   calls a policy module with `bisl $r0,$r11` at LS 0x800, but a PM exits by
   BRANCHING to 0x808 (`ila $r1,0x3FFD0`, the kernel's stack reset). 0x808 was
   no lifted entry, so the microstep/interpreter ran it and the drain waiting
   for 0x804 stayed open: one more host frame per workload switch (~8 per
   stall interrupt). Fix: lift_libsre.sh lifts the kernel with
   --extra-funcs spu_0000_at_00020380=0x808, and src/stubs.cpp registers
   spu_register_stack_reset_entry(0x808, 41) once libsre loads, so that exit
   longjmps back to the driver at depth 0. The longjmp now also clears
   ctx->irq_frame / drain_ret_pc (they pointed into the discarded frames).
   Result: depth stays at 4.
2. The runtime's "256 identical DMAs -> halt the SPU" guard killed two kernels
   mid-install: WWS jobs poll the SPU Light Task Kicker object (EA 0xA25800)
   by re-reading it until the PPU changes it. run.ps1 now sets
   SPU_DMA_REPEAT_LIMIT=100000000 (overridable).
Got (09:39 run, 5 min): no SPU faults at all, the HDD install writes its 48
files (same set every run; last is ugc_template_targetpractice.ium), the
main-thread wait in func_00505490 is gone and the frame loop keeps going
(kicker semaphores 35/36 cycle to the end). Window capture: black frame, title
bar says 75 FPS, 4 draws/frame.
3. Bink never answered the movie player ("SP Movie Player" waits on q11
   forever). Two gaps:
   - sys_spu_thread_bind_queue (syscall 193) was a silent stub. Now records
     {spuq_num -> queue} per SPU thread (unbind = 194 too).
   - a lifted `stop 0x110` / `0x111` (sys_spu_thread_[try]receive_event) was
     never serviced: spu_stop returned and the drain re-ran the routine from
     its entry (the endless "drain-resume return_pc=0x464 at 0xFA08" lines).
     spu_lifter now emits `if (spu_service_stop(ctx)) { pc = stop+4; ... }`
     for those two codes; the runtime takes spuq_num from the out-mailbox,
     receives from the bound queue (sys_event_queue_receive_for_spu) and
     answers {CELL_OK, d1, d2, d3} / CELL_EBUSY / CELL_EINVAL through the
     in-mailbox. relift.sh now always re-lifts the game SPU images (--relift)
     and lifts Bink's continuations 0xFA18/0xFA90 as entries.
   Result: `thread_bind_queue tid=0x2006 queue=10 spuq_num=0x72616435`, then
   `stop 0x110 ... reply CELL_OK (4 words)`, Bink fetches its command
   (0x4897D400, 1280x720 movie) and starts writing frames.
Stopped at: Bink writes the decoded frame to EA 0 and up (PUT 10 KB blocks,
0x0..0x38400; the runtime rejects the first 64 KB and ACCEPTS the rest --
i.e. it scribbles over the EBOOT image at 0x11800+). SPU_DUMP_GET of its
frame-buffer table at 0x00A08BC0 (BINKFRAMEBUFFERS shape): TotalFrames 2,
Y 1280x720 pitch 0x500, cR/cB 640x360 pitch 0x280, every plane
{Allocate=1, Buffer=0x00000000}. The planes were never given memory.
Next: find who should fill those Buffer pointers -- the game (allocate after
BinkGetFrameBuffersInfo, then BinkRegisterFrameBuffers) or Bink itself
(Allocate=1 -> its RADMalloc/BinkSetMemory callbacks) -- and which
allocation returned 0. A write-watch on 0xA08BC8 (Frames[0].YPlane.Buffer) on
the PPU side, or tracing the PPU calls between "SP Movie Loader" start and the
port_send, should show it. Also worth guarding: PUTs to EA < 0x10000+EBOOT
end are always a bug; the runtime should reject the whole range.
New diagnostics: SPU_DUMP_GET takes up to 4 comma-separated EAs and dumps
512 bytes; [spu] depth high-water lines; SPU_FLOWRING ring is 512 deep and
also dumps at a depth overflow or when depth grows between two interrupts at
the same pc. Screenshots: tools need nothing -- a PowerShell
System.Drawing CopyFromScreen of the game window works (title bar shows FPS
and draws/frame).

## 2026-10-05 — Wws_Job stall interrupts fixed; loading screen draws, HDD install starts
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | PC runs 07:21..08:03
Housekeeping first: the 07:00 log was a build WITHOUT the SPU-slot fix -- the
project folder's patch/NOTES were older than the bundle (888211c). Synced
both from the bundle; 07:21 run confirms RADG5 now starts tid 0x2006 (image
2, entry 0xD0) and SPURS kernel 0 is no longer reset.
Stopped at (07:21): main thread still spins on 0xA25A18 = 3. SPU_WHOPOLLS
showed all 4 Wws_Job kernels in the PM at 0x2C98..0x22B0: waiting for tag 5
(mask 0x20) via WaitTag (0x2290, a bie/bid interrupt window) with
stall-notify pending (ev st=0x402 mask=0x2) and NO interrupt ever taken after
the first one per SPU. A chain of SPU runtime bugs in the interrupt/SMC path:
1. spu_interp ignored the bi-family E/D bits. The PM's stall handler (vector
   0xA2C) builds a register save/restore stub at LS 0x3FEC0, so it runs in
   the microstepper/interpreter and returns with `irete` -- E bit dropped,
   interrupts off for good.
2. spu_lifted_lookup searched only ctx->image_id (the kernel, 41), never the
   PM's sticky resident_code region, so the interpreter could not rejoin
   lifted PM code: once in, the PM stayed interpreted forever, and the
   interpreter takes no interrupts. Now resolves like spu_indirect_branch.
3. With (2) the handler's `brasl $r79,0x3FEC0` came from lifted code and hit
   the "branched into unlifted LS -- ending the job" guard; it now lets the
   SMC microstep run while an interrupt handler is live (irq_saved).
4. Interpreted brsl/brasl/bisl into lifted code now run as host calls
   (depth++, call, spu_drain_call(link)), like a lifted call site. Jumping
   made the callee's `bi $r0` pop an unrelated host frame.
5. spu_drain_call's resume lookup had the same blind spot as (2): rejoining
   the handler loop at 0xA60 found nothing and spu_restart_dispatch threw
   away every host frame mid-handler ("IRET ... unwinds to the driver").
6. The real corrupter (found with SPU_LS_WATCH on the job's link slot): the
   microstepper decoded stqa/lqa as `i16 << 4` instead of `<< 2`, so the
   handler's save stub wrote r0..r75 to 0x3E700+64n instead of 0x3F9C0+16n --
   across the interrupted job's stack. Registers round-tripped (the restore
   stub read the same wrong slots); saved links did not: jobs returned to LS
   0x30000 and every kernel FAULTed.
Got (08:03): no kernel faults, 200+ stall interrupts taken and returned to
their taking frame, the 0xA25A18 spin is gone. core/music/english/
english_text.psarc open, 20 D3D12 draws (4 x 256-vertex batches per frame --
the loading screen), "Hard Drive Installation" thread starts copying
install1/infamous1.psarc_s to dev_hdd0.
Stopped at: `[ppu] unresolved indirect call -> 0x0012BF88` (tid 1; the lr
0x12BBA0 is stale), then a garbage tty write (len 1.2 GB, buf 0). 0x12BF88 is
case 1 of a switch at 0x12BE1C. find_functions' shape-based .opd scan took a
constant-pool pair {0x0012BC94, 0x00AC038C} (VA 0x93D5F4) for a descriptor
and seeded a "function" at 0x12BC94 -- the inline jump table right after
func_0012BA4C's first bctr. That split the function; the second table's
base comes through r30, set in the first half's prologue, so the lifter
dropped it and the bctr became ps3_indirect_call.
Fix: patch 0001 -- apply_seeds drops seeds that start on a primary-opcode-0
word (34 in this EBOOT); extend_function_extents walks inline switch-table
cases after a bctr (222 functions grown, 16 new branch-target starts).
func_0012BA4C is whole again (0x12BA4C..0x12C3D0) and both tables lift as
switches. Jump-table test vectors still pass.
New diagnostics: SPU_FLOWRING=1 (per-thread ring of the last 64 transfers
with r0/r1/depth, dumped with the LS around r1 and the 0x3FE80 stub at a
synthesised-stop exit); SPU_IRQTRACE also logs an IRQ frame dropped while
the handler is live; the synthesised-stop line now prints lr/depth/last PCs.
Windows note: `python` is not on PATH here (only the `py` launcher), so
build.ps1's full mode fails at check_inputs. Ran the lift as
`PYTHON=py bash tools/relift.sh`, then `build.ps1 -BuildOnly`.
Got (re-lift, run 08:13): no unresolved indirect call, no garbage tty write.
"Game State Thread" runs cellSaveData ListAutoLoad; the HDD install copies
install1/infamous1.psarc_s, install2/infamous2.psarc_s and the movies/tv/*.bik
feeds to dev_hdd0 -- 48 files / 2.4 GB in game\disc\game\BCUS98125_INSTALL
after 2 minutes, still copying when the run was killed. Main thread waits in
func_00505490 (HOTREAD64 on stack 0x0FEFFB40) -- presumably on the install.
Stopped at: `[spu-depth] img=41 pc=0x00324 lr=0x035FC host_depth=2000` --
kernel tid 0x2001 halted. host_depth creeps up across workload switches
(TAKE depth 10 -> 12 -> 14 seen at 07:54): a PM that leaves to the kernel by
BRANCHING (pc 0x290 etc.) instead of returning to the kernel's bisl link
(0x800) is resumed by spu_drain_call's drain-resume path, so the kernel's
host frame for that bisl never pops and every switch nests one more.
Next: make the kernel->PM call unwind when the PM exits to a kernel entry
(e.g. treat a dispatch to a kernel-image entry while a PM drain frame is open
as a return: longjmp to that frame, or spu_restart_dispatch at depth 0), then
let the install run to completion (expect several minutes) and see what the
title does after it.

## 2026-10-05 — splash movie opens; Bink SPU group started the wrong thread
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | PC run 06:25
Got (switch fix confirmed): past func_000B4068 into full init -- GCM tiles,
cellPad/cellKb, cellAudio, Wwise AK::BankManager, the RADG5 (Bink) SPU group,
"SP Movie Loader/Player" threads, Wws_Job PM dumped + job 220 streamed, Edge/
light-task SPU jobs issuing DMA lists, splash_america.bik opened and 1 MB
read, 512x256 fmt 0x85 textures bound (movie frame planes), frame loop
(CLEAR_SURFACE, pad polling, sysutil pumped).
Stopped at: main thread spins in func_003374BC (lr 0x1DB7DC) on 0xA25A18 = 3
(SPU Light Task Kicker object 0xA25700 + 0x318: work never completes), and
one kernel spins on GETLLAR ea=0.
Cause: RADG5 is a num=1 group whose thread is initialized at SPU index 5
(lv2 allows any spu_num < 8). lv2_register.c's group_start/join/destroy
walked slots 0..num-1, so slot 0 -- zeroed, i.e. s_spu_threads[0] -- was taken
as the group's thread: group_start re-ran spu_lifted_thread_setup on the LIVE
SPURS kernel tid 0x2000 (resetting its registers under its running host
thread -> "0x2000 exits at 0x11B0", GETLLAR ea=0) and never started the Bink
decoder; group_destroy would have freed kernel 0.
Fix: patch 0001 -- empty slots are 0xFFFFFFFF; start/join/destroy walk all 8.
Linux check: RADG5 now starts tid 0x2006 (image 2, entry 0xD0).
Resolved by reading: the "thread index arg oddity" (r3=0xFFFF.., r5=0x7F7F..)
is the group_start log printing sctx GPRs after the host thread already ran.
Next: Windows rebuild (-BuildOnly suffices: runtime only), rerun -DumpSpu.
## 2026-10-05 — install streams; boot dies in an unlifted switch
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | PC run 05:56
Got: the taskset fix works -- the decompressor task (image 1) runs under
pm_30023680 (taskset syscalls 2/3, WAIT_SIGNAL cycling), FIOS media threads
stream ~600 KB of zlib blocks from infamous1.psarc_s, and the main thread gets
past the 0x402E5392 spin into game init.
Stopped at: `[ppu] unresolved indirect call -> 0x000B42C8` (tid 1, lr 0xB40C8),
then "stuck calling 0x000B42C8 (2000 times) -- aborting". func_000B4068 has a
19-entry base-relative switch (table inline at 0xB41D8, cases 0xB4224..0xB42E4)
whose base is two-level: `lwz r30,-0x7E98(r2)` in the prologue, `lwz
r11,-0x7F7C(r30)` at the dispatch. Between them an early-exit epilogue
restores r30 from the stack and TAIL-CALLS (`b func_000B2A14`). The lifter's
jump-table pass only treated `blr` blocks as dead, took that `ld r30` as r30's
definition, and dropped the table -> bctr became ps3_indirect_call.
Fix: patch 0001 -- discover_jump_tables' liveness filter also drops blocks
ending in an unconditional `b` out of the function (target before the scan
start or at a function entry). +3 dispatchers / +16 cases game-wide (339 ->
342); libsre lift byte-identical. New vector in tools/test_ppu_jumptables.py.
Next: full re-lift on Windows (build.ps1 without -BuildOnly), rerun -DumpSpu.
Side notes from this log: `open FAILED ...\game\disc\USRDIR\ne.bv` (a path
missing PS3_GAME -- cellFs path mapping, not yet looked at); the game-data
dir maps to game\disc\game\BCUS98125_INSTALL (writes ICON0/PIC1 .tmp fail
because the dir does not exist yet).
## 2026-10-05 — Windows w/ real assets: decompressor task never scheduled
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | PC run 05:21
Got: libsre loaded, D3D12 up, both install psarcs opened, first zlib block read;
_cellSpursLFQueuePushBody no longer unresolved.
Stopped at: main thread spins on 0x402E5392; SPU kernels idle-poll their SPURS
instance (GETLLAR 0x9D3E80 / 0x9D4E80 x4M). No job module or task became
resident: the psarc decompressor is a SPURS *task* (EBOOT SPU ELF @0x7AB900,
linked at LS 0x3000, entry 0x3050 = lifted image 1) that libsre's taskset PM
(pm_30023680, image 122) loads. The runtime maps task code only via
spu_taskset_register_task_elf(elf_ea, image, policy_image) and only matched the
policy against resident_ovl -- but our PMs are sticky code REGIONS, and nothing
registered the task at all.
Fix: patch 0001 -- spu_taskset_resident_image also accepts a policy image held
in a resident_code slot; lift_policy.py emits
spu_taskset_register_task_elf(0x7AB900, 1, <taskset PM image>) (--task EA=IMG).
Next: rerun on Windows (setup.ps1, python tools/lift_policy.py, build -BuildOnly).

## 2026-10-05 — main loop reached (frame clears, pad polling)
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 (adds 5 SPU
interpreter ops) | headless Linux, EBOOT + PARAM.SFO only
The "bad function pointer" at 0x4BA0 was a misread: job 220 was running in the
SPU INTERPRETER (the runtime falls back to it after a drain-resume / interrupt
return lands mid-function: kernel 0x80C, PM 0xA68 / 0x305C / 0x3608), which
walked into a prologue and hit `mpyui` -- an op present in the interpreter's
decoder table but missing from its switch -- then logged BRANCH-TO-0 with a
stale lr. Added mpyui, mpyhhau, bgx, rothmi, rotmahi (all had helpers already).
(Debug method that found it: fprintf in the generated job C at the bisl and at
the job entry never fired -> the lifted job was never the one executing.)
Got: jobs run without errors; 4 of 5 kernel threads stay alive; no libspurs
assertion. PPU side reaches the frame loop: cellSysutilCheckCallback pumped,
cellPadGetData polling, RSX CLEAR_SURFACE each frame, "Splash movie
Installation" thread starts (exits -129 here: no assets on the Linux box),
HTTP thread up, cellNetCtl reports offline.
Linux limit: sys_lwmutex/sys_lwcond waits are Windows-only in the runtime
(lwm_sem), so FIOS/cond waiters busy-spin here (HOTREAD in 0x52884C/0x51C554).
Next: run on Windows with real assets; expect the splash/install path next.
Still open: kernel thread 0x2003 exits at 0x290 / 0x2000 at 0x11B0; interpreter
fallbacks (could seed those mid-function return points as extra lift entries).

## 2026-10-05 — SPURS policy modules + 27 WWS job modules running
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 (now also: WWS PM
image id configurable via g_spu_wws_pm_image; sticky overlay regions)
How the real kernel works here: kernel (libsre SPU image 41) picks a workload,
DMAs its policy module into LS 0xA00 and `bisl`s there (0x7F4..0x800). Three
PMs captured with SPU_DUMP_OVL=<dir>,0xA00 (run.ps1 -DumpSpu now sets it):
  pm_30021480  8,704 B  libsre system service      image 121
  pm_30023680  7,744 B  libsre taskset             image 122
  pm_00887080 11,904 B  game Wws_Job job manager   image 120 (= cellSpurs pm=)
tools/lift_policy.py lifts them at base 0xA00 and registers each as a STICKY
co-resident code region keyed by source EA: plain regions were invalidated by
the first data DMA into the PM's own variables (job state @0x12A0), after
which every PM pc missed. Signature-based registration worked for PMs alone but
then a job module (resident_ovl) displaces the PM.
Job modules: 27 WWS modules (`4 ila|entry|size|0|0|C0DEC0DE`) are linked
UNCOMPRESSED into the EBOOT (0x7A4E00..0x85E000, 840 KB) --
tools/extract_eboot_jobs.py cuts them out, lift_jobmods.py lifts them at 0x4000
(images 200..226, ~9,000 functions; the 128 "unsupported" words are data).
tools/lift_jobs.sh does extract + lift + include fix-up + lift_policy.
Got: no more libspurs handler.c:198 assertion on the first pass; system
service PM runs; Wws_Job PM loads job descriptors (GETL lists, SN interrupts
taken, vector 0xA2C); first job module streamed to LS 0x4000 and matched by
signature (image 220 = jobmod_c6e7383dc013, entry 0x9400).
Stopped at: job 220 `bisl $r0,$r98` @0x672C -> 0x4BA0, which is mid-prologue
of a function starting ~0x4B00 (not a real entry). r98 = r3 at 0x656C, i.e. a
function-pointer ARGUMENT -- trace the caller and how the job relocates its
pointers (job descriptor code base vs the 0x4000 lift base). Same pc/lr in the
sig-only run, so not caused by the region change. Also seen: PM HALT-ASSERT
hlgti @0x1320, kernel thread 0x2000 misses at 0x12F0/0xA380.
Note: ppu runtime's "WWS job-code" helpers that check ctx->policy_mode belong to
the HLE-kernel path and stay off here (real kernel does that work).

## 2026-10-05 — real libsre (firmware LLE) in
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | libsre.prx from the
the player's own RPCS3-installed firmware (dev_flash/sys/external, decrypted in RPCS3).
Done: tools/lift_libsre.sh relocates libsre at 0x30000000 (TOC 0x30039AB0),
lifts its PPU code (384 functions; code ends at the .lib.stub table 0x3001DB18 --
everything after is data + embedded SPU programs, lifting past it decodes data),
routes its 32 firmware imports through ps3_hle_call, emits the NID export table,
and lifts its 2 embedded SPU ELFs as SPU images 41/42 (game's are 1/2).
src/stubs.cpp loads it in ps3_load_prx_modules (IF2_HLE_SPURS=1 to skip);
CMake picks it up when generated/prx/ exists.
Result: all 41 of the game's cellSpurs imports (incl. _cellSpursLFQueuePushBody)
now resolve into Sony's code. libsre builds CellSpursKernelGroup x2 (5 + 1 SPUs)
and the lifted SPURS kernel (image 41, fp 0x52C547B9A76CF26C) runs.
Stopped at: every kernel thread stops at LS pc 0x290 (status 0x1, code 0) -- the
kernel DMAs a policy module (taskset / job-chain "Wws_Job" PM) into LS and
branches to it; that code is not in any lifted image yet. Also thread index 4's
args look wrong (r3=0xFFFFFFFFFFFFFFFF, r5=0x00007F7F00007F7F vs index<<32 on the
others) -- check sys_spu_thread_initialize arg copying for the last thread.
Unresolved NIDs: 0x9B882495 cellFsGetDirectoryEntries, 0x56DFE179
cellAudioSetPortLevel. Linux test runs ignore SIGTERM: use timeout -s KILL.
Next suspect: the toolkit's own Yakuza LLE path -- fw_spu/spu_libsre
(kernel/policy/taskset_policy lifts), tools/extract_jobmods.py,
tools/instrument_policy.py, generated taskset_policy_bytes.c (CMake already
looks for it). Lift the PMs from OUR libsre the same way and register them.

## 2026-10-05 — first Windows run (RTX 5080, real assets)
EBOOT: BCUS98125 disc v02.00 | ps3recomp a679051 + patch 0001 | tools/run.ps1 -DumpSpu
Windows gotchas fixed on the way: link dxguid (IID_IDXGIFactory6); run.ps1 must
merge stderr in cmd (PowerShell 5 + ErrorActionPreference=Stop killed the run at
the first log line); Smart App Control blocks the freshly built exe (the PC owner's decision; see the README).
Got (beyond the Linux run): D3D12 backend up, window open (1280x720, D24S8);
reads ui/xmb/gamedata/*.png and the install psarcs from /dev_bdvd; first zlib
(78DA) block read from install1/infamous1.psarc_s.
Stopped at: unresolved NID 0x8A85674D = _cellSpursLFQueuePushBody (one of the two
cellSpurs gaps in import_coverage.md). The PPU pushes work into a SPURS LFQueue
for SPU taskset task 0 (lifted image 1 = static SPU image @0x85E380, almost
certainly the zlib/psarc decompressor); with no push the task sits in
WAIT_SIGNAL forever and "fios mediathread 6" blocks in cellSpursEventFlagWait
(flag 0x47D7FC80, pattern 0x0001). Main thread spins on 0x402E5392.
No job dumps yet -- Wws_Job workloads never start (PM not lifted).
Next suspect: SPURS is the wall every ps3recomp port hits; the toolkit's answer
is firmware LLE (external/ps3recomp/docs/FIRMWARE_LLE.md): lift the player's own
decrypted libsre.prx so cellSpurs/LFQueue/Wws_Job run as real Sony code (flOw
does this). Needs dev_flash/sys/external/libsre.sprx decrypted via RPCS3.
Alternative: hand-write _cellSpursLFQueuePushBody against the BE CellSyncLFQueue
line documented in libs/spurs/cellSpurs.c (bs[]/v1 state machine is unknown).

## 2026-10-05 — past CRT + heap, into SPURS (M3 done, M4 started)
EBOOT: BCUS98125 disc v02.00 sha1 f1c20717d653 | ps3recomp a679051 + patches/ps3recomp/0001
Fixed (toolkit bugs -- patches/ps3recomp/0001, applied by tools/setup.*; send upstream):
- Entry ABI: runtime passed argv but left envp (r5) = 0. _start @0x10338 copies
  envp with a do-while, so it read 0x4 / wrote 0x0 on the first iteration.
  Now r5 -> the NULL envp slot after argv, r6 = 0.
- sys_lwcond_t layout: runtime wrote the lwmutex EA as be64 at +0 and the queue
  id at +8. Real layout is {be32 lwmutex; be32 lwcond_queue}. The game's FIOS
  cond wrapper (@0x52884C) checks the word at +0x10 of its object (= lwcond +0)
  and printed "wait for invalid cond" thousands of times.
Fixed (Linux-only harness): src/main.cpp reserved the guest arena with calloc,
which is not page-aligned, so every vm_commit mprotect failed -> the very first
sys_memory_allocate returned ENOMEM -> malloc returned NULL -> lwmutex created at
address 0. Now mmap. (Windows was never affected.)
Got: heap; 22 sysmodules; SPURS x2 initialised; cellGame BootCheck/DataCheck;
cellVideoOut 1280x720; _cellGcmInitBody + display buffers + flip handler;
Edge Geom / SPU Light / Trophy / FIOS (12 threads) created; SPURS taskset task 0
dispatched to the lifted image 1 (static SPU image at 0x85E380); trophy context
registered; first data-file opens (fail here only because the test box has no
assets).
Stopped at:
- SPURS policy module "Wws_Job" (pm=0x887080, 11,904 B, fp 0xB3523C4B17650EF8)
  is NOT lifted -> workloads 0..2 (General / EdgeGeom / SpuLight jobs) never run.
  It is a raw blob inside the EBOOT data, not an ELF, so extract_spu_images
  misses it. Try external/ps3recomp/tools/extract_jobmods.py / wrap_spu_elf.py.
- SPURS job binaries: dispatch MISS for 1040 B (fp 054414E2CCD626FF) and 272 B
  (fp 64D9D14299A2B7AB). Captured with --dump-spu into analysis/spu_runtime/.
  Many more will appear once real assets load.
- "Job Manager Event Handler" threads (entry 0x968408 -> func_00504898, libsre
  code linked into the EBOOT) call a NULL OPD after sys_event_queue_receive
  (syscall 130); check what event the runtime delivers on queues 1/2.
- Unresolved NID 0x9B882495 = cellFsGetDirectoryEntries (missing in coverage).
- Linux only: ppu_dump_guest_stack segfaults while reporting a bad indirect
  call on thread 4 -- the diagnostic, not the game. Windows unaffected?
Next suspect: lift Wws_Job PM + the dumped jobs; then run on Windows with the
real assets (game/disc) and --dump-spu to collect the full job set.

## 2026-10-05 — first lift, build and boot (M2 done, M3 started)
EBOOT: BCUS98125 disc v02.00 sha1 f1c20717d653 | ps3recomp a679051
Tried: relift.sh (35,399 PPU functions, 154 MB C++, 5 chunks; SPU images 661 + 856
functions), full build with clang (Linux, ~9 min on 2 cores, 58 MB binary), 90 s
headless boot with only EBOOT + PARAM.SFO present (no disc assets).
Got: loader, TLS, CRT entry all run; sys_initialize_tls OK.
Stopped at: CRT heap bring-up. Right after TLS init, func_00010338 reads/writes the
NULL page (read32 @0x4, write32 @0x0), then the allocator path
(func_005F8478 <- func_005F47EC <- func_005F7BAC) writes through a NULL heap
header, func_006E38D0 calls a NULL OPD (r12=0x22000042) repeatedly, and the game
loops sys_memory_allocate(1 MB) ~527 times until the guest VM is exhausted
(OOB at 0xE0000100). Also: 6x lv2 syscall 144 (stub).
Next suspect: the CRT's heap/process-param setup. flOw hit the same class of bug
("Guest malloc bypass -- Dinkumware malloc requires OS-level heap init"); read
sp00nznet/flow's notes and diff what its crt overrides do. Identify what
func_00010338 expects at the address it read as 0 (likely sys_process_param /
a malloc-page-size global). Syscall 144 = check what it is in lv2_syscall_table.h.

## 2026-10-05 — inputs + first analysis (M0, M1)
EBOOT: BCUS98125 disc v02.00 sha1 f1c20717d653 | ps3recomp a679051
Tried: check_inputs.py, analyze.sh on the disc EBOOT.
Gotcha: this Redump-named ISO was ALREADY decrypted. Running PS3Dec on it again
scrambled every file outside the first region (EBOOT.BIN lost its SCE magic and
RPCS3 asked for a KLIC). Check a file's magic (EBOOT.BIN = `SCE\0`, *.psarc =
`PSAR`) before decrypting a disc image.
Findings:
- cellSpurs: 41 imports, 39 implemented -- missing only the LFQueue pair.
- No cellSail / cellVdec: cutscenes are Bink (.bik), decoded by game code, not firmware.
- No cellGem import despite Move support.
- sys_prx_load_module* imported: the game loads its own PRX modules at runtime.
  Find which ones (search USRDIR for .sprx) -- each will need lifting too.
- Online-only gaps (sys_net, cellHttp, sceNp tickets, NpEula) should be stubbed
  to "offline" rather than implemented.
Next suspect: lift (relift.sh), then build and look for the first unresolved NID.

Template:

```
## YYYY-MM-DD — <short title>
EBOOT: <title id> APP_VER <x.yy> sha1 <first 12>  | ps3recomp <pin, short>
Tried:
Stopped at: <last meaningful log line>
Next suspect:
```

## Baseline numbers (fill in after M1)

| | |
|---|---|
| Title ID / APP_VER | BCUS98125, disc v02.00, APP_VER 01.00 (no title update yet) |
| EBOOT SHA-1 | `f1c20717d6532dfb27e292a5db6cdf5a3218760b` (EBOOT.elf, 10,159,776 B) |
| Firmware imports / libraries | 257 / 19 |
| Coverage (implemented / total) | 209 / 257 (81%) at ps3recomp a679051 |
| Functions (.opd / after find_functions) | 15,104 OPD descriptors, 13,472 unique / 35,399 lifted |
| `--code-end` | `0x6eb250` |
| Static SPU images | 2 (38,100 B at 0x79B900; 101,596 B at 0x85E380) |
| SPURS job images captured at runtime | 2 so far (1040 B, 272 B) -- headless, no assets |

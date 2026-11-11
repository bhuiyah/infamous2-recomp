# Run the recompiled build on Windows (the only host where ps3recomp boots a title today).
#   powershell -ExecutionPolicy Bypass -File tools\run.ps1 [-LiveDraw] [-Verbose2] [-DumpSpu]
#
# Start from a checkpoint instead of the beginning (tools\checkpoint.ps1 list):
#   tools\run.ps1 -Checkpoint 03_forward_momentum_rpcs3
#   tools\run.ps1 -Checkpoint @new                       # no save: a new game
#   tools\run.ps1 -Checkpoint @new -Step 16              # prologue step 16 (experimental, IF2_TASKJUMP)
#   tools\run.ps1 -Checkpoint @new -Step list -ExitAfter 1800   # what the task-graph watcher sees
#   tools\run.ps1 -Checkpoint auto-0012 -Script "0:0x4000|30-150:128,0,128,128" -ExitAfter 600
# The save is copied into a sandbox (the player's saves are untouched), the title
# flow runs on game events (no wall-clock timing), the story FMVs and the logo splash are skipped,
# and the run says READY in the log and in logs\run-<time>.ready (status=ready|
# fail|timeout, with a screenshot logs\run-<time>-ready.png). -Script is pad input
# in guest frames after READY; -ExitAfter quits that many frames after READY (at
# once on fail/timeout); the script then exits with the game's code (0 ready,
# 3 no such checkpoint, 4 fail -- also when the game exits before an outcome --,
# 5 timeout). tools\checkpoint.ps1 wait [-ReadyFile <the path printed below>]
# blocks until the .ready file has an outcome -- drive the window with
# tools\kbm_inject.ps1 from there. -Step without -Checkpoint turns saving off for
# the run (PS3_SAVEDATA_READONLY): a jump's fabricated progress never reaches
# the player's saves.
# -AutoSnap keeps every save the game writes as checkpoints\auto-NNNN (any run).
# Save states (docs\SAVESTATES.md):
#   tools\run.ps1 -LoadState 3 -ExitAfter 600             # start in slot 3's moment (or -LoadState <file.p3ss>)
#   tools\run.ps1 -Checkpoint 03_forward_momentum_rpcs3 -SaveAt 45 -SaveSlot 2 -ExitAfter 900
# -LoadState loads the state as soon as the game shows its first frame (SS_LOAD): READY is that load
# (logs\run-<time>.ready as for a checkpoint; -Script / -ExitAfter count from it), no title flow is
# driven, and the run's save data is an empty sandbox (-Checkpoint @new unless you give one).
# -SaveAt <s>[,<s>...] saves at those seconds after the start (SS_SAVE_AT) into slot -SaveSlot (1).
# Settings: a -Checkpoint run (a test) ignores config\options.ini -- the menu's
# saved choices (fps cap, AF, brightness...) must not leak into measurements --
# unless -Options is given (OPTIONS_CONFIG=none; an OPTIONS_CONFIG you set wins).
# config\display.ini and config\input.ini are read as always.
param([switch]$LiveDraw, [switch]$Verbose2, [switch]$DumpSpu,
      [switch]$Options,              # -Checkpoint runs: read config\options.ini too
      [string]$Checkpoint,           # a checkpoint name (tools\checkpoint.ps1 list) or @new
      [string]$Script,               # CHECKPOINT_SCRIPT: "<f>:<mask>[/<hold>]|<f1>-<f2>:<lx>,<ly>,<rx>,<ry>|..."
      [int]$ExitAfter = -1,          # CHECKPOINT_EXIT_AFTER: frames after READY
      [int]$Timeout = 0,             # CHECKPOINT_TIMEOUT: seconds to READY (default 600)
      [string]$ReadyOn,              # CHECKPOINT_READY_ON: signal names (default WarpedToCheckpoint)
      [string]$SigLog,               # IF2_SIGLOG: 1 | all | <signal>,<signal>
      [switch]$KeepMovies,           # play the story FMVs and the logo splash in a checkpoint run
      [switch]$AutoSnap,             # CHECKPOINT_AUTOSNAP: keep the game's saves as auto-NNNN checkpoints
      [string]$Step,                 # IF2_TASKJUMP: <step> | 0x<step oid> | list (prologue by default)
      [string]$StepMode,             # IF2_TASKJUMP_MODE: warp (default: graph + WarpToCheckpoint) | graph
      [int]$StepAfter = 0,           # IF2_TASKJUMP_AFTER: jump once play reached this step (prologue: 7)
      [string]$TaskLog,              # IF2_TASKLOG: 1 | 2 | 3
      [string]$LoadState,            # SS_LOAD: a slot number (q = the quick slot) or a .p3ss file
      [string]$SaveAt,               # SS_SAVE_AT: seconds after the start, comma-separated
      [string]$SaveSlot)             # SS_SAVE_SLOT: the slot -SaveAt writes (default 1)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

$inputs = Get-Content analysis\inputs.json -Raw | ConvertFrom-Json
$exe = @("build\infamous2.exe", "build\Release\infamous2.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { throw "no build\infamous2.exe -- build first (README, step 5)" }

# Variables this script sets only for this run: put back as they were afterwards,
# so a -Checkpoint run started from a shell does not leak into the next run.
$scoped = "CHECKPOINT", "CHECKPOINT_DIR", "CHECKPOINT_READY_FILE", "CHECKPOINT_READY_SHOT", "CHECKPOINT_SCRIPT",
          "CHECKPOINT_EXIT_AFTER", "CHECKPOINT_TIMEOUT", "CHECKPOINT_READY_ON", "CHECKPOINT_SKIP_MOVIES",
          "CHECKPOINT_AUTOSNAP", "IF2_SIGLOG", "IF2_TASKJUMP", "IF2_TASKJUMP_MODE", "IF2_TASKJUMP_AFTER",
          "IF2_TASKLOG", "OPTIONS_CONFIG", "SS_LOAD", "SS_SAVE_AT", "SS_SAVE_SLOT", "IF2_RUN_EXE", "IF2_RUN_EBOOT"
$saved = @{}
foreach ($v in $scoped) { $saved[$v] = [Environment]::GetEnvironmentVariable($v, "Process") }

# The runner derives the VFS root from <root>/PS3_GAME/USRDIR/EBOOT.elf. That is
# wrong when lifting the title-update EBOOT (it lives under hdd0/), so always pin it.
$env:PS3_VFS_ROOT  = (Resolve-Path game\disc).Path
# game\hdd0 holds the title update (and the HDD install beside it): only a build lifted from the update
# runs with it -- the disc EBOOT never runs with an update installed (tools\build.ps1 -Disc).
# Which one it is: the layout tools\if2_resolve.py saw (analysis\resolve.json, family 104 = the update's code; an
# EBOOT taken from the update folder whose code is a disc build's runs without it, like build-v100's).
$useHdd0 = [bool]$inputs.update_app_ver
if (Test-Path analysis\resolve.json) {
    try { $useHdd0 = ((Get-Content analysis\resolve.json -Raw | ConvertFrom-Json).family -eq 104) } catch { }
}
if ((Test-Path game\hdd0) -and $useHdd0) { $env:PS3_HDD0_ROOT = (Resolve-Path game\hdd0).Path }
$env:PS3_TITLE     = "inFamous 2 (ps3recomp)"
# The runtime halts an SPU after 256 identical DMAs in a row ("wedged"). WWS
# jobs here legitimately poll the SPU Light Task Kicker object (0xA25800) by
# re-reading it until the PPU updates it, which that heuristic mistook for a
# hang and killed two SPURS kernels mid-install. Keep it only for true runaways.
if (-not $env:SPU_DMA_REPEAT_LIMIT) { $env:SPU_DMA_REPEAT_LIMIT = "100000000" }
# Wwise's audio jobs call DSP code the title loads into local store at run time
# (LS 0x15280...), which no static lift can cover: interpret it instead of
# ending the job (which left the SPU dead and loading stalled at 4680 reads).
if (-not $env:SPU_INTERP_UNLIFTED) { $env:SPU_INTERP_UNLIFTED = "1" }
# NULL-page accesses are reported with guest stacks and register peeks (bounded,
# and free until one happens): the debris-physics hang (NOTES 2026-10-14) leaves
# its producer in the log of whichever play session hits it.
if (-not $env:PS3_NULL_READ_MAX)   { $env:PS3_NULL_READ_MAX = "400" }
if (-not $env:PS3_NULL_READ_STACK) { $env:PS3_NULL_READ_STACK = "40" }
if ($LiveDraw) { $env:RSX_LIVE_DRAW = "1" }   # the alternative NV4097 draw engine
if ($DumpSpu) {   # capture SPURS job binaries the title loads from data files (ROADMAP M4)
    New-Item -ItemType Directory -Force analysis\spu_runtime | Out-Null
    $env:SPU_DUMP_MISS = (Resolve-Path analysis\spu_runtime).Path
    # SPURS policy modules the kernel streams to LS 0xA00 (tools/lift_jobs.sh)
    New-Item -ItemType Directory -Force analysis\pm | Out-Null
    $env:SPU_DUMP_OVL = (Resolve-Path analysis\pm).Path + ",0xA00" }
if ($Verbose2) { $env:PS3_VERBOSE = "1"; $env:IF2_VERBOSE = "1" }

New-Item -ItemType Directory -Force logs | Out-Null
$stamp = "{0:yyyyMMdd-HHmmss}" -f (Get-Date)
$log = "logs\run-$stamp.log"

# Checkpoints (src\if2_checkpoint.cpp, src\if2_taskgraph.cpp, tools\checkpoint.ps1):
# the library, and what a run booted from one reports.
if (-not $env:CHECKPOINT_DIR) { $env:CHECKPOINT_DIR = Join-Path (Get-Location).Path "checkpoints" }
$ready = $null
if ($LoadState) {
    # a slot number stays a number (the game resolves it in its save-state folder); a file is made absolute
    if ($LoadState -notmatch '^(\d+|q|quick)$') { $LoadState = (Resolve-Path $LoadState).Path }
    $env:SS_LOAD = $LoadState
    if (-not $Checkpoint) { $Checkpoint = "@new" }   # the harness (READY file, -Script, -ExitAfter), an empty sandbox
    Write-Host "save state run: SS_LOAD=$LoadState (READY = its load at the game's first frame)"
}
if ($SaveAt) {
    $env:SS_SAVE_AT = $SaveAt
    $env:SS_SAVE_SLOT = if ($SaveSlot) { $SaveSlot } else { "1" }
    Write-Host "save state run: a save at t=$SaveAt s into slot $($env:SS_SAVE_SLOT)"
}
if ($Checkpoint) {
    $env:CHECKPOINT = $Checkpoint
    $ready = Join-Path (Get-Location).Path "logs\run-$stamp.ready"
    $env:CHECKPOINT_READY_FILE = $ready
    $env:CHECKPOINT_READY_SHOT = Join-Path (Get-Location).Path "logs\run-$stamp-ready.png"
    if ($Script)          { $env:CHECKPOINT_SCRIPT = $Script }
    if ($ExitAfter -ge 0) { $env:CHECKPOINT_EXIT_AFTER = "$ExitAfter" }
    if ($Timeout -gt 0)   { $env:CHECKPOINT_TIMEOUT = "$Timeout" }
    if ($ReadyOn)         { $env:CHECKPOINT_READY_ON = $ReadyOn }
    if ($KeepMovies)      { $env:CHECKPOINT_SKIP_MOVIES = "0" }
    if (-not $Options -and -not $env:OPTIONS_CONFIG) { $env:OPTIONS_CONFIG = "none" }
    # This run's file exists (status=booting) before the game starts, so
    # tools\checkpoint.ps1 wait never mistakes an earlier run's outcome for it.
    [IO.File]::WriteAllText($ready, "status=booting`ncheckpoint=$Checkpoint`nreason=tools\run.ps1 is starting the game`n")
    Write-Host "checkpoint run: '$Checkpoint' -> $ready"
} elseif ($Script -or $ExitAfter -ge 0 -or $ReadyOn) {
    Write-Host "-Script / -ExitAfter / -ReadyOn count from READY: they need -Checkpoint (ignored)" -ForegroundColor Yellow
}
if ($SigLog)   { $env:IF2_SIGLOG = $SigLog }
if ($AutoSnap) { $env:CHECKPOINT_AUTOSNAP = "1" }
if ($Step)     { $env:IF2_TASKJUMP = $Step }
if ($StepMode) { $env:IF2_TASKJUMP_MODE = $StepMode }
if ($StepAfter -gt 0) { $env:IF2_TASKJUMP_AFTER = "$StepAfter" }
if ($TaskLog)  { $env:IF2_TASKLOG = $TaskLog }
if ($Step -and $Step -ne "list" -and -not $Checkpoint) {
    Write-Host "-Step without -Checkpoint: the game saves nothing this run (PS3_SAVEDATA_READONLY), so the jump's fabricated progress never reaches your saves" -ForegroundColor Yellow
}

# Merge stderr inside cmd, not in PowerShell: Windows PowerShell 5 turns every
# native stderr line into an error record, and with ErrorActionPreference=Stop
# the first runtime log line killed the run.
$ErrorActionPreference = "Continue"
$bat = Join-Path $env:TEMP "infamous2_run_$PID.bat"     # per process: two runs never share it
# The exe and EBOOT paths go in as environment variables, not as the file's text: the batch file is ASCII (cmd
# reads it in the console code page), so a path with a letter outside ASCII would arrive as '?'.
$env:IF2_RUN_EXE = (Resolve-Path $exe).Path
$env:IF2_RUN_EBOOT = $inputs.eboot
'@"%IF2_RUN_EXE%" "%IF2_RUN_EBOOT%" 2>&1' | Set-Content -Encoding ASCII $bat
$code = $null
try {
    & cmd.exe /c $bat | Tee-Object -FilePath $log
    $code = $LASTEXITCODE
} finally {
    foreach ($v in $scoped) { [Environment]::SetEnvironmentVariable($v, $saved[$v], "Process") }
    Remove-Item $bat -ErrorAction SilentlyContinue
}
Write-Host "log: $log  (exit code $code)"
if ($ready) {
    # The game ended (closed, crashed, or quit during boot) before its driver
    # wrote an outcome: record that, so a waiting tools\checkpoint.ps1 wait
    # returns now instead of at its timeout.
    $status = if (Test-Path $ready) { @(Get-Content $ready | Where-Object { $_ -like "status=*" })[0] } else { $null }
    if (-not $status -or $status -eq "status=booting") {
        [IO.File]::WriteAllText($ready, "status=fail`ncheckpoint=$Checkpoint`nreason=the game exited (code $code) before the checkpoint run had an outcome`n")
        if ($code -eq 0) { $code = 4 }
    }
    $st = (Get-Content $ready | Where-Object { $_ -like "status=*" -or $_ -like "confidence=*" -or $_ -like "reason=*" }) -join "  "
    Write-Host "checkpoint: $st  ($ready)"
    exit $code
}

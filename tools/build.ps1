# One-shot Windows pipeline: setup (+ toolkit patches) -> check -> analyze -> lift -> libsre -> jobs -> build.
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1            # everything
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -BuildOnly # just recompile
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Disc      # everything, from the disc EBOOT
#     (a title update installed in game\hdd0 is otherwise the EBOOT that is lifted; -Disc leaves it out)
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Step lift # one step (the launcher runs them so)
#
# Steps, in order (each needs the ones before it):
#   setup    fetch the pinned ps3recomp toolkit into external\ and apply patches\ps3recomp
#            (the first time a git clone from GitHub, about 15 MB: asked first; -Yes allows it without asking,
#            and without a console to ask on it is needed -- see tools\setup.ps1)
#   check   tools\check_inputs.py: your files in game\ (game\README.md) -> analysis\inputs.json
#   analyze  tools\analyze.sh: functions, imports, SPU images of the EBOOT
#   lift     tools\relift.sh: PowerPC + SPU code -> C++ in generated\
#   libsre   tools\lift_libsre.sh: your decrypted firmware libsre.prx (SPURS) -> generated\prx
#   jobs     tools\extract_policy_modules.py + tools\lift_jobs.sh: SPURS policy modules, job modules
#   build    CMake + Ninja with clang-cl -> build\infamous2.exe
#
# Build options go to the CMake cache, so they stay set for later builds until
# changed (CMakeLists.txt, "PPU code speed"):
#   -Define IF2_PPU_VM_INLINE=reads,IF2_PPU_NO_TLS_GUARDS=ON   any -D NAME=VALUE list
#   -Pgo instrument   profile counters: then run the training routes with
#                     build\infamous2.exe; each run writes
#                     build\pgo-raw\if2-<pid>.profraw (IF2_PGO_DUMP_S, default
#                     every 15 s, and at exit)
#   -Pgo use          merge build\pgo-raw\*.profraw into pgo\infamous2.profdata
#                     (the raw files move to pgo\raw-<time>\), build with it
#   -Pgo off          a build without PGO again
#   -PgoScope ppu|host|all  what PGO covers (ppu by default: the lifted PPU code and
#                     the PPU scaffold; all: every game and runtime source; host:
#                     all but the SPU code)
param([switch]$BuildOnly, [switch]$Disc,
      [ValidateSet("", "setup", "check", "analyze", "lift", "libsre", "jobs", "build")][string]$Step = "",
      [ValidateSet("", "off", "instrument", "use")][string]$Pgo = "",
      [ValidateSet("", "ppu", "host", "all")][string]$PgoScope = "",
      [string]$Define = "",
      [switch]$Yes)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
. (Join-Path $PSScriptRoot "python.ps1")

function Step($m) { Write-Host "`n== $m" -ForegroundColor Cyan }
function Need($ok, $msg) { if (-not $ok) { throw $msg } }

$steps = if ($Step) { @($Step) } elseif ($BuildOnly) { @("build") }
         else { @("setup", "check", "analyze", "lift", "libsre", "jobs", "build") }
function Want($s) { return $steps -contains $s }

# --- tools ---------------------------------------------------------------------
$bash = @("$env:ProgramFiles\Git\bin\bash.exe", "${env:ProgramFiles(x86)}\Git\bin\bash.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
$clang = "$env:ProgramFiles\LLVM\bin\clang-cl.exe"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if ((Want "analyze") -or (Want "lift") -or (Want "libsre") -or (Want "jobs")) {
    Need $bash "Git Bash not found -- install Git for Windows"
}
if ((Want "setup") -or (Want "check") -or (Want "analyze") -or (Want "lift") -or (Want "libsre") -or (Want "jobs")) {
    $py = Resolve-Python      # also exported as $env:PYTHON for the tools\*.sh scripts
    Write-Host "Python: $py"
}
if (Want "build") {
    Need (Test-Path $clang) "clang-cl not found at $clang -- install LLVM"
    Need (Test-Path $vswhere) "vswhere not found -- install Visual Studio 2022 Build Tools (C++)"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    Need $vs "No Visual Studio install with the C++ tools found"
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
}

# Native tools write progress and warnings to stderr. When this script's output is redirected
# (the launcher, a log), Windows PowerShell 5 turns each such line into an error record, which
# "Stop" would make fatal: from here on every step checks exit codes instead.
$ErrorActionPreference = "Continue"

if (Want "setup") {
    Step "setup (toolkit + patches)"
    # The first setup clones the toolkit: asked here, in this console (setup.ps1's own question would not show
    # through the pipe below), unless -Yes or IF2_TOOLKIT_DOWNLOAD=yes (the launcher, after its own question).
    $sa = @()
    if (-not (Test-Path "external\ps3recomp\.git") -and -not ($Yes -or $env:IF2_TOOLKIT_DOWNLOAD -eq "yes")) {
        $url = if ($env:IF2_TOOLKIT_URL) { $env:IF2_TOOLKIT_URL } else { "https://github.com/sp00nznet/ps3recomp.git" }
        Write-Host "This first build downloads the ps3recomp toolkit -- the open-source static recompiler and PS3 runtime"
        Write-Host "this port is built with (MIT licence; no game code), about 15 MB -- with git from"
        Write-Host "    $url"
        Write-Host "into external\ps3recomp (about 35 MB on disk), and its Python packages with pip."
        $ok = $false
        if (-not [Console]::IsInputRedirected -and [Environment]::UserInteractive) {
            $ok = (Read-Host "Download it now? [y/N]") -match '^\s*(y|yes)\s*$'
        }
        if (-not $ok) { Write-Host "Not downloaded; nothing was changed. Run again with -Yes to allow it."; exit 3 }
    }
    if ($Yes -or -not (Test-Path "external\ps3recomp\.git")) { $sa = @("-Yes") }
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools\setup.ps1 @sa 2>&1 | ForEach-Object { "$_" }
    Need ($LASTEXITCODE -eq 0) "setup failed"
}

if (Want "check") {
    Step "check inputs"
    $a = if ($Disc) { @("--disc") } else { @() }
    & $py tools\check_inputs.py @a 2>&1 | ForEach-Object { "$_" }
    Need ($LASTEXITCODE -eq 0) "check_inputs failed -- see game\README.md"
}

if (Want "analyze") {
    Step "analyze"
    & $bash tools/analyze.sh 2>&1 | ForEach-Object { "$_" }
    Need ($LASTEXITCODE -eq 0) "analyze.sh failed"
}

if (Want "lift") {
    Step "lift (writes ~200 MB of C++ to generated\)"
    & $bash tools/relift.sh 2>&1 | ForEach-Object { "$_" }
    Need ($LASTEXITCODE -eq 0) "relift.sh failed"
}

if (Want "libsre") {
    Step "lift libsre (the real SPURS library, from your firmware)"
    $inputs = Get-Content analysis\inputs.json -Raw | ConvertFrom-Json
    $prx = if ($inputs.libsre) { $inputs.libsre } elseif (Test-Path game\firmware\libsre.prx) { (Resolve-Path game\firmware\libsre.prx).Path } else { $null }
    if ($prx) {
        & $bash tools/lift_libsre.sh ($prx -replace '\\', '/') 2>&1 | ForEach-Object { "$_" }
        Need ($LASTEXITCODE -eq 0) "lift_libsre.sh failed"
    } else {
        Need (Test-Path generated\prx\libsre.linked.bin) "no libsre.prx (game\firmware\libsre.prx) -- see game\README.md"
        Write-Host "no libsre.prx given: keeping the libsre lifted earlier (generated\prx)"
    }
}

if (Want "jobs") {
    Step "lift the SPURS policy modules, job modules and job chains"
    # The four policy modules are byte copies from the EBOOT and libsre (no run of the game needed);
    # a capture from tools\run.ps1 -DumpSpu in analysis\pm works the same.
    & $py tools\extract_policy_modules.py 2>&1 | ForEach-Object { "$_" }
    Need ($LASTEXITCODE -eq 0) "extract_policy_modules.py failed -- the [x] lines above say why (for a European copy, beta: press Save report in the launcher and send the file)"
    # (located in the EBOOT being lifted: a title update moves them, and 1.04 rebuilt 16 of the 27 job modules)
    & $bash tools/lift_jobs.sh 2>&1 | ForEach-Object { "$_" }
    Need ($LASTEXITCODE -eq 0) "lift_jobs.sh failed"
}

if (-not (Want "build")) { Step "done"; exit 0 }

# --- build options (CMake cache) --------------------------------------------------
$opts = @()
foreach ($d in ($Define -split '[,;]' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
    Need ($d -match '^[A-Za-z_][A-Za-z0-9_]*=') "-Define wants NAME=VALUE[,NAME=VALUE...] (got '$d')"
    $opts += "-D$d"
}
if ($PgoScope) { $opts += "-DIF2_PGO_SCOPE=$PgoScope" }
if ($Pgo -eq "instrument") {
    # Raw profiles of an earlier instrumented build would not match this one.
    $old = @(Get-ChildItem build\pgo-raw -Filter *.profraw -ErrorAction SilentlyContinue)
    if ($old.Count -gt 0) {
        $stale = "pgo\stale-{0:yyyyMMdd-HHmmss}" -f (Get-Date)
        New-Item -ItemType Directory -Force $stale | Out-Null
        $old | Move-Item -Destination $stale
        Write-Host "moved $($old.Count) earlier raw profile(s) to $stale"
    }
    $opts += "-DIF2_PGO=instrument"
} elseif ($Pgo -eq "use") {
    Step "PGO: merge the training profiles"
    $profdata = Join-Path (Split-Path $clang) "llvm-profdata.exe"
    Need (Test-Path $profdata) "llvm-profdata not found next to clang-cl ($profdata)"
    New-Item -ItemType Directory -Force pgo | Out-Null
    $raw = @(Get-ChildItem build\pgo-raw -Filter *.profraw -ErrorAction SilentlyContinue)
    if ($raw.Count -gt 0) {
        & $profdata merge -o pgo\infamous2.profdata @($raw | ForEach-Object { $_.FullName })
        Need ($LASTEXITCODE -eq 0) "llvm-profdata merge failed"
        $keep = "pgo\raw-{0:yyyyMMdd-HHmmss}" -f (Get-Date)
        New-Item -ItemType Directory -Force $keep | Out-Null
        $raw | Move-Item -Destination $keep
        Write-Host "merged $($raw.Count) raw profile(s) -> pgo\infamous2.profdata (raw files: $keep)"
    } else {
        Need (Test-Path pgo\infamous2.profdata) "no profile: build with -Pgo instrument, run the training routes, then -Pgo use"
        Write-Host "no new build\pgo-raw\*.profraw: using the existing pgo\infamous2.profdata"
    }
    $opts += "-DIF2_PGO=use", "-DIF2_PGO_PROFILE=$((Resolve-Path pgo\infamous2.profdata).Path -replace '\\', '/')"
} elseif ($Pgo -eq "off") {
    $opts += "-DIF2_PGO=OFF"
}
$optline = ($opts | ForEach-Object { "`"$_`"" }) -join " "

Step "build (long: the lifted code is huge)"
$cl  = $clang -replace '\\', '/'
# The icon resource is compiled with LLVM's llvm-rc (next to clang-cl) rather than the Windows SDK's rc.exe, which
# stops with "RC1107: invalid usage" on some setups (issues #2, #5).
$llvmrc = Join-Path (Split-Path $clang) "llvm-rc.exe"
$rcopt = if (Test-Path $llvmrc) { "-DCMAKE_RC_COMPILER=" + ($llvmrc -replace '\\', '/') } else { "" }
# A build directory configured elsewhere (the folder was moved or copied) or with another resource compiler keeps
# stale paths and settings in its cache: drop the cache so CMake configures it afresh (objects are rebuilt as needed;
# nothing else in build\ is touched).
$cache = "build\CMakeCache.txt"
if (Test-Path $cache) {
    $home_ = (Select-String -Path $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$' | Select-Object -First 1)
    $rcnow = (Select-String -Path $cache -Pattern '^CMAKE_RC_COMPILER:FILEPATH=(.*)$' | Select-Object -First 1)
    $here  = (Get-Location).Path -replace '\\', '/'
    $moved = $home_ -and ($home_.Matches[0].Groups[1].Value.TrimEnd('/') -ne $here.TrimEnd('/'))
    $rcchg = $rcopt -and -not ($rcnow -and ($rcnow.Matches[0].Groups[1].Value -ieq ($llvmrc -replace '\\', '/')))
    if ($moved -or $rcchg) {
        if ($moved) { Write-Host "the folder moved: configuring the build directory again" }
        else { Write-Host "switching the resource compiler to llvm-rc: configuring the build directory again" }
        Remove-Item $cache -Force
        Remove-Item "build\CMakeFiles" -Recurse -Force -ErrorAction SilentlyContinue
    }
}
$bat = Join-Path $env:TEMP "infamous2_build_$PID.bat"     # per process: two builds never share it
# Paths and options reach the batch file as environment variables, not as its text: the file is ASCII (cmd reads
# it in the console code page), so a path with a letter outside ASCII written into it would arrive as '?'.
# cmd expands %VAR% from the (Unicode) environment unchanged.
$env:IF2_BUILD_VCVARS = $vcvars
$env:IF2_BUILD_CC = $cl
$env:IF2_BUILD_OPTS = ($optline + " " + $(if ($rcopt) { "`"$rcopt`"" } else { "" })).Trim()
@"
@echo off
call "%IF2_BUILD_VCVARS%" >nul || exit /b 1
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=%IF2_BUILD_CC%" "-DCMAKE_CXX_COMPILER=%IF2_BUILD_CC%" %IF2_BUILD_OPTS% || exit /b 1
cmake --build build || exit /b 1
"@ | Set-Content -Encoding ASCII $bat
cmd /c "`"$bat`" 2>&1"
$code = $LASTEXITCODE
Remove-Item $bat -ErrorAction SilentlyContinue
Need ($code -eq 0) "build failed -- paste the first 'error:' lines"
$mode = @(Get-Content build\CMakeCache.txt -ErrorAction SilentlyContinue |
          Where-Object { $_ -match '^IF2_(PPU_VM_INLINE|PPU_NO_TLS_GUARDS|PPU_ARCH_V3|PGO|PGO_SCOPE):' } |
          ForEach-Object { $_ -replace ':[A-Z]+=', '=' }) -join "  "
if ($mode) { Write-Host "build options: $mode" }
if ($Pgo -eq "instrument") {
    Write-Host "PGO training build: run the training routes with build\infamous2.exe (each run writes build\pgo-raw\if2-<pid>.profraw), then: tools\build.ps1 -BuildOnly -Pgo use" -ForegroundColor Yellow
}

Step "done"
Write-Host "Play it:  inFamous2Launcher.exe, or double-click build\infamous2.exe, or"
Write-Host "          powershell -ExecutionPolicy Bypass -File tools\run.ps1"

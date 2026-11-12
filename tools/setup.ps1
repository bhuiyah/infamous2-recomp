# Fetch the pinned ps3recomp toolkit into external\ and install its Python deps.
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File tools\setup.ps1 [-Yes]
# The first run clones the toolkit from GitHub (about 15 MB). It asks first; -Yes (or IF2_TOOLKIT_DOWNLOAD=yes, which
# the launcher sets once the player said Continue) allows it without asking. Without a console to ask on, and without
# either, it stops (exit code 3) and changes nothing. IF2_TOOLKIT_URL=<git url or folder> clones from there instead.
param([switch]$Yes)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
. (Join-Path $PSScriptRoot "python.ps1")
# Native tools write progress to stderr; with this output redirected, Windows PowerShell 5 would make
# each such line a terminating error under "Stop". Exit codes are checked instead.
$ErrorActionPreference = "Continue"

$pin  = (Get-Content ps3recomp.pin -Raw).Trim()
$dest = "external\ps3recomp"
function Git-Ok($what) { if ($LASTEXITCODE -ne 0) { throw "git: $what failed (exit $LASTEXITCODE)" } }

$url  = if ($env:IF2_TOOLKIT_URL) { $env:IF2_TOOLKIT_URL } else { "https://github.com/sp00nznet/ps3recomp.git" }

if (-not (Test-Path "$dest\.git")) {
    if (-not ($Yes -or $env:IF2_TOOLKIT_DOWNLOAD -eq "yes")) {
        Write-Host "This first setup downloads the ps3recomp toolkit -- the open-source static recompiler and PS3 runtime"
        Write-Host "this port is built with (MIT licence; no game code), about 15 MB -- with git from"
        Write-Host "    $url"
        Write-Host "into $dest (about 35 MB on disk), and its Python packages with pip."
        $ok = $false
        if (-not [Console]::IsInputRedirected -and [Environment]::UserInteractive) {
            $ok = (Read-Host "Download it now? [y/N]") -match '^\s*(y|yes)\s*$'
        }
        if (-not $ok) {
            Write-Host "Not downloaded; nothing was changed. Run again with -Yes to allow it."
            exit 3
        }
    }
    New-Item -ItemType Directory -Force external | Out-Null
    git clone $url $dest 2>&1 | ForEach-Object { "$_" }
    Git-Ok "clone of $url"
}
# Fetch only when the pinned commit is not here yet (re-runs work offline).
git -C $dest cat-file -e "$pin^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) {
    git -C $dest fetch --quiet origin 2>&1 | ForEach-Object { "$_" }
    Git-Ok "fetch"
}
git -C $dest checkout --quiet $pin
Git-Ok "checkout $pin"
git -C $dest log -1 --format="ps3recomp pinned at %h %cd" --date=short

# Local fixes to the toolkit, kept here until they land upstream. Reset tracked
# files and remove untracked ones (files the patch itself adds are left over by
# an earlier apply, and `git apply` refuses to create a file that exists) so
# re-running setup re-applies cleanly. Ignored files are kept.
git -C $dest checkout --quiet -- .
git -C $dest clean --quiet -fd
foreach ($p in @(Get-ChildItem patches\ps3recomp\*.patch -ErrorAction SilentlyContinue)) {
    git -C $dest apply --ignore-whitespace --whitespace=nowarn $p.FullName 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "failed to apply $($p.Name)" }
    Write-Host "applied $($p.Name)"
}

$py = Resolve-Python
Write-Host "Python: $py"
& $py -m pip install --disable-pip-version-check -r "$dest\requirements.txt" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "pip install -r $dest\requirements.txt failed" }
Write-Host ""
Write-Host "Next: put your decrypted EBOOT + disc files under game\ (see game\README.md),"
Write-Host "then run:  py tools\check_inputs.py"

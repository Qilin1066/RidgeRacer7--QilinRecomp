# Builds ridgeracer7 in one go from the source, as the other Qilin recomps'
# build scripts do: configures (the first time), lifts your copy's program
# to C++ and builds the game from it. Run it again after a `git pull`: only
# what changed is rebuilt.
#
#   powershell -ExecutionPolicy Bypass -File tools\build-ridgeracer7.ps1 [-Game <path>] [-Run] [-ChooseGame]
#
# It needs Visual Studio 2022 17.10 or newer (with "Desktop development with
# C++") and finds its CMake itself, from any window. -Game is your copy: the
# disc's folder (the one holding PS3_GAME), with the decrypted EBOOT.elf
# beside EBOOT.BIN in USRDIR (docs/elf-analysis.md says how to make it). With
# no -Game it uses the one it used last (build-game\game-path.txt), else asks
# with a folder picker. -Run starts the game after.
#
# The lifted code (build-game\gen) and the executable are derived from the
# game: they stay on this PC. The first build compiles some 14,000 functions
# and takes a good while.
#
# Keep this file ASCII-only and Windows PowerShell 5.1-compatible.
param(
    [string]$Game = "",
    [switch]$Run,
    [switch]$ChooseGame
)
$ErrorActionPreference = "Stop"
trap {   # any failure: say what in one red line, and end with an error code
    Write-Host ""
    Write-Host "Stopped: $_" -ForegroundColor Red
    exit 1
}
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build-game"
$gen = Join-Path $build "gen"
$gameFile = Join-Path $build "game-path.txt"
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows   # Windows PowerShell 5.1 has no $IsWindows

function Invoke-Step([string]$what, [string]$exe, [string[]]$arguments) {
    Write-Host "== $what" -ForegroundColor Cyan
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

# CMake: on the PATH, else the one Visual Studio installs, else the usual place.
function Add-Tool([string]$name, [string[]]$folders) {
    if (Get-Command $name -ErrorAction SilentlyContinue) { return $true }
    foreach ($f in $folders) {
        if ($f -and (Test-Path (Join-Path $f "$name.exe"))) {
            $env:PATH = "$f;$env:PATH"
            return $true
        }
    }
    return $false
}
if ($onWindows) {
    $vs = ""
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -property installationPath | Select-Object -First 1 }
    $ide = if ($vs) { Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft" } else { "" }
    $haveCmake = Add-Tool "cmake" @($(if ($ide) { Join-Path $ide "CMake\CMake\bin" }), (Join-Path $env:ProgramFiles "CMake\bin"))
    if (-not $vs) { throw "Visual Studio 2022 wasn't found. Install it (the free Community edition) with 'Desktop development with C++'." }
    if (-not $haveCmake) { throw "CMake wasn't found. In the Visual Studio Installer, add 'C++ CMake tools for Windows'." }
} elseif (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake isn't installed."
}

# Your copy of the game: the one used last, else ask.
if (-not $Game -and -not $ChooseGame -and (Test-Path $gameFile)) { $Game = (Get-Content $gameFile -Raw).Trim() }
if (-not $Game -or -not (Test-Path $Game)) {
    if ($Game) { Write-Host "$Game isn't there any more: choose your copy again." -ForegroundColor Yellow }
    if (-not $onWindows) { throw "give your copy of the game with -Game <path>" }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "Choose your copy of Ridge Racer 7: the disc's folder, the one holding PS3_GAME"
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { throw "no game chosen" }
    $Game = $dialog.SelectedPath
}

$exe = if ($onWindows) { ".exe" } else { "" }
function Find-Built([string]$name) {
    return @((Join-Path $build "Release/$name$exe"), (Join-Path $build "$name$exe")) | Where-Object { Test-Path $_ } | Select-Object -First 1
}

# The tools first: the lifted code doesn't exist yet, so the game isn't part of this configuration.
$configure = @("-S", $root, "-B", $build, "-DQILIN_GEN_DIR=")
if ($onWindows -and -not (Test-Path (Join-Path $build "CMakeCache.txt"))) { $configure += @("-G", "Visual Studio 17 2022", "-A", "x64") }
if (-not $onWindows) { $configure += "-DCMAKE_BUILD_TYPE=Release" }
Invoke-Step "Configuring" "cmake" $configure
Set-Content -Path $gameFile -Value $Game -Encoding UTF8
Invoke-Step "Building the qilin tool" "cmake" @("--build", $build, "--config", "Release", "--target", "qilin", "--parallel")
$qilin = Find-Built "qilin"
if (-not $qilin) { throw "built, but qilin$exe isn't where it should be" }

# Your copy's program, to C++.
Invoke-Step "Lifting your copy's program" $qilin @("lift", $Game, "--out", $gen)
Invoke-Step "Translating your copy's shaders" $qilin @("shaders", $Game, "--out", $gen)

# The game, from that.
Invoke-Step "Configuring the game" "cmake" @("-S", $root, "-B", $build, "-DQILIN_GEN_DIR=$gen")
Invoke-Step "Building the game (the first time takes a good while)" "cmake" @("--build", $build, "--config", "Release", "--target", "ridgeracer7", "--parallel")
$ridgeracer7 = Find-Built "ridgeracer7"
if (-not $ridgeracer7) { throw "built, but ridgeracer7$exe isn't where it should be" }

Write-Host ""
Write-Host "Built: $ridgeracer7" -ForegroundColor Green
Write-Host "It is made from your copy of the game: keep it on this PC."
if ($Run) {
    Write-Host "== Starting the game" -ForegroundColor Cyan
    & $ridgeracer7 $Game
    exit $LASTEXITCODE
}

# Builds ridgeracer7 in one go from the source, as the other Qilin recomps'
# build scripts do: configures (the first time), lifts your copy's program
# to C++ and builds the game from it. Run it again after a `git pull`: only
# what changed is rebuilt.
#
#   powershell -ExecutionPolicy Bypass -File tools\build-ridgeracer7.ps1 [-Game <path>] [-Run] [-Pull] [-Play] [-ChooseGame]
#                [-Compiler auto|clang|msvc]
#
# On Windows, "Play Ridge Racer 7.bat" and "Update Ridge Racer 7.bat" in the
# project folder run this for you: Play starts the game (building it first if
# it isn't built), Update gets the latest code from GitHub (-Pull), rebuilds
# what changed and starts the game.
#
# It needs Visual Studio 2022 17.10 or newer (with "Desktop development with
# C++") and finds its CMake itself, from any window. The game's code runs
# faster built with Clang (docs/toolchain.md): with Visual Studio's "C++ Clang
# Compiler for Windows" (Clang 19 or newer) installed, or LLVM's own installer,
# a new build uses it (clang-cl, through Ninja); -Compiler msvc or clang says
# which. A build folder made with one compiler stays with it: to change, delete
# build-game and build again. -Game is your copy: the
# disc's folder (the one holding PS3_GAME), with the decrypted EBOOT.elf
# beside EBOOT.BIN in USRDIR (docs/elf-analysis.md says how to make it). With
# no -Game it uses the one it used last (build-game\game-path.txt), else asks
# with a folder picker. -Run starts the game after; -Pull fetches the latest
# code first; -Play starts it straight away if it's already built.
#
# The lifted code (build-game\gen) and the executable are derived from the
# game: they stay on this PC. The first build compiles some 14,000 functions
# and takes a good while.
#
# Keep this file ASCII-only and Windows PowerShell 5.1-compatible.
param(
    [string]$Game = "",
    [switch]$Run,
    [switch]$Pull,     # get the latest code first (Update Ridge Racer 7.bat)
    [switch]$Play,     # start the game if it's built (Play Ridge Racer 7.bat); else build it, then start it
    [switch]$ChooseGame,
    [ValidateSet("auto", "clang", "msvc")][string]$Compiler = "auto"   # the game's compiler: auto takes Clang if it is installed
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

$exe = if ($onWindows) { ".exe" } else { "" }
function Find-Built([string]$name) {
    # (Visual Studio's build puts it in Release, Ninja's in the folder itself.)
    return @((Join-Path $build "Release/$name$exe"), (Join-Path $build "$name$exe")) | Where-Object { Test-Path $_ } | Select-Object -First 1
}

if ($Play) {
    $built = Find-Built "ridgeracer7"
    if ($built -and (Test-Path $gameFile)) {
        $last = (Get-Content $gameFile -Raw).Trim()
        Start-Process -FilePath $built -ArgumentList "`"$last`"" -WorkingDirectory (Split-Path -Parent $built)
        exit 0
    }
    Write-Host "Ridge Racer 7 isn't built yet: building it now. The first build takes a long while."
    $Run = $true
}

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

# The compiler. A configured build folder keeps the one it was made with; a new one takes Clang (clang-cl, 19 or
# newer) if it is installed, from Visual Studio or LLVM's installer, else Visual Studio's own.
function Find-ClangCl {
    $candidates = @()
    if ($vs) { $candidates += Join-Path $vs "VC\Tools\Llvm\x64\bin\clang-cl.exe" }
    if ($env:ProgramFiles) { $candidates += Join-Path $env:ProgramFiles "LLVM\bin\clang-cl.exe" }
    $onPath = Get-Command clang-cl -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    foreach ($c in $candidates) {
        if (-not (Test-Path $c)) { continue }
        $version = (& $c --version 2>$null | Select-Object -First 1)
        if ($version -match "clang version (\d+)\." -and [int]$Matches[1] -ge 19) { return $c }
    }
    return ""
}
$cache = Join-Path $build "CMakeCache.txt"
$madeWith = ""   # the compiler an existing build folder was set up for
$madeClang = ""
if (Test-Path $cache) {
    $generator = Select-String -Path $cache -Pattern "^CMAKE_GENERATOR:\w+=(.*)$" | Select-Object -First 1
    $made = Select-String -Path $cache -Pattern "^CMAKE_CXX_COMPILER:\w+=(.*)$" | Select-Object -First 1
    if ($generator -and $generator.Matches[0].Groups[1].Value -match "^Visual Studio") {
        $madeWith = "msvc"   # (this script sets up Visual Studio's builds with its own compiler)
    } elseif ($made -and $made.Matches[0].Groups[1].Value -match "clang") {
        $madeWith = "clang"
        $madeClang = $made.Matches[0].Groups[1].Value
    } elseif ($made) {
        $madeWith = "msvc"
    }
}
$clangCl = ""
if ($onWindows) {
    if ($madeWith -and $Compiler -ne "auto" -and $Compiler -ne $madeWith) {
        throw "build-game was set up for $madeWith. To build with $Compiler, delete the folder build-game (everything in it is made again)."
    }
    $use = if ($madeWith) { $madeWith } elseif ($Compiler -ne "auto") { $Compiler } else { "" }
    if ($madeWith -eq "clang") {
        if (-not (Test-Path $madeClang)) { throw "build-game was set up with $madeClang, which isn't there any more: delete the folder build-game and build again." }
        $clangCl = $madeClang
    } elseif ($use -ne "msvc") {
        $clangCl = Find-ClangCl
        if (-not $clangCl -and $use -eq "clang") {
            throw "Clang 19 or newer wasn't found. In the Visual Studio Installer, add 'C++ Clang Compiler for Windows' (or install LLVM)."
        }
    }
    if ($clangCl) {
        # Ninja and the developer environment (the Windows SDK's headers and libraries) for clang-cl.
        if (-not (Add-Tool "ninja" @($(if ($ide) { Join-Path $ide "CMake\Ninja" })))) {
            throw "Ninja wasn't found (Visual Studio's 'C++ CMake tools for Windows' brings it)."
        }
        $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
        if (-not (Test-Path $vcvars)) { throw "Visual Studio's vcvars64.bat isn't there ($vcvars)." }
        foreach ($line in (& cmd.exe /c "`"$vcvars`" >nul && set")) {
            if ($line -match "^([^=]+)=(.*)$") { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
        }
        Write-Host "Compiler: Clang ($clangCl)"
    } else {
        Write-Host "Compiler: Visual Studio's"
    }
}

if ($Pull) {
    Write-Host "== Getting the latest code" -ForegroundColor Cyan
    & git -C $root pull --ff-only
    if ($LASTEXITCODE -ne 0) { throw "git pull failed (exit code $LASTEXITCODE). If you changed files in the project folder, that's why." }
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

# The tools first: the lifted code doesn't exist yet, so the game isn't part of this configuration.
$configure = @("-S", $root, "-B", $build, "-DQILIN_GEN_DIR=")
if ($onWindows -and -not (Test-Path $cache)) {
    if ($clangCl) {
        $configure += @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=$clangCl", "-DCMAKE_CXX_COMPILER=$clangCl")
        $lld = Join-Path (Split-Path -Parent $clangCl) "lld-link.exe"
        if (Test-Path $lld) { $configure += "-DCMAKE_LINKER=$lld" }
    } else {
        $configure += @("-G", "Visual Studio 17 2022", "-A", "x64")
    }
}
if (-not $onWindows) { $configure += "-DCMAKE_BUILD_TYPE=Release" }
Invoke-Step "Configuring" "cmake" $configure
Set-Content -Path $gameFile -Value $Game -Encoding UTF8
Invoke-Step "Building the qilin tool" "cmake" @("--build", $build, "--config", "Release", "--target", "qilin", "--parallel")
$qilin = Find-Built "qilin"
if (-not $qilin) { throw "built, but qilin$exe isn't where it should be" }

# Your copy's program, to C++.
Invoke-Step "Lifting your copy's program" $qilin @("lift", $Game, "--out", $gen)
Invoke-Step "Translating your copy's shaders" $qilin @("shaders", $Game, "--out", $gen)

# How many of the game's files to compile at once: one per core, but no more
# than the memory free now holds. The heaviest file (a lifted SPU program)
# takes Visual Studio's compiler 4 GB measured alone, but four at once with 5 GB
# counted each ran a 22 GB-free PC critically low: 8 GB are counted for each,
# and 2 GB kept back. Clang took a quarter of Visual Studio's memory or less on
# the same files (docs/toolchain.md): 3 GB are counted for each. Visual Studio's
# count is given through _CL_ (which comes after the project's own /MP) at build
# time, so a different count never rebuilds anything; Ninja's is its own.
$jobs = 1
$perJobGB = if ($clangCl) { 3 } else { 8 }
if ($onWindows) {
    $cores = [Environment]::ProcessorCount
    $os = Get-CimInstance Win32_OperatingSystem
    $memoryGB = [int][math]::Floor($os.TotalVisibleMemorySize / 1MB)
    $freeGB = [int][math]::Floor($os.FreePhysicalMemory / 1MB)
    $jobs = [int][math]::Max(1, [math]::Min([int]$cores, [int][math]::Floor(($freeGB - 2) / $perJobGB)))
    Write-Host "Compiling $jobs file(s) at once ($cores cores; $freeGB of $memoryGB GB of memory free)"
    if (-not $clangCl) { $env:_CL_ = "/MP$jobs" }
}

# The game, from that.
Invoke-Step "Configuring the game" "cmake" @("-S", $root, "-B", $build, "-DQILIN_GEN_DIR=$gen")
$parallel = if ($clangCl) { "$jobs" } else { "1" }
Invoke-Step "Building the game (the first time takes a long while)" "cmake" @("--build", $build, "--config", "Release", "--target", "ridgeracer7", "--parallel", $parallel)
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

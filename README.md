# RidgeRacer7--QilinRecomp

A static recompilation of **Ridge Racer 7** (Namco Bandai Games, PlayStation 3, 2006) into portable C++23 with a Vulkan 1.3 renderer, for 64-bit Windows, Linux and macOS on x86-64 or ARM64. Any GPU with a Vulkan 1.3 driver is in scope, down to the Raspberry Pi 4 (Mesa 24.3+). Macs run it through MoltenVK (macOS 10.15 or later).

> **No game data is included, and none ever will be; nor is any PS3 key.** You need your own copy of the game: the USA disc, `BLUS30001`, version 01.02. The tools read it from a path you give them.
>
> This is an independent project, not affiliated with or endorsed by Bandai Namco or Sony. *Ridge Racer* is Bandai Namco's trademark and *PlayStation* is Sony's.

## Status

Runs from start-up through its logos, title film and menus to its first race, with the 3D world drawn, music, films, a controller and saves. Some drawing is still wrong, there is no settings menu, and it has only been built and run on Windows.

| Part | State |
|---|---|
| Reading your copy | ✅ disc folder, `EBOOT.BIN` header, `EBOOT.elf`, `PARAM.SFO`, `RR7.DAT`'s 12,810 files (`docs/elf-analysis.md`) |
| PPU decoder and control flow | ✅ 383 instructions, matching objdump; 14,294 functions, 0 unknown words |
| PPU lifter | 🔨 all 14,294 functions lift, 0 differences in 1,404,384 test runs; exception landing pads as entries still to do (`docs/recompiler-core.md`) |
| SPU decoder and control flow | ✅ 199 instructions, matching objdump; 42 programs, 2,332 functions, 0 unknown words |
| SPU lifter | ✅ all 2,332 functions lift, 0 differences in 374,784 test runs |
| RSX shaders | ✅ all 242 translated to SPIR-V at build time and passed by `spirv-val` |
| Renderer (RSX on Vulkan 1.3) | 🔨 menus and race drawn; a grey patch over the near road (`docs/runtime.md`) |
| System libraries | 🔨 enough to reach a race; anything missing stops the game by name |
| Films and sound | ✅ MPEG-2 and ATRAC3plus through FFmpeg's decoders |
| Controller | ✅ gamepad or keyboard, triggers with pressure |
| Thread scheduling | 🔨 priorities not honoured yet |
| Settings menu (F1) | ⏳ |
| CI | ✅ six configurations, warnings as errors; the game itself needs your copy, so isn't built there |

## Playing

### Your copy of the game

Keep your disc's folder (the one holding `PS3_GAME`) outside this repository. Its `EBOOT.BIN` is encrypted, so decrypt it yourself, once:

1. In [RPCS3](https://rpcs3.net): **Utilities > Decrypt PS3 Binaries**, and choose your `EBOOT.BIN`. It writes `EBOOT.elf`.
2. Put `EBOOT.elf` beside `EBOOT.BIN`, in `USRDIR`. `qilin` refuses an ELF that isn't that `EBOOT.BIN`'s program.

The game also needs the console's fonts, from the `dev_flash` folder of a PS3's system software (`rpcs3 --installfw PS3UPDAT.PUP` makes one, from Sony's file). Pass it with `--flash <dev_flash>`; without it, an `RPCS3` folder beside your game's folder is looked in.

### From the source on Windows

You need Visual Studio 2022 17.10 or newer with **Desktop development with C++** and **C++ CMake tools for Windows**.

- **`Play Ridge Racer 7.bat`** starts the game, building it from your copy first if needed (the first build takes a long while).
- **`Update Ridge Racer 7.bat`** gets the latest code, rebuilds what changed and starts the game.
- **`Analyse game.bat`** builds `qilin` and writes what it reads from your copy to `analysis-out\`.

Each asks for your disc's folder the first time. Play and Update run:

```
powershell -ExecutionPolicy Bypass -File tools\build-ridgeracer7.ps1 [-Game <path>] [-Run] [-Pull] [-Play] [-ChooseGame]
```

It lifts your copy into `build-game\gen` and builds `build-game\Release\ridgeracer7.exe`, compiling as many files at once as free memory allows (8 GB each).

### Linux and macOS

Not yet tried. By hand, after the build below:

```sh
build/qilin lift    <game> --out build/gen
build/qilin shaders <game> --out build/gen
cmake -S . -B build -DQILIN_GEN_DIR="$PWD/build/gen"
cmake --build build --parallel 4 --target ridgeracer7
build/ridgeracer7 <game> [--flash <dev_flash>]
```

### Controls and saves

A gamepad works as the DualShock 3 in the first port. On the keyboard:

| Key | Button | Key | Button |
|---|---|---|---|
| Arrows | D-pad | Q / W | L1 / R1 |
| Z | Cross | 1 / 2 | L2 / R2 |
| X | Circle | Return | Start |
| A | Square | Backspace | Select |
| S | Triangle | | |

Saves go in `qilin/ridgeracer7` in your per-user application data, or in `RIDGERACER7_SAVE_DIR`.

## Building

The whole project is **C++23**. You need CMake 3.25 or newer and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops with a clear message if the compiler is older or the target is 32-bit. It fetches and pins SDL3, the Vulkan headers, volk, FreeType and part of FFmpeg's libavcodec (`cmake/ffmpeg.cmake`). On Linux, SDL3 needs the X11, Wayland and sound development packages listed in `.github/workflows/ci.yml`. The checks need Python 3.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

With Visual Studio, add `--config Release` and `-C Release`; the programs land in `build\Release\`. `-DQILIN_WERROR=ON` makes warnings errors.

CI builds and tests on Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64, and Windows x64 with warnings as errors, and runs both decoder sweeps.

## Using it

`<game>` is your copy: the disc's folder, `PS3_GAME` or `USRDIR` inside it, `EBOOT.BIN`, or `EBOOT.elf`.

```sh
build/qilin --version
build/qilin info    <game> [--json info.json]   # the disc's description, EBOOT.BIN's plain header, the program
build/qilin imports <game>                      # every system library imported and each function id taken from it
build/qilin exports <game>                      # what the program offers the system
build/qilin sfo     <game>                      # every entry of PARAM.SFO
build/qilin id      <name>                      # the id a system function of that name has
build/qilin archive <game> [--files]            # RR7.DAT's directory, read from the program
build/qilin data    <game>                      # the game's own files; the sound banks' and films' headers
build/qilin shaders <game> [--disasm] [--out folder]   # the compiled shader programs; their SPIR-V translation
build/qilin spu     <game>                      # the SPU programs the program carries
build/qilin cfg     <game>                      # the control-flow pass: functions, switches, system calls, coverage
build/qilin spu-cfg     <game>                  # the SPU programs' control flow: functions, coverage, channels
build/qilin spu-insns   <game>                  # every word of the SPU programs' code
build/qilin spu-profile <game>                  # how often the SPU programs use each instruction
build/qilin spu-disasm  <file> [--base address] [--ignore-reserved]   # a file of raw SPU words
build/qilin lift    <game> [--out folder] [--pc] [--registers]   # lift the PPU and SPU code to C++
build/qilin insns   <game>                      # every word of the program's code: address, word, instruction
build/qilin profile <game>                      # how often the code uses each instruction
build/qilin disasm  <file> [--base address]     # a file of raw big-endian PPU words
```

With only `EBOOT.BIN` there, `info` prints its plain header and says how to make `EBOOT.elf`. Everything `qilin` writes about your copy is derived from the game: keep it out of the repository.

### Tests

`ctest` runs the unit tests (on files built in memory: no game files needed), the host layer's tests, and the differential tests `qilin_difftest` and `qilin_spu_difftest`, which lift random encodings and compare the lifted code with a separately written reference interpreter.

### Decoder checks

```sh
python3 tools/check/verify_decoder.py     --qilin build/qilin --sweep
OBJDUMP_SPU=<spu-elf-objdump> python3 tools/check/verify_spu_decoder.py --qilin build/qilin --sweep
```

Each proves a decoder identical to GNU objdump (PowerPC; binutils 2.31.1 for the SPU) over a sweep of the encoding space. `--game <game>` checks the game's code instead.

### Shader checks

With the game built, `ridgeracer7_shader_check` builds a pipeline for every translated shader on the local Vulkan driver, and `python3 tools/check/validate_shaders.py build/gen --spirv-val <spirv-val>` runs Khronos' validator over them.

### Test runs

`ridgeracer7` takes `RR7_...` environment settings for headless, timed and logged runs (`RR7_QUIET=1`, `RR7_ANSWER`, `RR7_PRESS`, `RR7_SECONDS`, `RR7_SAVE_FRAME`, `RR7_DRAWLOG` and others), described in `docs/runtime.md`.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | The file readers (`elf`, `self`, `sfo`, `title`, `media`, `archive`), the decoders (`ppu`, `spu`), control-flow passes (`cfg`, `spu_cfg`), lifters (`lift`, `spu_lift`), the shader translation (`rsx_program`, `rsx_spirv`), `main` (the `qilin` CLI) |
| `recomp/tests/` | Unit tests; `difftest/` and `spu_difftest/`, the lifters against reference interpreters |
| `runtime/` | What lifted code compiles against: CPU state, floating point, VMX, the SPU, guest memory, error reports |
| `ridgeracer7/src/` | The game's machine and the system it calls, one file per part (threads, files, RSX and renderer, sound, films, SPURS, controller, saves) |
| `host/` | Window and input on SDL3, Vulkan 1.3 through volk, presentation, sound output |
| `cmake/` | `ffmpeg.cmake`: the parts of libavcodec the game needs |
| `tools/` | `build-ridgeracer7.ps1`, `analyse-game.ps1`, `build_shaders.py`; `check/` (decoder and shader checks) |
| `docs/` | Roadmap and design targets, the program's analysis, the data files, the recompiler's core, the runtime, prior art, naming |

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. A minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the whole game.

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0): you may use, share, change and sell it, but anything you distribute that is built from it must come with its full source under the same licence. The libraries CMake fetches keep their own licences; FFmpeg's libavcodec is LGPL-2.1-or-later.

The game itself is Bandai Namco's and is not covered by anything here: you need your own copy, and nothing built from it may be shared.

## Design targets

- **Efficiency and reliability come first.** Cheaper to run on low-end hardware, and failing loudly rather than misbehaving quietly.
- **Static recompilation, ahead of time:** PPU and SPU code → C++, RSX shaders → SPIR-V. No interpreter and no JIT. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only**, with a small baseline (1.3 core plus what the Pi 4's driver offers), so one render path runs everywhere.
- **The system is replaced natively at its API:** every imported function and system call. What is linked into the program (engine, C and C++ libraries, the graphics library's command writers) is lifted with it.
- **Ground truth is RPCS3** running the original game. Its code is not copied.

See [`docs/`](docs/) for the roadmap and the analysis notes.

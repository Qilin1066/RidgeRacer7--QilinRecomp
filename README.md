# RidgeRacer7--QilinRecomp

A static recompilation of **Ridge Racer 7** (Namco Bandai Games, PlayStation 3, 2006) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. Anything with a Vulkan 1.3 driver is in scope, including the Raspberry Pi 4 (Mesa 24.3+); every 64-bit Mac with a Metal GPU that runs macOS 10.15 (Catalina) or later, Intel or Apple Silicon, is covered through MoltenVK. Efficiency is the first priority: a Raspberry Pi 4–class board is our reference for typical low-end hardware, not a floor. The requirements and scope are the same as the other Qilin recomps' ([GBTGhoulies](https://github.com/Qilin1066/GBTGhoulies--QilinRecomp), [BeatTheBeat](https://github.com/Qilin1066/RhythmHeavenFever--QilinRecomp), [PunchOutWii](https://github.com/Qilin1066/PunchOutWii--QilinRecomp), [WindWakerHD](https://github.com/Qilin1066/WindWakerHD--QilinRecomp)), down to the settings menu.

> **No game data is included, and none ever will be; nor is any PS3 key.** You need your own copy of the game: the USA disc, `BLUS30001`, version 01.02. The tools read the program and the disc's files from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repo.
>
> This is an independent project, not affiliated with or endorsed by Bandai Namco or Sony. *Ridge Racer* is Bandai Namco's trademark and *PlayStation* is Sony's.

## Status

**Early. The game's code is lifted to C++ and runs natively through its start-up, sets the RSX up and opens its font, as far as the start of SPURS, where it stops, by name, at the first system function the runtime doesn't provide yet. Nothing is on screen.**

| Piece | State |
|---|---|
| The game found however you hold it: the disc's folder, `EBOOT.BIN`, or the decrypted `EBOOT.elf` | ✅ |
| `PARAM.SFO` (the disc's description of the game) | ✅ read from the owner's disc |
| `EBOOT.BIN`'s plain header (the SELF): the program's segments and sections, the SDK libraries it was linked from. Nothing is decrypted | ✅ read from the owner's disc (`docs/elf-analysis.md`): 4.25 MB of code, 14,046 function descriptors, 248 functions imported from 22 system libraries, SPURS and about 1 MB that looks like SPU programs |
| The decrypted program (ELF): segments, sections, the entry point's descriptor, process and PRX parameters, imports with their stubs, exports, thread-local storage | ✅ read from the owner's program with nothing to fix (`docs/elf-analysis.md`): all 248 imports with their stubs and their names, 78 system calls made directly, and **42 SPU programs** carried in its data (the car's physics, collision, particles, sound) |
| CI: Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64, Windows x64, warnings as errors | ✅ |
| PPU decoder: 64-bit PowerPC with VMX, 383 instructions (`docs/recompiler-core.md`) | ✅ identical to GNU objdump over a 4.9-million-word sweep of the encoding space, in CI; on the game's code 99.47% of 1,065,028 words decode and the rest are jump tables and constants; the game uses 151 of the instructions |
| The data files: the sound banks' and films' headers, a survey of the archive (`docs/data-files.md`) | ✅ read from the owner's disc: MPEG-2 films at 1280 × 720, ATRAC3plus music in 5.1, PCM speech; the archive's directory is in the program, and read from it: 12,810 files in 2,088 packs, matching `RR7.DAT` to the byte; the 242 compiled shaders are in the program too |
| Control-flow pass for the PPU code (`docs/recompiler-core.md`) | ✅ on the game: 14,294 functions, 296 switches, 2,845 exception landing pads, all 502 system-call sites numbered; **every one of 1,065,028 words accounted for, none unknown** |
| SPU decoder: 199 instructions (`docs/recompiler-core.md`) | ✅ identical to objdump (binutils 2.31.1, built by CI) over a 1.66-million-word sweep; on the game's 42 SPU programs all but a four-word jump table decode |
| SPU control-flow pass (`docs/recompiler-core.md`) | ✅ on the game's 42 SPU programs: 2,332 functions, every one of 209,492 words accounted for |
| PPU lifter: integers, loads and stores, branches, calls, switches, floating point bit-exact, VMX (`docs/recompiler-core.md`) | 🔨 all 14,294 of the game's functions lift (its 1,059,387 instructions); identical to a separately written, integer-only reference over 1.4 million runs of the differential test, in CI |
| SPU lifter (`docs/recompiler-core.md`), with the SPU's own floating point (which is not IEEE's) done exactly | 🔨 all 2,332 of the 42 programs' functions lift; identical to a separately written reference over 374,784 runs of its own differential test, in CI. SPU threads and SPURS tasks run them |
| The RSX's vertex and fragment programs: a decoder, and their translation to SPIR-V at build time (`docs/recompiler-core.md`) | 🔨 all 242 of the game's programs, 7,702 instructions, decode and agree with their own Cg tables, and translate; the local Vulkan driver takes all 242 and Khronos' validator passes them. Still owed: a test of their meaning |
| The rest of the lifters (the status-register functions, exception landing pads) | later |
| Runtime skeleton (`docs/runtime.md`): the program loaded at its addresses, the first thread, imports and system calls replaced by name and number, anything missing stops the game naming it | 🔨 the game builds from the owner's copy and runs through its start-up, the RSX's set-up and its first commands, and its font, to the start of SPURS (`cellSpursInitialize`); 67 of 248 imports and 40 of 78 system calls provided |
| The host layer (`host/`): window and input on SDL3, Vulkan 1.3 through volk, presentation, frame times: the other Qilin recomps' | ✅ built and tested; the game doesn't draw into it yet |
| The system libraries and system calls themselves; the RSX's command stream on Vulkan 1.3; sound, controllers, saves, films | later |
| The settings menu (F1), the same as the other Qilin recomps' | later, with the window |

`docs/roadmap-and-design-targets.md` has the plan and every design decision, with the open ones marked.

## Versions and changes

Releases are `vX.Y.Z`, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). Past entries are kept, never rewritten. The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. While the recompiler is being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game.

## Your copy of the game

Keep your disc's files in a folder of their own, outside this repository: the folder holding `PS3_GAME`.

The game's program is `PS3_GAME/USRDIR/EBOOT.BIN`, and on the disc it is encrypted. This project has no PS3 keys, so you make the decrypted program yourself, once:

1. In [RPCS3](https://rpcs3.net): **Utilities > Decrypt PS3 Binaries**, and choose your `EBOOT.BIN`. It writes `EBOOT.elf`.
2. Put `EBOOT.elf` beside `EBOOT.BIN`, in `USRDIR`.

`qilin` checks that the ELF is that `EBOOT.BIN`'s program, and refuses one that isn't.

## Building

The whole project is **C++23**. You need CMake ≥ 3.25 and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops with a clear message if the compiler is older, or if the target is 32-bit. Today's tools have no other dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

On Windows, double-click **`Play Ridge Racer 7.bat`** to build the game from your own copy (the first time: a long while) and start it, and **`Update Ridge Racer 7.bat`** to get the latest code, rebuild what changed and start it. Both ask for your disc's folder the first time; what they build from your copy (`build-game\`) stays on your PC. They compile as many files at once as your free memory allows (5 GB counted for each). **`Analyse game.bat`** builds `qilin`, asks for your disc's folder, and writes what it reads to `analysis-out\` (never committed).

## Using it

`<game>` is your copy: the disc's folder, `PS3_GAME` or `USRDIR` inside it, `EBOOT.BIN`, or `EBOOT.elf`.

```sh
build/qilin info    <game> [--json info.json]   # the disc's description, EBOOT.BIN's plain header, the program
build/qilin imports <game>                      # every system library imported and each function id taken from it
build/qilin exports <game>                      # what the program offers the system
build/qilin sfo     <game>                      # every entry of PARAM.SFO
build/qilin archive <game> [--files]            # RR7.DAT's directory, read from the program
build/qilin shaders <game>                      # the compiled shader programs the program carries
build/qilin data    <game>                      # the game's own files; the sound banks' and films' headers
build/qilin spu     <game>                      # the SPU programs the program carries
build/qilin cfg     <game>                      # the control-flow pass: functions, switches, system calls, coverage
build/qilin spu-profile <game>                  # how often the SPU programs use each instruction (spu-insns: every word)
build/qilin lift    <game> [--out folder]       # lift the PPU code to C++; how much the lifter covers
build/qilin insns   <game>                      # every word of the program's code: address, word, instruction
build/qilin profile <game>                      # how often the code uses each instruction
build/qilin disasm  <file> [--base address]     # a file of raw big-endian PPU words
```

With only `EBOOT.BIN` there, `info` prints what its header leaves plain and then stops, saying how to make `EBOOT.elf`. Everything `qilin` writes about your copy is derived from the game: keep it out of the repository.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | `elf` (the PS3 program loader), `self` (`EBOOT.BIN`'s plain header), `sfo` (`PARAM.SFO`), `title` (finds the game's files; the known builds), `media` (the sound banks' and films' headers), `spu_image` (the SPU programs inside the program), `import_names` (the imported functions' names), `sha256`, `ppu` (the PPU decoder), `spu` (the SPU decoder), `spu_cfg` (the SPU control-flow pass), `cfg` (the control-flow pass), `lift` (the PPU lifter), `archive` (RR7.DAT's directory), `shaders` (the compiled Cg programs), `main` (the `qilin` CLI) |
| `recomp/tests/` | Unit tests on a program, a SELF and a `PARAM.SFO` built in memory, so they need no game files; `difftest/`, the lifter against a reference interpreter |
| `runtime/` | What lifted code compiles against: the CPU state and its helpers (`cpu.hpp`, `fp.hpp`, `vmx.hpp`), the guest's address space (`memory`), messages and stopping (`report`) |
| `ridgeracer7/src/` | The game's machine: memory, the first thread, the imports and system calls provided natively |
| `tools/` | `build-ridgeracer7.ps1` (builds the game from your copy); `analyse-game.ps1`, behind `Analyse game.bat`; `check/verify_decoder.py` and `check/verify_spu_decoder.py` (the decoders against GNU objdump) |
| `docs/` | Roadmap and design targets, the program's analysis, the data files, the recompiler's core, the runtime, prior art, naming |

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE), as all the QilinRecomp projects are: anyone may use, share, change and sell it, but whatever they distribute that is built from it must come with its full source under the same licence.

The game itself is Bandai Namco's and is not covered by anything here: you need your own copy, and nothing built from it may be shared.

## Design targets (short version)

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, ahead of time, of everything the console runs:** the PPU's PowerPC code → C++, the SPU programs → C++, the RSX's shaders → SPIR-V. No interpreter, no JIT, no emulator underneath. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only.** The RSX's command stream is translated to Vulkan, with a deliberately small baseline feature set (1.3 core plus what the Pi 4's driver offers) so one render path runs on everything from low-end ARM GPUs upwards. macOS runs through MoltenVK.
- **The system is replaced natively at its API:** every imported library function and every system call. What is linked into the program (the game, its engine, the C and C++ libraries, the graphics library's command writers) is lifted with it, for exact behaviour.
- **Ground truth is RPCS3** running the original game. Its code is not copied.

See `docs/` for the full roadmap and the analysis notes.

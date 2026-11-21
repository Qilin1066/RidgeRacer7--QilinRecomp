# RidgeRacer7--QilinRecomp

A static recompilation of **Ridge Racer 7** (Namco Bandai Games, PlayStation 3, 2006) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. Anything with a Vulkan 1.3 driver is in scope, including the Raspberry Pi 4 (Mesa 24.3+); every 64-bit Mac with a Metal GPU that runs macOS 10.15 (Catalina) or later, Intel or Apple Silicon, is covered through MoltenVK. Efficiency is the first priority: a Raspberry Pi 4–class board is our reference for typical low-end hardware, not a floor. The requirements and scope are the same as the other Qilin recomps' ([GBTGhoulies](https://github.com/Qilin1066/GBTGhoulies-QilinRecomp), [BeatTheBeat](https://github.com/Qilin1066/BeatTheBeat-QilinRecomp), [PunchOutWii](https://github.com/Qilin1066/PunchOutWii-QilinRecomp), [WindWakerHD](https://github.com/Qilin1066/WindWakerHD-QilinRecomp)), down to the settings menu.

> **No game data is included, and none ever will be; nor is any PS3 key.** You need your own copy of the game: the USA disc, `BLUS30001`, version 01.02. The tools read the program and the disc's files from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repo.
>
> This is an independent project, not affiliated with or endorsed by Bandai Namco or Sony. *Ridge Racer* is Bandai Namco's trademark and *PlayStation* is Sony's.

## Status

**The very start: the tools that read the game. Nothing runs yet.**

| Piece | State |
|---|---|
| The game found however you hold it: the disc's folder, `EBOOT.BIN`, or the decrypted `EBOOT.elf` | ✅ |
| `PARAM.SFO` (the disc's description of the game) | ✅ read from the owner's disc |
| `EBOOT.BIN`'s plain header (the SELF): the program's segments and sections, the SDK libraries it was linked from. Nothing is decrypted | ✅ read from the owner's disc (`docs/elf-analysis.md`): 4.25 MB of code, 14,046 function descriptors, 248 functions imported from 22 system libraries, SPURS and about 1 MB that looks like SPU programs |
| The decrypted program (ELF): segments, sections, the entry point's descriptor, process and PRX parameters, imports with their stubs, exports, thread-local storage | ✅ on synthetic files; **not yet run on the real program** (it has to be decrypted first, below) |
| CI: Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64, Windows x64, warnings as errors | ✅ |
| PPU decoder (64-bit PowerPC with VMX), checked against GNU binutils | next |
| SPU decoder; control-flow passes; lifters to C++; differential tests | later |
| Runtime: the system libraries and system calls replaced natively; the RSX's command stream on Vulkan 1.3; sound, controllers, saves, films | later |
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

On Windows, double-click **`Analyse game.bat`**: it builds `qilin`, asks for your disc's folder, and writes what it reads to `analysis-out\` (never committed).

## Using it

`<game>` is your copy: the disc's folder, `PS3_GAME` or `USRDIR` inside it, `EBOOT.BIN`, or `EBOOT.elf`.

```sh
build/qilin info    <game> [--json info.json]   # the disc's description, EBOOT.BIN's plain header, the program
build/qilin imports <game>                      # every system library imported and each function id taken from it
build/qilin exports <game>                      # what the program offers the system
build/qilin sfo     <game>                      # every entry of PARAM.SFO
```

With only `EBOOT.BIN` there, `info` prints what its header leaves plain and then stops, saying how to make `EBOOT.elf`. Everything `qilin` writes about your copy is derived from the game: keep it out of the repository.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | `elf` (the PS3 program loader), `self` (`EBOOT.BIN`'s plain header), `sfo` (`PARAM.SFO`), `title` (finds the game's files; the known builds), `sha256`, `main` (the `qilin` CLI) |
| `recomp/tests/` | Unit tests on a program, a SELF and a `PARAM.SFO` built in memory, so they need no game files |
| `tools/` | `analyse-game.ps1`, behind `Analyse game.bat` |
| `docs/` | Roadmap and design targets, the program's analysis, prior art, naming |

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

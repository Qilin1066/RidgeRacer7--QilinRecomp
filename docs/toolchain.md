# The toolchain: how the game is compiled for speed

Which compiler builds the lifted code, with what options, and why: the measurements behind `cmake/toolchain.cmake` and
`tools/build-ridgeracer7.ps1`'s choice of compiler. Item 3 of `docs/efficiency-research.md` asked for Clang (clang-cl on
Windows), ThinLTO, profile-guided optimisation and the Raspberry Pi's processor flags; this page is what was measured on
the owner's PC on 2026-11-28 and what was made of it.

## What a build gets

| Setting | Where | What |
| --- | --- | --- |
| Floating point | every build | `-ffp-contract=off` for GCC and Clang, clang-cl included (it was skipped for clang-cl before, as CMake calls it MSVC). MSVC doesn't contract under `/fp:precise`. No `-ffast-math`: the relaxed mode for play is the runtime's own switch, not the compiler's. |
| SSSE3 | x86-64 with GCC or Clang | `-mssse3`, as MSVC's build assumes (`spu.hpp`'s byte shuffles); `spu.cpp` checks it before the first SPU program runs. |
| `QILIN_CPU` | a preset | `pi4`: `-mcpu=cortex-a72 -moutline-atomics` (runs on a Pi 5 too, using its LSE atomics); `pi5`: `-mcpu=cortex-a76` (not for a Pi 4); `x86-64-v2`, `x86-64-v3`; `native`. A preset that doesn't fit the target stops CMake. |
| `QILIN_LIFTED_OPT` | the lifted files | `O2` (default) or `O3`, GCC and Clang. CMake's Release is `-O3` for GCC and Clang; the lifted files are now `-O2`. |
| `QILIN_LTO` | the game's targets | OFF by default. ThinLTO with Clang (lld; `QILIN_LTO_JOBS` back ends at once, a cache in the build folder), LTO with GCC, `/GL` and `/LTCG` with MSVC. |
| `QILIN_PGO` | the game's targets | `OFF`, `GENERATE` or `USE`, with `QILIN_PGO_PROFILE` (below). |
| Module scanning | every build | off (`CMAKE_CXX_SCAN_FOR_MODULES`): the project has no C++ modules, and Ninja would otherwise preprocess every file twice. |

The game's targets are `ridgeracer7`, `ridgeracer7_machine` and `ridgeracer7_window`; the tools and tests are never built
for LTO or PGO.

**Windows.** `tools/build-ridgeracer7.ps1` builds a new `build-game` with Clang when it finds clang-cl 19 or newer
(Visual Studio's "C++ Clang Compiler for Windows" component, or LLVM's installer), through Ninja (Visual Studio's CMake
tools bring it) in Visual Studio's developer environment, with lld-link; otherwise with Visual Studio's own compiler as
before. `-Compiler msvc` or `-Compiler clang` chooses; a build folder stays with the compiler it was set up with. By
hand:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link
```

from a "x64 Native Tools" prompt, or `-T ClangCL` with the Visual Studio generator (CI's `windows-x64-clang-cl` job).
MSVC stays supported and in CI.

**A Raspberry Pi.** `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQILIN_CPU=pi4` (or `pi5`), with Clang 19 or GCC 14.
The largest lifted file took 1.6 GB to compile for ARM64 (below): on a 4 GB Pi, compile two files at a time at most.

## Measurements

The PC: the owner's (16 hardware threads), shared with other builds at the time, so times are the best of several
runs. MSVC 19.44 (Visual Studio 2022 17.14, `/O2 /Ob2`) against Clang 22.1.8 as clang-cl (`/O2`, which is `-O2`; zig
0.17.1's Clang, since Visual Studio's Clang component isn't installed here: the same compiler, an LLVM newer than
Visual Studio's 19). The lifted code is v0.13.1's lift of the game (14,294 PPU functions in 56 files, 42 SPU programs),
in a scratch folder; none of it is in the repository.

### Compiling

One file at a time; peak memory is the compiler's committed memory.

| File | Source | MSVC time | MSVC memory | Clang time | Clang memory | `.text`, MSVC | `.text`, Clang |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `lifted_020` (PPU, the largest) | 13.0 MB | 33 s | 0.89 GB | 33 s | 0.66 GB | 2.78 MB | 1.86 MB |
| `lifted_022` (PPU) | 3.4 MB | 14 s | 0.61 GB | 13 s | 0.24 GB | 2.62 MB | 0.72 MB |
| `spu_p41` (the largest SPU program) | 1.2 MB | 51 s | 1.12 GB | 12 s | 0.14 GB | 0.84 MB | 0.84 MB |
| `spu_p24` | 0.3 MB | 15 s | 0.60 GB | 5 s | 0.08 GB | 0.32 MB | 0.30 MB |

Clang compiles the SPU programs three to four times faster and every file in a quarter of the memory or less, and its
PPU code is a third to three quarters smaller: what a Pi's 48 KB (A72) or 64 KB (A76) instruction cache needs most.
For `aarch64-linux-gnu` (Clang 22 through zig, `-O2`), `lifted_020` took 44 s and 1.6 GB, `lifted_022` 17 s and 0.7 GB,
`spu_p26` 9 s and 0.4 GB, the same with `pi4`'s and `pi5`'s flags.

### Running: the SPU programs

A scratch benchmark runs one lifted SPU program from its entry on a synthetic machine (guest memory of pseudo-random
words, a small MFC), over and over, and hashes the end state: local store, registers and guest memory. Nine programs
that do real work on that input (2.5 to 200 ms a run); the best of 5 to 60 runs, in microseconds:

| Program | MSVC | Clang, no SSSE3 | Clang `-O2` | Clang `-O3` | Clang `x86-64-v2` | Clang `x86-64-v3` | MSVC `/arch:AVX2` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `spu_p03` | 3,888 | | 2,754 | 2,754 | 2,717 | 2,498 | 3,811 |
| `spu_p08` | 9,270 | | 6,882 | 6,895 | 6,845 | 6,192 | 8,869 |
| `spu_p10` | 51,753 | | 22,188 | 22,147 | 21,863 | 22,968 | 51,491 |
| `spu_p12` | 20,204 | | 15,579 | 15,509 | 15,455 | 14,542 | 19,272 |
| `spu_p14` | 12,023 | | 5,359 | 5,345 | 5,674 | 5,062 | 9,787 |
| `spu_p16` | 14,620 | | 9,330 | 9,347 | 9,152 | 8,348 | 13,278 |
| `spu_p18` | 4,238 | 10,775 | 3,691 | 3,683 | 3,810 | 3,823 | 4,072 |
| `spu_p22` | 4,087 | | 3,705 | 3,699 | 3,857 | 3,806 | 3,974 |
| `spu_p26` | 207,320 | | 68,367 | 68,512 | 68,271 | 61,788 | 206,765 |
| **Geometric mean, against MSVC** | 1.00 | | **0.62** | 0.62 | 0.62 | 0.59 | 0.95 |

- **Clang runs the SPU programs in 62% of MSVC's time** (1.6 times as fast; `spu_p26` three times, `spu_p10` 2.3 times).
- **SSSE3 matters**: built without it, Clang took 2.5 times MSVC's time on `spu_p18`, since `spu.hpp` then uses its
  plain C++ shuffles. This was every GCC and Clang build until now (Linux and macOS included).
- **`-O3` gains nothing over `-O2`** (within 0.5% everywhere): the lifted code is `-O2`.
- `x86-64-v2` gains nothing; `x86-64-v3` (AVX2) another 5%, so it is offered as a preset, not the default.
- Every variant gave the same end state for each program: the compilers agree to the bit.

### Running: the PPU code

A second scratch benchmark runs each function of `lifted_022` on a synthetic machine (a 256 KB arena of pseudo-random
words, registers pointing into it, the functions of other files as no-ops), timed by the time-stamp counter. 244 of its
256 functions finish on that input:

- The 243 short ones (most return early on random input, about 700 cycles each): **Clang 3% faster** than MSVC (172,600
  against 178,200 cycles summed, best of 5 rounds), which is within the PC's noise. `-O3` the same as `-O2`.
- One long loop (`fn_001CEE70`: a call in a loop, each call writing back and reloading 17 registers and the flags):
  **MSVC 14% faster** (1.96 against 2.29 billion cycles). Turning off Clang's SLP vectoriser didn't change it. *Why is
  open*; the write-back around calls is what the lifter's register caching (`docs/recompiler-core.md`) works on, and it
  is where LTO and PGO would help.
- Every compiler gave the same end state.

So the PPU code is about level between the two; the SPU programs, which the game runs on six SPUs' worth of work, are
much faster with Clang, and the PPU code is much smaller. Clang is the build script's choice where it is installed.

### Link-time optimisation

The four files above as one DLL exporting all their functions (18 MB of source, 14% of the game's 125 MB):

| | Compile (four files) | Link | Link memory |
| --- | --- | --- | --- |
| Clang | (as above) | 0.1 s | 0.01 GB |
| Clang ThinLTO, 2 back ends | 38 s | 21 s | 0.19 GB |
| MSVC | (as above) | 0.3 s | 0.01 GB |
| MSVC `/GL` and `/LTCG`, 2 threads | 8 s (front end only) | 136 s | 1.41 GB |

- **ThinLTO** moves the code generation to the link, in parallel back ends that each take about what a compile takes:
  the whole game's link would take the compile's time divided by the back ends, with `QILIN_LTO_JOBS` (default 2) times
  a file's memory (0.7 GB at most) plus the index. Its cache makes a relink after a small change cheap. Affordable.
- **MSVC's `/LTCG`** generates the whole program in one link: scaled to the whole game, *an estimated 10 GB and a
  quarter of an hour for every link*, and every change to any file redoes it. Not recommended; offered for measurement.
- **What LTO gains at run time is not measured**: it needs the whole game (calls between files, which these benchmarks
  stub). `QILIN_LTO` stays OFF by default, and the build script doesn't turn it on, until a game run measures it
  (`-DQILIN_LTO=ON` with clang-cl and lld-link, one configure).

### Profile-guided optimisation

Plumbing only: no profile has been made (that needs a game run).

1. Configure with `-DQILIN_PGO=GENERATE` and build `ridgeracer7`. With Clang the game counts what runs
   (`-fprofile-instr-generate`, atomic counters for the game's threads); clang-cl's profile runtime
   (`clang_rt.profile`, part of LLVM and Visual Studio's Clang) is linked in, and CMake stops if it isn't there. With
   MSVC, `/GL` and `/GENPROFILE` into `build/ridgeracer7.pgd` (or `QILIN_PGO_PROFILE`); with GCC, `-fprofile-generate`
   into `build/pgo`.
2. Run a race with the scripted presses (`RR7_PRESS`, `RR7_QUIET=1`, `RR7_ANSWER=1`, `RR7_SECONDS=<n>`, from a copy
   of the save): the run ends through `exit_process`, which writes the counts, as the runtimes' own exit handlers would
   (the game ends with `std::_Exit`, which runs none). Clang writes `default.profraw` in the working folder
   (`LLVM_PROFILE_FILE=rr7-%p.profraw` names it per process); MSVC writes `.pgc` files beside the executable.
3. Merge: `llvm-profdata merge -o rr7.profdata *.profraw` (Clang), or `pgomgr /merge` into the `.pgd` (MSVC).
4. Configure with `-DQILIN_PGO=USE -DQILIN_PGO_PROFILE=<rr7.profdata or the .pgd>` and build again.

A profile is the game's own (it says what the game's code does); it stays on the PC like the lifted code. It goes stale
as the lifter changes the code: Clang ignores the functions whose shape changed (warnings off), MSVC's must be made again.

## Checks

- The whole project (tools, tests, the machine, SDL3, FFmpeg's part, FreeType) builds with clang-cl and
  `QILIN_WERROR=ON` through Ninja, and all 48 of `ctest`'s tests pass, the PPU and SPU differential tests included.
- MSVC as before (Visual Studio's generator).
- `machine.cpp` and `main.cpp` with zig's Clang for `x86_64-linux-gnu`, `aarch64-macos` and `aarch64-linux-gnu` with
  `pi4`'s flags; the runtime's sources with `pi4`'s and `pi5`'s; `lifted_020`, `lifted_022` and `spu_p26` cross-compiled
  for `aarch64-linux-gnu` with both.
- Configure-time: each preset, a preset for the wrong processor (stops), LTO with clang-cl (its options in the build),
  PGO `USE` without a profile (stops).
- Not checked here: `-T ClangCL` (Visual Studio's Clang component isn't installed on this PC: CI's job checks it), the
  PGO GENERATE build with a real LLVM's profile runtime, any LTO or PGO build of the game itself.

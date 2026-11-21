# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/). While the recompiler is being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game. Past entries are never rewritten.

## [Unreleased]

## [0.11.0] - 2026-11-21

The game's first instructions run.

### Added
- The game's machine (`ridgeracer7/src/`): the program loaded at its linked addresses in a 4 GiB guest address space, the first thread with the registers the system hands a process, calls through a register dispatched to the lifted functions, and the boundary with the system: every import and system call arrives by name or number, is run if the runtime provides it, and otherwise **stops the game with a line naming it and its caller**.
- `ridgeracer7`, the game's executable, built from the player's own copy: `qilin lift --out` now also writes a table of every function and, for the functions not lifted yet, ones that stop the game if called. It checks at start-up that the program it is given is the one it was built from.
- `tools/build-ridgeracer7.ps1`: lifts your copy and builds the game in one go. `qilin lift` rewrites only files that changed.
- The runtime's messages and its way of stopping (`rt::log`, `rt::fatal`).
- The first import provided natively: `sys_initialize_tls`.
- `docs/runtime.md`; the planned shape of exception landing pads, in the roadmap.

### Checked
- On the owner's PC: the game builds (14,294 functions, a 40 MB executable), loads, runs its entry point and start-up code natively, and stops at `sys_lwmutex_create`, the first system function not provided.

## [0.10.0] - 2026-11-21

The lifter's third slice: VMX.

### Added
- VMX in the PPU lifter, the part the game uses, and its runtime (`runtime/include/qilin/rt/vmx.hpp`): vector loads and stores, logical operations, select, permute, shifts by bytes, merges, splats, word add and compare, and single-precision add, subtract, fused multiply-add, maximum, minimum, compare and conversions, element by element and bit-exact. Portable C++, no host intrinsics.
- The reference's VMX (`recomp/tests/difftest/ref_vmx.cpp`), on bytes and the integer soft-float.

### Checked
- On the game: 14,196 of 14,294 functions lift (1,016,630 of 1,059,387 instructions).
- The differential test: 5,403 cases, 1,327,632 runs, all agree. A fault put into `vmaxfp` on purpose was caught.

### Not done, and why
- `vrefp` and `vrsqrtefp` (96 functions): estimates whose exact values are the hardware's. Left unlifted until there is a source for them.
- Assumed from public descriptions, to be confirmed against RPCS3: VMX's non-Java mode (denormals as zeros), and the order of operands in a NaN's choice.

## [0.9.0] - 2026-11-21

The lifter's second slice: floating point, bit-exact.

### Added
- Floating point in the PPU lifter, and its runtime (`runtime/include/qilin/rt/fp.hpp`): loads and stores, arithmetic in both precisions, the fused forms, conversions, compares, `fsel`. PowerPC's NaN results, single-precision results rounded once whatever the operands, and the architecture's own conversions of singles in memory.
- The time base (`mftb`), through a hook.
- The reference's floating point (`recomp/tests/difftest/ref_fp.cpp`): integers only, exact in 128 bits, rounded once.

### Checked
- On the game: 13,732 of 14,294 functions lift (914,967 of 1,059,387 instructions). VMX (560 functions) and two functions that use the FPSCR are left.
- The differential test: 4,765 cases, 1,085,808 runs, all agree. Floating-point arithmetic runs from 1,200 states a case.

### Fixed
- Found by the reference on its first run, before any release had it: the slow path of the single-precision fused forms rounded a tie the wrong way when the product underflowed in double.

## [0.8.0] - 2026-11-21

The PPU lifter's first slice, and the differential test.

### Added
- The PPU lifter (`recomp/src/lift.*`): a guest function to a C++ function over `qilin::rt::Cpu`, registers in host locals, written back around calls. Slice 1: integer arithmetic and logic in both widths, rotates and shifts, compares and the condition register, loads and stores, LR, CTR and XER, traps, system calls, and every branch, call and switch. A function with anything else is not lifted, and counted under the reason.
- The runtime's first parts (`runtime/`): the CPU state and the helpers lifted code calls (`cpu.hpp`), and the guest's 4 GiB address space (`memory`).
- `qilin lift` (with `--out` to write the lifted code).
- The differential test (`recomp/tests/difftest/`): a reference interpreter written separately from the lifter, a generator of cases, and `qilin_difftest`, in `ctest`.

### Checked
- On the game: 11,873 of 14,294 functions lift (641,755 of 1,059,387 instructions); the rest wait for floating point (2,053), VMX (363) and the time base (5). All the lifted code compiles with Visual Studio 2022.
- The differential test: 3,602 cases, 172,896 runs, the lifted code and the reference agree on every register, flag and byte. A fault put into a helper on purpose was caught.

### Not done
- Floating point, VMX, the time base; entering a function at an exception landing pad; the SPU lifter. Nothing runs yet.

## [0.7.0] - 2026-11-21

The SPU control-flow pass.

### Added
- Control-flow pass for the SPU programs (`recomp/src/spu_cfg.*`, `qilin spu-cfg`): functions from the entry point, calls, tail calls, addresses in `ila` and in data, and unreferenced code at clean boundaries; the one switch; the channels each program uses; word-by-word coverage.

### Checked
- On the game's 42 SPU programs: 2,332 functions (730 of them called by nothing: library functions the linker kept); **209,492 words: 208,634 code, 4 jump-table entries, 854 padding, 0 unknown**, and no function reaches a word that isn't an instruction.

### Fixed
- The SPU programs' code was counted with each program's 16-byte `.SpuGUID`, which is marked executable but is an identifier: 209,492 words of code, not the 209,660 of v0.3.0 and v0.6.0.

## [0.6.0] - 2026-11-21

The SPU decoder.

### Added
- SPU decoder (`recomp/src/spu.*`, `qilin::spu`): 199 instructions, in the PPU decoder's table shape, reserved bits required to be zero (`docs/recompiler-core.md`).
- `qilin spu-insns`, `spu-profile` and `spu-disasm`.
- `tools/check/verify_spu_decoder.py`, and a CI job that builds its reference: objdump from binutils 2.31.1 for `spu-elf` (pinned by hash, cached), since no current binutils has the SPU.

### Checked
- Against binutils 2.31.1 in CI: 1,664,400 words swept, 1,476,288 valid and identical, 151,494 invalid for both, 36,618 that differ only in reserved bits binutils ignores, none unexplained. The first run found no fault in any opcode or field.
- On the game's 42 SPU programs: 209,656 of 209,660 words decode; the other four are a jump table. 147 of the 199 instructions are used; floating point is single precision but for two programs.

## [0.5.0] - 2026-11-21

The archive's directory, the shaders, and names for the system calls.

### Added
- `qilin archive` and its reader (`recomp/src/archive.*`): `RR7.DAT`'s directory, which is in the program's data, found by its shape and checked relation by relation. `--files` lists every pack and file.
- `qilin shaders` and its reader (`recomp/src/shaders.*`): the compiled Cg programs in the program's data.

### Checked
- The directory on the owner's program and disc: 12,810 files in 2,088 packs (908 compressed, 4.70 GB stored and 10.22 GB unpacked; 1,180 plain), running from sector 0 to the archive's last with no gap, **5,231,794,176 bytes: `RR7.DAT`'s size exactly**. Compressed packs sit at the end of their sectors, which accounts for the zero bytes the survey found.
- 242 compiled shader programs in the program: 121 vertex, 121 fragment. The shaders are a closed set known at build time, as the plan needed.

### Not done
- The archive's compression is not decoded.

### Added
- Names for the 78 system calls the game makes directly (`recomp/src/syscall_names.*`); `qilin cfg` lists them. 68 are confirmed by PSL1GHT's headers; 10 are flagged as unconfirmed.
- `import_id` and `qilin id <name>`: the id of a system function, computed from its name (SHA-1 of the name and the scheme's fixed suffix). The unit tests now recompute all 248 ids in the table of names, so it is checked on every push. The suffix is a constant of the naming scheme, not a key; it is in the repository by the owner's decision.

## [0.4.0] - 2026-11-21

The control-flow pass: every word of the game's code accounted for.

### Added
- Control-flow pass for the PPU code (`recomp/src/cfg.*`, `qilin cfg`): functions from the program's descriptors, unwinding tables, import stubs and calls; recursive descent bounded by each function's unwinding entry; GCC's in-code jump tables, their addresses read through the TOC and the per-file constant tables and their sizes proven by the code's own bound check where it is in reach; the C++ exception tables' landing pads as entry points; the number of every system call, by following the constant in `r11` along every path; word-by-word coverage (`docs/recompiler-core.md`).
- Readers for `.eh_frame` and the exception tables (`.gcc_except_table`).
- Names for all 248 imported functions (`recomp/src/import_names.*`): the SDK's own names, each confirmed by computing its id from the name. `qilin imports` prints them and `qilin info` counts them. `docs/elf-analysis.md` says what they show: SPURS tasksets, the system's ATRAC3plus, audio and video decoders, a USB driver of the game's own, saves and hard-disk data.

### Checked
- On the game: 14,294 functions; 296 switches with 5,634 entries, 264 sized by proof and 32 by where their targets begin; 2,845 landing pads in 932 functions; all 502 system-call sites numbered (78 different calls); **1,065,028 words: 1,059,387 code, 5,634 jump-table entries, 3 padding, 4 dead, 0 unknown**, and no function reaches a word that isn't an instruction.

### Found
- Every function has a descriptor, so the descriptors alone list the functions.
- The game uses C++ exceptions: the lifter has to make every landing pad an entry into its function.

### Changed
- **Links and mentions of the sister projects follow their new names** (WindWakerHD--QilinRecomp, PunchOutWii--QilinRecomp, GBTGhoulies--QilinRecomp, ApbArcade--QilinRecomp, and RhythmHeavenFever--QilinRecomp for Beat the Beat: renamed by the owner; GitHub redirects the old names).

## [0.3.0] - 2026-11-21

The decrypted program, read at last.

### Added
- `qilin spu` and the reader behind it (`recomp/src/spu_image.*`): the SPU programs a PS3 program carries in its data, each a whole ELF file, found and validated, with its name, segments and code. `qilin info` counts them, and says when the file has no section names.
- The decrypted program's hash among the known builds.
- `Analyse game.bat` writes the SPU programs' list.

### Checked
- **The ELF loader on the real program: nothing to fix.** Made with RPCS3 0.0.43 (`rpcs3.exe --decrypt`), it loads, matches `EBOOT.BIN`'s plain headers, and every table is as the loader expected: 22 libraries, 248 functions, every slot holding its stub's address, no exports.
- The decoder on the real code: 1,065,028 words, 99.47% decode. The 5,603 that don't are jump tables (291 of the 303 runs follow a `bcctr`) and a few constants. The game uses 151 of the decoder's 383 instructions: no atomic sequences, no cache instructions but `dcbt`, a narrow floating-point part of VMX, and none of the instructions whose presence on the PPU was in doubt.
- 502 `sc` instructions, 78 different system calls found at 477 of them.
- 42 SPU programs, 209,660 words of code: particles (23), collision (5), the car's dynamics (12, with the bulk of the code), sound (2). Built with GCC 4.0.2.

### Found
- RPCS3 writes zeros for the sections outside every segment, so the decrypted file has no section names and no `.comment`: the names in `docs/elf-analysis.md` stay inferred.

### Not checked
- The decoder against objdump over the game's code: no PowerPC objdump where the game is.

## [0.2.0] - 2026-11-21

The PPU decoder, and the first look inside the data files. The decrypted program still hasn't been read: it wasn't on the owner's PC when this was made.

### Added
- PPU decoder (`recomp/src/ppu.*`, `qilin::ppu`): the Cell's PowerPC core, in the table shape of the other Qilin recompilers. 383 instructions: the 64-bit PowerPC architecture, VMX, and the Cell's own vector loads and stores. Reserved bits set and invalid forms decode as invalid; the supervisor's instructions are left out (`docs/recompiler-core.md`).
- `qilin insns` (every word of the program's code), `profile` (how often each instruction is used, and what doesn't decode) and `disasm` (a file of raw words).
- `tools/check/verify_decoder.py`: the decoder against GNU objdump (`-M cell,raw`), word by word, over the game's code or a sweep of the encoding space. CI runs the sweep.
- Readers for the headers of the sound banks (`.nub`) and the films (`.pam`) (`recomp/src/media.*`), and `qilin data`, which lists the game's files with what those headers say. `Analyse game.bat` writes it, and the code's profile once there is a decrypted program.
- `docs/data-files.md`: what the data files are, and what that decides.

### Checked
- The decoder against binutils 2.42 in CI: 4,931,891 words swept, 4,266,210 valid and identical, 539,096 invalid for both, the rest reviewed differences (binutils accepting what the PPU lacks or ignoring reserved bits), none unexplained. The sweep's first run found one fault, fixed: `mftb` accepted any register number.
- On the owner's PC, for the first time: the build with Visual Studio 2022 (17.14), the unit tests, and `Analyse game.bat`'s script, which had never been run. It stops where it should, saying how to decrypt the program.
- On the owner's disc: the seven films are PAMF with **MPEG-2 video** (1280 × 720; one at 320 × 176), three with an ATRAC3plus sound stream; `BGM.nub` holds 43 pieces of **ATRAC3plus in 5.1** at 48 kHz (three and a half hours); `Game_DJ.nub` holds 714 sounds of 16-bit PCM. `RR7.DAT` has no directory of its own, and at least some of its files are compressed.

### Not checked
- Anything that needs the decrypted program: the ELF loader on the real file, `insns` and `profile` on the game's code, the decoder over every word of it.

## [0.1.0] - 2026-11-21

The start of the project: the tools that read the game. Nothing runs yet.

### Added
- The project's requirements, scope and roadmap, the same as the other Qilin recomps' and applied to the PS3 (`docs/roadmap-and-design-targets.md`), with the naming rules (`docs/naming.md`) and the prior art (`docs/prior-art.md`). The licence is GPL-3.0, as in all the QilinRecomp projects.
- The game found however the player holds it (`recomp/src/title.*`): the disc's folder, `PS3_GAME` or `USRDIR`, `EBOOT.BIN`, or the decrypted `EBOOT.elf`. A decrypted program that isn't its `EBOOT.BIN`'s is refused; with only `EBOOT.BIN`, the message says how to make the decrypted program with RPCS3.
- SELF reader (`recomp/src/self.*`): what `EBOOT.BIN`'s header leaves plain, with nothing decrypted: the copy of the ELF header, the program and section headers, where each segment is stored, and the list of SDK libraries the program was linked from.
- PS3 program loader (`recomp/src/elf.*`): the 64-bit big-endian ELF with the PS3's additions: the entry point's descriptor, the process and PRX parameters, the imports by library with each function's id, slot and stub, the exports, thread-local storage. Every table is validated when the file is opened. Tested on a synthetic program; not yet run on the real one, which has to be decrypted first (`docs/elf-analysis.md`).
- `PARAM.SFO` reader (`recomp/src/sfo.*`).
- The `qilin` tool: `info` (with `--json`), `imports`, `exports`, `sfo`.
- `Analyse game.bat` (Windows): builds `qilin` and writes the first analysis of your copy to `analysis-out\`.
- CI on six configurations (Linux x64 with GCC 14 and Clang 19, Linux ARM64, macOS ARM64 and x64, Windows x64), warnings as errors.

### Checked
- On the owner's disc (USA, `BLUS30001`, version 01.02): `PARAM.SFO` and the SELF's plain header read, and `EBOOT.BIN` recognised by its SHA-256. The results are in `docs/elf-analysis.md`: 4.25 MB of code, 14,046 function descriptors, 248 functions imported from 22 system libraries, C++ unwinding tables, SPURS, and a 1 MB section that is probably the SPU programs. The program was built with SDK 1.02 from `libc`, `libm`, `libstdc++` and the graphics library's command writers.

# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/). While the recompiler is being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game. Past entries are never rewritten.

## [Unreleased]

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

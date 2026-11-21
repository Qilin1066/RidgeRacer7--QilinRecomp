# CLAUDE.md

Static recompilation of **Ridge Racer 7** (PlayStation 3, USA, `BLUS30001`, version 01.02) to native code. Same requirements and scope as the sister projects (Qilin1066/GBTGhoulies-QilinRecomp, ApbArcade-QilinRecomp, BeatTheBeat-QilinRecomp, PunchOutWii-QilinRecomp, WindWakerHD-QilinRecomp), down to the same settings menu. This repository is Qilin1066/RidgeRacer7--QilinRecomp (two hyphens). The claude.ai Project "Ridge Racer 7 Recomp" holds a short status note (`claude/status.md`); this file and `docs/` are the full record.

## Requirements (non-negotiable)

- **Efficiency first, then accuracy, and fail loudly.** Never paper over an unknown: stop with a clear error (an error message and exit code in the tools, `rt::Stop` in the runtime, CMake `FATAL_ERROR`).
- **C++23** everywhere: recompiler, runtime and generated code.
- **Vulkan 1.3 only** for graphics. No OpenGL, D3D or Metal backends (macOS goes through MoltenVK).
- **Static recompilation only.** No interpreter, JIT or emulator at runtime, not even as a fallback. This covers the **SPU programs** and the game's **shaders** too: both are translated at build time.
- Targets: 64-bit **Windows, Linux, macOS** on **x86-64 and ARM64**. The low-end reference is ARM boards like the **Raspberry Pi 4** (whether it can hold this game's frame rate is open: measure, don't promise). No iPadOS, no 32-bit.
- Compiler minimums: GCC 14, Clang 19, Apple Clang (Xcode 16), MSVC 2022 17.10.
- **No PS3 keys, ever**, in the repository or the tools: `EBOOT.BIN` is not decrypted here. The player decrypts their own with RPCS3 (`docs/elf-analysis.md`).
- Nothing copied from an emulator (RPCS3 is GPL-2.0 and the behaviour reference: run, not read) and no code of other people's copied in. Third-party libraries are fetched by CMake at configure time, pinned, never committed (SDL3, the Vulkan headers and volk, Dear ImGui, when the window arrives, as in the other recomps). Keep dependencies to the fewest possible (the owner's standing preference). The project itself is GPL-3.0, as all the QilinRecomp projects are since 2026-11-21.
- Never commit game data or anything derived from it: `EBOOT.BIN`, `EBOOT.elf`, `PARAM.SFO`, `analysis-out/`, listings, import lists, lifted code. These are gitignored. Facts about the program written up in `docs/` (sizes, counts, hashes) are fine, as in the sister projects.

## Layout

- `recomp/src/`: `elf` (the PS3 program: segments, sections, the entry descriptor, process and PRX parameters, imports with ids, slots and stubs, exports, TLS), `self` (`EBOOT.BIN`'s plain header: the ELF's tables, the stored segments, the SDK library stamps), `sfo` (`PARAM.SFO`), `title` (finds the files in the player's copy; `known_build`; the decrypt message), `sha256`, `bytes.hpp`. These are CMake's `qilin_game`: **runtime code too, so no `std::format` in them** (macOS 10.15). `main.cpp` is the `qilin` CLI (tools only: `std::format` and `std::print` are fine there).
- `recomp/tests/`: `tests.cpp` (unit tests), `elf_builder.hpp` (builds a program, its SELF and a `PARAM.SFO` in memory).
- `tools/analyse-game.ps1` and `Analyse game.bat`: build `qilin` on Windows and write the first analysis to `analysis-out/`. Keep the script Windows PowerShell 5.1-compatible and ASCII-only. **It has not been run.**
- `docs/`: design and results. Keep them current. `roadmap-and-design-targets.md` is the roadmap and every design decision (open ones are marked **open**); `elf-analysis.md` the program and what has been read from the owner's disc; `prior-art.md` what may be taken from where; `naming.md` the names.
- Still to come, in WindWakerHD-QilinRecomp's layout: `host/` (window, Vulkan, input, audio), `runtime/` (`qilin_rt`), `ridgeracer7/src/` (the game's executable, the system libraries by file, `settings`, `menu`).

## Build and check

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQILIN_WERROR=ON
cmake --build build --parallel 4   # a number: with Makefiles, a bare --parallel starts every job at once
ctest --test-dir build --output-on-failure
build/qilin info <game>      # <game>: the disc's folder (holding PS3_GAME), EBOOT.BIN, or EBOOT.elf
build/qilin imports <game>
```

Before a milestone commit: tests pass, warnings are clean (`QILIN_WERROR`), and CI is green on all six configurations. Check CI after pushing (its results are readable without signing in: `https://api.github.com/repos/Qilin1066/RidgeRacer7--QilinRecomp/actions/runs`).

## Known facts worth not rediscovering

- The owner's copy is the disc's folder on the owner's PC, outside the repository (its path is in the claude.ai Project's `claude/status.md`; local paths are not written here). `RR7.DAT` is 5.23 GB and `BGM.nub` and `ending.pam` are over 600 MB: a cloud session can stage files of up to 400 MB, so those never leave the PC whole. `EBOOT.BIN` (7.8 MB) and `PARAM.SFO` stage fine, and so will `EBOOT.elf`.
- **`EBOOT.elf` does not exist yet** (v0.1.0): the owner has to make it with RPCS3. Until then the ELF loader has only met synthetic files. Its reading of the PS3's additions (field offsets of the two parameter blocks, the 44-byte import and 28-byte export entries, the stub's eight instructions, slots initially holding their stub's address) is from public descriptions and memory of them: each is a hard error or a reported count if the real file differs, so the first `qilin info` on the real ELF is the check.
- A cloud session reaches the PC only while the Claude desktop app is open there, and can run commands there only when the app offers its shell (`device_bash`). In the session that made v0.1.0 it offered listing and staging files, not the shell: nothing was run on the owner's PC, and the owner's sister-project folders were read by staging files.
- Tags don't reach GitHub from a cloud session (the proxy refuses pushing them: HTTP 403); `main` does. Releases are marked by their commit's title (`vX.Y.Z: ...`) and the changelog until a tag can be pushed from elsewhere.
- The cloud workspace has GCC 13 and Clang 18, both below the minimums: build there with zig's Clang (`pip install --break-system-packages ziglang==0.17.1`, then wrapper scripts running `python3 -m ziglang cc` / `c++` as `CMAKE_C_COMPILER` / `CMAKE_CXX_COMPILER`). It has no PowerPC objdump: a decoder sweep runs in CI.
- The SELF (`EBOOT.BIN`, SHA-256 `095ed6cb8e90538309b57662fb19da1ba2c4bdbac5fd40dc0be6122e76d4c0db`): header `0x980` bytes, then the ELF's file image at the same offsets plus `0x980`. The five loaded segments are encrypted; so are `.comment` and the section name table, though they lie outside every segment. The section headers and the SDK stamp list (section 33) are plain. Section names in `docs/elf-analysis.md` are inferred from name lengths: replace them when the ELF is read.
- The SELF's SDK version header (at the offset the extended header gives): type, present, size `0x30`, then at `+0x20` the stamp list's offset in the file and at `+0x28` its length. The stamps run together with no separators ("crt0:p102002crt1:p102002..."): a name, a colon, one letter and digits.
- `PARAM.SFO`'s `SOUND_FORMAT`: the compressed formats are two bits each sharing bit 1 (Dolby Digital `0x102`, DTS `0x202`); this disc's 263 is LPCM 2.0, LPCM 5.1 and Dolby Digital.
- An extraction of the owner's `.7z` was still running when the first session listed the folder (`RR7.DAT` showed 36 MB with a fresh date, and two films were missing). A file with today's date among 2006 ones is a file still being written: list again before recording sizes.
- Other PS3 static recompilations exist (`docs/prior-art.md`: ps3recomp and ports built on it). Their licences are unchecked: don't read their code until they are.
- Facts about the game not read from the disc (60 frames a second, 1080p, online play, what it shares with Ridge Racer 6) are from public descriptions.

## Status (v0.1.0)

- The tools find the game, read `PARAM.SFO` and the SELF's plain header, and load a decrypted PS3 program (on synthetic files). Read from the owner's disc: `docs/elf-analysis.md`.
- Checked: unit tests (zig's Clang in the cloud workspace); `qilin info` on the owner's disc folder (prints the disc's description and the SELF's plain facts, then stops with the decrypt message, exit 1).
- Unchecked: **the ELF loader on the real program**; the PowerShell script (never run).

## Next steps, in order

1. The owner: RPCS3, Utilities > Decrypt PS3 Binaries on `EBOOT.BIN`; `EBOOT.elf` beside it in `USRDIR`. Then stage it and run `qilin info` and `imports`. Fix whatever the ELF loader gets wrong on the real file. Record the ELF's SHA-256 in `kBuilds` (`title.cpp`), and in `docs/elf-analysis.md` the real section names, the compiler (`.comment`), the 22 libraries and the count taken from each, how many slots pair with a stub.
2. Names for the imported ids: the id is derived from the name by a public method (`docs/prior-art.md`); find a clean, permissively licensed list of names, compute, match all 248. Add `qilin imports` names.
3. The PPU decoder (`qilin::ppu`): the Nintendo recomps' table shape, for 64-bit PowerPC with VMX; `qilin insns`, `disasm`, `profile`; `tools/check/verify_decoder.py` against binutils (`--sweep` in CI). The profile says which VMX and 64-bit forms the game uses.
4. The SPU programs: find the embedded images (the section aligned to 128), count them, read their headers; the SPU decoder and its binutils check.
5. The control-flow pass on the PPU code: seeds from the descriptors (`.opd`) and `.eh_frame`; stubs as known import calls; `sc` sites listed with their numbers; coverage to 0 unknown words.
6. Survey `RR7.DAT` on the owner's PC (it can't be staged whole: a `qilin` command that lists it, run there by `Analyse game.bat`), the `.nub` and `.pam` headers. Decide the open items in the roadmap (shaders, codecs).
7. Then, in WindWakerHD's order: lifter and difftest, the runtime skeleton, the window and the settings menu, threads, files, the RSX on Vulkan, sound.

## Conventions

- **Push without asking, as in the sister projects, but only what is 100% legal and above board to share with the world, checked before every push:** audit every unpushed commit (every path added, binaries, anything named like game data or game-derived output, copied third-party text or code, personal details such as the owner's local paths, name or e-mail). No game data or anything derived from it, no keys, no copied third-party text or code. If anything fails the audit, fix it first; never force-push published history.
- Work on `main`; push with `git push origin main`. Releases are `vX.Y.Z`; the version lives only in `project(... VERSION ...)`. Every release gets a `CHANGELOG.md` entry (Keep a Changelog); never rewrite past entries.
- Prose in docs and commit messages: plain, specific, British spelling.
- UI conventions are shared with the other Qilin recomps (`docs/naming.md`, `docs/roadmap-and-design-targets.md`): keep them in step when either side changes.

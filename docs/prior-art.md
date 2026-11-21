# Prior art

What exists that bears on this port, and what we may take from each. The rule is the other Qilin recomps': **nothing copied from an emulator, and no code of other people's copied in**; permissively licensed data and public documentation are fine, cited where used. This page is a starting list (v0.1.0): each entry says how far it has been checked.

## 1. The sister projects

| Project | What carries over |
|---|---|
| [WindWakerHD--QilinRecomp](https://github.com/Qilin1066/WindWakerHD--QilinRecomp) | The newest of the family and the model for this one: the repository's layout, the tools' shape (`qilin`, the game found however the player holds it), the host layer (SDL3 window, input and audio, Vulkan through volk), the Dear ImGui settings menu, the scheduler for a console with more than one core, the Windows launchers. The Wii U is also the nearest console: an operating system reached through an import table, not hardware registers |
| [PunchOutWii--QilinRecomp](https://github.com/Qilin1066/PunchOutWii--QilinRecomp) | The PowerPC decoder, control-flow pass and lifter the Wii U project took over. The PS3's PPU is a different PowerPC (64-bit, with VMX and without paired singles), so they are the starting shape, not drop-in files |
| [RhythmHeavenFever--QilinRecomp](https://github.com/Qilin1066/RhythmHeavenFever--QilinRecomp) | The differential test of lifted code, the render thread, the frame pacing and low-latency sound work, the lockstep bot for scripted play |
| [GBTGhoulies--QilinRecomp](https://github.com/Qilin1066/GBTGhoulies--QilinRecomp) | The requirements and scope all of them share; resolution scaling, widescreen and unlocked frame rate on a game that wasn't written for them; a command-stream GPU (NV2A, the RSX's ancestor) translated to Vulkan with generated shaders |
| [ApbArcade--QilinRecomp](https://github.com/Qilin1066/ApbArcade--QilinRecomp) | Saved-data protection, input replay, checks against an independent reference |

## 2. The game

- **No decompilation or native port of Ridge Racer 7 was found** (a web search on 2026-11-21; other Ridge Racer games have them, the PS3 one does not). The program is stripped as far as its plain headers show: names for its functions will have to come from analysis. *To be re-checked when the program is read: whether it carries strings that name its own modules.*
- Ridge Racer 7 shares much with Ridge Racer 6 (Xbox 360, 2005), according to public descriptions. Nothing is taken from that game; it is noted in case a clean source of names ever exists for it.

## 3. The PS3

- **RPCS3** (GPL-2.0) is the PS3 emulator. RPCS3 running the original game is our **reference for behaviour, picture and sound**, as Dolphin and Cemu are for the other recomps, and its "Decrypt PS3 Binaries" is how a player makes the decrypted program from their own disc. Its code is not copied and not read for the runtime.
- **ps3recomp** (`sp00nznet/ps3recomp`) is a PS3 static-recompilation toolkit with several game ports built on it (found by a web search on 2026-11-21; none of them is this game). *Its licence and methods have not been checked.* Until they are, nothing is read from its code. This project stands on its own requirements either way: C++23, Vulkan 1.3 only, no emulator underneath, every result checked against an independent reference.
- **The formats** (`elf-analysis.md`): the ELF is the standard one; the PS3's additions (the process and PRX parameter segments, the import and export tables, function descriptors) and the SELF's header are publicly documented (the PS3 developer wiki, PSL1GHT's headers). The loaders are written from those descriptions and confirmed against the owner's disc; anything the real file contradicts is a hard error there, so the first run says which.
- **PSL1GHT** (`ps3dev/PSL1GHT`, the homebrew SDK; **MIT**, confirmed 2026-11-21 from its `LICENSE`): its export lists (`ppu/sprx/*/exports.h`) pair ids with PSL1GHT's own names for 15 of the game's 22 libraries. Used as a check, not a source: 138 of the game's 248 ids are in them, in the same libraries. The names this project uses are the SDK's own, each confirmed by computing its id (`elf-analysis.md`). Nothing of PSL1GHT's is in the repository.
- **The CPU.** The PPU is a 64-bit PowerPC with VMX (AltiVec); the SPU has an instruction set of its own. IBM's and Sony's public manuals are the source (*PowerPC User Instruction Set Architecture*, the *AltiVec Technology Programming Environments Manual*, the *Synergistic Processor Unit Instruction Set Architecture*; cited by title, not copied), with GNU binutils as the decoders' independent check, as in the other recomps.
- **The GPU.** The RSX is of NVIDIA's NV4x family: games write its command stream themselves (through `libgcm`'s inlined command writers) and ship its vertex and fragment programs compiled. The open-source nouveau driver's documentation of the NV40 command stream and the published ARB/NV vertex and fragment program specifications are the sources for translating both. *To be gathered in the analysis.*

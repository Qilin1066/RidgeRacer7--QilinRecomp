# The program: getting it, its format, and what has been read

How to get the game's program, what a PS3 program is, and what `qilin` reads from it. The owner's disc was read as far as it can be without decrypting (v0.1.0), and the decrypted program since (v0.3.0): both below.

## Getting the program

The game's code is one file on the disc, `PS3_GAME/USRDIR/EBOOT.BIN`. It is a **SELF**: the program encrypted with Sony's keys. This project has no keys and will never carry any, so the player makes the decrypted program from their own disc, once:

1. In RPCS3: **Utilities > Decrypt PS3 Binaries**, and choose your disc's `EBOOT.BIN`. It writes `EBOOT.elf`. (Or, from a command line: `rpcs3.exe --decrypt <path to EBOOT.BIN>`. RPCS3 needs no setting up for this, and no firmware.)
2. Put `EBOOT.elf` beside `EBOOT.BIN`, in `USRDIR`.

`qilin` takes your copy however you point at it: the disc's folder (the one holding `PS3_GAME`), `PS3_GAME` or `USRDIR`, `EBOOT.BIN`, or `EBOOT.elf`. With both files there it checks that the ELF is that `EBOOT.BIN`'s program (the same entry point and program headers) and refuses one that isn't. With only `EBOOT.BIN` it prints what the header leaves plain and says the two steps above.

On Windows, `Analyse game.bat` builds `qilin` and writes all of this to `analysis-out\` (never committed).

## The formats

**The ELF.** A big-endian 64-bit PowerPC ELF (machine 21, OS/ABI `0x66`), type executable, linked at fixed addresses. Although the processor is 64-bit, an LV2 process's addresses are 32 bits wide and every pointer the program keeps in memory is four bytes. What the PS3 adds:

- **Function descriptors.** A function pointer is the address of two 32-bit words: the code's address and the value of `r2` (the table of contents) the function expects. The ELF's entry point is the address of a descriptor, not of code. Every indirect call loads both.
- **Process parameters** (program header type `0x60000001`): a size, the magic number `0x13bcc5f6`, a version, the SDK version, the main thread's priority and stack size, and the heap's page size.
- **PRX parameters** (type `0x60000002`): a size, the magic number `0x1b434cec`, and the bounds of two tables, the exports and the imports.
- **Imports**: one 44-byte entry per system library, with the library's name, counts of functions, variables and thread-local variables, and for each kind a table of ids and a table of slots. An **id** is 32 bits: the PS3 doesn't name a function in the program, it identifies it by a number derived from its name. A **slot** is a word the system's loader fills with the address of the function's descriptor. The program calls an import through a **stub**: eight instructions that load the slot, save `r2`, and jump through the descriptor found there. `qilin` checks which slots start out holding the address of exactly such a stub that loads them, and reports the count (this pairing is how lifted code will know which import a `bl` reaches).
- **Exports**: 28-byte entries; a program has one unnamed entry for itself.
- **Thread-local storage**: the standard `PT_TLS` program header (an initialised part and a zero-filled part, copied for each thread).
- **System calls**: the `sc` instruction with the call's number in `r11`. These and the imports are the whole boundary between the program and the system. Which system calls the game makes directly is for the control-flow pass to list.

**The SELF.** A header that is not encrypted, then the ELF's contents with every loaded segment encrypted. The header holds the SELF's own fields (version, key revision, the program's type and authentication id), a copy of the ELF header and its program headers, the section headers, a table saying where each segment is stored, and (in this game's) the place of a list of the SDK libraries the program was linked from. `qilin` reads all of that and decrypts nothing.

**PARAM.SFO.** A small little-endian table of keys and values: the game's title, id and version, and the picture and sound formats it says it supports.

Every table is validated when a file is opened: a field that doesn't fit is an error naming it, never a guess. The loaders' reading of the formats is tested on synthetic files (`recomp/tests/elf_builder.hpp`), and the SELF and PARAM.SFO readers on the owner's disc. The ELF loader met the real program at v0.3.0 and read it with nothing to fix (below).

## The owner's disc (read 2026-11-21, v0.1.0)

*Ridge Racer 7* (USA), Redump's "Ridge Racer 7 (USA) (En,Ja)".

| | |
|---|---|
| Title id, version | `BLUS30001`, `01.02`; system version `01.0200` |
| Picture modes | 480, 720, 1080, 480 (16:9) |
| Sound formats | LPCM 2.0, LPCM 5.1, Dolby Digital |
| `EBOOT.BIN` | 7,821,232 bytes, SHA-256 `095ed6cb8e90538309b57662fb19da1ba2c4bdbac5fd40dc0be6122e76d4c0db` (`qilin` recognises it) |
| SELF | key revision 1, an application (type 4), every loaded segment encrypted |
| Entry point | the descriptor at `0x4bb1a0` |
| The decrypted program's SHA-256 | `689f9704ee9dbd982c48de646f07d69c193a46513d75b6f95f20a206c7754831`, as RPCS3 0.0.43 writes it (`known_build` in `recomp/src/title.cpp`) |

### Program headers

| | Address | In the file | In memory | |
|---|---|---|---|---|
| 0 | `0x00010000` | `0x47ae10` | `0x47ae10` | code and read-only tables (r-x) |
| 1 | `0x00490000` | `0x48f84` | `0x48f84` | data beside the code: constructors, descriptors, the table of contents (rw-) |
| 2 | `0x10000000` | `0x148d00` | `0x148d00` | read-only data (r--), with flag `0x400000` |
| 3 | `0x10150000` | `0x13c598` | `0x6751d0` | data, then 5.2 MB zero-filled (rw-), with flag `0x600000` |
| 4 | 0 | 0 | 0 | an empty segment with flags `0x6600006` |
| 5 | `0x004d8f80` | 4 | `0x1b8` | thread-local storage |
| 6 | `0x0048adc8` | `0x20` | | process parameters |
| 7 | `0x0048ade8` | `0x28` | | PRX parameters |

The flags above the usual three bits are the PS3's own (which processor a segment is for, by public descriptions). *Their meaning is to be confirmed, and whether anything depends on them.*

### Sections

The SELF keeps the section headers plain but the table of their names encrypted. The names below are **inferred** from each name's length (the headers give every name's offset in the table) and the PS3 linker's usual layout; the decrypted ELF was expected to confirm or correct them, but RPCS3 writes the table of names as zeros (below), so they stay inferred. What has been confirmed by content: `.text` (the entry point is its first word), the stubs, the import tables, and the nine-letter section, which is the SPU programs.

| Address | Size | Likely name | What it tells us |
|---|---|---|---|
| `0x00010200` | `0x2c` | `.init` | |
| `0x0001022c` | `0x40e1c0` | `.text` | 4.25 MB of code: 1,063,024 instruction words |
| `0x0041e3ec` | `0x24` | `.fini` | |
| `0x0041e410` | `0x1f00` | `.sceStub.text` | 248 import stubs of 32 bytes |
| `0x00420310` | `0x5fe34` | `.eh_frame` | C++ exception unwinding tables: they bound every function that can unwind, a gift to function discovery |
| `0x00480148` | `0xa370` | `.gcc_except_table` | C++ exception handlers: the game throws or at least catches |
| `0x0048a4b8` | `0x158` | `.rodata.sceResident` | the imported libraries' names |
| `0x0048a610` | `0x3e0` | `.rodata.sceFNID` | 248 function ids |
| `0x0048a9fc` | `0x3c8` | `.lib.stub` | 22 import entries of 44 bytes: **22 libraries, 248 functions** |
| `0x0048adc8` | `0x20` | `.sys_proc_param` | |
| `0x0048ade8` | `0x28` | `.sys_proc_prx_param` | |
| `0x00490000` | `0x2ec` | `.ctors` | about 185 static constructors |
| `0x004902ec` | `0x11c` | `.dtors` | |
| `0x0049040c` | `0x1d728` | `.data.rel.ro` | virtual tables and other constant pointers |
| `0x004adb34` | `0x3e0` | `.data.sceFStub` | the 248 import slots |
| `0x004adf18` | `0xd278` | (four letters) | |
| `0x004bb190` | `0x1b6f0` | `.opd` | **14,046 function descriptors**: an upper bound on the functions whose address is taken, and a list of function starts |
| `0x004d6880` | `0x2700` | `.got` | |
| `0x004d8f80` | 4, then `0x1b0` zero | `.tdata`, `.tbss` | thread-local storage |
| `0x10000000` | `0x52aac` | `.rodata` | |
| `0x10052b00` | `0xf6200` | (nine letters) | 1,008,128 writable bytes aligned to 128: **the 42 SPU programs** (below) |
| `0x10150000` | `0x13c598` | `.data` | |
| `0x1028c5a0` | `0x538c30` | `.bss` | 5.2 MB zero-filled |

### What it was linked from

The SELF leaves plain a list of the SDK's library stamps: every object file linked in, by library and SDK version. All are `p102002` (SDK 1.02).

| Linked into the program (to be lifted with it) | Object files |
|---|---|
| `libc` | 140 |
| `libm` | 31 |
| `libstdc++` | 21 |
| `libgcm_cmd` (the command-buffer writers of the graphics library) | 4 |
| `crt0`, `crt1` | 1 each |

| Stub libraries (the system libraries they reach are replaced natively) | Stub objects | What for |
|---|---|---|
| `liblv2_stub` | 18 | the system's own calls: threads, memory, timers, PRX loading |
| `libsysmodule_stub` | 3 | loading system modules |
| `libgcm_sys_stub` | 16 | graphics: the RSX's set-up, flips, memory mapping |
| `libspurs_stub` | 14 | **SPURS**: jobs on the SPUs |
| `libaudio_stub` | 10 | sound output |
| `libatrac3plus_stub`, `libadec_stub` | 15, 8 | ATRAC3plus and other audio decoding |
| `libpamf_stub`, `libdmux_stub`, `libvdec_stub` | 11, 11, 8 | the `.pam` films: container, demultiplexer, video decoder |
| `libio_stub` | 10 | controllers (and possibly keyboard and mouse) |
| `libusbd_stub` | 12 | USB devices addressed directly (steering wheels are the likely reason) |
| `libfs_stub` | 11 | files |
| `libsysutil_stub` | 27 | system utilities: saves, the video and audio output settings, system callbacks |
| `libsysutil_np_stub`, `libsysutil_np_matching_int_stub_03` | 47, 6 | PlayStation Network: accounts, matchmaking |
| `libnet_stub`, `libnetctl_stub` | 8, 5 | the network |
| `libfont_stub`, `libfontFT_stub` | 20, 3 | the system's font renderer |
| `libl10n_stub` | 3 | character-set conversion |
| `librtc_stub` | 6 | the clock |

The compiler is GCC (the C++ runtime is `libstdc++`, and the program has `.eh_frame` and `.gcc_except_table`). *Its version is in the ELF's `.comment` section, which the SELF encrypts.*

### The disc's other files

In `USRDIR`: `RR7.DAT` (5.23 GB: the game's own archive), `BGM.nub` (616 MB) and `Game_DJ.nub` (152 MB) (Namco's sound banks: music and the announcer), and seven `.pam` films (`intro`, `ending`, `menu`, `title`, `title2`, `auroravision`, `e3`; 1.19 GB together). Their headers have been read since (v0.2.0): `data-files.md`.

## The decrypted program (read 2026-11-21, v0.3.0)

Made on the owner's PC with RPCS3 0.0.43 (`rpcs3.exe --decrypt <EBOOT.BIN>` does from the command line what the menu does, and writes `EBOOT.elf` beside it). **The loader read it with nothing to fix**: every table was where and what the loader expected, and the ELF's headers match the SELF's plain copy.

| | |
|---|---|
| `EBOOT.elf` | 7,818,800 bytes (the SELF less its `0x980`-byte header), SHA-256 `689f9704ee9dbd982c48de646f07d69c193a46513d75b6f95f20a206c7754831` (`qilin` recognises it) |
| Entry point | the descriptor at `0x4bb1a0`: code at `0x1022c` (the first word of `.text`), `r2` = `0x4de880` |
| Process parameters | SDK version `0x102002`, main thread priority 1001, stack `0x20000` (128 KiB), heap page size `0x100000` (1 MiB) |
| Thread-local storage | at `0x4d8f80`: 4 bytes initialised, `0x1b8` in all, aligned to 8 |
| Imports | 22 libraries, 248 functions, no variables; **all 248 slots hold the address of their stub** |
| Exports | none: the export table is empty |

**What RPCS3's file lacks.** It writes zeros for the three sections that lie outside every segment: `.comment`, the list of SDK stamps (which the SELF holds plain, and `qilin` reads from there), and **the table of section names**. So the section names above stay inferred, and the PPU compiler's version can't be read from `.comment`. The SPU programs carry their own `.comment` (below), and the whole game was presumably built with one toolchain. *Whether the hash is the same from other versions of RPCS3 is not known*: it should be, since the decryption has one right answer and the rest is zeros.

### The imported libraries

| Library | Functions | | Library | Functions |
|---|---|---|---|---|
| `sceNp` | 46 | | `cellPamf` | 10 |
| `cellSysutil` | 26 | | `cellDmux` | 10 |
| `cellFont` | 18 | | `sys_io` | 9 |
| `sysPrxForUser` | 17 | | `cellAudio` | 9 |
| `cellGcmSys` | 15 | | `sys_net` | 7 |
| `cellAtrac` | 14 | | `libvdec` | 7 |
| `cellSpurs` | 13 | | `cellAdec` | 7 |
| `cellUsbd` | 11 | | `sceNpMatchingInt` | 5 |
| `sys_fs` | 10 | | `cellRtc` | 5 |
| `cellNetCtl` | 4 | | `cellL10n` | 2 |
| `cellSysmodule` | 2 | | `cellFontFT` | 1 |

Every entry has version 1 and attributes 9. Each stub library of the SDK's list gives one function fewer than it has object files (one object is the library's own entry).

### The imports by name

**All 248 are named** (`recomp/src/import_names.*`; `qilin imports` prints them). An id is the first four bytes, read little-endian, of the SHA-1 of the function's name followed by a fixed 16-byte suffix, so an id can't be turned back into a name but a proposed name can be checked. The names proposed were the SDK's own names for each library's functions, as public documentation and homebrew headers give them; each of the 248 was accepted only when its computed id matched. The computation was done outside the repository: **the table holds the pairs, and the suffix is not in this repository** (it is not a key and decrypts nothing, but the project keeps clear of Sony's constants as a matter of course; the owner may decide otherwise, and then a test can recompute every id). PSL1GHT's export lists (MIT) carry 138 of the same ids in the same libraries, under PSL1GHT's own names: an independent check that those ids are real.

What the names say about the game:

- **Graphics** (`cellGcmSys`, 15): initialisation, the display buffers and flips, tiled and Zcull memory, the vertical-blank handler. Everything else is the command stream.
- **SPUs**: SPURS **tasksets** (`cellSpursCreateTaskset`, `cellSpursCreateTask`, event flags, event queues), so the 41 SPURS-shaped SPU programs are tasks, not jobs; and `sys_spu_image_import` and `sys_spu_image_close`, for an SPU program started outside SPURS (*`bnusCoreSpu.elf`, presumably*; the thread-group calls it needs are system calls).
- **Sound**: `cellAudio` ports with an event queue; `cellAtrac` (14: the music, with its loops and second buffer); `cellAdec` (7: the films' sound).
- **Films**: `cellPamf`, `cellDmux`, `cellVdec`: the system's demultiplexer and video decoder, all three to be replaced natively.
- **Controllers**: `cellPad` with pressure and motion sensors asked for; `cellUsbd` (11) with its own device driver registered (`cellUsbdRegisterExtraLdd`): steering wheels, driven by the game itself.
- **Saves and the hard disk**: `cellSaveData` in its list, fixed and automatic forms; `cellGameDataCheckCreate2` (the game keeps data on the hard disk); `cellDiscGameGetBootDiscInfo`.
- **Dialogs**: `cellMsgDialog`, `cellOskDialog` (the on-screen keyboard), `cellNetCtl`'s network-start dialog.
- **Video and audio output**: `cellVideoOut` (state, resolution, configure) and `cellAudioOut` (availability, configure).
- **Text**: `cellFont` with FreeType (18 and 1), `cellL10n` (two conversions to UTF-8).
- **Files** (`cellFs`, 10): open, read, write, seek, stat, directories, unlink. No asynchronous or streaming reads.
- **Network**: `sceNp` (46: matchmaking, scores, signalling, messages, presence) and five sockets calls. All of it for the online modes, which are out of scope.
- **The process** (`sysPrxForUser`, 17): threads, lightweight mutexes, memory pools, thread-local storage, exit.

### The code

`qilin profile`: **1,065,028 words** in the four executable sections; **1,059,425 decode** (99.47%).

- **The 5,603 that don't are data in the code**: 303 runs of words, 291 of them straight after a `bcctr` and the rest after a function's last instruction. They are **jump tables of signed offsets** (the values are small, positive or negative) and a few floating-point constants. The control-flow pass will read them as such; none is an instruction the decoder lacks, as far as a look at them shows.
- **151 of the decoder's 383 instructions are used.** 200,564 words are 64-bit-only forms (`ld`, `std`, `rldicl`, `extsw`: the compiler widens and narrows constantly, although every pointer is 32 bits), 66,222 floating-point, 23,511 VMX.
- **VMX use is narrow**: `lvx`, `stvx`, `vmaddfp` and the rest of the float arithmetic, `vspltw`, `vsldoi`, `vsel`, `vperm`, the word merges, the logical operations, conversions to and from integers. Of integer vector arithmetic only `vadduwm` and `vcmpequw`; no saturating forms, no packs, none of the Cell's own unaligned loads and stores, no data-stream hints.
- **Not used at all**: `lwarx`/`stwcx.` and `ldarx`/`stdcx.` (the program takes its locks through the system's libraries, never inline), `dcbz` and every other cache instruction except `dcbt` (58), the string and multiple-word loads and stores, the byte-reversed ones, `mcrxr`, `mtcrf` (the one-field `mtocrf` is used, 425 times, and `mfocrf` 617).
- Of the loads and stores with update, only `stdu` (9,150: the stack frames), `lbzu` and `stbu` (6 each) and `ldu` (1).
- One each of `sync`, `tw`, `fsqrts`, `mffs` and `mtfsf`; 13 `mftb` (the time base read directly).
- **502 `sc` instructions**: the program makes system calls directly, from the system-call wrappers linked into it. A scan for the constant put in `r11` just before finds it at 477 of them: **78 different system calls**, among them the families at 90–108 (semaphores, mutexes, condition variables), 128–147 (event queues, timers), 341–352 (memory), 801–834 (files). The other 25 sites need the control-flow pass.
- 4,943 `bcctrl` (calls through a pointer: virtual calls, callbacks and the import stubs) and 544 `bcctr` (switches and tail calls).

### The SPU programs

`qilin spu`: the section at `0x10052b00` is **42 SPU programs**, each a whole ELF file (32-bit, machine 23), one after another, 1,004,604 bytes and **209,660 words of SPU code** in all: a fifth as much again as the PPU's.

| | Programs | |
|---|---|---|
| Particles and effects | 23 | pairs named `...FrameMove` and `...CreateVertex` for after-fire, tyre marks, glass, fireworks, leaves, light blur, nitro, smoke, sparks and water, and one for gravity; 2,300 to 3,800 words each |
| Collision | 5 | car against car and car against road, checks and reactions; 2,700 to 7,200 words |
| Car dynamics | 12 | road forces, springs and dampers, friction, torque, the update itself, and six named `CalcCarRI_...` of about 14,000 words each |
| Sound | 2 | a small one, and `bnusCoreSpu.elf` (17,746 words), by its name Namco's sound core |

- **The car's physics runs on the SPUs.** There is no playing the game without recompiling them, so the SPU decoder and lifter are not optional.
- 41 are linked at `0x3000` with their entry at `0x3010`: the shape of a SPURS program, loaded by the system's SPURS kernel (which this project replaces natively). `bnusCoreSpu.elf` is linked at `0x80` with its entry at `0x90`: *a program that owns its SPU outright, by the look of it*, started some other way.
- Each has `.text`, `.rodata`, `.data`, `.bss`, constructor tables, a 16-byte `.SpuGUID`, and a `.note.spu_name` giving the file it was built as. All fit an SPU's 256 KiB with room to spare (the largest reaches `0x2ddd0`, most of it `.bss`).
- Their `.comment` names the compiler: **GCC 4.0.2** (two builds of Sony's Cell toolchain, `CELL 4.1.7` and `CELL 4.1.13`), and their `.sceversion` the SDK's `libspurs`, at `s102002`.

## Next

1. ✅ The decrypted program read (v0.3.0).
2. ✅ Names for the 248 ids. Still to do: names for the 78 system calls.
3. ✅ The PPU decoder (v0.2.0, `recompiler-core.md`), its profile of the real code and the SPU programs found (v0.3.0).
4. The decoder against objdump over every word of the real code (it needs a PowerPC objdump where the game is); the control-flow pass, which reads the jump tables and the remaining 25 system calls; the SPU decoder.

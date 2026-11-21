# The program: getting it, its format, and what has been read

How to get the game's program, what a PS3 program is, and what `qilin` reads from it. The owner's disc has been read as far as it can be without decrypting (v0.1.0, below); the decrypted program has not been read yet.

## Getting the program

The game's code is one file on the disc, `PS3_GAME/USRDIR/EBOOT.BIN`. It is a **SELF**: the program encrypted with Sony's keys. This project has no keys and will never carry any, so the player makes the decrypted program from their own disc, once:

1. In RPCS3: **Utilities > Decrypt PS3 Binaries**, and choose your disc's `EBOOT.BIN`. It writes `EBOOT.elf`.
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

Every table is validated when a file is opened: a field that doesn't fit is an error naming it, never a guess. The loaders' reading of the formats is tested on synthetic files (`recomp/tests/elf_builder.hpp`), and the SELF and PARAM.SFO readers on the owner's disc. **The ELF loader has not met the real program yet**: each assumption above is a hard error if the real file contradicts it, so the first run says which.

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
| The decrypted program's SHA-256 | *to be recorded by the first analysis* (`known_build` in `recomp/src/title.cpp`) |

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

The SELF keeps the section headers plain but the table of their names encrypted. The names below are **inferred** from each name's length (the headers give every name's offset in the table) and the PS3 linker's usual layout; the decrypted ELF will confirm or correct them.

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
| `0x10052b00` | `0xf6200` | (nine letters) | 1,008,128 writable bytes aligned to 128, the alignment SPU program images are given: **probably the embedded SPU programs** |
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

## Next

1. The owner decrypts `EBOOT.BIN` (above); `qilin info` on the result. Fix whatever the ELF loader gets wrong on the real file, record the hash, replace the inferred section names, and list the 22 libraries with every id taken from each.
2. Names for the 248 ids (`prior-art.md`).
3. ✅ The PPU decoder (v0.2.0, `recompiler-core.md`); its profile of the real code, and the SPU programs found and counted, wait for step 1.

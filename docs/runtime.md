# The runtime

What the lifted game runs on: `runtime/` (what lifted code compiles against) and `ridgeracer7/src/` (the game's machine). Started at v0.11.0; this page says what exists and how it was checked. At v0.12.0 the game ran as far as graphics initialisation; at v0.13.0 it runs to the start of SPURS.

## Building and running the game

```
powershell -ExecutionPolicy Bypass -File tools\build-ridgeracer7.ps1 -Game <your copy> [-Run]
```

The script builds `qilin`, lifts your copy's program to C++ (`qilin lift <game> --out build-game\gen`), and builds `ridgeracer7` from that. By hand: configure with `-DQILIN_GEN_DIR=<the folder lift wrote>` and build the `ridgeracer7` target. `ridgeracer7 <game>` then runs it.

- The executable holds the lifted code of **your copy's** program, and nothing else of the game: the program's data is read from your copy each time it starts. At start-up it checks that the program it is given is the one it was built from (by SHA-256) and refuses any other.
- The lifted code and the executable are derived from the game: they stay on your PC, in folders the repository ignores.
- The first build compiles 14,294 functions: about 14 minutes on the owner's PC with one compiler at a time (v0.11.0 turns on MSVC's `/MP` for the lifted files). `qilin lift` rewrites only the files that changed, so later builds compile only those.

## The machine (`ridgeracer7/src/machine.*`)

- **Memory.** 4 GiB of host address space reserved in one piece (`rt::AddressSpace`), so a guest address is a host address by one addition; only what the guest has been given is backed by memory, and an access outside it faults. The program's segments go at the addresses they were linked for (code at `0x00010000`, data at `0x10000000`): nothing is relocated.
- **The first thread.** A stack of the size the program's parameters ask for (128 KiB), `r2` from the entry point's descriptor, and the registers the system hands a new process: the arguments in `r3` to `r5`, the thread's id in `r7`, the thread-local image's address and sizes in `r8` to `r10`, the heap's page size in `r12`. *The assignment is read off the start-up code's own use of them (it passes `r7` to `r10` straight to `sys_initialize_tls` and keeps `r12`) and public descriptions.*
- **Calls through a register** (`hooks->call`) look the target up in the table of lifted functions; a target that is no function stops the game.
- **The boundary with the system is exact, and anything not provided stops the game by name.** A PS3 game reaches the system only through its 248 imported functions and its 78 system calls:
  - each import's slot is given a descriptor whose code address is the import's own, in a range of guest addresses where nothing is, so the game's own import stub (which is lifted like any other code) arrives at `hooks->call` with an address that says which import it is;
  - a system call arrives at `hooks->system_call` with its number in `r11`.
  A function or call the runtime provides is run; any other ends the process with one line: its name, its library and id (or its number), and where it was called from. Nothing is answered with a guess.
- **Unlifted functions** (the 2 the lifter doesn't cover yet) exist as functions that stop the game with their reason if called.

## What is provided

Each was added when the game reached it, and does what the system's function does as far as the game can tell. The files are in `ridgeracer7/src/`.

**Imports** (94 of the game's 248):

| | Functions | What they are here |
|---|---|---|
| `imports.cpp` | `sys_initialize_tls`, `sys_time_get_system_time`, `sys_ppu_thread_get_id`, `sys_process_exit`, `cellSysmoduleLoadModule`, `cellSysmoduleUnloadModule` | thread-local storage (a copy of the image, `r13` 0x7000 past it: read off the game's 27 uses of `r13`); the system's clock; loading a system module is nothing to do, since every system library is the runtime's own (the game loads eight: `0x1e`, `0x19`, `0x1b`, `0x1a`, `0x1c`, `0x9`, `0`, `0x16`) |
| `sync.cpp` | `sys_lwmutex_create`, `_destroy`, `_lock`, `_trylock`, `_unlock` | lightweight mutexes: a host lock found by the id kept in the guest's structure, whose owner and count are written back as the library would leave them |
| `threads.cpp` | `sys_ppu_thread_create`, `sys_ppu_thread_exit` | a guest thread is a host thread with its own Cpu, stack and thread-local storage |
| `mempool.cpp` | `sys_mempool_create`, `_destroy`, `_allocate_block`, `_try_allocate_block`, `_free_block` | equal blocks from memory the caller gives |
| `sysutil.cpp` | `cellSysutilGetSystemParamInt`, `cellSysutilGetSystemParamString`, `cellDiscGameGetBootDiscInfo`, `cellSysutilRegisterCallback`, `UnregisterCallback`, `CheckCallback` | the console's settings (fixed for now: US English, cross confirms; the settings menu's Game page will hold them), and the disc's title id and parental level from `PARAM.SFO`. A parameter with no value here stops the game. The system's events (the player asks to quit) are a queue the runtime can put to (`system_event`), handed to the game's registered functions at its per-frame check; nothing puts to it yet |
| `gamedata.cpp` | `cellGameDataCheckCreate2` | the game's folder on the hard disk (below) |
| `fs.cpp` | `cellFsOpen`, `Close`, `Read`, `Write`, `Lseek`, `Fstat`, `Opendir`, `Readdir`, `Closedir`, `Unlink` | the file system (below) |
| `spurs.cpp` | `cellSpursInitialize`, `cellSpursAttachLv2EventQueue`, `cellSpursCreateTaskset` | the SPURS instance (4 SPUs) and its tasksets (the game makes four). `cellSpursCreateTask` stops the game and says which SPU program it was handed: that needs the SPU lifter |
| `net.cpp` | `sys_net_initialize_network_ex`, `sys_net_finalize_network`, `cellNetCtlInit`, `cellNetCtlTerm` | **a console with no network cable**: the libraries start, as they do on one. Sockets and the connection dialog are not provided yet |
| `np.cpp` | `sceNpInit`, `sceNpTerm`, `sceNpManagerGetStatus` | PlayStation Network on **a console that is not signed in**: the status is "offline" |
| `video.cpp` | `cellVideoOutGetNumberOfDevice`, `GetState`, `GetDeviceInfo`, `GetResolution`, `GetResolutionAvailability`, `Configure` | **the television is the runtime's to define**: one HDMI display, 16:9, 59.94 Hz, taking 1080p, 1080i, 720p and 480; it reports 1080p, the game's best, until the settings menu holds the choice. The game configures 1920 by 1080, pitch 8192. The sound output (`cellAudioOutGetSoundAvailability`, `cellAudioOutConfigure`, in the same file) is likewise defined: **two channels of linear PCM at 48 kHz** and nothing else, which the game takes (down-mixer 2). (The display information's modes begin at byte 32: read off the game's loop over them) |
| `gcm.cpp` | `_cellGcmInitBody`, `cellGcmGetConfiguration`, `AddressToOffset`, `GetControlRegister`, `GetLabelAddress`, `GetTiledPitchSize`, `SetDisplayBuffer`, `SetFlipMode`, `SetTile`, `SetInvalidateTile`, `SetZcull`, `SetFlip`, `SetWaitFlip`, `SetVBlankHandler` | the RSX's set-up (below) |
| `font.cpp` | `cellFontInitializeWithRevision`, `cellFontInitLibraryFreeTypeWithRevision`, and fourteen more: renderers, font sets, fonts in memory, scale, layout, glyph metrics, glyph images | the font library, on FreeType (below) |
| `pad.cpp` | `cellPadInit`, `cellPadEnd`, `cellPadGetInfo`, `cellPadGetData` and the five others by port | **a console with no controller plugged in**: host input isn't connected yet, and "no device" is an answer the real system gives |
| `usbd.cpp` | `cellUsbdInit`, `cellUsbdRegisterExtraLdd`, `cellUsbdUnregisterExtraLdd` | **a USB bus with nothing on it**: the game's own driver (for steering wheels) is registered and, since nothing attaches, never called |

**System calls** (42 of the game's 78):

| | Calls | |
|---|---|---|
| `memory.cpp` | 341, 342; 348, 349, 352 | memory containers (part of the process's memory set aside to lend a system utility: the game makes one of 3 MiB for its game-data check); memory in pages of 1 MiB or 64 KiB from one range of addresses, first fit. *The total a game may have, 213 MiB, is from public descriptions: the game asks for it, so it is to be confirmed* |
| `threads.cpp` | 43, 44, 45, 47, 48 | yield, join, detach, priorities |
| `sync_calls.cpp` | 90-94, 114; 100-104; 105-108; 141, 142, 145, 147 | semaphores, mutexes, condition variables; sleeping, the calendar's time, the time base's frequency. **A semaphore's post goes to the thread that has waited longest**: the game's threads greet each other by posting and then waiting on the same semaphore, and a post the poster could take back itself left the game reading a null pointer |
| `events.cpp` | 128-130, 134-138 | event queues and ports. The receive call returns its event in `r4` to `r7`: read off the game's code after each `sc` |
| `timers.cpp` | 70, 71, 73-76 | timers that send an event when due, served by one host thread. (Their names were among the ten unconfirmed; the game's use of them fits) |
| `tty.cpp` | 402, 403 | the console's terminal: what the game prints goes to the log as `[the game] ...` |
| `spu.cpp` | 169 | the process says how many SPUs it will use (six, none raw) |

**Not yet as designed: the scheduler.** Every guest thread simply runs as a host thread; priorities are kept and reported but decide nothing, and apart from semaphores waiters are woken in the host's order. The roadmap's design lets two guest threads run guest code at once, by priority, as the console's two hardware threads do. Until that exists, a game that leans on the console's order of running could go wrong here: when something looks like a race, this is the first suspect.

## The RSX (`gcm.cpp`, `rsx.cpp`)

The game writes the RSX's commands itself (that code is the game's, and lifted), into a buffer in its own memory which `cellGcmInit` maps for the RSX: 64 KiB of commands in 49 MiB at `0x20100000`. The layout handed out is the console's by public descriptions: the RSX's own memory (249 MiB) at `0xC0000000`, the mapped memory at offset 0, the first 4 KiB kept back, the command buffer in 32 KiB pieces of which the context holds one, and a callback (a function of the runtime's the game can call: `native_function`) that moves to the next piece when one is full.

- **The RSX is a thread** that runs the commands between `get` and `put` in the control register: jumps, calls and returns, and methods with their arguments. A jump to itself is the game's way of holding the RSX, and is waited on.
- **Methods that only set state are kept in a table** (`kState`: the chip's public names for them); the few that act read it. So far the acting ones are the labels (`SEMAPHORE_OFFSET`, `RELEASE`, `ACQUIRE`; `BACK_END_WRITE_SEMAPHORE_RELEASE`, which exchanges two of its argument's bytes as the chip does; `TEXTURE_READ_SEMAPHORE_RELEASE`) and `SET_REFERENCE`. **A method that is neither stops the game with its number**: nothing is skipped. Drawing is not provided yet.
- **Flips** reach the RSX as two commands of the runtime's own (`0xE940`, `0xE944`, outside the chip's methods), put in the stream by `cellGcmSetFlip` and `cellGcmSetWaitFlip` so that they happen in order with the drawing. Waiting for a flip when none is pending does nothing; showing one is not provided yet.
- **The vertical blank** is a thread at 59.94 Hz which calls the handler the game registers, as one of the system's own threads does on a console.
- Tiles and zcull regions are speed on a console, not results: their parameters are kept and change nothing. `cellGcmSetTile` stops the game on parameters out of the ordinary, because the library's own answer to them isn't known here.

## Fonts (`font.cpp`)

The game draws its text with the system's font library: it opens one of the console's own font sets (`0x101`, the default gothic set), sets a size, and has glyphs drawn, which it copies into its own buffers itself. (What the calls take and leave was read off the game's own loop around `cellFontRenderCharGlyphImage`.)

- The console's library is built on **FreeType**, and so is this: FreeType 2.13.3, fetched and pinned by CMake.
- **The console's fonts are the player's to supply, as the game is**: the `data/font` folder of a PS3's system software. Sony publishes the system software file (`PS3UPDAT.PUP`) on its support site; RPCS3 installs it (`rpcs3 --installfw PS3UPDAT.PUP`) and leaves a `dev_flash` folder beside itself. `ridgeracer7 <game> --flash <dev_flash>` names it; without the option, an `RPCS3` folder beside the game's is looked in. Nothing stands in for a missing font: the game stops and says what it needs.
- *Stated choices, to be compared with the console's picture:* glyphs are loaded unhinted; "scale pixel" is the em's size in pixels; glyphs sit on whole pixels; font set `0x101` is New Rodin's Japanese font with the extension font behind it. A character the font doesn't have stops the game (the library's answer to that isn't known here).

## Files (`fs.cpp`, `gamedata.cpp`)

A game names files by the console's mount points, and each is a folder of the host's:

| | |
|---|---|
| `/dev_bdvd` | the disc: the player's copy (the folder holding `PS3_GAME`). Read only |
| `/app_home` | where the program started from: the disc's `PS3_GAME/USRDIR`. Read only |
| `/dev_hdd0` | the hard disk: `dev_hdd0` in the port's data folder (`qilin/ridgeracer7` in the per-user application data, or `RIDGERACER7_SAVE_DIR`), laid out as a console's |
| `/dev_flash` | the system software, if the player gave one. Read only |

A path is kept inside its mount point (`..` can't climb out), and a name under no mount point is "not mounted". A directory's entries come in the names' order, the same on every host. The structures' layout is the SDK's as the game's own code reads it: a file's size at byte 36 of a packed, 52-byte `CellFsStat`. *Times are left at 0 until the game is seen to read them.* The file-system system calls (801 on: the C library's files) are not provided yet.

**Game data** is a folder of the game's on the hard disk (`/dev_hdd0/game/<name>`, described by a `PARAM.SFO` as on a console; `qilin_game`'s reader gained a writer). The utility works through a function of the game's: it is told what is there and answers whether to go on. The layout of what it is told (5,192 bytes; the description at byte 2,144) agrees with the game's own two functions. **The game asks for `BLJS10001`**, the Japanese release's name: the disc is the American one, the program the same. At start-up it only looks (its function answers "cancel"), finds nothing, and goes on.

## When something goes wrong

- **A bad memory access is reported** (`fault.cpp`): the guest address, the thread, and the lifted functions on the stack, found from the host's return addresses. Then the process stops, as for any other unknown. Before this a fault ended the process without a word.
- `qilin lift --pc` makes lifted code note each instruction's address before it touches memory (`Cpu::pc`), and the report then names the instruction. It is slower, so it is for finding faults, not for playing.
- Two aids in the environment, neither a way to play: `RR7_WATCH=<seconds>` prints what system function each guest thread is inside after that long, and stops; `RR7_RSX_SURVEY` counts the RSX methods not yet provided up to the first flip, without running them, prints the list and stops.

## Where it stops now

After v0.13.0: **`cellAudioInit`**. By then the game has started SPURS (4 SPUs) and made four tasksets, entered its main loop's system-event check, looked for its game data on the hard disk, opened a directory on the disc, started the network and PlayStation Network libraries (and been told: no cable, not signed in), and set the sound output to stereo. Next is the audio library itself: ports of mixed samples the system takes every 256 samples, out through `host/`'s sound output.

Still ahead, and large: **the SPU lifter** (the first SPURS task or SPU thread the game starts needs its program lifted; all 42 programs end with the system's "thread exit" stop, `0x102`, so they are built as plain SPU threads, whatever SPURS is handed), and **the RSX's drawing on Vulkan**.

## Checked

- The first run on the owner's PC (2026-11-21): the program loads, its entry point and start-up code run as lifted native code, and the process stops, by name, at the first system function not provided. With `sys_initialize_tls` provided it runs on to the next.
- v0.12.0, on the owner's PC: with 40 imports and 39 system calls provided, the game runs through its start-up to `_cellGcmInitBody`, printing its force-feedback library's version on the way.
- v0.13.0, on the owner's PC: with 67 imports and 40 system calls provided, the game runs to `cellSpursInitialize`. The host layer's tests pass there (a Vulkan 1.3 device, an offscreen frame read back, a virtual controller).
- `ridgeracer7_machine` is compiled in CI on all six configurations; the executable itself needs the game and is built only on the owner's PC.

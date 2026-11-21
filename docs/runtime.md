# The runtime

What the lifted game runs on: `runtime/` (what lifted code compiles against) and `ridgeracer7/src/` (the game's machine). Started at v0.11.0; this page says what exists and how it was checked. At v0.12.0 the game runs as far as graphics initialisation.

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
- **Unlifted functions** (the 98 the lifter doesn't cover yet) exist as functions that stop the game with their reason if called.

## What is provided

Each was added when the game reached it, and does what the system's function does as far as the game can tell. The files are in `ridgeracer7/src/`.

**Imports** (40 of the game's 248):

| | Functions | What they are here |
|---|---|---|
| `imports.cpp` | `sys_initialize_tls`, `sys_time_get_system_time`, `sys_ppu_thread_get_id`, `sys_process_exit`, `cellSysmoduleLoadModule`, `cellSysmoduleUnloadModule` | thread-local storage (a copy of the image, `r13` 0x7000 past it: read off the game's 27 uses of `r13`); the system's clock; loading a system module is nothing to do, since every system library is the runtime's own (the game loads eight: `0x1e`, `0x19`, `0x1b`, `0x1a`, `0x1c`, `0x9`, `0`, `0x16`) |
| `sync.cpp` | `sys_lwmutex_create`, `_destroy`, `_lock`, `_trylock`, `_unlock` | lightweight mutexes: a host lock found by the id kept in the guest's structure, whose owner and count are written back as the library would leave them |
| `threads.cpp` | `sys_ppu_thread_create`, `sys_ppu_thread_exit` | a guest thread is a host thread with its own Cpu, stack and thread-local storage |
| `mempool.cpp` | `sys_mempool_create`, `_destroy`, `_allocate_block`, `_try_allocate_block`, `_free_block` | equal blocks from memory the caller gives |
| `sysutil.cpp` | `cellSysutilGetSystemParamInt`, `cellSysutilGetSystemParamString`, `cellDiscGameGetBootDiscInfo` | the console's settings (fixed for now: US English, cross confirms; the settings menu's Game page will hold them), and the disc's title id and parental level from `PARAM.SFO`. A parameter with no value here stops the game |
| `pad.cpp` | `cellPadInit`, `cellPadEnd`, `cellPadGetInfo`, `cellPadGetData` and the five others by port | **a console with no controller plugged in**: there is no host input yet, and "no device" is an answer the real system gives |
| `usbd.cpp` | `cellUsbdInit`, `cellUsbdRegisterExtraLdd`, `cellUsbdUnregisterExtraLdd` | **a USB bus with nothing on it**: the game's own driver (for steering wheels) is registered and, since nothing attaches, never called |

**System calls** (39 of the game's 78):

| | Calls | |
|---|---|---|
| `memory.cpp` | 348, 349, 352 | memory in pages of 1 MiB or 64 KiB from one range of addresses, first fit. *The total a game may have, 213 MiB, is from public descriptions: the game asks for it, so it is to be confirmed* |
| `threads.cpp` | 43, 44, 45, 47, 48 | yield, join, detach, priorities |
| `sync_calls.cpp` | 90-94, 114; 100-104; 105-108; 141, 142, 145, 147 | semaphores, mutexes, condition variables; sleeping, the calendar's time, the time base's frequency |
| `events.cpp` | 128-130, 134-138 | event queues and ports. The receive call returns its event in `r4` to `r7`: read off the game's code after each `sc` |
| `timers.cpp` | 70, 71, 73-76 | timers that send an event when due, served by one host thread. (Their names were among the ten unconfirmed; the game's use of them fits) |
| `tty.cpp` | 402, 403 | the console's terminal: what the game prints goes to the log as `[the game] ...` |

**Not yet as designed: the scheduler.** Every guest thread simply runs as a host thread; priorities are kept and reported but decide nothing, and waiters are woken in the host's order. The roadmap's design lets two guest threads run guest code at once, by priority, as the console's two hardware threads do. Until that exists, a game that leans on the console's order of running could go wrong here: when something looks like a race, this is the first suspect.

## Where it stops now

At v0.12.0: **`_cellGcmInitBody`**, the start of the graphics library: the game has run its C and C++ start-up, loaded its system modules, initialised controllers and the USB library, started its force-feedback library (which printed its version: the first of the game's own words in the log), created its event queues, timers and first threads, and now wants the RSX. The next work is the window and the RSX on Vulkan.

## Checked

- The first run on the owner's PC (2026-11-21): the program loads, its entry point and start-up code run as lifted native code, and the process stops, by name, at the first system function not provided. With `sys_initialize_tls` provided it runs on to the next.
- v0.12.0, on the owner's PC: with 40 imports and 39 system calls provided, the game runs through its start-up to `_cellGcmInitBody`, printing its force-feedback library's version on the way.
- `ridgeracer7_machine` is compiled in CI on all six configurations; the executable itself needs the game and is built only on the owner's PC.

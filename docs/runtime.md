# The runtime

What the lifted game runs on: `runtime/` (what lifted code compiles against) and `ridgeracer7/src/` (the game's machine). Started at v0.11.0; this page says what exists and how it was checked.

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

Imports (`ridgeracer7/src/imports.cpp`), in the order the game reached them:

| Function | What it does here |
|---|---|
| `sys_initialize_tls` | gives the thread its thread-local storage: a copy of the image, zeroes after it, `r13` 0x7000 past its start (the game's code reaches a thread-local variable at `r13 - 0x7000 +` its offset: read off its 27 uses of `r13`) |

System calls (`ridgeracer7/src/syscalls.cpp`): none yet.

## Where it stops now

At v0.11.0: `sys_lwmutex_create`, called from the C library's start-up code. The next work is the thread and synchronisation layer (lightweight mutexes, threads, semaphores, event queues), as in WindWakerHD's order.

## Checked

- The first run on the owner's PC (2026-11-21): the program loads, its entry point and start-up code run as lifted native code, and the process stops, by name, at the first system function not provided. With `sys_initialize_tls` provided it runs on to the next.
- `ridgeracer7_machine` is compiled in CI on all six configurations; the executable itself needs the game and is built only on the owner's PC.

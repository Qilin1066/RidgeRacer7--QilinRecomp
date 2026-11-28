# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/). While the recompiler is being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game. Past entries are never rewritten.

## [Unreleased]

### Added
- The file system (`cellFs...`): the console's mount points as folders of the host's, with the hard disk in the port's own data folder (`docs/runtime.md`).
- The game data utility (`cellGameDataCheckCreate2`), and a `PARAM.SFO` writer beside the reader.
- The start of SPURS: the instance, event-queue attachment, tasksets.
- The system's event callbacks, memory containers (system calls 341, 342), the sound output's description (stereo linear PCM at 48 kHz), and the network and PlayStation Network libraries as a console with no cable and not signed in.

- The sound ports (`cellAudio`), mixed by the host's clock and played through `host/`'s sound output.
- The SPU lifter's first slice: everything but floating point (`docs/recompiler-core.md`). 1,719 of the 42 programs' 2,332 functions lift; `qilin lift --out` writes each program's code beside the PPU's. Its own differential test (`qilin_spu_difftest`: 3,552 cases, 227,328 runs against a separately written reference) is in `ctest`.
- The SPU lifter's floating point, from IBM's public instruction-set document: single precision in its extended range (truncation, no denormals, NaNs or infinities, saturation) done exactly in whole numbers; `fi` exactly; the estimates by tables of this runtime's own, a stated choice checked against the document's bound; double precision on the host's doubles. 2,330 of 2,332 SPU functions lift; the test is 4,080 cases and 362,496 runs.
- SPU threads (system calls 170 to 194, `sys_spu_image_import`): a thread runs its program's lifted code on a host thread, with memory transfers, the atomic line pair, mailboxes, events to the game's queues and the system's stop codes as hooks (`docs/runtime.md`).
- A decoder for the RSX's vertex and fragment programs (`recomp/src/rsx_program.*`), checked against each program's own Cg tables: all 242 of the game's agree. `qilin shaders --disasm` prints them (`docs/recompiler-core.md`).
- The RSX's programs translated to SPIR-V at build time, by a SPIR-V writer of the project's own (`recomp/src/rsx_spirv.*`, `spirv.*`): all 242. `qilin shaders --out` writes them; `ridgeracer7_shader_check` gives them to the local Vulkan driver, which takes all 242, and `tools/check/validate_shaders.py` to Khronos' `spirv-val`, which passes all 242 (`docs/recompiler-core.md`).
- The RSX's drawing on Vulkan, in a first design that keeps guest memory the only truth (`docs/runtime.md`): vertex program and constant uploads, draws of arrays, clears, flips into the window. The window opens on the main thread, the game runs on a thread of its own. The game's frames reach the window (black: it is loading).
- `RR7_SAVE_FRAME`: a flip's picture written to a file.
- The save data utility's automatic load and save (`cellSaveDataAutoLoad`, `AutoSave`): saves as folders on the port's hard disk with their `PARAM.SFO`.
- The message dialog (`cellMsgDialogOpen`, `Close`), shown in a message box over the window, the player's answer passed to the game.
- The renderer draws in batches: surfaces stay on the GPU between the points where the game can look, and go back to guest memory there.
- Textures (A8R8G8B8, the 16-bit formats, DXT1/3/5; swizzled or linear; mipmaps; the remap) and their samplers; the alpha test, in the translated fragment programs; points, lines, fans, quads and polygons.
- The game draws its first screen (the Dolby Digital logo) in the window.
- The depth test (Z24S8, as D32S8 where the GPU has it) and culling; the game reaches its title screen.
- The renderer's speed: host-cached staging, surfaces not uploaded again while guest memory is unchanged, only written surfaces copied back, a texture cache checked by hash. About 33 flips a second on the title screen, from 26 a minute.
- Films: PAMF, the demultiplexer and the MPEG-2 decoder (`film.cpp`), with FFmpeg's libavcodec built from the files two decoders and a parser need (`cmake/ffmpeg.cmake`). The game plays its title film.
- B8 textures; `cellFontGetFontIdCode`. The game reaches its main menu.
- Films' sound: `cellAdec` with FFmpeg's ATRAC3plus decoder. The game reaches Ridge State Grand Prix.
- Textures hashed once a batch, not at every draw.
- SPURS event flags in guest memory, in the SDK's layout as the game's tasks use it.
- The race's 3D world: the clear's rectangle starts covering everything, as the game assumes. `RR7_DRAWLOG` and `RR7_SCENE_TEST` for finding such faults.
- Memory: an ended SPURS task's record (with its 256 KiB local store) is dropped, and the film decoder holds at most four pictures. The process peaked at 10 GB; now 1.3 GB. The game reaches its first race (heads-up display drawn, 3D world black).
- The music: `cellAtrac` on FFmpeg's ATRAC3plus decoder. An event flag's queue is freed when it is detached (the game does so every frame).
- Tasks made by other tasks (the taskset's records); the RSX's inline transfers (NV3062, NV308A), indexed draws, primitive restart, cube maps, polygon offset; a renderer arena that grows. `RR7_QUIET` for test runs. The game reaches the loading of its first race.
- A SPURS kernel for the tasks: its data in their local store, its calls (end, wait to be signalled), the taskset's masks in guest memory; `SpuHooks::native_call` for the lifted code's calls outside the program. The game reaches Machine Select.
- SPURS tasks, run as SPU programs on host threads, and SPURS event flags; tasksets joined and shut down.
- `RR7_SECONDS`: a timed run that saves its last picture and reports where the renderer's time went.
- The controller: the window's gamepad or the keyboard as the first port's DualShock 3. `RR7_ANSWER` and `RR7_PRESS` for runs with nobody there.
- `RR7_RSX_TRACE` logs every RSX method and its argument.
- `RR7_ERRORS` and `RR7_FILES`: logs of the error codes handed to the game and of its file reads. `RR7_CALLS`, `RR7_DUMP` and `qilin lift --registers`, for finding a fault (`docs/runtime.md`).
- The clock library (`cellRtc`).
- The stencil test (both faces) in the renderer.
- `Play Ridge Racer 7.bat` and `Update Ridge Racer 7.bat`, as the other recomps have: build the game from the player's own copy (pulling the latest code first, for Update) and start it. The build compiles as many files at once as free memory allows.
- The FPSCR on the PPU (`mffs`, `mtfsf`) and the SPU (`fscrrd`, `fscrwr`), with their differential test cases: every function of the game lifts, 14,294 on the PPU and 2,332 on the SPU. A rounding mode other than to nearest, or an exception enabled, stops the game by name (`CpuHooks::fpscr_mode`, the SPU's halt).
- State the renderer doesn't take is refused by name instead of ignored: flat shading, logic operations, the depth bounds test, two-sided vertex colours, user clip planes, polygon stipple, polygons drawn as points or lines, a window offset.
- The control-flow pass proves 293 of the 296 switches' sizes from the bound the code checks (264 before): the compare is found anywhere above its branch, stopping at anything else that writes the same CR field, and a switch's block may follow another's table or be entered by `bgt` then `b`. Every newly proven size equals the size found before; unit tests hold each rule.
- The depth bounds test, as Vulkan's (the stored depth against the bounds, which are part of the pipeline); a GPU without it stops the game by name. The game uses it on its way to a race.
- `RR7_SAVE_FRAME` takes a list of flips; `RR7_DRAWLOG` gives each draw's viewport, clip and scissor.
- The settings menu, the other Qilin recomps' (Dear ImGui, fetched and pinned at WindWakerHD's commit, v1.93.0): F1 or Esc (a controller: Back and Start) opens it over the game, which pauses; Close and Quit top right; closing saves `ridgeracer7.ini` in the data folder. Controls (press-to-bind each DualShock 3 button and stick direction to keys, a pad's buttons or the mouse), Display (full screen, the display, whole multiples, fit or fill, smooth or sharp, the CRT look, vsync, the refresh's matching, the graphics device, frame times on F3 with the display's refresh), Game (the console's language, English or Japanese, which button confirms, its nickname, the user's name and the time zone, which `cellSysutilGetSystemParamInt` and `String` now give the game). The enhancements the renderer can't do yet are shown greyed out (`docs/runtime.md`).
- The pause under the menu (`pause.*`): the RSX's thread at a flip, the vertical blank's before its handler and the sound mixer's are held, each where it holds no lock and runs none of the game's code; the vertical blank goes on from the next, not catching up.
- `ridgeracer7_settings_test` (in `ctest`): the settings file read and written, bad lines refused by number, the controller built from the bindings (the triggers' pressure, the sticks' keys), the pause holding a thread and letting it go.
- `host/`: `Display::pause_frame_times` and `refresh_hz`, from WindWakerHD's.
- The scheduler (`kernel_sched.*`, `docs/runtime.md`): guest PPU threads run guest code only while they hold one of two core tokens, handed out by priority, the longest ready first among equals; a thread gives its token up whenever it waits (the system's objects, sleeps, yields, joins, file reads and writes, joins of SPU and SPURS work, memory pools, the films' decoder threads, the RSX's command-buffer wait) and a thread of better priority takes one at the holder's next call into the system. A thread kept waiting while the holders run on without calling the system runs on a lent token (at once if it outranks one of them, as the console would have preempted it; after 1 ms if it is as good as the worst, 50 ms if every holder is better), logged. Taking and giving back a token with nobody waiting is one compare-and-swap. `RR7_FREE_THREADS=1` turns the tokens off. Its game-free tests are `ridgeracer7_sched_test` in `ctest` (21 cases).
- C++ exceptions in the lifted code: the guest's unwinder can enter a landing pad (`docs/recompiler-core.md`, "C++ exceptions"). Its `__builtin_eh_return` epilogue (`add r1,r1,r10; blr`, found four times in the game and confirmed by shape: any other stack adjustment by a register is refused by name) throws `GuestUnwind`; the 932 functions with landing pads are lifted twice, from their start and from their pads, and run under the runtime's one `try` (`run_with_landing_pads`), which takes an exception for one of their pads in their own frame. A `GuestUnwind` nothing takes stops the game by name. The game's lifted code must be lifted again. The differential test runs a program that throws through the epilogue to a frame two calls up, against the reference.
- The control-flow pass finds the functions that never return (47 in the game: those whose every way ends in a call of one that doesn't, `sys_process_exit` and `sys_ppu_thread_exit` to begin with, a trap or a loop), and with them proves the last 3 switches' sizes: all 296.
- `ridgeracer7_render_test` (game-free, in `ctest`): each texture format's channel and byte order, the remap's slots and selectors, the swizzled layout, a colour surface's pixels as the renderer's image holds them (the same as a texture of those bytes, which the copy of a surface to a texture on the GPU relies on), vertex attributes and depth words, held to their definitions. Read against them, no texture format, the remap, the surfaces or the fragment programs' colour output had their channels exchanged: the colours seen exchanged in the first race were the fog's (the ADD fix below). The renderer's conversions between guest memory and the GPU are in `render_formats.*`.
- Polygons drawn as lines or points (Vulkan's polygon mode, with `fillModeNonSolid`, which the host layer now turns on where the GPU has it), with polygon offset by the mode's own enable; primitive restart for points and lines. Primitives are assembled by a tested pure function (`render_state.cpp`); a draw whose pieces were all too short for a triangle no longer hands the GPU restart indices.
- The window offset (`SET_WINDOW_OFFSET`): the window's origin on the surface, moving the viewport and the scissor alike (*a stated reading*), instead of a stop.
- Logic operations, as Vulkan's (in place of blending), where the GPU has `logicOp`; instead of a stop.
- Flat shading: a fragment program that reads a colour has a flat form, translated at build time beside the plain one (`qilin shaders` now writes a program's other forms for draw state; `validate_shaders.py` and `ridgeracer7_shader_check` take them too: 28, all passing), and primitives are made into lists with the RSX's provoking vertex first (its last, OpenGL's convention, or its first by `SET_FLAT_SHADE_OP`), their windings kept. `ridgeracer7_shader_forms_test` holds what each form declares.
- Two-sided colours: a fragment program's two-sided form reads the vertex program's back colours on back faces, made where some vertex program writes one (none of this game's does: a draw that needs one stops by name, as before but for the reason).
- User clip planes: a vertex program that writes a plane's distance (`FOGC` and `PSZ`'s y, z, w, a stated reading) has a clip form writing `ClipDistance`, each plane's test (off, LT, GE) a specialisation constant, used where the GPU has `shaderClipDistance` (the host layer turns it on where it is). None of this game's vertex programs writes one, so it has none and a draw with a plane on stops by name, as before but for the reason. Polygon stipple stays refused: Vulkan has none.
- Line stipple (`SET_LINE_STIPPLE`) is refused by name where lines are drawn (Vulkan 1.3 has none), instead of being ignored.
- Per draw, less work for the CPU and the driver: the constants are one descriptor set a batch with dynamic offsets, not a set allocated and written for every draw; a draw with the textures of the set bound reuses it; the pipeline, viewport, scissor, push constants and constant offsets are set only when they change; vertex attributes are bound in one call; a sampler is found without a vector allocated for its key, and a flush's reason without a string made. The RSX's thread finds a state method by a compile-time bitmap, not a scan of the table, for every command word.
- `renderer.cpp` split: the shader modules, pipelines and samplers (`renderer_pipelines.*`, a `PipelineCache`), the pure state (`render_state.*`) and conversions (`render_formats.*`) are files of their own; `renderer.cpp` keeps the batches, surfaces, write-back and presentation (1,833 lines to 1,584).

### Changed
- The sound mixer is paced by the sound card and waits for the game: its blocks follow the card's crystal on an absolute schedule trimmed by the card's queue (no resampling), it sleeps with a high-resolution timer at the host's highest priority (Windows' "Pro Audio" class), and a block the game hasn't written yet (a taken block is marked with a pattern, not zeroes) is waited for, up to 50 ms, instead of played empty. The game writes each block at the read index as it is when its sound core answers, so a late answer used to cost a gap and a lost block. Pausing fades the sound out and back in. Once a second the mixer logs late wake-ups, waits for the game, blocks missed, the card running dry and clipping when any happen (each second with `RR7_AUDIO_VERBOSE=1`), and their totals at the end of the run (`docs/runtime.md`, "The sound's timing").
- The controller's layout is the settings' (`[controls]`), its defaults the keyboard layout `main.cpp` had; L2 and R2 bound to a pad's triggers keep their pressure.
- `host/`: `present_again` no longer stops the frame times (as WindWakerHD's): showing the last picture again between the game's no longer drops the next frame time.
- README rewritten to the layout shared by the QilinRecomp projects: what works and how to build and play it, without the development history.
- SPURS tasks start for less: each SPU program's ELF is parsed once (checked against its headers at every use), local stores are kept zeroed between threads, a task's host thread is kept for the next task, the kernel's three calls are plain functions instead of `std::function`s, and a memory transfer no longer takes a lock to look through the (empty) list of forbidden ranges. Starting a task costs under 3 microseconds of its starter's time, from 45 to 80; a start and an end about 70, from 150 to 190 (`ridgeracer7_spu_test bench`). `ridgeracer7_spu_test` (in `ctest`) runs made-up SPU programs as tasks and as a thread group with no game.
- A film's pictures are converted to ARGB a row at a time, each pixel stored as one word: 3.8 ms for a 1280 x 720 picture with MSVC, from 8.2 (Clang's code was already as fast); the same bytes as before on 20,000 random pictures and every Y, U and V for both colour matrices. The demultiplexer keeps its list of streams from packet to packet instead of making one for each; the music decoder keeps its packet, frame and frame buffer (and frees its FFmpeg objects by RAII); the sound mixer reads each block from guest memory directly and keeps its list of queues to tell.
- A SPURS task waiting to be signalled is woken only by an atomic put to a taskset's line (only such a put can signal it), not by every SPU program's every atomic put. A thread group started again keeps its local store where it was (zeroed), as before the kept local stores, so `sys_spu_thread_read_ls` and `write_ls` never meet one given back.
- The SPU's `shufb` and its whole-register byte shifts and rotates (`rotqby`, `shlqby`, `rotqmby` and their forms), among the commonest instructions in the game's SPU code: `shufb` builds each word from its picks straight from the source words (about 35 ns to 20 a call, in a dependent chain, with MSVC and Clang), the byte shifts work on the register as two 64-bit halves (`rotqby` 35 ns to 8 with MSVC, 28 to 2 with Clang). `fsmbi` takes each word from a table of sixteen. The same results as before on 21 million random and exhaustive cases (every count, every control byte in every place, every `fsmbi` mask), and the SPU differential test passes.
- Mutexes, condition variables, lightweight mutexes, semaphores and event queues wake their waiters in the order their attribute asks for (`SYS_SYNC_FIFO`, or `SYS_SYNC_PRIORITY` and `_PRIORITY_INHERIT` by priority, the longest waiting first among equals), and hand what is let go (ownership, a count, an event) straight to the one woken. A protocol the runtime doesn't know stops the game by name.
- The RSX's side of a frame costs less CPU, measured without the game by `ridgeracer7_rsx_bench` (a synthetic frame of 2,000 draws through the command processing, the draws' gathering and the renderer with every Vulkan call stubbed; best frame, MSVC): 10.6 ms a frame to 6.5 with surfaces written back at every flush, 5.4 to 4.3 written back when touched, and the GPU given exactly the same (a digest of every draw's state, vertices, indices, constants and textures, and of the surfaces written back). A batch's texture sets are kept by what they hold and bound again, so such a frame no longer runs out of its 1,024 sets and flushes twice; commands' runs of state methods, vertex program words and constants are stored in one byte-swapping pass (276 to 206 us a frame); a draw's vertex program is made into bytes only when it changes and its fragment program copied at once (gathering 880 to 650 us); the pipeline of a draw like the one before is found without a look in the table; surfaces are hashed as they are copied, a surface a batch only read keeps the hash it was read with, and the hash runs eight lanes (12 to 21 GB/s); a depth surface's rows go back to guest memory in 1.16 ms from 1.83 (1280 by 720); put is read again only when get has caught up with it.
- The RSX's state table, vertex programs and constants and the gathering of a draw are `rsx_state.hpp`/`.cpp`, which need no game; `rsx.cpp` keeps the thread and the methods that act.

### Checked
- `ridgeracer7_audio_test`: the mixer's pacing simulated between a fake sound card and a fake game that answers as Ridge Racer 7's sound thread does, jittery and sometimes late: no block lost or missed in 3,751, the card never dry, its queue on its target, with drift of 1000 parts in a million either way, a large card request, stalls of 40 and 300 ms; the old mixer loses a tenth of the same game's blocks with a 1 ms timer, and most of them with the system's 15.6 ms one.
- On the owner's PC: the game starts its sound core (`bnusCoreSpu.elf`) as an SPU thread, which runs; it reads and unpacks its first packs of `RR7.DAT`, and runs on to its first drawing: it stops at the RSX method that loads a vertex program. 110 of its 248 imports and 54 of its 78 system calls are provided.

### Fixed
- FFmpeg's decoders were handed films' access units and the music's frames with nothing after their last byte, though they may read up to 64 bytes past a packet's end: each now has those bytes, zeroed.
- A film's sound stream reset (`cellDmuxResetEs`) while the demultiplexer's thread waited for room in the stream's ring left that thread reading past the end of what it had pending; it now drops it, as the reset meant.
- Building the game no longer needs 10 GB for one file: the SPU helpers that loop over sixteen bytes are left to MSVC's own inlining (`QILIN_SPU_BULKY`), which brings the heaviest file (`spu_p24`) to 4 GB and two minutes from six and a half. The build scripts count 5 GB per compiler.
- The renderer no longer writes every surface back each time the RSX briefly runs out of commands mid-frame: it waits 2 ms first (labels, waits and flips still flush at once).
- `sys_memory_allocate` could give the same address twice when blocks of both page sizes were allocated: the game's packs then overwrote one another, and it faulted going over one's table.
- The archive's directory: a record begins with its name, not ends with it. Every pack and file had its neighbour's name since v0.5.0 (`docs/data-files.md`).
- Trees, fence posts and banner frames were the fog's blue: a vertex program's ADD reads its second source from slot 2, and the translation and `qilin shaders --disasm` took slot 1 (unused, read as the position). `RR7_DRAWLOG` gives a draw's programs and vertex constants.
- Per draw, less work: the programs are found by comparing with the last eight drawn (a fragment program's constants left out) before any hashing; the vertex program's bytes are built a word at a time; a pipeline's key is a fixed array in a hash table, not a vector in an ordered map. A vertex attribute's array is converted in one call, its type chosen once (a template per type), a normal half built from its bits (the same floats: every half and 20,000 random arrays checked). A surface written back is hashed row by row as each row is written, while it is in the cache, not in a second pass over guest memory (rows only: the pitch's padding past them isn't part of the picture). One `Draw` is reused for every draw (`Draw::reset`), so its index list and programs no longer allocate each time. The draw's index list is no longer copied twice on the way to the GPU (spans over the game's list, or over a kept 0, 1, 2... list), and its other per-draw lists are fixed arrays (at most 16 each) or kept between draws. The texture cache is a hash table, as the pipelines are. The 468 vertex constants (7.5 KB) are copied into the batch's arena only when the game has changed them since the batch's last copy, not for every draw: less copying, and fewer batches ended for a full arena. Textures decode faster (the same texels, checked against the old decoder on 40,000 random textures): a swizzled texel's place from per-column and per-row tables, the remap as a table, and 32-bit texels kept as stored turned a word at a time (4.7 to 11.8 times as fast for large ones).
- Surfaces are written back to guest memory when something touches them, not at every flush. After a flush a drawn surface's guest pages are made unreadable; the first access to one (the game's code, an SPU, the RSX's own reads, a flip showing it) faults, and the fault writes that surface back from its staging buffer and lets the access go on. File reads and writes into such pages, which the host's system can't fault for, write them back first. A surface still waiting can't have changed in guest memory, so it is no longer hashed when the next batch draws to it. `RR7_EAGER_WRITEBACK=1` brings back writing everything at every flush (`docs/runtime.md`).
- Clears are done on the GPU, in the renderer's batch, not on guest memory after a flush (which wrote every surface back first): a clear of a whole surface doesn't read it in at all. And a surface waiting to be written back is copied off the GPU only when it is touched, not at every flush: the copies back were most of the GPU's time.
- A flip shows its picture from the GPU: the drawn surface is copied, on the GPU, to a picture of the renderer's own that the window shows (`renderer_flip`), not read back, converted on the CPU and uploaded again. Only a display buffer the CPU wrote goes the old way.
- Flushes no longer wait for the GPU: up to three batches are in flight, each with its own command buffer, arena, descriptor pool and timestamps, waited for only when their turn comes round again. Whatever touches a surface still drawing copies it out by a submission the queue runs after the batch.
- A texture that is a colour surface the renderer holds (2D, linear A8R8G8B8 with its channels as stored, at the surface's address with its size and pitch, drawn in the batch or not yet written back) is copied from the surface's image on the GPU: no flush, no write-back, no hash, no decode. A race's bloom and reflection passes read such pictures some 300,000 times in 20 minutes.
- The gamepad's triggers are L2 and R2 with their pressure (0 to 255 as pulled), not on or off at half way: a race's throttle and brake can be held part way.
- The RSX's inline transfers no longer flush the renderer's batch unless they write over a surface drawn in it: they were nearly all of the fly-by's flushes, at about a flip a second.
- SPURS: two SPUs' atomic puts to a taskset at once could both start the same task, whose second copy, ending, cleared the bits of a task made again with its number ("task 2 marked to run, and not enabled"). A task is now taken under the lock, and a task's end changes its bits and this runtime's record of it together.
- Surfaces are converted to and from guest memory a word at a time, not a byte (the same bytes, checked on every case against the old code): writing back took most of a race's time. A timed run's report counts flushes by what asked for them.
- Streamed music (`cellAtrac`) stopped the game in its first race: frames start after the file's header, so once the ring buffer had gone round, a frame ran over its end, and was read past it. Such a frame is now read in two parts. `RR7_DRAWLOG` gives a draw's fragment constants.
- The build scripts count 8 GB per compiler, not 5: four compilers at once ran a PC with 22 GB free critically low, though one measured alone peaks at 4 GB.

- A surface written back no longer puts back pixels nobody drew. Each surface keeps the rectangle drawn or cleared since it was last written back (draws' scissors, clears' rectangles), and only that goes back to guest memory and has its pages made unreadable. The rest of the image is a copy of guest memory from when a batch began; written back, it put old bytes over whatever the game had written there since. A surface declared larger than what is drawn on it reached over the game's command buffers, and the RSX then met zeroes: the rare stop at a jump to offset `0x1b00` (seen at about flip 4500, and again after flip 10,800).
- The PPU's `sync`, `lwsync`, `eieio` and `isync` are lifted as the host's fences, not as nothing (the compiler could move the game's stores past them, and an ARM host's memory reorders them); the RSX's thread fences after reading `put`.

- Flips wait for the vertical blank as the game's flip mode asks (2: at the next one). One flip may wait to be shown: the RSX goes past a flip and waits at the next until the last has been shown, so it is never more than a frame ahead of the display. Flips shown at once let the RSX's thread, once the renderer was fast, run ahead of the game: it reached the jump the game ends each frame's commands with (to `0x1b00`, in the default command buffer) before the game had written what follows there, and ran on through zeroes. That was the rare stop at a jump to `0x1b00`: a write watchpoint (`RR7_WATCH_WRITES`) showed that word is never wiped, only not yet written.
- A thread's stack is whole 64 KiB pages, however little the game asks for: one of its threads asks for 16 KiB and uses some 50 KB formatting a log line.
- An attribute a vertex program reads that the draw gives no array reads 0, 0, 0, 1 (its starting value: the game sends none of the methods that set one), instead of stopping.
- One image for a piece of memory drawn as surfaces of several sizes (by address, format and pitch): the game draws 256 by 128 passes in the corner of 1920 by 1080 buffers, which as separate surfaces flushed the batch, wrote one back and read the other in at each change (39,000 times in a 20-minute run).

- SPU single precision on the host's float unit (x86-64): each SPU thread sets its float unit to round toward zero with denormals zero in and out, under which an add, subtract or multiply gives the SPU's result bit for bit, and a fused multiply-add too, done in double precision; zeros are made +0 and lanes the host can't stand in for (an operand's exponent 255, a result's 254 or more) are done the exact way. 600 million lanes of the six operations, weighted to the hard cases, agree bit for bit with the exact way; an operation takes about a quarter of the time. ARM hosts, and the differential test, keep the exact way.
- A guest thread's record of where it is (`RR7_WATCH`, `RR7_PROFILE`, a fault's report) is reused once its thread has ended, not kept for ever: the game starts SPURS tasks, each a guest thread, every frame.
- `RR7_PROFILE`: every millisecond, where each guest thread is (which system call, which of the game's functions); a timed run logs each thread's commonest places, and the SPURS tasks' starts and running times by program. It showed the frame rate is set by the main thread waiting for the vertical blank and for the SPURS effects tasks.

### Known
- The heaviest lifted file (`spu_p24`) still takes MSVC 4 GB and about two minutes: its longest function is 2,500 lines.

## [0.13.1] - 2026-11-25

### Fixed
- Detaching a running PPU thread no longer detaches its host thread a second time at exit. Detached threads retire their bookkeeping; detaching a thread already claimed by a join is rejected.
- File reads keep their descriptor locked until the read completes, so another guest thread cannot close the host file underneath them.
- File creation and truncation check descriptor availability first. A full handle table leaves existing files intact and does not create new files.

### Tests
- Added game-free tests of the real file/thread syscalls: descriptor exhaustion, a controlled read/close race, detached-thread exit and a normal join. The first three fail against the previous code; they pass with the fixes.

## [0.13.0] - 2026-11-21

The game runs to the start of SPURS; the window layer is in.

### Added
- `host/`: the other Qilin recomps' host layer with this game's names: window and events on SDL3, Vulkan 1.3 through volk, presentation, frame times, with its tests. SDL3, the Vulkan headers and volk are fetched and pinned, as there. The game doesn't draw into it yet.
- The RSX's set-up (`cellGcmSys`) and the RSX itself: a thread that reads the commands the game writes, keeps the state they set, writes the labels, and stops at any method it doesn't know (`docs/runtime.md`). A vertical-blank thread calls the game's handler at 59.94 Hz.
- The video output (`cellVideoOut`): one display, taking 1080p, 720p and 480; the game configures 1920 by 1080.
- The font library (`cellFont`) on FreeType 2.13.3, fetched and pinned. The console's own fonts are the player's to supply from a PS3's system software (`--flash`, or an RPCS3 folder beside the game's): by the owner's decision of 2026-11-21, as is FreeType.
- `vrefp` and `vrsqrtefp`, as the correctly rounded values: a stated choice within the architecture's bound, checked against a whole-number reference (`docs/recompiler-core.md`). 14,292 of 14,294 functions now lift.
- A bad memory access is reported with the guest address, the thread and the lifted functions on the stack, where before the process ended silently. `qilin lift --pc` makes the report name the instruction.
- Development aids: `RR7_WATCH` (where each guest thread is) and `RR7_RSX_SURVEY` (the RSX methods not yet provided).

### Fixed
- A semaphore's post goes to the thread that has waited longest. The poster could take its own post back, which left the game's text thread drawing before its surface existed.
- `report.cpp` includes what it uses (macOS's build of v0.11.0 and v0.12.0 failed).

### Checked
- On the owner's PC: the game runs to `cellSpursInitialize`; the host layer's tests pass (Vulkan device, offscreen frame, virtual controller); the differential test agrees over 5,450 cases and 1,346,784 runs, and fails when either estimate is given a wrong last bit.

## [0.12.0] - 2026-11-21

The game runs to graphics initialisation.

### Added
- The first layer of the system, each function added when the game reached it (`docs/runtime.md`): thread-local storage; lightweight mutexes, mutexes, condition variables and semaphores; threads (create, exit, join, detach, priorities); event queues and ports; timers; memory in pages; memory pools; the clock and sleeping; the console's terminal; the console's settings and the disc's description; controllers and USB as a console with nothing plugged in. 40 of the 248 imports, 39 of the 78 system calls.
- What the game prints to the console's terminal appears in the log.

### Checked
- On the owner's PC: the game runs its start-up, loads its system modules, starts its force-feedback library (which prints its version), creates its event queues, timers and threads, and stops at `_cellGcmInitBody`: it wants the RSX.

### Not as designed yet
- The scheduler: guest threads run freely as host threads, and priorities decide nothing.

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


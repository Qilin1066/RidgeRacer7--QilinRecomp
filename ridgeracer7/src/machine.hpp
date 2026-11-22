// The machine the lifted game runs on: its memory, its first thread, and
// the boundary with the system.
//
// A PS3 game reaches the system in exactly two ways: imported functions
// (through its import table: 248 of them, each with a name) and system
// calls (the `sc` instruction, with a number). Both are replaced natively
// here, by name and by number, and **anything not provided stops the game
// with a message naming it and where it was called from**. Nothing is
// answered with a guess.
//
// Runtime code: no std::format.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "elf.hpp"
#include "qilin/rt/cpu.hpp"
#include "qilin/rt/lifted.hpp"

namespace rr7 {

using qilin::rt::Cpu;

// A system function the runtime provides, by the SDK's name for it. It
// reads its arguments from the registers (r3 on) and leaves its result in r3.
struct NativeImport {
    const char* library;
    const char* name;
    void (*handler)(Cpu&);
};
// The runtime's function for an import, or null (imports.cpp, which gathers each file's table).
void (*find_native_import(std::string_view library, std::string_view name))(Cpu&);
std::span<const NativeImport> sync_imports();     // sync.cpp
std::span<const NativeImport> sysutil_imports();  // sysutil.cpp
std::span<const NativeImport> pad_imports();      // pad.cpp
std::span<const NativeImport> usbd_imports();     // usbd.cpp
std::span<const NativeImport> mempool_imports();  // mempool.cpp
std::span<const NativeImport> gcm_imports();      // gcm.cpp
std::span<const NativeImport> savedata_imports();  // savedata.cpp
std::span<const NativeImport> film_imports();  // film.cpp
std::span<const NativeImport> rtc_imports();  // rtc.cpp
std::span<const NativeImport> spu_imports();      // spu.cpp
std::span<const NativeImport> audio_imports();  // audio.cpp
std::span<const NativeImport> np_imports();  // np.cpp
std::span<const NativeImport> net_imports();  // net.cpp
std::span<const NativeImport> fs_imports();  // fs.cpp
std::span<const NativeImport> gamedata_imports();  // gamedata.cpp
std::span<const NativeImport> spurs_imports();  // spurs.cpp
std::span<const NativeImport> font_imports();  // font.cpp
std::span<const NativeImport> video_imports();  // video.cpp

// A system call the runtime provides, by number.
struct NativeSystemCall {
    uint32_t number;
    void (*handler)(Cpu&);
};
// The runtime's function for a system call, or null (syscalls.cpp, which gathers each file's table).
void (*find_native_system_call(uint32_t number))(Cpu&);
std::span<const NativeSystemCall> memory_system_calls();  // memory.cpp
std::span<const NativeSystemCall> thread_system_calls();  // threads.cpp
std::span<const NativeSystemCall> sync_system_calls();    // sync_calls.cpp
std::span<const NativeSystemCall> tty_system_calls();     // tty.cpp
std::span<const NativeSystemCall> event_system_calls();   // events.cpp
std::span<const NativeSystemCall> timer_system_calls();   // timers.cpp
std::span<const NativeSystemCall> spu_system_calls();  // spu.cpp
// Makes a thread known to the system (threads.cpp).
void add_thread(uint32_t id, int32_t priority);
std::span<const NativeImport> thread_imports();  // threads.cpp

// Where things are in the guest's address space, beside the program itself.
inline constexpr uint32_t kImportCode = 0x0E000000u;         // a code address for each import (nothing is there: a call to one is caught)
inline constexpr uint32_t kImportDescriptors = 0x0E100000u;  // the descriptors the import slots point at
inline constexpr uint32_t kStackTop = 0xD0100000u;           // the first thread's stack ends here
inline constexpr uint32_t kNativeCode = 0x0E080000u;         // likewise for the runtime's own functions the game calls back (native_function)
inline constexpr uint32_t kReturnToHost = 0x0E0FFFFCu;       // the first thread's LR: where "returning from the entry point" goes
inline constexpr uint32_t kFirstThreadId = 0x01000000u;      // *a guess at the shape of a thread id; nothing has compared it yet*
inline constexpr uint32_t kRuntimeArea = 0x0E200000u, kRuntimeAreaEnd = 0x0F000000u;  // the runtime's own small allocations in guest memory

// What the runtime knows of the player's copy, beside its program.
struct GameInfo {
    std::string title_id;          // from PARAM.SFO: "BLUS30001"
    uint32_t parental_level = 0;   // likewise
    std::filesystem::path usrdir;  // the game's own files
    std::filesystem::path flash;   // a PS3's system software (dev_flash), for the console's fonts; empty if there is none
    std::filesystem::path hdd;     // the console's hard disk (dev_hdd0): a folder of the port's own
};
const GameInfo& game();

// Loads the program into a new address space, sets up the first thread and
// runs the entry point. Returns the process's exit code. `functions` are the
// lifted functions, in address order.
int run(const qilin::Elf& elf, std::span<const qilin::rt::LiftedFunction> functions, std::span<const qilin::rt::LiftedSpuProgram> spu_programs, GameInfo info);
// The game's SPU programs, lifted (spu.cpp runs them).
std::span<const qilin::rt::LiftedSpuProgram> spu_programs();

// From now on a bad memory access is reported, with the guest's address and call stack, before the process stops (fault.cpp).
void report_faults(std::span<const qilin::rt::LiftedFunction> functions);

// The guest's memory, for native code: plain host memory at base + address.
uint8_t* guest_memory();
// Backs guest memory with zeroed pages; stops the game if the host refuses.
void commit(uint32_t address, uint32_t size);
// The hooks every guest thread's Cpu is given.
const qilin::rt::CpuHooks& guest_hooks();
// Calls the lifted function at a code address; stops the game if there is none.
void call_guest(Cpu& c, uint32_t code);
// Makes a function of the runtime's callable by the game: the address of a descriptor for it, which the
// game may be handed as a callback. (At most kNativeFunctions of them, for the life of the process.)
inline constexpr uint32_t kNativeFunctions = 64;
uint32_t native_function(void (*handler)(Cpu&));
// For a host thread of the runtime's that must call the game's functions (a handler the game has
// registered): makes it a guest thread, with an id, a stack and thread-local storage of its own (threads.cpp).
void become_guest_thread(Cpu& c, uint32_t stack_size, int32_t priority);
// Calls the game's function behind a descriptor, with the arguments already in the registers.
void call_descriptor(Cpu& c, uint32_t descriptor);
// Tells the game of a system event at its next cellSysutilCheckCallback (sysutil.cpp): 0x0101 is "the player asks to quit".
void system_event(uint64_t status, uint64_t parameter);
// Where the game's sound goes (audio.cpp): called from the mixer's thread with 256 left-right pairs of 16-bit
// samples at 48 kHz, 187.5 times a second. Set before run().
void set_audio_sink(std::function<void(const int16_t* left_right, size_t frames)> sink);
// The console's time zone and summer time, in minutes (sysutil.cpp's settings).
int32_t console_time_zone_minutes();
int32_t console_summer_time_minutes();
// The system's message dialog, for the window's thread to show (sysutil.cpp): a dialog that asks the player something, if one
// is waiting to be shown (its text, and its buttons: 1 yes and no, 2 OK); and the player's answer (1 yes or OK, 2 no, 3 escaped).
bool take_dialog(std::string& text, uint32_t& buttons);
void answer_dialog(int32_t button);
// The player's controller as the console's first (pad.cpp): the buttons as the console has them (digital1: select 1, L3 2,
// R3 4, start 8, up 0x10, right 0x20, down 0x40, left 0x80; digital2: L2 1, R2 2, L1 4, R1 8, triangle 0x10, circle 0x20,
// cross 0x40, square 0x80), the sticks (0 to 255, 128 the middle, y down) and the pressures (right, left, up, down, triangle,
// circle, cross, square, L1, R1, L2, R2: 0 to 255). Set by the window's thread at each poll.
struct PadState {
    uint16_t digital1 = 0, digital2 = 0;
    uint16_t right_x = 128, right_y = 128, left_x = 128, left_y = 128;
    uint16_t pressure[12] = {};
};
void set_pad_state(const PadState& state);
// SPURS's tasks (spurs.cpp), run as SPU programs on host threads (spu.cpp): the program's ELF in guest memory, $3 and $4 as
// the task is given them, the SPURS instance's ports for the events it sends, and what to call when it exits (with its status).
void start_spu_task(const std::string& name, uint32_t image_address, const std::array<uint32_t, 4>& r3, const std::array<uint32_t, 4>& r4,
                    const std::map<uint32_t, uint32_t>& ports, std::function<void(uint32_t status)> exited);
// Guest memory no SPU transfer may touch (an object this runtime keeps natively, whose layout an SPU program would assume):
// one that does stops the game, naming it.
void forbid_spu_transfers(uint32_t address, uint32_t size, const char* what);
// The program's thread-local image: each thread gets a copy.
struct TlsImage {
    uint32_t address = 0, file_size = 0, memory_size = 0;
};
TlsImage tls_image();
// Microseconds since the machine started: the system's own clock.
uint64_t system_time();
// Ends the process with this code, at once, from any thread (it doesn't return).
[[noreturn]] void exit_process(int code);
// Zeroed guest memory for the runtime's own use (thread-local blocks, the arguments): never freed.
uint32_t allocate(uint32_t size, uint32_t alignment);

}  // namespace rr7

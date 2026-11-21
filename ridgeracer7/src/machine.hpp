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

#include <cstdint>
#include <filesystem>
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
};
const GameInfo& game();

// Loads the program into a new address space, sets up the first thread and
// runs the entry point. Returns the process's exit code. `functions` are the
// lifted functions, in address order.
int run(const qilin::Elf& elf, std::span<const qilin::rt::LiftedFunction> functions, GameInfo info);

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

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
#include <span>

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
std::span<const NativeImport> native_imports();  // imports.cpp

// A system call the runtime provides, by number.
struct NativeSystemCall {
    uint32_t number;
    void (*handler)(Cpu&);
};
std::span<const NativeSystemCall> native_system_calls();  // syscalls.cpp

// Where things are in the guest's address space, beside the program itself.
inline constexpr uint32_t kImportCode = 0x0E000000u;         // a code address for each import (nothing is there: a call to one is caught)
inline constexpr uint32_t kImportDescriptors = 0x0E100000u;  // the descriptors the import slots point at
inline constexpr uint32_t kStackTop = 0xD0100000u;           // the first thread's stack ends here
inline constexpr uint32_t kReturnToHost = 0x0E0FFFFCu;       // the first thread's LR: where "returning from the entry point" goes
inline constexpr uint32_t kRuntimeArea = 0x0E200000u, kRuntimeAreaEnd = 0x0F000000u;  // the runtime's own small allocations in guest memory

// Loads the program into a new address space, sets up the first thread and
// runs the entry point. Returns the process's exit code. `functions` are the
// lifted functions, in address order.
int run(const qilin::Elf& elf, std::span<const qilin::rt::LiftedFunction> functions);

// The guest's memory, for native code: plain host memory at base + address.
uint8_t* guest_memory();
// Backs guest memory with zeroed pages; stops the game if the host refuses.
void commit(uint32_t address, uint32_t size);
// Zeroed guest memory for the runtime's own use (thread-local blocks, the arguments): never freed.
uint32_t allocate(uint32_t size, uint32_t alignment);

}  // namespace rr7

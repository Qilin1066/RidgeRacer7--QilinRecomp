// What the generated code gives the runtime: every function of the game's
// program by its address, and which program they were lifted from.
//
// `qilin lift <game> --out <folder>` writes the definitions (table.cpp) from
// the player's own copy; they are never part of this repository.
#pragma once

#include <cstddef>
#include <cstdint>

#include "qilin/rt/cpu.hpp"
#include "qilin/rt/spu.hpp"

namespace qilin::rt {

struct LiftedFunction {
    uint32_t address;
    GuestFn fn;
};

extern const LiftedFunction kLiftedFunctions[];  // in address order
extern const size_t kLiftedFunctionCount;
extern const char kLiftedFrom[];  // the SHA-256 of the program they were lifted from

// The game's SPU programs, each lifted as a whole: where its ELF file lies in the PPU program's memory
// (how the game names it to the system), the file's length, its entry point, and the function that runs
// the lifted function at an address in the SPU's memory (it halts the SPU if there is none there).
struct LiftedSpuProgram {
    uint32_t address, size, entry;
    void (*call)(Spu&, uint32_t target, uint32_t pc);
};

extern const LiftedSpuProgram kLiftedSpuPrograms[];
extern const size_t kLiftedSpuProgramCount;

}  // namespace qilin::rt

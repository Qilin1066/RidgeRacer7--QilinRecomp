// What the generated code gives the runtime: every function of the game's
// program by its address, and which program they were lifted from.
//
// `qilin lift <game> --out <folder>` writes the definitions (table.cpp) from
// the player's own copy; they are never part of this repository.
#pragma once

#include <cstddef>
#include <cstdint>

#include "qilin/rt/cpu.hpp"

namespace qilin::rt {

struct LiftedFunction {
    uint32_t address;
    GuestFn fn;
};

extern const LiftedFunction kLiftedFunctions[];  // in address order
extern const size_t kLiftedFunctionCount;
extern const char kLiftedFrom[];  // the SHA-256 of the program they were lifted from

}  // namespace qilin::rt

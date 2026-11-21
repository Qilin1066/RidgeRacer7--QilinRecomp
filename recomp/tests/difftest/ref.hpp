// A reference interpreter for the lifter's slice, written separately from the
// lifter: from the architecture's definitions, on the raw instruction word's
// fields, with CR and XER held as the hardware holds them (packed words),
// carries taken bit by bit, masks built bit by bit, products by long
// multiplication, and memory read and written byte by byte, big-endian. It
// shares only the decoder's identification of the opcode with the lifter.
//
// It runs in tests only: nothing interprets at run time.
#pragma once

#include <cstdint>

#include "ppu.hpp"

namespace qilin::ref {

struct State {
    uint64_t gpr[32] = {};
    uint32_t cr = 0;
    uint64_t xer = 0, lr = 0, ctr = 0;
    uint8_t* mem = nullptr;  // guest address a is mem[a]
};

// Executes one instruction that isn't a branch, trap or system call. Returns
// false if it isn't one the reference knows (the test then fails: every
// instruction of the slice must be here).
bool step(State& s, const ppu::Insn& in);

// A conditional branch's decision (and its CTR update): taken or not.
bool branch_taken(State& s, unsigned bo, unsigned bi);

// Does this trap instruction fire?
bool trap_fires(const State& s, const ppu::Insn& in);

}  // namespace qilin::ref

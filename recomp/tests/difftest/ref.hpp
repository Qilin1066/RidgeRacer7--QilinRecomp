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
    uint64_t fpr[32] = {};  // as bit patterns
    uint8_t vr[32][16] = {};  // vector registers, in the guest's byte order
    uint32_t cr = 0;
    uint64_t xer = 0, lr = 0, ctr = 0;
    uint8_t* mem = nullptr;  // guest address a is mem[a]
};

// Executes one instruction that isn't a branch, trap or system call. Returns
// false if it isn't one the reference knows (the test then fails: every
// instruction of the slice must be here).
bool step(State& s, const ppu::Insn& in);

// The floating-point instructions (ref_fp.cpp: integers only, no host floating point). Called by step.
bool step_fp(State& s, const ppu::Insn& in);

// VMX (ref_vmx.cpp), on bytes and the soft-float below. Called by step_fp.
bool step_vmx(State& s, const ppu::Insn& in);

// The soft-float's pieces (ref_fp.cpp), on doubles' bits; `single` rounds to a single held in a double.
namespace soft {
uint64_t fused(uint64_t a, uint64_t c, uint64_t b, bool subtract, bool single);  // a * c + b, or - b
uint64_t from_integer(bool negative, uint64_t magnitude, int exponent, bool single);  // magnitude * 2^exponent
uint64_t to_integer(uint64_t value, unsigned bits, bool truncate);
uint32_t compare(uint64_t a, uint64_t b);  // 8 less, 4 greater, 2 equal, 1 unordered
uint64_t single_to_double(uint32_t word);
uint32_t double_to_single(uint64_t value);
}  // namespace soft

// A conditional branch's decision (and its CTR update): taken or not.
bool branch_taken(State& s, unsigned bo, unsigned bi);

// Does this trap instruction fire?
bool trap_fires(const State& s, const ppu::Insn& in);

}  // namespace qilin::ref

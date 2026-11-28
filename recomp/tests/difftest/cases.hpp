// What qilin_difftest_gen writes and qilin_difftest runs.
#pragma once

#include <cstddef>
#include <cstdint>

// The differential test checks the exact mode of the floating-point helpers (fp.hpp, QILIN_FP_EXACT): the relaxed
// mode, the game's, has its own checks (run.cpp, "relaxed floating point").
#if defined(QILIN_FP_EXACT) && !QILIN_FP_EXACT
#error "the differential test runs the exact floating point (QILIN_FP_EXACT 1)"
#endif
#undef QILIN_FP_EXACT
#define QILIN_FP_EXACT 1
#include "qilin/rt/cpu.hpp"
static_assert(qilin::rt::fp::kExact, "qilin/rt/cpu.hpp was included before cases.hpp, in the relaxed mode");

namespace difftest {

enum class Kind : uint8_t {
    Plain,   // one instruction, then blr: every register and the memory it touched must match the reference
    Bc,      // bc to a second return: r3 says which way it went
    Bclr,    // bclr, then a second return
    Bcctr,   // bcctr (a jump through CTR, reported by the call hook), then a return
    Trap,    // a trap instruction, then a return
    Call,    // bl to a second function, which returns
    Tail,    // b to a second function
    System,  // sc
    Switch,  // a jump table of two targets
    TimeBase,  // mftb: the hook's value, or its upper half
    Program,   // a function of many instructions, with branches and calls (gen.cpp): run by the reference word by word
};

// A word of a program the reference runs, at its address.
struct ProgramWord {
    uint32_t at, word;
};

struct Case {
    Kind kind;
    uint32_t word;  // the instruction under test
    uint32_t pc;    // where it is
    void (*fn)(qilin::rt::Cpu&);
    const ProgramWord* program = nullptr;  // Kind::Program: its words and its helper's, and where it starts
    uint32_t program_size = 0, entry = 0;
};

extern const Case kCases[];
extern const size_t kCaseCount;

// The unwinding program (gen.cpp, "C++ exceptions"): its words, for the reference, and its first function, lifted.
extern const ProgramWord kEhProgram[];
extern const size_t kEhProgramSize;
extern const uint32_t kEhEntry, kEhPad;
void eh_entry(qilin::rt::Cpu& c);

}  // namespace difftest

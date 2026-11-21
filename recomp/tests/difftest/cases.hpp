// What qilin_difftest_gen writes and qilin_difftest runs.
#pragma once

#include <cstddef>
#include <cstdint>

#include "qilin/rt/cpu.hpp"

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
};

struct Case {
    Kind kind;
    uint32_t word;  // the instruction under test
    uint32_t pc;    // where it is
    void (*fn)(qilin::rt::Cpu&);
};

extern const Case kCases[];
extern const size_t kCaseCount;

}  // namespace difftest

// The SPU differential test: what the generator writes, what the reference
// keeps, and what the two sides agree the hooks answer.
#pragma once

#include <cstddef>
#include <cstdint>

#include "qilin/rt/spu.hpp"
#include "spu.hpp"

namespace spu_difftest {

// One lifted instruction, as a function.
struct Case {
    uint32_t word;  // the instruction under test
    uint32_t pc;    // where it is
    void (*fn)(qilin::rt::Spu&);
};
extern const Case kCases[];
extern const size_t kCaseCount;

// The reference's state: registers as sixteen bytes each, in the SPU's order.
struct State {
    uint8_t r[128][16];
    uint8_t* ls = nullptr;
    bool stored = false;  // a quadword was written to the local store, at
    uint32_t stored_at = 0;
    bool wrote_channel = false;
    uint32_t channel = 0, channel_data = 0;
    bool stopped = false;
    uint32_t stop_code = 0;
    bool halted = false;
};

// Runs one instruction that is not a branch. False if the reference doesn't know it.
bool step(State& s, const qilin::spu::Insn& in, uint32_t pc);
// The floating-point instructions (ref_fp.cpp); `step` calls it.
bool step_fp(State& s, const qilin::spu::Insn& in);

// What reading a channel, or asking its count, gives in the test (the runner's hooks answer the same).
inline uint32_t channel_value(uint32_t channel) { return 0xC0DE0000u + channel * 0x101u; }
inline uint32_t channel_count(uint32_t channel) { return channel & 1u; }

}  // namespace spu_difftest

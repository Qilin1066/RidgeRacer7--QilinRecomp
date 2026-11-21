// The SPU lifter: one function of one SPU program, as the control-flow pass
// found it (spu_cfg.hpp), to a C++ function.
//
// The shape is the PPU lifter's (lift.hpp), and simpler: an SPU has no
// condition register and no table of contents, calls are `brsl` and returns
// `bi $0`. A lifted function is `void fn_XXXXX(Spu& s)`. Registers are read
// and written in the Spu itself (128 of 16 bytes: copying them in and out
// around every call, as the PPU's locals are, would cost more than it
// saves; to be measured). Each instruction is one call of a helper in
// runtime/include/qilin/rt/spu.hpp.
//
// A program's functions call each other by name. A branch through a
// register goes to `call(s, target, pc)`, which the program's file defines:
// a switch over its function starts. Channels and stops go to the Spu's
// hooks. As with the PPU, a function is lifted whole or not at all:
// `spu_in_slice` says which instructions the lifter knows.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "spu.hpp"
#include "spu_cfg.hpp"

namespace qilin {

struct SpuLifted {
    bool ok = false;
    std::string code;  // the definition, when ok
    std::string why;   // why not, when !ok
    uint32_t why_at = 0;
};

// "fn_03010": a function's name in its program's lifted code.
std::string spu_fn_name(uint32_t address);

// Whether the lifter knows this instruction. `why`, if given, is set when it doesn't.
bool spu_in_slice(const spu::Insn& in, std::string* why = nullptr);

SpuLifted spu_lift(const SpuCfg& cfg, const SpuFunction& f);

// One instruction that is not a branch, as the line the lifter writes for it: for the lifter's own
// test, which runs single instructions. Nothing if the lifter doesn't know it, or it is a branch.
std::optional<std::string> spu_lift_one(const spu::Insn& in, uint32_t pc);

}  // namespace qilin

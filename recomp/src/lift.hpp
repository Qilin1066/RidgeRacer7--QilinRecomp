// The PPU lifter: one guest function to one C++ function over qilin::rt::Cpu
// (runtime/include/qilin/rt/cpu.hpp).
//
// Slice 1 (v0.8.0): integer arithmetic and logic in both widths, rotates and
// shifts, compares and the condition register, integer loads and stores (with
// update, indexed, byte-reversed), LR, CTR and XER, traps, system calls, the
// cache and synchronisation instructions that need nothing done, and all
// branches and calls, switches among them. Slice 2 (v0.9.0): floating point,
// bit-exact (runtime/include/qilin/rt/fp.hpp): loads and stores, arithmetic
// in both precisions, the fused forms, conversions, compares, fsel; the time
// base. Slice 3 (v0.10.0): the part of VMX the game uses
// (runtime/include/qilin/rt/vmx.hpp). Anything else (the rest of VMX and
// its two estimates, the FPSCR, the scalar estimates, floating-point record
// forms, atomic sequences, the overflow-enable and string forms) makes the
// function one that isn't lifted yet, with the reason: nothing is skipped
// silently (docs/recompiler-core.md).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "cfg.hpp"
#include "ppu.hpp"

namespace qilin {

struct LiftInput {
    uint32_t start = 0;
    std::vector<std::pair<uint32_t, ppu::Insn>> insns;  // the instructions reached, by address
    std::vector<JumpTable> tables;
    std::vector<uint32_t> landing_pads;         // labels nothing in the function branches to
    uint32_t falls_into = 0;                    // the next function, if execution runs into it
    std::function<bool(uint32_t)> is_function;  // a known function start (tail and call targets)
    bool keep_pc = false;                       // note each instruction's address in Cpu::pc before it touches memory (for finding faults: slower)
    bool keep_registers = false;                // and write the registers back to the Cpu there too, so a fault's report can show them (slower still)
};

struct Lifted {
    bool ok = false;
    std::string code;  // the definition, when ok
    std::string why;   // why not, when !ok: the first instruction outside the slice
    uint32_t why_at = 0;
};

std::string fn_name(uint32_t address);  // "fn_00010200"

// Is this instruction in the current slice?
bool in_slice(const ppu::Insn& in, std::string* why = nullptr);

Lifted lift(const LiftInput& f);

// The input for one of Cfg's functions.
LiftInput lift_input(const Cfg& cfg, const Function& f);

}  // namespace qilin

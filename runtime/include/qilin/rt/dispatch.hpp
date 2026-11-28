// The lifted function at a code address, for calls through a register (bctrl, blrl) and the runtime's own calls.
//
// A dense index over the code's range, a slot for each word (an instruction's address, (address - first) / 4),
// each the function's number in the table or none: one load and a compare, where a binary search of the game's
// 14,294 functions took about 35 ns a call (its branches mispredict). The game's code spans about 4.5 MB, so the
// index is about 2.3 MB of 16-bit slots.
//
// Runtime code: no std::format here.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "qilin/rt/lifted.hpp"
#include "qilin/rt/report.hpp"

namespace qilin::rt {

class FunctionIndex {
public:
    FunctionIndex() = default;
    // `functions` in address order, as the lifter writes them; kept by reference.
    explicit FunctionIndex(std::span<const LiftedFunction> functions) : functions_(functions) {
        if (functions.empty()) return;
        if (functions.size() >= kNone) fatal("%zu lifted functions, more than the function index numbers (%u)", functions.size(), unsigned{kNone});
        first_ = functions.front().address;
        slots_.assign((functions.back().address - first_) / 4 + 1, kNone);
        for (size_t i = 0; i < functions.size(); ++i) {
            const uint32_t a = functions[i].address;
            if (a % 4 || a < first_ || (i && a <= functions[i - 1].address)) fatal("the lifted functions' table is out of order or misaligned at %08x", a);
            slots_[(a - first_) / 4] = static_cast<uint16_t>(i);
        }
    }
    // The function starting at `address`, or nullptr.
    GuestFn find(uint32_t address) const {
        const uint32_t slot = (address - first_) / 4;  // (an address below the first wraps past the end)
        if (address % 4 || slot >= slots_.size()) return nullptr;
        const uint16_t i = slots_[slot];
        return i == kNone ? nullptr : functions_[i].fn;
    }

private:
    static constexpr uint16_t kNone = 0xFFFF;
    std::span<const LiftedFunction> functions_;
    std::vector<uint16_t> slots_;
    uint32_t first_ = 0;
};

}  // namespace qilin::rt

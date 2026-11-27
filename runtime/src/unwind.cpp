// C++ exceptions in lifted code: the guest's unwinder entering a landing pad (cpu.hpp, docs/recompiler-core.md).
#include "qilin/rt/cpu.hpp"

namespace qilin::rt {

void eh_return(const Cpu& c) { throw GuestUnwind{static_cast<uint32_t>(c.lr) & ~3u, static_cast<uint32_t>(c.r[1])}; }

void run_with_landing_pads(Cpu& c, void (*start)(Cpu&), void (*at_pad)(Cpu&, uint32_t pad), std::span<const uint32_t> pads) {
    // This activation's frame: its function makes its frame below the stack pointer it was entered with, and the
    // frame's first word (the back chain, which `stdu r1` writes) is that stack pointer. The unwinder sends an exception
    // to a pad with the stack pointer of the frame it belongs to, so a recursive call of the same function (another
    // frame, whose back chain is not this one's) lets it pass to the activation it is for.
    const uint32_t entered = static_cast<uint32_t>(c.r[1]);
    uint32_t pad = 0;  // (0: not yet at one)
    for (;;) {
        try {
            if (pad) at_pad(c, pad);
            else start(c);
            return;
        } catch (const GuestUnwind& e) {
            bool own = false;
            for (const uint32_t p : pads) own |= p == e.pad;
            if (!own || static_cast<uint32_t>(read64(c, e.sp)) != entered) throw;
            pad = e.pad;
        }
    }
}

}  // namespace qilin::rt

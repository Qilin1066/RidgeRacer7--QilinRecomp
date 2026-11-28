// The guest CPU state and memory access that lifted PPU code compiles against.
//
// Lifted functions take a `Cpu&`. Inside a function the guest registers live
// in host locals; they are written back to the Cpu, and read again, only
// around calls and returns, where another function can see them
// (docs/recompiler-core.md, "The lifter").
//
// The PPU runs in 64-bit mode: registers are 64 bits wide, record forms and
// carries look at all 64, and only the instructions with a "w" in their
// name work on the low 32. But an LV2 process's addresses are 32 bits wide:
// every effective address is truncated to 32 bits before it is used.
//
// Where the architecture leaves a result undefined (the upper half of a word
// multiply-high or divide, a divide by zero), the helper says what it gives
// and that the value is a choice, to be confirmed against the hardware
// (RPCS3 running the game) if anything is ever found to depend on it.
//
// Runtime code: no std::format here (macOS 10.15, see CMakeLists.txt).
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "qilin/rt/fp.hpp"
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>  // the high half of a 64-bit multiply
#endif

// The lifted code's hottest helpers are forced inline: in a large lifted
// function the compiler's own limits otherwise leave a call (and its register
// spills) behind every load and store.
#if defined(_MSC_VER) && !defined(__clang__)
#define QILIN_INLINE __forceinline
#else
#define QILIN_INLINE [[gnu::always_inline]] inline
#endif

namespace qilin::rt {

static_assert(std::endian::native == std::endian::little, "hosts are little-endian; the guest is big-endian");

struct Cpu;
using GuestFn = void (*)(Cpu&);

// Called where lifted code leaves what was known at build time. Each must
// not return if it can't handle the case.
struct CpuHooks {
    void (*call)(Cpu&, uint32_t target);               // bctrl / bctr / blrl: a call or jump to a computed code address
    void (*system_call)(Cpu&, uint32_t pc);            // sc: the call's number is in r11
    void (*trap)(Cpu&, uint32_t pc, const char* why);  // a trap instruction that fired; must not return
    uint64_t (*time_base)(Cpu&);                       // mftb: the time base, which counts 79,800,000 to the second on a PS3
    // mtfsf left the FPSCR asking for what isn't provided: a rounding mode other than to nearest, non-IEEE mode, or an
    // exception enabled (its low byte not zero). The game's must not return.
    void (*fpscr_mode)(Cpu&, uint32_t pc);
};

// A vector register: four words, element 0 first (the guest's order), each a host integer.
struct Vec {
    uint32_t w[4];
};

struct Cpu {
    uint64_t r[32] = {};
    uint64_t lr = 0, ctr = 0;
    uint32_t cr = 0;  // CR0 in the top four bits, as mfcr reads it
    uint32_t xer_so = 0, xer_ov = 0, xer_ca = 0, xer_bc = 0;  // XER's bits, one per word
    double f[32] = {};
    Vec v[32] = {};
    uint32_t fpscr = 0, vscr = 0;
    uint32_t pc = 0;  // kept only by code lifted with `qilin lift --pc`: the last instruction to touch memory
    uint8_t* mem = nullptr;  // base of the 4 GiB guest address space
    const CpuHooks* hooks = nullptr;

    uint64_t xer() const { return uint64_t{xer_so} << 31 | uint64_t{xer_ov} << 30 | uint64_t{xer_ca} << 29 | (xer_bc & 0x7Fu); }
    void set_xer(uint64_t value) {
        xer_so = (value >> 31) & 1;
        xer_ov = (value >> 30) & 1;
        xer_ca = (value >> 29) & 1;
        xer_bc = value & 0x7F;
    }
};

// ---- C++ exceptions: the guest's unwinder entering a landing pad ----
// The game's own unwinder (lifted with the rest) ends, once it has found a landing pad, in __builtin_eh_return's
// epilogue: the registers of the pad's frame restored, the stack pointer moved up to that frame by a register, and a
// return to the pad through LR. A lifted function can't return into another function's middle, so the lifter makes
// that epilogue a throw of this, carrying the pad and the stack pointer, with every register written back to the Cpu.
// A function with landing pads is lifted twice, as a body entered at its start and one entered at a pad (which reads
// the registers from the Cpu), and run by run_with_landing_pads: it takes the exception when the pad is one of the
// function's and the pad's frame is this activation's, and runs the second body from that pad
// (docs/recompiler-core.md). Lifted code between the two (and runtime code that called guest code) lets it pass: the
// host's unwinding of their frames is what the guest's unwinder did to theirs. One nobody takes stops the thread.
struct GuestUnwind {
    uint32_t pad = 0;  // the landing pad's address
    uint32_t sp = 0;   // the stack pointer it runs with: its function's frame
};
[[noreturn]] void eh_return(const Cpu& c);
// The only try lifted code runs in (unwind.cpp): lifted functions have none of their own.
void run_with_landing_pads(Cpu& c, void (*start)(Cpu&), void (*at_pad)(Cpu&, uint32_t pad), std::span<const uint32_t> pads);

// mtfsf: the FPSCR's fields picked by the mask (bit 7 field 0, FPSCR bits 0-3) from the low word of frB's bits. FEX and VX
// (bits 1 and 2) are summaries, not written: VX is the OR of the invalid-operation bits, FEX of the exceptions enabled.
// (*The exception bits are only what was written: this runtime doesn't keep the flags, so a later mffs reads them back as
// set, not as arithmetic since would have left them. A stated choice.*)
inline uint32_t fpscr_fields(uint32_t old, unsigned fm, double b) {
    uint32_t mask = 0;
    for (unsigned k = 0; k < 8; ++k)
        if ((fm >> (7 - k)) & 1) mask |= 0xF0000000u >> (4 * k);
    uint32_t v = (old & ~mask) | (static_cast<uint32_t>(std::bit_cast<uint64_t>(b)) & mask);
    v &= ~0x60000000u;
    if (v & 0x01F80700u) v |= 0x20000000u;  // VX: VXSNAN, VXISI, VXIDI, VXZDZ, VXIMZ, VXVC, VXSOFT, VXSQRT, VXCVI
    const bool fex = ((v & 0x20000000u) && (v & 0x80u)) || ((v & 0x10000000u) && (v & 0x40u)) || ((v & 0x08000000u) && (v & 0x20u)) ||
                     ((v & 0x04000000u) && (v & 0x10u)) || ((v & 0x02000000u) && (v & 0x08u));
    if (fex) v |= 0x40000000u;
    return v;
}

// ---- memory: plain host memory at mem + address, in the guest's byte order ----
// Lifted code passes a local copy of Cpu::mem (`m`), not the Cpu: a store through a byte pointer may alias anything,
// so a compiler would otherwise read c.mem again after every store. A load is a memcpy and a byte swap: one mov and a
// bswap (or a movbe, or a rev on ARM).
QILIN_INLINE uint64_t read8(const uint8_t* m, uint32_t a) { return m[a]; }
QILIN_INLINE uint64_t read16(const uint8_t* m, uint32_t a) {
    uint16_t v;
    std::memcpy(&v, m + a, 2);
    return std::byteswap(v);
}
QILIN_INLINE uint64_t read32(const uint8_t* m, uint32_t a) {
    uint32_t v;
    std::memcpy(&v, m + a, 4);
    return std::byteswap(v);
}
QILIN_INLINE uint64_t read64(const uint8_t* m, uint32_t a) {
    uint64_t v;
    std::memcpy(&v, m + a, 8);
    return std::byteswap(v);
}
QILIN_INLINE void write8(uint8_t* m, uint32_t a, uint64_t v) { m[a] = static_cast<uint8_t>(v); }
QILIN_INLINE void write16(uint8_t* m, uint32_t a, uint64_t v) {
    const uint16_t s = std::byteswap(static_cast<uint16_t>(v));
    std::memcpy(m + a, &s, 2);
}
QILIN_INLINE void write32(uint8_t* m, uint32_t a, uint64_t v) {
    const uint32_t s = std::byteswap(static_cast<uint32_t>(v));
    std::memcpy(m + a, &s, 4);
}
QILIN_INLINE void write64(uint8_t* m, uint32_t a, uint64_t v) {
    const uint64_t s = std::byteswap(v);
    std::memcpy(m + a, &s, 8);
}
// The same through a Cpu, for the runtime's own code.
QILIN_INLINE uint64_t read8(const Cpu& c, uint32_t a) { return read8(c.mem, a); }
QILIN_INLINE uint64_t read16(const Cpu& c, uint32_t a) { return read16(c.mem, a); }
QILIN_INLINE uint64_t read32(const Cpu& c, uint32_t a) { return read32(c.mem, a); }
QILIN_INLINE uint64_t read64(const Cpu& c, uint32_t a) { return read64(c.mem, a); }
QILIN_INLINE void write8(const Cpu& c, uint32_t a, uint64_t v) { write8(c.mem, a, v); }
QILIN_INLINE void write16(const Cpu& c, uint32_t a, uint64_t v) { write16(c.mem, a, v); }
QILIN_INLINE void write32(const Cpu& c, uint32_t a, uint64_t v) { write32(c.mem, a, v); }
QILIN_INLINE void write64(const Cpu& c, uint32_t a, uint64_t v) { write64(c.mem, a, v); }

// ---- the condition register: a field is four bits, LT GT EQ SO from the top ----
// (Lifted branches mostly read a compare's operands instead, lift.cpp: these are for the fields' values. A form
// without branches, 2 << ((a < b) << 1 | (a > b)), was miscompiled by MSVC 19.44 in one of the differential test's
// programs, so it isn't used.)
QILIN_INLINE uint32_t compare_s(int64_t a, int64_t b, uint32_t so) { return (a < b ? 8u : a > b ? 4u : 2u) | so; }
QILIN_INLINE uint32_t compare_u(uint64_t a, uint64_t b, uint32_t so) { return (a < b ? 8u : a > b ? 4u : 2u) | so; }
// CR0 after a record form ("."): the sign of the 64-bit result, and XER's SO.
QILIN_INLINE uint32_t record(uint64_t v, uint32_t so) { return compare_s(static_cast<int64_t>(v), 0, so); }

// ---- arithmetic ----
// a + b + carry_in, with the carry out of bit 0 (all 64 bits take part: 64-bit mode).
QILIN_INLINE uint64_t add_carry(uint64_t a, uint64_t b, uint32_t carry_in, uint32_t& carry_out) {
    const uint64_t sum = a + b;
    const uint64_t result = sum + carry_in;
    carry_out = static_cast<uint32_t>(sum < a) | static_cast<uint32_t>(result < sum);
    return result;
}

// The high 32 bits of a 32 x 32 product, in the low word. The upper word is
// undefined by the architecture: here, the sign (or zero) extension of the low.
QILIN_INLINE uint64_t mulhw(uint64_t a, uint64_t b) {
    const int64_t product = int64_t{static_cast<int32_t>(a)} * int64_t{static_cast<int32_t>(b)};
    return static_cast<uint64_t>(product >> 32);
}
QILIN_INLINE uint64_t mulhwu(uint64_t a, uint64_t b) { return (uint64_t{static_cast<uint32_t>(a)} * uint64_t{static_cast<uint32_t>(b)}) >> 32; }

#if !defined(_MSC_VER) || defined(__clang__)
__extension__ typedef unsigned __int128 Unsigned128;  // (a compiler extension, said so to -Wpedantic)
__extension__ typedef __int128 Signed128;
#endif
QILIN_INLINE uint64_t mulhdu(uint64_t a, uint64_t b) {
#if defined(_MSC_VER) && !defined(__clang__)
    return __umulh(a, b);
#else
    return static_cast<uint64_t>((static_cast<Unsigned128>(a) * b) >> 64);
#endif
}
QILIN_INLINE uint64_t mulhd(uint64_t a, uint64_t b) {
#if defined(_MSC_VER) && !defined(__clang__)
    return static_cast<uint64_t>(__mulh(static_cast<int64_t>(a), static_cast<int64_t>(b)));
#else
    return static_cast<uint64_t>((static_cast<Signed128>(static_cast<int64_t>(a)) * static_cast<int64_t>(b)) >> 64);
#endif
}

// Division. A quotient that can't be represented (a divide by zero, the most
// negative number by -1) is undefined by the architecture: here, 0. A word
// divide's upper half is undefined too: here, zero.
QILIN_INLINE uint64_t divw(uint64_t a, uint64_t b) {
    const int32_t n = static_cast<int32_t>(a), d = static_cast<int32_t>(b);
    if (d == 0 || (n == std::numeric_limits<int32_t>::min() && d == -1)) return 0;
    return static_cast<uint32_t>(n / d);
}
QILIN_INLINE uint64_t divwu(uint64_t a, uint64_t b) {
    const uint32_t n = static_cast<uint32_t>(a), d = static_cast<uint32_t>(b);
    return d == 0 ? 0 : n / d;
}
QILIN_INLINE uint64_t divd(uint64_t a, uint64_t b) {
    const int64_t n = static_cast<int64_t>(a), d = static_cast<int64_t>(b);
    if (d == 0 || (n == std::numeric_limits<int64_t>::min() && d == -1)) return 0;
    return static_cast<uint64_t>(n / d);
}
QILIN_INLINE uint64_t divdu(uint64_t a, uint64_t b) { return b == 0 ? 0 : a / b; }

// ---- rotates and shifts ----
// The low word rotated left, in both halves of the result (as the architecture defines ROTL32).
QILIN_INLINE uint64_t rot32(uint64_t x, unsigned n) {
    const uint32_t rotated = std::rotl(static_cast<uint32_t>(x), static_cast<int>(n & 31));
    return uint64_t{rotated} << 32 | rotated;
}
// Shifts by a register take one more bit of the amount than the width needs: an amount past the width gives 0 (or all sign).
QILIN_INLINE uint64_t slw(uint64_t x, uint64_t amount) {
    const unsigned n = amount & 63;
    return n > 31 ? 0 : static_cast<uint32_t>(static_cast<uint32_t>(x) << n);
}
QILIN_INLINE uint64_t srw(uint64_t x, uint64_t amount) {
    const unsigned n = amount & 63;
    return n > 31 ? 0 : static_cast<uint32_t>(x) >> n;
}
QILIN_INLINE uint64_t sld(uint64_t x, uint64_t amount) {
    const unsigned n = amount & 127;
    return n > 63 ? 0 : x << n;
}
QILIN_INLINE uint64_t srd(uint64_t x, uint64_t amount) {
    const unsigned n = amount & 127;
    return n > 63 ? 0 : x >> n;
}
// Arithmetic shifts right set CA when the value is negative and any 1 bit was shifted out.
QILIN_INLINE uint64_t sraw(uint64_t x, uint64_t amount, uint32_t& ca) {
    const unsigned n = amount & 63;
    const int32_t value = static_cast<int32_t>(x);
    if (n > 31) {
        ca = value < 0;
        return value < 0 ? ~uint64_t{0} : 0;
    }
    ca = value < 0 && (static_cast<uint32_t>(value) & ((uint32_t{1} << n) - 1)) != 0;
    return static_cast<uint64_t>(int64_t{value >> n});
}
QILIN_INLINE uint64_t srad(uint64_t x, uint64_t amount, uint32_t& ca) {
    const unsigned n = amount & 127;
    const int64_t value = static_cast<int64_t>(x);
    if (n > 63) {
        ca = value < 0;
        return value < 0 ? ~uint64_t{0} : 0;
    }
    ca = value < 0 && (x & ((uint64_t{1} << n) - 1)) != 0;
    return static_cast<uint64_t>(value >> n);
}

// ---- traps: TO's five bits choose among the comparisons of a with b ----
QILIN_INLINE bool trap_on(unsigned to, int64_t a, int64_t b) {
    return ((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b) ||
           ((to & 2) && static_cast<uint64_t>(a) < static_cast<uint64_t>(b)) || ((to & 1) && static_cast<uint64_t>(a) > static_cast<uint64_t>(b));
}

}  // namespace qilin::rt

#include "qilin/rt/vmx.hpp"

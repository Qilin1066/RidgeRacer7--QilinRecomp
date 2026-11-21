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
};

struct Cpu {
    uint64_t r[32] = {};
    uint64_t lr = 0, ctr = 0;
    uint32_t cr = 0;  // CR0 in the top four bits, as mfcr reads it
    uint32_t xer_so = 0, xer_ov = 0, xer_ca = 0, xer_bc = 0;  // XER's bits, one per word
    double f[32] = {};
    uint8_t v[32][16] = {};  // vector registers, in the guest's byte order
    uint32_t fpscr = 0, vscr = 0;
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

// ---- memory: plain host memory at mem + address, in the guest's byte order ----
QILIN_INLINE uint64_t read8(Cpu& c, uint32_t a) { return c.mem[a]; }
QILIN_INLINE uint64_t read16(Cpu& c, uint32_t a) {
    uint16_t v;
    std::memcpy(&v, c.mem + a, 2);
    return std::byteswap(v);
}
QILIN_INLINE uint64_t read32(Cpu& c, uint32_t a) {
    uint32_t v;
    std::memcpy(&v, c.mem + a, 4);
    return std::byteswap(v);
}
QILIN_INLINE uint64_t read64(Cpu& c, uint32_t a) {
    uint64_t v;
    std::memcpy(&v, c.mem + a, 8);
    return std::byteswap(v);
}
QILIN_INLINE void write8(Cpu& c, uint32_t a, uint64_t v) { c.mem[a] = static_cast<uint8_t>(v); }
QILIN_INLINE void write16(Cpu& c, uint32_t a, uint64_t v) {
    const uint16_t s = std::byteswap(static_cast<uint16_t>(v));
    std::memcpy(c.mem + a, &s, 2);
}
QILIN_INLINE void write32(Cpu& c, uint32_t a, uint64_t v) {
    const uint32_t s = std::byteswap(static_cast<uint32_t>(v));
    std::memcpy(c.mem + a, &s, 4);
}
QILIN_INLINE void write64(Cpu& c, uint32_t a, uint64_t v) {
    const uint64_t s = std::byteswap(v);
    std::memcpy(c.mem + a, &s, 8);
}

// ---- the condition register: a field is four bits, LT GT EQ SO from the top ----
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

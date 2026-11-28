// VMX (AltiVec) for lifted PPU code: the part of it the game uses.
//
// A vector register is four words, element 0 first (the guest's order),
// each held as a host integer. Bytes are numbered as the guest numbers them:
// byte 0 is the top byte of word 0. Everything is plain portable C++ on
// fixed-size arrays, which compilers vectorise: no host intrinsics.
//
// Floating point is single precision, element by element, bit-exact:
//   - a NaN operand gives the first NaN operand (vA, then vB, then vC), made
//     quiet; an invalid operation gives the default NaN;
//   - vmaddfp and vnmsubfp are fused: the product is not rounded (it is
//     taken exactly in a double, and one sum rounded once, as fp.hpp does);
//   - **the "non-Java" mode is assumed on** (VSCR's NJ bit set, as public
//     descriptions say an LV2 thread starts): a denormal operand is taken as
//     zero and a denormal result becomes zero, each with its sign. The game
//     never touches the VSCR, so it runs in whatever mode its threads start
//     in. *To be confirmed against RPCS3*; `kNonJava` below is the switch.
// The VSCR's saturation bit is not kept, and the instructions that read or
// write the VSCR are not in the lifter's slice.
//
// **vrefp and vrsqrtefp are estimates**: the architecture asks only that
// they be within one part in 4,096 of 1/x and 1/sqrt(x), and what a PPU
// gives exactly comes from tables of its own, which are not known here.
// The stated choice: **the correctly rounded value**, which is within the
// architecture's bound and is one definite thing a reference can check.
// It can differ from a console's result in the low bits. *If the PPU's
// tables can be had, they replace this.*
//
// **The two modes** (fp.hpp, QILIN_FP_EXACT): all of the above is the exact
// mode (vmx::exact), which the differential test runs. The relaxed mode
// (vmx::relaxed, the game's build) is the host's single precision: no NaN
// rules, no denormal flush of its own (the host's flush-to-zero mode, set
// once per thread by fp::enter_thread_mode, does that), vmaddfp and
// vnmsubfp as the host's fused single multiply-add where it has one (ARM64,
// x86 built for FMA) and else the double sum of the exact product rounded to
// single (within one unit), vrefp and vrsqrtefp as the host's division and
// square root, vmaxfp and vminfp as a compare and a choice. It is SSE on
// x86-64 and NEON on ARM64, with a portable twin (vmx::portable, the plain
// C++ elsewhere and with QILIN_VMX_PORTABLE) that gives the same results.
// The integer, logical and permute instructions, and the conversions, are
// the same in both modes.
//
// Included by cpu.hpp. Runtime code: no std::format here.
#pragma once

#if !defined(QILIN_VMX_PORTABLE) && (defined(__x86_64__) || defined(_M_X64))
#include <emmintrin.h>
#if defined(__FMA__) || defined(__AVX2__)
#include <immintrin.h>
#endif
#define QILIN_VMX_SSE 1
#elif !defined(QILIN_VMX_PORTABLE) && (defined(__aarch64__) || defined(_M_ARM64))
#include <arm_neon.h>
#define QILIN_VMX_NEON 1
#endif

namespace qilin::rt::vmx {

inline constexpr bool kNonJava = true;

// ---- bytes ----
QILIN_INLINE unsigned byte(const Vec& v, unsigned i) { return (v.w[i >> 2] >> (24 - 8 * (i & 3))) & 0xffu; }
QILIN_INLINE void set_byte(Vec& v, unsigned i, unsigned b) {
    const unsigned shift = 24 - 8 * (i & 3);
    v.w[i >> 2] = (v.w[i >> 2] & ~(0xffu << shift)) | ((b & 0xffu) << shift);
}

// ---- memory: lvx and stvx ignore the low four bits of the address ----
QILIN_INLINE Vec lvx(const uint8_t* m, uint32_t ea) {
    ea &= ~15u;
    return {{static_cast<uint32_t>(read32(m, ea)), static_cast<uint32_t>(read32(m, ea + 4)), static_cast<uint32_t>(read32(m, ea + 8)),
             static_cast<uint32_t>(read32(m, ea + 12))}};
}
QILIN_INLINE void stvx(uint8_t* m, uint32_t ea, const Vec& v) {
    ea &= ~15u;
    for (unsigned i = 0; i < 4; ++i) write32(m, ea + 4 * i, v.w[i]);
}
// The permute controls for unaligned access: sixteen consecutive byte numbers, from the address's low four bits.
inline Vec lvsl(uint32_t ea) {
    Vec r{};
    for (unsigned i = 0; i < 16; ++i) set_byte(r, i, (ea & 15) + i);
    return r;
}
inline Vec lvsr(uint32_t ea) {
    Vec r{};
    for (unsigned i = 0; i < 16; ++i) set_byte(r, i, 16 - (ea & 15) + i);
    return r;
}

// ---- logical, select, permute, shift, merge, splat ----
QILIN_INLINE Vec vand(const Vec& a, const Vec& b) { return {{a.w[0] & b.w[0], a.w[1] & b.w[1], a.w[2] & b.w[2], a.w[3] & b.w[3]}}; }
QILIN_INLINE Vec vandc(const Vec& a, const Vec& b) { return {{a.w[0] & ~b.w[0], a.w[1] & ~b.w[1], a.w[2] & ~b.w[2], a.w[3] & ~b.w[3]}}; }
QILIN_INLINE Vec vor(const Vec& a, const Vec& b) { return {{a.w[0] | b.w[0], a.w[1] | b.w[1], a.w[2] | b.w[2], a.w[3] | b.w[3]}}; }
QILIN_INLINE Vec vxor(const Vec& a, const Vec& b) { return {{a.w[0] ^ b.w[0], a.w[1] ^ b.w[1], a.w[2] ^ b.w[2], a.w[3] ^ b.w[3]}}; }
QILIN_INLINE Vec vnor(const Vec& a, const Vec& b) { return {{~(a.w[0] | b.w[0]), ~(a.w[1] | b.w[1]), ~(a.w[2] | b.w[2]), ~(a.w[3] | b.w[3])}}; }
// Each bit from b where c has a 1, else from a.
QILIN_INLINE Vec vsel(const Vec& a, const Vec& b, const Vec& c) {
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = (a.w[i] & ~c.w[i]) | (b.w[i] & c.w[i]);
    return r;
}
// Each byte chosen from the 32 of a and b by the low five bits of c's byte.
inline Vec vperm(const Vec& a, const Vec& b, const Vec& c) {
    uint8_t from[32] = {};
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t x = std::byteswap(a.w[i]), y = std::byteswap(b.w[i]);
        std::memcpy(from + 4 * i, &x, 4);
        std::memcpy(from + 16 + 4 * i, &y, 4);
    }
    Vec r;
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t control = c.w[i];
        r.w[i] = uint32_t{from[(control >> 24) & 31]} << 24 | uint32_t{from[(control >> 16) & 31]} << 16 | uint32_t{from[(control >> 8) & 31]} << 8 |
                 from[control & 31];
    }
    return r;
}
// Sixteen bytes of a and b end to end, from byte `shift` (0 to 15): whole words, and the bytes between two.
QILIN_INLINE Vec vsldoi(const Vec& a, const Vec& b, unsigned shift) {
    const uint32_t w[8] = {a.w[0], a.w[1], a.w[2], a.w[3], b.w[0], b.w[1], b.w[2], b.w[3]};
    const unsigned word = shift >> 2, bits = 8 * (shift & 3);
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = bits ? (w[word + i] << bits) | (w[word + i + 1] >> (32 - bits)) : w[word + i];
    return r;
}
QILIN_INLINE Vec vmrghw(const Vec& a, const Vec& b) { return {{a.w[0], b.w[0], a.w[1], b.w[1]}}; }
QILIN_INLINE Vec vmrglw(const Vec& a, const Vec& b) { return {{a.w[2], b.w[2], a.w[3], b.w[3]}}; }
QILIN_INLINE Vec vspltw(const Vec& b, unsigned element) { return {{b.w[element], b.w[element], b.w[element], b.w[element]}}; }
QILIN_INLINE Vec vspltisw(int32_t value) {
    const uint32_t w = static_cast<uint32_t>(value);
    return {{w, w, w, w}};
}

// ---- integer ----
QILIN_INLINE Vec vadduwm(const Vec& a, const Vec& b) { return {{a.w[0] + b.w[0], a.w[1] + b.w[1], a.w[2] + b.w[2], a.w[3] + b.w[3]}}; }
QILIN_INLINE Vec vcmpequw(const Vec& a, const Vec& b) {
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = a.w[i] == b.w[i] ? 0xffffffffu : 0u;
    return r;
}
// CR6 after a recording compare: bit 0 if every element is true, bit 2 if none is.
QILIN_INLINE uint32_t cr6(const Vec& mask) {
    const uint32_t all = mask.w[0] & mask.w[1] & mask.w[2] & mask.w[3], any = mask.w[0] | mask.w[1] | mask.w[2] | mask.w[3];
    return (all == 0xffffffffu ? 8u : 0u) | (any == 0 ? 2u : 0u);
}

// ---- single-precision elements ----
inline constexpr uint32_t kQuiet32 = 0x00400000u, kDefaultNan32 = 0x7fc00000u;
QILIN_INLINE bool nan32(uint32_t w) { return (w & 0x7fffffffu) > 0x7f800000u; }
QILIN_INLINE bool denormal32(uint32_t w) { return (w & 0x7f800000u) == 0 && (w & 0x007fffffu) != 0; }
// An operand: a denormal is zero in non-Java mode.
QILIN_INLINE uint32_t operand(uint32_t w) { return kNonJava && denormal32(w) ? w & 0x80000000u : w; }
QILIN_INLINE double value(uint32_t w) { return static_cast<double>(std::bit_cast<float>(w)); }
// A result that is a single held in a double (not a NaN), to its bits; a denormal becomes zero in non-Java mode.
QILIN_INLINE uint32_t result(double r) {
    const uint32_t w = std::bit_cast<uint32_t>(static_cast<float>(r));
    return kNonJava && denormal32(w) ? w & 0x80000000u : w;
}

// a + b, or a - b with `subtract`.
QILIN_INLINE uint32_t add32(uint32_t a, uint32_t b, bool subtract) {
    if (nan32(a)) return a | kQuiet32;
    if (nan32(b)) return b | kQuiet32;
    const float x = std::bit_cast<float>(operand(a)), y = std::bit_cast<float>(operand(b));
    const float r = subtract ? x - y : x + y;
    return r == r ? result(r) : kDefaultNan32;  // infinity less infinity
}
// a * c + b, or a * c - b, fused; the negative of it with `negate`.
// The general case: NaNs and infinities among the operands, and sums whose rounding needs the two-sum.
QILIN_FP_SLOW uint32_t fused32_slow(uint32_t a, uint32_t c, uint32_t b, bool subtract, bool negate) {
    if (nan32(a)) return a | kQuiet32;
    if (nan32(b)) return b | kQuiet32;
    if (nan32(c)) return c | kQuiet32;
    const double product = value(operand(a)) * value(operand(c));  // exact: 24 bits by 24
    if (product != product) return kDefaultNan32;                  // infinity times zero
    const double addend = value(operand(b));
    const double r = fp::add_to_single(product, subtract ? -addend : addend);
    if (r != r) return kDefaultNan32;
    return result(negate ? -r : r);
}
// The usual case first: finite operands (a denormal is a zero) and a sum that rounds once to a normal single or
// zero (fp::rounds_once_to_single), so no denormal result to flush either.
QILIN_INLINE uint32_t fused32(uint32_t a, uint32_t c, uint32_t b, bool subtract, bool negate) {
    if (((a & 0x7f800000u) != 0x7f800000u) & ((b & 0x7f800000u) != 0x7f800000u) & ((c & 0x7f800000u) != 0x7f800000u)) {
        const double product = value(operand(a)) * value(operand(c));  // exact: 24 bits by 24
        const double addend = value(operand(b));
        const double r = product + (subtract ? -addend : addend);
        if (fp::rounds_once_to_single(r)) {
            const uint32_t w = std::bit_cast<uint32_t>(static_cast<float>(r));
            return negate ? w ^ 0x80000000u : w;
        }
    }
    return fused32_slow(a, c, b, subtract, negate);
}
// A normal single or a zero: what the whole-vector fast paths below take, with no NaN, infinity or denormal to mind.
// (Written with & and | on whole numbers, not && and ||: a compiler then tests four elements without a branch each.)
QILIN_INLINE unsigned plain32(uint32_t w) { return static_cast<unsigned>(((w >> 23) & 0xffu) - 1u < 254u) | static_cast<unsigned>((w << 1) == 0); }
// The four elements by the general path, out of line: what the fast path below leaves.
QILIN_FP_SLOW Vec fused_vec_slow(const Vec& a, const Vec& c, const Vec& b, bool subtract, bool negate) {
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = fused32(a.w[i], c.w[i], b.w[i], subtract, negate);
    return r;
}
// a * c + b (or - b, negated with `negate`), all four elements: with plain operands, each sum in a double from the
// exact product, and rounded once to single where fp::rounds_once_to_single says it may be; else element by element,
// out of line (so that the inline code in a long lifted function stays small).
QILIN_INLINE Vec fused_vec(const Vec& a, const Vec& c, const Vec& b, bool subtract, bool negate) {
    unsigned plain = 1;
    for (unsigned i = 0; i < 4; ++i) plain &= plain32(a.w[i]) & plain32(b.w[i]) & plain32(c.w[i]);
    if (plain) {
        Vec r;
        unsigned fine = 1;
        for (unsigned i = 0; i < 4; ++i) {
            const double addend = value(b.w[i]);
            const double sum = value(a.w[i]) * value(c.w[i]) + (subtract ? -addend : addend);
            fine &= fp::single_once(sum);
            r.w[i] = std::bit_cast<uint32_t>(static_cast<float>(sum)) ^ (negate ? 0x80000000u : 0u);
        }
        if (fine) return r;
    }
    return fused_vec_slow(a, c, b, subtract, negate);
}
namespace exact {
// (Element by element: a whole-vector fast path, as fused_vec has, measured slower with MSVC and no faster with Clang.)
QILIN_INLINE Vec vaddfp(const Vec& a, const Vec& b) {
    return {{add32(a.w[0], b.w[0], false), add32(a.w[1], b.w[1], false), add32(a.w[2], b.w[2], false), add32(a.w[3], b.w[3], false)}};
}
QILIN_INLINE Vec vsubfp(const Vec& a, const Vec& b) {
    return {{add32(a.w[0], b.w[0], true), add32(a.w[1], b.w[1], true), add32(a.w[2], b.w[2], true), add32(a.w[3], b.w[3], true)}};
}
// vD = vA * vC + vB
QILIN_INLINE Vec vmaddfp(const Vec& a, const Vec& c, const Vec& b) { return fused_vec(a, c, b, false, false); }
// vD = -(vA * vC - vB)
QILIN_INLINE Vec vnmsubfp(const Vec& a, const Vec& c, const Vec& b) { return fused_vec(a, c, b, true, true); }
}  // namespace exact
// ---- the estimates (see the top of the file) ----
// 1/x. A double holds more than twice a single's digits and two more, so its quotient rounds to the single's correctly.
QILIN_INLINE uint32_t reciprocal32(uint32_t w) {
    if (nan32(w)) return w | kQuiet32;
    w = operand(w);
    const uint32_t sign = w & 0x80000000u, magnitude = w & 0x7fffffffu;
    if (magnitude == 0) return sign | 0x7f800000u;
    if (magnitude == 0x7f800000u) return sign;
    return result(1.0 / value(w));
}
// How m * m * x compares with 2^power, exactly: -1 less, 0 equal, 1 greater. (m below 2^26, x below 2^24.)
QILIN_INLINE int compare_square(uint64_t m, uint32_t x, int power) {
    const uint64_t square = m * m;  // below 2^52
    const uint64_t low = (square & 0xffffffffu) * x, high = (square >> 32) * x + (low >> 32);  // the product is high * 2^32 + low's bottom half
    const uint32_t bottom = static_cast<uint32_t>(low);
    if (power >= 96) return -1;
    if (power < 0) return 1;
    if (power >= 32) {
        const uint64_t limit = uint64_t{1} << (power - 32);
        return high < limit ? -1 : high > limit || bottom != 0 ? 1 : 0;
    }
    const uint32_t limit = uint32_t{1} << power;
    return high != 0 || bottom > limit ? 1 : bottom < limit ? -1 : 0;
}
// 1/sqrt(x): the double's value is within a hair of it, and the two singles' midpoints either side settle the rounding exactly.
inline uint32_t reciprocal_root32(uint32_t w) {
    if (nan32(w)) return w | kQuiet32;
    w = operand(w);
    if ((w & 0x7fffffffu) == 0) return (w & 0x80000000u) | 0x7f800000u;
    if (w >> 31) return kDefaultNan32;
    if (w == 0x7f800000u) return 0;
    uint32_t f = std::bit_cast<uint32_t>(static_cast<float>(1.0 / std::sqrt(value(w))));
    const uint32_t x = (w & 0x007fffffu) | 0x00800000u;  // x = this * 2^ex
    const int ex = static_cast<int>(w >> 23) - 150;
    const uint32_t F = (f & 0x007fffffu) | 0x00800000u;  // f = F * 2^q
    const int q = static_cast<int>(f >> 23) - 150;
    // The true value is above a midpoint M * 2^g just when M * M * x is below 2^-(2g + ex).
    if (compare_square(2ull * F + 1, x, -(2 * (q - 1) + ex)) < 0) return f + 1;                    // above the midpoint over f
    if (F > 0x00800000u ? compare_square(2ull * F - 1, x, -(2 * (q - 1) + ex)) > 0                 // below the midpoint under f
                        : compare_square(4ull * F - 1, x, -(2 * (q - 2) + ex)) > 0)                // (closer, under a power of two)
        return f - 1;
    return f;
}
namespace exact {
QILIN_INLINE Vec vrefp(const Vec& b) { return {{reciprocal32(b.w[0]), reciprocal32(b.w[1]), reciprocal32(b.w[2]), reciprocal32(b.w[3])}}; }
QILIN_INLINE Vec vrsqrtefp(const Vec& b) {
    return {{reciprocal_root32(b.w[0]), reciprocal_root32(b.w[1]), reciprocal_root32(b.w[2]), reciprocal_root32(b.w[3])}};
}
}  // namespace exact

// The larger, or the smaller: of zeros of both signs, +0 is the larger.
QILIN_INLINE uint32_t extreme32(uint32_t a, uint32_t b, bool larger) {
    if (nan32(a)) return a | kQuiet32;
    if (nan32(b)) return b | kQuiet32;
    a = operand(a);
    b = operand(b);
    const float x = std::bit_cast<float>(a), y = std::bit_cast<float>(b);
    if (x == y) return larger ? a & b : a | b;  // equal: the same bits, or zeros whose signs differ
    return (x > y) == larger ? a : b;
}
namespace exact {
QILIN_INLINE Vec vmaxfp(const Vec& a, const Vec& b) {
    return {{extreme32(a.w[0], b.w[0], true), extreme32(a.w[1], b.w[1], true), extreme32(a.w[2], b.w[2], true), extreme32(a.w[3], b.w[3], true)}};
}
QILIN_INLINE Vec vminfp(const Vec& a, const Vec& b) {
    return {{extreme32(a.w[0], b.w[0], false), extreme32(a.w[1], b.w[1], false), extreme32(a.w[2], b.w[2], false), extreme32(a.w[3], b.w[3], false)}};
}
// Equal, element by element: a NaN equals nothing, zeros of both signs are equal, a denormal counts as zero in non-Java mode.
QILIN_INLINE Vec vcmpeqfp(const Vec& a, const Vec& b) {
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = std::bit_cast<float>(operand(a.w[i])) == std::bit_cast<float>(operand(b.w[i])) ? 0xffffffffu : 0u;
    return r;
}
}  // namespace exact
// Signed integers to singles, divided by 2^scale: one rounding, of the exact quotient. (Both modes.)
QILIN_INLINE Vec vcfsx(const Vec& b, unsigned scale) {
    const double factor = 1.0 / static_cast<double>(uint64_t{1} << scale);
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = std::bit_cast<uint32_t>(static_cast<float>(static_cast<double>(static_cast<int32_t>(b.w[i])) * factor));
    return r;
}
namespace exact {
// Singles times 2^scale to signed integers, toward zero, saturating; a NaN gives 0.
QILIN_INLINE Vec vctsxs(const Vec& b, unsigned scale) {
    const double factor = static_cast<double>(uint64_t{1} << scale);
    Vec r;
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t w = operand(b.w[i]);
        const double v = value(w) * factor;  // exact
        r.w[i] = nan32(w) ? 0u : v >= 2147483648.0 ? 0x7fffffffu : v <= -2147483648.0 ? 0x80000000u : static_cast<uint32_t>(static_cast<int32_t>(v));
    }
    return r;
}
}  // namespace exact

// ---- the relaxed mode (see the top) ----
// The portable twin: plain C++ on the four elements, what the host's vector code below must give too.
namespace portable {
QILIN_INLINE float f(uint32_t w) { return std::bit_cast<float>(w); }
QILIN_INLINE uint32_t u(float x) { return std::bit_cast<uint32_t>(x); }
QILIN_INLINE Vec vaddfp(const Vec& a, const Vec& b) {
    return {{u(f(a.w[0]) + f(b.w[0])), u(f(a.w[1]) + f(b.w[1])), u(f(a.w[2]) + f(b.w[2])), u(f(a.w[3]) + f(b.w[3]))}};
}
QILIN_INLINE Vec vsubfp(const Vec& a, const Vec& b) {
    return {{u(f(a.w[0]) - f(b.w[0])), u(f(a.w[1]) - f(b.w[1])), u(f(a.w[2]) - f(b.w[2])), u(f(a.w[3]) - f(b.w[3]))}};
}
// The exact product in a double, the sum rounded to double and then to single.
QILIN_INLINE uint32_t madd(uint32_t a, uint32_t c, uint32_t b) { return u(static_cast<float>(value(a) * value(c) + value(b))); }
QILIN_INLINE uint32_t nmsub(uint32_t a, uint32_t c, uint32_t b) { return u(static_cast<float>(value(a) * value(c) - value(b))) ^ 0x80000000u; }
QILIN_INLINE Vec vmaddfp(const Vec& a, const Vec& c, const Vec& b) {
    return {{madd(a.w[0], c.w[0], b.w[0]), madd(a.w[1], c.w[1], b.w[1]), madd(a.w[2], c.w[2], b.w[2]), madd(a.w[3], c.w[3], b.w[3])}};
}
QILIN_INLINE Vec vnmsubfp(const Vec& a, const Vec& c, const Vec& b) {
    return {{nmsub(a.w[0], c.w[0], b.w[0]), nmsub(a.w[1], c.w[1], b.w[1]), nmsub(a.w[2], c.w[2], b.w[2]), nmsub(a.w[3], c.w[3], b.w[3])}};
}
QILIN_INLINE Vec vrefp(const Vec& b) { return {{u(1.0f / f(b.w[0])), u(1.0f / f(b.w[1])), u(1.0f / f(b.w[2])), u(1.0f / f(b.w[3]))}}; }
QILIN_INLINE Vec vrsqrtefp(const Vec& b) {
    return {{u(1.0f / std::sqrt(f(b.w[0]))), u(1.0f / std::sqrt(f(b.w[1]))), u(1.0f / std::sqrt(f(b.w[2]))), u(1.0f / std::sqrt(f(b.w[3])))}};
}
// a where a > b, else b (so b for a NaN, and for zeros of either sign): what SSE's maxps does.
QILIN_INLINE Vec vmaxfp(const Vec& a, const Vec& b) {
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = f(a.w[i]) > f(b.w[i]) ? a.w[i] : b.w[i];
    return r;
}
QILIN_INLINE Vec vminfp(const Vec& a, const Vec& b) {
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = f(a.w[i]) < f(b.w[i]) ? a.w[i] : b.w[i];
    return r;
}
QILIN_INLINE Vec vcmpeqfp(const Vec& a, const Vec& b) {
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = f(a.w[i]) == f(b.w[i]) ? 0xffffffffu : 0u;
    return r;
}
// Saturating, a NaN 0, as in exact mode (the product by 2^scale is exact in a single, or past the range anyway).
QILIN_INLINE Vec vctsxs(const Vec& b, unsigned scale) {
    const float factor = static_cast<float>(uint32_t{1} << scale);
    Vec r;
    for (unsigned i = 0; i < 4; ++i) {
        const float v = f(b.w[i]) * factor;
        r.w[i] = v != v ? 0u : v >= 2147483648.0f ? 0x7fffffffu : v <= -2147483648.0f ? 0x80000000u : static_cast<uint32_t>(static_cast<int32_t>(v));
    }
    return r;
}
}  // namespace portable

// The host's vector unit, where it has one; else the portable twin.
namespace relaxed {
#if QILIN_VMX_SSE
QILIN_INLINE __m128 ps(const Vec& v) { return _mm_castsi128_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(v.w))); }
QILIN_INLINE Vec vec(__m128 x) {
    Vec r;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(r.w), _mm_castps_si128(x));
    return r;
}
QILIN_INLINE Vec vaddfp(const Vec& a, const Vec& b) { return vec(_mm_add_ps(ps(a), ps(b))); }
QILIN_INLINE Vec vsubfp(const Vec& a, const Vec& b) { return vec(_mm_sub_ps(ps(a), ps(b))); }
#if defined(__FMA__) || defined(__AVX2__)
QILIN_INLINE Vec vmaddfp(const Vec& a, const Vec& c, const Vec& b) { return vec(_mm_fmadd_ps(ps(a), ps(c), ps(b))); }
QILIN_INLINE Vec vnmsubfp(const Vec& a, const Vec& c, const Vec& b) { return vec(_mm_xor_ps(_mm_fmsub_ps(ps(a), ps(c), ps(b)), _mm_set1_ps(-0.0f))); }
#else
// a * c + b (or minus) in doubles, two elements at a time: the product is exact, the sum rounded twice.
QILIN_INLINE __m128 fused(__m128 a, __m128 c, __m128 b, bool subtract) {
    const __m128d lo = _mm_mul_pd(_mm_cvtps_pd(a), _mm_cvtps_pd(c)), hi = _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(a, a)), _mm_cvtps_pd(_mm_movehl_ps(c, c)));
    const __m128d blo = _mm_cvtps_pd(b), bhi = _mm_cvtps_pd(_mm_movehl_ps(b, b));
    const __m128d slo = subtract ? _mm_sub_pd(lo, blo) : _mm_add_pd(lo, blo), shi = subtract ? _mm_sub_pd(hi, bhi) : _mm_add_pd(hi, bhi);
    return _mm_movelh_ps(_mm_cvtpd_ps(slo), _mm_cvtpd_ps(shi));
}
QILIN_INLINE Vec vmaddfp(const Vec& a, const Vec& c, const Vec& b) { return vec(fused(ps(a), ps(c), ps(b), false)); }
QILIN_INLINE Vec vnmsubfp(const Vec& a, const Vec& c, const Vec& b) { return vec(_mm_xor_ps(fused(ps(a), ps(c), ps(b), true), _mm_set1_ps(-0.0f))); }
#endif
QILIN_INLINE Vec vrefp(const Vec& b) { return vec(_mm_div_ps(_mm_set1_ps(1.0f), ps(b))); }
QILIN_INLINE Vec vrsqrtefp(const Vec& b) { return vec(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(ps(b)))); }
QILIN_INLINE Vec vmaxfp(const Vec& a, const Vec& b) { return vec(_mm_max_ps(ps(a), ps(b))); }
QILIN_INLINE Vec vminfp(const Vec& a, const Vec& b) { return vec(_mm_min_ps(ps(a), ps(b))); }
QILIN_INLINE Vec vcmpeqfp(const Vec& a, const Vec& b) { return vec(_mm_cmpeq_ps(ps(a), ps(b))); }
// cvttps2dq gives 0x80000000 for a NaN and past either end: the top end is turned into 0x7fffffff, a NaN into 0.
QILIN_INLINE Vec vctsxs(const Vec& b, unsigned scale) {
    const __m128 v = _mm_mul_ps(ps(b), _mm_set1_ps(static_cast<float>(uint32_t{1} << scale)));
    const __m128i w = _mm_xor_si128(_mm_cvttps_epi32(v), _mm_castps_si128(_mm_cmpge_ps(v, _mm_set1_ps(2147483648.0f))));
    return vec(_mm_and_ps(_mm_castsi128_ps(w), _mm_cmpord_ps(v, v)));
}
#elif QILIN_VMX_NEON
QILIN_INLINE float32x4_t ps(const Vec& v) { return vreinterpretq_f32_u32(vld1q_u32(v.w)); }
QILIN_INLINE Vec vec(float32x4_t x) {
    Vec r;
    vst1q_u32(r.w, vreinterpretq_u32_f32(x));
    return r;
}
QILIN_INLINE Vec vec(uint32x4_t x) {
    Vec r;
    vst1q_u32(r.w, x);
    return r;
}
QILIN_INLINE Vec vaddfp(const Vec& a, const Vec& b) { return vec(vaddq_f32(ps(a), ps(b))); }
QILIN_INLINE Vec vsubfp(const Vec& a, const Vec& b) { return vec(vsubq_f32(ps(a), ps(b))); }
// The host's fused single multiply-add: one rounding, as the guest's.
QILIN_INLINE Vec vmaddfp(const Vec& a, const Vec& c, const Vec& b) { return vec(vfmaq_f32(ps(b), ps(a), ps(c))); }
QILIN_INLINE Vec vnmsubfp(const Vec& a, const Vec& c, const Vec& b) { return vec(vnegq_f32(vfmaq_f32(vnegq_f32(ps(b)), ps(a), ps(c)))); }
QILIN_INLINE Vec vrefp(const Vec& b) { return vec(vdivq_f32(vdupq_n_f32(1.0f), ps(b))); }
QILIN_INLINE Vec vrsqrtefp(const Vec& b) { return vec(vdivq_f32(vdupq_n_f32(1.0f), vsqrtq_f32(ps(b)))); }
QILIN_INLINE Vec vmaxfp(const Vec& a, const Vec& b) { return vec(vbslq_f32(vcgtq_f32(ps(a), ps(b)), ps(a), ps(b))); }
QILIN_INLINE Vec vminfp(const Vec& a, const Vec& b) { return vec(vbslq_f32(vcltq_f32(ps(a), ps(b)), ps(a), ps(b))); }
QILIN_INLINE Vec vcmpeqfp(const Vec& a, const Vec& b) { return vec(vceqq_f32(ps(a), ps(b))); }
// fcvtzs saturates, and gives 0 for a NaN: the guest's rule.
QILIN_INLINE Vec vctsxs(const Vec& b, unsigned scale) {
    return vec(vreinterpretq_u32_s32(vcvtq_s32_f32(vmulq_f32(ps(b), vdupq_n_f32(static_cast<float>(uint32_t{1} << scale))))));
}
#else
using portable::vaddfp, portable::vsubfp, portable::vmaddfp, portable::vnmsubfp, portable::vrefp, portable::vrsqrtefp, portable::vmaxfp, portable::vminfp,
    portable::vcmpeqfp, portable::vctsxs;
#endif
}  // namespace relaxed

#if QILIN_FP_EXACT
using namespace exact;
#else
using namespace relaxed;
#endif

}  // namespace qilin::rt::vmx

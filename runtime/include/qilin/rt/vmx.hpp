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
// write the VSCR are not in the lifter's slice. Nor are vrefp and vrsqrtefp:
// they are estimates, and their exact values are the hardware's.
//
// Included by cpu.hpp. Runtime code: no std::format here.
#pragma once

namespace qilin::rt::vmx {

inline constexpr bool kNonJava = true;

// ---- bytes ----
QILIN_INLINE unsigned byte(const Vec& v, unsigned i) { return (v.w[i >> 2] >> (24 - 8 * (i & 3))) & 0xffu; }
QILIN_INLINE void set_byte(Vec& v, unsigned i, unsigned b) {
    const unsigned shift = 24 - 8 * (i & 3);
    v.w[i >> 2] = (v.w[i >> 2] & ~(0xffu << shift)) | ((b & 0xffu) << shift);
}

// ---- memory: lvx and stvx ignore the low four bits of the address ----
QILIN_INLINE Vec lvx(Cpu& c, uint32_t ea) {
    ea &= ~15u;
    return {{static_cast<uint32_t>(read32(c, ea)), static_cast<uint32_t>(read32(c, ea + 4)), static_cast<uint32_t>(read32(c, ea + 8)),
             static_cast<uint32_t>(read32(c, ea + 12))}};
}
QILIN_INLINE void stvx(Cpu& c, uint32_t ea, const Vec& v) {
    ea &= ~15u;
    for (unsigned i = 0; i < 4; ++i) write32(c, ea + 4 * i, v.w[i]);
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
    Vec r{};
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned n = byte(c, i) & 31;
        set_byte(r, i, n < 16 ? byte(a, n) : byte(b, n - 16));
    }
    return r;
}
// Sixteen bytes of a and b end to end, from byte `shift`.
inline Vec vsldoi(const Vec& a, const Vec& b, unsigned shift) {
    Vec r{};
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned n = i + shift;
        set_byte(r, i, n < 16 ? byte(a, n) : byte(b, n - 16));
    }
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
QILIN_INLINE uint32_t fused32(uint32_t a, uint32_t c, uint32_t b, bool subtract, bool negate) {
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
QILIN_INLINE Vec vaddfp(const Vec& a, const Vec& b) {
    return {{add32(a.w[0], b.w[0], false), add32(a.w[1], b.w[1], false), add32(a.w[2], b.w[2], false), add32(a.w[3], b.w[3], false)}};
}
QILIN_INLINE Vec vsubfp(const Vec& a, const Vec& b) {
    return {{add32(a.w[0], b.w[0], true), add32(a.w[1], b.w[1], true), add32(a.w[2], b.w[2], true), add32(a.w[3], b.w[3], true)}};
}
// vD = vA * vC + vB
QILIN_INLINE Vec vmaddfp(const Vec& a, const Vec& c, const Vec& b) {
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = fused32(a.w[i], c.w[i], b.w[i], false, false);
    return r;
}
// vD = -(vA * vC - vB)
QILIN_INLINE Vec vnmsubfp(const Vec& a, const Vec& c, const Vec& b) {
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = fused32(a.w[i], c.w[i], b.w[i], true, true);
    return r;
}
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
// Signed integers to singles, divided by 2^scale: one rounding, of the exact quotient.
QILIN_INLINE Vec vcfsx(const Vec& b, unsigned scale) {
    const double factor = 1.0 / static_cast<double>(uint64_t{1} << scale);
    Vec r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = std::bit_cast<uint32_t>(static_cast<float>(static_cast<double>(static_cast<int32_t>(b.w[i])) * factor));
    return r;
}
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

}  // namespace qilin::rt::vmx

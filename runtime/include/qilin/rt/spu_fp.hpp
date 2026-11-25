// The SPU's floating point, for lifted SPU code. Written from IBM's "SPU
// Instruction Set Architecture" (version 1.2), section 9.
//
// **Single precision is not IEEE's.** It is the "extended-range mode":
//   - a word with a zero exponent is zero, whatever its sign and fraction
//     (there are no denormals);
//   - every other word is an ordinary number, the largest exponent too
//     (there are no infinities and no NaNs);
//   - results are truncated (rounded toward zero), the only mode there is;
//   - a result too large becomes the largest number of its sign
//     (0x7FFFFFFF or 0xFFFFFFFF); one too small becomes zero; and a zero
//     result is always +0.
// So it is done here in whole numbers: a product is exact in 48 bits, a sum
// is aligned with room to spare, and what is cut off is cut once.
//
// **The estimates** (frest, frsqest) give a base and a step for `fi` to
// interpolate with. `fi` is defined exactly by the document and is exact
// here. The base and step themselves are an implementation's own (the
// document gives their form and how good the result must be, not their
// values), and a console's are not known here. **The stated choice**: 32
// straight pieces to the binade for 1/x, and 64 (32 for each parity of the
// exponent) for 1/sqrt(x), each the line through the piece's ends lowered
// by half its error at the middle, rounded to the 13 and 10 bits the form
// has. It can differ from a console's result in the low bits; the test
// checks it against the document's own bound on the result after its
// Newton-Raphson step. *A console's tables, if they can be had, replace it.*
//
// **Double precision** is IEEE's, with two stated choices the document
// leaves to the implementation: a denormal operand is taken as zero (its
// sign kept), and a NaN result is always the default one. Rounding is to
// nearest: the status and control register is not kept, so the
// instructions that read and write it, and the fused double forms and
// double compares (which the game doesn't use), are outside the lifter's
// slice.
//
// The helpers here are left to the compiler to inline or not: forced into every one of the game's 13,000 uses,
// they made the lifted SPU code take over twenty minutes to compile. (What that costs at run time is to be measured.)
//
// **The host's float unit, where it can stand in (x86-64).** The SPU's single precision is IEEE's with truncation, no
// denormals, +0 for every zero and no infinities or NaNs: a host float unit set to round toward zero, with denormals
// flushed to zero in and out, gives an add, subtract or multiply's result bit for bit, and a fused multiply-add's
// too, done in double precision (the product of two singles is exact there, and truncating twice, to 53 bits and
// then to 24, is truncating once). What it gives otherwise is put right: a zero is made +0, and a lane with an operand
// whose exponent is 255 (a number to the SPU, infinity or NaN to the host) or a result whose exponent is 254 or more
// (where the SPU's range goes on and the host's saturates) is done again the exact way below. The mode is the SPU
// thread's own (enter_single_mode: the runtime enters it as each SPU thread starts); without it, as in the
// differential test, everything is done the exact way. The double-precision instructions and the estimates' tables
// want rounding to nearest: they have it, for their own few instructions (NearestMode).
//
// Included by spu.hpp's users after it. Runtime code: no std::format here.
#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define QILIN_SPU_HOST_SINGLE 1
#else
#define QILIN_SPU_HOST_SINGLE 0
#endif

#include "qilin/rt/spu.hpp"

namespace qilin::rt::spu {

// ---- the host's float unit for single precision (see the top) ----
inline thread_local bool t_single_mode = false;

// The calling thread's float unit set for SPU single precision (round toward zero, denormals zero in and out).
inline void enter_single_mode() {
#if QILIN_SPU_HOST_SINGLE
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8000u | 0x0040u);
    t_single_mode = true;
#endif
}

// For its own lifetime, the float unit as it is by default (round to nearest, denormals kept), if this thread had it
// set for single precision.
struct NearestMode {
#if QILIN_SPU_HOST_SINGLE
    unsigned saved = 0;
    bool set = false;
    NearestMode() {
        if (t_single_mode) saved = _mm_getcsr(), set = true, _mm_setcsr(saved & ~(0x6000u | 0x8000u | 0x0040u));
    }
    ~NearestMode() {
        if (set) _mm_setcsr(saved);
    }
#else
    NearestMode() {}  // (no mode to change: the exact way is the only one)
#endif
    NearestMode(const NearestMode&) = delete;
    NearestMode& operator=(const NearestMode&) = delete;
};

// ---- single precision, extended range ----
// A number that isn't zero is mantissa * 2^exponent, with its sign apart.
struct Single {
    bool zero = true, negative = false;
    uint32_t mantissa = 0;  // 2^23 to 2^24 - 1
    int exponent = 0;
};
inline Single unpack(uint32_t w) {
    const uint32_t e = (w >> 23) & 0xffu;
    if (e == 0) return {};
    return {false, (w >> 31) != 0, (w & 0x007fffffu) | 0x00800000u, static_cast<int>(e) - 150};
}
// The single nearest zero-wards to magnitude * 2^exponent: saturated if too large, +0 if too small or nothing.
inline uint32_t pack(bool negative, uint64_t magnitude, int exponent) {
    if (magnitude == 0) return 0;
    const int top = 63 - std::countl_zero(magnitude);
    if (top > 23) magnitude >>= top - 23;
    else magnitude <<= 23 - top;
    const int biased = exponent + top + 127;
    if (biased > 255) return negative ? 0xffffffffu : 0x7fffffffu;
    if (biased < 1) return 0;
    return (negative ? 0x80000000u : 0u) | (static_cast<uint32_t>(biased) << 23) | (static_cast<uint32_t>(magnitude) & 0x007fffffu);
}
// x + y, where each is a magnitude below 2^62 with at least 14 zero bits at its low end, times a power of two.
inline uint32_t sum(bool x_negative, uint64_t x, int x_exponent, bool y_negative, uint64_t y, int y_exponent) {
    if (x_exponent < y_exponent) {
        std::swap(x_negative, y_negative);
        std::swap(x, y);
        std::swap(x_exponent, y_exponent);
    }
    // y to x's scale: what falls off its end is remembered, not kept.
    const int down = x_exponent - y_exponent;
    bool lost = false;
    if (down >= 64) lost = y != 0, y = 0;
    else if (down > 0) lost = (y & ((uint64_t{1} << down) - 1)) != 0, y >>= down;
    if (x_negative == y_negative) return pack(x_negative, x + y, x_exponent);
    if (x > y) return pack(x_negative, x - y - (lost ? 1 : 0), x_exponent);  // (a little less than x - y: the next whole number down)
    return pack(y_negative, y - x, x_exponent);                              // (nothing was lost when y is the larger: its low bits were zeros)
}
inline uint32_t add32(uint32_t a, uint32_t b, bool subtract) {
    const Single x = unpack(a), y = unpack(b);
    if (x.zero && y.zero) return 0;
    if (y.zero) return a;
    if (x.zero) return subtract ? b ^ 0x80000000u : b;
    return sum(x.negative, uint64_t{x.mantissa} << 38, x.exponent - 38, y.negative != subtract, uint64_t{y.mantissa} << 38, y.exponent - 38);
}
inline uint32_t multiply32(uint32_t a, uint32_t b) {
    const Single x = unpack(a), y = unpack(b);
    if (x.zero || y.zero) return 0;
    return pack(x.negative != y.negative, uint64_t{x.mantissa} * y.mantissa, x.exponent + y.exponent);
}
// a * b + c, the product exact; with `negate_product` c - a * b, with `negate_addend` a * b - c.
inline uint32_t fused32(uint32_t a, uint32_t b, uint32_t c, bool negate_product, bool negate_addend) {
    const Single x = unpack(a), y = unpack(b), z = unpack(c);
    const bool product_zero = x.zero || y.zero, product_negative = (x.negative != y.negative) != negate_product;
    if (product_zero) return z.zero ? 0u : (negate_addend ? c ^ 0x80000000u : c);
    const uint64_t product = uint64_t{x.mantissa} * y.mantissa;  // 47 or 48 bits
    if (z.zero) return pack(product_negative, product, x.exponent + y.exponent);
    return sum(product_negative, product << 14, x.exponent + y.exponent - 14, z.negative != negate_addend, uint64_t{z.mantissa} << 38, z.exponent - 38);
}
#if QILIN_SPU_HOST_SINGLE
// The host's result, put right: zeros made +0, and the lanes the host can't stand in for (an operand's exponent 255, the
// result's 254 or more) done again by `exact(lane)`.
inline __m128i exponents(__m128i v) { return _mm_and_si128(_mm_srli_epi32(v, 23), _mm_set1_epi32(0xff)); }
template <typename Exact>
inline SpuReg host_result(__m128 result, __m128i bad_operands, Exact exact) {
    __m128i r = _mm_castps_si128(result);
    const __m128i e = exponents(r);
    const __m128i bad = _mm_or_si128(bad_operands, _mm_cmpgt_epi32(e, _mm_set1_epi32(253)));
    r = _mm_andnot_si128(_mm_cmpeq_epi32(e, _mm_setzero_si128()), r);
    SpuReg out;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out.w), r);
    if (const int lanes = _mm_movemask_ps(_mm_castsi128_ps(bad)))
        for (unsigned i = 0; i < 4; ++i)
            if (lanes & (1 << i)) out.w[i] = exact(i);
    return out;
}
inline __m128i load(const SpuReg& v) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(v.w)); }
inline __m128i top_exponent(__m128i v) { return _mm_cmpeq_epi32(exponents(v), _mm_set1_epi32(0xff)); }
// a * b + c (with `negate_product` c - a * b, with `negate_addend` a * b - c), in double precision.
inline __m128 fused_host(__m128i a, __m128i b, __m128i c, bool negate_product, bool negate_addend) {
    const __m128 x = _mm_castsi128_ps(a), y = _mm_castsi128_ps(b), z = _mm_castsi128_ps(c);
    const auto half = [&](__m128d px, __m128d py, __m128d pz) {
        __m128d product = _mm_mul_pd(px, py);
        if (negate_product) product = _mm_sub_pd(_mm_setzero_pd(), product);
        return negate_addend ? _mm_sub_pd(product, pz) : _mm_add_pd(product, pz);
    };
    const __m128d low = half(_mm_cvtps_pd(x), _mm_cvtps_pd(y), _mm_cvtps_pd(z));
    const __m128d high = half(_mm_cvtps_pd(_mm_movehl_ps(x, x)), _mm_cvtps_pd(_mm_movehl_ps(y, y)), _mm_cvtps_pd(_mm_movehl_ps(z, z)));
    return _mm_movelh_ps(_mm_cvtpd_ps(low), _mm_cvtpd_ps(high));
}
#endif

inline SpuReg fa(const SpuReg& a, const SpuReg& b) {
#if QILIN_SPU_HOST_SINGLE
    if (t_single_mode) {
        const __m128i x = load(a), y = load(b);
        return host_result(_mm_add_ps(_mm_castsi128_ps(x), _mm_castsi128_ps(y)), _mm_or_si128(top_exponent(x), top_exponent(y)),
                           [&](unsigned i) { return add32(a.w[i], b.w[i], false); });
    }
#endif
    return words(a, b, [](uint32_t p, uint32_t q) { return add32(p, q, false); });
}
inline SpuReg fs(const SpuReg& a, const SpuReg& b) {
#if QILIN_SPU_HOST_SINGLE
    if (t_single_mode) {
        const __m128i x = load(a), y = load(b);
        return host_result(_mm_sub_ps(_mm_castsi128_ps(x), _mm_castsi128_ps(y)), _mm_or_si128(top_exponent(x), top_exponent(y)),
                           [&](unsigned i) { return add32(a.w[i], b.w[i], true); });
    }
#endif
    return words(a, b, [](uint32_t p, uint32_t q) { return add32(p, q, true); });
}
inline SpuReg fm(const SpuReg& a, const SpuReg& b) {
#if QILIN_SPU_HOST_SINGLE
    if (t_single_mode) {
        const __m128i x = load(a), y = load(b);
        return host_result(_mm_mul_ps(_mm_castsi128_ps(x), _mm_castsi128_ps(y)), _mm_or_si128(top_exponent(x), top_exponent(y)),
                           [&](unsigned i) { return multiply32(a.w[i], b.w[i]); });
    }
#endif
    return words(a, b, [](uint32_t p, uint32_t q) { return multiply32(p, q); });
}
inline SpuReg fused(const SpuReg& a, const SpuReg& b, const SpuReg& c, bool negate_product, bool negate_addend) {
#if QILIN_SPU_HOST_SINGLE
    if (t_single_mode) {
        const __m128i x = load(a), y = load(b), z = load(c);
        return host_result(fused_host(x, y, z, negate_product, negate_addend), _mm_or_si128(_mm_or_si128(top_exponent(x), top_exponent(y)), top_exponent(z)),
                           [&](unsigned i) { return fused32(a.w[i], b.w[i], c.w[i], negate_product, negate_addend); });
    }
#endif
    return {{fused32(a.w[0], b.w[0], c.w[0], negate_product, negate_addend), fused32(a.w[1], b.w[1], c.w[1], negate_product, negate_addend),
             fused32(a.w[2], b.w[2], c.w[2], negate_product, negate_addend), fused32(a.w[3], b.w[3], c.w[3], negate_product, negate_addend)}};
}
inline SpuReg fma(const SpuReg& a, const SpuReg& b, const SpuReg& c) { return fused(a, b, c, false, false); }
inline SpuReg fms(const SpuReg& a, const SpuReg& b, const SpuReg& c) { return fused(a, b, c, false, true); }
inline SpuReg fnms(const SpuReg& a, const SpuReg& b, const SpuReg& c) { return fused(a, b, c, true, false); }

// ---- compares: every word with a zero exponent is the one zero ----
inline int64_t ordered(uint32_t w) {
    if (((w >> 23) & 0xffu) == 0) return 0;
    const int64_t magnitude = w & 0x7fffffffu;
    return (w >> 31) ? -magnitude : magnitude;
}
inline SpuReg fceq(const SpuReg& a, const SpuReg& b) { return words(a, b, [](uint32_t p, uint32_t q) { return ordered(p) == ordered(q) ? 0xffffffffu : 0u; }); }
inline SpuReg fcgt(const SpuReg& a, const SpuReg& b) { return words(a, b, [](uint32_t p, uint32_t q) { return ordered(p) > ordered(q) ? 0xffffffffu : 0u; }); }
inline SpuReg fcmeq(const SpuReg& a, const SpuReg& b) {
    return words(a, b, [](uint32_t p, uint32_t q) { return ordered(p & 0x7fffffffu) == ordered(q & 0x7fffffffu) ? 0xffffffffu : 0u; });
}
inline SpuReg fcmgt(const SpuReg& a, const SpuReg& b) {
    return words(a, b, [](uint32_t p, uint32_t q) { return ordered(p & 0x7fffffffu) > ordered(q & 0x7fffffffu) ? 0xffffffffu : 0u; });
}

// ---- conversions with whole numbers: `scale` is the number of bits after the binary point (0 to 127) ----
inline SpuReg csflt(const SpuReg& a, int scale) {
    return words(a, [scale](uint32_t w) {
        const bool negative = (w >> 31) != 0;
        return pack(negative, negative ? uint64_t{0u - w} : uint64_t{w}, -scale);
    });
}
inline SpuReg cuflt(const SpuReg& a, int scale) { return words(a, [scale](uint32_t w) { return pack(false, w, -scale); }); }
// The whole part of |x| * 2^scale, or 2^32 if it is that or more.
inline uint64_t whole(const Single& x, int scale) {
    const int up = x.exponent + scale;
    if (up > 8) return uint64_t{1} << 32;
    if (up >= 0) return uint64_t{x.mantissa} << up;
    return up <= -32 ? 0 : uint64_t{x.mantissa} >> -up;
}
inline SpuReg cflts(const SpuReg& a, int scale) {
    return words(a, [scale](uint32_t w) -> uint32_t {
        const Single x = unpack(w);
        if (x.zero) return 0;
        const uint64_t magnitude = whole(x, scale);
        if (x.negative) return magnitude >= 0x80000000u ? 0x80000000u : 0u - static_cast<uint32_t>(magnitude);
        return magnitude > 0x7fffffffu ? 0x7fffffffu : static_cast<uint32_t>(magnitude);
    });
}
inline SpuReg cfltu(const SpuReg& a, int scale) {
    return words(a, [scale](uint32_t w) -> uint32_t {
        const Single x = unpack(w);
        if (x.zero || x.negative) return 0;
        const uint64_t magnitude = whole(x, scale);
        return magnitude > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(magnitude);
    });
}

// ---- the estimates, and the interpolation that uses them ----
// fi: b is a sign, an exponent, a base 1.BBBBBBBBBBBBB and a step 0.000SSSSSSSSSS; Y is a's low 19 bits as a
// fraction. The result is (base - step * Y) * 2^(exponent - 127), exactly, then cut to a single.
inline uint32_t interpolate32(uint32_t a, uint32_t b) {
    const uint64_t base = 0x2000u | ((b >> 10) & 0x1fffu), step = b & 0x3ffu, y = a & 0x7ffffu;
    return pack((b >> 31) != 0, (base << 19) - step * y, static_cast<int>((b >> 23) & 0xffu) - 127 - 32);
}
inline SpuReg fi(const SpuReg& a, const SpuReg& b) { return words(a, b, [](uint32_t p, uint32_t q) { return interpolate32(p, q); }); }

// The base and step (as the low 23 bits of a result) for each of 32 pieces of [1, 2), for a function that falls and
// curves upward there (2/m; 2/sqrt(m); sqrt(2)/sqrt(m)), as the top of the file says.
template <typename G>
inline std::array<uint32_t, 32> estimate_table(G g) {
    std::array<uint32_t, 32> table{};
    for (unsigned j = 0; j < 32; ++j) {
        const double from = 1.0 + j / 32.0, to = 1.0 + (j + 1) / 32.0, middle = 1.0 + (2 * j + 1) / 64.0;
        const double error = (g(from) + g(to)) / 2 - g(middle);  // how far the chord is above the curve at the middle
        const double step = (g(from) - g(to)) * 2;                 // the fall over a sixteenth: what Y measures
        // The base is the line's value where Y is 0: the start of the sixteenth this piece is in.
        const double base = g(from) + ((j & 1) ? step / 2 : 0.0) - error / 2;
        const uint32_t base_bits = static_cast<uint32_t>(std::floor((base - 1.0) * 8192.0 + 0.5)), step_bits = static_cast<uint32_t>(std::floor(step * 8192.0 + 0.5));
        table[j] = (base_bits > 0x1fffu ? 0x1fffu : base_bits) << 10 | (step_bits > 0x3ffu ? 0x3ffu : step_bits);
    }
    return table;
}
inline uint32_t reciprocal_estimate32(uint32_t w) {
    static const std::array<uint32_t, 32> table = [] {
        const NearestMode nearest;
        return estimate_table([](double m) { return 2.0 / m; });
    }();
    const uint32_t sign = w & 0x80000000u, exponent = (w >> 23) & 0xffu;
    if (exponent == 0) return sign | 0x7fffffffu;  // 1/0: the largest there is
    if (exponent >= 253) return 0;                 // too small to hold
    return sign | ((253 - exponent) << 23) | table[(w >> 18) & 31u];
}
inline uint32_t reciprocal_root_estimate32(uint32_t w) {
    static const std::array<uint32_t, 32> even = [] {
        const NearestMode nearest;
        return estimate_table([](double m) { return 2.0 / std::sqrt(m); });
    }();
    static const std::array<uint32_t, 32> odd = [] {
        const NearestMode nearest;
        return estimate_table([](double m) { return std::sqrt(2.0) / std::sqrt(m); });
    }();
    const uint32_t exponent = (w >> 23) & 0xffu;
    if (exponent == 0) return 0x7fffffffu;
    // x = m * 2^e. With e even, 1/sqrt(x) = (2/sqrt(m)) * 2^(-e/2 - 1); with e odd, (sqrt(2)/sqrt(m)) * 2^(-(e-1)/2 - 1).
    const int e = static_cast<int>(exponent) - 127;
    const bool is_odd = (e & 1) != 0;
    const int half = (is_odd ? e - 1 : e) / 2;
    return (static_cast<uint32_t>(126 - half) << 23) | (is_odd ? odd : even)[(w >> 18) & 31u];
}
inline SpuReg frest(const SpuReg& a) { return words(a, [](uint32_t w) { return reciprocal_estimate32(w); }); }
inline SpuReg frsqest(const SpuReg& a) { return words(a, [](uint32_t w) { return reciprocal_root_estimate32(w); }); }

// ---- double precision: two to a register ----
inline constexpr uint64_t kDefaultNan64 = 0x7ff8000000000000ull;
inline uint64_t pair(const SpuReg& v, unsigned i) { return (uint64_t{v.w[2 * i]} << 32) | v.w[2 * i + 1]; }
inline bool nan64(uint64_t d) { return (d & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }
// An operand: a denormal is zero, with its sign.
inline double operand64(uint64_t d) {
    if ((d & 0x7ff0000000000000ull) == 0) d &= 0x8000000000000000ull;
    return std::bit_cast<double>(d);
}
template <typename F>
inline SpuReg doubles(const SpuReg& a, const SpuReg& b, F f) {
    const NearestMode nearest;
    SpuReg r;
    for (unsigned i = 0; i < 2; ++i) {
        const uint64_t x = pair(a, i), y = pair(b, i);
        uint64_t bits = kDefaultNan64;
        if (!nan64(x) && !nan64(y)) {
            const double value = f(operand64(x), operand64(y));
            if (value == value) bits = std::bit_cast<uint64_t>(value);
        }
        r.w[2 * i] = static_cast<uint32_t>(bits >> 32);
        r.w[2 * i + 1] = static_cast<uint32_t>(bits);
    }
    return r;
}
inline SpuReg dfa(const SpuReg& a, const SpuReg& b) { return doubles(a, b, [](double x, double y) { return x + y; }); }
inline SpuReg dfs(const SpuReg& a, const SpuReg& b) { return doubles(a, b, [](double x, double y) { return x - y; }); }
inline SpuReg dfm(const SpuReg& a, const SpuReg& b) { return doubles(a, b, [](double x, double y) { return x * y; }); }
// fesd: the single in the left word of each pair, as a double. frds: each double as a single in the left word, zeros in the right.
inline SpuReg fesd(const SpuReg& a) {
    const NearestMode nearest;
    SpuReg r;
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t w = a.w[2 * i];
        uint64_t bits;
        if ((w & 0x7fffffffu) > 0x7f800000u) {
            bits = kDefaultNan64;
        } else {
            if ((w & 0x7f800000u) == 0) w &= 0x80000000u;
            bits = std::bit_cast<uint64_t>(static_cast<double>(std::bit_cast<float>(w)));
        }
        r.w[2 * i] = static_cast<uint32_t>(bits >> 32);
        r.w[2 * i + 1] = static_cast<uint32_t>(bits);
    }
    return r;
}
inline SpuReg frds(const SpuReg& a) {
    const NearestMode nearest;
    SpuReg r;
    for (unsigned i = 0; i < 2; ++i) {
        const uint64_t d = pair(a, i);
        r.w[2 * i] = nan64(d) ? 0x7fc00000u : std::bit_cast<uint32_t>(static_cast<float>(operand64(d)));
        r.w[2 * i + 1] = 0;
    }
    return r;
}

}  // namespace qilin::rt::spu

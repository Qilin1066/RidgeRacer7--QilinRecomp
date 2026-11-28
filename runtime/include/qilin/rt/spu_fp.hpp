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
// The exact ways here are left to the compiler to inline or not: forced into every one of the game's 13,000 uses,
// they made the lifted SPU code take over twenty minutes to compile. On x86-64 and ARM64 the add, subtract, multiply,
// fused forms and compares are, in the relaxed way, the host's float unit forced inline (a few vector instructions);
// the exact ways are out of line (exact_single, exact_compare).
//
// **The host's float unit, where it can stand in (x86-64, ARM64).** The SPU's single precision is IEEE's with
// truncation, no denormals, +0 for every zero and no infinities or NaNs: a host float unit set to round toward zero,
// with denormals flushed to zero in and out, gives an add, subtract or multiply's result bit for bit, and a fused
// multiply-add's too (ARM64's own, which rounds once; on x86-64, which may lack one, done in double precision: the
// product of two singles is exact there, and truncating twice, to 53 bits and then to 24, is truncating once). What it
// gives otherwise is put right: a zero is made +0, and a lane with an operand whose exponent is 255 (a number to the
// SPU, infinity or NaN to the host) or a result whose exponent is 254 or more (where the SPU's range goes on and the
// host's saturates) is done again the exact way below. The double-precision instructions and the estimates' tables want
// rounding to nearest: they have it, for their own few instructions (NearestMode).
//
// **Three ways, one for the whole process** (FloatMode, set by enter_single_mode, which the runtime calls as each SPU
// thread starts; read once per instruction from a plain global, not the thread's own storage):
//   - `whole`: everything in whole numbers, whatever the float unit is set to. Exact; the differential test's.
//   - `exact`: the host's float unit, put right as above. Exact too, and the same results bit for bit as `whole`
//     (the differential test checks both); for a thread whose float unit enter_single_mode set.
//   - `relaxed`, for play (the default, as the owner chose): the host's float unit as it is, only a zero result made
//     +0. Like a console's within the ordinary range (normal numbers whose results stay below 2^128), bit for bit for
//     an add, subtract or multiply and for ARM64's fused forms; x86-64's fused forms are a multiply and an add, each
//     truncated (within two units of the result's last place of the larger of the product and the addend, which the
//     test checks). Where it differs: an operand with exponent 255 is an infinity or a NaN to the host (to the SPU, a
//     number from 2^128 up), a result of 2^128 or more is the largest float, 0x7F7FFFFF, not the SPU's next binade or
//     its saturation at 0x7FFFFFFF, and a compare with such an operand is IEEE's. The compares are the host's float
//     compares (denormals are zero to them too). Conversions, estimates and double precision are exact in every mode.
// QILIN_SPU_FLOAT=whole|exact|relaxed in the environment chooses at run time; QILIN_SPU_FLOAT_DEFAULT (0 whole,
// 1 exact, 2 relaxed) at build time. Without a host float unit (QILIN_SPU_PORTABLE, other hosts) every way is `whole`.
//
// Included by spu.hpp's users after it. Runtime code: no std::format here.
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "qilin/rt/spu.hpp"

#if QILIN_SPU_SSE2 || defined(QILIN_SPU_NEON_TEST)
#include <immintrin.h>
#endif
#if QILIN_SPU_SSE2
#define QILIN_SPU_HOST_SINGLE 1
#elif QILIN_SPU_NEON
#define QILIN_SPU_HOST_SINGLE 1
#else
#define QILIN_SPU_HOST_SINGLE 0
#endif
#if !defined(QILIN_SPU_FLOAT_DEFAULT)
#define QILIN_SPU_FLOAT_DEFAULT 2
#endif

namespace qilin::rt::spu {

// ---- the way single precision is done (see the top) ----
enum class FloatMode : uint8_t { whole, exact, relaxed };
inline std::atomic<FloatMode> g_float_mode{FloatMode::whole};
QILIN_SPU_INLINE FloatMode float_mode() { return g_float_mode.load(std::memory_order_relaxed); }
// The way play uses: QILIN_SPU_FLOAT's, or the build's default.
inline FloatMode play_float_mode() {
    if (const char* const text = std::getenv("QILIN_SPU_FLOAT")) {
        if (std::strcmp(text, "whole") == 0) return FloatMode::whole;
        if (std::strcmp(text, "exact") == 0) return FloatMode::exact;
        if (std::strcmp(text, "relaxed") == 0) return FloatMode::relaxed;
    }
    return static_cast<FloatMode>(QILIN_SPU_FLOAT_DEFAULT);
}

// The host's float unit's control register, and the bits of it for SPU single precision: round toward zero, denormals
// zero in and out.
#if QILIN_SPU_SSE2 || defined(QILIN_SPU_NEON_TEST)
// (NEON's tested on x86-64: its float unit is SSE's.)
using FloatControl = unsigned;
inline constexpr FloatControl kSingleBits = 0x6000u | 0x8000u | 0x0040u, kSingleMask = kSingleBits;
inline FloatControl float_control() { return _mm_getcsr(); }
inline void set_float_control(FloatControl c) { _mm_setcsr(c); }
#elif QILIN_SPU_NEON
using FloatControl = uint64_t;
inline constexpr FloatControl kSingleBits = (uint64_t{3} << 22) | (uint64_t{1} << 24), kSingleMask = kSingleBits;
inline FloatControl float_control() {
    uint64_t c;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(c) : : "memory");
    return c;
}
inline void set_float_control(FloatControl c) { __asm__ __volatile__("msr fpcr, %0" : : "r"(c) : "memory"); }
#endif

// The calling thread's float unit set for SPU single precision, and the process's way set to `mode` (every thread that
// runs lifted SPU code enters the same).
inline void enter_single_mode(FloatMode mode) {
#if QILIN_SPU_HOST_SINGLE
    if (mode != FloatMode::whole) set_float_control((float_control() & ~kSingleMask) | kSingleBits);
#endif
    g_float_mode.store(mode, std::memory_order_relaxed);
}
inline void enter_single_mode() {
    static const FloatMode mode = play_float_mode();
    enter_single_mode(mode);
}

// For its own lifetime, the float unit as it is by default (round to nearest, denormals kept).
struct NearestMode {
#if QILIN_SPU_HOST_SINGLE
    FloatControl saved;
    NearestMode() : saved(float_control()) {
        if (saved & kSingleMask) set_float_control(saved & ~kSingleMask);
    }
    ~NearestMode() {
        if (saved & kSingleMask) set_float_control(saved);
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
// The host's float unit for the add, subtract, multiply and fused forms, and their results put right (`exact`): zeros
// made +0, and the lanes the host can't stand in for (an operand's exponent 255, the result's 254 or more) done again
// the exact way, out of line (they are rare).
enum class HostOp : uint8_t { Add, Subtract, Multiply, Fused, FusedNegateProduct, FusedNegateAddend };
// (The vectors in the host's vector registers, so that lifted code keeps its own there around the call.)
#if defined(_MSC_VER)
#define QILIN_SPU_VECTORCALL __vectorcall
#else
#define QILIN_SPU_VECTORCALL
#endif
#if QILIN_SPU_SSE2
using HostVector = __m128i;
using HostFloats = __m128;
#else
using HostVector = uint32x4_t;
using HostFloats = float32x4_t;
#endif
// `result` with the lanes in `lanes` done the exact way.
QILIN_SPU_COLD inline HostVector QILIN_SPU_VECTORCALL exact_lanes(HostVector result, HostVector x, HostVector y, HostVector z, int lanes, HostOp op) {
    SpuReg out = vec(result);
    const SpuReg a = vec(x), b = vec(y), c = vec(z);
    for (unsigned i = 0; i < 4; ++i) {
        if (!(lanes & (1 << i))) continue;
        switch (op) {
        case HostOp::Add: out.w[i] = add32(a.w[i], b.w[i], false); break;
        case HostOp::Subtract: out.w[i] = add32(a.w[i], b.w[i], true); break;
        case HostOp::Multiply: out.w[i] = multiply32(a.w[i], b.w[i]); break;
        case HostOp::Fused: out.w[i] = fused32(a.w[i], b.w[i], c.w[i], false, false); break;
        case HostOp::FusedNegateProduct: out.w[i] = fused32(a.w[i], b.w[i], c.w[i], true, false); break;
        case HostOp::FusedNegateAddend: out.w[i] = fused32(a.w[i], b.w[i], c.w[i], false, true); break;
        }
    }
    return out.v;
}
#if QILIN_SPU_SSE2
QILIN_SPU_INLINE __m128 f32(const SpuReg& x) { return _mm_castsi128_ps(x.v); }
QILIN_SPU_INLINE __m128i exponents(__m128i v) { return _mm_and_si128(_mm_srli_epi32(v, 23), _mm_set1_epi32(0xff)); }
QILIN_SPU_INLINE __m128i top_exponent(const SpuReg& v) { return _mm_cmpeq_epi32(exponents(v.v), _mm_set1_epi32(0xff)); }
QILIN_SPU_INLINE __m128i either(__m128i x, __m128i y) { return _mm_or_si128(x, y); }
QILIN_SPU_INLINE __m128 host_add(__m128 x, __m128 y) { return _mm_add_ps(x, y); }
QILIN_SPU_INLINE __m128 host_subtract(__m128 x, __m128 y) { return _mm_sub_ps(x, y); }
QILIN_SPU_INLINE __m128 host_multiply(__m128 x, __m128 y) { return _mm_mul_ps(x, y); }
QILIN_SPU_INLINE SpuReg host_result(__m128 result, __m128i bad_operands, HostOp op, const SpuReg& a, const SpuReg& b, const SpuReg& c) {
    const __m128i r = _mm_castps_si128(result);
    const __m128i e = exponents(r);
    const __m128i bad = _mm_or_si128(bad_operands, _mm_cmpgt_epi32(e, _mm_set1_epi32(253)));
    const __m128i out = _mm_andnot_si128(_mm_cmpeq_epi32(e, _mm_setzero_si128()), r);
    if (const int lanes = _mm_movemask_ps(_mm_castsi128_ps(bad))) return vec(exact_lanes(out, a.v, b.v, c.v, lanes, op));
    return vec(out);
}
// A zero made +0 (to the float unit -0 is equal to 0; results are never denormal, flushed as they are).
QILIN_SPU_INLINE SpuReg plus_zero(__m128 r) { return vec(_mm_castps_si128(_mm_and_ps(r, _mm_cmpneq_ps(r, _mm_setzero_ps())))); }
// a * b + c (with `negate_product` c - a * b, with `negate_addend` a * b - c), in double precision.
QILIN_SPU_INLINE __m128 fused_exact(const SpuReg& a, const SpuReg& b, const SpuReg& c, bool negate_product, bool negate_addend) {
    const __m128 x = f32(a), y = f32(b), z = f32(c);
    const auto half = [&](__m128d px, __m128d py, __m128d pz) {
        __m128d product = _mm_mul_pd(px, py);
        if (negate_product) product = _mm_sub_pd(_mm_setzero_pd(), product);
        return negate_addend ? _mm_sub_pd(product, pz) : _mm_add_pd(product, pz);
    };
    const __m128d low = half(_mm_cvtps_pd(x), _mm_cvtps_pd(y), _mm_cvtps_pd(z));
    const __m128d high = half(_mm_cvtps_pd(_mm_movehl_ps(x, x)), _mm_cvtps_pd(_mm_movehl_ps(y, y)), _mm_cvtps_pd(_mm_movehl_ps(z, z)));
    return _mm_movelh_ps(_mm_cvtpd_ps(low), _mm_cvtpd_ps(high));
}
// The same as a multiply then an add or subtract, each truncated (relaxed: x86-64 need not have a fused multiply-add).
QILIN_SPU_INLINE __m128 fused_relaxed(const SpuReg& a, const SpuReg& b, const SpuReg& c, bool negate_product, bool negate_addend) {
    const __m128 product = _mm_mul_ps(f32(a), f32(b));
    return negate_product ? _mm_sub_ps(f32(c), product) : negate_addend ? _mm_sub_ps(product, f32(c)) : _mm_add_ps(product, f32(c));
}
#else
QILIN_SPU_INLINE float32x4_t f32(const SpuReg& x) { return vreinterpretq_f32_u32(x.v); }
QILIN_SPU_INLINE uint32x4_t exponents(uint32x4_t v) { return vandq_u32(vshrq_n_u32(v, 23), vdupq_n_u32(0xff)); }
QILIN_SPU_INLINE uint32x4_t top_exponent(const SpuReg& v) { return vceqq_u32(exponents(v.v), vdupq_n_u32(0xff)); }
QILIN_SPU_INLINE uint32x4_t either(uint32x4_t x, uint32x4_t y) { return vorrq_u32(x, y); }
QILIN_SPU_INLINE float32x4_t host_add(float32x4_t x, float32x4_t y) { return vaddq_f32(x, y); }
QILIN_SPU_INLINE float32x4_t host_subtract(float32x4_t x, float32x4_t y) { return vsubq_f32(x, y); }
QILIN_SPU_INLINE float32x4_t host_multiply(float32x4_t x, float32x4_t y) { return vmulq_f32(x, y); }
QILIN_SPU_INLINE SpuReg host_result(float32x4_t result, uint32x4_t bad_operands, HostOp op, const SpuReg& a, const SpuReg& b, const SpuReg& c) {
    const uint32x4_t r = vreinterpretq_u32_f32(result);
    const uint32x4_t e = exponents(r);
    const uint32x4_t bad = vorrq_u32(bad_operands, vcgtq_u32(e, vdupq_n_u32(253)));
    const uint32x4_t out = vbicq_u32(r, vceqzq_u32(e));
    if (vmaxvq_u32(bad)) {
        const uint32x4_t lane_bits = {1, 2, 4, 8};
        return vec(exact_lanes(out, a.v, b.v, c.v, static_cast<int>(vaddvq_u32(vandq_u32(bad, lane_bits))), op));
    }
    return vec(out);
}
QILIN_SPU_INLINE SpuReg plus_zero(float32x4_t r) { return vec(vbicq_u32(vreinterpretq_u32_f32(r), vceqzq_f32(r))); }
// ARM64's fused multiply-add rounds once, as the SPU's does: exact and relaxed alike.
QILIN_SPU_INLINE float32x4_t fused_exact(const SpuReg& a, const SpuReg& b, const SpuReg& c, bool negate_product, bool negate_addend) {
    if (negate_product) return vfmsq_f32(f32(c), f32(a), f32(b));
    return vfmaq_f32(negate_addend ? vnegq_f32(f32(c)) : f32(c), f32(a), f32(b));
}
QILIN_SPU_INLINE float32x4_t fused_relaxed(const SpuReg& a, const SpuReg& b, const SpuReg& c, bool negate_product, bool negate_addend) {
    return fused_exact(a, b, c, negate_product, negate_addend);
}
#endif
// The exact ways, out of line: `exact`'s float unit put right, or `whole`'s whole numbers. Each instruction's inline
// form is play's, `relaxed`, a few instructions; the others are a call (a test and a branch the processor predicts, the
// way being the same all through). Inline as well, they made the lifted SPU code's largest file take MSVC 60% longer
// to compile and a third more memory, for the ways play doesn't use.
template <HostOp op>
QILIN_SPU_NOINLINE HostVector QILIN_SPU_VECTORCALL exact_single(HostVector x, HostVector y, HostVector z) {
    if (float_mode() == FloatMode::whole) return exact_lanes(x, x, y, z, 15, op);
    const SpuReg a = vec(x), b = vec(y), c = vec(z);
    if constexpr (op == HostOp::Add) return host_result(host_add(f32(a), f32(b)), either(top_exponent(a), top_exponent(b)), op, a, b, c).v;
    else if constexpr (op == HostOp::Subtract) return host_result(host_subtract(f32(a), f32(b)), either(top_exponent(a), top_exponent(b)), op, a, b, c).v;
    else if constexpr (op == HostOp::Multiply) return host_result(host_multiply(f32(a), f32(b)), either(top_exponent(a), top_exponent(b)), op, a, b, c).v;
    else
        return host_result(fused_exact(a, b, c, op == HostOp::FusedNegateProduct, op == HostOp::FusedNegateAddend), either(either(top_exponent(a), top_exponent(b)), top_exponent(c)),
                           op, a, b, c)
            .v;
}
QILIN_SPU_INLINE SpuReg fa(const SpuReg& a, const SpuReg& b) {
    if (float_mode() != FloatMode::relaxed) [[unlikely]]
        return vec(exact_single<HostOp::Add>(a.v, b.v, b.v));
    return plus_zero(host_add(f32(a), f32(b)));
}
QILIN_SPU_INLINE SpuReg fs(const SpuReg& a, const SpuReg& b) {
    if (float_mode() != FloatMode::relaxed) [[unlikely]]
        return vec(exact_single<HostOp::Subtract>(a.v, b.v, b.v));
    return plus_zero(host_subtract(f32(a), f32(b)));
}
QILIN_SPU_INLINE SpuReg fm(const SpuReg& a, const SpuReg& b) {
    if (float_mode() != FloatMode::relaxed) [[unlikely]]
        return vec(exact_single<HostOp::Multiply>(a.v, b.v, b.v));
    return plus_zero(host_multiply(f32(a), f32(b)));
}
// a * b + c, the product exact; with `negate_product` c - a * b, with `negate_addend` a * b - c.
template <bool negate_product, bool negate_addend>
QILIN_SPU_INLINE SpuReg fused(const SpuReg& a, const SpuReg& b, const SpuReg& c) {
    constexpr HostOp op = negate_product ? HostOp::FusedNegateProduct : negate_addend ? HostOp::FusedNegateAddend : HostOp::Fused;
    if (float_mode() != FloatMode::relaxed) [[unlikely]]
        return vec(exact_single<op>(a.v, b.v, c.v));
    return plus_zero(fused_relaxed(a, b, c, negate_product, negate_addend));
}
#else
inline SpuReg fa(const SpuReg& a, const SpuReg& b) { return words(a, b, [](uint32_t p, uint32_t q) { return add32(p, q, false); }); }
inline SpuReg fs(const SpuReg& a, const SpuReg& b) { return words(a, b, [](uint32_t p, uint32_t q) { return add32(p, q, true); }); }
inline SpuReg fm(const SpuReg& a, const SpuReg& b) { return words(a, b, [](uint32_t p, uint32_t q) { return multiply32(p, q); }); }
template <bool negate_product, bool negate_addend>
inline SpuReg fused(const SpuReg& a, const SpuReg& b, const SpuReg& c) {
    return {{fused32(a.w[0], b.w[0], c.w[0], negate_product, negate_addend), fused32(a.w[1], b.w[1], c.w[1], negate_product, negate_addend),
             fused32(a.w[2], b.w[2], c.w[2], negate_product, negate_addend), fused32(a.w[3], b.w[3], c.w[3], negate_product, negate_addend)}};
}
#endif
QILIN_SPU_INLINE SpuReg fma(const SpuReg& a, const SpuReg& b, const SpuReg& c) { return fused<false, false>(a, b, c); }
QILIN_SPU_INLINE SpuReg fms(const SpuReg& a, const SpuReg& b, const SpuReg& c) { return fused<false, true>(a, b, c); }
QILIN_SPU_INLINE SpuReg fnms(const SpuReg& a, const SpuReg& b, const SpuReg& c) { return fused<true, false>(a, b, c); }

// ---- compares: every word with a zero exponent is the one zero ----
inline int64_t ordered(uint32_t w) {
    if (((w >> 23) & 0xffu) == 0) return 0;
    const int64_t magnitude = w & 0x7fffffffu;
    return (w >> 31) ? -magnitude : magnitude;
}
#if QILIN_SPU_HOST_SINGLE
// The exact compares (both exact ways), out of line as the arithmetic's are: the same order as signed words, zero for a
// zero exponent and the magnitude with the sign's (as ordered()). `relaxed` compares as the host's floats do.
enum class Compare : uint8_t { equal, greater, magnitude_equal, magnitude_greater };
#if QILIN_SPU_SSE2
QILIN_SPU_INLINE __m128i ordered_words(__m128i w, bool magnitude) {
    const __m128i zero = _mm_cmpeq_epi32(_mm_and_si128(w, _mm_set1_epi32(0x7f800000)), _mm_setzero_si128());
    const __m128i m = _mm_andnot_si128(zero, w);
    const __m128i size = _mm_and_si128(m, _mm_set1_epi32(0x7fffffff));
    if (magnitude) return size;
    const __m128i sign = _mm_srai_epi32(m, 31);
    return _mm_sub_epi32(_mm_xor_si128(size, sign), sign);
}
template <Compare k>
QILIN_SPU_NOINLINE __m128i QILIN_SPU_VECTORCALL exact_compare(__m128i x, __m128i y) {
    constexpr bool magnitude = k == Compare::magnitude_equal || k == Compare::magnitude_greater;
    const __m128i p = ordered_words(x, magnitude), q = ordered_words(y, magnitude);
    return k == Compare::equal || k == Compare::magnitude_equal ? _mm_cmpeq_epi32(p, q) : _mm_cmpgt_epi32(p, q);
}
QILIN_SPU_INLINE __m128 magnitudes(const SpuReg& a) { return _mm_and_ps(f32(a), _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff))); }
QILIN_SPU_INLINE __m128i relaxed_equal(const SpuReg& a, const SpuReg& b) { return _mm_castps_si128(_mm_cmpeq_ps(f32(a), f32(b))); }
QILIN_SPU_INLINE __m128i relaxed_greater(const SpuReg& a, const SpuReg& b) { return _mm_castps_si128(_mm_cmpgt_ps(f32(a), f32(b))); }
QILIN_SPU_INLINE __m128i relaxed_magnitude_equal(const SpuReg& a, const SpuReg& b) { return _mm_castps_si128(_mm_cmpeq_ps(magnitudes(a), magnitudes(b))); }
QILIN_SPU_INLINE __m128i relaxed_magnitude_greater(const SpuReg& a, const SpuReg& b) { return _mm_castps_si128(_mm_cmpgt_ps(magnitudes(a), magnitudes(b))); }
#else
QILIN_SPU_INLINE int32x4_t ordered_words(uint32x4_t w, bool magnitude) {
    const uint32x4_t m = vandq_u32(w, vtstq_u32(w, vdupq_n_u32(0x7f800000)));
    const int32x4_t size = vreinterpretq_s32_u32(vandq_u32(m, vdupq_n_u32(0x7fffffff)));
    if (magnitude) return size;
    const int32x4_t sign = vshrq_n_s32(vreinterpretq_s32_u32(m), 31);
    return vsubq_s32(veorq_s32(size, sign), sign);
}
template <Compare k>
QILIN_SPU_NOINLINE uint32x4_t exact_compare(uint32x4_t x, uint32x4_t y) {
    constexpr bool magnitude = k == Compare::magnitude_equal || k == Compare::magnitude_greater;
    const int32x4_t p = ordered_words(x, magnitude), q = ordered_words(y, magnitude);
    return k == Compare::equal || k == Compare::magnitude_equal ? vceqq_s32(p, q) : vcgtq_s32(p, q);
}
QILIN_SPU_INLINE uint32x4_t relaxed_equal(const SpuReg& a, const SpuReg& b) { return vceqq_f32(f32(a), f32(b)); }
QILIN_SPU_INLINE uint32x4_t relaxed_greater(const SpuReg& a, const SpuReg& b) { return vcgtq_f32(f32(a), f32(b)); }
QILIN_SPU_INLINE uint32x4_t relaxed_magnitude_equal(const SpuReg& a, const SpuReg& b) { return vceqq_f32(vabsq_f32(f32(a)), vabsq_f32(f32(b))); }
QILIN_SPU_INLINE uint32x4_t relaxed_magnitude_greater(const SpuReg& a, const SpuReg& b) { return vcagtq_f32(f32(a), f32(b)); }
#endif
#define QILIN_SPU_COMPARE(name, kind)                                                       \
    QILIN_SPU_INLINE SpuReg name(const SpuReg& a, const SpuReg& b) {                        \
        if (float_mode() != FloatMode::relaxed) [[unlikely]]                                \
            return vec(exact_compare<Compare::kind>(a.v, b.v));                             \
        return vec(relaxed_##kind(a, b));                                                   \
    }
QILIN_SPU_COMPARE(fceq, equal)
QILIN_SPU_COMPARE(fcgt, greater)
QILIN_SPU_COMPARE(fcmeq, magnitude_equal)
QILIN_SPU_COMPARE(fcmgt, magnitude_greater)
#undef QILIN_SPU_COMPARE
#else
inline SpuReg fceq(const SpuReg& a, const SpuReg& b) { return words(a, b, [](uint32_t p, uint32_t q) { return ordered(p) == ordered(q) ? 0xffffffffu : 0u; }); }
inline SpuReg fcgt(const SpuReg& a, const SpuReg& b) { return words(a, b, [](uint32_t p, uint32_t q) { return ordered(p) > ordered(q) ? 0xffffffffu : 0u; }); }
inline SpuReg fcmeq(const SpuReg& a, const SpuReg& b) {
    return words(a, b, [](uint32_t p, uint32_t q) { return ordered(p & 0x7fffffffu) == ordered(q & 0x7fffffffu) ? 0xffffffffu : 0u; });
}
inline SpuReg fcmgt(const SpuReg& a, const SpuReg& b) {
    return words(a, b, [](uint32_t p, uint32_t q) { return ordered(p & 0x7fffffffu) > ordered(q & 0x7fffffffu) ? 0xffffffffu : 0u; });
}
#endif

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

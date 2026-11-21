// Floating point for lifted PPU code: bit-exact with the guest.
//
// A floating-point register holds a double. What the host's own arithmetic
// doesn't give by itself, and this header adds:
//
//   - NaNs. PowerPC's result for a NaN operand is the first NaN operand in
//     the architecture's order (frA, frB, frC), made quiet, and for an
//     invalid operation the positive default NaN. Hosts differ from that and
//     from each other, so every operation checks its result and, if it is a
//     NaN, works it out here.
//   - Single-precision arithmetic (fadds and its kin) rounds the exact result
//     of its double operands once, to single. When the operands are
//     themselves singles (nearly always: they came from single loads and
//     single operations) the host's float arithmetic is exactly that. When
//     they aren't, rounding the double result again would round twice, so
//     the slow path finds which side of the double result the exact one lies
//     on (by error-free transformations) and rounds to single through
//     "round to odd", which makes the second rounding harmless.
//   - Loads and stores of singles convert formats without rounding or
//     quieting, as the architecture's own conversions do.
//
// The rounding mode is round to nearest, the mode a program starts in; the
// FPSCR's exception and result flags are not kept. Instructions that read or
// write the FPSCR are not in the lifter's slice yet, so a function that would
// notice is not lifted (docs/recompiler-core.md).
//
// Runtime code: no std::format here.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#if defined(_MSC_VER) && !defined(__clang__)
#define QILIN_FP_INLINE __forceinline
#define QILIN_FP_SLOW __declspec(noinline) inline
#else
#define QILIN_FP_INLINE [[gnu::always_inline]] inline
#define QILIN_FP_SLOW [[gnu::noinline, gnu::cold]] inline
#endif

namespace qilin::rt::fp {

QILIN_FP_INLINE uint64_t bits(double x) { return std::bit_cast<uint64_t>(x); }
QILIN_FP_INLINE double from_bits(uint64_t b) { return std::bit_cast<double>(b); }

inline constexpr uint64_t kSign = 0x8000000000000000ull, kQuiet = 0x0008000000000000ull, kDefaultNan = 0x7FF8000000000000ull;

QILIN_FP_INLINE bool is_nan(double x) { return x != x; }
QILIN_FP_INLINE bool finite(double x) { return (bits(x) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull; }

// The NaN an operation gives: the first NaN among its operands, in the
// architecture's order, made quiet; with none (an invalid operation), the default.
QILIN_FP_SLOW double nan_of(double a) { return is_nan(a) ? from_bits(bits(a) | kQuiet) : from_bits(kDefaultNan); }
QILIN_FP_SLOW double nan_of(double a, double b) { return is_nan(a) ? from_bits(bits(a) | kQuiet) : nan_of(b); }
QILIN_FP_SLOW double nan_of(double a, double b, double c) { return is_nan(a) ? from_bits(bits(a) | kQuiet) : nan_of(b, c); }

// ---- moves: bits, never arithmetic (a signalling NaN stays one) ----
QILIN_FP_INLINE double fneg(double x) { return from_bits(bits(x) ^ kSign); }
QILIN_FP_INLINE double fabs(double x) { return from_bits(bits(x) & ~kSign); }
QILIN_FP_INLINE double fnabs(double x) { return from_bits(bits(x) | kSign); }

// ---- double precision ----
QILIN_FP_INLINE double fadd(double a, double b) {
    const double r = a + b;
    return r == r ? r : nan_of(a, b);
}
QILIN_FP_INLINE double fsub(double a, double b) {
    const double r = a - b;
    return r == r ? r : nan_of(a, b);
}
QILIN_FP_INLINE double fmul(double a, double c) {
    const double r = a * c;
    return r == r ? r : nan_of(a, c);
}
QILIN_FP_INLINE double fdiv(double a, double b) {
    const double r = a / b;
    return r == r ? r : nan_of(a, b);
}
QILIN_FP_INLINE double fsqrt(double b) {
    const double r = std::sqrt(b);
    return r == r ? r : nan_of(b);
}
// a * c + b and its kin, fused: one rounding. (The operands' order in a NaN's choice is frA, frB, frC.)
QILIN_FP_INLINE double fmadd(double a, double c, double b) {
    const double r = std::fma(a, c, b);
    return r == r ? r : nan_of(a, b, c);
}
QILIN_FP_INLINE double fmsub(double a, double c, double b) {
    const double r = std::fma(a, c, -b);
    return r == r ? r : nan_of(a, b, c);
}
QILIN_FP_INLINE double fnmadd(double a, double c, double b) {  // the negative, except of a NaN
    const double r = std::fma(a, c, b);
    return r == r ? -r : nan_of(a, b, c);
}
QILIN_FP_INLINE double fnmsub(double a, double c, double b) {
    const double r = std::fma(a, c, -b);
    return r == r ? -r : nan_of(a, b, c);
}

// ---- single precision ----
// Is the double exactly a single? (False for a NaN.)
QILIN_FP_INLINE bool is_single(double x) { return static_cast<double>(static_cast<float>(x)) == x; }

// `r` is the exact value V rounded to double, and `side` the sign of V - r.
// The single nearest V: r moved, if it is inexact and even, to its odd
// neighbour on V's side, then rounded to single.
QILIN_FP_SLOW double single_from(double r, int side) {
    const uint64_t b = bits(r);
    const uint64_t magnitude = b & ~kSign;
    // Below 2^-500 the single is zero or the smallest whatever the last bit; infinity stays.
    if (side != 0 && !(b & 1) && finite(r) && magnitude > 0x20B0000000000000ull) {
        const bool away = (side > 0) == !(b & kSign);  // V is further from zero than r
        r = from_bits(away ? b + 1 : b - 1);
    }
    return static_cast<double>(static_cast<float>(r));
}

// The sign of the exact sum of up to four doubles (Shewchuk's expansion: each
// addition's rounding error is kept as a term of its own).
QILIN_FP_SLOW int sum_sign(const double* terms, int count) {
    double e[4];
    int n = 0;
    for (int t = 0; t < count; ++t) {
        double q = terms[t];
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const double sum = q + e[i];
            const double bv = sum - q, av = sum - bv;
            const double error = (q - av) + (e[i] - bv);
            if (error != 0) e[m++] = error;
            q = sum;
        }
        e[m++] = q;
        n = m;
    }
    for (int i = n - 1; i >= 0; --i)
        if (e[i] != 0) return e[i] > 0 ? 1 : -1;
    return 0;
}

QILIN_FP_SLOW double fadds_slow(double a, double b) {
    const double r = a + b;
    if (r != r) return nan_of(a, b);
    if (!finite(r)) return r;
    const double terms[] = {a, b, -r};
    return single_from(r, sum_sign(terms, 3));
}
QILIN_FP_SLOW double fmuls_slow(double a, double c) {
    const double r = a * c;
    if (r != r) return nan_of(a, c);
    if (!finite(r)) return r;
    const double error = std::fma(a, c, -r);  // exact, where r is far from underflow (single_from ignores the rest)
    return single_from(r, error > 0 ? 1 : error < 0 ? -1 : 0);
}
QILIN_FP_SLOW double fdivs_slow(double a, double b) {
    const double r = a / b;
    if (r != r) return nan_of(a, b);
    if (!finite(r) || !finite(a) || !finite(b)) return static_cast<double>(static_cast<float>(r));
    if ((bits(a) & ~kSign) < 0x0790000000000000ull) {
        // A dividend below 2^-900: scaled up (exactly, both operands alike) so that the remainder doesn't underflow.
        const double scale = 0x1p600;
        if (!finite(b * scale)) return static_cast<double>(static_cast<float>(r));  // (the quotient is then far below any single)
        a *= scale;
        b *= scale;
    }
    const double remainder = std::fma(-r, b, a);  // a - r*b, exact; the quotient is off by remainder / b
    const int sign = remainder > 0 ? 1 : remainder < 0 ? -1 : 0;
    return single_from(r, b < 0 ? -sign : sign);
}
QILIN_FP_SLOW double fsqrts_slow(double b) {
    const double r = std::sqrt(b);
    if (r != r) return nan_of(b);
    if (!finite(r)) return r;
    const double remainder = std::fma(-r, r, b);
    return single_from(r, remainder > 0 ? 1 : remainder < 0 ? -1 : 0);
}
// a * c + b rounded once to single (`b` already negated for the subtracting forms; the NaN's operands are the originals).
QILIN_FP_SLOW double fmadds_slow(double a, double c, double b, double nan_a, double nan_b, double nan_c) {
    const double r = std::fma(a, c, b);
    if (r != r) return nan_of(nan_a, nan_b, nan_c);
    const double p = a * c;
    if (!finite(r) || !finite(p)) return static_cast<double>(static_cast<float>(r));
    if ((bits(p) & ~kSign) < 0x03F0000000000000ull) {
        // The product is below 2^-960, where its rounding error can't be had exactly. But then it
        // is far below half a unit of any b that matters: r is b, and the true value lies on the
        // product's side of it. (With no product at all, r is b exactly. And when r isn't b, both
        // are too small for the side to change the single.)
        if (a == 0 || c == 0) return static_cast<double>(static_cast<float>(r));
        const int product_side = ((bits(a) ^ bits(c)) & kSign) ? -1 : 1;
        return single_from(r, r == b ? product_side : 0);
    }
    const double terms[] = {p, std::fma(a, c, -p), b, -r};  // the product exactly, as two terms; then b; less r
    return single_from(r, sum_sign(terms, 4));
}

QILIN_FP_INLINE double fadds(double a, double b) {
    if (is_single(a) && is_single(b)) {
        const float r = static_cast<float>(a) + static_cast<float>(b);
        if (r == r) return r;
    }
    return fadds_slow(a, b);
}
QILIN_FP_INLINE double fsubs(double a, double b) {
    if (is_single(a) && is_single(b)) {
        const float r = static_cast<float>(a) - static_cast<float>(b);
        if (r == r) return r;
    }
    if (is_nan(a) || is_nan(b)) return nan_of(a, b);
    return fadds_slow(a, -b);
}
QILIN_FP_INLINE double fmuls(double a, double c) {
    if (is_single(a) && is_single(c)) {
        const float r = static_cast<float>(a) * static_cast<float>(c);
        if (r == r) return r;
    }
    return fmuls_slow(a, c);
}
QILIN_FP_INLINE double fdivs(double a, double b) {
    if (is_single(a) && is_single(b)) {
        const float r = static_cast<float>(a) / static_cast<float>(b);
        if (r == r) return r;
    }
    return fdivs_slow(a, b);
}
QILIN_FP_INLINE double fsqrts(double b) {
    if (is_single(b)) {
        const float r = std::sqrt(static_cast<float>(b));
        if (r == r) return r;
    }
    return fsqrts_slow(b);
}
// x + y rounded once to single, for an exact x and y that are not NaNs: the
// sum in double, the error of that sum (exactly, by Knuth's two-sum), and
// from the error's sign the single nearest the true sum.
QILIN_FP_INLINE double add_to_single(double x, double y) {
    const double r = x + y;
    if (!finite(r)) return r == r ? r : from_bits(kDefaultNan);  // an infinity, or infinity less infinity
    const double bv = r - x, av = r - bv;
    const double error = (x - av) + (y - bv);
    if (error == 0) return static_cast<double>(static_cast<float>(r));
    return single_from(r, error > 0 ? 1 : -1);
}
// The fused forms. With frA and frC singles (as they nearly always are) their
// product is exact in a double, and what is left is one sum to round once.
// The host's single-precision fma is not used: not every C library's rounds
// once (the differential test found one that rounds twice).
QILIN_FP_INLINE double fmadds(double a, double c, double b) {
    if (is_single(a) && is_single(c) && !is_nan(b)) {
        const double product = a * c;
        if (product == product) return add_to_single(product, b);
    }
    return fmadds_slow(a, c, b, a, b, c);
}
QILIN_FP_INLINE double fmsubs(double a, double c, double b) {
    if (is_single(a) && is_single(c) && !is_nan(b)) {
        const double product = a * c;
        if (product == product) return add_to_single(product, fneg(b));
    }
    return fmadds_slow(a, c, fneg(b), a, b, c);
}
QILIN_FP_INLINE double fnmadds(double a, double c, double b) {
    const double r = fmadds(a, c, b);
    return r == r ? -r : r;
}
QILIN_FP_INLINE double fnmsubs(double a, double c, double b) {
    const double r = fmsubs(a, c, b);
    return r == r ? -r : r;
}

// ---- conversions ----
// frsp: round to single. A NaN is made quiet and keeps what of its payload a single holds.
QILIN_FP_INLINE double frsp(double b) {
    if (is_nan(b)) return from_bits((bits(b) | kQuiet) & 0xFFFFFFFFE0000000ull);
    return static_cast<double>(static_cast<float>(b));
}
// fcfid: the register's bits as a signed integer, to double.
QILIN_FP_INLINE double fcfid(double b) { return static_cast<double>(static_cast<int64_t>(bits(b))); }
// To an integer in the register's bits, saturating; a NaN gives the most
// negative. The word forms leave the upper half undefined: here, 0xFFF80000.
QILIN_FP_SLOW double to_word(double value) {  // `value` already an integer, or a NaN
    uint32_t w;
    if (is_nan(value)) w = 0x80000000u;
    else if (value >= 2147483648.0) w = 0x7FFFFFFFu;
    else if (value <= -2147483648.0) w = 0x80000000u;
    else w = static_cast<uint32_t>(static_cast<int32_t>(value));
    return from_bits(0xFFF8000000000000ull | w);
}
QILIN_FP_SLOW double to_doubleword(double value) {
    uint64_t d;
    if (is_nan(value)) d = kSign;
    else if (value >= 9223372036854775808.0) d = 0x7FFFFFFFFFFFFFFFull;
    else if (value <= -9223372036854775808.0) d = kSign;
    else d = static_cast<uint64_t>(static_cast<int64_t>(value));
    return from_bits(d);
}
QILIN_FP_INLINE double fctiwz(double b) { return to_word(std::trunc(b)); }
QILIN_FP_INLINE double fctidz(double b) { return to_doubleword(std::trunc(b)); }
QILIN_FP_INLINE double fctiw(double b) { return to_word(std::nearbyint(b)); }   // to nearest, ties to even: the mode in force
QILIN_FP_INLINE double fctid(double b) { return to_doubleword(std::nearbyint(b)); }

// fsel: frC if frA >= 0 (minus zero counts; a NaN doesn't), else frB.
QILIN_FP_INLINE double fsel(double a, double c, double b) { return a >= 0.0 ? c : b; }

// A compare's CR field: less, greater, equal, unordered.
QILIN_FP_INLINE uint32_t fcompare(double a, double b) { return a < b ? 8u : a > b ? 4u : a == b ? 2u : 1u; }

// ---- singles in memory ----
// lfs: the single's value as a double, with a NaN's bits carried over as they are.
QILIN_FP_INLINE double load_single(uint64_t word) {
    const uint32_t w = static_cast<uint32_t>(word);
    if ((w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu))
        return from_bits((uint64_t{w & 0x80000000u} << 32) | 0x7FF0000000000000ull | (uint64_t{w & 0x007FFFFFu} << 29));
    return static_cast<double>(std::bit_cast<float>(w));
}
// stfs: the register's value in single format, by the architecture's
// conversion: no rounding (the fraction's top 23 bits), a value too small
// for a single normal shifted into a denormal. A register that doesn't hold a
// single (too large, or too small even for a denormal) gives what the
// hardware's bit selection gives in the first case and, in the second (left
// undefined), zero with the sign.
QILIN_FP_INLINE uint64_t store_single(double value) {
    const uint64_t b = bits(value);
    const unsigned exponent = static_cast<unsigned>(b >> 52) & 0x7FF;
    if (exponent > 896 || (b & ~kSign) == 0) return ((b >> 32) & 0xC0000000u) | ((b >> 29) & 0x3FFFFFFFu);
    if (exponent >= 874) {
        const uint64_t fraction = (b & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
        return ((b >> 32) & 0x80000000u) | ((fraction >> (896 - exponent + 30)) & 0x007FFFFFu);
    }
    return (b >> 32) & 0x80000000u;
}

}  // namespace qilin::rt::fp

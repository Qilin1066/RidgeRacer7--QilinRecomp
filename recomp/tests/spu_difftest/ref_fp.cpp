// The SPU reference's floating point, from the instruction set's
// definition and apart from runtime/include/qilin/rt/spu_fp.hpp.
//
// Single precision here is done the long way: every number is put, whole,
// into one very long unsigned number at its true place (a single's value
// is its mantissa times 2^(exponent - 150), and a product's is two
// mantissas times 2^(exponents - 300), so everything is a whole number of
// units of 2^-300), sums and differences are exact, and only then is the
// answer cut to 24 bits. (The runtime aligns in 64 bits and remembers what
// fell off: another method.) Double precision uses the host's doubles: the
// definition is IEEE's.
#include <bit>
#include <cmath>
#include <cstring>

#include "ref.hpp"

namespace spu_difftest {

using qilin::spu::Insn;
using qilin::spu::Op;

namespace {

// An unsigned number of 704 bits, the least significant limb first.
struct Long {
    static constexpr unsigned kLimbs = 11;
    uint64_t limb[kLimbs] = {};

    static Long of(uint64_t value, unsigned shift) {  // value * 2^shift
        Long r;
        const unsigned at = shift / 64, bits = shift % 64;
        r.limb[at] = value << bits;
        if (bits) r.limb[at + 1] = value >> (64 - bits);
        return r;
    }
    bool is_zero() const {
        for (const uint64_t l : limb)
            if (l) return false;
        return true;
    }
    int top() const {  // the highest set bit's number, or -1
        for (int i = kLimbs - 1; i >= 0; --i)
            if (limb[i]) return 64 * i + 63 - std::countl_zero(limb[i]);
        return -1;
    }
    bool bit(int n) const { return n >= 0 && (limb[n / 64] >> (n % 64)) & 1; }
};
int compare(const Long& a, const Long& b) {
    for (int i = Long::kLimbs - 1; i >= 0; --i)
        if (a.limb[i] != b.limb[i]) return a.limb[i] < b.limb[i] ? -1 : 1;
    return 0;
}
Long add(const Long& a, const Long& b) {
    Long r;
    uint64_t carry = 0;
    for (unsigned i = 0; i < Long::kLimbs; ++i) {
        const uint64_t s = a.limb[i] + b.limb[i], t = s + carry;
        carry = (s < a.limb[i]) || (t < s);
        r.limb[i] = t;
    }
    return r;
}
Long subtract(const Long& a, const Long& b) {  // a >= b
    Long r;
    uint64_t borrow = 0;
    for (unsigned i = 0; i < Long::kLimbs; ++i) {
        const uint64_t d = a.limb[i] - b.limb[i], e = d - borrow;
        borrow = (a.limb[i] < b.limb[i]) || (d < borrow);
        r.limb[i] = e;
    }
    return r;
}

// A signed quantity in units of 2^-300.
struct Quantity {
    bool negative = false;
    Long magnitude;
};
bool is_zero_word(uint32_t w) { return ((w >> 23) & 0xff) == 0; }
uint64_t mantissa(uint32_t w) { return (w & 0x7fffff) | 0x800000; }
unsigned exponent(uint32_t w) { return (w >> 23) & 0xff; }
// A single: mantissa * 2^(exponent - 150) = mantissa * 2^(exponent + 150) units.
Quantity single(uint32_t w, bool negate = false) {
    if (is_zero_word(w)) return {};
    return {((w >> 31) != 0) != negate, Long::of(mantissa(w), exponent(w) + 150)};
}
// A product of two singles: mantissas * 2^(exponents - 300).
Quantity product(uint32_t a, uint32_t b, bool negate = false) {
    if (is_zero_word(a) || is_zero_word(b)) return {};
    return {(((a ^ b) >> 31) != 0) != negate, Long::of(mantissa(a) * mantissa(b), exponent(a) + exponent(b))};
}
Quantity plus(const Quantity& x, const Quantity& y) {
    if (x.negative == y.negative) return {x.negative, add(x.magnitude, y.magnitude)};
    const int order = compare(x.magnitude, y.magnitude);
    if (order == 0) return {};
    return order > 0 ? Quantity{x.negative, subtract(x.magnitude, y.magnitude)} : Quantity{y.negative, subtract(y.magnitude, x.magnitude)};
}
// The single for a quantity of `unit_exponent` (-300 for the above): cut toward zero; the largest if too large, +0 if too small.
uint32_t to_single(const Quantity& q, int unit_exponent = -300) {
    const int top = q.magnitude.top();
    if (top < 0) return 0;
    const int biased = top + unit_exponent + 127;  // the value is at least 2^(top + unit_exponent)
    if (biased > 255) return q.negative ? 0xffffffffu : 0x7fffffffu;
    if (biased < 1) return 0;
    uint32_t fraction = 0;
    for (int i = 1; i <= 23; ++i) fraction = (fraction << 1) | (q.magnitude.bit(top - i) ? 1u : 0u);
    return (q.negative ? 0x80000000u : 0u) | (static_cast<uint32_t>(biased) << 23) | fraction;
}

// The order of two singles' values: -1, 0, 1. Every zero-exponent word is the one zero.
int order(uint32_t a, uint32_t b, bool magnitudes) {
    Quantity x = single(a), y = single(b);
    if (magnitudes) x.negative = y.negative = false;
    const Quantity difference = plus(x, {!y.negative, y.magnitude});
    if (difference.magnitude.is_zero()) return 0;
    return difference.negative ? -1 : 1;
}

// The estimates' tables, as the runtime states them (the top of spu_fp.hpp): by whole numbers of 2^-40 from the
// curve's values, so that the putting together and the rounding are done another way.
uint32_t table_entry(double (*g)(double), unsigned j) {
    const auto fixed = [&](double m) { return static_cast<int64_t>(std::llround(std::ldexp(g(m), 40))); };
    const int64_t from = fixed(1.0 + j / 32.0), to = fixed(1.0 + (j + 1) / 32.0), middle = fixed(1.0 + (2 * j + 1) / 64.0);
    const int64_t error = (from + to) / 2 - middle, step = (from - to) * 2;
    const int64_t base = from + ((j & 1) ? step / 2 : 0) - error / 2 - (int64_t{1} << 40);
    const auto rounded = [](int64_t v, int64_t most) {  // to units of 2^-13, to the nearest
        const int64_t r = (v + (int64_t{1} << 26)) >> 27;
        return static_cast<uint32_t>(r > most ? most : r);
    };
    return (rounded(base, 0x1fff) << 10) | rounded(step, 0x3ff);
}
double g_reciprocal(double m) { return 2.0 / m; }
double g_root_even(double m) { return 2.0 / std::sqrt(m); }
double g_root_odd(double m) { return std::sqrt(2.0) / std::sqrt(m); }

uint32_t word_of(const uint8_t* r, unsigned i) { return (uint32_t{r[4 * i]} << 24) | (uint32_t{r[4 * i + 1]} << 16) | (uint32_t{r[4 * i + 2]} << 8) | r[4 * i + 3]; }
void set_word(uint8_t* r, unsigned i, uint32_t v) {
    for (unsigned b = 0; b < 4; ++b) r[4 * i + b] = static_cast<uint8_t>(v >> (24 - 8 * b));
}
uint64_t pair_of(const uint8_t* r, unsigned i) { return (uint64_t{word_of(r, 2 * i)} << 32) | word_of(r, 2 * i + 1); }
bool is_nan64(uint64_t d) { return ((d >> 52) & 0x7ff) == 0x7ff && (d & 0xfffffffffffffull) != 0; }
double as_double(uint64_t d) {
    if (((d >> 52) & 0x7ff) == 0) d &= uint64_t{1} << 63;  // a denormal operand is zero, with its sign
    return std::bit_cast<double>(d);
}

}  // namespace

bool step_fp(State& s, const Insn& in) {
    const uint8_t* const A = s.r[in.ra()];
    const uint8_t* const B = s.r[in.rb()];
    const uint8_t* const C = s.r[in.rt()];   // the four-register forms' third operand
    uint8_t out[16];
    const auto finish = [&](unsigned target) {
        std::memcpy(s.r[target], out, 16);
        return true;
    };
    const auto each = [&](unsigned target, auto f) {
        for (unsigned i = 0; i < 4; ++i) set_word(out, i, f(word_of(A, i), word_of(B, i), word_of(C, i)));
        return finish(target);
    };
    const auto truth = [](bool yes) { return yes ? 0xffffffffu : 0u; };
    const auto doubles = [&](auto f) {
        for (unsigned i = 0; i < 2; ++i) {
            const uint64_t x = pair_of(A, i), y = pair_of(B, i);
            uint64_t bits = 0x7ff8000000000000ull;  // any NaN in, or made: the default one
            if (!is_nan64(x) && !is_nan64(y)) {
                const double value = f(as_double(x), as_double(y));
                if (!std::isnan(value)) bits = std::bit_cast<uint64_t>(value);
            }
            set_word(out, 2 * i, static_cast<uint32_t>(bits >> 32));
            set_word(out, 2 * i + 1, static_cast<uint32_t>(bits));
        }
        return finish(in.rt());
    };

    switch (in.op) {
    case Op::fa: return each(in.rt(), [](uint32_t a, uint32_t b, uint32_t) { return to_single(plus(single(a), single(b))); });
    case Op::fs: return each(in.rt(), [](uint32_t a, uint32_t b, uint32_t) { return to_single(plus(single(a), single(b, true))); });
    case Op::fm: return each(in.rt(), [](uint32_t a, uint32_t b, uint32_t) { return to_single(product(a, b)); });
    case Op::fma: return each(in.rt4(), [](uint32_t a, uint32_t b, uint32_t c) { return to_single(plus(product(a, b), single(c))); });
    case Op::fms: return each(in.rt4(), [](uint32_t a, uint32_t b, uint32_t c) { return to_single(plus(product(a, b), single(c, true))); });
    case Op::fnms: return each(in.rt4(), [](uint32_t a, uint32_t b, uint32_t c) { return to_single(plus(single(c), product(a, b, true))); });
    case Op::fceq: return each(in.rt(), [&](uint32_t a, uint32_t b, uint32_t) { return truth(order(a, b, false) == 0); });
    case Op::fcgt: return each(in.rt(), [&](uint32_t a, uint32_t b, uint32_t) { return truth(order(a, b, false) > 0); });
    case Op::fcmeq: return each(in.rt(), [&](uint32_t a, uint32_t b, uint32_t) { return truth(order(a, b, true) == 0); });
    case Op::fcmgt: return each(in.rt(), [&](uint32_t a, uint32_t b, uint32_t) { return truth(order(a, b, true) > 0); });

    // Whole numbers to singles: the number, divided by 2^scale (scale is 155 less the field). In units of 2^-300.
    case Op::csflt: case Op::cuflt: {
        const unsigned scale = 155 - in.u8();
        const bool is_signed = in.op == Op::csflt;
        return each(in.rt(), [&](uint32_t a, uint32_t, uint32_t) {
            const bool negative = is_signed && (a >> 31);
            const uint64_t magnitude = negative ? (uint64_t{1} << 32) - a : a;
            return to_single({negative, Long::of(magnitude, 300 - scale)});
        });
    }
    // Singles to whole numbers: times 2^scale (173 less the field), the fraction dropped, kept within the result's range.
    case Op::cflts: case Op::cfltu: {
        const unsigned scale = 173 - in.u8();
        const bool is_signed = in.op == Op::cflts;
        return each(in.rt(), [&](uint32_t a, uint32_t, uint32_t) -> uint32_t {
            if (is_zero_word(a)) return 0;
            const bool negative = (a >> 31) != 0;
            // mantissa * 2^(exponent - 150 + scale): above 2^40 it is past any limit; else a 64-bit number, shifted.
            const int up = static_cast<int>(exponent(a)) - 150 + static_cast<int>(scale);
            uint64_t whole;
            if (up > 16) whole = uint64_t{1} << 40;
            else if (up >= 0) whole = mantissa(a) << up;
            else whole = up < -63 ? 0 : mantissa(a) >> -up;
            if (!is_signed) return negative ? 0u : whole > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(whole);
            if (negative) return whole > 0x80000000ull ? 0x80000000u : static_cast<uint32_t>(0 - whole);
            return whole > 0x7fffffffull ? 0x7fffffffu : static_cast<uint32_t>(whole);
        });
    }

    case Op::frest:
        return each(in.rt(), [](uint32_t a, uint32_t, uint32_t) -> uint32_t {
            const uint32_t sign = a & 0x80000000u;
            if (is_zero_word(a)) return sign | 0x7fffffffu;
            if (exponent(a) >= 253) return 0;
            return sign | ((253 - exponent(a)) << 23) | table_entry(g_reciprocal, (a >> 18) & 31);
        });
    case Op::frsqest:
        return each(in.rt(), [](uint32_t a, uint32_t, uint32_t) -> uint32_t {
            if (is_zero_word(a)) return 0x7fffffffu;
            const int e = static_cast<int>(exponent(a)) - 127;
            const bool odd = ((e % 2) + 2) % 2 == 1;
            const int half = odd ? (e - 1) / 2 : e / 2;
            return (static_cast<uint32_t>(127 - half - 1) << 23) | table_entry(odd ? g_root_odd : g_root_even, (a >> 18) & 31);
        });
    // (1.base - 0.000step * Y) * 2^(exponent - 127): in units of 2^-32 before the exponent, base * 2^19 - step * Y.
    case Op::fi:
        return each(in.rt(), [](uint32_t a, uint32_t b, uint32_t) {
            const uint64_t base = 8192 + ((b >> 10) & 8191), step = b & 1023, y = a & 0x7ffff;
            return to_single({(b >> 31) != 0, Long::of(base * 524288 - step * y, 0)}, static_cast<int>(exponent(b)) - 127 - 32);
        });

    case Op::dfa: return doubles([](double x, double y) { return x + y; });
    case Op::dfs: return doubles([](double x, double y) { return x - y; });
    case Op::dfm: return doubles([](double x, double y) { return x * y; });
    case Op::fesd:
        for (unsigned i = 0; i < 2; ++i) {
            const uint32_t w = word_of(A, 2 * i);
            uint64_t bits;
            if (exponent(w) == 255 && (w & 0x7fffff)) bits = 0x7ff8000000000000ull;
            else if (exponent(w) == 255) bits = (uint64_t{w >> 31} << 63) | 0x7ff0000000000000ull;
            else if (exponent(w) == 0) bits = uint64_t{w >> 31} << 63;
            else bits = (uint64_t{w >> 31} << 63) | (uint64_t{exponent(w) + 896} << 52) | (uint64_t{w & 0x7fffff} << 29);
            set_word(out, 2 * i, static_cast<uint32_t>(bits >> 32));
            set_word(out, 2 * i + 1, static_cast<uint32_t>(bits));
        }
        return finish(in.rt());
    case Op::frds:
        for (unsigned i = 0; i < 2; ++i) {
            const uint64_t d = pair_of(A, i);
            set_word(out, 2 * i, is_nan64(d) ? 0x7fc00000u : std::bit_cast<uint32_t>(static_cast<float>(as_double(d))));
            set_word(out, 2 * i + 1, 0);
        }
        return finish(in.rt());
    default: return false;
    }
}

}  // namespace spu_difftest

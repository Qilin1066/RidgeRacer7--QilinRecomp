// The reference interpreter's floating point: integers only. No host
// floating-point operation is used anywhere in this file, so nothing here
// can share a mistake, or a library, with the runtime's fp.hpp.
//
// A finite number is a sign, an integer significand and a power of two.
// Sums, products, quotients and square roots are formed exactly (or with a
// "sticky" note that something non-zero lies below the last bit kept), and
// rounded once, to nearest with ties to even, to the format wanted: double,
// or single held in a double.
#include "ref.hpp"

namespace qilin::ref {

using ppu::Insn;
using ppu::Op;

namespace {

constexpr uint64_t kSignBit = uint64_t{1} << 63, kQuietBit = uint64_t{1} << 51, kExponent = uint64_t{0x7ff} << 52, kFraction = (uint64_t{1} << 52) - 1;
constexpr uint64_t kDefaultNan = kExponent | kQuietBit;

// ---- 128-bit unsigned integers ----
struct Wide {
    uint64_t high = 0, low = 0;
    bool zero() const { return !high && !low; }
    bool bit(unsigned n) const { return n < 64 ? (low >> n) & 1 : (high >> (n - 64)) & 1; }
};
Wide shl(Wide x, unsigned n) {
    if (n == 0) return x;
    if (n >= 128) return {};
    if (n >= 64) return {x.low << (n - 64), 0};
    return {(x.high << n) | (x.low >> (64 - n)), x.low << n};
}
Wide shr(Wide x, unsigned n) {
    if (n == 0) return x;
    if (n >= 128) return {};
    if (n >= 64) return {0, x.high >> (n - 64)};
    return {x.high >> n, (x.low >> n) | (x.high << (64 - n))};
}
bool low_bits_set(Wide x, unsigned n) {  // any of the n lowest bits
    if (n == 0) return false;
    if (n >= 128) return !x.zero();
    return !shl(x, 128 - n).zero();
}
Wide add(Wide a, Wide b) {
    Wide r{a.high + b.high, a.low + b.low};
    if (r.low < a.low) ++r.high;
    return r;
}
Wide sub(Wide a, Wide b) {
    Wide r{a.high - b.high, a.low - b.low};
    if (a.low < b.low) --r.high;
    return r;
}
bool less(Wide a, Wide b) { return a.high != b.high ? a.high < b.high : a.low < b.low; }
unsigned top_bit(Wide x) {  // the position of the highest set bit; x is not zero
    unsigned n = 127;
    while (!x.bit(n)) --n;
    return n;
}
Wide multiply(uint64_t a, uint64_t b) {
    const uint64_t a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t middle = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    return {p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32), (middle << 32) | (p00 & 0xffffffffu)};
}

// ---- numbers ----
enum class Kind { zero, finite, infinite, nan };
struct Number {
    Kind kind = Kind::zero;
    bool negative = false;
    uint64_t significand = 0;  // finite: the value is significand * 2^exponent
    int exponent = 0;
};
Number unpack(uint64_t b) {
    Number n;
    n.negative = b >> 63;
    const unsigned e = static_cast<unsigned>((b >> 52) & 0x7ff);
    const uint64_t f = b & kFraction;
    if (e == 0x7ff) n.kind = f ? Kind::nan : Kind::infinite;
    else if (e == 0 && f == 0) n.kind = Kind::zero;
    else {
        n.kind = Kind::finite;
        n.significand = e ? (f | (uint64_t{1} << 52)) : f;
        n.exponent = static_cast<int>(e ? e : 1) - 1075;
    }
    return n;
}
uint64_t zero(bool negative) { return negative ? kSignBit : 0; }
uint64_t infinity(bool negative) { return (negative ? kSignBit : 0) | kExponent; }

// The value (significand + a fraction in (0,1) if `sticky`) * 2^exponent,
// rounded to nearest (ties to even) in the format, as a double's bits.
uint64_t round_pack(bool negative, Wide significand, int exponent, bool sticky, bool single) {
    if (significand.zero() && !sticky) return zero(negative);
    const int precision = single ? 24 : 53, lowest = single ? -126 : -1022, highest = single ? 127 : 1023;
    int leading = significand.zero() ? exponent - 1 : exponent + static_cast<int>(top_bit(significand));  // the exponent of the top bit
    // One unit in the last place kept: fewer bits are kept below the format's smallest normal.
    const int unit = (leading < lowest ? lowest : leading) - (precision - 1);
    const int drop = unit - exponent;  // bits of the significand below the unit
    uint64_t n;
    bool half = false, rest = sticky;
    if (drop <= 0) {
        n = shl(significand, static_cast<unsigned>(-drop)).low;
    } else {
        n = drop >= 128 ? 0 : shr(significand, static_cast<unsigned>(drop)).low;
        half = drop <= 128 && significand.bit(static_cast<unsigned>(drop - 1));
        rest = rest || low_bits_set(significand, static_cast<unsigned>(drop > 128 ? 128 : drop - 1));
    }
    if (half && (rest || (n & 1))) ++n;
    if (n == 0) return zero(negative);
    // n * 2^unit, exactly, as a double.
    int unit_out = unit;
    while (n >> 53) {  // (only 2^53, from a carry)
        n >>= 1;
        ++unit_out;
    }
    unsigned top = 63;
    while (!((n >> top) & 1)) --top;
    leading = unit_out + static_cast<int>(top);
    if (leading > highest) return infinity(negative);
    if (leading < -1022) return zero(negative) | (n << (unit_out + 1074));  // a double's own denormal
    const uint64_t fraction = (top <= 52 ? n << (52 - top) : n >> (top - 52)) & kFraction;
    return zero(negative) | (static_cast<uint64_t>(leading + 1023) << 52) | fraction;
}

uint64_t quieted(uint64_t nan) { return nan | kQuietBit; }
bool is_nan(uint64_t b) { return (b & kExponent) == kExponent && (b & kFraction); }
// The first NaN among the operands in the architecture's order, quieted; else the default.
uint64_t nan_result(uint64_t a, uint64_t b, uint64_t c) { return is_nan(a) ? quieted(a) : is_nan(b) ? quieted(b) : is_nan(c) ? quieted(c) : kDefaultNan; }

// a * c + b, with b's sign flipped if `subtract`, rounded once. The order of the operands for a NaN is a, b, c.
uint64_t fused(uint64_t a_bits, uint64_t c_bits, uint64_t b_bits, bool subtract, bool single) {
    const Number a = unpack(a_bits), c = unpack(c_bits);
    Number b = unpack(b_bits);
    if (a.kind == Kind::nan || b.kind == Kind::nan || c.kind == Kind::nan) return nan_result(a_bits, b_bits, c_bits);
    if (subtract) b.negative = !b.negative;
    const bool product_negative = a.negative != c.negative;
    const bool product_infinite = a.kind == Kind::infinite || c.kind == Kind::infinite;
    if (product_infinite && (a.kind == Kind::zero || c.kind == Kind::zero)) return kDefaultNan;  // infinity times zero
    if (product_infinite) {
        if (b.kind == Kind::infinite && b.negative != product_negative) return kDefaultNan;  // infinity less infinity
        return infinity(product_negative);
    }
    if (b.kind == Kind::infinite) return infinity(b.negative);
    const bool product_zero = a.kind == Kind::zero || c.kind == Kind::zero;
    if (product_zero && b.kind == Kind::zero) return zero(product_negative && b.negative);  // -0 only from -0 + -0
    if (product_zero) return round_pack(b.negative, {0, b.significand}, b.exponent, false, single);
    Wide product = multiply(a.significand, c.significand);
    int product_exponent = a.exponent + c.exponent;
    if (b.kind == Kind::zero) return round_pack(product_negative, product, product_exponent, false, single);

    // Both to a top bit at 126, then the smaller in magnitude shifted down to the larger's exponent.
    Wide addend{0, b.significand};
    int addend_exponent = b.exponent;
    const unsigned up_product = 126 - top_bit(product), up_addend = 126 - top_bit(addend);
    product = shl(product, up_product);
    product_exponent -= static_cast<int>(up_product);
    addend = shl(addend, up_addend);
    addend_exponent -= static_cast<int>(up_addend);
    const bool product_first = product_exponent != addend_exponent ? product_exponent > addend_exponent : !less(product, addend);
    const Wide big = product_first ? product : addend, small = product_first ? addend : product;
    const int big_exponent = product_first ? product_exponent : addend_exponent;
    const int distance = big_exponent - (product_first ? addend_exponent : product_exponent);
    const bool big_negative = product_first ? product_negative : b.negative;
    const bool same_sign = product_negative == b.negative;
    const bool lost = low_bits_set(small, static_cast<unsigned>(distance > 128 ? 128 : distance));
    const Wide shifted = shr(small, static_cast<unsigned>(distance > 128 ? 128 : distance));
    if (same_sign) return round_pack(big_negative, add(big, shifted), big_exponent, lost, single);
    // big - (shifted + a fraction): one less, and a fraction more.
    Wide difference = sub(big, shifted);
    if (lost) difference = sub(difference, {0, 1});
    if (difference.zero() && !lost) return zero(false);  // an exact zero from opposite signs is +0
    return round_pack(big_negative, difference, big_exponent, lost, single);
}

uint64_t divide(uint64_t a_bits, uint64_t b_bits, bool single) {
    Number a = unpack(a_bits), b = unpack(b_bits);
    if (a.kind == Kind::nan || b.kind == Kind::nan) return nan_result(a_bits, b_bits, 0);
    const bool negative = a.negative != b.negative;
    if (a.kind == Kind::infinite) return b.kind == Kind::infinite ? kDefaultNan : infinity(negative);
    if (b.kind == Kind::infinite) return zero(negative);
    if (b.kind == Kind::zero) return a.kind == Kind::zero ? kDefaultNan : infinity(negative);
    if (a.kind == Kind::zero) return zero(negative);
    // Both significands to 53 bits, then 70 more bits of quotient by long division.
    while (!(a.significand >> 52)) a.significand <<= 1, --a.exponent;
    while (!(b.significand >> 52)) b.significand <<= 1, --b.exponent;
    const Wide dividend = shl({0, a.significand}, 70);
    Wide quotient;
    uint64_t remainder = 0;
    for (int i = 127; i >= 0; --i) {
        remainder = (remainder << 1) | (dividend.bit(static_cast<unsigned>(i)) ? 1 : 0);
        quotient = shl(quotient, 1);
        if (remainder >= b.significand) {
            remainder -= b.significand;
            quotient.low |= 1;
        }
    }
    return round_pack(negative, quotient, a.exponent - b.exponent - 70, remainder != 0, single);
}

uint64_t square_root(uint64_t b_bits, bool single) {
    Number b = unpack(b_bits);
    if (b.kind == Kind::nan) return nan_result(b_bits, 0, 0);
    if (b.kind == Kind::zero) return b_bits;  // of -0, -0
    if (b.negative) return kDefaultNan;
    if (b.kind == Kind::infinite) return b_bits;
    while (!(b.significand >> 52)) b.significand <<= 1, --b.exponent;
    // An even exponent and about 126 bits, then the integer square root a bit pair at a time.
    const unsigned up = (b.exponent - 72) % 2 == 0 ? 72 : 73;
    const Wide radicand = shl({0, b.significand}, up);
    Wide remainder, root;
    for (int pair = 63; pair >= 0; --pair) {
        remainder = shl(remainder, 2);
        remainder.low |= (radicand.bit(static_cast<unsigned>(2 * pair + 1)) ? 2u : 0u) | (radicand.bit(static_cast<unsigned>(2 * pair)) ? 1u : 0u);
        root = shl(root, 1);
        const Wide trial = add(shl(root, 1), {0, 1});
        if (!less(remainder, trial)) {
            remainder = sub(remainder, trial);
            root.low |= 1;
        }
    }
    return round_pack(false, root, (b.exponent - static_cast<int>(up)) / 2, !remainder.zero(), single);
}

// To an integer, toward zero or to nearest (ties to even), saturating at `bits` wide; a NaN gives the most negative.
uint64_t to_integer(uint64_t b_bits, unsigned bits, bool truncate) {
    const Number b = unpack(b_bits);
    const uint64_t most_negative = uint64_t{1} << (bits - 1), most_positive = most_negative - 1;
    if (b.kind == Kind::nan) return most_negative;
    if (b.kind == Kind::infinite) return b.negative ? most_negative : most_positive;
    if (b.kind == Kind::zero) return 0;
    uint64_t magnitude;
    if (b.exponent >= 0) {
        if (b.exponent > 11) return b.negative ? most_negative : most_positive;  // 2^64 or more
        magnitude = b.significand << b.exponent;
        if (magnitude >> b.exponent != b.significand) return b.negative ? most_negative : most_positive;
    } else {
        const unsigned down = static_cast<unsigned>(-b.exponent);
        magnitude = down >= 64 ? 0 : b.significand >> down;
        if (!truncate) {
            const bool half = down <= 64 && ((b.significand >> (down - 1)) & 1);
            const bool rest = down > 64 ? true : (b.significand & ((uint64_t{1} << (down - 1)) - 1)) != 0;
            if (half && (rest || (magnitude & 1))) ++magnitude;
        }
    }
    if (b.negative) return magnitude > most_negative ? most_negative : (~magnitude + 1) & (bits == 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1);
    return magnitude > most_positive ? most_positive : magnitude;
}

// A compare's field: less, greater, equal, unordered.
uint32_t compare(uint64_t a_bits, uint64_t b_bits) {
    const Number a = unpack(a_bits), b = unpack(b_bits);
    if (a.kind == Kind::nan || b.kind == Kind::nan) return 1;
    if (a.kind == Kind::zero && b.kind == Kind::zero) return 2;
    if (a_bits == b_bits) return 2;
    // Order by sign, then by magnitude (the bits without the sign order magnitudes).
    const bool a_negative = a.negative && a.kind != Kind::zero, b_negative = b.negative && b.kind != Kind::zero;
    const uint64_t a_magnitude = a.kind == Kind::zero ? 0 : a_bits & ~kSignBit, b_magnitude = b.kind == Kind::zero ? 0 : b_bits & ~kSignBit;
    bool less_than;
    if (a_negative != b_negative) less_than = a_negative;
    else less_than = a_negative ? a_magnitude > b_magnitude : a_magnitude < b_magnitude;
    return less_than ? 8 : 4;
}

// The architecture's conversion of a single in memory to a register's double.
uint64_t single_to_double(uint32_t word) {
    const uint64_t sign = uint64_t{word >> 31} << 63;
    const unsigned exponent = (word >> 23) & 0xff;
    const uint64_t fraction = word & 0x7fffff;
    if (exponent > 0 && exponent < 255) return sign | (uint64_t{exponent + 896} << 52) | (fraction << 29);
    if (exponent == 0 && fraction != 0) {  // a denormal: normalise it
        int e = -126;
        uint64_t f = fraction << 29;  // 0 || fraction || zeros, 53 bits
        while (!(f >> 52)) f <<= 1, --e;
        return sign | (static_cast<uint64_t>(e + 1023) << 52) | (f & kFraction);
    }
    return sign | (exponent ? kExponent : 0) | (fraction << 29);  // zero, infinity, a NaN bit for bit
}
// ... and of a register to a single in memory: bit selection, no rounding.
uint32_t double_to_single(uint64_t b) {
    const unsigned exponent = static_cast<unsigned>((b >> 52) & 0x7ff);
    const uint32_t sign = static_cast<uint32_t>(b >> 63) << 31;
    if (exponent > 896 || (b << 1) == 0) return sign | (static_cast<uint32_t>((b >> 62) & 1) << 30) | static_cast<uint32_t>((b >> 29) & 0x3fffffff);
    if (exponent >= 874) {
        uint64_t f = (b & kFraction) | (uint64_t{1} << 52);
        for (int e = static_cast<int>(exponent) - 1023; e < -126; ++e) f >>= 1;
        return sign | static_cast<uint32_t>((f >> 29) & 0x7fffff);
    }
    return sign;  // undefined by the architecture: fp.hpp's choice
}

}  // namespace

namespace soft {
uint64_t fused(uint64_t a, uint64_t c, uint64_t b, bool subtract, bool single) { return ref::fused(a, c, b, subtract, single); }
uint64_t from_integer(bool negative, uint64_t magnitude, int exponent, bool single) { return round_pack(negative, {0, magnitude}, exponent, false, single); }
uint64_t to_integer(uint64_t value, unsigned bits, bool truncate) { return ref::to_integer(value, bits, truncate); }
uint32_t compare(uint64_t a, uint64_t b) { return ref::compare(a, b); }
uint64_t single_to_double(uint32_t word) { return ref::single_to_double(word); }
uint32_t double_to_single(uint64_t value) { return ref::double_to_single(value); }
}  // namespace soft

bool step_fp(State& s, const Insn& in) {
    const uint32_t w = in.word;
    const unsigned rt = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31, rc = (w >> 6) & 31;
    uint64_t* const f = s.fpr;
    uint64_t* const r = s.gpr;
    const uint64_t displacement = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(w & 0xffff)));
    const uint64_t ea_d = (ra ? r[ra] : 0) + displacement, ea_x = (ra ? r[ra] : 0) + r[rb];
    const auto read = [&](uint64_t ea, unsigned bytes) {
        uint64_t v = 0;
        for (unsigned i = 0; i < bytes; ++i) v = (v << 8) | s.mem[static_cast<uint32_t>(ea) + i];
        return v;
    };
    const auto write = [&](uint64_t ea, unsigned bytes, uint64_t v) {
        for (unsigned i = 0; i < bytes; ++i) s.mem[static_cast<uint32_t>(ea) + i] = static_cast<uint8_t>(v >> (8 * (bytes - 1 - i)));
    };
    const auto negated = [](uint64_t v) { return is_nan(v) ? v : v ^ kSignBit; };  // the negative forms leave a NaN alone
    switch (in.op) {
    case Op::lfs: f[rt] = single_to_double(static_cast<uint32_t>(read(ea_d, 4))); return true;
    case Op::lfsu: f[rt] = single_to_double(static_cast<uint32_t>(read(ea_d, 4))); r[ra] = ea_d; return true;
    case Op::lfsx: f[rt] = single_to_double(static_cast<uint32_t>(read(ea_x, 4))); return true;
    case Op::lfsux: f[rt] = single_to_double(static_cast<uint32_t>(read(ea_x, 4))); r[ra] = ea_x; return true;
    case Op::lfd: f[rt] = read(ea_d, 8); return true;
    case Op::lfdu: f[rt] = read(ea_d, 8); r[ra] = ea_d; return true;
    case Op::lfdx: f[rt] = read(ea_x, 8); return true;
    case Op::lfdux: f[rt] = read(ea_x, 8); r[ra] = ea_x; return true;
    case Op::stfs: write(ea_d, 4, double_to_single(f[rt])); return true;
    case Op::stfsu: write(ea_d, 4, double_to_single(f[rt])); r[ra] = ea_d; return true;
    case Op::stfsx: write(ea_x, 4, double_to_single(f[rt])); return true;
    case Op::stfsux: write(ea_x, 4, double_to_single(f[rt])); r[ra] = ea_x; return true;
    case Op::stfd: write(ea_d, 8, f[rt]); return true;
    case Op::stfdu: write(ea_d, 8, f[rt]); r[ra] = ea_d; return true;
    case Op::stfdx: write(ea_x, 8, f[rt]); return true;
    case Op::stfdux: write(ea_x, 8, f[rt]); r[ra] = ea_x; return true;
    case Op::stfiwx: write(ea_x, 4, f[rt] & 0xffffffffu); return true;

    case Op::fmr: f[rt] = f[rb]; return true;

    // Move From FPSCR: FPSCR into the low 32 bits of frD; the high 32 bits are undefined, taken as zero.
    case Op::mffs: f[rt] = s.fpscr; return true;
    // Move To FPSCR Fields: for each i in 0..7 with FM bit i set (FM's most significant bit is i = 0), FPSCR field i (bits
    // 4i..4i+3, bit 0 the most significant) is set from the same bits of frB's low word. FEX and VX can't be set
    // explicitly: FEX is the OR of each exception bit ANDed with its enable (VX/VE, OX/OE, UX/UE, ZX/ZE, XX/XE), VX the OR
    // of the nine VX* bits (8-12, 21-23 and 7).
    case Op::mtfsf: {
        const uint32_t from = static_cast<uint32_t>(f[rb]);
        uint32_t v = s.fpscr;
        for (unsigned i = 0; i < 8; ++i) {
            if (!((in.fm() >> (7 - i)) & 1)) continue;
            for (unsigned bit = 4 * i; bit < 4 * i + 4; ++bit) {
                const uint32_t m = 1u << (31 - bit);
                v = (v & ~m) | (from & m);
            }
        }
        const auto bit = [&](unsigned n) { return (v >> (31 - n)) & 1; };
        unsigned vx = 0;
        for (const unsigned n : {7u, 8u, 9u, 10u, 11u, 12u, 21u, 22u, 23u}) vx |= bit(n);
        v = (v & ~(1u << 29)) | (vx << 29);  // VX is bit 2
        // VX bit 2 with VE bit 24, OX 3 with OE 25, UX 4 with UE 26, ZX 5 with ZE 27, XX 6 with XE 28.
        const unsigned fex = (bit(2) & bit(24)) | (bit(3) & bit(25)) | (bit(4) & bit(26)) | (bit(5) & bit(27)) | (bit(6) & bit(28));
        v = (v & ~(1u << 30)) | (fex << 30);  // FEX is bit 1
        s.fpscr = v;
        return true;
    }
    case Op::fneg: f[rt] = f[rb] ^ kSignBit; return true;
    case Op::fabs: f[rt] = f[rb] & ~kSignBit; return true;
    case Op::fnabs: f[rt] = f[rb] | kSignBit; return true;

    // a + b is a * 1 + b; a * c is a * c + (-0), which changes neither the value nor a zero's sign.
    case Op::fadd: f[rt] = is_nan(f[ra]) || is_nan(f[rb]) ? nan_result(f[ra], f[rb], 0) : fused(f[ra], 0x3ff0000000000000ull, f[rb], false, false); return true;
    case Op::fadds: f[rt] = is_nan(f[ra]) || is_nan(f[rb]) ? nan_result(f[ra], f[rb], 0) : fused(f[ra], 0x3ff0000000000000ull, f[rb], false, true); return true;
    case Op::fsub: f[rt] = is_nan(f[ra]) || is_nan(f[rb]) ? nan_result(f[ra], f[rb], 0) : fused(f[ra], 0x3ff0000000000000ull, f[rb], true, false); return true;
    case Op::fsubs: f[rt] = is_nan(f[ra]) || is_nan(f[rb]) ? nan_result(f[ra], f[rb], 0) : fused(f[ra], 0x3ff0000000000000ull, f[rb], true, true); return true;
    case Op::fmul: f[rt] = is_nan(f[ra]) || is_nan(f[rc]) ? nan_result(f[ra], f[rc], 0) : fused(f[ra], f[rc], kSignBit, false, false); return true;
    case Op::fmuls: f[rt] = is_nan(f[ra]) || is_nan(f[rc]) ? nan_result(f[ra], f[rc], 0) : fused(f[ra], f[rc], kSignBit, false, true); return true;
    case Op::fdiv: f[rt] = divide(f[ra], f[rb], false); return true;
    case Op::fdivs: f[rt] = divide(f[ra], f[rb], true); return true;
    case Op::fsqrt: f[rt] = square_root(f[rb], false); return true;
    case Op::fsqrts: f[rt] = square_root(f[rb], true); return true;
    case Op::fmadd: f[rt] = fused(f[ra], f[rc], f[rb], false, false); return true;
    case Op::fmadds: f[rt] = fused(f[ra], f[rc], f[rb], false, true); return true;
    case Op::fmsub: f[rt] = fused(f[ra], f[rc], f[rb], true, false); return true;
    case Op::fmsubs: f[rt] = fused(f[ra], f[rc], f[rb], true, true); return true;
    case Op::fnmadd: f[rt] = negated(fused(f[ra], f[rc], f[rb], false, false)); return true;
    case Op::fnmadds: f[rt] = negated(fused(f[ra], f[rc], f[rb], false, true)); return true;
    case Op::fnmsub: f[rt] = negated(fused(f[ra], f[rc], f[rb], true, false)); return true;
    case Op::fnmsubs: f[rt] = negated(fused(f[ra], f[rc], f[rb], true, true)); return true;

    case Op::fsel: {
        const Number a = unpack(f[ra]);
        f[rt] = (a.kind != Kind::nan && (!a.negative || a.kind == Kind::zero)) ? f[rc] : f[rb];
        return true;
    }
    case Op::fcmpu: case Op::fcmpo: {
        const unsigned shift = 28 - 4 * ((w >> 23) & 7);
        s.cr = (s.cr & ~(uint32_t{15} << shift)) | (compare(f[ra], f[rb]) << shift);
        return true;
    }
    case Op::frsp: {
        const Number b = unpack(f[rb]);
        if (b.kind == Kind::nan) f[rt] = quieted(f[rb]) & ~uint64_t{0x1fffffff};  // what of the payload a single holds
        else if (b.kind != Kind::finite) f[rt] = f[rb];
        else f[rt] = round_pack(b.negative, {0, b.significand}, b.exponent, false, true);
        return true;
    }
    case Op::fcfid: {
        const bool negative = f[rb] >> 63;
        f[rt] = round_pack(negative, {0, negative ? ~f[rb] + 1 : f[rb]}, 0, false, false);
        return true;
    }
    case Op::fctiw: f[rt] = 0xfff8000000000000ull | to_integer(f[rb], 32, false); return true;  // the upper half: fp.hpp's choice
    case Op::fctiwz: f[rt] = 0xfff8000000000000ull | to_integer(f[rb], 32, true); return true;
    case Op::fctid: f[rt] = to_integer(f[rb], 64, false); return true;
    case Op::fctidz: f[rt] = to_integer(f[rb], 64, true); return true;
    default: return step_vmx(s, in);
    }
}

}  // namespace qilin::ref

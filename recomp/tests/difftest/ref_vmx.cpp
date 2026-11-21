// The reference interpreter's VMX: on sixteen bytes in the guest's order,
// with single-precision arithmetic by ref_fp.cpp's integer soft-float. Like
// the rest of the reference it is written from the definitions and shares
// only the decoder's naming of the opcode with the lifter and its runtime.
#include "ref.hpp"

namespace qilin::ref {

using ppu::Insn;
using ppu::Op;

namespace {

constexpr bool kNonJava = true;  // the mode vmx.hpp assumes: denormals are zeros

struct Bytes16 {
    uint8_t b[16];
};
uint32_t word(const uint8_t* v, unsigned i) { return uint32_t{v[4 * i]} << 24 | uint32_t{v[4 * i + 1]} << 16 | uint32_t{v[4 * i + 2]} << 8 | v[4 * i + 3]; }
void set_word(uint8_t* v, unsigned i, uint32_t w) {
    for (unsigned k = 0; k < 4; ++k) v[4 * i + k] = static_cast<uint8_t>(w >> (24 - 8 * k));
}

bool is_nan(uint32_t w) { return ((w >> 23) & 0xff) == 0xff && (w & 0x7fffff); }
uint32_t flushed(uint32_t w) { return kNonJava && ((w >> 23) & 0xff) == 0 ? w & 0x80000000u : w; }  // a denormal (or a zero) to a zero
// The first NaN among up to three operands, quieted; 0 if none is a NaN.
uint32_t first_nan(uint32_t a, uint32_t b, uint32_t c) {
    for (const uint32_t w : {a, b, c})
        if (is_nan(w)) return w | 0x00400000u;
    return 0;
}
// A soft-float result (a single held in double format, or the default NaN) as a single's bits, flushed.
uint32_t packed(uint64_t result) { return flushed(soft::double_to_single(result)); }

// a * c + b (or - b), rounded once to single; `negate` for the negative forms.
uint32_t fused(uint32_t a, uint32_t c, uint32_t b, bool subtract, bool negate) {
    if (const uint32_t nan = first_nan(a, b, c)) return nan;
    const uint64_t r = soft::fused(soft::single_to_double(flushed(a)), soft::single_to_double(flushed(c)), soft::single_to_double(flushed(b)), subtract, true);
    const uint32_t w = packed(r);
    return negate && !is_nan(w) ? w ^ 0x80000000u : w;
}
constexpr uint32_t kOne = 0x3f800000u;

}  // namespace

bool step_vmx(State& s, const Insn& in) {
    const uint32_t w = in.word;
    const unsigned vt = (w >> 21) & 31, va = (w >> 16) & 31, vb = (w >> 11) & 31, vc = (w >> 6) & 31;
    // Operands are copied: the target may be one of them.
    Bytes16 A, B, C, T{};
    for (unsigned i = 0; i < 16; ++i) A.b[i] = s.vr[va][i], B.b[i] = s.vr[vb][i], C.b[i] = s.vr[vc][i];
    const uint64_t ea = (va ? s.gpr[va] : 0) + s.gpr[vb];  // for the loads and stores, (rA|0) + rB
    const auto done = [&] {
        for (unsigned i = 0; i < 16; ++i) s.vr[vt][i] = T.b[i];
        return true;
    };
    const auto words = [&](auto&& element) {
        for (unsigned i = 0; i < 4; ++i) set_word(T.b, i, element(word(A.b, i), word(B.b, i), word(C.b, i)));
        return done();
    };
    // A recording compare: CR6 is "all true", 0, "none true", 0.
    const auto compared = [&] {
        done();
        if ((w >> 10) & 1) {
            bool all = true, none = true;
            for (unsigned i = 0; i < 16; ++i) all = all && T.b[i] == 0xff, none = none && T.b[i] == 0;
            s.cr = (s.cr & ~0xf0u) | (all ? 0x80u : 0u) | (none ? 0x20u : 0u);
        }
        return true;
    };
    switch (in.op) {
    case Op::lvx:
        for (unsigned i = 0; i < 16; ++i) T.b[i] = s.mem[(static_cast<uint32_t>(ea) & ~15u) + i];
        return done();
    case Op::stvx:
        for (unsigned i = 0; i < 16; ++i) s.mem[(static_cast<uint32_t>(ea) & ~15u) + i] = s.vr[vt][i];
        return true;
    case Op::lvsl:
        for (unsigned i = 0; i < 16; ++i) T.b[i] = static_cast<uint8_t>((ea & 15) + i);
        return done();
    case Op::lvsr:
        for (unsigned i = 0; i < 16; ++i) T.b[i] = static_cast<uint8_t>(16 - (ea & 15) + i);
        return done();
    case Op::vand: for (unsigned i = 0; i < 16; ++i) T.b[i] = A.b[i] & B.b[i]; return done();
    case Op::vandc: for (unsigned i = 0; i < 16; ++i) T.b[i] = A.b[i] & static_cast<uint8_t>(~B.b[i]); return done();
    case Op::vor: for (unsigned i = 0; i < 16; ++i) T.b[i] = A.b[i] | B.b[i]; return done();
    case Op::vxor: for (unsigned i = 0; i < 16; ++i) T.b[i] = A.b[i] ^ B.b[i]; return done();
    case Op::vnor: for (unsigned i = 0; i < 16; ++i) T.b[i] = static_cast<uint8_t>(~(A.b[i] | B.b[i])); return done();
    case Op::vsel: for (unsigned i = 0; i < 16; ++i) T.b[i] = static_cast<uint8_t>((A.b[i] & ~C.b[i]) | (B.b[i] & C.b[i])); return done();
    case Op::vperm:
        for (unsigned i = 0; i < 16; ++i) {
            const unsigned n = C.b[i] & 0x1f;
            T.b[i] = n < 16 ? A.b[n] : B.b[n - 16];
        }
        return done();
    case Op::vsldoi: {
        const unsigned shift = (w >> 6) & 15;
        uint8_t both[32];
        for (unsigned i = 0; i < 16; ++i) both[i] = A.b[i], both[16 + i] = B.b[i];
        for (unsigned i = 0; i < 16; ++i) T.b[i] = both[shift + i];
        return done();
    }
    case Op::vmrghw: set_word(T.b, 0, word(A.b, 0)), set_word(T.b, 1, word(B.b, 0)), set_word(T.b, 2, word(A.b, 1)), set_word(T.b, 3, word(B.b, 1)); return done();
    case Op::vmrglw: set_word(T.b, 0, word(A.b, 2)), set_word(T.b, 1, word(B.b, 2)), set_word(T.b, 2, word(A.b, 3)), set_word(T.b, 3, word(B.b, 3)); return done();
    case Op::vspltw: for (unsigned i = 0; i < 4; ++i) set_word(T.b, i, word(B.b, va & 3)); return done();
    case Op::vspltisw: {
        const uint32_t value = (va & 16) ? (0xffffffe0u | va) : va;  // the five-bit field, sign-extended
        for (unsigned i = 0; i < 4; ++i) set_word(T.b, i, value);
        return done();
    }
    case Op::vadduwm: return words([](uint32_t a, uint32_t b, uint32_t) { return a + b; });
    case Op::vcmpequw:
        for (unsigned i = 0; i < 4; ++i) set_word(T.b, i, word(A.b, i) == word(B.b, i) ? 0xffffffffu : 0);
        return compared();
    case Op::vcmpeqfp:
        for (unsigned i = 0; i < 4; ++i) {
            const uint32_t a = flushed(word(A.b, i)), b = flushed(word(B.b, i));
            const bool equal = !is_nan(a) && !is_nan(b) && soft::compare(soft::single_to_double(a), soft::single_to_double(b)) == 2;
            set_word(T.b, i, equal ? 0xffffffffu : 0);
        }
        return compared();
    case Op::vaddfp: return words([](uint32_t a, uint32_t b, uint32_t) { return fused(a, kOne, b, false, false); });
    case Op::vsubfp: return words([](uint32_t a, uint32_t b, uint32_t) { return fused(a, kOne, b, true, false); });
    case Op::vmaddfp: return words([](uint32_t a, uint32_t b, uint32_t c) { return fused(a, c, b, false, false); });
    case Op::vnmsubfp: return words([](uint32_t a, uint32_t b, uint32_t c) { return fused(a, c, b, true, true); });
    case Op::vmaxfp: case Op::vminfp: {
        const bool larger = in.op == Op::vmaxfp;
        return words([larger](uint32_t a, uint32_t b, uint32_t) {
            if (const uint32_t nan = first_nan(a, b, 0)) return nan;
            a = flushed(a), b = flushed(b);
            const uint32_t order = soft::compare(soft::single_to_double(a), soft::single_to_double(b));  // 8 less, 4 greater, 2 equal
            if (order == 2) return larger ? (a & b) : (a | b);  // zeros of both signs: +0 is the larger
            return (order == 4) == larger ? a : b;
        });
    }
    // The estimates, as the runtime states them: the correctly rounded 1/x and 1/sqrt(x). Here by whole numbers only.
    case Op::vrefp:
        return words([](uint32_t, uint32_t b, uint32_t) -> uint32_t {
            if (is_nan(b)) return b | 0x00400000u;
            b = flushed(b);
            const uint32_t sign = b & 0x80000000u;
            if ((b & 0x7fffffffu) == 0) return sign | 0x7f800000u;
            if ((b & 0x7fffffffu) == 0x7f800000u) return sign;
            const uint64_t x = (b & 0x007fffffu) | 0x00800000u;  // the value is x * 2^ex
            const int ex = static_cast<int>((b >> 23) & 0xff) - 150;
            const uint64_t top = uint64_t{1} << 50, quotient = top / x;  // 26 or 27 digits of 2^50 / x
            const uint64_t sticky = top % x ? 1 : 0;
            return packed(soft::from_integer(sign != 0, quotient << 1 | sticky, -51 - ex, true));
        });
    case Op::vrsqrtefp:
        return words([](uint32_t, uint32_t b, uint32_t) -> uint32_t {
            if (is_nan(b)) return b | 0x00400000u;
            b = flushed(b);
            if ((b & 0x7fffffffu) == 0) return (b & 0x80000000u) | 0x7f800000u;
            if (b >> 31) return 0x7fc00000u;
            if (b == 0x7f800000u) return 0;
            // x = m * 2^e with e even and m of 24 or 25 digits; then 1/sqrt(x) = 2^(-e/2) / sqrt(m), and
            // 2^37 / sqrt(m) is found by its square: the largest n with n * n * m <= 2^74.
            uint64_t m = (b & 0x007fffffu) | 0x00800000u;
            int e = static_cast<int>((b >> 23) & 0xff) - 150;
            if (e & 1) m <<= 1, e -= 1;
            // n * n * m against 2^74, in two halves: n below 2^26, so n * n below 2^52 and the product below 2^77.
            const auto over = [m](uint64_t n, bool& exact) {
                const uint64_t square = n * n;
                const uint64_t low = (square & 0xffffff) * m, high = (square >> 24) * m + (low >> 24);  // high * 2^24 + low's bottom 24 digits
                exact = high == (uint64_t{1} << 50) && (low & 0xffffff) == 0;
                return high > (uint64_t{1} << 50) || (high == (uint64_t{1} << 50) && (low & 0xffffff) != 0);
            };
            uint64_t n = 0;
            bool exact = false;
            for (uint64_t bit = uint64_t{1} << 26; bit; bit >>= 1) {
                bool e2;
                if (!over(n | bit, e2)) n |= bit;
            }
            over(n, exact);
            return packed(soft::from_integer(false, n << 1 | (exact ? 0 : 1), -38 - e / 2, true));
        });
    case Op::vcfsx:
        return words([va](uint32_t, uint32_t b, uint32_t) {
            const bool negative = b >> 31;
            return soft::double_to_single(soft::from_integer(negative, negative ? ~b + 1 : b, -static_cast<int>(va), true));
        });
    case Op::vctsxs:
        return words([va](uint32_t, uint32_t b, uint32_t) -> uint32_t {
            b = flushed(b);
            if (is_nan(b)) return 0;
            uint64_t d = soft::single_to_double(b);
            if ((d << 1) != 0 && ((d >> 52) & 0x7ff) != 0x7ff) d += uint64_t{va} << 52;  // times 2^scale: the exponent, raised
            return static_cast<uint32_t>(soft::to_integer(d, 32, true));
        });
    default: return false;
    }
}

}  // namespace qilin::ref

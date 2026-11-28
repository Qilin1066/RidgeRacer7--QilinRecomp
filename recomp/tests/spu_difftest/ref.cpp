// The SPU reference interpreter for the differential test.
//
// Written apart from the lifter and from runtime/include/qilin/rt/spu.hpp,
// from the instruction set's definition, and on purpose in another shape:
// a register here is sixteen bytes in the SPU's order, and every operation
// goes through `get` and `set` of an element by its width and number, or
// through the register as one 128-bit number in two halves. It reads only
// the decoder.
#include "ref.hpp"

#include <cstring>
#include <vector>

namespace spu_difftest {

using qilin::spu::Insn;
using qilin::spu::Op;

namespace {

using Reg = uint8_t[16];

// Element `index` of `width` bytes (1, 2, 4 or 8), the most significant byte first.
uint64_t get(const uint8_t* r, unsigned width, unsigned index) {
    uint64_t v = 0;
    for (unsigned i = 0; i < width; ++i) v = (v << 8) | r[index * width + i];
    return v;
}
void set(uint8_t* r, unsigned width, unsigned index, uint64_t v) {
    for (unsigned i = 0; i < width; ++i) r[index * width + i] = static_cast<uint8_t>(v >> (8 * (width - 1 - i)));
}
int64_t sign(uint64_t v, unsigned bits) {
    const uint64_t top = uint64_t{1} << (bits - 1);
    v &= (top << 1) - 1;
    return static_cast<int64_t>((v ^ top) - top);
}
uint64_t mask_of(unsigned bits) { return bits == 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1; }

// The register as a number: the first eight bytes are the upper half.
struct Wide {
    uint64_t hi, lo;
};
Wide wide(const uint8_t* r) { return {get(r, 8, 0), get(r, 8, 1)}; }
void put(uint8_t* r, Wide v) { set(r, 8, 0, v.hi), set(r, 8, 1, v.lo); }
Wide left(Wide v, unsigned bits) {  // 0 to 127
    if (bits == 0) return v;
    if (bits >= 64) return {v.lo << (bits - 64), 0};
    return {(v.hi << bits) | (v.lo >> (64 - bits)), v.lo << bits};
}
Wide right(Wide v, unsigned bits) {
    if (bits == 0) return v;
    if (bits >= 64) return {0, v.hi >> (bits - 64)};
    return {v.hi >> bits, (v.lo >> bits) | (v.hi << (64 - bits))};
}

uint32_t preferred(const uint8_t* r) { return static_cast<uint32_t>(get(r, 4, 0)); }

}  // namespace

bool step(State& s, const Insn& in, uint32_t pc) {
    uint8_t* const T = s.r[in.rt()];
    const uint8_t* const A = s.r[in.ra()];
    const uint8_t* const B = s.r[in.rb()];
    Reg out;
    std::memcpy(out, T, 16);
    const auto done = [&] {
        std::memcpy(T, out, 16);
        return true;
    };
    const int64_t i10 = in.i10(), i7 = in.i7();
    const uint64_t u16 = in.u16();

    // out[i] = f(a[i], b[i]) for elements of `width` bytes; b is a register or a constant.
    const auto each = [&](unsigned width, const uint8_t* b, auto f) {
        for (unsigned i = 0; i < 16 / width; ++i) set(out, width, i, f(get(A, width, i), get(b, width, i)));
        return done();
    };
    const auto each_k = [&](unsigned width, uint64_t k, auto f) {
        for (unsigned i = 0; i < 16 / width; ++i) set(out, width, i, f(get(A, width, i), k & mask_of(8 * width)));
        return done();
    };
    const auto all_ones = [](bool yes, unsigned width) { return yes ? mask_of(8 * width) : 0; };
    const auto local = [&](uint64_t address) { return static_cast<uint32_t>(address) & 0x3fff0u; };
    const auto load_at = [&](uint32_t address) {
        std::memcpy(out, s.ls + local(address), 16);
        return done();
    };
    const auto store_at = [&](uint32_t address) {
        std::memcpy(s.ls + local(address), T, 16);
        s.stored = true;
        s.stored_at = local(address);
        return true;
    };
    // The masks that put one element into a quadword with shufb.
    const auto insertion = [&](uint64_t address, unsigned width) {
        for (unsigned i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>(0x10 + i);
        const unsigned at = (static_cast<unsigned>(address) & 15u) / width * width;
        static const uint8_t from[9][8] = {{}, {3}, {2, 3}, {}, {0, 1, 2, 3}, {}, {}, {}, {0, 1, 2, 3, 4, 5, 6, 7}};
        for (unsigned i = 0; i < width; ++i) out[at + i] = from[width][i];
        return done();
    };
    const auto shift_count = [&](bool immediate) -> uint64_t { return immediate ? static_cast<uint64_t>(i7) : preferred(B); };

    switch (in.op) {
    // ---- memory ----
    case Op::lqd: return load_at(preferred(A) + static_cast<uint32_t>(i10 * 16));
    case Op::lqx: return load_at(preferred(A) + preferred(B));
    case Op::lqa: return load_at(static_cast<uint32_t>(in.i16()) * 4);
    case Op::lqr: return load_at(pc + static_cast<uint32_t>(in.i16()) * 4);
    case Op::stqd: return store_at(preferred(A) + static_cast<uint32_t>(i10 * 16));
    case Op::stqx: return store_at(preferred(A) + preferred(B));
    case Op::stqa: return store_at(static_cast<uint32_t>(in.i16()) * 4);
    case Op::stqr: return store_at(pc + static_cast<uint32_t>(in.i16()) * 4);
    case Op::cbd: return insertion(preferred(A) + static_cast<uint64_t>(i7), 1);
    case Op::chd: return insertion(preferred(A) + static_cast<uint64_t>(i7), 2);
    case Op::cwd: return insertion(preferred(A) + static_cast<uint64_t>(i7), 4);
    case Op::cdd: return insertion(preferred(A) + static_cast<uint64_t>(i7), 8);
    case Op::cbx: return insertion(uint64_t{preferred(A)} + preferred(B), 1);
    case Op::chx: return insertion(uint64_t{preferred(A)} + preferred(B), 2);
    case Op::cwx: return insertion(uint64_t{preferred(A)} + preferred(B), 4);
    case Op::cdx: return insertion(uint64_t{preferred(A)} + preferred(B), 8);

    // ---- constants ----
    case Op::il: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, static_cast<uint64_t>(in.i16())); return done();
    case Op::ilh: for (unsigned i = 0; i < 8; ++i) set(out, 2, i, u16); return done();
    case Op::ilhu: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, u16 << 16); return done();
    case Op::ila: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, in.u18()); return done();
    case Op::iohl: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, get(T, 4, i) | u16); return done();
    case Op::fsmbi: for (unsigned i = 0; i < 16; ++i) out[i] = (u16 >> (15 - i)) & 1 ? 0xff : 0x00; return done();
    case Op::fsmb: for (unsigned i = 0; i < 16; ++i) out[i] = (preferred(A) >> (15 - i)) & 1 ? 0xff : 0x00; return done();
    case Op::fsmh: for (unsigned i = 0; i < 8; ++i) set(out, 2, i, (preferred(A) >> (7 - i)) & 1 ? 0xffff : 0); return done();
    case Op::fsm: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, (preferred(A) >> (3 - i)) & 1 ? 0xffffffffu : 0); return done();
    case Op::gbb: case Op::gbh: case Op::gb: {
        const unsigned width = in.op == Op::gbb ? 1 : in.op == Op::gbh ? 2 : 4;
        uint64_t bits = 0;
        for (unsigned i = 0; i < 16 / width; ++i) bits = (bits << 1) | (get(A, width, i) & 1);
        std::memset(out, 0, 16);
        set(out, 4, 0, bits);
        return done();
    }

    // ---- arithmetic ----
    case Op::a: return each(4, B, [](uint64_t a, uint64_t b) { return a + b; });
    case Op::ah: return each(2, B, [](uint64_t a, uint64_t b) { return a + b; });
    case Op::ai: return each_k(4, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return a + b; });
    case Op::ahi: return each_k(2, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return a + b; });
    case Op::sf: return each(4, B, [](uint64_t a, uint64_t b) { return b - a; });
    case Op::sfh: return each(2, B, [](uint64_t a, uint64_t b) { return b - a; });
    case Op::sfi: return each_k(4, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return b - a; });
    case Op::sfhi: return each_k(2, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return b - a; });
    case Op::addx: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, get(A, 4, i) + get(B, 4, i) + (get(T, 4, i) & 1)); return done();
    case Op::cg: return each(4, B, [](uint64_t a, uint64_t b) { return (a + b) >> 32; });
    case Op::cgx: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, (get(A, 4, i) + get(B, 4, i) + (get(T, 4, i) & 1)) >> 32); return done();
    // b - a - 1 + carry, as b + (2^32 - 1 - a) + carry; "borrow generate" is 1 when no borrow is needed.
    case Op::sfx: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, get(B, 4, i) + (0xffffffffu - get(A, 4, i)) + (get(T, 4, i) & 1)); return done();
    case Op::bg: return each(4, B, [](uint64_t a, uint64_t b) { return uint64_t{b >= a}; });
    case Op::bgx: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, (get(B, 4, i) + (0xffffffffu - get(A, 4, i)) + (get(T, 4, i) & 1)) >> 32); return done();
    case Op::mpy: return each(4, B, [](uint64_t a, uint64_t b) { return static_cast<uint64_t>(sign(a, 16) * sign(b, 16)); });
    case Op::mpyu: return each(4, B, [](uint64_t a, uint64_t b) { return (a & 0xffff) * (b & 0xffff); });
    case Op::mpyi: return each_k(4, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return static_cast<uint64_t>(sign(a, 16) * sign(b, 16)); });
    case Op::mpyui: return each_k(4, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return (a & 0xffff) * (b & 0xffff); });
    case Op::mpyh: return each(4, B, [](uint64_t a, uint64_t b) { return ((a >> 16) * (b & 0xffff)) << 16; });
    case Op::mpys: return each(4, B, [](uint64_t a, uint64_t b) { return static_cast<uint64_t>((sign(a, 16) * sign(b, 16)) >> 16); });
    case Op::mpyhh: return each(4, B, [](uint64_t a, uint64_t b) { return static_cast<uint64_t>(sign(a >> 16, 16) * sign(b >> 16, 16)); });
    case Op::mpyhhu: return each(4, B, [](uint64_t a, uint64_t b) { return (a >> 16) * (b >> 16); });
    case Op::mpyhha: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, get(T, 4, i) + static_cast<uint64_t>(sign(get(A, 2, 2 * i), 16) * sign(get(B, 2, 2 * i), 16))); return done();
    case Op::mpyhhau: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, get(T, 4, i) + get(A, 2, 2 * i) * get(B, 2, 2 * i)); return done();
    case Op::mpya: {  // the four-register form: the target is in other bits, and rt's field names the addend
        uint8_t* const target = s.r[in.rt4()];
        Reg sum;
        for (unsigned i = 0; i < 4; ++i) set(sum, 4, i, static_cast<uint64_t>(sign(get(A, 2, 2 * i + 1), 16) * sign(get(B, 2, 2 * i + 1), 16)) + get(T, 4, i));
        std::memcpy(target, sum, 16);
        return true;
    }
    case Op::clz:
        for (unsigned i = 0; i < 4; ++i) {
            unsigned n = 0;
            for (uint64_t v = get(A, 4, i); n < 32 && !(v & 0x80000000u); v <<= 1) ++n;
            set(out, 4, i, n);
        }
        return done();
    case Op::cntb:
        for (unsigned i = 0; i < 16; ++i) {
            unsigned n = 0;
            for (unsigned bit = 0; bit < 8; ++bit) n += (A[i] >> bit) & 1;
            out[i] = static_cast<uint8_t>(n);
        }
        return done();
    case Op::avgb: return each(1, B, [](uint64_t a, uint64_t b) { return (a + b + 1) / 2; });
    case Op::absdb: return each(1, B, [](uint64_t a, uint64_t b) { return a > b ? a - b : b - a; });
    case Op::sumb:
        for (unsigned i = 0; i < 4; ++i) {
            set(out, 2, 2 * i, uint64_t{B[4 * i]} + B[4 * i + 1] + B[4 * i + 2] + B[4 * i + 3]);
            set(out, 2, 2 * i + 1, uint64_t{A[4 * i]} + A[4 * i + 1] + A[4 * i + 2] + A[4 * i + 3]);
        }
        return done();
    case Op::xsbh: for (unsigned i = 0; i < 8; ++i) set(out, 2, i, static_cast<uint64_t>(sign(A[2 * i + 1], 8))); return done();
    case Op::xshw: for (unsigned i = 0; i < 4; ++i) set(out, 4, i, static_cast<uint64_t>(sign(get(A, 2, 2 * i + 1), 16))); return done();
    case Op::xswd: for (unsigned i = 0; i < 2; ++i) set(out, 8, i, static_cast<uint64_t>(sign(get(A, 4, 2 * i + 1), 32))); return done();

    // ---- logic ----
    case Op::and_: return each(1, B, [](uint64_t a, uint64_t b) { return a & b; });
    case Op::andc: return each(1, B, [](uint64_t a, uint64_t b) { return a & ~b; });
    case Op::or_: return each(1, B, [](uint64_t a, uint64_t b) { return a | b; });
    case Op::orc: return each(1, B, [](uint64_t a, uint64_t b) { return a | ~b; });
    case Op::xor_: return each(1, B, [](uint64_t a, uint64_t b) { return a ^ b; });
    case Op::nand: return each(1, B, [](uint64_t a, uint64_t b) { return ~(a & b); });
    case Op::nor: return each(1, B, [](uint64_t a, uint64_t b) { return ~(a | b); });
    case Op::eqv: return each(1, B, [](uint64_t a, uint64_t b) { return ~(a ^ b); });
    case Op::andi: return each_k(4, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return a & b; });
    case Op::andhi: return each_k(2, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return a & b; });
    case Op::andbi: return each_k(1, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return a & b; });
    case Op::ori: return each_k(4, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return a | b; });
    case Op::orhi: return each_k(2, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return a | b; });
    case Op::orbi: return each_k(1, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return a | b; });
    case Op::xori: return each_k(4, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return a ^ b; });
    case Op::xorhi: return each_k(2, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return a ^ b; });
    case Op::xorbi: return each_k(1, static_cast<uint64_t>(i10), [](uint64_t a, uint64_t b) { return a ^ b; });
    case Op::orx:
        std::memset(out, 0, 16);
        set(out, 4, 0, get(A, 4, 0) | get(A, 4, 1) | get(A, 4, 2) | get(A, 4, 3));
        return done();
    case Op::selb: {  // four registers: each bit from b where the mask (rt's field) is 1, from a where it is 0
        uint8_t* const target = s.r[in.rt4()];
        Reg picked;
        for (unsigned i = 0; i < 16; ++i) picked[i] = static_cast<uint8_t>((T[i] & B[i]) | (~T[i] & A[i]));
        std::memcpy(target, picked, 16);
        return true;
    }
    case Op::shufb: {
        uint8_t* const target = s.r[in.rt4()];
        Reg picked;
        for (unsigned i = 0; i < 16; ++i) {
            const uint8_t c = T[i];
            if ((c >> 6) == 2) picked[i] = 0x00;
            else if ((c >> 5) == 6) picked[i] = 0xff;
            else if ((c >> 5) == 7) picked[i] = 0x80;
            else picked[i] = (c & 16) ? B[c & 15] : A[c & 15];
        }
        std::memcpy(target, picked, 16);
        return true;
    }

    // ---- shifts and rotates of elements: the count is each element's own, or the constant ----
    case Op::shlh: case Op::shlhi: case Op::shl: case Op::shli: {
        const bool immediate = in.op == Op::shlhi || in.op == Op::shli;
        const unsigned width = in.op == Op::shlh || in.op == Op::shlhi ? 2 : 4, bits = 8 * width;
        for (unsigned i = 0; i < 16 / width; ++i) {
            const uint64_t count = (immediate ? static_cast<uint64_t>(i7) : get(B, width, i)) & (2 * bits - 1);
            set(out, width, i, count >= bits ? 0 : get(A, width, i) << count);
        }
        return done();
    }
    case Op::roth: case Op::rothi: case Op::rot: case Op::roti: {
        const bool immediate = in.op == Op::rothi || in.op == Op::roti;
        const unsigned width = in.op == Op::roth || in.op == Op::rothi ? 2 : 4, bits = 8 * width;
        for (unsigned i = 0; i < 16 / width; ++i) {
            const uint64_t count = (immediate ? static_cast<uint64_t>(i7) : get(B, width, i)) & (bits - 1), v = get(A, width, i);
            set(out, width, i, (v << count) | (v >> (bits - count)));  // (v is below 2^32, so a shift by `bits` leaves nothing wrong)
        }
        return done();
    }
    case Op::rothm: case Op::rothmi: case Op::rotm: case Op::rotmi: case Op::rotmah: case Op::rotmahi: case Op::rotma: case Op::rotmai: {
        const bool immediate = in.op == Op::rothmi || in.op == Op::rotmi || in.op == Op::rotmahi || in.op == Op::rotmai;
        const bool arithmetic = in.op == Op::rotmah || in.op == Op::rotmahi || in.op == Op::rotma || in.op == Op::rotmai;
        const unsigned width = in.op == Op::rothm || in.op == Op::rothmi || in.op == Op::rotmah || in.op == Op::rotmahi ? 2 : 4, bits = 8 * width;
        for (unsigned i = 0; i < 16 / width; ++i) {
            // To the right by the negative of the count, taken in one more bit than the element has.
            const uint64_t count = (0 - (immediate ? static_cast<uint64_t>(i7) : get(B, width, i))) & (2 * bits - 1);
            const uint64_t v = get(A, width, i);
            if (arithmetic) set(out, width, i, static_cast<uint64_t>(sign(v, bits) >> (count >= bits ? bits - 1 : count)));
            else set(out, width, i, count >= bits ? 0 : v >> count);
        }
        return done();
    }

    // ---- shifts and rotates of the whole register ----
    case Op::shlqbi: case Op::shlqbii: put(out, left(wide(A), shift_count(in.op == Op::shlqbii) & 7)); return done();
    case Op::shlqby: case Op::shlqbyi: case Op::shlqbybi: {
        const uint64_t raw = shift_count(in.op == Op::shlqbyi);
        const unsigned count = static_cast<unsigned>((in.op == Op::shlqbybi ? raw >> 3 : raw) & 31);
        put(out, count >= 16 ? Wide{0, 0} : left(wide(A), 8 * count));
        return done();
    }
    case Op::rotqbi: case Op::rotqbii: case Op::rotqby: case Op::rotqbyi: case Op::rotqbybi: {
        const uint64_t raw = shift_count(in.op == Op::rotqbii || in.op == Op::rotqbyi);
        unsigned bits;
        if (in.op == Op::rotqbi || in.op == Op::rotqbii) bits = static_cast<unsigned>(raw & 7);
        else bits = 8 * static_cast<unsigned>((in.op == Op::rotqbybi ? raw >> 3 : raw) & 15);
        const Wide v = wide(A), up = left(v, bits), down = bits ? right(v, 128 - bits) : Wide{0, 0};
        put(out, {up.hi | down.hi, up.lo | down.lo});
        return done();
    }
    case Op::rotqmbi: case Op::rotqmbii: put(out, right(wide(A), static_cast<unsigned>((0 - shift_count(in.op == Op::rotqmbii)) & 7))); return done();
    case Op::rotqmby: case Op::rotqmbyi: case Op::rotqmbybi: {
        const uint64_t raw = shift_count(in.op == Op::rotqmbyi);
        const unsigned count = static_cast<unsigned>((0 - (in.op == Op::rotqmbybi ? raw >> 3 : raw)) & 31);
        put(out, count >= 16 ? Wide{0, 0} : right(wide(A), 8 * count));
        return done();
    }

    // ---- compares ----
    case Op::ceqb: return each(1, B, [&](uint64_t a, uint64_t b) { return all_ones(a == b, 1); });
    case Op::ceqh: return each(2, B, [&](uint64_t a, uint64_t b) { return all_ones(a == b, 2); });
    case Op::ceq: return each(4, B, [&](uint64_t a, uint64_t b) { return all_ones(a == b, 4); });
    case Op::cgtb: return each(1, B, [&](uint64_t a, uint64_t b) { return all_ones(sign(a, 8) > sign(b, 8), 1); });
    case Op::cgth: return each(2, B, [&](uint64_t a, uint64_t b) { return all_ones(sign(a, 16) > sign(b, 16), 2); });
    case Op::cgt: return each(4, B, [&](uint64_t a, uint64_t b) { return all_ones(sign(a, 32) > sign(b, 32), 4); });
    case Op::clgtb: return each(1, B, [&](uint64_t a, uint64_t b) { return all_ones(a > b, 1); });
    case Op::clgth: return each(2, B, [&](uint64_t a, uint64_t b) { return all_ones(a > b, 2); });
    case Op::clgt: return each(4, B, [&](uint64_t a, uint64_t b) { return all_ones(a > b, 4); });
    case Op::ceqbi: return each_k(1, static_cast<uint64_t>(i10), [&](uint64_t a, uint64_t b) { return all_ones(a == b, 1); });
    case Op::ceqhi: return each_k(2, static_cast<uint64_t>(i10), [&](uint64_t a, uint64_t b) { return all_ones(a == b, 2); });
    case Op::ceqi: return each_k(4, static_cast<uint64_t>(i10), [&](uint64_t a, uint64_t b) { return all_ones(a == b, 4); });
    case Op::cgtbi: return each_k(1, static_cast<uint64_t>(i10), [&](uint64_t a, uint64_t b) { return all_ones(sign(a, 8) > sign(b, 8), 1); });
    case Op::cgthi: return each_k(2, static_cast<uint64_t>(i10), [&](uint64_t a, uint64_t b) { return all_ones(sign(a, 16) > sign(b, 16), 2); });
    case Op::cgti: return each_k(4, static_cast<uint64_t>(i10), [&](uint64_t a, uint64_t b) { return all_ones(sign(a, 32) > sign(b, 32), 4); });
    case Op::clgtbi: return each_k(1, static_cast<uint64_t>(i10), [&](uint64_t a, uint64_t b) { return all_ones(a > b, 1); });
    case Op::clgthi: return each_k(2, static_cast<uint64_t>(i10), [&](uint64_t a, uint64_t b) { return all_ones(a > b, 2); });
    case Op::clgti: return each_k(4, static_cast<uint64_t>(i10), [&](uint64_t a, uint64_t b) { return all_ones(a > b, 4); });
    // The halts compare the preferred words; the immediate is sign-extended (and then, for hlgti, taken as unsigned).
    case Op::heq: s.halted = preferred(A) == preferred(B); return true;
    // fscrwr: RA into the FPSCR; its unused bits are undefined (kept zero here). fscrrd: the FPSCR, the unused bits zero.
    // The defined bits, by number from bit 0 (the most significant): 20-23, 29-31, 50-55, 61-63, 82-87, 93-95, 116-119,
    // 125-127. Bits 20-23 are the rounding modes of the two double-precision slices: this runtime provides only 00 (to
    // nearest), so another halts.
    case Op::fscrwr:
    case Op::fscrrd: {
        static constexpr unsigned kDefined[][2] = {{20, 23}, {29, 31}, {50, 55}, {61, 63}, {82, 87}, {93, 95}, {116, 119}, {125, 127}};
        uint8_t* const to = in.op == Op::fscrwr ? s.fpscr : T;
        const uint8_t* const from = in.op == Op::fscrwr ? A : s.fpscr;
        uint8_t v[16] = {};
        for (const auto& range : kDefined)
            for (unsigned bit = range[0]; bit <= range[1]; ++bit)
                if ((from[bit / 8] >> (7 - bit % 8)) & 1) v[bit / 8] |= static_cast<uint8_t>(1u << (7 - bit % 8));
        std::memcpy(to, v, 16);
        if (in.op == Op::fscrwr) s.halted = (s.fpscr[2] & 0xF) != 0;  // bits 20-23: byte 2's low four
        return true;
    }
    case Op::hgt: s.halted = sign(preferred(A), 32) > sign(preferred(B), 32); return true;
    case Op::hlgt: s.halted = preferred(A) > preferred(B); return true;
    case Op::heqi: s.halted = sign(preferred(A), 32) == i10; return true;
    case Op::hgti: s.halted = sign(preferred(A), 32) > i10; return true;
    case Op::hlgti: s.halted = preferred(A) > static_cast<uint32_t>(i10); return true;

    // ---- channels and stops: what the hooks would be asked ----
    case Op::rdch: case Op::rchcnt:
        std::memset(out, 0, 16);
        set(out, 4, 0, in.op == Op::rdch ? channel_value(in.ra()) : channel_count(in.ra()));
        return done();
    case Op::wrch:
        s.wrote_channel = true;
        s.channel = in.ra();
        s.channel_data = preferred(T);
        return true;
    case Op::stop: s.stopped = true, s.stop_code = in.u14(); return true;
    case Op::stopd: s.stopped = true, s.stop_code = 0x3fff; return true;
    case Op::nop: case Op::lnop: case Op::sync: case Op::dsync: case Op::hbr: case Op::hbra: case Op::hbrr: return true;
    default: return step_fp(s, in);
    }
}


// Branches, from the definition: a relative branch's target is pc plus four times its 16-bit field; brz and brnz
// test the preferred word, brhz and brhnz its lower halfword; brsl puts pc + 4 in rt's preferred word (the rest zero).
// A `bi $0` is taken as the return the lifter takes it for (a stack of the calls made), not as a jump to $0.
bool run_program(State& s, const uint32_t* words, size_t count) {
    std::vector<uint32_t> returns;
    uint32_t pc = 0x3000;
    for (size_t steps = 0; steps < 1000000; ++steps) {
        if (pc < 0x3000 || (pc - 0x3000) / 4 >= count) return false;
        const Insn in = qilin::spu::decode(words[(pc - 0x3000) / 4]);
        const uint64_t word = get(s.r[in.rt()], 4, 0), half = get(s.r[in.rt()], 2, 1);
        const auto ret = [&] {
            if (returns.empty()) return false;
            pc = returns.back();
            returns.pop_back();
            return true;
        };
        switch (in.op) {
        case Op::brz: pc = word == 0 ? in.relative(pc) : pc + 4; break;
        case Op::brnz: pc = word != 0 ? in.relative(pc) : pc + 4; break;
        case Op::brhz: pc = half == 0 ? in.relative(pc) : pc + 4; break;
        case Op::brhnz: pc = half != 0 ? in.relative(pc) : pc + 4; break;
        case Op::br: pc = in.relative(pc); break;
        case Op::brsl:
            std::memset(s.r[in.rt()], 0, 16);
            set(s.r[in.rt()], 4, 0, pc + 4);
            returns.push_back(pc + 4);
            pc = in.relative(pc);
            break;
        case Op::bi:
            if (in.ra() != 0) return false;
            if (!ret()) return true;  // the first function's return: the end
            break;
        case Op::biz: case Op::binz: case Op::bihz: case Op::bihnz: {
            if (in.ra() != 0) return false;
            const bool zero = (in.op == Op::biz || in.op == Op::binz) ? word == 0 : half == 0;
            const bool taken = (in.op == Op::biz || in.op == Op::bihz) ? zero : !zero;
            if (!taken) pc += 4;
            else if (!ret()) return true;
            break;
        }
        default:
            if (qilin::spu::spec(in.op).flags & qilin::spu::kBranch) return false;
            // (A halt that fired stays fired: step() says only whether this one does.)
            const bool halted = s.halted;
            if (!step(s, in, pc)) return false;
            s.halted = s.halted || halted;
            pc += 4;
        }
    }
    return false;
}

}  // namespace spu_difftest

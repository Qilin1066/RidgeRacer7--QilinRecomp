#include "ref.hpp"

namespace qilin::ref {

using ppu::Insn;
using ppu::Op;

namespace {

// ---- bits, numbered as the manuals number them: 0 is the most significant of 64 ----
constexpr uint64_t bit(unsigned n) { return uint64_t{1} << (63 - n); }

uint64_t mask(unsigned mb, unsigned me) {  // bits mb..me, wrapping
    uint64_t m = 0;
    for (unsigned i = mb;; i = (i + 1) & 63) {
        m |= bit(i);
        if (i == me) break;
    }
    return m;
}

uint64_t rotl64(uint64_t x, unsigned n) {
    for (unsigned i = 0; i < (n & 63); ++i) x = (x << 1) | (x >> 63);
    return x;
}
uint64_t rotl32(uint64_t x, unsigned n) {  // of the low word, repeated in both halves
    uint32_t w = static_cast<uint32_t>(x);
    for (unsigned i = 0; i < (n & 31); ++i) w = (w << 1) | (w >> 31);
    return (uint64_t{w} << 32) | w;
}

uint64_t sign_extend(uint64_t x, unsigned bits) {
    const uint64_t sign = uint64_t{1} << (bits - 1);
    x &= (sign << 1) - 1;
    return (x ^ sign) - sign;
}

// a + b + carry, bit by bit: the sum, and the carry out of the top bit.
uint64_t add(uint64_t a, uint64_t b, unsigned carry, unsigned& carry_out) {
    uint64_t sum = 0;
    for (unsigned i = 0; i < 64; ++i) {
        const unsigned x = (a >> i) & 1, y = (b >> i) & 1;
        sum |= uint64_t{x ^ y ^ carry} << i;
        carry = (x & y) | (x & carry) | (y & carry);
    }
    carry_out = carry;
    return sum;
}

// The 128-bit product of two unsigned 64-bit numbers, by 32-bit limbs.
void multiply(uint64_t a, uint64_t b, uint64_t& high, uint64_t& low) {
    const uint64_t a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t middle = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    low = (middle << 32) | (p00 & 0xffffffffu);
    high = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
}
uint64_t multiply_high_signed(uint64_t a, uint64_t b) {
    uint64_t high, low;
    multiply(a, b, high, low);
    if (a >> 63) high -= b;  // the signed product's high half differs by b where a is negative, and by a where b is
    if (b >> 63) high -= a;
    return high;
}

// ---- XER and CR ----
constexpr uint64_t kSo = uint64_t{1} << 31, kOv = uint64_t{1} << 30, kCa = uint64_t{1} << 29;

void set_ca(State& s, bool on) { s.xer = on ? (s.xer | kCa) : (s.xer & ~kCa); }

void set_cr_field(State& s, unsigned field, uint32_t value) {
    const unsigned shift = 28 - 4 * field;
    s.cr = (s.cr & ~(uint32_t{15} << shift)) | (value << shift);
}
uint32_t compare(bool less, bool greater, const State& s) { return (less ? 8u : greater ? 4u : 2u) | ((s.xer & kSo) ? 1u : 0u); }
void record(State& s, uint64_t result) {
    const int64_t v = static_cast<int64_t>(result);
    set_cr_field(s, 0, compare(v < 0, v > 0, s));
}
unsigned cr_bit(const State& s, unsigned n) { return (s.cr >> (31 - n)) & 1; }
void set_cr_bit(State& s, unsigned n, unsigned value) { s.cr = (s.cr & ~(uint32_t{1} << (31 - n))) | ((value & 1) << (31 - n)); }

// ---- memory, a byte at a time ----
uint64_t load(const State& s, uint32_t address, unsigned bytes) {
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i) v = (v << 8) | s.mem[address + i];
    return v;
}
void store(State& s, uint32_t address, unsigned bytes, uint64_t v) {
    for (unsigned i = 0; i < bytes; ++i) s.mem[address + i] = static_cast<uint8_t>(v >> (8 * (bytes - 1 - i)));
}
uint64_t reverse(uint64_t v, unsigned bytes) {
    uint64_t r = 0;
    for (unsigned i = 0; i < bytes; ++i) r = (r << 8) | ((v >> (8 * i)) & 0xff);
    return r;
}

}  // namespace

bool branch_taken(State& s, unsigned bo, unsigned bi) {
    bool ctr_ok = true, cond_ok = true;
    if (!(bo & 4)) {
        s.ctr -= 1;
        ctr_ok = (s.ctr != 0) != ((bo & 2) != 0);
    }
    if (!(bo & 16)) cond_ok = cr_bit(s, bi) == ((bo >> 3) & 1);
    return ctr_ok && cond_ok;
}

bool trap_fires(const State& s, const Insn& in) {
    const unsigned to = in.rd();
    const bool wide = in.op == Op::td || in.op == Op::tdi;
    uint64_t a = s.gpr[in.ra()];
    uint64_t b = (in.op == Op::twi || in.op == Op::tdi) ? sign_extend(in.word & 0xffff, 16) : s.gpr[in.rb()];
    if (!wide) {
        a = sign_extend(a, 32);
        b = sign_extend(b, 32);
    }
    const int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b);
    return ((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && a == b) || ((to & 2) && a < b) || ((to & 1) && a > b);
}

bool step(State& s, const Insn& in) {
    const uint32_t w = in.word;
    const unsigned rt = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31;
    const uint64_t simm = sign_extend(w & 0xffff, 16), uimm = w & 0xffff;
    const bool rc = w & 1;
    uint64_t* const r = s.gpr;
    const uint64_t a0 = ra ? r[ra] : 0;  // (rA|0)
    unsigned carry = 0;
    const unsigned ca = (s.xer & kCa) ? 1 : 0;
    // The logical and shift instructions write rA from rS (= rt); the arithmetic ones write rT.
    const auto logical = [&](uint64_t value) {
        r[ra] = value;
        if (rc) record(s, value);
    };
    const auto arithmetic = [&](uint64_t value) {
        r[rt] = value;
        if (rc) record(s, value);
    };
    const auto with_carry = [&](uint64_t x, uint64_t y, unsigned in_carry) {
        const uint64_t sum = add(x, y, in_carry, carry);
        set_ca(s, carry);
        arithmetic(sum);
    };
    const unsigned sh6 = ((w >> 11) & 31) | (((w >> 1) & 1) << 5), mb6 = ((w >> 6) & 31) | (((w >> 5) & 1) << 5);
    const unsigned mb = (w >> 6) & 31, me = (w >> 1) & 31;
    const auto load_to = [&](uint64_t ea, unsigned bytes, bool algebraic, bool update, bool reversed = false) {
        uint64_t v = load(s, static_cast<uint32_t>(ea), bytes);
        if (reversed) v = reverse(v, bytes);
        if (algebraic) v = sign_extend(v, 8 * bytes);
        r[rt] = v;
        if (update) r[ra] = ea;
    };
    const auto store_from = [&](uint64_t ea, unsigned bytes, bool update, bool reversed = false) {
        uint64_t v = r[rt];
        if (reversed) v = reverse(v, bytes);
        store(s, static_cast<uint32_t>(ea), bytes, v);
        if (update) r[ra] = ea;
    };
    const uint64_t ea_d = a0 + simm, ea_ds = a0 + sign_extend(w & 0xfffc, 16), ea_x = a0 + r[rb];

    switch (in.op) {
    case Op::addi: r[rt] = a0 + simm; return true;
    case Op::addis: r[rt] = a0 + (simm << 16); return true;
    case Op::add: arithmetic(r[ra] + r[rb]); return true;
    case Op::subf: arithmetic(~r[ra] + r[rb] + 1); return true;
    case Op::neg: arithmetic(~r[ra] + 1); return true;
    case Op::addic: { const uint64_t v = add(r[ra], simm, 0, carry); set_ca(s, carry); r[rt] = v; return true; }
    case Op::addic_rc: { const uint64_t v = add(r[ra], simm, 0, carry); set_ca(s, carry); r[rt] = v; record(s, v); return true; }
    case Op::subfic: { const uint64_t v = add(~r[ra], simm, 1, carry); set_ca(s, carry); r[rt] = v; return true; }
    case Op::addc: with_carry(r[ra], r[rb], 0); return true;
    case Op::subfc: with_carry(~r[ra], r[rb], 1); return true;
    case Op::adde: with_carry(r[ra], r[rb], ca); return true;
    case Op::subfe: with_carry(~r[ra], r[rb], ca); return true;
    case Op::addze: with_carry(r[ra], 0, ca); return true;
    case Op::subfze: with_carry(~r[ra], 0, ca); return true;
    case Op::addme: with_carry(r[ra], ~uint64_t{0}, ca); return true;
    case Op::subfme: with_carry(~r[ra], ~uint64_t{0}, ca); return true;
    case Op::mulli: { uint64_t high, low; multiply(r[ra], simm, high, low); r[rt] = low; return true; }
    case Op::mulld: { uint64_t high, low; multiply(r[ra], r[rb], high, low); arithmetic(low); return true; }
    case Op::mullw: { uint64_t high, low; multiply(sign_extend(r[ra], 32), sign_extend(r[rb], 32), high, low); arithmetic(low); return true; }
    case Op::mulhd: arithmetic(multiply_high_signed(r[ra], r[rb])); return true;
    case Op::mulhdu: { uint64_t high, low; multiply(r[ra], r[rb], high, low); arithmetic(high); return true; }
    // The word forms' upper halves are undefined: the reference takes the choices cpu.hpp states.
    case Op::mulhw: { uint64_t high, low; multiply(sign_extend(r[ra], 32), sign_extend(r[rb], 32), high, low); arithmetic(sign_extend(low >> 32, 32)); return true; }
    case Op::mulhwu: { uint64_t high, low; multiply(r[ra] & 0xffffffffu, r[rb] & 0xffffffffu, high, low); arithmetic(low >> 32); return true; }
    case Op::divw: {
        const int64_t n = static_cast<int64_t>(sign_extend(r[ra], 32)), d = static_cast<int64_t>(sign_extend(r[rb], 32));
        const bool bad = d == 0 || (n == -2147483648ll && d == -1);
        arithmetic(bad ? 0 : static_cast<uint64_t>(n / d) & 0xffffffffu);
        return true;
    }
    case Op::divwu: {
        const uint64_t n = r[ra] & 0xffffffffu, d = r[rb] & 0xffffffffu;
        arithmetic(d == 0 ? 0 : n / d);
        return true;
    }
    case Op::divd: {
        const uint64_t n = r[ra], d = r[rb];
        if (d == 0 || (n == bit(0) && d == ~uint64_t{0})) arithmetic(0);
        else {
            // On magnitudes, then the sign.
            const bool negative = ((n ^ d) >> 63) != 0;
            const uint64_t q = ((n >> 63) ? ~n + 1 : n) / ((d >> 63) ? ~d + 1 : d);
            arithmetic(negative ? ~q + 1 : q);
        }
        return true;
    }
    case Op::divdu: arithmetic(r[rb] == 0 ? 0 : r[ra] / r[rb]); return true;

    case Op::cmp: case Op::cmpi: case Op::cmpl: case Op::cmpli: {
        const bool wide = (w >> 21) & 1, is_signed = in.op == Op::cmp || in.op == Op::cmpi;
        uint64_t x = r[ra], y = in.op == Op::cmpi ? simm : in.op == Op::cmpli ? uimm : r[rb];
        if (!wide) {
            x = is_signed ? sign_extend(x, 32) : x & 0xffffffffu;
            y = is_signed ? sign_extend(y, 32) : y & 0xffffffffu;
        }
        const bool less = is_signed ? static_cast<int64_t>(x) < static_cast<int64_t>(y) : x < y;
        const bool greater = is_signed ? static_cast<int64_t>(x) > static_cast<int64_t>(y) : x > y;
        set_cr_field(s, (w >> 23) & 7, compare(less, greater, s));
        return true;
    }

    case Op::ori: r[ra] = r[rt] | uimm; return true;
    case Op::oris: r[ra] = r[rt] | (uimm << 16); return true;
    case Op::xori: r[ra] = r[rt] ^ uimm; return true;
    case Op::xoris: r[ra] = r[rt] ^ (uimm << 16); return true;
    case Op::andi_rc: r[ra] = r[rt] & uimm; record(s, r[ra]); return true;
    case Op::andis_rc: r[ra] = r[rt] & (uimm << 16); record(s, r[ra]); return true;
    case Op::and_: logical(r[rt] & r[rb]); return true;
    case Op::or_: logical(r[rt] | r[rb]); return true;
    case Op::xor_: logical(r[rt] ^ r[rb]); return true;
    case Op::andc: logical(r[rt] & ~r[rb]); return true;
    case Op::orc: logical(r[rt] | ~r[rb]); return true;
    case Op::nor: logical(~(r[rt] | r[rb])); return true;
    case Op::nand: logical(~(r[rt] & r[rb])); return true;
    case Op::eqv: logical(~(r[rt] ^ r[rb])); return true;
    case Op::extsb: logical(sign_extend(r[rt], 8)); return true;
    case Op::extsh: logical(sign_extend(r[rt], 16)); return true;
    case Op::extsw: logical(sign_extend(r[rt], 32)); return true;
    case Op::cntlzw: { unsigned n = 0; while (n < 32 && !(r[rt] & bit(32 + n))) ++n; logical(n); return true; }
    case Op::cntlzd: { unsigned n = 0; while (n < 64 && !(r[rt] & bit(n))) ++n; logical(n); return true; }

    case Op::rlwinm: logical(rotl32(r[rt], rb) & mask(mb + 32, me + 32)); return true;
    case Op::rlwnm: logical(rotl32(r[rt], static_cast<unsigned>(r[rb] & 31)) & mask(mb + 32, me + 32)); return true;
    case Op::rlwimi: { const uint64_t m = mask(mb + 32, me + 32); logical((rotl32(r[rt], rb) & m) | (r[ra] & ~m)); return true; }
    case Op::rldicl: logical(rotl64(r[rt], sh6) & mask(mb6, 63)); return true;
    case Op::rldicr: logical(rotl64(r[rt], sh6) & mask(0, mb6)); return true;
    case Op::rldic: logical(rotl64(r[rt], sh6) & mask(mb6, 63 - sh6)); return true;
    case Op::rldimi: { const uint64_t m = mask(mb6, 63 - sh6); logical((rotl64(r[rt], sh6) & m) | (r[ra] & ~m)); return true; }
    case Op::rldcl: logical(rotl64(r[rt], static_cast<unsigned>(r[rb] & 63)) & mask(mb6, 63)); return true;
    case Op::rldcr: logical(rotl64(r[rt], static_cast<unsigned>(r[rb] & 63)) & mask(0, mb6)); return true;
    case Op::slw: { const unsigned n = r[rb] & 63; logical(n > 31 ? 0 : (r[rt] << n) & 0xffffffffu); return true; }
    case Op::srw: { const unsigned n = r[rb] & 63; logical(n > 31 ? 0 : (r[rt] & 0xffffffffu) >> n); return true; }
    case Op::sld: { const unsigned n = r[rb] & 127; logical(n > 63 ? 0 : r[rt] << n); return true; }
    case Op::srd: { const unsigned n = r[rb] & 127; logical(n > 63 ? 0 : r[rt] >> n); return true; }
    case Op::sraw: case Op::srawi: case Op::srad: case Op::sradi: {
        const bool wide = in.op == Op::srad || in.op == Op::sradi;
        const unsigned width = wide ? 64 : 32;
        unsigned n = in.op == Op::srawi ? rb : in.op == Op::sradi ? sh6 : static_cast<unsigned>(r[rb] & (wide ? 127 : 63));
        const uint64_t value = wide ? r[rt] : sign_extend(r[rt], 32);
        const bool negative = value >> 63;
        // One place at a time: the bits that fall off decide CA.
        uint64_t result = value;
        bool lost = false;
        for (unsigned i = 0; i < n && i < width; ++i) {
            lost = lost || (result & 1);
            result = (result >> 1) | (negative ? bit(0) : 0);
        }
        if (n >= width) {
            result = negative ? ~uint64_t{0} : 0;
            lost = wide ? value != 0 : (value & 0xffffffffu) != 0;
        }
        set_ca(s, negative && lost);
        logical(result);
        return true;
    }

    case Op::mcrf: set_cr_field(s, (w >> 23) & 7, (s.cr >> (28 - 4 * ((w >> 18) & 7))) & 15); return true;
    case Op::mfcr: r[rt] = s.cr; return true;
    case Op::mfocrf: {  // the named field in its place, the rest zero (cpu.hpp's choice for the undefined bits)
        const unsigned fxm = (w >> 12) & 0xff;
        uint32_t keep = 0;
        for (unsigned f = 0; f < 8; ++f)
            if (fxm & (0x80u >> f)) keep |= uint32_t{15} << (28 - 4 * f);
        r[rt] = s.cr & keep;
        return true;
    }
    case Op::mtcrf: case Op::mtocrf: {
        const unsigned fxm = (w >> 12) & 0xff;
        for (unsigned f = 0; f < 8; ++f)
            if (fxm & (0x80u >> f)) set_cr_field(s, f, static_cast<uint32_t>(r[rt] >> (28 - 4 * f)) & 15);
        return true;
    }
    case Op::crand: set_cr_bit(s, rt, cr_bit(s, ra) & cr_bit(s, rb)); return true;
    case Op::cror: set_cr_bit(s, rt, cr_bit(s, ra) | cr_bit(s, rb)); return true;
    case Op::crxor: set_cr_bit(s, rt, cr_bit(s, ra) ^ cr_bit(s, rb)); return true;
    case Op::crnand: set_cr_bit(s, rt, ~(cr_bit(s, ra) & cr_bit(s, rb))); return true;
    case Op::crnor: set_cr_bit(s, rt, ~(cr_bit(s, ra) | cr_bit(s, rb))); return true;
    case Op::creqv: set_cr_bit(s, rt, ~(cr_bit(s, ra) ^ cr_bit(s, rb))); return true;
    case Op::crandc: set_cr_bit(s, rt, cr_bit(s, ra) & ~cr_bit(s, rb)); return true;
    case Op::crorc: set_cr_bit(s, rt, cr_bit(s, ra) | ~cr_bit(s, rb)); return true;

    case Op::lbz: load_to(ea_d, 1, false, false); return true;
    case Op::lbzu: load_to(ea_d, 1, false, true); return true;
    case Op::lbzx: load_to(ea_x, 1, false, false); return true;
    case Op::lbzux: load_to(ea_x, 1, false, true); return true;
    case Op::lhz: load_to(ea_d, 2, false, false); return true;
    case Op::lhzu: load_to(ea_d, 2, false, true); return true;
    case Op::lhzx: load_to(ea_x, 2, false, false); return true;
    case Op::lhzux: load_to(ea_x, 2, false, true); return true;
    case Op::lha: load_to(ea_d, 2, true, false); return true;
    case Op::lhau: load_to(ea_d, 2, true, true); return true;
    case Op::lhax: load_to(ea_x, 2, true, false); return true;
    case Op::lhaux: load_to(ea_x, 2, true, true); return true;
    case Op::lwz: load_to(ea_d, 4, false, false); return true;
    case Op::lwzu: load_to(ea_d, 4, false, true); return true;
    case Op::lwzx: load_to(ea_x, 4, false, false); return true;
    case Op::lwzux: load_to(ea_x, 4, false, true); return true;
    case Op::lwa: load_to(ea_ds, 4, true, false); return true;
    case Op::lwax: load_to(ea_x, 4, true, false); return true;
    case Op::lwaux: load_to(ea_x, 4, true, true); return true;
    case Op::ld: load_to(ea_ds, 8, false, false); return true;
    case Op::ldu: load_to(ea_ds, 8, false, true); return true;
    case Op::ldx: load_to(ea_x, 8, false, false); return true;
    case Op::ldux: load_to(ea_x, 8, false, true); return true;
    case Op::lhbrx: load_to(ea_x, 2, false, false, true); return true;
    case Op::lwbrx: load_to(ea_x, 4, false, false, true); return true;
    case Op::ldbrx: load_to(ea_x, 8, false, false, true); return true;

    case Op::stb: store_from(ea_d, 1, false); return true;
    case Op::stbu: store_from(ea_d, 1, true); return true;
    case Op::stbx: store_from(ea_x, 1, false); return true;
    case Op::stbux: store_from(ea_x, 1, true); return true;
    case Op::sth: store_from(ea_d, 2, false); return true;
    case Op::sthu: store_from(ea_d, 2, true); return true;
    case Op::sthx: store_from(ea_x, 2, false); return true;
    case Op::sthux: store_from(ea_x, 2, true); return true;
    case Op::stw: store_from(ea_d, 4, false); return true;
    case Op::stwu: store_from(ea_d, 4, true); return true;
    case Op::stwx: store_from(ea_x, 4, false); return true;
    case Op::stwux: store_from(ea_x, 4, true); return true;
    case Op::std: store_from(ea_ds, 8, false); return true;
    case Op::stdu: store_from(ea_ds, 8, true); return true;
    case Op::stdx: store_from(ea_x, 8, false); return true;
    case Op::stdux: store_from(ea_x, 8, true); return true;
    case Op::sthbrx: store_from(ea_x, 2, false, true); return true;
    case Op::stwbrx: store_from(ea_x, 4, false, true); return true;
    case Op::stdbrx: store_from(ea_x, 8, false, true); return true;

    case Op::mfspr: case Op::mtspr: {
        const unsigned spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5);
        uint64_t* const reg = spr == 1 ? &s.xer : spr == 8 ? &s.lr : spr == 9 ? &s.ctr : nullptr;
        if (!reg) return false;
        if (in.op == Op::mfspr) r[rt] = *reg;
        else *reg = spr == 1 ? (r[rt] & (kSo | kOv | kCa | 0x7f)) : r[rt];  // XER keeps only the bits it has
        return true;
    }

    case Op::sync: case Op::eieio: case Op::isync: case Op::dcbt: case Op::dcbtst: case Op::dcbst: case Op::dcbf: case Op::icbi:
        return true;  // nothing a program can see
    default: return false;
    }
}

}  // namespace qilin::ref

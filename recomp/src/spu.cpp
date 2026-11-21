#include "spu.hpp"

#include <array>
#include <format>
#include <stdexcept>

namespace qilin::spu {

namespace {

// The fields, as masks of the word.
constexpr uint32_t kRt = 0x0000007f, kRa = 0x00003f80, kRb = 0x001fc000, kRt4 = 0x0fe00000, kI8 = 0x003fc000, kI10 = 0x00ffc000,
                   kI16 = 0x007fff80, kI18 = 0x01ffff80, kU14 = 0x00003fff;
constexpr uint32_t kC = 0x00100000;        // bit 11: sync's C, hbr's P
constexpr uint32_t kDE = 0x000c0000;       // bits 12 and 13 of an indirect branch
constexpr uint32_t kHintLow = 0x0000007f;  // the low seven bits of a hint's branch offset
constexpr uint32_t kHintHighRa = 0x0000c000, kHintHigh16 = 0x01800000;  // its top two, in the two layouts

// The operand fields of a form (not the opcode's bits).
constexpr uint32_t fields(Form f) {
    switch (f) {
    case Form::none: return 0;
    case Form::sync: return kC;
    case Form::nop: case Form::rt1: return kRt;
    case Form::stop: return kU14;
    case Form::rr: case Form::ri7: case Form::ri7_index: return kRt | kRa | kRb;
    case Form::rr2: case Form::spr_read: case Form::spr_write: case Form::ch_read: case Form::ch_write: return kRt | kRa;
    case Form::ra1: return kRa;
    case Form::rrr: return kRt4 | kRb | kRa | kRt;
    case Form::scale173: case Form::scale155: return kRt | kRa | kI8;
    case Form::ri10: case Form::ri10_index: return kRt | kRa | kI10;
    case Form::ri16: case Form::rx16: case Form::rt_rel: case Form::rt_abs: return kRt | kI16;
    case Form::rel: case Form::abs: return kI16;
    case Form::ri18: return kRt | kI18;
    case Form::branch_ra: return kRa | kDE;
    case Form::branch_rt_ra: return kRt | kRa | kDE;
    case Form::hint_ra: return kHintLow | kHintHighRa | kRa | kC;
    case Form::hint_abs: case Form::hint_rel: return kHintLow | kHintHigh16 | kI16;
    }
    return 0;
}

struct Row {
    Op op;
    std::string_view name;
    uint8_t bits;
    uint16_t opcode;
    Form form;
    uint32_t flags;
};

constexpr Row kRows[] = {
#define QILIN_SPU_ROW(id, name, bits, opcode, form, flags) {Op::id, name, bits, opcode, Form::form, flags},
    QILIN_SPU_OPS(QILIN_SPU_ROW)
#undef QILIN_SPU_ROW
};
constexpr size_t kRowCount = std::size(kRows);
static_assert(kRowCount == static_cast<size_t>(Op::count_) - 1);

struct Tables {
    std::array<Spec, static_cast<size_t>(Op::count_)> by_op{};
    std::array<Spec, kRowCount> in_order{};
    std::array<uint16_t, 2048> by_opcode{};  // Op by the word's first 11 bits
};

Tables build() {
    Tables t;
    t.by_op[0] = Spec{Op::invalid, "(invalid)", 0, 0, Form::none, 0, 0};
    for (size_t i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        if (r.bits != 4 && (r.bits < 7 || r.bits > 11)) throw std::logic_error(std::format("SPU decoder: {} has an opcode of {} bits", r.name, r.bits));
        if (r.opcode >> r.bits) throw std::logic_error(std::format("SPU decoder: {}'s opcode doesn't fit its {} bits", r.name, r.bits));
        const uint32_t opcode_mask = ~uint32_t{0} << (32 - r.bits);
        if (opcode_mask & fields(r.form)) throw std::logic_error(std::format("SPU decoder: {}'s operands overlap its opcode", r.name));
        const Spec s{r.op, r.name, r.bits, r.opcode, r.form, r.flags, opcode_mask | fields(r.form)};
        t.by_op[static_cast<size_t>(r.op)] = s;
        t.in_order[i] = s;
        const unsigned shift = 11 - r.bits;
        for (unsigned low = 0; low < (1u << shift); ++low) {
            uint16_t& slot = t.by_opcode[(static_cast<unsigned>(r.opcode) << shift) | low];
            if (slot) throw std::logic_error(std::format("SPU decoder: {} overlaps {}", r.name, t.by_op[slot].name));
            slot = static_cast<uint16_t>(r.op);
        }
    }
    return t;
}

const Tables& tables() {
    static const Tables t = build();
    return t;
}

int32_t hint_offset(uint32_t word, bool high_at_16) {
    const uint32_t high = high_at_16 ? (word >> 14) & 3 : (word >> 23) & 3;
    const uint32_t nine = (high << 7) | (word & 0x7f);
    return (static_cast<int32_t>(nine << 23) >> 23) * 4;
}

std::string address(uint32_t a) { return std::format("0x{:x}", a); }

}  // namespace

uint32_t Insn::hinted(uint32_t pc) const { return pc + static_cast<uint32_t>(hint_offset(word, op == Op::hbr)); }

const Spec& spec(Op op) { return tables().by_op[static_cast<size_t>(op)]; }

std::span<const Spec> specs() { return tables().in_order; }

Insn decode(uint32_t word, bool ignore_reserved) {
    const Tables& t = tables();
    const uint16_t code = t.by_opcode[word >> 21];
    if (!code) return {word, Op::invalid};
    if (!ignore_reserved && (word & ~t.by_op[code].defined)) return {word, Op::invalid};
    return {word, static_cast<Op>(code)};
}

std::string disasm(const Insn& in, uint32_t pc) {
    if (!in.valid()) return std::format(".long 0x{:x}", in.word);
    const Spec& s = spec(in.op);
    std::string name(s.name);
    // objdump names bits 11, 12 and 13 of these forms in the mnemonic: "bid",
    // "bie", "syncc", "hbrp". (Only some are defined for each instruction;
    // the rest show when the caller asked to look past reserved bits.)
    if (s.form == Form::sync || s.form == Form::branch_ra || s.form == Form::branch_rt_ra || s.form == Form::hint_ra) {
        if (in.word & kC) name += s.form == Form::sync ? 'c' : 'p';
        if (in.word & 0x00080000) name += 'd';
        if (in.word & 0x00040000) name += 'e';
    }
    const auto r = [](unsigned n) { return std::format("${}", n); };
    // objdump prints a zero displacement as "0", and any other as the address it reaches.
    const auto rel = [&] { return in.i16() == 0 ? std::string("0") : address(in.relative(pc)); };
    const auto abs = [&] { return in.u16() == 0 ? std::string("0") : address(in.u16() << 2); };
    switch (s.form) {
    case Form::none: case Form::sync: return name;
    case Form::nop: case Form::rt1: return std::format("{} {}", name, r(in.rt()));
    case Form::stop: return name;  // objdump leaves the signal type out
    case Form::rr: return std::format("{} {},{},{}", name, r(in.rt()), r(in.ra()), r(in.rb()));
    case Form::rr2: return std::format("{} {},{}", name, r(in.rt()), r(in.ra()));
    case Form::ra1: case Form::branch_ra: return std::format("{} {}", name, r(in.ra()));
    case Form::branch_rt_ra: return std::format("{} {},{}", name, r(in.rt()), r(in.ra()));
    case Form::rrr: return std::format("{} {},{},{},{}", name, r(in.rt4()), r(in.ra()), r(in.rb()), r(in.rt()));
    case Form::ri7: return std::format("{} {},{},{}", name, r(in.rt()), r(in.ra()), in.i7());
    case Form::ri7_index: return std::format("{} {},{}({})", name, r(in.rt()), in.i7(), r(in.ra()));
    case Form::scale173: return std::format("{} {},{},{}", name, r(in.rt()), r(in.ra()), 173 - static_cast<int>(in.u8()));
    case Form::scale155: return std::format("{} {},{},{}", name, r(in.rt()), r(in.ra()), 155 - static_cast<int>(in.u8()));
    case Form::ri10: return std::format("{} {},{},{}", name, r(in.rt()), r(in.ra()), in.i10());
    case Form::ri10_index: return std::format("{} {},{}({})", name, r(in.rt()), in.i10() * 16, r(in.ra()));
    case Form::ri16: return std::format("{} {},{}", name, r(in.rt()), in.i16());
    case Form::rx16: return std::format("{} {},{}", name, r(in.rt()), in.u16());
    case Form::rt_rel: return std::format("{} {},{}", name, r(in.rt()), rel());
    case Form::rt_abs: return std::format("{} {},{}", name, r(in.rt()), abs());
    case Form::rel: return std::format("{} {}", name, rel());
    case Form::abs: return std::format("{} {}", name, abs());
    case Form::ri18: return std::format("{} {},{}", name, r(in.rt()), in.u18());
    case Form::spr_read: return std::format("{} {},$sp{}", name, r(in.rt()), in.ra());
    case Form::spr_write: return std::format("{} $sp{},{}", name, in.ra(), r(in.rt()));
    case Form::ch_read: return std::format("{} {},$ch{}", name, r(in.rt()), in.ra());
    case Form::ch_write: return std::format("{} $ch{},{}", name, in.ra(), r(in.rt()));
    case Form::hint_ra: return std::format("{} {},{}", name, address(in.hinted(pc)), r(in.ra()));
    case Form::hint_abs: return std::format("{} {},{}", name, address(in.hinted(pc)), abs());
    case Form::hint_rel: return std::format("{} {},{}", name, address(in.hinted(pc)), rel());
    }
    return name;
}

}  // namespace qilin::spu

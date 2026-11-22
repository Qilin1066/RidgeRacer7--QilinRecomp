#include "rsx_program.hpp"

#include <expected>
#include <cstdlib>
#include <format>
#include <set>

#include "bytes.hpp"

namespace qilin::rsx {

namespace {

constexpr char kComponent[] = "xyzw";

uint32_t bits(uint32_t w, unsigned lo, unsigned count) { return (w >> lo) & ((count == 32 ? 0u : 1u << count) - 1u); }

std::string swizzle_text(const std::array<uint8_t, 4>& s) {
    if (s == std::array<uint8_t, 4>{0, 1, 2, 3}) return "";
    std::string t = ".";
    for (const uint8_t c : s) t += kComponent[c];
    return t;
}
std::string mask_text(unsigned mask) {  // x = 8
    if (mask == 0xf) return "";
    std::string t = ".";
    for (unsigned i = 0; i < 4; ++i)
        if (mask & (8u >> i)) t += kComponent[i];
    return t;
}
constexpr const char* kCond[] = {"FL", "LT", "EQ", "LE", "GT", "NE", "GE", "TR"};

}  // namespace

// ---------------------------------------------------------------- vertex programs

const char* vp_vector_name(unsigned op) {
    static constexpr const char* kNames[] = {"NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN", "MAX", "SLT", "SGE",
                                             "ARL", "FRC", "FLR", "SEQ", "SFL", "SGT", "SLE", "SNE", "STR", "SSG", "ARR", "ARA", "TXL"};
    return op < std::size(kNames) ? kNames[op] : nullptr;
}
const char* vp_scalar_name(unsigned op) {
    static constexpr const char* kNames[] = {"NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT", nullptr, "BRA", nullptr, "CAL",
                                             "RET", "LG2", "EX2", "SIN", "COS", nullptr, nullptr, "PUSHA", "POPA"};
    return op < std::size(kNames) ? kNames[op] : nullptr;
}

VpInsn decode_vp(std::span<const uint32_t, 4> w) {
    VpInsn in;
    for (unsigned i = 0; i < 4; ++i) in.w[i] = w[i];
    std::array<uint32_t, 4> known{};
    const auto take = [&](unsigned word, unsigned lo, unsigned count) {
        known[word] |= (count == 32 ? ~0u : ((1u << count) - 1u)) << lo;
        return bits(w[word], lo, count);
    };
    // Word 0.
    in.addr_swizzle = take(0, 0, 2);
    for (unsigned i = 0; i < 4; ++i) in.cond_swizzle[i] = static_cast<uint8_t>(take(0, 8 - 2 * i, 2));
    in.cond = take(0, 10, 3);
    in.cond_test = take(0, 13, 1);
    const bool update_low = take(0, 14, 1);
    in.vec_temp = take(0, 15, 6);
    in.src[0].abs = take(0, 21, 1);
    in.src[1].abs = take(0, 22, 1);
    in.src[2].abs = take(0, 23, 1);
    in.addr_reg1 = take(0, 24, 1);
    in.cond_reg1 = take(0, 25, 1);
    in.saturate = take(0, 26, 1);
    in.index_input = take(0, 27, 1);
    const bool update_high = take(0, 29, 1);
    in.cond_update = update_low || update_high;
    in.vec_result = take(0, 30, 1);
    // Word 1.
    const uint32_t src0_high = take(1, 0, 8);
    in.input = take(1, 8, 4);
    in.constant = take(1, 12, 10);  // (bits 20 and 21 too: the game reads constant 467)
    in.vec_op = take(1, 22, 5);
    in.sca_op = take(1, 27, 5);
    // Word 2.
    const uint32_t src2_high = take(2, 0, 6);
    const uint32_t src1 = take(2, 6, 17);
    const uint32_t src0_low = take(2, 23, 9);
    // Word 3.
    in.last = take(3, 0, 1);
    in.index_const = take(3, 1, 1);
    in.dest = take(3, 2, 5);
    in.sca_temp = take(3, 7, 5);
    in.sca_result = take(3, 12, 1);
    in.vec_mask = take(3, 13, 4);
    in.sca_mask = take(3, 17, 4);
    const uint32_t src2_low = take(3, 21, 11);
    // The sources: 17 bits each, split across the words.
    const uint32_t raw[3] = {(src0_high << 9) | src0_low, src1, (src2_high << 11) | src2_low};
    for (unsigned i = 0; i < 3; ++i) {
        VpSource& s = in.src[i];
        s.type = static_cast<VpRegister>(bits(raw[i], 0, 2));
        s.temp = bits(raw[i], 2, 6);
        for (unsigned c = 0; c < 4; ++c) s.swizzle[c] = static_cast<uint8_t>(bits(raw[i], 14 - 2 * c, 2));
        s.negate = bits(raw[i], 16, 1);
    }
    in.branch_target = (src2_high << 3) | bits(w[3], 29, 3);
    for (unsigned i = 0; i < 4; ++i) in.unknown[i] = w[i] & ~known[i];
    return in;
}

namespace {

std::string vp_source(const VpInsn& in, unsigned i) {
    const VpSource& s = in.src[i];
    std::string t;
    switch (s.type) {
    case VpRegister::temp: t = std::format("R{}", s.temp); break;
    case VpRegister::input: t = in.index_input ? std::format("v[A0.{}+{}]", kComponent[in.addr_swizzle], in.input) : std::format("v[{}]", in.input); break;
    case VpRegister::constant: t = in.index_const ? std::format("c[A{}.{}+{}]", in.addr_reg1 ? 1 : 0, kComponent[in.addr_swizzle], in.constant) : std::format("c[{}]", in.constant); break;
    default: t = "?0"; break;
    }
    t += swizzle_text(s.swizzle);
    if (s.abs) t = "|" + t + "|";
    return s.negate ? "-" + t : t;
}
// How many sources each operation reads, and from which of the three slots.
unsigned vec_sources(unsigned op) {
    switch (op) {
    case kVecNop: return 0;
    case kVecMov: case kVecArl: case kVecFrc: case kVecFlr: case kVecSsg: case kVecArr: case kVecAra: case kVecTxl: return 1;
    case kVecMad: return 3;
    default: return 2;
    }
}

}  // namespace

std::string disasm(const VpInsn& in) {
    std::string out;
    const auto dest_text = [&](bool result, unsigned temp, unsigned mask) {
        static constexpr const char* kOut[] = {"o[POS]", "o[COL0]", "o[COL1]", "o[BFC0]", "o[BFC1]", "o[FOG]", "o[PSZ]", "o[TC0]", "o[TC1]", "o[TC2]",
                                               "o[TC3]", "o[TC4]", "o[TC5]", "o[TC6]", "o[TC7]", "o[TC8]", "o[TC9]"};
        std::string d = result ? (in.dest < std::size(kOut) ? kOut[in.dest] : std::format("o[{}]", in.dest)) : std::format("R{}", temp);
        if (result && temp != 0x3F) d += std::format(" R{}", temp);  // (both: an output and a temp)
        return d + mask_text(mask);
    };
    const std::string cond = in.cond_test ? std::format(" ({}{}{})", in.cond_reg1 ? "CC1." : "", kCond[in.cond], swizzle_text(in.cond_swizzle)) : "";
    const std::string flags = std::string(in.saturate ? "_SAT" : "") + (in.cond_update ? (in.cond_reg1 ? "C1" : "C") : "");
    if (in.vec_op != kVecNop) {
        const char* name = vp_vector_name(in.vec_op);
        out += std::format("{}{} {}", name ? name : std::format("vec{:#x}", in.vec_op).c_str(), flags, dest_text(in.vec_result, in.vec_temp, in.vec_mask));
        out += cond;
        const unsigned n = vec_sources(in.vec_op);
        for (unsigned i = 0; i < n; ++i) out += ", " + vp_source(in, i);
    }
    if (in.sca_op != kScaNop) {
        if (!out.empty()) out += "  +  ";
        const char* name = vp_scalar_name(in.sca_op);
        if (in.sca_op == kScaBra || in.sca_op == kScaCal) {
            out += std::format("{} {}{}", name, in.branch_target, cond);
        } else if (in.sca_op == kScaRet) {
            out += std::string(name) + cond;
        } else {
            out += std::format("{}{} {}{}, {}", name ? name : std::format("sca{:#x}", in.sca_op).c_str(), flags,
                               dest_text(in.sca_result, in.sca_result ? 0x3F : in.sca_temp, in.sca_mask), cond, vp_source(in, 2));
        }
    }
    if (out.empty()) out = "NOP";
    if (in.last) out += "  (last)";
    return out;
}

// ---------------------------------------------------------------- fragment programs

const char* fp_name(unsigned op) {
    switch (op) {
    case kFpNop: return "NOP"; case kFpMov: return "MOV"; case kFpMul: return "MUL"; case kFpAdd: return "ADD"; case kFpMad: return "MAD";
    case kFpDp3: return "DP3"; case kFpDp4: return "DP4"; case kFpDst: return "DST"; case kFpMin: return "MIN"; case kFpMax: return "MAX";
    case kFpSlt: return "SLT"; case kFpSge: return "SGE"; case kFpSle: return "SLE"; case kFpSgt: return "SGT"; case kFpSne: return "SNE";
    case kFpSeq: return "SEQ"; case kFpFrc: return "FRC"; case kFpFlr: return "FLR"; case kFpKil: return "KIL"; case kFpPk4b: return "PK4B";
    case kFpUp4b: return "UP4B"; case kFpDdx: return "DDX"; case kFpDdy: return "DDY"; case kFpTex: return "TEX"; case kFpTxp: return "TXP";
    case kFpTxd: return "TXD"; case kFpRcp: return "RCP"; case kFpRsq: return "RSQ"; case kFpEx2: return "EX2"; case kFpLg2: return "LG2";
    case kFpLit: return "LIT"; case kFpLrp: return "LRP"; case kFpStr: return "STR"; case kFpSfl: return "SFL"; case kFpCos: return "COS";
    case kFpSin: return "SIN"; case kFpPk2h: return "PK2H"; case kFpUp2h: return "UP2H"; case kFpPow: return "POW"; case kFpPk4ub: return "PK4UB";
    case kFpUp4ub: return "UP4UB"; case kFpPk2us: return "PK2US"; case kFpUp2us: return "UP2US"; case kFpDp2a: return "DP2A"; case kFpTxl: return "TXL";
    case kFpTxb: return "TXB"; case kFpRfl: return "RFL"; case kFpDp2: return "DP2"; case kFpNrm: return "NRM"; case kFpDiv: return "DIV";
    case kFpDivsq: return "DIVSQ"; case kFpLif: return "LIF"; case kFpFenct: return "FENCT"; case kFpFencb: return "FENCB";
    default: return nullptr;
    }
}
const char* fp_branch_name(unsigned op) {
    static constexpr const char* kNames[] = {"BRK", "CAL", "IF", "LOOP", "REP", "RET"};
    return op < std::size(kNames) ? kNames[op] : nullptr;
}

namespace {

uint32_t exchanged(const uint8_t* p) {  // a word of fragment microcode, its halves put back
    const uint32_t w = read_be32(p);
    return (w << 16) | (w >> 16);
}

}  // namespace

std::vector<FpInsn> decode_fp(std::span<const uint8_t> microcode, size_t* words) {
    std::vector<FpInsn> out;
    size_t at = 0;
    while (at + 16 <= microcode.size()) {
        FpInsn in;
        for (unsigned i = 0; i < 4; ++i) in.w[i] = exchanged(microcode.data() + at + 4 * i);
        at += 16;
        std::array<uint32_t, 4> known{};
        const auto take = [&](unsigned word, unsigned lo, unsigned count) {
            known[word] |= ((1u << count) - 1u) << lo;
            return bits(in.w[word], lo, count);
        };
        in.end = take(0, 0, 1);
        in.out = take(0, 1, 6);
        in.out_half = take(0, 7, 1);
        in.cond_write = take(0, 8, 1);
        const uint32_t mask = take(0, 9, 4);  // bit 9 x
        in.mask = ((mask & 1) << 3) | ((mask & 2) << 1) | ((mask & 4) >> 1) | ((mask & 8) >> 3);
        in.input = take(0, 13, 4);
        in.tex_unit = take(0, 17, 4);
        in.precision = take(0, 22, 2);
        in.op = take(0, 24, 6);
        in.no_dest = take(0, 30, 1);
        in.saturate = take(0, 31, 1);
        in.cond = take(1, 18, 3);
        for (unsigned i = 0; i < 4; ++i) in.cond_swizzle[i] = static_cast<uint8_t>(take(1, 21 + 2 * i, 2));
        in.src[0].abs = take(1, 29, 1);
        in.src[1].abs = take(2, 18, 1);
        in.scale = take(2, 28, 3);
        in.src1_extra = take(2, 19, 3);
        in.branch = take(2, 31, 1);
        in.src[2].abs = take(3, 18, 1);
        in.index_input = take(3, 30, 1);
        in.input_displacement = take(3, 19, 11);
        in.no_perspective = take(3, 31, 1);
        if (in.branch) {
            // Flow control: the sources' words hold its offsets and counts, not sources (nouveau's header).
            known[1] = known[2] = known[3] = ~0u;
            in.else_at = in.w[2] & 0x7fffffffu;
            in.target = in.w[2] & 0x7fffffffu;
            in.end_at = in.w[3] & 0x7fffffffu;
            in.loop_count = bits(in.w[2], 2, 8), in.loop_init = bits(in.w[2], 10, 8), in.loop_step = bits(in.w[2], 19, 8);
        } else {
            for (unsigned i = 0; i < 3; ++i) {
                FpSource& s = in.src[i];
                const uint32_t raw = in.w[1 + i];
                known[1 + i] |= 0x3ffffu;
                s.type = static_cast<FpRegister>(bits(raw, 0, 2));
                s.index = bits(raw, 2, 6);
                s.half = bits(raw, 8, 1);
                for (unsigned c = 0; c < 4; ++c) s.swizzle[c] = static_cast<uint8_t>(bits(raw, 9 + 2 * c, 2));
                s.negate = bits(raw, 17, 1);
            }
        }
        for (unsigned i = 0; i < 4; ++i) in.unknown[i] = in.w[i] & ~known[i];
        // A source that is a constant: it follows the instruction (only the sources the operation reads, which the
        // check confirms: a constant is never named by a source the operation doesn't read).
        if (!in.branch)
            for (const FpSource& s : in.src)
                if (s.type == FpRegister::constant) in.has_constant = true;
        if (in.has_constant && at + 16 <= microcode.size()) {
            for (unsigned i = 0; i < 4; ++i) in.constant[i] = exchanged(microcode.data() + at + 4 * i);
            at += 16;
        }
        out.push_back(in);
        if (in.end) break;
    }
    if (words) *words = at / 4;
    return out;
}

namespace {

unsigned fp_sources(unsigned op) {
    switch (op) {
    case kFpNop: case kFpKil: case kFpFenct: case kFpFencb: return 0;
    case kFpMov: case kFpFrc: case kFpFlr: case kFpPk4b: case kFpUp4b: case kFpDdx: case kFpDdy: case kFpTex: case kFpTxp: case kFpRcp:
    case kFpRsq: case kFpEx2: case kFpLg2: case kFpLit: case kFpCos: case kFpSin: case kFpPk2h: case kFpUp2h: case kFpPk4ub: case kFpUp4ub:
    case kFpPk2us: case kFpUp2us: case kFpNrm: case kFpTxl: case kFpTxb: return 1;
    case kFpMad: case kFpLrp: case kFpDp2a: case kFpTxd: return 3;
    default: return 2;
    }
}
std::string fp_source(const FpInsn& in, unsigned i) {
    static constexpr const char* kIn[] = {"WPOS", "COL0", "COL1", "FOGC", "TEX0", "TEX1", "TEX2", "TEX3", "TEX4", "TEX5", "TEX6", "TEX7", "TEX8", "TEX9", "SSA", "?"};
    const FpSource& s = in.src[i];
    std::string t;
    switch (s.type) {
    case FpRegister::temp: t = std::format("{}{}", s.half ? "H" : "R", s.index); break;
    case FpRegister::input: t = std::format("f[{}]", kIn[in.input & 15]); break;
    case FpRegister::constant: t = std::format("{{{:#x}, {:#x}, {:#x}, {:#x}}}", in.constant[0], in.constant[1], in.constant[2], in.constant[3]); break;
    default: t = "?3"; break;
    }
    t += swizzle_text(s.swizzle);
    if (s.abs) t = "|" + t + "|";
    return s.negate ? "-" + t : t;
}

}  // namespace

std::string disasm(const FpInsn& in) {
    std::string out;
    if (in.branch) {
        const char* name = fp_branch_name(in.op);
        out = name ? name : std::format("branch{:#x}", in.op);
        const std::string cond = std::format(" ({}{})", kCond[in.cond], swizzle_text(in.cond_swizzle));
        switch (in.op) {
        case kFpIf: out += std::format("{} else {} end {}", cond, in.else_at, in.end_at); break;
        case kFpLoop: case kFpRep: out += std::format("{} count {} init {} step {} end {}", cond, in.loop_count, in.loop_init, in.loop_step, in.end_at); break;
        case kFpCal: out += std::format("{} {}", cond, in.target); break;
        default: out += cond; break;
        }
    } else {
        const char* name = fp_name(in.op);
        static constexpr const char* kPrecision[] = {"R", "H", "X", "B"};
        static constexpr const char* kScale[] = {"", "_x2", "_x4", "_x8", "_?4", "_d2", "_d4", "_d8"};
        out = std::format("{}{}{}{}{}", name ? name : std::format("op{:#x}", in.op).c_str(), kPrecision[in.precision], kScale[in.scale], in.cond_write ? "C" : "",
                          in.saturate ? "_SAT" : "");
        out += in.no_dest ? " CC" : std::format(" {}{}{}", in.out_half ? "H" : "R", in.out, mask_text(in.mask));
        if (in.cond != 7 || in.cond_swizzle != std::array<uint8_t, 4>{0, 1, 2, 3}) out += std::format(" ({}{})", kCond[in.cond], swizzle_text(in.cond_swizzle));
        const unsigned n = fp_sources(in.op);
        for (unsigned i = 0; i < n; ++i) out += ", " + fp_source(in, i);
        if (in.op == kFpTex || in.op == kFpTxp || in.op == kFpTxl || in.op == kFpTxb || in.op == kFpTxd) out += std::format(", TEX{}", in.tex_unit);
    }
    if (in.end) out += "  (end)";
    return out;
}

// ---------------------------------------------------------------- the compiled programs

std::expected<CgProgram, std::string> parse_cg(std::span<const uint8_t> b, uint32_t address) {
    const auto fail = [&](std::string what) { return std::unexpected(std::format("the compiled shader at {:#x}: {}", address, what)); };
    if (b.size() < 32) return fail("too short");
    CgProgram p;
    p.address = address;
    const uint32_t profile = read_be32(b.data()), size = read_be32(b.data() + 8), count = read_be32(b.data() + 12), params_at = read_be32(b.data() + 16),
                   program_at = read_be32(b.data() + 20), microcode_size = read_be32(b.data() + 24), microcode_at = read_be32(b.data() + 28);
    p.vertex = profile == 7003;
    if (size > b.size() || program_at + 40 > size || params_at + uint64_t{count} * 48 > size || uint64_t{microcode_at} + microcode_size > size) return fail("its tables are outside it");
    for (unsigned i = 0; i < (p.vertex ? 6u : 7u); ++i) p.header.push_back(read_be32(b.data() + program_at + 4 * i));
    const auto text = [&](uint32_t at) -> std::string {
        if (at == 0) return "";
        std::string t;
        for (; at < size && b[at]; ++at) t += static_cast<char>(b[at]);
        return t;
    };
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* q = b.data() + params_at + 48 * i;
        CgParameter c;
        c.type = read_be32(q);
        c.resource = read_be32(q + 4);
        c.variability = read_be32(q + 8);
        c.resource_index = static_cast<int32_t>(read_be32(q + 12));
        c.name = text(read_be32(q + 16));
        const uint32_t embedded = read_be32(q + 24);
        c.semantic = text(read_be32(q + 28));
        c.direction = read_be32(q + 32);
        c.referenced = read_be32(q + 40) != 0;
        if (embedded) {
            if (embedded + 4 > size) return fail(std::format("parameter {}'s constants are outside it", c.name));
            const uint32_t n = read_be32(b.data() + embedded);
            if (embedded + 4 + uint64_t{n} * 4 > size) return fail(std::format("parameter {}'s constants are outside it", c.name));
            for (uint32_t k = 0; k < n; ++k) c.embedded.push_back(read_be32(b.data() + embedded + 4 + 4 * k));
        }
        p.parameters.push_back(std::move(c));
    }
    p.microcode.assign(b.begin() + microcode_at, b.begin() + microcode_at + microcode_size);
    return p;
}

std::vector<std::string> check_program(const CgProgram& p) {
    std::vector<std::string> wrong;
    const auto bad = [&](std::string what) { wrong.push_back(std::move(what)); };
    // Parameters' variability: a uniform the game sets, or a literal the compiler made ("internal-constant-N", its value its default).
    constexpr uint32_t kUniform = 0x1006, kLiteral = 0x1007, kIn = 0x1001, kOut = 0x1002;
    if (p.vertex) {
        if (p.microcode.size() % 16) bad("its microcode isn't whole instructions");
        const size_t n = p.microcode.size() / 16;
        if (p.header[0] != n) bad(std::format("its header says {} instructions and its microcode holds {}", p.header[0], n));
        std::set<unsigned> constants, inputs;
        uint32_t outputs = 0;
        for (size_t i = 0; i < n; ++i) {
            uint32_t w[4];
            for (unsigned k = 0; k < 4; ++k) w[k] = read_be32(p.microcode.data() + 16 * i + 4 * k);
            const VpInsn in = decode_vp(std::span<const uint32_t, 4>(w, 4));
            if (in.unknown != std::array<uint32_t, 4>{}) bad(std::format("instruction {} sets bits no field covers: {:08x} {:08x} {:08x} {:08x}", i, in.unknown[0], in.unknown[1], in.unknown[2], in.unknown[3]));
            if (!vp_vector_name(in.vec_op)) bad(std::format("instruction {}: vector operation {:#x}", i, in.vec_op));
            if (!vp_scalar_name(in.sca_op)) bad(std::format("instruction {}: scalar operation {:#x}", i, in.sca_op));
            if (in.last != (i + 1 == n)) bad(std::format("instruction {} is{} marked last", i, in.last ? "" : " not"));
            // The sources the operations read: the vector unit's from slots 0 to 2, the scalar unit's from slot 2.
            const unsigned reads = vec_sources(in.vec_op);
            const bool scalar_reads = in.sca_op != kScaNop && in.sca_op != kScaBra && in.sca_op != kScaCal && in.sca_op != kScaRet;
            for (unsigned k = 0; k < 3; ++k) {
                if (!(k < reads || (k == 2 && scalar_reads))) continue;
                if (in.src[k].type == VpRegister::constant) constants.insert(in.constant);
                if (in.src[k].type == VpRegister::input) inputs.insert(in.input);
                if (in.src[k].type == VpRegister::unknown) bad(std::format("instruction {}: source {} of register type 0", i, k));
            }
            if (in.vec_op != kVecNop && in.vec_result && in.vec_mask) outputs |= 1u << in.dest;
            if (scalar_reads && in.sca_result && in.sca_mask) outputs |= 1u << in.dest;
        }
        // The header's input mask is a bit for each input register read.
        uint32_t input_mask = 0;
        for (const unsigned v : inputs) input_mask |= 1u << v;
        if (input_mask != p.header[3]) bad(std::format("it reads inputs {:#x}; its header says {:#x}", input_mask, p.header[3]));
        // Every constant read is a uniform parameter's register, or one of an array's (the check takes a
        // parameter of `resource_index` r and type matrix as four), and every referenced uniform is read.
        // A uniform's register is its index, or the number in a semantic "C<n>" (a register bound by the source); an element of
        // an array or a matrix ("m[2]") is at its parent's register and the element's number on.
        std::set<unsigned> named;
        for (const CgParameter& c : p.parameters) {
            if (c.variability != kUniform && c.variability != kLiteral) continue;
            int base = c.resource_index;
            if (base < 0 && c.semantic.size() > 1 && c.semantic[0] == 'C') base = std::atoi(c.semantic.c_str() + 1);
            if (base < 0) continue;
            int element = 0;
            if (const size_t open = c.name.rfind('['); open != std::string::npos && c.name.back() == ']') element = std::atoi(c.name.c_str() + open + 1);
            named.insert(static_cast<unsigned>(base + (c.resource_index >= 0 ? 0 : element)));
        }
        for (const unsigned k : constants)
            if (!named.contains(k)) bad(std::format("it reads c[{}], which no parameter names", k));
        (void)outputs;
        (void)kIn;
        (void)kOut;
    } else {
        size_t words = 0;
        const std::vector<FpInsn> insns = decode_fp(p.microcode, &words);
        if (insns.empty() || !insns.back().end) bad("no instruction is marked as the end");
        if (words * 4 != p.microcode.size()) bad(std::format("it ends at word {} of {}", words, p.microcode.size() / 4));
        if (p.header[0] != words / 4) bad(std::format("its header says {} instructions and its microcode holds {} slots", p.header[0], words / 4));
        // The inline constants: where they are in the microcode, against where the parameters say theirs are patched.
        std::set<uint32_t> slots, patched;
        size_t at = 0;
        for (size_t i = 0; i < insns.size(); ++i) {
            const FpInsn& in = insns[i];
            if (in.unknown != std::array<uint32_t, 4>{}) bad(std::format("instruction {} sets bits no field covers: {:08x} {:08x} {:08x} {:08x}", i, in.unknown[0], in.unknown[1], in.unknown[2], in.unknown[3]));
            if (in.branch ? !fp_branch_name(in.op) : !fp_name(in.op)) bad(std::format("instruction {}: operation {:#x}{}", i, in.op, in.branch ? " (flow control)" : ""));
            if (!in.branch) {
                bool reads_input = false;
                for (unsigned k = 0; k < fp_sources(in.op); ++k) reads_input |= in.src[k].type == FpRegister::input;
                if (in.index_input) bad(std::format("instruction {} indexes its input, which the translation doesn't take yet", i));
                if (in.input_displacement != (reads_input ? 0x7FCu : 0u))
                    bad(std::format("instruction {}: word 3's bits 19 to 29 are {:#x}{}", i, in.input_displacement, reads_input ? ", reading an input" : ""));
                if (in.src1_extra != (in.op == kFpNrm ? 5u : 0u)) bad(std::format("instruction {}: word 2's bits 19 to 21 are {}", i, in.src1_extra));
                const unsigned reads = fp_sources(in.op);
                for (unsigned k = reads; k < 3; ++k)
                    if (in.src[k].type == FpRegister::constant) bad(std::format("instruction {}: source {}, which {} doesn't read, names a constant", i, k, fp_name(in.op) ? fp_name(in.op) : "it"));
            }
            at += 16;
            if (in.has_constant) slots.insert(static_cast<uint32_t>(at)), at += 16;
        }
        for (const CgParameter& c : p.parameters)
            for (const uint32_t e : c.embedded) patched.insert(e);
        for (const uint32_t e : patched)
            if (!slots.contains(e)) bad(std::format("a parameter's constant is patched at byte {}, which holds no constant", e));
    }
    return wrong;
}

}  // namespace qilin::rsx

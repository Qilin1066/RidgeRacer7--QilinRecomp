#include "spu_lift.hpp"

#include <array>
#include <format>
#include <map>
#include <set>

namespace qilin {

using spu::Insn;
using spu::Op;

std::string spu_fn_name(uint32_t address) { return std::format("fn_{:05X}", address); }

bool spu_in_slice(const Insn& in, std::string* why) {
    const auto no = [&](std::string w) {
        if (why) *why = std::move(w);
        return false;
    };
    if (!in.valid()) return no("an invalid word");
    const spu::Spec& s = spu::spec(in.op);
    switch (in.op) {
    case Op::fa: case Op::fs: case Op::fm: case Op::fma: case Op::fms: case Op::fnms: case Op::fceq: case Op::fcgt: case Op::fcmeq: case Op::fcmgt:
    case Op::frest: case Op::frsqest: case Op::fi: case Op::dfa: case Op::dfs: case Op::dfm: case Op::fesd: case Op::frds:
        return true;
    // The scale is 155 or 173 less the field, and is defined only from 0 to 127.
    case Op::csflt: case Op::cuflt:
        if (in.u8() < 28 || in.u8() > 155) return no(std::format("an {} whose scale the instruction set leaves undefined", s.name));
        return true;
    case Op::cflts: case Op::cfltu:
        if (in.u8() < 46 || in.u8() > 173) return no(std::format("an {} whose scale the instruction set leaves undefined", s.name));
        return true;
    case Op::fscrrd: case Op::fscrwr: return true;
    default:
        if (s.flags & spu::kFloat) return no(std::format("SPU floating point the game doesn't use ({})", s.name));
        break;
    }
    switch (in.op) {
    case Op::iret: case Op::bisled: return no(std::format("SPU interrupts ({})", s.name));
    case Op::mfspr: case Op::mtspr: return no(std::format("an SPU special-purpose register ({})", s.name));
    case Op::bi: case Op::bisl: case Op::biz: case Op::binz: case Op::bihz: case Op::bihnz:
        if (in.d() || in.e()) return no(std::format("SPU interrupts (a {} that enables or disables them)", s.name));
        return true;
    default: return true;
    }
}

namespace {

std::string hex(uint32_t v) { return std::format("0x{:08X}u", v); }
// A constant in every word; in both halves of every word; in every byte.
std::string splat(uint32_t word) { return std::format("spu::splat({})", hex(word)); }
std::string splat_half(uint32_t half) { return splat((half & 0xffffu) * 0x10001u); }
std::string splat_byte(uint32_t byte) { return splat((byte & 0xffu) * 0x01010101u); }

using Words = std::array<uint32_t, 4>;
// fsmbi's constant: each of 16 bits a byte of ones or zeros, the top bit the first byte.
Words byte_mask(uint32_t bits) {
    Words w{};
    for (unsigned i = 0; i < 16; ++i)
        if (bits & (0x8000u >> i)) w[i >> 2] |= 0xffu << (24 - 8 * (i & 3));
    return w;
}
std::string constant(const Words& w) {
    if (w[0] == w[1] && w[0] == w[2] && w[0] == w[3]) return splat(w[0]);
    return std::format("spu::quad({}, {}, {}, {})", hex(w[0]), hex(w[1]), hex(w[2]), hex(w[3]));
}

// The registers a function uses are locals (`r3`), read from the Spu at its start and after every call, and the ones it
// writes written back before every call and return: the placeholders, filled in when the function's uses are known.
constexpr std::string_view kOut = "@OUT@", kIn = "@IN@";

class Emitter {
public:
    Emitter(const SpuCfg* cfg, const SpuFunction& f) : cfg_(cfg), f_(f) {
        for (const SpuJumpTable& t : f.tables) tables_[t.at] = &t;
    }

    // One instruction (not a branch: those need the function around them), as a whole function's body.
    bool one(uint32_t pc, const Insn& in, std::string& out) {
        std::string line;
        if (!insn(pc, in, line)) return false;
        out = finish(line + std::format("   {}\n", kOut));
        return true;
    }

    SpuLifted run() {
        SpuLifted out;
        // The function's instructions: the code words from its start to its end.
        std::vector<uint32_t> at;
        for (uint32_t a = f_.start; a < f_.end; a += 4)
            if (cfg_->is_code(a)) at.push_back(a), present_.insert(a);
        for (const uint32_t a : at) {
            std::string why;
            if (!spu_in_slice(cfg_->at(a), &why)) {
                out.why = why;
                out.why_at = a;
                return out;
            }
        }
        // Labels: every branch target inside the function, and every switch target.
        for (const uint32_t a : at) {
            const Insn in = cfg_->at(a);
            if (const uint32_t t = direct_target(in, a); t != kNoTarget && present_.contains(t) && !(spu::spec(in.op).flags & spu::kCall)) labels_.insert(t);
            if (const auto it = tables_.find(a); it != tables_.end())
                for (const uint32_t t : it->second->targets) labels_.insert(t);
        }
        std::string body;
        for (const uint32_t a : at) {
            const Insn in = cfg_->at(a);
            if (labels_.contains(a)) {
                body += std::format("L_{:05X}: ;\n", a);
                known_.clear();  // (another way in: what this block knew is not known there)
            }
            std::string line;
            if (!insn(a, in, line)) {
                out.why = error_;
                out.why_at = a;
                return out;
            }
            body += line;
            if (ends(in, a) || present_.contains(a + 4)) continue;
            // Execution goes on past this instruction to something that isn't this function's next instruction.
            if (cfg_->functions().contains(a + 4)) {
                body += std::format("   {} {}(s); return;\n", kOut, spu_fn_name(a + 4));
            } else if (in.op == Op::stop || in.op == Op::stopd) {
                body += std::format("    s.hooks->halt(s, {}, \"the program went on after a stop that ends it\");\n", hex(a));
            } else {
                out.why = std::format("execution runs on to {:05X}, which is not code", a + 4);
                out.why_at = a;
                return out;
            }
        }
        out.code = std::format("void {}(Spu& s) {{\n{}}}\n", spu_fn_name(f_.start), finish(body));
        out.ok = true;
        return out;
    }

private:
    static constexpr uint32_t kNoTarget = 0xffffffffu;

    // A register read, and one written (whose constant, if it held one, is no longer known).
    std::string R(unsigned n) {
        used_.insert(n);
        return std::format("r{}", n);
    }
    std::string D(unsigned n) {
        written_.insert(n);
        known_.erase(n);
        return R(n);
    }
    std::string LS() {
        uses_ls_ = true;
        return "ls";
    }

    // The locals' declarations, then the body with its placeholders filled in.
    std::string finish(const std::string& body) const {
        std::string head, out_s, in_s;
        for (const unsigned n : used_) {
            head += std::format("    SpuReg r{0} = s.r[{0}];\n", n);
            in_s += std::format(" r{0} = s.r[{0}];", n);
        }
        for (const unsigned n : written_) out_s += std::format(" s.r[{0}] = r{0};", n);
        if (uses_ls_) head += "    uint8_t* const ls = s.ls;\n";
        std::string r = head + body;
        for (const auto& [placeholder, with] : {std::pair{kOut, out_s}, std::pair{kIn, in_s}})
            for (size_t p; (p = r.find(placeholder)) != std::string::npos;) r.replace(p, placeholder.size(), with);
        return r;
    }

    // The target of a branch whose target is in the instruction.
    static uint32_t direct_target(const Insn& in, uint32_t pc) {
        switch (in.op) {
        case Op::br: case Op::brsl: case Op::brz: case Op::brnz: case Op::brhz: case Op::brhnz: return in.relative(pc);
        case Op::bra: case Op::brasl: return in.absolute();
        default: return kNoTarget;
        }
    }
    // Nothing runs on past it.
    bool ends(const Insn& in, uint32_t) const { return in.op == Op::br || in.op == Op::bra || in.op == Op::bi; }

    bool fail(std::string why) {
        error_ = std::move(why);
        return false;
    }

    // "goto L;" for a target in the function, or a tail call and a return for another function's start.
    bool jump(uint32_t target, std::string& out) {
        if (present_.contains(target)) {
            out = std::format("goto L_{:05X};", target);
            return true;
        }
        if (cfg_->functions().contains(target)) {
            out = std::format("{{{} {}(s); return; }}", kOut, spu_fn_name(target));
            return true;
        }
        return fail(std::format("a branch to {:05X}, which is neither in the function nor a function", target));
    }
    static std::string ret() { return std::format("{{{} return; }}", kOut); }

    bool insn(uint32_t pc, const Insn& in, std::string& out) {
        const unsigned t = in.rt(), a = in.ra(), b = in.rb();
        const uint32_t i10 = static_cast<uint32_t>(in.i10()), i7 = static_cast<uint32_t>(in.i7()), u16 = in.u16();
        const auto line = [&](std::string text) { out = "    " + std::move(text) + "\n"; };
        // rt = f(ra, rb); rt = f(ra); rt = f(ra, a constant or a count).
        const auto rr = [&](const char* f) {
            const std::string x = R(a), y = R(b);
            line(std::format("{} = spu::{}({}, {});", D(t), f, x, y));
        };
        const auto r1 = [&](const char* f) {
            const std::string x = R(a);
            line(std::format("{} = spu::{}({});", D(t), f, x));
        };
        const auto ri = [&](const char* f, const std::string& k) {
            const std::string x = R(a);
            line(std::format("{} = spu::{}({}, {});", D(t), f, x, k));
        };
        // The rrr forms: the target is rt4, and rt is the third source.
        const auto rrr = [&](const char* f) {
            const std::string x = R(a), y = R(b), z = R(t);
            line(std::format("{} = spu::{}({}, {}, {});", D(in.rt4()), f, x, y, z));
        };
        // rt = a constant, known after it in the block.
        const auto set = [&](const Words& w) {
            line(std::format("{} = {};", D(t), constant(w)));
            known_[t] = w;
        };
        const auto preferred = [&](unsigned n) { return std::format("{}.w[0]", R(n)); };
        const std::string count_i = hex(i7);
        const std::string link = std::format("spu::scalar({})", hex(pc + 4));
        const auto move = [&](unsigned from) {
            const std::string x = R(from);
            line(std::format("{} = {};", D(t), x));
        };

        switch (in.op) {
        // ---------------------------------------------------------------- memory
        case Op::lqd: {
            const std::string x = preferred(a);
            line(std::format("{} = spu::load({}, {} + {});", D(t), LS(), x, hex(i10 << 4)));
            return true;
        }
        case Op::lqx: {
            const std::string x = preferred(a), y = preferred(b);
            line(std::format("{} = spu::load({}, {} + {});", D(t), LS(), x, y));
            return true;
        }
        case Op::lqa: line(std::format("{} = spu::load({}, {});", D(t), LS(), hex(in.absolute()))); return true;
        case Op::lqr: line(std::format("{} = spu::load({}, {});", D(t), LS(), hex(in.relative(pc)))); return true;
        case Op::stqd: line(std::format("spu::store({}, {} + {}, {});", LS(), preferred(a), hex(i10 << 4), R(t))); return true;
        case Op::stqx: line(std::format("spu::store({}, {} + {}, {});", LS(), preferred(a), preferred(b), R(t))); return true;
        case Op::stqa: line(std::format("spu::store({}, {}, {});", LS(), hex(in.absolute()), R(t))); return true;
        case Op::stqr: line(std::format("spu::store({}, {}, {});", LS(), hex(in.relative(pc)), R(t))); return true;
        case Op::cbd: case Op::chd: case Op::cwd: case Op::cdd: {
            const unsigned size = in.op == Op::cbd ? 1 : in.op == Op::chd ? 2 : in.op == Op::cwd ? 4 : 8;
            const std::string x = preferred(a);
            line(std::format("{} = spu::insertion_mask({} + {}, {});", D(t), x, hex(i7), size));
            return true;
        }
        case Op::cbx: case Op::chx: case Op::cwx: case Op::cdx: {
            const unsigned size = in.op == Op::cbx ? 1 : in.op == Op::chx ? 2 : in.op == Op::cwx ? 4 : 8;
            const std::string x = preferred(a), y = preferred(b);
            line(std::format("{} = spu::insertion_mask({} + {}, {});", D(t), x, y, size));
            return true;
        }

        // ---------------------------------------------------------------- constants
        case Op::il: {
            const uint32_t k = static_cast<uint32_t>(in.i16());
            set({k, k, k, k});
            return true;
        }
        case Op::ilh: {
            const uint32_t k = (u16 & 0xffffu) * 0x10001u;
            set({k, k, k, k});
            return true;
        }
        case Op::ilhu: set({u16 << 16, u16 << 16, u16 << 16, u16 << 16}); return true;
        case Op::ila: set({in.u18(), in.u18(), in.u18(), in.u18()}); return true;
        case Op::iohl: {
            // (After the ilhu that comes before it in the block: one constant.)
            if (const auto it = known_.find(t); it != known_.end()) {
                Words w = it->second;
                for (uint32_t& word : w) word |= u16;
                set(w);
                return true;
            }
            const std::string x = R(t);
            line(std::format("{} = spu::or_({}, {});", D(t), x, splat(u16)));
            return true;
        }
        case Op::fsmbi: set(byte_mask(u16)); return true;

        // ---------------------------------------------------------------- arithmetic
        case Op::a: rr("a"); return true;
        case Op::ah: rr("ah"); return true;
        case Op::sf: rr("sf"); return true;
        case Op::sfh: rr("sfh"); return true;
        case Op::ai: ri("a", splat(i10)); return true;
        case Op::ahi: ri("ah", splat_half(i10)); return true;
        case Op::sfi: ri("sf", splat(i10)); return true;
        case Op::sfhi: ri("sfh", splat_half(i10)); return true;
        case Op::addx: case Op::sfx: case Op::cgx: case Op::bgx: {
            const char* f = in.op == Op::addx ? "addx" : in.op == Op::sfx ? "sfx" : in.op == Op::cgx ? "cgx" : "bgx";
            const std::string x = R(a), y = R(b), z = R(t);
            line(std::format("{} = spu::{}({}, {}, {});", D(t), f, x, y, z));
            return true;
        }
        case Op::cg: rr("cg"); return true;
        case Op::bg: rr("bg"); return true;
        case Op::mpy: rr("mpy"); return true;
        case Op::mpyu: rr("mpyu"); return true;
        case Op::mpyh: rr("mpyh"); return true;
        case Op::mpys: rr("mpys"); return true;
        case Op::mpyhh: rr("mpyhh"); return true;
        case Op::mpyhhu: rr("mpyhhu"); return true;
        case Op::mpyi: ri("mpy", splat(i10)); return true;
        case Op::mpyui: ri("mpyu", splat(i10)); return true;
        case Op::mpya: {
            const std::string x = R(a), y = R(b), z = R(t);
            line(std::format("{} = spu::a(spu::mpy({}, {}), {});", D(in.rt4()), x, y, z));
            return true;
        }
        case Op::mpyhha: case Op::mpyhhau: {
            const std::string x = R(a), y = R(b), z = R(t);
            line(std::format("{} = spu::a(spu::{}({}, {}), {});", D(t), in.op == Op::mpyhha ? "mpyhh" : "mpyhhu", x, y, z));
            return true;
        }
        case Op::clz: r1("clz"); return true;
        case Op::cntb: r1("cntb"); return true;
        case Op::fsmb: r1("fsmb"); return true;
        case Op::fsmh: r1("fsmh"); return true;
        case Op::fsm: r1("fsm"); return true;
        case Op::gbb: r1("gbb"); return true;
        case Op::gbh: r1("gbh"); return true;
        case Op::gb: r1("gb"); return true;
        case Op::avgb: rr("avgb"); return true;
        case Op::absdb: rr("absdb"); return true;
        case Op::sumb: rr("sumb"); return true;
        case Op::xsbh: r1("xsbh"); return true;
        case Op::xshw: r1("xshw"); return true;
        case Op::xswd: r1("xswd"); return true;

        // ---------------------------------------------------------------- logic
        case Op::and_: rr("and_"); return true;
        case Op::andc: rr("andc"); return true;
        case Op::or_: rr("or_"); return true;
        case Op::orc: rr("orc"); return true;
        case Op::xor_: rr("xor_"); return true;
        case Op::nand: rr("nand"); return true;
        case Op::nor: rr("nor"); return true;
        case Op::eqv: rr("eqv"); return true;
        case Op::orx: r1("orx"); return true;
        case Op::andi: ri("and_", splat(i10)); return true;
        case Op::andhi: ri("and_", splat_half(i10)); return true;
        case Op::andbi: ri("and_", splat_byte(i10)); return true;
        case Op::ori:
            if (i10 == 0) move(a);  // (the compiler's move)
            else ri("or_", splat(i10));
            return true;
        case Op::orhi: ri("or_", splat_half(i10)); return true;
        case Op::orbi: ri("or_", splat_byte(i10)); return true;
        case Op::xori: ri("xor_", splat(i10)); return true;
        case Op::xorhi: ri("xor_", splat_half(i10)); return true;
        case Op::xorbi: ri("xor_", splat_byte(i10)); return true;
        case Op::selb: rrr("selb"); return true;
        case Op::shufb: rrr("shufb"); return true;

        // ---------------------------------------------------------------- shifts and rotates
        case Op::shlh: rr("shlh"); return true;
        case Op::shl: rr("shl"); return true;
        case Op::roth: rr("roth"); return true;
        case Op::rot: rr("rot"); return true;
        case Op::rothm: rr("rothm"); return true;
        case Op::rotm: rr("rotm"); return true;
        case Op::rotmah: rr("rotmah"); return true;
        case Op::rotma: rr("rotma"); return true;
        // (one count for every element: the helpers' immediate forms)
        case Op::shlhi: ri("shlhi", count_i); return true;
        case Op::shli: ri("shli", count_i); return true;
        case Op::rothi: ri("rothi", count_i); return true;
        case Op::roti: ri("roti", count_i); return true;
        case Op::rothmi: ri("rothmi", count_i); return true;
        case Op::rotmi: ri("rotmi", count_i); return true;
        case Op::rotmahi: ri("rotmahi", count_i); return true;
        case Op::rotmai: ri("rotmai", count_i); return true;
        case Op::shlqbi: ri("shlqbi", preferred(b)); return true;
        case Op::shlqby: ri("shlqby", preferred(b)); return true;
        case Op::shlqbybi: ri("shlqbybi", preferred(b)); return true;
        case Op::rotqbi: ri("rotqbi", preferred(b)); return true;
        case Op::rotqby: ri("rotqby", preferred(b)); return true;
        case Op::rotqbybi: ri("rotqbybi", preferred(b)); return true;
        case Op::rotqmbi: ri("rotqmbi", preferred(b)); return true;
        case Op::rotqmby: ri("rotqmby", preferred(b)); return true;
        case Op::rotqmbybi: ri("rotqmbybi", preferred(b)); return true;
        case Op::shlqbii: ri("shlqbi", count_i); return true;
        case Op::shlqbyi:
            if ((i7 & 0x1fu) == 0) move(a);  // (the compiler's other move)
            else ri("shlqby", count_i);
            return true;
        case Op::rotqbii: ri("rotqbi", count_i); return true;
        case Op::rotqbyi: ri("rotqby", count_i); return true;
        case Op::rotqmbii: ri("rotqmbi", count_i); return true;
        case Op::rotqmbyi: ri("rotqmby", count_i); return true;

        // ---------------------------------------------------------------- compares and halts
        case Op::ceq: rr("ceq"); return true;
        case Op::ceqh: rr("ceqh"); return true;
        case Op::ceqb: rr("ceqb"); return true;
        case Op::cgt: rr("cgt"); return true;
        case Op::cgth: rr("cgth"); return true;
        case Op::cgtb: rr("cgtb"); return true;
        case Op::clgt: rr("clgt"); return true;
        case Op::clgth: rr("clgth"); return true;
        case Op::clgtb: rr("clgtb"); return true;
        case Op::ceqi: ri("ceq", splat(i10)); return true;
        case Op::ceqhi: ri("ceqh", splat_half(i10)); return true;
        case Op::ceqbi: ri("ceqb", splat_byte(i10)); return true;
        case Op::cgti: ri("cgt", splat(i10)); return true;
        case Op::cgthi: ri("cgth", splat_half(i10)); return true;
        case Op::cgtbi: ri("cgtb", splat_byte(i10)); return true;
        case Op::clgti: ri("clgt", splat(i10)); return true;
        case Op::clgthi: ri("clgth", splat_half(i10)); return true;
        case Op::clgtbi: ri("clgtb", splat_byte(i10)); return true;
        case Op::heq: line(std::format("if ({} == {}) s.hooks->halt(s, {}, \"heq\");", preferred(a), preferred(b), hex(pc))); return true;
        case Op::hgt:
            line(std::format("if (static_cast<int32_t>({}) > static_cast<int32_t>({})) s.hooks->halt(s, {}, \"hgt\");", preferred(a), preferred(b), hex(pc)));
            return true;
        case Op::hlgt: line(std::format("if ({} > {}) s.hooks->halt(s, {}, \"hlgt\");", preferred(a), preferred(b), hex(pc))); return true;
        case Op::heqi: line(std::format("if ({} == {}) s.hooks->halt(s, {}, \"heqi\");", preferred(a), hex(i10), hex(pc))); return true;
        case Op::hgti: line(std::format("if (static_cast<int32_t>({}) > {}) s.hooks->halt(s, {}, \"hgti\");", preferred(a), in.i10(), hex(pc))); return true;
        case Op::hlgti: line(std::format("if ({} > {}) s.hooks->halt(s, {}, \"hlgti\");", preferred(a), hex(i10), hex(pc))); return true;

        // ---------------------------------------------------------------- floating point (spu_fp.hpp)
        case Op::fa: rr("fa"); return true;
        case Op::fs: rr("fs"); return true;
        case Op::fm: rr("fm"); return true;
        case Op::fceq: rr("fceq"); return true;
        case Op::fcgt: rr("fcgt"); return true;
        case Op::fcmeq: rr("fcmeq"); return true;
        case Op::fcmgt: rr("fcmgt"); return true;
        case Op::fi: rr("fi"); return true;
        case Op::dfa: rr("dfa"); return true;
        case Op::dfs: rr("dfs"); return true;
        case Op::dfm: rr("dfm"); return true;
        case Op::frest: r1("frest"); return true;
        case Op::frsqest: r1("frsqest"); return true;
        case Op::fesd: r1("fesd"); return true;
        case Op::frds: r1("frds"); return true;
        // The status and control register (spu.hpp). Double precision rounds to nearest here: another mode halts.
        case Op::fscrrd: line(std::format("{} = spu::fscrrd(s);", D(t))); return true;
        case Op::fscrwr:
            line(std::format("spu::fscrwr(s, {}); if (s.fpscr[0] & 0xF00u) s.hooks->halt(s, {}, \"a double-precision rounding mode other than to nearest, "
                             "which this runtime doesn't provide\");",
                             R(a), hex(pc)));
            return true;
        case Op::fma: rrr("fma"); return true;
        case Op::fms: rrr("fms"); return true;
        case Op::fnms: rrr("fnms"); return true;
        case Op::csflt: case Op::cuflt: case Op::cflts: case Op::cfltu: {
            const bool to_float = in.op == Op::csflt || in.op == Op::cuflt;
            const std::string x = R(a);
            line(std::format("{} = spu::{}({}, {});", D(t), spu::spec(in.op).name, x, (to_float ? 155 : 173) - static_cast<int>(in.u8())));
            return true;
        }

        // ---------------------------------------------------------------- channels and stops
        // (The hooks but native_call don't see the registers: nothing is written back for them.)
        case Op::rdch: line(std::format("{} = spu::scalar(s.hooks->read_channel(s, {}u, {}));", D(t), a, hex(pc))); return true;
        case Op::rchcnt: line(std::format("{} = spu::scalar(s.hooks->channel_count(s, {}u, {}));", D(t), a, hex(pc))); return true;
        case Op::wrch: line(std::format("s.hooks->write_channel(s, {}u, {}, {});", a, preferred(t), hex(pc))); return true;
        case Op::stop: line(std::format("s.hooks->stop(s, {}, {});", hex(in.u14()), hex(pc))); return true;
        case Op::stopd: line(std::format("s.hooks->stop(s, 0x00003FFFu, {});", hex(pc))); return true;
        case Op::nop: case Op::lnop: case Op::sync: case Op::dsync: case Op::hbr: case Op::hbra: case Op::hbrr:
            out = std::format("    // {}\n", spu::spec(in.op).name);
            return true;

        // ---------------------------------------------------------------- branches
        case Op::br: case Op::bra: {
            std::string go;
            if (!jump(direct_target(in, pc), go)) return false;
            line(go);
            return true;
        }
        case Op::brsl: case Op::brasl: {
            const uint32_t target = direct_target(in, pc);
            if (!cfg_->functions().contains(target)) return fail(std::format("a call to {:05X}, which is not a function", target));
            line(std::format("{} = {};{} {}(s);{}", D(t), link, kOut, spu_fn_name(target), kIn));
            known_.clear();  // (the callee may change any register)
            return true;
        }
        case Op::brz: case Op::brnz: case Op::brhz: case Op::brhnz: {
            std::string go;
            if (!jump(direct_target(in, pc), go)) return false;
            const bool half = in.op == Op::brhz || in.op == Op::brhnz, zero = in.op == Op::brz || in.op == Op::brhz;
            line(std::format("if (({}{}) {} 0u) {}", preferred(t), half ? " & 0xFFFFu" : "", zero ? "==" : "!=", go));
            return true;
        }
        case Op::bi: {
            if (const auto it = tables_.find(pc); it != tables_.end()) {
                std::string cases;
                std::set<uint32_t> seen;
                for (const uint32_t target : it->second->targets)
                    if (seen.insert(target).second) cases += std::format(" case {}: goto L_{:05X};", hex(target), target);
                line(std::format("switch ({} & 0x3FFFCu) {{{} default: s.hooks->halt(s, {}, \"a switch to an address outside its table\"); }}", preferred(a), cases, hex(pc)));
                return true;
            }
            // $0 holds the address a call left there: this is the function's return.
            if (a == 0) line(ret());
            else line(std::format("{{ const uint32_t target = {} & 0x3FFFCu;{} call(s, target, {}); return; }}", preferred(a), kOut, hex(pc)));
            return true;
        }
        case Op::bisl: {
            const std::string target = preferred(a);
            line(std::format("{{ const uint32_t target = {} & 0x3FFFCu; {} = {};{} call(s, target, {});{} }}", target, D(t), link, kOut, hex(pc), kIn));
            known_.clear();
            return true;
        }
        case Op::biz: case Op::binz: case Op::bihz: case Op::bihnz: {
            const bool half = in.op == Op::bihz || in.op == Op::bihnz, zero = in.op == Op::biz || in.op == Op::bihz;
            const std::string go = a == 0 ? ret() : std::format("{{ const uint32_t target = {} & 0x3FFFCu;{} call(s, target, {}); return; }}", preferred(a), kOut, hex(pc));
            line(std::format("if (({}{}) {} 0u) {}", preferred(t), half ? " & 0xFFFFu" : "", zero ? "==" : "!=", go));
            return true;
        }
        default: return fail(std::format("the lifter has no case for {}", spu::spec(in.op).name));
        }
    }

    const SpuCfg* cfg_;  // null for spu_lift_one: only branches and run() read it
    const SpuFunction& f_;
    std::map<uint32_t, const SpuJumpTable*> tables_;
    std::set<uint32_t> present_, labels_;
    std::set<unsigned> used_, written_;  // the registers the function reads or writes, and those it writes
    std::map<unsigned, Words> known_;    // registers holding a known constant here, within a block
    bool uses_ls_ = false;
    std::string error_;
};

}  // namespace

SpuLifted spu_lift(const SpuCfg& cfg, const SpuFunction& f) { return Emitter(&cfg, f).run(); }

std::optional<std::string> spu_lift_one(const Insn& in, uint32_t pc) {
    if (!spu_in_slice(in) || (spu::spec(in.op).flags & spu::kBranch)) return std::nullopt;
    // (No instruction but a branch looks at the program or the function.)
    static const SpuFunction no_function;
    std::string body;
    if (!Emitter(nullptr, no_function).one(pc, in, body)) return std::nullopt;
    return body;
}

}  // namespace qilin

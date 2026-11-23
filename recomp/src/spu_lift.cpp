#include "spu_lift.hpp"

#include <format>
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
std::string R(unsigned n) { return std::format("s.r[{}]", n); }
// A constant in every word; in both halves of every word; in every byte.
std::string splat(uint32_t word) { return std::format("spu::splat({})", hex(word)); }
std::string splat_half(uint32_t half) { return splat((half & 0xffffu) * 0x10001u); }
std::string splat_byte(uint32_t byte) { return splat((byte & 0xffu) * 0x01010101u); }

class Emitter {
public:
    Emitter(const SpuCfg* cfg, const SpuFunction& f) : cfg_(cfg), f_(f) {
        for (const SpuJumpTable& t : f.tables) tables_[t.at] = &t;
    }

    // One instruction's line (not a branch: those need the function around them).
    bool one(uint32_t pc, const Insn& in, std::string& out) { return insn(pc, in, out); }

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
            if (labels_.contains(a)) body += std::format("L_{:05X}: ;\n", a);
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
                body += std::format("    {}(s); return;\n", spu_fn_name(a + 4));
            } else if (in.op == Op::stop || in.op == Op::stopd) {
                body += std::format("    s.hooks->halt(s, {}, \"the program went on after a stop that ends it\");\n", hex(a));
            } else {
                out.why = std::format("execution runs on to {:05X}, which is not code", a + 4);
                out.why_at = a;
                return out;
            }
        }
        out.code = std::format("void {}(Spu& s) {{\n{}}}\n", spu_fn_name(f_.start), body);
        out.ok = true;
        return out;
    }

private:
    static constexpr uint32_t kNoTarget = 0xffffffffu;

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
            out = std::format("{{ {}(s); return; }}", spu_fn_name(target));
            return true;
        }
        return fail(std::format("a branch to {:05X}, which is neither in the function nor a function", target));
    }

    bool insn(uint32_t pc, const Insn& in, std::string& out) {
        const unsigned t = in.rt(), a = in.ra(), b = in.rb();
        const uint32_t i10 = static_cast<uint32_t>(in.i10()), i7 = static_cast<uint32_t>(in.i7()), u16 = in.u16();
        const auto line = [&](std::string text) { out = "    " + std::move(text) + "\n"; };
        // rt = f(ra, rb); rt = f(ra); rt = f(ra, constant).
        const auto rr = [&](const char* f) { line(std::format("{} = spu::{}({}, {});", R(t), f, R(a), R(b))); };
        const auto r1 = [&](const char* f) { line(std::format("{} = spu::{}({});", R(t), f, R(a))); };
        const auto ri = [&](const char* f, const std::string& constant) { line(std::format("{} = spu::{}({}, {});", R(t), f, R(a), constant)); };
        // rt = f(ra, a count in rb's preferred word, or a constant).
        const auto quad = [&](const char* f, const std::string& count) { line(std::format("{} = spu::{}({}, {});", R(t), f, R(a), count)); };
        const std::string count_b = std::format("{}.w[0]", R(b)), count_i = hex(i7);
        const std::string link = std::format("SpuReg{{{{{}, 0u, 0u, 0u}}}}", hex(pc + 4));
        const std::string word = std::format("{}.w[0]", R(t));

        switch (in.op) {
        // ---------------------------------------------------------------- memory
        case Op::lqd: line(std::format("{} = spu::load(s, {}.w[0] + {});", R(t), R(a), hex(i10 << 4))); return true;
        case Op::lqx: line(std::format("{} = spu::load(s, {}.w[0] + {}.w[0]);", R(t), R(a), R(b))); return true;
        case Op::lqa: line(std::format("{} = spu::load(s, {});", R(t), hex(in.absolute()))); return true;
        case Op::lqr: line(std::format("{} = spu::load(s, {});", R(t), hex(in.relative(pc)))); return true;
        case Op::stqd: line(std::format("spu::store(s, {}.w[0] + {}, {});", R(a), hex(i10 << 4), R(t))); return true;
        case Op::stqx: line(std::format("spu::store(s, {}.w[0] + {}.w[0], {});", R(a), R(b), R(t))); return true;
        case Op::stqa: line(std::format("spu::store(s, {}, {});", hex(in.absolute()), R(t))); return true;
        case Op::stqr: line(std::format("spu::store(s, {}, {});", hex(in.relative(pc)), R(t))); return true;
        case Op::cbd: line(std::format("{} = spu::insertion_mask({}.w[0] + {}, 1);", R(t), R(a), hex(i7))); return true;
        case Op::chd: line(std::format("{} = spu::insertion_mask({}.w[0] + {}, 2);", R(t), R(a), hex(i7))); return true;
        case Op::cwd: line(std::format("{} = spu::insertion_mask({}.w[0] + {}, 4);", R(t), R(a), hex(i7))); return true;
        case Op::cdd: line(std::format("{} = spu::insertion_mask({}.w[0] + {}, 8);", R(t), R(a), hex(i7))); return true;
        case Op::cbx: line(std::format("{} = spu::insertion_mask({}.w[0] + {}.w[0], 1);", R(t), R(a), R(b))); return true;
        case Op::chx: line(std::format("{} = spu::insertion_mask({}.w[0] + {}.w[0], 2);", R(t), R(a), R(b))); return true;
        case Op::cwx: line(std::format("{} = spu::insertion_mask({}.w[0] + {}.w[0], 4);", R(t), R(a), R(b))); return true;
        case Op::cdx: line(std::format("{} = spu::insertion_mask({}.w[0] + {}.w[0], 8);", R(t), R(a), R(b))); return true;

        // ---------------------------------------------------------------- constants
        case Op::il: line(std::format("{} = {};", R(t), splat(static_cast<uint32_t>(in.i16())))); return true;
        case Op::ilh: line(std::format("{} = {};", R(t), splat_half(u16))); return true;
        case Op::ilhu: line(std::format("{} = {};", R(t), splat(u16 << 16))); return true;
        case Op::ila: line(std::format("{} = {};", R(t), splat(in.u18()))); return true;
        case Op::iohl: line(std::format("{0} = spu::or_({0}, {1});", R(t), splat(u16))); return true;
        case Op::fsmbi: line(std::format("{} = spu::fsmbi({});", R(t), hex(u16))); return true;

        // ---------------------------------------------------------------- arithmetic
        case Op::a: rr("a"); return true;
        case Op::ah: rr("ah"); return true;
        case Op::sf: rr("sf"); return true;
        case Op::sfh: rr("sfh"); return true;
        case Op::ai: ri("a", splat(i10)); return true;
        case Op::ahi: ri("ah", splat_half(i10)); return true;
        case Op::sfi: ri("sf", splat(i10)); return true;
        case Op::sfhi: ri("sfh", splat_half(i10)); return true;
        case Op::addx: line(std::format("{0} = spu::addx({1}, {2}, {0});", R(t), R(a), R(b))); return true;
        case Op::sfx: line(std::format("{0} = spu::sfx({1}, {2}, {0});", R(t), R(a), R(b))); return true;
        case Op::cgx: line(std::format("{0} = spu::cgx({1}, {2}, {0});", R(t), R(a), R(b))); return true;
        case Op::bgx: line(std::format("{0} = spu::bgx({1}, {2}, {0});", R(t), R(a), R(b))); return true;
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
        // (the rrr form: the target is rt4, and rt is the fourth operand)
        case Op::mpya: line(std::format("{} = spu::a(spu::mpy({}, {}), {});", R(in.rt4()), R(a), R(b), R(t))); return true;
        case Op::mpyhha: line(std::format("{0} = spu::a(spu::mpyhh({1}, {2}), {0});", R(t), R(a), R(b))); return true;
        case Op::mpyhhau: line(std::format("{0} = spu::a(spu::mpyhhu({1}, {2}), {0});", R(t), R(a), R(b))); return true;
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
        case Op::ori: ri("or_", splat(i10)); return true;
        case Op::orhi: ri("or_", splat_half(i10)); return true;
        case Op::orbi: ri("or_", splat_byte(i10)); return true;
        case Op::xori: ri("xor_", splat(i10)); return true;
        case Op::xorhi: ri("xor_", splat_half(i10)); return true;
        case Op::xorbi: ri("xor_", splat_byte(i10)); return true;
        case Op::selb: line(std::format("{} = spu::selb({}, {}, {});", R(in.rt4()), R(a), R(b), R(t))); return true;
        case Op::shufb: line(std::format("{} = spu::shufb({}, {}, {});", R(in.rt4()), R(a), R(b), R(t))); return true;

        // ---------------------------------------------------------------- shifts and rotates
        case Op::shlh: rr("shlh"); return true;
        case Op::shl: rr("shl"); return true;
        case Op::roth: rr("roth"); return true;
        case Op::rot: rr("rot"); return true;
        case Op::rothm: rr("rothm"); return true;
        case Op::rotm: rr("rotm"); return true;
        case Op::rotmah: rr("rotmah"); return true;
        case Op::rotma: rr("rotma"); return true;
        case Op::shlhi: ri("shlh", splat_half(i7)); return true;
        case Op::shli: ri("shl", splat(i7)); return true;
        case Op::rothi: ri("roth", splat_half(i7)); return true;
        case Op::roti: ri("rot", splat(i7)); return true;
        case Op::rothmi: ri("rothm", splat_half(i7)); return true;
        case Op::rotmi: ri("rotm", splat(i7)); return true;
        case Op::rotmahi: ri("rotmah", splat_half(i7)); return true;
        case Op::rotmai: ri("rotma", splat(i7)); return true;
        case Op::shlqbi: quad("shlqbi", count_b); return true;
        case Op::shlqby: quad("shlqby", count_b); return true;
        case Op::shlqbybi: quad("shlqbybi", count_b); return true;
        case Op::rotqbi: quad("rotqbi", count_b); return true;
        case Op::rotqby: quad("rotqby", count_b); return true;
        case Op::rotqbybi: quad("rotqbybi", count_b); return true;
        case Op::rotqmbi: quad("rotqmbi", count_b); return true;
        case Op::rotqmby: quad("rotqmby", count_b); return true;
        case Op::rotqmbybi: quad("rotqmbybi", count_b); return true;
        case Op::shlqbii: quad("shlqbi", count_i); return true;
        case Op::shlqbyi: quad("shlqby", count_i); return true;
        case Op::rotqbii: quad("rotqbi", count_i); return true;
        case Op::rotqbyi: quad("rotqby", count_i); return true;
        case Op::rotqmbii: quad("rotqmbi", count_i); return true;
        case Op::rotqmbyi: quad("rotqmby", count_i); return true;

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
        case Op::heq: line(std::format("if ({}.w[0] == {}.w[0]) s.hooks->halt(s, {}, \"heq\");", R(a), R(b), hex(pc))); return true;
        case Op::hgt:
            line(std::format("if (static_cast<int32_t>({}.w[0]) > static_cast<int32_t>({}.w[0])) s.hooks->halt(s, {}, \"hgt\");", R(a), R(b), hex(pc)));
            return true;
        case Op::hlgt: line(std::format("if ({}.w[0] > {}.w[0]) s.hooks->halt(s, {}, \"hlgt\");", R(a), R(b), hex(pc))); return true;
        case Op::heqi: line(std::format("if ({}.w[0] == {}) s.hooks->halt(s, {}, \"heqi\");", R(a), hex(i10), hex(pc))); return true;
        case Op::hgti:
            line(std::format("if (static_cast<int32_t>({}.w[0]) > {}) s.hooks->halt(s, {}, \"hgti\");", R(a), in.i10(), hex(pc)));
            return true;
        case Op::hlgti: line(std::format("if ({}.w[0] > {}) s.hooks->halt(s, {}, \"hlgti\");", R(a), hex(i10), hex(pc))); return true;

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
        case Op::fscrrd: line(std::format("{} = spu::fscrrd(s);", R(t))); return true;
        case Op::fscrwr:
            line(std::format("spu::fscrwr(s, {}); if (s.fpscr[0] & 0xF00u) s.hooks->halt(s, {}, \"a double-precision rounding mode other than to nearest, "
                             "which this runtime doesn't provide\");",
                             R(a), hex(pc)));
            return true;
        case Op::fma: line(std::format("{} = spu::fma({}, {}, {});", R(in.rt4()), R(a), R(b), R(t))); return true;
        case Op::fms: line(std::format("{} = spu::fms({}, {}, {});", R(in.rt4()), R(a), R(b), R(t))); return true;
        case Op::fnms: line(std::format("{} = spu::fnms({}, {}, {});", R(in.rt4()), R(a), R(b), R(t))); return true;
        case Op::csflt: line(std::format("{} = spu::csflt({}, {});", R(t), R(a), 155 - static_cast<int>(in.u8()))); return true;
        case Op::cuflt: line(std::format("{} = spu::cuflt({}, {});", R(t), R(a), 155 - static_cast<int>(in.u8()))); return true;
        case Op::cflts: line(std::format("{} = spu::cflts({}, {});", R(t), R(a), 173 - static_cast<int>(in.u8()))); return true;
        case Op::cfltu: line(std::format("{} = spu::cfltu({}, {});", R(t), R(a), 173 - static_cast<int>(in.u8()))); return true;

        // ---------------------------------------------------------------- channels and stops
        case Op::rdch: line(std::format("{} = SpuReg{{{{s.hooks->read_channel(s, {}u, {}), 0u, 0u, 0u}}}};", R(t), a, hex(pc))); return true;
        case Op::rchcnt: line(std::format("{} = SpuReg{{{{s.hooks->channel_count(s, {}u, {}), 0u, 0u, 0u}}}};", R(t), a, hex(pc))); return true;
        case Op::wrch: line(std::format("s.hooks->write_channel(s, {}u, {}, {});", a, word, hex(pc))); return true;
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
            line(std::format("{} = {}; {}(s);", R(t), link, spu_fn_name(target)));
            return true;
        }
        case Op::brz: case Op::brnz: case Op::brhz: case Op::brhnz: {
            std::string go;
            if (!jump(direct_target(in, pc), go)) return false;
            const bool half = in.op == Op::brhz || in.op == Op::brhnz, zero = in.op == Op::brz || in.op == Op::brhz;
            line(std::format("if (({}{}) {} 0u) {}", word, half ? " & 0xFFFFu" : "", zero ? "==" : "!=", go));
            return true;
        }
        case Op::bi: {
            if (const auto it = tables_.find(pc); it != tables_.end()) {
                std::string cases;
                std::set<uint32_t> seen;
                for (const uint32_t target : it->second->targets)
                    if (seen.insert(target).second) cases += std::format(" case {}: goto L_{:05X};", hex(target), target);
                line(std::format("switch ({}.w[0] & 0x3FFFCu) {{{} default: s.hooks->halt(s, {}, \"a switch to an address outside its table\"); }}", R(a), cases, hex(pc)));
                return true;
            }
            // $0 holds the address a call left there: this is the function's return.
            if (a == 0) line("return;");
            else line(std::format("{{ call(s, {}.w[0] & 0x3FFFCu, {}); return; }}", R(a), hex(pc)));
            return true;
        }
        case Op::bisl:
            line(std::format("{{ const uint32_t target = {}.w[0] & 0x3FFFCu; {} = {}; call(s, target, {}); }}", R(a), R(t), link, hex(pc)));
            return true;
        case Op::biz: case Op::binz: case Op::bihz: case Op::bihnz: {
            const bool half = in.op == Op::bihz || in.op == Op::bihnz, zero = in.op == Op::biz || in.op == Op::bihz;
            const std::string go = a == 0 ? std::string("return;") : std::format("{{ call(s, {}.w[0] & 0x3FFFCu, {}); return; }}", R(a), hex(pc));
            line(std::format("if (({}{}) {} 0u) {}", word, half ? " & 0xFFFFu" : "", zero ? "==" : "!=", go));
            return true;
        }
        default: return fail(std::format("the lifter has no case for {}", spu::spec(in.op).name));
        }
    }

    const SpuCfg* cfg_;  // null for spu_lift_one: only branches and run() read it
    const SpuFunction& f_;
    std::map<uint32_t, const SpuJumpTable*> tables_;
    std::set<uint32_t> present_, labels_;
    std::string error_;
};

}  // namespace

SpuLifted spu_lift(const SpuCfg& cfg, const SpuFunction& f) { return Emitter(&cfg, f).run(); }

std::optional<std::string> spu_lift_one(const Insn& in, uint32_t pc) {
    if (!spu_in_slice(in) || (spu::spec(in.op).flags & spu::kBranch)) return std::nullopt;
    // (No instruction but a branch looks at the program or the function.)
    static const SpuFunction no_function;
    std::string line;
    if (!Emitter(nullptr, no_function).one(pc, in, line)) return std::nullopt;
    return line;
}

}  // namespace qilin

#include "lift.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace qilin {

using ppu::Insn;
using ppu::Op;

std::string fn_name(uint32_t address) { return std::format("fn_{:08X}", address); }

namespace {

constexpr unsigned kXer = 1, kLr = 8, kCtr = 9;

// Bits mb to me inclusive, numbered from 0 at the most significant, wrapping round if mb > me.
uint64_t mask64(unsigned mb, unsigned me) {
    const uint64_t from = ~uint64_t{0} >> mb;
    const uint64_t to = me == 63 ? ~uint64_t{0} : ~(~uint64_t{0} >> (me + 1));
    return mb <= me ? (from & to) : (from | to);
}

std::string hex64(uint64_t v) { return std::format("0x{:016X}ull", v); }
std::string hex32(uint32_t v) { return std::format("0x{:08X}u", v); }

// " + 16ull" or " - 16ull": a signed constant added to a 64-bit value.
std::string plus(int32_t v) {
    return v < 0 ? std::format(" - {}ull", static_cast<uint64_t>(-static_cast<int64_t>(v))) : std::format(" + {}ull", static_cast<uint64_t>(v));
}
// A sign-extended constant as a 64-bit value.
std::string imm64(int64_t v) { return hex64(static_cast<uint64_t>(v)); }

// `pattern` with each '$' replaced by x and each '#' by y.
std::string fill(std::string_view pattern, const std::string& x, const std::string& y = {}) {
    std::string out;
    for (const char ch : pattern) {
        if (ch == '$') out += x;
        else if (ch == '#') out += y;
        else out += ch;
    }
    return out;
}

// Nothing runs on past it.
bool terminator(const Insn& in) {
    const bool always = (in.rd() & 0x14) == 0x14;
    switch (in.op) {
    case Op::b: return !in.lk();
    case Op::bc: case Op::bclr: case Op::bcctr: return always && !in.lk();
    case Op::tw: case Op::td: case Op::twi: case Op::tdi: return in.rd() == 31;
    default: return false;
    }
}

}  // namespace

bool in_slice(const Insn& in, std::string* why) {
    const auto no = [&](std::string w) {
        if (why) *why = std::move(w);
        return false;
    };
    if (!in.valid()) return no("an invalid word");
    const ppu::Spec& s = ppu::spec(in.op);
    if (s.flags & ppu::kVector) {
        switch (in.op) {
        case Op::lvx: case Op::stvx: case Op::lvsl: case Op::lvsr: case Op::vand: case Op::vandc: case Op::vor: case Op::vxor: case Op::vnor:
        case Op::vmrghw: case Op::vmrglw: case Op::vadduwm: case Op::vaddfp: case Op::vsubfp: case Op::vmaxfp: case Op::vminfp:
        case Op::vcmpequw: case Op::vcmpeqfp: case Op::vsel: case Op::vperm: case Op::vsldoi: case Op::vmaddfp: case Op::vnmsubfp:
        case Op::vspltw: case Op::vspltisw: case Op::vcfsx: case Op::vctsxs: case Op::vrefp: case Op::vrsqrtefp:
            return true;
        default: return no(std::format("VMX ({})", s.name));
        }
    }
    if (s.flags & ppu::kFloat) {
        switch (in.op) {
        case Op::mffs: case Op::mtfsf: case Op::mtfsfi: case Op::mtfsb0: case Op::mtfsb1: case Op::mcrfs:
            return no(std::format("FPSCR access ({})", s.name));
        case Op::fres: case Op::frsqrte: return no(std::format("an estimate ({})", s.name));
        default: break;
        }
        if ((s.flags & ppu::kRc) && in.rc()) return no(std::format("floating-point record form ({}.)", s.name));
    }
    if ((s.flags & ppu::kOe) && in.oe()) return no(std::format("overflow-enable form ({}o)", s.name));
    switch (in.op) {
    case Op::lmw: case Op::stmw: case Op::lswi: case Op::lswx: case Op::stswi: case Op::stswx:
        return no("multiple or string load/store");
    case Op::lwarx: case Op::ldarx: case Op::stwcx_rc: case Op::stdcx_rc: return no("atomic sequence");
    case Op::mcrxr: return no("mcrxr");
    case Op::dcbz: return no("dcbz");
    case Op::mfspr: case Op::mtspr: {
        const unsigned n = in.spr();
        if (n == kXer || n == kLr || n == kCtr) return true;
        return no(std::format("special-purpose register {}", n));
    }
    default: return true;
    }
}

namespace {

class Emitter {
public:
    explicit Emitter(const LiftInput& f) : f_(f) {
        for (const auto& [a, in] : f.insns) present_.insert(a);
        for (const JumpTable& t : f.tables) tables_[t.at] = &t;
    }

    Lifted run() {
        Lifted out;
        for (const auto& [a, in] : f_.insns) {
            std::string why;
            if (!in_slice(in, &why)) {
                out.why = why;
                out.why_at = a;
                return out;
            }
        }
        // Labels: every branch target inside the function, every switch target, every landing pad.
        for (const auto& [a, in] : f_.insns) {
            if ((in.op == Op::b || in.op == Op::bc) && !in.lk()) {
                const uint32_t t = in.branch_target(a);
                if (present_.contains(t)) labels_.insert(t);
            }
            if (const auto it = tables_.find(a); it != tables_.end())
                for (const uint32_t t : it->second->targets) labels_.insert(t);
        }
        for (const uint32_t pad : f_.landing_pads) labels_.insert(pad);
        std::string body;
        for (size_t k = 0; k < f_.insns.size(); ++k) {
            const auto& [a, in] = f_.insns[k];
            if (labels_.contains(a)) body += std::format("L_{:08X}:\n", a);
            std::string line;
            if (!insn(a, in, line)) {
                out.why = error_;
                out.why_at = a;
                return out;
            }
            if (f_.keep_pc && (line.find("read") != std::string::npos || line.find("write") != std::string::npos)) body += std::format("    c.pc = 0x{:08X}u;\n", a);
            body += line;
            const bool last = k + 1 == f_.insns.size();
            const uint32_t next = last ? 0 : f_.insns[k + 1].first;
            if (!terminator(in) && !tables_.contains(a) && (last || next != a + 4)) {
                // Execution continues past this instruction into code that isn't the next one we have.
                if (f_.falls_into == a + 4) {
                    body += std::format("    {} {}(c); return;\n", sync_out(), fn_name(a + 4));
                } else {
                    out.why = std::format("execution runs on to {:08X}", a + 4);
                    out.why_at = a;
                    return out;
                }
            }
        }
        out.code = expand(std::format("void {}(Cpu& c) {{\n{}{}}}\n", fn_name(f_.start), prologue(), body));
        out.ok = true;
        return out;
    }

private:
    // Register uses, recorded as the body is emitted.
    std::string R(unsigned n) {
        gprs_.insert(n);
        return std::format("r{}", n);
    }
    std::string CR(unsigned n) {
        crs_.insert(n);
        return std::format("cr{}", n);
    }
    std::string F(unsigned n) {
        fprs_.insert(n);
        return std::format("f{}", n);
    }
    std::string V(unsigned n) {
        vrs_.insert(n);
        return std::format("v{}", n);
    }
    std::string use(const char* n) {
        specials_.insert(n);
        return n;
    }
    std::string SO() { return use("so"); }
    std::string OV() { return use("ov"); }
    std::string CA() { return use("ca"); }
    std::string LR() { return use("lr"); }
    std::string CTR() { return use("ctr"); }

    std::string prologue() const {
        std::string p;
        for (const unsigned n : gprs_) p += std::format("    uint64_t r{0} = c.r[{0}];\n", n);
        for (const unsigned n : crs_) p += std::format("    uint32_t cr{0} = (c.cr >> {1}) & 15u;\n", n, 28 - 4 * n);
        for (const unsigned n : fprs_) p += std::format("    double f{0} = c.f[{0}];\n", n);
        for (const unsigned n : vrs_) p += std::format("    Vec v{0} = c.v[{0}];\n", n);
        if (specials_.contains("lr")) p += "    uint64_t lr = c.lr;\n";
        if (specials_.contains("ctr")) p += "    uint64_t ctr = c.ctr;\n";
        if (specials_.contains("so")) p += "    uint32_t so = c.xer_so;\n";
        if (specials_.contains("ov")) p += "    uint32_t ov = c.xer_ov;\n";
        if (specials_.contains("ca")) p += "    uint32_t ca = c.xer_ca;\n";
        return p;
    }
    // The locals are written back before anything outside the function can
    // see the state (calls, returns, traps), and read again after a call.
    // The text is a placeholder until the function's uses are known.
    static std::string sync_out() { return "@OUT@"; }
    static std::string sync_in() { return "@IN@"; }

    std::string expand(const std::string& code) const {
        std::string out_s, in_s;
        for (const unsigned n : gprs_) {
            out_s += std::format("c.r[{0}] = r{0}; ", n);
            in_s += std::format("r{0} = c.r[{0}]; ", n);
        }
        for (const unsigned n : fprs_) {
            out_s += std::format("c.f[{0}] = f{0}; ", n);
            in_s += std::format("f{0} = c.f[{0}]; ", n);
        }
        for (const unsigned n : vrs_) {
            out_s += std::format("c.v[{0}] = v{0}; ", n);
            in_s += std::format("v{0} = c.v[{0}]; ", n);
        }
        if (!crs_.empty()) {
            uint32_t keep = 0xFFFFFFFFu;
            std::string merged;
            for (const unsigned n : crs_) {
                keep &= ~(15u << (28 - 4 * n));
                merged += std::format(" | (cr{} << {})", n, 28 - 4 * n);
                in_s += std::format("cr{0} = (c.cr >> {1}) & 15u; ", n, 28 - 4 * n);
            }
            out_s += std::format("c.cr = (c.cr & {}){}; ", hex32(keep), merged);
        }
        for (const char* s : {"lr", "ctr"})
            if (specials_.contains(s)) {
                out_s += std::format("c.{0} = {0}; ", s);
                in_s += std::format("{0} = c.{0}; ", s);
            }
        for (const char* s : {"so", "ov", "ca"})
            if (specials_.contains(s)) {
                out_s += std::format("c.xer_{0} = {0}; ", s);
                in_s += std::format("{0} = c.xer_{0}; ", s);
            }
        std::string r = code;
        for (const auto& [placeholder, with] : {std::pair{std::string("@OUT@"), out_s}, std::pair{std::string("@IN@"), in_s}})
            for (size_t p; (p = r.find(placeholder)) != std::string::npos;) r.replace(p, placeholder.size(), with);
        return r;
    }

    // The condition of a bc/bclr/bcctr, after decrementing CTR if BO says so.
    std::string condition(const Insn& in, std::string& pre) {
        const unsigned bo = in.rd(), bi = in.ra();
        std::vector<std::string> parts;
        if (!(bo & 4)) {
            pre += std::format("{0} -= 1ull; ", CTR());
            parts.push_back((bo & 2) ? "ctr == 0ull" : "ctr != 0ull");
        }
        if (!(bo & 0x10)) {
            const unsigned m = 8u >> (bi & 3);
            const std::string field = CR(bi >> 2);
            parts.push_back((bo & 8) ? std::format("({} & {}u)", field, m) : std::format("!({} & {}u)", field, m));
        }
        if (parts.empty()) return "true";
        std::string c = parts[0];
        for (size_t k = 1; k < parts.size(); ++k) c += " && " + parts[k];
        return c;
    }

    // A transfer to `target` that leaves the function: a tail call.
    bool tail_to(uint32_t target, std::string& out) {
        if (!f_.is_function || !f_.is_function(target)) {
            error_ = std::format("branch to {:08X}, which is neither in the function nor a function", target);
            return false;
        }
        out = std::format("{} {}(c); return;", sync_out(), fn_name(target));
        return true;
    }

    // One bit of the condition register, as 0 or 1.
    std::string cr_bit(unsigned bit) { return std::format("(({} >> {}) & 1u)", CR(bit >> 2), 3 - (bit & 3)); }

    bool insn(uint32_t pc, const Insn& in, std::string& out) {
        const unsigned d = in.rd(), a = in.ra(), b = in.rb();
        const bool record_form = (ppu::spec(in.op).flags & ppu::kRc) && in.rc();
        const auto line = [&](std::string s) { out = "    " + s + "\n"; };
        // The result register, then the record form on it.
        const auto result = [&](unsigned dst, std::string expr, bool record = false) {
            std::string s = std::format("{} = {};", R(dst), expr);
            if (record_form || record) s += std::format(" {} = record(r{}, {});", CR(0), dst, SO());
            line(s);
        };
        // Effective addresses, as the 64-bit sum (an update form writes it back whole) and truncated to an address.
        const auto ea_d = [&](int32_t disp) { return a ? R(a) + plus(disp) : imm64(disp); };
        const auto ea_x = [&] { return a ? std::format("{} + {}", R(a), R(b)) : R(b); };
        const auto addr = [](const std::string& ea) { return std::format("static_cast<uint32_t>({})", ea); };
        // Loads: `wrap` turns what was read into the register's value ("{}" is the read).
        const auto load = [&](const char* read, const std::string& ea, bool update, const char* wrap = "$") {
            if (update) {
                line(std::format("{{ const uint64_t ea = {}; {} = {}; {} = ea; }}", ea, R(d),
                                 fill(wrap, std::format("{}(c, static_cast<uint32_t>(ea))", read)), R(a)));
            } else {
                const std::string value = std::format("{}(c, {})", read, addr(ea));
                line(std::format("{} = {};", R(d), fill(wrap, value)));
            }
        };
        const auto store = [&](const char* write, const std::string& ea, bool update, const char* wrap = "$") {
            const std::string value = fill(wrap, R(d));
            if (update) line(std::format("{{ const uint64_t ea = {}; {}(c, static_cast<uint32_t>(ea), {}); {} = ea; }}", ea, write, value, R(a)));
            else line(std::format("{}(c, {}, {});", write, addr(ea), value));
        };
        const char* const kSext8 = "static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>($)))";
        const char* const kSext16 = "static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>($)))";
        const char* const kSext32 = "static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>($)))";
        const char* const kSwap16 = "std::byteswap(static_cast<uint16_t>($))";
        const char* const kSwap32 = "std::byteswap(static_cast<uint32_t>($))";
        const char* const kSwap64 = "std::byteswap(static_cast<uint64_t>($))";
        const auto carry = [&](const std::string& x, const std::string& y, const std::string& carry_in) {
            const std::string ca = CA();
            result(d, std::format("add_carry({}, {}, {}, {})", x, y, carry_in, ca));
        };
        const auto cr_logic = [&](const char* format) {
            const std::string x = cr_bit(a), y = cr_bit(b);
            const unsigned shift = 3 - (d & 3);
            const std::string field = CR(d >> 2);
            line(std::format("{{ const uint32_t bit = ({}) & 1u; {} = ({} & {}u) | (bit << {}); }}", fill(format, x, y),
                             field, field, 15u & ~(1u << shift), shift));
        };
        const auto trap = [&](const std::string& x, const std::string& y) {
            if (d == 31) line(std::format("{} c.hooks->trap(c, {}, \"trap\"); return;", sync_out(), hex32(pc)));
            else line(std::format("if (trap_on({}u, {}, {})) {{ {} c.hooks->trap(c, {}, \"trap\"); return; }}", d, x, y, sync_out(), hex32(pc)));
        };
        // Floating-point loads, stores and operations: the helpers are cpu.hpp's fp:: functions.
        const auto fload = [&](const char* read, const std::string& ea, bool update, const char* wrap) {
            if (update) line(std::format("{{ const uint64_t ea = {}; {} = {}; {} = ea; }}", ea, F(d), fill(wrap, std::format("{}(c, static_cast<uint32_t>(ea))", read)), R(a)));
            else line(std::format("{} = {};", F(d), fill(wrap, std::format("{}(c, {})", read, addr(ea)))));
        };
        const auto fstore = [&](const char* write, const std::string& ea, bool update, const char* wrap) {
            if (update) line(std::format("{{ const uint64_t ea = {}; {}(c, static_cast<uint32_t>(ea), {}); {} = ea; }}", ea, write, fill(wrap, F(d)), R(a)));
            else line(std::format("{}(c, {}, {});", write, addr(ea), fill(wrap, F(d))));
        };
        const auto fp1 = [&](const char* fn) { line(std::format("{} = fp::{}({});", F(d), fn, F(b))); };
        const auto fp2 = [&](const char* fn, unsigned x, unsigned y) { line(std::format("{} = fp::{}({}, {});", F(d), fn, F(x), F(y))); };
        const auto fp3 = [&](const char* fn) { line(std::format("{} = fp::{}({}, {}, {});", F(d), fn, F(a), F(in.rc_reg()), F(b))); };
        // A two-operand vector operation; a recording compare also sets CR6 from its result.
        const auto vx2 = [&](const char* fn, bool compare = false) {
            std::string s = std::format("{} = vmx::{}({}, {});", V(d), fn, V(a), V(b));
            if (compare && in.oe()) s += std::format(" {} = vmx::cr6(v{});", CR(6), d);
            line(s);
        };
        const auto s32 = [&](unsigned n) { return std::format("static_cast<int32_t>({})", R(n)); };
        const auto s64 = [&](unsigned n) { return std::format("static_cast<int64_t>({})", R(n)); };
        const auto u32 = [&](unsigned n) { return std::format("static_cast<uint32_t>({})", R(n)); };

        switch (in.op) {
        // ---------------------------------------------------------------- integer arithmetic
        case Op::addi: result(d, a ? R(a) + plus(in.simm()) : imm64(in.simm())); return true;
        case Op::addis: result(d, a ? std::format("{} + {}", R(a), imm64(int64_t{in.simm()} << 16)) : imm64(int64_t{in.simm()} << 16)); return true;
        case Op::add: result(d, std::format("{} + {}", R(a), R(b))); return true;
        case Op::subf: result(d, std::format("{} - {}", R(b), R(a))); return true;
        case Op::neg: result(d, std::format("0ull - {}", R(a))); return true;
        case Op::addic: carry(R(a), imm64(in.simm()), "0u"); return true;
        case Op::addic_rc: {
            const std::string ca = CA();
            result(d, std::format("add_carry({}, {}, 0u, {})", R(a), imm64(in.simm()), ca), true);
            return true;
        }
        case Op::subfic: carry("~" + R(a), imm64(in.simm()), "1u"); return true;
        case Op::addc: carry(R(a), R(b), "0u"); return true;
        case Op::subfc: carry("~" + R(a), R(b), "1u"); return true;
        case Op::adde: carry(R(a), R(b), CA()); return true;
        case Op::subfe: carry("~" + R(a), R(b), CA()); return true;
        case Op::addze: carry(R(a), "0ull", CA()); return true;
        case Op::subfze: carry("~" + R(a), "0ull", CA()); return true;
        case Op::addme: carry(R(a), "~0ull", CA()); return true;
        case Op::subfme: carry("~" + R(a), "~0ull", CA()); return true;
        case Op::mulli: result(d, std::format("{} * {}", R(a), imm64(in.simm()))); return true;
        case Op::mullw: result(d, std::format("static_cast<uint64_t>(int64_t{{{}}} * int64_t{{{}}})", s32(a), s32(b))); return true;
        case Op::mulld: result(d, std::format("{} * {}", R(a), R(b))); return true;
        case Op::mulhw: result(d, std::format("mulhw({}, {})", R(a), R(b))); return true;
        case Op::mulhwu: result(d, std::format("mulhwu({}, {})", R(a), R(b))); return true;
        case Op::mulhd: result(d, std::format("mulhd({}, {})", R(a), R(b))); return true;
        case Op::mulhdu: result(d, std::format("mulhdu({}, {})", R(a), R(b))); return true;
        case Op::divw: result(d, std::format("divw({}, {})", R(a), R(b))); return true;
        case Op::divwu: result(d, std::format("divwu({}, {})", R(a), R(b))); return true;
        case Op::divd: result(d, std::format("divd({}, {})", R(a), R(b))); return true;
        case Op::divdu: result(d, std::format("divdu({}, {})", R(a), R(b))); return true;

        // ---------------------------------------------------------------- compare
        case Op::cmp:
            line(std::format("{} = compare_s({}, {}, {});", CR(in.crfd()), in.l() ? s64(a) : s32(a), in.l() ? s64(b) : s32(b), SO()));
            return true;
        case Op::cmpi:
            line(std::format("{} = compare_s({}, {}, {});", CR(in.crfd()), in.l() ? s64(a) : s32(a), in.simm(), SO()));
            return true;
        case Op::cmpl:
            line(std::format("{} = compare_u({}, {}, {});", CR(in.crfd()), in.l() ? R(a) : u32(a), in.l() ? R(b) : u32(b), SO()));
            return true;
        case Op::cmpli:
            line(std::format("{} = compare_u({}, {}u, {});", CR(in.crfd()), in.l() ? R(a) : u32(a), in.uimm(), SO()));
            return true;

        // ---------------------------------------------------------------- logical (the result is rA, the source rS = d)
        case Op::ori:
            if (in.uimm() == 0 && a == d) line("// nop");
            else result(a, std::format("{} | {}ull", R(d), in.uimm()));
            return true;
        case Op::oris: result(a, std::format("{} | {}", R(d), hex64(uint64_t{in.uimm()} << 16))); return true;
        case Op::xori: result(a, std::format("{} ^ {}ull", R(d), in.uimm())); return true;
        case Op::xoris: result(a, std::format("{} ^ {}", R(d), hex64(uint64_t{in.uimm()} << 16))); return true;
        case Op::andi_rc: result(a, std::format("{} & {}ull", R(d), in.uimm()), true); return true;
        case Op::andis_rc: result(a, std::format("{} & {}", R(d), hex64(uint64_t{in.uimm()} << 16)), true); return true;
        case Op::and_: result(a, std::format("{} & {}", R(d), R(b))); return true;
        case Op::or_: result(a, d == b ? R(d) : std::format("{} | {}", R(d), R(b))); return true;
        case Op::xor_: result(a, std::format("{} ^ {}", R(d), R(b))); return true;
        case Op::andc: result(a, std::format("{} & ~{}", R(d), R(b))); return true;
        case Op::orc: result(a, std::format("{} | ~{}", R(d), R(b))); return true;
        case Op::nor: result(a, std::format("~({} | {})", R(d), R(b))); return true;
        case Op::nand: result(a, std::format("~({} & {})", R(d), R(b))); return true;
        case Op::eqv: result(a, std::format("~({} ^ {})", R(d), R(b))); return true;
        case Op::extsb: result(a, fill(kSext8, R(d))); return true;
        case Op::extsh: result(a, fill(kSext16, R(d))); return true;
        case Op::extsw: result(a, fill(kSext32, R(d))); return true;
        case Op::cntlzw: result(a, std::format("static_cast<uint64_t>(std::countl_zero({}))", u32(d))); return true;
        case Op::cntlzd: result(a, std::format("static_cast<uint64_t>(std::countl_zero({}))", R(d))); return true;

        // ---------------------------------------------------------------- rotate and shift
        case Op::rlwinm: result(a, std::format("rot32({}, {}u) & {}", R(d), b, hex64(mask64(in.mb() + 32, in.me() + 32)))); return true;
        case Op::rlwnm: result(a, std::format("rot32({}, static_cast<unsigned>({})) & {}", R(d), R(b), hex64(mask64(in.mb() + 32, in.me() + 32)))); return true;
        case Op::rlwimi: {
            const uint64_t m = mask64(in.mb() + 32, in.me() + 32);
            result(a, std::format("(rot32({}, {}u) & {}) | ({} & {})", R(d), b, hex64(m), R(a), hex64(~m)));
            return true;
        }
        case Op::rldicl: result(a, std::format("std::rotl({}, {}) & {}", R(d), in.sh6(), hex64(mask64(in.mb6(), 63)))); return true;
        case Op::rldicr: result(a, std::format("std::rotl({}, {}) & {}", R(d), in.sh6(), hex64(mask64(0, in.mb6())))); return true;
        case Op::rldic: result(a, std::format("std::rotl({}, {}) & {}", R(d), in.sh6(), hex64(mask64(in.mb6(), 63 - in.sh6())))); return true;
        case Op::rldimi: {
            const uint64_t m = mask64(in.mb6(), 63 - in.sh6());
            result(a, std::format("(std::rotl({}, {}) & {}) | ({} & {})", R(d), in.sh6(), hex64(m), R(a), hex64(~m)));
            return true;
        }
        case Op::rldcl: result(a, std::format("std::rotl({}, static_cast<int>({} & 63u)) & {}", R(d), R(b), hex64(mask64(in.mb6(), 63)))); return true;
        case Op::rldcr: result(a, std::format("std::rotl({}, static_cast<int>({} & 63u)) & {}", R(d), R(b), hex64(mask64(0, in.mb6())))); return true;
        case Op::slw: result(a, std::format("slw({}, {})", R(d), R(b))); return true;
        case Op::srw: result(a, std::format("srw({}, {})", R(d), R(b))); return true;
        case Op::sld: result(a, std::format("sld({}, {})", R(d), R(b))); return true;
        case Op::srd: result(a, std::format("srd({}, {})", R(d), R(b))); return true;
        case Op::sraw: { const std::string ca = CA(); result(a, std::format("sraw({}, {}, {})", R(d), R(b), ca)); return true; }
        case Op::srawi: { const std::string ca = CA(); result(a, std::format("sraw({}, {}ull, {})", R(d), b, ca)); return true; }
        case Op::srad: { const std::string ca = CA(); result(a, std::format("srad({}, {}, {})", R(d), R(b), ca)); return true; }
        case Op::sradi: { const std::string ca = CA(); result(a, std::format("srad({}, {}ull, {})", R(d), in.sh6(), ca)); return true; }

        // ---------------------------------------------------------------- the condition register
        case Op::mcrf: line(std::format("{} = {};", CR(in.crfd()), CR(in.crfs()))); return true;
        case Op::mfcr: {
            std::string all;
            for (unsigned n = 0; n < 8; ++n) all += std::format("{}({} << {})", n ? " | " : "", CR(n), 28 - 4 * n);
            result(d, std::format("uint64_t{{{}}}", all));
            return true;
        }
        case Op::mfocrf: {  // the one field named; the rest of the register is undefined: here, zero
            const unsigned n = static_cast<unsigned>(std::countl_zero(static_cast<uint8_t>(in.crm())));
            result(d, std::format("uint64_t{{{} << {}}}", CR(n), 28 - 4 * n));
            return true;
        }
        case Op::mtcrf: case Op::mtocrf: {
            std::string s;
            for (unsigned n = 0; n < 8; ++n)
                if (in.crm() & (0x80u >> n)) s += std::format("{} = static_cast<uint32_t>({} >> {}) & 15u; ", CR(n), R(d), 28 - 4 * n);
            line(s.empty() ? "// mtcrf of no fields" : s);
            return true;
        }
        case Op::crand: cr_logic("$ & #"); return true;
        case Op::cror: cr_logic("$ | #"); return true;
        case Op::crxor: cr_logic("$ ^ #"); return true;
        case Op::crnand: cr_logic("~($ & #)"); return true;
        case Op::crnor: cr_logic("~($ | #)"); return true;
        case Op::creqv: cr_logic("~($ ^ #)"); return true;
        case Op::crandc: cr_logic("$ & ~#"); return true;
        case Op::crorc: cr_logic("$ | ~#"); return true;

        // ---------------------------------------------------------------- loads
        case Op::lbz: load("read8", ea_d(in.simm()), false); return true;
        case Op::lbzu: load("read8", ea_d(in.simm()), true); return true;
        case Op::lbzx: load("read8", ea_x(), false); return true;
        case Op::lbzux: load("read8", ea_x(), true); return true;
        case Op::lhz: load("read16", ea_d(in.simm()), false); return true;
        case Op::lhzu: load("read16", ea_d(in.simm()), true); return true;
        case Op::lhzx: load("read16", ea_x(), false); return true;
        case Op::lhzux: load("read16", ea_x(), true); return true;
        case Op::lha: load("read16", ea_d(in.simm()), false, kSext16); return true;
        case Op::lhau: load("read16", ea_d(in.simm()), true, kSext16); return true;
        case Op::lhax: load("read16", ea_x(), false, kSext16); return true;
        case Op::lhaux: load("read16", ea_x(), true, kSext16); return true;
        case Op::lwz: load("read32", ea_d(in.simm()), false); return true;
        case Op::lwzu: load("read32", ea_d(in.simm()), true); return true;
        case Op::lwzx: load("read32", ea_x(), false); return true;
        case Op::lwzux: load("read32", ea_x(), true); return true;
        case Op::lwa: load("read32", ea_d(in.ds()), false, kSext32); return true;
        case Op::lwax: load("read32", ea_x(), false, kSext32); return true;
        case Op::lwaux: load("read32", ea_x(), true, kSext32); return true;
        case Op::ld: load("read64", ea_d(in.ds()), false); return true;
        case Op::ldu: load("read64", ea_d(in.ds()), true); return true;
        case Op::ldx: load("read64", ea_x(), false); return true;
        case Op::ldux: load("read64", ea_x(), true); return true;
        case Op::lhbrx: load("read16", ea_x(), false, "uint64_t{std::byteswap(static_cast<uint16_t>($))}"); return true;
        case Op::lwbrx: load("read32", ea_x(), false, "uint64_t{std::byteswap(static_cast<uint32_t>($))}"); return true;
        case Op::ldbrx: load("read64", ea_x(), false, kSwap64); return true;

        // ---------------------------------------------------------------- stores
        case Op::stb: store("write8", ea_d(in.simm()), false); return true;
        case Op::stbu: store("write8", ea_d(in.simm()), true); return true;
        case Op::stbx: store("write8", ea_x(), false); return true;
        case Op::stbux: store("write8", ea_x(), true); return true;
        case Op::sth: store("write16", ea_d(in.simm()), false); return true;
        case Op::sthu: store("write16", ea_d(in.simm()), true); return true;
        case Op::sthx: store("write16", ea_x(), false); return true;
        case Op::sthux: store("write16", ea_x(), true); return true;
        case Op::stw: store("write32", ea_d(in.simm()), false); return true;
        case Op::stwu: store("write32", ea_d(in.simm()), true); return true;
        case Op::stwx: store("write32", ea_x(), false); return true;
        case Op::stwux: store("write32", ea_x(), true); return true;
        case Op::std: store("write64", ea_d(in.ds()), false); return true;
        case Op::stdu: store("write64", ea_d(in.ds()), true); return true;
        case Op::stdx: store("write64", ea_x(), false); return true;
        case Op::stdux: store("write64", ea_x(), true); return true;
        case Op::sthbrx: store("write16", ea_x(), false, kSwap16); return true;
        case Op::stwbrx: store("write32", ea_x(), false, kSwap32); return true;
        case Op::stdbrx: store("write64", ea_x(), false, kSwap64); return true;

        // ---------------------------------------------------------------- floating point (cpu.hpp's fp::)
        case Op::lfs: fload("read32", ea_d(in.simm()), false, "fp::load_single($)"); return true;
        case Op::lfsu: fload("read32", ea_d(in.simm()), true, "fp::load_single($)"); return true;
        case Op::lfsx: fload("read32", ea_x(), false, "fp::load_single($)"); return true;
        case Op::lfsux: fload("read32", ea_x(), true, "fp::load_single($)"); return true;
        case Op::lfd: fload("read64", ea_d(in.simm()), false, "fp::from_bits($)"); return true;
        case Op::lfdu: fload("read64", ea_d(in.simm()), true, "fp::from_bits($)"); return true;
        case Op::lfdx: fload("read64", ea_x(), false, "fp::from_bits($)"); return true;
        case Op::lfdux: fload("read64", ea_x(), true, "fp::from_bits($)"); return true;
        case Op::stfs: fstore("write32", ea_d(in.simm()), false, "fp::store_single($)"); return true;
        case Op::stfsu: fstore("write32", ea_d(in.simm()), true, "fp::store_single($)"); return true;
        case Op::stfsx: fstore("write32", ea_x(), false, "fp::store_single($)"); return true;
        case Op::stfsux: fstore("write32", ea_x(), true, "fp::store_single($)"); return true;
        case Op::stfd: fstore("write64", ea_d(in.simm()), false, "fp::bits($)"); return true;
        case Op::stfdu: fstore("write64", ea_d(in.simm()), true, "fp::bits($)"); return true;
        case Op::stfdx: fstore("write64", ea_x(), false, "fp::bits($)"); return true;
        case Op::stfdux: fstore("write64", ea_x(), true, "fp::bits($)"); return true;
        case Op::stfiwx: fstore("write32", ea_x(), false, "fp::bits($)"); return true;  // the low word of the register's bits
        case Op::fmr: line(std::format("{} = {};", F(d), F(b))); return true;
        case Op::fneg: fp1("fneg"); return true;
        case Op::fabs: fp1("fabs"); return true;
        case Op::fnabs: fp1("fnabs"); return true;
        case Op::frsp: fp1("frsp"); return true;
        case Op::fcfid: fp1("fcfid"); return true;
        case Op::fctiw: fp1("fctiw"); return true;
        case Op::fctiwz: fp1("fctiwz"); return true;
        case Op::fctid: fp1("fctid"); return true;
        case Op::fctidz: fp1("fctidz"); return true;
        case Op::fsqrt: fp1("fsqrt"); return true;
        case Op::fsqrts: fp1("fsqrts"); return true;
        case Op::fadd: fp2("fadd", a, b); return true;
        case Op::fadds: fp2("fadds", a, b); return true;
        case Op::fsub: fp2("fsub", a, b); return true;
        case Op::fsubs: fp2("fsubs", a, b); return true;
        case Op::fdiv: fp2("fdiv", a, b); return true;
        case Op::fdivs: fp2("fdivs", a, b); return true;
        case Op::fmul: fp2("fmul", a, in.rc_reg()); return true;
        case Op::fmuls: fp2("fmuls", a, in.rc_reg()); return true;
        case Op::fmadd: fp3("fmadd"); return true;
        case Op::fmadds: fp3("fmadds"); return true;
        case Op::fmsub: fp3("fmsub"); return true;
        case Op::fmsubs: fp3("fmsubs"); return true;
        case Op::fnmadd: fp3("fnmadd"); return true;
        case Op::fnmadds: fp3("fnmadds"); return true;
        case Op::fnmsub: fp3("fnmsub"); return true;
        case Op::fnmsubs: fp3("fnmsubs"); return true;
        case Op::fsel: fp3("fsel"); return true;
        case Op::fcmpu: case Op::fcmpo:  // (they differ in the exceptions they raise, which are not kept)
            line(std::format("{} = fp::fcompare({}, {});", CR(in.crfd()), F(a), F(b)));
            return true;

        // ---------------------------------------------------------------- VMX (vmx.hpp)
        case Op::lvx: line(std::format("{} = vmx::lvx(c, {});", V(d), addr(ea_x()))); return true;
        case Op::stvx: line(std::format("vmx::stvx(c, {}, {});", addr(ea_x()), V(d))); return true;
        case Op::lvsl: line(std::format("{} = vmx::lvsl({});", V(d), addr(ea_x()))); return true;
        case Op::lvsr: line(std::format("{} = vmx::lvsr({});", V(d), addr(ea_x()))); return true;
        case Op::vand: vx2("vand"); return true;
        case Op::vandc: vx2("vandc"); return true;
        case Op::vor: vx2("vor"); return true;
        case Op::vxor: vx2("vxor"); return true;
        case Op::vnor: vx2("vnor"); return true;
        case Op::vmrghw: vx2("vmrghw"); return true;
        case Op::vmrglw: vx2("vmrglw"); return true;
        case Op::vadduwm: vx2("vadduwm"); return true;
        case Op::vaddfp: vx2("vaddfp"); return true;
        case Op::vsubfp: vx2("vsubfp"); return true;
        case Op::vmaxfp: vx2("vmaxfp"); return true;
        case Op::vminfp: vx2("vminfp"); return true;
        case Op::vcmpequw: vx2("vcmpequw", true); return true;
        case Op::vcmpeqfp: vx2("vcmpeqfp", true); return true;
        case Op::vsel: line(std::format("{} = vmx::vsel({}, {}, {});", V(d), V(a), V(b), V(in.rc_reg()))); return true;
        case Op::vperm: line(std::format("{} = vmx::vperm({}, {}, {});", V(d), V(a), V(b), V(in.rc_reg()))); return true;
        case Op::vsldoi: line(std::format("{} = vmx::vsldoi({}, {}, {}u);", V(d), V(a), V(b), in.shb())); return true;
        case Op::vmaddfp: line(std::format("{} = vmx::vmaddfp({}, {}, {});", V(d), V(a), V(in.rc_reg()), V(b))); return true;
        case Op::vnmsubfp: line(std::format("{} = vmx::vnmsubfp({}, {}, {});", V(d), V(a), V(in.rc_reg()), V(b))); return true;
        case Op::vspltw: line(std::format("{} = vmx::vspltw({}, {}u);", V(d), V(b), a & 3)); return true;
        case Op::vspltisw: line(std::format("{} = vmx::vspltisw({});", V(d), in.vsimm())); return true;
        case Op::vrefp: line(std::format("{} = vmx::vrefp({});", V(d), V(b))); return true;
        case Op::vrsqrtefp: line(std::format("{} = vmx::vrsqrtefp({});", V(d), V(b))); return true;
        case Op::vcfsx: line(std::format("{} = vmx::vcfsx({}, {}u);", V(d), V(b), a)); return true;
        case Op::vctsxs: line(std::format("{} = vmx::vctsxs({}, {}u);", V(d), V(b), a)); return true;

        // ---------------------------------------------------------------- LR, CTR, XER
        case Op::mfspr:
            if (in.spr() == kLr) result(d, LR());
            else if (in.spr() == kCtr) result(d, CTR());
            else result(d, std::format("(uint64_t{{{}}} << 31) | (uint64_t{{{}}} << 30) | (uint64_t{{{}}} << 29) | (c.xer_bc & 0x7Fu)", SO(), OV(), CA()));
            return true;
        case Op::mtspr:
            if (in.spr() == kLr) line(std::format("{} = {};", LR(), R(d)));
            else if (in.spr() == kCtr) line(std::format("{} = {};", CTR(), R(d)));
            else
                line(std::format("{0} = static_cast<uint32_t>({3} >> 31) & 1u; {1} = static_cast<uint32_t>({3} >> 30) & 1u; "
                                 "{2} = static_cast<uint32_t>({3} >> 29) & 1u; c.xer_bc = static_cast<uint32_t>({3}) & 0x7Fu;",
                                 SO(), OV(), CA(), R(d)));
            return true;

        // ---------------------------------------------------------------- nothing to do on a host
        case Op::sync: case Op::eieio: case Op::isync: case Op::dcbt: case Op::dcbtst: case Op::dcbst: case Op::dcbf: case Op::icbi:
            line(std::format("// {}", ppu::spec(in.op).name));
            return true;

        // ---------------------------------------------------------------- traps and system calls
        case Op::tw: trap(s32(a), s32(b)); return true;
        case Op::twi: trap(s32(a), std::format("{}", in.simm())); return true;
        case Op::td: trap(s64(a), s64(b)); return true;
        case Op::tdi: trap(s64(a), std::format("{}", in.simm())); return true;
        case Op::sc: line(std::format("{} c.hooks->system_call(c, {}); {}", sync_out(), hex32(pc), sync_in())); return true;
        case Op::mftb:  // the whole time base, or (register 269) its upper half
            result(d, in.spr() == 268 ? std::string("c.hooks->time_base(c)") : std::string("c.hooks->time_base(c) >> 32"));
            return true;

        // ---------------------------------------------------------------- branches
        case Op::b: {
            const uint32_t t = in.branch_target(pc);
            if (in.lk()) {
                if (!f_.is_function || !f_.is_function(t)) {
                    error_ = std::format("call to {:08X}, which is not a function", t);
                    return false;
                }
                line(std::format("{} = {}; {} {}(c); {}", LR(), hex64(pc + 4), sync_out(), fn_name(t), sync_in()));
                return true;
            }
            if (present_.contains(t)) {
                line(std::format("goto L_{:08X};", t));
                return true;
            }
            std::string s;
            if (!tail_to(t, s)) return false;
            line(s);
            return true;
        }
        case Op::bc: {
            const uint32_t t = in.branch_target(pc);
            std::string pre;
            const std::string cond = condition(in, pre);
            if (in.lk()) {
                if (!f_.is_function || !f_.is_function(t)) {
                    error_ = std::format("call to {:08X}, which is not a function", t);
                    return false;
                }
                line(std::format("{}{} = {}; if ({}) {{ {} {}(c); {} }}", pre, LR(), hex64(pc + 4), cond, sync_out(), fn_name(t), sync_in()));
                return true;
            }
            std::string go;
            if (present_.contains(t)) go = std::format("goto L_{:08X};", t);
            else if (!tail_to(t, go)) return false;
            line(cond == "true" ? pre + go : std::format("{}if ({}) {{ {} }}", pre, cond, go));
            return true;
        }
        case Op::bclr: {
            std::string pre;
            const std::string cond = condition(in, pre);
            if (in.lk()) {  // a call through LR
                line(std::format("{}{{ const uint64_t to = {}; {} = {}; if ({}) {{ {} c.hooks->call(c, static_cast<uint32_t>(to) & ~3u); {} }} }}", pre, LR(),
                                 LR(), hex64(pc + 4), cond, sync_out(), sync_in()));
                return true;
            }
            line(cond == "true" ? std::format("{}{} return;", pre, sync_out()) : std::format("{}if ({}) {{ {} return; }}", pre, cond, sync_out()));
            return true;
        }
        case Op::bcctr: {
            std::string pre;
            const std::string cond = condition(in, pre);
            if (in.lk()) {
                line(std::format("{}{} = {}; if ({}) {{ {} c.hooks->call(c, static_cast<uint32_t>({}) & ~3u); {} }}", pre, LR(), hex64(pc + 4), cond,
                                 sync_out(), CTR(), sync_in()));
                return true;
            }
            if (const auto it = tables_.find(pc); it != tables_.end()) {
                // A switch: CTR holds the address of one of the table's targets.
                std::set<uint32_t> targets(it->second->targets.begin(), it->second->targets.end());
                std::string s = std::format("switch (static_cast<uint32_t>({})) {{", CTR());
                for (const uint32_t t : targets) {
                    if (!present_.contains(t)) {
                        error_ = std::format("a switch's target {:08X} is not in the function", t);
                        return false;
                    }
                    s += std::format(" case {}: goto L_{:08X};", hex32(t), t);
                }
                s += std::format(" default: {} c.hooks->trap(c, {}, \"a switch jumped outside its table\"); return; }}", sync_out(), hex32(pc));
                line(s);
                return true;
            }
            line(cond == "true" ? std::format("{}{} c.hooks->call(c, static_cast<uint32_t>({}) & ~3u); return;", pre, sync_out(), CTR())
                                : std::format("{}if ({}) {{ {} c.hooks->call(c, static_cast<uint32_t>({}) & ~3u); return; }}", pre, cond, sync_out(), CTR()));
            return true;
        }
        default:
            error_ = std::format("the lifter has no case for {}", ppu::spec(in.op).name);
            return false;
        }
    }

    const LiftInput& f_;
    std::set<uint32_t> present_, labels_;
    std::map<uint32_t, const JumpTable*> tables_;
    std::set<unsigned> gprs_, crs_, fprs_, vrs_;
    std::set<std::string> specials_;
    std::string error_;
};

}  // namespace

Lifted lift(const LiftInput& f) { return Emitter(f).run(); }

LiftInput lift_input(const Cfg& cfg, const Function& f) {
    LiftInput in;
    in.start = f.start;
    in.insns.reserve(f.insns.size());
    for (const uint32_t a : f.insns) in.insns.emplace_back(a, cfg.insn(a));
    in.tables = f.tables;
    in.landing_pads = f.landing_pads;
    if (f.runs_off && cfg.functions().contains(f.bound)) in.falls_into = f.bound;
    in.is_function = [&cfg](uint32_t a) { return cfg.functions().contains(a); };
    return in;
}

}  // namespace qilin

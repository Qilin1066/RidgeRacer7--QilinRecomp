#include "lift.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <format>
#include <map>
#include <set>
#include <unordered_map>

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
        case Op::mtfsfi: case Op::mtfsb0: case Op::mtfsb1: case Op::mcrfs:
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

using Slots = std::bitset<kRegisterSlots>;

// ---- what a callee reads and writes (register_effects) ----
// Each function's own part, read off its lifted text as the lifter's dataflow reads it (assigned and used below): per
// instruction, its pieces of text in order, each with what it reads and assigns, and its direct calls (and tail calls);
// and whether anything in it reaches everything (a call through a register, a system call, a hook, a throw, a landing
// pad). A call to a function gives its effects at that point; a return and a trap give nothing.
struct FactEvent {
    Slots uses, defs;
    bool call = false;
    uint32_t callee = 0;
};
struct FunctionFacts {
    uint32_t start = 0;
    bool everything = false;
    std::vector<std::vector<FactEvent>> events;
    std::vector<std::vector<size_t>> succ;
    std::vector<uint32_t> callees;
};

class Emitter {
public:
    explicit Emitter(const LiftInput& f) : f_(f) {
        for (const auto& [a, in] : f.insns) present_.insert(a);
        for (const JumpTable& t : f.tables) tables_[t.at] = &t;
    }

    Lifted run() {
        Lifted out;
        if (!prepare(out)) return out;
        if (!flow()) {
            out.why = error_;
            out.why_at = f_.start;
            return out;
        }
        std::string body;
        for (size_t k = 0; k < f_.insns.size(); ++k) {
            if (labels_.contains(f_.insns[k].first)) body += std::format("L_{:08X}:\n", f_.insns[k].first);
            body += text_[k];
        }
        body = expand(body);
        out.code = f_.landing_pads.empty() ? std::format("void {}(Cpu& c) {{\n{}{}}}\n", fn_name(f_.start), prologue(), body) : entered_at_pads(body);
        out.ok = true;
        out.catches = !f_.landing_pads.empty();
        out.eh_returns = eh_returns_;
        out.narrowed = narrowed_;
        return out;
    }

    // What the function itself does to the registers, for register_effects: false if it doesn't lift.
    bool facts(FunctionFacts& out) {
        Lifted unused;
        if (!prepare(unused) || !cut()) return false;
        out.start = f_.start;
        out.everything = !f_.landing_pads.empty();  // (a pad's body reads every register from the Cpu)
        out.succ = succ_;
        out.events.resize(events_.size());
        Locals mentioned;
        for (size_t k = 0; k < events_.size(); ++k)
            for (const Event& e : events_[k]) {
                FactEvent fe;
                if (e.site < 0) {
                    fe.uses = e.uses;
                    fe.defs = e.defs;
                    mentioned |= e.uses | e.defs;
                } else {
                    if (e.reload) continue;
                    const Site& site = sites_[static_cast<size_t>(e.site)];
                    if (site.reach == Site::kAll && site.kind != Site::kFlush) out.everything = true;
                    if (site.reach != Site::kDirect) continue;
                    fe.call = true;
                    fe.callee = site.callee;
                    out.callees.push_back(site.callee);
                }
                out.events[k].push_back(fe);
            }
        // Every local the function declares must be one its text was seen to read or write: else the reading of the
        // text above has missed something, and the function is taken to read and write everything.
        if ((declared() & ~mentioned).any()) out.everything = true;
        std::ranges::sort(out.callees);
        out.callees.erase(std::ranges::unique(out.callees).begin(), out.callees.end());
        return true;
    }

private:
    // The function's text, with placeholders: false (and why in `out`) if it isn't lifted.
    bool prepare(Lifted& out) {
        for (const auto& [a, in] : f_.insns) {
            std::string why;
            if (!in_slice(in, &why)) {
                out.why = why;
                out.why_at = a;
                return false;
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
        for (const uint32_t pad : f_.landing_pads) {
            if (!present_.contains(pad)) {
                out.why = std::format("a landing pad at {:08X} that the function doesn't reach", pad);
                out.why_at = pad;
                return false;
            }
            labels_.insert(pad);
        }
        // Each instruction's text, with placeholders for what the dataflow below decides: the registers written back
        // and read again at each sync site (@S n@, @L n@), each condition-register bit read (@B..@), and each
        // compare's saved operands (@K..@).
        text_.resize(f_.insns.size());
        defs_.assign(f_.insns.size(), {});
        for (size_t k = 0; k < f_.insns.size(); ++k) {
            const auto& [a, in] = f_.insns[k];
            k_ = k;
            std::string line;
            if (!insn(a, in, line)) {
                out.why = error_;
                out.why_at = a;
                return false;
            }
            if (f_.keep_pc && (line.find("read") != std::string::npos || line.find("write") != std::string::npos))
                line = std::format("    c.pc = 0x{:08X}u;{}\n", a, f_.keep_registers ? " " + site(Site::kFlush) : std::string()) + line;
            const bool last = k + 1 == f_.insns.size();
            const uint32_t next = last ? 0 : f_.insns[k + 1].first;
            if (!terminator(in) && !tables_.contains(a) && (last || next != a + 4)) {
                // Execution continues past this instruction into code that isn't the next one we have.
                if (f_.falls_into == a + 4) {
                    line += std::format("    {} {}(c); return;\n", site(Site::kLeave, Site::kDirect, a + 4), fn_name(a + 4));
                } else {
                    out.why = std::format("execution runs on to {:08X}", a + 4);
                    out.why_at = a;
                    return false;
                }
            }
            text_[k] = std::move(line);
        }
        return true;
    }

    // A function with landing pads: the guest's unwinder "returns" into one of them from further down the stack (the
    // eh_return epilogue below throws GuestUnwind). The function is lifted twice: `fn_X_start` entered at its start,
    // as any function is, and `fn_X_pad`, which reads the registers and goes to a pad through a switch at its top, so
    // that only what the pads reach is compiled in it. `fn_X` hands both to the runtime's run_with_landing_pads,
    // whose try runs the second when an exception is for a pad of this activation's (cpu.hpp). The try is in the
    // runtime, once, and neither body has more than one way in from outside: each compiles as a plain function does.
    std::string entered_at_pads(const std::string& body) const {
        std::string enter, pads;
        for (const uint32_t pad : f_.landing_pads) {
            enter += std::format(" case {}: goto L_{:08X};", hex32(pad), pad);
            pads += std::format("{}{}", pads.empty() ? "" : ", ", hex32(pad));
        }
        const std::string name = fn_name(f_.start);
        return std::format("static void {0}_start(Cpu& c) {{\n{1}{3}}}\n"
                           "static void {0}_pad(Cpu& c, uint32_t pad) {{\n{1}    switch (pad) {{{2} default: c.hooks->trap(c, {5}, \"not a landing pad\"); return; }}\n{3}}}\n"
                           "void {0}(Cpu& c) {{\n    static constexpr uint32_t kPads[] = {{{4}}};\n    run_with_landing_pads(c, &{0}_start, &{0}_pad, kPads);\n}}\n",
                           name, prologue(), enter, body, pads, hex32(f_.start));
    }

    // The instruction at `a`, if the function has one there.
    const Insn* at(uint32_t a) const {
        const auto it = std::ranges::lower_bound(f_.insns, a, {}, &std::pair<uint32_t, Insn>::first);
        return it != f_.insns.end() && it->first == a ? &it->second : nullptr;
    }

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
        if (memory_) p += "    uint8_t* const m = c.mem;\n";
        for (const unsigned n : gprs_) p += std::format("    uint64_t r{0} = c.r[{0}];\n", n);
        for (const unsigned n : crs_) p += std::format("    uint32_t cr{0} = (c.cr >> {1}) & 15u;\n", n, 28 - 4 * n);
        for (const unsigned n : fprs_) p += std::format("    double f{0} = c.f[{0}];\n", n);
        for (const unsigned n : vrs_) p += std::format("    Vec v{0} = c.v[{0}];\n", n);
        if (specials_.contains("lr")) p += "    uint64_t lr = c.lr;\n";
        if (specials_.contains("ctr")) p += "    uint64_t ctr = c.ctr;\n";
        if (specials_.contains("so")) p += "    uint32_t so = c.xer_so;\n";
        if (specials_.contains("ov")) p += "    uint32_t ov = c.xer_ov;\n";
        if (specials_.contains("ca")) p += "    uint32_t ca = c.xer_ca;\n";
        // A compare's operands, kept for the branches that read its result (bit_text below).
        for (unsigned n = 0; n < 8; ++n) {
            if (saved_int_[n]) p += std::format("    uint64_t k{0}a = 0, k{0}b = 0;\n", n);
            if (saved_float_[n]) p += std::format("    double k{0}x = 0, k{0}y = 0;\n", n);
        }
        for (unsigned n = 0; n < 32; ++n)
            if (saved_bool_[n]) p += std::format("    bool kb{} = false;\n", n);
        for (unsigned n = 0; n < 8; ++n)
            if (lazy_[n] && selector_[n]) p += std::format("    uint32_t kf{} = 0u;\n", n);
        return p;
    }

    // ---- the dataflow: what each sync site writes back, and which compares a branch reads directly ----
    //
    // The locals are written back before anything outside the function can see the state (calls, returns, traps),
    // and read again after a call. A local is written back only where it may differ from the Cpu: where some path
    // from the function's start (whose locals were read from the Cpu), from a call (after which every local is read
    // again) or from a write-back has assigned it. That is exact, not a guess about the callee: a local not written
    // back holds what the Cpu holds already. A sync site's text is a placeholder until this is known.
    //
    // A condition-register bit is read straight from the compare that set it when every definition of it reaching
    // the read is a compare of one kind (signed integers, unsigned integers, floating point) or a CR logical
    // operation: the compare's operands are kept in locals (k7a and k7b; k7x and k7y; kb30) and the branch is
    // `if (k7a == k7b)`, which the compiler makes one compare and jump. Otherwise the bit is read from the field's
    // local. The field's local is set at each compare for the reads that can't fold and the write-backs, unless every
    // read of the field there is (a lazy field): every bit read folds, nothing reads the local itself (mfcr, mcrf, a CR
    // logical operation's own field), and at each write-back where it may have changed the field's reaching
    // definitions are all compares of one kind. Then the compares only keep their operands, and the write-back forms
    // the field from them (compare_s(k7a, k7b, so)): a compare and branch in a loop costs no more than that, where
    // before the field was built on every pass for the one write-back at the loop's exit.
    struct Site {
        enum Kind : uint8_t {
            kLeave,   // the function leaves (a return, a tail call, a trap, a throw): written back, nothing after
            kCall,    // a call: written back, then read again
            kCallIf,  // a call that may not happen
            kFlush,   // written back, and the function goes on (--registers: before each memory access)
        } kind;
        // What runs there: nothing of the guest's (a return; a trap, which doesn't come back), a lifted function by its
        // address, or anything at all (a call through a register, a system call, a hook, a throw).
        enum Reach : uint8_t { kNothing, kDirect, kAll } reach = kAll;
        uint32_t callee = 0;
    };
    // What each condition-register bit's reaching definitions are, as the dataflow carries them.
    enum Fold : uint8_t { kUnreached, kOpaque, kSigned, kUnsigned, kFloat, kBool };
    static constexpr unsigned kFprAt = 32, kVrAt = 64, kCrAt = 96, kLrAt = 104, kCtrAt = 105, kSoAt = 106, kOvAt = 107, kCaAt = 108, kLocals = 109;
    using Locals = std::bitset<kLocals>;
    static_assert(kLocals == kRegisterSlots, "lift.hpp's RegisterEffects number the locals as the dataflow does");
    struct Save {
        Fold kind;        // kSigned, kUnsigned, kFloat (a field's bits) or kBool (one bit)
        unsigned target;  // the field, or for kBool the bit
        std::string a, b;
        std::string field = {};  // a compare's assignment of its field (`cr7 = compare_s(...);`), left out where the field is lazy
    };

    // A sync site's placeholder (its write-back).
    std::string site(Site::Kind kind, Site::Reach reach = Site::kAll, uint32_t callee = 0) {
        sites_.push_back({kind, reach, callee});
        return std::format("@S{}@", sites_.size() - 1);
    }
    // A call's sync site: the write-back before it and what is read again after it. `callee` is the lifted function
    // called, or 0 for anything else (a call through a register, a system call, a hook).
    std::pair<std::string, std::string> call_site(bool conditional, uint32_t callee = 0) {
        std::string out = site(conditional ? Site::kCallIf : Site::kCall, callee ? Site::kDirect : Site::kAll, callee);
        return {std::move(out), std::format("@L{}@", sites_.size() - 1)};
    }
    // Notes the current instruction's definition of condition-register bits, of a kind a later read can fold; the
    // placeholder is where its operands are kept, if any read does fold.
    std::string saves(Fold kind, unsigned target, std::string a, std::string b = {}) {
        if (kind == kBool) defs_[k_][target] = kBool;
        else
            for (unsigned i = 0; i < (kind == kFloat ? 4u : 3u); ++i) defs_[k_][4 * target + i] = kind;  // (an integer compare's SO bit isn't kept)
        saves_.push_back({kind, target, std::move(a), std::move(b)});
        return std::format("@K{}@", saves_.size() - 1);
    }
    // A compare: its field set to `value` (unless the field turns out lazy), and its operands noted as saves() does.
    // The placeholder @F n.field@ counts as an assignment of the field for the dataflow.
    std::string compare_into(Fold kind, unsigned field, const std::string& value, std::string a, std::string b) {
        const std::string keep = saves(kind, field, std::move(a), std::move(b));
        saves_.back().field = std::format("{} = {};", CR(field), value);
        return std::format("@F{}.{}@{}", saves_.size() - 1, field, keep);
    }

    // The local a name in the lifted text is, or -1.
    static int local_index(std::string_view t) {
        const auto number = [&](size_t from, unsigned limit) -> int {
            if (from >= t.size() || t.size() - from > 2 || (t.size() - from == 2 && t[from] == '0')) return -1;
            unsigned v = 0;
            for (size_t i = from; i < t.size(); ++i) {
                if (t[i] < '0' || t[i] > '9') return -1;
                v = v * 10 + static_cast<unsigned>(t[i] - '0');
            }
            return v < limit ? static_cast<int>(v) : -1;
        };
        if (t == "lr") return kLrAt;
        if (t == "ctr") return kCtrAt;
        if (t == "so") return kSoAt;
        if (t == "ov") return kOvAt;
        if (t == "ca") return kCaAt;
        if (t.starts_with("cr")) {
            const int n = number(2, 8);
            return n < 0 ? -1 : static_cast<int>(kCrAt) + n;
        }
        if (t.empty()) return -1;
        const int n = number(1, 32);
        if (n < 0) return -1;
        switch (t[0]) {
        case 'r': return n;
        case 'f': return static_cast<int>(kFprAt) + n;
        case 'v': return static_cast<int>(kVrAt) + n;
        default: return -1;
        }
    }
    // The locals a piece of lifted text assigns: `x = `, `x -= ` and the like, and the carry's out-parameter `, ca)`;
    // and, unless `compares` is false, the fields of the compares' placeholders.
    static Locals assigned(std::string_view s, bool compares = true) {
        Locals out;
        const auto word = [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; };
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '@') {  // a placeholder; a compare's field (@F n.field@) is an assignment
                const size_t end = s.find('@', i + 1);
                if (compares && i + 1 < s.size() && s[i + 1] == 'F' && end != std::string_view::npos) {
                    const size_t dot = s.find('.', i);
                    if (dot != std::string_view::npos && dot + 2 == end) out.set(kCrAt + static_cast<size_t>(s[dot + 1] - '0'));
                }
                i = end == std::string_view::npos ? s.size() : end + 1;
                continue;
            }
            if (!word(s[i])) {
                ++i;
                continue;
            }
            const size_t start = i;
            while (i < s.size() && word(s[i])) ++i;
            if ((start > 0 && (s[start - 1] == '.' || s[start - 1] == ':')) || (s[start] >= '0' && s[start] <= '9')) continue;
            const int n = local_index(s.substr(start, i - start));
            if (n < 0) continue;
            size_t j = i;
            while (j < s.size() && s[j] == ' ') ++j;
            const bool plain = j < s.size() && s[j] == '=' && (j + 1 >= s.size() || s[j + 1] != '=');
            const bool compound = j + 1 < s.size() && std::string_view("-+|&^").find(s[j]) != std::string_view::npos && s[j + 1] == '=';
            const bool out_parameter = n == static_cast<int>(kCaAt) && j < s.size() && s[j] == ')';
            if (plain || compound || out_parameter) out.set(static_cast<size_t>(n));
        }
        return out;
    }

    // The locals a piece of lifted text reads: every name of one that isn't the target of a plain assignment (`x = `)
    // or the carry's out-parameter (`, ca)`); in the placeholders, the bit reads' fields and what the compares read.
    Locals used(std::string_view s) const {
        Locals out;
        const auto word = [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; };
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '@') {
                const size_t end = s.find('@', i + 1);
                if (end == std::string_view::npos) break;
                const std::string body(s.substr(i + 2, end - i - 2));
                switch (s[i + 1]) {
                case 'T': case 'V': out.set(kCrAt + std::stoul(body.substr(body.find('.') + 1)) / 4); break;
                case 'F': out |= used(saves_[std::stoul(body.substr(0, body.find('.')))].field); break;
                case 'K': {
                    const Save& v = saves_[std::stoul(body)];
                    out |= used(v.a) | used(v.b);
                    break;
                }
                default: break;
                }
                i = end + 1;
                continue;
            }
            if (s[i] == '"') {  // a string (a trap's message)
                const size_t end = s.find('"', i + 1);
                i = end == std::string_view::npos ? s.size() : end + 1;
                continue;
            }
            if (s.substr(i, 2) == "//") break;  // a comment, to the end of the line
            if (!word(s[i])) {
                ++i;
                continue;
            }
            const size_t start = i;
            while (i < s.size() && word(s[i])) ++i;
            if ((start > 0 && (s[start - 1] == '.' || s[start - 1] == ':' || (s[start - 1] == '>' && start > 1 && s[start - 2] == '-'))) || (s[start] >= '0' && s[start] <= '9'))
                continue;
            const int n = local_index(s.substr(start, i - start));
            if (n < 0) continue;
            size_t j = i;
            while (j < s.size() && s[j] == ' ') ++j;
            const bool plain = j < s.size() && s[j] == '=' && (j + 1 >= s.size() || s[j + 1] != '=');
            const bool out_parameter = n == static_cast<int>(kCaAt) && j < s.size() && s[j] == ')';  // (add_carry's and sraw's last: assigned())
            if (!plain && !out_parameter) out.set(static_cast<size_t>(n));
        }
        return out;
    }

    // The function's locals.
    Locals declared() const {
        Locals d;
        for (const unsigned n : gprs_) d.set(n);
        for (const unsigned n : fprs_) d.set(kFprAt + n);
        for (const unsigned n : vrs_) d.set(kVrAt + n);
        for (const unsigned n : crs_) d.set(kCrAt + n);
        const std::pair<const char*, unsigned> specials[] = {{"lr", kLrAt}, {"ctr", kCtrAt}, {"so", kSoAt}, {"ov", kOvAt}, {"ca", kCaAt}};
        for (const auto& [name, at] : specials)
            if (specials_.contains(name)) d.set(at);
        return d;
    }

    static Fold join(Fold x, Fold y) { return x == kUnreached ? y : y == kUnreached ? x : x == y ? x : kOpaque; }

    size_t index(uint32_t a) const {
        return static_cast<size_t>(std::ranges::lower_bound(f_.insns, a, {}, &std::pair<uint32_t, Insn>::first) - f_.insns.begin());
    }
    // Successors, and each instruction's text cut at its sync sites (what each piece reads and assigns).
    bool cut() {
        const size_t n = f_.insns.size();
        std::vector<std::vector<size_t>>& succ = succ_;
        succ.assign(n, {});
        std::vector<std::vector<Event>>& events = events_;
        events.assign(n, {});
        std::vector<Locals>& assigns = assigns_;
        assigns.assign(n, {});
        for (size_t k = 0; k < n; ++k) {
            const auto& [a, in] = f_.insns[k];
            if (const auto it = tables_.find(a); it != tables_.end()) {
                for (const uint32_t t : it->second->targets) succ[k].push_back(index(t));
            } else {
                if (!terminator(in) && k + 1 < n && f_.insns[k + 1].first == a + 4) succ[k].push_back(k + 1);
                if ((in.op == Op::b || in.op == Op::bc) && !in.lk() && present_.contains(in.branch_target(a))) succ[k].push_back(index(in.branch_target(a)));
            }
            const std::string_view t = text_[k];
            size_t from = 0;
            for (size_t p = 0; (p = t.find('@', p)) != std::string_view::npos;) {
                const size_t end = t.find('@', p + 1);
                if (end == std::string_view::npos) {
                    error_ = "a placeholder of the lifter's cut short (a fault in the lifter)";
                    return false;
                }
                if (t[p + 1] == 'S' || t[p + 1] == 'L') {
                    events[k].push_back({-1, assigned(t.substr(from, p - from)), used(t.substr(from, p - from))});
                    events[k].push_back({std::stoi(std::string(t.substr(p + 2, end - p - 2))), {}, {}, t[p + 1] == 'L'});
                    from = end + 1;
                }
                p = end + 1;
            }
            events[k].push_back({-1, assigned(t.substr(from)), used(t.substr(from))});
            assigns[k] = assigned(t);
        }
        return true;
    }

    // The callee's effects at a call site, where they are known (register_effects): what is written back before it
    // (what the callee may read or write: what it may write and doesn't must be in the Cpu too, since the local is
    // read again after it) and what is read again after it (what it may write; with SO, every CR field, whose lazy
    // forms take SO from its local). Elsewhere, everything.
    void site_masks() {
        Locals all;
        all.set();
        site_write_.assign(sites_.size(), all);
        site_read_.assign(sites_.size(), all);
        if (!f_.effects || f_.keep_registers) return;
        Locals fields;
        for (unsigned n = 0; n < 8; ++n) fields.set(kCrAt + n);
        for (size_t i = 0; i < sites_.size(); ++i) {
            const Site& site = sites_[i];
            if ((site.kind != Site::kCall && site.kind != Site::kCallIf) || site.reach != Site::kDirect) continue;
            const auto it = f_.effects->find(site.callee);
            if (it == f_.effects->end()) continue;
            const Locals reads = it->second.reads;
            Locals writes = it->second.writes;
            if (writes[kSoAt]) writes |= fields;
            site_write_[i] = reads | writes;
            site_read_[i] = writes;
            narrowed_ += (reads | writes) != all;
        }
    }

    bool flow() {
        const size_t n = f_.insns.size();
        if (!cut()) return false;
        site_masks();
        const std::vector<std::vector<size_t>>& succ = succ_;
        const std::vector<std::vector<Event>>& events = events_;
        const std::vector<Locals>& assigns = assigns_;
        // What each instruction reads again after its calls.
        std::vector<Locals> reloads(n);
        for (size_t k = 0; k < n; ++k)
            for (const Event& e : events[k])
                if (e.reload) reloads[k] |= site_read_[static_cast<size_t>(e.site)];
        // Forward to a fixed point: the locals that may differ from the Cpu, and each CR bit's reaching definitions.
        std::vector<Locals> dirty_in(n);
        std::vector<std::array<Fold, 32>> fold_in(n);
        for (auto& f : fold_in) f.fill(kUnreached);
        std::vector<bool> reached(n);
        const auto enter = [&](size_t k) {  // the start, and a landing pad (entered with every local read from the Cpu)
            reached[k] = true;
            fold_in[k].fill(kOpaque);
        };
        if (n) enter(0);
        for (const uint32_t pad : f_.landing_pads) enter(index(pad));
        site_dirty_.assign(sites_.size(), {});
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t k = 0; k < n; ++k) {
                if (!reached[k]) continue;
                Locals cur = dirty_in[k];
                for (const Event& e : events[k]) {
                    if (e.site < 0) {
                        cur |= e.defs;
                        continue;
                    }
                    if (e.reload) continue;
                    const size_t site = static_cast<size_t>(e.site);
                    site_dirty_[site] |= cur & site_write_[site];
                    // After a call: what was written back is in the Cpu, what is read again is the Cpu's; the rest
                    // (neither read nor written by the callee) is as it was. A call that may not happen changes nothing.
                    const Site::Kind kind = sites_[site].kind;
                    if (kind == Site::kCall || kind == Site::kFlush) cur &= ~site_write_[site];
                }
                std::array<Fold, 32> fold = fold_in[k];
                for (unsigned field = 0; field < 8; ++field)
                    if (assigns[k][kCrAt + field])
                        for (unsigned i = 0; i < 4; ++i) fold[4 * field + i] = kOpaque;
                for (unsigned bit = 0; bit < 32; ++bit)
                    if (defs_[k][bit] != kUnreached) fold[bit] = defs_[k][bit];
                for (unsigned field = 0; field < 8; ++field)
                    if (reloads[k][kCrAt + field])
                        for (unsigned i = 0; i < 4; ++i) fold[4 * field + i] = kOpaque;
                for (const size_t s : succ[k]) {
                    const Locals merged = dirty_in[s] | cur;
                    std::array<Fold, 32> joined{};
                    for (unsigned bit = 0; bit < 32; ++bit) joined[bit] = join(fold_in[s][bit], fold[bit]);
                    if (!reached[s] || merged != dirty_in[s] || joined != fold_in[s]) {
                        reached[s] = true;
                        dirty_in[s] = merged;
                        fold_in[s] = joined;
                        changed = true;
                    }
                }
            }
        }
        fold_ = std::move(fold_in);
        lazy_fields(assigns);
        const Locals all = declared();
        for (const Locals& d : site_dirty_)
            if ((d & ~all).any()) {
                error_ = "a local to be written back that the function doesn't declare (a fault in the lifter)";
                return false;
            }
        return true;
    }

    // Does a bit read fold (bit_text below)? Unreached code doesn't count against a lazy field.
    static bool folds(Fold kind, unsigned bit) {
        return kind == kUnreached || kind == kFloat || kind == kBool || ((kind == kSigned || kind == kUnsigned) && (bit & 3) != 3);
    }
    // The kind a field's local can be formed from at a write-back, or kOpaque: all its bits' reaching definitions
    // compares of one kind (an integer compare's SO bit is the `so` it was made with: see lazy_fields).
    static Fold field_kind(const std::array<Fold, 32>& fold, unsigned field) {
        const Fold k = fold[4 * field];
        if (k == kUnreached) return fold[4 * field + 1] == kUnreached && fold[4 * field + 2] == kUnreached ? kUnreached : kOpaque;
        if (k != kSigned && k != kUnsigned && k != kFloat) return kOpaque;
        for (unsigned i = 1; i < (k == kFloat ? 4u : 3u); ++i)
            if (fold[4 * field + i] != k) return kOpaque;
        return k;
    }
    // Which fields are lazy (see "the dataflow"), and how each write-back forms them. Where compares and the field as
    // the Cpu held it (at the start, after a call) both reach a write-back, or compares of different kinds, a small
    // local says which (kf7: 0 the field's local, else the kind of the last compare); the compiler sees it constant
    // on each path, so it costs nothing on them.
    static unsigned kind_code(Fold kind) { return kind == kSigned ? 1u : kind == kUnsigned ? 2u : 3u; }
    void lazy_fields(const std::vector<Locals>& assigns) {
        bool has_compare[8] = {}, int_compare[8] = {};
        for (const Save& v : saves_)
            if (!v.field.empty()) {
                has_compare[v.target] = true;
                int_compare[v.target] |= v.kind != kFloat;
                kinds_[v.target] |= 1u << kind_code(v.kind);
            }
        for (unsigned f = 0; f < 8; ++f) lazy_[f] = has_compare[f];
        // An integer compare's SO bit is XER's SO when it ran: the write-back's `so` is that one unless the function
        // sets SO itself (mtxer) somewhere.
        bool sets_so = false;
        for (const Locals& a : assigns) sets_so |= a[kSoAt];
        for (unsigned f = 0; f < 8; ++f)
            if (sets_so && int_compare[f]) lazy_[f] = false;
        site_kind_.assign(sites_.size(), {});
        const auto word = [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; };
        for (size_t k = 0; k < text_.size(); ++k) {
            const std::string_view t = text_[k];
            // Any other assignment of the field (mtcrf, mcrf, a CR logical operation, a vector compare's CR6).
            const Locals literal = assigned(t, false);
            for (unsigned f = 0; f < 8; ++f)
                if (literal[kCrAt + f]) lazy_[f] = false;
            // Bit reads that don't fold, and the field's local read by name (outside the placeholders, as anything
            // but the target of a plain assignment).
            for (size_t i = 0; i < t.size(); ++i) {
                if (t[i] == '@') {
                    const size_t end = t.find('@', i + 1);
                    if (end == std::string_view::npos) break;
                    if (t[i + 1] == 'T' || t[i + 1] == 'V') {
                        const std::string_view body = t.substr(i + 2, end - i - 2);
                        const unsigned bit = static_cast<unsigned>(std::stoul(std::string(body.substr(body.find('.') + 1))));
                        if (!folds(fold_[k][bit], bit)) lazy_[bit >> 2] = false;
                    }
                    i = end;
                    continue;
                }
                if (t.substr(i, 2) != "cr" || i + 2 >= t.size() || t[i + 2] < '0' || t[i + 2] > '7') continue;
                if ((i > 0 && (word(t[i - 1]) || t[i - 1] == '.' || t[i - 1] == ':')) || (i + 3 < t.size() && word(t[i + 3]))) continue;
                size_t j = i + 3;
                while (j < t.size() && t[j] == ' ') ++j;
                const bool assignment = j < t.size() && t[j] == '=' && (j + 1 >= t.size() || t[j + 1] != '=');
                if (!assignment) lazy_[t[i + 2] - '0'] = false;
            }
            // The write-backs: the fields' reaching definitions there (the instruction's own, if it assigned the
            // field before the site).
            std::array<Fold, 32> after = fold_[k];
            for (unsigned field = 0; field < 8; ++field)
                if (assigns[k][kCrAt + field])
                    for (unsigned i = 0; i < 4; ++i) after[4 * field + i] = kOpaque;
            for (unsigned bit = 0; bit < 32; ++bit)
                if (defs_[k][bit] != kUnreached) after[bit] = defs_[k][bit];
            Locals before;
            for (const Event& e : events_[k]) {
                if (e.site < 0) {
                    before |= e.defs;
                    continue;
                }
                if (e.reload) continue;
                const size_t site = static_cast<size_t>(e.site);
                for (unsigned field = 0; field < 8; ++field) {
                    if (!site_dirty_[site][kCrAt + field]) continue;
                    const Fold kind = field_kind(before[kCrAt + field] ? after : fold_[k], field);
                    site_kind_[site][field] = kind;
                    if (kind == kOpaque) selector_[field] = true;
                }
            }
        }
        for (unsigned f = 0; f < 8; ++f)
            if (lazy_[f])
                for (const Save& v : saves_)
                    if (!v.field.empty() && v.target == f) (v.kind == kFloat ? saved_float_ : saved_int_)[f] = true;
    }

    // A read of condition-register bit `bit` at the current instruction: as a truth value (`truth`) or as 0 or 1.
    std::string cr_read(unsigned bit, bool truth) {
        CR(bit >> 2);
        return std::format("@{}{}.{}@", truth ? 'T' : 'V', k_, bit);
    }
    // A bit read, now that the dataflow is done.
    std::string bit_text(size_t k, unsigned bit, bool truth) {
        const unsigned field = bit >> 2, which = bit & 3;
        const Fold kind = fold_[k][bit];
        std::string e;
        switch (kind) {
        case kSigned: case kUnsigned:
            if (which == 3) break;
            saved_int_[field] = true;
            e = which == 2 ? std::format("k{0}a == k{0}b", field)
                : kind == kSigned ? std::format("static_cast<int64_t>(k{0}a) {1} static_cast<int64_t>(k{0}b)", field, which == 0 ? '<' : '>')
                                  : std::format("k{0}a {1} k{0}b", field, which == 0 ? '<' : '>');
            break;
        case kFloat:
            saved_float_[field] = true;
            e = which == 3 ? std::format("k{0}x != k{0}x || k{0}y != k{0}y", field)
                : which == 2 ? std::format("k{0}x == k{0}y", field)
                             : std::format("k{0}x {1} k{0}y", field, which == 0 ? '<' : '>');
            break;
        case kBool:
            saved_bool_[bit] = true;
            e = std::format("kb{}", bit);
            break;
        default: break;
        }
        if (e.empty()) return truth ? std::format("(cr{} & {}u)", field, 8u >> which) : std::format("((cr{} >> {}) & 1u)", field, 3 - which);
        return truth ? "(" + e + ")" : "static_cast<uint32_t>(" + e + ")";
    }

    // The placeholders, filled in.
    std::string expand(const std::string& code) {
        const auto write_back = [&](size_t site) {
            const Locals& d = site_dirty_[site];
            std::string s;
            for (const unsigned n : gprs_)
                if (d[n]) s += std::format("c.r[{0}] = r{0}; ", n);
            for (const unsigned n : fprs_)
                if (d[kFprAt + n]) s += std::format("c.f[{0}] = f{0}; ", n);
            for (const unsigned n : vrs_)
                if (d[kVrAt + n]) s += std::format("c.v[{0}] = v{0}; ", n);
            uint32_t keep = 0xFFFFFFFFu;
            std::string merged;
            for (const unsigned n : crs_)
                if (d[kCrAt + n]) {
                    keep &= ~(15u << (28 - 4 * n));
                    std::string value = std::format("cr{}", n);
                    if (lazy_[n]) {
                        const auto formed = [&](Fold kind) {
                            return kind == kSigned ? std::format("compare_s(static_cast<int64_t>(k{0}a), static_cast<int64_t>(k{0}b), so)", n)
                                 : kind == kUnsigned ? std::format("compare_u(k{0}a, k{0}b, so)", n)
                                                     : std::format("fp::fcompare(k{0}x, k{0}y)", n);
                        };
                        const Fold kind = site_kind_[site][n];
                        if (kind == kSigned || kind == kUnsigned || kind == kFloat) value = formed(kind);
                        else if (kind == kOpaque)
                            for (const Fold k : {kFloat, kUnsigned, kSigned})
                                if (kinds_[n] & (1u << kind_code(k))) value = std::format("(kf{} == {}u ? {} : {})", n, kind_code(k), formed(k), value);
                    }
                    merged += std::format("{}({} << {})", merged.empty() ? "" : " | ", value, 28 - 4 * n);
                }
            if (!merged.empty()) s += keep ? std::format("c.cr = (c.cr & {}) | {}; ", hex32(keep), merged) : std::format("c.cr = {}; ", merged);
            const std::pair<const char*, unsigned> specials[] = {{"lr", kLrAt}, {"ctr", kCtrAt}};
            for (const auto& [name, at] : specials)
                if (d[at]) s += std::format("c.{0} = {0}; ", name);
            const std::pair<const char*, unsigned> xer[] = {{"so", kSoAt}, {"ov", kOvAt}, {"ca", kCaAt}};
            for (const auto& [name, at] : xer)
                if (d[at]) s += std::format("c.xer_{0} = {0}; ", name);
            return s;
        };
        const auto read_again = [&](size_t site) {
            const Locals& d = site_read_[site];
            std::string s;
            for (const unsigned n : gprs_)
                if (d[n]) s += std::format("r{0} = c.r[{0}]; ", n);
            for (const unsigned n : fprs_)
                if (d[kFprAt + n]) s += std::format("f{0} = c.f[{0}]; ", n);
            for (const unsigned n : vrs_)
                if (d[kVrAt + n]) s += std::format("v{0} = c.v[{0}]; ", n);
            for (const unsigned n : crs_)
                if (d[kCrAt + n]) s += std::format("cr{0} = (c.cr >> {1}) & 15u; ", n, 28 - 4 * n);
            for (unsigned n = 0; n < 8; ++n)
                if (lazy_[n] && selector_[n] && d[kCrAt + n]) s += std::format("kf{} = 0u; ", n);
            const std::pair<const char*, unsigned> specials[] = {{"lr", kLrAt}, {"ctr", kCtrAt}};
            for (const auto& [name, at] : specials)
                if (specials_.contains(name) && d[at]) s += std::format("{0} = c.{0}; ", name);
            const std::pair<const char*, unsigned> xer[] = {{"so", kSoAt}, {"ov", kOvAt}, {"ca", kCaAt}};
            for (const auto& [name, at] : xer)
                if (specials_.contains(name) && d[at]) s += std::format("{0} = c.xer_{0}; ", name);
            return s;
        };
        // Two passes: the bit reads decide which compares keep their operands.
        const auto pass = [&](const std::string& in, bool keeps) {
            std::string out;
            out.reserve(in.size() + in.size() / 2);
            size_t from = 0;
            for (size_t p; (p = in.find('@', from)) != std::string::npos;) {
                const size_t end = in.find('@', p + 1);
                const char kind = in[p + 1];
                const std::string body = in.substr(p + 2, end - p - 2);
                out.append(in, from, p - from);
                from = end + 1;
                if (keeps != (kind == 'K')) {
                    out.append(in, p, end + 1 - p);
                    continue;
                }
                switch (kind) {
                case 'S': out += write_back(std::stoul(body)); break;
                case 'F': {
                    const Save& v = saves_[std::stoul(body.substr(0, body.find('.')))];
                    if (!lazy_[v.target]) out += " " + v.field;
                    else if (selector_[v.target]) out += std::format(" kf{} = {}u;", v.target, kind_code(v.kind));
                    break;
                }
                case 'L': out += read_again(std::stoul(body)); break;
                case 'T': case 'V': {
                    const size_t dot = body.find('.');
                    out += bit_text(std::stoul(body.substr(0, dot)), static_cast<unsigned>(std::stoul(body.substr(dot + 1))), kind == 'T');
                    break;
                }
                case 'K': {
                    const Save& s = saves_[std::stoul(body)];
                    if (s.kind == kBool) {
                        if (saved_bool_[s.target]) out += std::format(" kb{} = {};", s.target, s.a);
                    } else if (s.kind == kFloat) {
                        if (saved_float_[s.target]) out += std::format(" k{0}x = {1}; k{0}y = {2};", s.target, s.a, s.b);
                    } else if (saved_int_[s.target]) {
                        out += std::format(" k{0}a = static_cast<uint64_t>({1}); k{0}b = static_cast<uint64_t>({2});", s.target, s.a, s.b);
                    }
                    break;
                }
                default: break;
                }
            }
            out.append(in, from);
            return out;
        };
        return pass(pass(code, false), true);
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
            const std::string bit = cr_read(bi, true);
            parts.push_back((bo & 8) ? bit : "!" + bit);
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
        out = std::format("{} {}(c); return;", site(Site::kLeave, Site::kDirect, target), fn_name(target));
        return true;
    }

    // One bit of the condition register, as 0 or 1.
    std::string cr_bit(unsigned bit) { return cr_read(bit, false); }
    // The guest's memory as lifted code addresses it: a local copy of the base, which a store can't be taken to change.
    std::string mem() {
        memory_ = true;
        return "m";
    }

    bool insn(uint32_t pc, const Insn& in, std::string& out) {
        const unsigned d = in.rd(), a = in.ra(), b = in.rb();
        const bool record_form = (ppu::spec(in.op).flags & ppu::kRc) && in.rc();
        const auto line = [&](std::string s) { out = "    " + s + "\n"; };
        // The result register, then the record form on it.
        const auto result = [&](unsigned dst, std::string expr, bool record = false) {
            std::string s = std::format("{} = {};", R(dst), expr);
            if (record_form || record) {
                s += compare_into(kSigned, 0, std::format("record(r{}, {})", dst, SO()), std::format("r{}", dst), "0");
            }
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
                                 fill(wrap, std::format("{}({}, static_cast<uint32_t>(ea))", read, mem())), R(a)));
            } else {
                const std::string value = std::format("{}({}, {})", read, mem(), addr(ea));
                line(std::format("{} = {};", R(d), fill(wrap, value)));
            }
        };
        const auto store = [&](const char* write, const std::string& ea, bool update, const char* wrap = "$") {
            const std::string value = fill(wrap, R(d));
            if (update) line(std::format("{{ const uint64_t ea = {}; {}({}, static_cast<uint32_t>(ea), {}); {} = ea; }}", ea, write, mem(), value, R(a)));
            else line(std::format("{}({}, {}, {});", write, mem(), addr(ea), value));
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
            line(std::format("{{ const uint32_t bit = ({}) & 1u; {} = ({} & {}u) | (bit << {});{} }}", fill(format, x, y), field, field, 15u & ~(1u << shift), shift,
                             saves(kBool, d, "bit != 0u")));
        };
        const auto trap = [&](const std::string& x, const std::string& y) {
            const std::string leave = site(Site::kLeave, Site::kNothing);  // (a trap doesn't come back)
            if (d == 31) line(std::format("{} c.hooks->trap(c, {}, \"trap\"); return;", leave, hex32(pc)));
            else line(std::format("if (trap_on({}u, {}, {})) {{ {} c.hooks->trap(c, {}, \"trap\"); return; }}", d, x, y, leave, hex32(pc)));
        };
        // Floating-point loads, stores and operations: the helpers are cpu.hpp's fp:: functions.
        const auto fload = [&](const char* read, const std::string& ea, bool update, const char* wrap) {
            if (update) line(std::format("{{ const uint64_t ea = {}; {} = {}; {} = ea; }}", ea, F(d), fill(wrap, std::format("{}({}, static_cast<uint32_t>(ea))", read, mem())), R(a)));
            else line(std::format("{} = {};", F(d), fill(wrap, std::format("{}({}, {})", read, mem(), addr(ea)))));
        };
        const auto fstore = [&](const char* write, const std::string& ea, bool update, const char* wrap) {
            if (update) line(std::format("{{ const uint64_t ea = {}; {}({}, static_cast<uint32_t>(ea), {}); {} = ea; }}", ea, write, mem(), fill(wrap, F(d)), R(a)));
            else line(std::format("{}({}, {}, {});", write, mem(), addr(ea), fill(wrap, F(d))));
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
        case Op::add:
            if (d == 1 && (a == 1) != (b == 1)) {
                // The stack pointer moved by a register: only __builtin_eh_return's epilogue does this, and only as
                // `add r1,r1,rX` straight before the return. It is the guest's unwinder installing a landing pad's
                // context: LR is the pad and rX how much further up the stack its frame is; rX is 0 when the function
                // returns normally. Anything else is a shape this lifter doesn't know, and the function isn't lifted.
                const Insn* next = at(pc + 4);
                if (record_form || !next || next->op != Op::bclr || next->lk() || (next->rd() & 0x14) != 0x14) {
                    error_ = "the stack pointer moved by a register, not as __builtin_eh_return's epilogue (add r1,r1,rX; blr)";
                    return false;
                }
                const std::string by = R(a == 1 ? b : a);
                LR();  // (synced for eh_return, which reads the pad from it)
                ++eh_returns_;
                line(std::format("{0} = {0} + {1}; if ({1} != 0ull) {{ {2} eh_return(c); }}", R(1), by, site(Site::kLeave)));
                return true;
            }
            result(d, std::format("{} + {}", R(a), R(b)));
            return true;
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
        case Op::cmp: case Op::cmpi: case Op::cmpl: case Op::cmpli: {
            const bool is_signed = in.op == Op::cmp || in.op == Op::cmpi;
            const std::string x = in.l() ? (is_signed ? s64(a) : R(a)) : (is_signed ? s32(a) : u32(a));
            const std::string y = in.op == Op::cmpi ? std::format("int64_t{{{}}}", in.simm())
                                : in.op == Op::cmpli ? std::format("{}u", in.uimm())
                                : in.l() ? (is_signed ? s64(b) : R(b)) : (is_signed ? s32(b) : u32(b));
            const unsigned field = in.crfd();
            line(compare_into(is_signed ? kSigned : kUnsigned, field, std::format("compare_{}({}, {}, {})", is_signed ? 's' : 'u', x, y, SO()), x, y));
            return true;
        }

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
        // The FPSCR (the C library's fegetenv and fesetenv): mffs reads it into the low word, the high word 0 (*undefined in
        // the architecture; a stated choice*); mtfsf writes fields of it (cpu.hpp), and calls the hook when that leaves a
        // mode this runtime doesn't provide.
        case Op::mffs: line(std::format("{} = std::bit_cast<double>(uint64_t{{c.fpscr}});", F(d))); return true;
        case Op::mtfsf:
        {
            const auto [out_s, in_s] = call_site(true);
            line(std::format("c.fpscr = fpscr_fields(c.fpscr, {}u, {}); if (c.fpscr & 0xFFu) {{ {} c.hooks->fpscr_mode(c, {}); {} }}", in.fm(), F(b), out_s, hex32(pc), in_s));
        }
            return true;
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
            line(compare_into(kFloat, in.crfd(), std::format("fp::fcompare({}, {})", F(a), F(b)), F(a), F(b)));
            return true;

        // ---------------------------------------------------------------- VMX (vmx.hpp)
        case Op::lvx: line(std::format("{} = vmx::lvx({}, {});", V(d), mem(), addr(ea_x()))); return true;
        case Op::stvx: line(std::format("vmx::stvx({}, {}, {});", mem(), addr(ea_x()), V(d))); return true;
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

        // ---------------------------------------------------------------- ordering: the host's fences
        // Another thread (the RSX's above all: commands written, then the put pointer moved) must see the game's memory
        // accesses in the order these promise, so they are fences, not nothing: the compiler may otherwise move the
        // game's stores past them, and an ARM host's memory reorders them too. sync: a full fence; lwsync and eieio
        // (which orders stores, as the game uses it): acquire and release, a compiler barrier on x86; isync: acquire.
        case Op::sync:
            line(in.ls() == 1 ? "std::atomic_thread_fence(std::memory_order_acq_rel);  // lwsync" : "std::atomic_thread_fence(std::memory_order_seq_cst);  // sync");
            return true;
        case Op::eieio: line("std::atomic_thread_fence(std::memory_order_acq_rel);  // eieio"); return true;
        case Op::isync: line("std::atomic_thread_fence(std::memory_order_acquire);  // isync"); return true;

        // ---------------------------------------------------------------- nothing to do on a host
        case Op::dcbt: case Op::dcbtst: case Op::dcbst: case Op::dcbf: case Op::icbi:
            line(std::format("// {}", ppu::spec(in.op).name));
            return true;

        // ---------------------------------------------------------------- traps and system calls
        case Op::tw: trap(s32(a), s32(b)); return true;
        case Op::twi: trap(s32(a), std::format("{}", in.simm())); return true;
        case Op::td: trap(s64(a), s64(b)); return true;
        case Op::tdi: trap(s64(a), std::format("{}", in.simm())); return true;
        case Op::sc: {
            const auto [out_s, in_s] = call_site(false);
            line(std::format("{} c.hooks->system_call(c, {}); {}", out_s, hex32(pc), in_s));
            return true;
        }
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
                const auto [out_s, in_s] = call_site(false, t);
                line(std::format("{} = {}; {} {}(c); {}", LR(), hex64(pc + 4), out_s, fn_name(t), in_s));
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
                const auto [out_s, in_s] = call_site(cond != "true", t);
                line(cond == "true" ? std::format("{}{} = {}; {} {}(c); {}", pre, LR(), hex64(pc + 4), out_s, fn_name(t), in_s)
                                    : std::format("{}{} = {}; if ({}) {{ {} {}(c); {} }}", pre, LR(), hex64(pc + 4), cond, out_s, fn_name(t), in_s));
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
                const auto [out_s, in_s] = call_site(cond != "true");
                line(cond == "true" ? std::format("{}{{ const uint64_t to = {}; {} = {}; {} c.hooks->call(c, static_cast<uint32_t>(to) & ~3u); {} }}", pre, LR(), LR(),
                                                  hex64(pc + 4), out_s, in_s)
                                    : std::format("{}{{ const uint64_t to = {}; {} = {}; if ({}) {{ {} c.hooks->call(c, static_cast<uint32_t>(to) & ~3u); {} }} }}", pre,
                                                  LR(), LR(), hex64(pc + 4), cond, out_s, in_s));
                return true;
            }
            const std::string leave = site(Site::kLeave, Site::kNothing);
            line(cond == "true" ? std::format("{}{} return;", pre, leave) : std::format("{}if ({}) {{ {} return; }}", pre, cond, leave));
            return true;
        }
        case Op::bcctr: {
            std::string pre;
            const std::string cond = condition(in, pre);
            if (in.lk()) {
                const auto [out_s, in_s] = call_site(cond != "true");
                line(cond == "true" ? std::format("{}{} = {}; {} c.hooks->call(c, static_cast<uint32_t>({}) & ~3u); {}", pre, LR(), hex64(pc + 4), out_s, CTR(), in_s)
                                    : std::format("{}{} = {}; if ({}) {{ {} c.hooks->call(c, static_cast<uint32_t>({}) & ~3u); {} }}", pre, LR(), hex64(pc + 4), cond,
                                                  out_s, CTR(), in_s));
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
                s += std::format(" default: {} c.hooks->trap(c, {}, \"a switch jumped outside its table\"); return; }}", site(Site::kLeave, Site::kNothing), hex32(pc));
                line(s);
                return true;
            }
            const std::string leave = site(Site::kLeave);
            line(cond == "true" ? std::format("{}{} c.hooks->call(c, static_cast<uint32_t>({}) & ~3u); return;", pre, leave, CTR())
                                : std::format("{}if ({}) {{ {} c.hooks->call(c, static_cast<uint32_t>({}) & ~3u); return; }}", pre, cond, leave, CTR()));
            return true;
        }
        default:
            error_ = std::format("the lifter has no case for {}", ppu::spec(in.op).name);
            return false;
        }
    }

    const LiftInput& f_;
    std::vector<std::string> text_;               // each instruction's text, with placeholders
    std::vector<std::array<Fold, 32>> defs_;      // each instruction's foldable definitions of CR bits (kUnreached: none)
    std::vector<std::array<Fold, 32>> fold_;      // each instruction's CR bits on entry, from the dataflow
    std::vector<Site> sites_;
    std::vector<Locals> site_dirty_;              // what each sync site writes back
    std::vector<Save> saves_;
    bool saved_int_[8] = {}, saved_float_[8] = {}, saved_bool_[32] = {};
    bool lazy_[8] = {};                            // fields formed only where written back (lazy_fields)
    bool selector_[8] = {};                        // ... with a local (kf7) saying from what, at some write-back
    unsigned kinds_[8] = {};                       // the kinds of compare into each field (1 << kind_code)
    std::vector<std::array<Fold, 8>> site_kind_;  // at each sync site, each field's reaching compares' kind
    struct Event {
        int site;            // or -1: a piece of text, which assigns `defs` and reads `uses`
        Locals defs, uses;
        bool reload = false;  // the site's read again (@L), not its write-back (@S)
    };
    std::vector<std::vector<Event>> events_;      // each instruction's text cut at its sync sites
    std::vector<std::vector<size_t>> succ_;       // each instruction's successors
    std::vector<Locals> assigns_;                 // what each instruction assigns
    std::vector<Locals> site_write_, site_read_;  // what each sync site writes back (of what has changed) and reads again
    unsigned narrowed_ = 0;                       // calls whose sync the callee's effects narrowed
    bool memory_ = false;
    size_t k_ = 0;                                // the instruction being emitted
    std::set<uint32_t> present_, labels_;
    std::map<uint32_t, const JumpTable*> tables_;
    std::set<unsigned> gprs_, crs_, fprs_, vrs_;
    std::set<std::string> specials_;
    std::string error_;
    unsigned eh_returns_ = 0;
};

}  // namespace

Lifted lift(const LiftInput& f) { return Emitter(f).run(); }

// The effects, from each function's own part (Emitter::facts) and its callees' effects, to a fixed point: least, so
// that a recursion adds only what its other paths read or write. What a function may write: all it assigns, and all
// its callees may write. What it may read before writing: a backward pass over its instructions, from its returns,
// where each piece of text reads what it uses after killing what it assigns, and each call reads what the callee may
// read (a callee's writes may not happen, so they kill nothing).
EffectsTable register_effects(const std::vector<LiftInput>& functions) {
    std::vector<FunctionFacts> facts;
    facts.reserve(functions.size());
    for (const LiftInput& f : functions) {
        FunctionFacts one;
        if (Emitter(f).facts(one)) facts.push_back(std::move(one));
    }
    EffectsTable table;
    std::unordered_map<uint32_t, size_t> at;
    for (size_t i = 0; i < facts.size(); ++i) {
        table[facts[i].start] = {};
        at[facts[i].start] = i;
    }
    Slots all;
    all.set();
    std::unordered_map<uint32_t, std::vector<size_t>> callers;
    for (size_t i = 0; i < facts.size(); ++i)
        for (const uint32_t callee : facts[i].callees) callers[callee].push_back(i);
    const auto effects_of = [&](const FunctionFacts& f) {
        if (f.everything) return RegisterEffects{all, all};
        RegisterEffects e;
        for (const auto& events : f.events)
            for (const FactEvent& ev : events) {
                if (!ev.call) {
                    e.writes |= ev.defs;
                    continue;
                }
                const auto it = table.find(ev.callee);
                if (it == table.end()) return RegisterEffects{all, all};  // not lifted, or not one of the program's
                e.writes |= it->second.writes;
            }
        const size_t n = f.events.size();
        std::vector<Slots> live_in(n);
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t k = n; k-- > 0;) {
                Slots live;
                for (const size_t s : f.succ[k]) live |= live_in[s];
                for (size_t i = f.events[k].size(); i-- > 0;) {
                    const FactEvent& ev = f.events[k][i];
                    if (ev.call) live |= table.at(ev.callee).reads;
                    else live = (live & ~ev.defs) | ev.uses;
                }
                if (live != live_in[k]) {
                    live_in[k] = live;
                    changed = true;
                }
            }
        }
        if (n) e.reads = live_in[0];
        return e;
    };
    std::vector<size_t> work(facts.size());
    std::vector<bool> queued(facts.size(), true);
    for (size_t i = 0; i < facts.size(); ++i) work[i] = facts.size() - 1 - i;
    while (!work.empty()) {
        const size_t i = work.back();
        work.pop_back();
        queued[i] = false;
        const RegisterEffects e = effects_of(facts[i]);
        RegisterEffects& old = table[facts[i].start];
        if (e.reads == old.reads && e.writes == old.writes) continue;
        old = e;
        if (const auto it = callers.find(facts[i].start); it != callers.end())
            for (const size_t c : it->second)
                if (!queued[c]) {
                    queued[c] = true;
                    work.push_back(c);
                }
    }
    return table;
}

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

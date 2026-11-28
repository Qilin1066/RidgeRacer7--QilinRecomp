#include "lift.hpp"

#include <algorithm>
#include <array>
#include <bitset>
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
        for (const uint32_t pad : f_.landing_pads) {
            if (!present_.contains(pad)) {
                out.why = std::format("a landing pad at {:08X} that the function doesn't reach", pad);
                out.why_at = pad;
                return out;
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
                return out;
            }
            if (f_.keep_pc && (line.find("read") != std::string::npos || line.find("write") != std::string::npos))
                line = std::format("    c.pc = 0x{:08X}u;{}\n", a, f_.keep_registers ? " " + site(Site::kFlush) : std::string()) + line;
            const bool last = k + 1 == f_.insns.size();
            const uint32_t next = last ? 0 : f_.insns[k + 1].first;
            if (!terminator(in) && !tables_.contains(a) && (last || next != a + 4)) {
                // Execution continues past this instruction into code that isn't the next one we have.
                if (f_.falls_into == a + 4) {
                    line += std::format("    {} {}(c); return;\n", site(Site::kLeave), fn_name(a + 4));
                } else {
                    out.why = std::format("execution runs on to {:08X}", a + 4);
                    out.why_at = a;
                    return out;
                }
            }
            text_[k] = std::move(line);
        }
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
        return out;
    }

private:
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
    // local. The field's local is still set at each compare, for the reads that can't fold and the write-backs.
    struct Site {
        enum Kind : uint8_t {
            kLeave,   // the function leaves (a return, a tail call, a trap, a throw): written back, nothing after
            kCall,    // a call: written back, then read again
            kCallIf,  // a call that may not happen
            kFlush,   // written back, and the function goes on (--registers: before each memory access)
        } kind;
    };
    // What each condition-register bit's reaching definitions are, as the dataflow carries them.
    enum Fold : uint8_t { kUnreached, kOpaque, kSigned, kUnsigned, kFloat, kBool };
    static constexpr unsigned kFprAt = 32, kVrAt = 64, kCrAt = 96, kLrAt = 104, kCtrAt = 105, kSoAt = 106, kOvAt = 107, kCaAt = 108, kLocals = 109;
    using Locals = std::bitset<kLocals>;
    struct Save {
        Fold kind;        // kSigned, kUnsigned, kFloat (a field's bits) or kBool (one bit)
        unsigned target;  // the field, or for kBool the bit
        std::string a, b;
    };

    // A sync site's placeholder (its write-back).
    std::string site(Site::Kind kind) {
        sites_.push_back({kind});
        return std::format("@S{}@", sites_.size() - 1);
    }
    // A call's sync site: the write-back before it and what is read again after it.
    std::pair<std::string, std::string> call_site(bool conditional) {
        std::string out = site(conditional ? Site::kCallIf : Site::kCall);
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
    // The locals a piece of lifted text assigns: `x = `, `x -= ` and the like, and the carry's out-parameter `, ca)`.
    static Locals assigned(std::string_view s) {
        Locals out;
        const auto word = [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; };
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '@') {  // a placeholder
                const size_t end = s.find('@', i + 1);
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

    bool flow() {
        const size_t n = f_.insns.size();
        const auto index = [&](uint32_t a) {
            return static_cast<size_t>(std::ranges::lower_bound(f_.insns, a, {}, &std::pair<uint32_t, Insn>::first) - f_.insns.begin());
        };
        // Successors, and each instruction's text cut at its sync sites.
        struct Event {
            int site;     // or -1: a piece of text, which assigns `defs`
            Locals defs;
        };
        std::vector<std::vector<size_t>> succ(n);
        std::vector<std::vector<Event>> events(n);
        std::vector<Locals> assigns(n);
        std::vector<bool> reloads(n);
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
                    events[k].push_back({-1, assigned(t.substr(from, p - from))});
                    if (t[p + 1] == 'S') events[k].push_back({std::stoi(std::string(t.substr(p + 2, end - p - 2))), {}});
                    else reloads[k] = true;
                    from = end + 1;
                }
                p = end + 1;
            }
            events[k].push_back({-1, assigned(t.substr(from))});
            assigns[k] = assigned(t);
        }
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
                    site_dirty_[static_cast<size_t>(e.site)] |= cur;
                    const Site::Kind kind = sites_[static_cast<size_t>(e.site)].kind;
                    if (kind == Site::kCall || kind == Site::kFlush) cur.reset();
                }
                std::array<Fold, 32> fold = fold_in[k];
                for (unsigned field = 0; field < 8; ++field)
                    if (assigns[k][kCrAt + field])
                        for (unsigned i = 0; i < 4; ++i) fold[4 * field + i] = kOpaque;
                for (unsigned bit = 0; bit < 32; ++bit)
                    if (defs_[k][bit] != kUnreached) fold[bit] = defs_[k][bit];
                if (reloads[k]) fold.fill(kOpaque);
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
        const Locals all = declared();
        for (const Locals& d : site_dirty_)
            if ((d & ~all).any()) {
                error_ = "a local to be written back that the function doesn't declare (a fault in the lifter)";
                return false;
            }
        return true;
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
        const auto write_back = [&](const Locals& d) {
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
                    merged += std::format("{}(cr{} << {})", merged.empty() ? "" : " | ", n, 28 - 4 * n);
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
        std::string read_again;
        for (const unsigned n : gprs_) read_again += std::format("r{0} = c.r[{0}]; ", n);
        for (const unsigned n : fprs_) read_again += std::format("f{0} = c.f[{0}]; ", n);
        for (const unsigned n : vrs_) read_again += std::format("v{0} = c.v[{0}]; ", n);
        for (const unsigned n : crs_) read_again += std::format("cr{0} = (c.cr >> {1}) & 15u; ", n, 28 - 4 * n);
        for (const char* s : {"lr", "ctr"})
            if (specials_.contains(s)) read_again += std::format("{0} = c.{0}; ", s);
        for (const char* s : {"so", "ov", "ca"})
            if (specials_.contains(s)) read_again += std::format("{0} = c.xer_{0}; ", s);
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
                case 'S': out += write_back(site_dirty_[std::stoul(body)]); break;
                case 'L': out += read_again; break;
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
        out = std::format("{} {}(c); return;", site(Site::kLeave), fn_name(target));
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
                s += std::format(" {} = record(r{}, {});", CR(0), dst, SO());
                s += saves(kSigned, 0, std::format("r{}", dst), "0");
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
            const std::string leave = site(Site::kLeave);
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
            line(std::format("{} = compare_{}({}, {}, {});{}", CR(field), is_signed ? 's' : 'u', x, y, SO(), saves(is_signed ? kSigned : kUnsigned, field, x, y)));
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
            line(std::format("{} = fp::fcompare({}, {});{}", CR(in.crfd()), F(a), F(b), saves(kFloat, in.crfd(), F(a), F(b))));
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
                const auto [out_s, in_s] = call_site(false);
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
                const auto [out_s, in_s] = call_site(cond != "true");
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
            const std::string leave = site(Site::kLeave);
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
                s += std::format(" default: {} c.hooks->trap(c, {}, \"a switch jumped outside its table\"); return; }}", site(Site::kLeave), hex32(pc));
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

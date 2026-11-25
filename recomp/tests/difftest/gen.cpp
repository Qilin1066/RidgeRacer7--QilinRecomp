// qilin_difftest_gen: writes the differential test's cases as C++ (run at
// build time; the output is compiled into qilin_difftest).
//
// For every instruction the lifter's slice covers, random valid encodings
// (the decoder decides validity), each lifted as a one-instruction function
// ending in blr. For the branches, traps, calls and switches, small programs
// whose result shows which way they went.
//
//   qilin_difftest_gen <out.cpp> [cases-per-instruction]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <random>
#include <string>
#include <vector>

#include "cases.hpp"
#include "lift.hpp"
#include "ppu.hpp"

using namespace qilin;
using difftest::Kind;
using ppu::Insn;
using ppu::Op;

namespace {

constexpr uint32_t kBase = 0x00100000u;  // case k's code lives at kBase + 64 * k
constexpr uint32_t kBlr = 0x4E800020u;
constexpr uint32_t li_r3(unsigned v) { return 0x38600000u | v; }      // addi r3,0,v
constexpr uint32_t addi_r3(unsigned v) { return 0x38630000u | v; }    // addi r3,r3,v

struct Out {
    std::string functions, table;
    size_t count = 0;
    uint32_t pc() const { return kBase + 64 * static_cast<uint32_t>(count); }

    // Lifts `words` (address, word) as one function at this case's pc and adds the case.
    bool add(Kind kind, uint32_t word, const std::vector<std::pair<uint32_t, uint32_t>>& words, std::vector<JumpTable> tables = {},
             std::vector<uint32_t> functions_known = {}) {
        LiftInput in;
        in.start = pc();
        for (const auto& [a, w] : words) in.insns.emplace_back(a, ppu::decode(w));
        in.tables = std::move(tables);
        in.is_function = [known = std::move(functions_known)](uint32_t a) { return std::ranges::find(known, a) != known.end(); };
        const Lifted l = lift(in);
        if (!l.ok) {
            std::println(stderr, "difftest_gen: {:08x} ({}) didn't lift: {}", word, ppu::disasm(ppu::decode(word), pc()), l.why);
            return false;
        }
        functions += l.code + "\n";
        table += std::format("    {{Kind({}), 0x{:08x}u, 0x{:08x}u, &{}}},  // {}\n", static_cast<int>(kind), word, pc(), fn_name(pc()),
                             ppu::disasm(ppu::decode(word), pc()));
        ++count;
        return true;
    }
    // A second function (a callee), lifted and declared before the case that uses it.
    bool helper(uint32_t at, const std::vector<std::pair<uint32_t, uint32_t>>& words) {
        LiftInput in;
        in.start = at;
        for (const auto& [a, w] : words) in.insns.emplace_back(a, ppu::decode(w));
        const Lifted l = lift(in);
        if (!l.ok) return false;
        functions += l.code + "\n";
        return true;
    }
};

bool d_form(const ppu::Spec& s) {
    for (const ppu::Opnd o : s.operands)
        if (o == ppu::Opnd::d_rA0 || o == ppu::Opnd::ds_rA0) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "usage: qilin_difftest_gen <out.cpp> [cases-per-instruction]");
        return 2;
    }
    const int per = argc > 2 ? std::atoi(argv[2]) : 24;
    std::mt19937 rng(20061111);  // fixed: the cases are the same on every host
    Out out;
    bool ok = true;

    // ---- one instruction, then blr ----
    size_t covered = 0;
    for (const ppu::Spec& s : ppu::specs()) {
        if ((s.flags & ppu::kBranch) || s.op == Op::sc || s.op == Op::mftb || s.op == Op::tw || s.op == Op::td || s.op == Op::twi || s.op == Op::tdi) continue;
        uint32_t fields = (s.flags & ppu::kRc) ? 1u : 0u;
        for (const ppu::Opnd o : s.operands) fields |= ppu::field_mask(o);
        const bool memory = s.flags & (ppu::kLoad | ppu::kStore);
        int made = 0;
        for (int tries = 0; tries < 4000 && made < per; ++tries) {
            const uint32_t word = s.match | (rng() & fields & ~s.mask);
            const Insn in = ppu::decode(word);
            if (in.op != s.op || !in_slice(in)) continue;
            if (memory && d_form(s) && in.ra() == 0) continue;  // (the runner steers the address through rA)
            ok &= out.add(Kind::Plain, word, {{out.pc(), word}, {out.pc() + 4, kBlr}});
            ++made;
        }
        covered += made > 0;
    }

    // ---- conditional branches: every BO the decoder accepts, a spread of BI ----
    for (unsigned bo = 0; bo < 32; ++bo)
        for (const unsigned bi : {0u, 1u, 2u, 3u, 6u, 13u, 31u}) {
            const uint32_t pc = out.pc();
            const uint32_t bc = (16u << 26) | (bo << 21) | (bi << 16) | 12u;
            if (ppu::decode(bc).valid())
                ok &= out.add(Kind::Bc, bc, {{pc, bc}, {pc + 4, li_r3(1)}, {pc + 8, kBlr}, {pc + 12, li_r3(2)}, {pc + 16, kBlr}});
            for (const auto& [kind, xo] : {std::pair{Kind::Bclr, 16u}, std::pair{Kind::Bcctr, 528u}}) {
                const uint32_t at = out.pc();
                const uint32_t word = (19u << 26) | (bo << 21) | (bi << 16) | (xo << 1);
                if (!ppu::decode(word).valid()) continue;
                const bool always = (bo & 0x14) == 0x14;
                if (always) ok &= out.add(kind, word, {{at, word}});
                else ok &= out.add(kind, word, {{at, word}, {at + 4, li_r3(1)}, {at + 8, kBlr}});
            }
        }

    // ---- traps ----
    for (int k = 0; k < 4 * per; ++k) {
        const uint32_t pc = out.pc();
        const unsigned to = k < 8 ? (k % 2 ? 31u : 0u) : rng() & 31, ra = rng() & 31, rb = rng() & 31;
        uint32_t word = 0;
        switch (k % 4) {
        case 0: word = (31u << 26) | (to << 21) | (ra << 16) | (rb << 11) | (4u << 1); break;   // tw
        case 1: word = (31u << 26) | (to << 21) | (ra << 16) | (rb << 11) | (68u << 1); break;  // td
        case 2: word = (3u << 26) | (to << 21) | (ra << 16) | (rng() & 0xffff); break;          // twi
        default: word = (2u << 26) | (to << 21) | (ra << 16) | (rng() & 0xffff); break;         // tdi
        }
        if (to == 31) ok &= out.add(Kind::Trap, word, {{pc, word}});
        else ok &= out.add(Kind::Trap, word, {{pc, word}, {pc + 4, li_r3(1)}, {pc + 8, kBlr}});
    }

    // ---- a call, a tail call, a system call, a switch ----
    {
        const uint32_t pc = out.pc(), callee = pc + 32;
        ok &= out.helper(callee, {{callee, addi_r3(16)}, {callee + 4, kBlr}});
        const uint32_t bl = 0x48000001u | (callee - pc);
        ok &= out.add(Kind::Call, bl, {{pc, bl}, {pc + 4, addi_r3(1)}, {pc + 8, kBlr}}, {}, {callee});
    }
    {
        const uint32_t pc = out.pc(), callee = pc + 32;
        ok &= out.helper(callee, {{callee, addi_r3(16)}, {callee + 4, kBlr}});
        const uint32_t b = 0x48000000u | (callee - pc);
        ok &= out.add(Kind::Tail, b, {{pc, b}}, {}, {callee});
    }
    {
        const uint32_t pc = out.pc();
        ok &= out.add(Kind::System, 0x44000002u, {{pc, 0x44000002u}, {pc + 4, kBlr}});
    }
    {
        const uint32_t pc = out.pc(), bcctr = 0x4E800420u;
        JumpTable table{pc, pc + 4, {pc + 16, pc + 24, pc + 16}, true};
        ok &= out.add(Kind::Switch, bcctr, {{pc, bcctr}, {pc + 16, li_r3(1)}, {pc + 20, kBlr}, {pc + 24, li_r3(2)}, {pc + 28, kBlr}}, {table});
    }
    for (const uint32_t mftb : {0x7C6C42E6u, 0x7C8D42E6u}) {  // mftb r3,268 and mftb r4,269
        const uint32_t pc = out.pc();
        ok &= out.add(Kind::TimeBase, mftb, {{pc, mftb}, {pc + 4, kBlr}});
    }
    if (!ok) return 1;

    std::ofstream f(argv[1], std::ios::binary);
    f << "// Written by qilin_difftest_gen: the lifter's output for the differential test's cases.\n"
         "#include <atomic>\n#include <bit>\n#include <cstdint>\n\n#include \"cases.hpp\"\n\nusing namespace qilin::rt;\nusing difftest::Kind;\n\n"
      << out.functions << "namespace difftest {\nconst Case kCases[] = {\n"
      << out.table << "};\nconst size_t kCaseCount = " << out.count << ";\n}  // namespace difftest\n";
    f.close();
    if (!f) {
        std::println(stderr, "difftest_gen: cannot write {}", argv[1]);
        return 1;
    }
    std::println("difftest_gen: {} cases; {} instructions covered one at a time", out.count, covered);
    return 0;
}

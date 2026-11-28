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

#include "../elf_builder.hpp"
#include "cases.hpp"
#include "cfg.hpp"
#include "elf.hpp"
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
    std::string functions, table, programs;
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
    // A program (add_programs): its function, lifted at `at`, and its words and its helper's for the reference.
    bool program(uint32_t pc, uint32_t at, const std::vector<std::pair<uint32_t, uint32_t>>& words, const std::vector<std::pair<uint32_t, uint32_t>>& helper_words,
                 std::vector<uint32_t> functions_known, bool registers) {
        LiftInput in;
        in.start = at;
        for (const auto& [a, w] : words) in.insns.emplace_back(a, ppu::decode(w));
        in.keep_pc = in.keep_registers = registers;
        in.is_function = [known = std::move(functions_known)](uint32_t a) { return std::ranges::find(known, a) != known.end(); };
        const Lifted l = lift(in);
        if (!l.ok) {
            std::println(stderr, "difftest_gen: a program at {:08x} didn't lift: {} (at {:08x})", at, l.why, l.why_at);
            return false;
        }
        functions += l.code + "\n";
        programs += std::format("const ProgramWord kProgram{:08x}[] = {{", pc);
        for (const auto* list : {&words, &helper_words})
            for (const auto& [a, w] : *list) programs += std::format("{{0x{:08x}u, 0x{:08x}u}}, ", a, w);
        programs += "};\n";
        table += std::format("    {{Kind::Program, 0x{:08x}u, 0x{:08x}u, &{}, kProgram{:08x}, {}u, 0x{:08x}u}},  // a program{}\n", words[1].second, at, fn_name(at), pc,
                             words.size() + helper_words.size(), at, registers ? ", lifted with --registers" : "");
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

// ---- C++ exceptions: a program that throws through __builtin_eh_return's epilogue ----
// Three functions of a program elf_builder makes, with their unwinding tables and A's exception table, found by the
// control-flow pass and lifted as the game's are. A(r3 depth, r4 mode, r5 target frame or 0, r6 the pad) calls itself
// `depth` times, then B, which calls U, which stands in for the unwinder: in mode 0 it returns normally (r10 = 0), else
// it ends as the unwinder does, LR the pad, r1 moved by r10 to the frame in r5: the outermost A's (mode 1, set by
// A when r5 is 0) or the innermost's (mode 2). Each normal return of A adds 1 to r7 and of B 16; the pad adds 256 and
// copies r3 to r8. The runner checks the lifted program against the reference interpreter, for which the epilogue is
// just an add and a return to LR.
constexpr uint32_t kEhA = elfb::kText + 0xc00, kEhB = kEhA + 0x60, kEhU = kEhB + 0x24, kEhPad = kEhA + 0x54;
constexpr uint32_t kEhFrame = elfb::kText + 0x1400, kEhTable = elfb::kText + 0x1800;
const uint32_t kEhWords[] = {
    // A
    0x7c0802a6, 0xf8010010, 0xf821ffc1,  // mflr r0; std r0,16(r1); stdu r1,-64(r1)
    0x2fa50000, 0x409e0008, 0x7c250b78,  // cmpdi cr7,r5,0; bne cr7,+8; mr r5,r1 (the outermost: the target unless mode 2 says otherwise)
    0x2f830000, 0x419e0014,              // cmpwi cr7,r3,0; beq cr7,+20
    0x3863ffff, 0x4bffffdd,              // addi r3,r3,-1; bl A (a call site of the pad)
    0x38e70001, 0x48000018,              // addi r7,r7,1; b to the epilogue
    0x2f840002, 0x409e0008, 0x7c250b78,  // cmpwi cr7,r4,2; bne cr7,+8; mr r5,r1 (mode 2: this one, the innermost)
    0x48000025,                          // bl B (a call site of the pad)
    0x38e70001,                          // addi r7,r7,1
    0x38210040, 0xe8010010, 0x7c0803a6, 0x4e800020,  // addi r1,r1,64; ld r0,16(r1); mtlr r0; blr
    0x38e70100, 0x7c681b78, 0x4bffffe8,  // the pad: addi r7,r7,256; mr r8,r3; b to the epilogue
    // B
    0x7c0802a6, 0xf8010010, 0xf821ffd1, 0x48000019,  // mflr r0; std r0,16(r1); stdu r1,-48(r1); bl U
    0x38e70010,                                      // addi r7,r7,16
    0x38210030, 0xe8010010, 0x7c0803a6, 0x4e800020,  // addi r1,r1,48; ld r0,16(r1); mtlr r0; blr
    // U
    0x7c0802a6, 0xf8010010, 0xf821ffd1,  // mflr r0; std r0,16(r1); stdu r1,-48(r1)
    0x39400000, 0x2f840000, 0x419e0010,  // li r10,0; cmpwi cr7,r4,0; beq cr7,+16
    0xf8c10040, 0x39210030, 0x7d492850,  // std r6,64(r1) (the pad, as the return address); addi r9,r1,48; subf r10,r9,r5
    0xe8010040, 0x7c0803a6, 0x38210030,  // ld r0,64(r1); mtlr r0; addi r1,r1,48
    0x7c215214, 0x4e800020,              // add r1,r1,r10; blr: __builtin_eh_return's epilogue
};

// Writes the program's functions (declarations first), and its words for the reference, into `out`.
bool eh_program(std::string& functions, std::string& data) {
    using namespace elfb;
    Bytes b = make_elf();
    b.resize(0x2000);
    put_be16(b, 60, 7);                                                  // seven sections
    put_segment(b, 0, 1, 5, kTextAt, kText, 0x1c00, 0x1c00, 0x10000);  // the code segment, now holding the tables too
    const auto file = [](uint32_t address) { return kTextAt + (address - kText); };
    for (size_t i = 0; i < std::size(kEhWords); ++i) put_be32(b, file(kEhA) + 4 * i, kEhWords[i]);
    put_section(b, 4, 0, 1, 6, kEhA, file(kEhA), sizeof kEhWords);
    // .eh_frame: a CIE ("zLR": exception tables' addresses absolute, 4 bytes; code addresses relative, 8), three FDEs.
    const size_t eh = file(kEhFrame);
    put_be32(b, eh, 0x10);
    b[eh + 8] = 1;
    put_text(b, eh + 9, std::string_view("zLR", 4));
    b[eh + 13] = 4;
    b[eh + 14] = 0x78;
    b[eh + 15] = 65;
    b[eh + 16] = 2;
    b[eh + 17] = 0x03;
    b[eh + 18] = 0x14;
    const std::pair<uint32_t, uint32_t> fdes[] = {{kEhA, 0x60}, {kEhB, 0x24}, {kEhU, 0x38}};
    for (size_t i = 0; i < 3; ++i) {
        const size_t at = 0x14 + 0x20 * i;
        put_be32(b, eh + at, 0x1c);
        put_be32(b, eh + at + 4, static_cast<uint32_t>(at + 4));
        put_be64(b, eh + at + 8, uint64_t{fdes[i].first} - (kEhFrame + at + 8));
        put_be64(b, eh + at + 16, fdes[i].second);
        b[eh + at + 24] = 4;
        put_be32(b, eh + at + 25, i == 0 ? kEhTable : 0);
    }
    put_section(b, 5, 0, 1, 2, kEhFrame, eh, 0x78);
    // A's exception table: both calls land at the pad.
    const uint8_t table[] = {0xff, 0xff, 0x01, 8, 0x24, 4, 0x54, 0, 0x3c, 4, 0x54, 0};
    std::memcpy(b.data() + file(kEhTable), table, sizeof table);
    put_section(b, 6, 0, 1, 2, kEhTable, file(kEhTable), sizeof table);

    const auto elf = Elf::parse(std::move(b));
    if (!elf) {
        std::println(stderr, "difftest_gen: the unwinding program: {}", elf.error());
        return false;
    }
    const auto cfg = Cfg::build(*elf);
    if (!cfg) {
        std::println(stderr, "difftest_gen: the unwinding program: {}", cfg.error());
        return false;
    }
    std::string code;
    for (const uint32_t start : {kEhA, kEhB, kEhU}) {
        if (!cfg->functions().contains(start)) {
            std::println(stderr, "difftest_gen: the unwinding program has no function at {:08x}", start);
            return false;
        }
        const Lifted l = lift(lift_input(*cfg, cfg->functions().at(start)));
        const bool shaped = start == kEhA ? l.catches && cfg->functions().at(start).landing_pads == std::vector<uint32_t>{kEhPad}
                                          : !l.catches && l.eh_returns == (start == kEhU ? 1u : 0u);
        if (!l.ok || !shaped) {
            std::println(stderr, "difftest_gen: the unwinding program's function at {:08x} didn't lift as expected: {}", start, l.why);
            return false;
        }
        functions += std::format("void {}(Cpu& c);\n", fn_name(start));
        code += l.code + "\n";
    }
    functions += code;
    data += "const ProgramWord kEhProgram[] = {\n";
    for (size_t i = 0; i < std::size(kEhWords); ++i) data += std::format("    {{0x{:08x}u, 0x{:08x}u}},\n", kEhA + 4 * i, kEhWords[i]);
    data += std::format("}};\nconst size_t kEhProgramSize = {};\nconst uint32_t kEhEntry = 0x{:08x}u, kEhPad = 0x{:08x}u;\nvoid eh_entry(Cpu& c) {{ {}(c); }}\n",
                        std::size(kEhWords), kEhA, kEhPad, fn_name(kEhA));
    return true;
}

// ---- programs: the lifter's dataflow (what each call and return writes back, which compares a branch reads) ----
// Random functions of a few dozen instructions: integer operations, compares into any CR field, record forms, CR
// logical operations, mfcr and mtcrf, carries, loads and stores off r1, forward conditional branches on any CR bit
// (and on CTR), and calls (some conditional) to a helper that changes registers, CR fields and the carry, or nothing. So a CR field is
// read after a call, or in a block two away from the compare that set it, or after paths that set it differently;
// a register is changed on one path to a call and not on the other. r0 holds the return address (mflr first, mtlr
// last), r1 the memory window, r2 nothing: the random instructions write none of them. Every third is lifted with
// --registers (a write-back before each memory access). The reference interprets the words.
std::vector<const ppu::Spec*> program_pool() {
    std::vector<const ppu::Spec*> pool;
    for (const ppu::Spec& s : ppu::specs()) {
        if (s.flags & (ppu::kBranch | ppu::kLoad | ppu::kStore | ppu::kFloat | ppu::kVector)) continue;
        if (s.op == Op::sc || s.op == Op::mftb || s.op == Op::tw || s.op == Op::td || s.op == Op::twi || s.op == Op::tdi || s.op == Op::mfspr ||
            s.op == Op::mtspr)
            continue;
        pool.push_back(&s);
    }
    return pool;
}

// A random instruction from the pool that writes none of r0, r1 and r2.
uint32_t program_word(std::mt19937& rng, const std::vector<const ppu::Spec*>& pool) {
    for (;;) {
        const ppu::Spec& s = *pool[rng() % pool.size()];
        uint32_t fields = (s.flags & ppu::kRc) ? 1u : 0u;
        for (const ppu::Opnd o : s.operands) fields |= ppu::field_mask(o);
        const uint32_t word = s.match | (rng() & fields & ~s.mask);
        const Insn in = ppu::decode(word);
        if (in.op != s.op || !in_slice(in)) continue;
        bool fine = true;
        for (const ppu::Opnd o : s.operands) {
            if (o == ppu::Opnd::rD) fine &= in.rd() > 2;
            if (o == ppu::Opnd::rA) fine &= in.ra() > 2;
            if (o == ppu::Opnd::rB) fine &= in.rb() > 2;
            if (o == ppu::Opnd::rA0) fine &= in.ra() == 0 || in.ra() > 2;
        }
        if (fine) return word;
    }
}

// A load or store of a random width off r1, of a register above r2.
uint32_t memory_word(std::mt19937& rng) {
    const uint32_t r = 3 + static_cast<uint32_t>(rng() % 29);
    const int32_t d = static_cast<int32_t>(rng() % 128) * 8 - 512;
    const uint32_t disp = static_cast<uint32_t>(d) & 0xffffu;
    static constexpr uint32_t kD[] = {32, 36, 34, 38, 40, 44};  // lwz stw lbz stb lhz sth
    if (rng() % 4 == 0) return ((rng() % 2 ? 62u : 58u) << 26) | (r << 21) | (1u << 16) | (disp & 0xfffcu);  // std / ld
    return (kD[rng() % std::size(kD)] << 26) | (r << 21) | (1u << 16) | disp;
}

bool add_programs(Out& out, std::mt19937& rng, int count) {
    const std::vector<const ppu::Spec*> pool = program_pool();
    bool ok = true;
    for (int p = 0; p < count; ++p) {
        const uint32_t pc = out.pc();
        // Its words lie away from the cases' 64 bytes each: the program at `at`, its helper after it.
        const uint32_t at = 0x00800000u + 0x400u * static_cast<uint32_t>(p), helper = at + 0x200u;
        // The helper: a few instructions (none at all, sometimes), then blr.
        std::vector<std::pair<uint32_t, uint32_t>> helper_words;
        const unsigned helper_size = rng() % 4 == 0 ? 0 : 1 + static_cast<unsigned>(rng() % 6);
        for (unsigned i = 0; i < helper_size; ++i) helper_words.emplace_back(helper + 4 * i, program_word(rng, pool));
        helper_words.emplace_back(helper + 4 * helper_size, kBlr);
        // The program: mflr r0, the body, mtlr r0, blr.
        const unsigned body = 12 + static_cast<unsigned>(rng() % 28);
        std::vector<uint32_t> words;
        words.push_back(0x7c0802a6u);  // mflr r0
        for (unsigned i = 0; i < body; ++i) {
            const unsigned pick = static_cast<unsigned>(rng() % 16);
            if (pick < 2) words.push_back(memory_word(rng));
            else if (pick < 3) words.push_back(0);  // a branch, filled in below
            else if (pick < 4) {
                // A compare and, a little later (past a call, sometimes), a branch on its field: the reads that fold.
                const uint32_t field = static_cast<uint32_t>(rng() % 8), ra = static_cast<uint32_t>(rng() % 32), rb = static_cast<uint32_t>(rng() % 32);
                const uint32_t l = static_cast<uint32_t>(rng() % 2) << 21, imm = static_cast<uint32_t>(rng()) & (rng() % 2 ? 0xffffu : 0x7u);
                switch (rng() % 5) {
                case 0: words.push_back((31u << 26) | (field << 23) | l | (ra << 16) | (rb << 11)); break;              // cmp
                case 1: words.push_back((31u << 26) | (field << 23) | l | (ra << 16) | (rb << 11) | (32u << 1)); break;  // cmpl
                case 2: words.push_back((11u << 26) | (field << 23) | l | (ra << 16) | imm); break;                      // cmpi
                case 3: words.push_back((10u << 26) | (field << 23) | l | (ra << 16) | imm); break;                      // cmpli
                default: words.push_back((63u << 26) | (field << 23) | (ra << 16) | (rb << 11)); break;                 // fcmpu
                }
                for (unsigned k = static_cast<unsigned>(rng() % 3); k > 0; --k) words.push_back(rng() % 4 == 0 ? 1u : program_word(rng, pool));
                words.push_back(0x10u | field);
            }
            else if (pick < 5) words.push_back(1);  // a call, likewise
            else if (pick < 6) words.push_back(0x7c000026u | ((3 + static_cast<uint32_t>(rng() % 29)) << 21));  // mfcr
            else words.push_back(program_word(rng, pool));
        }
        words.push_back(0x7c0803a6u);  // mtlr r0
        words.push_back(kBlr);
        for (size_t i = 0; i < words.size(); ++i) {
            const uint32_t here = at + 4 * static_cast<uint32_t>(i);
            if (words[i] == 0 || (words[i] & ~7u) == 0x10u) {  // bc to a later instruction (not past the epilogue's two)
                const size_t last = words.size() - 2;
                const size_t to = i + 1 + rng() % (last - i);
                static constexpr uint32_t kBo[] = {12, 4, 12, 4, 16, 18, 8, 0};  // true, false; CTR != 0, CTR == 0; and both
                const uint32_t bo = words[i] ? (rng() % 2 ? 12u : 4u) : kBo[rng() % std::size(kBo)];
                const uint32_t bi = words[i] ? 4 * (words[i] & 7u) + static_cast<uint32_t>(rng() % 4) : static_cast<uint32_t>(rng() % 32);
                words[i] = (16u << 26) | (bo << 21) | (bi << 16) | ((4 * static_cast<uint32_t>(to - i)) & 0xfffcu);
            } else if (words[i] == 1) {
                if (rng() % 3 == 0)  // a call that may not happen: bcl on a CR bit
                    words[i] = (16u << 26) | ((rng() % 2 ? 12u : 4u) << 21) | (static_cast<uint32_t>(rng() % 32) << 16) | ((helper - here) & 0xfffcu) | 1u;
                else
                    words[i] = 0x48000001u | ((helper - here) & 0x03fffffcu);  // bl helper
            }
        }
        std::vector<std::pair<uint32_t, uint32_t>> program;
        for (size_t i = 0; i < words.size(); ++i) program.emplace_back(at + 4 * static_cast<uint32_t>(i), words[i]);
        ok &= out.helper(helper, helper_words);
        ok &= out.program(pc, at, program, helper_words, {helper}, p % 3 == 2);
    }
    return ok;
}

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
            // `add r1,r1,rX` before a return is __builtin_eh_return's epilogue, which throws: the unwinding program tests it.
            if (in.op == Op::add && in.rd() == 1 && (in.ra() == 1) != (in.rb() == 1)) continue;
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
    ok &= add_programs(out, rng, 400);
    std::string eh_data;
    ok &= eh_program(out.functions, eh_data);
    if (!ok) return 1;

    std::ofstream f(argv[1], std::ios::binary);
    f << "// Written by qilin_difftest_gen: the lifter's output for the differential test's cases.\n"
         "#include <atomic>\n#include <bit>\n#include <cstdint>\n\n#include \"cases.hpp\"\n\nusing namespace qilin::rt;\nusing difftest::Kind;\n\n"
      << out.functions << "namespace difftest {\n" << out.programs << "const Case kCases[] = {\n"
      << out.table << "};\nconst size_t kCaseCount = " << out.count << ";\n" << eh_data << "}  // namespace difftest\n";
    f.close();
    if (!f) {
        std::println(stderr, "difftest_gen: cannot write {}", argv[1]);
        return 1;
    }
    std::println("difftest_gen: {} cases; {} instructions covered one at a time", out.count, covered);
    return 0;
}

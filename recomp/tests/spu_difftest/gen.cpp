// qilin_spu_difftest_gen: writes the SPU lifter's output for random
// encodings of every instruction in its slice that is not a branch, each as
// a function of its own, for qilin_spu_difftest to run against the
// reference (ref.cpp); and 200 small random programs of two functions (a
// loop, branches over code, a call, a conditional return, a constant split
// by a label), lifted whole, for what the lifter does around branches and
// calls (its registers in locals). (The switch is tested by the game's own
// programs running.)
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <print>
#include <random>
#include <string>
#include <vector>

#include "spu.hpp"
#include "spu_cfg.hpp"
#include "spu_image.hpp"
#include "spu_lift.hpp"

using namespace qilin;

namespace {

// ---------------------------------------------------------------- programs

// An SPU program's ELF file around `code` (at 0x3000, the entry point), as the SPU's linker lays one out.
std::vector<uint8_t> spu_elf(const std::vector<uint32_t>& code) {
    const uint32_t code_at = 0x100, code_bytes = static_cast<uint32_t>(4 * code.size()), note_at = code_at + code_bytes, names_at = note_at + 0x40,
                   sections_at = names_at + 0x40;
    const std::string_view names("\0.text\0.note.spu_name\0.shstrtab\0", 31);
    std::vector<uint8_t> b(sections_at + 4 * 40);
    const auto put16 = [&](size_t at, uint32_t v) { b[at] = static_cast<uint8_t>(v >> 8), b[at + 1] = static_cast<uint8_t>(v); };
    const auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (24 - 8 * i));
    };
    const uint8_t ident[7] = {0x7f, 'E', 'L', 'F', 1, 2, 1};
    std::memcpy(b.data(), ident, 7);
    put16(16, 2), put16(18, 23), put32(20, 1), put32(24, 0x3000), put32(28, 52), put32(32, sections_at);
    put16(40, 52), put16(42, 32), put16(44, 2), put16(46, 40), put16(48, 4), put16(50, 3);
    const auto segment = [&](size_t i, uint32_t type, uint32_t offset, uint32_t address, uint32_t file, uint32_t memory, uint32_t flags) {
        const size_t at = 52 + i * 32;
        put32(at, type), put32(at + 4, offset), put32(at + 8, address), put32(at + 16, file), put32(at + 20, memory), put32(at + 24, flags);
    };
    segment(0, 1, code_at, 0x3000, code_bytes, code_bytes, 5);
    segment(1, 4, note_at, 0, 0x20, 0, 4);
    put32(note_at, 8), put32(note_at + 4, 0x20), put32(note_at + 8, 1);
    std::memcpy(b.data() + note_at + 12, "SPUNAME", 7);
    std::memcpy(b.data() + note_at + 20, "Test_r.elf", 10);
    std::memcpy(b.data() + names_at, names.data(), names.size());
    const auto section = [&](size_t i, uint32_t name_at, uint32_t type, uint32_t flags, uint32_t address, uint32_t offset, uint32_t size) {
        const size_t at = sections_at + i * 40;
        put32(at, name_at), put32(at + 4, type), put32(at + 8, flags), put32(at + 12, address), put32(at + 16, offset), put32(at + 20, size);
    };
    section(1, 1, 1, 6, 0x3000, code_at, code_bytes);
    section(2, 7, 7, 0, 0, note_at, 0x34);
    section(3, 22, 3, 0, 0, names_at, static_cast<uint32_t>(names.size()));
    for (size_t i = 0; i < code.size(); ++i) put32(code_at + 4 * i, code[i]);
    return b;
}

// The encodings the programs are built of (checked by decoding them back).
uint32_t ri16(uint32_t opcode, unsigned rt, uint32_t i16) { return (opcode << 23) | ((i16 & 0xffffu) << 7) | rt; }
uint32_t ri10(uint32_t opcode, unsigned rt, unsigned ra, uint32_t i10) { return (opcode << 24) | ((i10 & 0x3ffu) << 14) | (ra << 7) | rt; }
uint32_t branch_rt(uint32_t opcode, unsigned rt, uint32_t pc, uint32_t target) { return ri16(opcode, rt, (target - pc) >> 2); }
uint32_t indirect(uint32_t opcode, unsigned rt, unsigned ra) { return (opcode << 21) | (ra << 7) | rt; }

// Registers the programs keep for themselves: the link register, the stack pointer, the loop's counter.
constexpr unsigned kCounter = 127;
bool kept(unsigned r) { return r == 0 || r == 1 || r == kCounter; }

// A random instruction that is no branch, stop or hint and writes none of the kept registers.
uint32_t random_insn(std::mt19937& rng) {
    const auto all = spu::specs();
    for (;;) {
        const spu::Spec& s = all[rng() % all.size()];
        if (s.flags & (spu::kBranch | spu::kStop | spu::kHint)) continue;
        const uint32_t opcode = static_cast<uint32_t>(s.opcode) << (32 - s.bits), opcode_mask = ~uint32_t{0} << (32 - s.bits);
        const uint32_t word = opcode | (rng() & s.defined & ~opcode_mask);
        const spu::Insn in = spu::decode(word);
        if (in.op != s.op || !spu_in_slice(in)) continue;
        if (kept(in.rt()) || (s.form == spu::Form::rrr && kept(in.rt4()))) continue;
        return word;
    }
}

// A program of two functions, made to exercise what the lifter does around branches and calls: the first loops a few
// times over a block that branches over code and calls the second; the second returns early on a condition. Constants
// are set before a label and completed after it (an `ilhu` the branch may skip, then an `iohl`).
struct Made {
    std::vector<uint32_t> words;
};
Made make_program(std::mt19937& rng) {
    std::vector<uint32_t> w;
    const auto pc = [&] { return 0x3000u + 4 * static_cast<uint32_t>(w.size()); };
    const auto block = [&](unsigned n) {
        for (unsigned i = 0; i < n; ++i) w.push_back(random_insn(rng));
    };
    const auto any = [&] { return 2 + static_cast<unsigned>(rng() % 125); };  // (not a kept register)
    static constexpr uint32_t kConditional[] = {0x040, 0x042, 0x044, 0x046};  // brz brnz brhz brhnz
    static constexpr uint32_t kReturns[] = {0x128, 0x129, 0x12a, 0x12b};      // biz binz bihz bihnz
    w.push_back(ri16(0x081, kCounter, 1 + rng() % 3));  // il $127, 1..3
    const uint32_t loop = pc();
    block(4 + rng() % 6);
    const unsigned constant = any();
    w.push_back(ri16(0x082, constant, rng()));  // ilhu (before the skip: its value is known on one way in only)
    const size_t skip_at = w.size();
    w.push_back(0);  // the conditional branch over the next block, filled in below
    w.push_back(ri16(0x082, constant, rng()));  // ilhu again, on the way that doesn't skip
    block(2 + rng() % 6);
    const uint32_t skip = pc();
    w[skip_at] = branch_rt(kConditional[rng() % 4], any(), 0x3000u + 4 * static_cast<uint32_t>(skip_at), skip);
    w.push_back(ri16(0x0c1, constant, rng()));  // iohl, after the label
    const size_t call_at = w.size();
    w.push_back(0);  // brsl $0 to the second function, filled in below
    block(2 + rng() % 6);
    w.push_back(ri10(0x1c, kCounter, kCounter, 0x3ff));                               // ai $127,$127,-1
    w.push_back(branch_rt(0x042, kCounter, pc(), loop));                               // brnz $127, loop
    w.push_back(indirect(0x1a8, 0, 0));                                                // bi $0
    const uint32_t second = pc();
    w[call_at] = branch_rt(0x066, 0, 0x3000u + 4 * static_cast<uint32_t>(call_at), second);
    block(3 + rng() % 6);
    w.push_back(indirect(kReturns[rng() % 4], any(), 0));  // a conditional return
    block(2 + rng() % 6);
    w.push_back(indirect(0x1a8, 0, 0));  // bi $0
    return {w};
}

// Lifts `count` such programs: the code (each in a namespace of its own) and the table.
bool lift_programs(std::mt19937& rng, int count, std::string& code, std::string& table) {
    for (int k = 0; k < count; ++k) {
        const Made made = make_program(rng);
        const std::vector<uint8_t> file = spu_elf(made.words);
        const auto image = parse_spu_image(file, 0x10000000u);
        if (!image) {
            std::println(stderr, "spu_difftest_gen: program {}: {}", k, image.error());
            return false;
        }
        const auto cfg = SpuCfg::build(*image, file);
        if (!cfg) {
            std::println(stderr, "spu_difftest_gen: program {}: {}", k, cfg.error());
            return false;
        }
        if (cfg->functions().size() != 2 || !cfg->functions().contains(0x3000)) {
            std::println(stderr, "spu_difftest_gen: program {} has {} functions, not the two it was made with", k, cfg->functions().size());
            return false;
        }
        std::string declarations, definitions;
        for (const auto& [start, f] : cfg->functions()) {
            const SpuLifted lifted = spu_lift(*cfg, f);
            if (!lifted.ok) {
                std::println(stderr, "spu_difftest_gen: program {} doesn't lift: {} at {:05x}", k, lifted.why, lifted.why_at);
                return false;
            }
            declarations += std::format("void {}(Spu& s);\n", spu_fn_name(start));
            definitions += lifted.code;
        }
        std::string words;
        for (const uint32_t word : made.words) words += std::format("0x{:08x}u, ", word);
        code += std::format("namespace program_{0} {{\nvoid call(Spu& s, uint32_t, uint32_t pc) {{ s.hooks->halt(s, pc, \"a call through a register\"); }}\n{1}{2}"
                            "const uint32_t kWords[] = {{{3}}};\n}}  // namespace program_{0}\n",
                            k, declarations, definitions, words);
        table += std::format("    {{&program_{0}::fn_03000, program_{0}::kWords, {1}}},\n", k, made.words.size());
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "usage: qilin_spu_difftest_gen <out.cpp> [cases-per-instruction]");
        return 2;
    }
    const int per = argc > 2 ? std::atoi(argv[2]) : 24;
    std::mt19937 rng(20061111);  // fixed: the cases are the same on every host
    std::string functions, table;
    size_t count = 0, covered = 0;
    for (const spu::Spec& s : spu::specs()) {
        if (s.flags & spu::kBranch) continue;
        const uint32_t opcode = static_cast<uint32_t>(s.opcode) << (32 - s.bits), opcode_mask = ~uint32_t{0} << (32 - s.bits);
        int made = 0;
        for (int tries = 0; tries < 4000 && made < per; ++tries) {
            const uint32_t word = opcode | (rng() & s.defined & ~opcode_mask);
            const spu::Insn in = spu::decode(word);
            if (in.op != s.op || !spu_in_slice(in)) continue;
            const uint32_t pc = 0x3000u + 4 * (rng() & 0xfffu);
            const auto line = spu_lift_one(in, pc);
            if (!line) {
                std::println(stderr, "spu_difftest_gen: {:08x} ({}) didn't lift", word, spu::disasm(in, pc));
                return 1;
            }
            functions += std::format("static void case_{}(Spu& s) {{\n{}}}\n", count, *line);
            table += std::format("    {{0x{:08x}u, 0x{:05x}u, &case_{}}},  // {}\n", word, pc, count, spu::disasm(in, pc));
            ++count;
            ++made;
        }
        covered += made > 0;
    }
    std::string programs, program_table;
    if (!lift_programs(rng, 200, programs, program_table)) return 1;
    std::ofstream f(argv[1], std::ios::binary);
    f << "// Written by qilin_spu_difftest_gen: the SPU lifter's output for the differential test's cases.\n"
         "#include \"qilin/rt/spu_fp.hpp\"\n#include \"ref.hpp\"\n\nusing namespace qilin::rt;\n\n"
      << functions << "\nnamespace spu_difftest {\nconst Case kCases[] = {\n"
      << table << "};\nconst size_t kCaseCount = " << count << ";\n}  // namespace spu_difftest\n\n"
      << programs << "\nnamespace spu_difftest {\nconst Program kPrograms[] = {\n" << program_table << "};\nconst size_t kProgramCount = 200;\n}  // namespace spu_difftest\n";
    f.close();
    if (!f) {
        std::println(stderr, "spu_difftest_gen: cannot write {}", argv[1]);
        return 1;
    }
    std::println("spu_difftest_gen: {} cases; {} instructions covered; 200 programs", count, covered);
    return 0;
}

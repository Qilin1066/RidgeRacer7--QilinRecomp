// qilin_spu_difftest_gen: writes the SPU lifter's output for random
// encodings of every instruction in its slice that is not a branch, each as
// a function of its own, for qilin_spu_difftest to run against the
// reference (ref.cpp). (Branches, calls and switches need a program around
// them: they are tested by the game's own programs running.)
#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <random>
#include <string>

#include "spu.hpp"
#include "spu_lift.hpp"

using namespace qilin;

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
    std::ofstream f(argv[1], std::ios::binary);
    f << "// Written by qilin_spu_difftest_gen: the SPU lifter's output for the differential test's cases.\n"
         "#include \"qilin/rt/spu_fp.hpp\"\n#include \"ref.hpp\"\n\nusing namespace qilin::rt;\n\n"
      << functions << "\nnamespace spu_difftest {\nconst Case kCases[] = {\n"
      << table << "};\nconst size_t kCaseCount = " << count << ";\n}  // namespace spu_difftest\n";
    f.close();
    if (!f) {
        std::println(stderr, "spu_difftest_gen: cannot write {}", argv[1]);
        return 1;
    }
    std::println("spu_difftest_gen: {} cases; {} instructions covered", count, covered);
    return 0;
}

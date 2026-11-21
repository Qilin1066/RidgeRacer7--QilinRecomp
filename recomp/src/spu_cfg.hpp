// Function discovery and control-flow recovery for one SPU program.
//
// An SPU program is a small, statically linked ELF: one `.text`, entered at
// its entry point. It gives less than the PPU program does: no descriptors
// and no unwinding tables. What there is:
//
//   - calls are `brsl` (the link register is $0) and returns `bi $0`;
//   - a function's address in a register comes from `ila`, or from a word in
//     the program's data;
//   - the compiler aligns functions with `nop` and `lnop`;
//   - a switch is `bi` through a register, with its table of absolute
//     addresses in the code after it, aligned to 16 bytes.
//
// And one thing in the way: the linker kept every function of every object
// it took, so a program holds functions nothing in it calls (inline library
// functions emitted out of line, mostly). They are real code, found as
// "gaps": code at a clean boundary (after a return or a jump, and any
// padding) that nothing reaches.
//
// Pipeline, as the PPU pass's (cfg.hpp): seed starts with the entry point,
// every `brsl` target, and (weaker) every `ila` and data word that holds an
// aligned address in the code; explore by recursive descent, bounded by the
// next start, until no new start turns up; then take gaps as starts and
// explore again; drop any start found on weak evidence whose function
// reaches a word that isn't an instruction; account for every word.
#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "spu.hpp"
#include "spu_image.hpp"

namespace qilin {

// How a function was found, strongest evidence first.
enum class SpuOrigin : uint8_t { Entry, Call, TailCall, Pointer, DataPointer, Gap };
const char* spu_origin_name(SpuOrigin o);
inline bool spu_strong(SpuOrigin o) { return o <= SpuOrigin::TailCall; }

struct SpuJumpTable {
    uint32_t at = 0;                // the bi
    uint32_t table = 0;
    std::vector<uint32_t> targets;  // in index order
};

struct SpuFunction {
    uint32_t start = 0, bound = 0, end = 0;
    SpuOrigin origin = SpuOrigin::Call;
    uint32_t words = 0;
    std::set<uint32_t> calls;              // brsl, brasl
    std::set<uint32_t> tails;              // branches out of the function
    std::vector<uint32_t> indirect_calls;  // bisl
    std::vector<uint32_t> indirect_tails;  // bi through a register that isn't a return or a switch
    std::vector<SpuJumpTable> tables;
    std::vector<uint32_t> stops;           // stop and stopd
    std::vector<uint32_t> bad;             // reached a word that isn't an instruction, or left the code
    bool runs_off = false;                 // reaches its bound without a return or a jump
};

struct SpuCoverage {
    uint64_t words = 0, code = 0, tables = 0, padding = 0, unknown = 0;
    std::vector<std::pair<uint32_t, uint32_t>> unknown_spans;  // [lo, hi)
};

class SpuCfg {
public:
    // `file`: the SPU program's own ELF file, whose headers `image` was read from.
    static std::expected<SpuCfg, std::string> build(const SpuImage& image, std::span<const uint8_t> file);

    const SpuImage& image() const { return *image_; }
    const std::map<uint32_t, SpuFunction>& functions() const { return functions_; }
    const SpuCoverage& coverage() const { return coverage_; }
    const std::vector<uint32_t>& rejected() const { return rejected_; }  // starts on weak evidence that weren't functions
    // Channels read and written, with how often.
    const std::map<unsigned, unsigned>& channels_read() const { return read_; }
    const std::map<unsigned, unsigned>& channels_written() const { return written_; }
    uint32_t lo() const { return lo_; }
    uint32_t hi() const { return hi_; }

private:
    SpuCfg() = default;
    void run();
    bool explore_all();
    SpuFunction explore(uint32_t start, SpuOrigin origin, std::vector<uint32_t>& found);
    bool read_table(uint32_t at, uint32_t start, uint32_t bound, SpuJumpTable& out) const;
    spu::Insn insn(uint32_t a) const { return in_code(a) ? decoded_[(a - lo_) / 4] : spu::Insn{}; }
    bool in_code(uint32_t a) const { return a >= lo_ && a < hi_ && a % 4 == 0; }
    void add_start(uint32_t a, SpuOrigin o);
    bool seed_gaps();  // adds the lowest gap, if there is one
    bool validate();
    void account();

    const SpuImage* image_ = nullptr;
    uint32_t lo_ = 0, hi_ = 0;        // the code section
    std::vector<spu::Insn> decoded_;
    std::vector<uint8_t> kind_;       // per word: not reached, code, or a jump table's entry
    std::map<uint32_t, SpuOrigin> starts_;
    std::set<uint32_t> dropped_;      // rejected starts, never seeded again
    std::map<uint32_t, SpuFunction> functions_;
    std::vector<uint32_t> rejected_;
    SpuCoverage coverage_;
    std::map<unsigned, unsigned> read_, written_;
};

// A text report over all of a program's SPU programs.
std::string spu_cfg_summary(const std::vector<SpuCfg>& programs);

}  // namespace qilin

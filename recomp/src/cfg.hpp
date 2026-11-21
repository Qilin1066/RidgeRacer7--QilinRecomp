// Function discovery and recursive-descent control-flow recovery for the
// PPU code of a PS3 program built by GCC.
//
// What the program gives, which makes this simpler than on the Nintendo
// consoles: every function whose address is ever taken has a descriptor
// (a table of them, one section: `.opd`), the C++ unwinding tables
// (`.eh_frame`) give the exact first and last address of every function that
// can unwind, and every instruction is one aligned word. What it has to
// handle: GCC keeps a switch's jump table in the code, right after the
// `bctr` (words that are not instructions), tail calls, functions that
// end in a call that never returns, and code nothing branches to: the
// landing pads the C++ unwinder enters a function at, which only the
// exception tables name.
//
// Pipeline:
//   1. Decode every word of the executable sections. Seed function starts:
//      the entry point, the descriptors, the unwinding tables' ranges, the
//      import stubs, and every direct call target (`bl`).
//   2. Explore each function by recursive descent, bounded by the end its
//      unwinding entry gives, else by the next known start, from its start
//      and from each of its landing pads. A switch is
//      recognised by its idiom (below) and its table read. A branch out of
//      the function is a tail call, and its target a start if it wasn't one.
//      Repeat until no new start turns up.
//   3. Account for every word: code some function reaches, a jump table's
//      entry, padding (zero or `nop`, unreached), dead (instructions straight
//      after a trap that always fires, up to the function's end: the
//      compiler's unreachable epilogue), or unknown.
//
// GCC's switch on this target:
//     cmplwi  crN, rI, N          (and `bgt crN, default`: the bound)
//     lwz     rT, table@toc(rX)   the table's address, from the TOC
//     sldi    rS, rI, 2
//     lwzx    rA, rS, rT          the entry: an offset from the table
//     extsw   rA, rA
//     add     rA, rA, rT
//     mtctr   rA
//     bctr
// The program has one TOC, so the table's address is read from the file.
#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "elf.hpp"
#include "ppu.hpp"

namespace qilin {

// ---- .eh_frame ----
struct UnwindRange {
    uint32_t begin = 0, end = 0;  // [begin, end): one function
    uint32_t lsda = 0;            // its exception table (in `.gcc_except_table`), or 0
};
// Every FDE's range. `address`: where the section is in memory (its pointers
// are relative to themselves). An encoding the reader doesn't know is an error.
std::expected<std::vector<UnwindRange>, std::string> parse_eh_frame(std::span<const uint8_t> section, uint32_t address);
// The landing pads of one function's exception table: where the unwinder
// resumes the function when an exception passes through it (a `catch`, or
// the destructors of its locals). `table`: memory from the table's address on.
std::expected<std::vector<uint32_t>, std::string> parse_exception_table(std::span<const uint8_t> table, uint32_t address, uint32_t function);

// ---- the pass ----
struct JumpTable {
    uint32_t at = 0;                // the bctr
    uint32_t table = 0;             // the table's address
    std::vector<uint32_t> targets;  // in index order
    bool proven = false;            // its size is the bound the code checks the index against
    uint32_t end() const { return table + 4 * static_cast<uint32_t>(targets.size()); }
};

struct SystemCall {
    uint32_t at = 0;                 // the sc
    std::optional<uint32_t> number;  // the constant in r11, if every path to the sc sets the same one
};

// How a function was found, strongest evidence first.
enum class Origin : uint8_t { Entry, Stub, Descriptor, Unwind, Call, TailCall };
const char* origin_name(Origin o);

struct Function {
    uint32_t start = 0;
    uint32_t bound = 0;        // where exploring stopped at the latest: its unwinding entry's end, or the next start
    uint32_t end = 0;          // one past the highest instruction reached
    Origin origin = Origin::Call;
    bool unwind = false;       // has an unwinding entry, so `bound` is its true end
    const ImportFunction* stub = nullptr;  // an import's stub: a call to it is a call to the system
    uint32_t words = 0;        // instructions reached
    std::set<uint32_t> calls;  // direct call targets (bl)
    std::set<uint32_t> tails;  // branch targets outside the function
    std::vector<JumpTable> tables;
    std::vector<SystemCall> system_calls;
    std::vector<uint32_t> landing_pads;    // where the unwinder may enter it
    std::vector<uint32_t> indirect_calls;  // bctrl, blrl
    std::vector<uint32_t> indirect_tails;  // bctr that isn't a switch
    std::vector<uint32_t> bad;             // reached a word that doesn't decode, or left the code
    bool runs_off = false;                 // reaches its bound without a terminator (ends in a call that doesn't return, or runs into the next function)
};

struct Coverage {
    uint64_t words = 0, code = 0, tables = 0, padding = 0, dead = 0, unknown = 0;
    std::vector<std::pair<uint32_t, uint32_t>> unknown_spans;  // [lo, hi)
};

class Cfg {
public:
    // Fails if the program's own tables (descriptors, unwinding) can't be found or read.
    static std::expected<Cfg, std::string> build(const Elf& elf);

    const std::map<uint32_t, Function>& functions() const { return functions_; }
    const Coverage& coverage() const { return coverage_; }
    uint32_t toc() const { return toc_; }
    size_t descriptors() const { return descriptors_; }
    size_t unwind_ranges() const { return unwind_.size(); }
    size_t exception_tables() const { return pads_.size(); }
    int rounds() const { return rounds_; }
    // A text report: counts, the system calls made, and everything that needs looking at.
    std::string summary() const;

private:
    explicit Cfg(const Elf& elf) : elf_(&elf) {}
    std::optional<std::string> prepare();
    void run();
    Function explore(uint32_t start, Origin origin, std::vector<uint32_t>& found);
    bool read_table(uint32_t at, uint32_t start, uint32_t bound, JumpTable& out) const;
    ppu::Insn insn(uint32_t a) const;
    bool in_code(uint32_t a) const;
    uint32_t bound_of(uint32_t start) const;
    void number_system_calls(Function& f) const;
    void add_start(uint32_t a, Origin o);
    void account();

    const Elf* elf_;
    uint32_t lo_ = 0, hi_ = 0;        // the span of the executable sections
    std::vector<ppu::Insn> decoded_;  // one per word of [lo_, hi_)
    std::vector<uint8_t> kind_;       // per word: kNone, kCode, kTable, or kGap (between sections)
    uint32_t toc_ = 0;
    size_t descriptors_ = 0;
    std::map<uint32_t, uint32_t> unwind_;  // begin -> end
    std::map<uint32_t, std::vector<uint32_t>> pads_;  // a function's landing pads
    std::map<uint32_t, Origin> starts_;
    std::map<uint32_t, const ImportFunction*> stubs_;
    std::map<uint32_t, Function> functions_;
    Coverage coverage_;
    int rounds_ = 0;
};

}  // namespace qilin

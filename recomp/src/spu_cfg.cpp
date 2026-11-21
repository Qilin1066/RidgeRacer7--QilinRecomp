#include "spu_cfg.hpp"

#include <algorithm>
#include <format>

#include "bytes.hpp"

namespace qilin {

using spu::Insn;
using spu::Op;

namespace {

enum : uint8_t { kNone, kCode, kTable };

// Alignment filler: nop (any register: the field is ignored), lnop, or zero.
bool padding(const Insn& in) { return in.word == 0 || in.op == Op::lnop || in.op == Op::nop; }

// Nothing runs on past it: a return, a jump, an interrupt return.
bool ends_flow(const Insn& in) { return in.op == Op::br || in.op == Op::bra || in.op == Op::bi || in.op == Op::iret; }

}  // namespace

const char* spu_origin_name(SpuOrigin o) {
    switch (o) {
    case SpuOrigin::Entry: return "the entry point";
    case SpuOrigin::Call: return "direct calls";
    case SpuOrigin::TailCall: return "tail calls";
    case SpuOrigin::Pointer: return "addresses loaded by ila";
    case SpuOrigin::DataPointer: return "addresses in data";
    case SpuOrigin::Gap: return "nothing: unreferenced code at a clean boundary";
    }
    return "";
}

void SpuCfg::add_start(uint32_t a, SpuOrigin o) {
    if (dropped_.contains(a)) return;
    const auto [it, added] = starts_.try_emplace(a, o);
    if (!added && o < it->second) it->second = o;
}

std::expected<SpuCfg, std::string> SpuCfg::build(const SpuImage& image, std::span<const uint8_t> file) {
    SpuCfg cfg;
    cfg.image_ = &image;
    const auto fail = [&](std::string_view what) { return std::unexpected(std::format("SPU program {}: {}", image.name, what)); };
    // The code: the executable section that isn't the 16-byte identifier before it.
    const SpuSection* text = nullptr;
    for (const SpuSection& s : image.sections) {
        if (s.type != kShtProgbits || !(s.flags & kShfExec) || s.name == ".SpuGUID" || s.size == 0) continue;
        if (text) return fail("more than one code section");
        text = &s;
    }
    if (!text) return fail("no code section");
    if (uint64_t{text->offset} + text->size > file.size()) return fail("its code isn't in the file");
    const auto bytes = file.subspan(text->offset, text->size);
    cfg.lo_ = text->address;
    cfg.hi_ = text->address + text->size;
    cfg.decoded_.resize(text->size / 4);
    for (size_t i = 0; i < cfg.decoded_.size(); ++i) cfg.decoded_[i] = spu::decode(read_be32(bytes.data() + 4 * i));
    cfg.kind_.assign(cfg.decoded_.size(), kNone);
    if (!cfg.in_code(image.entry)) return fail("its entry point isn't in its code");

    cfg.add_start(image.entry, SpuOrigin::Entry);
    for (uint32_t a = cfg.lo_; a < cfg.hi_; a += 4) {
        const Insn in = cfg.insn(a);
        if (in.op == Op::brsl && cfg.in_code(in.relative(a))) cfg.add_start(in.relative(a), SpuOrigin::Call);
        if (in.op == Op::brasl && cfg.in_code(in.absolute())) cfg.add_start(in.absolute(), SpuOrigin::Call);
        if (in.op == Op::ila && cfg.in_code(in.u18())) cfg.add_start(in.u18(), SpuOrigin::Pointer);
        if (in.op == Op::rdch || in.op == Op::rchcnt) ++cfg.read_[in.ra()];
        if (in.op == Op::wrch) ++cfg.written_[in.ra()];
    }
    for (const SpuSection& s : image.sections) {
        if (s.type != kShtProgbits || !(s.flags & kShfAlloc) || (s.flags & kShfExec)) continue;
        if (uint64_t{s.offset} + s.size > file.size()) return fail("its data isn't in the file");
        const auto data = file.subspan(s.offset, s.size);
        for (size_t i = 0; i + 4 <= data.size(); i += 4)
            if (const uint32_t word = read_be32(data.data() + i); cfg.in_code(word)) cfg.add_start(word, SpuOrigin::DataPointer);
    }
    cfg.run();
    cfg.account();
    return cfg;
}

// The switch whose `bi` is at `at`: after the bi and its padding, at the
// next 16-byte boundary, a run of addresses inside the function.
bool SpuCfg::read_table(uint32_t at, uint32_t start, uint32_t bound, SpuJumpTable& out) const {
    uint32_t table = at + 4;
    while (table < bound && table % 16 && padding(insn(table))) table += 4;
    if (table % 16) return false;
    out = SpuJumpTable{at, table, {}};
    for (uint32_t entry = table; entry < bound; entry += 4) {
        if (entry != table && starts_.contains(entry)) break;
        const uint32_t to = insn(entry).word;
        if (to % 4 || to < start || to >= bound || (to >= table && to <= entry)) break;
        out.targets.push_back(to);
    }
    return out.targets.size() >= 2;
}

SpuFunction SpuCfg::explore(uint32_t start, SpuOrigin origin, std::vector<uint32_t>& found) {
    SpuFunction f;
    f.start = f.end = start;
    f.origin = origin;
    f.bound = hi_;
    if (const auto next = starts_.upper_bound(start); next != starts_.end()) f.bound = next->first;
    std::vector<uint32_t> work{start};
    const auto leave = [&](uint32_t to) {
        if (to >= start && to < f.bound) {
            work.push_back(to);
            return;
        }
        f.tails.insert(to);
        if (in_code(to)) found.push_back(to);
        else f.bad.push_back(to);
    };
    while (!work.empty()) {
        uint32_t pc = work.back();
        work.pop_back();
        for (;; pc += 4) {
            if (pc >= f.bound) {
                f.runs_off = true;
                break;
            }
            uint8_t& kind = kind_[(pc - lo_) / 4];
            if (kind == kCode) break;
            const Insn in = decoded_[(pc - lo_) / 4];
            if (kind != kNone || !in.valid()) {  // a table's entry, or not an instruction
                f.bad.push_back(pc);
                break;
            }
            kind = kCode;
            ++f.words;
            f.end = std::max(f.end, pc + 4);
            const uint32_t flags = spu::spec(in.op).flags;
            if (in.op == Op::stop || in.op == Op::stopd) f.stops.push_back(pc);
            // `stop` with signal type 0 is where the start-up code ends, after `exit` (which doesn't
            // return): nothing runs on. Any other stop is a service the program resumes after.
            if (in.word == 0) break;
            if (!(flags & spu::kBranch)) continue;
            if (flags & spu::kCall) {
                if (flags & spu::kIndirect) f.indirect_calls.push_back(pc);
                else {
                    const uint32_t to = in.op == Op::brsl ? in.relative(pc) : in.absolute();
                    f.calls.insert(to);
                    if (!in_code(to)) f.bad.push_back(pc);
                }
                continue;
            }
            if (!(flags & spu::kIndirect)) {  // br, bra, and the conditional four
                leave(in.op == Op::bra ? in.absolute() : in.relative(pc));
                if (flags & spu::kConditional) continue;
                break;
            }
            if (flags & spu::kConditional) continue;  // biz and its kin: a conditional return, or a jump through a register
            if (in.op == Op::bi && in.ra() != 0) {
                SpuJumpTable table;
                if (read_table(pc, start, f.bound, table)) {
                    for (uint32_t entry = table.table; entry < table.table + 4 * table.targets.size(); entry += 4) {
                        uint8_t& k = kind_[(entry - lo_) / 4];
                        if (k == kCode) f.bad.push_back(entry);
                        else k = kTable;
                    }
                    for (const uint32_t to : table.targets) work.push_back(to);
                    f.tables.push_back(std::move(table));
                } else {
                    f.indirect_tails.push_back(pc);
                }
            }
            break;  // bi, iret
        }
    }
    std::ranges::sort(f.indirect_calls);
    std::ranges::sort(f.indirect_tails);
    std::ranges::sort(f.stops);
    std::ranges::sort(f.bad);
    return f;
}

// Explores every start until no branch out of a function finds a new one.
bool SpuCfg::explore_all() {
    bool any = false;
    for (;;) {
        functions_.clear();
        std::ranges::fill(kind_, uint8_t{kNone});
        std::vector<uint32_t> found;
        for (const auto& [start, origin] : starts_) functions_.emplace(start, explore(start, origin, found));
        bool changed = false;
        for (const uint32_t to : found)
            if (!starts_.contains(to) && !dropped_.contains(to)) {
                add_start(to, SpuOrigin::TailCall);
                changed = true;
            }
        if (!changed) return any;
        any = true;
    }
}

// Unreached code at a clean boundary: after an instruction nothing runs on past, and any padding.
bool SpuCfg::seed_gaps() {
    for (uint32_t a = lo_; a < hi_; a += 4) {
        const size_t i = (a - lo_) / 4;
        if (kind_[i] != kNone || padding(decoded_[i]) || !decoded_[i].valid() || starts_.contains(a) || dropped_.contains(a)) continue;
        if (a == lo_) continue;
        // The instruction before it, past any padding (the compiler pads inside functions too,
        // so padding alone proves nothing): it must be one nothing runs on past.
        size_t before = i - 1;
        while (before > 0 && padding(decoded_[before])) --before;
        if (kind_[before] == kTable || !ends_flow(decoded_[before])) continue;
        // One at a time, the lowest first: the code after it is most likely its own, and exploring
        // it says how far it goes.
        add_start(a, SpuOrigin::Gap);
        return true;
    }
    return false;
}

// A start on weak evidence whose function reaches a word that isn't an instruction was not a function.
bool SpuCfg::validate() {
    bool any = false;
    for (const auto& [start, f] : functions_)
        if (!f.bad.empty() && !spu_strong(f.origin)) {
            starts_.erase(start);
            dropped_.insert(start);
            rejected_.push_back(start);
            any = true;
        }
    return any;
}

void SpuCfg::run() {
    for (;;) {
        explore_all();
        if (validate()) {
            // What was found from the functions as they then stood is found again.
            std::erase_if(starts_, [](const auto& s) { return s.second == SpuOrigin::TailCall || s.second == SpuOrigin::Gap; });
            continue;
        }
        if (!seed_gaps()) break;
    }
    std::ranges::sort(rejected_);
}

void SpuCfg::account() {
    coverage_ = {};
    bool open = false;
    uint32_t span = 0;
    for (uint32_t a = lo_; a < hi_; a += 4) {
        const size_t i = (a - lo_) / 4;
        ++coverage_.words;
        bool unknown = false;
        if (kind_[i] == kCode) ++coverage_.code;
        else if (kind_[i] == kTable) ++coverage_.tables;
        else if (padding(decoded_[i])) ++coverage_.padding;
        else unknown = true;
        if (unknown) {
            ++coverage_.unknown;
            if (!open) span = a;
            open = true;
        } else if (open) {
            coverage_.unknown_spans.emplace_back(span, a);
            open = false;
        }
    }
    if (open) coverage_.unknown_spans.emplace_back(span, hi_);
}

std::string spu_cfg_summary(const std::vector<SpuCfg>& programs) {
    std::string out;
    const auto line = [&](std::string text) { out += text + "\n"; };
    std::string first_bad;
    size_t by_origin[6] = {}, functions = 0, calls = 0, indirect_calls = 0, indirect_tails = 0, tables = 0, entries = 0, stops = 0, bad = 0,
           runs_off = 0, rejected = 0;
    SpuCoverage all;
    std::map<unsigned, unsigned> read, written;
    for (const SpuCfg& p : programs) {
        size_t own[6] = {}, own_bad = 0, own_tails = 0;
        for (const auto& [start, f] : p.functions()) {
            ++own[static_cast<size_t>(f.origin)];
            calls += f.calls.size();
            indirect_calls += f.indirect_calls.size();
            own_tails += f.indirect_tails.size();
            tables += f.tables.size();
            for (const SpuJumpTable& t : f.tables) entries += t.targets.size();
            stops += f.stops.size();
            own_bad += f.bad.size();
            if (!f.bad.empty() && first_bad.size() < 400) first_bad += std::format(" {}:{:#x} (function {:#x}, found by {})", p.image().name, f.bad[0], start, spu_origin_name(f.origin));
            runs_off += f.runs_off;
        }
        for (size_t o = 0; o < 6; ++o) by_origin[o] += own[o];
        functions += p.functions().size();
        indirect_tails += own_tails;
        bad += own_bad;
        rejected += p.rejected().size();
        const SpuCoverage& c = p.coverage();
        all.words += c.words;
        all.code += c.code;
        all.tables += c.tables;
        all.padding += c.padding;
        all.unknown += c.unknown;
        for (const auto& [channel, n] : p.channels_read()) read[channel] += n;
        for (const auto& [channel, n] : p.channels_written()) written[channel] += n;
        std::string notes;
        if (c.unknown) {
            notes += std::format("  {} unknown:", c.unknown);
            for (size_t i = 0; i < c.unknown_spans.size() && i < 6; ++i) notes += std::format(" {:#x}-{:#x}", c.unknown_spans[i].first, c.unknown_spans[i].second);
        }
        if (own_bad) notes += std::format("  {} bad", own_bad);
        if (own_tails) notes += std::format("  {} indirect jumps", own_tails);
        line(std::format("{:32} {:4} functions ({:3} called, {:3} unreferenced)  {:6} code {:5} padding{}", p.image().name, p.functions().size(),
                         own[0] + own[1] + own[2], own[5], c.code, c.padding, notes));
    }
    line(std::format("{} SPU programs, {} functions", programs.size(), functions));
    for (size_t o = 0; o < 6; ++o)
        if (by_origin[o]) line(std::format("  {:6} from {}", by_origin[o], spu_origin_name(static_cast<SpuOrigin>(o))));
    line(std::format("{} starts on weak evidence were not functions and were dropped", rejected));
    line(std::format("{} direct call sites' targets, {} indirect calls, {} switches with {} entries, {} indirect jumps that are neither a return nor a switch",
                     calls, indirect_calls, tables, entries, indirect_tails));
    line(std::format("{} stop instructions reached; {} functions reach their end without a return or jump", stops, runs_off));
    std::string channels = "channels read:";
    for (const auto& [channel, n] : read) channels += std::format(" {}x{}", channel, n);
    line(channels);
    channels = "channels written:";
    for (const auto& [channel, n] : written) channels += std::format(" {}x{}", channel, n);
    line(channels);
    line(std::format("{} places where a function reaches a word that isn't an instruction, or leaves the code:{}", bad, first_bad));
    line(std::format("{} words: {} code, {} jump-table entries, {} padding, {} unknown", all.words, all.code, all.tables, all.padding, all.unknown));
    return out;
}

}  // namespace qilin

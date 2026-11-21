#include "cfg.hpp"

#include <algorithm>
#include <format>

#include "bytes.hpp"
#include "syscall_names.hpp"

namespace qilin {

using ppu::Insn;
using ppu::Op;

namespace {

enum : uint8_t { kGap, kNone, kCode, kTable };

constexpr uint32_t kNop = 0x60000000;  // ori r0,r0,0
constexpr unsigned kCtr = 9;           // the count register's number, for mtspr

// ---- DWARF's exception-handling pointer encodings ----
constexpr uint8_t kEncAbsolute = 0x00, kEncPcRelative = 0x10, kEncIndirect = 0x80, kEncOmit = 0xff;

// The size of a pointer of this encoding's format (0: one the reader doesn't know).
size_t encoded_size(uint8_t encoding) {
    switch (encoding & 0x0f) {
    case 0x00: return 8;  // a native pointer: this is a 64-bit ELF
    case 0x02: case 0x0a: return 2;
    case 0x03: case 0x0b: return 4;
    case 0x04: case 0x0c: return 8;
    default: return 0;
    }
}

uint64_t read_encoded(const uint8_t* p, uint8_t encoding) {
    switch (encoding & 0x0f) {
    case 0x02: return read_be16(p);
    case 0x0a: return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(read_be16(p))));
    case 0x03: return read_be32(p);
    case 0x0b: return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(read_be32(p))));
    default: return read_be64(p);
    }
}

bool read_uleb(std::span<const uint8_t> s, size_t& at, uint64_t& value);

// A value of any encoding's format, LEB128 among them, at `at`, which moves past it.
bool read_value(std::span<const uint8_t> s, size_t& at, uint8_t encoding, uint64_t& value) {
    const uint8_t format = encoding & 0x0f;
    if (format == 0x01) return read_uleb(s, at, value);
    if (format == 0x09) {  // signed LEB128
        value = 0;
        unsigned shift = 0;
        uint8_t byte = 0x80;
        while ((byte & 0x80) && at < s.size() && shift < 64) {
            byte = s[at++];
            value |= uint64_t{byte & 0x7fu} << shift;
            shift += 7;
        }
        if (byte & 0x80) return false;
        if ((byte & 0x40) && shift < 64) value |= ~uint64_t{0} << shift;
        return true;
    }
    const size_t size = encoded_size(encoding);
    if (size == 0 || at + size > s.size()) return false;
    value = read_encoded(s.data() + at, encoding);
    at += size;
    return true;
}

bool read_uleb(std::span<const uint8_t> s, size_t& at, uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; at < s.size() && shift < 64; shift += 7) {
        const uint8_t byte = s[at++];
        value |= uint64_t{byte & 0x7fu} << shift;
        if (!(byte & 0x80)) return true;
    }
    return false;
}

std::unexpected<std::string> eh_error(size_t at, std::string_view what) {
    return std::unexpected(std::format("unwinding tables: at {:#x}: {}", at, what));
}

}  // namespace

std::expected<std::vector<UnwindRange>, std::string> parse_eh_frame(std::span<const uint8_t> s, uint32_t address) {
    std::vector<UnwindRange> ranges;
    struct Cie {
        uint8_t addresses = kEncAbsolute, lsda = kEncOmit;  // how its FDEs encode each
        bool sized = false;                                  // its FDEs have augmentation data
    };
    std::map<size_t, Cie> cies;  // by offset
    size_t at = 0;
    while (at + 4 <= s.size()) {
        const uint32_t length = read_be32(s.data() + at);
        if (length == 0) break;  // the terminator
        if (length == 0xffffffff) return eh_error(at, "a 64-bit record");
        const size_t body = at + 4, end = body + length;
        if (length < 4 || end > s.size()) return eh_error(at, "a record that runs past the section");
        const uint32_t id = read_be32(s.data() + body);
        if (id == 0) {  // a CIE
            size_t p = body + 4;
            if (p >= end) return eh_error(at, "an empty CIE");
            const uint8_t version = s[p++];
            if (version != 1 && version != 3) return eh_error(at, std::format("a CIE of version {}", version));
            const size_t text = p;
            while (p < end && s[p]) ++p;
            if (p >= end) return eh_error(at, "a CIE whose augmentation has no end");
            const std::string_view augmentation(reinterpret_cast<const char*>(s.data() + text), p - text);
            ++p;
            uint64_t skipped = 0;
            if (!read_uleb(s.first(end), p, skipped) || !read_uleb(s.first(end), p, skipped)) return eh_error(at, "a CIE cut short");
            if (version == 1) ++p;
            else if (!read_uleb(s.first(end), p, skipped)) return eh_error(at, "a CIE cut short");
            Cie cie;
            uint8_t& encoding = cie.addresses;
            if (!augmentation.empty()) {
                cie.sized = true;
                if (augmentation[0] != 'z') return eh_error(at, std::format("a CIE with the augmentation \"{}\"", augmentation));
                if (!read_uleb(s.first(end), p, skipped)) return eh_error(at, "a CIE cut short");
                for (const char letter : augmentation.substr(1)) {
                    if (p >= end) return eh_error(at, "a CIE cut short");
                    if (letter == 'R') encoding = s[p++];
                    else if (letter == 'L') cie.lsda = s[p++];
                    else if (letter == 'P') {
                        const uint8_t personality = s[p++];
                        const size_t size = encoded_size(personality);
                        if (size == 0) return eh_error(at, std::format("a personality pointer encoded as {:#x}", personality));
                        p += size;
                    } else if (letter != 'S') return eh_error(at, std::format("a CIE with the augmentation \"{}\"", augmentation));
                }
                if (p > end) return eh_error(at, "a CIE cut short");
            }
            const uint8_t how = encoding & 0x70;
            if (encoding == kEncOmit || (encoding & kEncIndirect) || encoded_size(encoding) == 0 || (how != kEncAbsolute && how != kEncPcRelative))
                return eh_error(at, std::format("addresses encoded as {:#x}", encoding));
            if (cie.lsda != kEncOmit && ((cie.lsda & kEncIndirect) || encoded_size(cie.lsda) == 0))
                return eh_error(at, std::format("exception tables' addresses encoded as {:#x}", cie.lsda));
            cies[at] = cie;
        } else {  // an FDE: `id` is how far back its CIE starts
            if (id > body) return eh_error(at, "an FDE whose CIE is before the section");
            const auto cie = cies.find(body - id);
            if (cie == cies.end()) return eh_error(at, "an FDE whose CIE wasn't read");
            const uint8_t encoding = cie->second.addresses;
            const size_t size = encoded_size(encoding), p = body + 4;
            if (p + 2 * size > end) return eh_error(at, "an FDE cut short");
            uint64_t begin = read_encoded(s.data() + p, encoding);
            if ((encoding & 0x70) == kEncPcRelative) begin += uint64_t{address} + p;
            const uint64_t size_of = read_encoded(s.data() + p + size, encoding & 0x0f);
            begin &= 0xffffffffu;  // an LV2 process's addresses are 32 bits wide
            if (size_of == 0 || begin + size_of > 0xffffffffu) return eh_error(at, std::format("an FDE for {:#x} bytes at {:#x}", size_of, begin));
            UnwindRange range{static_cast<uint32_t>(begin), static_cast<uint32_t>(begin + size_of), 0};
            if (cie->second.sized && cie->second.lsda != kEncOmit) {
                size_t q = p + 2 * size;
                uint64_t skipped = 0, lsda = 0;
                if (!read_uleb(s.first(end), q, skipped)) return eh_error(at, "an FDE cut short");
                const size_t field = q;
                if (!read_value(s.first(end), q, cie->second.lsda, lsda)) return eh_error(at, "an FDE cut short");
                if (lsda != 0) {
                    if ((cie->second.lsda & 0x70) == kEncPcRelative) lsda += uint64_t{address} + field;
                    else if ((cie->second.lsda & 0x70) != kEncAbsolute) return eh_error(at, std::format("an exception table's address encoded as {:#x}", cie->second.lsda));
                    range.lsda = static_cast<uint32_t>(lsda);
                }
            }
            ranges.push_back(range);
        }
        at = end;
    }
    return ranges;
}

std::expected<std::vector<uint32_t>, std::string> parse_exception_table(std::span<const uint8_t> s, uint32_t address, uint32_t function) {
    const auto fail = [&](std::string_view what) {
        return std::unexpected(std::format("the exception table at {:#x} (of the function at {:#x}): {}", address, function, what));
    };
    std::vector<uint32_t> pads;
    size_t at = 0;
    uint64_t value = 0;
    if (s.size() < 4) return fail("cut short");
    uint64_t base = function;  // what landing pads are measured from
    if (const uint8_t encoding = s[at++]; encoding != kEncOmit) {
        const size_t field = at;
        if ((encoding & kEncIndirect) || !read_value(s, at, encoding, base)) return fail(std::format("a base encoded as {:#x}", encoding));
        if ((encoding & 0x70) == kEncPcRelative) base += uint64_t{address} + field;
    }
    if (at >= s.size()) return fail("cut short");
    if (s[at++] != kEncOmit && !read_uleb(s, at, value)) return fail("cut short");  // the types' table: not needed
    if (at >= s.size()) return fail("cut short");
    const uint8_t encoding = s[at++];
    uint64_t length = 0;
    if (!read_uleb(s, at, length) || at + length > s.size()) return fail("its call sites run past the section");
    if ((encoding & 0x70) != kEncAbsolute) return fail(std::format("call sites encoded as {:#x}", encoding));
    const auto sites = s.first(at + static_cast<size_t>(length));
    while (at < sites.size()) {
        uint64_t start = 0, size = 0, pad = 0, action = 0;
        if (!read_value(sites, at, encoding, start) || !read_value(sites, at, encoding, size) || !read_value(sites, at, encoding, pad) ||
            !read_uleb(sites, at, action))
            return fail(std::format("a call site cut short, or encoded as {:#x}", encoding));
        if (pad != 0) pads.push_back(static_cast<uint32_t>(base + pad));
    }
    std::ranges::sort(pads);
    pads.erase(std::unique(pads.begin(), pads.end()), pads.end());
    return pads;
}

const char* origin_name(Origin o) {
    switch (o) {
    case Origin::Entry: return "the entry point";
    case Origin::Stub: return "import stubs";
    case Origin::Descriptor: return "descriptors";
    case Origin::Unwind: return "unwinding tables";
    case Origin::Call: return "direct calls";
    case Origin::TailCall: return "tail calls";
    }
    return "";
}

// ---------------------------------------------------------------- set-up

std::expected<Cfg, std::string> Cfg::build(const Elf& elf) {
    Cfg cfg(elf);
    if (auto error = cfg.prepare()) return std::unexpected(std::move(*error));
    cfg.run();
    cfg.account();
    return cfg;
}

ppu::Insn Cfg::insn(uint32_t a) const { return in_code(a) ? decoded_[(a - lo_) / 4] : Insn{}; }

bool Cfg::in_code(uint32_t a) const { return a >= lo_ && a < hi_ && a % 4 == 0 && kind_[(a - lo_) / 4] != kGap; }

void Cfg::add_start(uint32_t a, Origin o) {
    const auto [it, added] = starts_.try_emplace(a, o);
    if (!added && o < it->second) it->second = o;
}

std::optional<std::string> Cfg::prepare() {
    const Elf& elf = *elf_;
    // The code: the executable sections, or the executable segments of a program without sections.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (const ElfSection& s : elf.sections())
        if (s.type == kShtProgbits && (s.flags & kShfAlloc) && (s.flags & kShfExec) && s.size >= 4)
            ranges.emplace_back(static_cast<uint32_t>(s.address), static_cast<uint32_t>(s.address + (s.size & ~uint64_t{3})));
    if (ranges.empty())
        for (const ElfSegment& s : elf.segments())
            if (s.type == kPtLoad && (s.flags & kPfExec) && s.file_size >= 4)
                ranges.emplace_back(static_cast<uint32_t>(s.address), static_cast<uint32_t>(s.address + (s.file_size & ~uint64_t{3})));
    if (ranges.empty()) return "the program has no code";
    std::ranges::sort(ranges);
    lo_ = ranges.front().first;
    hi_ = ranges.back().second;
    decoded_.assign((hi_ - lo_) / 4, Insn{});
    kind_.assign(decoded_.size(), kGap);
    for (const auto& [lo, hi] : ranges) {
        if (lo % 4) return std::format("the code at {:#x} isn't aligned to a word", lo);
        const auto bytes = elf.bytes_at(lo, hi - lo);
        if (bytes.empty()) return std::format("the code at {:#x} isn't in the file", lo);
        for (uint32_t a = lo; a < hi; a += 4) {
            decoded_[(a - lo_) / 4] = ppu::decode(read_be32(bytes.data() + (a - lo)));
            kind_[(a - lo_) / 4] = kNone;
        }
    }

    toc_ = elf.entry_toc();
    if (!in_code(elf.entry_code())) return std::format("the entry point {:#x} isn't in the code", elf.entry_code());
    add_start(elf.entry_code(), Origin::Entry);

    // The descriptors: the section holding the entry point's, if all of it is
    // descriptors (pairs of a code address and the one TOC).
    for (const ElfSection& s : elf.sections()) {
        const uint64_t entry = elf.entry_descriptor();
        if (s.type != kShtProgbits || entry < s.address || entry - s.address >= s.size || s.size % 8) continue;
        const auto bytes = elf.bytes_at(s.address, s.size);
        if (bytes.empty()) continue;
        std::vector<uint32_t> codes;
        for (size_t i = 0; i < bytes.size(); i += 8) {
            const uint32_t code = read_be32(bytes.data() + i), toc = read_be32(bytes.data() + i + 4);
            if (!in_code(code) || toc != toc_) {
                codes.clear();
                break;
            }
            codes.push_back(code);
        }
        descriptors_ = codes.size();
        for (const uint32_t code : codes) add_start(code, Origin::Descriptor);
    }

    // The unwinding tables: the section so named, or (the names may be
    // missing) the read-only one that begins with a CIE.
    for (const ElfSection& s : elf.sections()) {
        if (s.type != kShtProgbits || !(s.flags & kShfAlloc) || (s.flags & (kShfExec | kShfWrite)) || s.size < 16) continue;
        const auto bytes = elf.bytes_at(s.address, s.size);
        if (bytes.empty()) continue;
        const bool named = s.name == ".eh_frame";
        const bool looks = read_be32(bytes.data()) >= 8 && read_be32(bytes.data() + 4) == 0 && bytes[8] == 1 && bytes[9] == 'z';
        if (!named && !(s.name.empty() && looks)) continue;
        const auto ranges_found = parse_eh_frame(bytes, static_cast<uint32_t>(s.address));
        if (!ranges_found) return ranges_found.error();
        for (const UnwindRange& r : *ranges_found) {
            if (!in_code(r.begin) || r.end % 4 || r.end > hi_) return std::format("unwinding tables: a function at {:#x}-{:#x}, which isn't code", r.begin, r.end);
            unwind_[r.begin] = r.end;
            add_start(r.begin, Origin::Unwind);
            if (!r.lsda) continue;
            // The exception table: read from its address to the end of the section it is in.
            std::span<const uint8_t> table;
            for (const ElfSection& t : elf.sections())
                if (t.type == kShtProgbits && r.lsda >= t.address && r.lsda - t.address < t.size)
                    table = elf.bytes_at(r.lsda, t.address + t.size - r.lsda);
            if (table.empty()) return std::format("the function at {:#x} has its exception table at {:#x}, which is in no section", r.begin, r.lsda);
            auto pads = parse_exception_table(table, r.lsda, r.begin);
            if (!pads) return pads.error();
            for (const uint32_t pad : *pads)
                if (pad < r.begin || pad >= r.end || pad % 4)
                    return std::format("the function at {:#x}-{:#x} has a landing pad at {:#x}", r.begin, r.end, pad);
            pads_[r.begin] = std::move(*pads);
        }
        break;
    }

    for (const ImportLibrary& library : elf.imports())
        for (const ImportFunction& f : library.functions)
            if (f.stub && in_code(f.initial)) {
                stubs_[f.initial] = &f;
                add_start(f.initial, Origin::Stub);
            }

    // Every direct call's target.
    for (uint32_t a = lo_; a < hi_; a += 4) {
        const Insn in = decoded_[(a - lo_) / 4];
        if (in.op == Op::b && in.lk() && in_code(in.branch_target(a))) add_start(in.branch_target(a), Origin::Call);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- exploring

uint32_t Cfg::bound_of(uint32_t start) const {
    uint32_t bound = hi_;
    if (const auto next = starts_.upper_bound(start); next != starts_.end()) bound = next->first;
    if (const auto u = unwind_.find(start); u != unwind_.end()) bound = std::min(bound, u->second);
    return bound;
}

// The switch whose `bctr` is at `at`: the idiom in cfg.hpp, on the straight line into it.
bool Cfg::read_table(uint32_t at, uint32_t start, uint32_t bound, JumpTable& out) const {
    if (at < start + 16) return false;
    const Insn move = insn(at - 4), sum = insn(at - 8);
    if (move.op != Op::mtspr || move.spr() != kCtr) return false;
    const unsigned target = move.rd();
    if (sum.op != Op::add || sum.rc() || sum.oe() || sum.rd() != target) return false;
    unsigned base;
    if (sum.ra() == target) base = sum.rb();
    else if (sum.rb() == target) base = sum.ra();
    else return false;
    uint32_t a = at - 12;
    if (insn(a).op == Op::extsw && insn(a).ra() == target && insn(a).rd() == target) a -= 4;
    const Insn load = insn(a);
    if ((load.op != Op::lwzx && load.op != Op::lwax) || load.rd() != target || (load.ra() != base && load.rb() != base)) return false;
    // The table's address: the nearest `lwz base, d(rX)` above, read from the TOC.
    uint32_t table = 0, table_load = 0;
    for (uint32_t back = a - 4; back >= start && back + 40 >= a; back -= 4) {
        const Insn in = insn(back);
        if (!in.valid() || (ppu::spec(in.op).flags & ppu::kBranch)) return false;
        if (in.op == Op::lwz && in.rd() == base) {
            // From the TOC itself (r2), or from the file's own table of
            // constants, whose address the function took from the TOC into
            // a register it keeps (`lwz rX, d(r2)`, once, in its prologue).
            uint32_t from = toc_;
            if (in.ra() != 2) {
                std::optional<uint32_t> kept;
                for (uint32_t above = start; above < back; above += 4) {
                    const Insn set = insn(above);
                    if (set.op != Op::lwz || set.rd() != in.ra() || set.ra() != 2) continue;
                    const auto value = elf_->read32(toc_ + static_cast<uint32_t>(set.simm()));
                    if (!value || (kept && *kept != *value)) return false;
                    kept = value;
                }
                if (!kept) return false;
                from = *kept;
            }
            const auto slot = elf_->read32(from + static_cast<uint32_t>(in.simm()));
            if (!slot) return false;
            table = *slot;
            table_load = back;
            break;
        }
        if (back == start) break;
    }
    if (!table_load || table % 4 || table < start || table >= bound) return false;

    // The bound: `cmplwi crN, rI, n`, and a branch on crN's "greater" bit.
    // The nearest compare into that CR field above a branch, on its straight line.
    const auto bound_above = [&](uint32_t branch, unsigned field) -> std::optional<uint32_t> {
        for (uint32_t above = branch - 4; above >= start && above + 32 >= branch; above -= 4) {
            const Insn compare = insn(above);
            if (!compare.valid() || (ppu::spec(compare.op).flags & ppu::kBranch)) break;
            const bool compares = compare.op == Op::cmpli || compare.op == Op::cmpi || compare.op == Op::cmpl || compare.op == Op::cmp;
            if (compares && compare.crfd() == field) {
                if (compare.op == Op::cmpli && !compare.l()) return compare.uimm() + 1;
                break;
            }
            if (above == start) break;
        }
        return std::nullopt;
    };
    const auto on_greater = [](const Insn& in, unsigned options) { return in.op == Op::bc && !in.lk() && (in.rd() & ~3u) == options && in.ra() % 4 == 1; };
    // Either `bgt crN, default` on the straight line into the switch ...
    uint32_t checked = 0;  // how many entries the code allows; 0: no check found
    uint32_t head = table_load;  // the first instruction of that straight line
    for (uint32_t back = table_load - 4; back >= start; back -= 4) {
        const Insn in = insn(back);
        if (on_greater(in, 12)) checked = bound_above(back, in.ra() / 4).value_or(0);
        if (!in.valid() || (ppu::spec(in.op).flags & ppu::kBranch)) break;
        head = back;
        if (back == start) break;
    }
    // ... or the switch is a block of its own, entered only by `ble crN` after the compare.
    const Insn before = insn(head - 4);
    const bool own_block = head > start && !before.lk() && (before.op == Op::b || ((before.op == Op::bclr || before.op == Op::bcctr || before.op == Op::bc) && (before.rd() & 0x14) == 0x14));
    if (!checked && own_block) {
        bool agreed = true;
        for (uint32_t from = start; from < bound && agreed; from += 4) {
            const Insn in = insn(from);
            if ((in.op != Op::b && in.op != Op::bc) || in.lk()) continue;
            const uint32_t to = in.branch_target(from);
            if (to < head || to > table_load) continue;
            const auto n = on_greater(in, 4) ? bound_above(from, in.ra() / 4) : std::nullopt;
            if (!n || (checked && checked != *n)) agreed = false;
            checked = n.value_or(0);
        }
        if (!agreed) checked = 0;
    }

    // Entries are offsets from the table. Its end, where the code doesn't
    // say: the first address some entry jumps to past the table's start.
    out = JumpTable{at, table, {}, false};
    uint32_t limit = bound;
    for (uint32_t entry = table; entry < limit; entry += 4) {
        if (checked && out.targets.size() == checked) break;
        if (entry != table && starts_.contains(entry)) break;
        const auto offset = elf_->read32(entry);
        if (!offset) break;
        const uint32_t to = table + *offset;
        if (to % 4 || to < start || to >= bound) break;
        if (to > entry) limit = std::min(limit, to);
        out.targets.push_back(to);
    }
    if (out.targets.empty()) return false;
    out.proven = checked && out.targets.size() == checked;
    return true;
}

Function Cfg::explore(uint32_t start, Origin origin, std::vector<uint32_t>& found) {
    Function f;
    f.start = start;
    f.origin = origin;
    f.bound = bound_of(start);
    f.unwind = unwind_.contains(start);
    if (const auto stub = stubs_.find(start); stub != stubs_.end()) f.stub = stub->second;
    f.end = start;
    std::vector<uint32_t> work{start};
    if (const auto pads = pads_.find(start); pads != pads_.end())
        for (const uint32_t pad : pads->second)
            if (pad < f.bound) {  // (a start inside the function may have cut it short: its other part has the rest)
                f.landing_pads.push_back(pad);
                work.push_back(pad);
            }
    const auto leave = [&](uint32_t to) {  // a branch, not a call, to `to`
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
            if (kind != kNone || !in.valid()) {  // a jump table, a gap between sections, or not an instruction
                f.bad.push_back(pc);
                break;
            }
            kind = kCode;
            ++f.words;
            f.end = std::max(f.end, pc + 4);
            const bool always = (in.rd() & 0x14) == 0x14;  // of a conditional branch's BO
            if (in.op == Op::b) {
                const uint32_t to = in.branch_target(pc);
                if (in.lk()) {
                    f.calls.insert(to);
                    if (!in_code(to)) f.bad.push_back(pc);
                    continue;
                }
                leave(to);
                break;
            }
            if (in.op == Op::bc) {
                const uint32_t to = in.branch_target(pc);
                if (in.lk()) {
                    f.calls.insert(to);
                    if (!in_code(to)) f.bad.push_back(pc);
                    continue;
                }
                leave(to);
                if (always) break;
                continue;
            }
            if (in.op == Op::bclr) {
                if (in.lk()) f.indirect_calls.push_back(pc);
                else if (always) break;
                continue;
            }
            if (in.op == Op::bcctr) {
                if (in.lk()) {
                    f.indirect_calls.push_back(pc);
                    continue;
                }
                JumpTable table;
                if (always && read_table(pc, start, f.bound, table)) {
                    for (uint32_t entry = table.table; entry < table.end(); entry += 4) {
                        uint8_t& k = kind_[(entry - lo_) / 4];
                        if (k == kCode) f.bad.push_back(entry);  // a table over instructions already reached
                        else k = kTable;
                    }
                    for (const uint32_t to : table.targets) work.push_back(to);
                    f.tables.push_back(std::move(table));
                } else {
                    f.indirect_tails.push_back(pc);
                }
                if (always) break;
                continue;
            }
            if (in.op == Op::sc) {
                f.system_calls.push_back({pc, std::nullopt});
                continue;
            }
            // A trap that always fires: nothing runs after it.
            if ((in.op == Op::tw || in.op == Op::td || in.op == Op::twi || in.op == Op::tdi) && in.rd() == 31) break;
        }
    }
    std::ranges::sort(f.tables, {}, &JumpTable::at);
    std::ranges::sort(f.system_calls, {}, &SystemCall::at);
    if (!f.system_calls.empty()) number_system_calls(f);
    std::ranges::sort(f.indirect_calls);
    std::ranges::sort(f.indirect_tails);
    std::ranges::sort(f.bad);
    return f;
}

// Might the instruction write GPR r? (Conservative: the string and multiple
// loads count as writing anything.)
static bool writes_gpr(const Insn& in, unsigned r) {
    const ppu::Spec& s = ppu::spec(in.op);
    if (in.op == Op::lmw) return r >= in.rd();
    if (in.op == Op::lswi || in.op == Op::lswx) return true;
    const bool update = s.flags & (ppu::kRaNonZero | ppu::kRaNotZeroOrRd);  // the forms that write their base back
    if (update && in.ra() == r) return true;
    if (s.flags & ppu::kStore) return false;  // a store's first operand is what it stores
    switch (s.operands[0]) {
    case ppu::Opnd::rD: return in.rd() == r;
    case ppu::Opnd::rA: return in.ra() == r;
    default: return false;
    }
}

// The system call's number is the value of r11 at the `sc`. The compiler
// sets it with `li r11, n`, usually just above but sometimes before a branch
// or a join, so: the constant in r11 along every path through the function.
// A call may change r11, and so may anything that writes it otherwise.
void Cfg::number_system_calls(Function& f) const {
    constexpr int64_t kVaries = -1;
    std::map<uint32_t, int64_t> at_entry;  // r11 on reaching each instruction
    std::vector<uint32_t> work;
    const auto reach = [&](uint32_t pc, int64_t value) {
        if (pc < f.start || pc >= f.bound) return;
        const auto [it, added] = at_entry.try_emplace(pc, value);
        if (added) work.push_back(pc);
        else if (it->second != value && it->second != kVaries) {
            it->second = kVaries;
            work.push_back(pc);
        }
    };
    reach(f.start, kVaries);
    for (const uint32_t pad : f.landing_pads) reach(pad, kVaries);
    while (!work.empty()) {
        const uint32_t pc = work.back();
        work.pop_back();
        const Insn in = insn(pc);
        if (!in.valid() || kind_[(pc - lo_) / 4] != kCode) continue;
        int64_t value = at_entry[pc];
        const bool always = (in.rd() & 0x14) == 0x14;
        const bool branch = in.op == Op::b || in.op == Op::bc || in.op == Op::bclr || in.op == Op::bcctr;
        if (branch && in.lk()) value = kVaries;  // a call
        else if (in.op == Op::addi && in.rd() == 11 && in.ra() == 0) value = static_cast<uint16_t>(in.simm());
        else if (!branch && writes_gpr(in, 11)) value = kVaries;
        if (in.op == Op::b) {
            if (in.lk()) reach(pc + 4, value);
            else reach(in.branch_target(pc), value);
        } else if (in.op == Op::bc) {
            if (!in.lk()) reach(in.branch_target(pc), value);
            if (in.lk() || !always) reach(pc + 4, value);
        } else if (in.op == Op::bclr) {
            if (in.lk() || !always) reach(pc + 4, value);
        } else if (in.op == Op::bcctr) {
            for (const JumpTable& table : f.tables)
                if (table.at == pc)
                    for (const uint32_t to : table.targets) reach(to, value);
            if (in.lk() || !always) reach(pc + 4, value);
        } else if ((in.op == Op::tw || in.op == Op::td || in.op == Op::twi || in.op == Op::tdi) && in.rd() == 31) {
        } else {
            reach(pc + 4, value);
        }
    }
    for (SystemCall& call : f.system_calls)
        if (const auto it = at_entry.find(call.at); it != at_entry.end() && it->second != kVaries) call.number = static_cast<uint32_t>(it->second);
}

void Cfg::run() {
    for (rounds_ = 1;; ++rounds_) {
        functions_.clear();
        for (uint8_t& kind : kind_)
            if (kind != kGap) kind = kNone;
        std::vector<uint32_t> found;
        for (const auto& [start, origin] : starts_) functions_.emplace(start, explore(start, origin, found));
        bool changed = false;
        for (const uint32_t to : found)
            if (!starts_.contains(to)) {
                add_start(to, Origin::TailCall);
                changed = true;
            }
        if (!changed || rounds_ == 64) break;
    }
}

void Cfg::account() {
    coverage_ = {};
    uint32_t span = 0;
    bool open = false, dead_before = false;  // the word before was counted dead
    const auto always_traps = [](const Insn& in) {
        return (in.op == Op::tw || in.op == Op::td || in.op == Op::twi || in.op == Op::tdi) && in.rd() == 31;
    };
    const auto close = [&](uint32_t at) {
        if (open) coverage_.unknown_spans.emplace_back(span, at);
        open = false;
    };
    for (uint32_t a = lo_; a < hi_; a += 4) {
        const uint8_t kind = kind_[(a - lo_) / 4];
        if (kind == kGap) {
            close(a);
            dead_before = false;
            continue;
        }
        ++coverage_.words;
        const uint32_t word = decoded_[(a - lo_) / 4].word;
        if (kind == kCode) ++coverage_.code;
        else if (kind == kTable) ++coverage_.tables;
        else if (word == 0 || word == kNop) ++coverage_.padding;
        else if (decoded_[(a - lo_) / 4].valid() && !open && a > lo_ && !starts_.contains(a) &&
                 (kind_[(a - lo_) / 4 - 1] == kCode ? (always_traps(decoded_[(a - lo_) / 4 - 1])) : dead_before)) {
            ++coverage_.dead;
            dead_before = true;
            continue;
        } else {
            ++coverage_.unknown;
            if (!open) span = a;
            open = true;
            continue;
        }
        dead_before = false;
        close(a);
    }
    close(hi_);
}

// ---------------------------------------------------------------- the report

std::string Cfg::summary() const {
    std::string out;
    const auto line = [&](std::string text) { out += text + "\n"; };
    size_t by_origin[6] = {}, pads = 0, unwind = 0, tables = 0, proven = 0, entries = 0, apart = 0, calls = 0, indirect_calls = 0, runs_off = 0,
           runs_off_unknown = 0, sites = 0, numbered = 0;
    std::vector<uint32_t> bad, tails, open_ended;
    std::vector<std::pair<uint32_t, uint32_t>> unproven;
    std::map<uint32_t, size_t> numbers;
    for (const auto& [start, f] : functions_) {
        ++by_origin[static_cast<size_t>(f.origin)];
        unwind += f.unwind;
        pads += f.landing_pads.size();
        calls += f.calls.size();
        indirect_calls += f.indirect_calls.size();
        if (f.runs_off) {
            ++runs_off;
            runs_off_unknown += !f.unwind;
            if (!f.unwind) open_ended.push_back(f.end - 4);
        }
        for (const JumpTable& t : f.tables) {
            ++tables;
            proven += t.proven;
            entries += t.targets.size();
            apart += t.table != t.at + 4;
            if (!t.proven) unproven.emplace_back(t.at, static_cast<uint32_t>(t.targets.size()));
        }
        for (const SystemCall& c : f.system_calls) {
            ++sites;
            if (c.number) {
                ++numbered;
                ++numbers[*c.number];
            }
        }
        bad.insert(bad.end(), f.bad.begin(), f.bad.end());
        if (!f.stub) tails.insert(tails.end(), f.indirect_tails.begin(), f.indirect_tails.end());
    }
    const auto some = [&](const std::vector<uint32_t>& list) {
        std::string text;
        for (size_t i = 0; i < list.size() && i < 24; ++i) text += std::format(" {:#x}", list[i]);
        if (list.size() > 24) text += " ...";
        return text;
    };
    line(std::format("{} functions, found in {} rounds; {} have an unwinding entry", functions_.size(), rounds_, unwind));
    for (size_t o = 0; o < 6; ++o)
        if (by_origin[o]) line(std::format("  {:6} from {}", by_origin[o], origin_name(static_cast<Origin>(o))));
    line(std::format("the program's tables: {} descriptors, {} unwinding entries, {} import stubs; TOC {:#x}", descriptors_, unwind_.size(),
                     stubs_.size(), toc_));
    line(std::format("{} functions have an exception table; {} landing pads", pads_.size(), pads));
    line(std::format("{} direct call sites' targets, {} indirect calls", calls, indirect_calls));
    line(std::format("{} switches with {} entries; the size of {} is the bound the code checks; {} have their table elsewhere than straight after the bctr",
                     tables, entries, proven, apart));
    if (!unproven.empty()) {
        std::string text;
        for (size_t i = 0; i < unproven.size() && i < 24; ++i) text += std::format(" {:#x} ({})", unproven[i].first, unproven[i].second);
        line(std::format("  sized by where their targets begin ({}):{}{}", unproven.size(), text, unproven.size() > 24 ? " ..." : ""));
    }
    line(std::format("{} indirect jumps that are not switches, outside the import stubs:{}", tails.size(), some(tails)));
    line(std::format("{} functions reach their end without a return or jump (a call that doesn't return, or the next function); {} of them have no unwinding entry to say where they end; their last instructions:{}",
                     runs_off, runs_off_unknown, some(open_ended)));
    line(std::format("{} system call sites; the number is known at {}: {} different calls", sites, numbered, numbers.size()));
    for (const auto& [number, count] : numbers) {
        const SystemCallName* name = system_call_name(number);
        line(std::format("  {:4} {:3} sites  {}{}", number, count, name ? name->name : "(no name)", name && !name->confirmed ? " (name unconfirmed)" : ""));
    }
    line(std::format("{} places where a function reaches a word that isn't an instruction, or leaves the code:{}", bad.size(), some(bad)));
    const Coverage& c = coverage_;
    line(std::format("{} words: {} code, {} jump-table entries, {} padding, {} dead after a trap, {} unknown in {} spans", c.words, c.code,
                     c.tables, c.padding, c.dead, c.unknown, c.unknown_spans.size()));
    for (size_t i = 0; i < c.unknown_spans.size() && i < 40; ++i)
        line(std::format("  unknown {:#x}-{:#x} ({} words)", c.unknown_spans[i].first, c.unknown_spans[i].second,
                         (c.unknown_spans[i].second - c.unknown_spans[i].first) / 4));
    if (c.unknown_spans.size() > 40) line("  ...");
    return out;
}

}  // namespace qilin

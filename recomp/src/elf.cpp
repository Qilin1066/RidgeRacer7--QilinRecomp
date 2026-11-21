#include "elf.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "bytes.hpp"
#include "sha256.hpp"

namespace qilin {

std::string hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::expected<std::vector<uint8_t>, std::string> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return std::unexpected("cannot open " + path.string());
    const std::streamoff size = f.tellg();
    if (size < 0) return std::unexpected("cannot read " + path.string());
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.seekg(0);
    if (size > 0) f.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!f) return std::unexpected("cannot read " + path.string());
    return bytes;
}

namespace {

constexpr size_t kHeaderSize = 64, kSegmentSize = 56, kSectionSize = 64;

// [at, at + size) lies in a file of `total` bytes.
bool within(uint64_t at, uint64_t size, uint64_t total) { return at <= total && size <= total - at; }

}  // namespace

std::expected<ElfLayout, std::string> parse_elf_layout(std::span<const uint8_t> file, uint64_t header_at,
                                                       std::optional<uint64_t> segments_at,
                                                       std::optional<uint64_t> sections_at) {
    if (!within(header_at, kHeaderSize, file.size())) return std::unexpected(std::string("too short for an ELF header"));
    const uint8_t* h = file.data() + header_at;
    if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') return std::unexpected(std::string("not an ELF file"));
    if (h[4] != 2 || h[5] != 2 || h[6] != 1)
        return std::unexpected(std::string("not a 64-bit big-endian ELF (a PS3 program is one)"));
    ElfLayout l;
    l.os_abi = h[7];
    l.type = read_be16(h + 16);
    l.machine = read_be16(h + 18);
    if (l.machine != kEmPpc64) return std::unexpected("not a PowerPC64 program (machine " + std::to_string(l.machine) + ")");
    if (l.os_abi != kOsAbiCellLv2) return std::unexpected("not a PS3 program (OS/ABI " + hex(l.os_abi) + ", expected 0x66)");
    if (l.type != kEtExec) return std::unexpected("not an executable (ELF type " + std::to_string(l.type) + ")");
    if (read_be32(h + 20) != 1) return std::unexpected(std::string("unknown ELF version"));
    l.entry = read_be64(h + 24);
    l.program_headers_at = read_be64(h + 32);
    l.section_headers_at = read_be64(h + 40);
    l.flags = read_be32(h + 48);
    const uint16_t header_size = read_be16(h + 52), segment_size = read_be16(h + 54), segments = read_be16(h + 56),
                   section_size = read_be16(h + 58), sections = read_be16(h + 60);
    l.section_names = read_be16(h + 62);
    if (header_size != kHeaderSize) return std::unexpected("ELF header size " + std::to_string(header_size) + ", expected 64");
    if (segments == 0) return std::unexpected(std::string("no program headers"));
    if (segment_size != kSegmentSize)
        return std::unexpected("program header size " + std::to_string(segment_size) + ", expected 56");
    if (sections != 0 && section_size != kSectionSize)
        return std::unexpected("section header size " + std::to_string(section_size) + ", expected 64");

    const uint64_t seg_at = segments_at.value_or(l.program_headers_at);
    if (!within(seg_at, uint64_t(segments) * kSegmentSize, file.size()))
        return std::unexpected(std::string("the program headers run past the end of the file"));
    for (unsigned i = 0; i < segments; ++i) {
        const uint8_t* p = file.data() + seg_at + i * kSegmentSize;
        ElfSegment s;
        s.type = read_be32(p);
        s.flags = read_be32(p + 4);
        s.offset = read_be64(p + 8);
        s.address = read_be64(p + 16);
        s.physical = read_be64(p + 24);
        s.file_size = read_be64(p + 32);
        s.memory_size = read_be64(p + 40);
        s.align = read_be64(p + 48);
        l.segments.push_back(s);
    }
    const uint64_t sec_at = sections_at.value_or(l.section_headers_at);
    if (sections != 0 && !within(sec_at, uint64_t(sections) * kSectionSize, file.size()))
        return std::unexpected(std::string("the section headers run past the end of the file"));
    for (unsigned i = 0; i < sections; ++i) {
        const uint8_t* p = file.data() + sec_at + i * kSectionSize;
        ElfSection s;
        s.index = i;
        s.name_offset = read_be32(p);
        s.type = read_be32(p + 4);
        s.flags = read_be64(p + 8);
        s.address = read_be64(p + 16);
        s.offset = read_be64(p + 24);
        s.size = read_be64(p + 32);
        s.link = read_be32(p + 40);
        s.info = read_be32(p + 44);
        s.align = read_be64(p + 48);
        s.entry_size = read_be64(p + 56);
        l.sections.push_back(std::move(s));
    }
    if (sections != 0 && l.section_names >= sections)
        return std::unexpected(std::string("the section name table's index is not a section"));
    return l;
}

std::expected<Elf, std::string> Elf::load(const std::filesystem::path& path) {
    auto bytes = read_file(path);
    if (!bytes) return std::unexpected(bytes.error());
    auto elf = parse(std::move(*bytes));
    if (!elf) return std::unexpected(path.string() + ": " + elf.error());
    return elf;
}

std::expected<Elf, std::string> Elf::parse(std::vector<uint8_t> file) {
    if (file.size() >= 4 && file[0] == 'S' && file[1] == 'C' && file[2] == 'E' && file[3] == 0)
        return std::unexpected(std::string("this is the encrypted program (a SELF), not the decrypted ELF"));
    auto layout = parse_elf_layout(file, 0);
    if (!layout) return std::unexpected(layout.error());
    Elf elf;
    elf.sha256_ = sha256_hex(file);
    elf.file_ = std::move(file);
    elf.layout_ = std::move(*layout);
    if (auto error = elf.read_tables()) return std::unexpected(*error);
    return elf;
}

const ElfSection* Elf::section(std::string_view name) const {
    for (const ElfSection& s : layout_.sections)
        if (s.name == name) return &s;
    return nullptr;
}

std::span<const uint8_t> Elf::bytes_at(uint64_t address, uint64_t size) const {
    for (const ElfSegment& s : layout_.segments) {
        if (s.type != kPtLoad || address < s.address) continue;
        const uint64_t at = address - s.address;
        if (at <= s.file_size && size <= s.file_size - at)
            return std::span(file_).subspan(static_cast<size_t>(s.offset + at), static_cast<size_t>(size));
    }
    return {};
}

std::optional<uint32_t> Elf::read32(uint64_t address) const {
    const auto b = bytes_at(address, 4);
    if (b.empty()) return std::nullopt;
    return read_be32(b.data());
}

bool Elf::executable(uint64_t address) const {
    return std::ranges::any_of(layout_.segments, [&](const ElfSegment& s) {
        return s.type == kPtLoad && (s.flags & kPfExec) && s.contains(address);
    });
}

namespace {

// A function's stub, as the SDK's linker writes it: eight instructions that
// load the slot's address, save r2, and call through the descriptor the
// loader put in the slot.
//   li r12,0; oris r12,r12,HI; lwz r12,LO(r12); std r2,40(r1);
//   lwz r0,0(r12); lwz r2,4(r12); mtctr r0; bctr
bool calls_through(const Elf& elf, uint32_t code, uint32_t slot) {
    const auto b = elf.bytes_at(code, 32);
    if (b.empty() || !elf.executable(code)) return false;
    uint32_t w[8];
    for (int i = 0; i < 8; ++i) w[i] = read_be32(b.data() + i * 4);
    if (w[0] != 0x39800000 || (w[1] >> 16) != 0x658c || (w[2] >> 16) != 0x818c) return false;
    if (w[3] != 0xf8410028 || w[4] != 0x800c0000 || w[5] != 0x804c0004 || w[6] != 0x7c0903a6 || w[7] != 0x4e800420)
        return false;
    const uint32_t loads = ((w[1] & 0xffff) << 16) + static_cast<uint32_t>(static_cast<int16_t>(w[2] & 0xffff));
    return loads == slot;
}

}  // namespace

std::optional<std::string> Elf::read_tables() {
    const uint64_t size = file_.size();

    // Section names.
    if (!layout_.sections.empty()) {
        const ElfSection& names = layout_.sections[layout_.section_names];
        if (names.type != kShtStrtab || !within(names.offset, names.size, size))
            return "the section name table is not a string table inside the file";
        const std::string_view table(reinterpret_cast<const char*>(file_.data() + names.offset),
                                     static_cast<size_t>(names.size));
        for (ElfSection& s : layout_.sections) {
            if (s.name_offset >= table.size()) return "section " + std::to_string(s.index) + "'s name is outside the name table";
            const size_t end = table.find('\0', s.name_offset);
            if (end == table.npos) return "section " + std::to_string(s.index) + "'s name has no end";
            s.name = table.substr(s.name_offset, end - s.name_offset);
            if (s.type != kShtNobits && s.type != kShtNull && !within(s.offset, s.size, size))
                return "section " + s.name + " runs past the end of the file";
        }
    }

    // Segments: inside the file, inside a 32-bit address space, not overlapping.
    for (size_t i = 0; i < layout_.segments.size(); ++i) {
        const ElfSegment& s = layout_.segments[i];
        const std::string which = "program header " + std::to_string(i);
        if (!within(s.offset, s.file_size, size)) return which + " runs past the end of the file";
        if (s.type != kPtLoad) continue;
        if (s.file_size > s.memory_size) return which + " has more bytes in the file than in memory";
        if (!within(s.address, s.memory_size, uint64_t(1) << 32))
            return which + " is outside the 32-bit address space an LV2 process has (" + hex(s.address) + ")";
        for (size_t j = 0; j < i; ++j) {
            const ElfSegment& o = layout_.segments[j];
            if (o.type == kPtLoad && s.memory_size && o.memory_size && s.address < o.address + o.memory_size &&
                o.address < s.address + s.memory_size)
                return which + " overlaps program header " + std::to_string(j);
        }
    }

    // The entry point is a descriptor: the code's address, then r2's value.
    const auto entry = bytes_at(layout_.entry, 8);
    if (entry.empty()) return "the entry point (" + hex(layout_.entry) + ") is not a descriptor in the file";
    entry_code_ = read_be32(entry.data());
    entry_toc_ = read_be32(entry.data() + 4);
    if (!executable(entry_code_))
        return "the entry point's descriptor names " + hex(entry_code_) + ", which is not code";

    const auto reads = [&](uint64_t address, uint64_t count, const std::string& what) -> std::expected<const uint8_t*, std::string> {
        const auto b = bytes_at(address, count);
        if (b.empty() && count != 0) return std::unexpected(what + " (" + hex(address) + ") is not in the file");
        return b.data();
    };
    const auto text = [&](uint32_t address, const std::string& what) -> std::expected<std::string, std::string> {
        std::string s;
        for (uint32_t a = address; s.size() <= 256; ++a) {
            const auto b = bytes_at(a, 1);
            if (b.empty()) return std::unexpected(what + " (" + hex(address) + ") is not in the file");
            if (b[0] == 0) return s;
            if (b[0] < 0x20 || b[0] > 0x7e) return std::unexpected(what + " (" + hex(address) + ") is not text");
            s.push_back(static_cast<char>(b[0]));
        }
        return std::unexpected(what + " (" + hex(address) + ") has no end");
    };

    for (const ElfSegment& s : layout_.segments) {
        if (s.type == kPtTls) {
            if (tls_) return "more than one thread-local storage segment";
            if (!within(s.address, s.memory_size, uint64_t(1) << 32) || s.file_size > s.memory_size)
                return "the thread-local storage segment is malformed";
            tls_ = TlsImage{static_cast<uint32_t>(s.address), static_cast<uint32_t>(s.file_size),
                            static_cast<uint32_t>(s.memory_size), static_cast<uint32_t>(s.align)};
        } else if (s.type == kPtProcParam && s.file_size != 0) {
            if (process_param_) return "more than one process parameter segment";
            if (s.file_size < 0x20) return "the process parameters are too short";
            const uint8_t* p = file_.data() + s.offset;
            if (read_be32(p + 4) != kProcParamMagic) return "the process parameters' magic number is wrong";
            ProcessParam q;
            q.size = read_be32(p);
            q.version = read_be32(p + 8);
            q.sdk_version = read_be32(p + 12);
            q.primary_priority = static_cast<int32_t>(read_be32(p + 16));
            q.primary_stack_size = read_be32(p + 20);
            q.malloc_page_size = read_be32(p + 24);
            q.ppc_seg = read_be32(p + 28);
            if (q.size < 0x20 || q.size > s.file_size) return "the process parameters' size is wrong";
            process_param_ = q;
        } else if (s.type == kPtProcPrx && s.file_size != 0) {
            if (prx_param_) return "more than one PRX parameter segment";
            if (s.file_size < 0x20) return "the PRX parameters are too short";
            const uint8_t* p = file_.data() + s.offset;
            if (read_be32(p + 4) != kPrxParamMagic) return "the PRX parameters' magic number is wrong";
            PrxParam q;
            q.size = read_be32(p);
            q.version = read_be32(p + 8);
            q.exports_start = read_be32(p + 16);
            q.exports_end = read_be32(p + 20);
            q.imports_start = read_be32(p + 24);
            q.imports_end = read_be32(p + 28);
            if (q.size < 0x20 || q.size > s.file_size) return "the PRX parameters' size is wrong";
            if (q.exports_end < q.exports_start || q.imports_end < q.imports_start)
                return "the PRX parameters' tables end before they start";
            prx_param_ = q;
        }
    }
    if (!prx_param_) return std::nullopt;  // a program that imports nothing

    // Imports: one entry per library.
    for (uint32_t at = prx_param_->imports_start; at < prx_param_->imports_end;) {
        const std::string which = "the import entry at " + hex(at);
        const auto e = bytes_at(at, kImportEntrySize);
        if (e.empty() || prx_param_->imports_end - at < kImportEntrySize) return which + " is not in the file";
        if (e[0] != kImportEntrySize) return which + " has size " + std::to_string(e[0]) + ", expected 44";
        ImportLibrary lib;
        lib.entry = at;
        lib.version = read_be16(e.data() + 2);
        lib.attributes = read_be16(e.data() + 4);
        const uint16_t functions = read_be16(e.data() + 6), variables = read_be16(e.data() + 8);
        lib.tls_variables = read_be16(e.data() + 10);
        const uint32_t name = read_be32(e.data() + 16), ids = read_be32(e.data() + 20), slots = read_be32(e.data() + 24),
                       variable_ids = read_be32(e.data() + 28), variable_slots = read_be32(e.data() + 32);
        auto n = text(name, which + "'s library name");
        if (!n) return n.error();
        if (n->empty()) return which + " names no library";
        lib.name = std::move(*n);
        auto id = reads(ids, uint64_t(functions) * 4, lib.name + "'s function ids");
        if (!id) return id.error();
        auto slot = reads(slots, uint64_t(functions) * 4, lib.name + "'s function slots");
        if (!slot) return slot.error();
        for (uint32_t i = 0; i < functions; ++i) {
            ImportFunction f;
            f.id = read_be32(*id + i * 4);
            f.slot = slots + i * 4;
            f.initial = read_be32(*slot + i * 4);
            f.stub = calls_through(*this, f.initial, f.slot);
            lib.functions.push_back(f);
        }
        auto vid = reads(variable_ids, uint64_t(variables) * 4, lib.name + "'s variable ids");
        if (!vid) return vid.error();
        auto vslot = reads(variable_slots, uint64_t(variables) * 4, lib.name + "'s variable slots");
        if (!vslot) return vslot.error();
        for (uint32_t i = 0; i < variables; ++i)
            lib.variables.push_back({read_be32(*vid + i * 4), variable_slots + i * 4});
        imports_.push_back(std::move(lib));
        at += kImportEntrySize;
    }

    // Exports: what the program offers the system (its own entry is unnamed).
    for (uint32_t at = prx_param_->exports_start; at < prx_param_->exports_end;) {
        const std::string which = "the export entry at " + hex(at);
        const auto e = bytes_at(at, kExportEntrySize);
        if (e.empty() || prx_param_->exports_end - at < kExportEntrySize) return which + " is not in the file";
        if (e[0] != kExportEntrySize) return which + " has size " + std::to_string(e[0]) + ", expected 28";
        ExportLibrary lib;
        lib.version = read_be16(e.data() + 2);
        lib.attributes = read_be16(e.data() + 4);
        const uint32_t functions = read_be16(e.data() + 6);
        const uint32_t variables = uint32_t(read_be16(e.data() + 8)) + read_be16(e.data() + 10);
        const uint32_t name = read_be32(e.data() + 16), ids = read_be32(e.data() + 20), addresses = read_be32(e.data() + 24);
        if (name != 0) {
            auto n = text(name, which + "'s library name");
            if (!n) return n.error();
            lib.name = std::move(*n);
        }
        const uint64_t count = uint64_t(functions) + variables;
        auto id = reads(ids, count * 4, which + "'s ids");
        if (!id) return id.error();
        auto address = reads(addresses, count * 4, which + "'s addresses");
        if (!address) return address.error();
        for (uint32_t i = 0; i < count; ++i)
            (i < functions ? lib.functions : lib.variables).push_back({read_be32(*id + i * 4), read_be32(*address + i * 4)});
        exports_.push_back(std::move(lib));
        at += kExportEntrySize;
    }
    return std::nullopt;
}

}  // namespace qilin

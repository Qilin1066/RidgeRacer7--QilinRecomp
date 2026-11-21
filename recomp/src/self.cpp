#include "self.hpp"

#include "bytes.hpp"
#include "sha256.hpp"

namespace qilin {

namespace {

bool within(uint64_t at, uint64_t size, uint64_t total) { return at <= total && size <= total - at; }

}  // namespace

bool Self::is_self(std::span<const uint8_t> file) {
    return file.size() >= 4 && file[0] == 'S' && file[1] == 'C' && file[2] == 'E' && file[3] == 0;
}

std::expected<std::vector<SdkLibrary>, std::string> parse_sdk_stamps(std::string_view text) {
    std::vector<SdkLibrary> libraries;
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    for (size_t at = 0; at < text.size();) {
        const size_t colon = text.find(':', at);
        if (colon == text.npos || colon == at) return std::unexpected("the SDK library list is malformed at byte " + std::to_string(at));
        for (size_t i = at; i < colon; ++i) {
            const char c = text[i];
            if (!letter(c) && !digit(c) && c != '_' && c != '+' && c != '-' && c != '.')
                return std::unexpected("the SDK library list is malformed at byte " + std::to_string(i));
        }
        // The version: one letter, then digits ("p102002").
        size_t end = colon + 1;
        if (end < text.size() && letter(text[end])) ++end;
        const size_t digits = end;
        while (end < text.size() && digit(text[end])) ++end;
        if (end == digits) return std::unexpected("the SDK library list is malformed at byte " + std::to_string(colon));
        const std::string_view name = text.substr(at, colon - at), version = text.substr(colon + 1, end - colon - 1);
        bool found = false;
        for (SdkLibrary& l : libraries) {
            if (l.name == name && l.version == version) {
                ++l.objects;
                found = true;
                break;
            }
        }
        if (!found) libraries.push_back({std::string(name), std::string(version), 1});
        at = end;
    }
    return libraries;
}

std::expected<Self, std::string> Self::load(const std::filesystem::path& path) {
    auto bytes = read_file(path);
    if (!bytes) return std::unexpected(bytes.error());
    auto self = parse(*bytes);
    if (!self) return std::unexpected(path.string() + ": " + self.error());
    return self;
}

std::expected<Self, std::string> Self::parse(std::span<const uint8_t> file) {
    if (!is_self(file)) return std::unexpected(std::string("not a SELF (no SCE header)"));
    if (file.size() < 0x70) return std::unexpected(std::string("too short for a SELF header"));
    const uint8_t* d = file.data();
    Self s;
    s.file_size = file.size();
    s.sha256 = sha256_hex(file);
    s.version = read_be32(d + 4);
    s.key_revision = read_be16(d + 8);
    s.header_type = read_be16(d + 10);
    s.header_size = read_be64(d + 0x10);
    s.data_size = read_be64(d + 0x18);
    if (s.header_type != 1) return std::unexpected("an SCE file, but not a SELF (type " + std::to_string(s.header_type) + ")");
    if (!within(s.header_size, s.data_size, file.size()) || s.header_size + s.data_size != file.size())
        return std::unexpected("the file is " + std::to_string(file.size()) + " bytes, but its header says " +
                               std::to_string(s.header_size + s.data_size) + ": truncated, or not a whole SELF");
    const uint64_t app_info = read_be64(d + 0x28), elf_at = read_be64(d + 0x30), segments_at = read_be64(d + 0x38),
                   sections_at = read_be64(d + 0x40), segment_info = read_be64(d + 0x48), sdk_version = read_be64(d + 0x50);
    if (!within(app_info, 0x18, file.size())) return std::unexpected(std::string("the application information is outside the file"));
    s.authentication_id = read_be64(d + app_info);
    s.vendor_id = read_be32(d + app_info + 8);
    s.self_type = read_be32(d + app_info + 12);
    s.application_version = read_be64(d + app_info + 16);

    auto layout = parse_elf_layout(file, elf_at, segments_at, sections_at);
    if (!layout) return std::unexpected("the SELF's copy of the ELF header: " + layout.error());
    s.elf = std::move(*layout);

    if (!within(segment_info, s.elf.segments.size() * 32, file.size()))
        return std::unexpected(std::string("the segment table is outside the file"));
    for (size_t i = 0; i < s.elf.segments.size(); ++i) {
        const uint8_t* p = d + segment_info + i * 32;
        SelfSegment seg{read_be64(p), read_be64(p + 8), read_be32(p + 16), read_be32(p + 28)};
        if (!within(seg.offset, seg.size, file.size()))
            return std::unexpected("segment " + std::to_string(i) + " runs past the end of the file");
        if (seg.compression != 1 && seg.compression != 2)
            return std::unexpected("segment " + std::to_string(i) + " has an unknown compression (" + std::to_string(seg.compression) + ")");
        s.segments.push_back(seg);
    }

    // The SDK's library stamps, where the header points at them: a section of
    // the ELF outside every segment, so not encrypted.
    if (sdk_version != 0) {
        if (!within(sdk_version, 0x10, file.size())) return std::unexpected(std::string("the SDK version header is outside the file"));
        const uint32_t present = read_be32(d + sdk_version + 4), size = read_be32(d + sdk_version + 8);
        if (present == 1 && size == 0x30) {
            if (!within(sdk_version, 0x30, file.size())) return std::unexpected(std::string("the SDK version header is outside the file"));
            const uint64_t at = read_be64(d + sdk_version + 0x20), length = read_be64(d + sdk_version + 0x28);
            if (!within(at, length, file.size())) return std::unexpected(std::string("the SDK library list is outside the file"));
            auto libraries = parse_sdk_stamps({reinterpret_cast<const char*>(d + at), static_cast<size_t>(length)});
            if (!libraries) return std::unexpected(libraries.error());
            s.libraries = std::move(*libraries);
        }
    }
    return s;
}

std::string differs_from(const Self& self, const ElfLayout& elf) {
    if (self.elf.entry != elf.entry) return "the entry point (" + hex(self.elf.entry) + " in EBOOT.BIN, " + hex(elf.entry) + " in the ELF)";
    if (self.elf.segments.size() != elf.segments.size()) return "the number of program headers";
    for (size_t i = 0; i < elf.segments.size(); ++i) {
        const ElfSegment &a = self.elf.segments[i], &b = elf.segments[i];
        if (a.type != b.type || a.flags != b.flags || a.address != b.address || a.file_size != b.file_size ||
            a.memory_size != b.memory_size)
            return "program header " + std::to_string(i);
    }
    return {};
}

}  // namespace qilin

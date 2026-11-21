#include "spu_image.hpp"

#include <algorithm>
#include <cstring>

#include "bytes.hpp"

namespace qilin {

namespace {

std::unexpected<std::string> fail(uint32_t address, const std::string& message) {
    return std::unexpected("SPU program at " + hex(address) + ": " + message);
}

constexpr size_t kHeader = 52, kSegment = 32, kSection = 40;
// The start of a 32-bit big-endian ELF, version 1.
constexpr uint8_t kMagic[7] = {0x7f, 'E', 'L', 'F', 1, 2, 1};

// The text at `chars`, up to a zero byte or `limit` bytes.
std::string text_at(const uint8_t* chars, size_t limit) {
    const uint8_t* end = std::find(chars, chars + limit, uint8_t{0});
    return std::string(reinterpret_cast<const char*>(chars), static_cast<size_t>(end - chars));
}

}  // namespace

uint32_t SpuImage::code_size() const {
    uint32_t bytes = 0;
    for (const SpuSection& s : sections)
        if (s.type == kShtProgbits && (s.flags & kShfExec)) bytes += s.size;
    return bytes;
}

const SpuSection* SpuImage::section(std::string_view section_name) const {
    for (const SpuSection& s : sections)
        if (s.name == section_name) return &s;
    return nullptr;
}

std::expected<SpuImage, std::string> parse_spu_image(std::span<const uint8_t> file, uint32_t address) {
    if (file.size() < kHeader || std::memcmp(file.data(), kMagic, sizeof kMagic) != 0) return fail(address, "not a 32-bit big-endian ELF");
    const uint8_t* h = file.data();
    if (read_be16(h + 16) != kEtExec) return fail(address, "not an executable (ELF type " + std::to_string(read_be16(h + 16)) + ")");
    if (read_be16(h + 18) != kEmSpu) return fail(address, "not for the SPU (machine " + std::to_string(read_be16(h + 18)) + ")");
    SpuImage image;
    image.address = address;
    image.entry = read_be32(h + 24);
    const uint32_t segments_at = read_be32(h + 28), sections_at = read_be32(h + 32);
    const uint16_t segment_size = read_be16(h + 42), segments = read_be16(h + 44), section_size = read_be16(h + 46),
                   sections = read_be16(h + 48), names = read_be16(h + 50);
    if (segments == 0 || segment_size != kSegment) return fail(address, "no program headers of 32 bytes");
    if (sections == 0 || section_size != kSection || names >= sections) return fail(address, "no section headers of 40 bytes with a table of names");
    const uint64_t segments_end = uint64_t{segments_at} + uint64_t{segments} * kSegment;
    const uint64_t sections_end = uint64_t{sections_at} + uint64_t{sections} * kSection;
    if (segments_end > file.size() || sections_end > file.size()) return fail(address, "its tables run past the data it lies in");
    // The file ends with its section headers: that is how the SPU's linker lays one out.
    image.size = static_cast<uint32_t>(sections_end);
    bool entry_in_code = false;
    for (unsigned i = 0; i < segments; ++i) {
        const uint8_t* p = h + segments_at + i * kSegment;
        SpuSegment s;
        s.type = read_be32(p);
        s.offset = read_be32(p + 4);
        s.address = read_be32(p + 8);
        s.file_size = read_be32(p + 16);
        s.memory_size = read_be32(p + 20);
        s.flags = read_be32(p + 24);
        const std::string which = "segment " + std::to_string(i);
        if (uint64_t{s.offset} + s.file_size > image.size) return fail(address, which + " runs past the file's end");
        if (s.type == kPtLoad) {
            if (s.memory_size < s.file_size) return fail(address, which + " is larger in the file than in memory");
            if (uint64_t{s.address} + s.memory_size > kSpuMemory)
                return fail(address, which + " (" + hex(s.memory_size) + " bytes at " + hex(s.address) + ") doesn't fit an SPU's memory");
            if ((s.flags & kPfExec) && image.entry >= s.address && image.entry - s.address < s.file_size) entry_in_code = true;
        }
        image.segments.push_back(s);
    }
    if (!entry_in_code || image.entry % 4) return fail(address, "its entry point " + hex(image.entry) + " isn't in its code");
    const uint8_t* names_header = h + sections_at + names * kSection;
    const uint32_t names_at = read_be32(names_header + 16), names_size = read_be32(names_header + 20);
    if (uint64_t{names_at} + names_size > image.size) return fail(address, "its table of section names runs past the file's end");
    for (unsigned i = 0; i < sections; ++i) {
        const uint8_t* p = h + sections_at + i * kSection;
        SpuSection s;
        const uint32_t name = read_be32(p);
        s.type = read_be32(p + 4);
        s.flags = read_be32(p + 8);
        s.address = read_be32(p + 12);
        s.offset = read_be32(p + 16);
        s.size = read_be32(p + 20);
        if (name >= names_size) return fail(address, "section " + std::to_string(i) + "'s name is outside the table");
        s.name = text_at(h + names_at + name, names_size - name);
        if (s.type != kShtNobits && s.type != kShtNull && uint64_t{s.offset} + s.size > image.size)
            return fail(address, "section " + s.name + " runs past the file's end");
        if (s.type == kShtProgbits && (s.flags & kShfExec) && (s.address % 4 || s.size % 4))
            return fail(address, "code section " + s.name + " isn't a whole number of words");
        image.sections.push_back(std::move(s));
    }
    // The name: a note of type 1 whose owner is "SPUNAME"; its text is the file the program was built as.
    if (const SpuSection* note = image.section(".note.spu_name")) {
        const uint8_t* n = h + note->offset;
        if (note->size >= 12) {
            const uint32_t owner = read_be32(n), text = read_be32(n + 4);
            const uint32_t text_offset = 12 + ((owner + 3) & ~3u);
            if (uint64_t{text_offset} + text > note->size) return fail(address, "its name note doesn't fit its section");
            image.name = text_at(n + text_offset, text);
        }
    }
    return image;
}

std::expected<std::vector<SpuImage>, std::string> find_spu_images(const Elf& elf) {
    std::vector<SpuImage> images;
    for (const ElfSegment& segment : elf.segments()) {
        if (segment.type != kPtLoad || segment.file_size < kHeader) continue;
        const auto bytes = elf.bytes_at(segment.address, segment.file_size);
        if (bytes.empty()) continue;
        size_t at = 0;
        while (at + kHeader <= bytes.size()) {
            const auto found = std::search(bytes.begin() + static_cast<ptrdiff_t>(at), bytes.end(), std::begin(kMagic), std::end(kMagic));
            if (found == bytes.end()) break;
            at = static_cast<size_t>(found - bytes.begin());
            // Any 32-bit big-endian ELF for the SPU; other bytes that happen to spell the magic are passed over.
            if (at + kHeader > bytes.size() || read_be16(bytes.data() + at + 18) != kEmSpu) {
                at += 4;
                continue;
            }
            auto image = parse_spu_image(bytes.subspan(at), static_cast<uint32_t>(segment.address + at));
            if (!image) return std::unexpected(image.error());
            at += image->size;
            images.push_back(std::move(*image));
        }
    }
    std::ranges::sort(images, {}, &SpuImage::address);
    return images;
}

}  // namespace qilin

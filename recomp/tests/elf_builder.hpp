// Builds a small PS3 program, the SELF that would wrap it, and a PARAM.SFO,
// in memory: the tests need no game files.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace elfb {

using Bytes = std::vector<uint8_t>;

inline void put_be16(Bytes& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v >> 8);
    b[at + 1] = static_cast<uint8_t>(v);
}
inline void put_be32(Bytes& b, size_t at, uint32_t v) {
    put_be16(b, at, static_cast<uint16_t>(v >> 16));
    put_be16(b, at + 2, static_cast<uint16_t>(v));
}
inline void put_be64(Bytes& b, size_t at, uint64_t v) {
    put_be32(b, at, static_cast<uint32_t>(v >> 32));
    put_be32(b, at + 4, static_cast<uint32_t>(v));
}
inline void put_le16(Bytes& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}
inline void put_le32(Bytes& b, size_t at, uint32_t v) {
    put_le16(b, at, static_cast<uint16_t>(v));
    put_le16(b, at + 2, static_cast<uint16_t>(v >> 16));
}
inline void put_text(Bytes& b, size_t at, std::string_view s) { std::memcpy(b.data() + at, s.data(), s.size()); }

// Where things are in the program make_elf() builds.
inline constexpr uint32_t kText = 0x10000, kData = 0x20000;
inline constexpr size_t kTextAt = 0x400, kDataAt = 0x600, kSectionsAt = 0x700, kNamesAt = 0x680;
inline constexpr size_t kSegments = 5, kSegmentsAt = 64;
inline constexpr uint32_t kStub0 = kText + 0x20, kStub1 = kText + 0x40, kName = kText + 0x60, kIds = kText + 0x70,
                          kProcParam = kText + 0x80, kPrxParam = kText + 0xa0, kImport = kText + 0xc8, kExport = kText + 0xf4,
                          kExportIds = kText + 0x110, kExportAddresses = kText + 0x114, kTextSize = 0x118;
inline constexpr uint32_t kEntry = kData, kSlots = kData + 8, kTlsData = kData + 0x10, kDataSize = 0x14, kDataMemory = 0x100;
inline constexpr uint32_t kToc = kData + 0x8000, kId0 = 0x11223344, kId1 = 0xa1b2c3d4, kExportId = 0x0badf00d;

inline void put_stub(Bytes& b, size_t at, uint32_t slot) {
    const uint32_t low = slot & 0xffff, high = (slot >> 16) + (low >> 15);  // lwz sign-extends the low half
    const uint32_t words[8] = {0x39800000, 0x658c0000 | (high & 0xffff), 0x818c0000 | low, 0xf8410028,
                               0x800c0000, 0x804c0004,                   0x7c0903a6,       0x4e800420};
    for (int i = 0; i < 8; ++i) put_be32(b, at + static_cast<size_t>(i) * 4, words[i]);
}

inline void put_segment(Bytes& b, size_t index, uint32_t type, uint32_t flags, uint64_t offset, uint64_t address, uint64_t file_size,
                        uint64_t memory_size, uint64_t align) {
    const size_t at = kSegmentsAt + index * 56;
    put_be32(b, at, type);
    put_be32(b, at + 4, flags);
    put_be64(b, at + 8, offset);
    put_be64(b, at + 16, address);
    put_be64(b, at + 24, address);
    put_be64(b, at + 32, file_size);
    put_be64(b, at + 40, memory_size);
    put_be64(b, at + 48, align);
}

inline void put_section(Bytes& b, size_t index, uint32_t name, uint32_t type, uint64_t flags, uint64_t address, uint64_t offset,
                        uint64_t size) {
    const size_t at = kSectionsAt + index * 64;
    put_be32(b, at, name);
    put_be32(b, at + 4, type);
    put_be64(b, at + 8, flags);
    put_be64(b, at + 16, address);
    put_be64(b, at + 24, offset);
    put_be64(b, at + 32, size);
}

// A program with one function (blr), one imported library ("cellTest", two
// functions, each with its stub), one export entry, process and PRX
// parameters, and four bytes of thread-local data.
inline Bytes make_elf() {
    Bytes b(kSectionsAt + 4 * 64);
    const uint8_t ident[8] = {0x7f, 'E', 'L', 'F', 2, 2, 1, 0x66};
    std::memcpy(b.data(), ident, 8);
    put_be16(b, 16, 2);    // executable
    put_be16(b, 18, 21);   // PowerPC64
    put_be32(b, 20, 1);
    put_be64(b, 24, kEntry);
    put_be64(b, 32, kSegmentsAt);
    put_be64(b, 40, kSectionsAt);
    put_be16(b, 52, 64);
    put_be16(b, 54, 56);
    put_be16(b, 56, kSegments);
    put_be16(b, 58, 64);
    put_be16(b, 60, 4);
    put_be16(b, 62, 3);

    put_segment(b, 0, 1, 5, kTextAt, kText, kTextSize, kTextSize, 0x10000);
    put_segment(b, 1, 1, 6, kDataAt, kData, kDataSize, kDataMemory, 0x10000);
    put_segment(b, 2, 7, 4, kDataAt + 0x10, kTlsData, 4, 0x40, 8);
    put_segment(b, 3, 0x60000001, 0, kTextAt + 0x80, kProcParam, 0x20, 0x20, 8);
    put_segment(b, 4, 0x60000002, 0, kTextAt + 0xa0, kPrxParam, 0x28, 0x28, 4);

    const auto text = [](uint32_t address) { return kTextAt + (address - kText); };
    put_be32(b, kTextAt, 0x4e800020);  // blr
    put_stub(b, text(kStub0), kSlots);
    put_stub(b, text(kStub1), kSlots + 4);
    put_text(b, text(kName), "cellTest");
    put_be32(b, text(kIds), kId0);
    put_be32(b, text(kIds) + 4, kId1);
    // Process parameters.
    put_be32(b, text(kProcParam), 0x20);
    put_be32(b, text(kProcParam) + 4, 0x13bcc5f6);
    put_be32(b, text(kProcParam) + 8, 0x00009000);
    put_be32(b, text(kProcParam) + 12, 0x00102000);
    put_be32(b, text(kProcParam) + 16, 1001);
    put_be32(b, text(kProcParam) + 20, 0x10000);
    put_be32(b, text(kProcParam) + 24, 0x100000);
    // PRX parameters.
    put_be32(b, text(kPrxParam), 0x28);
    put_be32(b, text(kPrxParam) + 4, 0x1b434cec);
    put_be32(b, text(kPrxParam) + 8, 1);
    put_be32(b, text(kPrxParam) + 16, kExport);
    put_be32(b, text(kPrxParam) + 20, kExport + 0x1c);
    put_be32(b, text(kPrxParam) + 24, kImport);
    put_be32(b, text(kPrxParam) + 28, kImport + 0x2c);
    // The import entry.
    b[text(kImport)] = 0x2c;
    put_be16(b, text(kImport) + 2, 1);
    put_be16(b, text(kImport) + 4, 9);
    put_be16(b, text(kImport) + 6, 2);
    put_be32(b, text(kImport) + 16, kName);
    put_be32(b, text(kImport) + 20, kIds);
    put_be32(b, text(kImport) + 24, kSlots);
    // The export entry: the program's own, unnamed, one function.
    b[text(kExport)] = 0x1c;
    put_be16(b, text(kExport) + 4, 0x8000);
    put_be16(b, text(kExport) + 6, 1);
    put_be32(b, text(kExport) + 20, kExportIds);
    put_be32(b, text(kExport) + 24, kExportAddresses);
    put_be32(b, text(kExportIds), kExportId);
    put_be32(b, text(kExportAddresses), kEntry);

    put_be32(b, kDataAt, kText);  // the entry point's descriptor
    put_be32(b, kDataAt + 4, kToc);
    put_be32(b, kDataAt + 8, kStub0);
    put_be32(b, kDataAt + 12, kStub1);
    put_be32(b, kDataAt + 16, 0xdeadbeef);

    put_text(b, kNamesAt, std::string_view("\0.text\0.data\0.shstrtab\0", 23));
    put_section(b, 1, 1, 1, 6, kText, kTextAt, kTextSize);
    put_section(b, 2, 7, 1, 3, kData, kDataAt, kDataSize);
    put_section(b, 3, 13, 3, 0, 0, kNamesAt, 23);
    return b;
}

// The SELF that would hold `elf`: the plain header with its copies of the
// ELF's tables, a list of SDK libraries, and the ELF's bytes scrambled where
// the real thing is encrypted.
inline constexpr size_t kSelfHeader = 0x400;
inline Bytes make_self(const Bytes& elf, std::string_view stamps = "crt0:p102002libc:p102002libc:p102002libgcm_sys_stub:p102002") {
    Bytes b(kSelfHeader + elf.size() + stamps.size());
    put_text(b, 0, std::string_view("SCE\0", 4));
    put_be32(b, 4, 2);
    put_be16(b, 8, 1);
    put_be16(b, 10, 1);
    put_be64(b, 0x10, kSelfHeader);
    put_be64(b, 0x18, elf.size() + stamps.size());
    const size_t app = 0x70, header = 0x90, segments = 0xd0, info = segments + kSegments * 56, version = info + kSegments * 32;
    put_be64(b, 0x28, app);
    put_be64(b, 0x30, header);
    put_be64(b, 0x38, segments);
    put_be64(b, 0x40, kSelfHeader + kSectionsAt);
    put_be64(b, 0x48, info);
    put_be64(b, 0x50, version);
    put_be64(b, app, 0x1010000001000003);
    put_be32(b, app + 8, 0x01000002);
    put_be32(b, app + 12, 4);
    std::memcpy(b.data() + header, elf.data(), 64);
    std::memcpy(b.data() + segments, elf.data() + kSegmentsAt, kSegments * 56);
    for (size_t i = 0; i < elf.size(); ++i) b[kSelfHeader + i] = static_cast<uint8_t>(elf[i] ^ 0xa5);
    std::memcpy(b.data() + kSelfHeader + kSectionsAt, elf.data() + kSectionsAt, 4 * 64);  // the section headers stay plain
    for (size_t i = 0; i < kSegments; ++i) {
        const size_t at = kSegmentsAt + i * 56;
        uint64_t offset = 0, size = 0;
        for (int k = 0; k < 8; ++k) {
            offset = offset << 8 | elf[at + 8 + static_cast<size_t>(k)];
            size = size << 8 | elf[at + 32 + static_cast<size_t>(k)];
        }
        put_be64(b, info + i * 32, kSelfHeader + offset);
        put_be64(b, info + i * 32 + 8, size);
        put_be32(b, info + i * 32 + 16, 1);
        put_be32(b, info + i * 32 + 28, i < 2 ? 1 : 0);
    }
    put_be32(b, version, 1);
    put_be32(b, version + 4, 1);
    put_be32(b, version + 8, 0x30);
    put_be64(b, version + 0x20, kSelfHeader + elf.size());
    put_be64(b, version + 0x28, stamps.size());
    put_text(b, kSelfHeader + elf.size(), stamps);
    return b;
}

// A PARAM.SFO with the given entries; a value that is all digits after '#'
// ("#29") is a number.
inline Bytes make_sfo(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string keys;
    Bytes data;
    Bytes index(entries.size() * 16);
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& [key, value] = entries[i];
        put_le16(index, i * 16, static_cast<uint16_t>(keys.size()));
        keys += key;
        keys.push_back('\0');
        put_le32(index, i * 16 + 12, static_cast<uint32_t>(data.size()));
        if (!value.empty() && value[0] == '#') {
            put_le16(index, i * 16 + 2, 0x0404);
            put_le32(index, i * 16 + 4, 4);
            put_le32(index, i * 16 + 8, 4);
            data.resize(data.size() + 4);
            put_le32(data, data.size() - 4, static_cast<uint32_t>(std::stoul(value.substr(1))));
        } else {
            put_le16(index, i * 16 + 2, 0x0204);
            put_le32(index, i * 16 + 4, static_cast<uint32_t>(value.size() + 1));
            put_le32(index, i * 16 + 8, static_cast<uint32_t>(value.size() + 1));
            data.insert(data.end(), value.begin(), value.end());
            data.push_back(0);
        }
    }
    while (keys.size() % 4) keys.push_back('\0');
    Bytes b(20);
    put_text(b, 0, std::string_view("\0PSF", 4));
    put_le32(b, 4, 0x101);
    put_le32(b, 8, static_cast<uint32_t>(20 + index.size()));
    put_le32(b, 12, static_cast<uint32_t>(20 + index.size() + keys.size()));
    put_le32(b, 16, static_cast<uint32_t>(entries.size()));
    b.insert(b.end(), index.begin(), index.end());
    b.insert(b.end(), keys.begin(), keys.end());
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

}  // namespace elfb

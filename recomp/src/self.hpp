// SELF reader: the plain parts of EBOOT.BIN.
//
// A PS3 disc holds the game's program as a SELF ("signed ELF"): the ELF's
// segments encrypted with keys this project does not have and will not
// carry, wrapped in a header that is not encrypted. That header holds a copy
// of the ELF header, its program headers and its section headers, so the
// program's shape (what is loaded where, how big each section is) can be
// read from the disc as it is. Some SELFs also leave plain a list of the SDK
// libraries the program was linked from, with their versions.
//
// Nothing here decrypts anything. The decrypted program is the player's to
// make from their own disc (docs/elf-analysis.md). Runtime code: no
// std::format.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "elf.hpp"

namespace qilin {

struct SelfSegment {
    uint64_t offset = 0, size = 0;       // in the SELF file
    uint32_t compression = 0;            // as stored: 1 not compressed, 2 deflated
    uint32_t encryption = 0;             // as stored (the game's loaded segments: 1)
};

// A library the program was linked from, as the SDK stamps it: "libc",
// "p102002" (SDK 1.02), and how many of its object files are in the program.
struct SdkLibrary {
    std::string name, version;
    unsigned objects = 0;
};

struct Self {
    uint32_t version = 0;
    uint16_t key_revision = 0, header_type = 0;
    uint64_t header_size = 0, data_size = 0;
    uint64_t authentication_id = 0, application_version = 0;
    uint32_t vendor_id = 0, self_type = 0;
    ElfLayout elf;                      // the plain copy: sections have no names
    std::vector<SelfSegment> segments;  // one per program header
    std::vector<SdkLibrary> libraries;  // empty if the SELF has no plain list
    std::string sha256;
    uint64_t file_size = 0;

    static bool is_self(std::span<const uint8_t> file);
    static std::expected<Self, std::string> parse(std::span<const uint8_t> file);
    static std::expected<Self, std::string> load(const std::filesystem::path& path);
};

// "crt0:p102002crt1:p102002...": the SDK's stamps, run together.
std::expected<std::vector<SdkLibrary>, std::string> parse_sdk_stamps(std::string_view text);

// Whether a decrypted ELF is this SELF's program: the same entry point and
// the same program headers. Empty if so, else what differs.
std::string differs_from(const Self& self, const ElfLayout& elf);

}  // namespace qilin

// The SPU programs a PS3 program carries inside itself.
//
// An SPU runs a program of its own, with its own instruction set, in its own
// 256 KiB of memory. The PPU program holds each one as a whole ELF file in
// its data (32-bit, big-endian, machine 23), and hands its address to the
// system (SPURS, or a raw SPU thread) to load. This finds them: every such
// ELF in the program's loaded data, validated, with its segments, its code
// and the name its `.note.spu_name` section gives. Runtime code: no
// std::format.
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "elf.hpp"

namespace qilin {

inline constexpr uint16_t kEmSpu = 23;
inline constexpr uint32_t kSpuMemory = 0x40000;  // an SPU's local store

struct SpuSegment {
    uint32_t type = 0, flags = 0;
    uint32_t offset = 0, address = 0, file_size = 0, memory_size = 0;
};

struct SpuSection {
    std::string name;
    uint32_t type = 0, flags = 0, address = 0, offset = 0, size = 0;
};

struct SpuImage {
    uint32_t address = 0;  // where the ELF file lies in the PPU program's memory
    uint32_t size = 0;     // the ELF file's length: to the end of its section headers
    uint32_t entry = 0;
    std::string name;      // from .note.spu_name; empty if it has none
    std::vector<SpuSegment> segments;
    std::vector<SpuSection> sections;

    uint32_t code_size() const;                 // its executable sections, in bytes
    const SpuSection* section(std::string_view section_name) const;
};

// One image from the bytes of an ELF file (`file` may run past its end).
std::expected<SpuImage, std::string> parse_spu_image(std::span<const uint8_t> file, uint32_t address);

// Every SPU image in the program's loaded segments, in address order. An
// ELF header for the SPU that doesn't hold up is an error, not skipped.
std::expected<std::vector<SpuImage>, std::string> find_spu_images(const Elf& elf);

}  // namespace qilin

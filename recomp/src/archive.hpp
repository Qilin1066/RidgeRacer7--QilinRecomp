// The directory of the game's archive (RR7.DAT), which is not in the archive
// but in the program's data.
//
// The archive is a run of **packs**, one after another in 2048-byte sectors.
// A pack holds a few files that are loaded together. It is stored either as
// it is, or compressed, in which case its bytes are pushed to the end of its
// sectors (so it can be unpacked in place, from the front of the buffer it
// was read into). The program holds two tables, one after the other:
//
//   files, 20 bytes each:  a position in sectors (see below), the number of
//       sectors the file spans, its alignment (64 or 2048), its size, its
//       offset in its pack's unpacked bytes, the address of its name;
//   packs, 36 bytes each:  the first sector, the number of sectors, the
//       first file's number and one past the last's, whether it is
//       compressed, the stored and unpacked sizes if so, three words (the
//       first is small and usually 0; not understood), the address of its
//       name.
//
// All of it as read from the owner's program (docs/data-files.md); the
// reader finds the tables by their shape and checks every relation between
// them, so a build that differs fails here, not later. A file's first field
// is its first sector in the archive when its pack is stored plain; for a
// compressed pack it is some position in the unpacked image, not understood
// and not used.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "elf.hpp"

namespace qilin {

inline constexpr uint32_t kArchiveSector = 2048;
inline constexpr size_t kArchiveFileRecord = 20, kArchivePackRecord = 36;

struct ArchiveFile {
    std::string name;       // not unique: "Tex", "Mdl", ... once per pack that has one
    uint32_t pack = 0;      // which pack; kNoPack for the few that are in none
    uint32_t offset = 0;    // in the pack's unpacked bytes
    uint32_t size = 0;
    uint32_t align = 0;
    static constexpr uint32_t kNoPack = 0xffffffff;
};

struct ArchivePack {
    std::string name;        // unique
    uint32_t sector = 0, sectors = 0;
    uint32_t first_file = 0, files = 0;
    bool compressed = false;
    uint32_t stored_size = 0;  // compressed: the bytes in the archive, which end at the pack's last sector's end
    uint32_t size = 0;         // unpacked; for a plain pack, its sectors' bytes

    uint64_t stored_at() const {  // where its bytes begin in the archive
        return uint64_t{sector} * kArchiveSector + (compressed ? uint64_t{sectors} * kArchiveSector - stored_size : 0);
    }
};

struct Archive {
    uint32_t files_at = 0, packs_at = 0;  // the tables' addresses in the program
    std::vector<ArchiveFile> files;
    std::vector<ArchivePack> packs;
    uint64_t size() const { return packs.empty() ? 0 : (uint64_t{packs.back().sector} + packs.back().sectors) * kArchiveSector; }
};

// Memory to search: `bytes` at `address`. `text` gives the string at an
// address (empty if there is none there).
struct ArchiveMemory {
    uint32_t address = 0;
    std::span<const uint8_t> bytes;
};
std::expected<Archive, std::string> find_archive(std::span<const ArchiveMemory> memory, const std::function<std::string(uint32_t)>& text);

// In a program: its writable data is searched, and names are read from anywhere in it.
std::expected<Archive, std::string> find_archive(const Elf& elf);

}  // namespace qilin

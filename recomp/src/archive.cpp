#include "archive.hpp"

#include <format>

#include "bytes.hpp"

namespace qilin {

namespace {

constexpr size_t kFewestPacks = 8;  // fewer in a row is taken for chance

struct PackRecord {
    uint32_t sector, stored, unpacked, name;
    uint16_t sectors, first, end, compressed;
};

PackRecord pack_at(const uint8_t* p) {
    return {read_be32(p + 4), read_be32(p + 16), read_be32(p + 20), read_be32(p), read_be16(p + 8), read_be16(p + 10), read_be16(p + 12), read_be16(p + 14)};
}

}  // namespace

std::expected<Archive, std::string> find_archive(std::span<const ArchiveMemory> memory, const std::function<std::string(uint32_t)>& text) {
    // The pack table: records that follow one another sector by sector from sector 0, each beginning with its name's address.
    // (A record's name is its first word. This was first read as its last, which gave every pack and file its neighbour's name:
    // the game's own table of pointers to the records, seen at run time, settled it.)
    for (const ArchiveMemory& m : memory) {
        const size_t size = m.bytes.size();
        for (size_t at = 0; at + kFewestPacks * kArchivePackRecord <= size; at += 4) {
            const uint8_t* p = m.bytes.data() + at;
            // Cheap tests first: sector 0, some sectors, files from number 0.
            if (read_be32(p + 4) != 0 || read_be16(p + 8) == 0 || read_be16(p + 10) != 0 || read_be16(p + 14) > 1) continue;
            std::vector<PackRecord> records;
            uint32_t next_sector = 0;
            for (size_t q = at; q + kArchivePackRecord <= size; q += kArchivePackRecord) {
                const PackRecord r = pack_at(m.bytes.data() + q);
                if (r.sector != next_sector || r.compressed > 1 || text(r.name).empty()) break;
                records.push_back(r);
                next_sector += r.sectors;
            }
            if (records.size() < kFewestPacks) continue;

            // Found. From here on anything that doesn't fit is an error.
            Archive archive;
            archive.packs_at = m.address + static_cast<uint32_t>(at);
            const auto fail = [&](std::string what) {
                return std::unexpected(std::format("the archive's directory (packs at {:#x}): {}", archive.packs_at, what));
            };
            uint32_t files = 0;
            for (const PackRecord& r : records) files = std::max<uint32_t>(files, r.end);
            if (files == 0) return fail("no files");
            if (uint64_t{files} * kArchiveFileRecord > at) return fail(std::format("{} files don't fit before the packs", files));
            const size_t files_at = at - size_t{files} * kArchiveFileRecord;
            archive.files_at = m.address + static_cast<uint32_t>(files_at);
            archive.files.resize(files);
            for (uint32_t i = 0; i < files; ++i) {
                const uint8_t* f = m.bytes.data() + files_at + size_t{i} * kArchiveFileRecord;
                ArchiveFile& file = archive.files[i];
                file.pack = ArchiveFile::kNoPack;
                file.align = read_be16(f + 10);
                file.size = read_be32(f + 12);
                file.offset = read_be32(f + 16);
                file.name = text(read_be32(f));
                if (file.name.empty()) return fail(std::format("file {} has no name", i));
                if (file.align != 0 && file.align != 64 && file.align != kArchiveSector)  // (0: one file has none)
                    return fail(std::format("file {} ({}) is aligned to {}", i, file.name, file.align));
                if (file.align && file.offset % file.align) return fail(std::format("file {} ({}) is at {:#x}, off its alignment", i, file.name, file.offset));
            }
            for (const PackRecord& r : records) {
                ArchivePack pack;
                pack.name = text(r.name);
                pack.sector = r.sector;
                pack.sectors = r.sectors;
                pack.compressed = r.compressed;
                const uint32_t index = static_cast<uint32_t>(archive.packs.size());
                const std::string which = std::format("pack {} ({})", index, pack.name);
                const uint64_t room = uint64_t{pack.sectors} * kArchiveSector;
                if (pack.compressed) {
                    pack.stored_size = r.stored;
                    pack.size = r.unpacked;
                    if (pack.stored_size == 0 || pack.stored_size > room || (room - pack.stored_size) >= kArchiveSector)
                        return fail(std::format("{}: {:#x} stored bytes in {} sectors", which, pack.stored_size, pack.sectors));
                    if (pack.size == 0) return fail(which + " unpacks to nothing");
                } else {
                    if (r.stored || r.unpacked) return fail(which + " is stored plain but has compressed sizes");
                    pack.size = static_cast<uint32_t>(room);
                }
                if (r.end != 0 || r.sectors != 0) {  // (an empty pack has no sectors and names no files)
                    if (r.end <= r.first) return fail(which + " holds no files");
                    pack.first_file = r.first;
                    pack.files = r.end - r.first;
                    uint32_t previous_end = 0;
                    for (uint32_t i = r.first; i < r.end; ++i) {
                        ArchiveFile& file = archive.files[i];
                        if (file.pack != ArchiveFile::kNoPack) return fail(std::format("file {} ({}) is in two packs", i, file.name));
                        file.pack = index;
                        // (Checked for files in packs only: the few in none say they span no sectors.)
                        const uint8_t* f = m.bytes.data() + files_at + size_t{i} * kArchiveFileRecord;
                        const uint32_t spans = (file.offset % kArchiveSector + file.size + kArchiveSector - 1) / kArchiveSector;
                        if (read_be16(f + 8) != spans)
                            return fail(std::format("file {} ({}) says it spans {} sectors, and its size says {}", i, file.name, read_be16(f + 8), spans));
                        if (file.offset < previous_end || uint64_t{file.offset} + file.size > pack.size)
                            return fail(std::format("{}: file {} ({}), {:#x} bytes at {:#x}, overlaps the one before or runs past the pack's {:#x} bytes", which, i,
                                                    file.name, file.size, file.offset, pack.size));
                        previous_end = file.offset + file.size;
                    }
                }
                for (const ArchivePack& other : archive.packs)
                    if (other.name == pack.name) return fail("two packs are called " + pack.name);
                archive.packs.push_back(std::move(pack));
            }
            return archive;
        }
    }
    return std::unexpected(std::string("the archive's directory wasn't found in the program's data"));
}

std::expected<Archive, std::string> find_archive(const Elf& elf) {
    std::vector<ArchiveMemory> memory;
    for (const ElfSegment& s : elf.segments())
        if (s.type == kPtLoad && (s.flags & kPfWrite) && !(s.flags & kPfExec) && s.file_size) {
            const auto bytes = elf.bytes_at(s.address, s.file_size);
            if (!bytes.empty()) memory.push_back({static_cast<uint32_t>(s.address), bytes});
        }
    // A name: printable ASCII with its zero byte, anywhere in the file's image.
    const auto text = [&](uint32_t address) -> std::string {
        std::string name;
        for (uint32_t a = address; name.size() < 256; ++a) {
            const auto byte = elf.bytes_at(a, 1);
            if (byte.empty()) return {};
            if (byte[0] == 0) return name;
            if (byte[0] < 0x20 || byte[0] > 0x7e) return {};
            name += static_cast<char>(byte[0]);
        }
        return {};
    };
    return find_archive(memory, text);
}

}  // namespace qilin

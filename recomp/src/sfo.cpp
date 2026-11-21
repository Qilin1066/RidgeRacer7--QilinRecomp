#include "sfo.hpp"

#include "bytes.hpp"
#include "elf.hpp"

namespace qilin {

std::expected<Sfo, std::string> Sfo::load(const std::filesystem::path& path) {
    auto bytes = read_file(path);
    if (!bytes) return std::unexpected(bytes.error());
    auto sfo = parse(*bytes);
    if (!sfo) return std::unexpected(path.string() + ": " + sfo.error());
    return sfo;
}

std::expected<Sfo, std::string> Sfo::parse(std::span<const uint8_t> file) {
    if (file.size() < 20 || file[0] != 0 || file[1] != 'P' || file[2] != 'S' || file[3] != 'F')
        return std::unexpected(std::string("not a PARAM.SFO"));
    const uint8_t* d = file.data();
    const uint32_t keys = read_le32(d + 8), data = read_le32(d + 12), count = read_le32(d + 16);
    if (keys > file.size() || data > file.size() || count > (file.size() - 20) / 16)
        return std::unexpected(std::string("the PARAM.SFO's tables are outside the file"));
    Sfo sfo;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = d + 20 + i * 16;
        const uint32_t key = read_le16(e), format = read_le16(e + 2), length = read_le32(e + 4), at = read_le32(e + 12);
        SfoEntry entry;
        for (size_t k = size_t(keys) + key;; ++k) {
            if (k >= file.size()) return std::unexpected("PARAM.SFO entry " + std::to_string(i) + "'s key has no end");
            if (d[k] == 0) break;
            entry.key.push_back(static_cast<char>(d[k]));
        }
        if (at > file.size() - data || length > file.size() - data - at)
            return std::unexpected("PARAM.SFO's " + entry.key + " is outside the file");
        const uint8_t* value = d + data + at;
        if (format == 0x0404) {
            if (length != 4) return std::unexpected("PARAM.SFO's " + entry.key + " is a number of " + std::to_string(length) + " bytes");
            entry.integer = true;
            entry.number = read_le32(value);
        } else if (format == 0x0204 || format == 0x0004) {
            entry.text.assign(reinterpret_cast<const char*>(value), length);
            if (const size_t end = entry.text.find('\0'); end != entry.text.npos) entry.text.resize(end);
        } else {
            return std::unexpected("PARAM.SFO's " + entry.key + " has an unknown format (" + hex(format) + ")");
        }
        sfo.entries.push_back(std::move(entry));
    }
    return sfo;
}

std::optional<std::string> Sfo::text(std::string_view key) const {
    for (const SfoEntry& e : entries)
        if (e.key == key && !e.integer) return e.text;
    return std::nullopt;
}

std::optional<uint32_t> Sfo::number(std::string_view key) const {
    for (const SfoEntry& e : entries)
        if (e.key == key && e.integer) return e.number;
    return std::nullopt;
}

namespace {

std::string names(uint32_t bits, std::span<const char* const> table) {
    std::string out;
    for (size_t i = 0; i < 32; ++i) {
        if (!(bits >> i & 1)) continue;
        if (!out.empty()) out += ", ";
        out += i < table.size() ? std::string(table[i]) : "bit " + std::to_string(i);
    }
    return out.empty() ? "none" : out;
}

}  // namespace

std::string sfo_resolutions(uint32_t bits) {
    static constexpr const char* kNames[] = {"480", "576", "720", "1080", "480 (16:9)", "576 (16:9)"};
    return names(bits, kNames);
}

std::string sfo_sound_formats(uint32_t bits) {
    // The compressed formats are two bits each, sharing bit 1.
    struct Format {
        uint32_t mask;
        const char* name;
    };
    static constexpr Format kFormats[] = {
        {0x001, "LPCM 2.0"}, {0x004, "LPCM 5.1"}, {0x010, "LPCM 7.1"}, {0x102, "Dolby Digital"}, {0x202, "DTS"}};
    std::string out;
    uint32_t known = 0;
    for (const Format& f : kFormats) {
        if ((bits & f.mask) != f.mask) continue;
        if (!out.empty()) out += ", ";
        out += f.name;
        known |= f.mask;
    }
    if (const uint32_t rest = bits & ~known) out += (out.empty() ? "" : ", ") + ("unknown bits " + hex(rest));
    return out.empty() ? "none" : out;
}

}  // namespace qilin

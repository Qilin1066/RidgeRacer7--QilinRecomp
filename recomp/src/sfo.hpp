// PARAM.SFO: the disc's own description of the game (its title, its id, its
// version, the picture and sound formats it supports). Little-endian, unlike
// everything else on the disc. Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qilin {

struct SfoEntry {
    std::string key;
    bool integer = false;
    uint32_t number = 0;  // if integer
    std::string text;     // otherwise
};

struct Sfo {
    std::vector<SfoEntry> entries;

    static std::expected<Sfo, std::string> parse(std::span<const uint8_t> file);
    static std::expected<Sfo, std::string> load(const std::filesystem::path& path);

    // The file's bytes for these entries, in their order (a console's are in the keys' order).
    std::vector<uint8_t> bytes() const;

    std::optional<std::string> text(std::string_view key) const;
    std::optional<uint32_t> number(std::string_view key) const;
};

// RESOLUTION's bits as the picture modes they stand for ("480p, 720p, 1080i,
// 1080p"), and SOUND_FORMAT's as the sound formats.
std::string sfo_resolutions(uint32_t bits);
std::string sfo_sound_formats(uint32_t bits);

}  // namespace qilin

// The headers of the game's sound banks (.nub) and films (.pam).
//
// Both layouts are as read from the owner's disc (docs/data-files.md says
// what was seen and what is inferred). Only the headers are read: the files
// run to hundreds of megabytes. Anything the reader doesn't know (a sound of
// an unknown kind, a stream of an unknown coding) is an error, not a guess.
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace qilin {

// ---- .nub: a bank of sounds ----
//
// A 32-byte header, a table of offsets (one per sound), the sounds' headers,
// then the sounds' data from `data_at`. All big-endian, except the format
// block of an ATRAC3plus sound, which is the little-endian block a RIFF file
// would hold.
enum class NubCodec : uint8_t { pcm16_be, atrac3plus };

struct NubSound {
    uint32_t index = 0;
    NubCodec codec = NubCodec::pcm16_be;
    uint32_t data_offset = 0, data_size = 0;  // within the bank's data
    uint16_t channels = 0, bits = 0, block_align = 0;
    uint32_t sample_rate = 0, bytes_per_second = 0;
};

struct NubBank {
    uint32_t version = 0, id = 0;
    uint32_t data_at = 0, data_size = 0;
    std::vector<NubSound> sounds;

    // `head`: the file from its start to at least `data_at` (headers_size()
    // says how much that is, from the first 32 bytes).
    static std::expected<NubBank, std::string> parse(std::span<const uint8_t> head, uint64_t file_size);
    static std::expected<uint32_t, std::string> headers_size(std::span<const uint8_t> first32);
    static std::expected<NubBank, std::string> load(const std::filesystem::path& path);
};

// ---- .pam: a film (PAMF: an MPEG-2 program stream behind a header) ----
inline constexpr uint8_t kPamfMpeg2Video = 0x02, kPamfAvcVideo = 0x1b, kPamfAtrac3plus = 0xdc, kPamfLpcm = 0x80,
                         kPamfAc3 = 0x81;
inline constexpr size_t kPamfHeaderRead = 0x800;

struct PamfStream {
    uint8_t coding = 0;     // one of the kPamf* codings
    uint8_t stream_id = 0;  // the program stream's id: 0xe0 video, 0xbd private (audio)
    uint8_t sub_id = 0;
    uint16_t width = 0, height = 0;  // video, in pixels
    bool video() const { return coding == kPamfMpeg2Video || coding == kPamfAvcVideo; }
};

struct Pamf {
    std::string version;                // "0041"
    uint64_t data_at = 0, data_size = 0;  // the program stream, in bytes
    uint64_t start = 0, end = 0;        // presentation times, 90 000 to the second
    uint32_t mux_rate = 0;              // in units of 50 bytes a second
    std::vector<PamfStream> streams;

    double seconds() const { return static_cast<double>(end - start) / 90000.0; }

    // `head`: the file's first kPamfHeaderRead bytes (or all of it, if shorter).
    static std::expected<Pamf, std::string> parse(std::span<const uint8_t> head, uint64_t file_size);
    static std::expected<Pamf, std::string> load(const std::filesystem::path& path);
};

const char* pamf_coding_name(uint8_t coding);

// `size` bytes of a file from `offset`; an error names the file. Fewer bytes
// than asked is an error.
std::expected<std::vector<uint8_t>, std::string> read_file_part(const std::filesystem::path& path, uint64_t offset, size_t size);

}  // namespace qilin

#include "media.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "bytes.hpp"
#include "elf.hpp"  // hex()

namespace qilin {

namespace {

std::unexpected<std::string> fail(std::string message) { return std::unexpected(std::move(message)); }

// The sound header's fields (offsets within it).
constexpr size_t kNubHeader = 0x20;
constexpr size_t kSoundIndex = 0x08, kSoundSize = 0x14, kSoundOffset = 0x18, kSoundFormatSize = 0x1c, kSoundFormat = 0xbc;
// The sub-format of ATRAC3plus in a WAVE_FORMAT_EXTENSIBLE block.
constexpr uint8_t kAtrac3plusGuid[16] = {0xbf, 0xaa, 0x23, 0xe9, 0x58, 0xcb, 0x71, 0x44,
                                         0xa1, 0x19, 0xff, 0xfa, 0x01, 0xe4, 0xce, 0x62};
constexpr uint16_t kWaveFormatPcm = 1, kWaveFormatExtensible = 0xfffe;

// The film header's fields.
constexpr size_t kPamfTableSize = 0x50, kPamfStart = 0x56, kPamfEnd = 0x5c, kPamfMuxRate = 0x62, kPamfStreamCount = 0x86,
                 kPamfStreams = 0x88, kPamfStream = 0x30, kPamfTableFixed = 0x34;

uint64_t read_be48(const uint8_t* p) { return uint64_t(read_be16(p)) << 32 | read_be32(p + 2); }

}  // namespace

std::expected<std::vector<uint8_t>, std::string> read_file_part(const std::filesystem::path& path, uint64_t offset, size_t size) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open " + path.string());
    std::vector<uint8_t> bytes(size);
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!f || static_cast<size_t>(f.gcount()) != size)
        return fail(path.string() + ": cannot read " + hex(size) + " bytes at " + hex(offset));
    return bytes;
}

// ---------------------------------------------------------------- .nub

std::expected<uint32_t, std::string> NubBank::headers_size(std::span<const uint8_t> first32) {
    if (first32.size() < kNubHeader) return fail("shorter than a sound bank's header");
    const uint32_t data_at = read_be32(first32.data() + 0x10);
    if (data_at < kNubHeader || data_at > (64u << 20)) return fail("sound bank: its data starts at " + hex(data_at) + ", which can't be");
    return data_at;
}

std::expected<NubBank, std::string> NubBank::parse(std::span<const uint8_t> head, uint64_t file_size) {
    const auto data_at = headers_size(head);
    if (!data_at) return fail(data_at.error());
    const uint8_t* h = head.data();
    NubBank bank;
    bank.version = read_be32(h);
    bank.id = read_be32(h + 8);
    const uint32_t count = read_be32(h + 0xc), offsets_at = read_be32(h + 0x18), first_at = read_be32(h + 0x1c);
    bank.data_at = *data_at;
    bank.data_size = read_be32(h + 0x14);
    if (bank.version != 0x00020100) return fail("sound bank: version " + hex(bank.version) + ", expected 0x20100");
    if (read_be32(h + 4) != 0) return fail("sound bank: the header's second word is " + hex(read_be32(h + 4)) + ", expected 0");
    if (head.size() < bank.data_at) return fail("sound bank: the headers run to " + hex(bank.data_at) + " and only " + hex(head.size()) + " bytes were read");
    if (uint64_t{bank.data_at} + bank.data_size > file_size)
        return fail("sound bank: its data runs to " + hex(uint64_t{bank.data_at} + bank.data_size) + ", past the file's end at " + hex(file_size));
    if (count == 0) return fail("sound bank: no sounds");
    if (offsets_at < kNubHeader || uint64_t{offsets_at} + uint64_t{count} * 4 > first_at || first_at > bank.data_at)
        return fail("sound bank: the table of " + std::to_string(count) + " offsets at " + hex(offsets_at) + " doesn't fit before the sounds' headers at " + hex(first_at));
    bank.sounds.reserve(count);
    uint32_t previous = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = read_be32(h + offsets_at + size_t{i} * 4);
        const std::string where = "sound " + std::to_string(i) + " at " + hex(at);
        const uint32_t end = i + 1 < count ? read_be32(h + offsets_at + size_t{i + 1} * 4) : bank.data_at;
        if (at < first_at || at <= previous || end > bank.data_at || end < at || end - at < kSoundFormat)
            return fail("sound bank: " + where + ": its header is out of place");
        previous = at;
        const uint8_t* s = h + at;
        NubSound sound;
        sound.index = read_be32(s + kSoundIndex);
        sound.data_size = read_be32(s + kSoundSize);
        sound.data_offset = read_be32(s + kSoundOffset);
        const uint32_t format_size = read_be32(s + kSoundFormatSize);
        if (sound.index != i) return fail("sound bank: " + where + " says it is sound " + std::to_string(sound.index));
        if (read_be32(s + 4) != bank.id) return fail("sound bank: " + where + " belongs to bank " + hex(read_be32(s + 4)) + ", not " + hex(bank.id));
        if (uint64_t{sound.data_offset} + sound.data_size > bank.data_size)
            return fail("sound bank: " + where + ": its data (" + hex(sound.data_size) + " bytes at " + hex(sound.data_offset) + ") runs past the bank's");
        if (format_size < 16 || kSoundFormat + uint64_t{format_size} > end - at)
            return fail("sound bank: " + where + ": a format block of " + hex(format_size) + " bytes doesn't fit its header");
        const uint8_t* f = s + kSoundFormat;
        if (std::memcmp(s, "wav\0", 4) == 0) {
            if (read_be16(f) != kWaveFormatPcm) return fail("sound bank: " + where + ": a wav of format " + hex(read_be16(f)) + ", not PCM");
            sound.codec = NubCodec::pcm16_be;
            sound.channels = read_be16(f + 2);
            sound.sample_rate = read_be32(f + 4);
            sound.bytes_per_second = read_be32(f + 8);
            sound.block_align = read_be16(f + 12);
            sound.bits = read_be16(f + 14);
            if (sound.bits != 16) return fail("sound bank: " + where + ": PCM of " + std::to_string(sound.bits) + " bits, expected 16");
            if (sound.block_align != sound.channels * 2 || sound.bytes_per_second != sound.sample_rate * sound.block_align)
                return fail("sound bank: " + where + ": its PCM format doesn't add up");
        } else if (std::memcmp(s, "at3\0", 4) == 0) {
            if (format_size < 40 || read_le16(f) != kWaveFormatExtensible || std::memcmp(f + 24, kAtrac3plusGuid, 16) != 0)
                return fail("sound bank: " + where + ": an at3 that isn't ATRAC3plus (format " + hex(read_le16(f)) + ")");
            sound.codec = NubCodec::atrac3plus;
            sound.channels = read_le16(f + 2);
            sound.sample_rate = read_le32(f + 4);
            sound.bytes_per_second = read_le32(f + 8);
            sound.block_align = read_le16(f + 12);
            sound.bits = read_le16(f + 14);
        } else {
            return fail("sound bank: " + where + ": a sound of an unknown kind (tag " + hex(read_be32(s)) + ")");
        }
        if (sound.channels == 0 || sound.sample_rate == 0 || sound.block_align == 0)
            return fail("sound bank: " + where + ": no channels, rate or block size");
        bank.sounds.push_back(sound);
    }
    return bank;
}

std::expected<NubBank, std::string> NubBank::load(const std::filesystem::path& path) {
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) return fail("cannot open " + path.string());
    const auto first = read_file_part(path, 0, static_cast<size_t>(std::min<uint64_t>(size, kNubHeader)));
    if (!first) return fail(first.error());
    const auto headers = headers_size(*first);
    if (!headers) return fail(path.string() + ": " + headers.error());
    if (*headers > size) return fail(path.string() + ": sound bank: its data starts past the file's end");
    const auto head = read_file_part(path, 0, *headers);
    if (!head) return fail(head.error());
    auto bank = parse(*head, size);
    if (!bank) return fail(path.string() + ": " + bank.error());
    return bank;
}

// ---------------------------------------------------------------- .pam

const char* pamf_coding_name(uint8_t coding) {
    switch (coding) {
    case kPamfMpeg2Video: return "MPEG-2 video";
    case kPamfAvcVideo: return "AVC video";
    case kPamfAtrac3plus: return "ATRAC3plus";
    case kPamfLpcm: return "LPCM";
    case kPamfAc3: return "AC-3";
    default: return nullptr;
    }
}

std::expected<Pamf, std::string> Pamf::parse(std::span<const uint8_t> head, uint64_t file_size) {
    if (head.size() < kPamfStreams || std::memcmp(head.data(), "PAMF", 4) != 0) return fail("not a film (no PAMF header)");
    const uint8_t* h = head.data();
    Pamf film;
    film.version.assign(reinterpret_cast<const char*>(h + 4), 4);
    if (film.version != "0041") return fail("film: PAMF version " + film.version + ", expected 0041");
    film.data_at = uint64_t{read_be32(h + 8)} * 0x800;
    film.data_size = uint64_t{read_be32(h + 12)} * 0x800;
    if (film.data_at == 0 || film.data_at + film.data_size > file_size)
        return fail("film: its stream (" + hex(film.data_size) + " bytes at " + hex(film.data_at) + ") runs past the file's end at " + hex(file_size));
    film.start = read_be48(h + kPamfStart);
    film.end = read_be48(h + kPamfEnd);
    film.mux_rate = read_be32(h + kPamfMuxRate);
    if (film.end <= film.start) return fail("film: it ends (" + hex(film.end) + ") before it starts (" + hex(film.start) + ")");
    const uint32_t table_size = read_be32(h + kPamfTableSize);
    const unsigned count = read_be16(h + kPamfStreamCount);
    if (count == 0) return fail("film: no streams");
    if (table_size != kPamfTableFixed + count * kPamfStream)
        return fail("film: a table of " + hex(table_size) + " bytes for " + std::to_string(count) + " streams: not the layout this reader knows");
    if (kPamfStreams + count * kPamfStream > head.size() || kPamfStreams + count * kPamfStream > film.data_at)
        return fail("film: its " + std::to_string(count) + " streams don't fit the header");
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t* s = h + kPamfStreams + i * kPamfStream;
        PamfStream stream;
        stream.coding = s[0];
        stream.stream_id = s[4];
        stream.sub_id = s[5];
        if (!pamf_coding_name(stream.coding))
            return fail("film: stream " + std::to_string(i) + " has an unknown coding (" + hex(stream.coding) + ")");
        if (stream.video()) {
            // In macroblocks of 16, and for MPEG-2 in pixels too.
            stream.width = static_cast<uint16_t>(read_be16(s + 0x18) * 16);
            stream.height = static_cast<uint16_t>(read_be16(s + 0x1a) * 16);
            if (stream.coding == kPamfMpeg2Video) {
                const uint16_t width = read_be16(s + 0x1c), height = read_be16(s + 0x1e);
                if ((width + 15) / 16 * 16 != stream.width || (height + 15) / 16 * 16 != stream.height)
                    return fail("film: stream " + std::to_string(i) + ": its two sizes disagree");
                stream.width = width;
                stream.height = height;
            }
            if (stream.width == 0 || stream.height == 0) return fail("film: stream " + std::to_string(i) + ": a picture of no size");
        }
        film.streams.push_back(stream);
    }
    return film;
}

std::expected<Pamf, std::string> Pamf::load(const std::filesystem::path& path) {
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) return fail("cannot open " + path.string());
    const auto head = read_file_part(path, 0, static_cast<size_t>(std::min<uint64_t>(size, kPamfHeaderRead)));
    if (!head) return fail(head.error());
    auto film = parse(*head, size);
    if (!film) return fail(path.string() + ": " + film.error());
    return film;
}

}  // namespace qilin

// `qilin identify` (identify.hpp). Developer tool code: std::format is fine here.
#include "identify.hpp"

#include <format>
#include <optional>
#include <system_error>
#include <vector>

#include "self.hpp"
#include "sfo.hpp"
#include "sha256.hpp"

namespace qilin {

namespace fs = std::filesystem;

namespace {

// The one build so far: the owner's disc (docs/elf-analysis.md). The program's hash is of its loaded segments in the
// EBOOT.elf RPCS3 0.0.43 wrote (whose whole file's SHA-256 is title.cpp's 689f9704...).
constexpr SupportedBuild kBuilds[] = {
    {"Ridge Racer 7 (USA)", "BLUS30001", "01.02", "095ed6cb8e90538309b57662fb19da1ba2c4bdbac5fd40dc0be6122e76d4c0db",
     "1e074ef0fcd7282bf6662e37feb799c766b62fd9f67cd7491c3ecd566874fdfc"},
};

// "Ridge Racer 7 (USA), BLUS30001 version 01.02"
std::string describe(const SupportedBuild& b) { return std::format("{}, {} version {}", b.name, b.title_id, b.version); }

Identity answer(int code, std::string message) {
    Identity i;
    i.code = code;
    i.ok = code == 0;
    i.message = std::move(message);
    return i;
}

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20) {
            out += std::format("\\u{:04x}", u);
        } else {
            out += c;
        }
    }
    return out + "\"";
}

}  // namespace

std::span<const SupportedBuild> supported_builds() { return kBuilds; }

std::string program_sha256(const Elf& elf) {
    std::vector<uint8_t> bytes;
    for (const ElfSegment& s : elf.segments()) {
        if (s.type != kPtLoad || s.file_size == 0) continue;
        const auto part = elf.bytes_at(s.address, s.file_size);
        bytes.insert(bytes.end(), part.begin(), part.end());
    }
    return sha256_hex(bytes);
}

Identity identify(const fs::path& input, const IdentifyOptions& options) {
    // Which builds are wanted: --build's, or every one this port supports.
    std::vector<const SupportedBuild*> wanted;
    for (const SupportedBuild& b : options.builds)
        if (options.want.empty() || b.name == options.want) wanted.push_back(&b);
    if (wanted.empty()) return answer(2, std::format("This recompiler doesn't support the build \"{}\": the download is damaged. Unpack it again.", options.want));
    const SupportedBuild& first = *wanted.front();
    const std::string needed = "This download is for " + describe(first) + ".";

    std::error_code ec;
    if (!fs::is_directory(input, ec))
        return answer(2, "That isn't a game folder. Choose the folder holding PS3_GAME, the copy of your Ridge Racer 7 disc. " + needed);
    const auto files = find_game(input);
    if (!files || files->usrdir.empty())
        return answer(2, "There's no PS3 game in that folder. Choose the folder holding PS3_GAME (with PS3_GAME/USRDIR/EBOOT.BIN in it), the copy of "
                         "your Ridge Racer 7 disc. " + needed);
    if (files->sfo.empty())
        return answer(2, "That folder has a PS3 game's USRDIR but no PARAM.SFO beside it, so it can't be checked. Choose the folder holding PS3_GAME, "
                         "copied whole from your disc. " + needed);
    const auto sfo = Sfo::load(files->sfo);
    if (!sfo) return answer(2, "The game folder's PARAM.SFO can't be read: it may be damaged. Copy the disc again. " + needed);

    Identity id;
    id.title = sfo->text("TITLE").value_or("");
    id.id = sfo->text("TITLE_ID").value_or("");
    id.version = sfo->text("VERSION").value_or(sfo->text("APP_VER").value_or(""));
    const auto with = [&](int code, std::string message) {
        Identity out = id;
        out.code = code;
        out.ok = code == 0;
        out.message = std::move(message);
        return out;
    };
    // The game and its version, from PARAM.SFO.
    const SupportedBuild* build = nullptr;
    for (const SupportedBuild* b : wanted)
        if (b->title_id == id.id && b->version == id.version) build = b;
    if (!build) {
        const std::string what = std::format("{} ({}{})", id.title.empty() ? std::string("a PS3 game") : id.title, id.id.empty() ? "no title id" : id.id,
                                             id.version.empty() ? "" : ", version " + id.version);
        return with(3, "This is " + what + ". " + needed);
    }
    id.title = std::string(build->name);
    // EBOOT.BIN as the disc holds it (an update's, or another copy's, is a different program).
    std::optional<Self> self;
    if (!files->self.empty()) {
        auto loaded = Self::load(files->self);
        if (!loaded) return with(2, "The game folder's EBOOT.BIN can't be read: it may be damaged. Copy the disc again. " + needed);
        self = std::move(*loaded);
        if (self->sha256 != build->self_sha256)
            return with(3, "This is " + std::string(build->name) + ", but its EBOOT.BIN isn't the one on the disc (it may be from a game update, or "
                           "changed). Copy PS3_GAME from your disc again. " + needed);
    }
    // The decrypted program, which the player makes with RPCS3.
    if (files->elf.empty())
        return with(3, "This is " + describe(*build) + ", the right game, but the decrypted program, EBOOT.elf, isn't in the folder yet. This project has "
                       "no PS3 keys and can't decrypt the game, so make EBOOT.elf once with RPCS3: in RPCS3 choose Utilities > Decrypt PS3 Binaries "
                       "and pick EBOOT.BIN in your game folder's PS3_GAME/USRDIR. RPCS3 writes EBOOT.elf beside it (if it ends up anywhere else, "
                       "move it into PS3_GAME/USRDIR, beside EBOOT.BIN). Then choose the game folder again.");
    const auto elf = Elf::load(files->elf);
    const std::string again = " Make it again with RPCS3 (Utilities > Decrypt PS3 Binaries, then pick EBOOT.BIN in PS3_GAME/USRDIR), then choose the "
                              "game folder again.";
    if (!elf) return with(3, "This is " + describe(*build) + ", but its EBOOT.elf isn't a program that can be read: it may be damaged." + again);
    if (program_sha256(*elf) != build->program_sha256)
        return with(3, "This is " + describe(*build) + ", but its EBOOT.elf isn't the decrypted program of this disc's EBOOT.BIN." + again);
    if (self) {
        // (The segments' hash says it already; the headers too, as load_game_elf checks them for the game.)
        if (!differs_from(*self, elf->layout()).empty())
            return with(3, "This is " + describe(*build) + ", but its EBOOT.elf doesn't match its EBOOT.BIN." + again);
    }
    // The console's fonts.
    if (!options.flash || options.flash(*files).empty())
        return with(3, "This is " + describe(*build) + ", the right game, with its EBOOT.elf. " + how_to_get_fonts());
    return with(0, "This is " + describe(*build) + ": the right game folder, with its decrypted EBOOT.elf and the PS3's fonts.");
}

std::string to_json(const Identity& i) {
    return std::format("{{\"ok\": {}, \"title\": {}, \"id\": {}, \"version\": {}, \"message\": {}}}", i.ok ? "true" : "false", json_string(i.title),
                       json_string(i.id), json_string(i.version), json_string(i.message));
}

}  // namespace qilin

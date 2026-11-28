// `qilin identify` (recomp/src/identify.hpp) on game folders made in memory (elf_builder.hpp): the supported build, the
// decrypted program or the fonts missing, another game, another version, another program, and what can't be read. No
// game files are needed: the tests give a supported build of their own, hashed from the synthetic files.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

#include "elf.hpp"
#include "elf_builder.hpp"
#include "identify.hpp"
#include "sha256.hpp"
#include "title.hpp"

using namespace qilin;
using namespace elfb;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::println(stderr, "{}:{}: CHECK failed: {}", __FILE__, __LINE__, #cond); \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

bool has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

void write(const fs::path& path, const Bytes& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A game folder: PS3_GAME/PARAM.SFO, PS3_GAME/USRDIR/EBOOT.BIN and (if `decrypted`) EBOOT.elf, and (if `fonts`) the
// console's fonts in dev_flash beside PS3_GAME.
void make_game(const fs::path& root, const Bytes& elf, std::string_view title_id, std::string_view version, bool decrypted, bool fonts) {
    std::error_code ec;
    fs::remove_all(root, ec);
    write(root / "PS3_GAME" / "PARAM.SFO", make_sfo({{"TITLE", "RIDGE RACER 7"}, {"TITLE_ID", std::string(title_id)}, {"VERSION", std::string(version)}}));
    write(root / "PS3_GAME" / "USRDIR" / "EBOOT.BIN", make_self(elf));
    if (decrypted) write(root / "PS3_GAME" / "USRDIR" / "EBOOT.elf", elf);
    if (fonts)
        for (const char* name : {"SCE-PS3-NR-R-JPN.TTF", "SCE-PS3-NR-R-EXT.TTF"}) write(root / "dev_flash" / "data" / "font" / name, Bytes{1, 2, 3});
}

// Only the fonts beside the game count here (not an RPCS3 the computer running the tests may have).
fs::path flash_in_game(const GameFiles& files) {
    const fs::path flash = files.usrdir.parent_path().parent_path() / "dev_flash";
    return holds_game_fonts(flash) ? flash : fs::path();
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "qilin-rr7-identify-test";
    const fs::path game = root / "Ridge Racer 7 (USA)";
    const Bytes elf = make_elf();
    const auto program = Elf::parse(elf);
    CHECK(program.has_value());
    if (!program) return 1;
    const std::string self_hash = sha256_hex(make_self(elf)), program_hash = program_sha256(*program);
    const SupportedBuild builds[] = {{"Ridge Racer 7 (USA)", "BLUS30001", "01.02", self_hash, program_hash}};
    const IdentifyOptions options{.want = "Ridge Racer 7 (USA)", .builds = builds, .flash = flash_in_game};

    // The supported build, from the disc's folder, PS3_GAME or USRDIR.
    make_game(game, elf, "BLUS30001", "01.02", true, true);
    for (const fs::path& given : {game, game / "PS3_GAME", game / "PS3_GAME" / "USRDIR"}) {
        const Identity i = identify(given, options);
        CHECK(i.code == 0 && i.ok);
        CHECK(i.title == "Ridge Racer 7 (USA)" && i.id == "BLUS30001" && i.version == "01.02");
        CHECK(has(i.message, "the right game folder"));
        const std::string json = to_json(i);
        CHECK(has(json, "\"ok\": true") && has(json, "\"id\": \"BLUS30001\""));
    }
    // The program's hash is of its segments: what lies outside them (the zeros RPCS3 writes there) doesn't count.
    {
        Bytes changed = elf;
        changed[kDataAt + 0x40] ^= 0xff;  // (padding before the section names: outside every segment)
        const auto other = Elf::parse(changed);
        CHECK(other && program_sha256(*other) == program_hash);
    }
    // No decrypted program: how to make it with RPCS3.
    make_game(game, elf, "BLUS30001", "01.02", false, true);
    {
        const Identity i = identify(game, options);
        CHECK(i.code == 3 && !i.ok && i.id == "BLUS30001");
        CHECK(has(i.message, "EBOOT.elf") && has(i.message, "Decrypt PS3 Binaries") && has(i.message, "PS3_GAME/USRDIR"));
    }
    // No fonts: how to install the system software.
    make_game(game, elf, "BLUS30001", "01.02", true, false);
    {
        const Identity i = identify(game, options);
        CHECK(i.code == 3 && !i.ok);
        CHECK(has(i.message, "fonts") && has(i.message, "Install Firmware") && has(i.message, "dev_flash"));
    }
    // Another game, another version.
    make_game(game, elf, "BLES00001", "01.00", true, true);
    {
        const Identity i = identify(game, options);
        CHECK(i.code == 3 && !i.ok && i.id == "BLES00001" && i.version == "01.00");
        CHECK(has(i.message, "BLES00001") && has(i.message, "This download is for Ridge Racer 7 (USA), BLUS30001 version 01.02."));
    }
    make_game(game, elf, "BLUS30001", "01.00", true, true);
    CHECK(identify(game, options).code == 3);
    // Another program (a changed EBOOT.BIN; a decrypted program that isn't this EBOOT.BIN's).
    {
        Bytes changed = elf;
        changed[kTextAt] ^= 0xff;
        make_game(game, changed, "BLUS30001", "01.02", true, true);
        const Identity i = identify(game, options);
        CHECK(i.code == 3 && has(i.message, "EBOOT.BIN isn't the one on the disc"));
        make_game(game, elf, "BLUS30001", "01.02", true, true);
        write(game / "PS3_GAME" / "USRDIR" / "EBOOT.elf", changed);
        const Identity j = identify(game, options);
        CHECK(j.code == 3 && has(j.message, "isn't the decrypted program"));
        write(game / "PS3_GAME" / "USRDIR" / "EBOOT.elf", Bytes{1, 2});
        CHECK(identify(game, options).code == 3);
    }
    // What can't be read: nothing there, a file, an empty folder, a folder with no PARAM.SFO; a build it doesn't know.
    {
        CHECK(identify(root / "nothing", options).code == 2);
        write(root / "two bytes.bin", Bytes{9, 0});
        CHECK(identify(root / "two bytes.bin", options).code == 2);
        fs::create_directories(root / "empty");
        const Identity i = identify(root / "empty", options);
        CHECK(i.code == 2 && !i.ok && has(i.message, "PS3_GAME"));
        make_game(game, elf, "BLUS30001", "01.02", true, true);
        fs::remove(game / "PS3_GAME" / "PARAM.SFO");
        CHECK(identify(game, options).code == 2);
        IdentifyOptions unknown = options;
        unknown.want = "Ridge Racer 7 (Japan)";
        CHECK(identify(game, unknown).code == 2);
    }
    // The fonts are found beside the game by title.hpp's own search too.
    make_game(game, elf, "BLUS30001", "01.02", true, true);
    {
        const auto files = find_game(game);
        CHECK(files && fs::equivalent(find_flash(*files), game / "dev_flash"));
        CHECK(files && !flash_candidates(*files).empty());
    }
    // A JSON string keeps quotes, backslashes and control characters escaped.
    CHECK(to_json(Identity{2, false, "a \"b\"\\", "", "", "x\ny"}) ==
          "{\"ok\": false, \"title\": \"a \\\"b\\\"\\\\\", \"id\": \"\", \"version\": \"\", \"message\": \"x\\ny\"}");
    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures) {
        std::println(stderr, "{} checks failed", failures);
        return 1;
    }
    std::println("identify: all checks passed");
    return 0;
}

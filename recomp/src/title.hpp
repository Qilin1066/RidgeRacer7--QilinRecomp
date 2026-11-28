// The game, however the player holds their copy: the disc's folder (the one
// holding PS3_GAME), PS3_GAME or USRDIR inside it, EBOOT.BIN, or the
// decrypted program itself. Runtime code: no std::format.
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "elf.hpp"

namespace qilin {

struct GameFiles {
    std::filesystem::path usrdir;  // the game's own files (EBOOT.BIN, its data); empty if only a file was given
    std::filesystem::path sfo;     // PARAM.SFO, if found
    std::filesystem::path self;    // EBOOT.BIN, if found
    std::filesystem::path elf;     // the decrypted program, if found
};

// Finds the files; fails only if `game` isn't there or holds none of them.
std::expected<GameFiles, std::string> find_game(const std::filesystem::path& game);

// The decrypted program. With only EBOOT.BIN there, the error says how to
// make it. With both, the ELF must be that EBOOT.BIN's program.
std::expected<Elf, std::string> load_game_elf(const GameFiles& files);

// The builds this port knows, by SHA-256. `self`: of EBOOT.BIN as it is on
// the disc; otherwise of the decrypted program.
struct KnownBuild {
    std::string_view sha256, name;
    bool self;
};
const KnownBuild* known_build(std::string_view sha256);

// What to tell a player who has only the encrypted program.
std::string how_to_decrypt(const std::filesystem::path& self);

// The console's system software (a dev_flash folder, as RPCS3 makes it when it installs Sony's PS3UPDAT.PUP), for the
// console's own fonts. Where it is looked for, in order: in the disc's folder (beside PS3_GAME); in an RPCS3 folder
// beside the disc's folder, or beside it directly; in RPCS3's own folder where the system has a standard one
// (RPCS3_CONFIG_DIR if set; Linux: $XDG_CONFIG_HOME/rpcs3 or ~/.config/rpcs3, and the Flatpak's; macOS:
// ~/Library/Application Support/rpcs3); and in this port's data folder (qilin/ridgeracer7, where qilin::host::user_folder
// finds it, RIDGERACER7_SAVE_DIR too). Windows' RPCS3 keeps it beside rpcs3.exe, which can be anywhere: that is what
// the places beside the game and the data folder are for.
std::vector<std::filesystem::path> flash_candidates(const GameFiles& files);
// Whether `flash` holds the fonts the game opens (font.cpp's font set 0x101).
bool holds_game_fonts(const std::filesystem::path& flash);
// The first candidate that holds them; empty if none does.
std::filesystem::path find_flash(const GameFiles& files);
// What to tell a player whose fonts weren't found: how to install the system software with RPCS3, and where it may go.
std::string how_to_get_fonts();

}  // namespace qilin

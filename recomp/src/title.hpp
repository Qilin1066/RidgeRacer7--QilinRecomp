// The game, however the player holds their copy: the disc's folder (the one
// holding PS3_GAME), PS3_GAME or USRDIR inside it, EBOOT.BIN, or the
// decrypted program itself. Runtime code: no std::format.
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

}  // namespace qilin

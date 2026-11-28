// `qilin identify <game folder> --json [--build <name>]`: what the player's game folder is, for the players' launcher
// (tools/player-kit/launcher), as the contract every QilinRecomp project's recompiler keeps (ADOPTING.md there). One JSON
// object on standard output:
//
//   {"ok": true|false, "title": "...", "id": "...", "version": "...", "message": "..."}
//
// and the exit code: 0 when it is the supported build and everything the game needs from the player is there, 3 when it
// can be read but isn't that (another game, region or version; or the right game with the decrypted program or the
// console's fonts missing: the message says what to do), 2 when it can't be read at all. The message is a plain
// sentence for the player.
//
// The input is the disc's folder (the one holding PS3_GAME; PS3_GAME or USRDIR inside it do too, as title.hpp's
// find_game takes them). What is checked: PARAM.SFO's title id and version; EBOOT.BIN's SHA-256 (the disc's, as it is);
// the decrypted EBOOT.elf the player makes with RPCS3 (this project has no PS3 keys), by the SHA-256 of its loaded
// segments (what any correct decryption holds, whatever it leaves outside them) and its program headers against
// EBOOT.BIN's; and the console's fonts (title.hpp's find_flash). Nothing is written.
#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "title.hpp"

namespace qilin {

struct Identity {
    int code = 2;  // 0, 3 or 2, as above
    bool ok = false;
    std::string title, id, version, message;
};

// A build this port supports: its name (game.txt's `build`), PARAM.SFO's title id and version, the SHA-256 of
// EBOOT.BIN and of the decrypted program's loaded segments (program_sha256).
struct SupportedBuild {
    std::string_view name, title_id, version, self_sha256, program_sha256;
};
std::span<const SupportedBuild> supported_builds();

// The SHA-256 of every loaded segment's bytes in the file, in program-header order.
std::string program_sha256(const Elf& elf);

struct IdentifyOptions {
    std::string_view want;                                                   // a supported build's name, or empty for any
    std::span<const SupportedBuild> builds = supported_builds();             // (the tests give their own)
    std::function<std::filesystem::path(const GameFiles&)> flash = find_flash;  // where the fonts are; empty: not found
};

Identity identify(const std::filesystem::path& input, const IdentifyOptions& options = {});

std::string to_json(const Identity& identity);

}  // namespace qilin

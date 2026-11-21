#include "title.hpp"

#include <algorithm>

#include "self.hpp"

namespace qilin {

namespace {

namespace fs = std::filesystem;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }

bool same_nocase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

// A file in `folder` by name, whatever its letters' case (a disc's names are
// upper case; tools that write beside them don't always keep to that).
fs::path file_in(const fs::path& folder, std::string_view name) {
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && same_nocase(it->path().filename().string(), name)) return it->path();
    return {};
}

fs::path folder_in(const fs::path& folder, std::string_view name) {
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && same_nocase(it->path().filename().string(), name)) return it->path();
    return {};
}

// The one build so far: the owner's disc, as it is (docs/elf-analysis.md).
// The decrypted program's hash joins it when the first analysis records it.
constexpr KnownBuild kBuilds[] = {
    {"095ed6cb8e90538309b57662fb19da1ba2c4bdbac5fd40dc0be6122e76d4c0db",
     "Ridge Racer 7 (USA), BLUS30001, version 01.02: EBOOT.BIN as on the disc", true},
};

}  // namespace

const KnownBuild* known_build(std::string_view sha256) {
    for (const KnownBuild& b : kBuilds)
        if (b.sha256 == sha256) return &b;
    return nullptr;
}

std::string how_to_decrypt(const fs::path& self) {
    return self.string() +
           " is the game's program as the disc holds it: encrypted. This project has no PS3 keys and can't decrypt it.\n"
           "Make the decrypted program from your own disc once: in RPCS3, Utilities > Decrypt PS3 Binaries, choose this\n"
           "EBOOT.BIN, and it writes EBOOT.elf. Put EBOOT.elf beside EBOOT.BIN (in USRDIR) and run this again.";
}

std::expected<GameFiles, std::string> find_game(const fs::path& game) {
    std::error_code ec;
    GameFiles files;
    if (fs::is_directory(game, ec)) {
        // The disc's folder, PS3_GAME, or USRDIR.
        fs::path ps3_game = folder_in(game, "PS3_GAME");
        fs::path usrdir = folder_in(ps3_game.empty() ? game : ps3_game, "USRDIR");
        if (ps3_game.empty() && !usrdir.empty()) ps3_game = game;
        if (usrdir.empty()) {
            usrdir = game;
            ps3_game = game.parent_path();
        }
        files.usrdir = usrdir;
        files.self = file_in(usrdir, "EBOOT.BIN");
        files.elf = file_in(usrdir, "EBOOT.elf");
        files.sfo = file_in(ps3_game, "PARAM.SFO");
        if (files.self.empty() && files.elf.empty())
            return std::unexpected("no EBOOT.BIN or EBOOT.elf in " + usrdir.string() +
                                   " (give the disc's folder, the one holding PS3_GAME)");
        return files;
    }
    if (!fs::is_regular_file(game, ec)) return std::unexpected("cannot open " + game.string());
    const fs::path folder = game.parent_path();
    if (same_nocase(game.filename().string(), "PARAM.SFO")) {
        files.sfo = game;
        return files;
    }
    auto head = read_file(game);
    if (!head) return std::unexpected(head.error());
    if (Self::is_self(*head)) {
        files.self = game;
        files.elf = file_in(folder, "EBOOT.elf");
    } else {
        files.elf = game;
        files.self = file_in(folder, "EBOOT.BIN");
    }
    files.usrdir = folder;
    files.sfo = file_in(folder.parent_path(), "PARAM.SFO");
    return files;
}

std::expected<Elf, std::string> load_game_elf(const GameFiles& files) {
    if (files.elf.empty()) {
        if (files.self.empty()) return std::unexpected(std::string("no program to read"));
        return std::unexpected(how_to_decrypt(files.self));
    }
    auto elf = Elf::load(files.elf);
    if (!elf) return elf;
    if (!files.self.empty()) {
        auto self = Self::load(files.self);
        if (!self) return std::unexpected(self.error());
        if (const std::string what = differs_from(*self, elf->layout()); !what.empty())
            return std::unexpected(files.elf.string() + " is not the program in " + files.self.string() + ": they differ in " + what);
    }
    return elf;
}

}  // namespace qilin

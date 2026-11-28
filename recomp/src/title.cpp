#include "title.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

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
    // As RPCS3 0.0.43 writes it: the sections outside every segment (the names, .comment) are zero-filled.
    {"689f9704ee9dbd982c48de646f07d69c193a46513d75b6f95f20a206c7754831",
     "Ridge Racer 7 (USA), BLUS30001, version 01.02: the program decrypted by RPCS3", false},
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

namespace {

// An environment variable as a path (on Windows read as UTF-16, so a name with any letters survives); empty if unset.
fs::path env_path(const char* name) {
#if defined(_WIN32)
    const std::wstring wide(name, name + std::strlen(name));
    const wchar_t* value = _wgetenv(wide.c_str());
#else
    const char* value = std::getenv(name);
#endif
    return value && *value ? fs::path(value) : fs::path();
}

// The disc's folder (the one holding PS3_GAME), from the game's own files' folder.
fs::path disc_folder(const GameFiles& files) {
    if (files.usrdir.empty()) return {};
    std::error_code ec;
    fs::path usrdir = fs::weakly_canonical(files.usrdir, ec);
    if (ec) usrdir = files.usrdir;
    const fs::path ps3_game = usrdir.parent_path();
    return same_nocase(ps3_game.filename().string(), "PS3_GAME") ? ps3_game.parent_path() : usrdir;
}

// This port's data folder, as qilin::host::user_folder (host/src/paths.cpp) finds it, without making it.
fs::path data_folder() {
    if (fs::path p = env_path("RIDGERACER7_SAVE_DIR"); !p.empty()) return p;
#if defined(_WIN32)
    const fs::path base = env_path("LOCALAPPDATA");
#elif defined(__APPLE__)
    const fs::path home = env_path("HOME"), base = home.empty() ? fs::path() : home / "Library" / "Application Support";
#else
    fs::path base = env_path("XDG_DATA_HOME");
    if (const fs::path home = env_path("HOME"); base.empty() && !home.empty()) base = home / ".local" / "share";
#endif
    return base.empty() ? fs::path() : base / "qilin" / "ridgeracer7";
}

}  // namespace

std::vector<fs::path> flash_candidates(const GameFiles& files) {
    std::vector<fs::path> out;
    if (const fs::path disc = disc_folder(files); !disc.empty()) {
        out.push_back(disc / "dev_flash");
        const fs::path beside = disc.parent_path();
        out.push_back(beside / "RPCS3" / "dev_flash");
        out.push_back(beside / "rpcs3" / "dev_flash");  // (the same folder where names ignore case)
        out.push_back(beside / "dev_flash");
    }
    if (const fs::path config = env_path("RPCS3_CONFIG_DIR"); !config.empty()) out.push_back(config / "dev_flash");
#if defined(__APPLE__)
    if (const fs::path home = env_path("HOME"); !home.empty()) out.push_back(home / "Library" / "Application Support" / "rpcs3" / "dev_flash");
#elif !defined(_WIN32)
    const fs::path home = env_path("HOME");
    if (const fs::path config = env_path("XDG_CONFIG_HOME"); !config.empty()) out.push_back(config / "rpcs3" / "dev_flash");
    if (!home.empty()) {
        out.push_back(home / ".config" / "rpcs3" / "dev_flash");
        out.push_back(home / ".var" / "app" / "net.rpcs3.RPCS3" / "config" / "rpcs3" / "dev_flash");  // the Flatpak's
    }
#endif
    if (const fs::path data = data_folder(); !data.empty()) out.push_back(data / "dev_flash");
    return out;
}

bool holds_game_fonts(const fs::path& flash) {
    // font.cpp's kFontSets: the files of the one set the game opens.
    std::error_code ec;
    for (const char* name : {"SCE-PS3-NR-R-JPN.TTF", "SCE-PS3-NR-R-EXT.TTF"})
        if (!fs::is_regular_file(flash / "data" / "font" / name, ec)) return false;
    return true;
}

fs::path find_flash(const GameFiles& files) {
    for (const fs::path& p : flash_candidates(files))
        if (holds_game_fonts(p)) return p;
    return {};
}

std::string how_to_get_fonts() {
    std::string text =
        "The game needs the PS3's own fonts, which are in the PS3's system software, and they weren't found. Install the system software in "
        "RPCS3 once: download the PS3 system software update (PS3UPDAT.PUP) from PlayStation's website, then in RPCS3 choose File > "
        "Install Firmware and pick that file. RPCS3 puts it in a folder named dev_flash";
#if defined(_WIN32)
    text += " beside rpcs3.exe. Copy that dev_flash folder into your game folder, beside PS3_GAME, then choose the game folder again.";
#elif defined(__APPLE__)
    text += " in your home folder's Library/Application Support/rpcs3, where it is found by itself: choose the game folder again once it is "
            "installed (or copy dev_flash into your game folder, beside PS3_GAME).";
#else
    text += " in ~/.config/rpcs3 (the Flatpak's in ~/.var/app/net.rpcs3.RPCS3/config/rpcs3), where it is found by itself: choose the game "
            "folder again once it is installed (or copy dev_flash into your game folder, beside PS3_GAME).";
#endif
    return text;
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

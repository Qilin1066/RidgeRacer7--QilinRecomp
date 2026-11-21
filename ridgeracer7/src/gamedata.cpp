// cellSysutil's game data utility (cellGameData...): a folder of the
// game's own on the console's hard disk, where a game installs what it wants
// to load faster than from the disc.
//
// The hard disk here is a folder of the port's own (`dev_hdd0` in its data
// folder, laid out as a console's, so `/dev_hdd0/game/<name>` is
// `dev_hdd0/game/<name>` there). The folder's description is a PARAM.SFO,
// as on a console.
//
// The utility works through a function of the game's: it tells the game
// what is there (CellGameDataStatGet), and the game answers whether to go
// on and with what description (CellGameDataStatSet). The structures'
// layout is the SDK's, and agrees with the game's own use of them: it
// copies 5,192 bytes of the first, and reads and writes the description at
// byte 2,144 (title), 2,272 (twenty titles by language), 4,832 (title id),
// 4,844 (version), 4,852 (parental level) and 4,856 (attribute), and the
// system's own size at 5,120.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "bytes.hpp"
#include "kernel.hpp"
#include "qilin/rt/report.hpp"
#include "sfo.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;

// The utility's results and errors (the SDK's CELL_GAMEDATA_...).
constexpr uint64_t kRetOk = 0, kRetCancel = 1;
constexpr uint64_t kErrorCallback = 0x8002cb01u, kErrorAccess = 0x8002cb02u, kErrorParam = 0x8002cb04u, kErrorBroken = 0x8002cb06u;
constexpr int32_t kCallbackOkCancel = 1, kCallbackOk = 0;

constexpr uint32_t kGetSize = 5192, kPath = 1055;
constexpr uint32_t kGetIsNew = 4, kGetContentPath = 8, kGetDataPath = kGetContentPath + kPath, kGetTimes = kGetDataPath + kPath + 2, kGetParam = kGetTimes + 24;
constexpr uint32_t kParamTitle = 0, kParamTitleLang = 128, kParamTitleId = 2688, kParamVersion = 2700, kParamParental = 2708, kParamAttribute = 2712, kParamSize = 2972;
constexpr uint32_t kGetSizeKb = kGetParam + kParamSize, kGetSystemSizeKb = kGetSizeKb + 4;
static_assert(kGetParam == 2144 && kGetSystemSizeKb == 5120 && kGetSystemSizeKb + 4 + 68 == kGetSize);

std::mutex g_mutex;  // one at a time, as the utility is
uint32_t g_block = 0;  // the three structures, in guest memory

std::string guest_string(uint32_t address, size_t most) {
    const char* const p = reinterpret_cast<const char*>(guest_memory() + address);
    return std::string(p, strnlen(p, most));
}
void put_string(uint32_t address, const std::string& text, size_t room) {
    const size_t n = std::min(text.size(), room - 1);
    std::memcpy(guest_memory() + address, text.data(), n);
    guest_memory()[address + n] = 0;
}

// Kilobytes of the files under a folder, each rounded up.
uint64_t folder_kb(const std::filesystem::path& folder) {
    uint64_t kb = 0;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        if (it->is_regular_file(error)) kb += (it->file_size(error) + 1023) / 1024;
    return kb;
}

// The description, from the game's structure to a PARAM.SFO and back.
qilin::Sfo sfo_from(uint32_t param) {
    qilin::Sfo sfo;
    const auto text = [&](std::string key, uint32_t at, size_t room) {
        const std::string value = guest_string(param + at, room);
        if (!value.empty() || key == "TITLE") sfo.entries.push_back({std::move(key), false, 0, value});
    };
    sfo.entries.push_back({"ATTRIBUTE", true, load32(param + kParamAttribute), {}});
    sfo.entries.push_back({"CATEGORY", false, 0, "GD"});
    sfo.entries.push_back({"PARENTAL_LEVEL", true, load32(param + kParamParental), {}});
    text("TITLE", kParamTitle, 128);
    for (uint32_t i = 0; i < 20; ++i) {
        char key[16];
        std::snprintf(key, sizeof key, "TITLE_%02u", i);
        text(key, kParamTitleLang + 128 * i, 128);
    }
    text("TITLE_ID", kParamTitleId, 10);
    text("VERSION", kParamVersion, 6);
    return sfo;
}
void sfo_to(uint32_t param, const qilin::Sfo& sfo) {
    put_string(param + kParamTitle, sfo.text("TITLE").value_or(""), 128);
    for (uint32_t i = 0; i < 20; ++i) {
        char key[16];
        std::snprintf(key, sizeof key, "TITLE_%02u", i);
        put_string(param + kParamTitleLang + 128 * i, sfo.text(key).value_or(""), 128);
    }
    put_string(param + kParamTitleId, sfo.text("TITLE_ID").value_or(""), 10);
    put_string(param + kParamVersion, sfo.text("VERSION").value_or(""), 6);
    store32(param + kParamParental, sfo.number("PARENTAL_LEVEL").value_or(0));
    store32(param + kParamAttribute, sfo.number("ATTRIBUTE").value_or(0));
}

// cellGameDataCheckCreate2(version, const char* folder name, error dialogs, the game's function, memory container)
void cellGameDataCheckCreate2(Cpu& c) {
    const uint32_t version = static_cast<uint32_t>(c.r[3]), name_at = static_cast<uint32_t>(c.r[4]), dialogs = static_cast<uint32_t>(c.r[5]),
                   function = static_cast<uint32_t>(c.r[6]);
    if (!name_at || !function) {
        c.r[3] = kErrorParam;
        return;
    }
    const std::string name = guest_string(name_at, 32);
    // A folder's name is capitals, digits, '_' and '-' on a console; anything else here would also be a path of the host's.
    if (name.empty() || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != name.npos) {
        c.r[3] = kErrorParam;
        return;
    }
    if (game().hdd.empty()) fatal("the game asks for its folder on the hard disk (%s), and this run has no hard disk folder", name.c_str());
    const std::lock_guard lock(g_mutex);
    const std::filesystem::path folder = game().hdd / "game" / name, description = folder / "PARAM.SFO";
    std::error_code error;
    const bool exists = std::filesystem::is_regular_file(description, error);
    if (!exists && std::filesystem::exists(folder, error)) {
        // A folder with no description: broken data, on a console. The game's function isn't called.
        qilin::rt::log("the game's folder %s is there without its PARAM.SFO: reported as broken", folder.string().c_str());
        c.r[3] = kErrorBroken;
        return;
    }
    if (!g_block) g_block = allocate(16 + kGetSize + 8, 16);
    const uint32_t result = g_block, get = g_block + 16, set = get + kGetSize;
    std::memset(guest_memory() + g_block, 0, 16 + kGetSize + 8);

    const std::filesystem::space_info space = std::filesystem::space(game().hdd, error);
    const uint64_t free_kb = error ? 0 : space.available / 1024;
    store32(get, static_cast<uint32_t>(std::min<uint64_t>(free_kb, 0x7fffffffu)));
    store32(get + kGetIsNew, exists ? 0 : 1);
    put_string(get + kGetContentPath, "/dev_hdd0/game/" + name, kPath);
    put_string(get + kGetDataPath, "/dev_hdd0/game/" + name + "/USRDIR", kPath);
    uint64_t system_kb = 0;
    if (exists) {
        const auto sfo = qilin::Sfo::load(description);
        if (!sfo) {
            qilin::rt::log("%s: reported as broken", sfo.error().c_str());
            c.r[3] = kErrorBroken;
            return;
        }
        sfo_to(get + kGetParam, *sfo);
        const auto modified = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::last_write_time(description, error).time_since_epoch()).count();
        (void)modified;  // (the host's file clock is not the calendar's on every host: the times are left at 0 until the game is seen to read them)
        system_kb = (std::filesystem::file_size(description, error) + 1023) / 1024;
        store32(get + kGetSizeKb, static_cast<uint32_t>(folder_kb(folder)));
    }
    // What the system's own files take: here, the description alone (a kilobyte, when it is yet to be written).
    store32(get + kGetSystemSizeKb, static_cast<uint32_t>(exists ? system_kb : 1));

    // The game's function: (CellGameDataCBResult* result, CellGameDataStatGet* get, CellGameDataStatSet* set).
    {
        const uint64_t toc = c.r[2], lr = c.lr;
        c.r[3] = result;
        c.r[4] = get;
        c.r[5] = set;
        c.r[2] = load32(function + 4);
        c.hooks->call(c, load32(function));
        c.r[2] = toc;
        c.lr = lr;
    }
    const int32_t answer = static_cast<int32_t>(load32(result));
    qilin::rt::log("game data %s (version %u, dialogs %u): %s; the game answers %d", name.c_str(), version, dialogs, exists ? "there" : "not there", answer);
    if (answer == kCallbackOkCancel) {
        c.r[3] = kRetCancel;
        return;
    }
    if (answer != kCallbackOk) {
        // (With dialogs on, a console tells the player why first: there is no such dialog here yet, and the log says it.)
        qilin::rt::log("the game refuses its game data: result %d, %d KiB more needed", answer, static_cast<int32_t>(load32(result + 4)));
        c.r[3] = kErrorCallback;
        return;
    }
    const uint32_t param = load32(set);
    if (!param) {
        if (!exists) {
            c.r[3] = kErrorParam;  // nothing to describe new data with
            return;
        }
        c.r[3] = kRetOk;
        return;
    }
    std::filesystem::create_directories(folder / "USRDIR", error);
    if (error) fatal("can't create %s: %s", (folder / "USRDIR").string().c_str(), error.message().c_str());
    const std::vector<uint8_t> bytes = sfo_from(param).bytes();
    std::FILE* file = std::fopen(description.string().c_str(), "wb");
    if (!file || std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size() || std::fclose(file) != 0) {
        qilin::rt::log("can't write %s", description.string().c_str());
        c.r[3] = kErrorAccess;
        return;
    }
    c.r[3] = kRetOk;
}

constexpr NativeImport kImports[] = {
    {"cellSysutil", "cellGameDataCheckCreate2", cellGameDataCheckCreate2},
};

}  // namespace

std::span<const NativeImport> gamedata_imports() { return kImports; }

}  // namespace rr7

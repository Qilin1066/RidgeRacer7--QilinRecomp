// cellSysutil's save data utility (cellSaveData...): the game's saves, in
// folders on the console's hard disk (`/dev_hdd0/home/<user>/savedata/<name>`,
// here in the port's own hard disk folder: fs.cpp's /dev_hdd0), each with a
// PARAM.SFO describing it, as on a console.
//
// The utility works through two functions of the game's: the first is told
// what is there and answers whether to go on (and, saving, with what
// description); the second is called again and again, each time asking for
// one file to be read or written, until it says it is done. The structures'
// layout is the SDK's, and agrees with the game's own functions: it copies
// or fills the description (1,552 bytes) at offset 64 of what it is told,
// reads the list of files (56 bytes each) whose count and address are at
// 1,628 to 1,636, and asks for a file with its operation at 0, its name at
// 28, its size at 36 and its buffer at 44.
//
// *A "secure" file (type 0), encrypted on a console, is kept plain here:
// this port has no keys and wants none.*
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"
#include "sfo.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;
namespace stdfs = std::filesystem;

// The utility's errors (the SDK's CELL_SAVEDATA_ERROR_...: *as recalled, to be confirmed*).
constexpr uint64_t kErrorCallbackResult = 0x8002b401u, kErrorAccess = 0x8002b402u, kErrorParam = 0x8002b404u;
constexpr uint32_t kUser = 1;  // the console's user: the first
// What the game is told about a save (CellSaveDataStatGet), and what it asks (CellSaveDataStatSet, CellSaveDataFileGet and Set).
constexpr uint32_t kGetFree = 0, kGetIsNew = 4, kGetDirName = 32, kGetParam = 64, kGetBind = 1616, kGetSizeKb = 1620, kGetSysSizeKb = 1624,
                   kGetFileNum = 1628, kGetFileListNum = 1632, kGetFileList = 1636;
constexpr uint32_t kParamTitle = 0, kParamSubTitle = 128, kParamDetail = 256, kParamAttribute = 1280, kParamParental = 1284, kParamList = 1288;
constexpr uint32_t kFileEntry = 56;
constexpr uint32_t kFileOperation = 0, kFileType = 8, kFileName = 28, kFileOffset = 32, kFileSize = 36, kFileBufferSize = 40, kFileBuffer = 44;
constexpr int32_t kResultNext = 0, kResultLast = 1, kResultLastNoConfirm = 2;

std::mutex g_mutex;  // one at a time, as the utility is
uint32_t g_block = 0;

std::string guest_string(uint32_t address, size_t most) {
    if (!address) return {};
    const char* const p = reinterpret_cast<const char*>(guest_memory() + address);
    return std::string(p, strnlen(p, most));
}
void put_string(uint32_t address, const std::string& text, size_t room) {
    const size_t n = std::min(text.size(), room - 1);
    std::memcpy(guest_memory() + address, text.data(), n);
    guest_memory()[address + n] = 0;
}

// The game's function: (CellSaveDataCBResult* result, get, set). Returns the result it gives.
int32_t call_game(Cpu& c, uint32_t function, uint32_t result, uint32_t get, uint32_t set) {
    const uint64_t toc = c.r[2], lr = c.lr;
    c.r[3] = result, c.r[4] = get, c.r[5] = set;
    c.r[2] = load32(function + 4);
    c.hooks->call(c, load32(function));
    c.r[2] = toc;
    c.lr = lr;
    return static_cast<int32_t>(load32(result));
}

bool good_name(const std::string& name) {
    return !name.empty() && name.size() < 32 && name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == name.npos;
}
bool good_file_name(const std::string& name) {
    return !name.empty() && name.size() <= 12 && name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") == name.npos;
}

uint32_t kilobytes(uintmax_t bytes) { return static_cast<uint32_t>((bytes + 1023) / 1024); }

// One automatic load or save: `saving` says which.
void auto_load_or_save(Cpu& c, bool saving) {
    const uint32_t name_at = static_cast<uint32_t>(c.r[4]), buffer = static_cast<uint32_t>(c.r[6]), status = static_cast<uint32_t>(c.r[7]),
                   file_function = static_cast<uint32_t>(c.r[8]);
    if (!name_at || !buffer || !status || !file_function) {
        c.r[3] = kErrorParam;
        return;
    }
    const std::string name = guest_string(name_at, 32);
    if (!good_name(name)) {
        c.r[3] = kErrorParam;
        return;
    }
    if (game().hdd.empty()) fatal("the game uses its save (%s), and this run has no hard disk folder", name.c_str());
    char user[16];
    std::snprintf(user, sizeof user, "%08u", kUser);
    const stdfs::path folder = game().hdd / "home" / user / "savedata" / name;
    std::error_code error;
    const bool exists = stdfs::is_regular_file(folder / "PARAM.SFO", error);

    const std::lock_guard lock(g_mutex);
    if (!g_block) g_block = allocate(32 + 2048 + 32 + 128 + 64, 16);
    const uint32_t result = g_block, get = g_block + 32, set = get + 2048, file_get = set + 32, file_set = file_get + 128;
    std::memset(guest_memory() + g_block, 0, 32 + 2048 + 32 + 128 + 64);

    // What is there: the space free, whether the save is new, its name, its description, its files.
    const stdfs::space_info space = stdfs::space(game().hdd, error);
    store32(get + kGetFree, static_cast<uint32_t>(std::min<uintmax_t>(error ? 0 : space.available / 1024, 0x7fffffffu)));
    store32(get + kGetIsNew, exists ? 0 : 1);
    std::memcpy(guest_memory() + get + kGetDirName, name.data(), name.size());
    const uint32_t list = load32(buffer + 36), list_room = load32(buffer + 4);  // CellSaveDataSetBuf: fileListMax at 4, buf at 36
    uint32_t files = 0, listed = 0, size_kb = 0, system_kb = 0;
    if (exists) {
        const auto sfo = qilin::Sfo::load(folder / "PARAM.SFO");
        if (!sfo) fatal("%s", sfo.error().c_str());
        const uint32_t param = get + kGetParam;
        put_string(param + kParamTitle, sfo->text("TITLE").value_or(""), 128);
        put_string(param + kParamSubTitle, sfo->text("SUB_TITLE").value_or(""), 128);
        put_string(param + kParamDetail, sfo->text("DETAIL").value_or(""), 1024);
        store32(param + kParamAttribute, sfo->number("ATTRIBUTE").value_or(0));
        store32(param + kParamParental, sfo->number("PARENTAL_LEVEL").value_or(0));
        put_string(param + kParamList, sfo->text("SAVEDATA_LIST_PARAM").value_or(""), 8);
        system_kb = kilobytes(stdfs::file_size(folder / "PARAM.SFO", error));
        std::vector<stdfs::directory_entry> entries;
        for (stdfs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
            if (it->is_regular_file(error) && it->path().filename() != "PARAM.SFO") entries.push_back(*it);
        std::ranges::sort(entries, {}, [](const stdfs::directory_entry& e) { return e.path().filename().string(); });
        for (const stdfs::directory_entry& e : entries) {
            const uintmax_t bytes = e.file_size(error);
            size_kb += kilobytes(bytes);
            ++files;
            if (listed < list_room && list) {
                const uint32_t entry = list + listed * kFileEntry;
                std::memset(guest_memory() + entry, 0, kFileEntry);
                store32(entry, 0);  // a secure file
                store64(entry + 8, bytes);
                put_string(entry + 40, e.path().filename().string(), 13);
                ++listed;
            }
        }
    }
    store32(get + kGetBind, 0);
    store32(get + kGetSizeKb, size_kb + system_kb), store32(get + kGetSysSizeKb, system_kb);
    store32(get + kGetFileNum, files), store32(get + kGetFileListNum, listed), store32(get + kGetFileList, list);

    const int32_t answer = call_game(c, status, result, get, set);
    qilin::rt::log("save data %s: %s; %s, the game answers %d", name.c_str(), exists ? "there" : "not there", saving ? "saving" : "loading", answer);
    if (answer < 0) {
        c.r[3] = kErrorCallbackResult;  // the game knows why from its own answer
        return;
    }
    if (!saving && !exists) fatal("the game goes on to load save data %s, which isn't there: the utility's answer to that isn't known here", name.c_str());
    const uint32_t description = load32(set);  // CellSaveDataStatSet: setParam at 0, reCreateMode at 4, indicator at 8
    const uint32_t recreate = load32(set + 4);
    if (saving) {
        if (recreate & 0xff) fatal("the game saves %s with recreate mode %u, which isn't provided yet", name.c_str(), recreate);
        stdfs::create_directories(folder, error);
        if (error) {
            c.r[3] = kErrorAccess;
            return;
        }
    } else if (answer == kResultLast || answer == kResultLastNoConfirm) {
        c.r[3] = kOk;  // nothing to read
        return;
    }

    // The files, one call at a time, until the game says it is done.
    for (unsigned calls = 0; answer >= 0; ++calls) {
        if (calls > 1000) fatal("save data %s: the game's file function never says it is done", name.c_str());
        std::memset(guest_memory() + file_set, 0, 64);
        const int32_t r = call_game(c, file_function, result, file_get, file_set);
        if (r < 0) {
            c.r[3] = kErrorCallbackResult;
            return;
        }
        if (r == kResultLast || r == kResultLastNoConfirm) break;
        if (r != kResultNext) fatal("save data %s: the file function answers %d", name.c_str(), r);
        const uint32_t operation = load32(file_set + kFileOperation), type = load32(file_set + kFileType);
        // Types 2 to 5 are the save's pictures, film and sound, whose names are fixed (*the SDK's: as recalled*); 0 and 1 are named by the game.
        static constexpr const char* kContentNames[] = {"ICON0.PNG", "ICON1.PAM", "PIC1.PNG", "SND0.AT3"};
        const std::string file = type >= 2 && type <= 5 ? std::string(kContentNames[type - 2]) : guest_string(load32(file_set + kFileName), 13);
        const uint32_t offset = load32(file_set + kFileOffset), size = load32(file_set + kFileSize), room = load32(file_set + kFileBufferSize),
                       data = load32(file_set + kFileBuffer);
        if (type > 5) fatal("save data %s: a file of type %u", name.c_str(), type);
        if (!good_file_name(file)) fatal("save data %s: a file named \"%s\"", name.c_str(), file.c_str());
        const stdfs::path path = folder / file;
        uint32_t done = 0;
        switch (operation) {
        case 0: {  // read
            if (size > room) fatal("save data %s: reading %u bytes of %s into %u", name.c_str(), size, file.c_str(), room);
            std::FILE* f = std::fopen(path.string().c_str(), "rb");
            if (!f) fatal("save data %s: %s isn't there to read", name.c_str(), file.c_str());
            std::fseek(f, static_cast<long>(offset), SEEK_SET);
            done = static_cast<uint32_t>(std::fread(guest_memory() + data, 1, size, f));
            std::fclose(f);
            break;
        }
        case 1: case 3: {  // write, emptying the file first (1) or not (3)
            if (size > room) fatal("save data %s: writing %u bytes of %s from %u", name.c_str(), size, file.c_str(), room);
            std::FILE* f = std::fopen(path.string().c_str(), operation == 1 || !stdfs::exists(path, error) ? "wb" : "r+b");
            if (!f) {
                c.r[3] = kErrorAccess;
                return;
            }
            std::fseek(f, static_cast<long>(offset), SEEK_SET);
            done = static_cast<uint32_t>(std::fwrite(guest_memory() + data, 1, size, f));
            if (std::fclose(f) != 0 || done != size) {
                c.r[3] = kErrorAccess;
                return;
            }
            break;
        }
        case 2:  // delete
            stdfs::remove(path, error);
            break;
        default: fatal("save data %s: file operation %u", name.c_str(), operation);
        }
        store32(file_get, done);  // CellSaveDataFileGet: excSize, what the operation did
        qilin::rt::log("save data %s: %s %s, %u bytes", name.c_str(), operation == 0 ? "read" : operation == 2 ? "deleted" : "wrote", file.c_str(), done);
    }
    // Saving: the description, as the console keeps it beside the files.
    if (saving && description) {
        qilin::Sfo sfo;
        sfo.entries.push_back({"ACCOUNT_ID", false, 0, ""});
        sfo.entries.push_back({"ATTRIBUTE", true, load32(description + kParamAttribute), {}});
        sfo.entries.push_back({"CATEGORY", false, 0, "SD"});
        sfo.entries.push_back({"DETAIL", false, 0, guest_string(description + kParamDetail, 1024)});
        sfo.entries.push_back({"PARAMS", false, 0, ""});
        sfo.entries.push_back({"PARAMS2", false, 0, ""});
        sfo.entries.push_back({"PARENTAL_LEVEL", true, load32(description + kParamParental), {}});
        sfo.entries.push_back({"SAVEDATA_DIRECTORY", false, 0, name});
        sfo.entries.push_back({"SAVEDATA_LIST_PARAM", false, 0, guest_string(description + kParamList, 8)});
        sfo.entries.push_back({"SUB_TITLE", false, 0, guest_string(description + kParamSubTitle, 128)});
        sfo.entries.push_back({"TITLE", false, 0, guest_string(description + kParamTitle, 128)});
        const std::vector<uint8_t> bytes = sfo.bytes();
        std::FILE* f = std::fopen((folder / "PARAM.SFO").string().c_str(), "wb");
        if (!f || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size() || std::fclose(f) != 0) {
            c.r[3] = kErrorAccess;
            return;
        }
        qilin::rt::log("save data %s: described as \"%s\"", name.c_str(), guest_string(description + kParamTitle, 128).c_str());
    }
    c.r[3] = kOk;
}

// cellSaveDataAutoLoad / cellSaveDataAutoSave(version, const char* folder name, error dialogs, CellSaveDataSetBuf* buffer, status function,
// file function, container)
void cellSaveDataAutoLoad(Cpu& c) { auto_load_or_save(c, false); }
void cellSaveDataAutoSave(Cpu& c) { auto_load_or_save(c, true); }

constexpr NativeImport kImports[] = {
    {"cellSysutil", "cellSaveDataAutoLoad", cellSaveDataAutoLoad},
    {"cellSysutil", "cellSaveDataAutoSave", cellSaveDataAutoSave},
};

}  // namespace

std::span<const NativeImport> savedata_imports() { return kImports; }

}  // namespace rr7

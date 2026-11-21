// The file system (sys_fs: cellFsOpen and the rest).
//
// A game names files by the console's mount points. Each is a folder of
// the host's here:
//
//   /dev_bdvd   the disc: the player's copy (the folder holding PS3_GAME); read only
//   /app_home   where the program was started from: the disc's PS3_GAME/USRDIR; read only
//   /dev_hdd0   the hard disk: a folder of the port's own
//   /dev_flash  the system software, if the player gave one; read only
//
// A path is kept inside its mount point: `..` can't climb out of it. A name
// is looked up as the game spells it; on a host whose file names ignore
// case that is laxer than a console, never stricter.
//
// The structures are the SDK's, with the layout the game's own code reads:
// a file's size is at byte 36 of CellFsStat (which is packed: 52 bytes), and
// a directory entry is its type, its name's length and its name.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;
namespace stdfs = std::filesystem;

// The file system's errors (the SDK's CELL_FS_E...).
constexpr uint64_t kEisdir = 0x80010012u, kEexist = 0x80010014u, kErofs = 0x80010026u, kEbadf = 0x8001002Au, kEmfile = 0x8001002Bu, kEnotdir = 0x8001002Eu,
                   kEnotmounted = 0x8001003Au;
// cellFsOpen's flags.
constexpr uint32_t kAccessMode = 3, kWriteOnly = 1, kReadWrite = 2, kCreate = 0100, kExclusive = 0200, kTruncate = 01000, kAppend = 02000;
constexpr uint32_t kKnownFlags = kAccessMode | kCreate | kExclusive | kTruncate | kAppend;
// A file's mode.
constexpr uint32_t kDirectory = 0040000, kRegular = 0100000;

struct Handle {
    std::FILE* file = nullptr;  // an open file, or
    bool directory = false;     // an open directory:
    std::vector<stdfs::directory_entry> entries;
    size_t next = 0;
    stdfs::path path;
    bool writable = false, append = false;
};
// A development aid: with RR7_FILES in the environment, every file opened and every read is logged.
const bool g_trace = std::getenv("RR7_FILES") != nullptr;
std::mutex g_mutex;
std::vector<Handle> g_handles(256);  // by descriptor; 0 to 2 are never given (the console's own)

struct Resolved {
    stdfs::path host;
    bool read_only = true;
    uint64_t error = 0;  // if the path isn't under a mount point
};

Resolved resolve(const std::string& path) {
    struct Mount {
        const char* name;
        stdfs::path host;
        bool read_only;
    };
    const stdfs::path& usrdir = game().usrdir;
    const Mount mounts[] = {
        {"dev_bdvd", usrdir.empty() ? stdfs::path() : usrdir.parent_path().parent_path(), true},
        {"app_home", usrdir, true},
        {"dev_hdd0", game().hdd, false},
        {"dev_flash", game().flash, true},
    };
    Resolved out;
    // The path's parts; `.` is nothing and `..` goes up, but not past the mount point.
    std::vector<std::string> parts;
    for (size_t at = 0; at < path.size();) {
        const size_t end = std::min(path.find('/', at), path.size());
        const std::string part = path.substr(at, end - at);
        at = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (parts.size() > 1) parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    if (path.empty() || path[0] != '/' || parts.empty()) {
        out.error = kEnoent;
        return out;
    }
    for (const Mount& mount : mounts) {
        if (parts[0] != mount.name) continue;
        if (mount.host.empty()) {
            out.error = kEnotmounted;
            return out;
        }
        out.host = mount.host;
        for (size_t i = 1; i < parts.size(); ++i) {
            // A part that is a path of the host's in itself (a drive, a backslash) is no name a console's file could have.
            if (parts[i].find_first_of("\\:") != std::string::npos) {
                out.error = kEnoent;
                return out;
            }
            out.host /= stdfs::path(std::u8string(parts[i].begin(), parts[i].end()));  // the name is UTF-8, as the console's are
        }
        out.read_only = mount.read_only;
        return out;
    }
    qilin::rt::log("the game names %s, which is under no mount point this runtime has", path.c_str());
    out.error = kEnotmounted;
    return out;
}

std::string guest_path(uint32_t address) {
    const char* const p = reinterpret_cast<const char*>(guest_memory() + address);
    return std::string(p, strnlen(p, 1056));
}

std::FILE* open_host(const stdfs::path& path, const char* mode) {
#if defined(_WIN32)
    wchar_t wide[8] = {};
    for (size_t i = 0; mode[i] && i < 7; ++i) wide[i] = static_cast<wchar_t>(mode[i]);
    return _wfopen(path.c_str(), wide);
#else
    return std::fopen(path.c_str(), mode);
#endif
}
bool seek_host(std::FILE* file, int64_t offset, int whence) {
#if defined(_WIN32)
    return _fseeki64(file, offset, whence) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), whence) == 0;
#endif
}
int64_t tell_host(std::FILE* file) {
#if defined(_WIN32)
    return _ftelli64(file);
#else
    return static_cast<int64_t>(ftello(file));
#endif
}

// A free descriptor, or 0.
uint32_t free_descriptor() {
    for (uint32_t fd = 3; fd < g_handles.size(); ++fd)
        if (!g_handles[fd].file && !g_handles[fd].directory) return fd;
    return 0;
}
Handle* handle(uint64_t fd, bool directory) {
    if (fd < 3 || fd >= g_handles.size()) return nullptr;
    Handle& h = g_handles[fd];
    if (directory ? !h.directory : !h.file) return nullptr;
    return &h;
}

// CellFsStat: mode, uid, gid (words), then access, modification and change times and the size and block size (8 bytes each), packed.
void store_stat(uint32_t out, const stdfs::path& path, bool read_only) {
    std::error_code error;
    const bool directory = stdfs::is_directory(path, error);
    const uint64_t size = directory ? 4096 : stdfs::file_size(path, error);
    std::memset(guest_memory() + out, 0, 52);
    store32(out, (directory ? kDirectory : kRegular) | (read_only ? (directory ? 0555u : 0444u) : (directory ? 0777u : 0666u)));
    // The times are left at 0: the host's file clock is not the calendar's on every host, and the game has not been seen to read them.
    store32(out + 36, static_cast<uint32_t>(size >> 32));
    store32(out + 40, static_cast<uint32_t>(size));
    store32(out + 44, 0);
    store32(out + 48, 4096);
}

// cellFsOpen(path, flags, int* descriptor, argument, its size)
void cellFsOpen(Cpu& c) {
    const std::string path = guest_path(static_cast<uint32_t>(c.r[3]));
    const uint32_t flags = static_cast<uint32_t>(c.r[4]), out = static_cast<uint32_t>(c.r[5]);
    if (flags & ~kKnownFlags) fatal("cellFsOpen(%s): flags %#o, some of which this runtime doesn't know", path.c_str(), flags);
    const Resolved where = resolve(path);
    if (where.error) {
        c.r[3] = where.error;
        return;
    }
    const uint32_t access = flags & kAccessMode;
    const bool writing = access == kWriteOnly || access == kReadWrite;
    if (access == 3) {
        c.r[3] = kEinval;
        return;
    }
    if (where.read_only && (writing || (flags & (kCreate | kTruncate)))) {
        c.r[3] = kErofs;
        return;
    }
    std::error_code error;
    const bool exists = stdfs::exists(where.host, error);
    if (exists && stdfs::is_directory(where.host, error)) {
        c.r[3] = kEisdir;
        return;
    }
    if (!exists && !(flags & kCreate)) {
        c.r[3] = kEnoent;
        return;
    }
    if (exists && (flags & kCreate) && (flags & kExclusive)) {
        c.r[3] = kEexist;
        return;
    }
    // The host's mode: reading alone; or writing, on a file made or emptied first if that was asked.
    const char* mode = "rb";
    if (writing) {
        if (!exists || (flags & kTruncate)) {
            std::FILE* made = open_host(where.host, "wb");
            if (!made) {
                c.r[3] = kEnoent;  // (its folder isn't there)
                return;
            }
            std::fclose(made);
        }
        mode = "r+b";
    }
    const std::lock_guard lock(g_mutex);
    const uint32_t fd = free_descriptor();
    if (!fd) {
        c.r[3] = kEmfile;
        return;
    }
    std::FILE* file = open_host(where.host, mode);
    if (!file) fatal("cellFsOpen(%s): the host won't open %s", path.c_str(), where.host.string().c_str());
    Handle& h = g_handles[fd];
    h = Handle{};
    h.file = file;
    h.path = where.host;
    h.writable = writing;
    h.append = (flags & kAppend) != 0;
    store32(out, fd);
    if (g_trace) qilin::rt::log("(files) open %s -> %u", path.c_str(), fd);
    c.r[3] = kOk;
}

// cellFsClose(descriptor)
void cellFsClose(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Handle* h = handle(c.r[3], false);
    if (!h) {
        c.r[3] = kEbadf;
        return;
    }
    std::fclose(h->file);
    *h = Handle{};
    c.r[3] = kOk;
}

// cellFsRead(descriptor, buffer, bytes, uint64_t* read)
void cellFsRead(Cpu& c) {
    const uint32_t buffer = static_cast<uint32_t>(c.r[4]), out = static_cast<uint32_t>(c.r[6]);
    const uint64_t bytes = c.r[5];
    std::FILE* file;
    {
        const std::lock_guard lock(g_mutex);
        Handle* h = handle(c.r[3], false);
        if (!h) {
            c.r[3] = kEbadf;
            return;
        }
        file = h->file;
    }
    if (bytes > (uint64_t{1} << 32) - buffer) {
        c.r[3] = kEfault;
        return;
    }
    const int64_t from = g_trace ? tell_host(file) : 0;
    const size_t got = std::fread(guest_memory() + buffer, 1, static_cast<size_t>(bytes), file);
    if (g_trace) qilin::rt::log("(files) read %u: %llu bytes at %lld to %08x, got %zu", static_cast<uint32_t>(c.r[3]), static_cast<unsigned long long>(bytes), static_cast<long long>(from), buffer, got);
    if (out) store64(out, got);
    c.r[3] = kOk;
}

// cellFsWrite(descriptor, buffer, bytes, uint64_t* written)
void cellFsWrite(Cpu& c) {
    const uint32_t buffer = static_cast<uint32_t>(c.r[4]), out = static_cast<uint32_t>(c.r[6]);
    const uint64_t bytes = c.r[5];
    const std::lock_guard lock(g_mutex);
    Handle* h = handle(c.r[3], false);
    if (!h || !h->writable) {
        c.r[3] = kEbadf;
        return;
    }
    if (bytes > (uint64_t{1} << 32) - buffer) {
        c.r[3] = kEfault;
        return;
    }
    if (h->append) seek_host(h->file, 0, SEEK_END);
    const size_t put = std::fwrite(guest_memory() + buffer, 1, static_cast<size_t>(bytes), h->file);
    std::fflush(h->file);
    if (put != bytes) fatal("the host wrote %zu of %llu bytes to %s", put, static_cast<unsigned long long>(bytes), h->path.string().c_str());
    if (out) store64(out, put);
    c.r[3] = kOk;
}

// cellFsLseek(descriptor, offset, whence, uint64_t* position)
void cellFsLseek(Cpu& c) {
    const int64_t offset = static_cast<int64_t>(c.r[4]);
    const uint32_t whence = static_cast<uint32_t>(c.r[5]), out = static_cast<uint32_t>(c.r[6]);
    const std::lock_guard lock(g_mutex);
    Handle* h = handle(c.r[3], false);
    if (!h) {
        c.r[3] = kEbadf;
        return;
    }
    if (whence > 2 || !seek_host(h->file, offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END)) {
        c.r[3] = kEinval;
        return;
    }
    if (out) store64(out, static_cast<uint64_t>(tell_host(h->file)));
    c.r[3] = kOk;
}

// cellFsFstat(descriptor, CellFsStat* status)
void cellFsFstat(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Handle* h = handle(c.r[3], false);
    if (!h) h = handle(c.r[3], true);
    if (!h) {
        c.r[3] = kEbadf;
        return;
    }
    if (h->file) std::fflush(h->file);
    store_stat(static_cast<uint32_t>(c.r[4]), h->path, !h->writable);
    c.r[3] = kOk;
}

// cellFsOpendir(path, int* descriptor)
void cellFsOpendir(Cpu& c) {
    const std::string path = guest_path(static_cast<uint32_t>(c.r[3]));
    const uint32_t out = static_cast<uint32_t>(c.r[4]);
    const Resolved where = resolve(path);
    if (where.error) {
        c.r[3] = where.error;
        return;
    }
    std::error_code error;
    if (!stdfs::exists(where.host, error)) {
        c.r[3] = kEnoent;
        return;
    }
    if (!stdfs::is_directory(where.host, error)) {
        c.r[3] = kEnotdir;
        return;
    }
    const std::lock_guard lock(g_mutex);
    const uint32_t fd = free_descriptor();
    if (!fd) {
        c.r[3] = kEmfile;
        return;
    }
    Handle& h = g_handles[fd];
    h = Handle{};
    h.directory = true;
    h.path = where.host;
    for (stdfs::directory_iterator it(where.host, error), end; !error && it != end; it.increment(error)) h.entries.push_back(*it);
    // In the names' order: a host's own order is no console's, and this one is at least the same everywhere.
    std::ranges::sort(h.entries, {}, [](const stdfs::directory_entry& e) { return e.path().filename().u8string(); });
    store32(out, fd);
    c.r[3] = kOk;
}

// cellFsReaddir(descriptor, CellFsDirent* entry, uint64_t* read): the next entry (its type: 1 a directory, 2 a file; its
// name's length; its name), and 258, the entry's size; or 0 read at the end.
void cellFsReaddir(Cpu& c) {
    const uint32_t entry = static_cast<uint32_t>(c.r[4]), out = static_cast<uint32_t>(c.r[5]);
    const std::lock_guard lock(g_mutex);
    Handle* h = handle(c.r[3], true);
    if (!h) {
        c.r[3] = kEbadf;
        return;
    }
    if (h->next == h->entries.size()) {
        store64(out, 0);
        c.r[3] = kOk;
        return;
    }
    const stdfs::directory_entry& e = h->entries[h->next++];
    const auto name8 = e.path().filename().u8string();
    const std::string name(name8.begin(), name8.end());
    if (name.size() > 255) fatal("a file's name in %s is longer than a console's can be", h->path.string().c_str());
    std::error_code error;
    std::memset(guest_memory() + entry, 0, 258);
    guest_memory()[entry] = e.is_directory(error) ? 1 : 2;
    guest_memory()[entry + 1] = static_cast<uint8_t>(name.size());
    std::memcpy(guest_memory() + entry + 2, name.data(), name.size());
    store64(out, 258);
    c.r[3] = kOk;
}

// cellFsClosedir(descriptor)
void cellFsClosedir(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Handle* h = handle(c.r[3], true);
    if (!h) {
        c.r[3] = kEbadf;
        return;
    }
    *h = Handle{};
    c.r[3] = kOk;
}

// cellFsUnlink(path)
void cellFsUnlink(Cpu& c) {
    const std::string path = guest_path(static_cast<uint32_t>(c.r[3]));
    const Resolved where = resolve(path);
    if (where.error) {
        c.r[3] = where.error;
        return;
    }
    if (where.read_only) {
        c.r[3] = kErofs;
        return;
    }
    std::error_code error;
    if (!stdfs::exists(where.host, error)) {
        c.r[3] = kEnoent;
        return;
    }
    if (stdfs::is_directory(where.host, error)) {
        c.r[3] = kEisdir;
        return;
    }
    if (!stdfs::remove(where.host, error)) fatal("cellFsUnlink(%s): the host won't remove %s", path.c_str(), where.host.string().c_str());
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"sys_fs", "cellFsOpen", cellFsOpen},       {"sys_fs", "cellFsClose", cellFsClose},     {"sys_fs", "cellFsRead", cellFsRead},
    {"sys_fs", "cellFsWrite", cellFsWrite},     {"sys_fs", "cellFsLseek", cellFsLseek},     {"sys_fs", "cellFsFstat", cellFsFstat},
    {"sys_fs", "cellFsOpendir", cellFsOpendir}, {"sys_fs", "cellFsReaddir", cellFsReaddir}, {"sys_fs", "cellFsClosedir", cellFsClosedir},
    {"sys_fs", "cellFsUnlink", cellFsUnlink},
};

}  // namespace

std::span<const NativeImport> fs_imports() { return kImports; }

}  // namespace rr7

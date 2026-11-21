// sceNp: PlayStation Network, as it is on a console that is not signed
// in. The game's servers closed long ago, and this runtime has no network
// (net.cpp): the library starts, as it does on any console, and everything
// that would need the network is to answer as a console answers offline.
//
// So far only the library's start is here. Each further function stops
// the game by name when it is first called, to be given its offline answer
// then.
#include <mutex>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

// The library's errors (the SDK's SCE_NP_ERROR_...).
constexpr uint64_t kNotInitialised = 0x8002aa01u, kAlreadyInitialised = 0x8002aa02u;

std::mutex g_mutex;
bool g_initialised = false;

// sceNpInit(the size of a pool of memory for the library, the pool)
void sceNpInit(Cpu& c) {
    const uint32_t size = static_cast<uint32_t>(c.r[3]), pool = static_cast<uint32_t>(c.r[4]);
    if (!pool || size < 128 * 1024) qilin::rt::fatal("sceNpInit: a pool of %u bytes at %08x, which the library refuses (its answer isn't known here)", size, pool);
    const std::lock_guard lock(g_mutex);
    if (g_initialised) {
        c.r[3] = kAlreadyInitialised;
        return;
    }
    g_initialised = true;
    c.r[3] = kOk;
}

// sceNpTerm()
void sceNpTerm(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (!g_initialised) {
        c.r[3] = kNotInitialised;
        return;
    }
    g_initialised = false;
    c.r[3] = kOk;
}

// sceNpManagerGetStatus(int* status): where signing in has got to. Here always -1: offline.
void sceNpManagerGetStatus(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]);
    const std::lock_guard lock(g_mutex);
    if (!g_initialised) {
        c.r[3] = kNotInitialised;
        return;
    }
    if (!out) qilin::rt::fatal("sceNpManagerGetStatus: no place for the status (the library's answer isn't known here)");
    store32(out, static_cast<uint32_t>(-1));
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"sceNp", "sceNpManagerGetStatus", sceNpManagerGetStatus},
    {"sceNp", "sceNpInit", sceNpInit},
    {"sceNp", "sceNpTerm", sceNpTerm},
};

}  // namespace

std::span<const NativeImport> np_imports() { return kImports; }

}  // namespace rr7

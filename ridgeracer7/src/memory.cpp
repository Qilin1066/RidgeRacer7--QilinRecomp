// The process's memory, as the system hands it out: sys_memory_allocate,
// sys_memory_free and sys_memory_get_user_memory_size (system calls 348, 349
// and 352).
//
// A process asks for memory in whole pages of 1 MiB or 64 KiB and is given
// an address. Here the addresses come from one range of the guest's address
// space, first fit, and the pages behind them are committed when given and
// zeroed. How much there is in all is the console's: a PS3 has 256 MiB of
// main memory and a game is given what the system doesn't keep.
// *kUserMemory is that figure by public descriptions (213 MiB); the game
// asks how much there is (system call 352), so it matters, and is to be
// confirmed against RPCS3.*
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

constexpr uint32_t kHeapStart = 0x20000000u, kHeapEnd = 0x30000000u;  // where the addresses come from
constexpr uint32_t kUserMemory = 0x0D500000u;                         // what the game may have in all
constexpr uint64_t kPage1M = 0x400, kPage64K = 0x200;                 // the flags' page sizes
constexpr uint64_t kEalign = 0x80010010u;

std::mutex g_mutex;
std::map<uint32_t, uint32_t> g_blocks;  // address -> size, of what is allocated
uint32_t g_used = 0;

// sys_memory_allocate(size, flags, uint32_t* address)
void sys_memory_allocate(Cpu& c) {
    const uint64_t size = c.r[3], flags = c.r[4];
    const uint32_t out = static_cast<uint32_t>(c.r[5]);
    if (flags != kPage1M && flags != kPage64K && flags != 0) {
        c.r[3] = kEinval;
        return;
    }
    const uint32_t page = flags == kPage64K ? 0x10000u : 0x100000u;
    if (size == 0 || size % page) {
        c.r[3] = kEalign;
        return;
    }
    const std::lock_guard lock(g_mutex);
    if (size > kUserMemory - g_used) {
        c.r[3] = kEnomem;
        return;
    }
    // First fit among the gaps, at the page's alignment.
    uint32_t at = kHeapStart;
    for (const auto& [address, length] : g_blocks) {
        if (address - at >= size) break;
        at = (address + length + page - 1) & ~(page - 1);
    }
    if (at >= kHeapEnd || size > kHeapEnd - at) {
        c.r[3] = kEnomem;
        return;
    }
    commit(at, static_cast<uint32_t>(size));
    std::memset(guest_memory() + at, 0, size);  // (a block given again was used before)
    g_blocks.emplace(at, static_cast<uint32_t>(size));
    g_used += static_cast<uint32_t>(size);
    store32(out, at);
    c.r[3] = kOk;
}

// sys_memory_free(address): of a block sys_memory_allocate gave.
void sys_memory_free(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto it = g_blocks.find(static_cast<uint32_t>(c.r[3]));
    if (it == g_blocks.end()) {
        c.r[3] = kEinval;
        return;
    }
    g_used -= it->second;
    g_blocks.erase(it);
    c.r[3] = kOk;
}

// sys_memory_get_user_memory_size(sys_memory_info_t* info): the total, and what is left.
void sys_memory_get_user_memory_size(Cpu& c) {
    const uint32_t info = static_cast<uint32_t>(c.r[3]);
    const std::lock_guard lock(g_mutex);
    store32(info, kUserMemory);
    store32(info + 4, kUserMemory - g_used);
    c.r[3] = kOk;
}

constexpr NativeSystemCall kCalls[] = {
    {348, sys_memory_allocate},
    {349, sys_memory_free},
    {352, sys_memory_get_user_memory_size},
};

}  // namespace

std::span<const NativeSystemCall> memory_system_calls() { return kCalls; }

}  // namespace rr7

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

#include "gcm.hpp"
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

// A development aid: a block given or freed over the memory the RSX reads its commands from is logged (its commands
// were once found zeroed).
void note_io_overlap(const char* what, uint32_t at, uint64_t size) {
    const RsxSetup& io = rsx_setup();
    if (io.io_size && at < uint64_t{io.io_address} + io.io_size && io.io_address < at + size)
        qilin::rt::log("(memory) thread %08x %s %llu bytes at %08x, over the RSX's memory at %08x (%u bytes)", current_thread(), what,
                       static_cast<unsigned long long>(size), at, io.io_address, io.io_size);
}
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
    // First fit among the gaps, at the page's alignment. (`at` can pass a block's start when it is rounded up to a
    // larger page than that block's: such a block is no gap's end, and is stepped over. Leaving that out once gave
    // two of the game's packs the same memory.)
    uint32_t at = kHeapStart;
    for (const auto& [address, length] : g_blocks) {
        if (address >= at && address - at >= size) break;
        at = std::max(at, (address + length + page - 1) & ~(page - 1));
    }
    if (at >= kHeapEnd || size > kHeapEnd - at) {
        c.r[3] = kEnomem;
        return;
    }
    note_io_overlap("is given", at, size);
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
    note_io_overlap("frees", it->first, it->second);
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

// Memory containers (system calls 341 and 342): a part of the process's memory set aside, which the
// game then lends to one of the system's utilities (a save dialog, say) to do its work in. Setting it
// aside takes it out of what sys_memory_allocate can give; nothing is committed for it until it is used.
// (The game makes one of 3 MiB and passes its id on: read off the code around the call.)
std::map<uint32_t, uint32_t> g_containers;  // id -> size
uint32_t g_next_container = 0x3F000001u;

// sys_memory_container_create(sys_memory_container_t* id, size)
void sys_memory_container_create(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]);
    const uint64_t size = c.r[4] & ~uint64_t{0xfffff};  // whole megabytes
    if (!out) {
        c.r[3] = kEfault;
        return;
    }
    const std::lock_guard lock(g_mutex);
    if (size == 0 || size > kUserMemory - g_used) {
        c.r[3] = kEnomem;
        return;
    }
    g_used += static_cast<uint32_t>(size);
    const uint32_t id = g_next_container++;
    g_containers.emplace(id, static_cast<uint32_t>(size));
    store32(out, id);
    c.r[3] = kOk;
}

// sys_memory_container_destroy(id)
void sys_memory_container_destroy(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto it = g_containers.find(static_cast<uint32_t>(c.r[3]));
    if (it == g_containers.end()) {
        c.r[3] = kEsrch;
        return;
    }
    g_used -= it->second;
    g_containers.erase(it);
    c.r[3] = kOk;
}

constexpr NativeSystemCall kCalls[] = {
    {341, sys_memory_container_create},
    {342, sys_memory_container_destroy},
    {348, sys_memory_allocate},
    {349, sys_memory_free},
    {352, sys_memory_get_user_memory_size},
};

}  // namespace

std::span<const NativeSystemCall> memory_system_calls() { return kCalls; }

}  // namespace rr7

#include "kernel.hpp"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>

#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

struct Where {
    uint32_t thread = 0;
    std::atomic<const char*> what{nullptr};
    std::atomic<uint32_t> number{0}, from{0};
    std::atomic<uint64_t> calls{0};
};
thread_local uint32_t t_thread = 0;
thread_local Where* t_where = nullptr;
std::mutex g_where_mutex;
std::deque<Where> g_where;  // one for each guest thread there has been

}  // namespace

uint32_t current_thread() { return t_thread; }
void set_current_thread(uint32_t id) {
    t_thread = id;
    const std::lock_guard lock(g_where_mutex);
    t_where = &g_where.emplace_back();
    t_where->thread = id;
}

void thread_enters(const char* what, uint32_t number, uint32_t from) {
    if (!t_where) return;
    t_where->number.store(number, std::memory_order_relaxed);
    t_where->from.store(from, std::memory_order_relaxed);
    t_where->what.store(what, std::memory_order_relaxed);
    t_where->calls.fetch_add(1, std::memory_order_relaxed);
}
void thread_leaves() {
    if (t_where) t_where->what.store(nullptr, std::memory_order_relaxed);
}

void start_watch() {
    const char* const seconds = std::getenv("RR7_WATCH");
    if (!seconds) return;
    std::thread([wait = std::atoi(seconds)] {
        std::this_thread::sleep_for(std::chrono::seconds(wait));
        const std::lock_guard lock(g_where_mutex);
        for (const Where& w : g_where) {
            const char* const what = w.what.load();
            qilin::rt::log("watch: thread %08x, %llu calls to the system; %s%s (%u), called from %08x", w.thread, static_cast<unsigned long long>(w.calls.load()),
                           what ? "inside " : "in the game's own code, last out of ", what ? what : "its last call", w.number.load(), w.from.load());
        }
        qilin::rt::fatal("the watch's %d seconds are over", wait);
    }).detach();
}

uint32_t load32(uint32_t address) {
    uint32_t v;
    std::memcpy(&v, guest_memory() + address, 4);
    return std::byteswap(v);
}
void store32(uint32_t address, uint32_t value) {
    const uint32_t v = std::byteswap(value);
    std::memcpy(guest_memory() + address, &v, 4);
}
uint64_t load64(uint32_t address) {
    uint64_t v;
    std::memcpy(&v, guest_memory() + address, 8);
    return std::byteswap(v);
}
void store64(uint32_t address, uint64_t value) {
    const uint64_t v = std::byteswap(value);
    std::memcpy(guest_memory() + address, &v, 8);
}

}  // namespace rr7

#include "kernel.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "kernel_sched.hpp"
#include "machine.hpp"
#include "qilin/rt/report.hpp"
#include "syscall_names.hpp"

namespace rr7 {

namespace {

struct Where {
    uint32_t thread = 0;
    std::atomic<const char*> what{nullptr};
    std::atomic<uint32_t> number{0}, from{0};
    std::atomic<uint64_t> calls{0};
    PastCall past[16] = {};  // a ring of the thread's last calls (written by the thread itself)
    std::atomic<const Cpu*> cpu{nullptr};  // its registers, when it is a PPU thread (for its instruction: RR7_PROFILE)
    std::atomic<bool> ended{false};        // its host thread has ended (RR7_PROFILE no longer counts it)
};
thread_local uint32_t t_thread = 0;
thread_local Where* t_where = nullptr;
thread_local const Cpu* t_cpu = nullptr;
thread_local int t_native_depth = 0;  // native calls the thread is inside (more than one: a handler called the game back)
std::mutex g_where_mutex;
std::deque<Where> g_where;  // one for each guest thread there is, and those ended not yet reused
std::vector<Where*> g_spare;  // ended ones, to be reused (the game starts SPURS tasks every frame: each a guest thread)

// A thread's record made ready for another guest thread (under g_where_mutex, while no thread writes it).
void reset_where(Where& w, uint32_t id) {
    w.thread = id;
    w.what.store(nullptr, std::memory_order_relaxed);
    w.number.store(0, std::memory_order_relaxed);
    w.from.store(0, std::memory_order_relaxed);
    w.calls.store(0, std::memory_order_relaxed);
    std::fill(std::begin(w.past), std::end(w.past), PastCall{});
    w.cpu.store(nullptr, std::memory_order_relaxed);
    w.ended.store(false, std::memory_order_relaxed);
}

}  // namespace

// Marks the thread's Where as the host thread ends, and clears its registers from it before its stack (which holds
// them) is gone: not every guest thread ends through run_thread (a callback's thread, made by become_guest_thread,
// just ends), and SPU threads end by themselves.
struct EndOnExit {
    ~EndOnExit();
};
thread_local EndOnExit t_end_on_exit;

uint32_t current_thread() { return t_thread; }
void set_current_thread(uint32_t id) {
    t_thread = id;
    static_cast<void>(&t_end_on_exit);  // (made, so that it is destroyed with the thread)
    const std::lock_guard lock(g_where_mutex);
    // A host thread that becomes another guest thread (a SPURS task's kept host thread) keeps its record; a new one
    // takes an ended thread's if there is one. Only the thread itself writes its record outside the lock.
    if (!t_where) {
        if (g_spare.empty()) {
            t_where = &g_where.emplace_back();
        } else {
            t_where = g_spare.back();
            g_spare.pop_back();
        }
    }
    reset_where(*t_where, id);
}

EndOnExit::~EndOnExit() {
    const std::lock_guard lock(g_where_mutex);
    if (!t_where) return;
    t_where->cpu.store(nullptr, std::memory_order_relaxed);
    t_where->ended.store(true, std::memory_order_relaxed);
    g_spare.push_back(t_where);
    t_where = nullptr;
}

void thread_enters(const char* what, uint32_t number, uint32_t from) {
    ++t_native_depth;
    if (!t_where) return;
    t_where->number.store(number, std::memory_order_relaxed);
    t_where->from.store(from, std::memory_order_relaxed);
    t_where->what.store(what, std::memory_order_relaxed);
    t_where->past[t_where->calls.load(std::memory_order_relaxed) % 16] = {number ? nullptr : what, number, from};
    t_where->calls.fetch_add(1, std::memory_order_relaxed);
}

size_t past_calls(PastCall* out, size_t most) {
    if (!t_where) return 0;
    const uint64_t calls = t_where->calls.load(std::memory_order_relaxed);
    size_t n = 0;
    for (; n < most && n < 16 && n < calls; ++n) out[n] = t_where->past[(calls - 1 - n) % 16];
    return n;
}
void set_current_cpu(const Cpu* cpu) {
    t_cpu = cpu;
    // (Under the lock RR7_PROFILE's sampling holds: never while a sample reads them.)
    const std::lock_guard lock(g_where_mutex);
    if (t_where) t_where->cpu.store(cpu, std::memory_order_relaxed);
}
const Cpu* current_cpu() { return t_cpu; }

void thread_leaves() {
    if (t_where) t_where->what.store(nullptr, std::memory_order_relaxed);
    // Back to the game's code: on a token (a wait gives it up and takes it back itself; this is only in case one didn't),
    // and giving way to a thread of better priority waiting for one, as the console would have when it became ready. Not
    // inside another native call (the game's function a handler calls back): the handler may hold a lock of its own.
    take_core();
    if (--t_native_depth == 0) switch_point();
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

namespace {

// RR7_PROFILE's counts: by thread, by place. A place is an import the runtime provides (its name's address, with the top
// bit set), a numbered system call (its number, with the next bit set too) or the start of a lifted function; 0 is the
// game's own code where no instruction is known (an SPU thread's).
constexpr uint64_t kSystemCall = uint64_t{1} << 63, kNumbered = uint64_t{1} << 62;
std::mutex g_profile_mutex;
std::map<uint32_t, std::map<uint64_t, uint64_t>> g_profile;
std::atomic<bool> g_profiling{false};

}  // namespace

void start_profile() {
    if (!std::getenv("RR7_PROFILE")) return;
    g_profiling = true;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::vector<std::pair<uint32_t, uint64_t>> seen;
            {
                const std::lock_guard lock(g_where_mutex);
                for (const Where& w : g_where) {
                    if (w.ended.load(std::memory_order_relaxed)) continue;
                    uint64_t place = 0;
                    if (const char* what = w.what.load(std::memory_order_relaxed)) {
                        const uint32_t number = w.number.load(std::memory_order_relaxed);
                        place = number ? kSystemCall | kNumbered | number : kSystemCall | reinterpret_cast<uintptr_t>(what);
                    } else if (const Cpu* cpu = w.cpu.load(std::memory_order_relaxed)) {
                        // (Read while the thread runs: a sample, which may be a moment stale.)
                        uint32_t pc;
                        std::memcpy(&pc, &cpu->pc, 4);
                        place = lifted_function_at(pc);
                    }
                    seen.emplace_back(w.thread, place);
                }
            }
            const std::lock_guard lock(g_profile_mutex);
            for (const auto& [thread, place] : seen) ++g_profile[thread][place];
        }
    }).detach();
}

void profile_report() {
    if (!g_profiling) return;
    const std::lock_guard lock(g_profile_mutex);
    for (const auto& [thread, places] : g_profile) {
        uint64_t total = 0;
        for (const auto& [place, n] : places) total += n;
        if (total < 500) continue;  // (a thread that lived half a second or less)
        std::vector<std::pair<uint64_t, uint64_t>> top(places.begin(), places.end());
        std::ranges::sort(top, [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string line;
        for (size_t i = 0; i < top.size() && i < 6; ++i) {
            char part[160];
            const double share = 100.0 * static_cast<double>(top[i].second) / static_cast<double>(total);
            if ((top[i].first & kSystemCall) && (top[i].first & kNumbered)) {
                const uint32_t number = static_cast<uint32_t>(top[i].first);
                const qilin::SystemCallName* name = qilin::system_call_name(number);
                std::snprintf(part, sizeof part, "%s%.0f%% in system call %u (%.*s)", i ? ", " : "", share, number, name ? static_cast<int>(name->name.size()) : 1,
                              name ? name->name.data() : "?");
            } else if (top[i].first & kSystemCall)
                std::snprintf(part, sizeof part, "%s%.0f%% in %s", i ? ", " : "", share, reinterpret_cast<const char*>(top[i].first & ~kSystemCall));
            else if (top[i].first)
                std::snprintf(part, sizeof part, "%s%.0f%% in %08x", i ? ", " : "", share, static_cast<uint32_t>(top[i].first));
            else
                std::snprintf(part, sizeof part, "%s%.0f%% in its own code", i ? ", " : "", share);
            line += part;
        }
        qilin::rt::log("profile: thread %08x, %llu samples: %s", thread, static_cast<unsigned long long>(total), line.c_str());
    }
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

// The scheduler (kernel_sched.hpp says how it works).
#include "kernel_sched.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

#include "qilin/rt/report.hpp"

namespace rr7 {

struct SchedThread {
    uint32_t id = 0;
    std::atomic<int32_t> priority{0};  // changed by any thread, under g_mutex; read by its own without it
    std::atomic<bool> holding{false};  // written by its own thread; read by the lender's scan, under g_mutex
    bool granted = false, queued = false;  // under g_mutex
    int64_t seq = 0;                       // its place among equals while queued
    std::chrono::steady_clock::time_point since;  // queued since
    std::condition_variable cv;
};

namespace {

// How long a thread waits for a token before it runs on a lent one: when the threads holding them are no better than
// it (the console would have preempted one, or shared its time with it), and when they are (something holds a token
// that should have given it up).
constexpr auto kPatience = std::chrono::milliseconds(2);
constexpr auto kLongPatience = std::chrono::milliseconds(50);
constexpr uint64_t kLentLogged = 8;  // the first so many lent tokens are logged

// The tokens free (negative while some are lent) and the threads waiting for one, in one word, so that both are seen
// and changed together. Waiting threads > 0 means none is free: a token given back then goes straight to one.
std::atomic<uint64_t> g_state{0};
constexpr uint64_t pack(int32_t free, uint32_t waiting) { return uint64_t{waiting} << 32 | static_cast<uint32_t>(free); }
constexpr int32_t free_of(uint64_t s) { return static_cast<int32_t>(static_cast<uint32_t>(s)); }
constexpr uint32_t waiting_of(uint64_t s) { return static_cast<uint32_t>(s >> 32); }

std::mutex g_mutex;                          // guards what follows, and every thread's granted, queued and seq
std::vector<SchedThread*> g_ready;           // waiting for a token
std::map<uint32_t, SchedThread*> g_threads;  // every scheduled thread, by id
int64_t g_back_seq = 0, g_front_seq = 0;     // places: a new waiter's (last among equals), a preempted one's (first)
std::atomic<int32_t> g_best_waiting{INT32_MAX};  // the best priority in g_ready (INT32_MAX: none), read without the lock
std::atomic<bool> g_free{false};
std::atomic<bool> g_configured{false};
bool g_lend = true;  // (under g_mutex)
std::atomic<uint64_t> g_lent{0}, g_waits{0};
thread_local SchedThread* t_self = nullptr;

void configure_locked(int tokens, bool free_threads, bool lend) {
    g_lend = lend;
    g_state.store(pack(tokens, 0));
    g_free.store(free_threads);
    g_configured.store(true, std::memory_order_release);
}

void configure_from_environment() {
    if (g_configured.load(std::memory_order_acquire)) return;
    const std::lock_guard lock(g_mutex);
    if (g_configured.load()) return;
    const char* const text = std::getenv("RR7_FREE_THREADS");
    const bool free_threads = text && *text && *text != '0';
    configure_locked(kCoreTokens, free_threads, true);
    if (free_threads) qilin::rt::log("(scheduler) RR7_FREE_THREADS: every guest thread runs freely");
    else qilin::rt::log("(scheduler) %d core tokens, by priority", kCoreTokens);
}

bool before(const SchedThread* a, const SchedThread* b) {
    const int32_t pa = a->priority.load(std::memory_order_relaxed), pb = b->priority.load(std::memory_order_relaxed);
    return pa != pb ? pa < pb : a->seq < b->seq;
}

// The best waiting thread (null if none). Under g_mutex.
std::vector<SchedThread*>::iterator best_ready() { return std::ranges::min_element(g_ready, before); }

void note_best() {
    const auto it = best_ready();
    g_best_waiting.store(it == g_ready.end() ? INT32_MAX : (*it)->priority.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void enqueue(SchedThread* t) {
    t->queued = true;
    t->since = std::chrono::steady_clock::now();
    g_ready.push_back(t);
    note_best();
}
void dequeue(SchedThread* t) {
    std::erase(g_ready, t);
    t->queued = false;
    note_best();
}

void grant(SchedThread* t) {
    dequeue(t);
    t->granted = true;
    t->cv.notify_one();
}

// Whether `t`, waiting this long, may run on a lent token. Under g_mutex.
bool may_borrow(const SchedThread* t, std::chrono::steady_clock::duration waited) {
    if (!g_lend) return false;
    if (waited >= kLongPatience) return true;
    int32_t worst = INT32_MIN;
    for (const auto& [id, other] : g_threads)
        if (other->holding.load(std::memory_order_relaxed)) worst = std::max(worst, other->priority.load(std::memory_order_relaxed));
    return worst >= t->priority.load(std::memory_order_relaxed);
}

// Waits, queued, until `t` is given a token (or lent one). Under g_mutex.
void wait_for_grant(std::unique_lock<std::mutex>& lock, SchedThread* t) {
    while (!t->granted) {
        if (t->cv.wait_for(lock, kPatience) == std::cv_status::no_timeout || t->granted) continue;
        const auto waited = std::chrono::steady_clock::now() - t->since;
        if (!may_borrow(t, waited)) continue;
        // Lent: one waiting fewer, one token fewer free (below zero). Nothing else changes the word while some wait.
        uint64_t s = g_state.load();
        while (!g_state.compare_exchange_weak(s, pack(free_of(s) - 1, waiting_of(s) - 1))) {
        }
        dequeue(t);
        const uint64_t lent = g_lent.fetch_add(1) + 1;
        if (lent <= kLentLogged) {
            std::string holders;
            for (const auto& [id, other] : g_threads)
                if (other->holding.load(std::memory_order_relaxed)) {
                    char part[48];
                    std::snprintf(part, sizeof part, " %08x (priority %d)", id, other->priority.load(std::memory_order_relaxed));
                    holders += part;
                }
            qilin::rt::log("(scheduler) thread %08x (priority %d) runs on a lent token after %lld us waiting; the tokens are held by%s%s", t->id,
                           t->priority.load(std::memory_order_relaxed),
                           static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(waited).count()), holders.empty() ? " none" : holders.c_str(),
                           lent == kLentLogged ? " (no more of these are logged)" : "");
        }
        break;
    }
    t->granted = false;
    t->holding.store(true, std::memory_order_relaxed);
}

// A token given back: to the best waiting thread if one waits, else to the pool (or a lent one repaid).
void give_back() {
    uint64_t s = g_state.load(std::memory_order_relaxed);
    while (waiting_of(s) == 0)
        if (g_state.compare_exchange_weak(s, pack(free_of(s) + 1, 0), std::memory_order_release, std::memory_order_relaxed)) return;
    const std::lock_guard lock(g_mutex);
    s = g_state.load();
    for (;;) {
        if (waiting_of(s) == 0 || free_of(s) < 0) {  // (a lent token repaid: the waiting go on waiting for a real one)
            if (g_state.compare_exchange_weak(s, pack(free_of(s) + 1, waiting_of(s)))) return;
            continue;
        }
        if (g_state.compare_exchange_weak(s, pack(free_of(s), waiting_of(s) - 1))) break;
    }
    grant(*best_ready());
}

// `t` gives its token straight to the best waiting thread if that is better than it (or as good, with `equal`), and
// waits for one again: first among equals when preempted, last when yielding.
void give_way(SchedThread* t, bool equal) {
    std::unique_lock lock(g_mutex);
    const auto it = best_ready();
    if (it == g_ready.end()) return;
    const int32_t theirs = (*it)->priority.load(std::memory_order_relaxed), mine = t->priority.load(std::memory_order_relaxed);
    if (theirs > mine || (theirs == mine && !equal)) return;
    // (One waiting fewer, one more: the word doesn't change.)
    SchedThread* const next = *it;
    t->holding.store(false, std::memory_order_relaxed);
    t->seq = equal ? ++g_back_seq : --g_front_seq;
    enqueue(t);
    grant(next);
    wait_for_grant(lock, t);
}

}  // namespace

void configure_scheduler(int tokens, bool free_threads, bool lend) {
    const std::lock_guard lock(g_mutex);
    if (!g_threads.empty()) qilin::rt::fatal("the scheduler was configured with threads in it");
    configure_locked(tokens, free_threads, lend);
}

bool threads_run_freely() { return g_free.load(std::memory_order_relaxed); }

SchedThread* new_sched_thread(uint32_t id, int32_t priority) {
    configure_from_environment();
    auto* t = new SchedThread;
    t->id = id;
    t->priority.store(priority);
    const std::lock_guard lock(g_mutex);
    if (!g_threads.emplace(id, t).second) qilin::rt::fatal("the scheduler was given thread %08x twice", id);
    return t;
}

void bind_sched_thread(SchedThread* thread) {
    if (t_self) end_sched_thread();  // (a host thread that becomes another guest thread)
    t_self = thread;
}

void end_sched_thread() {
    SchedThread* const t = t_self;
    if (!t) return;
    give_up_core();
    t_self = nullptr;
    const std::lock_guard lock(g_mutex);
    g_threads.erase(t->id);
    delete t;
}

void take_core() {
    SchedThread* const t = t_self;
    if (!t || t->holding.load(std::memory_order_relaxed) || g_free.load(std::memory_order_relaxed)) return;
    uint64_t s = g_state.load(std::memory_order_relaxed);
    while (free_of(s) > 0 && waiting_of(s) == 0)
        if (g_state.compare_exchange_weak(s, pack(free_of(s) - 1, 0), std::memory_order_acquire, std::memory_order_relaxed)) {
            t->holding.store(true, std::memory_order_relaxed);
            return;
        }
    std::unique_lock lock(g_mutex);
    s = g_state.load();
    for (;;) {
        if (free_of(s) > 0 && waiting_of(s) == 0) {
            if (g_state.compare_exchange_weak(s, pack(free_of(s) - 1, 0))) {
                t->holding.store(true, std::memory_order_relaxed);
                return;
            }
            continue;
        }
        if (g_state.compare_exchange_weak(s, pack(free_of(s), waiting_of(s) + 1))) break;
    }
    t->seq = ++g_back_seq;
    enqueue(t);
    wait_for_grant(lock, t);
}

void give_up_core() {
    SchedThread* const t = t_self;
    if (!t || !t->holding.load(std::memory_order_relaxed)) return;
    t->holding.store(false, std::memory_order_relaxed);
    give_back();
}

bool holds_core() {
    const SchedThread* const t = t_self;
    return t && t->holding.load(std::memory_order_relaxed);
}

void switch_point() {
    SchedThread* const t = t_self;
    if (!t || !t->holding.load(std::memory_order_relaxed)) return;
    if (g_best_waiting.load(std::memory_order_relaxed) >= t->priority.load(std::memory_order_relaxed)) return;
    give_way(t, false);
}

void yield_core() {
    SchedThread* const t = t_self;
    if (!t || !t->holding.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
        return;
    }
    if (g_best_waiting.load(std::memory_order_relaxed) > t->priority.load(std::memory_order_relaxed)) return;
    give_way(t, true);
}

void set_sched_priority(uint32_t id, int32_t priority) {
    const std::lock_guard lock(g_mutex);
    const auto it = g_threads.find(id);
    if (it == g_threads.end()) return;  // (a thread the scheduler never runs)
    it->second->priority.store(priority, std::memory_order_relaxed);
    if (it->second->queued) note_best();
}

int32_t current_priority() {
    const SchedThread* const t = t_self;
    return t ? t->priority.load(std::memory_order_relaxed) : kHostPriority;
}

uint64_t lent_tokens() { return g_lent.load(); }
int32_t free_core_tokens() { return free_of(g_state.load()); }
uint32_t threads_waiting_for_core() { return waiting_of(g_state.load()); }
uint64_t waits_begun() { return g_waits.load(); }

// ---- waking orders ----

bool wake_order(uint32_t protocol, WakeOrder& order) {
    switch (protocol) {
    case kSyncFifo:
    case kSyncRetry: order = WakeOrder::Fifo; return true;
    case kSyncPriority:
    case kSyncPriorityInherit: order = WakeOrder::Priority; return true;
    default: return false;
    }
}

bool Waiter::wait(std::unique_lock<std::mutex>& lock, uint64_t timeout) {
    // (A timeout of more than a year is for ever: the clock's arithmetic would overflow on the largest.)
    constexpr uint64_t kForever = uint64_t{366} * 24 * 3600 * 1000000;
    if (timeout == 0 || timeout > kForever) {
        cv.wait(lock, [this] { return woken; });
        return true;
    }
    return cv.wait_for(lock, std::chrono::microseconds(timeout), [this] { return woken; });
}

void WaitQueue::push(Waiter& w) {
    g_waits.fetch_add(1, std::memory_order_relaxed);
    w.seq = ++seq_;
    w.woken = false;
    waiters_.push_back(&w);
}

Waiter* WaitQueue::pop() {
    if (waiters_.empty()) return nullptr;
    auto it = waiters_.begin();  // (pushed in order: the first is the longest waiting)
    if (order_ == WakeOrder::Priority)
        it = std::ranges::min_element(waiters_, [](const Waiter* a, const Waiter* b) { return a->priority != b->priority ? a->priority < b->priority : a->seq < b->seq; });
    Waiter* const w = *it;
    waiters_.erase(it);
    return w;
}

bool WaitQueue::remove(Waiter& w) { return std::erase(waiters_, &w) != 0; }

}  // namespace rr7

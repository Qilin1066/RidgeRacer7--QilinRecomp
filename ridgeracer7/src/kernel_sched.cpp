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

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <sys/prctl.h>
#endif

namespace rr7 {

struct SchedThread {
    uint32_t id = 0;
    std::atomic<int32_t> priority{0};  // changed by any thread, under g_mutex; read by its own without it
    std::atomic<bool> holding{false};  // written by its own thread; read by the lender's scan, under g_mutex
    bool granted = false, queued = false;  // under g_mutex
    int64_t seq = 0;                       // its place among equals while queued
    std::chrono::steady_clock::time_point since;  // queued since
    // When it wakes to look at lending again if not given a token before (under g_mutex): the earliest it may borrow.
    std::chrono::steady_clock::time_point wake_at = std::chrono::steady_clock::time_point::max();
    Parker parker;
};

namespace {

// How long a thread waits for a token before it runs on a lent one: when a thread holding one is worse than it, not at
// all (the console would have preempted that thread at once); when the worst holder is as good as it, kPatience (the
// console would have shared its time with them); when every holder is better, kLongPatience (something holds a token
// that should have given it up).
constexpr auto kPatience = std::chrono::microseconds(250);  // (the game's sound thread, priority 0, has 5.33 ms a block)
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
std::atomic<uint64_t> g_lent{0}, g_waits{0}, g_wakeups{0};
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
    t->parker.unpark();
}

// The worst priority among the threads holding a token (INT32_MIN if none holds one). Under g_mutex.
int32_t worst_holder() {
    int32_t worst = INT32_MIN;
    for (const auto& [id, other] : g_threads)
        if (other->holding.load(std::memory_order_relaxed)) worst = std::max(worst, other->priority.load(std::memory_order_relaxed));
    return worst;
}

// When `t` may run on a lent token, by the holders now: its time queued (t->since) at once, or after a patience; never
// with lending off. Under g_mutex.
std::chrono::steady_clock::time_point borrow_time(const SchedThread* t) {
    if (!g_lend) return std::chrono::steady_clock::time_point::max();
    const int32_t worst = worst_holder(), mine = t->priority.load(std::memory_order_relaxed);
    if (worst > mine) return t->since;  // (at once: the console would have preempted that holder)
    return t->since + (worst == mine ? std::chrono::steady_clock::duration(kPatience) : kLongPatience);
}

// The holders or a priority have changed: a waiting thread that may now borrow sooner than it is waiting to is woken to
// look again. (Nothing else changes when a thread may borrow but time: the waiters wake for that by themselves, so none
// polls.) Under g_mutex.
void hurry_waiters() {
    for (SchedThread* const other : g_ready)
        if (borrow_time(other) < other->wake_at) other->parker.unpark();
}

// Waits, queued, until `t` is given a token (or lent one). Under g_mutex.
void wait_for_grant(std::unique_lock<std::mutex>& lock, SchedThread* t) {
    while (!t->granted) {
        // (Asked before waiting at all, so that a thread outranking a holder runs at once, and again after each wait: it
        // wakes when given a token, when it may borrow one, and when hurry_waiters says that time has come sooner.)
        const auto now = std::chrono::steady_clock::now();
        if (const auto when = borrow_time(t); when > now) {
            t->wake_at = when;
            lock.unlock();
            t->parker.park_until(when);
            lock.lock();
            g_wakeups.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const auto waited = now - t->since;
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
    t->wake_at = std::chrono::steady_clock::time_point::max();
    t->holding.store(true, std::memory_order_relaxed);
    hurry_waiters();  // (a holder more: one waiting may now outrank it, or equal it)
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
    // (Holding is marked before the token is taken, so that a thread queueing for one meanwhile counts this one among the
    // holders when it works out when it may borrow: it is woken again only if that time comes sooner.)
    t->holding.store(true, std::memory_order_seq_cst);
    uint64_t s = g_state.load(std::memory_order_relaxed);
    while (free_of(s) > 0 && waiting_of(s) == 0)
        if (g_state.compare_exchange_weak(s, pack(free_of(s) - 1, 0), std::memory_order_seq_cst, std::memory_order_relaxed)) return;
    std::unique_lock lock(g_mutex);
    t->holding.store(false, std::memory_order_relaxed);
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
    hurry_waiters();
}

int32_t current_priority() {
    const SchedThread* const t = t_self;
    return t ? t->priority.load(std::memory_order_relaxed) : kHostPriority;
}

uint64_t lent_tokens() { return g_lent.load(); }
uint64_t core_wakeups() { return g_wakeups.load(); }
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

// ---- waiting without spinning ----

namespace {

#if defined(_WIN32)
// The calling thread's high-resolution waitable timer (Windows 10 1803 on; an ordinary one before, whose resolution is
// the system timer's), made at its first timed wait and closed as the thread ends.
struct ThreadTimer {
    HANDLE handle = nullptr;
    ThreadTimer() {
        handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!handle) handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (!handle) qilin::rt::fatal("the host refused a waitable timer (error %lu)", GetLastError());
    }
    ~ThreadTimer() { CloseHandle(handle); }
    ThreadTimer(const ThreadTimer&) = delete;
    ThreadTimer& operator=(const ThreadTimer&) = delete;
};
// The timer, set to go off at `when` (which is in the future).
HANDLE timer_at(std::chrono::steady_clock::time_point when) {
    thread_local ThreadTimer timer;
    const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(when - std::chrono::steady_clock::now()).count();
    LARGE_INTEGER due;
    due.QuadPart = -std::max<long long>(left / 100, 1);  // (relative, in 100 ns)
    if (!SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) qilin::rt::fatal("the host refused to set a timer (error %lu)", GetLastError());
    return timer.handle;
}
#else
// Linux delays a thread's timed waits by up to its timer slack (50 us unless changed) to gather wake-ups: a thread that
// sleeps briefly to poll asks for 1 ns. (Other hosts have no such setting.)
void precise_timers() {
#if defined(__linux__)
    thread_local const bool set = prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL) == 0;
    static_cast<void>(set);
#endif
}
#endif

}  // namespace

void sleep_until_precisely(std::chrono::steady_clock::time_point when) {
    if (when <= std::chrono::steady_clock::now()) return;
#if defined(_WIN32)
    WaitForSingleObject(timer_at(when), INFINITE);
#else
    precise_timers();
    std::this_thread::sleep_until(when);
#endif
}

#if defined(_WIN32)
Parker::Parker() : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
    if (!event_) qilin::rt::fatal("the host refused an event (error %lu)", GetLastError());
}
Parker::~Parker() { CloseHandle(event_); }
void Parker::unpark() { SetEvent(event_); }
bool Parker::park_until(std::chrono::steady_clock::time_point when) {
    if (when == std::chrono::steady_clock::time_point::max()) return WaitForSingleObject(event_, INFINITE) == WAIT_OBJECT_0;
    if (when <= std::chrono::steady_clock::now()) return WaitForSingleObject(event_, 0) == WAIT_OBJECT_0;
    const HANDLE handles[2] = {event_, timer_at(when)};
    return WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0;  // (the event first, if both)
}
#else
Parker::Parker() = default;
Parker::~Parker() = default;
void Parker::unpark() {
    {
        const std::lock_guard lock(mutex_);
        permit_ = true;
    }
    cv_.notify_one();
}
bool Parker::park_until(std::chrono::steady_clock::time_point when) {
    precise_timers();
    std::unique_lock lock(mutex_);
    if (when == std::chrono::steady_clock::time_point::max()) cv_.wait(lock, [this] { return permit_; });
    else cv_.wait_until(lock, when, [this] { return permit_; });
    const bool woken = permit_;
    permit_ = false;
    return woken;
}
#endif

void Doorbell::begin() {
    if (waiting_.exchange(true, std::memory_order_seq_cst)) qilin::rt::fatal("two threads waited on one doorbell at once");
    std::atomic_thread_fence(std::memory_order_seq_cst);  // (marked waiting before looking: ring() looks the other way)
}

void Doorbell::park(Clock::time_point when) {
    parker_.park_until(when);
    wakeups_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace rr7

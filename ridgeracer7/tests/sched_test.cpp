// The scheduler's core tokens and the synchronisation objects' waking orders, with guest memory stood in. No game files,
// window or GPU are needed. Each case runs in its own process (the scheduler is set up once per process).
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "events.hpp"
#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace {
using namespace std::chrono_literals;
using rr7::Cpu;

std::vector<uint8_t> memory(1u << 20);
thread_local uint32_t thread_id = rr7::kFirstThreadId;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::_Exit(1);  // (threads may still be waiting)
    }
}

// Waits (polling) until `ready`, at most five seconds.
void until(const std::function<bool()>& ready, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ready()) {
        check(std::chrono::steady_clock::now() < deadline, what);
        std::this_thread::sleep_for(200us);
    }
}

// What the threads did, in order.
std::mutex g_record_mutex;
std::vector<uint32_t> g_record;
void record(uint32_t what) {
    const std::lock_guard lock(g_record_mutex);
    g_record.push_back(what);
}
size_t recorded() {
    const std::lock_guard lock(g_record_mutex);
    return g_record.size();
}
void expect(std::initializer_list<uint32_t> wanted, const char* what) {
    const std::lock_guard lock(g_record_mutex);
    if (!std::ranges::equal(g_record, wanted)) {
        std::string got;
        for (const uint32_t v : g_record) got += " " + std::to_string(v);
        std::fprintf(stderr, "FAIL: %s: the order was%s\n", what, got.c_str());
        std::_Exit(1);
    }
}

using Handler = void (*)(Cpu&);
Handler system_call(uint32_t number) {
    for (const auto& entry : rr7::sync_system_calls())
        if (entry.number == number) return entry.handler;
    for (const auto& entry : rr7::event_system_calls())
        if (entry.number == number) return entry.handler;
    check(false, "a system call the test needs is missing");
    return nullptr;
}
Handler import(const char* name) {
    for (const auto& entry : rr7::sync_imports())
        if (std::strcmp(entry.name, name) == 0) return entry.handler;
    check(false, "an import the test needs is missing");
    return nullptr;
}
// A call into the system as the game makes it, with arguments from r3 on; then the switch point kernel.cpp's
// thread_leaves would pass. Returns the registers.
Cpu call(Handler handler, std::initializer_list<uint64_t> arguments) {
    Cpu c;
    int r = 3;
    for (const uint64_t a : arguments) c.r[r++] = a;
    handler(c);
    rr7::switch_point();
    return c;
}
uint64_t sc(uint32_t number, std::initializer_list<uint64_t> arguments) { return call(system_call(number), arguments).r[3]; }
uint64_t lw(const char* name, std::initializer_list<uint64_t> arguments) { return call(import(name), arguments).r[3]; }

// A guest thread of the test's: a host thread the scheduler knows, holding a token (unless `take` is false) while `body` runs.
std::thread guest(uint32_t id, int32_t priority, std::function<void()> body, bool take = true) {
    rr7::SchedThread* const sched = rr7::new_sched_thread(id, priority);
    return std::thread([id, sched, body = std::move(body), take] {
        thread_id = id;
        rr7::bind_sched_thread(sched);
        if (take) rr7::take_core();
        body();
        rr7::end_sched_thread();
    });
}
// The test's own thread becomes a guest thread, holding a token.
void become_guest(int32_t priority) {
    thread_id = rr7::kFirstThreadId;
    rr7::bind_sched_thread(rr7::new_sched_thread(thread_id, priority));
    rr7::take_core();
}
void join(std::vector<std::thread>& threads) {
    for (std::thread& t : threads) t.join();
    threads.clear();
}
void spin_for(std::chrono::microseconds time) {
    const auto end = std::chrono::steady_clock::now() + time;
    while (std::chrono::steady_clock::now() < end) {
    }
}

// ---------------------------------------------------------------- the tokens

// Taking and giving back with tokens free: the word alone.
void fast() {
    rr7::configure_scheduler(2, false, false);
    become_guest(1000);
    check(rr7::holds_core() && rr7::free_core_tokens() == 1, "a token taken");
    rr7::give_up_core();
    check(!rr7::holds_core() && rr7::free_core_tokens() == 2, "a token given back");
    rr7::give_up_core();  // (nothing: none held)
    check(rr7::free_core_tokens() == 2, "giving up no token changes nothing");
    rr7::take_core();
    rr7::take_core();  // (nothing: one held)
    check(rr7::free_core_tokens() == 1, "taking a token twice takes one");
    std::vector<std::thread> threads;
    threads.push_back(guest(2, 500, [] { check(rr7::free_core_tokens() == 0 && rr7::threads_waiting_for_core() == 0, "the second token taken at once"); }));
    join(threads);
    check(rr7::free_core_tokens() == 1 && rr7::lent_tokens() == 0, "the second token given back as its thread ends");
}

// One token, four threads waiting for it: they get it by priority, the longest waiting first among equals.
void priority() {
    rr7::configure_scheduler(1, false, false);
    become_guest(0);
    std::vector<std::thread> threads;
    uint32_t waiting = 0;
    for (const auto& [id, priority] : {std::pair{11u, 3}, {12u, 1}, {13u, 2}, {14u, 1}}) {
        threads.push_back(guest(id, priority, [id] { record(id); }));
        ++waiting;
        until([waiting] { return rr7::threads_waiting_for_core() == waiting; }, "a thread waits for the token");
    }
    check(recorded() == 0, "nobody runs while the token is held");
    rr7::give_up_core();
    join(threads);
    expect({12, 14, 13, 11}, "the token goes by priority, then first come");
    check(rr7::free_core_tokens() == 1 && rr7::threads_waiting_for_core() == 0, "the token back at the end");
}

// Two tokens and eight threads taking and giving them: never more than two hold one.
void two_tokens() {
    rr7::configure_scheduler(2, false, false);
    std::atomic<int> running{0}, most{0};
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < 8; ++i)
        threads.push_back(guest(100 + i, static_cast<int32_t>(i % 3) * 10, [&] {
            for (int n = 0; n < 2000; ++n) {
                const int now = ++running;
                check(now <= 2, "more than two threads hold a token");
                int seen = most.load();
                while (now > seen && !most.compare_exchange_weak(seen, now)) {
                }
                spin_for(2us);
                --running;
                rr7::give_up_core();
                if (n % 16 == 0) std::this_thread::yield();
                rr7::take_core();
            }
        }));
    join(threads);
    check(most.load() == 2, "two threads ran at once");
    check(rr7::free_core_tokens() == 2 && rr7::threads_waiting_for_core() == 0 && rr7::lent_tokens() == 0, "both tokens back at the end");
}

// A thread of better priority waiting for the token gets it at the holder's next call into the system; one of worse
// priority doesn't.
void switch_point() {
    rr7::configure_scheduler(1, false, false);
    become_guest(1000);
    std::vector<std::thread> threads;
    threads.push_back(guest(2000, 2000, [] { record(2000); }));
    until([] { return rr7::threads_waiting_for_core() == 1; }, "the worse thread waits");
    rr7::switch_point();
    check(rr7::holds_core() && recorded() == 0, "no way given to a worse thread");
    threads.push_back(guest(10, 10, [] { record(10); }));
    until([] { return rr7::threads_waiting_for_core() == 2; }, "the better thread waits");
    rr7::switch_point();  // (to the better thread; back here when it ends, before the worse)
    check(rr7::holds_core(), "the token back after the better thread");
    record(1000);
    rr7::give_up_core();
    join(threads);
    expect({10, 1000, 2000}, "preempted at the switch point, and first again among the rest");
}

// A yield gives way to a thread as good, not to a worse one.
void yield() {
    rr7::configure_scheduler(1, false, false);
    become_guest(5);
    std::vector<std::thread> threads;
    threads.push_back(guest(6, 6, [] { record(6); }));
    until([] { return rr7::threads_waiting_for_core() == 1; }, "the worse thread waits");
    rr7::yield_core();
    check(rr7::holds_core() && recorded() == 0, "no yield to a worse thread");
    threads.push_back(guest(5, 5, [] { record(5); }));
    until([] { return rr7::threads_waiting_for_core() == 2; }, "the equal thread waits");
    rr7::yield_core();
    record(0);
    rr7::give_up_core();
    join(threads);
    expect({5, 0, 6}, "a yield to an equal thread, then back before the worse");
}

// A thread blocking in a native call gives its token up for it: the thread that has to run for the wait to end can.
void blocking() {
    rr7::configure_scheduler(1, false, false);
    become_guest(100);
    std::mutex mutex;
    std::condition_variable changed;
    bool ran = false;
    std::vector<std::thread> threads;
    threads.push_back(guest(200, 200, [&] {
        check(rr7::holds_core(), "the woken thread holds the token");
        const std::lock_guard lock(mutex);
        ran = true;
        changed.notify_all();
    }));
    until([] { return rr7::threads_waiting_for_core() == 1; }, "the other thread waits");
    {
        const rr7::ScopedBlocking outer;
        check(!rr7::holds_core(), "no token while blocked");
        {
            const rr7::ScopedBlocking inner;  // (nested: nothing more to give up, nothing to take back)
        }
        check(!rr7::holds_core(), "still none after a nested block");
        std::unique_lock lock(mutex);
        check(changed.wait_for(lock, 5s, [&] { return ran; }), "the other thread ran while this one blocked");
    }
    check(rr7::holds_core(), "the token taken back after the block");
    // A Blocking that is never released keeps the token.
    {
        rr7::Blocking unused;
        check(rr7::holds_core(), "an unreleased Blocking keeps the token");
    }
    check(rr7::holds_core(), "and has nothing to take back");
    join(threads);
    check(rr7::lent_tokens() == 0, "no token lent");
}

// sys_timer_usleep gives the token up while it sleeps.
void sleeping() {
    rr7::configure_scheduler(1, false, false);
    become_guest(100);
    std::vector<std::thread> threads;
    threads.push_back(guest(200, 200, [] { record(200); }));
    until([] { return rr7::threads_waiting_for_core() == 1; }, "the other thread waits");
    check(sc(141, {50000}) == rr7::kOk, "sys_timer_usleep");
    check(rr7::holds_core(), "the token back after the sleep");
    expect({200}, "the other thread ran during the sleep");
    join(threads);
}

// A thread of better priority waiting while the holder runs on without calling the system runs on a lent token after a
// short wait; one of worse priority only after a long one; the lent tokens are repaid.
void lend() {
    rr7::configure_scheduler(1, false, true);
    become_guest(1000);
    std::vector<std::thread> threads;
    threads.push_back(guest(10, 10, [] { record(10); }));
    const auto from = std::chrono::steady_clock::now();
    while (recorded() == 0 && std::chrono::steady_clock::now() - from < 5s) {
    }
    check(recorded() == 1 && rr7::lent_tokens() == 1, "the better thread ran on a lent token");
    join(threads);
    check(rr7::free_core_tokens() == 0, "the lent token repaid");
    threads.push_back(guest(2000, 2000, [] { record(2000); }));
    until([] { return rr7::threads_waiting_for_core() == 1; }, "the worse thread waits");
    spin_for(20ms);
    check(recorded() == 1, "no lent token for a worse thread soon");
    const auto again = std::chrono::steady_clock::now();
    while (recorded() == 1 && std::chrono::steady_clock::now() - again < 5s) {
    }
    check(recorded() == 2 && rr7::lent_tokens() == 2, "the worse thread ran on a lent token in the end");
    join(threads);
    check(rr7::free_core_tokens() == 0 && rr7::threads_waiting_for_core() == 0, "every lent token repaid");
    rr7::give_up_core();
    check(rr7::free_core_tokens() == 1, "the token back");
}

// RR7_FREE_THREADS: no tokens; every thread runs at once.
void free_threads() {
    rr7::configure_scheduler(2, true);
    std::atomic<int> running{0};
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < 5; ++i)
        threads.push_back(guest(300 + i, 100, [&] {
            check(!rr7::holds_core(), "no tokens with free threads");
            ++running;
            until([&] { return running.load() == 5; }, "every thread runs at once with free threads");
        }));
    join(threads);
    check(rr7::threads_run_freely(), "free threads reported");
}

// ---------------------------------------------------------------- waking orders

constexpr uint32_t kAttribute = 0x100, kOut = 0x200, kLwMutex = 0x300, kLwAttribute = 0x340;

// Three waiters, of priorities 3, 1 and 2, start waiting in that order; `wait` is what each calls (it records itself
// once it has what it waited for), `wake` what wakes the next. Woken by priority, the order is 22, 23, 21; first come,
// 21, 22, 23.
void three_waiters(const std::function<void(uint32_t)>& wait, const std::function<void()>& wake, bool by_priority, const char* what) {
    std::vector<std::thread> threads;
    uint64_t waits = rr7::waits_begun();
    for (const auto& [id, priority] : {std::pair{21u, 3}, {22u, 1}, {23u, 2}}) {
        threads.push_back(guest(id, priority, [id, &wait] { wait(id); }));
        ++waits;
        until([waits] { return rr7::waits_begun() == waits; }, "a waiter waits");
    }
    for (size_t n = 1; n <= 3; ++n) {
        wake();
        until([n] { return recorded() >= n; }, "a waiter is woken");  // (a mutex's waiter hands it on to the next by itself)
    }
    join(threads);
    if (by_priority) expect({22, 23, 21}, what);
    else expect({21, 22, 23}, what);
}

void sync_set_up() {
    rr7::configure_scheduler(64, false, false);  // (tokens enough for all: only the objects' order is tested)
    become_guest(500);
}

void mutex_order(uint32_t protocol) {
    sync_set_up();
    rr7::store32(kAttribute, protocol);
    rr7::store32(kAttribute + 4, 0x20);
    check(sc(100, {kOut, kAttribute}) == rr7::kOk, "sys_mutex_create");
    const uint32_t mutex = rr7::load32(kOut);
    check(sc(102, {mutex, 0}) == rr7::kOk, "the first lock");
    three_waiters(
        [mutex](uint32_t id) {
            check(sc(102, {mutex, 0}) == rr7::kOk, "a waiter's lock");
            record(id);
            check(sc(104, {mutex}) == rr7::kOk, "a waiter's unlock");
        },
        [mutex, first = true]() mutable {
            if (first) check(sc(104, {mutex}) == rr7::kOk, "the first unlock");  // (each waiter then hands it on)
            first = false;
        },
        protocol == rr7::kSyncPriority, "a mutex's waiters");
    check(sc(103, {mutex}) == rr7::kOk && sc(104, {mutex}) == rr7::kOk, "the mutex free at the end");
    check(sc(102, {mutex, 1000}) == rr7::kOk, "a lock with a timeout, free");
    std::vector<std::thread> threads;
    threads.push_back(guest(30, 30, [mutex] { check(sc(102, {mutex, 20000}) == rr7::kEtimedout, "a lock that times out"); }));
    join(threads);
    check(sc(104, {mutex}) == rr7::kOk, "still the owner after another's timeout");
}

void semaphore_order(uint32_t protocol) {
    sync_set_up();
    rr7::store32(kAttribute, protocol);
    check(sc(90, {kOut, kAttribute, 0, 10}) == rr7::kOk, "sys_semaphore_create");
    const uint32_t semaphore = rr7::load32(kOut);
    three_waiters(
        [semaphore](uint32_t id) {
            check(sc(92, {semaphore, 0}) == rr7::kOk, "a waiter's wait");
            record(id);
        },
        [semaphore] { check(sc(94, {semaphore, 1}) == rr7::kOk, "a post"); }, protocol == rr7::kSyncPriority, "a semaphore's waiters");
    check(sc(93, {semaphore}) == rr7::kEbusy, "every post taken by a waiter");
}

void lwmutex_order(uint32_t protocol) {
    sync_set_up();
    rr7::store32(kLwAttribute, protocol);
    rr7::store32(kLwAttribute + 4, 0x20);
    check(lw("sys_lwmutex_create", {kLwMutex, kLwAttribute}) == rr7::kOk, "sys_lwmutex_create");
    check(lw("sys_lwmutex_lock", {kLwMutex, 0}) == rr7::kOk, "the first lock");
    three_waiters(
        [](uint32_t id) {
            check(lw("sys_lwmutex_lock", {kLwMutex, 0}) == rr7::kOk, "a waiter's lock");
            check(rr7::load32(kLwMutex) == id && rr7::load32(kLwMutex + 12) == 1, "the structure names the owner handed the mutex");
            record(id);
            check(lw("sys_lwmutex_unlock", {kLwMutex}) == rr7::kOk, "a waiter's unlock");
        },
        [first = true]() mutable {
            if (first) check(lw("sys_lwmutex_unlock", {kLwMutex}) == rr7::kOk, "the first unlock");
            first = false;
        },
        protocol == rr7::kSyncPriority, "a lightweight mutex's waiters");
    check(rr7::load32(kLwMutex) == 0xFFFFFFFFu, "free at the end");
}

// A condition's waiters are woken in its mutex's order, each returning with the mutex.
void cond_priority() {
    sync_set_up();
    rr7::store32(kAttribute, rr7::kSyncPriority);
    rr7::store32(kAttribute + 4, 0x10);  // (recursive: each waiter holds it twice)
    check(sc(100, {kOut, kAttribute}) == rr7::kOk, "sys_mutex_create");
    const uint32_t mutex = rr7::load32(kOut);
    check(sc(105, {kOut + 4, mutex, kAttribute + 0x40}) == rr7::kOk, "sys_cond_create");
    const uint32_t cond = rr7::load32(kOut + 4);
    three_waiters(
        [mutex, cond](uint32_t id) {
            check(sc(102, {mutex, 0}) == rr7::kOk && sc(102, {mutex, 0}) == rr7::kOk, "a waiter locks twice");
            check(sc(107, {cond, 0}) == rr7::kOk, "a waiter's wait");
            record(id);
            check(sc(104, {mutex}) == rr7::kOk && sc(104, {mutex}) == rr7::kOk, "held twice again after the wait");
            check(sc(104, {mutex}) == rr7::kEperm, "and no more");
        },
        [mutex, cond] {
            check(sc(102, {mutex, 0}) == rr7::kOk, "the signaller locks");
            check(sc(108, {cond}) == rr7::kOk, "a signal");
            check(sc(104, {mutex}) == rr7::kOk, "the signaller unlocks");
        },
        true, "a condition's waiters");
    check(sc(106, {cond}) == rr7::kOk, "sys_cond_destroy with nobody waiting");
}

// A wait that times out returns with the mutex all the same, once it is free.
void cond_timeout() {
    sync_set_up();
    rr7::store32(kAttribute, rr7::kSyncFifo);
    rr7::store32(kAttribute + 4, 0x20);
    check(sc(100, {kOut, kAttribute}) == rr7::kOk, "sys_mutex_create");
    const uint32_t mutex = rr7::load32(kOut);
    check(sc(105, {kOut + 4, mutex, kAttribute + 0x40}) == rr7::kOk, "sys_cond_create");
    const uint32_t cond = rr7::load32(kOut + 4);
    std::vector<std::thread> threads;
    const uint64_t waits = rr7::waits_begun();
    threads.push_back(guest(40, 40, [mutex, cond] {
        check(sc(102, {mutex, 0}) == rr7::kOk, "the waiter locks");
        check(sc(107, {cond, 20000}) == rr7::kEtimedout, "a wait that times out");
        record(40);
        check(sc(104, {mutex}) == rr7::kOk, "the waiter owns the mutex after its timeout");
    }));
    until([waits] { return rr7::waits_begun() == waits + 1; }, "the waiter waits");
    check(sc(102, {mutex, 0}) == rr7::kOk, "taken while it waits");
    std::this_thread::sleep_for(60ms);  // (its time runs out: it waits for the mutex now)
    check(recorded() == 0, "no return without the mutex");
    check(sc(104, {mutex}) == rr7::kOk, "let go");
    join(threads);
    expect({40}, "the timed-out waiter returned");
}

void events_order(uint32_t protocol) {
    sync_set_up();
    rr7::store32(kAttribute, protocol);
    rr7::store32(kAttribute + 4, 1);
    check(sc(128, {kOut, kAttribute, 0, 16}) == rr7::kOk, "sys_event_queue_create");
    const uint32_t queue = rr7::load32(kOut);
    std::mutex mutex;
    std::vector<uint64_t> data;
    uint64_t sent = 0;
    three_waiters(
        [&, queue](uint32_t id) {
            const Cpu c = call(system_call(130), {queue, 0, 0});
            check(c.r[3] == rr7::kOk && c.r[4] == 0x55, "a receive");
            {
                const std::lock_guard lock(mutex);
                data.push_back(c.r[5]);
            }
            record(id);
        },
        [&, queue] { check(rr7::send_event(queue, {0x55, ++sent, 0, 0}) == rr7::kOk, "a send"); }, protocol == rr7::kSyncPriority, "an event queue's receivers");
    check(data == std::vector<uint64_t>{1, 2, 3}, "each woken receiver got the next event");
    // With nobody waiting, events are queued, up to the size.
    for (uint64_t i = 0; i < 16; ++i) check(rr7::send_event(queue, {1, i, 0, 0}) == rr7::kOk, "queued");
    check(rr7::send_event(queue, {1, 16, 0, 0}) == rr7::kEbusy, "full");
    for (uint64_t i = 0; i < 16; ++i) check(call(system_call(130), {queue, 0, 0}).r[5] == i, "received in order");
    check(sc(130, {queue, 0, 1000}) == rr7::kEtimedout, "a receive that times out");
}

void events_cancel() {
    sync_set_up();
    rr7::store32(kAttribute, rr7::kSyncFifo);
    rr7::store32(kAttribute + 4, 1);
    check(sc(128, {kOut, kAttribute, 0, 4}) == rr7::kOk, "sys_event_queue_create");
    const uint32_t queue = rr7::load32(kOut);
    std::vector<std::thread> threads;
    const uint64_t waits = rr7::waits_begun();
    for (uint32_t i = 0; i < 2; ++i)
        threads.push_back(guest(50 + i, 50, [queue] {
            check(sc(130, {queue, 0, 0}) == rr7::kEcanceled, "woken as cancelled");
            record(1);
        }));
    until([waits] { return rr7::waits_begun() == waits + 2; }, "the receivers wait");
    check(sc(129, {queue, 0}) == rr7::kOk, "sys_event_queue_destroy");
    join(threads);
    check(recorded() == 2, "both receivers woken");
    check(sc(130, {queue, 0, 0}) == rr7::kEsrch, "the queue is gone");
}

// ---------------------------------------------------------------- no deadlock

// Twelve threads of four priorities on two tokens, with no lending (a token kept by a blocked thread would hang this):
// semaphores, a mutex, an event queue, sleeps and yields, every call followed by its switch point.
void stress() {
    rr7::configure_scheduler(2, false, false);
    rr7::store32(kAttribute, rr7::kSyncPriority);
    rr7::store32(kAttribute + 4, 0x20);
    check(sc(100, {kOut, kAttribute}) == rr7::kOk, "sys_mutex_create");
    const uint32_t mutex = rr7::load32(kOut);
    rr7::store32(kAttribute + 0x10, rr7::kSyncFifo);
    check(sc(90, {kOut + 4, kAttribute + 0x10, 3, 3}) == rr7::kOk, "sys_semaphore_create");
    const uint32_t semaphore = rr7::load32(kOut + 4);
    rr7::store32(kAttribute + 0x20, rr7::kSyncPriority);
    rr7::store32(kAttribute + 0x24, 1);
    check(sc(128, {kOut + 8, kAttribute + 0x20, 0, 127}) == rr7::kOk, "sys_event_queue_create");
    const uint32_t queue = rr7::load32(kOut + 8);
    std::atomic<int> running{0};
    uint64_t counter = 0;  // (under the guest mutex)
    constexpr int kThreads = 12, kRounds = 400;
    const auto guest_code = [&] {
        check(rr7::holds_core(), "back in guest code without a token");
        check(++running <= 2, "more than two threads in guest code");
        spin_for(1us);
        --running;
    };
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < kThreads; ++i)
        threads.push_back(guest(400 + i, static_cast<int32_t>(i % 4) * 100, [&, i] {
            for (int n = 0; n < kRounds; ++n) {
                guest_code();
                check(sc(92, {semaphore, 0}) == rr7::kOk, "semaphore wait");
                guest_code();
                check(sc(102, {mutex, 0}) == rr7::kOk, "mutex lock");
                guest_code();
                ++counter;
                check(sc(104, {mutex}) == rr7::kOk, "mutex unlock");
                guest_code();
                check(sc(94, {semaphore, 1}) == rr7::kOk, "semaphore post");
                guest_code();
                if ((n + static_cast<int>(i)) % 5 == 0) {
                    // (Sent before received: there is always an event to come for every receive.)
                    check(rr7::send_event(queue, {i, static_cast<uint64_t>(n), 0, 0}) == rr7::kOk, "send");
                    guest_code();
                    check(sc(130, {queue, 0, 0}) == rr7::kOk, "receive");
                    guest_code();
                }
                if (n % 7 == 0) check(sc(141, {50}) == rr7::kOk, "usleep");
                guest_code();
                rr7::yield_core();
                guest_code();
            }
        }));
    join(threads);
    check(counter == uint64_t{kThreads} * kRounds, "every round counted");
    // A relay: each thread waits for the one before it to post its semaphore, so that at every moment the one thread that
    // can go on is one that waits for a token (none would get one if a waiting thread kept its token).
    std::vector<uint32_t> relay;
    for (uint32_t i = 0; i < kThreads; ++i) {
        check(sc(90, {kOut + 0x20 + 4 * i, kAttribute + 0x10, 0, 1}) == rr7::kOk, "a relay semaphore");
        relay.push_back(rr7::load32(kOut + 0x20 + 4 * i));
    }
    std::atomic<int> passes{0};
    for (uint32_t i = 0; i < kThreads; ++i)
        threads.push_back(guest(500 + i, static_cast<int32_t>(i % 3) * 7, [&, i] {
            for (int n = 0; n < 100; ++n) {
                if (i != 0 || n != 0) check(sc(92, {relay[i], 0}) == rr7::kOk, "the relay's wait");
                guest_code();
                ++passes;
                check(sc(94, {relay[(i + 1) % kThreads], 1}) == rr7::kOk, "the relay's post");
                guest_code();
            }
        }));
    join(threads);
    check(passes.load() == kThreads * 100, "the relay went round");
    check(rr7::free_core_tokens() == 2 && rr7::threads_waiting_for_core() == 0 && rr7::lent_tokens() == 0, "both tokens back, none lent");
}

}  // namespace

namespace rr7 {
uint8_t* guest_memory() { return memory.data(); }
uint32_t load32(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, memory.data() + address, 4);
    return std::byteswap(value);
}
uint64_t load64(uint32_t address) {
    uint64_t value;
    std::memcpy(&value, memory.data() + address, 8);
    return std::byteswap(value);
}
void store32(uint32_t address, uint32_t value) {
    value = std::byteswap(value);
    std::memcpy(memory.data() + address, &value, 4);
}
void store64(uint32_t address, uint64_t value) {
    value = std::byteswap(value);
    std::memcpy(memory.data() + address, &value, 8);
}
uint32_t current_thread() { return thread_id; }
}  // namespace rr7

namespace qilin::rt {
void log(const char*, ...) {}
[[noreturn]] void fatal(const char* format, ...) {
    std::fprintf(stderr, "unexpected fatal: %s\n", format);
    std::_Exit(1);
}
}  // namespace qilin::rt

int main(int argc, char** argv) {
    check(argc == 2, "name a case");
    // (A hang fails the case, rather than waiting for ctest's timeout.)
    std::thread([] {
        std::this_thread::sleep_for(25s);
        std::fprintf(stderr, "FAIL: a deadlock (the case did not end in 25 seconds)\n");
        std::_Exit(1);
    }).detach();
    const std::string name = argv[1];
    if (name == "fast") fast();
    else if (name == "priority") priority();
    else if (name == "two-tokens") two_tokens();
    else if (name == "switch-point") switch_point();
    else if (name == "yield") yield();
    else if (name == "blocking") blocking();
    else if (name == "sleep") sleeping();
    else if (name == "lend") lend();
    else if (name == "free") free_threads();
    else if (name == "mutex-fifo") mutex_order(rr7::kSyncFifo);
    else if (name == "mutex-priority") mutex_order(rr7::kSyncPriority);
    else if (name == "semaphore-fifo") semaphore_order(rr7::kSyncFifo);
    else if (name == "semaphore-priority") semaphore_order(rr7::kSyncPriority);
    else if (name == "lwmutex-fifo") lwmutex_order(rr7::kSyncFifo);
    else if (name == "lwmutex-priority") lwmutex_order(rr7::kSyncPriority);
    else if (name == "cond-priority") cond_priority();
    else if (name == "cond-timeout") cond_timeout();
    else if (name == "events-fifo") events_order(rr7::kSyncFifo);
    else if (name == "events-priority") events_order(rr7::kSyncPriority);
    else if (name == "events-cancel") events_cancel();
    else if (name == "stress") stress();
    else check(false, "no such case");
    std::printf("scheduler case %s passed\n", name.c_str());
    return 0;
}

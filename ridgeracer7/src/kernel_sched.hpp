// The scheduler: which guest PPU threads may run guest code, and the order
// threads waiting on the system's objects are woken in.
//
// The PS3's PPU runs two hardware threads at once. Here every guest thread
// is a host thread, and the two hardware threads are two **core tokens**: a
// PPU thread runs guest code only while it holds one. A thread gives its
// token up whenever it blocks (a wait on a mutex, condition, semaphore,
// lightweight mutex or event queue, a sleep, a join, and the native calls
// that block: file reads and writes, a join of SPU work, ...), and takes one
// back as it wakes. Tokens are handed out by priority (lower numbers first,
// as on the console), the longest ready first among equals.
//
// A thread of better priority waiting for a token takes one from a running
// thread at that thread's next return from a call into the system (as the
// console would preempt it at once; not inside a handler that has called the
// game back, which may hold a lock of its own). One that outranks a thread holding a token runs at
// once, on a token lent until the next one is given back (one as good as the
// worst holder after kPatience, 1 ms; one worse than every holder after 50 ms): guest code between system calls can't be
// interrupted here, and a thread polling memory in a loop would otherwise
// hold its token for ever. That stands in for the console's preemption; it is
// counted and the first few are logged.
//
// The common case costs a compare-and-swap on one word: taking a token when
// one is free and nobody waits, giving one back when nobody waits. Only a
// thread that must wait, or that hands its token to a waiter, takes the
// scheduler's lock. The lock is never held while anything else is waited for,
// and no other lock is ever taken under it.
//
// RR7_FREE_THREADS=1 in the environment turns the tokens off: every guest
// thread then runs whenever the host lets it, as before the scheduler.
// The objects' waking orders stay as they are.
//
// Runtime code: no std::format.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rr7 {

inline constexpr int kCoreTokens = 2;

// Sets the scheduler up: how many tokens, whether they are used at all, and whether a token may be lent (tests; the
// machine leaves it to the first use, which reads RR7_FREE_THREADS). Only while no thread is scheduled.
void configure_scheduler(int tokens, bool free_threads, bool lend = true);
bool threads_run_freely();

// A guest thread the scheduler knows. Made by its creator (so that its priority can be changed before it starts), then
// bound to the host thread that runs it.
struct SchedThread;
SchedThread* new_sched_thread(uint32_t id, int32_t priority);
// The calling host thread is that guest thread from now on. It holds no token yet.
void bind_sched_thread(SchedThread* thread);
// The calling host thread's guest thread ends: its token (if it holds one) is given back and it is forgotten.
void end_sched_thread();

// Takes a token for the calling thread, waiting for one: no host lock may be held. Nothing for a thread that holds one
// already, or that is not a scheduled guest thread (the runtime's own host threads), or with free threads.
void take_core();
// Gives the calling thread's token back, if it holds one. Never waits: it may be called with a lock held.
void give_up_core();
bool holds_core();

// A call into the system returns: if a thread of better priority waits for a token, this one gives its own up to it
// and waits for another (cheap when none does: two loads).
void switch_point();
// sys_ppu_thread_yield: the token goes to a waiting thread of the same or better priority, if any, and this one waits
// for one again behind it.
void yield_core();

// A thread's priority (by guest id: it may be another thread), and the calling thread's (kHostPriority for a host
// thread of the runtime's).
void set_sched_priority(uint32_t id, int32_t priority);
inline constexpr int32_t kHostPriority = 0;
int32_t current_priority();

// How many times a thread ran on a lent token (see the top); the tokens free now (below 0 while some are lent), the
// threads waiting for one, and how many waits on objects have begun (for tests).
uint64_t lent_tokens();
int32_t free_core_tokens();
uint32_t threads_waiting_for_core();
uint64_t waits_begun();

// A wait that may block: declared before the lock the wait is under, released (release()) just before waiting, so
// that a call that doesn't wait keeps its token; the token is taken back by the destructor, after the lock is gone.
class Blocking {
public:
    Blocking() = default;
    Blocking(const Blocking&) = delete;
    Blocking& operator=(const Blocking&) = delete;
    ~Blocking() {
        if (released_) take_core();
    }
    void release() {
        if (!released_ && holds_core()) {
            give_up_core();
            released_ = true;
        }
    }

private:
    bool released_ = false;
};

// For a native call that blocks from start to end (a file read, a join): the token is given up for its whole life.
// Declare it before any lock the call takes, so that the token is taken back after the lock is released.
class ScopedBlocking : public Blocking {
public:
    ScopedBlocking() { release(); }
};

// ---- the order the system wakes waiters in ----

// SYS_SYNC_FIFO (1), SYS_SYNC_PRIORITY (2), SYS_SYNC_PRIORITY_INHERIT (3: woken by priority; *the inheritance itself is
// not modelled*), SYS_SYNC_RETRY (4, a lightweight mutex's: woken first come first, *a stated reading*).
enum class WakeOrder : uint8_t { Fifo, Priority };
inline constexpr uint32_t kSyncFifo = 1, kSyncPriority = 2, kSyncPriorityInherit = 3, kSyncRetry = 4;
// The order for a protocol, or false if it is none of those.
bool wake_order(uint32_t protocol, WakeOrder& order);

// A thread waiting on an object, on its own stack, under the object's lock. Whoever wakes it sets `woken` and notifies
// it, under the same lock.
struct Waiter {
    std::condition_variable cv;
    uint32_t thread = 0;                     // the guest thread waiting (the caller sets it)
    int32_t priority = current_priority();  // the waiting thread's, as it began to wait
    uint64_t seq = 0;
    bool woken = false;
    uint64_t status = 0;    // what the waker says (0, or the system's code for "cancelled")
    uint64_t data[4] = {};  // what it hands over (an event; a recursion count)

    // Waits under `lock` until woken, or for `timeout` microseconds if that isn't 0. False if the time ran out first.
    bool wait(std::unique_lock<std::mutex>& lock, uint64_t timeout);
    void wake() {
        woken = true;
        cv.notify_one();
    }
};

class WaitQueue {
public:
    explicit WaitQueue(WakeOrder order = WakeOrder::Fifo) : order_(order) {}
    WakeOrder order() const { return order_; }
    // `w` joins the queue, last of those waiting (not yet woken).
    void push(Waiter& w);
    // The next to wake (by the queue's order), taken off the queue; null if none waits.
    Waiter* pop();
    // Takes `w` off the queue (a wait that timed out); false if it isn't on it.
    bool remove(Waiter& w);
    bool empty() const { return waiters_.empty(); }
    size_t size() const { return waiters_.size(); }

private:
    std::vector<Waiter*> waiters_;
    uint64_t seq_ = 0;
    WakeOrder order_;
};

}  // namespace rr7

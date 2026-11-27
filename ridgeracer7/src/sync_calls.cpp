// Synchronisation by system call: mutexes (100-104), condition variables
// (105-108), semaphores (90-94, 114); and sleeping and the clock (141, 142,
// 145, 147).
//
// Each object is a host object found by its id, with its waiters in the
// order the system wakes them: first come first, or by priority (the
// longest waiting among equals), as the object's attribute says
// (kernel_sched.hpp). What is let go is handed straight to the waiter woken:
// a mutex's ownership, a semaphore's count. So a thread that lets go and
// takes again at once doesn't take back from one already waiting (the game's
// threads greet each other by posting a semaphore and then waiting on it, and
// a post the poster could take back itself left the game reading a null
// pointer). A thread gives its core token up only when it really waits.
#include <chrono>
#include <condition_variable>
#include <thread>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

constexpr uint32_t kRecursive = 0x10, kNotRecursive = 0x20;

// The waking order an object's attribute asks for (SYS_SYNC_FIFO, _PRIORITY or _PRIORITY_INHERIT); anything else stops
// the game, naming it.
WakeOrder order_for(uint32_t protocol, const char* what) {
    WakeOrder order{};
    if (protocol == kSyncRetry || !wake_order(protocol, order)) qilin::rt::fatal("%s with protocol %u, which this runtime doesn't know", what, protocol);
    return order;
}

// ---------------------------------------------------------------- mutexes

struct Mutex {
    std::mutex mutex;
    WaitQueue waiters;  // each waiter's data[0]: how many times it will hold it
    uint32_t owner = 0, count = 0;
    bool recursive = false;
    uint32_t conditions = 0;  // condition variables made on it
};
ObjectTable<Mutex> g_mutexes(0x85000001u);

// Lets go of the mutex (its lock held): the next waiter owns it now, if one waits. (So a mutex with no owner has no
// waiters.)
void hand_on(Mutex& m) {
    if (Waiter* const w = m.waiters.pop()) {
        m.owner = w->thread;
        m.count = static_cast<uint32_t>(w->data[0]);
        w->wake();
    } else {
        m.owner = 0;
        m.count = 0;
    }
}

// sys_mutex_create(sys_mutex_t* id, const sys_mutex_attribute_t* attribute)
// The attribute begins with the waking order and whether it is recursive.
void sys_mutex_create(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]), attribute = static_cast<uint32_t>(c.r[4]);
    if (!out || !attribute) {
        c.r[3] = kEfault;
        return;
    }
    const uint32_t recursive = load32(attribute + 4);
    if (recursive != kRecursive && recursive != kNotRecursive) {
        c.r[3] = kEinval;
        return;
    }
    auto object = std::make_shared<Mutex>();
    object->waiters = WaitQueue(order_for(load32(attribute), "sys_mutex_create"));
    object->recursive = recursive == kRecursive;
    store32(out, g_mutexes.add(std::move(object)));
    c.r[3] = kOk;
}

// sys_mutex_destroy(id): busy while it is held or a condition variable is made on it.
void sys_mutex_destroy(Cpu& c) {
    const uint32_t id = static_cast<uint32_t>(c.r[3]);
    const auto object = g_mutexes.find(id);
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    {
        const std::lock_guard lock(object->mutex);
        if (object->owner || object->conditions) {
            c.r[3] = kEbusy;
            return;
        }
    }
    g_mutexes.remove(id);
    c.r[3] = kOk;
}

// sys_mutex_lock(id, timeout in microseconds; 0 waits for ever), sys_mutex_trylock(id)
void mutex_lock(Cpu& c, bool waiting) {
    const auto object = g_mutexes.find(static_cast<uint32_t>(c.r[3]));
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    const uint32_t me = current_thread();
    Blocking blocking;  // (before the lock: the token is taken back once the lock is let go)
    std::unique_lock lock(object->mutex);
    if (object->owner == me) {
        if (!object->recursive) {
            c.r[3] = kEdeadlk;
            return;
        }
        ++object->count;
        c.r[3] = kOk;
        return;
    }
    if (object->owner == 0) {
        object->owner = me;
        object->count = 1;
        c.r[3] = kOk;
        return;
    }
    if (!waiting) {
        c.r[3] = kEbusy;
        return;
    }
    Waiter self;
    self.thread = me;
    self.data[0] = 1;
    object->waiters.push(self);
    blocking.release();
    if (!self.wait(lock, c.r[4])) {
        object->waiters.remove(self);
        c.r[3] = kEtimedout;
        return;
    }
    c.r[3] = kOk;  // (handed over: hand_on made it the owner)
}
void sys_mutex_lock(Cpu& c) { mutex_lock(c, true); }
void sys_mutex_trylock(Cpu& c) { mutex_lock(c, false); }

// sys_mutex_unlock(id): only its owner may.
void sys_mutex_unlock(Cpu& c) {
    const auto object = g_mutexes.find(static_cast<uint32_t>(c.r[3]));
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    const std::lock_guard lock(object->mutex);
    if (object->owner != current_thread()) {
        c.r[3] = kEperm;
        return;
    }
    if (--object->count == 0) hand_on(*object);
    c.r[3] = kOk;
}

// ---------------------------------------------------------------- condition variables

// Waiters are woken in the order of the mutex the condition is made on (*a stated reading*: its attribute has no order
// of its own). A signalled waiter joins the mutex's waiters (or owns it at once, if it is free), and returns once it
// owns it.
struct Condition {
    std::shared_ptr<Mutex> mutex;  // the one it was made on, whose host lock guards these too
    WaitQueue waiters;             // each waiter's data[0]: how many times it held the mutex
};
ObjectTable<Condition> g_conditions(0x86000001u);

// sys_cond_create(sys_cond_t* id, mutex, const sys_cond_attribute_t* attribute)
void sys_cond_create(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]);
    const auto mutex = g_mutexes.find(static_cast<uint32_t>(c.r[4]));
    if (!mutex) {
        c.r[3] = kEsrch;
        return;
    }
    auto object = std::make_shared<Condition>();
    object->mutex = mutex;
    {
        const std::lock_guard lock(mutex->mutex);
        object->waiters = WaitQueue(mutex->waiters.order());
        ++mutex->conditions;
    }
    store32(out, g_conditions.add(std::move(object)));
    c.r[3] = kOk;
}

// sys_cond_destroy(id): busy while anyone waits on it.
void sys_cond_destroy(Cpu& c) {
    const uint32_t id = static_cast<uint32_t>(c.r[3]);
    const auto object = g_conditions.find(id);
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    {
        const std::lock_guard lock(object->mutex->mutex);
        if (!object->waiters.empty()) {
            c.r[3] = kEbusy;
            return;
        }
        --object->mutex->conditions;
    }
    g_conditions.remove(id);
    c.r[3] = kOk;
}

// sys_cond_wait(id, timeout): the caller holds the mutex; it is released while waiting and held again on return, as often as before.
void sys_cond_wait(Cpu& c) {
    const auto object = g_conditions.find(static_cast<uint32_t>(c.r[3]));
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    Mutex& mutex = *object->mutex;
    const uint32_t me = current_thread();
    Blocking blocking;
    std::unique_lock lock(mutex.mutex);
    if (mutex.owner != me) {
        c.r[3] = kEperm;
        return;
    }
    Waiter self;
    self.thread = me;
    self.data[0] = mutex.count;
    hand_on(mutex);
    object->waiters.push(self);
    blocking.release();
    bool signalled = true;
    if (!self.wait(lock, c.r[4]) && object->waiters.remove(self)) {
        // The time ran out before a signal: the mutex is to be had again all the same.
        signalled = false;
        if (mutex.owner == 0) {
            mutex.owner = me;
            mutex.count = static_cast<uint32_t>(self.data[0]);
            self.woken = true;
        } else {
            mutex.waiters.push(self);
        }
    }
    self.wait(lock, 0);  // (until the mutex is handed over, if it hasn't been)
    c.r[3] = signalled ? kOk : kEtimedout;
}

// sys_cond_signal(id): wakes one waiter, if there is one.
void sys_cond_signal(Cpu& c) {
    const auto object = g_conditions.find(static_cast<uint32_t>(c.r[3]));
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    Mutex& mutex = *object->mutex;
    const std::lock_guard lock(mutex.mutex);
    if (Waiter* const w = object->waiters.pop()) {
        if (mutex.owner == 0) {
            mutex.owner = w->thread;
            mutex.count = static_cast<uint32_t>(w->data[0]);
            w->wake();
        } else {
            mutex.waiters.push(*w);
        }
    }
    c.r[3] = kOk;
}

// ---------------------------------------------------------------- semaphores

struct Semaphore {
    std::mutex mutex;
    WaitQueue waiters;
    int32_t value = 0, maximum = 0;  // (a count left over means nobody waits: a post goes to a waiter first)
};
ObjectTable<Semaphore> g_semaphores(0x96000001u);

// sys_semaphore_create(sys_semaphore_t* id, const sys_semaphore_attribute_t* attribute (the waking order first),
//                      initial value, maximum value)
void sys_semaphore_create(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]), attribute = static_cast<uint32_t>(c.r[4]);
    const int32_t initial = static_cast<int32_t>(c.r[5]), maximum = static_cast<int32_t>(c.r[6]);
    if (!out || !attribute) {
        c.r[3] = kEfault;
        return;
    }
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        c.r[3] = kEinval;
        return;
    }
    auto object = std::make_shared<Semaphore>();
    object->waiters = WaitQueue(order_for(load32(attribute), "sys_semaphore_create"));
    object->value = initial;
    object->maximum = maximum;
    store32(out, g_semaphores.add(std::move(object)));
    c.r[3] = kOk;
}

// sys_semaphore_destroy(id): busy while anyone waits on it.
void sys_semaphore_destroy(Cpu& c) {
    const uint32_t id = static_cast<uint32_t>(c.r[3]);
    const auto object = g_semaphores.find(id);
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    {
        const std::lock_guard lock(object->mutex);
        if (!object->waiters.empty()) {
            c.r[3] = kEbusy;
            return;
        }
    }
    g_semaphores.remove(id);
    c.r[3] = kOk;
}

// sys_semaphore_wait(id, timeout), sys_semaphore_trywait(id)
void semaphore_wait(Cpu& c, bool waiting) {
    const auto object = g_semaphores.find(static_cast<uint32_t>(c.r[3]));
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    Blocking blocking;
    std::unique_lock lock(object->mutex);
    if (object->value > 0) {
        --object->value;
        c.r[3] = kOk;
        return;
    }
    if (!waiting) {
        c.r[3] = kEbusy;
        return;
    }
    Waiter self;
    self.thread = current_thread();
    object->waiters.push(self);
    blocking.release();
    if (!self.wait(lock, c.r[4])) {
        object->waiters.remove(self);  // (not woken: so still queued)
        c.r[3] = kEtimedout;
        return;
    }
    c.r[3] = kOk;
}
void sys_semaphore_wait(Cpu& c) { semaphore_wait(c, true); }
void sys_semaphore_trywait(Cpu& c) { semaphore_wait(c, false); }

// sys_semaphore_post(id, count): busy if that would pass the maximum.
void sys_semaphore_post(Cpu& c) {
    const auto object = g_semaphores.find(static_cast<uint32_t>(c.r[3]));
    int32_t count = static_cast<int32_t>(c.r[4]);
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    if (count < 0) {
        c.r[3] = kEinval;
        return;
    }
    const std::lock_guard lock(object->mutex);
    if (count > object->maximum - object->value + static_cast<int32_t>(object->waiters.size())) {
        c.r[3] = kEbusy;
        return;
    }
    for (; count > 0; --count) {
        Waiter* const w = object->waiters.pop();
        if (!w) break;
        w->wake();
    }
    object->value += count;
    c.r[3] = kOk;
}

// sys_semaphore_get_value(id, int* value)
void sys_semaphore_get_value(Cpu& c) {
    const auto object = g_semaphores.find(static_cast<uint32_t>(c.r[3]));
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    const std::lock_guard lock(object->mutex);
    store32(static_cast<uint32_t>(c.r[4]), static_cast<uint32_t>(object->value));
    c.r[3] = kOk;
}

// ---------------------------------------------------------------- sleeping, and the clock

// sys_timer_usleep(microseconds), sys_timer_sleep(seconds): the core token is given up while sleeping.
void sys_timer_usleep(Cpu& c) {
    const ScopedBlocking blocking;
    std::this_thread::sleep_for(std::chrono::microseconds(c.r[3]));
    c.r[3] = kOk;
}
void sys_timer_sleep(Cpu& c) {
    const ScopedBlocking blocking;
    std::this_thread::sleep_for(std::chrono::seconds(static_cast<uint32_t>(c.r[3])));
    c.r[3] = kOk;
}

// sys_time_get_current_time(seconds*, nanoseconds*): the calendar's time, as two 64-bit numbers.
void sys_time_get_current_time(Cpu& c) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    store64(static_cast<uint32_t>(c.r[3]), static_cast<uint64_t>(seconds.count()));
    store64(static_cast<uint32_t>(c.r[4]), static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count()));
    c.r[3] = kOk;
}

// sys_time_get_timebase_frequency(): what the time base (mftb) counts in a second.
void sys_time_get_timebase_frequency(Cpu& c) { c.r[3] = 79800000; }

constexpr NativeSystemCall kCalls[] = {
    {90, sys_semaphore_create},  {91, sys_semaphore_destroy}, {92, sys_semaphore_wait}, {93, sys_semaphore_trywait}, {94, sys_semaphore_post},
    {100, sys_mutex_create},     {101, sys_mutex_destroy},    {102, sys_mutex_lock},    {103, sys_mutex_trylock},    {104, sys_mutex_unlock},
    {105, sys_cond_create},      {106, sys_cond_destroy},     {107, sys_cond_wait},     {108, sys_cond_signal},      {114, sys_semaphore_get_value},
    {141, sys_timer_usleep},     {142, sys_timer_sleep},      {145, sys_time_get_current_time},
    {147, sys_time_get_timebase_frequency},
};

}  // namespace

std::span<const NativeSystemCall> sync_system_calls() { return kCalls; }

}  // namespace rr7

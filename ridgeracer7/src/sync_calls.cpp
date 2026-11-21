// Synchronisation by system call: mutexes (100-104), condition variables
// (105-108), semaphores (90-94, 114); and sleeping and the clock (141, 142,
// 145, 147).
//
// Each object is a host object found by its id. Waiters are woken in the
// host's order: the system's own orders (first come, or by priority) are the
// scheduler's business, which is to come; until then nothing here promises
// one.
#include <chrono>
#include <condition_variable>
#include <thread>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

constexpr uint32_t kRecursive = 0x10, kNotRecursive = 0x20;

// Waits on `cv` until `ready`, or for `timeout` microseconds if it isn't 0. False if the time ran out.
template <typename Ready>
bool wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, uint64_t timeout, Ready ready) {
    if (timeout == 0) {
        cv.wait(lock, ready);
        return true;
    }
    return cv.wait_for(lock, std::chrono::microseconds(timeout), ready);
}

// ---------------------------------------------------------------- mutexes

struct Mutex {
    std::mutex mutex;
    std::condition_variable freed;
    uint32_t owner = 0, count = 0;
    bool recursive = false;
    uint32_t conditions = 0;  // condition variables made on it
};
ObjectTable<Mutex> g_mutexes(0x85000001u);

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
    if (object->owner) {
        if (!waiting) {
            c.r[3] = kEbusy;
            return;
        }
        if (!wait(object->freed, lock, c.r[4], [&] { return object->owner == 0; })) {
            c.r[3] = kEtimedout;
            return;
        }
    }
    object->owner = me;
    object->count = 1;
    c.r[3] = kOk;
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
    {
        const std::lock_guard lock(object->mutex);
        if (object->owner != current_thread()) {
            c.r[3] = kEperm;
            return;
        }
        if (--object->count) {
            c.r[3] = kOk;
            return;
        }
        object->owner = 0;
    }
    object->freed.notify_one();
    c.r[3] = kOk;
}

// ---------------------------------------------------------------- condition variables

struct Condition {
    std::shared_ptr<Mutex> mutex;  // the one it was made on, whose host lock guards these too
    std::condition_variable signalled;
    uint32_t waiters = 0, signals = 0;  // signals not yet taken by a waiter
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
        if (object->waiters) {
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
    std::unique_lock lock(mutex.mutex);
    if (mutex.owner != me) {
        c.r[3] = kEperm;
        return;
    }
    const uint32_t held = mutex.count;
    mutex.owner = 0;
    mutex.count = 0;
    mutex.freed.notify_one();
    ++object->waiters;
    const bool signalled = wait(object->signalled, lock, c.r[4], [&] { return object->signals > 0; });
    --object->waiters;
    if (signalled) --object->signals;
    mutex.freed.wait(lock, [&] { return mutex.owner == 0; });
    mutex.owner = me;
    mutex.count = held;
    c.r[3] = signalled ? kOk : kEtimedout;
}

// sys_cond_signal(id): wakes one waiter, if there is one.
void sys_cond_signal(Cpu& c) {
    const auto object = g_conditions.find(static_cast<uint32_t>(c.r[3]));
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    {
        const std::lock_guard lock(object->mutex->mutex);
        if (object->signals < object->waiters) ++object->signals;
    }
    object->signalled.notify_all();  // (all: the one that takes the signal is whichever sees it first)
    c.r[3] = kOk;
}

// ---------------------------------------------------------------- semaphores

struct Semaphore {
    std::mutex mutex;
    std::condition_variable posted;
    int32_t value = 0, maximum = 0;
};
ObjectTable<Semaphore> g_semaphores(0x96000001u);

// sys_semaphore_create(sys_semaphore_t* id, attribute, initial value, maximum value)
void sys_semaphore_create(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]);
    const int32_t initial = static_cast<int32_t>(c.r[5]), maximum = static_cast<int32_t>(c.r[6]);
    if (!out || !c.r[4]) {
        c.r[3] = kEfault;
        return;
    }
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        c.r[3] = kEinval;
        return;
    }
    auto object = std::make_shared<Semaphore>();
    object->value = initial;
    object->maximum = maximum;
    store32(out, g_semaphores.add(std::move(object)));
    c.r[3] = kOk;
}

// sys_semaphore_destroy(id)
void sys_semaphore_destroy(Cpu& c) { c.r[3] = g_semaphores.remove(static_cast<uint32_t>(c.r[3])) ? kOk : kEsrch; }

// sys_semaphore_wait(id, timeout), sys_semaphore_trywait(id)
void semaphore_wait(Cpu& c, bool waiting) {
    const auto object = g_semaphores.find(static_cast<uint32_t>(c.r[3]));
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    std::unique_lock lock(object->mutex);
    if (object->value <= 0) {
        if (!waiting) {
            c.r[3] = kEbusy;
            return;
        }
        if (!wait(object->posted, lock, c.r[4], [&] { return object->value > 0; })) {
            c.r[3] = kEtimedout;
            return;
        }
    }
    --object->value;
    c.r[3] = kOk;
}
void sys_semaphore_wait(Cpu& c) { semaphore_wait(c, true); }
void sys_semaphore_trywait(Cpu& c) { semaphore_wait(c, false); }

// sys_semaphore_post(id, count): busy if that would pass the maximum.
void sys_semaphore_post(Cpu& c) {
    const auto object = g_semaphores.find(static_cast<uint32_t>(c.r[3]));
    const int32_t count = static_cast<int32_t>(c.r[4]);
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    if (count < 0) {
        c.r[3] = kEinval;
        return;
    }
    {
        const std::lock_guard lock(object->mutex);
        if (count > object->maximum - object->value) {
            c.r[3] = kEbusy;
            return;
        }
        object->value += count;
    }
    object->posted.notify_all();
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

// sys_timer_usleep(microseconds), sys_timer_sleep(seconds)
void sys_timer_usleep(Cpu& c) {
    std::this_thread::sleep_for(std::chrono::microseconds(c.r[3]));
    c.r[3] = kOk;
}
void sys_timer_sleep(Cpu& c) {
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

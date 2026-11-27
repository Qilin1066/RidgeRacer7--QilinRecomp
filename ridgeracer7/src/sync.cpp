// Synchronisation: the lightweight mutex (sysPrxForUser's sys_lwmutex_*).
//
// The guest holds a 24-byte structure for each (sys_lwmutex_t, from the
// SDK's public headers): the owner's thread id and a count of waiters, the
// attributes, the recursion count, and the id of the system object behind
// it. On a console the library takes the lock by an atomic exchange on the
// structure itself and only goes to the system when it must wait. Here the
// lock is a host object found by the id in the structure; the owner and
// the recursion count are written back to the structure as the library
// would leave them, in case anything reads them. Waiters are woken in the
// attribute's order (first come, or by priority: kernel_sched.hpp), and
// the one woken is handed the mutex.

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

// The structure's fields, and what the library puts in them.
constexpr uint32_t kOwner = 0, kWaiter = 4, kAttribute = 8, kRecursiveCount = 12, kSleepQueue = 16;
constexpr uint32_t kFree = 0xFFFFFFFFu;                                 // no owner
constexpr uint32_t kRecursive = 0x10, kNotRecursive = 0x20;             // the attribute's recursion half
constexpr uint32_t kFifo = 1, kPriority = 2, kRetry = 4;               // ... and its order of waking (3, priority inheritance, is a mutex's)

struct LwMutex {
    std::mutex mutex;
    WaitQueue waiters;
    uint32_t owner = 0;  // a thread id, or 0
    uint32_t count = 0;  // how many times the owner has it
    bool recursive = false;
};
ObjectTable<LwMutex> g_lwmutexes(0x95000001u);

// sys_lwmutex_create(sys_lwmutex_t* lwmutex, const sys_lwmutex_attribute_t* attribute)
// The attribute: the order waiters are woken in, whether it is recursive, a name of eight bytes.
void sys_lwmutex_create(Cpu& c) {
    const uint32_t lwmutex = static_cast<uint32_t>(c.r[3]), attribute = static_cast<uint32_t>(c.r[4]);
    if (!lwmutex || !attribute) {
        c.r[3] = kEfault;
        return;
    }
    const uint32_t protocol = load32(attribute), recursive = load32(attribute + 4);
    // (Priority inheritance is not something a lightweight mutex has.)
    if ((recursive != kRecursive && recursive != kNotRecursive) || (protocol != kFifo && protocol != kPriority && protocol != kRetry)) {
        c.r[3] = kEinval;
        return;
    }
    auto object = std::make_shared<LwMutex>();
    WakeOrder order{};
    wake_order(protocol, order);  // (a lightweight mutex's SYS_SYNC_RETRY is first come, first woken: a stated reading)
    object->waiters = WaitQueue(order);
    object->recursive = recursive == kRecursive;
    const uint32_t id = g_lwmutexes.add(std::move(object));
    store32(lwmutex + kOwner, kFree);
    store32(lwmutex + kWaiter, 0);
    store32(lwmutex + kAttribute, protocol | recursive);
    store32(lwmutex + kRecursiveCount, 0);
    store32(lwmutex + kSleepQueue, id);
    c.r[3] = kOk;
}

// sys_lwmutex_destroy(sys_lwmutex_t* lwmutex): busy if someone holds it.
void sys_lwmutex_destroy(Cpu& c) {
    const uint32_t lwmutex = static_cast<uint32_t>(c.r[3]);
    const uint32_t id = load32(lwmutex + kSleepQueue);
    const auto object = g_lwmutexes.find(id);
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    {
        const std::lock_guard lock(object->mutex);
        if (object->owner) {
            c.r[3] = kEbusy;
            return;
        }
    }
    g_lwmutexes.remove(id);
    c.r[3] = kOk;
}

// sys_lwmutex_lock(sys_lwmutex_t* lwmutex, timeout in microseconds; 0 waits for ever)
// sys_lwmutex_trylock(sys_lwmutex_t* lwmutex)
void lock(Cpu& c, bool wait) {
    const uint32_t lwmutex = static_cast<uint32_t>(c.r[3]);
    const uint64_t timeout = wait ? c.r[4] : 0;
    const auto object = g_lwmutexes.find(load32(lwmutex + kSleepQueue));
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
        store32(lwmutex + kRecursiveCount, ++object->count);
        c.r[3] = kOk;
        return;
    }
    if (object->owner) {
        if (!wait) {
            c.r[3] = kEbusy;
            return;
        }
        Waiter self;
        self.thread = me;
        object->waiters.push(self);
        blocking.release();
        if (!self.wait(lock, timeout)) {
            object->waiters.remove(self);
            c.r[3] = kEtimedout;
            return;
        }
        c.r[3] = kOk;  // (handed over by the unlock, which wrote the structure)
        return;
    }
    object->owner = me;
    object->count = 1;
    store32(lwmutex + kOwner, me);
    store32(lwmutex + kRecursiveCount, 1);
    c.r[3] = kOk;
}
void sys_lwmutex_lock(Cpu& c) { lock(c, true); }
void sys_lwmutex_trylock(Cpu& c) { lock(c, false); }

// sys_lwmutex_unlock(sys_lwmutex_t* lwmutex): only its owner may.
void sys_lwmutex_unlock(Cpu& c) {
    const uint32_t lwmutex = static_cast<uint32_t>(c.r[3]);
    const auto object = g_lwmutexes.find(load32(lwmutex + kSleepQueue));
    if (!object) {
        c.r[3] = kEsrch;
        return;
    }
    const std::lock_guard lock(object->mutex);
    if (object->owner != current_thread()) {
        c.r[3] = kEperm;
        return;
    }
    if (--object->count) {
        store32(lwmutex + kRecursiveCount, object->count);
        c.r[3] = kOk;
        return;
    }
    if (Waiter* const w = object->waiters.pop()) {  // handed to the next waiter
        object->owner = w->thread;
        object->count = 1;
        store32(lwmutex + kOwner, w->thread);
        store32(lwmutex + kRecursiveCount, 1);
        w->wake();
    } else {
        object->owner = 0;
        store32(lwmutex + kOwner, kFree);
        store32(lwmutex + kRecursiveCount, 0);
    }
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"sysPrxForUser", "sys_lwmutex_create", sys_lwmutex_create}, {"sysPrxForUser", "sys_lwmutex_destroy", sys_lwmutex_destroy},
    {"sysPrxForUser", "sys_lwmutex_lock", sys_lwmutex_lock},     {"sysPrxForUser", "sys_lwmutex_trylock", sys_lwmutex_trylock},
    {"sysPrxForUser", "sys_lwmutex_unlock", sys_lwmutex_unlock},
};

}  // namespace

std::span<const NativeImport> sync_imports() { return kImports; }

}  // namespace rr7

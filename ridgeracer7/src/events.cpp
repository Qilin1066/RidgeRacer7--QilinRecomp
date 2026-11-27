// Event queues and ports (system calls 128-130 and 134-138): how threads,
// timers, the sound system and the SPUs tell a thread that something
// happened.
//
// An event is four 64-bit numbers: where it came from, and three of data.
// A queue holds up to as many as it was made for (127 at most); a thread
// receives from it, waiting if it is empty. A port, once connected to a
// queue, is something to send from. The receive call returns the event in
// registers r4 to r7, not through its pointer: the game's own code stores
// them (read off the code after each `sc`).
//
// Threads waiting to receive are woken in the queue's order (first come, or
// by priority: kernel_sched.hpp), and an event sent while one waits goes
// straight to the one woken.
#include "events.hpp"

#include <deque>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

struct Queue {
    std::mutex mutex;
    WaitQueue receivers;      // (while any waits, no event is queued: each goes to one of them)
    std::deque<Event> events;
    size_t capacity = 0;
    uint64_t key = 0;
    bool destroyed = false;
};
ObjectTable<Queue> g_queues(0x8D000001u);
std::mutex g_keys_mutex;
std::map<uint64_t, uint32_t> g_keys;  // a queue's key (if it has one) -> its id

struct Port {
    std::mutex mutex;
    uint64_t name = 0;
    uint32_t queue = 0;  // the queue it is connected to, or 0
};
ObjectTable<Port> g_ports(0x0E000001u);

// Takes the next event from a queue, waiting for one (for `timeout` microseconds if that isn't 0).
uint64_t receive(Queue& queue, Event& event, uint64_t timeout) {
    Blocking blocking;  // (before the lock: the token is taken back once the lock is let go)
    std::unique_lock lock(queue.mutex);
    if (!queue.events.empty()) {
        event = queue.events.front();
        queue.events.pop_front();
        return kOk;
    }
    if (queue.destroyed) return kEcanceled;
    Waiter self;
    self.thread = current_thread();
    queue.receivers.push(self);
    blocking.release();
    if (!self.wait(lock, timeout)) {
        queue.receivers.remove(self);
        return kEtimedout;
    }
    if (self.status != kOk) return self.status;
    event = {self.data[0], self.data[1], self.data[2], self.data[3]};
    return kOk;
}

// Marks a queue destroyed: its waiting threads are woken with "cancelled".
void cancel(Queue& queue) {
    const std::lock_guard lock(queue.mutex);
    queue.destroyed = true;
    while (Waiter* const w = queue.receivers.pop()) {
        w->status = kEcanceled;
        w->wake();
    }
}

// The waking order an event queue's attribute asks for (it begins with it); anything else stops the game, naming it.
WakeOrder queue_order(uint32_t attribute) {
    const uint32_t protocol = load32(attribute);
    if (protocol != kSyncFifo && protocol != kSyncPriority) qilin::rt::fatal("sys_event_queue_create with protocol %u, which this runtime doesn't know", protocol);
    return protocol == kSyncFifo ? WakeOrder::Fifo : WakeOrder::Priority;
}

// sys_event_queue_create(sys_event_queue_t* id, attribute*, key, size)
void sys_event_queue_create(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]);
    const uint64_t key = c.r[5];
    const int32_t size = static_cast<int32_t>(c.r[6]);
    if (!out || !c.r[4]) {
        c.r[3] = kEfault;
        return;
    }
    if (size <= 0 || size > 127) {
        c.r[3] = kEinval;
        return;
    }
    auto queue = std::make_shared<Queue>();
    queue->receivers = WaitQueue(queue_order(static_cast<uint32_t>(c.r[4])));
    queue->capacity = static_cast<size_t>(size);
    queue->key = key;
    const uint32_t id = g_queues.add(std::move(queue));
    if (key) {
        const std::lock_guard lock(g_keys_mutex);
        g_keys[key] = id;
    }
    store32(out, id);
    c.r[3] = kOk;
}

// sys_event_queue_destroy(id, mode): threads waiting on it are woken with "cancelled".
void sys_event_queue_destroy(Cpu& c) {
    const uint32_t id = static_cast<uint32_t>(c.r[3]);
    const auto queue = g_queues.find(id);
    if (!queue) {
        c.r[3] = kEsrch;
        return;
    }
    cancel(*queue);
    if (queue->key) {
        const std::lock_guard lock(g_keys_mutex);
        g_keys.erase(queue->key);
    }
    g_queues.remove(id);
    c.r[3] = kOk;
}

// sys_event_queue_receive(id, sys_event_t* unused, timeout in microseconds; 0 waits for ever): the event, in r4 to r7.
void sys_event_queue_receive(Cpu& c) {
    const auto queue = g_queues.find(static_cast<uint32_t>(c.r[3]));
    const uint64_t timeout = c.r[5];
    if (!queue) {
        c.r[3] = kEsrch;
        return;
    }
    Event event;
    const uint64_t result = receive(*queue, event, timeout);
    if (result != kOk) {
        c.r[3] = result;
        return;
    }
    c.r[4] = event.source;
    c.r[5] = event.data1;
    c.r[6] = event.data2;
    c.r[7] = event.data3;
    c.r[3] = kOk;
}

// sys_event_port_create(sys_event_port_t* id, type, name): `name` is what its events say they came from.
void sys_event_port_create(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]);
    if (!out) {
        c.r[3] = kEfault;
        return;
    }
    auto port = std::make_shared<Port>();
    port->name = c.r[5];
    store32(out, g_ports.add(std::move(port)));
    c.r[3] = kOk;
}

// sys_event_port_destroy(id): not while it is connected.
void sys_event_port_destroy(Cpu& c) {
    const uint32_t id = static_cast<uint32_t>(c.r[3]);
    const auto port = g_ports.find(id);
    if (!port) {
        c.r[3] = kEsrch;
        return;
    }
    {
        const std::lock_guard lock(port->mutex);
        if (port->queue) {
            c.r[3] = kEisconn;
            return;
        }
    }
    g_ports.remove(id);
    c.r[3] = kOk;
}

// sys_event_port_connect_local(port, queue)
void sys_event_port_connect_local(Cpu& c) {
    const auto port = g_ports.find(static_cast<uint32_t>(c.r[3]));
    const uint32_t queue = static_cast<uint32_t>(c.r[4]);
    if (!port || !g_queues.find(queue)) {
        c.r[3] = kEsrch;
        return;
    }
    const std::lock_guard lock(port->mutex);
    if (port->queue) {
        c.r[3] = kEisconn;
        return;
    }
    port->queue = queue;
    c.r[3] = kOk;
}

// sys_event_port_disconnect(port)
void sys_event_port_disconnect(Cpu& c) {
    const auto port = g_ports.find(static_cast<uint32_t>(c.r[3]));
    if (!port) {
        c.r[3] = kEsrch;
        return;
    }
    const std::lock_guard lock(port->mutex);
    if (!port->queue) {
        c.r[3] = kEnotconn;
        return;
    }
    port->queue = 0;
    c.r[3] = kOk;
}

// sys_event_port_send(port, data1, data2, data3): busy if the queue is full.
void sys_event_port_send(Cpu& c) {
    const uint32_t id = static_cast<uint32_t>(c.r[3]);
    const auto port = g_ports.find(id);
    if (!port) {
        c.r[3] = kEsrch;
        return;
    }
    uint32_t queue;
    uint64_t source;
    {
        const std::lock_guard lock(port->mutex);
        queue = port->queue;
        // A port made with no name says its events come from the process and the port itself.
        source = port->name ? port->name : (uint64_t{1} << 32) | id;
    }
    if (!queue) {
        c.r[3] = kEnotconn;
        return;
    }
    c.r[3] = send_event(queue, {source, c.r[4], c.r[5], c.r[6]});
}

constexpr NativeSystemCall kCalls[] = {
    {128, sys_event_queue_create},  {129, sys_event_queue_destroy},      {130, sys_event_queue_receive},   {134, sys_event_port_create},
    {135, sys_event_port_destroy},  {136, sys_event_port_connect_local}, {137, sys_event_port_disconnect}, {138, sys_event_port_send},
};

}  // namespace

uint64_t send_event(uint32_t queue_id, const Event& event) {
    const auto queue = g_queues.find(queue_id);
    if (!queue) return kEsrch;
    const std::lock_guard lock(queue->mutex);
    if (Waiter* const w = queue->receivers.pop()) {
        w->data[0] = event.source, w->data[1] = event.data1, w->data[2] = event.data2, w->data[3] = event.data3;
        w->wake();
        return kOk;
    }
    if (queue->events.size() >= queue->capacity) return kEbusy;
    queue->events.push_back(event);
    return kOk;
}

uint64_t receive_event(uint32_t queue_id, Event& event) {
    const auto queue = g_queues.find(queue_id);
    if (!queue) return kEsrch;
    return receive(*queue, event, 0);
}

uint32_t make_event_queue(size_t capacity) {
    auto queue = std::make_shared<Queue>();
    queue->capacity = capacity;
    return g_queues.add(std::move(queue));
}

void destroy_event_queue(uint32_t id) {
    const auto queue = g_queues.find(id);
    if (!queue) return;
    cancel(*queue);
    g_queues.remove(id);
}

uint32_t event_queue_by_key(uint64_t key) {
    const std::lock_guard lock(g_keys_mutex);
    const auto it = g_keys.find(key);
    return it == g_keys.end() ? 0 : it->second;
}

std::span<const NativeSystemCall> event_system_calls() { return kCalls; }

}  // namespace rr7

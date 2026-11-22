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
#include "events.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

struct Queue {
    std::mutex mutex;
    std::condition_variable arrived;
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
    {
        const std::lock_guard lock(queue->mutex);
        queue->destroyed = true;
    }
    queue->arrived.notify_all();
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
    std::unique_lock lock(queue->mutex);
    const auto ready = [&] { return !queue->events.empty() || queue->destroyed; };
    if (timeout) {
        if (!queue->arrived.wait_for(lock, std::chrono::microseconds(timeout), ready)) {
            c.r[3] = kEtimedout;
            return;
        }
    } else {
        queue->arrived.wait(lock, ready);
    }
    if (queue->events.empty()) {
        c.r[3] = kEcanceled;
        return;
    }
    const Event event = queue->events.front();
    queue->events.pop_front();
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
    {
        const std::lock_guard lock(queue->mutex);
        if (queue->events.size() >= queue->capacity) return kEbusy;
        queue->events.push_back(event);
    }
    queue->arrived.notify_one();
    return kOk;
}

uint64_t receive_event(uint32_t queue_id, Event& event) {
    const auto queue = g_queues.find(queue_id);
    if (!queue) return kEsrch;
    std::unique_lock lock(queue->mutex);
    queue->arrived.wait(lock, [&] { return !queue->events.empty() || queue->destroyed; });
    if (queue->events.empty()) return kEcanceled;
    event = queue->events.front();
    queue->events.pop_front();
    return kOk;
}

uint32_t make_event_queue(size_t capacity) {
    auto queue = std::make_shared<Queue>();
    queue->capacity = capacity;
    return g_queues.add(std::move(queue));
}

uint32_t event_queue_by_key(uint64_t key) {
    const std::lock_guard lock(g_keys_mutex);
    const auto it = g_keys.find(key);
    return it == g_keys.end() ? 0 : it->second;
}

std::span<const NativeSystemCall> event_system_calls() { return kCalls; }

}  // namespace rr7

// Sending events from native code (events.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace rr7 {

struct Event {
    uint64_t source = 0, data1 = 0, data2 = 0, data3 = 0;
};

// Puts an event on a queue. Returns the system's code: 0, "no such queue", or "busy" if it is full.
uint64_t send_event(uint32_t queue_id, const Event& event);

// Takes the next event from a queue, waiting for one. Returns the system's code: 0, "no such queue", or "cancelled" if the queue was destroyed.
uint64_t receive_event(uint32_t queue_id, Event& event);

// A queue made by the runtime itself (the system's own queues, such as a SPURS event flag's), holding up to `capacity` events.
uint32_t make_event_queue(size_t capacity);
// Destroys a queue the runtime made: threads waiting on it are woken with "cancelled".
void destroy_event_queue(uint32_t id);

// The queue made with this key, or 0. (A key is how one part of a program names a queue to another.)
uint32_t event_queue_by_key(uint64_t key);

}  // namespace rr7

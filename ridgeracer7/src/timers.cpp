// Timers (system calls 70, 71 and 73 to 76): a timer, once connected to an
// event queue and started, sends that queue an event when its time comes,
// once or every period.
//
// Their numbers are among the ten the table of names couldn't confirm
// (syscall_names.hpp); the game's use of them fits: 70 is given a pointer
// for an id, 75 a timer, a queue, a name and two numbers, 73 a timer, a time
// a few milliseconds past the system time, and a period.
//
// One host thread serves every timer: it sleeps until the earliest is due.
#include <chrono>
#include <condition_variable>
#include <thread>

#include "events.hpp"
#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

struct Timer {
    bool running = false;
    uint64_t due = 0, period = 0;  // in microseconds of the system's time
    uint32_t queue = 0;            // connected to this queue, or 0
    uint64_t source = 0, data1 = 0, data2 = 0;
};

std::mutex g_mutex;  // guards every timer, and the table
std::condition_variable g_changed;
std::map<uint32_t, Timer> g_timers;
uint32_t g_next_id = 0x11000001u;
std::thread g_service;

void serve() {
    std::unique_lock lock(g_mutex);
    for (;;) {  // (for the life of the process, which ends without unwinding: machine.cpp)
        // The earliest timer due, if any is running.
        Timer* next = nullptr;
        for (auto& [id, timer] : g_timers)
            if (timer.running && (!next || timer.due < next->due)) next = &timer;
        if (!next) {
            g_changed.wait(lock);
            continue;
        }
        const uint64_t now = system_time();
        if (next->due > now) {
            g_changed.wait_for(lock, std::chrono::microseconds(next->due - now));
            continue;  // (the table may have changed: look again)
        }
        if (next->queue) send_event(next->queue, {next->source, next->data1, next->data2, 0});  // a full queue loses the event, as on the console
        if (next->period) next->due += next->period;
        else next->running = false;
    }
}

Timer* find(uint64_t id) {
    const auto it = g_timers.find(static_cast<uint32_t>(id));
    return it == g_timers.end() ? nullptr : &it->second;
}

// sys_timer_create(sys_timer_t* id)
void sys_timer_create(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]);
    if (!out) {
        c.r[3] = kEfault;
        return;
    }
    const std::lock_guard lock(g_mutex);
    const uint32_t id = g_next_id++;
    g_timers.emplace(id, Timer{});
    if (!g_service.joinable()) g_service = std::thread(serve);
    store32(out, id);
    c.r[3] = kOk;
}

// sys_timer_destroy(id): not while it is connected to a queue.
void sys_timer_destroy(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Timer* timer = find(c.r[3]);
    if (!timer) {
        c.r[3] = kEsrch;
        return;
    }
    if (timer->queue) {
        c.r[3] = 0x80010015u;  // EISCONN
        return;
    }
    g_timers.erase(static_cast<uint32_t>(c.r[3]));
    g_changed.notify_all();
    c.r[3] = kOk;
}

// _sys_timer_start(id, when, period): at the system time `when`, then every `period` microseconds if that isn't 0.
void sys_timer_start(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Timer* timer = find(c.r[3]);
    if (!timer) {
        c.r[3] = kEsrch;
        return;
    }
    if (timer->running) {
        c.r[3] = kEbusy;
        return;
    }
    if (!timer->queue) {
        c.r[3] = 0x80010016u;  // ENOTCONN
        return;
    }
    const uint64_t period = c.r[5];
    if (period != 0 && period < 100) {
        c.r[3] = kEinval;
        return;
    }
    timer->due = c.r[4];
    timer->period = period;
    timer->running = true;
    g_changed.notify_all();
    c.r[3] = kOk;
}

// sys_timer_stop(id)
void sys_timer_stop(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Timer* timer = find(c.r[3]);
    if (!timer) {
        c.r[3] = kEsrch;
        return;
    }
    timer->running = false;
    g_changed.notify_all();
    c.r[3] = kOk;
}

// sys_timer_connect_event_queue(id, queue, name, data1, data2): the event it will send.
void sys_timer_connect_event_queue(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Timer* timer = find(c.r[3]);
    if (!timer) {
        c.r[3] = kEsrch;
        return;
    }
    if (timer->queue) {
        c.r[3] = 0x80010015u;  // EISCONN
        return;
    }
    timer->queue = static_cast<uint32_t>(c.r[4]);
    // With no name given, the event says it came from the process and the timer.
    timer->source = c.r[5] ? c.r[5] : (uint64_t{1} << 32) | static_cast<uint32_t>(c.r[3]);
    timer->data1 = c.r[6];
    timer->data2 = c.r[7];
    c.r[3] = kOk;
}

// sys_timer_disconnect_event_queue(id): it stops, too.
void sys_timer_disconnect_event_queue(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Timer* timer = find(c.r[3]);
    if (!timer) {
        c.r[3] = kEsrch;
        return;
    }
    if (!timer->queue) {
        c.r[3] = 0x80010016u;  // ENOTCONN
        return;
    }
    timer->queue = 0;
    timer->running = false;
    g_changed.notify_all();
    c.r[3] = kOk;
}

constexpr NativeSystemCall kCalls[] = {
    {70, sys_timer_create}, {71, sys_timer_destroy}, {73, sys_timer_start}, {74, sys_timer_stop}, {75, sys_timer_connect_event_queue},
    {76, sys_timer_disconnect_event_queue},
};

}  // namespace

std::span<const NativeSystemCall> timer_system_calls() { return kCalls; }

}  // namespace rr7

// Pausing the game under the settings menu (pause.hpp). Runtime code: no std::format.
#include "pause.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace rr7 {

namespace {

std::mutex g_mutex;
std::condition_variable g_changed;
std::atomic<bool> g_requested{false};  // read without the lock on every flip, blank and block of sound
std::array<bool, static_cast<size_t>(HoldPoint::count)> g_held{};

}  // namespace

void request_pause(bool on) {
    {
        const std::lock_guard lock(g_mutex);
        g_requested.store(on, std::memory_order_release);
    }
    g_changed.notify_all();
}

bool pause_requested() { return g_requested.load(std::memory_order_acquire); }

bool held_at(HoldPoint where) {
    const std::lock_guard lock(g_mutex);
    return g_held[static_cast<size_t>(where)];
}

bool hold_while_paused(HoldPoint where) {
    if (!g_requested.load(std::memory_order_acquire)) return false;
    std::unique_lock lock(g_mutex);
    if (!g_requested.load(std::memory_order_relaxed)) return false;
    bool& held = g_held[static_cast<size_t>(where)];
    held = true;
    g_changed.wait(lock, [] { return !g_requested.load(std::memory_order_relaxed); });
    held = false;
    return true;
}

}  // namespace rr7

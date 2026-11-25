// What the system's objects are made of here: threads' identities, error
// codes, and tables of objects by id.
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include "machine.hpp"

namespace rr7 {

// The system's error codes (the SDK's CELL_E... values, from its public documentation).
inline constexpr uint64_t kOk = 0;
inline constexpr uint64_t kEagain = 0x80010001u, kEinval = 0x80010002u, kEnosys = 0x80010003u, kEnomem = 0x80010004u, kEsrch = 0x80010005u,
                          kEnoent = 0x80010006u, kEdeadlk = 0x80010008u, kEperm = 0x80010009u, kEbusy = 0x8001000Au, kEtimedout = 0x8001000Bu,
                          kEfault = 0x8001000Du, kEcanceled = 0x80010013u, kEisconn = 0x80010015u, kEnotconn = 0x80010016u;

// The id of the guest thread this host thread runs.
uint32_t current_thread();
void set_current_thread(uint32_t id);

// Where each guest thread is, for the watch (a development aid: with RR7_WATCH=<seconds> in the
// environment, after that long the runtime prints what system function each thread is inside, and stops).
// `what` is a name that lives for ever, or null on the way out; `number` a system call's, `from` the caller's address.
void thread_enters(const char* what, uint32_t number, uint32_t from);
// The Cpu of the guest thread this host thread runs (for the fault report), or null.
void set_current_cpu(const Cpu* cpu);
const Cpu* current_cpu();
void thread_leaves();
// The calling thread's last calls to the system, newest first, for the fault report: each a name (or null: a system call), a number and the caller's address.
struct PastCall {
    const char* what;
    uint32_t number, from;
};
size_t past_calls(PastCall* out, size_t most);
void start_watch();
void start_profile();  // (machine.hpp)

// Objects of one kind, by id. Ids start at `first` and are never reused
// while the process runs (a stale id is then always an error, never another object).
template <typename T>
class ObjectTable {
public:
    explicit ObjectTable(uint32_t first) : next_(first) {}

    uint32_t add(std::shared_ptr<T> object) {
        const std::lock_guard lock(mutex_);
        const uint32_t id = next_++;
        objects_.emplace(id, std::move(object));
        return id;
    }
    std::shared_ptr<T> find(uint32_t id) {
        const std::lock_guard lock(mutex_);
        const auto it = objects_.find(id);
        return it == objects_.end() ? nullptr : it->second;
    }
    bool remove(uint32_t id) {
        const std::lock_guard lock(mutex_);
        return objects_.erase(id) != 0;
    }

private:
    std::mutex mutex_;
    std::map<uint32_t, std::shared_ptr<T>> objects_;
    uint32_t next_;
};

// Guest memory, for native code: big-endian reads and writes at an address.
uint32_t load32(uint32_t address);
void store32(uint32_t address, uint32_t value);
uint64_t load64(uint32_t address);
void store64(uint32_t address, uint64_t value);

}  // namespace rr7

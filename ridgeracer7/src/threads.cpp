// Threads: sysPrxForUser's sys_ppu_thread_create and sys_ppu_thread_exit,
// and the system calls on a thread by its id (yield 43, join 44, detach 45,
// priorities 47 and 48).
//
// A guest thread is a host thread with a Cpu of its own, a stack of its
// own in guest memory, and its own copy of the thread-local image.
//
// **Not yet as designed.** The roadmap's scheduler lets only two guest
// threads run guest code at once, by priority, as the console's two
// hardware threads do. Until it exists every guest thread simply runs, and a
// priority is kept and reported but decides nothing. A game that leans on
// the console's order of running could go wrong here in ways it wouldn't
// there: when something looks like a race, this is the first suspect.
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

constexpr int32_t kLowestPriority = 3071;
constexpr uint64_t kJoinable = 1;  // sys_ppu_thread_create's flag
constexpr uint32_t kStacks = 0xD0200000u, kStacksEnd = 0xE0000000u;  // other threads' stacks, each followed by a gap that is not memory

struct Thread {
    int32_t priority = 0;
    bool joinable = false, finished = false, joined = false;
    uint64_t exit_value = 0;
    std::thread host;
};
std::mutex g_mutex;
std::condition_variable g_finished;
std::map<uint32_t, Thread> g_threads;
uint32_t g_next_id = kFirstThreadId + 1;
uint32_t g_next_stack = kStacks;

struct ThreadExit {
    uint64_t value;
};

// What a new thread starts with, worked out by its creator.
struct Start {
    uint32_t id, code, toc, stack_top, tls;
    uint64_t argument;
};

void run_thread(Start start) {
    set_current_thread(start.id);
    Cpu cpu;
    cpu.mem = guest_memory();
    cpu.hooks = &guest_hooks();
    cpu.r[1] = start.stack_top - 0x80;
    cpu.r[2] = start.toc;
    cpu.r[3] = start.argument;
    cpu.r[13] = start.tls + 0x7000u;
    cpu.lr = kReturnToHost;
    uint64_t value = 0;
    try {
        call_guest(cpu, start.code);
        value = cpu.r[3];
    } catch (const ThreadExit& exit) {
        value = exit.value;
    }
    {
        const std::lock_guard lock(g_mutex);
        Thread& self = g_threads[start.id];
        self.finished = true;
        self.exit_value = value;
        if (!self.joinable) self.host.detach();  // nobody will join it: the host thread ends by itself
    }
    g_finished.notify_all();
}

// sys_ppu_thread_create(sys_ppu_thread_t* id, entry (a function descriptor), argument, priority, stack size, flags, name)
void sys_ppu_thread_create(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]), entry = static_cast<uint32_t>(c.r[4]);
    const int32_t priority = static_cast<int32_t>(c.r[6]);
    const uint64_t stack_size = c.r[7], flags = c.r[8];
    if (!out || !entry) {
        c.r[3] = kEfault;
        return;
    }
    if (priority < 0 || priority > kLowestPriority) {
        c.r[3] = kEinval;
        return;
    }
    Start start{};
    start.code = load32(entry);
    start.toc = load32(entry + 4);
    start.argument = c.r[5];
    // Its thread-local storage: the image, then zeroes.
    const TlsImage image = tls_image();
    start.tls = allocate(image.memory_size ? image.memory_size : 16, 16);
    std::memcpy(guest_memory() + start.tls, guest_memory() + image.address, image.file_size);
    const std::lock_guard lock(g_mutex);
    // Its stack: whole pages, at least one.
    const uint32_t size = static_cast<uint32_t>(std::max<uint64_t>((stack_size + 0xfff) & ~uint64_t{0xfff}, 0x1000));
    if (size > kStacksEnd - g_next_stack) {
        c.r[3] = kEnomem;
        return;
    }
    commit(g_next_stack, size);
    start.stack_top = g_next_stack + size;
    g_next_stack += size + 0x10000;
    start.id = g_next_id++;
    Thread& thread = g_threads[start.id];
    thread.priority = priority;
    thread.joinable = (flags & kJoinable) != 0;
    store64(out, start.id);
    thread.host = std::thread(run_thread, start);
    c.r[3] = kOk;
}

// sys_ppu_thread_exit(value): the calling thread ends.
void sys_ppu_thread_exit(Cpu& c) {
    if (current_thread() == kFirstThreadId) qilin::rt::fatal("the first thread called sys_ppu_thread_exit, which this runtime doesn't provide for it yet");
    throw ThreadExit{c.r[3]};
}

// sys_ppu_thread_join(thread, uint64_t* value): waits for it to end.
void sys_ppu_thread_join(Cpu& c) {
    const uint32_t id = static_cast<uint32_t>(c.r[3]), out = static_cast<uint32_t>(c.r[4]);
    if (id == current_thread()) {
        c.r[3] = kEdeadlk;
        return;
    }
    std::thread host;
    uint64_t value = 0;
    {
        std::unique_lock lock(g_mutex);
        const auto it = g_threads.find(id);
        if (it == g_threads.end()) {
            c.r[3] = kEsrch;
            return;
        }
        if (!it->second.joinable || it->second.joined) {
            c.r[3] = kEinval;
            return;
        }
        it->second.joined = true;
        g_finished.wait(lock, [&] { return g_threads[id].finished; });
        value = g_threads[id].exit_value;
        host = std::move(g_threads[id].host);
        g_threads.erase(id);
    }
    if (host.joinable()) host.join();
    if (out) store64(out, value);
    c.r[3] = kOk;
}

// sys_ppu_thread_detach(thread): it will not be joined.
void sys_ppu_thread_detach(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto it = g_threads.find(static_cast<uint32_t>(c.r[3]));
    if (it == g_threads.end()) {
        c.r[3] = kEsrch;
        return;
    }
    if (!it->second.joinable) {
        c.r[3] = kEinval;
        return;
    }
    it->second.joinable = false;
    if (it->second.host.joinable()) it->second.host.detach();
    c.r[3] = kOk;
}

// sys_ppu_thread_get_priority(thread, int* priority)
void sys_ppu_thread_get_priority(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto it = g_threads.find(static_cast<uint32_t>(c.r[3]));
    if (it == g_threads.end()) {
        c.r[3] = kEsrch;
        return;
    }
    store32(static_cast<uint32_t>(c.r[4]), static_cast<uint32_t>(it->second.priority));
    c.r[3] = kOk;
}

// sys_ppu_thread_set_priority(thread, priority): kept and reported (see the top of the file).
void sys_ppu_thread_set_priority(Cpu& c) {
    const int32_t priority = static_cast<int32_t>(c.r[4]);
    if (priority < 0 || priority > kLowestPriority) {
        c.r[3] = kEinval;
        return;
    }
    const std::lock_guard lock(g_mutex);
    const auto it = g_threads.find(static_cast<uint32_t>(c.r[3]));
    if (it == g_threads.end()) {
        c.r[3] = kEsrch;
        return;
    }
    it->second.priority = priority;
    c.r[3] = kOk;
}

// sys_ppu_thread_yield()
void sys_ppu_thread_yield(Cpu& c) {
    std::this_thread::yield();
    c.r[3] = kOk;
}

constexpr NativeSystemCall kCalls[] = {
    {43, sys_ppu_thread_yield}, {44, sys_ppu_thread_join}, {45, sys_ppu_thread_detach}, {47, sys_ppu_thread_set_priority}, {48, sys_ppu_thread_get_priority},
};

constexpr NativeImport kImports[] = {
    {"sysPrxForUser", "sys_ppu_thread_create", sys_ppu_thread_create},
    {"sysPrxForUser", "sys_ppu_thread_exit", sys_ppu_thread_exit},
};

}  // namespace

void add_thread(uint32_t id, int32_t priority) {
    const std::lock_guard lock(g_mutex);
    g_threads[id].priority = priority;
}

std::span<const NativeSystemCall> thread_system_calls() { return kCalls; }
std::span<const NativeImport> thread_imports() { return kImports; }

}  // namespace rr7

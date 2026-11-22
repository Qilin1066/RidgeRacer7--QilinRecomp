// SPU threads: the system's own way of running an SPU program (system
// calls 169 to 194, and sysPrxForUser's sys_spu_image_import).
//
// A game hands the system an SPU program (an ELF file in its own memory),
// makes a group of threads, gives each thread a program and four
// arguments, and starts the group. Each thread then runs its program in an
// SPU's 256 KiB of memory until it exits, and the game joins the group.
//
// Here a thread is a host thread running the program's lifted code
// (`qilin lift` lifts each of the game's 42 programs; the one to run is
// found by where its ELF file lies in the game's memory, and a program that
// was not lifted stops the game). What an SPU program can do beyond its own
// memory, it does through channels and the stop instruction, which arrive
// here as hooks:
//
//   - memory transfers (channels 16 to 24): get and put between the SPU's
//     memory and the game's, done at once when commanded, so every tag is
//     always complete; and the atomic pair (get a 128-byte line with a
//     reservation, put it back only if nobody changed it), which is done by
//     comparing: *a put succeeds if the line still holds what was read*,
//     which is how a lost reservation shows in everything but a change back
//     to the same bytes;
//   - mailboxes (28 to 30): a word in from the game (channel 29), a word
//     out (28), and the "interrupt" word (30) by which a thread sends the
//     game's event queues an event;
//   - stops: 0x101 ends the group, 0x102 ends the thread, 0x110 waits for an
//     event from a queue bound to the thread.
//
// **Not as a console does it, and to be kept in mind:** SPU threads run
// freely, each on a host thread, with no priorities (the scheduler is to
// come).
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "events.hpp"
#include "kernel.hpp"
#include "qilin/rt/report.hpp"
#include "qilin/rt/spu.hpp"
#include "spu_image.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;
using qilin::rt::Spu;
using qilin::rt::SpuReg;

constexpr uint64_t kSpuUserEvent = 0xFFFFFFFF53505501ull;  // where a thread's own events say they come from
constexpr uint32_t kJoinGroupExit = 1, kJoinAllThreadsExit = 2;

struct Group;

struct Thread {
    uint32_t id = 0;
    Group* group = nullptr;
    std::string name;
    const qilin::rt::LiftedSpuProgram* program = nullptr;
    qilin::SpuImage image;
    uint32_t image_address = 0;  // the ELF file, in the game's memory
    uint64_t arguments[4] = {};
    std::vector<uint8_t> ls;
    Spu spu;
    std::thread host;

    std::mutex mutex;  // the mailboxes, the ports
    std::condition_variable arrived;
    std::deque<uint32_t> in_mailbox;
    uint32_t out_mailbox = 0;
    std::map<uint32_t, uint32_t> ports;   // SPU port -> event queue (to send to)
    std::map<uint32_t, uint32_t> queues;  // SPU queue number -> event queue (to receive from)

    // The memory-transfer command being built, and the atomic line last read.
    uint32_t lsa = 0, eah = 0, eal = 0, size = 0, tag = 0, tag_mask = 0, atomic_status = 0;
    bool reserved = false;
    uint32_t reserved_at = 0;
    std::array<uint8_t, 128> reserved_data{};

    // A SPURS task (no group): its registers as SPURS gives them, and what to tell when it exits.
    bool task = false;
    SpuReg task_r3{}, task_r4{};
    std::function<void(uint32_t)> exited;
    std::function<uint32_t(uint32_t)> port_queue;  // a task's ports: its SPURS instance's, as they are now
};

struct Group {
    uint32_t id = 0;
    std::string name;
    uint32_t size = 0;
    int32_t priority = 0;
    std::vector<std::shared_ptr<Thread>> threads;
    bool running = false, ended = false;
    uint32_t alive = 0, cause = 0, status = 0;
    std::condition_variable changed;
};

std::mutex g_mutex;  // the groups and threads, and each group's state
std::map<uint32_t, std::shared_ptr<Group>> g_groups;
std::map<uint32_t, std::shared_ptr<Thread>> g_threads;
uint32_t g_next_group = 0x04000100u, g_next_thread = 0x02000000u;
bool g_initialised = false;
std::mutex g_atomic;  // the atomic line transfers, one at a time

struct Forbidden {
    uint32_t address, size;
    const char* what;
};
std::mutex g_forbidden_mutex;
std::vector<Forbidden> g_forbidden;  // forbid_spu_transfers

struct ThreadExit {
    uint32_t status;
};
struct GroupExit {
    uint32_t status;
};

Thread& self(Spu& s) { return *static_cast<Thread*>(s.owner); }

std::string guest_string(uint32_t address, size_t most) {
    if (!address) return {};
    const char* const p = reinterpret_cast<const char*>(guest_memory() + address);
    return std::string(p, strnlen(p, most));
}

// ---------------------------------------------------------------- memory transfers

uint8_t* local(Thread& t, uint32_t address, uint32_t size, uint32_t pc) {
    if (address > qilin::rt::kSpuLocalStore || size > qilin::rt::kSpuLocalStore - address)
        fatal("SPU thread %s: a transfer of %u bytes at %05x of its memory, which runs off the end (at %05x)", t.name.c_str(), size, address, pc);
    return t.ls.data() + address;
}
uint8_t* shared(Thread& t, uint32_t address, uint32_t size, uint32_t pc) {
    if (t.eah != 0) fatal("SPU thread %s: a transfer to an address above 4 GiB (at %05x)", t.name.c_str(), pc);
    if (size > 0xffffffffu - address) fatal("SPU thread %s: a transfer of %u bytes at %08x, which runs off the end of memory (at %05x)", t.name.c_str(), size, address, pc);
    return guest_memory() + address;
}

void transfer_command(Thread& t, uint32_t command, uint32_t pc) {
    {
        const uint32_t bytes = std::max<uint32_t>(t.size, 128);  // (an atomic transfer is a 128-byte line)
        const std::lock_guard lock(g_forbidden_mutex);
        for (const Forbidden& f : g_forbidden)
            if (t.eah == 0 && t.eal < f.address + f.size && f.address < t.eal + bytes)
                fatal("SPU program %s transfers (command %#x) at %08x, inside %s at %08x, which this runtime keeps natively: its layout is the SDK's, not known here (at %05x)",
                      t.name.c_str(), command, t.eal, f.what, f.address, pc);
    }
    // Bits 0 and 1 ask that the command wait for others of its tag: commands are done at once and in order here, so they say nothing.
    switch (command & ~3u) {
    case 0x40:  // get: the game's memory to the SPU's
        std::memcpy(local(t, t.lsa, t.size, pc), shared(t, t.eal, t.size, pc), t.size);
        return;
    case 0x20:  // put: the SPU's to the game's
        std::memcpy(shared(t, t.eal, t.size, pc), local(t, t.lsa, t.size, pc), t.size);
        return;
    case 0x44: case 0x24: {  // get and put by a list: pairs of (size, address), in the SPU's memory at `eal`, `size` bytes of them
        const bool get = (command & ~3u) == 0x44;
        uint32_t at = t.lsa;
        const uint8_t* const list = local(t, t.eal & 0x3fff8u, t.size, pc);
        for (uint32_t i = 0; i + 8 <= t.size; i += 8) {
            const uint32_t first = (uint32_t{list[i]} << 24) | (uint32_t{list[i + 1]} << 16) | (uint32_t{list[i + 2]} << 8) | list[i + 3];
            const uint32_t address = (uint32_t{list[i + 4]} << 24) | (uint32_t{list[i + 5]} << 16) | (uint32_t{list[i + 6]} << 8) | list[i + 7];
            const uint32_t bytes = first & 0x7fffu;
            if (first & 0x80000000u) fatal("SPU thread %s: a transfer list that asks to stall and notify, which this runtime doesn't provide yet (at %05x)", t.name.c_str(), pc);
            // Each piece lies in the SPU's memory where its address's low four bits put it within a quadword.
            at = (at & ~15u) | (address & 15u);
            if (get) std::memcpy(local(t, at, bytes, pc), shared(t, address, bytes, pc), bytes);
            else std::memcpy(shared(t, address, bytes, pc), local(t, at, bytes, pc), bytes);
            at = (at + bytes + 15u) & ~15u;
        }
        return;
    }
    case 0xD0: {  // get a line and reserve it
        const std::lock_guard lock(g_atomic);
        t.reserved_at = t.eal & ~127u;
        std::memcpy(t.reserved_data.data(), shared(t, t.reserved_at, 128, pc), 128);
        std::memcpy(local(t, t.lsa & 0x3ff80u, 128, pc), t.reserved_data.data(), 128);
        t.reserved = true;
        t.atomic_status = 4;
        return;
    }
    case 0xB4: {  // put the line back, if nobody changed it
        const std::lock_guard lock(g_atomic);
        const uint32_t line = t.eal & ~127u;
        uint8_t* const there = shared(t, line, 128, pc);
        if (t.reserved && t.reserved_at == line && std::memcmp(there, t.reserved_data.data(), 128) == 0) {
            std::memcpy(there, local(t, t.lsa & 0x3ff80u, 128, pc), 128);
            t.atomic_status = 0;
        } else {
            t.atomic_status = 1;
        }
        t.reserved = false;
        return;
    }
    case 0xB0: case 0xB8: {  // put the line whatever happened (the second form reports nothing)
        const std::lock_guard lock(g_atomic);
        std::memcpy(shared(t, t.eal & ~127u, 128, pc), local(t, t.lsa & 0x3ff80u, 128, pc), 128);
        if ((command & ~3u) == 0xB0) t.atomic_status = 2;
        t.reserved = false;
        return;
    }
    default: fatal("SPU thread %s: memory-transfer command %#x, which this runtime doesn't provide yet (at %05x)", t.name.c_str(), command, pc);
    }
}

// ---------------------------------------------------------------- the hooks

// A thread's "interrupt" mailbox: the top byte says what is wanted.
void interrupt_word(Thread& t, uint32_t value, uint32_t pc) {
    const uint32_t code = value >> 24;
    if (code >= 0x80) fatal("SPU thread %s: system request %#x through its interrupt mailbox, which this runtime doesn't provide yet (at %05x)", t.name.c_str(), code, pc);
    // An event to a queue of the game's: sys_spu_thread_send_event (the thread then reads the answer from its
    // mailbox), or with 0x40 sys_spu_thread_throw_event (it doesn't).
    const uint32_t port = code & 0x3fu, data0 = value & 0x00ffffffu;
    uint32_t queue = 0, data1;
    {
        const std::lock_guard lock(t.mutex);
        if (t.task) queue = t.port_queue(port);
        else if (const auto it = t.ports.find(port); it != t.ports.end()) queue = it->second;
        data1 = t.out_mailbox;
    }
    // *The event's layout is from public descriptions of the system: to be checked against the game's reading of it.*
    const uint64_t result = queue ? send_event(queue, {kSpuUserEvent, t.id, (uint64_t{port} << 32) | data0, data1}) : kEnotconn;
    if (!(code & 0x40u)) {
        const std::lock_guard lock(t.mutex);
        t.in_mailbox.push_back(static_cast<uint32_t>(result));
        t.arrived.notify_all();
    }
}

uint32_t read_channel(Spu& s, uint32_t channel, uint32_t pc) {
    Thread& t = self(s);
    switch (channel) {
    case 24: return t.tag_mask;  // which of the tags asked about are complete: all, always
    case 27: return t.atomic_status;
    case 29: {  // the mailbox in: waits for a word
        std::unique_lock lock(t.mutex);
        t.arrived.wait(lock, [&] { return !t.in_mailbox.empty(); });
        const uint32_t value = t.in_mailbox.front();
        t.in_mailbox.pop_front();
        return value;
    }
    default: fatal("SPU thread %s reads channel %u, which this runtime doesn't provide yet (at %05x)", t.name.c_str(), channel, pc);
    }
}

void write_channel(Spu& s, uint32_t channel, uint32_t value, uint32_t pc) {
    Thread& t = self(s);
    switch (channel) {
    case 16: t.lsa = value; return;
    case 17: t.eah = value; return;
    case 18: t.eal = value; return;
    case 19: t.size = value; return;
    case 20: t.tag = value; return;
    case 21: transfer_command(t, value, pc); return;
    case 22: t.tag_mask = value; return;
    case 23: return;  // when to report the tags' status: they are complete already
    case 28: {
        const std::lock_guard lock(t.mutex);
        t.out_mailbox = value;
        return;
    }
    case 30: interrupt_word(t, value, pc); return;
    default: fatal("SPU thread %s writes %08x to channel %u, which this runtime doesn't provide yet (at %05x)", t.name.c_str(), value, channel, pc);
    }
}

uint32_t channel_count(Spu& s, uint32_t channel, uint32_t pc) {
    Thread& t = self(s);
    switch (channel) {
    case 29: {
        const std::lock_guard lock(t.mutex);
        return static_cast<uint32_t>(t.in_mailbox.size());
    }
    case 24: case 27: case 28: case 30: return 1;  // a status to read, or room to write: always
    default: fatal("SPU thread %s asks the count of channel %u, which this runtime doesn't provide yet (at %05x)", t.name.c_str(), channel, pc);
    }
}

void stop(Spu& s, uint32_t code, uint32_t pc) {
    Thread& t = self(s);
    uint32_t word;
    {
        const std::lock_guard lock(t.mutex);
        word = t.out_mailbox;
    }
    switch (code) {
    case 0x101: throw GroupExit{word};
    case 0x102: throw ThreadExit{word};
    case 0x110: {  // sys_spu_thread_receive_event: the queue's number is in the mailbox out; the answer comes in the mailbox in
        uint32_t queue = 0;
        {
            const std::lock_guard lock(t.mutex);
            if (const auto it = t.queues.find(word); it != t.queues.end()) queue = it->second;
        }
        Event event;
        const uint64_t result = queue ? receive_event(queue, event) : kEinval;
        const std::lock_guard lock(t.mutex);
        t.in_mailbox.push_back(static_cast<uint32_t>(result));
        if (result == kOk) {
            t.in_mailbox.push_back(static_cast<uint32_t>(event.data1));
            t.in_mailbox.push_back(static_cast<uint32_t>(event.data2));
            t.in_mailbox.push_back(static_cast<uint32_t>(event.data3));
        }
        return;
    }
    default: fatal("SPU thread %s stops with code %#x, which this runtime doesn't provide yet (at %05x)", t.name.c_str(), code, pc);
    }
}

[[noreturn]] void halt(Spu& s, uint32_t pc, const char* why) { fatal("SPU thread %s at %05x: %s", self(s).name.c_str(), pc, why); }

const qilin::rt::SpuHooks kHooks{read_channel, write_channel, channel_count, stop, [](Spu& s, uint32_t pc, const char* why) { halt(s, pc, why); }};

// ---------------------------------------------------------------- running

void finish(Thread& t, uint32_t cause, uint32_t status) {
    if (t.task) {
        t.exited(status);
        return;
    }
    const std::lock_guard lock(g_mutex);
    Group& group = *t.group;
    if (cause == kJoinGroupExit && !group.ended) {
        group.cause = kJoinGroupExit;
        group.status = status;
    }
    if (--group.alive == 0) {
        if (!group.cause) group.cause = kJoinAllThreadsExit;
        group.ended = true;
        group.running = false;
        group.changed.notify_all();
    }
}

void run_thread(std::shared_ptr<Thread> thread) {
    Thread& t = *thread;
    set_current_thread(t.id);
    try {
        t.program->call(t.spu, t.image.entry, t.image.entry);
        fatal("SPU thread %s: its program returned from its entry point", t.name.c_str());
    } catch (const ThreadExit& exit) {
        finish(t, 0, exit.status);
    } catch (const GroupExit& exit) {
        // (The group's other threads go on until they exit themselves: a console ends them at once. The game's groups seen so far have one thread.)
        if (t.task) fatal("SPURS task %s ends with the SPU thread group's exit (stop 0x101)", t.name.c_str());
        if (t.group->size > 1) fatal("SPU thread %s ends its group of %u threads, which this runtime doesn't provide yet", t.name.c_str(), t.group->size);
        finish(t, kJoinGroupExit, exit.status);
    }
}

// The program's memory as the system leaves it for a new thread: its segments, zeroes elsewhere, the arguments in $3 to $6.
void prepare(Thread& t) {
    t.ls.assign(qilin::rt::kSpuLocalStore, 0);
    const uint8_t* const file = guest_memory() + t.image_address;
    for (const qilin::SpuSegment& segment : t.image.segments) {
        if (segment.type != 1 || segment.memory_size == 0) continue;  // (1: a segment to load)
        if (segment.address > qilin::rt::kSpuLocalStore || segment.memory_size > qilin::rt::kSpuLocalStore - segment.address)
            fatal("SPU program %s has a segment outside an SPU's memory", t.name.c_str());
        std::memcpy(t.ls.data() + segment.address, file + segment.offset, segment.file_size);
    }
    t.spu = Spu{};
    t.spu.ls = t.ls.data();
    t.spu.hooks = &kHooks;
    t.spu.owner = &t;
    for (unsigned i = 0; i < 4; ++i) t.spu.r[3 + i] = SpuReg{{static_cast<uint32_t>(t.arguments[i] >> 32), static_cast<uint32_t>(t.arguments[i]), 0, 0}};
    if (t.task) t.spu.r[3] = t.task_r3, t.spu.r[4] = t.task_r4, t.spu.r[5] = t.spu.r[6] = SpuReg{};
    t.spu.r[1] = SpuReg{{0x3fff0u, 0, 0, 0}};  // the stack starts at the top
    t.in_mailbox.clear();
    t.out_mailbox = 0;
    t.reserved = false;
    t.lsa = t.eah = t.eal = t.size = t.tag = t.tag_mask = t.atomic_status = 0;
}

const qilin::rt::LiftedSpuProgram& lifted(uint32_t image_address, const std::string& name) {
    for (const qilin::rt::LiftedSpuProgram& program : spu_programs())
        if (program.address == image_address) return program;
    fatal("the game starts an SPU program at %08x (%s) that is not among the %zu lifted: every SPU program must be known when the game is built", image_address, name.c_str(),
          spu_programs().size());
}

std::shared_ptr<Thread> thread_of(uint64_t id) {
    const auto it = g_threads.find(static_cast<uint32_t>(id));
    return it == g_threads.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------- the calls

// sys_spu_initialize(the most SPUs the process will use, how many of them raw). A PS3 gives a process up to six.
void sys_spu_initialize(Cpu& c) {
    const uint32_t max_usable = static_cast<uint32_t>(c.r[3]), max_raw = static_cast<uint32_t>(c.r[4]);
    const std::lock_guard lock(g_mutex);
    if (g_initialised) {
        c.r[3] = kEbusy;
        return;
    }
    if (max_usable > 6 || max_raw > 5 || max_raw > max_usable) {
        c.r[3] = kEinval;
        return;
    }
    qilin::rt::log("the game will use %u SPUs (%u raw)", max_usable, max_raw);
    g_initialised = true;
    c.r[3] = kOk;
}

// sys_spu_image_import(sys_spu_image_t* image, const void* the program's ELF file, type)
// On a console this lays the program out for loading. Here the program is lifted already: the image is made to
// name it (its type 0, its entry point, its file's address where a console keeps its segment list, its segments' count).
void sys_spu_image_import(Cpu& c) {
    const uint32_t image = static_cast<uint32_t>(c.r[3]), file = static_cast<uint32_t>(c.r[4]);
    if (!image || !file) {
        c.r[3] = kEinval;
        return;
    }
    const auto parsed = qilin::parse_spu_image({guest_memory() + file, 0x40000}, file);
    if (!parsed) fatal("sys_spu_image_import: what the game gives at %08x is not an SPU program: %s", file, parsed.error().c_str());
    store32(image, 0);
    store32(image + 4, parsed->entry);
    store32(image + 8, file);
    store32(image + 12, static_cast<uint32_t>(parsed->segments.size()));
    c.r[3] = kOk;
}

// sys_spu_image_close(image)
void sys_spu_image_close(Cpu& c) { c.r[3] = kOk; }

// sys_spu_thread_group_create(uint32_t* id, threads, priority, sys_spu_thread_group_attribute* {name's length, name, type, ...})
void sys_spu_thread_group_create(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]), size = static_cast<uint32_t>(c.r[4]), attribute = static_cast<uint32_t>(c.r[6]);
    const int32_t priority = static_cast<int32_t>(c.r[5]);
    if (!out || !attribute) {
        c.r[3] = kEfault;
        return;
    }
    if (size == 0 || size > 6 || priority < 16 || priority > 255) {
        c.r[3] = kEinval;
        return;
    }
    if (const uint32_t type = load32(attribute + 8); type != 0) fatal("sys_spu_thread_group_create: a group of type %#x, which this runtime doesn't provide yet", type);
    auto group = std::make_shared<Group>();
    group->size = size;
    group->priority = priority;
    group->name = guest_string(load32(attribute + 4), load32(attribute));
    const std::lock_guard lock(g_mutex);
    group->id = g_next_group++;
    g_groups.emplace(group->id, group);
    store32(out, group->id);
    c.r[3] = kOk;
}

// sys_spu_thread_group_destroy(id)
void sys_spu_thread_group_destroy(Cpu& c) {
    std::shared_ptr<Group> group;
    {
        const std::lock_guard lock(g_mutex);
        const auto it = g_groups.find(static_cast<uint32_t>(c.r[3]));
        if (it == g_groups.end()) {
            c.r[3] = kEsrch;
            return;
        }
        if (it->second->running) {
            c.r[3] = kEbusy;
            return;
        }
        group = it->second;
        g_groups.erase(it);
        for (const auto& thread : group->threads) g_threads.erase(thread->id);
    }
    for (const auto& thread : group->threads)
        if (thread->host.joinable()) thread->host.join();
    c.r[3] = kOk;
}

// sys_spu_thread_initialize(uint32_t* id, group, the thread's number in it, sys_spu_image_t* image,
//                           sys_spu_thread_attribute* {name, its length, option}, sys_spu_thread_argument* {four 64-bit arguments})
void sys_spu_thread_initialize(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]), number = static_cast<uint32_t>(c.r[5]), image = static_cast<uint32_t>(c.r[6]),
                   attribute = static_cast<uint32_t>(c.r[7]), arguments = static_cast<uint32_t>(c.r[8]);
    if (!out || !image) {
        c.r[3] = kEfault;
        return;
    }
    auto thread = std::make_shared<Thread>();
    thread->image_address = load32(image + 8);
    const auto parsed = qilin::parse_spu_image({guest_memory() + thread->image_address, 0x40000}, thread->image_address);
    if (!parsed) fatal("sys_spu_thread_initialize: the image names no SPU program (%08x): %s", thread->image_address, parsed.error().c_str());
    thread->image = *parsed;
    thread->name = attribute ? guest_string(load32(attribute), load32(attribute + 4)) : std::string();
    if (thread->name.empty()) thread->name = parsed->name.empty() ? "(no name)" : parsed->name;
    thread->program = &lifted(thread->image_address, parsed->name);
    if (arguments)
        for (unsigned i = 0; i < 4; ++i) thread->arguments[i] = load64(arguments + 8 * i);
    const std::lock_guard lock(g_mutex);
    const auto it = g_groups.find(static_cast<uint32_t>(c.r[4]));
    if (it == g_groups.end()) {
        c.r[3] = kEsrch;
        return;
    }
    Group& group = *it->second;
    if (group.running) {
        c.r[3] = kEbusy;
        return;
    }
    // The thread's number is its own (the game numbers the one thread of a group of one "1"), not a place below the group's size.
    if (number > 255 || group.threads.size() >= group.size) {
        c.r[3] = kEinval;
        return;
    }
    thread->group = &group;
    thread->id = g_next_thread++;
    group.threads.push_back(thread);
    g_threads.emplace(thread->id, thread);
    store32(out, thread->id);
    qilin::rt::log("the game makes SPU thread %s in group %s: the program at %08x, arguments %llx %llx %llx %llx", thread->name.c_str(), group.name.c_str(),
                   thread->image_address, static_cast<unsigned long long>(thread->arguments[0]), static_cast<unsigned long long>(thread->arguments[1]),
                   static_cast<unsigned long long>(thread->arguments[2]), static_cast<unsigned long long>(thread->arguments[3]));
    c.r[3] = kOk;
}

// sys_spu_thread_group_start(id): every thread of it starts its program from the beginning.
void sys_spu_thread_group_start(Cpu& c) {
    std::shared_ptr<Group> group;
    {
        const std::lock_guard lock(g_mutex);
        const auto it = g_groups.find(static_cast<uint32_t>(c.r[3]));
        if (it == g_groups.end()) {
            c.r[3] = kEsrch;
            return;
        }
        group = it->second;
        if (group->running || group->threads.empty()) {
            c.r[3] = group->running ? kEbusy : kEinval;
            return;
        }
        group->running = true;
        group->ended = false;
        group->cause = group->status = 0;
        group->alive = static_cast<uint32_t>(group->threads.size());
    }
    for (const auto& thread : group->threads) {
        if (thread->host.joinable()) thread->host.join();  // (its last run, long over)
        prepare(*thread);
        thread->host = std::thread(run_thread, thread);
    }
    c.r[3] = kOk;
}

// sys_spu_thread_group_join(id, int* cause, int* status): waits for the group to end; why it did (1: a thread ended the
// group; 2: every thread exited), and the status given.
void sys_spu_thread_group_join(Cpu& c) {
    const uint32_t cause = static_cast<uint32_t>(c.r[4]), status = static_cast<uint32_t>(c.r[5]);
    std::unique_lock lock(g_mutex);
    const auto it = g_groups.find(static_cast<uint32_t>(c.r[3]));
    if (it == g_groups.end()) {
        c.r[3] = kEsrch;
        return;
    }
    const std::shared_ptr<Group> group = it->second;
    group->changed.wait(lock, [&] { return group->ended; });
    if (cause) store32(cause, group->cause);
    if (status) store32(status, group->status);
    c.r[3] = kOk;
}

// sys_spu_thread_write_ls(thread, address, value, bytes: 1, 2, 4 or 8), sys_spu_thread_read_ls(thread, address, uint64_t* value, bytes)
void local_store_word(Cpu& c, bool write) {
    const uint32_t address = static_cast<uint32_t>(c.r[4]), bytes = static_cast<uint32_t>(c.r[6]);
    const std::lock_guard lock(g_mutex);
    const auto thread = thread_of(c.r[3]);
    if (!thread) {
        c.r[3] = kEsrch;
        return;
    }
    if ((bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) || address > qilin::rt::kSpuLocalStore - bytes || address % bytes || thread->ls.empty()) {
        c.r[3] = kEinval;
        return;
    }
    uint8_t* const p = thread->ls.data() + address;
    if (write) {
        for (uint32_t i = 0; i < bytes; ++i) p[i] = static_cast<uint8_t>(c.r[5] >> (8 * (bytes - 1 - i)));
    } else {
        uint64_t value = 0;
        for (uint32_t i = 0; i < bytes; ++i) value = (value << 8) | p[i];
        store64(static_cast<uint32_t>(c.r[5]), value);
    }
    c.r[3] = kOk;
}
void sys_spu_thread_write_ls(Cpu& c) { local_store_word(c, true); }
void sys_spu_thread_read_ls(Cpu& c) { local_store_word(c, false); }

// sys_spu_thread_write_spu_mb(thread, value): a word into the thread's mailbox.
void sys_spu_thread_write_spu_mb(Cpu& c) {
    std::shared_ptr<Thread> thread;
    {
        const std::lock_guard lock(g_mutex);
        thread = thread_of(c.r[3]);
    }
    if (!thread) {
        c.r[3] = kEsrch;
        return;
    }
    const std::lock_guard lock(thread->mutex);
    thread->in_mailbox.push_back(static_cast<uint32_t>(c.r[4]));
    thread->arrived.notify_all();
    c.r[3] = kOk;
}

// sys_spu_thread_connect_event(thread, queue, type (1: the thread's own events), port 0 to 63), sys_spu_thread_disconnect_event(thread, type, port)
void sys_spu_thread_connect_event(Cpu& c) {
    const uint32_t queue = static_cast<uint32_t>(c.r[4]), type = static_cast<uint32_t>(c.r[5]), port = static_cast<uint32_t>(c.r[6]);
    std::shared_ptr<Thread> thread;
    {
        const std::lock_guard lock(g_mutex);
        thread = thread_of(c.r[3]);
    }
    if (!thread) {
        c.r[3] = kEsrch;
        return;
    }
    if (type != 1 || port > 63) {
        c.r[3] = kEinval;
        return;
    }
    const std::lock_guard lock(thread->mutex);
    if (thread->ports.contains(port)) {
        c.r[3] = kEisconn;
        return;
    }
    thread->ports[port] = queue;
    c.r[3] = kOk;
}
void sys_spu_thread_disconnect_event(Cpu& c) {
    const uint32_t type = static_cast<uint32_t>(c.r[4]), port = static_cast<uint32_t>(c.r[5]);
    std::shared_ptr<Thread> thread;
    {
        const std::lock_guard lock(g_mutex);
        thread = thread_of(c.r[3]);
    }
    if (!thread) {
        c.r[3] = kEsrch;
        return;
    }
    if (type != 1 || port > 63) {
        c.r[3] = kEinval;
        return;
    }
    const std::lock_guard lock(thread->mutex);
    c.r[3] = thread->ports.erase(port) ? kOk : kEnotconn;
}

// sys_spu_thread_bind_queue(thread, queue, the number the thread will know it by), sys_spu_thread_unbind_queue(thread, number)
void sys_spu_thread_bind_queue(Cpu& c) {
    const uint32_t queue = static_cast<uint32_t>(c.r[4]), number = static_cast<uint32_t>(c.r[5]);
    std::shared_ptr<Thread> thread;
    {
        const std::lock_guard lock(g_mutex);
        thread = thread_of(c.r[3]);
    }
    if (!thread) {
        c.r[3] = kEsrch;
        return;
    }
    const std::lock_guard lock(thread->mutex);
    if (thread->queues.contains(number)) {
        c.r[3] = kEbusy;
        return;
    }
    thread->queues[number] = queue;
    c.r[3] = kOk;
}
void sys_spu_thread_unbind_queue(Cpu& c) {
    std::shared_ptr<Thread> thread;
    {
        const std::lock_guard lock(g_mutex);
        thread = thread_of(c.r[3]);
    }
    if (!thread) {
        c.r[3] = kEsrch;
        return;
    }
    const std::lock_guard lock(thread->mutex);
    c.r[3] = thread->queues.erase(static_cast<uint32_t>(c.r[4])) ? kOk : kEinval;
}

constexpr NativeSystemCall kCalls[] = {
    {169, sys_spu_initialize},          {170, sys_spu_thread_group_create},   {171, sys_spu_thread_group_destroy},    {172, sys_spu_thread_initialize},
    {173, sys_spu_thread_group_start},  {178, sys_spu_thread_group_join},     {181, sys_spu_thread_write_ls},         {182, sys_spu_thread_read_ls},
    {190, sys_spu_thread_write_spu_mb}, {191, sys_spu_thread_connect_event},  {192, sys_spu_thread_disconnect_event}, {193, sys_spu_thread_bind_queue},
    {194, sys_spu_thread_unbind_queue},
};

constexpr NativeImport kImports[] = {
    {"sysPrxForUser", "sys_spu_image_import", sys_spu_image_import},
    {"sysPrxForUser", "sys_spu_image_close", sys_spu_image_close},
};

}  // namespace

std::span<const NativeSystemCall> spu_system_calls() { return kCalls; }
std::span<const NativeImport> spu_imports() { return kImports; }

// A SPURS task: a thread of no group, started at once, on a host thread of its own (spurs.cpp: what SPURS makes of it).
void start_spu_task(const std::string& name, uint32_t image_address, const std::array<uint32_t, 4>& r3, const std::array<uint32_t, 4>& r4,
                    std::function<uint32_t(uint32_t port)> port_queue, std::function<void(uint32_t status)> exited) {
    auto thread = std::make_shared<Thread>();
    thread->image_address = image_address;
    const auto parsed = qilin::parse_spu_image({guest_memory() + image_address, 0x40000}, image_address);
    if (!parsed) fatal("a SPURS task's program at %08x is no SPU program: %s", image_address, parsed.error().c_str());
    thread->image = *parsed;
    thread->name = name.empty() ? parsed->name : name;
    thread->program = &lifted(image_address, parsed->name);
    thread->task = true;
    thread->task_r3 = SpuReg{{r3[0], r3[1], r3[2], r3[3]}};
    thread->task_r4 = SpuReg{{r4[0], r4[1], r4[2], r4[3]}};
    thread->port_queue = std::move(port_queue);
    thread->exited = std::move(exited);
    {
        const std::lock_guard lock(g_mutex);
        thread->id = g_next_thread++;
        g_threads.emplace(thread->id, thread);
    }
    prepare(*thread);
    // (The host thread is left to end by itself: the task's record stays in g_threads, as an exited SPU thread's does.)
    std::thread(run_thread, thread).detach();
}

std::mutex& spu_atomic_mutex() { return g_atomic; }

void forbid_spu_transfers(uint32_t address, uint32_t size, const char* what) {
    const std::lock_guard lock(g_forbidden_mutex);
    g_forbidden.push_back({address, size, what});
}

}  // namespace rr7

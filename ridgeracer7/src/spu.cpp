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
//
// What a start costs is kept small, since a SPURS frame starts many tasks:
// each ELF file is parsed once (and checked against its headers at every
// use), local stores are kept zeroed between threads, and a task's host
// thread is kept for the next task when it ends.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "events.hpp"
#include "kernel.hpp"
#include "machine.hpp"
#include "qilin/rt/report.hpp"
#include "qilin/rt/spu.hpp"
#include "qilin/rt/spu_fp.hpp"
#include "spu_image.hpp"

namespace rr7 {

namespace {

// SPURS tasks' times (spu_report): starting them; running them is counted by program (Program).
using Clock = std::chrono::steady_clock;
uint64_t since_ns(Clock::time_point t) { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count()); }
std::atomic<uint64_t> g_tasks_started{0}, g_task_setup_ns{0};

using qilin::rt::fatal;
using qilin::rt::kSpuLocalStore;
using qilin::rt::Spu;
using qilin::rt::SpuReg;

// Big-endian numbers in the SPU's memory or the game's.
uint32_t be32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return std::endian::native == std::endian::little ? std::byteswap(v) : v;
}
uint32_t be16(const uint8_t* p) { return (uint32_t{p[0]} << 8) | p[1]; }

// ---------------------------------------------------------------- programs and local stores

// An SPU program as the game's memory holds it, parsed once: the system is handed the same few dozen ELF files again and
// again (a SPURS task is started from its ELF every time). Kept by the file's address and checked at every use against
// the headers there now (the ELF header, the program headers and the section headers, which hold everything loading
// uses), so a different program put at the same address is parsed afresh. (Only the name, from a note's contents, could
// change unseen: it is used for messages and spu_report.)
struct Program {
    uint32_t address = 0;
    qilin::SpuImage image;
    const qilin::rt::LiftedSpuProgram* lifted = nullptr;  // null: not among the lifted (a thread of it stops the game)
    std::array<uint8_t, 52> header{};                     // the ELF header as parsed: where the two tables are
    std::vector<uint8_t> segments, sections;              // the program and section headers as parsed
    std::atomic<uint64_t> runs{0}, ns{0};                 // as SPURS tasks, for spu_report
};
std::mutex g_programs_mutex;
std::map<uint32_t, std::shared_ptr<Program>> g_programs;  // by address

// A table of headers: where in the file the ELF header puts it, and its length.
struct Table {
    uint32_t offset, bytes;
};
Table program_headers(const uint8_t* file) { return {be32(file + 28), be16(file + 42) * be16(file + 44)}; }
Table section_headers(const uint8_t* file) { return {be32(file + 32), be16(file + 46) * be16(file + 48)}; }

bool unchanged(const Program& p, const uint8_t* file) {
    // (The same ELF header puts the two tables in the same places.)
    return std::memcmp(file, p.header.data(), p.header.size()) == 0 &&
           std::memcmp(file + program_headers(file).offset, p.segments.data(), p.segments.size()) == 0 &&
           std::memcmp(file + section_headers(file).offset, p.sections.data(), p.sections.size()) == 0;
}

// The program whose ELF file is at `address` in the game's memory, or why there is none.
std::expected<std::shared_ptr<Program>, std::string> program_at(uint32_t address) {
    const uint8_t* const file = guest_memory() + address;
    {
        const std::lock_guard lock(g_programs_mutex);
        if (const auto it = g_programs.find(address); it != g_programs.end() && unchanged(*it->second, file)) return it->second;
    }
    auto parsed = qilin::parse_spu_image({file, 0x40000}, address);
    if (!parsed) return std::unexpected(std::move(parsed.error()));
    auto program = std::make_shared<Program>();
    program->address = address;
    program->image = std::move(*parsed);
    for (const qilin::rt::LiftedSpuProgram& lifted : spu_programs())
        if (lifted.address == address) program->lifted = &lifted;
    std::memcpy(program->header.data(), file, program->header.size());
    const Table segments = program_headers(file), sections = section_headers(file);  // (within the file: parsing checked)
    program->segments.assign(file + segments.offset, file + segments.offset + segments.bytes);
    program->sections.assign(file + sections.offset, file + sections.offset + sections.bytes);
    const std::lock_guard lock(g_programs_mutex);
    g_programs.insert_or_assign(address, program);
    return program;
}

// The lifted code of a program a thread is to run: every SPU program must be known when the game is built.
const qilin::rt::LiftedSpuProgram& lifted(const Program& program) {
    if (!program.lifted)
        fatal("the game starts an SPU program at %08x (%s) that is not among the %zu lifted: every SPU program must be known when the game is built", program.address,
              program.image.name.c_str(), spu_programs().size());
    return *program.lifted;
}

// An SPU's 256 KiB of memory, all zeroes when had. Kept for the next thread when one is done with it, and zeroed then by
// the thread giving it back: a SPURS task is given a fresh local store each time it starts and gives it back on its own
// host thread as it ends, so its starter only copies the program in.
class LocalStore {
public:
    LocalStore() = default;
    LocalStore(const LocalStore&) = delete;
    LocalStore& operator=(const LocalStore&) = delete;
    ~LocalStore() { give_back(std::move(bytes_)); }

    uint8_t* data() const { return bytes_.get(); }
    bool empty() const { return !bytes_; }
    // All zeroes again: the one held, zeroed (so it stays where it was, as a thread group started again has it), or one had.
    void renew() {
        if (bytes_) std::memset(bytes_.get(), 0, kSpuLocalStore);
        else bytes_ = take();
    }

private:
    static constexpr size_t kKept = 16;  // the most kept (4 MiB): more than the game runs at once
    struct Kept {
        std::mutex mutex;
        std::vector<std::unique_ptr<uint8_t[]>> stores;
    };
    // (Never destroyed: a host thread may give one back as the process ends.)
    static Kept& kept() {
        static Kept& k = *new Kept;
        return k;
    }
    static std::unique_ptr<uint8_t[]> take() {
        {
            Kept& k = kept();
            const std::lock_guard lock(k.mutex);
            if (!k.stores.empty()) {
                auto store = std::move(k.stores.back());
                k.stores.pop_back();
                return store;
            }
        }
        return std::make_unique<uint8_t[]>(kSpuLocalStore);  // (zeroed)
    }
    static void give_back(std::unique_ptr<uint8_t[]> store) {
        if (!store) return;
        std::memset(store.get(), 0, kSpuLocalStore);
        Kept& k = kept();
        const std::lock_guard lock(k.mutex);
        if (k.stores.size() < kKept) k.stores.push_back(std::move(store));
    }

    std::unique_ptr<uint8_t[]> bytes_;
};

constexpr uint64_t kSpuUserEvent = 0xFFFFFFFF53505501ull;  // where a thread's own events say they come from
constexpr uint32_t kJoinGroupExit = 1, kJoinAllThreadsExit = 2;

struct Group;

struct Thread {
    uint32_t id = 0;
    Group* group = nullptr;
    std::string name;
    std::shared_ptr<Program> program;  // its ELF file in the game's memory, and its lifted code
    uint64_t arguments[4] = {};
    LocalStore ls;
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

    // A SPURS task (no group): how it was started.
    bool task = false;
    SpuTaskSetup setup;
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
std::condition_variable g_atomic_changed;  // an atomic put has changed a line
std::atomic<void (*)(uint32_t)> g_atomic_watch{nullptr};
// Where a task's SPURS kernel is, in its local store below the program (spu.cpp's prepare): its context and its entry.
constexpr uint32_t kTaskContext = 0x2d00, kKernelEntry = 0x2e00;

// forbid_spu_transfers' ranges: few, only ever added, and read by every transfer without a lock (an entry is written
// before the count that takes it in).
struct Forbidden {
    uint32_t address = 0, size = 0;
    const char* what = nullptr;
};
constexpr size_t kMostForbidden = 32;
std::mutex g_forbidden_mutex;  // (those adding)
std::array<Forbidden, kMostForbidden> g_forbidden{};
std::atomic<size_t> g_forbidden_count{0};

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

// The local store is in the host's word order (spu.hpp): bytes go to and from it through ls_write and ls_read.
uint32_t local(Thread& t, uint32_t address, uint32_t size, uint32_t pc) {
    if (address > kSpuLocalStore || size > kSpuLocalStore - address)
        fatal("SPU thread %s: a transfer of %u bytes at %05x of its memory, which runs off the end (at %05x)", t.name.c_str(), size, address, pc);
    return address;
}
void to_local(Thread& t, uint32_t address, const uint8_t* from, uint32_t size, uint32_t pc) {
    qilin::rt::spu::ls_write(t.ls.data(), local(t, address, size, pc), from, size);
}
void from_local(Thread& t, uint32_t address, uint8_t* to, uint32_t size, uint32_t pc) { qilin::rt::spu::ls_read(t.ls.data(), local(t, address, size, pc), to, size); }
uint8_t* shared(Thread& t, uint32_t address, uint32_t size, uint32_t pc) {
    if (t.eah != 0) fatal("SPU thread %s: a transfer to an address above 4 GiB (at %05x)", t.name.c_str(), pc);
    if (size > 0xffffffffu - address) fatal("SPU thread %s: a transfer of %u bytes at %08x, which runs off the end of memory (at %05x)", t.name.c_str(), size, address, pc);
    return guest_memory() + address;
}

void transfer_command(Thread& t, uint32_t command, uint32_t pc) {
    if (const size_t forbidden = g_forbidden_count.load(std::memory_order_acquire)) {
        const uint32_t bytes = std::max<uint32_t>(t.size, 128);  // (an atomic transfer is a 128-byte line)
        for (const Forbidden& f : std::span(g_forbidden.data(), forbidden))
            if (t.eah == 0 && t.eal < f.address + f.size && f.address < t.eal + bytes)
                fatal("SPU program %s transfers (command %#x) at %08x, inside %s at %08x, which this runtime keeps natively: its layout is the SDK's, not known here (at %05x)",
                      t.name.c_str(), command, t.eal, f.what, f.address, pc);
    }
    // Bits 0 and 1 ask that the command wait for others of its tag: commands are done at once and in order here, so they say nothing.
    switch (command & ~3u) {
    case 0x40:  // get: the game's memory to the SPU's
        to_local(t, t.lsa, shared(t, t.eal, t.size, pc), t.size, pc);
        return;
    case 0x20:  // put: the SPU's to the game's
        from_local(t, t.lsa, shared(t, t.eal, t.size, pc), t.size, pc);
        return;
    case 0x44: case 0x24: {  // get and put by a list: pairs of (size, address), in the SPU's memory at `eal`, `size` bytes of them
        const bool get = (command & ~3u) == 0x44;
        uint32_t at = t.lsa;
        const uint32_t list = local(t, t.eal & 0x3fff8u, t.size, pc);
        for (uint32_t i = 0; i + 8 <= t.size; i += 8) {
            const uint32_t first = qilin::rt::spu::ls_word(t.ls.data(), list + i), address = qilin::rt::spu::ls_word(t.ls.data(), list + i + 4);
            const uint32_t bytes = first & 0x7fffu;
            if (first & 0x80000000u) fatal("SPU thread %s: a transfer list that asks to stall and notify, which this runtime doesn't provide yet (at %05x)", t.name.c_str(), pc);
            // Each piece lies in the SPU's memory where its address's low four bits put it within a quadword.
            at = (at & ~15u) | (address & 15u);
            if (get) to_local(t, at, shared(t, address, bytes, pc), bytes, pc);
            else from_local(t, at, shared(t, address, bytes, pc), bytes, pc);
            at = (at + bytes + 15u) & ~15u;
        }
        return;
    }
    case 0xD0: {  // get a line and reserve it
        const std::lock_guard lock(g_atomic);
        t.reserved_at = t.eal & ~127u;
        std::memcpy(t.reserved_data.data(), shared(t, t.reserved_at, 128, pc), 128);
        to_local(t, t.lsa & 0x3ff80u, t.reserved_data.data(), 128, pc);
        t.reserved = true;
        t.atomic_status = 4;
        return;
    }
    case 0xB4: {  // put the line back, if nobody changed it
        std::unique_lock lock(g_atomic);
        const uint32_t line = t.eal & ~127u;
        uint8_t* const there = shared(t, line, 128, pc);
        if (t.reserved && t.reserved_at == line && std::memcmp(there, t.reserved_data.data(), 128) == 0) {
            from_local(t, t.lsa & 0x3ff80u, there, 128, pc);
            t.atomic_status = 0;
            g_atomic_changed.notify_all();
        } else {
            t.atomic_status = 1;
        }
        t.reserved = false;
        if (t.atomic_status == 0) {
            lock.unlock();
            if (const auto watch = g_atomic_watch.load()) watch(line);
        }
        return;
    }
    case 0xB0: case 0xB8: {  // put the line whatever happened (the second form reports nothing)
        {
            const std::lock_guard lock(g_atomic);
            from_local(t, t.lsa & 0x3ff80u, shared(t, t.eal & ~127u, 128, pc), 128, pc);
            if ((command & ~3u) == 0xB0) t.atomic_status = 2;
            t.reserved = false;
            g_atomic_changed.notify_all();
        }
        if (const auto watch = g_atomic_watch.load()) watch(t.eal & ~127u);
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
        if (t.task) queue = spurs_port_queue(t.setup.spurs, port);
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

// A task's calls to the SPURS kernel's entry, by the number in $3: 0 the task ends; 2 it waits to be signalled.
void native_call(Spu& s, uint32_t target, uint32_t pc) {
    Thread& t = self(s);
    if (!t.task || target != kKernelEntry) halt(s, pc, "a branch through a register to an address that is not a function");
    const uint32_t call = s.r[3].w[0];
    switch (call) {
    case 0: throw ThreadExit{0};
    case 2:
        spurs_task_wait_signal(t.setup);
        s.r[3] = SpuReg{};
        return;
    default: fatal("SPURS task %s calls the kernel with %u, which this runtime doesn't provide yet (at %05x)", t.name.c_str(), call, pc);
    }
}

const qilin::rt::SpuHooks kHooks{read_channel, write_channel, channel_count, stop, [](Spu& s, uint32_t pc, const char* why) { halt(s, pc, why); }, native_call};

// ---------------------------------------------------------------- running

void finish(Thread& t, uint32_t cause, uint32_t status) {
    if (t.task) {
        spurs_task_exited(t.setup, status);
        // Its record goes with it: the game starts tasks every frame, each with its own local store.
        const std::lock_guard lock(g_mutex);
        g_threads.erase(t.id);
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
    qilin::rt::spu::enter_single_mode();  // (the host's float unit for its single precision: spu_fp.hpp)
    const Clock::time_point started = Clock::now();
    // (A task's time, from its start to its end, however it ends.)
    struct Timed {
        const Thread& t;
        Clock::time_point started;
        ~Timed() {
            if (!t.task) return;
            t.program->ns.fetch_add(since_ns(started), std::memory_order_relaxed);
            t.program->runs.fetch_add(1, std::memory_order_relaxed);
        }
    } timed{t, started};
    try {
        const uint32_t entry = t.program->image.entry;
        t.program->lifted->call(t.spu, entry, entry);
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

// The host threads SPURS tasks run on. A task is handed to one waiting for work, or to a new one if none is (so every task
// still starts at once, as when each had a thread of its own); a thread whose task has ended waits for the next. Making a
// host thread costs more than many a task's whole run.
class TaskHosts {
public:
    void run(std::shared_ptr<Thread> task) {
        {
            const std::lock_guard lock(mutex_);
            if (idle_ > 0) {
                --idle_;  // (that thread's: so no more tasks are handed over than there are threads waiting)
                handed_.push_back(std::move(task));
                work_.notify_one();
                return;
            }
        }
        std::thread([this, first = std::move(task)]() mutable { host(std::move(first)); }).detach();
    }

private:
    void host(std::shared_ptr<Thread> task) {
        for (;;) {
            run_thread(std::move(task));
            std::unique_lock lock(mutex_);
            ++idle_;
            work_.wait(lock, [&] { return !handed_.empty(); });
            task = std::move(handed_.front());
            handed_.pop_front();
        }
    }

    std::mutex mutex_;
    std::condition_variable work_;
    std::deque<std::shared_ptr<Thread>> handed_;  // tasks handed over, not yet taken up
    size_t idle_ = 0;                             // threads waiting, less the tasks handed over
};
// (Never destroyed: its threads wait on it as the process ends.)
TaskHosts& task_hosts() {
    static TaskHosts& hosts = *new TaskHosts;
    return hosts;
}

// The program's memory as the system leaves it for a new thread: its segments, zeroes elsewhere, the arguments in $3 to $6.
void prepare(Thread& t) {
    // (The SPU helpers' shuffles need SSSE3 when built to assume it: spu.hpp.)
    static const bool host_fits = qilin::rt::spu::host_has_ssse3() || !QILIN_SPU_SSSE3;
    if (!host_fits) fatal("this computer's processor lacks SSSE3, which the SPU programs' translation is built to use");
    t.ls.renew();
    uint8_t* const ls = t.ls.data();
    const uint8_t* const file = guest_memory() + t.program->address;
    for (const qilin::SpuSegment& segment : t.program->image.segments) {
        if (segment.type != 1 || segment.memory_size == 0) continue;  // (1: a segment to load)
        if (segment.address > kSpuLocalStore || segment.memory_size > kSpuLocalStore - segment.address)
            fatal("SPU program %s has a segment outside an SPU's memory", t.name.c_str());
        qilin::rt::spu::ls_write(ls, segment.address, file + segment.offset, segment.file_size);
    }
    t.spu = Spu{};
    t.spu.ls = ls;
    t.spu.hooks = &kHooks;
    t.spu.owner = &t;
    for (unsigned i = 0; i < 4; ++i) t.spu.r[3 + i] = SpuReg{{static_cast<uint32_t>(t.arguments[i] >> 32), static_cast<uint32_t>(t.arguments[i]), 0, 0}};
    if (t.task) {
        const auto& a = t.setup.r3;
        const auto& b = t.setup.r4;
        t.spu.r[3] = SpuReg{{a[0], a[1], a[2], a[3]}}, t.spu.r[4] = SpuReg{{b[0], b[1], b[2], b[3]}}, t.spu.r[5] = t.spu.r[6] = SpuReg{};
        // The kernel's data, where the SDK's code linked into the task reads it (docs/runtime.md): the instance's address at
        // 0x1C0, the taskset's workload number in 0x1D0's last word, and at 0x2FB8 the address of a context: +152 non-zero
        // in a task, +184 the taskset's 64-bit address, +196 the kernel's entry, +208 the DMA tag the kernel lends the library, +212 the task.
        // *The tag (31) and +152's value (1) are stated choices.*
        const auto put = [ls](uint32_t at, uint32_t v) { qilin::rt::spu::set_ls_word(ls, at, v); };
        put(0x1c4, t.setup.spurs);
        put(0x1dc, t.setup.workload);
        put(0x2fb8, kTaskContext);
        put(kTaskContext + 152, 1);
        put(kTaskContext + 188, t.setup.taskset);  // (+184: a 64-bit address, its high word 0)
        put(kTaskContext + 196, kKernelEntry);
        put(kTaskContext + 208, 31);
        put(kTaskContext + 212, t.setup.id);
    }
    t.spu.r[1] = SpuReg{{0x3fff0u, 0, 0, 0}};  // the stack starts at the top
    t.in_mailbox.clear();
    t.out_mailbox = 0;
    t.reserved = false;
    t.lsa = t.eah = t.eal = t.size = t.tag = t.tag_mask = t.atomic_status = 0;
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
    const auto program = program_at(file);
    if (!program) fatal("sys_spu_image_import: what the game gives at %08x is not an SPU program: %s", file, program.error().c_str());
    store32(image, 0);
    store32(image + 4, (*program)->image.entry);
    store32(image + 8, file);
    store32(image + 12, static_cast<uint32_t>((*program)->image.segments.size()));
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
    const uint32_t file = load32(image + 8);
    auto program = program_at(file);
    if (!program) fatal("sys_spu_thread_initialize: the image names no SPU program (%08x): %s", file, program.error().c_str());
    thread->program = std::move(*program);
    lifted(*thread->program);
    const std::string& program_name = thread->program->image.name;
    thread->name = attribute ? guest_string(load32(attribute), load32(attribute + 4)) : std::string();
    if (thread->name.empty()) thread->name = program_name.empty() ? "(no name)" : program_name;
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
                   file, static_cast<unsigned long long>(thread->arguments[0]), static_cast<unsigned long long>(thread->arguments[1]),
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
    Blocking blocking;  // (the core token while it waits: its SPU threads may wait on another guest thread)
    std::unique_lock lock(g_mutex);
    const auto it = g_groups.find(static_cast<uint32_t>(c.r[3]));
    if (it == g_groups.end()) {
        c.r[3] = kEsrch;
        return;
    }
    const std::shared_ptr<Group> group = it->second;
    if (!group->ended) blocking.release();
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
    if ((bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) || address > kSpuLocalStore - bytes || address % bytes || thread->ls.empty()) {
        c.r[3] = kEinval;
        return;
    }
    uint8_t* const p = thread->ls.data();
    if (write) {
        for (uint32_t i = 0; i < bytes; ++i) p[qilin::rt::spu::ls_index(address + i)] = static_cast<uint8_t>(c.r[5] >> (8 * (bytes - 1 - i)));
    } else {
        uint64_t value = 0;
        for (uint32_t i = 0; i < bytes; ++i) value = (value << 8) | p[qilin::rt::spu::ls_index(address + i)];
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
void start_spu_task(const SpuTaskSetup& setup) {
    const Clock::time_point setup_start = Clock::now();
    auto thread = std::make_shared<Thread>();
    auto program = program_at(setup.image);
    if (!program) fatal("a SPURS task's program at %08x is no SPU program: %s", setup.image, program.error().c_str());
    thread->program = std::move(*program);
    lifted(*thread->program);
    thread->name = thread->program->image.name;
    thread->task = true;
    thread->setup = setup;
    {
        const std::lock_guard lock(g_mutex);
        thread->id = g_next_thread++;
        g_threads.emplace(thread->id, thread);
    }
    prepare(*thread);
    // (The task's record is dropped when it ends: finish.)
    task_hosts().run(std::move(thread));
    ++g_tasks_started;
    g_task_setup_ns += since_ns(setup_start);
}

void spu_report() {
    qilin::rt::log("spu: %llu SPURS tasks started (%.0f ms starting them)", static_cast<unsigned long long>(g_tasks_started.load()),
                   static_cast<double>(g_task_setup_ns.load()) / 1e6);
    struct Times {
        std::string name;
        uint64_t runs, ns;
    };
    std::vector<Times> top;
    {
        const std::lock_guard lock(g_programs_mutex);
        for (const auto& [address, program] : g_programs)
            if (const uint64_t runs = program->runs.load(std::memory_order_relaxed))
                top.push_back({program->image.name, runs, program->ns.load(std::memory_order_relaxed)});
    }
    std::ranges::sort(top, std::ranges::greater{}, &Times::ns);
    for (size_t i = 0; i < top.size() && i < 12; ++i)
        qilin::rt::log("spu: task %s: %llu runs, %.0f ms in all, %.3f ms each", top[i].name.c_str(), static_cast<unsigned long long>(top[i].runs), static_cast<double>(top[i].ns) / 1e6,
                       static_cast<double>(top[i].ns) / 1e6 / static_cast<double>(top[i].runs));
}

std::mutex& spu_atomic_mutex() { return g_atomic; }
std::condition_variable& spu_atomic_changed() { return g_atomic_changed; }
void set_spu_atomic_watch(void (*watch)(uint32_t line)) { g_atomic_watch.store(watch); }

void forbid_spu_transfers(uint32_t address, uint32_t size, const char* what) {
    const std::lock_guard lock(g_forbidden_mutex);
    const size_t n = g_forbidden_count.load(std::memory_order_relaxed);
    if (n == kMostForbidden) fatal("more than %zu ranges of memory forbidden to SPU transfers (%s at %08x)", kMostForbidden, what, address);
    g_forbidden[n] = {address, size, what};
    g_forbidden_count.store(n + 1, std::memory_order_release);
}

}  // namespace rr7

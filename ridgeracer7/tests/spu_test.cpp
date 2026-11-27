// SPU threads and SPURS tasks (spu.cpp) with no game: guest memory, the lifted programs and SPURS's side stood in.
//
// Two made-up SPU programs, written as lifted code is (a function the runtime calls, reaching the system through the
// hooks), check what a thread is started with: its program's segments in a local store that is otherwise all zeroes
// (also when the store was used before, and dirtied, by an earlier thread), its arguments, the SPURS kernel's data; and
// what it can do: memory transfers (single and by list), the atomic line pair, the mailboxes, ending. `spu_test tasks`
// starts tasks one after another and many at once; `spu_test group` runs a thread group twice; `spu_test bench` reports
// what a task's start and end cost.
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "events.hpp"
#include "kernel.hpp"
#include "qilin/rt/report.hpp"
#include "qilin/rt/spu.hpp"

namespace {

using namespace std::chrono_literals;
using qilin::rt::Spu;
using rr7::Cpu;

std::vector<uint8_t> memory(1u << 20);
std::atomic<int> failures{0};

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures.fetch_add(1);
    }
}
[[noreturn]] void finish() {
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(failures.load() == 0 ? 0 : 1);  // (the runtime's task threads are still waiting for work)
}

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = static_cast<uint8_t>(v >> 8), b[at + 1] = static_cast<uint8_t>(v); }
uint32_t get32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }

// An SPU program: code at 0x3000 (the bytes 0..255, standing for code: only their place in the local store is looked at),
// data at 0x4000 (16 bytes in the file, 0x1000 in memory: the rest zeroes), a name note, the sections. Laid out as the
// SPU's linker does, section headers last.
constexpr uint32_t kCodeAt = 0x3000, kCodeBytes = 256, kDataAt = 0x4000, kDataFile = 16, kDataMemory = 0x1000;
std::vector<uint8_t> make_program(std::string_view name) {
    const uint32_t code = 0x100, data = code + kCodeBytes, note = data + kDataFile, names = note + 0x40, sections = names + 0x40;
    const std::string_view strings("\0.text\0.note.spu_name\0.shstrtab\0.data\0", 37);
    std::vector<uint8_t> b(sections + 5 * 40);
    const uint8_t ident[7] = {0x7f, 'E', 'L', 'F', 1, 2, 1};
    std::memcpy(b.data(), ident, 7);
    put16(b, 16, 2), put16(b, 18, 23), put32(b, 20, 1), put32(b, 24, kCodeAt), put32(b, 28, 52), put32(b, 32, sections);
    put16(b, 40, 52), put16(b, 42, 32), put16(b, 44, 3), put16(b, 46, 40), put16(b, 48, 5), put16(b, 50, 3);
    const auto segment = [&](size_t i, uint32_t type, uint32_t offset, uint32_t address, uint32_t file, uint32_t in_memory, uint32_t flags) {
        const size_t at = 52 + i * 32;
        put32(b, at, type), put32(b, at + 4, offset), put32(b, at + 8, address), put32(b, at + 16, file), put32(b, at + 20, in_memory), put32(b, at + 24, flags);
    };
    segment(0, 1, code, kCodeAt, kCodeBytes, kCodeBytes, 5);
    segment(1, 1, data, kDataAt, kDataFile, kDataMemory, 6);
    segment(2, 4, note, 0, 0x40, 0, 4);
    for (uint32_t i = 0; i < kCodeBytes; ++i) b[code + i] = static_cast<uint8_t>(i);
    for (uint32_t i = 0; i < kDataFile; ++i) b[data + i] = static_cast<uint8_t>(0xd0 + i);
    put32(b, note, 8), put32(b, note + 4, static_cast<uint32_t>(name.size() + 1)), put32(b, note + 8, 1);
    std::memcpy(b.data() + note + 12, "SPUNAME", 7);
    std::memcpy(b.data() + note + 20, name.data(), name.size());
    std::memcpy(b.data() + names, strings.data(), strings.size());
    const auto section = [&](size_t i, uint32_t name_at, uint32_t type, uint32_t flags, uint32_t address, uint32_t offset, uint32_t size) {
        const size_t at = sections + i * 40;
        put32(b, at, name_at), put32(b, at + 4, type), put32(b, at + 8, flags), put32(b, at + 12, address), put32(b, at + 16, offset), put32(b, at + 20, size);
    };
    section(1, 1, 1, 6, kCodeAt, code, kCodeBytes);
    section(2, 7, 7, 0, 0, note, 0x40);
    section(3, 22, 3, 0, 0, names, static_cast<uint32_t>(strings.size()));
    section(4, 32, 1, 3, kDataAt, data, kDataFile);
    return b;
}

// Where things are in the stand-in guest memory.
constexpr uint32_t kTaskProgram = 0x40000, kGroupProgram = 0x80000;
constexpr uint32_t kSource = 0x8000, kTarget = 0x9000, kLine = 0xA000, kListTarget = 0xB000, kSpurs = 0xC000, kTaskset = 0xC100;

uint32_t rdch(Spu& s, uint32_t channel) { return s.hooks->read_channel(s, channel, 0x3000); }
void wrch(Spu& s, uint32_t channel, uint32_t value) { s.hooks->write_channel(s, channel, value, 0x3000); }
void dma(Spu& s, uint32_t ls, uint32_t ea, uint32_t size, uint32_t command) {
    wrch(s, 16, ls), wrch(s, 17, 0), wrch(s, 18, ea), wrch(s, 19, size), wrch(s, 20, 1), wrch(s, 21, command);
}

// What a new thread's local store must be: the segments, zeroes everywhere else.
bool fresh(const Spu& s) {
    for (uint32_t i = 0; i < kCodeBytes; ++i)
        if (s.ls[kCodeAt + i] != i) return false;
    for (uint32_t i = 0; i < kDataFile; ++i)
        if (s.ls[kDataAt + i] != 0xd0 + i) return false;
    const auto zero = [&](uint32_t from, uint32_t to) { return std::all_of(s.ls + from, s.ls + to, [](uint8_t b) { return b == 0; }); };
    // (Below 0x3000 a task's kernel data is: checked apart.)
    return zero(0x3000 + kCodeBytes, kDataAt) && zero(kDataAt + kDataFile, qilin::rt::kSpuLocalStore) && zero(0, 0x1c0);
}

std::mutex seen_mutex;
std::set<std::thread::id> hosts;  // the host threads tasks have run on

// The task: checks what it was started with, moves memory about, dirties its whole local store, and ends.
void task_program(Spu& s, uint32_t target, uint32_t) {
    {
        const std::lock_guard lock(seen_mutex);
        hosts.insert(std::this_thread::get_id());
    }
    check(target == kCodeAt, "a task starts at its entry");
    check(fresh(s), "a task's local store holds its program and zeroes");
    check(s.r[3].w[0] == 0x11111111u && s.r[3].w[3] == 0x44444444u && s.r[4].w[0] == 0 && s.r[4].w[1] == 0x5555u, "a task's $3 and $4");
    check(get32(s.ls + 0x1c4) == kSpurs && get32(s.ls + 0x2fb8) == 0x2d00 && get32(s.ls + 0x2d00 + 196) == 0x2e00 && get32(s.ls + 0x2d00 + 188) == kTaskset,
          "the SPURS kernel's data in a task's local store");
    // A get and a put.
    dma(s, 0x5000, kSource, 32, 0x40);
    check(std::memcmp(s.ls + 0x5000, memory.data() + kSource, 32) == 0, "a get");
    for (uint32_t i = 0; i < 32; ++i) s.ls[0x6000 + i] = static_cast<uint8_t>(0x80 + i);
    dma(s, 0x6000, kTarget, 32, 0x20);
    check(memory[kTarget] == 0x80 && memory[kTarget + 31] == 0x80 + 31, "a put");
    // A list: 8 bytes from kSource + 4, then 20 from kSource + 0x23, each to where its address's low four bits put it.
    const uint32_t list = 0x7000;
    const uint32_t elements[4] = {8, kSource + 4, 20, kSource + 0x23};
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t k = 0; k < 4; ++k) s.ls[list + 4 * i + k] = static_cast<uint8_t>(elements[i] >> (24 - 8 * k));
    dma(s, 0x8000, list, 16, 0x44);
    check(std::memcmp(s.ls + 0x8004, memory.data() + kSource + 4, 8) == 0 && std::memcmp(s.ls + 0x8013, memory.data() + kSource + 0x23, 20) == 0, "a get by a list");
    // And back by a list, elsewhere.
    const uint32_t back[4] = {8, kListTarget + 4, 20, kListTarget + 0x23};
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t k = 0; k < 4; ++k) s.ls[list + 4 * i + k] = static_cast<uint8_t>(back[i] >> (24 - 8 * k));
    dma(s, 0x8000, list, 16, 0x24);
    check(std::memcmp(memory.data() + kListTarget + 4, memory.data() + kSource + 4, 8) == 0 &&
              std::memcmp(memory.data() + kListTarget + 0x23, memory.data() + kSource + 0x23, 20) == 0,
          "a put by a list");
    // The atomic pair: a counter in the line's first word, raised by one; a put with no reservation fails.
    for (;;) {
        dma(s, 0x9000, kLine, 128, 0xD0);
        check(rdch(s, 27) == 4, "the line is reserved");
        const uint32_t count = get32(s.ls + 0x9000) + 1;
        for (int k = 0; k < 4; ++k) s.ls[0x9000 + k] = static_cast<uint8_t>(count >> (24 - 8 * k));
        dma(s, 0x9000, kLine, 128, 0xB4);
        if (rdch(s, 27) == 0) break;
    }
    dma(s, 0x9000, kLine, 128, 0xB4);
    check(rdch(s, 27) == 1, "a conditional put with no reservation fails");
    // Dirty everything, for the next task given this local store.
    std::memset(s.ls, 0xAA, qilin::rt::kSpuLocalStore);
    // The kernel's call 0: the task ends.
    s.r[3] = {{0, 0, 0, 0}};
    s.hooks->native_call(s, 0x2e00, 0x3100);
    check(false, "a task goes on after the kernel's call to end it");
}

// The group's thread: takes a word from its mailbox and ends with it plus one, after dirtying its memory: the first time
// as the thread's exit, the second as the group's.
void group_program(Spu& s, uint32_t, uint32_t) {
    check(fresh(s), "a group thread's local store holds its program and zeroes");
    check(s.r[3].w[0] == 0 && s.r[3].w[1] == 7 && s.r[6].w[1] == 10, "a group thread's arguments");
    const uint32_t word = rdch(s, 29);
    std::memset(s.ls + 0x10000, 0x55, 0x30000);
    wrch(s, 28, word + 1);
    s.hooks->stop(s, word == 41 ? 0x102 : 0x101, 0x3104);  // (the thread's end, then the group's)
    check(false, "a thread goes on after stop 0x101 or 0x102");
}

const qilin::rt::LiftedSpuProgram programs[] = {
    {kTaskProgram, 0, kCodeAt, task_program},
    {kGroupProgram, 0, kCodeAt, group_program},
};

// SPURS's side: tasks' ends counted.
std::mutex exits_mutex;
std::condition_variable exits_changed;
uint32_t exits = 0;
void wait_exits(uint32_t n) {
    std::unique_lock lock(exits_mutex);
    if (!exits_changed.wait_for(lock, 10s, [&] { return exits >= n; })) {
        check(false, "tasks didn't end");
        finish();
    }
}

rr7::SpuTaskSetup setup(uint32_t id) {
    return {.image = kTaskProgram, .spurs = kSpurs, .taskset = kTaskset, .workload = 2, .id = id, .r3 = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u},
            .r4 = {0, 0x5555u, 0, 0}};
}

void tasks() {
    // One after another: each must find its local store fresh, though the last dirtied it.
    for (uint32_t i = 0; i < 40; ++i) {
        rr7::start_spu_task(setup(i % 32));
        wait_exits(i + 1);
        std::this_thread::sleep_for(1ms);  // (for its host thread to be waiting for work again)
    }
    {
        const std::lock_guard lock(seen_mutex);
        check(hosts.size() <= 20, "host threads are kept for the next task");
    }
    // Many at once.
    for (uint32_t i = 0; i < 24; ++i) rr7::start_spu_task(setup(i));
    wait_exits(64);
    check(get32(memory.data() + kLine) == 64, "every task's atomic increment counted once");
}

void syscall(uint32_t number, Cpu& c) {
    for (const auto& call : rr7::spu_system_calls())
        if (call.number == number) return call.handler(c);
    check(false, "an SPU system call is missing");
}

void group() {
    Cpu c;
    // The group: one thread, priority 100, its attribute's name "g".
    rr7::store32(0x2000, 2), rr7::store32(0x2004, 0x2100), rr7::store32(0x2008, 0);
    std::memcpy(memory.data() + 0x2100, "g", 2);
    c.r[3] = 0x2010, c.r[4] = 1, c.r[5] = 100, c.r[6] = 0x2000;
    syscall(170, c);
    check(c.r[3] == rr7::kOk, "a group is made");
    const uint32_t group_id = rr7::load32(0x2010);
    rr7::store32(0x2208, kGroupProgram);  // the image's third word: the ELF's address
    for (uint32_t i = 0; i < 4; ++i) rr7::store64(0x2300 + 8 * i, 7 + i);
    c.r[3] = 0x2014, c.r[4] = group_id, c.r[5] = 1, c.r[6] = 0x2200, c.r[7] = 0, c.r[8] = 0x2300;
    syscall(172, c);
    check(c.r[3] == rr7::kOk, "a thread is made");
    const uint32_t thread_id = rr7::load32(0x2014);
    for (uint32_t run = 0; run < 2; ++run) {
        c.r[3] = group_id;
        syscall(173, c);
        check(c.r[3] == rr7::kOk, "the group starts");
        c.r[3] = thread_id, c.r[4] = 41 + run;
        syscall(190, c);
        check(c.r[3] == rr7::kOk, "a word to the thread's mailbox");
        c.r[3] = group_id, c.r[4] = 0x2400, c.r[5] = 0x2404;
        syscall(178, c);
        // Its one thread's exit (2: every thread exited), then the group's (1) with the status the thread gave.
        check(c.r[3] == rr7::kOk && rr7::load32(0x2400) == 2 - run && rr7::load32(0x2404) == (run ? 43u : 0u), "the group ends as its thread ended it");
    }
    c.r[3] = group_id;
    syscall(171, c);
    check(c.r[3] == rr7::kOk, "the group is destroyed");
}

void bench() {
    using Clock = std::chrono::steady_clock;
    constexpr uint32_t kRuns = 2000;
    Clock::duration starting{};
    const Clock::time_point start = Clock::now();
    for (uint32_t i = 0; i < kRuns; ++i) {
        const auto task = setup(i % 32);
        const Clock::time_point before = Clock::now();
        rr7::start_spu_task(task);
        starting += Clock::now() - before;
        wait_exits(i + 1);
    }
    const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count() / kRuns;
    const double start_us = std::chrono::duration<double, std::micro>(starting).count() / kRuns;
    std::printf("a task started and ended: %.1f us each (%.1f us of it in start_spu_task), on %zu host threads\n", us, start_us, hosts.size());
}

}  // namespace

namespace rr7 {
uint8_t* guest_memory() { return memory.data(); }
uint32_t load32(uint32_t address) { return get32(memory.data() + address); }
uint64_t load64(uint32_t address) { return (uint64_t{load32(address)} << 32) | load32(address + 4); }
void store32(uint32_t address, uint32_t value) {
    for (int i = 0; i < 4; ++i) memory[address + static_cast<uint32_t>(i)] = static_cast<uint8_t>(value >> (24 - 8 * i));
}
void store64(uint32_t address, uint64_t value) { store32(address, static_cast<uint32_t>(value >> 32)), store32(address + 4, static_cast<uint32_t>(value)); }
void set_current_thread(uint32_t) {}
std::span<const qilin::rt::LiftedSpuProgram> spu_programs() { return programs; }
uint64_t send_event(uint32_t, const Event&) { return kOk; }
uint64_t receive_event(uint32_t, Event&) { return kEinval; }
uint32_t spurs_port_queue(uint32_t, uint32_t) { return 0; }
void spurs_task_exited(const SpuTaskSetup&, uint32_t status) {
    check(status == 0, "a task ends with status 0");
    const std::lock_guard lock(exits_mutex);
    ++exits;
    exits_changed.notify_all();
}
void spurs_task_wait_signal(const SpuTaskSetup&) {}
}  // namespace rr7

namespace qilin::rt {
void log(const char*, ...) {}
[[noreturn]] void fatal(const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    std::fputs("unexpected fatal: ", stderr);
    std::vfprintf(stderr, format, arguments);
    std::fputs("\n", stderr);
    va_end(arguments);
    std::_Exit(2);
}
}  // namespace qilin::rt

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("spu_test tasks | group | bench\n", stderr);
        return 2;
    }
    const auto place = [](uint32_t at, const std::vector<uint8_t>& program) { std::memcpy(memory.data() + at, program.data(), program.size()); };
    place(kTaskProgram, make_program("Task_r.elf"));
    place(kGroupProgram, make_program("Group_r.elf"));
    for (uint32_t i = 0; i < 0x100; ++i) memory[kSource + i] = static_cast<uint8_t>(i * 7 + 3);
    const std::string mode = argv[1];
    if (mode == "tasks") tasks();
    else if (mode == "group") group();
    else if (mode == "bench") bench();
    else check(false, "an unknown test");
    if (failures.load() == 0) std::puts("SPU threads and tasks: passed");
    finish();
}

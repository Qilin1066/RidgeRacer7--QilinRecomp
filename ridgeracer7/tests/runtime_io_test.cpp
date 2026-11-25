// Real file and thread syscalls, with guest memory and the renderer stood in.
// No game files, window, sound device or GPU are needed.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace {
using namespace std::chrono_literals;
using rr7::Cpu;
std::vector<uint8_t> memory(1u << 20);
rr7::GameInfo info;
thread_local uint32_t thread_id = rr7::kFirstThreadId;
std::mutex gate_mutex;
std::condition_variable gate_changed;
bool entered = false, released = false;
std::atomic<bool> block_read{false};

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::_Exit(1);  // a negative test may still have a parked worker
    }
}
void park() {
    std::unique_lock lock(gate_mutex);
    entered = true;
    gate_changed.notify_all();
    check(gate_changed.wait_for(lock, 5s, [] { return released; }), "worker release timed out");
}
void wait_entered() {
    std::unique_lock lock(gate_mutex);
    check(gate_changed.wait_for(lock, 5s, [] { return entered; }), "worker did not start");
}
void release() {
    const std::lock_guard lock(gate_mutex);
    released = true;
    gate_changed.notify_all();
}
void reset_gate() {
    const std::lock_guard lock(gate_mutex);
    entered = released = false;
}
using Handler = void (*)(Cpu&);
Handler file_call(const char* name) {
    for (const auto& entry : rr7::fs_imports())
        if (std::strcmp(entry.name, name) == 0) return entry.handler;
    check(false, "missing file syscall");
    return nullptr;
}
Handler thread_call(uint32_t number) {
    for (const auto& entry : rr7::thread_system_calls())
        if (entry.number == number) return entry.handler;
    check(false, "missing thread syscall");
    return nullptr;
}
uint64_t open_file(const char* name, uint32_t flags, uint32_t& fd) {
    const std::string path = std::string("/dev_hdd0/") + name;
    std::memcpy(memory.data() + 0x1000, path.c_str(), path.size() + 1);
    Cpu cpu;
    cpu.r[3] = 0x1000; cpu.r[4] = flags; cpu.r[5] = 0x2000;
    file_call("cellFsOpen")(cpu);
    if (cpu.r[3] == rr7::kOk) fd = rr7::load32(0x2000);
    return cpu.r[3];
}
void close_file(uint32_t fd) {
    Cpu cpu;
    cpu.r[3] = fd;
    file_call("cellFsClose")(cpu);
    check(cpu.r[3] == rr7::kOk, "close failed");
}
void full_table() {
    std::vector<uint32_t> descriptors;
    uint32_t fd = 0;
    for (unsigned i = 0; i < 253; ++i) {
        check(open_file("probe.bin", 0, fd) == rr7::kOk, "fill descriptor table");
        descriptors.push_back(fd);
    }
    check(open_file("probe.bin", 1 | 01000, fd) == 0x8001002Bu, "full table must reject truncation");
    std::ifstream input(info.hdd / "probe.bin", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    check(bytes == "hello", "failed open truncated the existing file");
    check(open_file("new.bin", 1 | 0100, fd) == 0x8001002Bu, "full table must reject creation");
    check(!std::filesystem::exists(info.hdd / "new.bin"), "failed open created a file");
    for (uint32_t descriptor : descriptors) close_file(descriptor);
}
void read_close() {
    uint32_t fd = 0;
    check(open_file("probe.bin", 0, fd) == rr7::kOk, "open for read/close");
    reset_gate();
    block_read = true;
    std::thread reader([fd] {
        Cpu cpu;
        cpu.r[3] = fd; cpu.r[4] = 0x3000; cpu.r[5] = 5; cpu.r[6] = 0x4000;
        file_call("cellFsRead")(cpu);
        check(cpu.r[3] == rr7::kOk, "read failed");
    });
    wait_entered();
    std::promise<void> started;
    auto ready = started.get_future();
    auto closer = std::async(std::launch::async, [fd, &started] {
        started.set_value();
        close_file(fd);
    });
    ready.wait();
    check(closer.wait_for(200ms) == std::future_status::timeout, "close released the file during an active read");
    release();
    reader.join();
    check(closer.wait_for(5s) == std::future_status::ready, "close remained blocked after read");
    closer.get();
    block_read = false;
    check(rr7::load64(0x4000) == 5 && std::memcmp(memory.data() + 0x3000, "hello", 5) == 0, "read data changed");
}
uint32_t create_thread(bool joinable) {
    reset_gate();
    rr7::store32(0x5000, 0x1234);
    rr7::store32(0x5004, 0);
    Cpu cpu;
    cpu.r[3] = 0x5100; cpu.r[4] = 0x5000; cpu.r[6] = 1000; cpu.r[7] = 0x10000; cpu.r[8] = joinable ? 1 : 0;
    for (const auto& entry : rr7::thread_imports())
        if (std::strcmp(entry.name, "sys_ppu_thread_create") == 0) entry.handler(cpu);
    check(cpu.r[3] == rr7::kOk, "thread creation failed");
    wait_entered();
    return static_cast<uint32_t>(rr7::load64(0x5100));
}
void wait_removed(uint32_t id) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        Cpu cpu;
        cpu.r[3] = id; cpu.r[4] = 0x5200;
        thread_call(48)(cpu);
        if (cpu.r[3] == rr7::kEsrch) return;
        check(std::chrono::steady_clock::now() < deadline, "detached thread was not retired");
        std::this_thread::yield();
    }
}
void threads() {
    uint32_t id = create_thread(true);
    Cpu cpu;
    cpu.r[3] = id;
    thread_call(45)(cpu);
    check(cpu.r[3] == rr7::kOk, "detach running thread");
    release();
    wait_removed(id);  // previously the worker detached twice and terminated
    id = create_thread(false);
    release();
    wait_removed(id);
    id = create_thread(true);
    release();
    cpu.r[3] = id; cpu.r[4] = 0x5300;
    thread_call(44)(cpu);
    check(cpu.r[3] == rr7::kOk && rr7::load64(0x5300) == 42, "join preserves exit value");
}
}  // namespace

namespace rr7 {
uint8_t* guest_memory() { return memory.data(); }
const GameInfo& game() { return info; }
uint32_t load32(uint32_t address) { uint32_t value; std::memcpy(&value, memory.data() + address, 4); return std::byteswap(value); }
uint64_t load64(uint32_t address) { uint64_t value; std::memcpy(&value, memory.data() + address, 8); return std::byteswap(value); }
void store32(uint32_t address, uint32_t value) { value = std::byteswap(value); std::memcpy(memory.data() + address, &value, 4); }
void store64(uint32_t address, uint64_t value) { value = std::byteswap(value); std::memcpy(memory.data() + address, &value, 8); }
void renderer_guest_access(uint32_t, uint64_t) { if (block_read) park(); }
uint32_t current_thread() { return thread_id; }
void set_current_thread(uint32_t id) { thread_id = id; }
void set_current_cpu(const Cpu*) {}
const qilin::rt::CpuHooks& guest_hooks() { static const qilin::rt::CpuHooks hooks{}; return hooks; }
TlsImage tls_image() { return {}; }
uint32_t allocate(uint32_t size, uint32_t alignment) {
    static uint32_t next = 0x8000;
    const uint32_t at = (next + alignment - 1) & ~(alignment - 1);
    next = at + size;
    check(next < memory.size(), "test guest allocations exhausted");
    return at;
}
void commit(uint32_t, uint32_t) {}  // the synthetic worker never uses its stack
void call_guest(Cpu& cpu, uint32_t) { park(); cpu.r[3] = 42; }
}  // namespace rr7

namespace qilin::rt {
void log(const char*, ...) {}
[[noreturn]] void fatal(const char* format, ...) { std::fprintf(stderr, "unexpected fatal: %s\n", format); std::abort(); }
}  // namespace qilin::rt

int main(int argc, char** argv) {
    check(argc == 2, "select open, read-close or threads");
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    info.hdd = std::filesystem::temp_directory_path() / ("qilin-rr7-io-" + std::to_string(stamp));
    std::filesystem::create_directories(info.hdd);
    { std::ofstream output(info.hdd / "probe.bin", std::ios::binary); output << "hello"; }
    const std::string mode = argv[1];
    if (mode == "open") full_table();
    else if (mode == "read-close") read_close();
    else if (mode == "threads") threads();
    else check(false, "unknown test case");
    std::filesystem::remove_all(info.hdd);
    std::puts("runtime I/O regression passed");
}

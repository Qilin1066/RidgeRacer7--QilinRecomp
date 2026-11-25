// When the host stops the process for a bad memory access (or anything
// else it would otherwise end silently for), say so: which guest address
// was touched, on which guest thread, and through which lifted functions
// the thread got there. Then stop, as for every other unknown.
//
// The functions are found from the host's return addresses: a lifted
// function is host code, so the table of lifted functions, sorted by host
// address, names the function each return address is in.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <thread>
#include <vector>

#include "gcm.hpp"
#include "kernel.hpp"
#include "qilin/rt/report.hpp"
#include "renderer.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define QILIN_HAS_BACKTRACE 1
#endif
#endif

namespace rr7 {

namespace {

struct HostFunction {
    uintptr_t host;
    uint32_t guest;
};
std::vector<HostFunction> g_by_host;  // sorted by host address
std::vector<uint32_t> g_starts;       // the lifted functions' guest addresses, sorted
uintptr_t g_guest_base = 0;
bool g_installed = false;

// RR7_WATCH_WRITES=<guest address, hex> (a development aid): the page holding it is made read-only; each write there
// faults, is remembered (the guest thread, and the last of its instructions to touch memory) and let through, and a
// watcher thread makes the page read-only again each millisecond, logging who wrote last whenever the word at the
// address goes from non-zero to zero.
std::atomic<bool> g_watching{false};
uint32_t g_watch_address = 0;
std::atomic<uint64_t> g_last_writer{0};  // thread << 32 | instruction

// Guest pages [from, to) made read-only (true) or readable and writable; false if the host refused (not committed).
bool protect_read_only(uint64_t from, uint64_t to, bool read_only);

bool watch_fault(uint32_t guest) {
    if (!g_watching.load(std::memory_order_relaxed)) return false;
    const uint32_t page = guest_page_size();
    if (guest / page != g_watch_address / page) return false;
    const Cpu* cpu = current_cpu();
    g_last_writer = uint64_t{current_thread()} << 32 | (cpu ? cpu->pc : 0u);
    const uint64_t from = uint64_t{guest} / page * page;
    protect_read_only(from, from + page, false);
    return true;
}

void start_write_watch() {
    const char* text = std::getenv("RR7_WATCH_WRITES");
    if (!text) return;
    g_watch_address = static_cast<uint32_t>(std::strtoul(text, nullptr, 16));
    g_watching = true;
    std::thread([] {
        const uint32_t page = guest_page_size();
        const uint64_t from = uint64_t{g_watch_address} / page * page;
        // (Not before the RSX's memory is set up: the page isn't the game's until then.)
        while (rsx_setup().io_size == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        uint32_t last = 0, writes_seen = 0;
        uint64_t last_writer = 0;
        for (;;) {
            protect_read_only(from, from + page, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const uint32_t now = load32(g_watch_address);
            const uint64_t writer = g_last_writer.load();
            if (writer != last_writer && writes_seen < 40) {
                ++writes_seen;
                qilin::rt::log("(watch) %08x is %08x; written last by thread %08x, its instruction at %08x", g_watch_address, now, static_cast<uint32_t>(writer >> 32),
                               static_cast<uint32_t>(writer));
            }
            last_writer = writer;
            if (last != 0 && now == 0)
                qilin::rt::log("(watch) %08x became zero (was %08x); written last by thread %08x, its instruction at %08x", g_watch_address, last,
                               static_cast<uint32_t>(writer >> 32), static_cast<uint32_t>(writer));
            last = now;
        }
    }).detach();
}

// The guest address a host address is, if it is in the guest's memory.
bool to_guest(uintptr_t address, uint32_t& guest) {
    if (!g_guest_base || address < g_guest_base || address - g_guest_base >= (uint64_t{1} << 32)) return false;
    guest = static_cast<uint32_t>(address - g_guest_base);
    return true;
}

// Prints the lifted functions these host addresses are in, innermost first.
void print_guest_stack(const uintptr_t* frames, size_t count) {
    if (const Cpu* cpu = current_cpu(); cpu && cpu->pc) {
        std::fprintf(stderr, "  the last of the game's instructions to touch memory was at %08x\n", cpu->pc);
        // (The registers are as the lifted code last wrote them back: current only in code lifted with --registers.)
        for (unsigned i = 0; i < 32; i += 4)
            std::fprintf(stderr, "  r%-2u %016llx  r%-2u %016llx  r%-2u %016llx  r%-2u %016llx\n", i, static_cast<unsigned long long>(cpu->r[i]), i + 1,
                         static_cast<unsigned long long>(cpu->r[i + 1]), i + 2, static_cast<unsigned long long>(cpu->r[i + 2]), i + 3, static_cast<unsigned long long>(cpu->r[i + 3]));
    }
    // A development aid: RR7_DUMP=<address>[,<address>...] in hex prints 96 bytes of guest memory at each.
    if (const char* list = std::getenv("RR7_DUMP")) {
        while (*list) {
            char* end = nullptr;
            const uint32_t at = static_cast<uint32_t>(std::strtoul(list, &end, 16));
            if (end == list) break;
            std::fprintf(stderr, "  memory at %08x:", at);
            for (uint32_t i = 0; i < 96; ++i) std::fprintf(stderr, "%s%02x", i % 4 == 0 ? " " : "", guest_memory()[at + i]);
            std::fputc(10, stderr);
            list = *end == ',' ? end + 1 : end;
        }
    }
    PastCall past[16];
    const size_t calls = past_calls(past, 16);
    for (size_t i = 0; i < calls; ++i) {
        if (past[i].what) std::fprintf(stderr, "  %s it called %s (from %08x)\n", i ? "before that" : "its last call to the system:", past[i].what, past[i].from);
        else std::fprintf(stderr, "  %s system call %u (at %08x)\n", i ? "before that" : "its last call to the system:", past[i].number, past[i].from);
    }
    if (g_by_host.empty()) return;
    const uintptr_t first = g_by_host.front().host, last = g_by_host.back().host + 0x100000;
    size_t shown = 0;
    for (size_t i = 0; i < count && shown < 24; ++i) {
        if (frames[i] < first || frames[i] >= last) continue;
        auto it = std::upper_bound(g_by_host.begin(), g_by_host.end(), frames[i], [](uintptr_t a, const HostFunction& f) { return a < f.host; });
        if (it == g_by_host.begin()) continue;
        --it;
        std::fprintf(stderr, "  %s the game's function %08x\n", shown == 0 ? "in" : "called from", it->guest);
        ++shown;
    }
    if (shown == 0) std::fprintf(stderr, "  (not in the game's code: in the runtime's own)\n");
}

void describe(const char* what, uintptr_t address, bool has_address) {
    if (has_address && address >= g_guest_base && address - g_guest_base < (uint64_t{1} << 32))
        std::fprintf(stderr, "stopped: %s at guest address %08x, which is not memory the game has (thread %08x)\n", what, static_cast<uint32_t>(address - g_guest_base),
                     current_thread());
    else if (has_address)
        std::fprintf(stderr, "stopped: %s at host address %p, outside the guest's memory (thread %08x)\n", what, reinterpret_cast<void*>(address), current_thread());
    else
        std::fprintf(stderr, "stopped: %s (thread %08x)\n", what, current_thread());
}

#if defined(_WIN32)

// Before anything else sees an access violation: one on a page the renderer made unreadable (a surface still to be
// written back) is the renderer's to resolve, and the access is made again.
LONG WINAPI on_first_chance(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    uint32_t guest = 0;
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record.NumberParameters < 2 || !to_guest(static_cast<uintptr_t>(record.ExceptionInformation[1]), guest))
        return EXCEPTION_CONTINUE_SEARCH;
    if (record.ExceptionInformation[0] == 1 && watch_fault(guest)) return EXCEPTION_CONTINUE_EXECUTION;
    return renderer_fault(guest) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    switch (record.ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        describe(record.ExceptionInformation[0] == 1 ? "a write" : "a read", static_cast<uintptr_t>(record.ExceptionInformation[1]), true);
        break;
    case EXCEPTION_STACK_OVERFLOW: describe("the host's stack is used up (the game's calls are too deep for it)", 0, false); break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: describe("the host met an instruction it can't run", 0, false); break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: describe("a division by zero in host code", 0, false); break;
    default:
        std::fprintf(stderr, "stopped: the host raised exception %08lx (thread %08x)\n", static_cast<unsigned long>(record.ExceptionCode), current_thread());
        break;
    }
    // Walk the stack from where it happened.
    CONTEXT context = *info->ContextRecord;
    uintptr_t frames[256];
    size_t count = 0;
    while (count < std::size(frames)) {
#if defined(_M_ARM64)
        const DWORD64 pc = context.Pc;
#else
        const DWORD64 pc = context.Rip;
#endif
        if (!pc) break;
        frames[count++] = static_cast<uintptr_t>(pc);
        DWORD64 image = 0;
        const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &image, nullptr);
        if (!function) break;
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, function, &context, &handler_data, &establisher, nullptr);
    }
    print_guest_stack(frames, count);
    std::fflush(stderr);
    std::_Exit(70);
}

void install() {
    if (!AddVectoredExceptionHandler(1, on_first_chance)) qilin::rt::fatal("the host refused an exception handler");
    SetUnhandledExceptionFilter(on_exception);
    ULONG guarantee = 0x10000;  // room to report a used-up stack
    SetThreadStackGuarantee(&guarantee);
}

#else

void on_signal(int signal, siginfo_t* info, void*) {
    // (A page the renderer made unreadable: its to resolve, and the access is made again when this returns.)
    if (uint32_t guest = 0; (signal == SIGSEGV || signal == SIGBUS) && to_guest(reinterpret_cast<uintptr_t>(info->si_addr), guest) && (watch_fault(guest) || renderer_fault(guest)))
        return;
    describe(signal == SIGSEGV || signal == SIGBUS ? "an access" : signal == SIGILL ? "the host met an instruction it can't run" : "an arithmetic fault in host code",
             reinterpret_cast<uintptr_t>(info->si_addr), signal == SIGSEGV || signal == SIGBUS);
#if defined(QILIN_HAS_BACKTRACE)
    void* raw[256];
    const int count = backtrace(raw, 256);
    uintptr_t frames[256];
    for (int i = 0; i < count; ++i) frames[i] = reinterpret_cast<uintptr_t>(raw[i]);
    print_guest_stack(frames, static_cast<size_t>(count));
#endif
    std::fflush(stderr);
    std::_Exit(70);
}

void install() {
    struct sigaction action {};
    action.sa_sigaction = on_signal;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(signal, &action, nullptr);
}

#endif

}  // namespace

void report_faults(std::span<const qilin::rt::LiftedFunction> functions) {
    g_guest_base = reinterpret_cast<uintptr_t>(guest_memory());
    g_by_host.reserve(functions.size());
    for (const qilin::rt::LiftedFunction& f : functions) g_by_host.push_back({reinterpret_cast<uintptr_t>(f.fn), f.address});
    std::ranges::sort(g_by_host, {}, &HostFunction::host);
    g_starts.reserve(functions.size());
    for (const qilin::rt::LiftedFunction& f : functions) g_starts.push_back(f.address);
    std::ranges::sort(g_starts);
    install();
    g_installed = true;
    start_write_watch();
}

bool faults_handled() { return g_installed; }

uint32_t lifted_function_at(uint32_t address) {
    const auto it = std::ranges::upper_bound(g_starts, address);
    return it == g_starts.begin() ? 0 : *std::prev(it);
}

uint32_t guest_page_size() {
    static const uint32_t size = [] {
#if defined(_WIN32)
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<uint32_t>(info.dwPageSize);
#else
        return static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
#endif
    }();
    return size;
}

namespace {
bool protect_read_only(uint64_t from, uint64_t to, bool read_only) {
    void* const at = guest_memory() + from;
#if defined(_WIN32)
    DWORD old = 0;
    return VirtualProtect(at, static_cast<SIZE_T>(to - from), read_only ? PAGE_READONLY : PAGE_READWRITE, &old) != 0;
#else
    return mprotect(at, static_cast<size_t>(to - from), read_only ? PROT_READ : PROT_READ | PROT_WRITE) == 0;
#endif
}
}  // namespace

void guest_protect(uint64_t from, uint64_t to, bool access) {
    void* const at = guest_memory() + from;
#if defined(_WIN32)
    DWORD old = 0;
    if (!VirtualProtect(at, static_cast<SIZE_T>(to - from), access ? PAGE_READWRITE : PAGE_NOACCESS, &old))
        qilin::rt::fatal("the host refused to make guest pages %08llx to %08llx %s (error %lu)", static_cast<unsigned long long>(from), static_cast<unsigned long long>(to),
                         access ? "readable" : "unreadable", static_cast<unsigned long>(GetLastError()));
#else
    if (mprotect(at, static_cast<size_t>(to - from), access ? PROT_READ | PROT_WRITE : PROT_NONE) != 0)
        qilin::rt::fatal("the host refused to make guest pages %08llx to %08llx %s", static_cast<unsigned long long>(from), static_cast<unsigned long long>(to),
                         access ? "readable" : "unreadable");
#endif
}

}  // namespace rr7

// When the host stops the process for a bad memory access (or anything
// else it would otherwise end silently for), say so: which guest address
// was touched, on which guest thread, and through which lifted functions
// the thread got there. Then stop, as for every other unknown.
//
// The functions are found from the host's return addresses: a lifted
// function is host code, so the table of lifted functions, sorted by host
// address, names the function each return address is in.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
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
uintptr_t g_guest_base = 0;

// Prints the lifted functions these host addresses are in, innermost first.
void print_guest_stack(const uintptr_t* frames, size_t count) {
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
    SetUnhandledExceptionFilter(on_exception);
    ULONG guarantee = 0x10000;  // room to report a used-up stack
    SetThreadStackGuarantee(&guarantee);
}

#else

void on_signal(int signal, siginfo_t* info, void*) {
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
    install();
}

}  // namespace rr7

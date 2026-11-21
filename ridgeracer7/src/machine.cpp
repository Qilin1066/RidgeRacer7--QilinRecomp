#include "machine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "import_names.hpp"
#include "kernel.hpp"
#include "qilin/rt/memory.hpp"
#include "qilin/rt/report.hpp"
#include "syscall_names.hpp"

namespace rr7 {

using qilin::rt::fatal;
using qilin::rt::log;

namespace {

struct Import {
    std::string library;
    std::string name;  // empty if the id has no name
    uint32_t id = 0;
    void (*handler)(Cpu&) = nullptr;
};

struct Machine {
    qilin::rt::AddressSpace space;
    std::span<const qilin::rt::LiftedFunction> functions;
    std::vector<Import> imports;
    std::chrono::steady_clock::time_point started;
    uint32_t next_allocation = kRuntimeArea;
    GameInfo info;
    TlsImage tls;
    std::array<std::atomic<void (*)(Cpu&)>, kNativeFunctions> natives{};
    std::mutex natives_mutex;
    uint32_t natives_count = 0;
};
Machine* g_machine = nullptr;

qilin::rt::GuestFn find_function(uint32_t address) {
    const auto& functions = g_machine->functions;
    const auto it = std::ranges::lower_bound(functions, address, {}, &qilin::rt::LiftedFunction::address);
    return it != functions.end() && it->address == address ? it->fn : nullptr;
}

// ---- the hooks lifted code calls ----

void on_call(Cpu& c, uint32_t target) {
    if (target >= kImportCode && target < kImportCode + 4 * g_machine->imports.size() && target % 4 == 0) {
        const Import& import = g_machine->imports[(target - kImportCode) / 4];
        if (!import.handler)
            fatal("the game called %s (%s, id %08x), which this runtime doesn't provide yet; it would return to %08x", import.name.empty() ? "a function with no name" : import.name.c_str(),
                  import.library.c_str(), import.id, static_cast<uint32_t>(c.lr));
        thread_enters(import.name.c_str(), 0, static_cast<uint32_t>(c.lr));
        import.handler(c);
        thread_leaves();
        return;
    }
    if (target >= kNativeCode && target < kNativeCode + 4 * kNativeFunctions && target % 4 == 0) {
        const auto handler = g_machine->natives[(target - kNativeCode) / 4].load();
        if (!handler) fatal("a call to %08x, where the runtime has no function (it would return to %08x)", target, static_cast<uint32_t>(c.lr));
        handler(c);
        return;
    }
    if (target == kReturnToHost) fatal("a jump to the address a thread's first function returns to (LR %08x)", static_cast<uint32_t>(c.lr));
    const qilin::rt::GuestFn fn = find_function(target);
    if (!fn) fatal("a call through a register to %08x, which is not a function (it would return to %08x)", target, static_cast<uint32_t>(c.lr));
    fn(c);
}

void on_system_call(Cpu& c, uint32_t pc) {
    const uint32_t number = static_cast<uint32_t>(c.r[11]);
    if (const auto handler = find_native_system_call(number)) {
        thread_enters("a system call", number, pc);
        handler(c);
        thread_leaves();
        return;
    }
    const qilin::SystemCallName* name = qilin::system_call_name(number);
    fatal("system call %u (%s) at %08x, which this runtime doesn't provide yet", number, name ? std::string(name->name).c_str() : "no name", pc);
}

void on_trap(Cpu& c, uint32_t pc, const char* why) { fatal("at %08x: %s (LR %08x)", pc, why, static_cast<uint32_t>(c.lr)); }

// The time base counts 79,800,000 to the second on a PS3.
uint64_t on_time_base(Cpu&) {
    const auto elapsed = std::chrono::steady_clock::now() - g_machine->started;
    const uint64_t nanoseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    return nanoseconds / 1000 * 798 / 10 + nanoseconds % 1000 * 798 / 10000;
}

const qilin::rt::CpuHooks kHooks{on_call, on_system_call, on_trap, on_time_base};

}  // namespace

uint8_t* guest_memory() { return g_machine->space.base(); }

const GameInfo& game() { return g_machine->info; }

uint64_t system_time() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_machine->started).count());
}

void exit_process(int code) {
    log("the process ended with code %d", code);
    // At once: other threads may be running, and the objects they use must not be torn down under them.
    std::_Exit(code);
}

const qilin::rt::CpuHooks& guest_hooks() { return kHooks; }

void call_guest(Cpu& c, uint32_t code) {
    const qilin::rt::GuestFn fn = find_function(code);
    if (!fn) fatal("%08x is to be run, and is not among the lifted functions", code);
    fn(c);
}

TlsImage tls_image() { return g_machine->tls; }

uint32_t native_function(void (*handler)(Cpu&)) {
    uint32_t index;
    {
        const std::lock_guard lock(g_machine->natives_mutex);
        if (g_machine->natives_count == kNativeFunctions) fatal("the runtime has made more of its functions callable than it has room for");
        index = g_machine->natives_count++;
    }
    g_machine->natives[index].store(handler);
    const uint32_t descriptor = allocate(8, 8);
    store32(descriptor, kNativeCode + 4 * index);
    store32(descriptor + 4, 0);
    return descriptor;
}

void commit(uint32_t address, uint32_t size) {
    if (!g_machine->space.commit(address, size)) fatal("the host refused %u bytes of memory for the guest at %08x", size, address);
}

uint32_t allocate(uint32_t size, uint32_t alignment) {
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    const uint32_t at = (g_machine->next_allocation + alignment - 1) & ~(alignment - 1);
    if (size > kRuntimeAreaEnd - at) fatal("the runtime's own area of guest memory is full (%u more bytes wanted)", size);
    commit(at, size ? size : 1);
    g_machine->next_allocation = at + size;
    return at;
}

int run(const qilin::Elf& elf, std::span<const qilin::rt::LiftedFunction> functions, GameInfo info) {
    Machine machine;
    g_machine = &machine;
    machine.info = std::move(info);
    machine.functions = functions;
    machine.started = std::chrono::steady_clock::now();
    if (!machine.space.base()) fatal("the host refused the 4 GiB of address space the guest's memory needs");
    uint8_t* const memory = machine.space.base();
    report_faults(functions);

    // The program, at the addresses it was linked for.
    for (const qilin::ElfSegment& s : elf.segments()) {
        if (s.type != qilin::kPtLoad || s.memory_size == 0) continue;
        commit(static_cast<uint32_t>(s.address), static_cast<uint32_t>(s.memory_size));
        const auto bytes = elf.bytes_at(s.address, s.file_size);
        if (s.file_size && bytes.empty()) fatal("the program's segment at %08x isn't in the file", static_cast<uint32_t>(s.address));
        std::memcpy(memory + s.address, bytes.data(), bytes.size());
    }

    // The imports: each slot is given a descriptor whose code address is the import's own, in a
    // range where nothing is, so that the call through it arrives at on_call.
    for (const qilin::ImportLibrary& library : elf.imports())
        for (const qilin::ImportFunction& f : library.functions) {
            Import import;
            import.library = library.name;
            import.id = f.id;
            import.name = std::string(qilin::import_name(library.name, f.id));
            if (!import.name.empty()) import.handler = find_native_import(import.library, import.name);
            machine.imports.push_back(std::move(import));
        }
    commit(kImportDescriptors, static_cast<uint32_t>(8 * machine.imports.size()));
    Cpu cpu;
    cpu.mem = memory;
    cpu.hooks = &kHooks;
    {
        uint32_t index = 0;
        for (const qilin::ImportLibrary& library : elf.imports())
            for (const qilin::ImportFunction& f : library.functions) {
                const uint32_t descriptor = kImportDescriptors + 8 * index;
                qilin::rt::write32(cpu, descriptor, kImportCode + 4 * index);
                qilin::rt::write32(cpu, descriptor + 4, 0);
                qilin::rt::write32(cpu, f.slot, descriptor);
                ++index;
            }
    }
    size_t provided = 0;
    for (const Import& import : machine.imports) provided += import.handler != nullptr;
    log("the program is loaded: %zu functions, %zu imports (%zu provided natively)", functions.size(), machine.imports.size(), provided);

    // The first thread: its stack, the table of contents, and a return address that ends the process.
    const uint32_t stack_size = elf.process_param() ? elf.process_param()->primary_stack_size : 0x10000u;
    commit(kStackTop - stack_size, stack_size);
    set_current_thread(kFirstThreadId);
    add_thread(kFirstThreadId, elf.process_param() ? elf.process_param()->primary_priority : 1001);
    cpu.r[1] = kStackTop - 0x80;
    cpu.r[2] = elf.entry_toc();
    cpu.lr = kReturnToHost;
    // What the system hands a process's first thread. From the start-up code's own use of them (it passes r7 to r10
    // straight on to sys_initialize_tls, and keeps r12) and public descriptions: the arguments, the thread's id,
    // the thread-local image's address and its two sizes, and the heap's page size.
    {
        static constexpr char kPath[] = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";
        const uint32_t path = allocate(sizeof kPath, 1), arguments = allocate(16, 8);
        std::memcpy(memory + path, kPath, sizeof kPath);
        qilin::rt::write32(cpu, arguments, path);  // argv[0]; argv[1] and the environment's only entry are null
        cpu.r[3] = 1;
        cpu.r[4] = arguments;
        cpu.r[5] = arguments + 8;
        cpu.r[7] = kFirstThreadId;
        if (const auto& tls = elf.tls()) {
            machine.tls = {tls->address, tls->file_size, tls->memory_size};
            cpu.r[8] = tls->address;
            cpu.r[9] = tls->file_size;
            cpu.r[10] = tls->memory_size;
        }
        cpu.r[12] = elf.process_param() ? elf.process_param()->malloc_page_size : 0x100000u;
    }
    start_watch();
    set_current_cpu(&cpu);
    const qilin::rt::GuestFn entry = find_function(elf.entry_code());
    if (!entry) fatal("the program's entry point %08x is not among the lifted functions", elf.entry_code());
    entry(cpu);
    log("the entry point returned");
    exit_process(static_cast<int>(cpu.r[3]));
}

}  // namespace rr7

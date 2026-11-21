#include "machine.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "import_names.hpp"
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
};
Machine* g_machine = nullptr;

qilin::rt::GuestFn find_function(uint32_t address) {
    const auto& functions = g_machine->functions;
    const auto it = std::ranges::lower_bound(functions, address, {}, &qilin::rt::LiftedFunction::address);
    return it != functions.end() && it->address == address ? it->fn : nullptr;
}

constexpr uint64_t kFirstThreadId = 0x01000000;  // *a guess at the shape of a thread id; nothing has compared it yet*

struct ProcessExit {
    int code;
};

// ---- the hooks lifted code calls ----

void on_call(Cpu& c, uint32_t target) {
    if (target >= kImportCode && target < kImportCode + 4 * g_machine->imports.size() && target % 4 == 0) {
        const Import& import = g_machine->imports[(target - kImportCode) / 4];
        if (!import.handler)
            fatal("the game called %s (%s, id %08x), which this runtime doesn't provide yet; it would return to %08x", import.name.empty() ? "a function with no name" : import.name.c_str(),
                  import.library.c_str(), import.id, static_cast<uint32_t>(c.lr));
        import.handler(c);
        return;
    }
    if (target == kReturnToHost) throw ProcessExit{static_cast<int>(c.r[3])};
    const qilin::rt::GuestFn fn = find_function(target);
    if (!fn) fatal("a call through a register to %08x, which is not a function (it would return to %08x)", target, static_cast<uint32_t>(c.lr));
    fn(c);
}

void on_system_call(Cpu& c, uint32_t pc) {
    const uint32_t number = static_cast<uint32_t>(c.r[11]);
    for (const NativeSystemCall& call : native_system_calls())
        if (call.number == number) {
            call.handler(c);
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

void commit(uint32_t address, uint32_t size) {
    if (!g_machine->space.commit(address, size)) fatal("the host refused %u bytes of memory for the guest at %08x", size, address);
}

uint32_t allocate(uint32_t size, uint32_t alignment) {
    const uint32_t at = (g_machine->next_allocation + alignment - 1) & ~(alignment - 1);
    if (size > kRuntimeAreaEnd - at) fatal("the runtime's own area of guest memory is full (%u more bytes wanted)", size);
    commit(at, size ? size : 1);
    g_machine->next_allocation = at + size;
    return at;
}

int run(const qilin::Elf& elf, std::span<const qilin::rt::LiftedFunction> functions) {
    Machine machine;
    g_machine = &machine;
    machine.functions = functions;
    machine.started = std::chrono::steady_clock::now();
    if (!machine.space.base()) fatal("the host refused the 4 GiB of address space the guest's memory needs");
    uint8_t* const memory = machine.space.base();

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
    const auto natives = native_imports();
    for (const qilin::ImportLibrary& library : elf.imports())
        for (const qilin::ImportFunction& f : library.functions) {
            Import import;
            import.library = library.name;
            import.id = f.id;
            import.name = std::string(qilin::import_name(library.name, f.id));
            for (const NativeImport& native : natives)
                if (import.library == native.library && import.name == native.name) import.handler = native.handler;
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
            cpu.r[8] = tls->address;
            cpu.r[9] = tls->file_size;
            cpu.r[10] = tls->memory_size;
        }
        cpu.r[12] = elf.process_param() ? elf.process_param()->malloc_page_size : 0x100000u;
    }
    const qilin::rt::GuestFn entry = find_function(elf.entry_code());
    if (!entry) fatal("the program's entry point %08x is not among the lifted functions", elf.entry_code());
    try {
        entry(cpu);
    } catch (const ProcessExit& exit) {
        log("the process ended with code %d", exit.code);
        return exit.code;
    }
    log("the entry point returned %d", static_cast<int>(cpu.r[3]));
    return static_cast<int>(cpu.r[3]);
}

}  // namespace rr7

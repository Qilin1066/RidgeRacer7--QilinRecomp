// cellSpurs: the system's manager of SPU work. A game makes a SPURS
// instance (a group of SPU threads running the library's own kernel), then
// tasksets and tasks for it to run.
//
// The kernel is the library's, an SPU program in the console's system
// software and not in the game; so SPURS is provided here natively, and
// what it runs is the game's own SPU programs, lifted. What is here so far
// is the instance itself. Tasksets, tasks and event flags stop the game by
// name until they are provided.
#include <map>
#include <mutex>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

// The library's errors (the SDK's CELL_SPURS_CORE_ERROR_...).
constexpr uint64_t kInval = 0x80410702u, kNullPointer = 0x80410711u, kAlign = 0x80410710u, kStat = 0x8041070fu;

struct Instance {
    uint32_t spus = 0;
    int32_t spu_priority = 0, ppu_priority = 0;
    bool exit_if_no_work = false;
    std::map<uint32_t, uint32_t> ports;  // SPU port -> the event queue attached to it
};
std::mutex g_mutex;
std::map<uint32_t, Instance> g_instances;  // by the address of the game's CellSpurs

struct Taskset {
    uint32_t spurs = 0;
    uint64_t argument = 0;
    uint8_t priority[8] = {};
    uint32_t max_contention = 0;
};
std::map<uint32_t, Taskset> g_tasksets;  // by the address of the game's CellSpursTaskset

// cellSpursInitialize(CellSpurs* spurs, SPUs, the SPU threads' priority, the handler thread's priority, exit when there is no work)
void cellSpursInitialize(Cpu& c) {
    const uint32_t spurs = static_cast<uint32_t>(c.r[3]), spus = static_cast<uint32_t>(c.r[4]);
    if (!spurs) {
        c.r[3] = kNullPointer;
        return;
    }
    if (spurs & 127) {
        c.r[3] = kAlign;
        return;
    }
    if (spus == 0 || spus > 6) {
        c.r[3] = kInval;
        return;
    }
    const std::lock_guard lock(g_mutex);
    if (g_instances.contains(spurs)) {
        c.r[3] = kStat;
        return;
    }
    const Instance instance{spus, static_cast<int32_t>(c.r[5]), static_cast<int32_t>(c.r[6]), c.r[7] != 0, {}};
    qilin::rt::log("the game starts SPURS at %08x: %u SPUs, SPU priority %d, PPU priority %d%s", spurs, spus, instance.spu_priority, instance.ppu_priority,
                   instance.exit_if_no_work ? ", exiting when there is no work" : "");
    g_instances.emplace(spurs, instance);
    c.r[3] = kOk;
}

// cellSpursAttachLv2EventQueue(spurs, queue, uint8_t* port, dynamic)
// Lets the SPURS instance's SPU programs send events to a queue, through one of 64 ports. With
// `dynamic` the library picks a free port from 16 up and writes it to *port; otherwise *port is the one wanted.
void cellSpursAttachLv2EventQueue(Cpu& c) {
    const uint32_t spurs = static_cast<uint32_t>(c.r[3]), queue = static_cast<uint32_t>(c.r[4]), port_at = static_cast<uint32_t>(c.r[5]);
    const bool dynamic = c.r[6] != 0;
    if (!spurs || !port_at) {
        c.r[3] = kNullPointer;
        return;
    }
    const std::lock_guard lock(g_mutex);
    const auto it = g_instances.find(spurs);
    if (it == g_instances.end()) {
        c.r[3] = kStat;
        return;
    }
    Instance& instance = it->second;
    uint32_t port = guest_memory()[port_at];
    if (dynamic) {
        for (port = 16; port < 64 && instance.ports.contains(port); ++port) {}
        if (port == 64) qilin::rt::fatal("cellSpursAttachLv2EventQueue: every dynamic port of the SPURS instance at %08x is taken", spurs);
    } else if (port > 63 || instance.ports.contains(port)) {
        qilin::rt::fatal("cellSpursAttachLv2EventQueue: port %u of the SPURS instance at %08x can't be had (the library's answer to that isn't known here)", port, spurs);
    }
    instance.ports[port] = queue;
    guest_memory()[port_at] = static_cast<uint8_t>(port);
    c.r[3] = kOk;
}

// cellSpursCreateTaskset(spurs, CellSpursTaskset* taskset, argument, const uint8_t priority[8], the most SPUs to run it at once)
// A taskset is a group of up to 32 tasks which the instance's SPUs take up by the priority given for each SPU (0: never).
void cellSpursCreateTaskset(Cpu& c) {
    const uint32_t spurs = static_cast<uint32_t>(c.r[3]), taskset = static_cast<uint32_t>(c.r[4]), priority = static_cast<uint32_t>(c.r[6]);
    if (!spurs || !taskset || !priority) {
        c.r[3] = kNullPointer;
        return;
    }
    if ((spurs & 127) || (taskset & 127)) {
        c.r[3] = kAlign;
        return;
    }
    const std::lock_guard lock(g_mutex);
    if (!g_instances.contains(spurs) || g_tasksets.contains(taskset)) {
        c.r[3] = kStat;
        return;
    }
    Taskset t;
    t.spurs = spurs;
    t.argument = c.r[5];
    for (uint32_t i = 0; i < 8; ++i) t.priority[i] = guest_memory()[priority + i];
    t.max_contention = static_cast<uint32_t>(c.r[7]);
    qilin::rt::log("the game makes a SPURS taskset at %08x: priorities %u %u %u %u %u %u %u %u, on %u SPUs at most", taskset, t.priority[0], t.priority[1], t.priority[2],
                   t.priority[3], t.priority[4], t.priority[5], t.priority[6], t.priority[7], t.max_contention);
    g_tasksets.emplace(taskset, t);
    c.r[3] = kOk;
}

// cellSpursCreateTask(taskset, uint32_t* task id, the task's SPU program (an ELF), context save area, its size, CellSpursTaskLsPattern*, CellSpursTaskArgument*)
// Not provided yet: it needs the SPU programs lifted. It says what it was asked, for the work ahead.
void cellSpursCreateTask(Cpu& c) {
    const uint32_t elf = static_cast<uint32_t>(c.r[5]), argument = static_cast<uint32_t>(c.r[9]);
    qilin::rt::fatal("the game creates a SPURS task from the SPU program at %08x (taskset %08x, context %08x of %u bytes, pattern %08x, argument %08x %08x %08x %08x), "
                     "and running SPU programs isn't provided yet; it would return to %08x",
                     elf, static_cast<uint32_t>(c.r[3]), static_cast<uint32_t>(c.r[6]), static_cast<uint32_t>(c.r[7]), static_cast<uint32_t>(c.r[8]),
                     argument ? load32(argument) : 0, argument ? load32(argument + 4) : 0, argument ? load32(argument + 8) : 0, argument ? load32(argument + 12) : 0,
                     static_cast<uint32_t>(c.lr));
}

constexpr NativeImport kImports[] = {
    {"cellSpurs", "cellSpursCreateTaskset", cellSpursCreateTaskset},
    {"cellSpurs", "cellSpursCreateTask", cellSpursCreateTask},
    {"cellSpurs", "cellSpursAttachLv2EventQueue", cellSpursAttachLv2EventQueue},
    {"cellSpurs", "cellSpursInitialize", cellSpursInitialize},
};

}  // namespace

std::span<const NativeImport> spurs_imports() { return kImports; }

}  // namespace rr7

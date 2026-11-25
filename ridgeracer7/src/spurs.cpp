// cellSpurs: the system's manager of SPU work. A game makes a SPURS
// instance (a group of SPU threads running the library's own kernel), then
// tasksets and tasks for it to run.
//
// The kernel is the library's, an SPU program in the console's system
// software and not in the game; so SPURS is provided here natively, and
// what it runs is the game's own SPU programs, lifted. What is here so far
// is the instance itself. Tasksets, tasks and event flags stop the game by
// name until they are provided.
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "events.hpp"
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
    uint32_t workloads = 0;              // tasksets made: each is a workload of the instance's, numbered from 0
};
std::mutex g_mutex;
std::map<uint32_t, Instance> g_instances;  // by the address of the game's CellSpurs

struct Taskset {
    uint32_t spurs = 0;
    uint64_t argument = 0;
    uint8_t priority[8] = {};
    uint32_t max_contention = 0;
    uint32_t running = 0;      // tasks not yet exited
    uint32_t started = 0;      // which tasks (by id, bit 0 task 0) this runtime has started and not seen end
    uint32_t workload = 0;
    bool shut_down = false;
};

// A taskset's first 128 bytes, as the SDK's code in the tasks reads and changes them (docs/runtime.md): masks of one bit a
// task (task 0 the top bit) at +0x00 running, +0x10 ready, +0x20 pending, +0x30 enabled, +0x40 signalled, +0x50 waiting;
// +0x60 the instance's address (as the kernel's 0x1C0); +0x74 the workload's number. Changed under the SPU's atomic lock.
constexpr uint32_t kRunning = 0x00, kPending = 0x20, kEnabled = 0x30, kSignalled = 0x40, kWaiting = 0x50;
// A task's record, 48 bytes at +0x80 + 48 * id: its argument (16 bytes), its ELF's 64-bit address, its context save area,
// its local-store pattern (as the SDK's SPU-side cellSpursCreateTask writes one, at 0x3d40 in LightBlurFrameMove_r.elf;
// it then sets the task's bit at +0x20 and tells the kernel the workload has work).
constexpr uint32_t kTaskRecords = 0x80, kTaskRecord = 48;
void watch_atomic_put(uint32_t line);

// RR7_SPURS=1: tasks started, ended, waiting and woken, and the PPU's waits on event flags, logged.
bool spurs_log() {
    static const bool on = std::getenv("RR7_SPURS") != nullptr;
    return on;
}

bool task_bit(uint32_t taskset, uint32_t mask, uint32_t id) { return (guest_memory()[taskset + mask + id / 8] >> (7 - id % 8)) & 1; }
void set_task_bit(uint32_t taskset, uint32_t mask, uint32_t id, bool on) {
    uint8_t& b = guest_memory()[taskset + mask + id / 8];
    const uint8_t bit = static_cast<uint8_t>(0x80u >> (id % 8));
    b = on ? static_cast<uint8_t>(b | bit) : static_cast<uint8_t>(b & ~bit);
}
std::condition_variable g_changed;  // a task exited (under g_mutex)

// An event flag. It lives in the game's memory in the SDK's layout, since the game's SPU tasks change it themselves, with the
// SDK's code linked into them (seen in LightBlurFrameMove_r.elf at 0x46d8: an atomic update of the 128-byte line, then an
// event thrown to the PPU). What that code reads and writes, so what this runtime keeps there:
//   +0 the sixteen event bits; +4 the bits the PPU waits for and +6 how (0 any of them, 1 all); +7 set when the PPU is
//   woken, with the bits it got at +0x30; +12 the SPU port to wake it through; +14 the direction (0 SPU to SPU, 1 SPU to
//   PPU, 2 PPU to SPU: an SPU setting that is refused); +15 the clearing (0: the bits a waiter gets are cleared).
// *The port is 0xff until a queue is attached: a stated choice (the task's code sends to no port above 63).*
constexpr uint32_t kFlagEvents = 0, kFlagPpuMask = 4, kFlagPpuMode = 6, kFlagPpuWoken = 7, kFlagPort = 12, kFlagDirection = 14, kFlagClear = 15,
                   kFlagPpuGot = 0x30;
struct EventFlag {
    uint32_t spurs = 0, taskset = 0, queue = 0, port = 0;
};
std::map<uint32_t, EventFlag> g_flags;  // by the address of the game's CellSpursEventFlag
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
    set_spu_atomic_watch(watch_atomic_put);
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
    Instance& instance = g_instances.at(spurs);
    if (instance.workloads >= 16) qilin::rt::fatal("a 17th workload in the SPURS instance at %08x", spurs);
    t.workload = instance.workloads++;
    {
        const std::lock_guard atomic(spu_atomic_mutex());
        std::memset(guest_memory() + taskset, 0, 128);
        store32(taskset + 0x64, spurs);
        store32(taskset + 0x74, t.workload);
    }
    qilin::rt::log("the game makes a SPURS taskset at %08x: priorities %u %u %u %u %u %u %u %u, on %u SPUs at most", taskset, t.priority[0], t.priority[1], t.priority[2],
                   t.priority[3], t.priority[4], t.priority[5], t.priority[6], t.priority[7], t.max_contention);
    g_tasksets.emplace(taskset, t);
    c.r[3] = kOk;
}

// Starts task `id` of a taskset from its record, on a host thread (spu.cpp), with the kernel's calls answered here.
void start_task(uint32_t taskset, uint32_t id) {
    uint32_t spurs, workload, elf;
    std::array<uint32_t, 4> r3{}, r4{};
    {
        const std::lock_guard lock(g_mutex);
        Taskset& t = g_tasksets.at(taskset);
        spurs = t.spurs, workload = t.workload;
        r4[0] = static_cast<uint32_t>(t.argument >> 32), r4[1] = static_cast<uint32_t>(t.argument);
        ++t.running;
        t.started |= 1u << id;
        const std::lock_guard atomic(spu_atomic_mutex());
        const uint32_t record = taskset + kTaskRecords + kTaskRecord * id;
        for (unsigned i = 0; i < 4; ++i) r3[i] = load32(record + 4 * i);
        if (load32(record + 16) != 0) qilin::rt::fatal("taskset %08x task %u: an ELF above 4 GiB", taskset, id);
        elf = load32(record + 20);
        set_task_bit(taskset, kPending, id, false);
        set_task_bit(taskset, kRunning, id, true);
    }
    const auto port_queue = [spurs](uint32_t port) -> uint32_t {
        const std::lock_guard lock(g_mutex);
        const auto it = g_instances.find(spurs);
        if (it == g_instances.end()) return 0;
        const auto p = it->second.ports.find(port);
        return p == it->second.ports.end() ? 0 : p->second;
    };
    if (spurs_log()) qilin::rt::log("(spurs) taskset %08x starts task %u: the program at %08x, argument %08x %08x %08x %08x", taskset, id, elf, r3[0], r3[1], r3[2], r3[3]);
    SpuTaskSetup setup;
    setup.image = elf, setup.spurs = spurs, setup.taskset = taskset, setup.workload = workload, setup.id = id, setup.r3 = r3, setup.r4 = r4;
    setup.port_queue = port_queue;
    setup.exited = [taskset, id](uint32_t) {
        if (spurs_log()) qilin::rt::log("(spurs) taskset %08x task %u ends", taskset, id);
        // The task's bits and this runtime's record of it change together (under both locks, as watch_atomic_put takes
        // them): otherwise a task made again with the same id in between would be missed, or taken for this one.
        const std::lock_guard lock(g_mutex);
        Taskset& t = g_tasksets.at(taskset);
        {
            const std::lock_guard atomic(spu_atomic_mutex());
            for (const uint32_t mask : {kRunning, kPending, kEnabled, kSignalled, kWaiting}) set_task_bit(taskset, mask, id, false);
            --t.running;
            t.started &= ~(1u << id);
            spu_atomic_changed().notify_all();
        }
        g_changed.notify_all();
    };
    // The kernel's wait: a task not yet signalled is waiting, not running, until it is (cellSpursSendSignal in the tasks sets
    // the signalled bit); the signal is used up.
    setup.wait_signal = [taskset, id] {
        std::unique_lock atomic(spu_atomic_mutex());
        if (spurs_log()) qilin::rt::log("(spurs) taskset %08x task %u waits%s", taskset, id, task_bit(taskset, kSignalled, id) ? " (signalled already)" : "");
        if (!task_bit(taskset, kSignalled, id)) {
            set_task_bit(taskset, kRunning, id, false);
            set_task_bit(taskset, kWaiting, id, true);
            spu_atomic_changed().notify_all();
            spu_atomic_changed().wait(atomic, [&] { return task_bit(taskset, kSignalled, id); });
            set_task_bit(taskset, kWaiting, id, false);
            set_task_bit(taskset, kRunning, id, true);
            if (spurs_log()) qilin::rt::log("(spurs) taskset %08x task %u is signalled", taskset, id);
        }
        set_task_bit(taskset, kSignalled, id, false);
    };
    start_spu_task(std::move(setup));
}

// After an SPU program's atomic put: if the line is a taskset's, the tasks the program has marked at +0x20 and this runtime
// hasn't started (made by a task, with the SDK's cellSpursCreateTask) are started.
void watch_atomic_put(uint32_t line) {
    uint32_t fresh = 0;
    {
        const std::lock_guard lock(g_mutex);
        const auto it = g_tasksets.find(line);
        if (it == g_tasksets.end()) return;
        const std::lock_guard atomic(spu_atomic_mutex());
        for (uint32_t id = 0; id < 32; ++id)
            if (task_bit(line, kPending, id) && !(it->second.started & (1u << id))) {
                if (!task_bit(line, kEnabled, id)) qilin::rt::fatal("taskset %08x: task %u marked to run, and not enabled", line, id);
                // Taken here, under the lock: two SPUs' puts to the line at once would otherwise both start it.
                it->second.started |= 1u << id;
                fresh |= 1u << id;
            }
    }
    for (uint32_t id = 0; id < 32; ++id)
        if (fresh & (1u << id)) start_task(line, id);
}

// cellSpursCreateTask(taskset, uint32_t* task id, the task's SPU program (an ELF), context save area, its size, CellSpursTaskLsPattern*, CellSpursTaskArgument*)
// The task is run at once, on a host thread of its own, as an SPU program (spu.cpp): $3 its argument (16 bytes), $4 the
// taskset's (in the first doubleword), as the SDK's task entry main(qword, uint64) takes them. *Not as SPURS does it: no
// SPU is shared between tasks and none waits for one; a context save area is never used, since no task is ever
// swapped out. The game's tasks seen so far call nothing of a SPURS kernel and end as SPU threads do (stop 0x102).*
void cellSpursCreateTask(Cpu& c) {
    const uint32_t taskset = static_cast<uint32_t>(c.r[3]), id_out = static_cast<uint32_t>(c.r[4]), elf = static_cast<uint32_t>(c.r[5]),
                   argument = static_cast<uint32_t>(c.r[9]);
    if (!taskset || !id_out || !elf) {
        c.r[3] = kNullPointer;
        return;
    }
    uint32_t id = 32;
    {
        const std::lock_guard lock(g_mutex);
        const auto it = g_tasksets.find(taskset);
        if (it == g_tasksets.end() || it->second.shut_down) qilin::rt::fatal("cellSpursCreateTask on taskset %08x, which isn't running", taskset);
        const std::lock_guard atomic(spu_atomic_mutex());
        // The first id free, as the SDK's code in the tasks picks one (the first not enabled).
        for (uint32_t i = 0; i < 32 && id == 32; ++i)
            if (!task_bit(taskset, kEnabled, i)) id = i;
        if (id == 32) qilin::rt::fatal("cellSpursCreateTask: a 33rd task in taskset %08x", taskset);
        const uint32_t record = taskset + kTaskRecords + kTaskRecord * id;
        std::memset(guest_memory() + record, 0, kTaskRecord);
        if (argument) std::memcpy(guest_memory() + record, guest_memory() + argument, 16);
        store32(record + 20, elf);
        set_task_bit(taskset, kEnabled, id, true);
    }
    store32(id_out, id);
    start_task(taskset, id);
    c.r[3] = kOk;
}

// cellSpursShutdownTaskset(taskset): no more tasks; it ends when those running have exited.
void cellSpursShutdownTaskset(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto it = g_tasksets.find(static_cast<uint32_t>(c.r[3]));
    if (it == g_tasksets.end()) qilin::rt::fatal("cellSpursShutdownTaskset on %08x, which is no taskset", static_cast<uint32_t>(c.r[3]));
    it->second.shut_down = true;
    c.r[3] = kOk;
}

// cellSpursJoinTaskset(taskset): waits for every task to have exited, and forgets the taskset.
void cellSpursJoinTaskset(Cpu& c) {
    const uint32_t taskset = static_cast<uint32_t>(c.r[3]);
    std::unique_lock lock(g_mutex);
    if (!g_tasksets.contains(taskset)) qilin::rt::fatal("cellSpursJoinTaskset on %08x, which is no taskset", taskset);
    g_changed.wait(lock, [&] { return g_tasksets.at(taskset).running == 0; });
    g_tasksets.erase(taskset);
    c.r[3] = kOk;
}

// cellSpursFinalize(spurs): the instance ends; its tasksets must be over.
void cellSpursFinalize(Cpu& c) {
    const uint32_t spurs = static_cast<uint32_t>(c.r[3]);
    const std::lock_guard lock(g_mutex);
    if (!g_instances.contains(spurs)) qilin::rt::fatal("cellSpursFinalize on %08x, which is no SPURS instance", spurs);
    for (const auto& [address, t] : g_tasksets)
        if (t.spurs == spurs) qilin::rt::fatal("cellSpursFinalize with taskset %08x not yet joined", address);
    g_instances.erase(spurs);
    c.r[3] = kOk;
}

// cellSpursDetachLv2EventQueue(spurs, port)
void cellSpursDetachLv2EventQueue(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto it = g_instances.find(static_cast<uint32_t>(c.r[3]));
    if (it == g_instances.end() || !it->second.ports.erase(static_cast<uint32_t>(c.r[4])))
        qilin::rt::fatal("cellSpursDetachLv2EventQueue: port %u of %08x isn't attached", static_cast<uint32_t>(c.r[4]), static_cast<uint32_t>(c.r[3]));
    c.r[3] = kOk;
}

// ---- event flags (above: their layout)

uint16_t half(uint32_t at) { return static_cast<uint16_t>((guest_memory()[at] << 8) | guest_memory()[at + 1]); }
void set_half(uint32_t at, uint16_t v) { guest_memory()[at] = static_cast<uint8_t>(v >> 8), guest_memory()[at + 1] = static_cast<uint8_t>(v); }

EventFlag& flag_at(uint32_t address, const char* call) {
    const auto it = g_flags.find(address);
    if (it == g_flags.end()) qilin::rt::fatal("%s on %08x, which is no event flag", call, address);
    return it->second;
}

// _cellSpursEventFlagInitialize(spurs, taskset, CellSpursEventFlag* flag, clear mode (0 automatic, 1 manual), direction)
void cellSpursEventFlagInitialize(Cpu& c) {
    const uint32_t spurs = static_cast<uint32_t>(c.r[3]), taskset = static_cast<uint32_t>(c.r[4]), flag = static_cast<uint32_t>(c.r[5]);
    const uint32_t clear_mode = static_cast<uint32_t>(c.r[6]), direction = static_cast<uint32_t>(c.r[7]);
    if (!flag || (!spurs && !taskset)) {
        c.r[3] = kNullPointer;
        return;
    }
    if (flag & 127) {
        c.r[3] = kAlign;
        return;
    }
    if (clear_mode > 1 || direction > 3) qilin::rt::fatal("_cellSpursEventFlagInitialize: clear mode %u, direction %u", clear_mode, direction);
    const std::lock_guard lock(g_mutex);
    uint32_t instance = spurs;
    if (!instance) {
        const auto it = g_tasksets.find(taskset);
        if (it == g_tasksets.end()) qilin::rt::fatal("_cellSpursEventFlagInitialize on taskset %08x, which isn't one", taskset);
        instance = it->second.spurs;
    }
    g_flags[flag] = EventFlag{instance, taskset, 0, 0};
    const std::lock_guard atomic(spu_atomic_mutex());
    std::memset(guest_memory() + flag, 0, 128);
    guest_memory()[flag + kFlagPort] = 0xff;
    guest_memory()[flag + kFlagDirection] = static_cast<uint8_t>(direction);
    guest_memory()[flag + kFlagClear] = static_cast<uint8_t>(clear_mode);
    c.r[3] = kOk;
}

// cellSpursEventFlagAttachLv2EventQueue(flag): a queue of the runtime's, attached to the SPURS instance on a free port
// from 16 (*as cellSpursAttachLv2EventQueue's dynamic ports: a stated choice*), the port written where the tasks read it.
void cellSpursEventFlagAttachLv2EventQueue(Cpu& c) {
    const uint32_t flag = static_cast<uint32_t>(c.r[3]);
    const std::lock_guard lock(g_mutex);
    EventFlag& f = flag_at(flag, "cellSpursEventFlagAttachLv2EventQueue");
    if (f.queue) qilin::rt::fatal("cellSpursEventFlagAttachLv2EventQueue on flag %08x, which has a queue already", flag);
    Instance& instance = g_instances.at(f.spurs);
    uint32_t port = 16;
    while (port < 64 && instance.ports.contains(port)) ++port;
    if (port == 64) qilin::rt::fatal("cellSpursEventFlagAttachLv2EventQueue: every port of the SPURS instance at %08x is taken", f.spurs);
    f.queue = make_event_queue(127);
    f.port = port;
    instance.ports[port] = f.queue;
    const std::lock_guard atomic(spu_atomic_mutex());
    guest_memory()[flag + kFlagPort] = static_cast<uint8_t>(port);
    c.r[3] = kOk;
}
// cellSpursEventFlagDetachLv2EventQueue(flag)
void cellSpursEventFlagDetachLv2EventQueue(Cpu& c) {
    const uint32_t flag = static_cast<uint32_t>(c.r[3]);
    const std::lock_guard lock(g_mutex);
    EventFlag& f = flag_at(flag, "cellSpursEventFlagDetachLv2EventQueue");
    if (!f.queue) qilin::rt::fatal("cellSpursEventFlagDetachLv2EventQueue on flag %08x, which has no queue", flag);
    if (const auto it = g_instances.find(f.spurs); it != g_instances.end()) it->second.ports.erase(f.port);
    destroy_event_queue(f.queue);  // (the game attaches and detaches each flag every frame)
    f.queue = 0;
    const std::lock_guard atomic(spu_atomic_mutex());
    guest_memory()[flag + kFlagPort] = 0xff;
    c.r[3] = kOk;
}

// cellSpursEventFlagSet(flag, bits) from the PPU: waking SPU tasks would need SPURS's signals, which aren't provided.
void cellSpursEventFlagSet(Cpu& c) {
    qilin::rt::fatal("cellSpursEventFlagSet from the PPU on flag %08x (waking SPU tasks isn't provided yet)", static_cast<uint32_t>(c.r[3]));
}

// cellSpursEventFlagWait(flag, uint16_t* bits, mode (0 any, 1 all)): as the tasks' own code treats a waiting PPU (above).
// The bits it gets, it gives back in *bits.
void cellSpursEventFlagWait(Cpu& c) {
    const uint32_t flag = static_cast<uint32_t>(c.r[3]), bits_at = static_cast<uint32_t>(c.r[4]), mode = static_cast<uint32_t>(c.r[5]);
    if (!bits_at) {
        c.r[3] = kNullPointer;
        return;
    }
    if (mode > 1) qilin::rt::fatal("cellSpursEventFlagWait: mode %u", mode);
    const uint16_t wanted = half(bits_at);
    uint32_t queue;
    {
        const std::lock_guard lock(g_mutex);
        queue = flag_at(flag, "cellSpursEventFlagWait").queue;
    }
    uint16_t got = 0;
    {
        const std::lock_guard atomic(spu_atomic_mutex());
        const uint8_t direction = guest_memory()[flag + kFlagDirection];
        if (direction != 1) qilin::rt::fatal("cellSpursEventFlagWait from the PPU on a flag of direction %u (only SPU to PPU, 1, is provided)", direction);
        if (half(flag + kFlagPpuMask) != 0) qilin::rt::fatal("cellSpursEventFlagWait on flag %08x, which the PPU waits on already", flag);
        const uint16_t events = half(flag + kFlagEvents);
        const uint16_t matched = events & wanted;
        if (spurs_log())
            qilin::rt::log("(spurs) the PPU waits on flag %08x for %04x (mode %u): events %04x, line %08x %08x %08x %08x", flag, wanted, mode, events, load32(flag), load32(flag + 4),
                           load32(flag + 8), load32(flag + 12));
        if (mode == 1 ? matched == wanted : matched != 0) {
            got = matched;
            if (guest_memory()[flag + kFlagClear] == 0) set_half(flag + kFlagEvents, static_cast<uint16_t>(events & ~matched));
        } else {
            if (!queue) qilin::rt::fatal("cellSpursEventFlagWait on flag %08x, which has no queue to be woken through", flag);
            set_half(flag + kFlagPpuMask, wanted);
            guest_memory()[flag + kFlagPpuMode] = static_cast<uint8_t>(mode);
            guest_memory()[flag + kFlagPpuWoken] = 0;
        }
    }
    // Woken by a task: it has cleared the mask, set +7 and left the bits at +0x30, and thrown an event to the queue.
    while (wanted && !got) {
        Event event;
        if (receive_event(queue, event) != kOk) qilin::rt::fatal("cellSpursEventFlagWait: the flag's queue is gone");
        const std::lock_guard atomic(spu_atomic_mutex());
        if (guest_memory()[flag + kFlagPpuWoken]) {
            got = half(flag + kFlagPpuGot);
            guest_memory()[flag + kFlagPpuWoken] = 0;
            set_half(flag + kFlagPpuGot, 0);
        }
    }
    if (spurs_log()) qilin::rt::log("(spurs) the PPU's wait on flag %08x gets %04x", flag, got);
    set_half(bits_at, got);
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"cellSpurs", "cellSpursShutdownTaskset", cellSpursShutdownTaskset},
    {"cellSpurs", "cellSpursJoinTaskset", cellSpursJoinTaskset},
    {"cellSpurs", "cellSpursFinalize", cellSpursFinalize},
    {"cellSpurs", "cellSpursDetachLv2EventQueue", cellSpursDetachLv2EventQueue},
    {"cellSpurs", "_cellSpursEventFlagInitialize", cellSpursEventFlagInitialize},
    {"cellSpurs", "cellSpursEventFlagAttachLv2EventQueue", cellSpursEventFlagAttachLv2EventQueue},
    {"cellSpurs", "cellSpursEventFlagDetachLv2EventQueue", cellSpursEventFlagDetachLv2EventQueue},
    {"cellSpurs", "cellSpursEventFlagSet", cellSpursEventFlagSet},
    {"cellSpurs", "cellSpursEventFlagWait", cellSpursEventFlagWait},
    {"cellSpurs", "cellSpursCreateTaskset", cellSpursCreateTaskset},
    {"cellSpurs", "cellSpursCreateTask", cellSpursCreateTask},
    {"cellSpurs", "cellSpursAttachLv2EventQueue", cellSpursAttachLv2EventQueue},
    {"cellSpurs", "cellSpursInitialize", cellSpursInitialize},
};

}  // namespace

std::span<const NativeImport> spurs_imports() { return kImports; }

}  // namespace rr7

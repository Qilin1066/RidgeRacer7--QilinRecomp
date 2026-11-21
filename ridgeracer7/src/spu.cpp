// The SPUs, as the system gives them to a process: system call 169 and,
// to come, SPU thread groups and SPURS.
//
// What is here is only the first word: the process says how many SPUs it
// will use. Running SPU programs needs the SPU lifter, which is the next
// piece of the recompiler (docs/roadmap-and-design-targets.md): until then
// the first call that would start one stops the game by name.
#include <mutex>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

std::mutex g_mutex;
bool g_initialised = false;
uint32_t g_max_usable = 0, g_max_raw = 0;

// sys_spu_initialize(the most SPUs the process will use, how many of them raw)
// A PS3 gives a process up to six.
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
    g_max_usable = max_usable;
    g_max_raw = max_raw;
    c.r[3] = kOk;
}

constexpr NativeSystemCall kCalls[] = {
    {169, sys_spu_initialize},
};

}  // namespace

std::span<const NativeSystemCall> spu_system_calls() { return kCalls; }

}  // namespace rr7

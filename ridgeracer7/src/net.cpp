// The network (sys_net, cellNetCtl), as a console with no network cable
// plugged in and no wireless set up: a state the real system has, and the
// honest one for a game whose servers closed long ago.
//
// The libraries start, as they do on such a console. What they then report
// is "not connected". Sockets and the connection dialog are not provided
// yet: each stops the game by name when it is first called.
#include <mutex>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

// cellNetCtl's errors (the SDK's CELL_NET_CTL_ERROR_...).
constexpr uint64_t kNotInitialised = 0x80130101u, kNotTerminated = 0x80130102u;

std::mutex g_mutex;
bool g_net = false, g_netctl = false;

// sys_net_initialize_network_ex(sys_net_initialize_parameter_t* {memory, its size, flags}): the socket library starts.
void sys_net_initialize_network_ex(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    g_net = true;
    c.r[3] = 0;
}

// sys_net_finalize_network()
void sys_net_finalize_network(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    g_net = false;
    c.r[3] = 0;
}

// cellNetCtlInit(), cellNetCtlTerm(): the library that reports the connection's state.
void cellNetCtlInit(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (g_netctl) {
        c.r[3] = kNotTerminated;
        return;
    }
    g_netctl = true;
    c.r[3] = kOk;
}
void cellNetCtlTerm(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (!g_netctl) {
        c.r[3] = kNotInitialised;
        return;
    }
    g_netctl = false;
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"sys_net", "sys_net_initialize_network_ex", sys_net_initialize_network_ex},
    {"sys_net", "sys_net_finalize_network", sys_net_finalize_network},
    {"cellNetCtl", "cellNetCtlInit", cellNetCtlInit},
    {"cellNetCtl", "cellNetCtlTerm", cellNetCtlTerm},
};

}  // namespace

std::span<const NativeImport> net_imports() { return kImports; }

}  // namespace rr7

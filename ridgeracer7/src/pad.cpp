// sys_io's controller functions (cellPad...).
//
// There is no host input yet (SDL comes with the window), so what these
// report is a console with no controller plugged in: a state the real system
// has and a game must cope with. Nothing is invented: every function
// answers as the system does when no controller is there.
#include <cstring>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

// The library's errors (the SDK's CELL_PAD_ERROR_...).
constexpr uint64_t kInvalidParameter = 0x80121102u, kAlreadyInitialised = 0x80121103u, kUninitialised = 0x80121104u, kNoDevice = 0x80121107u;
constexpr uint32_t kMostPads = 127;  // the size of the tables in CellPadInfo

bool g_initialised = false;
uint32_t g_max_connect = 0;

// cellPadInit(the most controllers the game will use)
void cellPadInit(Cpu& c) {
    const uint32_t max_connect = static_cast<uint32_t>(c.r[3]);
    if (g_initialised) {
        c.r[3] = kAlreadyInitialised;
        return;
    }
    if (max_connect == 0 || max_connect > kMostPads) {
        c.r[3] = kInvalidParameter;
        return;
    }
    g_initialised = true;
    g_max_connect = max_connect;
    c.r[3] = kOk;
}

// cellPadEnd()
void cellPadEnd(Cpu& c) {
    if (!g_initialised) {
        c.r[3] = kUninitialised;
        return;
    }
    g_initialised = false;
    c.r[3] = kOk;
}

// cellPadGetInfo(CellPadInfo* info): how many may connect, how many are connected (none), then
// tables of vendor ids, product ids and statuses for 127 ports, all zero.
void cellPadGetInfo(Cpu& c) {
    const uint32_t info = static_cast<uint32_t>(c.r[3]);
    if (!g_initialised) {
        c.r[3] = kUninitialised;
        return;
    }
    if (!info) {
        c.r[3] = kInvalidParameter;
        return;
    }
    std::memset(guest_memory() + info, 0, 12 + 2 * kMostPads + 2 * kMostPads + kMostPads);
    store32(info, g_max_connect);
    c.r[3] = kOk;
}

// Everything that addresses one controller by its port: with none connected, "no device".
// cellPadGetData(port, data*), cellPadClearBuf(port), cellPadSetPressMode(port, mode), cellPadSetSensorMode(port, mode),
// cellPadInfoPressMode(port), cellPadInfoSensorMode(port)
void by_port(Cpu& c) {
    if (!g_initialised) {
        c.r[3] = kUninitialised;
        return;
    }
    c.r[3] = static_cast<uint32_t>(c.r[3]) >= g_max_connect ? kInvalidParameter : kNoDevice;
}

constexpr NativeImport kImports[] = {
    {"sys_io", "cellPadInit", cellPadInit},
    {"sys_io", "cellPadEnd", cellPadEnd},
    {"sys_io", "cellPadGetInfo", cellPadGetInfo},
    {"sys_io", "cellPadGetData", by_port},
    {"sys_io", "cellPadClearBuf", by_port},
    {"sys_io", "cellPadSetPressMode", by_port},
    {"sys_io", "cellPadSetSensorMode", by_port},
    {"sys_io", "cellPadInfoPressMode", by_port},
    {"sys_io", "cellPadInfoSensorMode", by_port},
};

}  // namespace

std::span<const NativeImport> pad_imports() { return kImports; }

}  // namespace rr7

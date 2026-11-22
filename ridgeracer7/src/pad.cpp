// sys_io's controller functions (cellPad...).
//
// The player's controller (the window's gamepad, or the keyboard: main.cpp
// reads them) is the console's first controller, a DualShock 3 (Sony's
// vendor and product ids: *a stated choice*). Every other port has none.
//
// The data's layout is the SDK's, and agrees with the game's own reading of
// it: a length in halfwords (8 for the buttons and sticks, 20 with the
// buttons' pressure, 24 with the motion sensors), then the halfwords:
// [2] and [3] the buttons, [4] to [7] the right and left sticks (0 to 255,
// 128 the middle, y down), [8] to [19] the pressures. The full data is given
// at every call (*a console may give a length of 0 when nothing changed;
// the game takes the data either way*).
#include <atomic>
#include <cstring>
#include <mutex>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

// The library's errors (the SDK's CELL_PAD_ERROR_...).
constexpr uint64_t kInvalidParameter = 0x80121102u, kAlreadyInitialised = 0x80121103u, kUninitialised = 0x80121104u, kNoDevice = 0x80121107u;
constexpr uint32_t kMostPads = 127;  // the size of the tables in CellPadInfo
constexpr uint16_t kVendorSony = 0x054c, kProductDualShock3 = 0x0268;

std::mutex g_mutex;
bool g_initialised = false;
uint32_t g_max_connect = 0, g_press_mode = 0, g_sensor_mode = 0;
PadState g_state;

// cellPadInit(the most controllers the game will use)
void cellPadInit(Cpu& c) {
    const uint32_t max_connect = static_cast<uint32_t>(c.r[3]);
    const std::lock_guard lock(g_mutex);
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
    const std::lock_guard lock(g_mutex);
    if (!g_initialised) {
        c.r[3] = kUninitialised;
        return;
    }
    g_initialised = false;
    c.r[3] = kOk;
}

// cellPadGetInfo(CellPadInfo* info): how many may connect, how many are (one), the system's flags, then tables of vendor
// ids, product ids and statuses (1: connected) for 127 ports.
void cellPadGetInfo(Cpu& c) {
    const uint32_t info = static_cast<uint32_t>(c.r[3]);
    const std::lock_guard lock(g_mutex);
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
    store32(info + 4, 1);
    const uint32_t vendors = info + 12, products = vendors + 2 * kMostPads, statuses = products + 2 * kMostPads;
    guest_memory()[vendors] = kVendorSony >> 8, guest_memory()[vendors + 1] = kVendorSony & 0xff;
    guest_memory()[products] = kProductDualShock3 >> 8, guest_memory()[products + 1] = kProductDualShock3 & 0xff;
    guest_memory()[statuses] = 1;
    c.r[3] = kOk;
}

// Whether a port is the connected one; otherwise its error.
bool connected(Cpu& c, uint32_t port) {
    if (!g_initialised) {
        c.r[3] = kUninitialised;
        return false;
    }
    if (port >= g_max_connect) {
        c.r[3] = kInvalidParameter;
        return false;
    }
    if (port != 0) {
        c.r[3] = kNoDevice;
        return false;
    }
    return true;
}

// cellPadGetData(port, CellPadData* data)
void cellPadGetData(Cpu& c) {
    const uint32_t port = static_cast<uint32_t>(c.r[3]), data = static_cast<uint32_t>(c.r[4]);
    const std::lock_guard lock(g_mutex);
    if (!connected(c, port)) return;
    if (!data) {
        c.r[3] = kInvalidParameter;
        return;
    }
    uint16_t button[64] = {};
    button[2] = g_state.digital1;
    button[3] = g_state.digital2;
    button[4] = g_state.right_x, button[5] = g_state.right_y, button[6] = g_state.left_x, button[7] = g_state.left_y;
    for (unsigned i = 0; i < 12; ++i) button[8 + i] = g_state.pressure[i];
    // The motion sensors, still: 512 is the middle of their 10 bits, and gravity is 399 on the z axis' reading (*a stated reading*).
    button[20] = 512, button[21] = 399, button[22] = 512, button[23] = 512;
    const uint32_t length = g_sensor_mode ? 24 : g_press_mode ? 20 : 8;
    store32(data, length);
    for (unsigned i = 0; i < 64; ++i) guest_memory()[data + 4 + 2 * i] = static_cast<uint8_t>(button[i] >> 8), guest_memory()[data + 5 + 2 * i] = static_cast<uint8_t>(button[i]);
    c.r[3] = kOk;
}

// cellPadClearBuf(port): nothing is buffered here.
void cellPadClearBuf(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (connected(c, static_cast<uint32_t>(c.r[3]))) c.r[3] = kOk;
}

// cellPadSetPressMode(port, mode), cellPadSetSensorMode(port, mode): the buttons' pressures, the motion sensors, on or off.
void cellPadSetPressMode(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (!connected(c, static_cast<uint32_t>(c.r[3]))) return;
    g_press_mode = c.r[4] != 0;
    c.r[3] = kOk;
}
void cellPadSetSensorMode(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (!connected(c, static_cast<uint32_t>(c.r[3]))) return;
    g_sensor_mode = c.r[4] != 0;
    c.r[3] = kOk;
}
// cellPadInfoPressMode(port), cellPadInfoSensorMode(port): whether the controller has them (a DualShock 3 has both).
void cellPadInfoPressMode(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (connected(c, static_cast<uint32_t>(c.r[3]))) c.r[3] = 1;
}
void cellPadInfoSensorMode(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (connected(c, static_cast<uint32_t>(c.r[3]))) c.r[3] = 1;
}

constexpr NativeImport kImports[] = {
    {"sys_io", "cellPadInit", cellPadInit},
    {"sys_io", "cellPadEnd", cellPadEnd},
    {"sys_io", "cellPadGetInfo", cellPadGetInfo},
    {"sys_io", "cellPadGetData", cellPadGetData},
    {"sys_io", "cellPadClearBuf", cellPadClearBuf},
    {"sys_io", "cellPadSetPressMode", cellPadSetPressMode},
    {"sys_io", "cellPadSetSensorMode", cellPadSetSensorMode},
    {"sys_io", "cellPadInfoPressMode", cellPadInfoPressMode},
    {"sys_io", "cellPadInfoSensorMode", cellPadInfoSensorMode},
};

}  // namespace

void set_pad_state(const PadState& state) {
    const std::lock_guard lock(g_mutex);
    g_state = state;
}

std::span<const NativeImport> pad_imports() { return kImports; }

}  // namespace rr7

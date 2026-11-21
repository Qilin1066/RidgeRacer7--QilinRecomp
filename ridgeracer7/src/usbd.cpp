// cellUsbd: USB devices a game drives itself (this game: steering wheels,
// through a driver of its own that it registers).
//
// What is here is a USB bus with nothing plugged in: the library starts,
// the game's driver is registered, and since no device ever attaches, the
// driver's probe is never called. The functions that talk to a device
// (pipes, transfers) are not provided: they can only follow an attach, and
// would stop the game by name if one were ever called.
#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

// The library's errors (the SDK's CELL_USBD_ERROR_...).
constexpr uint64_t kNotInitialised = 0x80110001u, kAlreadyInitialised = 0x80110002u;

bool g_initialised = false;

// cellUsbdInit()
void cellUsbdInit(Cpu& c) {
    if (g_initialised) {
        c.r[3] = kAlreadyInitialised;
        return;
    }
    g_initialised = true;
    c.r[3] = kOk;
}

// cellUsbdRegisterExtraLdd(driver operations*, vendor id, product id), cellUsbdUnregisterExtraLdd(driver operations*)
// The driver is noted and never called: nothing attaches.
void registered(Cpu& c) { c.r[3] = g_initialised ? kOk : kNotInitialised; }

constexpr NativeImport kImports[] = {
    {"cellUsbd", "cellUsbdInit", cellUsbdInit},
    {"cellUsbd", "cellUsbdRegisterExtraLdd", registered},
    {"cellUsbd", "cellUsbdUnregisterExtraLdd", registered},
};

}  // namespace

std::span<const NativeImport> usbd_imports() { return kImports; }

}  // namespace rr7

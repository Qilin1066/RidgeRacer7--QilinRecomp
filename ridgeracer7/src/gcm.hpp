// The RSX as the game is given it (gcm.cpp) and as it runs (rsx.cpp).
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>

namespace rr7 {

// The RSX's own memory, as a game sees it: 249 MiB at a fixed address.
inline constexpr uint32_t kRsxLocal = 0xC0000000u, kRsxLocalSize = 0x0F900000u;

// The control register's three words, where the game and the RSX meet:
// the game moves `put`, the RSX follows with `get`.
struct RsxSetup {
    uint32_t control = 0;  // the address of put, get, ref
    uint32_t io_address = 0, io_size = 0;  // the game's memory the RSX can read, mapped at offset 0
    uint32_t labels = 0;                   // 256 of 16 bytes
};
const RsxSetup& rsx_setup();

// The address of an offset in the memory the RSX reads commands and data
// from (the game's mapped memory); 0 if nothing is mapped there.
uint32_t rsx_io_address(uint32_t offset);

// Starts the RSX (rsx.cpp): from now on commands between get and put are run.
void start_rsx();

// The runtime's own commands in the stream, put there by cellGcmSys's
// functions (which on a console reach the RSX's driver another way). Their
// numbers are outside what the RSX's own methods use.
inline constexpr uint32_t kMethodFlip = 0xE940;      // argument: the display buffer to show
inline constexpr uint32_t kMethodWaitFlip = 0xE944;  // wait until the last flip has been shown

// What cellGcmSys's functions have told the RSX, for rsx.cpp.
struct DisplayBuffer {
    uint32_t offset = 0, pitch = 0, width = 0, height = 0;
    bool set = false;
};
inline constexpr uint32_t kDisplayBuffers = 8;
DisplayBuffer display_buffer(uint32_t id);
// The flip mode the game set (cellGcmSetFlipMode): 1 shows a flip at once, 2 and 3 at the next vertical blank.
uint32_t flip_mode();
// The vertical blanks (59.94 a second, on the clock the vertical blank handler's thread keeps): the number of the next
// one to come, and a wait until the nth has come.
uint64_t next_vertical_blank();
void wait_for_vertical_blank(uint64_t n);

}  // namespace rr7

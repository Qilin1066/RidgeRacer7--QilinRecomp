// cellGcmSys: how a game sets the RSX up and talks to its driver.
//
// The game writes the RSX's commands itself (that code is the game's own,
// and lifted): into a buffer in its own memory, which this library maps for
// the RSX, moving `put` in the control register when a run of commands is
// ready. What this library does is the set-up, the mapping, and the few
// things that reach the driver directly.
//
// The layout it hands out is the console's, by public descriptions: the
// RSX's memory at 0xC0000000, the game's mapped memory at offset 0, the
// first 4 KiB of that kept back, and the command buffer cut in 32 KiB
// pieces of which the context holds one at a time.
#include "gcm.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

// The library's errors (the SDK's CELL_GCM_ERROR_...).
constexpr uint64_t kFailure = 0x802100ffu, kInvalidValue = 0x80210002u, kInvalidAlignment = 0x80210004u;

constexpr uint32_t kPiece = 0x8000;      // the command buffer's pieces
constexpr uint32_t kKeptBack = 0x1000;   // the start of the mapped memory, which is not the game's to write
constexpr uint32_t kLabels = 256, kTiles = 15, kZculls = 8;

struct Tile {
    uint32_t location = 0, offset = 0, size = 0, pitch = 0, compression = 0, base = 0, bank = 0;
    bool bound = false;
};
struct Zcull {
    uint32_t offset = 0, width = 0, height = 0, start = 0, format = 0, antialias = 0, direction = 0, depth_format = 0, s_func = 0, s_ref = 0, s_mask = 0;
    bool bound = false;
};

struct Gcm {
    std::mutex mutex;
    bool initialised = false;
    RsxSetup setup;
    uint32_t context = 0;      // CellGcmContextData: begin, end, current, callback
    uint32_t command_size = 0;
    uint32_t flip_mode = 2;    // vsync (the SDK's default)
    uint32_t vblank_handler = 0;
    std::array<DisplayBuffer, kDisplayBuffers> displays;
    std::array<Tile, kTiles> tiles;
    std::array<Zcull, kZculls> zculls;
};
Gcm g;

// Room for `words` more commands in the context, as the game's own code makes it: through the context's callback.
// (Used by the functions here that put commands in the stream.)
bool reserve(Cpu& c, uint32_t context, uint32_t words) {
    if (load32(context + 8) + 4 * words <= load32(context + 4)) return true;
    const uint32_t callback = load32(context + 12);
    Cpu call = c;
    call.r[3] = context;
    call.r[4] = words;
    call.r[2] = load32(callback + 4);
    c.hooks->call(call, load32(callback));
    return call.r[3] == kOk;
}

void put_command(Cpu& c, uint32_t context, uint32_t method, uint32_t argument) {
    if (!reserve(c, context, 2)) qilin::rt::fatal("the command buffer's callback refused room for a command (method %04x)", method);
    const uint32_t at = load32(context + 8);
    store32(at, (1u << 18) | method);
    store32(at + 4, argument);
    store32(context + 8, at + 8);
}

// The context's callback, as the library sets it: called by the game's own code when the piece of
// the command buffer it is writing has no room for `count` more words.
//
// It ends the piece with a jump to the next one (the last is followed by the first), hands what was
// written to the RSX, waits until the RSX is not reading in the piece about to be reused, and points
// the context at it.
void command_buffer_callback(Cpu& c) {
    const uint32_t context = static_cast<uint32_t>(c.r[3]);
    const uint32_t io = g.setup.io_address, control = g.setup.control;
    const uint32_t current = load32(context + 8);
    // The next piece. The first begins after what is kept back.
    uint32_t next = ((current - io) / kPiece + 1) * kPiece;
    if (next + kPiece > g.command_size) next = 0;
    const uint32_t begin = next ? next : kKeptBack, end = next + kPiece - 4;
    if (static_cast<uint32_t>(c.r[4]) * 4 > end - begin) {
        c.r[3] = kInvalidValue;
        return;
    }
    store32(current, 0x20000000u | begin);  // a jump
    store32(control, begin);                // put
    for (;;) {
        const uint32_t get = load32(control + 4);
        if (get < next || get > end) break;
        if (get == begin) break;  // it has taken the jump and waits there: the piece is free
        std::this_thread::yield();
    }
    store32(context, io + begin);
    store32(context + 4, io + end);
    store32(context + 8, io + begin);
    c.r[3] = kOk;
}

// _cellGcmInitBody(CellGcmContextData** context, command buffer size, mapped size, mapped address)
// (cellGcmInit, as the game's own wrapper passes it on.)
void cellGcmInitBody(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]), command_size = static_cast<uint32_t>(c.r[4]), io_size = static_cast<uint32_t>(c.r[5]),
                   io_address = static_cast<uint32_t>(c.r[6]);
    const std::lock_guard lock(g.mutex);
    if (g.initialised) {
        c.r[3] = kFailure;
        return;
    }
    if (io_size == 0 || (io_size & 0xfffff) || (io_address & 0xfffff)) {
        c.r[3] = kInvalidAlignment;
        return;
    }
    if (command_size < 2 * kPiece || (command_size & (kPiece - 1)) || command_size > io_size) {
        c.r[3] = kInvalidValue;
        return;
    }
    qilin::rt::log("the game sets the RSX up: %u KiB of commands in %u MiB of its memory at %08x", command_size >> 10, io_size >> 20, io_address);
    commit(kRsxLocal, kRsxLocalSize);
    g.setup.io_address = io_address;
    g.setup.io_size = io_size;
    g.command_size = command_size;
    g.setup.control = allocate(12, 16);
    g.setup.labels = allocate(kLabels * 16, 16);
    g.context = allocate(16, 16);
    // The context holds the first piece, after what is kept back; put and get both start there.
    store32(g.context, io_address + kKeptBack);
    store32(g.context + 4, io_address + kPiece - 4);
    store32(g.context + 8, io_address + kKeptBack);
    store32(g.context + 12, native_function(command_buffer_callback));
    store32(g.setup.control, kKeptBack);
    store32(g.setup.control + 4, kKeptBack);
    store32(g.setup.control + 8, 0);
    store32(out, g.context);
    g.initialised = true;
    start_rsx();
    c.r[3] = kOk;
}

// cellGcmGetConfiguration(CellGcmConfig* config): where the RSX's memory and the mapped memory are, and the clocks.
void cellGcmGetConfiguration(Cpu& c) {
    const uint32_t config = static_cast<uint32_t>(c.r[3]);
    store32(config, kRsxLocal);
    store32(config + 4, g.setup.io_address);
    store32(config + 8, kRsxLocalSize);
    store32(config + 12, g.setup.io_size);
    store32(config + 16, 650000000);  // the memory's clock
    store32(config + 20, 500000000);  // the core's
}

// cellGcmAddressToOffset(address, uint32_t* offset): an address in the RSX's memory or in the mapped memory, as the RSX names it.
void cellGcmAddressToOffset(Cpu& c) {
    const uint32_t address = static_cast<uint32_t>(c.r[3]), out = static_cast<uint32_t>(c.r[4]);
    if (address >= kRsxLocal && address - kRsxLocal < kRsxLocalSize) {
        store32(out, address - kRsxLocal);
    } else if (g.setup.io_size && address >= g.setup.io_address && address - g.setup.io_address < g.setup.io_size) {
        store32(out, address - g.setup.io_address);
    } else {
        c.r[3] = kFailure;
        return;
    }
    c.r[3] = kOk;
}

// cellGcmGetControlRegister(): the address of put, get and ref.
void cellGcmGetControlRegister(Cpu& c) { c.r[3] = g.setup.control; }

// cellGcmGetLabelAddress(index): one of the 256 labels the RSX can write to.
void cellGcmGetLabelAddress(Cpu& c) {
    const uint32_t index = static_cast<uint32_t>(c.r[3]);
    c.r[3] = index < kLabels ? g.setup.labels + 16 * index : 0;
}

// cellGcmGetTiledPitchSize(size): the smallest pitch a tiled region may have that is at least `size`; 0 if none.
void cellGcmGetTiledPitchSize(Cpu& c) {
    static constexpr uint32_t kPitches[] = {0x200,  0x300,  0x400,  0x500,  0x600,  0x700,  0x800,  0xA00,  0xC00,  0xD00,  0xE00,
                                            0x1000, 0x1400, 0x1800, 0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800, 0x4000,
                                            0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000, 0xC000, 0xD000, 0xE000, 0x10000};
    const uint32_t size = static_cast<uint32_t>(c.r[3]);
    c.r[3] = 0;
    for (const uint32_t pitch : kPitches)
        if (pitch >= size) {
            c.r[3] = pitch;
            return;
        }
}

// cellGcmSetDisplayBuffer(id, offset, pitch, width, height): a picture in the RSX's memory that can be shown.
void cellGcmSetDisplayBuffer(Cpu& c) {
    const uint32_t id = static_cast<uint32_t>(c.r[3]);
    if (id >= kDisplayBuffers) {
        c.r[3] = kFailure;
        return;
    }
    const std::lock_guard lock(g.mutex);
    g.displays[id] = {static_cast<uint32_t>(c.r[4]), static_cast<uint32_t>(c.r[5]), static_cast<uint32_t>(c.r[6]), static_cast<uint32_t>(c.r[7]), true};
    c.r[3] = kOk;
}

// cellGcmSetFlipMode(mode): 1 shows a flip at once, 2 at the next vertical blank, 3 likewise (hsync, on a console).
void cellGcmSetFlipMode(Cpu& c) {
    const uint32_t mode = static_cast<uint32_t>(c.r[3]);
    if (mode < 1 || mode > 3) qilin::rt::fatal("cellGcmSetFlipMode: mode %u, which isn't one of the three", mode);
    const std::lock_guard lock(g.mutex);
    g.flip_mode = mode;
}

// cellGcmSetTile(index, location, offset, size, pitch, compression, base, bank)
// cellGcmSetInvalidateTile(index)
// A tiled region is how the RSX lays a render target out in its memory for speed; what is read and written
// there is the same either way. The parameters are checked and kept, and change nothing.
void cellGcmSetTile(Cpu& c) {
    const uint32_t index = static_cast<uint32_t>(c.r[3]);
    const Tile tile{static_cast<uint32_t>(c.r[4]), static_cast<uint32_t>(c.r[5]), static_cast<uint32_t>(c.r[6]), static_cast<uint32_t>(c.r[7]),
                    static_cast<uint32_t>(c.r[8]),  static_cast<uint32_t>(c.r[9]), static_cast<uint32_t>(c.r[10]), true};
    // What the library refuses is not all known here, so anything out of the ordinary stops the game rather than be let through or refused wrongly.
    if (index >= kTiles || tile.base >= 800 || tile.bank >= 4 || (tile.offset & 0xffff) || (tile.size & 0xffff) || (tile.pitch & 0xff) || tile.location > 1 || tile.compression > 12)
        qilin::rt::fatal("cellGcmSetTile(%u, location %u, offset %#x, size %#x, pitch %#x, compression %u, base %u, bank %u): parameters this runtime doesn't know the library's answer to",
                         index, tile.location, tile.offset, tile.size, tile.pitch, tile.compression, tile.base, tile.bank);
    const std::lock_guard lock(g.mutex);
    g.tiles[index] = tile;
    c.r[3] = kOk;
}
void cellGcmSetInvalidateTile(Cpu& c) {
    const uint32_t index = static_cast<uint32_t>(c.r[3]);
    if (index >= kTiles) {
        c.r[3] = kInvalidValue;
        return;
    }
    const std::lock_guard lock(g.mutex);
    g.tiles[index].bound = false;
    c.r[3] = kOk;
}

// cellGcmSetZcull(index, offset, width, height, cull start, z format, antialias, direction, depth format, s func, s ref, s mask)
// A zcull region lets the RSX reject hidden pixels early: again speed, not results (its only visible
// effect is in the zcull reports, which stop the game if it asks for one). Kept, and changes nothing.
void cellGcmSetZcull(Cpu& c) {
    const uint32_t index = static_cast<uint32_t>(c.r[3]);
    if (index >= kZculls) {
        c.r[3] = kInvalidValue;
        return;
    }
    // The last four arguments are on the caller's stack, after the eight in registers.
    const uint32_t stack = static_cast<uint32_t>(c.r[1]) + 0x70;
    Zcull z{static_cast<uint32_t>(c.r[4]), static_cast<uint32_t>(c.r[5]), static_cast<uint32_t>(c.r[6]), static_cast<uint32_t>(c.r[7]),
            static_cast<uint32_t>(c.r[8]), static_cast<uint32_t>(c.r[9]), static_cast<uint32_t>(c.r[10]), 0, 0, 0, 0, true};
    z.depth_format = static_cast<uint32_t>(load64(stack));
    z.s_func = static_cast<uint32_t>(load64(stack + 8));
    z.s_ref = static_cast<uint32_t>(load64(stack + 16));
    z.s_mask = static_cast<uint32_t>(load64(stack + 24));
    const std::lock_guard lock(g.mutex);
    g.zculls[index] = z;
    c.r[3] = kOk;
}

// cellGcmSetFlip(context, id): puts "show display buffer id" in the stream, after everything already there.
void cellGcmSetFlip(Cpu& c) {
    const uint32_t context = static_cast<uint32_t>(c.r[3]), id = static_cast<uint32_t>(c.r[4]);
    if (id >= kDisplayBuffers) {
        c.r[3] = kFailure;
        return;
    }
    put_command(c, context, kMethodFlip, id);
    c.r[3] = kOk;
}

// cellGcmSetWaitFlip(context): puts "wait until the last flip is shown" in the stream.
void cellGcmSetWaitFlip(Cpu& c) { put_command(c, static_cast<uint32_t>(c.r[3]), kMethodWaitFlip, 0); }

// The vertical blank: 59.94 times a second, as the display this runtime describes refreshes (video.cpp).
// One thread keeps the count and, if the game has registered a handler, calls it each time, with the
// display's head (1) as its argument: on a console that is done by one of the system's own threads.
std::atomic<uint32_t> g_vblank_handler{0};
std::atomic<uint64_t> g_vblanks{0};
std::once_flag g_vblank_started;

void vblank_thread() {
    Cpu cpu;
    become_guest_thread(cpu, 0x10000, 0);
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    for (uint64_t n = 1;; ++n) {
        std::this_thread::sleep_until(start + std::chrono::nanoseconds(n * 1001000000ull / 60));
        g_vblanks.store(n, std::memory_order_relaxed);
        if (const uint32_t handler = g_vblank_handler.load()) {
            cpu.r[3] = 1;
            call_descriptor(cpu, handler);
        }
    }
}

// cellGcmSetVBlankHandler(handler): a function of the game's to call at every vertical blank; null for none.
void cellGcmSetVBlankHandler(Cpu& c) {
    g_vblank_handler.store(static_cast<uint32_t>(c.r[3]));
    std::call_once(g_vblank_started, [] { std::thread(vblank_thread).detach(); });
}

constexpr NativeImport kImports[] = {
    {"cellGcmSys", "cellGcmSetVBlankHandler", cellGcmSetVBlankHandler},
    {"cellGcmSys", "_cellGcmInitBody", cellGcmInitBody},
    {"cellGcmSys", "cellGcmGetConfiguration", cellGcmGetConfiguration},
    {"cellGcmSys", "cellGcmAddressToOffset", cellGcmAddressToOffset},
    {"cellGcmSys", "cellGcmGetControlRegister", cellGcmGetControlRegister},
    {"cellGcmSys", "cellGcmGetLabelAddress", cellGcmGetLabelAddress},
    {"cellGcmSys", "cellGcmGetTiledPitchSize", cellGcmGetTiledPitchSize},
    {"cellGcmSys", "cellGcmSetDisplayBuffer", cellGcmSetDisplayBuffer},
    {"cellGcmSys", "cellGcmSetFlipMode", cellGcmSetFlipMode},
    {"cellGcmSys", "cellGcmSetTile", cellGcmSetTile},
    {"cellGcmSys", "cellGcmSetInvalidateTile", cellGcmSetInvalidateTile},
    {"cellGcmSys", "cellGcmSetZcull", cellGcmSetZcull},
    {"cellGcmSys", "cellGcmSetFlip", cellGcmSetFlip},
    {"cellGcmSys", "cellGcmSetWaitFlip", cellGcmSetWaitFlip},
};

}  // namespace

const RsxSetup& rsx_setup() { return g.setup; }

uint32_t rsx_io_address(uint32_t offset) { return offset < g.setup.io_size ? g.setup.io_address + offset : 0; }

DisplayBuffer display_buffer(uint32_t id) {
    const std::lock_guard lock(g.mutex);
    return id < kDisplayBuffers ? g.displays[id] : DisplayBuffer{};
}

std::span<const NativeImport> gcm_imports() { return kImports; }

}  // namespace rr7

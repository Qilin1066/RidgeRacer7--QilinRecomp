// cellSysutil's video output (cellVideoOut...): the television, as a game
// asks about it and sets it.
//
// The television here is the runtime's to define: one display on the
// primary output, digital, 16:9, 59.94 Hz, taking 1080p, 720p and 480p. Which
// of those it reports as current is `kCurrent`: 1080p, the game's best
// (public descriptions), until the settings menu holds it. What the game
// then configures is kept for the RSX's flips (video_mode()).
#include "video.hpp"

#include <cstring>
#include <iterator>
#include <mutex>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;

// The resolutions' numbers (the SDK's CELL_VIDEO_OUT_RESOLUTION_...). The last four are 1080-line
// pictures of fewer columns, which the console stretches to the screen's width.
struct Resolution {
    uint32_t id, width, height;
    bool available;
};
constexpr Resolution kResolutions[] = {
    {1, 1920, 1080, true}, {2, 1280, 720, true},   {4, 720, 480, true},    {5, 720, 576, false},
    {10, 1600, 1080, true}, {11, 1440, 1080, true}, {12, 1280, 1080, true}, {13, 960, 1080, true},
};
constexpr uint32_t kCurrent = 1;
constexpr uint32_t kPrimary = 0, kSecondary = 1;
constexpr uint32_t kAspect16x9 = 2;

std::mutex g_mutex;
VideoMode g_mode;

const Resolution* find(uint32_t id) {
    for (const Resolution& r : kResolutions)
        if (r.id == id) return &r;
    return nullptr;
}

// cellVideoOutGetNumberOfDevice(output): how many displays are on it.
void cellVideoOutGetNumberOfDevice(Cpu& c) {
    const uint32_t output = static_cast<uint32_t>(c.r[3]);
    if (output == kPrimary) c.r[3] = 1;
    else if (output == kSecondary) c.r[3] = 0;
    else fatal("cellVideoOutGetNumberOfDevice: video output %u, which isn't one of the two", output);
}

// cellVideoOutGetState(output, device, CellVideoOutState* state)
// The state: enabled (0), colour space RGB (1), six bytes kept back, then the display mode: resolution,
// scan mode (1: progressive), conversion (0: none), aspect, two bytes kept back, refresh rates (1: 59.94 Hz).
void cellVideoOutGetState(Cpu& c) {
    const uint32_t output = static_cast<uint32_t>(c.r[3]), device = static_cast<uint32_t>(c.r[4]), state = static_cast<uint32_t>(c.r[5]);
    if (output != kPrimary || device != 0) fatal("cellVideoOutGetState: output %u, device %u, where there is no display", output, device);
    uint8_t* const p = guest_memory() + state;
    std::memset(p, 0, 16);
    p[0] = 0;
    p[1] = 1;
    {
        const std::lock_guard lock(g_mutex);
        p[8] = static_cast<uint8_t>(g_mode.configured ? g_mode.resolution : kCurrent);
    }
    p[9] = 1;
    p[10] = 0;
    p[11] = kAspect16x9;
    p[14] = 0;
    p[15] = 1;
    c.r[3] = kOk;
}

// cellVideoOutGetResolution(resolution, CellVideoOutResolution* {width, height})
void cellVideoOutGetResolution(Cpu& c) {
    const Resolution* r = find(static_cast<uint32_t>(c.r[3]));
    if (!r) fatal("cellVideoOutGetResolution: resolution %u, which this runtime doesn't know", static_cast<uint32_t>(c.r[3]));
    const uint32_t out = static_cast<uint32_t>(c.r[4]);
    guest_memory()[out] = static_cast<uint8_t>(r->width >> 8);
    guest_memory()[out + 1] = static_cast<uint8_t>(r->width);
    guest_memory()[out + 2] = static_cast<uint8_t>(r->height >> 8);
    guest_memory()[out + 3] = static_cast<uint8_t>(r->height);
    c.r[3] = kOk;
}

// cellVideoOutGetResolutionAvailability(output, resolution, aspect, option): 1 if the display takes it.
void cellVideoOutGetResolutionAvailability(Cpu& c) {
    const uint32_t output = static_cast<uint32_t>(c.r[3]);
    const Resolution* r = find(static_cast<uint32_t>(c.r[4]));
    if (output != kPrimary) fatal("cellVideoOutGetResolutionAvailability: video output %u, where there is no display", output);
    if (!r) fatal("cellVideoOutGetResolutionAvailability: resolution %u, which this runtime doesn't know", static_cast<uint32_t>(c.r[4]));
    c.r[3] = r->available ? 1 : 0;
}

// cellVideoOutConfigure(output, CellVideoOutConfiguration* {resolution, format, aspect, nine bytes kept back, pitch}, option*, wait)
void cellVideoOutConfigure(Cpu& c) {
    const uint32_t output = static_cast<uint32_t>(c.r[3]), config = static_cast<uint32_t>(c.r[4]);
    if (output != kPrimary) fatal("cellVideoOutConfigure: video output %u, where there is no display", output);
    const uint8_t* const p = guest_memory() + config;
    const Resolution* r = find(p[0]);
    if (!r || !r->available) fatal("cellVideoOutConfigure: resolution %u, which the display doesn't take", p[0]);
    if (p[1] > 2) fatal("cellVideoOutConfigure: buffer format %u, which this runtime doesn't know", p[1]);
    const std::lock_guard lock(g_mutex);
    g_mode = {true, r->id, r->width, r->height, p[1], p[2], load32(config + 12)};
    qilin::rt::log("the game sets the video output: %ux%u, format %u, aspect %u, pitch %u", r->width, r->height, p[1], p[2], g_mode.pitch);
    c.r[3] = kOk;
}

// cellVideoOutGetDeviceInfo(output, device, CellVideoOutDeviceInfo* info)
// The display itself: its port (1: HDMI), colour space (1: RGB), latency, how many modes it takes, its
// state (0: enabled), its RGB range (1: full), five bytes kept back, its colours' coordinates (20 bytes),
// then up to 32 modes of 8 bytes, each as in cellVideoOutGetState. (The modes begin at byte 32: read off
// the game's own loop over them, which also reads the count at byte 4.)
// *The colours' coordinates are left zero: their encoding isn't known here, and the game reads none of them.*
void cellVideoOutGetDeviceInfo(Cpu& c) {
    const uint32_t output = static_cast<uint32_t>(c.r[3]), device = static_cast<uint32_t>(c.r[4]), info = static_cast<uint32_t>(c.r[5]);
    if (output != kPrimary || device != 0) fatal("cellVideoOutGetDeviceInfo: output %u, device %u, where there is no display", output, device);
    struct Mode {
        uint8_t resolution, scan;
    };
    static constexpr Mode kModes[] = {{1, 1}, {1, 0}, {2, 1}, {4, 1}, {4, 0}};  // 1080p, 1080i, 720p, 480p, 480i
    uint8_t* const p = guest_memory() + info;
    std::memset(p, 0, 32 + 32 * 8);
    p[0] = 1;
    p[1] = 1;
    p[4] = static_cast<uint8_t>(std::size(kModes));
    p[5] = 0;
    p[6] = 1;
    uint8_t* mode = p + 32;
    for (const Mode& m : kModes) {
        mode[0] = m.resolution;
        mode[1] = m.scan;
        mode[3] = kAspect16x9;
        mode[7] = 1;  // 59.94 Hz
        mode += 8;
    }
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"cellSysutil", "cellVideoOutGetDeviceInfo", cellVideoOutGetDeviceInfo},
    {"cellSysutil", "cellVideoOutGetNumberOfDevice", cellVideoOutGetNumberOfDevice},
    {"cellSysutil", "cellVideoOutGetState", cellVideoOutGetState},
    {"cellSysutil", "cellVideoOutGetResolution", cellVideoOutGetResolution},
    {"cellSysutil", "cellVideoOutGetResolutionAvailability", cellVideoOutGetResolutionAvailability},
    {"cellSysutil", "cellVideoOutConfigure", cellVideoOutConfigure},
};

}  // namespace

VideoMode video_mode() {
    const std::lock_guard lock(g_mutex);
    return g_mode;
}

std::span<const NativeImport> video_imports() { return kImports; }

}  // namespace rr7

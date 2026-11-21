// The RSX: a thread that runs the commands the game has written, from
// `get` to `put`, as the console's graphics processor does.
//
// A command is a 32-bit header and its arguments: the header names a
// method (a register of the RSX, by its offset) and says how many
// arguments follow; each argument goes to the next method along, or all to
// the same one. Jumps, calls and returns move `get` about.
//
// Most methods only set state, which is kept here in a table by method;
// the few that do something (draw, clear, write a label) act on that
// state. **A method that isn't in the table of known ones stops the game
// with its number**: none is skipped.
#include <array>
#include <chrono>
#include <cstdlib>
#include <map>
#include <thread>

#include "gcm.hpp"
#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;

struct Rsx {
    uint32_t get = 0;
    uint32_t return_to = 0;  // where a call came from, or 0: calls don't nest
    bool in_call = false;
    uint32_t label = 0;  // NV406E_SEMAPHORE_OFFSET
    bool flip_pending = false;  // a flip has been asked for and not yet shown
    std::array<uint32_t, 0x10000 / 4> state{};  // the last argument given to each method
    uint64_t commands = 0;
};
Rsx r;

// The RSX's state: methods that only set something, which is kept until a method that acts reads it.
// Each entry is a run of methods (4 apart) by the name the graphics chip's public documentation gives
// the first. A method in none of these runs, and not one of those that act (run_method), stops the game.
struct StateMethods {
    uint16_t first, count;
    const char* name;
};
constexpr StateMethods kState[] = {
    {0x0180, 1, "SET_CONTEXT_DMA_NOTIFIES"},
    {0x0184, 14, "SET_CONTEXT_DMA_A"},  // ... B, COLOR_B, STATE, COLOR_A, ZETA, VERTEX_A, VERTEX_B, SEMAPHORE, REPORT, CLIP_ID, CULL_DATA, COLOR_C, COLOR_D
    {0x0200, 9, "SET_SURFACE_CLIP_HORIZONTAL"},  // ... CLIP_VERTICAL, FORMAT, PITCH_A, COLOR_AOFFSET, ZETA_OFFSET, COLOR_BOFFSET, PITCH_B, COLOR_TARGET
    {0x022c, 1, "SET_SURFACE_PITCH_Z"},
    {0x0238, 2, "SET_CYLINDRICAL_WRAP"},
    {0x0280, 4, "SET_SURFACE_PITCH_C"},  // ... PITCH_D, COLOR_COFFSET, COLOR_DOFFSET
    {0x02b8, 2, "SET_WINDOW_OFFSET"},    // ... WINDOW_CLIP_TYPE
    {0x02c0, 16, "SET_WINDOW_CLIP_HORIZONTAL"},
    {0x0300, 1, "SET_DITHER_ENABLE"},
    {0x0304, 26, "SET_ALPHA_TEST_ENABLE"},  // alpha, blend, colour mask, stencil front and back, shade mode
    {0x036c, 8, "SET_BLEND_ENABLE_MRT"},    // ... COLOR_MASK_MRT, LOGIC_OP_ENABLE, LOGIC_OP, BLEND_COLOR2, DEPTH_BOUNDS_TEST_ENABLE, DEPTH_BOUNDS_MIN, MAX
    {0x0394, 2, "SET_CLIP_MIN"},
    {0x03b0, 1, "SET_CONTROL0"},
    {0x03b8, 2, "SET_LINE_WIDTH"},  // ... LINE_SMOOTH_ENABLE
    {0x03c0, 16, "SET_ANISO_SPREAD"},
    {0x08c0, 2, "SET_SCISSOR_HORIZONTAL"},
    {0x08cc, 3, "SET_FOG_MODE"},  // ... FOG_PARAMS (2)
    {0x08e4, 1, "SET_SHADER_PROGRAM"},
    {0x0900, 32, "SET_VERTEX_TEXTURE_OFFSET"},  // four vertex textures of eight methods
    {0x0a00, 2, "SET_VIEWPORT_HORIZONTAL"},
    {0x0a0c, 1, "SET_POINT_CENTER_MODE"},
    {0x0a1c, 1, "ZCULL_SYNC"},
    {0x0a20, 8, "SET_VIEWPORT_OFFSET"},  // ... (4), VIEWPORT_SCALE (4)
    {0x0a60, 8, "SET_POLY_OFFSET_POINT_ENABLE"},  // ... LINE, FILL, DEPTH_FUNC, DEPTH_MASK, DEPTH_TEST_ENABLE, POLYGON_OFFSET_SCALE_FACTOR, BIAS
    {0x0b00, 16, "SET_TEXTURE_CONTROL2"},
    {0x0b40, 10, "SET_TEX_COORD_CONTROL"},
    {0x1428, 2, "SET_SPECULAR_ENABLE"},  // ... TWO_SIDE_LIGHT_EN
    {0x143c, 1, "SET_PERFORMANCE_PARAMS"},
    {0x1450, 2, "SET_FLAT_SHADE_OP"},  // ... EDGE_FLAG
    {0x1478, 2, "SET_USER_CLIP_PLANE_CONTROL"},  // ... POLYGON_STIPPLE
    {0x1480, 32, "SET_POLYGON_STIPPLE_PATTERN"},
    {0x1680, 16, "SET_VERTEX_DATA_ARRAY_OFFSET"},
    {0x1738, 2, "SET_VERTEX_DATA_BASE_OFFSET"},  // ... BASE_INDEX
    {0x1740, 16, "SET_VERTEX_DATA_ARRAY_FORMAT"},
    {0x17cc, 1, "SET_ZPASS_PIXEL_COUNT_ENABLE"},
    {0x1804, 1, "SET_ZCULL_STATS_ENABLE"},
    {0x181c, 2, "SET_INDEX_ARRAY_ADDRESS"},  // ... INDEX_ARRAY_DMA
    {0x1828, 6, "SET_FRONT_POLYGON_MODE"},   // ... BACK_POLYGON_MODE, CULL_FACE, FRONT_FACE, POLY_SMOOTH_ENABLE, CULL_FACE_ENABLE
    {0x1840, 16, "SET_TEXTURE_CONTROL3"},
    {0x1a00, 128, "SET_TEXTURE_OFFSET"},  // sixteen textures of eight methods
    {0x1d00, 16, "SET_COLOR_KEY_COLOR"},
    {0x1d60, 2, "SET_SHADER_CONTROL"},  // ... INDEXED_CONSTANT_READ_LIMITS
    {0x1d6c, 1, "SET_SEMAPHORE_OFFSET"},
    {0x1d78, 7, "SET_ZMIN_MAX_CONTROL"},  // ... ANTI_ALIASING_CONTROL, SURFACE_COMPRESSION, ZCULL_EN, SHADER_WINDOW, ZSTENCIL_CLEAR_VALUE, COLOR_CLEAR_VALUE
    {0x1d98, 2, "SET_CLEAR_RECT_HORIZONTAL"},
    {0x1da4, 1, "SET_CLIP_ID_TEST_ENABLE"},
    {0x1dac, 4, "SET_RESTART_INDEX_ENABLE"},  // ... RESTART_INDEX, LINE_STIPPLE, LINE_STIPPLE_PATTERN
    {0x1e94, 2, "SET_TRANSFORM_EXECUTION_MODE"},  // ... RENDER_ENABLE
    {0x1ea0, 4, "SET_TRANSFORM_PROGRAM_START"},   // ... ZCULL_CONTROL0, ZCULL_CONTROL1, SCULL_CONTROL
    {0x1ee0, 3, "SET_POINT_SIZE"},                // ... POINT_PARAMS_ENABLE, POINT_SPRITE_CONTROL
    {0x1ef8, 1, "SET_TRANSFORM_TIMEOUT"},
    {0x1fc0, 6, "SET_FREQUENCY_DIVIDER_OPERATION"},  // ... ATTRIB_COLOR, ATTRIB_TEX_COORD, ATTRIB_TEX_COORD_EX, ATTRIB_UCLIP0, ATTRIB_UCLIP1
    {0x1fe0, 1, "SET_REDUCE_DST_COLOR"},
    {0x1fe8, 5, "SET_NO_PARANOID_TEXTURE_FETCHES"},  // ... SHADER_PACKER, VERTEX_ATTRIB_INPUT_MASK, VERTEX_ATTRIB_OUTPUT_MASK, TRANSFORM_BRANCH_BITS
};

bool is_state(uint32_t method) {
    for (const StateMethods& m : kState)
        if (method >= m.first && method < m.first + 4u * m.count) return true;
    return false;
}

// A development aid, never a way to play: with RR7_RSX_SURVEY set in the environment, methods not yet
// provided are counted, not run, and at the first flip (or after ten seconds) the list is printed and
// the process stops. It shows the work ahead at once and not one stop at a time.
bool g_survey = false;
struct Surveyed {
    uint64_t count = 0;
    uint32_t first = 0;
};
std::map<uint32_t, Surveyed> g_surveyed;
std::chrono::steady_clock::time_point g_survey_ends;

[[noreturn]] void end_survey() {
    for (const auto& [method, seen] : g_surveyed) qilin::rt::log("survey: method %04x, %llu times, first argument %08x", method, static_cast<unsigned long long>(seen.count), seen.first);
    fatal("the survey of RSX methods is over: %zu not provided", g_surveyed.size());
}

// The address of the label at this offset among the labels. (The game may point the RSX's semaphore
// object elsewhere, SET_CONTEXT_DMA_SEMAPHORE; anything but the labels stops the game.)
uint32_t label_address(uint32_t offset, uint32_t at) {
    constexpr uint32_t kLabelsObject = 0x66616661u, kLabelsSize = 256 * 16;
    if (const uint32_t object = r.state[0x01a4 / 4]; object != 0 && object != kLabelsObject)
        fatal("the RSX is to write a label through object %08x, which isn't the labels (at offset %08x)", object, at);
    if (offset > kLabelsSize - 4 || (offset & 3)) fatal("the RSX is to write a label at offset %#x, outside the labels (at offset %08x)", offset, at);
    return rsx_setup().labels + offset;
}

// One method with one argument.
void run_method(uint32_t method, uint32_t argument, uint32_t at) {
    if (is_state(method)) {
        r.state[method / 4] = argument;
        return;
    }
    switch (method) {
    case 0x0050:  // NV406E_SET_REFERENCE: the game's own mark of how far the RSX has got, readable beside put and get
        store32(rsx_setup().control + 8, argument);
        return;
    case 0x0100:  // NO_OPERATION
        return;
    // Labels: words the RSX writes when it reaches the command, by which the game knows how far it has got.
    // (Everything before the command is done by then here: commands are run one after another.)
    case 0x0064:  // NV406E_SEMAPHORE_OFFSET: which label the next two act on
        r.label = argument;
        return;
    case 0x006c:  // NV406E_SEMAPHORE_RELEASE: write it
        store32(label_address(r.label, at), argument);
        return;
    case 0x0068:  // NV406E_SEMAPHORE_ACQUIRE: wait until the game has made it this value
        for (const uint32_t address = label_address(r.label, at); load32(address) != argument;) std::this_thread::yield();
        return;
    case 0x1d70:  // BACK_END_WRITE_SEMAPHORE_RELEASE: write the label at SET_SEMAPHORE_OFFSET once everything drawn so far is in the surface.
        // The chip writes the argument's red and blue bytes exchanged (the game's library exchanges them first, so the label reads as meant).
        store32(label_address(r.state[0x1d6c / 4], at), (argument & 0xff00ff00u) | ((argument & 0xff) << 16) | ((argument >> 16) & 0xff));
        return;
    case 0x1d74:  // TEXTURE_READ_SEMAPHORE_RELEASE: the same label, as given, once every texture read so far is done
        store32(label_address(r.state[0x1d6c / 4], at), argument);
        return;
    case kMethodWaitFlip:  // (the runtime's own) wait until the last flip has been shown
        if (r.flip_pending) fatal("the RSX is to wait for a flip, and showing pictures isn't provided yet");
        return;
    default:
        if (g_survey) {
            auto& seen = g_surveyed[method];
            if (seen.count++ == 0) seen.first = argument;
            if (method == kMethodFlip || std::chrono::steady_clock::now() > g_survey_ends) end_survey();
            return;
        }
        fatal("the RSX is given method %04x (argument %08x, at offset %08x), which this runtime doesn't provide yet", method, argument, at);
    }
}

void serve() {
    const uint32_t control = rsx_setup().control;
    r.get = load32(control + 4);
    g_survey = std::getenv("RR7_RSX_SURVEY") != nullptr;
    g_survey_ends = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    uint32_t idle = 0;
    for (;;) {
        const uint32_t put = load32(control);
        if (r.get == put) {
            if (g_survey && std::chrono::steady_clock::now() > g_survey_ends) end_survey();
            // Nothing to do. (The console's RSX is told when put moves; here the game's own code moves it
            // in memory, so it is watched: closely at first, then at a millisecond's remove.)
            if (++idle < 2000) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        idle = 0;
        const uint32_t address = rsx_io_address(r.get);
        if (!address) fatal("the RSX's get is at offset %08x, where nothing is mapped (put %08x)", r.get, put);
        const uint32_t header = load32(address);
        uint32_t next;
        if ((header & 0xe0000003u) == 0x20000000u) {  // a jump, as first made
            next = header & 0x1ffffffcu;
            if (next == r.get) {  // to itself: the game's way of holding the RSX here until it writes over it
                if (g_survey && std::chrono::steady_clock::now() > g_survey_ends) end_survey();
                std::this_thread::yield();
                continue;
            }
        } else if ((header & 3) == 1) {  // a jump, as made later
            next = header & 0xfffffffcu;
        } else if ((header & 3) == 2) {  // a call
            if (r.in_call) fatal("the RSX is given a call inside a call, at offset %08x", r.get);
            r.in_call = true;
            r.return_to = r.get + 4;
            next = header & 0xfffffffcu;
        } else if (header == 0x00020000u) {  // a return
            if (!r.in_call) fatal("the RSX is given a return with no call, at offset %08x", r.get);
            r.in_call = false;
            next = r.return_to;
        } else {
            if (header & 0xa0030003u) fatal("the RSX is given a command it can't read: %08x at offset %08x", header, r.get);
            const uint32_t count = (header >> 18) & 0x7ff, method = header & 0xfffc;
            const bool same = (header & 0x40000000u) != 0;  // every argument to the one method
            for (uint32_t i = 0; i < count; ++i) run_method(same ? method : method + 4 * i, load32(address + 4 + 4 * i), r.get);
            next = r.get + 4 + 4 * count;
            ++r.commands;
        }
        r.get = next;
        store32(control + 4, next);
    }
}

}  // namespace

void start_rsx() { std::thread(serve).detach(); }

}  // namespace rr7

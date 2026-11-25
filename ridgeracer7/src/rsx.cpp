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
#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

#include "gcm.hpp"
#include "renderer.hpp"
#include "video.hpp"
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
    // The vertex programs' memory (512 instructions of four words) and their constants (468 vec4), and where the next
    // word written to each goes.
    std::array<uint32_t, 512 * 4> program{};
    uint32_t program_at = 0;
    std::array<float, 468 * 4> constants{};
    uint32_t constant_at = 0;
    uint64_t constants_version = 0;  // counts writes to them: the renderer reuses its copy while this is unchanged
    // Between SET_BEGIN_END's begin and its end: the primitive, and the ranges of vertices drawn.
    uint32_t primitive = 0;
    struct Range {
        uint32_t first, count;
        bool indexed;  // DRAW_INDEX_ARRAY's: first and count are of the index array
    };
    std::vector<Range> ranges;
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
    // The 2D objects the game's library puts on subchannels 3 and 5 (its command headers say so: 0x6xxx and 0xaxxx), as
    // PSL1GHT's headers name their methods. Subchannel 3: the 2D surface (NV3062) a blit writes to.
    {0x6000, 1, "NV3062 SET_OBJECT"},
    {0x6180, 3, "NV3062 SET_CONTEXT_DMA_NOTIFIES"},  // ... IMAGE_SOURCE, IMAGE_DESTIN
    {0x6300, 4, "NV3062 SET_COLOR_FORMAT"},          // ... SET_PITCH (destination << 16 | source), SET_OFFSET_SOURCE, SET_OFFSET_DESTIN
    // Subchannel 5: the image from the CPU (NV308A), whose COLOR words (0xa400 on) are written to the 2D surface.
    {0xa000, 1, "NV308A SET_OBJECT"},
    {0xa180, 8, "NV308A SET_CONTEXT_DMA_NOTIFIES"},  // ... COLOR_KEY, CLIP_RECTANGLE, PATTERN, ROP, BETA1, BETA4, SURFACE
    {0xa2fc, 4, "NV308A SET_OPERATION"},             // ... SET_COLOR_FORMAT, POINT, SIZE_OUT
    {0xa30c, 1, "NV308A SIZE_IN"},
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
bool g_trace = false;  // RR7_RSX_TRACE: every method and its argument, logged
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

// An address in guest memory from an offset and the RSX's location for it: 0 its own memory, 1 the mapped main memory.
uint32_t guest_address(uint32_t location, uint32_t offset, const char* what) {
    if (location == 0) return kRsxLocal + offset;
    const uint32_t address = rsx_io_address(offset);
    if (!address) fatal("the RSX is to find %s at offset %08x of main memory, where nothing is mapped", what, offset);
    return address;
}

// A surface's memory, by its context object: SET_CONTEXT_DMA_COLOR_A's 0xFEED0000 is the RSX's memory, 0xFEED0001 the mapped main memory.
uint32_t surface_location(uint32_t object) {
    if (object == 0xFEED0000u) return 0;
    if (object == 0xFEED0001u) return 1;
    fatal("a surface in context object %08x, which this runtime doesn't know", object);
}

uint32_t state(uint32_t method) { return r.state[method / 4]; }

// A development aid: RR7_DRAWLOG=<n> logs every draw, clear and flip of the frame after the nth flip, in one line each;
// RR7_DRAWLOG=busy, the frame after the first with more than 300 draws, some to a surface in main memory (a race's scene).
uint64_t g_flips = 0, g_frame_draws = 0, g_busy_frame = 0;
bool g_frame_main_memory = false;
std::map<uint32_t, Surface> g_logged_surfaces;  // the surfaces drawn in the logged frame, written out at its flip
bool draw_logged() {
    static const char* const asked = std::getenv("RR7_DRAWLOG");
    static const uint64_t which = asked ? std::strtoull(asked, nullptr, 10) : 0;
    if (asked && std::strcmp(asked, "busy") == 0) return g_busy_frame && g_flips == g_busy_frame;
    return which && g_flips == which;
}
float state_float(uint32_t method) { return std::bit_cast<float>(r.state[method / 4]); }

// One draw of vertices [first, first + count), with the state the methods have left.
void draw(uint32_t first, uint32_t count, bool indexed, uint32_t at) {
    static Draw d;  // (one, reused: only the RSX's thread draws)
    d.reset();
    // The colour surface: only surface A, linear, not antialiased, in this first renderer.
    const uint32_t format = state(0x0208);
    if (((format >> 8) & 0xf) != 1) fatal("a draw to a surface of type %u (not linear), which the renderer doesn't take yet (at offset %08x)", (format >> 8) & 0xf, at);
    if ((format >> 12) & 0xf) fatal("a draw to an antialiased surface, which the renderer doesn't take yet (at offset %08x)", at);
    if (state(0x0220) != 1) fatal("a draw to colour targets %#x (only surface A is taken yet) (at offset %08x)", state(0x0220), at);
    d.color.format = format & 0x1f;
    d.color.pitch = state(0x020c);
    d.color.address = guest_address(surface_location(state(0x0194)), state(0x0210), "a colour surface");
    d.color.width = state(0x0200) >> 16, d.color.height = state(0x0204) >> 16;
    if ((state(0x0200) & 0xffff) || (state(0x0204) & 0xffff)) fatal("a surface whose clip doesn't start at 0, 0 (at offset %08x)", at);
    if (state(0x02b8)) fatal("a draw with window offset %08x, which the renderer doesn't take yet (at offset %08x)", state(0x02b8), at);
    d.color_mask = state(0x0324);
    // What the first renderer doesn't take: said by name.
    if (state(0x0a74)) {
        d.depth_test = true;
        d.depth_func = state(0x0a6c), d.depth_write = state(0x0a70) != 0;
        if (d.depth_func < 0x200 || d.depth_func > 0x207) fatal("a depth test with function %#x (at offset %08x)", d.depth_func, at);
    }
    // The depth bounds test (SET_DEPTH_BOUNDS_TEST_ENABLE, DEPTH_BOUNDS_MIN, MAX: floats) reads the depth surface too.
    if (state(0x0380)) d.depth_bounds = true, d.bounds_min = state_float(0x0384), d.bounds_max = state_float(0x0388);
    if (d.depth_test || state(0x0328) || d.depth_bounds) {
        d.depth_surface = true;
        const uint32_t depth_format = (format >> 5) & 7;  // 1 Z16, 2 Z24S8
        if (depth_format != 2) fatal("a depth test on a depth surface of format %u, which the renderer doesn't take yet (at offset %08x)", depth_format, at);
        d.depth.format = depth_format, d.depth.pitch = state(0x022c);
        d.depth.address = guest_address(surface_location(state(0x0198)), state(0x0214), "a depth surface");
        d.depth.width = state(0x0200) >> 16, d.depth.height = state(0x0204) >> 16;
    }
    // Polygon offset for filled primitives (SET_POLY_OFFSET_FILL_ENABLE, SCALE_FACTOR, BIAS): Vulkan's depth bias, the slope
    // factor and the constant. *With D32S8 a unit of bias follows the depth's exponent rather than 2^-24: a stated difference.*
    if (state(0x0a60) || state(0x0a64)) fatal("a draw with polygon offset for points or lines, which the renderer doesn't take yet (at offset %08x)", at);
    if (state(0x0a68)) d.depth_bias = true, d.bias_factor = state_float(0x0a78), d.bias_units = state_float(0x0a7c);
    // The stencil test (NV30/40's: Mesa's nv30-40_3d.xml.h): 0x328 on, 0x32c write mask, 0x330 function, 0x334 reference,
    // 0x338 function mask, 0x33c/0x340/0x344 the operations on failing, the depth test failing, passing; the back faces' at
    // 0x348 on (two-sided) to 0x364.
    if (state(0x0328)) {
        const auto side = [&](uint32_t base) {
            Draw::Stencil st;
            st.write_mask = state(base + 4) & 0xff, st.func = state(base + 8), st.ref = state(base + 12) & 0xff, st.func_mask = state(base + 16) & 0xff;
            st.fail = state(base + 20), st.zfail = state(base + 24), st.zpass = state(base + 28);
            if (st.func < 0x200 || st.func > 0x207) fatal("a stencil test with function %#x (at offset %08x)", st.func, at);
            return st;
        };
        d.stencil_test = true;
        d.stencil_front = side(0x0328);
        d.stencil_back = state(0x0348) ? side(0x0348) : d.stencil_front;
    }
    // State this renderer doesn't take, refused by name rather than ignored: flat shading, logic operations, two-sided vertex
    // colours, user clip planes, polygon stipple, and polygons drawn as points or lines.
    if (state(0x0368) == 0x1d00) fatal("a draw with flat shading, which the renderer doesn't take yet (at offset %08x)", at);
    if (state(0x0374)) fatal("a draw with a logic operation (%#x), which the renderer doesn't take yet (at offset %08x)", state(0x0378), at);
    if (state(0x142c)) fatal("a draw with two-sided vertex colours, which the renderer doesn't take yet (at offset %08x)", at);
    if (state(0x1478)) fatal("a draw with user clip planes (%#x), which the renderer doesn't take yet (at offset %08x)", state(0x1478), at);
    if (state(0x147c)) fatal("a draw with polygon stipple, which the renderer doesn't take yet (at offset %08x)", at);
    for (const uint32_t mode : {state(0x1828), state(0x182c)})
        if (mode && mode != 0x1b02) fatal("a draw with polygons drawn as %s, which the renderer doesn't take yet (at offset %08x)", mode == 0x1b00 ? "points" : "lines", at);
    // The alpha test (SET_ALPHA_FUNC: OpenGL's NEVER 0x200 to ALWAYS 0x207, bit 0 less, bit 1 equal, bit 2 greater; SET_ALPHA_REF):
    // on 8-bit surfaces the reference's low byte against the alpha as stored. *Its reference on a float surface isn't known here.*
    if (state(0x0304)) {
        const uint32_t function = state(0x0308);
        if (function < 0x200 || function > 0x207) fatal("an alpha test with function %#x (at offset %08x)", function, at);
        if (d.color.format != 8) fatal("an alpha test on a surface of format %u, which the renderer doesn't take yet (at offset %08x)", d.color.format, at);
        d.alpha_test = {static_cast<float>(state(0x030c) & 0xff), 255.0f, 0, 0, float(function & 1), float((function >> 1) & 1), float((function >> 2) & 1), 0};
    }
    if (state(0x173c)) fatal("a draw with a base vertex index, which the renderer doesn't take yet (at offset %08x)", at);
    d.primitive = r.primitive, d.first = first, d.count = count;
    if (indexed) {
        // The indices (SET_INDEX_ARRAY_ADDRESS, and INDEX_ARRAY_DMA: location in bit 0, type in bits 4 to 7, 0 32-bit and 1
        // 16-bit), big-endian, from `first`: the vertices fetched are the range they use, and the indices rebased to it.
        const uint32_t dma = state(0x1820), type = (dma >> 4) & 0xf;
        if (type > 1 || (dma & ~0xf1u)) fatal("an index array of format %08x, which the renderer doesn't take yet (at offset %08x)", dma, at);
        // Primitive restart (SET_RESTART_INDEX_ENABLE, RESTART_INDEX): the index that ends a primitive and starts the next,
        // marked in the list as kRestart for the renderer.
        const bool restart = state(0x1dac) != 0;
        const uint32_t restart_index = state(0x1db0);
        const uint32_t size = type ? 2 : 4;
        const uint32_t base = guest_address(dma & 1, state(0x181c), "an index array") + first * size;
        uint32_t lowest = UINT32_MAX, highest = 0;
        d.indices.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t v = size == 2 ? (uint32_t{guest_memory()[base + 2 * i]} << 8 | guest_memory()[base + 2 * i + 1]) : load32(base + 4 * i);
            if (restart && v == restart_index) {
                d.indices[i] = kRestart;
                continue;
            }
            d.indices[i] = v, lowest = std::min(lowest, v), highest = std::max(highest, v);
        }
        if (lowest > highest) return;  // (nothing but restarts)
        for (uint32_t& v : d.indices)
            if (v != kRestart) v -= lowest;
        d.first = lowest, d.count = highest - lowest + 1;
        d.indexed = true;
    }
    for (uint32_t a = 0; a < 16; ++a) {
        d.attribute_format[a] = state(0x1740 + 4 * a);
        if (((d.attribute_format[a] >> 4) & 0xf) == 0) continue;
        const uint32_t offset = state(0x1680 + 4 * a);
        d.attribute_address[a] = guest_address(offset >> 31, (offset & 0x7fffffffu) + state(0x1738), "a vertex array");
    }
    d.vertex_constants = r.constants.data(), d.constants_version = r.constants_version;
    // The vertex program: from its start to the instruction marked last, as big-endian bytes.
    {
        const uint32_t start = state(0x1ea0);
        if (start >= 512) fatal("a vertex program started at slot %u, past the 512", start);
        uint32_t last = start;
        while (last < 512 && !(r.program[4 * last + 3] & 1)) ++last;
        if (last == 512) fatal("the vertex program from slot %u has no last instruction", start);
        d.vertex_program.resize(size_t{last - start + 1} * 16);
        for (size_t w = 0; w < d.vertex_program.size() / 4; ++w) {
            const uint32_t big = std::byteswap(r.program[4 * start + w]);
            std::memcpy(d.vertex_program.data() + 4 * w, &big, 4);
        }
    }
    // The fragment program: from its address to the instruction marked as the end, with its inline constants.
    {
        const uint32_t program = state(0x08e4);
        const uint32_t location = program & 3;
        if (location != 1 && location != 2) fatal("a fragment program in location %u (at offset %08x)", location, at);
        const uint32_t address = guest_address(location == 1 ? 0 : 1, program & ~3u, "a fragment program");
        for (uint32_t offset = 0;; ) {
            const uint8_t* p = guest_memory() + address + offset;
            const auto word = [&](unsigned i) {
                const uint32_t w = (uint32_t{p[4 * i]} << 24) | (uint32_t{p[4 * i + 1]} << 16) | (uint32_t{p[4 * i + 2]} << 8) | p[4 * i + 3];
                return (w << 16) | (w >> 16);
            };
            const bool end = word(0) & 1, branch = word(2) >> 31;
            const bool constant = !branch && ((word(1) & 3) == 2 || (word(2) & 3) == 2 || (word(3) & 3) == 2);
            const uint32_t size = constant ? 32 : 16;
            d.fragment_program.insert(d.fragment_program.end(), p, p + size);
            offset += size;
            if (end) break;
            if (offset > 0x10000) fatal("the fragment program at %08x has no end in 64 KiB", address);
        }
    }
    // The viewport and the window.
    for (unsigned i = 0; i < 4; ++i) d.viewport_offset[i] = state_float(0x0a20 + 4 * i), d.viewport_scale[i] = state_float(0x0a30 + 4 * i);
    d.clip_x = state(0x0a00) & 0xffff, d.clip_width = state(0x0a00) >> 16, d.clip_y = state(0x0a04) & 0xffff, d.clip_height = state(0x0a04) >> 16;
    const uint32_t window = state(0x1d88);
    d.window_height = window & 0xfff;
    // The window's origin field (bits 12 to 15): 1 has window y counting down from the top row. *A reading from evidence:
    // PSL1GHT always writes 1 with a negative y scale and its samples draw upright, and this game does the same (its logo
    // came out upside down when 1 was taken as the bottom).* What 0 does isn't known here.
    const uint32_t origin = (window >> 12) & 0xf;
    if (origin != 1) fatal("a window with origin field %u, which the renderer doesn't take yet (at offset %08x)", origin, at);
    d.origin_bottom = false;
    if ((window >> 16) & 0xf) fatal("a window whose pixel centres are on whole numbers, which the renderer doesn't take yet (at offset %08x)", at);
    d.scissor_x = state(0x08c0) & 0xffff, d.scissor_width = state(0x08c0) >> 16, d.scissor_y = state(0x08c4) & 0xffff, d.scissor_height = state(0x08c4) >> 16;
    d.blend = state(0x0310) != 0;
    d.blend_src = state(0x0314), d.blend_dst = state(0x0318), d.blend_equation = state(0x0320);
    d.cull = state(0x183c) != 0;
    // The texture units: their state, and where an enabled one's texels are (the format word's location: 1 the RSX's memory, 2 main memory).
    for (uint32_t u = 0; u < 16; ++u) {
        TextureUnit& t = d.textures[u];
        const uint32_t base = 0x1a00 + 32 * u;
        t.offset = state(base), t.format = state(base + 4), t.wrap = state(base + 8), t.control0 = state(base + 0xc);
        t.remap = state(base + 0x10), t.filter = state(base + 0x14), t.rect = state(base + 0x18), t.border = state(base + 0x1c);
        t.control3 = state(0x1840 + 4 * u);
        // (Units the program doesn't sample may be enabled with no texture at all: the renderer checks those it reads.)
        const uint32_t location = t.format & 3;
        if (t.enabled() && (location == 1 || location == 2)) t.address = guest_address(location == 1 ? 0 : 1, t.offset, "a texture");
    }
    d.cull_face = state(0x1830), d.front_face = state(0x1834);
    // A development aid, never a way to play: RR7_SCENE_TEST=1 draws to main-memory surfaces (a race's scene) with no depth
    // test, culling or alpha test, to tell a fragment test's fault from the geometry's.
    // (RR7_SCENE_TEST=depth: only the depth test.)
    static const char* const scene_test = std::getenv("RR7_SCENE_TEST");
    if (scene_test && d.color.address < kRsxLocal) {
        d.depth_test = false;
        if (std::strcmp(scene_test, "depth") != 0) d.cull = false, d.alpha_test = {0, 0, 0, 0, 1, 1, 1, 0};
    }
    ++g_frame_draws;
    if (d.color.address < kRsxLocal) g_frame_main_memory = true;
    if (draw_logged()) {
        d.logged = true;
        uint32_t textures = 0;
        for (uint32_t u = 0; u < 16; ++u) textures |= d.textures[u].enabled() && d.textures[u].format ? 1u << u : 0;
        qilin::rt::log("(draw) surface %08x %ux%u format %u mask %08x | depth %s func %#x write %d at %08x | blend %d %08x %08x eq %08x | alpha test %d | prim %u, %u vertices%s | cull %d | textures %#x | vp %08x fp %08x",
                       d.color.address, d.color.width, d.color.height, d.color.format, d.color_mask, d.depth_test ? "on" : "off", d.depth_func, d.depth_write,
                       d.depth.address, d.blend, d.blend_src, d.blend_dst, d.blend_equation, state(0x0304) != 0, d.primitive, d.count, d.indexed ? " indexed" : "", d.cull,
                       textures, state(0x1ea0), state(0x08e4));
        qilin::rt::log("(draw)   viewport scale %g %g offset %g %g | clip %u,%u %ux%u | scissor %u,%u %ux%u | window height %u%s", d.viewport_scale[0],
                       d.viewport_scale[1], d.viewport_offset[0], d.viewport_offset[1], d.clip_x, d.clip_y, d.clip_width, d.clip_height, d.scissor_x,
                       d.scissor_y, d.scissor_width, d.scissor_height, d.window_height, d.origin_bottom ? " bottom" : "");
        for (uint32_t u = 0; u < 16; ++u)
            if (textures & (1u << u))
            {
                const TextureUnit& t = d.textures[u];
                qilin::rt::log("(draw)   texture %u at %08x format %08x size %ux%u remap %08x", u, t.address, t.format, t.rect >> 16, t.rect & 0xffff, t.remap);
                // Each texture once, decoded as the renderer decodes it (its first level), to texture-<address>.bmp.
                static std::map<uint32_t, bool> written;
                Texels tx;
                if (!written[t.address] && decode_texture(t, guest_memory(), tx).empty()) {
                    written[t.address] = true;
                    char name[64];
                    std::snprintf(name, sizeof name, "texture-%08x.bmp", t.address);
                    if (std::FILE* f = std::fopen(name, "wb")) {
                        const uint32_t bytes = tx.width * tx.height * 4;
                        uint8_t header[54] = {'B', 'M'};
                        const auto put = [&](size_t at2, uint32_t v) { for (unsigned i = 0; i < 4; ++i) header[at2 + i] = static_cast<uint8_t>(v >> (8 * i)); };
                        put(2, 54 + bytes), put(10, 54), put(14, 40), put(18, tx.width), put(22, static_cast<uint32_t>(-static_cast<int32_t>(tx.height)));
                        header[26] = 1, header[28] = 32;
                        std::fwrite(header, 1, 54, f);
                        for (uint32_t i = 0; i < tx.width * tx.height; ++i) {
                            const uint8_t* q = tx.rgba.data() + 4 * i;
                            const uint8_t bgra[4] = {q[2], q[1], q[0], q[3]};
                            std::fwrite(bgra, 1, 4, f);
                        }
                        std::fclose(f);
                    }
                }
            }
        g_logged_surfaces[d.color.address] = d.color;
        if (state(0x0a74)) g_logged_surfaces[d.depth.address != 0 ? d.depth.address : 0] = Surface{guest_address(surface_location(state(0x0198)), state(0x0214), "a depth surface"),
                                                                                                     state(0x022c), 0x100, state(0x0200) >> 16, state(0x0204) >> 16};
    }
    renderer_draw(d);
}

// CLEAR_SURFACE: the colour surface and the depth-stencil surface filled with their clear values: on the GPU, in the
// renderer's batch, or (a clear of only some colour channels) in guest memory after a flush. The argument's bits: depth 1,
// stencil 2, red 0x10, green 0x20, blue 0x40, alpha 0x80. *The clear rectangle is taken in the surface's own coordinates, from its top-left corner, and
// the colour write mask is not applied to a clear: stated choices, to be compared with the console. Whether the scissor limits
// a clear isn't known here: the game's clears seen so far are under a scissor covering their surface.*
void clear(uint32_t mask, uint32_t at) {
    if (draw_logged())
        qilin::rt::log("(draw) clear %#x of surface offset %08x %ux%u, colour %08x, depth-stencil %08x at offset %08x | scissor %u,%u %ux%u", mask, state(0x0210),
                       state(0x0200) >> 16, state(0x0204) >> 16, state(0x1d90), state(0x1d8c), state(0x0214), state(0x08c0) & 0xffff, state(0x08c4) & 0xffff,
                       state(0x08c0) >> 16, state(0x08c4) >> 16);
    if (mask & ~0xf3u) fatal("CLEAR_SURFACE with bits %#x, which this runtime doesn't know (at offset %08x)", mask, at);
    const uint32_t format = state(0x0208);
    if (((format >> 8) & 0xf) != 1) fatal("a clear of a surface of type %u (not linear) (at offset %08x)", (format >> 8) & 0xf, at);
    const uint32_t x0 = state(0x1d98) & 0xffff, x1 = x0 + (state(0x1d98) >> 16), y0 = state(0x1d9c) & 0xffff, y1 = y0 + (state(0x1d9c) >> 16);
    const uint32_t width = state(0x0200) >> 16, height = state(0x0204) >> 16;
    const uint32_t right = std::min(x1, width), bottom = std::min(y1, height);
    if ((mask & 0xf0) && state(0x0220) != 1) fatal("a clear of colour targets %#x (only surface A is taken yet) (at offset %08x)", state(0x0220), at);
    if ((mask & 0xf0) && (format & 0x1f) != 8) fatal("a clear of a colour surface of format %u, which this runtime doesn't clear yet (at offset %08x)", format & 0x1f, at);
    if ((mask & 3) && ((format >> 5) & 7) != 2) fatal("a clear of a depth surface of format %u, which this runtime doesn't clear yet (at offset %08x)", (format >> 5) & 7, at);
    // All four colour channels or none: the renderer's.
    if ((mask & 0xf0) == 0xf0 || (mask & 0xf0) == 0) {
        Clear c;
        c.x0 = std::min(x0, width), c.y0 = std::min(y0, height), c.x1 = right, c.y1 = bottom;
        if (mask & 0xf0) {
            c.color = true, c.color_value = state(0x1d90);
            c.color_surface = {guest_address(surface_location(state(0x0194)), state(0x0210), "a colour surface"), state(0x020c), format & 0x1f, width, height};
        }
        if (mask & 3) {
            c.depth = (mask & 1) != 0, c.stencil = (mask & 2) != 0, c.depth_value = state(0x1d8c);
            c.depth_surface = {guest_address(surface_location(state(0x0198)), state(0x0214), "a depth surface"), state(0x022c), 2, width, height};
        }
        renderer_clear(c);
        return;
    }
    renderer_flush("a clear");
    if (mask & 0xf0) {
        const uint32_t address = guest_address(surface_location(state(0x0194)), state(0x0210), "a colour surface"), pitch = state(0x020c);
        const uint32_t value = state(0x1d90);  // A8R8G8B8
        // The channels kept: a pixel is A, R, G, B in memory.
        const bool keep[4] = {!(mask & 0x80), !(mask & 0x10), !(mask & 0x20), !(mask & 0x40)};
        const uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
        for (uint32_t y = y0; y < bottom; ++y)
            for (uint32_t x = x0; x < right; ++x) {
                uint8_t* q = guest_memory() + address + y * pitch + 4 * x;
                for (unsigned c = 0; c < 4; ++c)
                    if (!keep[c]) q[c] = bytes[c];
            }
    }
    if (mask & 3) {
        const uint32_t address = guest_address(surface_location(state(0x0198)), state(0x0214), "a depth surface"), pitch = state(0x022c);
        const uint32_t value = state(0x1d8c);  // depth in the top 24 bits, stencil in the bottom 8
        for (uint32_t y = y0; y < bottom; ++y)
            for (uint32_t x = x0; x < right; ++x) {
                const uint32_t at_pixel = address + y * pitch + 4 * x;
                uint32_t word = load32(at_pixel);
                if (mask & 1) word = (word & 0xffu) | (value & 0xffffff00u);
                if (mask & 2) word = (word & 0xffffff00u) | (value & 0xffu);
                store32(at_pixel, word);
            }
    }
}

// A flip: the display buffer, from guest memory, to the window.
void flip(uint32_t id, uint32_t at) {
    if (draw_logged()) {
        qilin::rt::log("(draw) flip of display buffer %u", id);
        renderer_flush("a logged flip");
        // Each surface drawn in the frame, as guest memory holds it, to surface-<address>.bmp (A8R8G8B8 ones).
        for (const auto& [address, sf] : g_logged_surfaces) {
            if (sf.format != 8 && sf.format != 0x100) continue;  // (0x100: a Z24S8 surface, its depth as grey)
            char name[64];
            std::snprintf(name, sizeof name, "surface-%08x.bmp", address);
            std::FILE* f = std::fopen(name, "wb");
            if (!f) continue;
            const uint32_t bytes = sf.width * sf.height * 4;
            uint8_t header[54] = {'B', 'M'};
            const auto put = [&](size_t pos, uint32_t v) { for (unsigned i = 0; i < 4; ++i) header[pos + i] = static_cast<uint8_t>(v >> (8 * i)); };
            put(2, 54 + bytes), put(10, 54), put(14, 40), put(18, sf.width), put(22, static_cast<uint32_t>(-static_cast<int32_t>(sf.height)));
            header[26] = 1, header[28] = 32;
            std::fwrite(header, 1, 54, f);
            std::vector<uint8_t> row(sf.width * 4);
            for (uint32_t y = 0; y < sf.height; ++y) {
                const uint8_t* p = guest_memory() + address + y * sf.pitch;
                for (uint32_t x = 0; x < sf.width; ++x) {
                    if (sf.format == 0x100) row[4 * x] = row[4 * x + 1] = row[4 * x + 2] = p[4 * x], row[4 * x + 3] = 255;  // depth's top byte
                    else row[4 * x] = p[4 * x + 3], row[4 * x + 1] = p[4 * x + 2], row[4 * x + 2] = p[4 * x + 1], row[4 * x + 3] = p[4 * x];
                }
                std::fwrite(row.data(), 1, row.size(), f);
            }
            std::fclose(f);
        }
    }
    ++g_flips;
    if (!g_busy_frame && g_frame_draws > 300 && g_frame_main_memory) g_busy_frame = g_flips;
    g_frame_draws = 0, g_frame_main_memory = false;
    renderer_flush("a flip");
    const DisplayBuffer b = display_buffer(id);
    if (!b.set) fatal("the RSX is to show display buffer %u, which the game never set (at offset %08x)", id, at);
    const VideoMode mode = video_mode();
    if (!mode.configured || mode.format != 0) fatal("a flip with the video output in format %u, which this runtime doesn't show yet", mode.format);
    // Drawn by the renderer and still on the GPU: shown from there.
    if (renderer_flip(kRsxLocal + b.offset, b.pitch, b.width, b.height)) return;
    std::vector<uint8_t> rgba(size_t{b.width} * b.height * 4);
    const uint8_t* from = guest_memory() + kRsxLocal + b.offset;
    for (uint32_t y = 0; y < b.height; ++y)
        for (uint32_t x = 0; x < b.width; ++x) {
            const uint8_t* p = from + size_t{y} * b.pitch + 4 * x;  // X8R8G8B8: X, R, G, B in memory
            uint8_t* q = rgba.data() + (size_t{y} * b.width + x) * 4;
            q[0] = p[1], q[1] = p[2], q[2] = p[3], q[3] = 255;
        }
    present_frame(Frame{rgba.data(), -1, b.width, b.height});
}

// NV308A's image from the CPU: words given in the command stream (COLOR, 0xa400 on) written into the 2D surface from POINT,
// a row of SIZE_IN's width at a time. At its first word the renderer is told which bytes it covers: its batch is written
// back first if it drew there, so the words land in guest memory after everything drawn before (renderer_guest_written). Fields as PSL1GHT's headers name them (it puts
// the object on subchannel 5 too, and calls the set-up rsxSetTransferImage's inline form). Only 32-bit surface formats
// (Y32, A8R8G8B8) are taken: one word a pixel.
void inline_word(uint32_t index, uint32_t word, uint32_t at) {
    const uint32_t format = state(0x6300);
    if (format != 0xb && format != 0xa) fatal("an inline transfer to a surface of format %u, which this runtime doesn't take yet (at offset %08x)", format, at);
    const uint32_t point = state(0xa304), in_size = state(0xa30c), out_size = state(0xa308);
    if (in_size != out_size) fatal("an inline transfer that scales (%08x to %08x) (at offset %08x)", in_size, out_size, at);
    const uint32_t width = in_size & 0xffff, height = in_size >> 16;
    // The words come padded to an even count (PSL1GHT's padSizeInWords): the padding is no pixel.
    if (width && index >= width * height && index < ((width * height + 1) & ~1u)) return;
    if (!width || index >= width * height) fatal("an inline transfer's word %u, past its %u by %u (at offset %08x)", index, width, height, at);
    const uint32_t x = (point & 0xffff) + index % width, y = (point >> 16) + index / width;
    const uint32_t dest = guest_address(surface_location(state(0x6188)), state(0x630c), "an inline transfer's surface"), pitch = state(0x6304) >> 16;
    if (index == 0)
        renderer_guest_written(dest + (point >> 16) * pitch + (point & 0xffff) * 4, uint64_t{height - 1} * pitch + uint64_t{width} * 4, "an inline transfer");
    store32(dest + y * pitch + x * 4, word);
}

// The RSX's last jumps, calls and returns (from, to), for the report of a stop.
std::array<std::pair<uint32_t, uint32_t>, 8> g_transfers{};
std::array<uint32_t, 8> g_transfer_puts{};
uint32_t g_transfer_count = 0;
uint32_t g_put = 0;  // put, as last read
void note_transfer(uint32_t from, uint32_t to) {
    g_transfer_puts[g_transfer_count % g_transfers.size()] = g_put;
    g_transfers[g_transfer_count++ % g_transfers.size()] = {from, to};
}
std::string recent_transfers() {
    std::string out;
    for (uint32_t i = 0; i < g_transfers.size() && i < g_transfer_count; ++i) {
        const auto& [from, to] = g_transfers[(g_transfer_count - 1 - i) % g_transfers.size()];
        char item[64];
        std::snprintf(item, sizeof item, "%s%08x->%08x (put %08x)", out.empty() ? "" : ", ", from, to, g_transfer_puts[(g_transfer_count - 1 - i) % g_transfers.size()]);
        out += item;
    }
    return out;
}
// The words at a command offset, as they are now (for a stop's message): the newest jump's target, where the bad header is.
std::string words_at(uint32_t offset, unsigned count) {
    std::string out;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t address = rsx_io_address(offset + 4 * i);
        char item[16];
        std::snprintf(item, sizeof item, "%s%08x", i ? " " : "", address ? load32(address) : 0u);
        out += address ? item : " (unmapped)";
        if (!address) break;
    }
    return out;
}

// One method with one argument.
void run_method(uint32_t method, uint32_t argument, uint32_t at) {
    if (is_state(method)) {
        r.state[method / 4] = argument;
        return;
    }
    if (method >= 0xa400 && method < 0xa600) {  // NV308A COLOR: an inline transfer's words
        inline_word((method - 0xa400) / 4, argument, at);
        return;
    }
    if (method >= 0x0b80 && method < 0x0c00) {  // SET_TRANSFORM_PROGRAM: a word of a vertex program
        if (r.program_at >= r.program.size()) fatal("a vertex program written past its 512 instructions (at offset %08x)", at);
        r.program[r.program_at++] = argument;
        return;
    }
    if (method >= 0x1f00 && method < 0x1f80) {  // SET_TRANSFORM_CONSTANT: a word of the constants
        if (r.constant_at >= r.constants.size()) fatal("vertex constants written past the 468 (at offset %08x)", at);
        r.constants[r.constant_at++] = std::bit_cast<float>(argument);
        ++r.constants_version;
        return;
    }
    switch (method) {
    // (Before every label, reference or wait, everything drawn so far is written back to guest memory: renderer.hpp.)
    case 0x0050:  // NV406E_SET_REFERENCE: the game's own mark of how far the RSX has got, readable beside put and get
        renderer_flush("a reference");
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
        renderer_flush("a semaphore release");
        store32(label_address(r.label, at), argument);
        return;
    case 0x0068:  // NV406E_SEMAPHORE_ACQUIRE: wait until the game has made it this value
        renderer_flush("a semaphore wait");
        for (const uint32_t address = label_address(r.label, at); load32(address) != argument;) std::this_thread::yield();
        return;
    case 0x1d70:  // BACK_END_WRITE_SEMAPHORE_RELEASE: write the label at SET_SEMAPHORE_OFFSET once everything drawn so far is in the surface.
        renderer_flush("a back-end semaphore");
        // The chip writes the argument's red and blue bytes exchanged (the game's library exchanges them first, so the label reads as meant).
        store32(label_address(r.state[0x1d6c / 4], at), (argument & 0xff00ff00u) | ((argument & 0xff) << 16) | ((argument >> 16) & 0xff));
        return;
    case 0x1d74:  // TEXTURE_READ_SEMAPHORE_RELEASE: the same label, as given, once every texture read so far is done
        renderer_flush("a texture-read semaphore");
        store32(label_address(r.state[0x1d6c / 4], at), argument);
        return;
    case kMethodWaitFlip:  // (the runtime's own) wait until the last flip has been shown: a flip is shown at once here
        return;
    case 0x1d94:  // CLEAR_SURFACE
        clear(argument, at);
        return;
    case kMethodFlip:  // (the runtime's own) show a display buffer
        flip(argument, at);
        return;
    // Vertex programs and their constants: uploaded a word at a time from where the load methods point.
    case 0x1e9c:  // SET_TRANSFORM_PROGRAM_LOAD: the instruction slot the next words go to
        if (argument >= 512) fatal("a vertex program loaded at slot %u, past the 512 (at offset %08x)", argument, at);
        r.program_at = argument * 4;
        return;
    case 0x1efc:  // SET_TRANSFORM_CONSTANT_LOAD: the constant the next words go to
        if (argument >= 468) fatal("vertex constants loaded at %u, past the 468 (at offset %08x)", argument, at);
        r.constant_at = argument * 4;
        return;
    case 0x1710:  // INVALIDATE_VERTEX_CACHE_FILE
    case 0x1714:  // INVALIDATE_VERTEX_FILE: the RSX's caches of vertices; this one reads guest memory at each draw
        return;
    case 0x1fd8:  // the texture cache's control (PSL1GHT's TEX_CACHE_CTL): 1 invalidates the fragment programs' textures, 2 the
                  // vertex programs'. Here textures are checked against guest memory once a batch (renderer.cpp: why that is enough).
        if (argument != 1 && argument != 2) qilin::rt::fatal("the RSX's texture cache is given %08x", argument);
        return;
    case 0x1808:  // SET_BEGIN_END: a primitive's start (its kind), or 0 for its end, when the ranges since are drawn
        if (argument) {
            r.primitive = argument;
            r.ranges.clear();
        } else {
            for (const Rsx::Range& range : r.ranges) draw(range.first, range.count, range.indexed, at);
            r.ranges.clear();
        }
        return;
    case 0x1824:  // DRAW_INDEX_ARRAY: indices [first, first + count) of the index array, count - 1 in the top byte
        if (!r.primitive) fatal("DRAW_INDEX_ARRAY outside SET_BEGIN_END (at offset %08x)", at);
        r.ranges.push_back({argument & 0xffffffu, (argument >> 24) + 1, true});
        return;
    case 0x1814:  // DRAW_ARRAYS: vertices [first, first + count) of the arrays, count - 1 in the top byte
        if (!r.primitive) fatal("DRAW_ARRAYS outside SET_BEGIN_END (at offset %08x)", at);
        r.ranges.push_back({argument & 0xffffffu, (argument >> 24) + 1, false});
        return;
    default:
        if (g_survey) {
            auto& seen = g_surveyed[method];
            if (seen.count++ == 0) seen.first = argument;
            if (method == kMethodFlip || std::chrono::steady_clock::now() > g_survey_ends) end_survey();
            return;
        }
        const uint32_t target = g_transfer_count ? g_transfers[(g_transfer_count - 1) % g_transfers.size()].second : 0;
        fatal("the RSX is given method %04x (argument %08x, at offset %08x), which this runtime doesn't provide yet; its last jumps, newest first: %s; "
              "now at the newest jump's target: %s; before the header: %s",
              method, argument, at, recent_transfers().c_str(), words_at(target, 8).c_str(), words_at(at >= 32 ? at - 32 : 0, 8).c_str());
    }
}

void serve() {
    const uint32_t control = rsx_setup().control;
    r.get = load32(control + 4);
    // The state the system's start-up leaves, which the game relies on without setting: the depth surface in the RSX's own
    // memory. *A stated choice: the game sets its other surfaces' context objects (colour B to D in the RSX's memory) but
    // never the depth surface's, whose offset fits the RSX's memory.*
    r.state[0x0198 / 4] = 0xFEED0000u;
    // Likewise the 2D surface's (subchannel 3) source and destination: the game sets neither and its offsets fit the RSX's
    // memory (*a stated choice, as the depth surface's*).
    r.state[0x6184 / 4] = r.state[0x6188 / 4] = 0xFEED0000u;
    // And the clear's rectangle (SET_CLEAR_RECT_HORIZONTAL, VERTICAL: origin and size) covering everything: the game never
    // sets it and clears with it, so the system's start-up must (*a stated choice*: 0 to 4095 each way, clipped to the surface).
    r.state[0x1d98 / 4] = r.state[0x1d9c / 4] = 0x0fff0000u;
    g_survey = std::getenv("RR7_RSX_SURVEY") != nullptr;
    g_trace = std::getenv("RR7_RSX_TRACE") != nullptr;
    g_survey_ends = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    uint32_t idle = 0;
    std::chrono::steady_clock::time_point idle_since{};
    bool idle_flushed = false;
    for (;;) {
        const uint32_t put = load32(control);
        // (The commands before put were written before it moved: the game's own fence orders that, and this one keeps
        // this thread's reads of them after its read of put.)
        std::atomic_thread_fence(std::memory_order_acquire);
        g_put = put;
        if (r.get == put) {
            // Out of commands for 2 ms: what was drawn goes to guest memory. (Not at once: the game often runs the RSX dry
            // mid-frame while it writes the rest, and a flush writes every surface drawn back. A game can only rely on drawn
            // results through a label, a wait or a flip, which flush at once; this is for one that watches get.)
            if (idle == 0) idle_since = std::chrono::steady_clock::now(), idle_flushed = false;
            if (!idle_flushed && std::chrono::steady_clock::now() - idle_since > std::chrono::milliseconds(2)) {
                renderer_flush("the RSX idle");
                idle_flushed = true;
            }
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
            if (next != r.get) note_transfer(r.get, next);
            if (next == r.get) {  // to itself: the game's way of holding the RSX here until it writes over it
                renderer_flush("a jump to itself");
                if (g_survey && std::chrono::steady_clock::now() > g_survey_ends) end_survey();
                std::this_thread::yield();
                continue;
            }
        } else if ((header & 3) == 1) {  // a jump, as made later
            next = header & 0xfffffffcu;
            note_transfer(r.get, next);
        } else if ((header & 3) == 2) {  // a call
            if (r.in_call) fatal("the RSX is given a call inside a call, at offset %08x", r.get);
            r.in_call = true;
            r.return_to = r.get + 4;
            next = header & 0xfffffffcu;
            note_transfer(r.get, next);
        } else if (header == 0x00020000u) {  // a return
            if (!r.in_call) fatal("the RSX is given a return with no call, at offset %08x", r.get);
            r.in_call = false;
            next = r.return_to;
            note_transfer(r.get, next);
        } else {
            if (header & 0xa0030003u) fatal("the RSX is given a command it can't read: %08x at offset %08x", header, r.get);
            const uint32_t count = (header >> 18) & 0x7ff, method = header & 0xfffc;
            const bool same = (header & 0x40000000u) != 0;  // every argument to the one method
            for (uint32_t i = 0; i < count; ++i) {
                if (g_trace) qilin::rt::log("(rsx) %08x: %04x = %08x", r.get, same ? method : method + 4 * i, load32(address + 4 + 4 * i));
                run_method(same ? method : method + 4 * i, load32(address + 4 + 4 * i), r.get);
            }
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

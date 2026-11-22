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
#include <array>
#include <bit>
#include <cstring>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <map>
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
    // Between SET_BEGIN_END's begin and its end: the primitive, and the ranges of vertices drawn.
    uint32_t primitive = 0;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
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
float state_float(uint32_t method) { return std::bit_cast<float>(r.state[method / 4]); }

// One draw of vertices [first, first + count), with the state the methods have left.
void draw(uint32_t first, uint32_t count, uint32_t at) {
    Draw d;
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
    d.color_mask = state(0x0324);
    // What the first renderer doesn't take: said by name.
    if (state(0x0a74)) fatal("a draw with the depth test, which the renderer doesn't take yet (at offset %08x)", at);
    if (state(0x032c)) fatal("a draw with the stencil test, which the renderer doesn't take yet (at offset %08x)", at);
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
    for (uint32_t a = 0; a < 16; ++a) {
        d.attribute_format[a] = state(0x1740 + 4 * a);
        if (((d.attribute_format[a] >> 4) & 0xf) == 0) continue;
        const uint32_t offset = state(0x1680 + 4 * a);
        d.attribute_address[a] = guest_address(offset >> 31, (offset & 0x7fffffffu) + state(0x1738), "a vertex array");
    }
    d.vertex_constants = r.constants.data();
    // The vertex program: from its start to the instruction marked last.
    for (uint32_t i = state(0x1ea0); i < 512; ++i) {
        for (unsigned k = 0; k < 4; ++k)
            for (int b = 24; b >= 0; b -= 8) d.vertex_program.push_back(static_cast<uint8_t>(r.program[4 * i + k] >> b));
        if (r.program[4 * i + 3] & 1) break;
        if (i == 511) fatal("the vertex program from slot %u has no last instruction", state(0x1ea0));
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
    renderer_draw(d);
}

// CLEAR_SURFACE: the colour surface and the depth-stencil surface filled with their clear values, in guest memory (where
// this first renderer keeps every surface: renderer.hpp). The argument's bits: depth 1, stencil 2, red 0x10, green 0x20,
// blue 0x40, alpha 0x80. *The clear rectangle is taken in the surface's own coordinates, from its top-left corner, and
// the colour write mask is not applied to a clear: stated choices, to be compared with the console.*
void clear(uint32_t mask, uint32_t at) {
    renderer_flush();
    if (mask & ~0xf3u) fatal("CLEAR_SURFACE with bits %#x, which this runtime doesn't know (at offset %08x)", mask, at);
    const uint32_t format = state(0x0208);
    if (((format >> 8) & 0xf) != 1) fatal("a clear of a surface of type %u (not linear) (at offset %08x)", (format >> 8) & 0xf, at);
    const uint32_t x0 = state(0x1d98) & 0xffff, x1 = x0 + (state(0x1d98) >> 16), y0 = state(0x1d9c) & 0xffff, y1 = y0 + (state(0x1d9c) >> 16);
    const uint32_t width = state(0x0200) >> 16, height = state(0x0204) >> 16;
    const uint32_t right = std::min(x1, width), bottom = std::min(y1, height);
    if (mask & 0xf0) {
        if (state(0x0220) != 1) fatal("a clear of colour targets %#x (only surface A is taken yet) (at offset %08x)", state(0x0220), at);
        if ((format & 0x1f) != 8) fatal("a clear of a colour surface of format %u, which this runtime doesn't clear yet (at offset %08x)", format & 0x1f, at);
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
        const uint32_t depth_format = (format >> 5) & 7;  // 1 Z16, 2 Z24S8
        if (depth_format != 2) fatal("a clear of a depth surface of format %u, which this runtime doesn't clear yet (at offset %08x)", depth_format, at);
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
    renderer_flush();
    const DisplayBuffer b = display_buffer(id);
    if (!b.set) fatal("the RSX is to show display buffer %u, which the game never set (at offset %08x)", id, at);
    const VideoMode mode = video_mode();
    if (!mode.configured || mode.format != 0) fatal("a flip with the video output in format %u, which this runtime doesn't show yet", mode.format);
    std::vector<uint8_t> rgba(size_t{b.width} * b.height * 4);
    const uint8_t* from = guest_memory() + kRsxLocal + b.offset;
    for (uint32_t y = 0; y < b.height; ++y)
        for (uint32_t x = 0; x < b.width; ++x) {
            const uint8_t* p = from + size_t{y} * b.pitch + 4 * x;  // X8R8G8B8: X, R, G, B in memory
            uint8_t* q = rgba.data() + (size_t{y} * b.width + x) * 4;
            q[0] = p[1], q[1] = p[2], q[2] = p[3], q[3] = 255;
        }
    present_frame(rgba.data(), b.width, b.height);
}

// One method with one argument.
void run_method(uint32_t method, uint32_t argument, uint32_t at) {
    if (is_state(method)) {
        r.state[method / 4] = argument;
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
        return;
    }
    switch (method) {
    // (Before every label, reference or wait, everything drawn so far is written back to guest memory: renderer.hpp.)
    case 0x0050:  // NV406E_SET_REFERENCE: the game's own mark of how far the RSX has got, readable beside put and get
        renderer_flush();
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
        renderer_flush();
        store32(label_address(r.label, at), argument);
        return;
    case 0x0068:  // NV406E_SEMAPHORE_ACQUIRE: wait until the game has made it this value
        renderer_flush();
        for (const uint32_t address = label_address(r.label, at); load32(address) != argument;) std::this_thread::yield();
        return;
    case 0x1d70:  // BACK_END_WRITE_SEMAPHORE_RELEASE: write the label at SET_SEMAPHORE_OFFSET once everything drawn so far is in the surface.
        renderer_flush();
        // The chip writes the argument's red and blue bytes exchanged (the game's library exchanges them first, so the label reads as meant).
        store32(label_address(r.state[0x1d6c / 4], at), (argument & 0xff00ff00u) | ((argument & 0xff) << 16) | ((argument >> 16) & 0xff));
        return;
    case 0x1d74:  // TEXTURE_READ_SEMAPHORE_RELEASE: the same label, as given, once every texture read so far is done
        renderer_flush();
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
                  // vertex programs'. Textures are read from guest memory at each draw here, so there is nothing to invalidate.
        if (argument != 1 && argument != 2) qilin::rt::fatal("the RSX's texture cache is given %08x", argument);
        return;
    case 0x1808:  // SET_BEGIN_END: a primitive's start (its kind), or 0 for its end, when the ranges since are drawn
        if (argument) {
            r.primitive = argument;
            r.ranges.clear();
        } else {
            for (const auto& [first, count] : r.ranges) draw(first, count, at);
            r.ranges.clear();
        }
        return;
    case 0x1814:  // DRAW_ARRAYS: vertices [first, first + count) of the arrays, count - 1 in the top byte
        if (!r.primitive) fatal("DRAW_ARRAYS outside SET_BEGIN_END (at offset %08x)", at);
        r.ranges.emplace_back(argument & 0xffffffu, (argument >> 24) + 1);
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
    // The state the system's start-up leaves, which the game relies on without setting: the depth surface in the RSX's own
    // memory. *A stated choice: the game sets its other surfaces' context objects (colour B to D in the RSX's memory) but
    // never the depth surface's, whose offset fits the RSX's memory.*
    r.state[0x0198 / 4] = 0xFEED0000u;
    g_survey = std::getenv("RR7_RSX_SURVEY") != nullptr;
    g_trace = std::getenv("RR7_RSX_TRACE") != nullptr;
    g_survey_ends = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    uint32_t idle = 0;
    for (;;) {
        const uint32_t put = load32(control);
        if (r.get == put) {
            if (idle == 0) renderer_flush();  // out of commands: what was drawn goes to guest memory
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
                renderer_flush();
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

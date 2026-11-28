// The RSX's state as its commands leave it, and a draw gathered from it (rsx_state.cpp): the part of the RSX's thread
// (rsx.cpp) that needs no game, so that ridgeracer7_rsx_bench can run it on a synthetic command stream.
//
// A command is a header and its arguments: the header names a method (a register of the RSX, by its offset) and how many
// arguments follow; each goes to the next method along, or all to the same one. Most methods only set state, kept here in
// a table by method; the vertex programs' words and constants are kept as they are uploaded, and SET_BEGIN_END's ranges
// until its end. Everything else is the caller's (`Acts`).
//
// Runtime code: no std::format.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include "qilin/rt/report.hpp"
#include "renderer.hpp"

namespace rr7 {

// The RSX's state: methods that only set something, which is kept until a method that acts reads it.
// Each entry is a run of methods (4 apart) by the name the graphics chip's public documentation gives
// the first. A method in none of these runs, and not one of those that act, stops the game.
struct StateMethods {
    uint16_t first, count;
    const char* name;
};
inline constexpr StateMethods kState[] = {
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

// The table as a bit a method, made when the program is compiled: every command word asks.
inline constexpr auto kStateBits = [] {
    std::array<uint64_t, 0x10000 / 4 / 64> bits{};
    for (const StateMethods& m : kState)
        for (uint32_t i = 0; i < m.count; ++i) {
            const uint32_t word = (m.first + 4u * i) / 4;
            bits[word / 64] |= uint64_t{1} << (word % 64);
        }
    return bits;
}();
constexpr bool is_state(uint32_t method) { return method < 0x10000 && ((kStateBits[method / 4 / 64] >> (method / 4 % 64)) & 1) != 0; }
// How many state methods in a row from each method on (0 for one that isn't state): a command's arguments to them are
// stored in one pass.
inline constexpr auto kStateRun = [] {
    std::array<uint16_t, 0x10000 / 4> run{};
    for (uint32_t word = 0x10000 / 4; word-- > 0;)
        run[word] = is_state(word * 4) ? static_cast<uint16_t>(word + 1 < run.size() ? run[word + 1] + 1 : 1) : uint16_t{0};
    return run;
}();
static_assert(kStateRun[0x1a00 / 4] == 128 && kStateRun[0x1a04 / 4] == 127 && kStateRun[0x1a00 / 4 + 128] == 0 && kStateRun[0x0100 / 4] == 0);
// (Every run's ends are in, and methods that act are not.)
static_assert([] {
    for (const StateMethods& m : kState)
        if (!is_state(m.first) || !is_state(m.first + 4u * (m.count - 1u))) return false;
    for (const uint32_t acting : {0x0050u, 0x0064u, 0x0068u, 0x006cu, 0x0100u, 0x1808u, 0x1814u, 0x1824u, 0x1d70u, 0x1d74u, 0x1d94u, 0x1e9cu, 0x1efcu, 0xa400u, 0x10000u})
        if (is_state(acting)) return false;
    return true;
}());

inline uint32_t load_be32(const uint8_t* p) {
    uint32_t w;
    std::memcpy(&w, p, 4);
    return std::byteswap(w);
}
// `count` big-endian words to host words, in one pass (the compiler makes it a run of vector shuffles).
inline void load_be32s(uint32_t* to, const uint8_t* from, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) to[i] = load_be32(from + 4 * i);
}

struct RsxState {
    std::array<uint32_t, 0x10000 / 4> state{};  // the last argument given to each method
    // The vertex programs' memory (512 instructions of four words) and their constants (468 vec4), and where the next
    // word written to each goes.
    std::array<uint32_t, 512 * 4> program{};
    uint32_t program_at = 0;
    uint64_t program_version = 0;  // counts uploads to it (gather_draw remakes a draw's program only when this changes)
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
    // The vertex program as the last draw had it (gather_draw): from slot `made_start`, at `made_version`, as bytes.
    uint32_t made_start = UINT32_MAX;
    uint64_t made_version = 0;
    std::vector<uint8_t> made_program;

    uint32_t operator[](uint32_t method) const { return state[method / 4]; }
    float as_float(uint32_t method) const { return std::bit_cast<float>(state[method / 4]); }
};

// One method with one argument. `Acts` has what the RSX's thread does itself: draw(first, count, indexed, at) for each
// range at SET_BEGIN_END's end, inline_word(index, word, at) for NV308A's COLOR words, method(method, argument, at) for
// every other method.
template <class Acts>
void run_method(RsxState& s, uint32_t method, uint32_t argument, uint32_t at, Acts& acts) {
    using qilin::rt::fatal;
    if (is_state(method)) {
        s.state[method / 4] = argument;
        return;
    }
    if (method >= 0xa400 && method < 0xa600) {  // NV308A COLOR: an inline transfer's words
        acts.inline_word((method - 0xa400) / 4, argument, at);
        return;
    }
    if (method >= 0x0b80 && method < 0x0c00) {  // SET_TRANSFORM_PROGRAM: a word of a vertex program
        if (s.program_at >= s.program.size()) fatal("a vertex program written past its 512 instructions (at offset %08x)", at);
        s.program[s.program_at++] = argument;
        ++s.program_version;
        return;
    }
    if (method >= 0x1f00 && method < 0x1f80) {  // SET_TRANSFORM_CONSTANT: a word of the constants
        if (s.constant_at >= s.constants.size()) fatal("vertex constants written past the 468 (at offset %08x)", at);
        s.constants[s.constant_at++] = std::bit_cast<float>(argument);
        ++s.constants_version;
        return;
    }
    switch (method) {
    // Vertex programs and their constants: uploaded a word at a time from where the load methods point.
    case 0x1e9c:  // SET_TRANSFORM_PROGRAM_LOAD: the instruction slot the next words go to
        if (argument >= 512) fatal("a vertex program loaded at slot %u, past the 512 (at offset %08x)", argument, at);
        s.program_at = argument * 4;
        return;
    case 0x1efc:  // SET_TRANSFORM_CONSTANT_LOAD: the constant the next words go to
        if (argument >= 468) fatal("vertex constants loaded at %u, past the 468 (at offset %08x)", argument, at);
        s.constant_at = argument * 4;
        return;
    case 0x1808:  // SET_BEGIN_END: a primitive's start (its kind), or 0 for its end, when the ranges since are drawn
        if (argument) {
            s.primitive = argument;
            s.ranges.clear();
        } else {
            for (const RsxState::Range& range : s.ranges) acts.draw(range.first, range.count, range.indexed, at);
            s.ranges.clear();
        }
        return;
    case 0x1824:  // DRAW_INDEX_ARRAY: indices [first, first + count) of the index array, count - 1 in the top byte
        if (!s.primitive) fatal("DRAW_INDEX_ARRAY outside SET_BEGIN_END (at offset %08x)", at);
        s.ranges.push_back({argument & 0xffffffu, (argument >> 24) + 1, true});
        return;
    case 0x1814:  // DRAW_ARRAYS: vertices [first, first + count) of the arrays, count - 1 in the top byte
        if (!s.primitive) fatal("DRAW_ARRAYS outside SET_BEGIN_END (at offset %08x)", at);
        s.ranges.push_back({argument & 0xffffffu, (argument >> 24) + 1, false});
        return;
    default:
        acts.method(method, argument, at);
    }
}

// A command's methods: `count` arguments (big-endian, at `args`) from `method` on, or all to `method` (`same`), as
// run_method would run them one by one. Runs of state methods are stored in one pass (all to the one method: its last
// argument), as are words of the vertex programs and their constants; the rest go one at a time.
template <class Acts>
void run_command(RsxState& s, uint32_t method, uint32_t count, bool same, const uint8_t* args, uint32_t at, Acts& acts) {
    using qilin::rt::fatal;
    if (acts.trace) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t m = same ? method : method + 4 * i, argument = load_be32(args + 4 * i);
            qilin::rt::log("(rsx) %08x: %04x = %08x", at, m, argument);
            run_method(s, m, argument, at, acts);
        }
        return;
    }
    for (uint32_t i = 0; i < count;) {
        const uint32_t m = same ? method : method + 4 * i, left = count - i;
        if (const uint32_t run = m < 0x10000 ? kStateRun[m / 4] : 0) {
            if (same) {  // (every argument left to this one method: the last is what stays)
                s.state[m / 4] = load_be32(args + 4 * (count - 1));
                return;
            }
            const uint32_t n = std::min(run, left);
            load_be32s(&s.state[m / 4], args + 4 * i, n);
            i += n;
            continue;
        }
        if (m >= 0x1f00 && m < 0x1f80) {  // SET_TRANSFORM_CONSTANT: words of the constants
            const uint32_t n = same ? left : std::min(left, (0x1f80 - m) / 4);
            if (n > s.constants.size() - s.constant_at) fatal("vertex constants written past the 468 (at offset %08x)", at);
            float* to = s.constants.data() + s.constant_at;
            for (uint32_t k = 0; k < n; ++k) to[k] = std::bit_cast<float>(load_be32(args + 4 * (i + k)));
            s.constant_at += n, s.constants_version += n, i += n;
            continue;
        }
        if (m >= 0x0b80 && m < 0x0c00) {  // SET_TRANSFORM_PROGRAM: words of a vertex program
            const uint32_t n = same ? left : std::min(left, (0x0c00 - m) / 4);
            if (n > s.program.size() - s.program_at) fatal("a vertex program written past its 512 instructions (at offset %08x)", at);
            load_be32s(s.program.data() + s.program_at, args + 4 * i, n);
            s.program_at += n, s.program_version += n, i += n;
            continue;
        }
        run_method(s, m, load_be32(args + 4 * i), at, acts);
        ++i;
    }
}

// Where the RSX finds what a draw reads: guest memory, and the mapping of its main memory (rsx_io_address: 0 where nothing
// is mapped).
struct GuestView {
    const uint8_t* memory = nullptr;
    uint32_t (*io_address)(uint32_t offset) = nullptr;
};

// An address in guest memory from an offset and the RSX's location for it: 0 its own memory, 1 the mapped main memory.
uint32_t guest_address(const GuestView& g, uint32_t location, uint32_t offset, const char* what);
// A surface's memory, by its context object: SET_CONTEXT_DMA_COLOR_A's 0xFEED0000 is the RSX's memory, 0xFEED0001 the mapped main memory.
uint32_t surface_location(uint32_t object);

// One draw of vertices [first, first + count) (of the index array, when `indexed`), with the state the methods have left,
// into `d`; false if there is nothing to draw (indices that are all restarts). What the renderer doesn't take stops the game.
// (`s` keeps the vertex program last made into bytes, for the next draw with the same one.)
bool gather_draw(RsxState& s, const GuestView& g, uint32_t first, uint32_t count, bool indexed, uint32_t at, Draw& d);

}  // namespace rr7

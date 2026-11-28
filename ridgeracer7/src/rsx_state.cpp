#include "rsx_state.hpp"

#include <algorithm>

#include "gcm.hpp"

namespace rr7 {

using qilin::rt::fatal;

uint32_t guest_address(const GuestView& g, uint32_t location, uint32_t offset, const char* what) {
    if (location == 0) return kRsxLocal + offset;
    const uint32_t address = g.io_address(offset);
    if (!address) fatal("the RSX is to find %s at offset %08x of main memory, where nothing is mapped", what, offset);
    return address;
}

uint32_t surface_location(uint32_t object) {
    if (object == 0xFEED0000u) return 0;
    if (object == 0xFEED0001u) return 1;
    fatal("a surface in context object %08x, which this runtime doesn't know", object);
}

bool gather_draw(const RsxState& s, const GuestView& g, uint32_t first, uint32_t count, bool indexed, uint32_t at, Draw& d) {
    const auto state = [&](uint32_t method) { return s[method]; };
    const auto state_float = [&](uint32_t method) { return s.as_float(method); };
    d.reset();
    // The colour surface: only surface A, linear, not antialiased, in this first renderer.
    const uint32_t format = state(0x0208);
    if (((format >> 8) & 0xf) != 1) fatal("a draw to a surface of type %u (not linear), which the renderer doesn't take yet (at offset %08x)", (format >> 8) & 0xf, at);
    if ((format >> 12) & 0xf) fatal("a draw to an antialiased surface, which the renderer doesn't take yet (at offset %08x)", at);
    if (state(0x0220) != 1) fatal("a draw to colour targets %#x (only surface A is taken yet) (at offset %08x)", state(0x0220), at);
    d.color.format = format & 0x1f;
    d.color.pitch = state(0x020c);
    d.color.address = guest_address(g, surface_location(state(0x0194)), state(0x0210), "a colour surface");
    d.color.width = state(0x0200) >> 16, d.color.height = state(0x0204) >> 16;
    if ((state(0x0200) & 0xffff) || (state(0x0204) & 0xffff)) fatal("a surface whose clip doesn't start at 0, 0 (at offset %08x)", at);
    // The window offset (SET_WINDOW_OFFSET: x in the low half, y in the high: *a stated reading*, as the RSX's other pairs).
    d.window_offset_x = state(0x02b8) & 0xffff, d.window_offset_y = state(0x02b8) >> 16;
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
        d.depth.address = guest_address(g, surface_location(state(0x0198)), state(0x0214), "a depth surface");
        d.depth.width = state(0x0200) >> 16, d.depth.height = state(0x0204) >> 16;
    }
    // Polygon offset (SET_POLY_OFFSET_POINT_ENABLE, LINE, FILL, SCALE_FACTOR, BIAS): Vulkan's depth bias, the slope factor and the
    // constant, for polygons drawn in a mode whose enable is on (render_state.hpp). *With D32S8 a unit of bias follows the depth's
    // exponent rather than 2^-24: a stated difference.*
    d.offset_point = state(0x0a60) != 0, d.offset_line = state(0x0a64) != 0, d.offset_fill = state(0x0a68) != 0;
    d.bias_factor = state_float(0x0a78), d.bias_units = state_float(0x0a7c);
    // Polygons' modes (SET_FRONT_POLYGON_MODE, BACK_POLYGON_MODE) and the edge flag (SET_EDGE_FLAG), which a polygon drawn as lines
    // needs on.
    d.front_polygon_mode = state(0x1828), d.back_polygon_mode = state(0x182c), d.edge_flag = state(0x1454) != 0;
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
    // Flat shading (SET_SHADE_MODE: OpenGL's FLAT 0x1D00, SMOOTH 0x1D01, 0 as the system leaves it), with the provoking vertex
    // SET_FLAT_SHADE_OP gives: 0 a primitive's last, OpenGL's convention, 1 its first (*a stated reading*: NV40's FLATSHADE_FIRST,
    // as Mesa's driver names the method).
    if (const uint32_t shade = state(0x0368); shade == 0x1d00) {
        d.flat = true;
        const uint32_t op = state(0x1450);
        if (op > 1) fatal("a draw with flat shading by %#x (SET_FLAT_SHADE_OP), which the renderer doesn't know (at offset %08x)", op, at);
        d.flat_first = op == 1;
    } else if (shade != 0 && shade != 0x1d01) {
        fatal("a draw with shade mode %#x, which the renderer doesn't know (at offset %08x)", shade, at);
    }
    // A logic operation (SET_LOGIC_OP_ENABLE, SET_LOGIC_OP: OpenGL's), which replaces blending.
    if (state(0x0374)) d.logic = true, d.logic_op = state(0x0378);
    // State this renderer doesn't take, refused by name rather than ignored: polygon stipple (Vulkan has none).
    // Two-sided colours (SET_TWO_SIDE_LIGHT_EN): on back faces the vertex program's back colours.
    d.two_sided = state(0x142c) != 0;
    // User clip planes (SET_USER_CLIP_PLANE_CONTROL: a plane's test in each of the low six nibbles, 0 off, 1 LT, 2 GE).
    d.clip_planes = state(0x1478);
    for (unsigned n = 0; n < 8; ++n)
        if (const uint32_t test = (d.clip_planes >> (4 * n)) & 0xf; test > 2 || (n >= 6 && test))
            fatal("a draw with user clip plane %u's test %u, which the renderer doesn't know (at offset %08x)", n, test, at);
    if (state(0x147c)) fatal("a draw with polygon stipple, which the renderer doesn't take yet (at offset %08x)", at);
    // Line stipple (SET_LINE_STIPPLE): refused by the renderer where lines are drawn (Vulkan 1.3 has none either).
    d.line_stipple = state(0x1db4) != 0;
    // The alpha test (SET_ALPHA_FUNC: OpenGL's NEVER 0x200 to ALWAYS 0x207, bit 0 less, bit 1 equal, bit 2 greater; SET_ALPHA_REF):
    // on 8-bit surfaces the reference's low byte against the alpha as stored. *Its reference on a float surface isn't known here.*
    if (state(0x0304)) {
        const uint32_t function = state(0x0308);
        if (function < 0x200 || function > 0x207) fatal("an alpha test with function %#x (at offset %08x)", function, at);
        if (d.color.format != 8) fatal("an alpha test on a surface of format %u, which the renderer doesn't take yet (at offset %08x)", d.color.format, at);
        d.alpha_test = {static_cast<float>(state(0x030c) & 0xff), 255.0f, 0, 0, float(function & 1), float((function >> 1) & 1), float((function >> 2) & 1), 0};
    }
    if (state(0x173c)) fatal("a draw with a base vertex index, which the renderer doesn't take yet (at offset %08x)", at);
    d.primitive = s.primitive, d.first = first, d.count = count;
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
        const uint32_t base = guest_address(g, dma & 1, state(0x181c), "an index array") + first * size;
        uint32_t lowest = UINT32_MAX, highest = 0;
        d.indices.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t v = size == 2 ? (uint32_t{g.memory[base + 2 * i]} << 8 | g.memory[base + 2 * i + 1]) : load_be32(g.memory + base + 4 * i);
            if (restart && v == restart_index) {
                d.indices[i] = kRestart;
                continue;
            }
            d.indices[i] = v, lowest = std::min(lowest, v), highest = std::max(highest, v);
        }
        if (lowest > highest) return false;  // (nothing but restarts)
        for (uint32_t& v : d.indices)
            if (v != kRestart) v -= lowest;
        d.first = lowest, d.count = highest - lowest + 1;
        d.indexed = true;
    }
    for (uint32_t a = 0; a < 16; ++a) {
        d.attribute_format[a] = state(0x1740 + 4 * a);
        if (((d.attribute_format[a] >> 4) & 0xf) == 0) continue;
        const uint32_t offset = state(0x1680 + 4 * a);
        d.attribute_address[a] = guest_address(g, offset >> 31, (offset & 0x7fffffffu) + state(0x1738), "a vertex array");
    }
    d.vertex_constants = s.constants.data(), d.constants_version = s.constants_version;
    // The vertex program: from its start to the instruction marked last, as big-endian bytes.
    {
        const uint32_t start = state(0x1ea0);
        if (start >= 512) fatal("a vertex program started at slot %u, past the 512", start);
        uint32_t last = start;
        while (last < 512 && !(s.program[4 * last + 3] & 1)) ++last;
        if (last == 512) fatal("the vertex program from slot %u has no last instruction", start);
        d.vertex_program.resize(size_t{last - start + 1} * 16);
        for (size_t w = 0; w < d.vertex_program.size() / 4; ++w) {
            const uint32_t big = std::byteswap(s.program[4 * start + w]);
            std::memcpy(d.vertex_program.data() + 4 * w, &big, 4);
        }
    }
    // The fragment program: from its address to the instruction marked as the end, with its inline constants.
    {
        const uint32_t program = state(0x08e4);
        const uint32_t location = program & 3;
        if (location != 1 && location != 2) fatal("a fragment program in location %u (at offset %08x)", location, at);
        const uint32_t address = guest_address(g, location == 1 ? 0 : 1, program & ~3u, "a fragment program");
        for (uint32_t offset = 0;;) {
            const uint8_t* p = g.memory + address + offset;
            const auto word = [&](unsigned i) {
                const uint32_t w = load_be32(p + 4 * i);
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
        if (t.enabled() && (location == 1 || location == 2)) t.address = guest_address(g, location == 1 ? 0 : 1, t.offset, "a texture");
    }
    d.cull_face = state(0x1830), d.front_face = state(0x1834);
    return true;
}

}  // namespace rr7

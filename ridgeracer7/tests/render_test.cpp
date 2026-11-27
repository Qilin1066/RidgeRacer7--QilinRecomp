// The renderer's pure parts, held to the formats' definitions: each texture format's channel order and byte order, the
// remap's slots and selectors, the swizzled layout, a colour surface's pixels as the image holds them (which must agree
// with a texture of the same bytes: the renderer copies one into the other on the GPU), vertex attributes and depth words.
// No GPU and no game files.
//
// The definitions: PSL1GHT's public headers for the texture format word and the remap (CELL_GCM_TEXTURE_REMAP: output A,
// R, G, B in bits 0-1, 2-3, 4-5, 6-7 each taking input A 0, R 1, G 2 or B 3; their kinds in bits 8-15, 0 zero, 1 one, 2 the
// input); the formats' channels as their names give them, 16-bit texels big-endian; DXT as on a PC.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "render_formats.hpp"
#include "render_state.hpp"
#include "textures.hpp"

namespace {

int failures = 0;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

using rr7::Texels;
using rr7::TextureUnit;
using Rgba = std::array<uint8_t, 4>;

constexpr uint32_t kIdentity = 0xaae4;  // every output its own input (A from A, R from R...), each of kind "the input"
constexpr uint32_t kLinear = 0x20;      // the format byte's LN bit
// The format bytes (bits 8 to 15 of the format word), as CELL_GCM_TEXTURE_... gives them.
constexpr uint32_t kB8 = 0x81, kA1R5G5B5 = 0x82, kA4R4G4B4 = 0x83, kR5G6B5 = 0x84, kA8R8G8B8 = 0x85, kDxt1 = 0x86, kDxt23 = 0x87, kDxt45 = 0x88;

// A 2D texture in the RSX's memory at guest address 0, of one level.
TextureUnit unit(uint32_t format_byte, uint32_t width, uint32_t height, uint32_t pitch, uint32_t remap) {
    TextureUnit t;
    t.format = 1u | (2u << 4) | (format_byte << 8) | (1u << 16);  // location 1, two dimensions, one level
    t.rect = (width << 16) | height;
    t.control3 = (1u << 20) | pitch;  // depth 1
    t.remap = remap;
    t.control0 = 1u << 31;
    return t;
}

Texels decode(const TextureUnit& t, const std::vector<uint8_t>& memory) {
    Texels out;
    const std::string why = rr7::decode_texture(t, memory.data(), out);
    if (!why.empty()) std::fprintf(stderr, "decode_texture: %s\n", why.c_str());
    CHECK(why.empty());
    return out;
}
Rgba texel(const Texels& t, uint32_t x, uint32_t y) {
    const size_t at = (size_t{y} * t.width + x) * 4;
    if (at + 4 > t.rgba.size()) return {};
    return {t.rgba[at], t.rgba[at + 1], t.rgba[at + 2], t.rgba[at + 3]};
}

// A remap of every output from its own input but `output`, which takes `input` (A 0, R 1, G 2, B 3 for both).
uint32_t remap_one(unsigned output, unsigned input) { return (kIdentity & ~(3u << (2 * output))) | (input << (2 * output)); }

void test_a8r8g8b8() {
    // A, R, G, B in memory; R, G, B, A in the image.
    const std::vector<uint8_t> memory{0x11, 0x22, 0x33, 0x44, 0xa0, 0xb0, 0xc0, 0xd0};
    const Texels t = decode(unit(kA8R8G8B8 | kLinear, 2, 1, 8, kIdentity), memory);
    CHECK(t.width == 2 && t.height == 1 && t.levels == 1);
    CHECK(texel(t, 0, 0) == (Rgba{0x22, 0x33, 0x44, 0x11}));
    CHECK(texel(t, 1, 0) == (Rgba{0xb0, 0xc0, 0xd0, 0xa0}));
    // Each output slot from each input: the image's channel (R 0, G 1, B 2, A 3) for output A 0, R 1, G 2, B 3 is the
    // memory byte (A 0, R 1, G 2, B 3) of its input.
    static constexpr unsigned kImageChannel[4] = {3, 0, 1, 2};
    for (unsigned output = 0; output < 4; ++output)
        for (unsigned input = 0; input < 4; ++input) {
            const Texels r = decode(unit(kA8R8G8B8 | kLinear, 2, 1, 8, remap_one(output, input)), memory);
            CHECK(texel(r, 0, 0)[kImageChannel[output]] == memory[input]);
            for (unsigned other = 0; other < 4; ++other)
                if (other != output) CHECK(texel(r, 0, 0)[kImageChannel[other]] == memory[other]);
        }
    // Red and blue exchanged by the remap: output R from input B, output B from input R.
    CHECK(texel(decode(unit(kA8R8G8B8 | kLinear, 2, 1, 8, 0xaa6c), memory), 0, 0) == (Rgba{0x44, 0x33, 0x22, 0x11}));
    // The kinds: output A one, output R zero, G and B their inputs.
    CHECK(texel(decode(unit(kA8R8G8B8 | kLinear, 2, 1, 8, 0xa1e4), memory), 0, 0) == (Rgba{0x00, 0x33, 0x44, 0xff}));
    // A kind of 3 is no kind: refused.
    Texels refused;
    CHECK(!rr7::decode_texture(unit(kA8R8G8B8 | kLinear, 2, 1, 8, 0xeae4), memory.data(), refused).empty());
}

void test_sixteen_bits() {
    // Big-endian halves: R5G6B5's red, green and blue at their tops; alpha one.
    {
        const std::vector<uint8_t> memory{0xf8, 0x00, 0x07, 0xe0, 0x00, 0x1f, 0x84, 0x10};
        const Texels t = decode(unit(kR5G6B5 | kLinear, 4, 1, 8, kIdentity), memory);
        CHECK(texel(t, 0, 0) == (Rgba{0xff, 0x00, 0x00, 0xff}));
        CHECK(texel(t, 1, 0) == (Rgba{0x00, 0xff, 0x00, 0xff}));
        CHECK(texel(t, 2, 0) == (Rgba{0x00, 0x00, 0xff, 0xff}));
        CHECK(texel(t, 3, 0) == (Rgba{0x84, 0x82, 0x84, 0xff}));  // 10000 100000 10000: the high bits repeated
    }
    // A1R5G5B5: alpha the top bit.
    {
        const std::vector<uint8_t> memory{0xfc, 0x00, 0x03, 0xe0, 0x80, 0x1f, 0x00, 0x00};
        const Texels t = decode(unit(kA1R5G5B5 | kLinear, 4, 1, 8, kIdentity), memory);
        CHECK(texel(t, 0, 0) == (Rgba{0xff, 0x00, 0x00, 0xff}));
        CHECK(texel(t, 1, 0) == (Rgba{0x00, 0xff, 0x00, 0x00}));
        CHECK(texel(t, 2, 0) == (Rgba{0x00, 0x00, 0xff, 0xff}));
        CHECK(texel(t, 3, 0) == (Rgba{0x00, 0x00, 0x00, 0x00}));
    }
    // A4R4G4B4: a nibble each, alpha the top.
    {
        const std::vector<uint8_t> memory{0x12, 0x34};
        CHECK(texel(decode(unit(kA4R4G4B4 | kLinear, 1, 1, 2, kIdentity), memory), 0, 0) == (Rgba{0x22, 0x33, 0x44, 0x11}));
        // And through the remap, as A8R8G8B8 is: output R from input B.
        CHECK(texel(decode(unit(kA4R4G4B4 | kLinear, 1, 1, 2, remap_one(1, 3)), memory), 0, 0) == (Rgba{0x44, 0x33, 0x44, 0x11}));
    }
}

void test_b8() {
    // B8's byte is input B: every output from B gives it in all four; output A of kind one gives 255 there.
    const std::vector<uint8_t> memory{0x5a, 0xc3};
    const Texels all = decode(unit(kB8 | kLinear, 2, 1, 2, 0xaaff), memory);
    CHECK(texel(all, 0, 0) == (Rgba{0x5a, 0x5a, 0x5a, 0x5a}));
    CHECK(texel(all, 1, 0) == (Rgba{0xc3, 0xc3, 0xc3, 0xc3}));
    CHECK(texel(decode(unit(kB8 | kLinear, 2, 1, 2, 0xa9ff), memory), 1, 0) == (Rgba{0xc3, 0xc3, 0xc3, 0xff}));
    // A remap that reads another input of it is refused (Mesa's driver reads only B: what the others hold isn't known here).
    Texels refused;
    CHECK(!rr7::decode_texture(unit(kB8 | kLinear, 2, 1, 2, kIdentity), memory.data(), refused).empty());
}

void test_dxt() {
    // DXT1: endpoints little-endian R5G6B5 (red 0xf800, blue 0x001f), then the sixteen 2-bit indices, texel 0 lowest.
    {
        const std::vector<uint8_t> memory{0x00, 0xf8, 0x1f, 0x00, 0xe4, 0x00, 0x00, 0x00};
        const Texels t = decode(unit(kDxt1, 4, 4, 0, kIdentity), memory);
        CHECK(texel(t, 0, 0) == (Rgba{0xff, 0x00, 0x00, 0xff}));
        CHECK(texel(t, 1, 0) == (Rgba{0x00, 0x00, 0xff, 0xff}));
        CHECK(texel(t, 2, 0) == (Rgba{0xaa, 0x00, 0x55, 0xff}));  // two thirds of the first
        CHECK(texel(t, 3, 0) == (Rgba{0x55, 0x00, 0xaa, 0xff}));
        CHECK(texel(t, 0, 1) == (Rgba{0xff, 0x00, 0x00, 0xff}));
    }
    // With the first endpoint not above the second: three colours and transparent black.
    {
        const std::vector<uint8_t> memory{0x1f, 0x00, 0x00, 0xf8, 0xe4, 0x00, 0x00, 0x00};
        const Texels t = decode(unit(kDxt1, 4, 4, 0, kIdentity), memory);
        CHECK(texel(t, 2, 0) == (Rgba{0x7f, 0x00, 0x7f, 0xff}));
        CHECK(texel(t, 3, 0) == (Rgba{0x00, 0x00, 0x00, 0x00}));
    }
    // DXT3: sixteen 4-bit alphas first, texel 0 the low nibble; then a colour block (white).
    {
        const std::vector<uint8_t> memory{0x2f, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        const Texels t = decode(unit(kDxt23, 4, 4, 0, kIdentity), memory);
        CHECK(texel(t, 0, 0) == (Rgba{0xff, 0xff, 0xff, 0xff}));
        CHECK(texel(t, 1, 0) == (Rgba{0xff, 0xff, 0xff, 0x22}));
        CHECK(texel(t, 2, 0) == (Rgba{0xff, 0xff, 0xff, 0x00}));
    }
    // DXT5: two alpha endpoints, then 3-bit indices little-endian (texel 0 code 0, texel 1 code 1, texel 2 code 2).
    {
        const std::vector<uint8_t> memory{0xff, 0x00, 0x88, 0, 0, 0, 0, 0, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        const Texels t = decode(unit(kDxt45, 4, 4, 0, kIdentity), memory);
        CHECK(texel(t, 0, 0)[3] == 0xff && texel(t, 1, 0)[3] == 0x00 && texel(t, 2, 0)[3] == (6 * 255) / 7);
        CHECK(texel(t, 2, 0)[0] == 0xff);
    }
}

void test_swizzled() {
    // Swizzled (no LN bit): the bits of x and y interleaved, x's first, the longer side's last on their own. Texel k in
    // memory has red k.
    std::vector<uint8_t> memory(4 * 8);
    for (uint8_t k = 0; k < 8; ++k) memory[4 * k + 1] = k;
    const Texels t = decode(unit(kA8R8G8B8, 4, 2, 0, kIdentity), memory);
    CHECK(texel(t, 1, 0)[0] == 1 && texel(t, 0, 1)[0] == 2 && texel(t, 1, 1)[0] == 3 && texel(t, 2, 0)[0] == 4 && texel(t, 3, 1)[0] == 7);
}

void test_surfaces_as_textures() {
    // A colour surface's pixels as its image holds them are a linear A8R8G8B8 texture's of the same bytes with the remap as
    // stored: the renderer copies one to the other on the GPU (texture_is_plain_argb), so the two orders must agree.
    std::vector<uint8_t> memory(4 * 16);
    for (size_t i = 0; i < memory.size(); ++i) memory[i] = static_cast<uint8_t>(i * 37 + 11);
    const Texels t = decode(unit(kA8R8G8B8 | kLinear, 16, 1, 64, kIdentity), memory);
    std::vector<uint8_t> image(memory.size()), back(memory.size());
    rr7::pixels_to_host(8, memory.data(), image.data(), 16);
    CHECK(image == t.rgba);
    rr7::pixels_to_guest(8, image.data(), back.data(), 16);
    CHECK(back == memory);
    // F_X32: a big-endian float, and back.
    const std::vector<uint8_t> one{0x3f, 0x80, 0x00, 0x00};
    std::vector<uint8_t> f(4), f_back(4);
    rr7::pixels_to_host(13, one.data(), f.data(), 1);
    float v = 0;
    std::memcpy(&v, f.data(), 4);
    CHECK(v == 1.0f);
    rr7::pixels_to_guest(13, f.data(), f_back.data(), 1);
    CHECK(f_back == one);
    // Which textures take that path: only the remap as stored, linear, one level.
    CHECK(rr7::texture_is_plain_argb(unit(kA8R8G8B8 | kLinear, 16, 1, 64, kIdentity)));
    CHECK(!rr7::texture_is_plain_argb(unit(kA8R8G8B8 | kLinear, 16, 1, 64, 0xaa6c)));
    CHECK(!rr7::texture_is_plain_argb(unit(kA8R8G8B8 | kLinear, 16, 1, 64, 0xa1e4)));
    CHECK(!rr7::texture_is_plain_argb(unit(kA8R8G8B8, 16, 1, 64, kIdentity)));
    CHECK(!rr7::texture_is_plain_argb(unit(kR5G6B5 | kLinear, 16, 1, 64, kIdentity)));
}

void test_format_word() {
    // The format byte's LN (0x20) is the linear layout and UN (0x40) unnormalised coordinates (refused); bits 8-12 the format.
    size_t bytes = 0;
    CHECK(rr7::texture_extent(unit(kA8R8G8B8 | kLinear, 4, 4, 16, kIdentity), bytes).empty() && bytes == 64);
    CHECK(rr7::texture_extent(unit(kA8R8G8B8, 4, 4, 0, kIdentity), bytes).empty() && bytes == 64);
    CHECK(!rr7::texture_extent(unit(kA8R8G8B8 | kLinear | 0x40, 4, 4, 16, kIdentity), bytes).empty());
    CHECK(!rr7::texture_extent(unit(0x8b /* G8B8 */, 4, 4, 0, kIdentity), bytes).empty());
    CHECK(rr7::texture_extent(unit(kDxt1, 8, 8, 0, kIdentity), bytes).empty() && bytes == 32);
}

void test_attributes() {
    // UB: bytes in memory order as x, y, z, w (a colour's R, G, B, A as stored), as fractions of 255.
    const uint8_t ub[] = {255, 0, 51, 102};
    float out[8] = {};
    CHECK(rr7::decode_attributes(4 | (4 << 4), ub, 4, 1, out));
    CHECK(out[0] == 1.0f && out[1] == 0.0f && out[2] == 51.0f / 255.0f && out[3] == 102.0f / 255.0f);
    // F, big-endian; two components, the rest 0 and 1.
    const uint8_t fl[] = {0x3f, 0x80, 0, 0, 0xc0, 0, 0, 0};
    CHECK(rr7::decode_attributes(2 | (2 << 4), fl, 8, 1, out));
    CHECK(out[0] == 1.0f && out[1] == -2.0f && out[2] == 0.0f && out[3] == 1.0f);
    // S1: signed 16-bit fractions of 32767, -32768 clamped to -1.
    const uint8_t s1[] = {0x7f, 0xff, 0x80, 0x00};
    CHECK(rr7::decode_attributes(1 | (2 << 4), s1, 4, 1, out));
    CHECK(out[0] == 1.0f && out[1] == -1.0f);
    // CMP (6) is not taken.
    CHECK(!rr7::decode_attributes(6 | (1 << 4), ub, 4, 1, out));
    // Halves.
    const uint8_t h[][2] = {{0x3c, 0x00}, {0xc0, 0x00}, {0x00, 0x01}, {0x7c, 0x00}, {0x7b, 0xff}};
    CHECK(rr7::half_float(h[0]) == 1.0f && rr7::half_float(h[1]) == -2.0f && rr7::half_float(h[2]) == std::ldexp(1.0f, -24));
    CHECK(std::isinf(rr7::half_float(h[3])) && rr7::half_float(h[4]) == 65504.0f);
}

void test_depth() {
    // Every 24-bit depth comes back as it went, as a float or as itself.
    bool all = true;
    for (uint32_t d = 0; d < (1u << 24); d += 97) all = all && rr7::depth_to_guest(rr7::depth_to_host(d, false), false) == d;
    CHECK(all);
    CHECK(rr7::depth_to_guest(rr7::depth_to_host(0xffffff, false), false) == 0xffffff);
    CHECK(rr7::depth_to_host(0x123456, true) == 0x123456 && rr7::depth_to_guest(0xab123456u, true) == 0x123456);
}


using Indices = std::vector<uint32_t>;
Indices iota(uint32_t n) {
    Indices v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = i;
    return v;
}

void test_assembly() {
    using rr7::assemble, rr7::Provoking, rr7::Topology, rr7::kRestart;
    Indices made;
    // What Vulkan has is drawn as it is: triangles, strips, lines, points.
    {
        const Indices order = iota(6);
        const auto a = assemble(5, order, false, Provoking::any, made);
        CHECK(a && a->topology == Topology::triangles && !a->indexed && !a->made);
        const auto strip = assemble(6, order, true, Provoking::any, made);
        CHECK(strip && strip->topology == Topology::triangle_strip && strip->indexed && !strip->made);
    }
    // Quads, quad strips, fans and polygons as triangles: (a, b, c), (a, c, d); (v, v + 1, v + 3), (v, v + 3, v + 2); about the first.
    {
        const auto q = assemble(8, iota(8), false, Provoking::any, made);
        CHECK(q && q->topology == Topology::triangles && q->indexed && q->made && made == (Indices{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7}));
        CHECK(assemble(9, iota(6), false, Provoking::any, made) && made == (Indices{0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4}));
        CHECK(assemble(7, iota(5), false, Provoking::any, made) && made == (Indices{0, 1, 2, 0, 2, 3, 0, 3, 4}));
        CHECK(assemble(10, iota(5), false, Provoking::any, made) && made == (Indices{0, 1, 2, 0, 2, 3, 0, 3, 4}));
    }
    // Primitive restart: each piece its own primitive, a strip's triangles keeping their alternate windings.
    {
        const Indices order{0, 1, 2, 3, kRestart, 4, 5, 6};
        const auto a = assemble(6, order, true, Provoking::any, made);
        CHECK(a && a->topology == Topology::triangles && a->made && made == (Indices{0, 1, 2, 2, 1, 3, 4, 5, 6}));
        // Pieces too short for a triangle make nothing (and never hand the GPU a restart index).
        const auto none = assemble(5, Indices{0, 1, kRestart, 2}, true, Provoking::any, made);
        CHECK(none && none->made && made.empty());
        // Lines and points with restarts: lists.
        const auto lines = assemble(4, Indices{0, 1, 2, kRestart, 3, 4}, true, Provoking::any, made);
        CHECK(lines && lines->topology == Topology::lines && made == (Indices{0, 1, 1, 2, 3, 4}));
        const auto points = assemble(1, Indices{0, kRestart, 1}, true, Provoking::any, made);
        CHECK(points && points->topology == Topology::points && made == (Indices{0, 1}));
    }
    // Line loops (3) and numbers past polygons aren't taken.
    CHECK(!assemble(3, iota(4), false, Provoking::any, made));
    CHECK(!assemble(11, iota(4), false, Provoking::any, made));
}

void test_rasterisation() {
    using rr7::PolygonMode, rr7::PolygonState, rr7::rasterisation;
    PolygonState s;
    s.offset_fill = true;
    // Filled (0 as the system leaves it, or FILL): polygon offset is the fill enable's.
    auto r = rasterisation(5, s);
    CHECK(r && r->mode == PolygonMode::fill && r->depth_bias);
    // Both faces as lines: the line enable's.
    s.front_mode = s.back_mode = 0x1b01;
    r = rasterisation(6, s);
    CHECK(r && r->mode == PolygonMode::line && !r->depth_bias);
    s.offset_line = true;
    CHECK(rasterisation(6, s) && rasterisation(6, s)->depth_bias);
    // Points and lines aren't polygons.
    r = rasterisation(2, s);
    CHECK(r && r->mode == PolygonMode::fill && !r->depth_bias);
    // Faces in different modes: refused while both are drawn, the drawn one's when the other is culled.
    s.front_mode = 0x1b00, s.back_mode = 0x1b02;
    CHECK(!rasterisation(5, s));
    s.cull = true, s.cull_face = 0x405;
    r = rasterisation(5, s);
    CHECK(r && r->mode == PolygonMode::point);
    s.cull_face = 0x404;
    r = rasterisation(5, s);
    CHECK(r && r->mode == PolygonMode::fill && r->depth_bias);
    // Quads and polygons as lines or points (they would be drawn as triangles), and lines with the edge flag off.
    s.cull = false, s.front_mode = s.back_mode = 0x1b01;
    CHECK(!rasterisation(8, s) && !rasterisation(10, s) && rasterisation(7, s));
    s.edge_flag = false;
    CHECK(!rasterisation(5, s));
    // A mode that isn't one.
    s.front_mode = s.back_mode = 0x1b03;
    CHECK(!rasterisation(5, s));
}

void test_window() {
    rr7::Draw d;
    d.color.width = 100, d.color.height = 50;
    d.viewport_scale = {50, 25, 0.5f, 0}, d.viewport_offset = {50, 25, 0.5f, 0};
    d.scissor_width = 100, d.scissor_height = 50, d.window_height = 50;
    // A viewport covering the surface: clip x = x / 50 - 1 + ..., as the vertex programs take it.
    rr7::WindowMapping w = rr7::window_mapping(d);
    CHECK(w.push[0] == 1.0f && w.push[1] == 1.0f && w.push[2] == 0.0f && w.push[3] == 0.0f && w.push[4] == 0.5f && w.push[5] == 0.5f);
    CHECK(w.x0 == 0 && w.y0 == 0 && w.x1 == 100 && w.y1 == 50);
    // The window offset moves the viewport and the scissor alike, the scissor clipped to the surface.
    d.window_offset_x = 10, d.window_offset_y = 4;
    w = rr7::window_mapping(d);
    CHECK(std::abs(w.push[2] - 0.2f) < 1e-6f && std::abs(w.push[3] - 0.16f) < 1e-6f);
    CHECK(w.x0 == 10 && w.y0 == 4 && w.x1 == 100 && w.y1 == 50);
    // Window y up from the bottom: the scissor's rows counted from the window's top, then moved.
    d.origin_bottom = true, d.scissor_height = 20;
    w = rr7::window_mapping(d);
    CHECK(w.y0 == 34 && w.y1 == 50 && w.push[1] == -1.0f);
    // A scissor wholly off the surface is empty.
    d.origin_bottom = false, d.window_offset_x = 0, d.scissor_x = 200, d.scissor_width = 10;
    w = rr7::window_mapping(d);
    CHECK(w.x0 == 200 && w.x1 == 200);
}

}  // namespace

int main() {
    test_a8r8g8b8();
    test_sixteen_bits();
    test_b8();
    test_dxt();
    test_swizzled();
    test_surfaces_as_textures();
    test_format_word();
    test_attributes();
    test_depth();
    test_assembly();
    test_rasterisation();
    test_window();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("render tests passed");
    return 0;
}

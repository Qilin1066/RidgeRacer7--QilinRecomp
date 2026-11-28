// What the renderer converts between guest memory and the GPU: colour surfaces' pixels, depth words, vertex attributes.
// Pure functions, so that their channel and byte orders are pinned by tests without a GPU (ridgeracer7/tests/render_test.cpp).
//
// Runtime code: no std::format.
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace rr7 {

// A colour surface's pixels, guest memory to the image's bytes and back. A8R8G8B8 (format 8) is A, R, G, B in memory and
// R, G, B, A in the image, as a texture of the same bytes decodes (textures.hpp: the renderer copies a surface's image to a
// texture's on the GPU, so the two must agree); F_X32 (13) is a big-endian float. (As words, on a little-endian host as
// load32 and store32 assume: A8R8G8B8 is a rotation by a byte, a float a byte swap.)
// (One loop for each, so that the compiler can make each a run of vector shuffles.)
template <class Word>
void map_words(const uint8_t* from, uint8_t* to, size_t pixels, Word word) {
    for (size_t i = 0; i < pixels; ++i) {
        uint32_t w;
        std::memcpy(&w, from + 4 * i, 4);
        w = word(w);
        std::memcpy(to + 4 * i, &w, 4);
    }
}
inline void pixels_to_host(uint32_t format, const uint8_t* from, uint8_t* to, size_t pixels) {
    if (format == 8) map_words(from, to, pixels, [](uint32_t w) { return std::rotr(w, 8); });
    else map_words(from, to, pixels, [](uint32_t w) { return std::byteswap(w); });
}
inline void pixels_to_guest(uint32_t format, const uint8_t* from, uint8_t* to, size_t pixels) {
    if (format == 8) map_words(from, to, pixels, [](uint32_t w) { return std::rotl(w, 8); });
    else map_words(from, to, pixels, [](uint32_t w) { return std::byteswap(w); });
}

// A Z24S8 surface's depth (a big-endian word's top 24 bits) as the depth aspect's word, and back. With D24_UNORM_S8_UINT
// (`unorm`) the word holds the 24 bits as they are; with D32_SFLOAT_S8_UINT a float, depth / (2^24 - 1) (*then the test
// compares floats, not 24-bit numbers: a stated difference. Each 24-bit depth is a float of its own, so order and the value
// written back are kept; a fragment exactly at a stored depth may test otherwise than on the RSX*).
inline uint32_t depth_to_host(uint32_t depth24, bool unorm) {
    // (Through a signed conversion, exact below 2^24, which unlike an unsigned one is a vector instruction.)
    return unorm ? depth24 : std::bit_cast<uint32_t>(static_cast<float>(static_cast<int32_t>(depth24 & 0xffffffu)) / 16777215.0f);
}
inline uint32_t depth_to_guest(uint32_t host, bool unorm) {
    if (unorm) return host & 0xffffffu;
    // (Rounded half away from zero, as lround, on a non-negative float below 2^24, in floats: the fraction left after the
    // whole part is taken away is exact. The same as adding a half in a double, for every float but NaN, which is taken
    // as 0: checked exhaustively. Written with no call and no unsigned conversion, so that a row of them is vector code.)
    const float f = std::bit_cast<float>(host);
    const float v = (f > 0.0f ? (f < 1.0f ? f : 1.0f) : 0.0f) * 16777215.0f;
    const int32_t whole = static_cast<int32_t>(v);
    return static_cast<uint32_t>(whole + (v - static_cast<float>(whole) >= 0.5f ? 1 : 0));
}

// A row of a Z24S8 surface: the image's depth words and stencil bytes (its two aspects, as copied out) to guest memory's
// big-endian words, and back.
inline void depth_row_to_guest(const uint32_t* depth, const uint8_t* stencil, uint8_t* to, size_t pixels, bool unorm) {
    const auto row = [&](auto as_unorm) {  // (one loop for each depth format, with nothing to decide inside)
        for (size_t i = 0; i < pixels; ++i) {
            const uint32_t w = std::byteswap((depth_to_guest(depth[i], as_unorm) << 8) | stencil[i]);
            std::memcpy(to + 4 * i, &w, 4);
        }
    };
    if (unorm) row(std::true_type{});
    else row(std::false_type{});
}
inline void depth_row_to_host(const uint8_t* from, uint32_t* depth, uint8_t* stencil, size_t pixels, bool unorm) {
    const auto row = [&](auto as_unorm) {
        for (size_t i = 0; i < pixels; ++i) {
            uint32_t w;
            std::memcpy(&w, from + 4 * i, 4);
            w = std::byteswap(w);
            depth[i] = depth_to_host(w >> 8, as_unorm);
            stencil[i] = static_cast<uint8_t>(w);
        }
    };
    if (unorm) row(std::true_type{});
    else row(std::false_type{});
}

// A half (big-endian bytes) as a float. A normal one is built from its bits (exactly 1.m times 2 to the e - 15).
float half_float(const uint8_t* p);

// A vertex attribute's `count` vertices from `p`, `stride` bytes apart, as four floats each (missing components 0, 0, 0, 1),
// by its format word (SET_VERTEX_DATA_ARRAY_FORMAT: the type in bits 0 to 3, CELL_GCM_VERTEX_...; the size in 4 to 7), its
// components big-endian. False for a type not taken (nothing is written then).
bool decode_attributes(uint32_t format, const uint8_t* p, size_t stride, uint32_t count, float* out);

// How a vertex attribute is given to the GPU, as a byte (the renderer's pipeline keys hold one for each attribute): its own
// format, the RSX's type | size << 4 (type 1 to 7 but 6, size 1 to 4), each component in the GPU's format of the same kind
// (renderer_pipelines.cpp: vertex_input); or kVertexFloats, four floats made by decode_attributes (a size past four, or a
// format the GPU can't read); or kVertexConstant, no array: one value, 0, 0, 0, 1, for every vertex. 0: not read.
inline constexpr uint8_t kVertexFloats = 0xff, kVertexConstant = 0xfe;
inline constexpr bool vertex_format_native(uint8_t f) {
    const uint32_t type = f & 0xf, size = f >> 4;
    return f != kVertexFloats && f != kVertexConstant && type >= 1 && type <= 7 && type != 6 && size >= 1 && size <= 4;
}
// The bytes a vertex takes in an attribute's own format (its components' bytes, three made four: the GPU's formats of
// three 8- and 16-bit components are not required to exist), a whole number of words.
inline constexpr uint32_t native_stride(uint8_t f) {
    const uint32_t type = f & 0xf, size = f >> 4;
    const uint32_t bytes = (type == 2 ? 4 : type == 4 || type == 7 ? 1 : 2) * (type != 2 && size == 3 ? 4 : size);
    return (bytes + 3) / 4 * 4;
}
// An attribute's `count` vertices from `p`, `stride` bytes apart, in its own format (vertex_format_native): each component
// in the host's byte order, a fourth component of 1 where three are given, native_stride(f) bytes a vertex.
void encode_attributes(uint8_t f, const uint8_t* p, size_t stride, uint32_t count, uint8_t* out);

}  // namespace rr7

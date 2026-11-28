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
    return unorm ? depth24 : std::bit_cast<uint32_t>(static_cast<float>(depth24) / 16777215.0f);
}
inline uint32_t depth_to_guest(uint32_t host, bool unorm) {
    if (unorm) return host & 0xffffffu;
    // (Rounded half away from zero, as lround, on a non-negative float below 2^24, in floats: the fraction left after the
    // whole part is taken away is exact. The same as adding a half in a double, for every float: checked exhaustively.)
    const float v = std::clamp(std::bit_cast<float>(host), 0.0f, 1.0f) * 16777215.0f;
    const uint32_t whole = static_cast<uint32_t>(v);
    return whole + (v - static_cast<float>(whole) >= 0.5f ? 1u : 0u);
}

// A half (big-endian bytes) as a float. A normal one is built from its bits (exactly 1.m times 2 to the e - 15).
float half_float(const uint8_t* p);

// A vertex attribute's `count` vertices from `p`, `stride` bytes apart, as four floats each (missing components 0, 0, 0, 1),
// by its format word (SET_VERTEX_DATA_ARRAY_FORMAT: the type in bits 0 to 3, CELL_GCM_VERTEX_...; the size in 4 to 7), its
// components big-endian. False for a type not taken (nothing is written then).
bool decode_attributes(uint32_t format, const uint8_t* p, size_t stride, uint32_t count, float* out);

}  // namespace rr7

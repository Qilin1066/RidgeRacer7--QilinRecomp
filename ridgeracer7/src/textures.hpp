// The RSX's textures, read from guest memory: their fields and their texels.
//
// A texture unit is nine words of state (0x1A00 + 32 * unit for eight of
// them, 0x1840 + 4 * unit for the ninth). Their fields are as PSL1GHT's
// public headers give them (`docs/runtime.md`); what isn't taken yet stops
// the game by name.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace rr7 {

struct TextureUnit {
    uint32_t address = 0;  // the texels' address in guest memory (from the offset and the format word's location)
    uint32_t offset = 0, format = 0, wrap = 0, control0 = 0, remap = 0, filter = 0, rect = 0, border = 0, control3 = 0;
    bool enabled() const { return control0 >> 31; }
};

// A texture's texels as RGBA8, every level one after the other, and what each level is.
struct Texels {
    uint32_t width = 0, height = 0, levels = 0;
    uint32_t faces = 1;  // 6 for a cube map: level_offset is then by face, then level
    std::vector<uint8_t> rgba;
    std::vector<size_t> level_offset;
    size_t guest_bytes = 0;  // how much of guest memory the texture covers
};

// Checks a texture's fields and finds how many bytes of guest memory it covers, without reading it; or what isn't taken.
std::expected<size_t, std::string> texture_extent(const TextureUnit& t);

// Whether a texture's texels are a colour surface's pixels as the renderer's image holds them: 2D, linear A8R8G8B8, one
// level, its channels as stored (the renderer then copies the surface's image on the GPU instead of reading guest memory).
bool texture_is_plain_argb(const TextureUnit& t);

// Reads a texture's texels from `memory` (guest memory); or what isn't taken, and the texture is not read.
std::expected<Texels, std::string> decode_texture(const TextureUnit& t, const uint8_t* memory);

// The sampler's fields, decoded: filters (0 nearest, 1 linear), the mip mode (0 none, 1 nearest, 2 linear), the three wrap
// modes (the RSX's numbers), the LOD bias and range, the anisotropy (1 to 16) and the border colour (A, R, G, B in bytes).
struct SamplerFields {
    uint32_t mag = 0, min = 0, mip = 0;
    std::array<uint32_t, 3> wrap{};
    float bias = 0, min_lod = 0, max_lod = 0;
    uint32_t anisotropy = 1;
    uint32_t border = 0;
};
std::expected<SamplerFields, std::string> decode_sampler(const TextureUnit& t);

}  // namespace rr7

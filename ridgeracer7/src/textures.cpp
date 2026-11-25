#include "textures.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rr7 {

namespace {

std::string what(const char* format, uint32_t value) {
    char text[160];
    std::snprintf(text, sizeof text, format, value);
    return text;
}

// The formats taken (the format word's bits 8 to 12), with their bytes per texel, or per 4 by 4 block when compressed.
// B8 (1) is NV40's L8: its byte is in source channel B, the only one Mesa's nouveau driver reads (its nv30_format.c), so a
// remap that takes another channel of it is refused.
enum : uint32_t { kB8 = 1, kA1R5G5B5 = 2, kA4R4G4B4 = 3, kR5G6B5 = 4, kA8R8G8B8 = 5, kDxt1 = 6, kDxt23 = 7, kDxt45 = 8 };
bool compressed(uint32_t f) { return f == kDxt1 || f == kDxt23 || f == kDxt45; }
uint32_t unit_bytes(uint32_t f) {
    switch (f) {
    case kB8: return 1;
    case kA1R5G5B5: case kA4R4G4B4: case kR5G6B5: return 2;
    case kA8R8G8B8: return 4;
    case kDxt1: return 8;
    case kDxt23: case kDxt45: return 16;
    default: return 0;
    }
}

uint8_t expand(uint32_t v, unsigned bits) {  // n bits to 8 by repeating the high bits (*a stated choice*)
    v &= (1u << bits) - 1;
    uint32_t out = v << (8 - bits);
    for (unsigned have = bits; have < 8; have += bits) out |= v << (8 - bits) >> have;
    return static_cast<uint8_t>(out);
}

// One texel of an uncompressed format, big-endian in memory, as A, R, G, B.
void texel(uint32_t f, const uint8_t* p, uint8_t* argb) {
    const uint32_t h = (uint32_t{p[0]} << 8) | p[1];
    switch (f) {
    case kA8R8G8B8: argb[0] = p[0], argb[1] = p[1], argb[2] = p[2], argb[3] = p[3]; break;
    case kB8: argb[0] = argb[1] = argb[2] = 0, argb[3] = p[0]; break;
    case kA1R5G5B5: argb[0] = (h >> 15) ? 255 : 0, argb[1] = expand(h >> 10, 5), argb[2] = expand(h >> 5, 5), argb[3] = expand(h, 5); break;
    case kA4R4G4B4: argb[0] = expand(h >> 12, 4), argb[1] = expand(h >> 8, 4), argb[2] = expand(h >> 4, 4), argb[3] = expand(h, 4); break;
    case kR5G6B5: argb[0] = 255, argb[1] = expand(h >> 11, 5), argb[2] = expand(h >> 5, 6), argb[3] = expand(h, 5); break;
    default: break;
    }
}

// One 4 by 4 block of DXT1, 3 or 5 (as on a PC: little-endian; *the RSX's byte order for them is a stated reading, to
// be confirmed by the picture*), as sixteen A, R, G, B texels in rows. Interpolated values round down (*a stated choice*).
void block(uint32_t f, const uint8_t* p, uint8_t (*argb)[4]) {
    const uint8_t* colour = f == kDxt1 ? p : p + 8;
    const uint32_t c0 = colour[0] | (uint32_t{colour[1]} << 8), c1 = colour[2] | (uint32_t{colour[3]} << 8);
    const uint32_t indices = colour[4] | (uint32_t{colour[5]} << 8) | (uint32_t{colour[6]} << 16) | (uint32_t{colour[7]} << 24);
    uint8_t palette[4][4];
    const auto rgb = [](uint32_t c, uint8_t* out) { out[0] = 255, out[1] = expand(c >> 11, 5), out[2] = expand(c >> 5, 6), out[3] = expand(c, 5); };
    rgb(c0, palette[0]);
    rgb(c1, palette[1]);
    const bool four = f != kDxt1 || c0 > c1;  // DXT3 and 5 always take four colours
    for (unsigned k = 1; k < 4; ++k) {
        palette[2][k] = static_cast<uint8_t>(four ? (2 * palette[0][k] + palette[1][k]) / 3 : (palette[0][k] + palette[1][k]) / 2);
        palette[3][k] = static_cast<uint8_t>(four ? (palette[0][k] + 2 * palette[1][k]) / 3 : 0);
    }
    palette[2][0] = 255;
    palette[3][0] = four ? 255 : 0;
    for (unsigned i = 0; i < 16; ++i) std::copy_n(palette[(indices >> (2 * i)) & 3], 4, argb[i]);
    if (f == kDxt23) {
        for (unsigned i = 0; i < 16; ++i) argb[i][0] = expand(p[i / 2] >> (4 * (i & 1)), 4);
    } else if (f == kDxt45) {
        const uint32_t a0 = p[0], a1 = p[1];
        uint64_t bits = 0;
        for (unsigned i = 0; i < 6; ++i) bits |= uint64_t{p[2 + i]} << (8 * i);
        uint32_t alpha[8] = {a0, a1};
        for (unsigned k = 2; k < 8; ++k) alpha[k] = a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7 : k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : k == 6 ? 0 : 255;
        for (unsigned i = 0; i < 16; ++i) argb[i][0] = static_cast<uint8_t>(alpha[(bits >> (3 * i)) & 7]);
    }
}

// The swizzled layout's offset of texel (x, y), in texels, for sides of 2^lw by 2^lh: the bits of x and y interleaved, x's
// first, the longer side's last bits on their own.
uint32_t swizzled(uint32_t x, uint32_t y, uint32_t lw, uint32_t lh) {
    uint32_t offset = 0, bit = 0;
    for (uint32_t i = 0; i < std::max(lw, lh); ++i) {
        if (i < lw) offset |= ((x >> i) & 1) << bit++;
        if (i < lh) offset |= ((y >> i) & 1) << bit++;
    }
    return offset;
}

}  // namespace

std::string texture_extent(const TextureUnit& t, size_t& guest_bytes) {
    const uint32_t f = (t.format >> 8) & 0x1f;
    const bool linear = (t.format >> 13) & 1;
    if ((t.format & 3) != 1 && (t.format & 3) != 2) return what("a texture in location %u", t.format & 3);
    if (((t.format >> 4) & 0xf) != 2) return what("a texture of %u dimensions", (t.format >> 4) & 0xf);
    if ((t.format >> 14) & 1) return "unnormalised texture coordinates";
    if (!unit_bytes(f)) return what("texture format %u", f);
    const bool cube = (t.format & 4) != 0;
    if (cube && (t.rect >> 16) != (t.rect & 0xffff)) return "a cube map that isn't square";
    const uint32_t width = t.rect >> 16, height = t.rect & 0xffff, levels = (t.format >> 16) & 0xffff, depth = t.control3 >> 20, pitch = t.control3 & 0xfffff;
    if (!width || !height || !levels) return what("a texture with no texels (%08x)", t.rect);
    if (depth != 1) return what("a texture of depth %u", depth);
    if (linear && !compressed(f) && levels > 1) return "a linear texture with mipmaps";
    if (!linear && !compressed(f) && !(std::has_single_bit(width) && std::has_single_bit(height))) return "a swizzled texture whose sides aren't powers of two";
    size_t at = 0;
    for (uint32_t face = 0; face < (cube ? 6u : 1u); ++face) {
        at = (at + 127) & ~size_t{127};  // a cube map's faces each begin on 128 bytes (*a stated reading*)
        for (uint32_t l = 0; l < levels; ++l) {
            const uint32_t w = std::max(1u, width >> l), h = std::max(1u, height >> l);
            if (compressed(f)) {
                const uint32_t bh = (h + 3) / 4, row = (w + 3) / 4 * unit_bytes(f);
                const uint32_t stride = linear && pitch && l == 0 ? pitch : row;
                if (stride < row) return what("a compressed texture's pitch %u", pitch);
                at += size_t{stride} * bh;
            } else {
                if (linear && pitch < w * unit_bytes(f)) return what("a linear texture's pitch %u", pitch);
                at += linear ? size_t{pitch} * h : size_t{w} * h * unit_bytes(f);
            }
        }
    }
    guest_bytes = at;
    return {};
}

bool texture_is_plain_argb(const TextureUnit& t) {
    const uint32_t f = (t.format >> 8) & 0x1f, levels = (t.format >> 16) & 0xffff;
    const bool linear = (t.format >> 13) & 1, cube = (t.format & 4) != 0;
    if (f != kA8R8G8B8 || !linear || cube || levels != 1 || ((t.format >> 4) & 0xf) != 2 || ((t.format >> 14) & 1) || (t.control3 >> 20) != 1) return false;
    // The remap as stored: each output (A, R, G, B) its own input.
    for (unsigned c = 0; c < 4; ++c)
        if (((t.remap >> (2 * c)) & 3) != c || ((t.remap >> (8 + 2 * c)) & 3) != 2) return false;
    return (t.remap >> 16) == 0;
}

std::string decode_texture(const TextureUnit& t, const uint8_t* memory, Texels& out) {
    if (size_t bytes = 0; !texture_extent(t, bytes).empty()) return texture_extent(t, bytes);
    const uint32_t f = (t.format >> 8) & 0x1f;
    const bool linear = (t.format >> 13) & 1;
    // (Checked by texture_extent.)
    const uint32_t width = t.rect >> 16, height = t.rect & 0xffff, levels = (t.format >> 16) & 0xffff, pitch = t.control3 & 0xfffff;
    // The remap: each output (A, R, G, B) zero, one, or one of the inputs (A, R, G, B).
    uint32_t source[4], kind[4];
    for (unsigned c = 0; c < 4; ++c) source[c] = (t.remap >> (2 * c)) & 3, kind[c] = (t.remap >> (8 + 2 * c)) & 3;
    for (unsigned c = 0; c < 4; ++c)
        if (kind[c] == 3) return what("texture remap %08x", t.remap);
    if (t.remap >> 17) return what("texture remap %08x", t.remap);
    if (f == kB8)
        for (unsigned c = 0; c < 4; ++c)
            if (kind[c] == 2 && source[c] != 3) return what("a B8 texture's remap %08x, which reads a channel other than B", t.remap);
    // Each channel out (R, G, B, A, as the image holds them) as an index into {A, R, G, B, 0, 255}: output R is the remap's
    // channel 1, G 2, B 3, A 0.
    std::array<uint8_t, 4> pick{};
    for (unsigned c = 0; c < 4; ++c) {
        const unsigned o = c == 3 ? 0 : c + 1;
        pick[c] = static_cast<uint8_t>(kind[o] == 0 ? 4 : kind[o] == 1 ? 5 : source[o]);
    }
    const bool as_stored = pick == std::array<uint8_t, 4>{1, 2, 3, 0};  // R, G, B, A as they are

    const uint32_t faces = (t.format & 4) ? 6 : 1;
    out = {width, height, levels, faces, {}, {}};
    size_t at = 0;  // in guest memory, from the texture's start
    for (uint32_t face = 0; face < faces; ++face) {
    if (faces > 1) at = (at + 127) & ~size_t{127};
    for (uint32_t l = 0; l < levels; ++l) {
        const uint32_t w = std::max(1u, width >> l), h = std::max(1u, height >> l);
        out.level_offset.push_back(out.rgba.size());
        const size_t base = out.rgba.size();
        out.rgba.resize(base + size_t{w} * h * 4);
        uint8_t* rgba = out.rgba.data() + base;
        const uint8_t* level = memory + t.address + at;
        const auto put = [&](uint32_t x, uint32_t y, const uint8_t* argb) {
            const uint8_t choices[6] = {argb[0], argb[1], argb[2], argb[3], 0, 255};
            uint8_t* q = rgba + (size_t{y} * w + x) * 4;
            for (unsigned c = 0; c < 4; ++c) q[c] = choices[pick[c]];
        };
        if (compressed(f)) {
            const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, row = bw * unit_bytes(f);
            const uint32_t stride = linear && pitch && l == 0 ? pitch : row;
            if (stride < row) return what("a compressed texture's pitch %u", pitch);
            for (uint32_t by = 0; by < bh; ++by)
                for (uint32_t bx = 0; bx < bw; ++bx) {
                    uint8_t argb[16][4] = {};
                    block(f, level + size_t{by} * stride + size_t{bx} * unit_bytes(f), argb);
                    for (uint32_t i = 0; i < 16; ++i) {
                        const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                        if (x < w && y < h) put(x, y, argb[i]);
                    }
                }
            at += size_t{stride} * bh;
        } else {
            const uint32_t bytes = unit_bytes(f);
            if (linear && pitch < w * bytes) return what("a linear texture's pitch %u", pitch);
            const uint32_t lw = static_cast<uint32_t>(std::countr_zero(w)), lh = static_cast<uint32_t>(std::countr_zero(h));
            // A texel's place in guest memory, as a column's part and a row's: swizzled, x's bits and y's never share a
            // place, so the offset of (x, y) is that of (x, 0) plus that of (0, y).
            std::vector<size_t> xs(w), ys(h);
            for (uint32_t x = 0; x < w; ++x) xs[x] = linear ? size_t{x} * bytes : size_t{swizzled(x, 0, lw, lh)} * bytes;
            for (uint32_t y = 0; y < h; ++y) ys[y] = linear ? size_t{y} * pitch : size_t{swizzled(0, y, lw, lh)} * bytes;
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* row = level + ys[y];
                if (f == kA8R8G8B8 && as_stored) {
                    // A, R, G, B in memory to R, G, B, A: a word turned by a byte (on a little-endian host, as load32 assumes).
                    uint8_t* q = rgba + size_t{y} * w * 4;
                    for (uint32_t x = 0; x < w; ++x) {
                        uint32_t v;
                        std::memcpy(&v, row + xs[x], 4);
                        v = std::rotr(v, 8);
                        std::memcpy(q + 4 * size_t{x}, &v, 4);
                    }
                    continue;
                }
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t argb[4] = {};  // (every format texel() takes fills all four)
                    texel(f, row + xs[x], argb);
                    put(x, y, argb);
                }
            }
            at += linear ? size_t{pitch} * h : size_t{w} * h * bytes;
        }
    }
    }
    out.guest_bytes = at;
    return {};
}

std::string decode_sampler(const TextureUnit& t, SamplerFields& out) {
    out = {};
    // The wrap word: S, T, R at bits 0, 8, 16; gamma at 20 and the depth comparison at 28 (only for depth formats).
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t mode = (t.wrap >> (8 * i)) & 0xf;
        if (mode < 1 || mode > 4) return what("texture wrap mode %u", mode);
        out.wrap[i] = mode;
    }
    if (t.wrap & 0x0ff0f0f0u) return what("texture wrap word %08x (gamma, or fields not known here)", t.wrap);
    // The filter word: the LOD bias (bits 0 to 12, signed, 8 bits of fraction: *a stated reading*), the convolution
    // (13, 14: used only by the convolution filters), min (16 to 19), mag (24 to 27).
    const uint32_t min = (t.filter >> 16) & 0xf, mag = (t.filter >> 24) & 0xf;
    if (t.filter & 0xf0f08000u) return what("texture filter word %08x (fields not known here)", t.filter);
    if (mag != 1 && mag != 2) return what("texture magnification filter %u", mag);
    if (min < 1 || min > 6) return what("texture minification filter %u", min);
    out.mag = mag - 1;
    out.min = (min == 2 || min == 4 || min == 6) ? 1 : 0;
    out.mip = min <= 2 ? 0 : (min == 3 || min == 4) ? 1 : 2;
    const int32_t bias = static_cast<int32_t>((t.filter & 0x1fff) << 19) >> 19;
    out.bias = static_cast<float>(bias) / 256.0f;
    // Control 0: enabled (31), the LOD range (4.8 fixed point: min at 19, max at 7), the anisotropy (4 to 6: 1, 2, 4, 6, 8,
    // 10, 12, 16 times).
    out.min_lod = static_cast<float>((t.control0 >> 19) & 0xfff) / 256.0f;
    out.max_lod = static_cast<float>((t.control0 >> 7) & 0xfff) / 256.0f;
    static constexpr uint32_t kAnisotropy[8] = {1, 2, 4, 6, 8, 10, 12, 16};
    out.anisotropy = kAnisotropy[(t.control0 >> 4) & 7];
    if (t.control0 & 0xf) return what("texture control word %08x (fields not known here)", t.control0);
    out.border = t.border;
    return {};
}

}  // namespace rr7

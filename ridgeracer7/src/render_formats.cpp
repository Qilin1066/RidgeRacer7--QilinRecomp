#include "render_formats.hpp"

#include <cmath>

namespace rr7 {

namespace {

uint32_t load_be32(const uint8_t* p) {
    uint32_t w;
    std::memcpy(&w, p, 4);
    return std::byteswap(w);
}

// One component of a vertex attribute of the RSX's type `Type` (CELL_GCM_VERTEX_...), big-endian.
template <uint32_t Type>
float component(const uint8_t* p, uint32_t c) {
    if constexpr (Type == 1) return std::max(-1.0f, static_cast<float>(static_cast<int16_t>((p[2 * c] << 8) | p[2 * c + 1])) / 32767.0f);  // S1
    if constexpr (Type == 2) return std::bit_cast<float>(load_be32(p + 4 * c));                                                         // F
    if constexpr (Type == 3) return half_float(p + 2 * c);                                                                             // SF
    if constexpr (Type == 4) return static_cast<float>(p[c]) / 255.0f;                                                                 // UB
    if constexpr (Type == 5) return static_cast<float>(static_cast<int16_t>((p[2 * c] << 8) | p[2 * c + 1]));                         // S32K
    if constexpr (Type == 7) return static_cast<float>(p[c]);                                                                          // UB256
}
// (By its size, 1 to 4, known when compiled: each vertex's four floats made in registers and stored at once.)
template <uint32_t Type, uint32_t Size>
void decode_sized(const uint8_t* p, size_t stride, uint32_t count, float* out) {
    for (uint32_t v = 0; v < count; ++v, p += stride, out += 4) {
        float f[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (uint32_t c = 0; c < Size; ++c) f[c] = component<Type>(p, c);
        std::memcpy(out, f, sizeof f);
    }
}
template <uint32_t Type>
void decode_attribute(const uint8_t* p, size_t stride, uint32_t size, uint32_t count, float* out) {
    switch (size) {
    case 1: return decode_sized<Type, 1>(p, stride, count, out);
    case 2: return decode_sized<Type, 2>(p, stride, count, out);
    case 3: return decode_sized<Type, 3>(p, stride, count, out);
    case 4: return decode_sized<Type, 4>(p, stride, count, out);
    default: break;
    }
    // (A size past four, as the format word allows: written as it always was, its components past the fourth into the next
    // vertex's, which that vertex's own then replace.)
    for (uint32_t v = 0; v < count; ++v, p += stride, out += 4) {
        out[0] = out[1] = out[2] = 0.0f, out[3] = 1.0f;
        for (uint32_t c = 0; c < size; ++c) out[c] = component<Type>(p, c);
    }
}

// An attribute's vertices in its own format: `Bytes` a component, `Size` given, `One` the fourth's value where three are.
// (All known when compiled: a vertex is a few loads, swaps and a store.)
template <uint32_t Bytes, uint32_t Size, uint32_t One>
void encode_sized(const uint8_t* p, size_t stride, uint32_t count, uint8_t* out) {
    constexpr uint32_t padded = Bytes < 4 && Size == 3 ? 4 : Size;
    constexpr uint32_t out_stride = (Bytes * padded + 3) / 4 * 4;
    for (uint32_t v = 0; v < count; ++v, p += stride, out += out_stride) {
        uint8_t vertex[out_stride] = {};
        for (uint32_t c = 0; c < Size; ++c)
            for (uint32_t b = 0; b < Bytes; ++b) vertex[Bytes * c + b] = p[Bytes * c + Bytes - 1 - b];
        if constexpr (padded != Size)
            for (uint32_t b = 0; b < Bytes; ++b) vertex[Bytes * 3 + b] = static_cast<uint8_t>(One >> (8 * b));
        std::memcpy(out, vertex, out_stride);
    }
}
template <uint32_t Bytes, uint32_t One>
void encode_type(uint32_t size, const uint8_t* p, size_t stride, uint32_t count, uint8_t* out) {
    switch (size) {
    case 1: return encode_sized<Bytes, 1, One>(p, stride, count, out);
    case 2: return encode_sized<Bytes, 2, One>(p, stride, count, out);
    case 3: return encode_sized<Bytes, 3, One>(p, stride, count, out);
    default: return encode_sized<Bytes, 4, One>(p, stride, count, out);
    }
}

}  // namespace

void encode_attributes(uint8_t f, const uint8_t* p, size_t stride, uint32_t count, uint8_t* out) {
    const uint32_t size = f >> 4;
    switch (f & 0xf) {
    case 1: return encode_type<2, 0x7fff>(size, p, stride, count, out);  // S1: 1.0 is 32767
    case 2: return encode_type<4, 0>(size, p, stride, count, out);       // F (three floats are a format of their own)
    case 3: return encode_type<2, 0x3c00>(size, p, stride, count, out);  // SF: 1.0 as a half
    case 4: return encode_type<1, 0xff>(size, p, stride, count, out);    // UB: 1.0 is 255
    case 5: return encode_type<2, 1>(size, p, stride, count, out);       // S32K
    case 7: return encode_type<1, 1>(size, p, stride, count, out);       // UB256
    default: break;
    }
}

float half_float(const uint8_t* p) {
    const uint16_t h = static_cast<uint16_t>((p[0] << 8) | p[1]);
    const uint32_t sign = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    if (e != 0 && e != 31) return std::bit_cast<float>((sign << 31) | ((e + 112) << 23) | (m << 13));
    const float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : m ? NAN : INFINITY;
    return sign ? -v : v;
}

bool decode_attributes(uint32_t format, const uint8_t* p, size_t stride, uint32_t count, float* out) {
    const uint32_t type = format & 0xf, size = (format >> 4) & 0xf;
    if (!count || !size) {
        for (uint32_t v = 0; v < count; ++v) out[4 * v] = out[4 * v + 1] = out[4 * v + 2] = 0.0f, out[4 * v + 3] = 1.0f;
        return true;
    }
    switch (type) {
    case 1: decode_attribute<1>(p, stride, size, count, out); return true;
    case 2: decode_attribute<2>(p, stride, size, count, out); return true;
    case 3: decode_attribute<3>(p, stride, size, count, out); return true;
    case 4: decode_attribute<4>(p, stride, size, count, out); return true;
    case 5: decode_attribute<5>(p, stride, size, count, out); return true;
    case 7: decode_attribute<7>(p, stride, size, count, out); return true;
    default: return false;
    }
}

}  // namespace rr7

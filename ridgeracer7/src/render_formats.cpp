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
template <uint32_t Type>
void decode_attribute(const uint8_t* p, size_t stride, uint32_t size, uint32_t count, float* out) {
    for (uint32_t v = 0; v < count; ++v, p += stride, out += 4) {
        out[0] = out[1] = out[2] = 0.0f, out[3] = 1.0f;
        for (uint32_t c = 0; c < size; ++c) out[c] = component<Type>(p, c);
    }
}

}  // namespace

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

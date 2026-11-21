// The video output as the game has set it (video.cpp).
#pragma once

#include <cstdint>

namespace rr7 {

struct VideoMode {
    bool configured = false;
    uint32_t resolution = 0, width = 0, height = 0;
    uint32_t format = 0;  // 0: X8R8G8B8, 1: X8B8G8R8, 2: 16-bit float RGBA
    uint32_t aspect = 0, pitch = 0;
};
VideoMode video_mode();

}  // namespace rr7

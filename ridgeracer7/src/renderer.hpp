// The RSX's drawing, on Vulkan (renderer.cpp). The RSX thread (rsx.cpp)
// reads the commands and gathers each draw's state; this draws it.
//
// **The first design, chosen for being plainly correct, not fast:** guest
// memory is the only truth. Each draw uploads its colour surface from guest
// memory, draws, and writes it back; a flip reads the display buffer from
// guest memory. So whatever the game reads from memory (a render target as a
// texture, a picture the CPU looks at) is what was drawn. Keeping surfaces
// on the GPU and writing them back only when something reads them is the
// next design, once the picture is right.
//
// Runtime code: no std::format.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "textures.hpp"

namespace qilin::host {
struct Gpu;
}

namespace rr7 {

// A colour surface: where it is, how it is laid out.
struct Surface {
    uint32_t address = 0;  // in guest memory
    uint32_t pitch = 0, format = 0, width = 0, height = 0;
};

// One draw, as the RSX's methods left it.
struct Draw {
    Surface color;
    uint32_t color_mask = 0;  // the RSX's: blue bit 0, green 8, red 16, alpha 24
    uint32_t primitive = 0, first = 0, count = 0;
    // Vertex attributes: the raw format word (SET_VERTEX_DATA_ARRAY_FORMAT) and the address of the first vertex's.
    std::array<uint32_t, 16> attribute_format{}, attribute_address{};
    const float* vertex_constants = nullptr;  // 468 vec4
    std::vector<uint8_t> vertex_program;      // its microcode, big-endian, from its first instruction to its last
    std::vector<uint8_t> fragment_program;    // likewise, as the RSX reads it (halves exchanged), with its constants
    // The viewport: the RSX's scale and offset, its clip rectangle, the window's height and origin.
    std::array<float, 4> viewport_scale{}, viewport_offset{};
    uint32_t clip_x = 0, clip_y = 0, clip_width = 0, clip_height = 0, window_height = 0;
    bool origin_bottom = false;
    uint32_t scissor_x = 0, scissor_y = 0, scissor_width = 0, scissor_height = 0;
    bool blend = false;
    uint32_t blend_src = 0, blend_dst = 0, blend_equation = 0;
    bool cull = false;
    uint32_t cull_face = 0, front_face = 0;
    // The alpha test, for the fragment program (rsx_spirv.cpp): {reference, scale, -, -}, {less, equal, greater, -} passing.
    std::array<float, 8> alpha_test{0, 0, 0, 0, 1, 1, 1, 0};
    std::array<TextureUnit, 16> textures{};
    // The depth test: on or off, its function (OpenGL's NEVER 0x200 to ALWAYS 0x207), whether depth is written, and the
    // depth-stencil surface (format 2: Z24S8, a big-endian word of depth in the top 24 bits and stencil in the low 8).
    bool depth_test = false, depth_write = false;
    uint32_t depth_func = 0;
    Surface depth;
};

// The device to draw with, and the lock every use of its queue takes (the window's presentation uses the queue too).
void renderer_start(qilin::host::Gpu& gpu, std::mutex& queue);
bool renderer_started();
// Draws it, into a batch: surfaces stay on the GPU until the batch is flushed, when everything drawn is written back to
// guest memory. The RSX's thread flushes at every point where the game may look (rsx.cpp); the renderer itself when a
// draw would read a surface drawn in the batch (a texture, or the same memory as another surface), or its batch is full.
void renderer_draw(const Draw& d);
void renderer_flush();
// The texture cache's invalidation (RSX method 0x1FD8): textures are hashed again at their next draw.
void renderer_invalidate_textures();
// Where the renderer's time has gone (a development aid: logged when a timed run ends).
void renderer_report();

// Where a flip's picture goes (main.cpp: the window): RGBA8, `width` by `height`, rows packed.
void set_present_sink(std::function<void(const uint8_t* rgba, uint32_t width, uint32_t height)> sink);
void present_frame(const uint8_t* rgba, uint32_t width, uint32_t height);

}  // namespace rr7

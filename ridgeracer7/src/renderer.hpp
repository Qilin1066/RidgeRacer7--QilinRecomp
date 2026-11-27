// The RSX's drawing, on Vulkan (renderer.cpp). The RSX thread (rsx.cpp)
// reads the commands and gathers each draw's state; this draws it.
//
// Guest memory is the truth wherever the game can look: draws are recorded
// into batches, surfaces stay on the GPU between the points where the game
// can look, and what was drawn goes back to guest memory when something
// first touches it (docs/runtime.md). The pure parts are render_formats
// (guest memory to the GPU and back), render_state (primitives, polygons, the
// window) and textures; renderer_pipelines makes the Vulkan objects.
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
// (Vulkan's handles, as vulkan_core.h declares them on a 64-bit host, so that this header needn't include it.)
typedef struct VkImage_T* VkImage;
typedef struct VkImageView_T* VkImageView;

namespace rr7 {

// A colour surface: where it is, how it is laid out.
struct Surface {
    uint32_t address = 0;  // in guest memory
    uint32_t pitch = 0, format = 0, width = 0, height = 0;
};

inline constexpr uint32_t kRestart = 0xffffffffu;

// One draw, as the RSX's methods left it.
struct Draw {
    Surface color;
    uint32_t color_mask = 0;  // the RSX's: blue bit 0, green 8, red 16, alpha 24
    uint32_t primitive = 0, first = 0, count = 0;  // the vertices fetched: [first, first + count)
    bool indexed = false;
    std::vector<uint32_t> indices;  // when indexed: the order the vertices are drawn in, from 0 for `first`; kRestart ends a primitive
    // Vertex attributes: the raw format word (SET_VERTEX_DATA_ARRAY_FORMAT) and the address of the first vertex's.
    std::array<uint32_t, 16> attribute_format{}, attribute_address{};
    const float* vertex_constants = nullptr;  // 468 vec4
    uint64_t constants_version = 0;           // changes whenever any of them does (rsx.cpp)
    std::vector<uint8_t> vertex_program;      // its microcode, big-endian, from its first instruction to its last
    std::vector<uint8_t> fragment_program;    // likewise, as the RSX reads it (halves exchanged), with its constants
    // The viewport: the RSX's scale and offset, its clip rectangle, the window's height and origin.
    std::array<float, 4> viewport_scale{}, viewport_offset{};
    uint32_t clip_x = 0, clip_y = 0, clip_width = 0, clip_height = 0, window_height = 0;
    bool origin_bottom = false;
    uint32_t window_offset_x = 0, window_offset_y = 0;  // the window's origin on the surface (SET_WINDOW_OFFSET)
    uint32_t scissor_x = 0, scissor_y = 0, scissor_width = 0, scissor_height = 0;
    bool blend = false;
    uint32_t blend_src = 0, blend_dst = 0, blend_equation = 0;
    bool logic = false;      // a logic operation in place of blending (SET_LOGIC_OP_ENABLE)
    uint32_t logic_op = 0;   // OpenGL's CLEAR 0x1500 to SET 0x150F (SET_LOGIC_OP)
    bool cull = false;
    uint32_t cull_face = 0, front_face = 0;
    // Flat shading (SET_SHADE_MODE), and whether a primitive's first vertex gives its colour rather than its last
    // (SET_FLAT_SHADE_OP).
    bool flat = false, flat_first = false;
    bool two_sided = false;  // back faces take the vertex program's back colours (SET_TWO_SIDE_LIGHT_EN)
    // The user clip planes (SET_USER_CLIP_PLANE_CONTROL): plane n's test in bits 4n to 4n + 3, 0 off, 1 LT, 2 GE.
    uint32_t clip_planes = 0;
    bool line_stipple = false;  // SET_LINE_STIPPLE: refused where lines are drawn
    // The alpha test, for the fragment program (rsx_spirv.cpp): {reference, scale, -, -}, {less, equal, greater, -} passing.
    std::array<float, 8> alpha_test{0, 0, 0, 0, 1, 1, 1, 0};
    std::array<TextureUnit, 16> textures{};
    // The depth test: on or off, its function (OpenGL's NEVER 0x200 to ALWAYS 0x207), whether depth is written, and the
    // depth-stencil surface (format 2: Z24S8, a big-endian word of depth in the top 24 bits and stencil in the low 8).
    bool depth_test = false, depth_write = false;
    bool depth_surface = false;  // the depth-stencil surface is attached (for the depth test, the stencil test, or both)
    // The stencil test (OpenGL's functions and operations), the front faces' and the back faces' (the front's when it isn't
    // two-sided): function, reference, function mask, write mask, and the operations on failing, on the depth test failing,
    // on passing.
    struct Stencil {
        uint32_t func = 0x207, ref = 0, func_mask = 0xff, write_mask = 0xff, fail = 0x1e00, zfail = 0x1e00, zpass = 0x1e00;
    };
    bool stencil_test = false;
    Stencil stencil_front, stencil_back;
    // Polygons (render_state.hpp: rasterisation): their faces' modes (OpenGL's POINT 0x1B00, LINE 0x1B01, FILL 0x1B02), polygon
    // offset's enable for each mode and its factor and units, the edge flag.
    uint32_t front_polygon_mode = 0, back_polygon_mode = 0;
    bool offset_point = false, offset_line = false, offset_fill = false;
    float bias_factor = 0, bias_units = 0;
    bool edge_flag = true;
    // The depth bounds test: the depth already stored at the pixel against [min, max], as Vulkan's.
    bool depth_bounds = false;
    float bounds_min = 0, bounds_max = 1;
    uint32_t depth_func = 0;
    Surface depth;
    bool logged = false;  // RR7_DRAWLOG: the renderer logs the fragment program's inline constants too

    // Back to a new draw's state, its vectors emptied but keeping their storage: the RSX's thread reuses one Draw for
    // every draw rather than allocate three vectors a draw.
    void reset() {
        std::vector<uint32_t> kept_indices = std::move(indices);
        std::vector<uint8_t> kept_vertex = std::move(vertex_program), kept_fragment = std::move(fragment_program);
        *this = Draw{};
        kept_indices.clear(), kept_vertex.clear(), kept_fragment.clear();
        indices = std::move(kept_indices), vertex_program = std::move(kept_vertex), fragment_program = std::move(kept_fragment);
    }
};

// The device to draw with, and the lock every use of its queue takes (the window's presentation uses the queue too).
void renderer_start(qilin::host::Gpu& gpu, std::mutex& queue);
bool renderer_started();
// Draws it, into a batch: surfaces stay on the GPU until the batch is flushed. Then what was drawn is written back to guest
// memory: when something first touches it (its pages are unreadable until then: renderer_fault), or at once where faults
// aren't handled or RR7_EAGER_WRITEBACK is set. The RSX's thread flushes at every point where the game may look (rsx.cpp); the renderer itself when a
// draw would read a surface drawn in the batch (a texture, or the same memory as another surface), or its batch is full.
void renderer_draw(const Draw& d);
// A clear (CLEAR_SURFACE) of whole pixels in a rectangle: the colour surface's four channels, the depth, the stencil.
struct Clear {
    bool color = false;  // all four channels (a clear of only some is done on guest memory: rsx.cpp)
    Surface color_surface;
    uint32_t color_value = 0;  // A8R8G8B8
    bool depth = false, stencil = false;
    Surface depth_surface;     // Z24S8
    uint32_t depth_value = 0;  // depth in the top 24 bits, stencil in the low 8
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) by [y0, y1), within the surfaces
};
// Records it into the batch, on the surfaces kept on the GPU, as a draw would be.
void renderer_clear(const Clear& c);
// `why`: what asked for it, a short phrase (a timed run's report counts flushes by it).
void renderer_flush(const char* why);
// The RSX is about to write these guest bytes itself (an inline transfer): the batch is flushed first only if a surface
// drawn in it overlaps them (so the bytes land after what was drawn); otherwise textures the batch has read from them
// are read again at their next use.
void renderer_guest_written(uint32_t address, uint64_t bytes, const char* why);
// Host code is about to read or write these guest bytes through the host's system (a file read or write): any surface
// still to be written back there is written back first, since the system can't take a fault for it as code can.
void renderer_guest_access(uint32_t address, uint64_t bytes);
// A fault at this guest address (fault.cpp): true if it was on a page of a surface still to be written back, which
// now has been, so the access can be made again.
bool renderer_fault(uint32_t address);
// Where the renderer's time has gone (a development aid: logged when a timed run ends).
void renderer_report();

// A flip's picture: RGBA8 pixels, `width` by `height`, rows packed; or (rgba null) one the renderer keeps on the GPU,
// numbered `gpu_frame` (renderer_frame_image).
struct Frame {
    const uint8_t* rgba = nullptr;
    int gpu_frame = -1;
    uint32_t width = 0, height = 0;
};
// Where a flip's picture goes (main.cpp: the window).
void set_present_sink(std::function<void(const Frame&)> sink);
void present_frame(const Frame& frame);
// The flip of the display buffer at `address`: true if it is a surface the renderer holds on the GPU, newer than guest
// memory, which is then copied to a picture of the renderer's own on the GPU and given to the present sink without
// passing through guest memory; false if guest memory holds it (the caller shows it from there).
bool renderer_flip(uint32_t address, uint32_t pitch, uint32_t width, uint32_t height);
// A picture renderer_flip made: its image (TRANSFER_SRC_OPTIMAL, sampled; its copy submitted to the queue before the
// flip gave it) and a view of it with alpha 1, for Display::present_image. Kept until two flips later.
void renderer_frame_image(int gpu_frame, VkImage& image, VkImageView& view);
// Its pixels, read back (RGBA8, rows packed): a development aid, for pictures saved to files.
void renderer_frame_pixels(int gpu_frame, std::vector<uint8_t>& rgba);

}  // namespace rr7

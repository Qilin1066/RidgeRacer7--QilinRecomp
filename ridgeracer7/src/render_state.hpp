// The RSX's drawing state as the renderer gives it to Vulkan: how primitives are assembled, how polygons are rasterised,
// the window's place on the surface, the logic operations. Pure functions, tested without a GPU (ridgeracer7/tests/render_test.cpp); what Vulkan can't do the same way is refused
// by name, never approximated.
//
// Runtime code: no std::format.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "renderer.hpp"

namespace rr7 {

// What Vulkan draws a draw's vertices as.
enum class Topology : uint8_t { points, lines, line_strip, triangles, triangle_strip };
// Which vertex of a primitive gives its colour when shading is flat: `any` with smooth shading (the order is kept as it is),
// or the RSX's choice (SET_FLAT_SHADE_OP), which assemble() turns into Vulkan's (the first vertex of each primitive).
enum class Provoking : uint8_t { any, first, last };

struct Assembly {
    Topology topology = Topology::triangles;
    bool indexed = false;  // drawn through indices
    bool made = false;     // ...those assemble() made (in its `indices`), not the order it was given
};

// The RSX's primitive (OpenGL's numbers plus one: 1 points, 2 lines, 4 a line strip, 5 triangles, 6 a triangle strip, 7 a fan,
// 8 quads, 9 a quad strip, 10 a polygon) over the vertices in `order` (kRestart ends one primitive and begins the next;
// `given_indexed`: the order is the game's indices, not 0, 1, 2...), as Vulkan draws it. Those Vulkan lacks, or may lack
// (fans, on portability devices), become lists of triangles: a quad (a, b, c, d) as (a, b, c) and (a, c, d), a fan's or
// polygon's triangles about its first vertex (*the diagonals are a stated choice*), a strip's keeping their alternate
// windings. With primitive restart every piece is made into a list. With flat shading every primitive is made into a list
// whose first vertex is the RSX's provoking one, its winding kept (a quad's diagonal then runs from the provoking vertex).
std::expected<Assembly, std::string> assemble(uint32_t primitive, std::span<const uint32_t> order, bool given_indexed, Provoking provoking,
                                              std::vector<uint32_t>& indices);

// How polygons are rasterised: their faces' polygon mode (SET_FRONT_POLYGON_MODE, SET_BACK_POLYGON_MODE: OpenGL's POINT 0x1B00,
// LINE 0x1B01, FILL 0x1B02, and 0, as the system leaves it, taken as FILL) and whether polygon offset applies (the
// enable for the mode drawn in: SET_POLY_OFFSET_POINT_ENABLE, LINE, FILL). Vulkan has one polygon mode for both faces: faces
// drawn in different modes are refused, as are quads and polygons drawn as lines or points (made into triangles, they would
// show a diagonal's edge or points twice) and lines with the edge flag off. Points and lines aren't polygons: neither
// applies to them.
enum class PolygonMode : uint8_t { fill, line, point };
struct Rasterisation {
    PolygonMode mode = PolygonMode::fill;
    bool depth_bias = false;
};
struct PolygonState {
    uint32_t front_mode = 0, back_mode = 0;
    bool cull = false;
    uint32_t cull_face = 0;  // OpenGL's FRONT 0x404, BACK 0x405, FRONT_AND_BACK 0x408
    bool offset_point = false, offset_line = false, offset_fill = false;
    bool edge_flag = true;
};
std::expected<Rasterisation, std::string> rasterisation(uint32_t primitive, const PolygonState& s);

// A logic operation (SET_LOGIC_OP: OpenGL's CLEAR 0x1500 to SET 0x150F) as Vulkan's VkLogicOp, which lists them in the same
// order; none for any other value.
std::optional<uint32_t> logic_op(uint32_t gl);

// The window's place on the surface: the viewport as the translated vertex programs take it (rsx_spirv.hpp: clip_xy,
// clip_z, for a Vulkan viewport covering the surface) and the scissor, in the surface's pixels, clipped to it. The RSX's
// window coordinates (its scale and offset; y down from the top, or up from the bottom of a window `window_height` high)
// are the surface's less the window offset (SET_WINDOW_OFFSET: *a stated reading, as on NVIDIA's chips, of the window's
// origin on the surface: it moves the viewport and the scissor alike*).
struct WindowMapping {
    std::array<float, 8> push{};  // clip_xy, then clip_z
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the scissor: [x0, x1) by [y0, y1)
};
WindowMapping window_mapping(const Draw& d);

}  // namespace rr7

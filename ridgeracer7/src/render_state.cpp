#include "render_state.hpp"

#include <algorithm>
#include <cstdio>

namespace rr7 {

namespace {

std::string what(const char* format, uint32_t value) {
    char text[200];
    std::snprintf(text, sizeof text, format, value);
    return text;
}

// One piece of a primitive (between restarts), as a list (points, lines or triangles) into `out`, its provoking vertex
// first where `provoking` names one. (A fan's and a polygon's provoking vertex differ: OpenGL's last-vertex convention
// takes a fan triangle's last, its first-vertex convention the triangle's second; a polygon's is always its first.)
void list_of(uint32_t primitive, std::span<const uint32_t> s, Provoking provoking, std::vector<uint32_t>& out) {
    const size_t m = s.size();
    const bool last = provoking == Provoking::last, first = provoking == Provoking::first;
    const auto tri = [&](uint32_t a, uint32_t b, uint32_t c) { out.insert(out.end(), {a, b, c}); };
    switch (primitive) {
    case 1: out.insert(out.end(), s.begin(), s.end()); break;
    case 2:
        for (size_t v = 0; v + 2 <= m; v += 2) last ? out.insert(out.end(), {s[v + 1], s[v]}) : out.insert(out.end(), {s[v], s[v + 1]});
        break;
    case 4:
        for (size_t v = 1; v < m; ++v) last ? out.insert(out.end(), {s[v], s[v - 1]}) : out.insert(out.end(), {s[v - 1], s[v]});
        break;
    case 5:
        for (size_t v = 0; v + 3 <= m; v += 3) last ? tri(s[v + 2], s[v], s[v + 1]) : tri(s[v], s[v + 1], s[v + 2]);
        break;
    case 6:  // triangle v: (v - 2, v - 1, v), every other one wound the other way
        for (size_t v = 2; v < m; ++v) {
            const bool even = v % 2 == 0;
            if (last) even ? tri(s[v], s[v - 2], s[v - 1]) : tri(s[v], s[v - 1], s[v - 2]);
            else if (first && !even) tri(s[v - 2], s[v], s[v - 1]);
            else even ? tri(s[v - 2], s[v - 1], s[v]) : tri(s[v - 1], s[v - 2], s[v]);
        }
        break;
    case 7:
        for (size_t v = 2; v < m; ++v) last ? tri(s[v], s[0], s[v - 1]) : first ? tri(s[v - 1], s[v], s[0]) : tri(s[0], s[v - 1], s[v]);
        break;
    case 10:
        for (size_t v = 2; v < m; ++v) tri(s[0], s[v - 1], s[v]);
        break;
    case 8:  // quads (a, b, c, d); the provoking vertex is the last, d, or the first, a
        for (size_t v = 0; v + 4 <= m; v += 4) {
            if (last) tri(s[v + 3], s[v], s[v + 1]), tri(s[v + 3], s[v + 1], s[v + 2]);
            else tri(s[v], s[v + 1], s[v + 2]), tri(s[v], s[v + 2], s[v + 3]);
        }
        break;
    case 9:  // quad strip: quads (v, v + 1, v + 3, v + 2); the provoking vertex is v + 3, or v
        for (size_t v = 0; v + 4 <= m; v += 2) {
            if (last) tri(s[v + 3], s[v + 2], s[v]), tri(s[v + 3], s[v], s[v + 1]);
            else tri(s[v], s[v + 1], s[v + 3]), tri(s[v], s[v + 3], s[v + 2]);
        }
        break;
    default: break;
    }
}

Topology list_topology(uint32_t primitive) { return primitive == 1 ? Topology::points : primitive == 2 || primitive == 4 ? Topology::lines : Topology::triangles; }

}  // namespace

std::expected<Assembly, std::string> assemble(uint32_t primitive, std::span<const uint32_t> order, bool given_indexed, Provoking provoking,
                                              std::vector<uint32_t>& indices) {
    indices.clear();
    if (primitive < 1 || primitive > 10 || primitive == 3) return std::unexpected(what("a draw of primitive %u, which the renderer doesn't take yet", primitive));
    const bool restart = given_indexed && std::ranges::find(order, kRestart) != order.end();  // (0, 1, 2... has none)
    if (!restart && provoking == Provoking::any) {
        switch (primitive) {
        case 1: return Assembly{Topology::points, given_indexed, false};
        case 2: return Assembly{Topology::lines, given_indexed, false};
        case 4: return Assembly{Topology::line_strip, given_indexed, false};
        case 5: return Assembly{Topology::triangles, given_indexed, false};
        case 6: return Assembly{Topology::triangle_strip, given_indexed, false};
        default: break;  // fans, quads, quad strips and polygons: made into triangles
        }
    }
    for (size_t a = 0; a <= order.size();) {
        size_t b = a;
        while (b < order.size() && order[b] != kRestart) ++b;
        list_of(primitive, order.subspan(a, b - a), provoking, indices);
        a = b + 1;
    }
    return Assembly{list_topology(primitive), true, true};
}

std::expected<Rasterisation, std::string> rasterisation(uint32_t primitive, const PolygonState& s) {
    if (primitive < 5) return Rasterisation{};  // points and lines: no polygon mode, no polygon offset
    const auto mode_of = [](uint32_t gl) -> std::optional<PolygonMode> {
        switch (gl) {
        case 0: case 0x1b02: return PolygonMode::fill;
        case 0x1b01: return PolygonMode::line;
        case 0x1b00: return PolygonMode::point;
        default: return std::nullopt;
        }
    };
    const std::optional<PolygonMode> front = mode_of(s.front_mode), back = mode_of(s.back_mode);
    if (!front) return std::unexpected(what("a front polygon mode of %#x", s.front_mode));
    if (!back) return std::unexpected(what("a back polygon mode of %#x", s.back_mode));
    if (s.cull && s.cull_face != 0x404 && s.cull_face != 0x405 && s.cull_face != 0x408) return std::unexpected(what("culling of face %#x", s.cull_face));
    const bool front_drawn = !(s.cull && (s.cull_face == 0x404 || s.cull_face == 0x408));
    const bool back_drawn = !(s.cull && (s.cull_face == 0x405 || s.cull_face == 0x408));
    if (front_drawn && back_drawn && *front != *back)
        return std::unexpected(std::string("polygons whose front and back faces are drawn in different modes, which Vulkan's one polygon mode can't"));
    const PolygonMode mode = front_drawn ? *front : back_drawn ? *back : PolygonMode::fill;  // (both culled: nothing is drawn)
    if (mode != PolygonMode::fill && primitive >= 8)
        return std::unexpected(what("primitive %u drawn as lines or points: made into triangles, it would show a diagonal's edge or points twice", primitive));
    if (mode == PolygonMode::line && !s.edge_flag) return std::unexpected(std::string("polygons drawn as lines with the edge flag off"));
    const bool bias = mode == PolygonMode::fill ? s.offset_fill : mode == PolygonMode::line ? s.offset_line : s.offset_point;
    return Rasterisation{mode, bias};
}

std::optional<uint32_t> logic_op(uint32_t gl) {
    if (gl < 0x1500 || gl > 0x150f) return std::nullopt;
    return gl - 0x1500;
}

WindowMapping window_mapping(const Draw& d) {
    WindowMapping w;
    const float width = static_cast<float>(d.color.width), height = static_cast<float>(d.color.height);
    const float ox = static_cast<float>(d.window_offset_x), oy = static_cast<float>(d.window_offset_y);
    w.push[0] = d.viewport_scale[0] * 2.0f / width;
    w.push[2] = (d.viewport_offset[0] + ox) * 2.0f / width - 1.0f;
    if (d.origin_bottom) {
        // The RSX's window y counts up from the bottom of a window `window_height` high; Vulkan's down from the top.
        w.push[1] = -d.viewport_scale[1] * 2.0f / height;
        w.push[3] = (static_cast<float>(d.window_height) - d.viewport_offset[1] + oy) * 2.0f / height - 1.0f;
    } else {
        w.push[1] = d.viewport_scale[1] * 2.0f / height;
        w.push[3] = (d.viewport_offset[1] + oy) * 2.0f / height - 1.0f;
    }
    w.push[4] = d.viewport_scale[2], w.push[5] = d.viewport_offset[2];
    // The scissor, in the same window coordinates, moved by the offset and clipped to the surface.
    int64_t x0 = d.scissor_x, y0 = d.scissor_y;
    if (d.origin_bottom) y0 = int64_t{d.window_height} - (int64_t{d.scissor_y} + d.scissor_height);
    x0 += d.window_offset_x, y0 += d.window_offset_y;
    const int64_t x1 = std::min<int64_t>(d.color.width, x0 + d.scissor_width), y1 = std::min<int64_t>(d.color.height, y0 + d.scissor_height);
    x0 = std::max<int64_t>(0, x0), y0 = std::max<int64_t>(0, y0);
    w.x0 = static_cast<uint32_t>(x0), w.y0 = static_cast<uint32_t>(y0);
    w.x1 = static_cast<uint32_t>(std::max(x0, x1)), w.y1 = static_cast<uint32_t>(std::max(y0, y1));
    return w;
}

}  // namespace rr7

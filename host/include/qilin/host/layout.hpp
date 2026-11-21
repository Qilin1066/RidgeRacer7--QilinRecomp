// Where the second picture goes in the window, and where a
// point in the window falls in a picture: plain arithmetic, apart from the
// display so it can be tested without a GPU.
#pragma once

#include <algorithm>

namespace qilin::host {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;  // window pixels
};

// A picture of `aspect` (width / height) in the window's bottom-right
// corner, `fraction` of the window's width wide (clamped to 0.05-0.5), and
// never more than half its height: then as tall as that allows. A margin of
// 1.5% of the window's smaller side keeps it off the edges.
inline Rect corner_inset(float window_w, float window_h, float fraction, float aspect) {
    fraction = std::clamp(fraction, 0.05f, 0.5f);
    float w = window_w * fraction, h = w / aspect;
    if (h > window_h * 0.5f) h = window_h * 0.5f, w = h * aspect;
    const float margin = std::min(window_w, window_h) * 0.015f;
    return {window_w - w - margin, window_h - h - margin, w, h};
}

// Where (px, py) falls in `r`: 0 to 1 across and down. False outside it (u
// and v are then left as they were).
inline bool point_in(const Rect& r, float px, float py, float& u, float& v) {
    if (r.w <= 0 || r.h <= 0 || px < r.x || py < r.y || px >= r.x + r.w || py >= r.y + r.h) return false;
    u = (px - r.x) / r.w;
    v = (py - r.y) / r.h;
    return true;
}

}  // namespace qilin::host

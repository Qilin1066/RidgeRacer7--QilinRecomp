// The second picture's place in the window, and points in a picture.
#include <cmath>
#include <cstdio>

#include "qilin/host/layout.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("layout_test: FAIL %s\n", what);
        ++failures;
    }
}
bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }
}  // namespace

int main() {
    using namespace qilin::host;
    const float gamepad = 854.0f / 480.0f;
    // 30% of a 1920x1080 window: 576 wide, 323.7 tall, 16.2 pixels from the edges.
    Rect r = corner_inset(1920, 1080, 0.3f, gamepad);
    check(near(r.w, 576) && near(r.h, 576 / gamepad), "a 30% inset's size");
    check(near(r.x + r.w, 1920 - 16.2f) && near(r.y + r.h, 1080 - 16.2f), "its corner and margin");
    // Half the width of a wide, low window would be more than half its height: limited by the height.
    r = corner_inset(800, 400, 0.5f, gamepad);  // 400 x 225 would be over 200 tall
    check(near(r.h, 200) && near(r.w, 200 * gamepad), "limited to half the height");
    // Out-of-range sizes are clamped.
    check(near(corner_inset(1000, 1000, 0.0f, gamepad).w, 50), "at least 5% wide");
    check(near(corner_inset(1000, 1000, 2.0f, gamepad).w, 500), "at most 50% wide");

    float u = -1, v = -1;
    const Rect p{100, 50, 200, 100};
    check(point_in(p, 100, 50, u, v) && near(u, 0) && near(v, 0), "the top-left corner is in");
    check(point_in(p, 200, 100, u, v) && near(u, 0.5f) && near(v, 0.5f), "the middle");
    u = v = -1;
    check(!point_in(p, 300, 100, u, v) && u == -1 && v == -1, "the right edge is out, and nothing written");
    check(!point_in(p, 99, 60, u, v), "left of it");
    check(!point_in(Rect{}, 0, 0, u, v), "an empty rectangle holds nothing");
    if (failures == 0) std::puts("layout_test: all passed");
    return failures ? 1 : 0;
}

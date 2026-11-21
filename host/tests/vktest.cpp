// The host Vulkan layer, headless: a frame presented into a 64x36 target at a
// 2:1 aspect must land letterboxed, with black bars above and below.
// Exit 77 (skipped) where there's no Vulkan 1.3 device.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "qilin/host/display.hpp"
#include "qilin/host/vulkan.hpp"

namespace {
bool vulkan_13_device() {
    if (volkInitialize() != VK_SUCCESS || volkGetInstanceVersion() < VK_API_VERSION_1_3) return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return false;
    volkLoadInstance(inst);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    bool ok = false;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        ok |= p.apiVersion >= VK_API_VERSION_1_3;
    }
    vkDestroyInstance(inst, nullptr);
    return ok;
}
}  // namespace

int main() {
    if (!vulkan_13_device()) {
        std::puts("vktest: no Vulkan 1.3 device here: skipped");
        return 77;
    }
    qilin::host::Display::Options o;
    o.headless = true;
    o.width = 64;
    o.height = 36;
    qilin::host::Display d(o);
    std::vector<uint8_t> frame(8 * 4 * 4);
    for (size_t i = 0; i < frame.size(); i += 4) frame[i] = 255, frame[i + 1] = 0, frame[i + 2] = 0, frame[i + 3] = 255;  // red
    d.present(8, 4, frame.data(), 2.0f, 4);  // 64x32 inside 64x36: bars of 2 rows
    const std::vector<uint8_t> px = d.read_back();
    auto at = [&](int x, int y) { return &px[static_cast<size_t>(y * 64 + x) * 4]; };
    int failures = 0;
    auto expect = [&](int x, int y, uint8_t r, const char* what) {
        if (at(x, y)[0] != r || at(x, y)[1] != 0 || at(x, y)[2] != 0) {
            std::printf("vktest: %s at (%d,%d) is %u,%u,%u\n", what, x, y, at(x, y)[0], at(x, y)[1], at(x, y)[2]);
            ++failures;
        }
    };
    expect(32, 0, 0, "the top bar");
    expect(32, 35, 0, "the bottom bar");
    expect(32, 18, 255, "the frame");
    expect(0, 2, 255, "the frame's corner");

    // Fill the window: no bars.
    qilin::host::Picture pic;
    pic.scaling = qilin::host::Scaling::stretch;
    d.set_picture(pic);
    d.present(8, 4, frame.data(), 2.0f, 4);
    {
        const std::vector<uint8_t> s = d.read_back();
        if (s[0] != 255 || s[(35 * 64 + 32) * 4] != 255) {
            std::puts("vktest: stretched, the picture doesn't reach the corners");
            ++failures;
        }
    }
    // Whole multiples: a 4-line, 2:1 picture is 8x4; 64x36 holds it 8 times (64x32), so it's
    // 64x32 again; with 6 lines (12x6), 5 times: 60x30, from (2,3).
    pic.scaling = qilin::host::Scaling::integer;
    d.set_picture(pic);
    d.present(8, 4, frame.data(), 2.0f, 6);
    {
        const std::vector<uint8_t> s = d.read_back();
        auto red = [&](int x, int y) { return s[static_cast<size_t>(y * 64 + x) * 4] == 255; };
        if (red(1, 18) || !red(2, 18) || !red(61, 18) || red(62, 18) || red(32, 2) || !red(32, 3) || !red(32, 32) || red(32, 33)) {
            std::puts("vktest: whole multiples: the picture isn't at 60x30 from (2,3)");
            ++failures;
        }
    }
    // A CRT: mid grey on 4 raster lines into 36 rows (9 a line, enough for the
    // gaps to show): a line's centre is bright, the gap between two dark.
    // (Bright lines spread to nearly fill their gaps, as a tube's do.)
    pic.scaling = qilin::host::Scaling::stretch;
    pic.look = qilin::host::Look::crt;
    pic.scanlines = 1.0f;
    pic.mask = 0.0f;
    d.set_picture(pic);
    std::vector<uint8_t> grey(8 * 4 * 4, 128);
    d.present(8, 4, grey.data(), 2.0f, 4);
    {
        const std::vector<uint8_t> s = d.read_back();
        auto g = [&](int y) { return int(s[static_cast<size_t>(y * 64 + 32) * 4 + 1]); };
        // 36 rows, 9 a line: centres at 4.5, 13.5 ...; the gap at 9.
        // crt.frag's beams worked out by hand for grey 128: 148 at a centre, 103 in the gap.
        if (std::abs(g(4) - 148) > 3 || std::abs(g(13) - 148) > 3 || std::abs(g(9) - 103) > 3) {
            std::printf("vktest: CRT: line centre %d, gap %d, next centre %d\n", g(4), g(9), g(13));
            ++failures;
        }
    }
    // Two pictures: red stretched over the window, green (the second,
    // 854:480) half its width in the bottom-right corner: 32x18 at (31,17),
    // the margin rounded away. Not drawn while the inset is 0.
    pic = {};
    pic.scaling = qilin::host::Scaling::stretch;
    d.set_picture(pic);
    std::vector<uint8_t> green(4 * 4 * 4);
    for (size_t i = 0; i < green.size(); i += 4) green[i] = 0, green[i + 1] = 255, green[i + 2] = 0, green[i + 3] = 255;
    const qilin::host::Display::Frame tv{8, 4, frame.data(), 2.0f, 4}, pad{4, 4, green.data(), 854.0f / 480.0f, 480};
    d.set_inset(0.5f);
    d.present_pair(tv, pad);
    for (int pass = 0; pass < 2; ++pass) {  // presented, then shown again (present_again)
        const std::vector<uint8_t> s = d.read_back();
        auto px2 = [&](int x, int y) { return &s[static_cast<size_t>(y * 64 + x) * 4]; };
        auto is = [&](int x, int y, uint8_t r, uint8_t g) { return px2(x, y)[0] == r && px2(x, y)[1] == g; };
        if (!is(10, 10, 255, 0) || !is(30, 30, 255, 0) || !is(31, 16, 255, 0) || !is(32, 18, 0, 255) || !is(62, 34, 0, 255)) {
            std::printf("vktest: the inset (pass %d): (10,10) %u,%u (30,30) %u,%u (31,16) %u,%u (32,18) %u,%u (62,34) %u,%u\n",
                        pass, px2(10, 10)[0], px2(10, 10)[1], px2(30, 30)[0], px2(30, 30)[1], px2(31, 16)[0], px2(31, 16)[1],
                        px2(32, 18)[0], px2(32, 18)[1], px2(62, 34)[0], px2(62, 34)[1]);
            ++failures;
        }
        d.present_again();
    }
    d.set_inset(0);
    d.present_pair(tv, pad);
    if (d.read_back()[(30 * 64 + 50) * 4 + 1] != 0) {
        std::puts("vktest: the inset is drawn while it's set to 0");
        ++failures;
    }
    std::printf("vktest: %s on %s\n", failures ? "FAILED" : "passed", d.device_name().c_str());
    return failures ? 1 : 0;
}

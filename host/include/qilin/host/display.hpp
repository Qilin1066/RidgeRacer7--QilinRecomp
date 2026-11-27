// The window and what's shown in it: SDL3 for the window and its events,
// Vulkan to present. Headless, it draws into an image of its own instead, for
// tests and for machines without a display.
//
// The picture is drawn into the window through a look (plain, or a CRT
// television), at a size the settings choose, and anything drawn over it
// (the settings menu) after. F11 or Alt+Enter switch borderless full screen.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "qilin/host/pad.hpp"
#include "qilin/host/frame_times.hpp"
#include "qilin/host/picture.hpp"
#include "qilin/host/vulkan.hpp"

namespace qilin::host {

class Display {
public:
    struct Options {
        const char* title = "Ridge Racer 7";
        uint32_t width = 1280, height = 720;  // the window's, or the headless target's, size
        bool headless = false;
        bool fullscreen = false;              // borderless, at the desktop's mode
        std::string monitor;                  // the display's name; empty or missing: the main one
        bool stick_as_dpad = true;            // the left stick also presses the D-pad's bits past half way
        Picture picture;
    };
    explicit Display(const Options& options);
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    // Handle the window's events. False once the player has closed it.
    bool poll();
    // The pad's buttons and the mouse's left button held now (PadButton bits),
    // as of the last poll.
    uint32_t buttons() const;
    // A key by its SDL name ("X", "Space", "Left Shift"): a code for key_down,
    // or -1 if there's no such key. Whether it's held, as of the last poll
    // (never, headless).
    int key_code(const char* name) const;
    bool key_down(int code) const;
    // The sticks, -1 to 1 each way (y up), dead zone removed.
    void right_stick(float& x, float& y) const;
    void left_stick(float& x, float& y) const;
    // The triggers, 0 (let go) to 1 (pulled all the way), as of the last poll.
    void triggers(float& left, float& right) const;
    // If the mouse moved over the window since the last poll: where, over the
    // picture last presented, -1 to 1 each way (y up; beyond 1 off the picture).
    bool mouse_moved(float& x, float& y) const;
    // The pad's gyroscope, as of the last poll: radians a second about its
    // own axes (x to its right, y up, z towards the player; anticlockwise
    // looking down an axis is positive), as SDL gives them. False (and 0s)
    // without a pad that has one.
    bool gyro(float& x, float& y, float& z) const;
    bool has_gyro() const;
    // How far the mouse moved over the window during the last poll (window
    // pixels, y down), and whether its right button is held.
    void mouse_delta(float& dx, float& dy) const;
    bool mouse_right() const;
    // The pointer hidden and held in the window, its movement still
    // reported (for games that steer with the mouse), or let go.
    void capture_mouse(bool on);
    // Rumble on the first pad, on or off.
    void rumble(bool on);
    // Show a frame of RGBA8 pixels, at `aspect` (displayed width / height);
    // `lines`: the console picture's height in its own pixels (whole
    // multiples and the CRT's raster go by it). Black around it.
    void present(uint32_t width, uint32_t height, const uint8_t* rgba, float aspect, uint32_t lines);
    // Show an image drawn on this display's GPU (in TRANSFER_SRC_OPTIMAL
    // layout, with sampled usage and a colour view, its work submitted to the
    // same queue before this call), the same way. It's left as it was given.
    void present_image(VkImage image, VkImageView view, uint32_t width, uint32_t height, float aspect, uint32_t lines);
    // Two pictures: the television's, placed as above, and a second (the
    // one) in the window's bottom-right corner over it, at the size
    // set_inset gives (not drawn while that is 0). Uploaded, or drawn on this
    // display's GPU (as present_image's).
    struct Frame {
        uint32_t width = 0, height = 0;
        const uint8_t* rgba = nullptr;
        float aspect = 16.0f / 9.0f;
        uint32_t lines = 0;
    };
    void present_pair(const Frame& main, const Frame& second);
    struct Drawn {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        float aspect = 16.0f / 9.0f;
        uint32_t lines = 0;
    };
    void present_images(const Drawn& main, const Drawn& second);
    // The second picture's width, as a fraction of the window's (clamped to
    // 0.05-0.5; 0: not shown), from the next present.
    void set_inset(float fraction);
    // Where the mouse is, as of the last poll, 0 to 1 across and down: on
    // the second picture as last drawn, or on the main one where the second
    // isn't over it. False when it isn't on that picture.
    bool mouse_on_inset(float& u, float& v) const;
    bool mouse_on_picture(float& u, float& v) const;
    // Show the last picture again: between a slow game's pictures, or for a
    // menu over a paused game. Not a new picture, so not counted in
    // frame_times() (the next new one counts the time since the last).
    void present_again();
    // The time until the next new picture isn't a frame time (a pause).
    void pause_frame_times();
    // The intervals between new pictures presented (present and
    // present_image), over the last 20 s (frame_times.hpp).
    const FrameTimes& frame_times() const;
    // Fullscreen's mode, when it isn't the desktop's (or why it couldn't be
    // even): for the log. Empty otherwise.
    const std::string& mode_note() const;
    // The refresh rate of the screen the window is on, in Hz (0 headless, or not known).
    double refresh_hz() const;
    // Since the last call: frames presented and the time spent waiting to
    // present them (for the previous one to reach the screen, and for a
    // swapchain image): with vsync, mostly the wait for the display.
    struct PresentStats {
        uint32_t presents = 0;
        double wait_ms = 0;  // in all
    };
    PresentStats take_present_stats();
    Gpu& gpu();
    // Headless: the last frame presented, as RGBA8 at the target's size.
    std::vector<uint8_t> read_back();
    const std::string& device_name() const;

    // The picture's settings, from the next present.
    void set_picture(const Picture& p);
    const Picture& picture() const;
    bool fullscreen() const;
    void set_fullscreen(bool on);
    // The displays connected, by name, and moving the window to one (empty: the main one).
    std::vector<std::string> monitors() const;
    void move_to(const std::string& monitor);

    // For a UI drawn over the picture: the SDL window (SDL_Window*), the
    // window images' format and count. Null and 0 when headless.
    void* sdl_window() const;
    VkFormat swap_format() const;
    uint32_t swap_image_count() const;
    // Called with every SDL event (an SDL_Event*) before the display's own
    // handling; true: taken, the display ignores it.
    void set_event_hook(std::function<bool(const void* sdl_event)> hook);
    // Called inside each present's drawing (dynamic rendering into the window
    // image, after the picture), to draw over it.
    void set_overlay(std::function<void(VkCommandBuffer)> overlay);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qilin::host

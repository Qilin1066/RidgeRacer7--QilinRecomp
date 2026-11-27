// The settings menu over the game, as the other Qilin recomps have it: F1 or
// Escape (a controller: Back and Start together, which alone are SELECT and
// START) open and close it. It has Controls, Display and Game pages, and
// Close and Quit top right; while it's open the game is paused (pause.hpp),
// and closing it saves the settings (settings.hpp). A hint says how to open
// it for the first seconds of a run. F3 shows the frame times.
//
// Controls are set by pressing them: Add on a DualShock 3 control's row, then
// the key, the pad's button or a click. The picture's settings show at once
// (Look at the picture hides the menu until any key or button); the graphics
// device and the console's settings wait for the next start.
//
// Dear ImGui (MIT) with its SDL3 backend for input and its Vulkan backend
// drawing over the picture (display.hpp's overlay, inside the window's
// presentation, which the main loop calls holding the GPU's queue).
// Everything here runs on the main thread. Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include "qilin/host/display.hpp"
#include "settings.hpp"

namespace rr7 {

class Menu {
public:
    // Without a window there is no menu (ok() is false); the game runs as before.
    // `queue`: the lock the GPU's queue is used under (main.cpp's).
    Menu(qilin::host::Display& display, Settings& settings, std::filesystem::path folder, std::mutex& queue);
    ~Menu();
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    bool ok() const { return ok_; }
    // Open and showing: the game is paused under it, and is given no input.
    bool is_open() const { return open_; }
    // Once per pass of the main loop, after the display's poll: opens once
    // the game's frames have stopped, builds the menu's (or the hint's, or
    // the frame times') drawing.
    void tick();
    bool quit_requested() const { return quit_; }
    // Whether the controls changed since the last call (the main loop's table of them is rebuilt).
    bool take_controls_changed();

private:
    bool handle(const void* sdl_event);
    void toggle();
    void close();
    void save();
    void draw_menu();
    void controls_page();
    void display_page();
    void game_page();
    void draw_hint();
    void draw_frame_times();
    void bind(const Binding& b);

    qilin::host::Display& display_;
    Settings& s_;
    std::filesystem::path folder_;
    std::mutex& queue_;
    bool ok_ = false, opening_ = false, open_ = false, quit_ = false, close_asked_ = false;
    bool sdl_backend_ = false, vulkan_backend_ = false;
    bool hint_ = false, drawn_ = false, peek_ = false, controls_changed_ = false;
    uint64_t hint_until_ = 0;  // SDL ticks (ms)
    uint64_t opening_since_ = 0;
    // Setting a control by pressing it.
    int capture_ = -1;           // the Control being set, or -1
    uint64_t capture_until_ = 0;
    uint32_t capture_held_ = 0;  // the pad's buttons held when it began (they don't count)
    bool wait_release_ = false;  // after a pad button was taken: gamepad navigation back once it's let go
    std::string note_;           // said under the table (what moved where)
};

}  // namespace rr7

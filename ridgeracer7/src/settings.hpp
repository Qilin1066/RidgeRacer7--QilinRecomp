// The settings file: ridgeracer7.ini in the game's data folder
// (host/paths.hpp), as the other Qilin recomps keep theirs: [display],
// [controls] and [console] sections, written with every key and its comments
// the first time the game runs. The settings menu (F1 or Esc in the game,
// menu.hpp) changes them and saves them back; the file can be edited by hand
// too. A line that can't be read stops the program, naming the line, rather
// than being ignored.
//
// Also here: the controller the game is given, built from the bindings
// (compose), and the console's own settings the game reads (sysutil.cpp).
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "machine.hpp"
#include "qilin/host/picture.hpp"

namespace rr7 {

// The DualShock 3's buttons, and its sticks' four directions as keys can
// press them (a controller's own sticks move the DualShock's sticks as they are).
enum Control : unsigned {
    kCtlCross, kCtlCircle, kCtlSquare, kCtlTriangle, kCtlL1, kCtlR1, kCtlL2, kCtlR2, kCtlL3, kCtlR3, kCtlStart, kCtlSelect,
    kCtlUp, kCtlDown, kCtlLeft, kCtlRight,
    kCtlLeftUp, kCtlLeftDown, kCtlLeftLeft, kCtlLeftRight,
    kCtlRightUp, kCtlRightDown, kCtlRightLeft, kCtlRightRight,
    kControlCount
};
inline constexpr unsigned kButtonCount = kCtlLeftUp;  // the controls before the stick directions
const char* control_name(Control c);   // as the file names it: "l2"
const char* control_label(Control c);  // as the menu shows it: "L2"

// One input that presses a control.
struct Binding {
    enum Kind { kPad, kKey, kMouse } kind = kPad;
    uint32_t pad = 0;  // kPad: a qilin::host::PadButton bit
    std::string key;   // kKey: the key's name, as SDL names keys ("X", "Space", "Left")
    bool operator==(const Binding&) const = default;
};
// As the file writes it ("pad:A", "key:Space", "mouse"), and as the menu
// shows it ("Pad A", "Space", "Mouse").
std::string binding_text(const Binding& b);
std::string binding_label(const Binding& b);
// The pad buttons that can be bound, by the file's names (null if it isn't one).
const char* pad_button_name(uint32_t bit);
// Keys kept for the program: Escape and F1 (the menu), F3 (frame times), F11 (full screen).
bool key_reserved(const std::string& name);

// The console's own settings, as the game reads them (cellSysutilGetSystemParamInt and
// ...String, sysutil.cpp), in the SDK's numbers.
struct ConsoleSettings {
    int32_t language = 1;      // CELL_SYSUTIL_LANG_: 1 English (US), 0 Japanese
    int32_t enter_button = 1;  // which button confirms: 1 cross, 0 circle (a Japanese console's)
    int32_t time_zone = 0;     // minutes from UTC
    std::string nickname = "PS3";
    std::string user_name = "User";
};
// Given to the system before the game starts (sysutil.cpp: it reads them at its start).
void set_console_settings(const ConsoleSettings& c);
const ConsoleSettings& console_settings();

struct Settings {
    // [display]
    bool fullscreen = false;          // borderless, at the desktop's mode (F11 or Alt+Enter switch it)
    std::string monitor;              // the display's name (empty: the main one)
    qilin::host::Scaling scaling = qilin::host::Scaling::fit;
    bool smooth = true;               // bilinear; off: nearest pixel
    qilin::host::Look look = qilin::host::Look::plain;
    unsigned scanlines = 60;          // CRT: the gaps between raster lines, 0-100 (percent dark)
    unsigned mask = 30;               // CRT: the phosphor stripes, 0-100
    bool vsync = true;                // at the display's refresh (no tearing); off: the lowest latency
    bool match_refresh = true;        // fullscreen: a refresh that's a multiple of 60 Hz when the desktop's isn't
    bool variable_refresh = false;    // a G-Sync/FreeSync/VRR display: no mode switch, no locking to the refresh
    std::string gpu;                  // the graphics device's name (empty: the best one); from the next start
    bool performance_overlay = false; // frame rate, lows and frame times in a corner (F3 switches it)
    // [controls]
    std::array<std::vector<Binding>, kControlCount> controls;
    // [console]: what the game reads as the console's own settings (from the next start)
    std::string language = "english";  // english or japanese (this disc's two)
    std::string enter_button = "cross";  // cross or circle
    std::string nickname = "PS3";
    std::string user_name = "User";
    int time_zone = 0;  // minutes from UTC, -720 to 840
};

Settings default_settings();
// The picture as the display takes it.
qilin::host::Picture picture_of(const Settings& s);
// The console's settings in the SDK's numbers.
ConsoleSettings console_of(const Settings& s);

// Read settings from text. On a problem: false, with `error` saying what
// and where ("line 12: ...").
bool parse_settings(const std::string& text, Settings& out, std::string& error);
// The file's text for the given settings, with comments explaining each part.
std::string settings_text(const Settings& s);

// ridgeracer7.ini in `folder`: read it, or write the defaults there if it's
// missing. A file that doesn't read stops the program.
Settings load_settings(const std::filesystem::path& folder);
// Write it back (through a temporary file, so a failure leaves the old one);
// false if it can't be written.
bool save_settings(const std::filesystem::path& folder, const Settings& s);
std::filesystem::path settings_file(const std::filesystem::path& folder);

// ---- The controller, from the bindings ----

// The bindings made ready for each poll: pad buttons as one mask a control,
// keys as the window's codes. `key_code` gives a key's code by its name, or
// -1 for a name the window system doesn't know: those are listed in `unknown`.
struct ControlTable {
    struct Entry {
        uint32_t pad = 0;        // host::PadButton bits
        bool mouse = false;      // the mouse's left button
        std::vector<int> keys;   // key codes
    };
    std::array<Entry, kControlCount> entries;
};
ControlTable control_table(const Settings& s, const std::function<int(const std::string&)>& key_code,
                           std::vector<std::string>& unknown);

// What the host has, as of one poll.
struct HostInput {
    uint32_t buttons = 0;                  // host::PadButton bits (and kPadClick)
    std::function<bool(int)> key_down;     // by code; may be empty (no keyboard)
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;  // -1 to 1, y up
    float left_trigger = 0, right_trigger = 0;               // 0 to 1
};
// The DualShock 3 as the game reads it (pad.cpp's PadState). A control bound
// to a trigger (pad:LT, pad:RT) is held from a sixteenth of its pull and
// takes its pressure from it, so that throttle and brake can be held part
// way; any other input presses its button all the way. A key bound to a
// stick's direction pushes that stick all the way.
PadState compose(const ControlTable& t, const HostInput& in);

}  // namespace rr7

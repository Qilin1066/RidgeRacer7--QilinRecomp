// The settings file (settings.hpp): what's written reads back the same, a line that can't be read is refused with its
// number, the controller is built from the bindings as the console reports it, and the pause (pause.hpp) holds a thread
// and lets it go. No game and no window.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <thread>

#include "pause.hpp"
#include "qilin/host/pad.hpp"
#include "settings.hpp"

using namespace rr7;
namespace host = qilin::host;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "settings_test: %s\n", what);
    ++failures;
}

bool refused(const std::string& text, const char* line) {
    Settings s;
    std::string error;
    return !parse_settings(text, s, error) && error.starts_with(line);
}

// Key codes for the test: a name's place in this list, or -1.
constexpr const char* kKeys[] = {"Z", "X", "A", "S", "Q", "W", "1", "2", "C", "V", "Return", "Backspace", "Up", "Down", "Left", "Right",
                                 "I", "K", "J", "L", "Keypad 8", "Keypad 2", "Keypad 4", "Keypad 6", "Left Shift"};
int code_of(const std::string& name) {
    for (int i = 0; i < int(std::size(kKeys)); ++i)
        if (name == kKeys[i]) return i;
    return -1;
}
ControlTable table(const Settings& s) {
    std::vector<std::string> unknown;
    ControlTable t = control_table(s, code_of, unknown);
    check(unknown.empty(), "every default key has a code");
    return t;
}
HostInput keys(std::initializer_list<const char*> held) {
    HostInput in;
    std::vector<int> codes;
    for (const char* h : held) codes.push_back(code_of(h));
    in.key_down = [codes](int code) {
        for (const int c : codes)
            if (c == code) return true;
        return false;
    };
    return in;
}

}  // namespace

int main() {
    // ---- The file ----
    const Settings d = default_settings();
    const std::string text = settings_text(d);
    Settings back;
    std::string error;
    check(parse_settings(text, back, error), ("the defaults don't read back: " + error).c_str());
    check(settings_text(back) == text, "the defaults read back differently");
    for (unsigned c = 0; c < kControlCount; ++c) check(!d.controls[c].empty(), "a control with no default");
    check(parse_settings("", back, error) && settings_text(back) == text, "an empty file is the defaults");

    Settings changed = d;
    changed.fullscreen = true;
    changed.monitor = "DELL U2720Q";
    changed.scaling = host::Scaling::integer;
    changed.smooth = false;
    changed.look = host::Look::crt;
    changed.scanlines = 45, changed.mask = 0;
    changed.vsync = false, changed.match_refresh = false, changed.variable_refresh = true;
    changed.gpu = "NVIDIA GeForce RTX 4070";
    changed.performance_overlay = true;
    changed.language = "japanese";
    changed.enter_button = "circle";
    changed.nickname = "Racing PS3";
    changed.user_name = "Reiko";
    changed.time_zone = -300;
    changed.controls[kCtlCross] = {{Binding::kMouse, 0, {}}, {Binding::kKey, 0, "Left Shift"}};
    changed.controls[kCtlR3].clear();
    check(parse_settings(settings_text(changed), back, error), ("changed settings don't read back: " + error).c_str());
    check(settings_text(back) == settings_text(changed), "changed settings read back differently");
    check(back.fullscreen && back.monitor == "DELL U2720Q" && back.scaling == host::Scaling::integer && !back.smooth &&
              back.look == host::Look::crt && back.scanlines == 45 && back.mask == 0,
          "the window and the picture");
    check(!back.vsync && !back.match_refresh && back.variable_refresh && back.gpu == "NVIDIA GeForce RTX 4070" && back.performance_overlay,
          "vsync, the refresh, the device, the frame times");
    check(back.controls[kCtlR3].empty() && back.controls[kCtlCross].size() == 2, "the controls");
    const ConsoleSettings console = console_of(back);
    check(console.language == 0 && console.enter_button == 0 && console.time_zone == -300 && console.nickname == "Racing PS3" &&
              console.user_name == "Reiko",
          "the console's settings in the SDK's numbers (Japanese 0, circle 0)");
    const ConsoleSettings us = console_of(d);
    check(us.language == 1 && us.enter_button == 1 && us.time_zone == 0, "by default, a US console: English (US) 1, cross 1");
    set_console_settings(console);
    check(console_settings().nickname == "Racing PS3", "the console's settings, given to the system");
    {
        const host::Picture p = picture_of(back);
        check(p.scaling == host::Scaling::integer && !p.smooth && p.look == host::Look::crt && p.scanlines == 0.45f && p.mask == 0.0f &&
                  !p.vsync && !p.match_refresh && p.variable_refresh,
              "the picture as the display takes it");
    }
    {
        Settings t;
        check(parse_settings("[console]\ntime_zone = +540\n", t, error) && t.time_zone == 540, "a time zone with a plus");
        check(parse_settings("[controls]\ncross = PAD:a , KEY:Space,\n", t, error) && t.controls[kCtlCross].size() == 2 &&
                  t.controls[kCtlCross][0] == Binding{Binding::kPad, host::kPadA, {}} && t.controls[kCtlCross][1].key == "Space",
              "inputs in any case, with spaces and a trailing comma");
        check(parse_settings("[controls]\nstart =\n", t, error) && t.controls[kCtlStart].empty(), "an empty list: unused");
        check(parse_settings("[DISPLAY] # a comment\nVsync = FALSE\n", t, error) && !t.vsync, "names and values in any case, comments");
    }
    check(refused("vsync = true\n", "line 1:"), "a key before any section");
    check(refused("[sound]\n", "line 1:"), "a section there isn't");
    check(refused("[display]\nvsync = yes\n", "line 2:"), "a switch that isn't true or false");
    check(refused("[display]\n\nscanlines = 101\n", "line 3:"), "scanlines over 100");
    check(refused("[display]\nscaling = zoom\n", "line 2:"), "a scaling there isn't");
    check(refused("[display]\ninternal_scale = 2\n", "line 2:"), "an enhancement the renderer can't do yet");
    check(refused("[console]\nlanguage = french\n", "line 2:"), "a language this disc doesn't have");
    check(refused("[console]\nenter_button = square\n", "line 2:"), "a confirming button that isn't cross or circle");
    check(refused("[console]\ntime_zone = 900\n", "line 2:"), "a time zone past +14 hours");
    check(refused("[console]\ntime_zone = -12x\n", "line 2:"), "a time zone that isn't a number");
    check(refused("[console]\nnickname =\n", "line 2:"), "an empty nickname");
    check(refused("[console]\nuser_name = " + std::string(64, 'a') + "\n", "line 2:"), "a user's name over 63 characters");
    check(refused("[controls]\n\ncross = key:Escape\n", "line 3:"), "a key the program keeps");
    check(refused("[controls]\ncross = key:F11\n", "line 2:"), "full screen's key");
    check(refused("[controls]\nhome = pad:Guide\n", "line 2:"), "a control the DualShock 3 doesn't have");
    check(refused("[controls]\ncross = pad:Z\n", "line 2:"), "a pad button there isn't");
    check(refused("[controls]\ncross = joystick:1\n", "line 2:"), "a kind of input there isn't");
    check(refused("[controls]\ncross = pad:A\ncross = pad:B\n", "line 3:"), "a control set twice");
    check(refused("[controls]\ncross = pad:A, pad:A\n", "line 2:"), "an input given to one control twice");

    // Saved and loaded through a file (in a folder of the test's own).
    {
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "ridgeracer7_settings_test";
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        std::filesystem::create_directories(folder, ec);
        check(!ec, "the test's folder");
        const Settings first = load_settings(folder);  // missing: the defaults, written
        check(settings_text(first) == text && std::filesystem::exists(settings_file(folder)), "a missing file is written with the defaults");
        check(settings_file(folder).filename() == "ridgeracer7.ini", "the file's name");
        check(save_settings(folder, changed), "saved");
        check(settings_text(load_settings(folder)) == settings_text(changed), "loaded as saved");
        check(!std::filesystem::exists(settings_file(folder).string() + ".tmp"), "no temporary file left");
        std::filesystem::remove_all(folder, ec);
    }

    // ---- The controller from the bindings ----
    {
        const ControlTable t = table(d);
        HostInput in = keys({});
        PadState p = compose(t, in);
        check(p.digital1 == 0 && p.digital2 == 0 && p.left_x == 128 && p.left_y == 128 && p.right_x == 128 && p.right_y == 128,
              "nothing held: no buttons, the sticks in the middle");
        in.buttons = host::kPadA | host::kPadStart | host::kPadLeft;
        p = compose(t, in);
        check(p.digital2 == 0x40 && p.digital1 == (0x08 | 0x80), "pad A is cross, Start START, Left the D-pad's left");
        check(p.pressure[6] == 255 && p.pressure[1] == 255 && p.pressure[5] == 0, "a button's pressure all the way");
        in = keys({"Z", "Q", "Backspace", "Up"});
        p = compose(t, in);
        check(p.digital2 == (0x40 | 0x04) && p.digital1 == (0x01 | 0x10) && p.pressure[8] == 255 && p.pressure[2] == 255,
              "keys: Z cross, Q L1, Backspace SELECT, Up the D-pad");
        // The triggers: held from a sixteenth of their pull, with it as their pressure; the digital bit alone is not it.
        in = keys({});
        in.right_trigger = 0.5f, in.left_trigger = 0.05f;
        p = compose(t, in);
        check(p.digital2 == 0x02 && p.pressure[11] == 128 && p.pressure[10] == 0, "RT half way: R2 at half pressure; LT under a sixteenth");
        in.buttons = host::kPadLT;
        in.left_trigger = 0.04f;
        p = compose(t, in);
        check(!(p.digital2 & 0x01), "the trigger's past-half-way bit doesn't press L2 on its own");
        in = keys({"1"});
        in.left_trigger = 0.25f;
        p = compose(t, in);
        check((p.digital2 & 0x01) && p.pressure[10] == 255, "a key on L2 presses it all the way, whatever the trigger");
        // The sticks: the controller's as they are, y down; keys push them all the way.
        in = keys({});
        in.left_x = -1.0f, in.left_y = 1.0f, in.right_x = 0.5f;
        p = compose(t, in);
        check(p.left_x == 0 && p.left_y == 0 && p.right_x == 191 && p.right_y == 128, "the controller's sticks");
        in = keys({"L", "K", "Keypad 4"});
        p = compose(t, in);
        check(p.left_x == 255 && p.left_y == 255 && p.right_x == 0 && p.right_y == 128 && p.digital1 == 0 && p.digital2 == 0,
              "keys for the sticks, pressing no button");
        // The mouse, when bound; and the changed layout.
        const ControlTable t2 = table(changed);
        in = keys({"Left Shift"});
        check(compose(t2, in).digital2 == 0x40, "Left Shift is cross now");
        in = keys({});
        in.buttons = host::kPadClick;
        check(compose(t2, in).digital2 == 0x40 && compose(t, in).digital2 == 0, "the mouse presses cross where it's bound, and nothing by default");
        in.buttons = host::kPadR3;
        check(compose(t2, in).digital1 == 0 && compose(t, in).digital1 == 0x04, "R3 unbound, and by default pad R3");
        // A key the window system doesn't know is reported, by name.
        Settings odd = d;
        odd.controls[kCtlSquare].push_back({Binding::kKey, 0, "Hyper"});
        std::vector<std::string> unknown;
        control_table(odd, code_of, unknown);
        check(unknown.size() == 1 && unknown[0] == "Hyper", "an unknown key's name");
    }

    // ---- The pause ----
    {
        check(!pause_requested() && !hold_while_paused(HoldPoint::flip), "no pause: a hold point goes straight on");
        request_pause(true);
        std::atomic<bool> through{false}, waited{false};
        std::thread flip([&] {
            waited = hold_while_paused(HoldPoint::flip);
            through = true;
        });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!held_at(HoldPoint::flip) && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(held_at(HoldPoint::flip) && frames_held() && !held_at(HoldPoint::sound), "the flip is held, and only it");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(!through, "held while the pause lasts");
        request_pause(false);
        flip.join();
        check(through && waited && !frames_held(), "let go when it ends, saying it waited");
    }

    if (failures) return 1;
    std::puts("settings_test: all passed");
    return 0;
}

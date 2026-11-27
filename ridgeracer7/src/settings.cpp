// The settings file and the controller from its bindings (settings.hpp). Runtime code: no std::format.
#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

#include "qilin/host/pad.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {
namespace host = qilin::host;

namespace {

constexpr const char* kControlNames[kControlCount] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "start", "select",
    "up", "down", "left", "right",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right"};
constexpr const char* kControlLabels[kControlCount] = {
    "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3 (left stick, pressed)", "R3 (right stick, pressed)",
    "START", "SELECT", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "Left stick up", "Left stick down", "Left stick left", "Left stick right",
    "Right stick up", "Right stick down", "Right stick left", "Right stick right"};

// Each button as the console reports it (machine.hpp's PadState): which word, its bit, its place among the pressures (or -1).
struct Ds3Button {
    bool second;
    uint16_t bit;
    int pressure;
};
constexpr Ds3Button kDs3[kButtonCount] = {
    {true, 0x40, 6},  {true, 0x20, 5},  {true, 0x80, 7},  {true, 0x10, 4},   // cross circle square triangle
    {true, 0x04, 8},  {true, 0x08, 9},  {true, 0x01, 10}, {true, 0x02, 11},  // L1 R1 L2 R2
    {false, 0x02, -1}, {false, 0x04, -1}, {false, 0x08, -1}, {false, 0x01, -1},  // L3 R3 start select
    {false, 0x10, 2}, {false, 0x40, 3}, {false, 0x80, 1}, {false, 0x20, 0},  // up down left right
};

struct PadName {
    const char* name;   // in the file
    const char* label;  // in the menu
    uint32_t bit;
};
constexpr PadName kPadNames[] = {
    {"Up", "Pad Up", host::kPadUp},       {"Down", "Pad Down", host::kPadDown},  {"Left", "Pad Left", host::kPadLeft},
    {"Right", "Pad Right", host::kPadRight}, {"A", "Pad A", host::kPadA},           {"B", "Pad B", host::kPadB},
    {"X", "Pad X", host::kPadX},          {"Y", "Pad Y", host::kPadY},           {"Start", "Pad Start", host::kPadStart},
    {"Back", "Pad Back", host::kPadBack}, {"Guide", "Pad Guide", host::kPadGuide},
    {"LB", "Pad LB", host::kPadLB},       {"RB", "Pad RB", host::kPadRB},        {"LT", "Pad LT", host::kPadLT},
    {"RT", "Pad RT", host::kPadRT},       {"L3", "Pad L3", host::kPadL3},        {"R3", "Pad R3", host::kPadR3},
};

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool parse_binding(const std::string& text, Binding& b, std::string& why) {
    const std::string t = trim(text);
    if (lower(t) == "mouse") {
        b = {Binding::kMouse, 0, {}};
        return true;
    }
    const size_t colon = t.find(':');
    if (colon == std::string::npos) {
        why = "'" + t + "' isn't an input: write pad:<button>, key:<key> or mouse";
        return false;
    }
    const std::string kind = lower(trim(t.substr(0, colon))), name = trim(t.substr(colon + 1));
    if (kind == "pad") {
        for (const PadName& p : kPadNames)
            if (lower(name) == lower(p.name)) {
                b = {Binding::kPad, p.bit, {}};
                return true;
            }
        why = "'" + name + "' isn't a pad button (Up Down Left Right A B X Y Start Back Guide LB RB LT RT L3 R3)";
        return false;
    }
    if (kind == "key") {
        if (name.empty()) {
            why = "key: needs a key's name";
            return false;
        }
        if (key_reserved(name)) {
            why = "key:" + name + " is kept for the program (Escape and F1 open the settings menu, F3 the frame times, F11 is full screen)";
            return false;
        }
        b = {Binding::kKey, 0, name};
        return true;
    }
    why = "'" + kind + "' isn't a kind of input (pad, key, mouse)";
    return false;
}

Binding pad(uint32_t bit) { return {Binding::kPad, bit, {}}; }
Binding key(const char* name) { return {Binding::kKey, 0, name}; }

const char* scaling_name(host::Scaling s) {
    switch (s) {
        case host::Scaling::integer: return "integer";
        case host::Scaling::stretch: return "stretch";
        case host::Scaling::fit: break;
    }
    return "fit";
}

// A name for the console: printable ASCII, 1 to `most` characters (the system's own limits, less its zero), with no #
// (the file's comments) and no spaces at its ends (trimmed when read), so that it reads back as written.
bool console_name(const std::string& value, size_t most) {
    if (value.empty() || value.size() > most || value.front() == ' ' || value.back() == ' ') return false;
    return std::all_of(value.begin(), value.end(), [](char ch) { return ch >= 0x20 && ch < 0x7f && ch != '#'; });
}

ConsoleSettings g_console;

}  // namespace

const char* control_name(Control c) { return kControlNames[c]; }
const char* control_label(Control c) { return kControlLabels[c]; }

const char* pad_button_name(uint32_t bit) {
    for (const PadName& p : kPadNames)
        if (p.bit == bit) return p.name;
    return nullptr;
}

bool key_reserved(const std::string& name) {
    const std::string l = lower(trim(name));
    return l == "escape" || l == "f1" || l == "f3" || l == "f11";
}

std::string binding_text(const Binding& b) {
    switch (b.kind) {
        case Binding::kMouse: return "mouse";
        case Binding::kKey: return "key:" + b.key;
        case Binding::kPad:
            if (const char* n = pad_button_name(b.pad)) return std::string("pad:") + n;
    }
    return "?";
}

std::string binding_label(const Binding& b) {
    switch (b.kind) {
        case Binding::kMouse: return "Mouse";
        case Binding::kKey: return b.key;
        case Binding::kPad:
            for (const PadName& p : kPadNames)
                if (p.bit == b.pad) return p.label;
    }
    return "?";
}

// The DualShock 3's face buttons where they sit: cross at the bottom, circle on the right, square on the left, triangle at
// the top, which on an Xbox-style pad are A, B, X and Y. The keyboard's layout is a stated choice (the one main.cpp had
// before the settings): arrows the D-pad, Z cross, X circle, A square, S triangle, Q L1, W R1, 1 L2, 2 R2, Return START,
// Backspace SELECT; C and V the sticks pressed, J L I K the left stick, the keypad's 4 6 8 2 the right.
Settings default_settings() {
    Settings s;
    s.controls[kCtlCross] = {pad(host::kPadA), key("Z")};
    s.controls[kCtlCircle] = {pad(host::kPadB), key("X")};
    s.controls[kCtlSquare] = {pad(host::kPadX), key("A")};
    s.controls[kCtlTriangle] = {pad(host::kPadY), key("S")};
    s.controls[kCtlL1] = {pad(host::kPadLB), key("Q")};
    s.controls[kCtlR1] = {pad(host::kPadRB), key("W")};
    s.controls[kCtlL2] = {pad(host::kPadLT), key("1")};
    s.controls[kCtlR2] = {pad(host::kPadRT), key("2")};
    s.controls[kCtlL3] = {pad(host::kPadL3), key("C")};
    s.controls[kCtlR3] = {pad(host::kPadR3), key("V")};
    s.controls[kCtlStart] = {pad(host::kPadStart), key("Return")};
    s.controls[kCtlSelect] = {pad(host::kPadBack), key("Backspace")};
    s.controls[kCtlUp] = {pad(host::kPadUp), key("Up")};
    s.controls[kCtlDown] = {pad(host::kPadDown), key("Down")};
    s.controls[kCtlLeft] = {pad(host::kPadLeft), key("Left")};
    s.controls[kCtlRight] = {pad(host::kPadRight), key("Right")};
    s.controls[kCtlLeftUp] = {key("I")};
    s.controls[kCtlLeftDown] = {key("K")};
    s.controls[kCtlLeftLeft] = {key("J")};
    s.controls[kCtlLeftRight] = {key("L")};
    s.controls[kCtlRightUp] = {key("Keypad 8")};
    s.controls[kCtlRightDown] = {key("Keypad 2")};
    s.controls[kCtlRightLeft] = {key("Keypad 4")};
    s.controls[kCtlRightRight] = {key("Keypad 6")};
    return s;
}

host::Picture picture_of(const Settings& s) {
    host::Picture p;
    p.scaling = s.scaling;
    p.smooth = s.smooth;
    p.look = s.look;
    p.scanlines = float(s.scanlines) / 100.0f;
    p.mask = float(s.mask) / 100.0f;
    p.vsync = s.vsync;
    p.match_refresh = s.match_refresh;
    p.variable_refresh = s.variable_refresh;
    return p;
}

ConsoleSettings console_of(const Settings& s) {
    ConsoleSettings c;
    c.language = s.language == "japanese" ? 0 : 1;
    c.enter_button = s.enter_button == "circle" ? 0 : 1;
    c.time_zone = s.time_zone;
    c.nickname = s.nickname;
    c.user_name = s.user_name;
    return c;
}

void set_console_settings(const ConsoleSettings& c) { g_console = c; }
const ConsoleSettings& console_settings() { return g_console; }

bool parse_settings(const std::string& text, Settings& out, std::string& error) {
    Settings s = default_settings();
    std::istringstream in(text);
    std::string line, section;
    bool seen[kControlCount] = {};
    for (int n = 1; std::getline(in, line); ++n) {
        const auto fail = [&](const std::string& why) {
            error = "line " + std::to_string(n) + ": " + why;
            return false;
        };
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') return fail("a section is written [name]");
            section = lower(trim(line.substr(1, line.size() - 2)));
            if (section != "display" && section != "controls" && section != "console")
                return fail("there's no section [" + section + "] (display, controls, console)");
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("expected name = value");
        const std::string name = lower(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));
        if (section.empty()) return fail("'" + name + "' comes before any [section]");
        const std::string v = lower(value);
        const auto boolean = [&](bool& to) {
            if (v == "true") to = true;
            else if (v == "false") to = false;
            else return fail(name + " is true or false");
            return true;
        };
        const auto number = [&](unsigned& to, unsigned lo, unsigned hi) {
            unsigned x = 0;
            bool ok = !v.empty() && v.size() <= 4;
            for (char ch : v) ok = ok && std::isdigit(static_cast<unsigned char>(ch)), x = x * 10 + unsigned(ch - '0');
            if (!ok || x < lo || x > hi) return fail(name + " is a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
            to = x;
            return true;
        };
        if (section == "display") {
            if (name == "fullscreen") {
                if (!boolean(s.fullscreen)) return false;
            } else if (name == "monitor") {
                s.monitor = value;
            } else if (name == "scaling") {
                if (v == "integer") s.scaling = host::Scaling::integer;
                else if (v == "fit") s.scaling = host::Scaling::fit;
                else if (v == "stretch") s.scaling = host::Scaling::stretch;
                else return fail("scaling is integer, fit or stretch");
            } else if (name == "smooth") {
                if (!boolean(s.smooth)) return false;
            } else if (name == "look") {
                if (v == "plain") s.look = host::Look::plain;
                else if (v == "crt") s.look = host::Look::crt;
                else return fail("look is plain or crt");
            } else if (name == "scanlines") {
                if (!number(s.scanlines, 0, 100)) return false;
            } else if (name == "mask") {
                if (!number(s.mask, 0, 100)) return false;
            } else if (name == "vsync") {
                if (!boolean(s.vsync)) return false;
            } else if (name == "match_refresh") {
                if (!boolean(s.match_refresh)) return false;
            } else if (name == "variable_refresh") {
                if (!boolean(s.variable_refresh)) return false;
            } else if (name == "gpu") {
                s.gpu = value;
            } else if (name == "performance_overlay") {
                if (!boolean(s.performance_overlay)) return false;
            } else {
                return fail("[display] has no '" + name +
                            "' (fullscreen monitor scaling smooth look scanlines mask vsync match_refresh variable_refresh gpu "
                            "performance_overlay)");
            }
            continue;
        }
        if (section == "console") {
            if (name == "language") {
                if (v != "english" && v != "japanese") return fail("language is english or japanese (this disc's two)");
                s.language = v;
            } else if (name == "enter_button") {
                if (v != "cross" && v != "circle") return fail("enter_button is cross or circle");
                s.enter_button = v;
            } else if (name == "nickname") {
                if (!console_name(value, 127)) return fail("nickname is 1 to 127 printable ASCII characters, no #");
                s.nickname = value;
            } else if (name == "user_name") {
                if (!console_name(value, 63)) return fail("user_name is 1 to 63 printable ASCII characters, no #");
                s.user_name = value;
            } else if (name == "time_zone") {
                const bool minus = !v.empty() && v.front() == '-';
                unsigned x = 0;
                const std::string digits = v.substr(!v.empty() && (v.front() == '-' || v.front() == '+') ? 1 : 0);
                bool ok = !digits.empty() && digits.size() <= 3;
                for (char ch : digits) ok = ok && std::isdigit(static_cast<unsigned char>(ch)), x = x * 10 + unsigned(ch - '0');
                const int minutes = minus ? -int(x) : int(x);
                if (!ok || minutes < -720 || minutes > 840) return fail("time_zone is minutes from UTC, -720 to 840 (-300: New York)");
                s.time_zone = minutes;
            } else {
                return fail("[console] has no '" + name + "' (language enter_button nickname user_name time_zone)");
            }
            continue;
        }
        // [controls]
        unsigned c = 0;
        while (c < kControlCount && name != kControlNames[c]) ++c;
        if (c == kControlCount)
            return fail("'" + name + "' isn't one of the DualShock 3's controls (cross circle square triangle l1 r1 l2 r2 l3 r3 "
                        "start select up down left right, left_stick_up/down/left/right, right_stick_up/down/left/right)");
        if (seen[c]) return fail("'" + name + "' is set twice");
        seen[c] = true;
        s.controls[c].clear();
        std::istringstream items(value);
        for (std::string item; std::getline(items, item, ',');) {
            if (trim(item).empty()) continue;
            Binding b;
            std::string why;
            if (!parse_binding(item, b, why)) return fail(why);
            if (std::find(s.controls[c].begin(), s.controls[c].end(), b) != s.controls[c].end())
                return fail(binding_text(b) + " is given to '" + name + "' twice");
            s.controls[c].push_back(b);
        }
    }
    out = std::move(s);
    return true;
}

std::string settings_text(const Settings& s) {
    const auto b = [](bool v) { return v ? "true" : "false"; };
    std::string t =
        "# The Ridge Racer 7 QilinRecomp settings. The settings menu (F1 or Esc in the game) changes these and\n"
        "# saves them here; any text editor works too. Delete the file to get the defaults back.\n"
        "# Each line is name = value; # starts a comment.\n"
        "\n"
        "[display]\n"
        "# Borderless full screen (F11 or Alt+Enter switch it), and on which display (its name; empty: the main one).\n"
        "fullscreen = " + std::string(b(s.fullscreen)) + "\n"
        "monitor = " + s.monitor + "\n"
        "# The picture's size: integer (whole multiples of the game's, the sharpest), fit (the largest that\n"
        "# keeps its shape) or stretch (the whole window). smooth: filtered; false: nearest pixel.\n"
        "scaling = " + scaling_name(s.scaling) + "\n"
        "smooth = " + b(s.smooth) + "\n"
        "# Its look: plain, or crt (a CRT television: raster lines and phosphor stripes). For crt, how dark the\n"
        "# gaps between lines are and how strong the stripes, 0 to 100.\n"
        "look = " + (s.look == host::Look::crt ? "crt" : "plain") + "\n"
        "scanlines = " + std::to_string(s.scanlines) + "\n"
        "mask = " + std::to_string(s.mask) + "\n"
        "# vsync: true, no tearing; false, the lowest latency.\n"
        "vsync = " + b(s.vsync) + "\n"
        "# Fullscreen on a display whose refresh isn't a multiple of 60 Hz (75, 144, 165 Hz): switch to the\n"
        "# highest refresh at the same resolution that is (120 Hz on a 144 Hz monitor), so frames are even.\n"
        "match_refresh = " + b(s.match_refresh) + "\n"
        "# A G-Sync, FreeSync or VRR display with it on in its driver: no switch and no locking to the refresh.\n"
        "variable_refresh = " + b(s.variable_refresh) + "\n"
        "# The graphics device, by name (empty: the best one). From the next start.\n"
        "gpu = " + s.gpu + "\n"
        "# Frame rate, 1% and 0.1% lows and recent frame times in a corner (F3 switches it).\n"
        "performance_overlay = " + b(s.performance_overlay) + "\n"
        "# (The enhancements, internal resolution, anisotropic filtering and MSAA, come when the renderer can\n"
        "# do them: it draws at the game's own resolution for now.)\n"
        "\n"
        "[controls]\n"
        "# The DualShock 3. Each of its controls takes a list of inputs, separated by commas; an empty list\n"
        "# leaves it unused. The settings menu's Controls page sets them by pressing them.\n"
        "#   pad:<button>  a controller's button by an Xbox pad's names: Up Down Left Right A B X Y Start Back\n"
        "#                 Guide LB RB LT RT L3 R3 (LT and RT give L2 and R2 their pressure: throttle and brake)\n"
        "#   key:<name>    a key, as SDL names it: A-Z, 0-9, Up, Down, Left, Right, Space, Return, Backspace,\n"
        "#                 Tab, Left Shift, Keypad 8 ... (not Escape, F1, F3 or F11: they're the program's)\n"
        "#   mouse         the mouse's left button\n"
        "# The face buttons are where they sit on the DualShock: cross at the bottom (an Xbox pad's A), circle on\n"
        "# the right (B). A controller's sticks are the DualShock's sticks; the left_stick_ and right_stick_ lines\n"
        "# are keys for them, each pushing its stick all the way.\n";
    for (unsigned c = 0; c < kControlCount; ++c) {
        t += kControlNames[c];
        t += " =";
        for (size_t i = 0; i < s.controls[c].size(); ++i) t += (i ? ", " : " ") + binding_text(s.controls[c][i]);
        t += "\n";
    }
    t += "\n"
         "[console]\n"
         "# What the game reads as the console's own settings, from the next start.\n"
         "# language: english or japanese (the two this disc has).\n"
         "language = " + s.language + "\n"
         "# enter_button: which button confirms, cross (as on American and European consoles) or circle\n"
         "# (as on Japanese ones); the other goes back.\n"
         "enter_button = " + s.enter_button + "\n"
         "# The console's nickname and the user's name, as the system gives them to the game.\n"
         "nickname = " + s.nickname + "\n"
         "user_name = " + s.user_name + "\n"
         "# The time zone, in minutes from UTC (-300: New York, 0: London, 540: Tokyo).\n"
         "time_zone = " + std::to_string(s.time_zone) + "\n";
    return t;
}

std::filesystem::path settings_file(const std::filesystem::path& folder) { return folder / "ridgeracer7.ini"; }

bool save_settings(const std::filesystem::path& folder, const Settings& s) {
    const std::filesystem::path file = settings_file(folder), tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << settings_text(s);
        if (!out.flush()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

Settings load_settings(const std::filesystem::path& folder) {
    const std::filesystem::path file = settings_file(folder);
    std::ifstream in(file, std::ios::binary);
    if (in) {
        std::stringstream buf;
        buf << in.rdbuf();
        Settings s;
        std::string error;
        if (!parse_settings(buf.str(), s, error))
            qilin::rt::fatal("%s, %s (fix it, or delete the file to get the defaults back)", file.string().c_str(), error.c_str());
        qilin::rt::log("settings: read %s", file.string().c_str());
        return s;
    }
    Settings s = default_settings();
    if (!save_settings(folder, s)) qilin::rt::fatal("cannot write %s", file.string().c_str());
    qilin::rt::log("settings: wrote the defaults to %s", file.string().c_str());
    return s;
}

ControlTable control_table(const Settings& s, const std::function<int(const std::string&)>& key_code, std::vector<std::string>& unknown) {
    ControlTable t;
    for (unsigned c = 0; c < kControlCount; ++c)
        for (const Binding& b : s.controls[c]) {
            ControlTable::Entry& e = t.entries[c];
            switch (b.kind) {
                case Binding::kPad: e.pad |= b.pad; break;
                case Binding::kMouse: e.mouse = true; break;
                case Binding::kKey:
                    if (const int code = key_code(b.key); code >= 0) e.keys.push_back(code);
                    else unknown.push_back(b.key);
                    break;
            }
        }
    return t;
}

PadState compose(const ControlTable& t, const HostInput& in) {
    constexpr uint32_t kTriggers = host::kPadLT | host::kPadRT;
    constexpr float kHeld = 1.0f / 16;
    // How far each control is pressed, 0 to 1.
    const auto amount = [&](unsigned c) {
        const ControlTable::Entry& e = t.entries[c];
        float pull = 0;
        if ((e.pad & host::kPadLT) && in.left_trigger > kHeld) pull = std::max(pull, std::min(in.left_trigger, 1.0f));
        if ((e.pad & host::kPadRT) && in.right_trigger > kHeld) pull = std::max(pull, std::min(in.right_trigger, 1.0f));
        if (e.pad & in.buttons & ~kTriggers) return 1.0f;
        if (e.mouse && (in.buttons & host::kPadClick)) return 1.0f;
        if (in.key_down)
            for (const int code : e.keys)
                if (in.key_down(code)) return 1.0f;
        return pull;
    };
    PadState pad;
    for (unsigned c = 0; c < kButtonCount; ++c) {
        const float a = amount(c);
        if (a <= 0) continue;
        const Ds3Button& d = kDs3[c];
        (d.second ? pad.digital2 : pad.digital1) |= d.bit;
        if (d.pressure >= 0) pad.pressure[d.pressure] = static_cast<uint16_t>(std::lround(a * 255.0f));
    }
    float lx = in.left_x, ly = in.left_y, rx = in.right_x, ry = in.right_y;
    if (amount(kCtlLeftUp) > 0) ly = 1;
    if (amount(kCtlLeftDown) > 0) ly = -1;
    if (amount(kCtlLeftLeft) > 0) lx = -1;
    if (amount(kCtlLeftRight) > 0) lx = 1;
    if (amount(kCtlRightUp) > 0) ry = 1;
    if (amount(kCtlRightDown) > 0) ry = -1;
    if (amount(kCtlRightLeft) > 0) rx = -1;
    if (amount(kCtlRightRight) > 0) rx = 1;
    // 0 to 255, 128 the middle, y down.
    const auto axis = [](float v) { return static_cast<uint16_t>(std::clamp(128.0f + v * 127.5f, 0.0f, 255.0f)); };
    pad.left_x = axis(lx), pad.left_y = axis(-ly);
    pad.right_x = axis(rx), pad.right_y = axis(-ry);
    return pad;
}

}  // namespace rr7

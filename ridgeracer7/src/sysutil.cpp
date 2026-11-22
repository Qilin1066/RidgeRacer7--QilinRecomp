// cellSysutil: the console's settings a game may read.
//
// These are what the settings menu's "Game" page will hold (the roadmap):
// for now they are the fixed values below, a console set up in US English.
// A parameter the game asks for that isn't here stops the game with its
// number, so that none is answered blindly.
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

// The parameters' numbers (the SDK's CELL_SYSUTIL_SYSTEMPARAM_ID_...).
constexpr uint32_t kLanguage = 0x0111, kEnterButton = 0x0112, kNickname = 0x0113, kDateFormat = 0x0114, kTimeFormat = 0x0115, kTimeZone = 0x0116,
                   kSummerTime = 0x0117, kParentalLevel = 0x0121, kUserName = 0x0131;

struct ConsoleSettings {
    int32_t language = 1;      // English (US); 0 is Japanese
    int32_t enter_button = 1;  // cross confirms (0: circle, as on a Japanese console)
    int32_t date_format = 2;   // month, day, year
    int32_t time_format = 0;   // 12-hour
    int32_t time_zone = 0;     // minutes from UTC
    int32_t summer_time = 0;
    int32_t parental_level = 0;  // off
    const char* nickname = "PS3";
    const char* user_name = "User";
};
const ConsoleSettings g_settings;

// cellSysutilGetSystemParamInt(id, int* value)
void cellSysutilGetSystemParamInt(Cpu& c) {
    const uint32_t id = static_cast<uint32_t>(c.r[3]), out = static_cast<uint32_t>(c.r[4]);
    int32_t value;
    switch (id) {
    case kLanguage: value = g_settings.language; break;
    case kEnterButton: value = g_settings.enter_button; break;
    case kDateFormat: value = g_settings.date_format; break;
    case kTimeFormat: value = g_settings.time_format; break;
    case kTimeZone: value = g_settings.time_zone; break;
    case kSummerTime: value = g_settings.summer_time; break;
    case kParentalLevel: value = g_settings.parental_level; break;
    default: qilin::rt::fatal("cellSysutilGetSystemParamInt: the game asks for system parameter %#x, which this runtime has no value for yet", id);
    }
    store32(out, static_cast<uint32_t>(value));
    c.r[3] = kOk;
}

// cellSysutilGetSystemParamString(id, char* buffer, size)
void cellSysutilGetSystemParamString(Cpu& c) {
    const uint32_t id = static_cast<uint32_t>(c.r[3]), buffer = static_cast<uint32_t>(c.r[4]), size = static_cast<uint32_t>(c.r[5]);
    const char* value;
    switch (id) {
    case kNickname: value = g_settings.nickname; break;
    case kUserName: value = g_settings.user_name; break;
    default: qilin::rt::fatal("cellSysutilGetSystemParamString: the game asks for system parameter %#x, which this runtime has no value for yet", id);
    }
    if (size == 0) {
        c.r[3] = kEinval;
        return;
    }
    const size_t length = std::min<size_t>(std::strlen(value), size - 1);
    std::memcpy(guest_memory() + buffer, value, length);
    guest_memory()[buffer + length] = 0;
    c.r[3] = kOk;
}

// cellDiscGameGetBootDiscInfo(CellDiscGameSystemFileParam* param)
// For a game started from a disc: the disc's title id (ten bytes, with its zero) and, after two
// bytes of padding, its parental level, both from the disc's PARAM.SFO.
void cellDiscGameGetBootDiscInfo(Cpu& c) {
    const uint32_t param = static_cast<uint32_t>(c.r[3]);
    if (!param) {
        c.r[3] = 0x8002bd04u;  // CELL_DISCGAME_ERROR_PARAM
        return;
    }
    std::memset(guest_memory() + param, 0, 32);
    std::memcpy(guest_memory() + param, game().title_id.data(), std::min<size_t>(game().title_id.size(), 9));
    store32(param + 12, game().parental_level);
    c.r[3] = kOk;
}

// The system's events for a game (the player asks to quit, the system menu opens): the game registers up
// to four functions, and calls cellSysutilCheckCallback every frame, which calls them for each event that
// has happened since. Events are put here by the runtime (system_event); so far nothing sends one.
struct Callback {
    uint32_t function = 0, user = 0;
};
std::mutex g_callbacks_mutex;
Callback g_callbacks[4];
std::deque<std::pair<uint64_t, uint64_t>> g_events;  // status, parameter

// cellSysutilRegisterCallback(slot, function, user data), cellSysutilUnregisterCallback(slot)
void cellSysutilRegisterCallback(Cpu& c) {
    const uint32_t slot = static_cast<uint32_t>(c.r[3]);
    if (slot > 3) {
        c.r[3] = 0x8002b102u;  // CELL_SYSUTIL_ERROR_VALUE
        return;
    }
    const std::lock_guard lock(g_callbacks_mutex);
    g_callbacks[slot] = {static_cast<uint32_t>(c.r[4]), static_cast<uint32_t>(c.r[5])};
    c.r[3] = kOk;
}
void cellSysutilUnregisterCallback(Cpu& c) {
    const uint32_t slot = static_cast<uint32_t>(c.r[3]);
    if (slot > 3) {
        c.r[3] = 0x8002b102u;
        return;
    }
    const std::lock_guard lock(g_callbacks_mutex);
    g_callbacks[slot] = {};
    c.r[3] = kOk;
}

// ---- the message dialog (cellMsgDialog...) ----
// The system's dialog over the game's picture: a message, and perhaps buttons for the player. As in the system, opening one
// returns at once, and the player's answer reaches the game's function at a later cellSysutilCheckCallback. The dialog is
// shown by the window's thread (main.cpp: a message box of the host's own); one with no buttons is held open, its text in
// the log, until the game closes it. *Drawing it over the game's picture, as the console does, comes with the settings menu.*
struct Dialog {
    bool open = false, waiting = false, answered = false;
    uint32_t type = 0, function = 0, user = 0;
    int32_t answer = 0;
    std::string text;
};
Dialog g_dialog;  // guarded by g_callbacks_mutex

// The dialog's buttons, from its type (bits 4 to 6): 1 yes and no, 2 OK. *0 is taken as OK too, a stated reading of this first
// version of the call (cellMsgDialogOpen, before Open2): the game opens its notice at start-up with 0 and then waits for the
// dialog's function to be called, with no other way to end it (it never aborts it), so the player must be able to dismiss it.*
uint32_t dialog_buttons(uint32_t type) {
    const uint32_t buttons = (type >> 4) & 7;
    return buttons == 0 ? 2 : buttons;
}

// cellMsgDialogOpen(type, const char* message, function, user data, extra parameter)
void cellMsgDialogOpen(Cpu& c) {
    const uint32_t type = static_cast<uint32_t>(c.r[3]), message = static_cast<uint32_t>(c.r[4]);
    const std::lock_guard lock(g_callbacks_mutex);
    if (g_dialog.open) {
        c.r[3] = 0x8002b301u;  // CELL_SYSUTIL_ERROR_BUSY *(as recalled)*
        return;
    }
    if (!message || dialog_buttons(type) > 2) qilin::rt::fatal("cellMsgDialogOpen: type %#x, message %08x: the system's answer to that isn't known here", type, message);
    const char* const p = reinterpret_cast<const char*>(guest_memory() + message);
    g_dialog = {true, dialog_buttons(type) != 0, false, type, static_cast<uint32_t>(c.r[5]), static_cast<uint32_t>(c.r[6]), 0, std::string(p, strnlen(p, 512))};
    qilin::rt::log("the game opens a dialog (type %#x): \"%s\"", type, g_dialog.text.c_str());
    c.r[3] = kOk;
}

// cellMsgDialogClose(delay in milliseconds): the dialog goes, and its function is told: no button (-1).
void cellMsgDialogClose(Cpu& c) {
    const std::lock_guard lock(g_callbacks_mutex);
    if (!g_dialog.open) {
        c.r[3] = 0x8002b302u;  // *(as recalled)*
        return;
    }
    g_dialog.waiting = false;
    g_dialog.answered = true;
    g_dialog.answer = -1;
    c.r[3] = kOk;
}

// cellSysutilCheckCallback(): calls each registered function(status, parameter, user data) for each event waiting.
void cellSysutilCheckCallback(Cpu& c) {
    // A dialog's answer first: its own function(button, user data).
    {
        Dialog done;
        {
            const std::lock_guard lock(g_callbacks_mutex);
            if (g_dialog.open && g_dialog.answered) {
                done = g_dialog;
                g_dialog = {};
            }
        }
        if (done.open && done.function) {
            const uint64_t toc = c.r[2], lr = c.lr;
            c.r[3] = static_cast<uint64_t>(static_cast<int64_t>(done.answer));
            c.r[4] = done.user;
            c.r[2] = load32(done.function + 4);
            c.hooks->call(c, load32(done.function));
            c.r[2] = toc;
            c.lr = lr;
        }
    }
    for (;;) {
        std::pair<uint64_t, uint64_t> event;
        Callback callbacks[4];
        {
            const std::lock_guard lock(g_callbacks_mutex);
            if (g_events.empty()) break;
            event = g_events.front();
            g_events.pop_front();
            for (uint32_t i = 0; i < 4; ++i) callbacks[i] = g_callbacks[i];
        }
        for (const Callback& callback : callbacks) {
            if (!callback.function) continue;
            const uint64_t toc = c.r[2], lr = c.lr;
            c.r[3] = event.first;
            c.r[4] = event.second;
            c.r[5] = callback.user;
            c.r[2] = load32(callback.function + 4);
            c.hooks->call(c, load32(callback.function));
            c.r[2] = toc;
            c.lr = lr;
        }
    }
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"cellSysutil", "cellMsgDialogOpen", cellMsgDialogOpen},
    {"cellSysutil", "cellMsgDialogClose", cellMsgDialogClose},
    {"cellSysutil", "cellSysutilRegisterCallback", cellSysutilRegisterCallback},
    {"cellSysutil", "cellSysutilUnregisterCallback", cellSysutilUnregisterCallback},
    {"cellSysutil", "cellSysutilCheckCallback", cellSysutilCheckCallback},
    {"cellSysutil", "cellDiscGameGetBootDiscInfo", cellDiscGameGetBootDiscInfo},
    {"cellSysutil", "cellSysutilGetSystemParamInt", cellSysutilGetSystemParamInt},
    {"cellSysutil", "cellSysutilGetSystemParamString", cellSysutilGetSystemParamString},
};

}  // namespace

int32_t console_time_zone_minutes() { return g_settings.time_zone; }
int32_t console_summer_time_minutes() { return g_settings.summer_time; }

bool take_dialog(std::string& text, uint32_t& buttons) {
    const std::lock_guard lock(g_callbacks_mutex);
    if (!g_dialog.open || !g_dialog.waiting) return false;
    g_dialog.waiting = false;
    text = g_dialog.text;
    buttons = dialog_buttons(g_dialog.type);
    return true;
}
void answer_dialog(int32_t button) {
    const std::lock_guard lock(g_callbacks_mutex);
    if (!g_dialog.open) return;
    g_dialog.answered = true;
    g_dialog.answer = button;
    qilin::rt::log("the player answers the dialog: %d", button);
}

void system_event(uint64_t status, uint64_t parameter) {
    const std::lock_guard lock(g_callbacks_mutex);
    g_events.emplace_back(status, parameter);
}

std::span<const NativeImport> sysutil_imports() { return kImports; }

}  // namespace rr7

// ridgeracer7: the game, recompiled.
//
//   ridgeracer7 <game> [--flash <dev_flash>]
//     <game>: your copy: the disc's folder (the one holding PS3_GAME), or EBOOT.elf
//     --flash: a PS3's system software, for the console's own fonts (the dev_flash folder RPCS3 makes when it
//              installs Sony's system software file). Without it, an RPCS3 folder beside the game's is looked in.
//
// The executable is built from your own copy of the game (tools/build-ridgeracer7.ps1):
// it holds the lifted code of that copy's program, and at start-up checks
// that the program it is given is the one it was lifted from. The program's
// data is read from your copy each time; nothing of the game is in this
// repository.
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <string>
#include <utility>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#include "machine.hpp"
#include "qilin/host/audio.hpp"
#include <SDL3/SDL.h>

#include "qilin/host/display.hpp"
#include "qilin/host/pad.hpp"
#include "renderer.hpp"
#include "qilin/host/paths.hpp"
#include "qilin/rt/lifted.hpp"
#include "qilin/rt/report.hpp"
#include "sfo.hpp"
#include "title.hpp"

// A picture as a 32-bit BMP (bottom row first, B, G, R, A).
void write_bmp(uint64_t n, const uint8_t* rgba, uint32_t width, uint32_t height) {
    char name[64];
    std::snprintf(name, sizeof name, "frame-%llu.bmp", static_cast<unsigned long long>(n));
    std::FILE* f = std::fopen(name, "wb");
    if (!f) return;
    const uint32_t image = width * height * 4, file = 54 + image;
    uint8_t header[54] = {'B', 'M'};
    const auto put = [&](size_t at, uint32_t v) { for (unsigned i = 0; i < 4; ++i) header[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
    put(2, file), put(10, 54), put(14, 40), put(18, width), put(22, height);
    header[26] = 1, header[28] = 32;
    put(34, image);
    std::fwrite(header, 1, sizeof header, f);
    std::vector<uint8_t> row(size_t{width} * 4);
    for (uint32_t y = height; y-- > 0;) {
        const uint8_t* p = rgba + size_t{y} * width * 4;
        for (uint32_t x = 0; x < width; ++x) row[4 * x] = p[4 * x + 2], row[4 * x + 1] = p[4 * x + 1], row[4 * x + 2] = p[4 * x], row[4 * x + 3] = p[4 * x + 3];
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    qilin::rt::log("flip %llu written to %s", static_cast<unsigned long long>(n), name);
}

// The controller from the window's gamepad, and from the keyboard until the settings menu holds a layout (*the default
// layout is a stated choice*: arrows the d-pad, Z cross, X circle, A square, S triangle, Q L1, W R1, 1 L2, 2 R2, Return
// start, Backspace select). `pressed` adds buttons a test asks for (RR7_PRESS).
rr7::PadState read_pad(const qilin::host::Display& display, uint16_t pressed1, uint16_t pressed2) {
    namespace host = qilin::host;
    rr7::PadState pad;
    const uint32_t b = display.buttons();
    const auto key = [&](const char* name) { return display.key_down(display.key_code(name)); };
    struct Map {
        uint32_t host;
        const char* key;
        bool second;
        uint16_t bit;
        int pressure;  // its place among the pressures, or -1
    };
    static constexpr Map kMap[] = {
        {host::kPadBack, "Backspace", false, 0x01, -1}, {host::kPadL3, nullptr, false, 0x02, -1},   {host::kPadR3, nullptr, false, 0x04, -1},
        {host::kPadStart, "Return", false, 0x08, -1},   {host::kPadUp, "Up", false, 0x10, 2},        {host::kPadRight, "Right", false, 0x20, 0},
        {host::kPadDown, "Down", false, 0x40, 3},       {host::kPadLeft, "Left", false, 0x80, 1},    {host::kPadLT, "1", true, 0x01, 10},
        {host::kPadRT, "2", true, 0x02, 11},            {host::kPadLB, "Q", true, 0x04, 8},          {host::kPadRB, "W", true, 0x08, 9},
        {host::kPadY, "S", true, 0x10, 4},              {host::kPadB, "X", true, 0x20, 5},           {host::kPadA, "Z", true, 0x40, 6},
        {host::kPadX, "A", true, 0x80, 7},
    };
    for (const Map& m : kMap) {
        const bool down = (b & m.host) || (m.key && key(m.key)) || ((m.second ? pressed2 : pressed1) & m.bit);
        if (!down) continue;
        (m.second ? pad.digital2 : pad.digital1) |= m.bit;
        if (m.pressure >= 0) pad.pressure[m.pressure] = 255;
    }
    const auto axis = [](float v) { return static_cast<uint16_t>(std::clamp(128.0f + v * 127.5f, 0.0f, 255.0f)); };
    float x = 0, y = 0;
    display.left_stick(x, y);
    pad.left_x = axis(x), pad.left_y = axis(-y);
    display.right_stick(x, y);
    pad.right_x = axis(x), pad.right_y = axis(-y);
    return pad;
}

// A development aid: RR7_PRESS=<button>@<flip>[,...] holds a button for six flips from that flip (cross, circle, square,
// triangle, start, select, up, down, left, right, l1, r1, l2, r2), for runs with nobody at the controller.
struct Press {
    uint64_t flip;
    bool second;
    uint16_t bit;
};
std::vector<Press> presses_asked() {
    std::vector<Press> out;
    const char* list = std::getenv("RR7_PRESS");
    if (!list) return out;
    struct Name {
        const char* name;
        bool second;
        uint16_t bit;
    };
    static constexpr Name kNames[] = {
        {"select", false, 0x01},
        {"start", false, 0x08},
        {"up", false, 0x10},
        {"right", false, 0x20},
        {"down", false, 0x40},
        {"left", false, 0x80},
        {"l2", true, 0x01},
        {"r2", true, 0x02},
        {"l1", true, 0x04},
        {"r1", true, 0x08},
        {"triangle", true, 0x10},
        {"circle", true, 0x20},
        {"cross", true, 0x40},
        {"square", true, 0x80},
    };
    std::string text = list;
    for (size_t at = 0; at < text.size();) {
        const size_t end = std::min(text.find(',', at), text.size());
        const std::string item = text.substr(at, end - at);
        at = end + 1;
        const size_t mark = item.find('@');
        bool found = false;
        for (const Name& n : kNames)
            if (mark != std::string::npos && item.substr(0, mark) == n.name) {
                out.push_back({std::strtoull(item.c_str() + mark + 1, nullptr, 10), n.second, n.bit});
                found = true;
            }
        if (!found) qilin::rt::fatal("RR7_PRESS: \"%s\" is not <button>@<flip>", item.c_str());
    }
    return out;
}

int main(int argc, char** argv) {
    using qilin::rt::fatal;
    if (argc != 2 && !(argc == 4 && std::strcmp(argv[2], "--flash") == 0)) {
        std::fprintf(stderr, "usage: ridgeracer7 <game> [--flash <dev_flash>]   (<game>: the disc's folder, the one holding PS3_GAME, or the decrypted EBOOT.elf)\n");
        return 2;
    }
    const auto files = qilin::find_game(argv[1]);
    if (!files) fatal("%s", files.error().c_str());
    const auto elf = qilin::load_game_elf(*files);
    if (!elf) fatal("%s", elf.error().c_str());
    if (elf->sha256() != qilin::rt::kLiftedFrom)
        fatal("%s is not the program this executable was built from (its SHA-256 is %s, and the lifted code's program's is %s): build again from this copy",
              files->elf.string().c_str(), elf->sha256().c_str(), qilin::rt::kLiftedFrom);
    // The disc's description of the game: the system hands parts of it to the game.
    rr7::GameInfo info;
    info.usrdir = files->usrdir;
    if (argc == 4) {
        info.flash = argv[3];
        if (!std::filesystem::is_directory(info.flash / "data" / "font")) fatal("%s holds no data/font folder: it is not a PS3's system software", argv[3]);
    } else if (!files->usrdir.empty()) {
        // <disc>/PS3_GAME/USRDIR -> the folder the disc's folder is in, then RPCS3/dev_flash.
        std::error_code error;
        const auto beside = std::filesystem::weakly_canonical(files->usrdir, error).parent_path().parent_path().parent_path() / "RPCS3" / "dev_flash";
        if (!error && std::filesystem::is_directory(beside / "data" / "font", error)) info.flash = beside;
    }
    info.hdd = qilin::host::user_folder() / "dev_hdd0";
    {
        std::error_code error;
        std::filesystem::create_directories(info.hdd / "game", error);
        if (error) fatal("can't create %s: %s", (info.hdd / "game").string().c_str(), error.message().c_str());
    }
    if (files->sfo.empty()) fatal("%s: no PARAM.SFO was found for this copy (give the disc's folder, the one holding PS3_GAME)", argv[1]);
    const auto sfo = qilin::Sfo::load(files->sfo);
    if (!sfo) fatal("%s", sfo.error().c_str());
    info.title_id = sfo->text("TITLE_ID").value_or("");
    info.parental_level = sfo->number("PARENTAL_LEVEL").value_or(0);
    if (info.title_id.empty()) fatal("%s names no title id", files->sfo.string().c_str());
    // A development aid: RR7_QUIET=1 runs with no window (headless), no sound and no controller but RR7_PRESS's, so that a
    // test run disturbs nobody at the computer. Its dialogs must be answered by RR7_ANSWER.
    const bool quiet = std::getenv("RR7_QUIET") != nullptr;
    if (quiet && !std::getenv("RR7_ANSWER")) fatal("RR7_QUIET needs RR7_ANSWER: with no window, nobody can answer the system's dialogs");
    // The game's sound, to the host's output: opened when the first of it comes.
    if (quiet) {
        rr7::set_audio_sink([](const int16_t*, size_t) {});
    } else {
        rr7::set_audio_sink([](const int16_t* left_right, size_t frames) {
            static qilin::host::AudioOut out;
            out.push(left_right, frames, 48000);
        });
    }
    // The window, on this thread (the window system wants its events here); the game on a thread of its own.
    namespace host = qilin::host;
    host::Display::Options o;
    o.title = "Ridge Racer 7";
    o.width = 1280, o.height = 720;
    o.headless = quiet;
    host::Display display(o);
    qilin::rt::log("graphics device: %s", display.device_name().c_str());
    static std::mutex queue;  // the GPU's queue: the renderer's draws and the window's presentation take turns on it
    rr7::renderer_start(display.gpu(), queue);
    // Each flip's picture, kept for the window until it is shown.
    static std::mutex frame_mutex;
    static std::vector<uint8_t> frame;
    static uint32_t frame_width = 0, frame_height = 0;
    static std::atomic<uint64_t> flips{0};
    // A development aid: RR7_SAVE_FRAME=<n> writes the nth flip's picture to frame-<n>.bmp in the current folder.
    static const uint64_t save_frame = std::getenv("RR7_SAVE_FRAME") ? std::strtoull(std::getenv("RR7_SAVE_FRAME"), nullptr, 10) : 0;
    rr7::set_present_sink([](const uint8_t* rgba, uint32_t width, uint32_t height) {
        const std::lock_guard lock(frame_mutex);
        frame.assign(rgba, rgba + size_t{width} * height * 4);
        frame_width = width, frame_height = height;
        const uint64_t n = ++flips;
        if (n == 1 || n % 600 == 0) qilin::rt::log("flip %llu: %ux%u", static_cast<unsigned long long>(n), width, height);
        if (n == save_frame) write_bmp(n, rgba, width, height);
    });
    static rr7::GameInfo game_info = std::move(info);
    static const qilin::Elf* program = &*elf;
    std::thread([] {
        rr7::run(*program, {qilin::rt::kLiftedFunctions, qilin::rt::kLiftedFunctionCount}, {qilin::rt::kLiftedSpuPrograms, qilin::rt::kLiftedSpuProgramCount}, std::move(game_info));
    }).detach();
    // Until the game's first flip, black.
    const uint8_t black[4] = {0, 0, 0, 255};
    {
        const std::lock_guard lock(queue);
        display.present(1, 1, black, 16.0f / 9.0f, 1080);
    }
    uint64_t shown = 0;
    std::vector<uint8_t> picture;
    const std::vector<Press> presses = presses_asked();
    uint32_t width = 0, height = 0;
    // A development aid: RR7_SECONDS=<n> ends the run after n seconds, writing the last flip's picture (frame-0.bmp) and
    // where the renderer's time went.
    const char* seconds = std::getenv("RR7_SECONDS");
    const auto ends = std::chrono::steady_clock::now() + std::chrono::seconds(seconds ? std::strtoull(seconds, nullptr, 10) : 0);
    while (display.poll()) {
        if (seconds && std::chrono::steady_clock::now() > ends) {
            const std::lock_guard lock(frame_mutex);
            if (!frame.empty()) write_bmp(0, frame.data(), frame_width, frame_height);
            rr7::renderer_report();
            qilin::rt::log("stopped: RR7_SECONDS's %s seconds are over, at flip %llu", seconds, static_cast<unsigned long long>(flips.load()));
            std::fflush(nullptr);
            std::_Exit(0);
        }
        {
            uint16_t pressed1 = 0, pressed2 = 0;
            const uint64_t now = flips;
            for (const Press& p : presses)
                if (now >= p.flip && now < p.flip + 6) (p.second ? pressed2 : pressed1) |= p.bit;
            if (quiet) {
                rr7::PadState pad;
                pad.digital1 = pressed1, pad.digital2 = pressed2;
                rr7::set_pad_state(pad);
            } else {
                rr7::set_pad_state(read_pad(display, pressed1, pressed2));
            }
        }
        // A dialog of the system's that asks the player something: a message box of the host's, over the window.
        {
            std::string text;
            uint32_t buttons = 0;
            // A development aid: RR7_ANSWER=<1 yes or OK, 2 no, 3 back> answers the system's dialogs without showing them.
            static const char* const answer = std::getenv("RR7_ANSWER");
            if (answer && rr7::take_dialog(text, buttons)) {
                rr7::answer_dialog(std::atoi(answer));
            } else if (!answer && rr7::take_dialog(text, buttons)) {
                const SDL_MessageBoxButtonData yes_no[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"}, {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "No"}};
                const SDL_MessageBoxButtonData ok[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "OK"}};
                SDL_MessageBoxData box{};
                box.flags = SDL_MESSAGEBOX_INFORMATION;
                box.window = static_cast<SDL_Window*>(display.sdl_window());
                box.title = "Ridge Racer 7";
                box.message = text.c_str();
                box.numbuttons = buttons == 1 ? 2 : 1;
                box.buttons = buttons == 1 ? yes_no : ok;
                int chosen = -1;
                if (!SDL_ShowMessageBox(&box, &chosen)) qilin::rt::fatal("the host can't show the system's dialog: %s", SDL_GetError());
                rr7::answer_dialog(chosen < 0 ? 3 : chosen);
            }
        }
        if (quiet) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (flips != shown) {
            {
                const std::lock_guard lock(frame_mutex);
                picture = frame;
                width = frame_width, height = frame_height;
                shown = flips;
            }
            const std::lock_guard lock(queue);
            display.present(width, height, picture.data(), 16.0f / 9.0f, height);
        } else {
            const std::lock_guard lock(queue);
            display.present_again();
        }
    }
    qilin::rt::log("the window was closed");
    rr7::exit_process(0);
}

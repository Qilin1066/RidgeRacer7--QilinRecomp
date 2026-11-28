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
#include <cmath>
#include <tuple>
#include <atomic>
#include <string>
#include <utility>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "machine.hpp"
#include "menu.hpp"
#include "pause.hpp"
#include "settings.hpp"
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

// The controller from the window's gamepad and the keyboard, through the settings' bindings (settings.hpp). `pressed` adds
// buttons a test asks for (RR7_PRESS).
rr7::PadState read_pad(const qilin::host::Display& display, const rr7::ControlTable& controls, uint16_t pressed1, uint16_t pressed2) {
    rr7::HostInput in;
    in.buttons = display.buttons();
    in.key_down = [&](int code) { return display.key_down(code); };
    display.left_stick(in.left_x, in.left_y);
    display.right_stick(in.right_x, in.right_y);
    display.triggers(in.left_trigger, in.right_trigger);
    rr7::PadState pad = rr7::compose(controls, in);
    pad.digital1 |= pressed1, pad.digital2 |= pressed2;
    return pad;
}

// The bindings' table for the window's keys. A key the window system doesn't know by the name the settings give stops the
// game, naming it, rather than leaving the control silently dead.
rr7::ControlTable control_table(const qilin::host::Display& display, const rr7::Settings& settings, const std::filesystem::path& folder) {
    std::vector<std::string> unknown;
    rr7::ControlTable t = rr7::control_table(settings, [&](const std::string& name) { return display.key_code(name.c_str()); }, unknown);
    if (!unknown.empty())
        qilin::rt::fatal("%s: [controls] names a key the window system doesn't know: \"%s\" (SDL's key names: A, 1, Return, Left Shift, Keypad 8...)",
                         rr7::settings_file(folder).string().c_str(), unknown.front().c_str());
    return t;
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
    // The settings file (ridgeracer7.ini), in the data folder with the saves; the console's settings the game reads from it.
    const std::filesystem::path data_folder = qilin::host::user_folder();
    static rr7::Settings settings = rr7::load_settings(data_folder);
    rr7::set_console_settings(rr7::console_of(settings));
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
    // The game's sound, to the host's output (none headless): opened by the mixer's thread when the game starts its sound
    // library. The mixer paces itself by it and is held there while the settings menu pauses the game (audio.cpp).
    if (quiet) rr7::set_audio_output(nullptr);
    else rr7::set_audio_output([] { return std::make_unique<qilin::host::AudioOut>(48000); });
    // The window, on this thread (the window system wants its events here); the game on a thread of its own.
    namespace host = qilin::host;
    host::Display::Options o;
    o.title = "Ridge Racer 7";
    o.width = 1280, o.height = 720;
    o.headless = quiet;
    o.fullscreen = settings.fullscreen && !quiet;
    o.monitor = settings.monitor;
    o.picture = rr7::picture_of(settings);
    host::Gpu::prefer(settings.gpu);
    host::Display display(o);
    qilin::rt::log("graphics device: %s", display.device_name().c_str());
    static std::mutex queue;  // the GPU's queue: the renderer's draws and the window's presentation take turns on it
    rr7::renderer_start(display.gpu(), queue, data_folder);  // (its pipelines recorded there, made ahead at the next start)
    // The settings menu (F1 or Esc), over the game; none headless.
    rr7::Menu menu(display, settings, data_folder, queue);
    rr7::ControlTable controls = quiet ? rr7::ControlTable{} : control_table(display, settings, data_folder);
    // Each flip's picture, kept for the window until it is shown.
    static std::mutex frame_mutex;
    static std::vector<uint8_t> frame;
    static uint32_t frame_width = 0, frame_height = 0;
    static int gpu_frame = -1;  // the last flip's picture, when the renderer keeps it on the GPU
    static std::atomic<uint64_t> flips{0};
    // A development aid: RR7_SAVE_FRAME=<n>[,<n>...] writes the nth flip's picture to frame-<n>.bmp in the current folder.
    static const std::set<uint64_t> save_frames = [] {
        std::set<uint64_t> s;
        for (const char* p = std::getenv("RR7_SAVE_FRAME"); p && *p;) {
            char* end = nullptr;
            s.insert(std::strtoull(p, &end, 10));
            if (end == p) qilin::rt::fatal("RR7_SAVE_FRAME: flip numbers separated by commas, not \"%s\"", p);
            p = *end == ',' ? end + 1 : end;
            if (*end && *end != ',') qilin::rt::fatal("RR7_SAVE_FRAME: flip numbers separated by commas, not \"%s\"", end);
        }
        return s;
    }();
    rr7::set_present_sink([](const rr7::Frame& f) {
        {
            const std::lock_guard lock(frame_mutex);
            // (A picture on the GPU is kept as its number: the window shows it from there.)
            if (f.rgba) frame.assign(f.rgba, f.rgba + size_t{f.width} * f.height * 4);
            else frame.clear();
            gpu_frame = f.gpu_frame;
            frame_width = f.width, frame_height = f.height;
            const uint64_t n = ++flips;
            if (n == 1 || n % 600 == 0) qilin::rt::log("flip %llu: %ux%u", static_cast<unsigned long long>(n), f.width, f.height);
            if (save_frames.contains(n)) {
                std::vector<uint8_t> pixels;
                if (!f.rgba) rr7::renderer_frame_pixels(f.gpu_frame, pixels);
                write_bmp(n, f.rgba ? f.rgba : pixels.data(), f.width, f.height);
            }
        }
        // The RSX's thread is held here, its picture handed over, while the settings menu pauses the game (pause.hpp).
        rr7::hold_while_paused(rr7::HoldPoint::flip);
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
        menu.tick();
        if (menu.quit_requested()) {
            qilin::rt::log("quit from the settings menu");
            std::fflush(nullptr);
            rr7::exit_process(0);
        }
        if (menu.take_controls_changed()) controls = control_table(display, settings, data_folder);
        if (seconds && std::chrono::steady_clock::now() > ends) {
            const std::lock_guard lock(frame_mutex);
            if (gpu_frame >= 0) rr7::renderer_frame_pixels(gpu_frame, frame);
            if (!frame.empty()) write_bmp(0, frame.data(), frame_width, frame_height);
            rr7::renderer_report();
            rr7::profile_report();
            rr7::spu_report();
            qilin::rt::log("stopped: RR7_SECONDS's %s seconds are over, at flip %llu", seconds, static_cast<unsigned long long>(flips.load()));
            std::fflush(nullptr);
            std::_Exit(0);
        }
        {
            uint16_t pressed1 = 0, pressed2 = 0;
            const uint64_t now = flips;
            for (const Press& p : presses)
                if (now >= p.flip && now < p.flip + 6) (p.second ? pressed2 : pressed1) |= p.bit;
            if (quiet || menu.is_open()) {  // (the menu's input isn't the game's)
                rr7::PadState pad;
                pad.digital1 = pressed1, pad.digital2 = pressed2;
                rr7::set_pad_state(pad);
            } else {
                rr7::set_pad_state(read_pad(display, controls, pressed1, pressed2));
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
        if (menu.is_open()) display.pause_frame_times();  // the pause isn't a frame time
        if (flips != shown) {
            int on_gpu = -1;
            {
                const std::lock_guard lock(frame_mutex);
                on_gpu = gpu_frame;
                if (on_gpu < 0) picture = frame;
                width = frame_width, height = frame_height;
                shown = flips;
            }
            const std::lock_guard lock(queue);
            if (on_gpu >= 0) {
                VkImage image = VK_NULL_HANDLE;
                VkImageView view = VK_NULL_HANDLE;
                rr7::renderer_frame_image(on_gpu, image, view);
                display.present_image(image, view, width, height, 16.0f / 9.0f, height);
            } else {
                display.present(width, height, picture.data(), 16.0f / 9.0f, height);
            }
        } else {
            const std::lock_guard lock(queue);
            display.present_again();
        }
    }
    qilin::rt::log("the window was closed");
    rr7::exit_process(0);
}

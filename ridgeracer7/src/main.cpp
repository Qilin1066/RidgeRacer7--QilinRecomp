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
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#include "machine.hpp"
#include "qilin/host/audio.hpp"
#include "qilin/host/display.hpp"
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
    // The game's sound, to the host's output: opened when the first of it comes.
    rr7::set_audio_sink([](const int16_t* left_right, size_t frames) {
        static qilin::host::AudioOut out;
        out.push(left_right, frames, 48000);
    });
    // The window, on this thread (the window system wants its events here); the game on a thread of its own.
    namespace host = qilin::host;
    host::Display::Options o;
    o.title = "Ridge Racer 7";
    o.width = 1280, o.height = 720;
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
    uint32_t width = 0, height = 0;
    while (display.poll()) {
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

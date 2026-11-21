// ridgeracer7: the game, recompiled.
//
//   ridgeracer7 <game>     <game>: your copy: the disc's folder (the one holding PS3_GAME), or EBOOT.elf
//
// The executable is built from your own copy of the game (tools/build-ridgeracer7.ps1):
// it holds the lifted code of that copy's program, and at start-up checks
// that the program it is given is the one it was lifted from. The program's
// data is read from your copy each time; nothing of the game is in this
// repository.
#include <cstdio>
#include <cstring>
#include <string>

#include "machine.hpp"
#include "qilin/rt/lifted.hpp"
#include "qilin/rt/report.hpp"
#include "sfo.hpp"
#include "title.hpp"

int main(int argc, char** argv) {
    using qilin::rt::fatal;
    if (argc != 2) {
        std::fprintf(stderr, "usage: ridgeracer7 <game>   (the disc's folder, the one holding PS3_GAME, or the decrypted EBOOT.elf)\n");
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
    if (files->sfo.empty()) fatal("%s: no PARAM.SFO was found for this copy (give the disc's folder, the one holding PS3_GAME)", argv[1]);
    const auto sfo = qilin::Sfo::load(files->sfo);
    if (!sfo) fatal("%s", sfo.error().c_str());
    info.title_id = sfo->text("TITLE_ID").value_or("");
    info.parental_level = sfo->number("PARENTAL_LEVEL").value_or(0);
    if (info.title_id.empty()) fatal("%s names no title id", files->sfo.string().c_str());
    return rr7::run(*elf, {qilin::rt::kLiftedFunctions, qilin::rt::kLiftedFunctionCount}, std::move(info));
}

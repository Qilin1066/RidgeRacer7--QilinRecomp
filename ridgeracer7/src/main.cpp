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
    return rr7::run(*elf, {qilin::rt::kLiftedFunctions, qilin::rt::kLiftedFunctionCount});
}

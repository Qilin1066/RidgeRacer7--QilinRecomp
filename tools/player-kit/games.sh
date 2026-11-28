# The game's details for the packaging scripts (sourced), as Toy Commander's and Rhythm Heaven Fever's games.sh.
# Everything game-specific in a download comes from here: the README templates' @NAME@ places (fill) and kit/game.txt
# (write_game_txt), which is all the launcher knows about the game (tools/player-kit/launcher/ADOPTING.md).
#
# `game` is the per-user data folder's name, qilin/ridgeracer7, which the game itself uses (host/src/paths.cpp): the
# launcher's remembered folder and build cache sit beside the player's saves and settings.
game=ridgeracer7
REPO="Qilin1066/RidgeRacer7--QilinRecomp"
TITLE="Ridge Racer 7"; PACKAGE="RidgeRacer7--QilinRecomp"; EXE="ridgeracer7.exe"; BOOT="EBOOT.elf"
BUILD_TITLE="Ridge Racer 7 (USA)"; PRODUCT="BLUS30001"; DISC_VERSION="01.02"
RELEASE="the USA disc, BLUS30001, version 01.02"
STUDIO="Namco Bandai Games, PlayStation 3, 2006"
FULL_TITLE="Ridge Racer 7"
# How much lifted code goes to a file (qilin lift --file-kb) and what one compile needs at its peak: see write_game_txt.
FILE_KB=2048
COMPILE_MEMORY_MB=900
# Fills a template's @NAME@ places with the game's details.
fill() {
    sed -e "s|@GAME@|$game|g" -e "s|@FULL_TITLE@|$FULL_TITLE|g" -e "s|@TITLE@|$TITLE|g" -e "s|@PACKAGE@|$PACKAGE|g" -e "s|@EXE@|$EXE|g" \
        -e "s|@BOOT@|$BOOT|g" -e "s|@BUILD_TITLE@|$BUILD_TITLE|g" -e "s|@RELEASE@|$RELEASE|g" -e "s|@STUDIO@|$STUDIO|g" \
        -e "s|@REPO@|$REPO|g" -e "s|@COMPILE_MEMORY_MB@|$COMPILE_MEMORY_MB|g" "$1"
}
# Writes kit/game.txt, one name=value a line:
#   write_game_txt <file> <exe> <compiler> [<compiler_args>]
# with Windows line ends where GAME_TXT_CRLF=1. Every key is written, the launcher's defaults too, so the file says the
# whole story: the game's names, the data it builds from (the disc's folder, the one holding PS3_GAME, with the EBOOT.elf
# the player decrypts with RPCS3 in USRDIR; `build` is recomp/src/identify.cpp's supported build, which
# `qilin identify --build` checks, together with the console's fonts), how it is recompiled, what a compile needs, how
# the game starts (ridgeracer7 <folder>: main.cpp finds the fonts itself, title.hpp's find_flash), and where releases are
# published (tags vX.Y.Z).
#
# The steps, as tools/build-ridgeracer7.ps1 builds the game:
#   1. qilin lift: the PPU code and the 42 SPU programs of the player's EBOOT.elf to C++ in work/gen;
#   2. qilin shaders: the game's 242 RSX shader programs, translated to SPIR-V, into work/gen/shaders.cpp.
# Both write the same text every time for the same EBOOT.elf, so a second build recompiles nothing (qilin lift leaves
# unchanged files alone, and removes an earlier lift's files past its own last).
#
# compile_memory_mb: measured, not guessed (2026-11-28, zig 0.17.1's Clang 22 on Windows x86-64, the kit's cxxflags and
# -march=native, zig and its Clang together). qilin lift's own files of 256 functions are up to 11.2 MB of C++, and the
# largest peaked at 1,115 MB (tools/build-ridgeracer7.ps1 allows MSVC and clang-cl 8 GB a compile). With --file-kb 2048 a file is
# closed once it holds 2 MiB (70 PPU files instead of 56): the largest then peak at 447-461 MB, and the largest compile
# of all is an SPU program's (spu_p41.cpp, 1.4 MB, which no split changes) at 544 MB. 900 leaves about 65% for the other
# compilers (llvm-mingw's, Apple's), ARM64 and a larger file after a lifter change. The split costs the game almost no
# speed: functions stay in address order, and the share of direct calls whose callee is in the same file (the ones
# Clang can inline) goes from 29.7% to 27.7%, where 64 functions a file would have made it 16.8%.
#
# old_files: none. Before these downloads the game was built only in a clone of the source (Play Ridge Racer 7.bat and
# Update Ridge Racer 7.bat, tools/build-ridgeracer7.ps1, which build into build-game there), never into a download
# folder, so nothing of it is here; those .bat files stay for developers.
write_game_txt() {
    {
        printf 'game=%s\ntitle=%s\nexe=%s\nboot=%s\n' "$game" "$TITLE" "$2" "$BOOT"
        printf 'build=%s\nproduct=%s\ndisc_version=%s\nrelease=%s\n' "$BUILD_TITLE" "$PRODUCT" "$DISC_VERSION" "$RELEASE"
        printf 'package=%s\nrepo=%s\nrelease_tag_prefix=v\nrelease_asset_prefix=%s-\n' "$PACKAGE" "$REPO" "$PACKAGE"
        printf 'input=folder\ninput_noun=game folder\n'
        printf 'input_hint=Choose the folder holding PS3_GAME, copied from your disc, with the EBOOT.elf RPCS3 decrypts (Utilities > Decrypt PS3 Binaries) in PS3_GAME/USRDIR beside EBOOT.BIN.\n'
        printf 'identify={qilin} identify {input} --json --build {game:build}\n'
        printf 'step.1={qilin} lift {input} --out {gen} --file-kb %s\n' "$FILE_KB"
        printf 'step.2={qilin} shaders {input} --out {gen}\n'
        printf 'compile_memory_mb=%s\nbuild_space_mb=2048\ncache_mb=2048\nrun_args={input}\n' "$COMPILE_MEMORY_MB"
        printf '# Nothing from before the launcher is in a download folder (the .bat files build in a clone of the source).\n'
        printf 'old_files=\nold_input_file=\n'
        printf 'compiler=%s\n' "$3"
        if [ -n "${4:-}" ]; then printf 'compiler_args=%s\n' "$4"; fi
    } | if [ "${GAME_TXT_CRLF:-0}" = 1 ]; then sed 's/$/\r/'; else cat; fi > "$1"
}

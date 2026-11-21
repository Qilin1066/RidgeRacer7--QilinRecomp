# Naming

**Qilin** is the author's name. It goes on the *tools*, the things that do the recompiling. It doesn't go on the game. This is the same rule as the other Qilin recomps' (GBTGhoulies-QilinRecomp, ApbArcade-QilinRecomp, BeatTheBeat-QilinRecomp, PunchOutWii-QilinRecomp, WindWakerHD-QilinRecomp).

| Thing | Name | Why |
|---|---|---|
| The repo / project | RidgeRacer7--QilinRecomp | The game plus the toolchain that ports it |
| The recompiler CLI | `qilin` | The author's tool: short, unique, easy to type |
| C++ namespaces of the recompiler and runtime | `qilin`, `qilin::ppu` and `qilin::spu` (the decoders), `qilin::rt`, `qilin::gen` | Project namespace; avoids collisions with game, SDK or system names |
| Build options, macros, CMake targets | `QILIN_*`, `qilin_*` | Same prefix as the namespace, so it's obvious what owns them |
| The game executable players run | `ridgeracer7` | Named after the game. Players launch Ridge Racer 7, not a tool |
| Settings file, save folder, window title | after the game (`qilin/ridgeracer7` is the data folder, `ridgeracer7.ini` the settings file, `ridgeracer7.log` the log) | Same reason |
| The Windows launchers | `Play Ridge Racer 7.bat`, `Update Ridge Racer 7.bat` (when there is something to play; `Analyse game.bat` today) | As the other recomps name theirs |
| Lifted functions | `fn_<address>`, e.g. `fn_0001022c` | Named by their address in the original program, as linked (code starts at `0x00010200`); real names where analysis supplies them (`prior-art.md`) |
| SPU functions | `spu_<image>_<address>` | An SPU program has its own 256 KiB address space, so the image is part of the name |
| System functions the runtime provides | their own names (`cellGcmInit`, `cellPadGetData`, ...) once the ids are matched to names; until then `<library>_<id>` | The program identifies them by library and a 32-bit id (`elf-analysis.md`); nothing is found by address |

New code follows the same rule: the prefix marks the toolchain, and anything a player sees is named after the game.

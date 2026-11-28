// Stands in for the code `qilin lift` and `qilin shaders` make from the player's own EBOOT.elf (empty tables: no
// functions, no SPU programs, no shaders), so that the game's program (ridgeracer7/src/main.cpp and everything it links)
// links without it: CMake builds this as ridgeracer7_link_check, and the player kit's checks link it with the kit's
// archives as the launcher links the lifted code. Started with no game folder, the program stops at once with its usage
// (exit 2).
#include "qilin/rt/lifted.hpp"
#include "qilin/rt/shaders.hpp"

namespace qilin::rt {
extern const LiftedFunction kLiftedFunctions[] = {{0, nullptr}};
extern const size_t kLiftedFunctionCount = 0;
extern const char kLiftedFrom[] = "";
extern const LiftedSpuProgram kLiftedSpuPrograms[] = {{0, 0, 0, nullptr}};
extern const size_t kLiftedSpuProgramCount = 0;
extern const TranslatedShader kTranslatedShaders[] = {{}};
extern const size_t kTranslatedShaderCount = 0;
}  // namespace qilin::rt

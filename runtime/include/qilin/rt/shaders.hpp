// What the translated shaders give the runtime: every one of the game's
// vertex and fragment programs as SPIR-V, found by its key.
//
// `qilin shaders <game> --out <folder>` writes the definitions
// (shaders.cpp) from the player's own copy; they are never part of this
// repository. What each program expects of the renderer is
// recomp/src/rsx_spirv.hpp's.
#pragma once

#include <cstddef>
#include <cstdint>

namespace qilin::rt {

struct TranslatedShader {
    uint64_t key;      // the hash of its microcode, a fragment program's inline constants left out (rsx_spirv.hpp)
    bool vertex;
    uint32_t address;  // where its compiled form lies in the program
    const uint32_t* spirv;
    size_t spirv_words;
    uint32_t inputs, outputs, textures, cube_textures;
    const uint32_t* constant_slots;  // a fragment program's inline constants: each one's offset in its microcode, in bytes
    size_t constant_slot_count;
    // Its forms for draw state the plain one can't serve, by number (rsx_spirv.hpp's Variant: 0 is the plain one, `spirv`;
    // a fragment program's 1 has flat colours, 2 two-sided ones, 3 both; a vertex program's 1 writes clip distances), each
    // null where the program has none (it needs none, or nothing draws with it).
    const uint32_t* variant_spirv[4];
    size_t variant_words[4];
    uint32_t clip_planes;  // a vertex program's: the user clip planes whose distances it writes (bit n for plane n)
    // A vertex program's constants read, [first, end): all 468 if it indexes them; end 0 where not known (a table written
    // before they were recorded) or none are read, when the renderer gives it all of them.
    uint32_t constants_first, constants_end;
};

extern const TranslatedShader kTranslatedShaders[];
extern const size_t kTranslatedShaderCount;

}  // namespace qilin::rt

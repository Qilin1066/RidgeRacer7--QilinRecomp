// The RSX's vertex and fragment programs translated to SPIR-V, at build
// time (docs/recompiler-core.md): no shader translator runs in the game.
//
// What a translated program expects of the renderer, and gives it:
//
// Vertex programs
//   - inputs: attribute n at location n (a vec4), for each the program reads;
//   - constants: set 0, binding 0, a uniform block of 468 vec4 (c[0] to c[467]);
//   - push constants, offset 0: vec4 `clip_xy` and vec4 `clip_z`, the RSX's viewport turned into Vulkan's: the
//     position written is (x * clip_xy.x + w * clip_xy.z, y * clip_xy.y + w * clip_xy.w, z * clip_z.x + w * clip_z.y, w);
//   - outputs: o[POS] as the position, o[PSZ] as the point size, every other output register n written at location n.
//     A component of an output the program doesn't write is (0, 0, 0, 1)'s: *a stated choice*.
// Fragment programs
//   - inputs: COL0 at location 1, COL1 at 2, FOGC at 5, TEXn at 7 + n (the vertex outputs' numbers);
//   - inline constants: set 0, binding 1, a uniform block of vec4, one for each constant slot in the order they come
//     (`constant_slots` says where each is in the microcode, for the renderer to read the game's values from it);
//   - textures: set 1, binding n for texture unit n: a 2D or cube sampler, as the program's Cg parameters say;
//   - outputs: location 0 the colour (R0, or H0 when the header says), locations 1 to 3 R2, R3, R4 (H4, H6, H8)
//     when the program writes them (the RSX's other colour targets).
// The H registers are the halves of the R registers (H2n and H2n+1 of Rn: the programs' register counts show it).
// The translation keeps them as separate values and refuses a program in which a value crosses between the two.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "rsx_program.hpp"

namespace qilin::rsx {

struct Translation {
    std::vector<uint32_t> spirv;
    uint32_t inputs = 0;   // vertex: attributes read; fragment: input registers read (bit n for input n)
    uint32_t outputs = 0;  // vertex: output registers written; fragment: colour targets written (bit 0 to 3)
    uint32_t textures = 0, cube_textures = 0;  // fragment: units sampled, and which of them are cubes
    std::vector<uint32_t> constant_slots;      // fragment: each inline constant's offset in the microcode, in bytes
};

std::expected<Translation, std::string> translate(const CgProgram& program);

// The key the renderer finds a translated program by: a hash (64-bit FNV-1a) of its microcode, with a fragment
// program's inline constants left out (the game patches them; their values come to the shader another way).
uint64_t program_key(const CgProgram& program);

}  // namespace qilin::rsx

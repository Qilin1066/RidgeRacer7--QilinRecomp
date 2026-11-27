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
//   - inputs: COL0 at location 1, COL1 at 2, FOGC at 5, TEXn at 7 + n (the vertex outputs' numbers); COL0 and COL1 flat in
//     the flat form, and with their back colours at 3 and 4 in the two-sided form (Variant);
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

// A program's forms for draw state its plain translation can't serve, each a module of its own made at build time (a
// decoration or a built-in can't be changed by the renderer). They are numbered for the shader table (shaders.hpp):
//   fragment programs: 1 flat colours (COL0 and COL1, and the back colours, not interpolated: each primitive's provoking
//     vertex's, which the renderer puts first), for SET_SHADE_MODE FLAT; 2 two-sided colours (on a back face COL0 and COL1
//     are the vertex program's back colours, BFC0 and BFC1 at locations 3 and 4), for SET_TWO_SIDE_LIGHT_EN; 3 both;
//   vertex programs: 1 clip distances (ClipDistance[6], for SET_USER_CLIP_PLANE_CONTROL: plane n's distance is the
//     program's output FOGC's y, z, w for planes 0 to 2 and PSZ's for 3 to 5, *a stated reading of NV40's, as Mesa's
//     driver writes them*; each plane's test is specialisation constant n, 0 off, 1 kept where its distance is below 0
//     (LT), 2 where it is 0 or more (GE): the pipeline's state, so one module serves every combination).
struct Variant {
    bool flat = false;
    bool two_sided = false;
    bool clip = false;
};
constexpr unsigned variant_number(const Variant& v) { return (v.flat || v.clip ? 1u : 0u) | (v.two_sided ? 2u : 0u); }
constexpr Variant variant_of(bool vertex, unsigned number) {
    return vertex ? Variant{false, false, number == 1} : Variant{(number & 1) != 0, (number & 2) != 0, false};
}
constexpr unsigned kVariants = 4;  // numbers 0 (the plain form) to 3

struct Translation {
    std::vector<uint32_t> spirv;
    uint32_t inputs = 0;   // vertex: attributes read; fragment: input registers read (bit n for input n)
    uint32_t outputs = 0;  // vertex: output registers written; fragment: colour targets written (bit 0 to 3)
    uint32_t textures = 0, cube_textures = 0;  // fragment: units sampled, and which of them are cubes
    uint32_t clip_planes = 0;                  // vertex: the user clip planes whose distances it writes (bit n for plane n)
    std::vector<uint32_t> constant_slots;      // fragment: each inline constant's offset in the microcode, in bytes
};

std::expected<Translation, std::string> translate(const CgProgram& program, const Variant& variant = {});

// The key the renderer finds a translated program by: a hash (64-bit FNV-1a) of its microcode, with a fragment
// program's inline constants left out (the game patches them; their values come to the shader another way).
uint64_t program_key(const CgProgram& program);

}  // namespace qilin::rsx

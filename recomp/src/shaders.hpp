// The game's shaders: compiled Cg programs for the RSX, which the program
// carries in its data.
//
// Each is a "Cg binary program": a 32-byte header of big-endian words (the
// profile, 7003 for a vertex program and 7004 for a fragment program; the
// format's revision; the whole program's size; the number of parameters and
// where their table is; where the program's own header is; the size of the
// microcode and where it is), then the parameters with their names and
// semantics, then the microcode the RSX runs. The layout is NVIDIA's,
// published with the Cg toolkit. This finds every one in a program's loaded
// data and checks its header; translating them to SPIR-V at build time is
// the recompiler's later work (docs/roadmap-and-design-targets.md).
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "elf.hpp"

namespace qilin {

inline constexpr uint32_t kCgVertexProfile = 7003, kCgFragmentProfile = 7004, kCgRevision = 6;

struct ShaderProgram {
    uint32_t address = 0;  // in the PPU program's memory
    uint32_t profile = 0, size = 0;
    uint32_t parameters = 0, parameters_at = 0, program_at = 0;  // offsets are from the header
    uint32_t microcode_size = 0, microcode_at = 0;
    bool vertex() const { return profile == kCgVertexProfile; }
};

// Every compiled program in the loaded data, in address order. A header of
// the right profile and revision whose fields don't hold up is an error.
std::expected<std::vector<ShaderProgram>, std::string> find_shader_programs(const Elf& elf);

}  // namespace qilin

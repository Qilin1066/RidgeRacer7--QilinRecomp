#include "shaders.hpp"

#include <format>

#include "bytes.hpp"

namespace qilin {

std::expected<std::vector<ShaderProgram>, std::string> find_shader_programs(const Elf& elf) {
    constexpr uint32_t kHeader = 32;
    std::vector<ShaderProgram> programs;
    for (const ElfSegment& segment : elf.segments()) {
        if (segment.type != kPtLoad || (segment.flags & kPfExec) || segment.file_size < kHeader) continue;
        const auto bytes = elf.bytes_at(segment.address, segment.file_size);
        if (bytes.empty()) continue;
        for (size_t at = 0; at + kHeader <= bytes.size(); at += 4) {
            const uint8_t* p = bytes.data() + at;
            const uint32_t profile = read_be32(p);
            if ((profile != kCgVertexProfile && profile != kCgFragmentProfile) || read_be32(p + 4) != kCgRevision) continue;
            ShaderProgram s;
            s.address = static_cast<uint32_t>(segment.address + at);
            s.profile = profile;
            s.size = read_be32(p + 8);
            s.parameters = read_be32(p + 12);
            s.parameters_at = read_be32(p + 16);
            s.program_at = read_be32(p + 20);
            s.microcode_size = read_be32(p + 24);
            s.microcode_at = read_be32(p + 28);
            // Two words of the right values can be chance; a size that fits nothing settles it.
            if (s.size < kHeader || s.size % 4 || s.size > bytes.size() - at) continue;
            const auto fail = [&](std::string_view what) {
                return std::unexpected(std::format("the compiled shader at {:#x} ({} bytes): {}", s.address, s.size, what));
            };
            if (s.parameters_at < kHeader || s.parameters_at > s.size) return fail("its parameters are outside it");
            if (s.program_at < kHeader || s.program_at >= s.size) return fail("its program header is outside it");
            if (s.microcode_size == 0 || s.microcode_size % 4 || s.microcode_at < kHeader || uint64_t{s.microcode_at} + s.microcode_size > s.size)
                return fail("its microcode is outside it");
            programs.push_back(s);
            at += s.size - 4;
        }
    }
    return programs;
}

}  // namespace qilin

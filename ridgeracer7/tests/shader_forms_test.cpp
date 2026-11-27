// The translated programs' other forms for draw state (rsx_spirv.hpp's Variant), on synthetic programs built field by
// field from the documented layout: what each form declares that the plain one doesn't. No game files; the modules are
// given to spirv-val where the game is (tools/check/validate_shaders.py) and here only read back. With
// RR7_FORMS_OUT=<folder> set, each module is also written there as a .spv file, for spirv-val by hand.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "rsx_program.hpp"
#include "rsx_spirv.hpp"

namespace {

using namespace qilin::rsx;

int failures = 0;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

void put_be32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
}

// A module's instructions: each as its words (opcode and count first).
std::vector<std::vector<uint32_t>> instructions(const std::vector<uint32_t>& spirv) {
    std::vector<std::vector<uint32_t>> out;
    for (size_t at = 5; at < spirv.size();) {
        const uint32_t count = spirv[at] >> 16;
        if (count == 0 || at + count > spirv.size()) break;
        out.emplace_back(spirv.begin() + static_cast<std::ptrdiff_t>(at), spirv.begin() + static_cast<std::ptrdiff_t>(at + count));
        at += count;
    }
    return out;
}
// The ids decorated with `decoration` (OpDecorate, 71), and those given a location (30) of `location`.
std::vector<uint32_t> decorated(const std::vector<uint32_t>& spirv, uint32_t decoration) {
    std::vector<uint32_t> ids;
    for (const auto& in : instructions(spirv))
        if ((in[0] & 0xffff) == 71 && in.size() >= 3 && in[2] == decoration) ids.push_back(in[1]);
    return ids;
}
uint32_t at_location(const std::vector<uint32_t>& spirv, uint32_t location) {
    for (const auto& in : instructions(spirv))
        if ((in[0] & 0xffff) == 71 && in.size() >= 4 && in[2] == 30 && in[3] == location) return in[1];
    return 0;
}
bool has(const std::vector<uint32_t>& ids, uint32_t id) {
    for (const uint32_t i : ids)
        if (i == id) return true;
    return false;
}

void write_out(const char* name, const std::vector<uint32_t>& spirv) {
    const char* folder = std::getenv("RR7_FORMS_OUT");
    if (!folder) return;
    const std::string path = std::string(folder) + "/" + name + ".spv";
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(spirv.data(), 4, spirv.size(), f);
        std::fclose(f);
    }
}

// MOV R0, f[COL0] (or COL1, or TEX0), the end: a fragment program that reads one input.
CgProgram fragment_reading(uint32_t input) {
    const auto source = [](uint32_t type, uint32_t index, uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
        return type | (index << 2) | (x << 9) | (y << 11) | (z << 13) | (w << 15);
    };
    CgProgram fp;
    fp.header = {2, 0, 0, 1u << input, 0, 0, 0};
    const uint32_t words[] = {(uint32_t{kFpMov} << 24) | (input << 13) | (0xfu << 9) | 1u, source(1, 0, 0, 1, 2, 3) | (7u << 18) | (0xe4u << 21), 0, 0};
    for (const uint32_t v : words) put_be32(fp.microcode, (v << 16) | (v >> 16));
    return fp;
}

void test_flat() {
    // The flat form: COL0 (location 1) decorated Flat, and not without perspective; the plain form neither.
    const CgProgram fp = fragment_reading(1);
    const auto plain = translate(fp);
    const auto flat = translate(fp, Variant{.flat = true});
    CHECK(plain && flat);
    if (!plain || !flat) return;
    write_out("fs_col0", plain->spirv);
    write_out("fs_col0_flat", flat->spirv);
    CHECK(plain->inputs == 2 && flat->inputs == 2);
    CHECK(decorated(plain->spirv, 14).empty());
    const uint32_t col0 = at_location(flat->spirv, 1);
    CHECK(col0 != 0 && has(decorated(flat->spirv, 14), col0) && !has(decorated(flat->spirv, 13), col0));
    // A texture coordinate is never made flat.
    const auto tex = translate(fragment_reading(4), Variant{.flat = true});
    CHECK(tex && decorated(tex->spirv, 14).empty());
    // The form's number, for the shader table.
    CHECK(variant_number(Variant{.flat = true}) == 1 && variant_of(false, 1).flat && !variant_of(true, 1).flat);
}

}  // namespace

int main() {
    test_flat();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("shader form tests passed");
    return 0;
}

// The translated programs' other forms for draw state (rsx_spirv.hpp's Variant), on synthetic programs built field by
// field from the documented layout: what each form declares that the plain one doesn't. No game files; the modules are
// given to spirv-val where the game is (tools/check/validate_shaders.py) and here only read back. With
// RR7_FORMS_OUT=<folder> set, each module is also written there as a .spv file, for spirv-val by hand.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <utility>
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

// Whether the module has a built-in input of this number (OpDecorate BuiltIn, 11).
bool has_built_in(const std::vector<uint32_t>& spirv, uint32_t built_in) {
    for (const auto& in : instructions(spirv))
        if ((in[0] & 0xffff) == 71 && in.size() >= 4 && in[2] == 11 && in[3] == built_in) return true;
    return false;
}

// A vertex program of MOVs from v[0], one an instruction: to each output register given, with its mask (x = 8).
CgProgram vertex_writing(std::initializer_list<std::pair<uint32_t, uint32_t>> writes) {
    const uint32_t s0 = 2u | (0x1bu << 8);  // v[...], swizzle xyzw
    CgProgram vp;
    vp.vertex = true;
    vp.header = {2, 0, 1, 1, 0, 0};
    size_t i = 0;
    for (const auto& [dest, mask] : writes) {
        const bool last = ++i == writes.size();
        const uint32_t w[4] = {(1u << 30) | (0x3fu << 15) | (7u << 10) | (0x1bu << 2), (uint32_t{kVecMov} << 22) | (s0 >> 9), (s0 & 0x1ff) << 23,
                               (mask << 13) | (dest << 2) | (last ? 1u : 0u)};
        for (const uint32_t v : w) put_be32(vp.microcode, v);
    }
    return vp;
}

void test_two_sided() {
    // The two-sided form: the back colour at location 3 (BFC0) for COL0, 4 (BFC1) for COL1, and FrontFacing to choose.
    const auto plain = translate(fragment_reading(1));
    const auto two = translate(fragment_reading(1), Variant{.two_sided = true});
    const auto two1 = translate(fragment_reading(2), Variant{.two_sided = true});
    const auto both = translate(fragment_reading(1), Variant{.flat = true, .two_sided = true});
    CHECK(plain && two && two1 && both);
    if (!plain || !two || !two1 || !both) return;
    write_out("fs_col0_two_sided", two->spirv);
    write_out("fs_col0_flat_two_sided", both->spirv);
    CHECK(!has_built_in(plain->spirv, 17) && at_location(plain->spirv, 3) == 0);
    CHECK(has_built_in(two->spirv, 17) && at_location(two->spirv, 1) != 0 && at_location(two->spirv, 3) != 0 && at_location(two->spirv, 4) == 0);
    CHECK(has_built_in(two1->spirv, 17) && at_location(two1->spirv, 4) != 0 && at_location(two1->spirv, 3) == 0);
    // Both: front and back colours flat.
    const std::vector<uint32_t> flat = decorated(both->spirv, 14);
    CHECK(has(flat, at_location(both->spirv, 1)) && has(flat, at_location(both->spirv, 3)));
    // A program reading no colour: nothing to choose.
    const auto tex = translate(fragment_reading(4), Variant{.two_sided = true});
    CHECK(tex && !has_built_in(tex->spirv, 17));
    // A vertex program writing a colour and its back colour: outputs 1 and 3 (for a pipeline with the two-sided forms).
    const auto back = translate(vertex_writing({{0, 0xf}, {1, 0xf}, {3, 0xf}}));
    CHECK(back && back->outputs == 0xbu);
    if (back) write_out("vs_back", back->spirv);
    if (const auto tex0 = translate(fragment_reading(4))) write_out("fs_tex0", tex0->spirv);
    CHECK(variant_number(Variant{.flat = true, .two_sided = true}) == 3 && variant_of(false, 2).two_sided && !variant_of(false, 2).flat);
}

bool has_capability(const std::vector<uint32_t>& spirv, uint32_t capability) {
    for (const auto& in : instructions(spirv))
        if ((in[0] & 0xffff) == 17 && in.size() == 2 && in[1] == capability) return true;
    return false;
}

void test_clip() {
    // Plane 0's distance is FOGC's y, plane 5's PSZ's w: what the program writes of them is its clip planes.
    const CgProgram vp = vertex_writing({{0, 0xf}, {5, 0x4}, {6, 0x1}, {7, 0xf}});
    const auto plain = translate(vp);
    const auto clip = translate(vp, Variant{.clip = true});
    CHECK(plain && clip);
    if (!plain || !clip) return;
    write_out("vs_clip_plain", plain->spirv);
    write_out("vs_clip", clip->spirv);
    CHECK(plain->clip_planes == ((1u << 0) | (1u << 5)) && clip->clip_planes == plain->clip_planes);
    CHECK(translate(vertex_writing({{0, 0xf}, {5, 0x8}}))->clip_planes == 0);  // FOGC's x is the fog, no plane's
    CHECK(translate(vertex_writing({{0, 0xf}, {5, 0x3}}))->clip_planes == 0x6);
    // The clip form: ClipDistance (its capability and built-in), and a test for each plane as specialisation constants 0 to 5.
    CHECK(!has_capability(plain->spirv, 32) && !has_built_in(plain->spirv, 3) && decorated(plain->spirv, 1).empty());
    CHECK(has_capability(clip->spirv, 32) && has_built_in(clip->spirv, 3) && decorated(clip->spirv, 1).size() == 6);
    // Forms that don't apply are refused.
    CHECK(!translate(vp, Variant{.flat = true}) && !translate(fragment_reading(1), Variant{.clip = true}));
    CHECK(variant_number(Variant{.clip = true}) == 1 && variant_of(true, 1).clip && !variant_of(true, 2).clip && !variant_of(false, 1).clip);
}

}  // namespace

int main() {
    test_flat();
    test_two_sided();
    test_clip();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("shader form tests passed");
    return 0;
}

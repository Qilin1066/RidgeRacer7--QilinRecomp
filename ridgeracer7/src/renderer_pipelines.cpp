#include "renderer_pipelines.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include "render_formats.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;
namespace host = qilin::host;

}  // namespace

VkShaderModule PipelineCache::module_of(const qilin::rt::TranslatedShader& s, unsigned form) {
    const std::lock_guard lock(modules_mutex_);
    auto& m = modules_[&s][form];
    if (!m) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = (form ? s.variant_words[form] : s.spirv_words) * 4;
        info.pCode = form ? s.variant_spirv[form] : s.spirv;
        if (!info.pCode) fatal("the %s program at %08x has no form %u", s.vertex ? "vertex" : "fragment", s.address, form);
        host::check(vkCreateShaderModule(gpu_->device, &info, nullptr, &m), "vkCreateShaderModule");
    }
    return m;
}

namespace {

VkBlendFactor blend_factor(uint32_t f) {
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: fatal("blend factor %#x, which the renderer doesn't take yet", f);
    }
}
VkBlendOp blend_op(uint32_t e) {
    switch (e) {
    case 0x8006: return VK_BLEND_OP_ADD;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800a: return VK_BLEND_OP_SUBTRACT;
    case 0x800b: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: fatal("blend equation %#x, which the renderer doesn't take yet", e);
    }
}

VkSamplerAddressMode address_mode(uint32_t mode) {
    switch (mode) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
}

}  // namespace

VkSampler PipelineCache::sampler(uint32_t unit, const SamplerFields& s, uint32_t levels) {
    const std::array<uint32_t, 12> key{s.mag, s.min, s.mip, s.wrap[0], s.wrap[1], s.wrap[2], std::bit_cast<uint32_t>(s.bias), std::bit_cast<uint32_t>(s.min_lod),
                                       std::bit_cast<uint32_t>(s.max_lod), s.anisotropy, s.border, levels > 1};
    VkSampler& made = samplers_[key];
    if (made) return made;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = s.mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = s.min ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = s.mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = address_mode(s.wrap[0]), info.addressModeV = address_mode(s.wrap[1]), info.addressModeW = address_mode(s.wrap[2]);
    info.mipLodBias = s.bias;
    // With no mipmapping, level 0 alone (Vulkan's way of saying so: a range of a quarter).
    info.minLod = s.mip ? s.min_lod : 0.0f;
    info.maxLod = s.mip ? s.max_lod : 0.25f;
    if (s.anisotropy > 1) {
        if (gpu_->max_anisotropy < 1.0f) fatal("texture %u is sampled anisotropically, and this GPU can't", unit);
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(static_cast<float>(s.anisotropy), gpu_->max_anisotropy);
    }
    if (s.wrap[0] == 4 || s.wrap[1] == 4 || s.wrap[2] == 4) {
        // The border colour (A, R, G, B): Vulkan's three standard ones are taken.
        switch (s.border) {
        case 0x00000000u: info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
        case 0xff000000u: info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
        case 0xffffffffu: info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
        default: fatal("texture %u's border colour %08x, which the renderer doesn't take yet", unit, s.border);
        }
    }
    host::check(vkCreateSampler(gpu_->device, &info, nullptr, &made), "vkCreateSampler");
    return made;
}

namespace {

VkCompareOp compare_op(uint32_t f) { return static_cast<VkCompareOp>(f - 0x200); }  // OpenGL's order is Vulkan's

VkStencilOp stencil_op(uint32_t op) {
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1e00: return VK_STENCIL_OP_KEEP;
    case 0x1e01: return VK_STENCIL_OP_REPLACE;
    case 0x1e02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1e03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150a: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: fatal("a stencil operation %#x, which the renderer doesn't know", op);
    }
}
VkStencilOpState stencil_state(const Draw::Stencil& s) {
    VkStencilOpState v{};
    v.failOp = stencil_op(s.fail), v.depthFailOp = stencil_op(s.zfail), v.passOp = stencil_op(s.zpass);
    v.compareOp = compare_op(s.func), v.compareMask = s.func_mask, v.writeMask = s.write_mask, v.reference = s.ref;
    return v;
}

static_assert(VK_LOGIC_OP_CLEAR == 0 && VK_LOGIC_OP_COPY == 3 && VK_LOGIC_OP_XOR == 6 && VK_LOGIC_OP_SET == 15, "Vulkan lists the logic operations in OpenGL's order (logic_op)");

// A topology's class, which a pipeline fixes (Vulkan 1.3 sets the topology per draw only within one), and the class's list
// topology a pipeline is made with.
uint32_t topology_class(VkPrimitiveTopology t) {
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return 0;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return 1;
    default: return 2;
    }
}
constexpr VkPrimitiveTopology kClassTopology[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};

uint64_t fnv64(const void* data, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const auto* b = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// The files kept in the data folder: the driver's pipeline cache behind a header of ours (the device and driver it was
// written by, its length and hash: a cache from another device or driver, or a damaged one, is not given to the driver), and
// the keys of the pipelines made (behind their layout's version, their count and hash).
constexpr char kCacheMagic[8] = {'R', 'R', '7', 'P', 'C', 'A', 'C', 'H'};
constexpr char kKeysMagic[8] = {'R', 'R', '7', 'P', 'K', 'E', 'Y', 'S'};
constexpr uint32_t kCacheVersion = 1;
constexpr const char* kCacheFile = "pipelines.cache";
constexpr const char* kKeysFile = "pipelines.keys";

struct CacheHeader {
    char magic[8];
    uint32_t version, vendor, device, driver;
    uint8_t uuid[VK_UUID_SIZE];
    uint64_t bytes, hash;
};
CacheHeader cache_header(const host::Gpu& gpu, uint64_t bytes, uint64_t hash) {
    CacheHeader h{};
    std::memcpy(h.magic, kCacheMagic, sizeof h.magic);
    h.version = kCacheVersion, h.vendor = gpu.properties.vendorID, h.device = gpu.properties.deviceID, h.driver = gpu.properties.driverVersion;
    std::memcpy(h.uuid, gpu.properties.pipelineCacheUUID, VK_UUID_SIZE);
    h.bytes = bytes, h.hash = hash;
    return h;
}
struct KeysHeader {
    char magic[8];
    uint32_t version, count;
    uint64_t hash;
};

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
// Written whole or not at all: to a file beside it, then put in its place.
bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::path partial = path;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    std::error_code e;
    std::filesystem::rename(partial, path, e);
    return !e;
}
template <class T>
void append(std::vector<uint8_t>& out, const T& v) {
    const auto* b = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof v);
}

}  // namespace

VkPrimitiveTopology vulkan_topology(Topology t) {
    switch (t) {
    case Topology::points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case Topology::lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case Topology::line_strip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case Topology::triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case Topology::triangle_strip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VertexInput vertex_input(uint8_t f) {
    if (f == kVertexFloats) return {};
    if (f == kVertexConstant) return {VK_FORMAT_R32G32B32A32_SFLOAT, 16, true};
    // By the RSX's type (S1, F, SF, UB, S32K, -, UB256) and size (three 8- and 16-bit components given as four).
    static constexpr VkFormat kFormats[8][4] = {
        {},
        {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM},
        {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT},
        {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
        {VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED},
        {},
        {VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_USCALED},
    };
    if (!vertex_format_native(f)) fatal("a vertex format %02x, which the renderer doesn't make", f);
    return {kFormats[f & 0xf][(f >> 4) - 1], native_stride(f), false};
}

uint8_t PipelineCache::vertex_format(uint32_t format_word) const {
    const uint32_t type = format_word & 0xf, size = (format_word >> 4) & 0xf;
    if (size == 0) return kVertexConstant;
    const auto f = static_cast<uint8_t>(type | size << 4);
    return vertex_format_native(f) && native_[f] ? f : kVertexFloats;
}

DynamicState dynamic_state(const Draw& d, VkPrimitiveTopology topology, const Rasterisation& rasterising, bool gpu_depth_bounds) {
    DynamicState s;
    s.topology = topology;
    // Culling (OpenGL's faces: front 0x404, back 0x405, both 0x408; windings: clockwise 0x900, anticlockwise 0x901). The
    // winding is in window coordinates, which count down from the top as Vulkan's framebuffer does (rsx.cpp: the window's
    // origin), so the RSX's anticlockwise is Vulkan's (*a stated reading, held to the picture*).
    if (d.cull) {
        switch (d.cull_face) {
        case 0x404: s.cull = VK_CULL_MODE_FRONT_BIT; break;
        case 0x405: s.cull = VK_CULL_MODE_BACK_BIT; break;
        case 0x408: s.cull = VK_CULL_MODE_FRONT_AND_BACK; break;
        default: fatal("culling of face %#x", d.cull_face);
        }
    }
    if (d.front_face != 0x900 && d.front_face != 0x901 && d.cull) fatal("a front face of %#x", d.front_face);
    s.front = d.front_face == 0x900 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (rasterising.depth_bias) s.bias = VK_TRUE, s.bias_slope = d.bias_factor, s.bias_constant = d.bias_units;
    if (d.depth_test) {
        s.depth_test = VK_TRUE;
        s.depth_write = d.depth_write ? VK_TRUE : VK_FALSE;
        s.depth_compare = compare_op(d.depth_func);
    }
    if (d.depth_bounds) {
        if (!gpu_depth_bounds) fatal("a draw with the depth bounds test, which this GPU doesn't have");
        // Vulkan takes bounds within [0, 1] only; the depth stored is within it, so clamping them changes no result.
        s.bounds_test = VK_TRUE;
        s.bounds_min = std::clamp(d.bounds_min, 0.0f, 1.0f), s.bounds_max = std::clamp(d.bounds_max, 0.0f, 1.0f);
    }
    if (d.stencil_test) {
        s.stencil_test = VK_TRUE;
        s.stencil_front = stencil_state(d.stencil_front), s.stencil_back = stencil_state(d.stencil_back);
    }
    return s;
}

void set_dynamic_state(VkCommandBuffer cmd, const DynamicState& want, const DynamicState* have, bool bounds_dynamic) {
    if (!have || have->topology != want.topology) vkCmdSetPrimitiveTopology(cmd, want.topology);
    if (!have || have->cull != want.cull) vkCmdSetCullMode(cmd, want.cull);
    if (!have || have->front != want.front) vkCmdSetFrontFace(cmd, want.front);
    if (!have || have->depth_test != want.depth_test) vkCmdSetDepthTestEnable(cmd, want.depth_test);
    if (!have || have->depth_write != want.depth_write) vkCmdSetDepthWriteEnable(cmd, want.depth_write);
    if (!have || have->depth_compare != want.depth_compare) vkCmdSetDepthCompareOp(cmd, want.depth_compare);
    if (bounds_dynamic) {
        if (!have || have->bounds_test != want.bounds_test) vkCmdSetDepthBoundsTestEnable(cmd, want.bounds_test);
        if (!have || have->bounds_min != want.bounds_min || have->bounds_max != want.bounds_max) vkCmdSetDepthBounds(cmd, want.bounds_min, want.bounds_max);
    }
    if (!have || have->bias != want.bias) vkCmdSetDepthBiasEnable(cmd, want.bias);
    if (!have || have->bias_constant != want.bias_constant || have->bias_slope != want.bias_slope) vkCmdSetDepthBias(cmd, want.bias_constant, 0.0f, want.bias_slope);
    if (!have || have->stencil_test != want.stencil_test) vkCmdSetStencilTestEnable(cmd, want.stencil_test);
    // Each face's operations, masks and reference, the two faces in one command where they agree.
    const auto faces = [&](auto same, auto set) {
        const bool front = !have || !same(have->stencil_front, want.stencil_front), back = !have || !same(have->stencil_back, want.stencil_back);
        if (front && back && same(want.stencil_front, want.stencil_back)) set(VK_STENCIL_FACE_FRONT_AND_BACK, want.stencil_front);
        else {
            if (front) set(VK_STENCIL_FACE_FRONT_BIT, want.stencil_front);
            if (back) set(VK_STENCIL_FACE_BACK_BIT, want.stencil_back);
        }
    };
    faces([](const VkStencilOpState& a, const VkStencilOpState& b) { return a.failOp == b.failOp && a.passOp == b.passOp && a.depthFailOp == b.depthFailOp && a.compareOp == b.compareOp; },
          [&](VkStencilFaceFlags f, const VkStencilOpState& s) { vkCmdSetStencilOp(cmd, f, s.failOp, s.passOp, s.depthFailOp, s.compareOp); });
    faces([](const VkStencilOpState& a, const VkStencilOpState& b) { return a.compareMask == b.compareMask; },
          [&](VkStencilFaceFlags f, const VkStencilOpState& s) { vkCmdSetStencilCompareMask(cmd, f, s.compareMask); });
    faces([](const VkStencilOpState& a, const VkStencilOpState& b) { return a.writeMask == b.writeMask; },
          [&](VkStencilFaceFlags f, const VkStencilOpState& s) { vkCmdSetStencilWriteMask(cmd, f, s.writeMask); });
    faces([](const VkStencilOpState& a, const VkStencilOpState& b) { return a.reference == b.reference; },
          [&](VkStencilFaceFlags f, const VkStencilOpState& s) { vkCmdSetStencilReference(cmd, f, s.reference); });
}

PipelineKey PipelineCache::key(const Draw& d, const qilin::rt::TranslatedShader& vs, const qilin::rt::TranslatedShader& fs, const Forms& forms, VkFormat format,
                               VkPrimitiveTopology topology, const Rasterisation& rasterising, const std::array<uint8_t, 16>& vertex_formats) const {
    uint32_t logic = 0;
    if (d.logic) {
        // In place of blending, in Vulkan as on the RSX. (Vulkan applies none to a float surface.)
        const std::optional<uint32_t> op = logic_op(d.logic_op);
        if (!op) fatal("a logic operation %#x, which the renderer doesn't know", d.logic_op);
        if (!gpu_->logic_op) fatal("a draw with a logic operation, which this GPU can't (logicOp)");
        if (format == VK_FORMAT_R32_SFLOAT) fatal("a logic operation on a float surface, which Vulkan doesn't apply");
        logic = *op;
    }
    // The RSX's colour mask is blue bit 0, green 8, red 16, alpha 24.
    constexpr VkColorComponentFlags kR = VK_COLOR_COMPONENT_R_BIT, kG = VK_COLOR_COMPONENT_G_BIT, kB = VK_COLOR_COMPONENT_B_BIT,
                                    kA = VK_COLOR_COMPONENT_A_BIT, kNone = 0;
    const uint32_t write_mask = ((d.color_mask >> 16) & 1 ? kR : kNone) | ((d.color_mask >> 8) & 1 ? kG : kNone) |
                                (d.color_mask & 1 ? kB : kNone) | ((d.color_mask >> 24) & 1 ? kA : kNone);
    PipelineKey k{};
    k[0] = static_cast<uint32_t>(vs.key), k[1] = static_cast<uint32_t>(vs.key >> 32), k[2] = static_cast<uint32_t>(fs.key), k[3] = static_cast<uint32_t>(fs.key >> 32);
    k[4] = forms.vertex | forms.fragment << 8 | topology_class(topology) << 16 | static_cast<uint32_t>(rasterising.mode) << 20 | uint32_t{d.depth_surface} << 24 |
           uint32_t{d.blend} << 25 | uint32_t{d.logic} << 26;
    k[5] = forms.vertex == 1 ? d.clip_planes : 0;
    k[6] = static_cast<uint32_t>(format);
    k[7] = write_mask;
    if (d.blend) k[8] = d.blend_src, k[9] = d.blend_dst, k[10] = d.blend_equation;
    k[11] = logic;
    for (uint32_t a = 0; a < 16; ++a) k[12 + a / 4] |= uint32_t{vertex_formats[a]} << (8 * (a % 4));
    return k;
}

VkPipeline PipelineCache::make(const PipelineKey& k, bool recorded) {
    const auto shader = [&](uint32_t lo, uint32_t hi, bool vertex) -> const qilin::rt::TranslatedShader* {
        const auto it = shaders_.find(uint64_t{lo} | uint64_t{hi} << 32);
        if (it == shaders_.end() || it->second->vertex != vertex) {
            if (!recorded) fatal("a pipeline's %s program (key %08x%08x) isn't among the translated", vertex ? "vertex" : "fragment", hi, lo);
            return nullptr;
        }
        return it->second;
    };
    const qilin::rt::TranslatedShader* vs = shader(k[0], k[1], true);
    const qilin::rt::TranslatedShader* fs = shader(k[2], k[3], false);
    const unsigned vertex_form = k[4] & 0xff, fragment_form = (k[4] >> 8) & 0xff, topology = (k[4] >> 16) & 0xf;
    const auto mode = static_cast<PolygonMode>((k[4] >> 20) & 0xf);
    const bool depth_attachment = (k[4] >> 24) & 1, blend_on = (k[4] >> 25) & 1, logic_on = (k[4] >> 26) & 1;
    if (recorded) {
        // (A key written by an earlier run, perhaps on another device: made only if this one can.)
        if (!vs || !fs || vertex_form > 3 || fragment_form > 3 || topology > 2 || (vertex_form && !vs->variant_spirv[vertex_form]) ||
            (fragment_form && !fs->variant_spirv[fragment_form]) || (logic_on && !gpu_->logic_op) || (mode != PolygonMode::fill && !gpu_->fill_non_solid) ||
            (vertex_form == 1 && !gpu_->clip_distance) || (k[6] != VK_FORMAT_R8G8B8A8_UNORM && k[6] != VK_FORMAT_R32_SFLOAT))
            return VK_NULL_HANDLE;
        for (uint32_t a = 0; a < 16; ++a)
            if (const uint8_t f = static_cast<uint8_t>(k[12 + a / 4] >> (8 * (a % 4))); f && !vertex_format_usable(f)) return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = module_of(*vs, vertex_form), stages[0].pName = "main";
    // The clip form's planes' tests, as its specialisation constants.
    std::array<int32_t, 6> clip{};
    for (uint32_t n = 0; n < 6; ++n) clip[n] = static_cast<int32_t>((k[5] >> (4 * n)) & 0xf);
    std::array<VkSpecializationMapEntry, 6> clip_entries{};
    for (uint32_t n = 0; n < 6; ++n) clip_entries[n] = {n, n * 4, 4};
    const VkSpecializationInfo clip_info{6, clip_entries.data(), sizeof clip, clip.data()};
    if (vertex_form == 1) stages[0].pSpecializationInfo = &clip_info;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = module_of(*fs, fragment_form), stages[1].pName = "main";
    std::array<VkVertexInputBindingDescription, 16> bindings{};
    std::array<VkVertexInputAttributeDescription, 16> attributes{};
    uint32_t inputs = 0;
    for (uint32_t a = 0; a < 16; ++a)
        if (const uint8_t f = static_cast<uint8_t>(k[12 + a / 4] >> (8 * (a % 4)))) {
            const VertexInput in = vertex_input(f);
            bindings[inputs] = {a, in.stride, in.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
            attributes[inputs] = {a, a, static_cast<VkFormat>(in.format), 0};
            ++inputs;
        }
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = inputs, vertex.pVertexBindingDescriptions = bindings.data();
    vertex.vertexAttributeDescriptionCount = inputs, vertex.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = kClassTopology[topology];  // (the draw's own, of this class, is dynamic)
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1, viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.lineWidth = 1.0f;
    raster.polygonMode = mode == PolygonMode::line ? VK_POLYGON_MODE_LINE : mode == PolygonMode::point ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = k[7];
    if (blend_on) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = blend_factor(k[8] & 0xffff), blend.srcAlphaBlendFactor = blend_factor(k[8] >> 16);
        blend.dstColorBlendFactor = blend_factor(k[9] & 0xffff), blend.dstAlphaBlendFactor = blend_factor(k[9] >> 16);
        blend.colorBlendOp = blend_op(k[10] & 0xffff), blend.alphaBlendOp = blend_op(k[10] >> 16);
    }
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1, blending.pAttachments = &blend;
    if (logic_on) blending.logicOpEnable = VK_TRUE, blending.logicOp = static_cast<VkLogicOp>(k[11]);
    // Everything extended dynamic state 1 and 2 set per draw (set_dynamic_state), but primitive restart, which is never on (render_state.hpp: assemble makes
    // lists of a draw that restarts); the depth bounds only where the GPU has the test.
    std::array<VkDynamicState, 20> dynamic_states{};
    uint32_t dynamic_count = 0;
    for (const VkDynamicState s : {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_CULL_MODE,
                                   VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                   VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
                                   VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE})
        dynamic_states[dynamic_count++] = s;
    if (bounds_dynamic()) dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS;
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = dynamic_count, dynamic.pDynamicStates = dynamic_states.data();
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    const VkFormat format = static_cast<VkFormat>(k[6]);
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1, rendering.pColorAttachmentFormats = &format;
    if (depth_attachment) rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat = depth_format_;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2, info.pStages = stages;
    info.pVertexInputState = &vertex, info.pInputAssemblyState = &assembly, info.pViewportState = &viewport;
    info.pRasterizationState = &raster, info.pMultisampleState = &multisample, info.pColorBlendState = &blending, info.pDynamicState = &dynamic;
    info.pDepthStencilState = &depth;
    info.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    host::check(vkCreateGraphicsPipelines(gpu_->device, vk_cache_, 1, &info, nullptr, &p), "vkCreateGraphicsPipelines");
    return p;
}

VkPipeline PipelineCache::pipeline(const PipelineKey& key) {
    // (A draw like the one before, as most are: its pipeline without a look in the table.)
    if (last_pipeline_ && key == last_key_) return last_pipeline_;
    std::unique_lock lock(mutex_);
    for (bool waited = false;;) {
        const auto it = pipelines_.find(key);
        if (it == pipelines_.end()) break;
        if (it->second) {
            last_key_ = key;
            return last_pipeline_ = it->second;
        }
        // (The worker is making it.)
        if (!waited) ++waited_, waited = true;
        made_.wait(lock);
    }
    pipelines_.emplace(key, VK_NULL_HANDLE);
    lock.unlock();
    const VkPipeline p = make(key, false);
    lock.lock();
    pipelines_[key] = p;
    made_.notify_all();
    ++made_drawing_, ++made_since_save_;
    last_key_ = key;
    return last_pipeline_ = p;
}

void PipelineCache::start(const host::Gpu& gpu, VkPipelineLayout layout, VkFormat depth_format, const std::filesystem::path& folder) {
    gpu_ = &gpu, layout_ = layout, depth_format_ = depth_format, folder_ = folder;
    for (size_t i = 0; i < qilin::rt::kTranslatedShaderCount; ++i) shaders_[qilin::rt::kTranslatedShaders[i].key] = &qilin::rt::kTranslatedShaders[i];
    // The attributes' formats the GPU reads from a vertex buffer: the floats every attribute can be given as must be there;
    // the attributes' own formats are used where they are (each one missing named, and given as floats).
    const auto readable = [&](VkFormat f) {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(gpu.physical, f, &p);
        return (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    };
    if (!readable(VK_FORMAT_R32G32B32A32_SFLOAT)) fatal("the GPU reads no vertices of four floats (VK_FORMAT_R32G32B32A32_SFLOAT)");
    static constexpr const char* kTypes[8] = {"", "S1", "F", "SF", "UB", "S32K", "", "UB256"};
    for (uint32_t type = 1; type < 8; ++type)
        for (uint32_t size = 1; size <= 4; ++size) {
            const auto f = static_cast<uint8_t>(type | size << 4);
            if (!vertex_format_native(f)) continue;
            native_[f] = readable(vertex_input(f).format);
            if (!native_[f]) qilin::rt::log("the GPU reads no vertices of Vulkan format %d: the RSX's %s x%u is given to it as floats", static_cast<int>(vertex_input(f).format), kTypes[type], size);
        }
    // The driver's cache as an earlier run left it, if this device and driver wrote it and it is whole.
    std::vector<uint8_t> data;
    if (!folder_.empty()) {
        std::vector<uint8_t> file = read_file(folder_ / kCacheFile);
        if (file.size() >= sizeof(CacheHeader)) {
            CacheHeader h;
            std::memcpy(&h, file.data(), sizeof h);
            const uint64_t bytes = file.size() - sizeof h;
            const CacheHeader want = cache_header(gpu, bytes, fnv64(file.data() + sizeof h, bytes));
            if (std::memcmp(&h, &want, sizeof h) == 0) data.assign(file.begin() + sizeof h, file.end());
            else qilin::rt::log("the pipeline cache on disk was written by another device, driver or version, or is damaged: not used");
        }
    }
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pci.initialDataSize = data.size(), pci.pInitialData = data.empty() ? nullptr : data.data();
    host::check(vkCreatePipelineCache(gpu.device, &pci, nullptr, &vk_cache_), "vkCreatePipelineCache");
    if (folder_.empty()) return;
    // The pipelines an earlier run made, made again on the worker before the game draws.
    const std::vector<uint8_t> keys = read_file(folder_ / kKeysFile);
    if (keys.size() >= sizeof(KeysHeader)) {
        KeysHeader h;
        std::memcpy(&h, keys.data(), sizeof h);
        const size_t bytes = keys.size() - sizeof h;
        if (std::memcmp(h.magic, kKeysMagic, sizeof h.magic) == 0 && h.version == kPipelineKeyVersion && bytes == size_t{h.count} * sizeof(PipelineKey) &&
            h.hash == fnv64(keys.data() + sizeof h, bytes)) {
            ahead_.resize(h.count);
            std::memcpy(ahead_.data(), keys.data() + sizeof h, bytes);
        } else {
            qilin::rt::log("the pipelines recorded on disk are of another version, or damaged: not made ahead");
        }
    }
    qilin::rt::log("the renderer makes %zu pipelines ahead (recorded by an earlier run), with %zu bytes of the driver's cache", ahead_.size(), data.size());
    thread_ = std::thread([this] { worker(); });
    thread_.detach();  // (for the process's life: it ends without unwinding)
}

void PipelineCache::worker() {
    for (const PipelineKey& key : ahead_) {
        {
            const std::lock_guard lock(mutex_);
            if (!pipelines_.emplace(key, VK_NULL_HANDLE).second) continue;  // (made already, or being made, for a draw)
        }
        const VkPipeline p = make(key, true);
        const std::lock_guard lock(mutex_);
        if (p) pipelines_[key] = p, ++made_ahead_;
        else pipelines_.erase(key);  // (this device can't: a draw that needs it will say why)
        made_.notify_all();
    }
    std::unique_lock lock(mutex_);
    ahead_done_ = true;
    made_.notify_all();
    for (;;) {
        work_.wait(lock, [&] { return save_asked_; });
        save_asked_ = false;
        lock.unlock();
        write_files();
        lock.lock();
    }
}

void PipelineCache::wait_ahead() {
    if (folder_.empty()) return;
    std::unique_lock lock(mutex_);
    made_.wait(lock, [&] { return ahead_done_; });
}

void PipelineCache::save(bool now) {
    if (folder_.empty() || made_since_save_.load() == 0) return;
    if (now) return write_files();
    const std::lock_guard lock(mutex_);
    save_asked_ = true;
    work_.notify_all();
}

void PipelineCache::write_files() {
    const std::lock_guard files(files_mutex_);
    made_since_save_ = 0;
    std::vector<PipelineKey> keys;
    {
        const std::lock_guard lock(mutex_);
        keys.reserve(pipelines_.size());
        for (const auto& [key, p] : pipelines_)
            if (p) keys.push_back(key);
    }
    std::ranges::sort(keys);  // (the same pipelines, the same file)
    std::vector<uint8_t> out;
    KeysHeader kh{};
    std::memcpy(kh.magic, kKeysMagic, sizeof kh.magic);
    kh.version = kPipelineKeyVersion, kh.count = static_cast<uint32_t>(keys.size());
    kh.hash = fnv64(keys.data(), keys.size() * sizeof(PipelineKey));
    append(out, kh);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(keys.data()), reinterpret_cast<const uint8_t*>(keys.data() + keys.size()));
    std::error_code e;
    std::filesystem::create_directories(folder_, e);
    const bool keys_written = write_file(folder_ / kKeysFile, out);
    std::vector<uint8_t> data;
    for (VkResult r = VK_INCOMPLETE; r == VK_INCOMPLETE;) {  // (VK_INCOMPLETE: the cache grew in between, and is asked for again)
        size_t bytes = 0;
        host::check(vkGetPipelineCacheData(gpu_->device, vk_cache_, &bytes, nullptr), "vkGetPipelineCacheData");
        data.resize(bytes);
        r = vkGetPipelineCacheData(gpu_->device, vk_cache_, &bytes, data.data());
        if (r != VK_INCOMPLETE) host::check(r, "vkGetPipelineCacheData"), data.resize(bytes);
    }
    out.clear();
    append(out, cache_header(*gpu_, data.size(), fnv64(data.data(), data.size())));
    out.insert(out.end(), data.begin(), data.end());
    const bool cache_written = write_file(folder_ / kCacheFile, out);
    if (!keys_written || !cache_written)
        qilin::rt::log("the renderer couldn't write its pipelines to %s (the next start makes them when they are drawn)", folder_.string().c_str());
}

}  // namespace rr7

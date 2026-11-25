#include "renderer.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>

#include "kernel.hpp"
#include "qilin/host/vulkan.hpp"
#include "qilin/rt/report.hpp"
#include "qilin/rt/shaders.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;
namespace host = qilin::host;

constexpr unsigned kVertexConstants = 468;

struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
};

// A colour surface kept on the GPU: its image, and a buffer it comes from and goes back to guest memory through.
struct SurfaceEntry {
    Surface desc;
    host::Image image;
    VkImageView attachment_view = VK_NULL_HANDLE;  // a depth surface's view of both its aspects, to attach for depth and stencil
    host::Buffer staging;  // colour: RGBA rows; depth: the depth aspect's words, then the stencil aspect's bytes
    bool depth = false;
    bool coherent = true;  // whether its memory needs invalidating before the CPU reads what the GPU wrote
    // After a write-back the image holds what guest memory does, until something else writes there: the hash of the
    // guest bytes as written back tells.
    bool resident = false;
    uint64_t written_hash = 0;
    bool touched = false;  // drawn in the batch being recorded
    bool written = false;  // and written by a draw in it (a depth surface only tested isn't): only then copied back
};

// A buffer the CPU reads back from: in host-cached memory where there is some (uncached memory is read a word at a time
// across the bus: a 1920 by 1080 surface took most of a second), else as host_visible makes it.
host::Buffer readback_buffer(const host::Gpu& gpu, VkDeviceSize size, bool& coherent) {
    host::Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    host::check(vkCreateBuffer(gpu.device, &bci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gpu.device, b.buffer, &req);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(gpu.physical, &memory);
    uint32_t chosen = UINT32_MAX;
    for (const VkMemoryPropertyFlags want : {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                                             VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
                                             VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)}) {
        for (uint32_t i = 0; i < memory.memoryTypeCount && chosen == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) chosen = i;
        if (chosen != UINT32_MAX) break;
    }
    if (chosen == UINT32_MAX) fatal("the GPU has no memory the CPU can read back from");
    coherent = (memory.memoryTypes[chosen].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = chosen;
    host::check(vkAllocateMemory(gpu.device, &mai, nullptr, &b.memory), "vkAllocateMemory");
    host::check(vkBindBufferMemory(gpu.device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    host::check(vkMapMemory(gpu.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    return b;
}

// A texture kept on the GPU between draws, by its fields; its texels are decoded again only when the guest bytes it
// covers hash differently.
struct CachedTexture {
    host::Image image;
    uint64_t hash = 0;
    uint64_t used = 0;  // the batch it was last used in
    uint64_t checked = UINT64_MAX;  // the batch its hash was last taken in
};

// A hash of guest bytes (not for security: to see that a texture has changed), in four lanes of eight bytes the CPU can
// run side by side, so that it goes at about the speed memory is read.
uint64_t hash_bytes(const uint8_t* p, size_t n) {
    constexpr uint64_t kMul = 0xff51afd7ed558ccdull;
    uint64_t h[4] = {0x9e3779b97f4a7c15ull ^ n, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (unsigned k = 0; k < 4; ++k) {
            uint64_t w;
            std::memcpy(&w, p + i + 8 * k, 8);
            h[k] = (h[k] ^ w) * kMul;
            h[k] ^= h[k] >> 29;
        }
    uint64_t out = h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7);
    for (; i < n; ++i) out = (out ^ p[i]) * 0x100000001b3ull;
    return out;
}

constexpr uint32_t kMostSets = 1024;                   // descriptor sets in a batch
constexpr VkDeviceSize kArenaBytes = 64u << 20;        // vertices, indices, constants and texels of a batch

struct Renderer {
    host::Gpu* gpu = nullptr;
    std::mutex* queue = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSetLayout sets[2] = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    std::map<uint64_t, const qilin::rt::TranslatedShader*> shaders;  // by key
    std::map<const qilin::rt::TranslatedShader*, VkShaderModule> modules;
    std::map<std::vector<uint32_t>, VkPipeline> pipelines;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, SurfaceEntry> surfaces;  // by address, format, width, height
    // The batch being recorded (renderer.hpp).
    bool recording = false;
    host::Buffer arena;
    VkDeviceSize arena_used = 0, align = 256;
    uint32_t sets_used = 0;
    std::map<std::array<uint32_t, 6>, CachedTexture> textures;  // by address, format, size, pitch and remap
    std::vector<SurfaceEntry*> touched;     // the surfaces it draws to
    SurfaceEntry* rendering = nullptr;      // the surface a rendering instance is open on
    SurfaceEntry* rendering_depth = nullptr;  // and its depth surface, if any
    VkFormat depth_format = VK_FORMAT_UNDEFINED;  // for Z24S8: D32_SFLOAT_S8_UINT, or D24_UNORM_S8_UINT where the GPU lacks it
    uint64_t draws = 0, flushes = 0;
    VkQueryPool timestamps = VK_NULL_HANDLE;  // three a batch: its start, before its copies back, its end
    std::map<std::vector<uint32_t>, VkSampler> samplers;  // by the fields they're made from
    host::Buffer dummy;  // bound where a program has no inline constants
};
Renderer* g_renderer = nullptr;

// The renderer's time, by part (nanoseconds), for renderer_report.
struct Stats {
    std::atomic<uint64_t> draws{0}, flushes{0}, textures{0}, texture_bytes{0}, uploads{0};
    std::atomic<uint64_t> draw_ns{0}, texture_ns{0}, upload_ns{0}, flush_ns{0}, wait_ns{0}, writeback_ns{0};
    std::atomic<uint64_t> gpu_draw_ns{0}, gpu_copy_ns{0};  // the GPU's own time: the batch's commands, and its copies back
};
Stats g_stats;
using Clock = std::chrono::steady_clock;
uint64_t since(Clock::time_point t) { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count()); }
std::mutex g_present_mutex;
std::function<void(const uint8_t*, uint32_t, uint32_t)> g_present;

uint64_t fnv(const std::vector<uint8_t>& bytes) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const uint8_t c : bytes) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

// The Vulkan format a colour surface of the RSX's is drawn in, and how many bytes a pixel has in guest memory.
VkFormat surface_format(uint32_t format, uint32_t& bytes) {
    switch (format) {
    case 8: bytes = 4; return VK_FORMAT_R8G8B8A8_UNORM;   // A8R8G8B8
    case 13: bytes = 4; return VK_FORMAT_R32_SFLOAT;      // F_X32
    default: fatal("a colour surface of format %u, which the renderer doesn't draw yet", format);
    }
}

// Guest memory to the image's bytes, and back: A8R8G8B8 is A, R, G, B in memory and R, G, B, A in the image; floats are big-endian.
void to_host(uint32_t format, const uint8_t* from, uint8_t* to, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i, from += 4, to += 4) {
        if (format == 8) to[0] = from[1], to[1] = from[2], to[2] = from[3], to[3] = from[0];
        else to[0] = from[3], to[1] = from[2], to[2] = from[1], to[3] = from[0];
    }
}
void to_guest(uint32_t format, const uint8_t* from, uint8_t* to, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i, from += 4, to += 4) {
        if (format == 8) to[0] = from[3], to[1] = from[0], to[2] = from[1], to[3] = from[2];
        else to[0] = from[3], to[1] = from[2], to[2] = from[1], to[3] = from[0];
    }
}

// One vertex attribute's components as floats: the RSX's types (CELL_GCM_VERTEX_...), big-endian.
void attribute(uint32_t format, const uint8_t* p, float* out) {
    const uint32_t type = format & 0xf, size = (format >> 4) & 0xf;
    out[0] = out[1] = out[2] = 0.0f, out[3] = 1.0f;
    for (uint32_t c = 0; c < size; ++c) {
        switch (type) {
        case 1: out[c] = std::max(-1.0f, static_cast<float>(static_cast<int16_t>((p[2 * c] << 8) | p[2 * c + 1])) / 32767.0f); break;  // S1
        case 2: out[c] = std::bit_cast<float>((uint32_t{p[4 * c]} << 24) | (uint32_t{p[4 * c + 1]} << 16) | (uint32_t{p[4 * c + 2]} << 8) | p[4 * c + 3]); break;  // F
        case 3: {  // SF: a half
            const uint16_t h = static_cast<uint16_t>((p[2 * c] << 8) | p[2 * c + 1]);
            const uint32_t sign = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
            float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
            out[c] = sign ? -v : v;
            break;
        }
        case 4: out[c] = static_cast<float>(p[c]) / 255.0f; break;  // UB
        case 5: out[c] = static_cast<float>(static_cast<int16_t>((p[2 * c] << 8) | p[2 * c + 1])); break;  // S32K
        case 7: out[c] = static_cast<float>(p[c]); break;  // UB256
        default: fatal("a vertex attribute of type %u, which the renderer doesn't take yet", type);
        }
    }
}

const qilin::rt::TranslatedShader& find_shader(const std::vector<uint8_t>& microcode, bool vertex) {
    std::vector<uint8_t> keyed = microcode;
    if (!vertex) {
        // The inline constants left out, as `qilin shaders` keys it: after any instruction with a source of type 2.
        for (size_t at = 0; at + 16 <= keyed.size();) {
            const auto word = [&](size_t i) {
                const uint32_t w = (uint32_t{keyed[at + 4 * i]} << 24) | (uint32_t{keyed[at + 4 * i + 1]} << 16) | (uint32_t{keyed[at + 4 * i + 2]} << 8) | keyed[at + 4 * i + 3];
                return (w << 16) | (w >> 16);
            };
            const bool branch = word(2) >> 31;
            const bool constant = !branch && ((word(1) & 3) == 2 || (word(2) & 3) == 2 || (word(3) & 3) == 2);
            at += 16;
            if (constant && at + 16 <= keyed.size()) std::memset(keyed.data() + at, 0, 16), at += 16;
        }
    }
    const uint64_t key = fnv(keyed);
    const auto it = g_renderer->shaders.find(key);
    if (it == g_renderer->shaders.end() || it->second->vertex != vertex)
        fatal("the game draws with a %s program (key %016llx, %zu bytes) that is not among the %zu translated: every program must be known when the game is built",
              vertex ? "vertex" : "fragment", static_cast<unsigned long long>(key), microcode.size(), qilin::rt::kTranslatedShaderCount);
    return *it->second;
}

VkShaderModule module_of(const qilin::rt::TranslatedShader& s) {
    auto& m = g_renderer->modules[&s];
    if (!m) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = s.spirv_words * 4;
        info.pCode = s.spirv;
        host::check(vkCreateShaderModule(g_renderer->gpu->device, &info, nullptr, &m), "vkCreateShaderModule");
    }
    return m;
}

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

// A texture's image: 2D, or a cube map's six layers with a cube view (which the host layer doesn't make).
host::Image texture_image(const host::Gpu& gpu, const Texels& t) {
    if (t.faces == 1)
        return host::Image::create(gpu, t.width, t.height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, t.levels);
    host::Image im;
    im.format = VK_FORMAT_R8G8B8A8_UNORM, im.width = t.width, im.height = t.height, im.levels = t.levels;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ici.imageType = VK_IMAGE_TYPE_2D, ici.format = im.format, ici.extent = {t.width, t.height, 1};
    ici.mipLevels = t.levels, ici.arrayLayers = 6, ici.samples = VK_SAMPLE_COUNT_1_BIT, ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    host::check(vkCreateImage(gpu.device, &ici, nullptr, &im.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(gpu.device, im.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size, mai.memoryTypeIndex = gpu.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    host::check(vkAllocateMemory(gpu.device, &mai, nullptr, &im.memory), "vkAllocateMemory");
    host::check(vkBindImageMemory(gpu.device, im.image, im.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.image, vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE, vci.format = im.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0, 6};
    host::check(vkCreateImageView(gpu.device, &vci, nullptr, &im.view), "vkCreateImageView");
    return im;
}

// A layout transition of every level and layer of a texture's image (the host layer's covers one layer).
void transition_texture(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1, dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkSamplerAddressMode address_mode(uint32_t mode) {
    switch (mode) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
}

// A texture unit's sampler, made once for each set of fields.
VkSampler sampler_for(uint32_t unit, const SamplerFields& s, uint32_t levels) {
    const std::vector<uint32_t> key{s.mag, s.min, s.mip, s.wrap[0], s.wrap[1], s.wrap[2], std::bit_cast<uint32_t>(s.bias), std::bit_cast<uint32_t>(s.min_lod),
                                    std::bit_cast<uint32_t>(s.max_lod), s.anisotropy, s.border, levels > 1};
    VkSampler& sampler = g_renderer->samplers[key];
    if (sampler) return sampler;
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
        if (g_renderer->gpu->max_anisotropy < 1.0f) fatal("texture %u is sampled anisotropically, and this GPU can't", unit);
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(static_cast<float>(s.anisotropy), g_renderer->gpu->max_anisotropy);
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
    host::check(vkCreateSampler(g_renderer->gpu->device, &info, nullptr, &sampler), "vkCreateSampler");
    return sampler;
}

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

VkPipeline pipeline_for(const Draw& d, const qilin::rt::TranslatedShader& vs, const qilin::rt::TranslatedShader& fs, VkFormat format, VkPrimitiveTopology topology) {
    std::vector<uint32_t> key{static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&vs)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&vs) >> 32),
                              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fs)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fs) >> 32),
                              static_cast<uint32_t>(format), static_cast<uint32_t>(topology), d.color_mask, d.blend, d.blend_src, d.blend_dst, d.blend_equation,
                              d.depth_test, d.depth_write, d.depth_func, d.cull, d.cull_face, d.front_face, d.depth_bias, d.depth_surface, d.stencil_test,
                              d.stencil_front.func, d.stencil_front.ref, d.stencil_front.func_mask, d.stencil_front.write_mask, d.stencil_front.fail, d.stencil_front.zfail,
                              d.stencil_front.zpass, d.stencil_back.func, d.stencil_back.ref, d.stencil_back.func_mask, d.stencil_back.write_mask, d.stencil_back.fail,
                              d.stencil_back.zfail, d.stencil_back.zpass,
                              std::bit_cast<uint32_t>(d.bias_factor), std::bit_cast<uint32_t>(d.bias_units),
                              d.depth_bounds, std::bit_cast<uint32_t>(d.bounds_min), std::bit_cast<uint32_t>(d.bounds_max)};
    auto& p = g_renderer->pipelines[key];
    if (p) return p;
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = module_of(vs), stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = module_of(fs), stages[1].pName = "main";
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (uint32_t a = 0; a < 16; ++a)
        if (vs.inputs & (1u << a)) {
            bindings.push_back({a, 16, VK_VERTEX_INPUT_RATE_VERTEX});
            attributes.push_back({a, a, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
        }
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size()), vertex.pVertexBindingDescriptions = bindings.data();
    vertex.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size()), vertex.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1, viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.lineWidth = 1.0f;
    raster.cullMode = VK_CULL_MODE_NONE;
    if (d.depth_bias) raster.depthBiasEnable = VK_TRUE, raster.depthBiasSlopeFactor = d.bias_factor, raster.depthBiasConstantFactor = d.bias_units;
    // Culling (OpenGL's faces: front 0x404, back 0x405, both 0x408; windings: clockwise 0x900, anticlockwise 0x901). The
    // winding is in window coordinates, which count down from the top as Vulkan's framebuffer does (rsx.cpp: the window's
    // origin), so the RSX's anticlockwise is Vulkan's (*a stated reading, held to the picture*).
    if (d.cull) {
        switch (d.cull_face) {
        case 0x404: raster.cullMode = VK_CULL_MODE_FRONT_BIT; break;
        case 0x405: raster.cullMode = VK_CULL_MODE_BACK_BIT; break;
        case 0x408: raster.cullMode = VK_CULL_MODE_FRONT_AND_BACK; break;
        default: fatal("culling of face %#x", d.cull_face);
        }
    }
    if (d.front_face != 0x900 && d.front_face != 0x901 && d.cull) fatal("a front face of %#x", d.front_face);
    raster.frontFace = d.front_face == 0x900 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    // The RSX's colour mask is blue bit 0, green 8, red 16, alpha 24.
    blend.colorWriteMask = ((d.color_mask >> 16) & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) | ((d.color_mask >> 8) & 1 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                           (d.color_mask & 1 ? VK_COLOR_COMPONENT_B_BIT : 0) | ((d.color_mask >> 24) & 1 ? VK_COLOR_COMPONENT_A_BIT : 0);
    if (d.blend) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = blend_factor(d.blend_src & 0xffff), blend.srcAlphaBlendFactor = blend_factor(d.blend_src >> 16);
        blend.dstColorBlendFactor = blend_factor(d.blend_dst & 0xffff), blend.dstAlphaBlendFactor = blend_factor(d.blend_dst >> 16);
        blend.colorBlendOp = blend_op(d.blend_equation & 0xffff), blend.alphaBlendOp = blend_op(d.blend_equation >> 16);
    }
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1, blending.pAttachments = &blend;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2, dynamic.pDynamicStates = dynamic_states;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (d.depth_test) {
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = d.depth_write ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = compare_op(d.depth_func);
    }
    if (d.depth_bounds) {
        if (!g_renderer->gpu->depth_bounds) fatal("a draw with the depth bounds test, which this GPU doesn't have");
        // Vulkan takes bounds within [0, 1] only; the depth stored is within it, so clamping them changes no result.
        depth.depthBoundsTestEnable = VK_TRUE;
        depth.minDepthBounds = std::clamp(d.bounds_min, 0.0f, 1.0f), depth.maxDepthBounds = std::clamp(d.bounds_max, 0.0f, 1.0f);
    }
    if (d.stencil_test) {
        depth.stencilTestEnable = VK_TRUE;
        depth.front = stencil_state(d.stencil_front), depth.back = stencil_state(d.stencil_back);
    }
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1, rendering.pColorAttachmentFormats = &format;
    if (d.depth_surface) rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat = g_renderer->depth_format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2, info.pStages = stages;
    info.pVertexInputState = &vertex, info.pInputAssemblyState = &assembly, info.pViewportState = &viewport;
    info.pRasterizationState = &raster, info.pMultisampleState = &multisample, info.pColorBlendState = &blending, info.pDynamicState = &dynamic;
    info.pDepthStencilState = &depth;
    info.layout = g_renderer->layout;
    host::check(vkCreateGraphicsPipelines(g_renderer->gpu->device, VK_NULL_HANDLE, 1, &info, nullptr, &p), "vkCreateGraphicsPipelines");
    return p;
}

}  // namespace

void renderer_start(host::Gpu& gpu, std::mutex& queue) {
    auto* r = new Renderer;  // (for the process's life: it ends without unwinding)
    r->gpu = &gpu;
    r->queue = &queue;
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = gpu.family;
    host::check(vkCreateCommandPool(gpu.device, &pool, nullptr, &r->pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = r->pool, alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, alloc.commandBufferCount = 1;
    host::check(vkAllocateCommandBuffers(gpu.device, &alloc, &r->cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    host::check(vkCreateFence(gpu.device, &fence, nullptr, &r->fence), "vkCreateFence");
    // The layout every translated program fits (rsx_spirv.hpp): set 0 the constant blocks, set 1 sixteen textures, 32 bytes of push constants.
    VkDescriptorSetLayoutBinding constants[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        constants[i].binding = i, constants[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, constants[i].descriptorCount = 1;
        constants[i].stageFlags = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutBinding textures[16] = {};
    for (uint32_t i = 0; i < 16; ++i) {
        textures[i].binding = i, textures[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, textures[i].descriptorCount = 1;
        textures[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = 2, set.pBindings = constants;
    host::check(vkCreateDescriptorSetLayout(gpu.device, &set, nullptr, &r->sets[0]), "vkCreateDescriptorSetLayout");
    set.bindingCount = 16, set.pBindings = textures;
    host::check(vkCreateDescriptorSetLayout(gpu.device, &set, nullptr, &r->sets[1]), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange push[] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 32}, {VK_SHADER_STAGE_FRAGMENT_BIT, 32, 32}};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 2, layout.pSetLayouts = r->sets, layout.pushConstantRangeCount = 2, layout.pPushConstantRanges = push;
    host::check(vkCreatePipelineLayout(gpu.device, &layout, nullptr, &r->layout), "vkCreatePipelineLayout");
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kMostSets}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * kMostSets}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = kMostSets, dp.poolSizeCount = 2, dp.pPoolSizes = sizes;
    host::check(vkCreateDescriptorPool(gpu.device, &dp, nullptr, &r->descriptors), "vkCreateDescriptorPool");
    for (size_t i = 0; i < qilin::rt::kTranslatedShaderCount; ++i) r->shaders[qilin::rt::kTranslatedShaders[i].key] = &qilin::rt::kTranslatedShaders[i];
    r->dummy = host::Buffer::host_visible(gpu, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    r->arena = host::Buffer::host_visible(gpu, kArenaBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    // D32S8 first: NVIDIA's copies of D24S8's depth out of the image took 32 ms a 1920 by 1080 surface against D32S8's 4.5,
    // and AMD has no D24S8 at all.
    for (const VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(gpu.physical, f, &p);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((p.optimalTilingFeatures & need) == need) {
            r->depth_format = f;
            break;
        }
    }
    if (r->depth_format == VK_FORMAT_UNDEFINED) fatal("the GPU has no depth-stencil format the renderer can use (D24S8 or D32S8)");
    VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query.queryType = VK_QUERY_TYPE_TIMESTAMP, query.queryCount = 3;
    host::check(vkCreateQueryPool(gpu.device, &query, nullptr, &r->timestamps), "vkCreateQueryPool");
    r->align = std::max<VkDeviceSize>({256, gpu.properties.limits.minUniformBufferOffsetAlignment, gpu.properties.limits.optimalBufferCopyOffsetAlignment});
    g_renderer = r;
    qilin::rt::log("the renderer draws on %s, with %zu translated shaders", gpu.name.c_str(), qilin::rt::kTranslatedShaderCount);
}

bool renderer_started() { return g_renderer != nullptr; }

namespace {

// Room in the batch's arena: an offset, aligned for any use (uniform buffers' alignment the strictest).
VkDeviceSize arena_take(Renderer& r, VkDeviceSize size) {
    const VkDeviceSize at = (r.arena_used + r.align - 1) / r.align * r.align;
    if (at + size > r.arena.size) fatal("the renderer's arena is full mid-draw (a draw's needs are checked before it starts)");
    r.arena_used = at + size;
    return at;
}
uint8_t* arena_at(Renderer& r, VkDeviceSize offset) { return static_cast<uint8_t*>(r.arena.mapped) + offset; }

void end_rendering(Renderer& r) {
    if (r.rendering) vkCmdEndRendering(r.cmd), r.rendering = nullptr, r.rendering_depth = nullptr;
}

bool overlaps(uint32_t a, uint64_t a_bytes, uint32_t b, uint64_t b_bytes) { return a < b + b_bytes && b < a + a_bytes; }
uint64_t surface_bytes(const Surface& s) { return uint64_t{s.pitch} * s.height; }

// Whether guest memory [address, address + bytes) overlaps a surface drawn in this batch (other than `except`).
bool touched_overlap(const Renderer& r, uint32_t address, uint64_t bytes, const SurfaceEntry* except) {
    for (const SurfaceEntry* s : r.touched)
        if (s != except && overlaps(address, bytes, s->desc.address, surface_bytes(s->desc))) return true;
    return false;
}

VkImageAspectFlags aspects(const SurfaceEntry& s) {
    return s.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
}

// The copies between a surface's image and its staging buffer: one for a colour surface; a depth surface's two aspects,
// depth words first, then stencil bytes.
uint32_t surface_copies(const SurfaceEntry& s, VkBufferImageCopy (&copies)[2]) {
    copies[0] = {}, copies[1] = {};
    copies[0].imageExtent = copies[1].imageExtent = {s.desc.width, s.desc.height, 1};
    if (!s.depth) {
        copies[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        return 1;
    }
    copies[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copies[1].imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
    copies[1].bufferOffset = VkDeviceSize{s.desc.width} * s.desc.height * 4;
    return 2;
}

// A Z24S8 surface's words to the staging buffer's two aspects and back. With D24_UNORM_S8_UINT the depth aspect is a word
// with depth in its low 24 bits; with D32_SFLOAT_S8_UINT a float, depth / (2^24 - 1) (*then the test compares floats, not
// 24-bit numbers: a stated difference. Each 24-bit depth is a float of its own, so order and the value written back are
// kept; a fragment exactly at a stored depth may test otherwise than on the RSX*).
void depth_to_host(const SurfaceEntry& s) {
    const bool unorm = g_renderer->depth_format == VK_FORMAT_D24_UNORM_S8_UINT;
    const size_t texels = size_t{s.desc.width} * s.desc.height;
    uint32_t* depth = static_cast<uint32_t*>(s.staging.mapped);
    uint8_t* stencil = static_cast<uint8_t*>(s.staging.mapped) + texels * 4;
    for (uint32_t y = 0; y < s.desc.height; ++y)
        for (uint32_t x = 0; x < s.desc.width; ++x) {
            const uint32_t w = load32(s.desc.address + y * s.desc.pitch + 4 * x);
            const size_t i = size_t{y} * s.desc.width + x;
            depth[i] = unorm ? w >> 8 : std::bit_cast<uint32_t>(static_cast<float>(w >> 8) / 16777215.0f);
            stencil[i] = static_cast<uint8_t>(w);
        }
}
void surface_to_guest(const SurfaceEntry& s) {
    if (!s.depth) {
        for (uint32_t y = 0; y < s.desc.height; ++y)
            to_guest(s.desc.format, static_cast<const uint8_t*>(s.staging.mapped) + size_t{y} * s.desc.width * 4, guest_memory() + s.desc.address + size_t{y} * s.desc.pitch,
                     s.desc.width);
        return;
    }
    const bool unorm = g_renderer->depth_format == VK_FORMAT_D24_UNORM_S8_UINT;
    const size_t texels = size_t{s.desc.width} * s.desc.height;
    const uint32_t* depth = static_cast<const uint32_t*>(s.staging.mapped);
    const uint8_t* stencil = static_cast<const uint8_t*>(s.staging.mapped) + texels * 4;
    for (uint32_t y = 0; y < s.desc.height; ++y)
        for (uint32_t x = 0; x < s.desc.width; ++x) {
            const size_t i = size_t{y} * s.desc.width + x;
            uint32_t d = depth[i] & 0xffffffu;
            if (!unorm) d = static_cast<uint32_t>(std::lround(std::clamp(std::bit_cast<float>(depth[i]), 0.0f, 1.0f) * 16777215.0f));
            store32(s.desc.address + y * s.desc.pitch + 4 * x, (d << 8) | stencil[i]);
        }
}

// The surface from guest memory into its image, as a batch first draws to it, ready to draw to.
void surface_from_guest(VkCommandBuffer cmd, SurfaceEntry& s) {
    // (Not copied when the image already holds what guest memory does.)
    const VkImageLayout ready = s.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (s.resident && hash_bytes(guest_memory() + s.desc.address, surface_bytes(s.desc)) == s.written_hash) {
        host::transition(cmd, s.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ready, aspects(s));
        return;
    }
    ++g_stats.uploads;
    if (s.depth) {
        depth_to_host(s);
    } else {
        for (uint32_t y = 0; y < s.desc.height; ++y)
            to_host(s.desc.format, guest_memory() + s.desc.address + size_t{y} * s.desc.pitch, static_cast<uint8_t*>(s.staging.mapped) + size_t{y} * s.desc.width * 4, s.desc.width);
    }
    if (!s.coherent) {
        const VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, s.staging.memory, 0, VK_WHOLE_SIZE};
        host::check(vkFlushMappedMemoryRanges(g_renderer->gpu->device, 1, &range), "vkFlushMappedMemoryRanges");
    }
    host::transition(cmd, s.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspects(s));
    VkBufferImageCopy copies[2];
    const uint32_t n = surface_copies(s, copies);
    vkCmdCopyBufferToImage(cmd, s.staging.buffer, s.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, copies);
    host::transition(cmd, s.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     s.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, aspects(s));
}

}  // namespace

// The batch: everything drawn since the last flush is submitted, waited for, and its surfaces written back to guest memory.
void renderer_flush() {
    if (!g_renderer || !g_renderer->recording) return;
    const Clock::time_point flush_start = Clock::now();
    Renderer& r = *g_renderer;
    const host::Gpu& gpu = *r.gpu;
    end_rendering(r);
    vkCmdWriteTimestamp2(r.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, r.timestamps, 1);
    for (SurfaceEntry* s : r.touched) {
        const VkImageLayout layout = s->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        host::transition(r.cmd, s->image.image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspects(*s));
        if (!s->written) continue;
        VkBufferImageCopy copies[2];
        const uint32_t n = surface_copies(*s, copies);
        vkCmdCopyImageToBuffer(r.cmd, s->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->staging.buffer, n, copies);
    }
    // Everything copied back is visible to the host once the fence is signalled.
    VkMemoryBarrier2 to_host_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    to_host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, to_host_barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    to_host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, to_host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1, dependency.pMemoryBarriers = &to_host_barrier;
    vkCmdPipelineBarrier2(r.cmd, &dependency);
    vkCmdWriteTimestamp2(r.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, r.timestamps, 2);
    host::check(vkEndCommandBuffer(r.cmd), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1, submit.pCommandBuffers = &r.cmd;
    {
        const std::lock_guard lock(*r.queue);
        host::check(vkQueueSubmit(gpu.queue, 1, &submit, r.fence), "vkQueueSubmit");
    }
    const Clock::time_point wait_start = Clock::now();
    host::check(vkWaitForFences(gpu.device, 1, &r.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    host::check(vkResetFences(gpu.device, 1, &r.fence), "vkResetFences");
    g_stats.wait_ns += since(wait_start);
    {
        uint64_t t[3];
        host::check(vkGetQueryPoolResults(gpu.device, r.timestamps, 0, 3, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults");
        const double period = gpu.properties.limits.timestampPeriod;
        g_stats.gpu_draw_ns += static_cast<uint64_t>(static_cast<double>(t[1] - t[0]) * period);
        g_stats.gpu_copy_ns += static_cast<uint64_t>(static_cast<double>(t[2] - t[1]) * period);
    }
    const Clock::time_point writeback_start = Clock::now();
    // The surfaces back to guest memory: drawn, as the game expects to find them.
    for (SurfaceEntry* s : r.touched) {
        if (!s->written) {
            // Unchanged by the batch: the image still holds what guest memory does.
            s->written_hash = hash_bytes(guest_memory() + s->desc.address, surface_bytes(s->desc));
            s->resident = true;
            s->touched = false;
            continue;
        }
        s->written = false;
        if (!s->coherent) {
            const VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, s->staging.memory, 0, VK_WHOLE_SIZE};
            host::check(vkInvalidateMappedMemoryRanges(gpu.device, 1, &range), "vkInvalidateMappedMemoryRanges");
        }
        surface_to_guest(*s);
        s->written_hash = hash_bytes(guest_memory() + s->desc.address, surface_bytes(s->desc));
        s->resident = true;
        s->touched = false;
    }
    r.touched.clear();
    g_stats.writeback_ns += since(writeback_start);
    // (The GPU is idle here: textures unused for a while can go.)
    if (r.textures.size() > 1024) {
        for (auto it = r.textures.begin(); it != r.textures.end();) {
            if (it->second.used + 300 < r.flushes) {
                it->second.image.destroy(gpu);
                it = r.textures.erase(it);
            } else {
                ++it;
            }
        }
    }
    host::check(vkResetDescriptorPool(gpu.device, r.descriptors, 0), "vkResetDescriptorPool");
    r.sets_used = 0;
    r.arena_used = 0;
    r.recording = false;
    ++r.flushes;
    ++g_stats.flushes;
    g_stats.flush_ns += since(flush_start);
}


void renderer_report() {
    const auto ms = [](const std::atomic<uint64_t>& ns) { return static_cast<double>(ns.load()) / 1e6; };
    qilin::rt::log("renderer: %llu draws (%.0f ms, of which textures %.0f ms: %llu decoded, %llu MB; surfaces from guest memory %.0f ms, %llu times), "
                   "%llu flushes (%.0f ms, of which waiting for the GPU %.0f ms, writing back %.0f ms); on the GPU, batches %.0f ms and copies back %.0f ms",
                   static_cast<unsigned long long>(g_stats.draws.load()), ms(g_stats.draw_ns), ms(g_stats.texture_ns), static_cast<unsigned long long>(g_stats.textures.load()),
                   static_cast<unsigned long long>(g_stats.texture_bytes.load() >> 20), ms(g_stats.upload_ns), static_cast<unsigned long long>(g_stats.uploads.load()),
                   static_cast<unsigned long long>(g_stats.flushes.load()), ms(g_stats.flush_ns), ms(g_stats.wait_ns), ms(g_stats.writeback_ns),
                   ms(g_stats.gpu_draw_ns), ms(g_stats.gpu_copy_ns));
}

void renderer_draw(const Draw& d) {
    if (!g_renderer) fatal("the game draws, and there is no renderer (no window)");
    const Clock::time_point draw_start = Clock::now();
    Renderer& r = *g_renderer;
    const host::Gpu& gpu = *r.gpu;
    const qilin::rt::TranslatedShader& vs = find_shader(d.vertex_program, true);
    const qilin::rt::TranslatedShader& fs = find_shader(d.fragment_program, false);
    // What the fragment program reads, the vertex program must write (COL0 1, COL1 2, FOGC 5, TEXn 7 + n).
    static constexpr unsigned kLocation[14] = {0, 1, 2, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    for (unsigned n = 1; n < 14; ++n)
        if ((fs.inputs & (1u << n)) && !(vs.outputs & (1u << kLocation[n])))
            fatal("the fragment program at %08x reads input %u, which the vertex program at %08x doesn't write", fs.address, n, vs.address);
    // The primitive (the RSX's numbers: OpenGL's plus one). Those Vulkan lacks, or may lack (fans, on portability devices), are
    // drawn as lists of triangles through indices: a quad (a, b, c, d) as (a, b, c) and (a, c, d), a fan's or polygon's
    // triangles about its first vertex (*the diagonals are a stated choice*).
    // The order the vertices are drawn in: the game's indices, or the vertices one after another.
    std::vector<uint32_t> order;
    if (d.indexed) {
        order = d.indices;
    } else {
        order.resize(d.count);
        for (uint32_t v = 0; v < d.count; ++v) order[v] = v;
    }
    const uint32_t n = static_cast<uint32_t>(order.size());
    VkPrimitiveTopology topology;
    std::vector<uint32_t> indices;
    bool indexed = d.indexed;
    // With primitive restart, each piece between restarts is a primitive of its own: all made into a list of triangles (a
    // strip's triangles keeping their alternate windings).
    if (std::find(order.begin(), order.end(), kRestart) != order.end()) {
        if (d.primitive < 5) fatal("primitive restart in a draw of primitive %u, which the renderer doesn't take yet", d.primitive);
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, indexed = true;
        for (size_t a = 0; a < order.size();) {
            size_t b = a;
            while (b < order.size() && order[b] != kRestart) ++b;
            const uint32_t* s = order.data() + a;
            const uint32_t m = static_cast<uint32_t>(b - a);
            switch (d.primitive) {
            case 5: indices.insert(indices.end(), s, s + m / 3 * 3); break;
            case 6:
                for (uint32_t v = 2; v < m; ++v)
                    if (v % 2 == 0) indices.insert(indices.end(), {s[v - 2], s[v - 1], s[v]});
                    else indices.insert(indices.end(), {s[v - 1], s[v - 2], s[v]});
                break;
            case 7: case 10: for (uint32_t v = 2; v < m; ++v) indices.insert(indices.end(), {s[0], s[v - 1], s[v]}); break;
            case 8: for (uint32_t v = 0; v + 4 <= m; v += 4) indices.insert(indices.end(), {s[v], s[v + 1], s[v + 2], s[v], s[v + 2], s[v + 3]}); break;
            case 9: for (uint32_t v = 0; v + 4 <= m; v += 2) indices.insert(indices.end(), {s[v], s[v + 1], s[v + 3], s[v], s[v + 3], s[v + 2]}); break;
            default: fatal("a draw of primitive %u, which the renderer doesn't take yet", d.primitive);
            }
            a = b + 1;
        }
    } else switch (d.primitive) {
    case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 4: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case 7: case 10:  // triangle fan, polygon
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, indexed = true;
        for (uint32_t v = 2; v < n; ++v) indices.insert(indices.end(), {order[0], order[v - 1], order[v]});
        break;
    case 8:  // quads
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, indexed = true;
        for (uint32_t v = 0; v + 4 <= n; v += 4) indices.insert(indices.end(), {order[v], order[v + 1], order[v + 2], order[v], order[v + 2], order[v + 3]});
        break;
    case 9:  // quad strip: quads (v, v + 1, v + 3, v + 2)
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, indexed = true;
        for (uint32_t v = 0; v + 4 <= n; v += 2) indices.insert(indices.end(), {order[v], order[v + 1], order[v + 3], order[v], order[v + 3], order[v + 2]});
        break;
    default: fatal("a draw of primitive %u, which the renderer doesn't take yet", d.primitive);
    }
    if (indexed && indices.empty() && d.primitive <= 6) indices = order;  // the game's own indices, as they are
    uint32_t pixel_bytes = 0;
    const VkFormat format = surface_format(d.color.format, pixel_bytes);

    // The surface: kept on the GPU for the batch. One that overlaps another drawn in this batch (the same memory seen as
    // another surface) first has the batch written back.
    SurfaceEntry& target = r.surfaces[{d.color.address, d.color.format, d.color.width, d.color.height}];
    if (!target.image.image) {
        target.desc = d.color;
        target.image = host::Image::create(gpu, d.color.width, d.color.height, format,
                                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        target.staging = readback_buffer(gpu, VkDeviceSize{d.color.width} * d.color.height * 4, target.coherent);
    }
    if (target.desc.pitch != d.color.pitch) fatal("a surface at %08x drawn with pitch %u, and before with %u", d.color.address, d.color.pitch, target.desc.pitch);
    SurfaceEntry* depth = nullptr;
    if (d.depth_surface) {
        depth = &r.surfaces[{d.depth.address, 0x100 | d.depth.format, d.depth.width, d.depth.height}];
        if (!depth->image.image) {
            depth->desc = d.depth, depth->depth = true;
            depth->image = host::Image::create(gpu, d.depth.width, d.depth.height, r.depth_format,
                                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            depth->staging = readback_buffer(gpu, VkDeviceSize{d.depth.width} * d.depth.height * 5, depth->coherent);
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = depth->image.image, vci.viewType = VK_IMAGE_VIEW_TYPE_2D, vci.format = r.depth_format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
            host::check(vkCreateImageView(gpu.device, &vci, nullptr, &depth->attachment_view), "vkCreateImageView");
        }
        if (depth->desc.pitch != d.depth.pitch) fatal("a depth surface at %08x used with pitch %u, and before with %u", d.depth.address, d.depth.pitch, depth->desc.pitch);
        if (overlaps(d.depth.address, surface_bytes(d.depth), d.color.address, surface_bytes(d.color))) fatal("a draw's depth surface overlaps its colour surface");
    }
    if (touched_overlap(r, d.color.address, surface_bytes(d.color), &target) || (depth && touched_overlap(r, d.depth.address, surface_bytes(d.depth), depth)))
        renderer_flush();

    // The textures the fragment program samples, read from guest memory: after the batch is written back, if they
    // overlap a surface drawn in it (a picture drawn and then sampled).
    struct Upload {
        uint32_t unit;
        CachedTexture* cached;
        bool decode;    // its texels changed (or are new): decoded, and uploaded in this draw
        Texels texels;  // when decoded
        VkSampler sampler;
    };
    std::vector<Upload> uploads;
    const Clock::time_point texture_start = Clock::now();
    for (int pass = 0; pass < 2; ++pass) {
        uploads.clear();
        bool overlap = false;
        for (uint32_t u = 0; u < 16; ++u) {
            if (!(fs.textures & (1u << u))) continue;
            const TextureUnit& t = d.textures[u];
            if (!t.enabled()) fatal("the fragment program at %08x samples texture %u, which is not enabled", fs.address, u);
            size_t bytes = 0;
            if (const std::string why = texture_extent(t, bytes); !why.empty())
                fatal("texture %u (at %08x, format %08x): %s, which the renderer doesn't take yet", u, t.address, t.format, why.c_str());
            overlap = overlap || touched_overlap(r, t.address, bytes, nullptr);
            Upload& up = uploads.emplace_back();
            up.unit = u;
            up.cached = &r.textures[{t.address, t.format, t.rect, t.control3, t.remap, (fs.cube_textures >> u) & 1}];
            if (((fs.cube_textures >> u) & 1) != ((t.format >> 2) & 1))
                fatal("the fragment program at %08x samples texture %u as %s, and it is %s", fs.address, u, (fs.cube_textures >> u) & 1 ? "a cube map" : "2D",
                      (t.format >> 2) & 1 ? "a cube map" : "2D");
            // Its bytes are hashed once a batch. Within one the game can't change a texture the batch has drawn with: it can't
            // know the RSX is done with it without a label, a wait or a flip, each of which ends the batch. (The texture
            // cache's invalidation doesn't change that, and the game sends one before nearly every draw.)
            if (!up.cached->image.image || up.cached->checked != r.flushes) {
                const uint64_t hash = hash_bytes(guest_memory() + t.address, bytes);
                up.decode = !up.cached->image.image || up.cached->hash != hash;
                up.cached->hash = hash;
                up.cached->checked = r.flushes;
            } else {
                up.decode = false;
            }
            up.cached->used = r.flushes;
            SamplerFields fields;
            if (const std::string why = decode_sampler(t, fields); !why.empty()) fatal("texture %u: %s, which the renderer doesn't take yet", u, why.c_str());
            up.sampler = sampler_for(u, fields, (t.format >> 16) & 0xffff);
        }
        if (!overlap) break;
        renderer_flush();
    }
    for (Upload& up : uploads) {
        if (!up.decode) continue;
        const TextureUnit& t = d.textures[up.unit];
        if (const std::string why = decode_texture(t, guest_memory(), up.texels); !why.empty())
            fatal("texture %u (at %08x, format %08x): %s, which the renderer doesn't take yet", up.unit, t.address, t.format, why.c_str());
        ++g_stats.textures, g_stats.texture_bytes += up.texels.rgba.size();
    }
    g_stats.texture_ns += since(texture_start);
    bool any_decoded = false;
    for (const Upload& up : uploads) any_decoded = any_decoded || up.decode;

    // What the draw takes from the arena, and two descriptor sets: if the batch hasn't room, it is flushed first.
    uint32_t attributes = 0;
    for (uint32_t a = 0; a < 16; ++a) attributes += (vs.inputs >> a) & 1;
    VkDeviceSize needed = r.align * 8 + VkDeviceSize{16} * attributes * d.count + indices.size() * 4 + kVertexConstants * 16 + fs.constant_slot_count * 16;
    for (const Upload& up : uploads) needed += up.decode ? up.texels.rgba.size() + r.align : 0;
    if (needed > r.arena.size) {
        // A draw larger than the arena: the batch is written back and the arena made big enough (a power of two).
        renderer_flush();
        VkDeviceSize size = r.arena.size;
        while (size < needed) size *= 2;
        r.arena.destroy(gpu);
        r.arena = host::Buffer::host_visible(gpu, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        size_t texels = 0;
        for (const Upload& up : uploads) texels += up.decode ? up.texels.rgba.size() : 0;
        qilin::rt::log("the renderer's arena grows to %llu MB for a draw of %llu MB: %u vertices from %u (%s, %zu indices), %u attributes, %zu MB of texels",
                       static_cast<unsigned long long>(size >> 20), static_cast<unsigned long long>(needed >> 20), d.count, d.first, d.indexed ? "indexed" : "arrays",
                       d.indices.size(), attributes, texels >> 20);
    }
    if (r.recording && (r.arena_used + needed > r.arena.size || r.sets_used + 2 > kMostSets)) renderer_flush();
    VkCommandBuffer cmd = r.cmd;
    if (!r.recording) {
        host::check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        host::check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
        vkCmdResetQueryPool(cmd, r.timestamps, 0, 3);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, r.timestamps, 0);
        r.recording = true;
    }

    // The vertices: every attribute the program reads, as four floats.
    std::vector<VkDeviceSize> offsets;
    const VkDeviceSize vertex_at = arena_take(r, std::max<VkDeviceSize>(16, VkDeviceSize{16} * attributes * d.count));
    for (uint32_t a = 0, slot = 0; a < 16; ++a) {
        if (!(vs.inputs & (1u << a))) continue;
        const uint32_t format_word = d.attribute_format[a], stride = (format_word >> 8) & 0xff;
        if (((format_word >> 4) & 0xf) == 0) fatal("the vertex program at %08x reads attribute %u, which the draw gives no array", vs.address, a);
        const VkDeviceSize at = vertex_at + VkDeviceSize{slot} * 16 * d.count;
        float* out = reinterpret_cast<float*>(arena_at(r, at));
        for (uint32_t v = 0; v < d.count; ++v) attribute(format_word, guest_memory() + d.attribute_address[a] + (d.first + v) * stride, out + 4 * v);
        if (d.logged && d.count) {
            const uint8_t* raw = guest_memory() + d.attribute_address[a] + d.first * stride;
            qilin::rt::log("(draw)   attribute %u format %08x: first vertex %g %g %g %g (bytes %02x %02x %02x %02x)", a, format_word, out[0], out[1], out[2], out[3], raw[0],
                           raw[1], raw[2], raw[3]);
        }
        offsets.push_back(at);
        ++slot;
    }
    VkDeviceSize index_at = 0;
    if (!indices.empty()) {
        index_at = arena_take(r, indices.size() * 4);
        std::memcpy(arena_at(r, index_at), indices.data(), indices.size() * 4);
    }
    // The constants.
    const VkDeviceSize vertex_constants_at = arena_take(r, kVertexConstants * 16);
    std::memcpy(arena_at(r, vertex_constants_at), d.vertex_constants, kVertexConstants * 16);
    VkDeviceSize fragment_constants_at = 0;
    if (fs.constant_slot_count) {
        fragment_constants_at = arena_take(r, fs.constant_slot_count * 16);
        for (size_t i = 0; i < fs.constant_slot_count; ++i)
            for (unsigned c = 0; c < 4; ++c) {
                const uint8_t* p = d.fragment_program.data() + fs.constant_slots[i] + 4 * c;
                const uint32_t stored = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
                reinterpret_cast<uint32_t*>(arena_at(r, fragment_constants_at))[4 * i + c] = (stored << 16) | (stored >> 16);
            }
        if (d.logged)
            for (size_t i = 0; i < fs.constant_slot_count; ++i) {
                const float* v = reinterpret_cast<const float*>(arena_at(r, fragment_constants_at)) + 4 * i;
                qilin::rt::log("(draw)   fragment constant %zu (at byte %u): %g %g %g %g", i, static_cast<unsigned>(fs.constant_slots[i]), v[0], v[1], v[2], v[3]);
            }
    }
    // Their descriptors.
    VkDescriptorSet set0;
    VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ds.descriptorPool = r.descriptors, ds.descriptorSetCount = 1, ds.pSetLayouts = &r.sets[0];
    host::check(vkAllocateDescriptorSets(gpu.device, &ds, &set0), "vkAllocateDescriptorSets");
    ++r.sets_used;
    const VkDescriptorBufferInfo infos[2] = {{r.arena.buffer, vertex_constants_at, kVertexConstants * 16},
                                             {fs.constant_slot_count ? r.arena.buffer : r.dummy.buffer, fragment_constants_at, fs.constant_slot_count ? fs.constant_slot_count * 16 : VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].dstSet = set0, writes[i].dstBinding = i, writes[i].descriptorCount = 1, writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(gpu.device, 2, writes, 0, nullptr);

    // The textures' uploads, and the surface's own when it is first drawn in this batch: outside a rendering instance.
    const bool first_touch = !target.touched, depth_first_touch = depth && !depth->touched;
    if (any_decoded || first_touch || depth_first_touch || r.rendering != &target || r.rendering_depth != depth) end_rendering(r);
    VkDescriptorSet set1 = VK_NULL_HANDLE;
    if (!uploads.empty()) {
        ds.pSetLayouts = &r.sets[1];
        host::check(vkAllocateDescriptorSets(gpu.device, &ds, &set1), "vkAllocateDescriptorSets");
        ++r.sets_used;
        std::vector<VkDescriptorImageInfo> images(uploads.size());
        std::vector<VkWriteDescriptorSet> texture_writes(uploads.size(), {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET});
        for (size_t i = 0; i < uploads.size(); ++i) {
            Upload& up = uploads[i];
            host::Image& image = up.cached->image;
            if (!image.image) image = texture_image(gpu, up.texels);
            images[i] = {up.sampler, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            texture_writes[i].dstSet = set1, texture_writes[i].dstBinding = up.unit, texture_writes[i].descriptorCount = 1;
            texture_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texture_writes[i].pImageInfo = &images[i];
            if (!up.decode) continue;
            // (Draws earlier in the batch that sampled it are ordered before this copy by the transition's barrier.)
            const VkDeviceSize at = arena_take(r, up.texels.rgba.size());
            std::memcpy(arena_at(r, at), up.texels.rgba.data(), up.texels.rgba.size());
            transition_texture(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            std::vector<VkBufferImageCopy> levels(size_t{up.texels.levels} * up.texels.faces);
            for (uint32_t face = 0; face < up.texels.faces; ++face)
                for (uint32_t l = 0; l < up.texels.levels; ++l) {
                    VkBufferImageCopy& c = levels[size_t{face} * up.texels.levels + l];
                    c.bufferOffset = at + up.texels.level_offset[size_t{face} * up.texels.levels + l];
                    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, face, 1};
                    c.imageExtent = {std::max(1u, up.texels.width >> l), std::max(1u, up.texels.height >> l), 1};
                }
            vkCmdCopyBufferToImage(cmd, r.arena.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(levels.size()), levels.data());
            transition_texture(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        vkUpdateDescriptorSets(gpu.device, static_cast<uint32_t>(texture_writes.size()), texture_writes.data(), 0, nullptr);
    }
    // The surfaces from guest memory, as the batch begins with them.
    const Clock::time_point upload_start = Clock::now();
    if (first_touch) {
        surface_from_guest(cmd, target);
        target.touched = true;
        r.touched.push_back(&target);
    }
    if (depth_first_touch) {
        surface_from_guest(cmd, *depth);
        depth->touched = true;
        r.touched.push_back(depth);
    }
    g_stats.upload_ns += since(upload_start);
    if (r.rendering != &target) {
        // (Between rendering instances on a surface, its earlier draws are made visible to the next.)
        if (!first_touch) host::transition(cmd, target.image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (depth && !depth_first_touch)
            host::transition(cmd, depth->image.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, aspects(*depth));
        VkRenderingAttachmentInfo depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (depth) {
            depth_attachment.imageView = depth->attachment_view, depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = target.image.view, attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {d.color.width, d.color.height}};
        rendering.layerCount = 1, rendering.colorAttachmentCount = 1, rendering.pColorAttachments = &attachment;
        if (depth) rendering.pDepthAttachment = rendering.pStencilAttachment = &depth_attachment;
        vkCmdBeginRendering(cmd, &rendering);
        r.rendering = &target;
        r.rendering_depth = depth;
    }

    // The viewport, as the translated programs take it (rsx_spirv.hpp): the RSX's scale and offset give window coordinates
    // in its convention, which this turns into Vulkan's clip space for a viewport covering the surface.
    const float width = static_cast<float>(d.color.width), height = static_cast<float>(d.color.height);
    float push[8];
    push[0] = d.viewport_scale[0] * 2.0f / width;
    push[2] = d.viewport_offset[0] * 2.0f / width - 1.0f;
    if (d.origin_bottom) {
        // The RSX's window y counts up from the bottom of a window `window_height` high; Vulkan's down from the top.
        push[1] = -d.viewport_scale[1] * 2.0f / height;
        push[3] = (static_cast<float>(d.window_height) - d.viewport_offset[1]) * 2.0f / height - 1.0f;
    } else {
        push[1] = d.viewport_scale[1] * 2.0f / height;
        push[3] = d.viewport_offset[1] * 2.0f / height - 1.0f;
    }
    push[4] = d.viewport_scale[2], push[5] = d.viewport_offset[2], push[6] = push[7] = 0.0f;
    // The scissor, in the same window coordinates.
    VkRect2D scissor{{static_cast<int32_t>(d.scissor_x), static_cast<int32_t>(d.scissor_y)}, {d.scissor_width, d.scissor_height}};
    if (d.origin_bottom) scissor.offset.y = static_cast<int32_t>(d.window_height) - static_cast<int32_t>(d.scissor_y + d.scissor_height);
    {
        const int32_t x0 = std::max(0, scissor.offset.x), y0 = std::max(0, scissor.offset.y);
        const int32_t x1 = std::min(static_cast<int32_t>(d.color.width), scissor.offset.x + static_cast<int32_t>(scissor.extent.width));
        const int32_t y1 = std::min(static_cast<int32_t>(d.color.height), scissor.offset.y + static_cast<int32_t>(scissor.extent.height));
        scissor = {{x0, y0}, {static_cast<uint32_t>(std::max(0, x1 - x0)), static_cast<uint32_t>(std::max(0, y1 - y0))}};
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_for(d, vs, fs, format, topology));
    const VkViewport viewport{0, 0, width, height, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushConstants(cmd, r.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    vkCmdPushConstants(cmd, r.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 32, sizeof d.alpha_test, d.alpha_test.data());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout, 0, 1, &set0, 0, nullptr);
    if (set1) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout, 1, 1, &set1, 0, nullptr);
    // One binding for each attribute read, at its own number (the pipeline's bindings are the attributes' numbers).
    for (uint32_t a = 0, slot = 0; a < 16; ++a)
        if (vs.inputs & (1u << a)) vkCmdBindVertexBuffers(cmd, a, 1, &r.arena.buffer, &offsets[slot]), ++slot;
    if (indexed) {
        if (!indices.empty()) {
            vkCmdBindIndexBuffer(cmd, r.arena.buffer, index_at, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
        }
    } else {
        vkCmdDraw(cmd, d.count, 1, 0, 0);
    }
    target.written = true;
    // (A depth-stencil surface is written by depth writes, and by a stencil test that may change it.)
    const auto changes = [](const Draw::Stencil& st) { return st.write_mask && (st.fail != 0x1e00 || st.zfail != 0x1e00 || st.zpass != 0x1e00); };
    if (depth && (d.depth_write || (d.stencil_test && (changes(d.stencil_front) || changes(d.stencil_back))))) depth->written = true;
    ++r.draws;
    ++g_stats.draws;
    g_stats.draw_ns += since(draw_start);
}

void set_present_sink(std::function<void(const uint8_t*, uint32_t, uint32_t)> sink) {
    const std::lock_guard lock(g_present_mutex);
    g_present = std::move(sink);
}
void present_frame(const uint8_t* rgba, uint32_t width, uint32_t height) {
    const std::lock_guard lock(g_present_mutex);
    if (!g_present) fatal("the game flips, and there is no window to show it in");
    g_present(rgba, width, height);
}

}  // namespace rr7

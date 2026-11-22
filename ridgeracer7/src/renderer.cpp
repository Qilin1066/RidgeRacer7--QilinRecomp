#include "renderer.hpp"

#include <bit>
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
    host::Buffer staging;
    bool touched = false;  // drawn in the batch being recorded
};

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
    std::vector<host::Image> batch_images;  // its textures
    std::vector<SurfaceEntry*> touched;     // the surfaces it draws to
    SurfaceEntry* rendering = nullptr;      // the surface a rendering instance is open on
    uint64_t draws = 0, flushes = 0;
    std::map<std::vector<uint32_t>, VkSampler> samplers;  // by the fields they're made from
    host::Buffer dummy;  // bound where a program has no inline constants
};
Renderer* g_renderer = nullptr;
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

VkPipeline pipeline_for(const Draw& d, const qilin::rt::TranslatedShader& vs, const qilin::rt::TranslatedShader& fs, VkFormat format, VkPrimitiveTopology topology) {
    std::vector<uint32_t> key{static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&vs)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&vs) >> 32),
                              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fs)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fs) >> 32),
                              static_cast<uint32_t>(format), static_cast<uint32_t>(topology), d.color_mask, d.blend, d.blend_src, d.blend_dst, d.blend_equation};
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
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1, rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2, info.pStages = stages;
    info.pVertexInputState = &vertex, info.pInputAssemblyState = &assembly, info.pViewportState = &viewport;
    info.pRasterizationState = &raster, info.pMultisampleState = &multisample, info.pColorBlendState = &blending, info.pDynamicState = &dynamic;
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
    if (r.rendering) vkCmdEndRendering(r.cmd), r.rendering = nullptr;
}

bool overlaps(uint32_t a, uint64_t a_bytes, uint32_t b, uint64_t b_bytes) { return a < b + b_bytes && b < a + a_bytes; }
uint64_t surface_bytes(const Surface& s) { return uint64_t{s.pitch} * s.height; }

// Whether guest memory [address, address + bytes) overlaps a surface drawn in this batch (other than `except`).
bool touched_overlap(const Renderer& r, uint32_t address, uint64_t bytes, const SurfaceEntry* except) {
    for (const SurfaceEntry* s : r.touched)
        if (s != except && overlaps(address, bytes, s->desc.address, surface_bytes(s->desc))) return true;
    return false;
}

}  // namespace

// The batch: everything drawn since the last flush is submitted, waited for, and its surfaces written back to guest memory.
void renderer_flush() {
    if (!g_renderer || !g_renderer->recording) return;
    Renderer& r = *g_renderer;
    const host::Gpu& gpu = *r.gpu;
    end_rendering(r);
    for (SurfaceEntry* s : r.touched) {
        host::transition(r.cmd, s->image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {s->desc.width, s->desc.height, 1};
        vkCmdCopyImageToBuffer(r.cmd, s->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->staging.buffer, 1, &copy);
    }
    // Everything copied back is visible to the host once the fence is signalled.
    VkMemoryBarrier2 to_host_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    to_host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, to_host_barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    to_host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, to_host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1, dependency.pMemoryBarriers = &to_host_barrier;
    vkCmdPipelineBarrier2(r.cmd, &dependency);
    host::check(vkEndCommandBuffer(r.cmd), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1, submit.pCommandBuffers = &r.cmd;
    {
        const std::lock_guard lock(*r.queue);
        host::check(vkQueueSubmit(gpu.queue, 1, &submit, r.fence), "vkQueueSubmit");
    }
    host::check(vkWaitForFences(gpu.device, 1, &r.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    host::check(vkResetFences(gpu.device, 1, &r.fence), "vkResetFences");
    // The surfaces back to guest memory: drawn, as the game expects to find them.
    for (SurfaceEntry* s : r.touched) {
        for (uint32_t y = 0; y < s->desc.height; ++y)
            to_guest(s->desc.format, static_cast<const uint8_t*>(s->staging.mapped) + size_t{y} * s->desc.width * 4, guest_memory() + s->desc.address + size_t{y} * s->desc.pitch,
                     s->desc.width);
        s->touched = false;
    }
    r.touched.clear();
    for (host::Image& image : r.batch_images) image.destroy(gpu);
    r.batch_images.clear();
    host::check(vkResetDescriptorPool(gpu.device, r.descriptors, 0), "vkResetDescriptorPool");
    r.sets_used = 0;
    r.arena_used = 0;
    r.recording = false;
    ++r.flushes;
}

void renderer_draw(const Draw& d) {
    if (!g_renderer) fatal("the game draws, and there is no renderer (no window)");
    Renderer& r = *g_renderer;
    const host::Gpu& gpu = *r.gpu;
    const qilin::rt::TranslatedShader& vs = find_shader(d.vertex_program, true);
    const qilin::rt::TranslatedShader& fs = find_shader(d.fragment_program, false);
    if (fs.cube_textures) fatal("the fragment program at %08x samples cube maps (units %#x), which the renderer doesn't provide yet", fs.address, fs.cube_textures);
    // What the fragment program reads, the vertex program must write (COL0 1, COL1 2, FOGC 5, TEXn 7 + n).
    static constexpr unsigned kLocation[14] = {0, 1, 2, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    for (unsigned n = 1; n < 14; ++n)
        if ((fs.inputs & (1u << n)) && !(vs.outputs & (1u << kLocation[n])))
            fatal("the fragment program at %08x reads input %u, which the vertex program at %08x doesn't write", fs.address, n, vs.address);
    // The primitive (the RSX's numbers: OpenGL's plus one). Those Vulkan lacks, or may lack (fans, on portability devices), are
    // drawn as lists of triangles through indices: a quad (a, b, c, d) as (a, b, c) and (a, c, d), a fan's or polygon's
    // triangles about its first vertex (*the diagonals are a stated choice*).
    VkPrimitiveTopology topology;
    std::vector<uint32_t> indices;
    bool indexed = false;
    switch (d.primitive) {
    case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 4: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case 7: case 10:  // triangle fan, polygon
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, indexed = true;
        for (uint32_t v = 2; v < d.count; ++v) indices.insert(indices.end(), {0, v - 1, v});
        break;
    case 8:  // quads
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, indexed = true;
        for (uint32_t v = 0; v + 4 <= d.count; v += 4) indices.insert(indices.end(), {v, v + 1, v + 2, v, v + 2, v + 3});
        break;
    case 9:  // quad strip: quads (v, v + 1, v + 3, v + 2)
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, indexed = true;
        for (uint32_t v = 0; v + 4 <= d.count; v += 2) indices.insert(indices.end(), {v, v + 1, v + 3, v, v + 3, v + 2});
        break;
    default: fatal("a draw of primitive %u, which the renderer doesn't take yet", d.primitive);
    }
    if (d.cull) fatal("a draw with culling, which the renderer doesn't take yet");
    uint32_t pixel_bytes = 0;
    const VkFormat format = surface_format(d.color.format, pixel_bytes);

    // The surface: kept on the GPU for the batch. One that overlaps another drawn in this batch (the same memory seen as
    // another surface) first has the batch written back.
    SurfaceEntry& target = r.surfaces[{d.color.address, d.color.format, d.color.width, d.color.height}];
    if (!target.image.image) {
        target.desc = d.color;
        target.image = host::Image::create(gpu, d.color.width, d.color.height, format,
                                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        target.staging = host::Buffer::host_visible(gpu, VkDeviceSize{d.color.width} * d.color.height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    if (target.desc.pitch != d.color.pitch) fatal("a surface at %08x drawn with pitch %u, and before with %u", d.color.address, d.color.pitch, target.desc.pitch);
    if (touched_overlap(r, d.color.address, surface_bytes(d.color), &target)) renderer_flush();

    // The textures the fragment program samples, read from guest memory: after the batch is written back, if they
    // overlap a surface drawn in it (a picture drawn and then sampled).
    struct Upload {
        uint32_t unit;
        Texels texels;
        VkSampler sampler;
    };
    std::vector<Upload> uploads;
    for (int pass = 0; pass < 2; ++pass) {
        uploads.clear();
        bool overlap = false;
        for (uint32_t u = 0; u < 16; ++u) {
            if (!(fs.textures & (1u << u))) continue;
            const TextureUnit& t = d.textures[u];
            if (!t.enabled()) fatal("the fragment program at %08x samples texture %u, which is not enabled", fs.address, u);
            Upload& up = uploads.emplace_back();
            up.unit = u;
            if (const std::string why = decode_texture(t, guest_memory(), up.texels); !why.empty())
                fatal("texture %u (at %08x, format %08x): %s, which the renderer doesn't take yet", u, t.address, t.format, why.c_str());
            SamplerFields fields;
            if (const std::string why = decode_sampler(t, fields); !why.empty()) fatal("texture %u: %s, which the renderer doesn't take yet", u, why.c_str());
            up.sampler = sampler_for(u, fields, up.texels.levels);
            overlap = overlap || touched_overlap(r, t.address, up.texels.guest_bytes, nullptr);
        }
        if (!overlap) break;
        renderer_flush();
    }

    // What the draw takes from the arena, and two descriptor sets: if the batch hasn't room, it is flushed first.
    uint32_t attributes = 0;
    for (uint32_t a = 0; a < 16; ++a) attributes += (vs.inputs >> a) & 1;
    VkDeviceSize needed = r.align * 8 + VkDeviceSize{16} * attributes * d.count + indices.size() * 4 + kVertexConstants * 16 + fs.constant_slot_count * 16;
    for (const Upload& up : uploads) needed += up.texels.rgba.size() + r.align;
    if (needed > r.arena.size) fatal("a draw needs %llu bytes of the renderer's arena, which has %llu", static_cast<unsigned long long>(needed), static_cast<unsigned long long>(r.arena.size));
    if (r.recording && (r.arena_used + needed > r.arena.size || r.sets_used + 2 > kMostSets)) renderer_flush();
    VkCommandBuffer cmd = r.cmd;
    if (!r.recording) {
        host::check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        host::check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
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
    const bool first_touch = !target.touched;
    if (!uploads.empty() || first_touch || r.rendering != &target) end_rendering(r);
    VkDescriptorSet set1 = VK_NULL_HANDLE;
    if (!uploads.empty()) {
        ds.pSetLayouts = &r.sets[1];
        host::check(vkAllocateDescriptorSets(gpu.device, &ds, &set1), "vkAllocateDescriptorSets");
        ++r.sets_used;
        std::vector<VkDescriptorImageInfo> images(uploads.size());
        std::vector<VkWriteDescriptorSet> texture_writes(uploads.size(), {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET});
        for (size_t i = 0; i < uploads.size(); ++i) {
            Upload& up = uploads[i];
            host::Image& image = r.batch_images.emplace_back(host::Image::create(gpu, up.texels.width, up.texels.height, VK_FORMAT_R8G8B8A8_UNORM,
                                                                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, up.texels.levels));
            const VkDeviceSize at = arena_take(r, up.texels.rgba.size());
            std::memcpy(arena_at(r, at), up.texels.rgba.data(), up.texels.rgba.size());
            host::transition(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            std::vector<VkBufferImageCopy> levels(up.texels.levels);
            for (uint32_t l = 0; l < up.texels.levels; ++l) {
                levels[l].bufferOffset = at + up.texels.level_offset[l];
                levels[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                levels[l].imageExtent = {std::max(1u, up.texels.width >> l), std::max(1u, up.texels.height >> l), 1};
            }
            vkCmdCopyBufferToImage(cmd, r.arena.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, up.texels.levels, levels.data());
            host::transition(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            images[i] = {up.sampler, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            texture_writes[i].dstSet = set1, texture_writes[i].dstBinding = up.unit, texture_writes[i].descriptorCount = 1;
            texture_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texture_writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(gpu.device, static_cast<uint32_t>(texture_writes.size()), texture_writes.data(), 0, nullptr);
    }
    if (first_touch) {
        // The surface from guest memory, as the batch begins with it.
        for (uint32_t y = 0; y < d.color.height; ++y)
            to_host(d.color.format, guest_memory() + d.color.address + size_t{y} * d.color.pitch, static_cast<uint8_t*>(target.staging.mapped) + size_t{y} * d.color.width * 4,
                    d.color.width);
        host::transition(cmd, target.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {d.color.width, d.color.height, 1};
        vkCmdCopyBufferToImage(cmd, target.staging.buffer, target.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        host::transition(cmd, target.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        target.touched = true;
        r.touched.push_back(&target);
    }
    if (r.rendering != &target) {
        // (Between rendering instances on a surface, its earlier draws are made visible to the next.)
        if (!first_touch) host::transition(cmd, target.image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = target.image.view, attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {d.color.width, d.color.height}};
        rendering.layerCount = 1, rendering.colorAttachmentCount = 1, rendering.pColorAttachments = &attachment;
        vkCmdBeginRendering(cmd, &rendering);
        r.rendering = &target;
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
    ++r.draws;
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

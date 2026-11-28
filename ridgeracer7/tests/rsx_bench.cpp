// The RSX's side of a frame, timed on the CPU with no game and no GPU: a synthetic frame of command words (2,000 draws with
// the state changes, vertex constant uploads and texture binds a race's frame has) run through the RSX's command
// processing and draw gathering (rsx_state.hpp) and the renderer's per-draw work (renderer.cpp), with every Vulkan call
// answered by a stub of this file's. The stubs also digest what the GPU would have been given (the draws, their vertices,
// indices, constants and push constants, the texels uploaded, the pipelines' state, the surfaces written back), so that a
// change meant to be faster can be shown to give the GPU exactly the same.
//
//   ridgeracer7_rsx_bench [--frames N] [--lazy] [--touch]
//     --lazy   surfaces are written back when touched (as when faults are handled), not at every flush
//     --touch  ...and touched after every frame, as the game reading a surface would (implies --lazy)
//
// It prints microseconds per frame for each part, and the digest. In ctest it runs two frames, as a test that the whole
// path runs.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "gcm.hpp"
#include "qilin/host/vulkan.hpp"
#include "qilin/rt/memory.hpp"
#include "qilin/rt/shaders.hpp"
#include "render_formats.hpp"
#include "renderer.hpp"
#include "rsx_state.hpp"

namespace {

// ---- The guest: its address space, the RSX's mapping of main memory ----

qilin::rt::AddressSpace* g_space = nullptr;
bool g_lazy = false;
constexpr uint32_t kIo = 0x20000000u, kIoSize = 16u << 20;  // main memory the RSX reads: the command stream
uint32_t io_address(uint32_t offset) { return offset < kIoSize ? kIo + offset : 0; }

// The RSX's memory, as the synthetic frame lays it out.
constexpr uint32_t kWidth = 1280, kHeight = 720, kPitch = kWidth * 4;
constexpr uint32_t kColour = 0, kDepth = 4u << 20, kVertices = 8u << 20, kIndices = 16u << 20, kTextures = 20u << 20, kFragment = 24u << 20;
constexpr uint32_t kDraws = 2000, kTextureCount = 64, kVertexStride = 32, kVerticesPerDraw = 100, kIndicesPerDraw = 150;

// ---- A digest of what the GPU is given ----

uint64_t g_digest = 0xcbf29ce484222325ull;
bool g_digesting = true;  // (off while timing: the digest reads far more than the renderer writes)
void digest(const void* p, size_t n) {
    if (!g_digesting) return;
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) g_digest = (g_digest ^ b[i]) * 0x100000001b3ull;
}
template <class T>
void digest_value(const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    digest(&v, sizeof v);
}

// ---- Vulkan, stubbed ----

// Handles counted by type, so that one kind made more or less often (descriptor sets) leaves the others' numbers alone.
template <class H>
H fake_handle() {
    static uintptr_t next = 0x10000;
    next += 16;
    return reinterpret_cast<H>(next);
}
// A buffer: its size, and where its memory is once bound.
struct FakeBuffer {
    VkDeviceSize size = 0;
    uint8_t* memory = nullptr;
};
template <class F>
struct Nop;
template <class R, class... A>
struct Nop<R(VKAPI_PTR*)(A...)> {
    static R VKAPI_PTR call(A...) {
        if constexpr (!std::is_void_v<R>) return R{};
    }
};
#define NOP(name) name = Nop<PFN_##name>::call

// What a pipeline fixes of a draw's GPU state, and what the command buffer's dynamic state commands set: at each draw the
// stubs digest the state in effect (the pipeline's, with its dynamic states taken from the command buffer), not handles,
// so that state moved out of the pipelines into dynamic state (or pipelines made fewer) can be shown to give the GPU the same.
struct GpuState {
    VkPrimitiveTopology topology{};
    VkBool32 restart = VK_FALSE;
    VkCullModeFlags cull = 0;
    VkFrontFace front{};
    VkPolygonMode polygon{};
    VkBool32 bias = VK_FALSE;
    float bias_constant = 0, bias_clamp = 0, bias_slope = 0;
    VkBool32 depth_test = VK_FALSE, depth_write = VK_FALSE;
    VkCompareOp depth_compare{};
    VkBool32 bounds = VK_FALSE;
    float bounds_min = 0, bounds_max = 0;
    VkBool32 stencil = VK_FALSE;
    VkStencilOpState stencil_front{}, stencil_back{};
};
void digest_stencil(const VkStencilOpState& s) {
    digest_value(s.failOp), digest_value(s.passOp), digest_value(s.depthFailOp), digest_value(s.compareOp);
    digest_value(s.compareMask), digest_value(s.writeMask), digest_value(s.reference);
}
void digest_state(const GpuState& s) {
    digest_value(s.topology), digest_value(s.restart), digest_value(s.cull), digest_value(s.front), digest_value(s.polygon);
    digest_value(s.bias), digest_value(s.bias_constant), digest_value(s.bias_clamp), digest_value(s.bias_slope);
    digest_value(s.depth_test), digest_value(s.depth_write), digest_value(s.depth_compare);
    digest_value(s.bounds), digest_value(s.bounds_min), digest_value(s.bounds_max);
    digest_value(s.stencil), digest_stencil(s.stencil_front), digest_stencil(s.stencil_back);
}
// The dynamic states the renderer may use, as bits.
enum Dynamic : uint32_t {
    kCull = 1u << 0, kFront = 1u << 1, kTopology = 1u << 2, kRestart = 1u << 3, kDepthTest = 1u << 4, kDepthWrite = 1u << 5, kDepthCompare = 1u << 6,
    kBoundsTest = 1u << 7, kStencilTest = 1u << 8, kStencilOp = 1u << 9, kBiasEnable = 1u << 10, kBias = 1u << 11, kBounds = 1u << 12,
    kStencilCompareMask = 1u << 13, kStencilWriteMask = 1u << 14, kStencilReference = 1u << 15, kViewport = 1u << 16, kScissor = 1u << 17,
};
uint32_t dynamic_bit(VkDynamicState d) {
    switch (d) {
    case VK_DYNAMIC_STATE_CULL_MODE: return kCull;
    case VK_DYNAMIC_STATE_FRONT_FACE: return kFront;
    case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: return kTopology;
    case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE: return kRestart;
    case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE: return kDepthTest;
    case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE: return kDepthWrite;
    case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP: return kDepthCompare;
    case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE: return kBoundsTest;
    case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE: return kStencilTest;
    case VK_DYNAMIC_STATE_STENCIL_OP: return kStencilOp;
    case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE: return kBiasEnable;
    case VK_DYNAMIC_STATE_DEPTH_BIAS: return kBias;
    case VK_DYNAMIC_STATE_DEPTH_BOUNDS: return kBounds;
    case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: return kStencilCompareMask;
    case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK: return kStencilWriteMask;
    case VK_DYNAMIC_STATE_STENCIL_REFERENCE: return kStencilReference;
    case VK_DYNAMIC_STATE_VIEWPORT: return kViewport;
    case VK_DYNAMIC_STATE_SCISSOR: return kScissor;
    default: qilin::rt::fatal("the bench's stubs don't know dynamic state %d", static_cast<int>(d));
    }
}
// A pipeline as made: its fixed state, which of it is dynamic, its shaders and their constants, its blending, formats and
// vertex input.
struct PipelineRecord {
    GpuState state;
    uint32_t dynamic = 0;
    std::array<VkShaderModule, 2> modules{};
    std::vector<uint8_t> specialisation;
    VkPipelineColorBlendAttachmentState blend{};
    VkBool32 logic = VK_FALSE;
    VkLogicOp logic_op{};
    VkFormat colour{}, depth{}, stencil{};
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkVertexInputBindingDescription> bindings;
};
std::mutex g_pipelines_mutex;  // (pipelines may be made on the renderer's worker thread)
std::unordered_map<VkPipeline, PipelineRecord> g_pipelines;
std::unordered_map<VkShaderModule, uint64_t> g_modules;  // each module's code, hashed (not its handle: modules may be made in any order)
uint64_t g_pipelines_made = 0;

// What the command buffer has bound and set, for the digest at each draw.
struct Bound {
    std::array<const uint8_t*, 16> vertices{};
    const uint8_t* indices = nullptr;
    std::array<uint32_t, 2> constant_offsets{};
    const uint8_t* arena = nullptr;  // the buffer set 0 points into (the batch's arena)
    const PipelineRecord* pipeline = nullptr;
    GpuState dynamic;  // the dynamic state set
    VkDescriptorSet textures = VK_NULL_HANDLE;
    VkRect2D scissor{};
    VkViewport viewport{};
    std::array<uint8_t, 64> push{};
} g_bound;
// What each texture set holds: (binding, view, sampler) for each texture written to it.
struct SetTexture {
    uint32_t binding;
    VkImageView view;
    VkSampler sampler;
};
std::unordered_map<VkDescriptorSet, std::vector<SetTexture>> g_sets;
std::unordered_map<VkBuffer, FakeBuffer*> g_buffers;
uint32_t g_vertex_bindings_first = 0, g_vertex_bindings_count = 0;
uint64_t g_vk_draws = 0;

// A vertex attribute's value as the GPU reads it, by its Vulkan format (little-endian components; missing ones 0, 0, 0, 1).
std::array<float, 4> read_attribute(VkFormat format, const uint8_t* p) {
    std::array<float, 4> v{0, 0, 0, 1};
    const auto f32 = [&](uint32_t c) {
        float x;
        std::memcpy(&x, p + 4 * c, 4);
        return x;
    };
    const auto s16 = [&](uint32_t c) {
        int16_t x;
        std::memcpy(&x, p + 2 * c, 2);
        return x;
    };
    const auto f16 = [&](uint32_t c) {
        const uint8_t be[2] = {p[2 * c + 1], p[2 * c]};
        return rr7::half_float(be);
    };
    const auto components = [](VkFormat one, VkFormat two, VkFormat f) { return f == one ? 1u : f == two ? 2u : 4u; };
    switch (format) {
    case VK_FORMAT_R32_SFLOAT: v[0] = f32(0); break;
    case VK_FORMAT_R32G32_SFLOAT: v[0] = f32(0), v[1] = f32(1); break;
    case VK_FORMAT_R32G32B32_SFLOAT: v[0] = f32(0), v[1] = f32(1), v[2] = f32(2); break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: v = {f32(0), f32(1), f32(2), f32(3)}; break;
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        for (uint32_t c = 0; c < components(VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, format); ++c) v[c] = f16(c);
        break;
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
        for (uint32_t c = 0; c < components(VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, format); ++c) v[c] = std::max(-1.0f, static_cast<float>(s16(c)) / 32767.0f);
        break;
    case VK_FORMAT_R16_SSCALED:
    case VK_FORMAT_R16G16_SSCALED:
    case VK_FORMAT_R16G16B16A16_SSCALED:
        for (uint32_t c = 0; c < components(VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, format); ++c) v[c] = static_cast<float>(s16(c));
        break;
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
        for (uint32_t c = 0; c < components(VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, format); ++c) v[c] = static_cast<float>(p[c]) / 255.0f;
        break;
    case VK_FORMAT_R8_USCALED:
    case VK_FORMAT_R8G8_USCALED:
    case VK_FORMAT_R8G8B8A8_USCALED:
        for (uint32_t c = 0; c < components(VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, format); ++c) v[c] = static_cast<float>(p[c]);
        break;
    default: qilin::rt::fatal("the bench's stubs don't read vertex format %d", static_cast<int>(format));
    }
    return v;
}

// What a draw sees: its state, shaders, blending, textures, window, push constants, vertices (each attribute's values, as
// the GPU reads them through the pipeline's formats) and constants.
void digest_draw(uint32_t vertices) {
    if (!g_digesting) return;
    const PipelineRecord& p = *g_bound.pipeline;
    GpuState s = p.state;
    const GpuState& d = g_bound.dynamic;
    if (p.dynamic & kCull) s.cull = d.cull;
    if (p.dynamic & kFront) s.front = d.front;
    if (p.dynamic & kTopology) s.topology = d.topology;
    if (p.dynamic & kRestart) s.restart = d.restart;
    if (p.dynamic & kDepthTest) s.depth_test = d.depth_test;
    if (p.dynamic & kDepthWrite) s.depth_write = d.depth_write;
    if (p.dynamic & kDepthCompare) s.depth_compare = d.depth_compare;
    if (p.dynamic & kBoundsTest) s.bounds = d.bounds;
    if (p.dynamic & kBounds) s.bounds_min = d.bounds_min, s.bounds_max = d.bounds_max;
    if (p.dynamic & kStencilTest) s.stencil = d.stencil;
    if (p.dynamic & kBiasEnable) s.bias = d.bias;
    if (p.dynamic & kBias) s.bias_constant = d.bias_constant, s.bias_clamp = d.bias_clamp, s.bias_slope = d.bias_slope;
    const std::pair<VkStencilOpState*, const VkStencilOpState*> faces[2] = {{&s.stencil_front, &d.stencil_front}, {&s.stencil_back, &d.stencil_back}};
    for (const auto& [face, set] : faces) {
        if (p.dynamic & kStencilOp) face->failOp = set->failOp, face->passOp = set->passOp, face->depthFailOp = set->depthFailOp, face->compareOp = set->compareOp;
        if (p.dynamic & kStencilCompareMask) face->compareMask = set->compareMask;
        if (p.dynamic & kStencilWriteMask) face->writeMask = set->writeMask;
        if (p.dynamic & kStencilReference) face->reference = set->reference;
    }
    digest_state(s);
    for (const VkShaderModule m : p.modules) digest_value(g_modules.at(m));
    digest(p.specialisation.data(), p.specialisation.size());
    digest_value(p.blend), digest_value(p.logic), digest_value(p.logic_op), digest_value(p.colour), digest_value(p.depth), digest_value(p.stencil);
    digest_value(g_bound.scissor), digest_value(g_bound.viewport), digest(g_bound.push.data(), g_bound.push.size());
    if (g_bound.textures)
        for (const SetTexture& t : g_sets.at(g_bound.textures)) digest_value(t.binding), digest_value(t.view), digest_value(t.sampler);
    for (const VkVertexInputAttributeDescription& a : p.attributes) {
        const auto binding = std::ranges::find_if(p.bindings, [&](const VkVertexInputBindingDescription& b) { return b.binding == a.binding; });
        if (binding == p.bindings.end()) qilin::rt::fatal("a pipeline's attribute %u reads binding %u, which it doesn't describe", a.location, a.binding);
        const uint32_t slot = a.binding - g_vertex_bindings_first;
        if (slot >= g_vertex_bindings_count || !g_bound.vertices[slot]) qilin::rt::fatal("a draw reads binding %u, which isn't bound", a.binding);
        digest_value(a.location);
        for (uint32_t v = 0; v < vertices; ++v) {
            const size_t at = a.offset + (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? 0 : size_t{v} * binding->stride);
            digest_value(read_attribute(a.format, g_bound.vertices[slot] + at));
        }
    }
    if (g_bound.arena) {
        digest(g_bound.arena + g_bound.constant_offsets[0], 468 * 16);
        digest(g_bound.arena + g_bound.constant_offsets[1], 32);
    }
}

void stub_vulkan() {
    vkCreateBuffer = [](VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* out) {
        *out = fake_handle<VkBuffer>();
        g_buffers[*out] = new FakeBuffer{info->size, nullptr};
        return VK_SUCCESS;
    };
    vkDestroyBuffer = [](VkDevice, VkBuffer b, const VkAllocationCallbacks*) {
        if (const auto it = g_buffers.find(b); it != g_buffers.end()) delete it->second, g_buffers.erase(it);
    };
    vkGetBufferMemoryRequirements = [](VkDevice, VkBuffer b, VkMemoryRequirements* req) {
        *req = {};
        req->size = g_buffers.at(b)->size, req->alignment = 256, req->memoryTypeBits = 1;
    };
    vkGetImageMemoryRequirements = [](VkDevice, VkImage, VkMemoryRequirements* req) {
        *req = {};
        req->size = 256, req->alignment = 256, req->memoryTypeBits = 1;
    };
    vkAllocateMemory = [](VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* out) {
        *out = reinterpret_cast<VkDeviceMemory>(std::calloc(1, info->allocationSize));
        return VK_SUCCESS;
    };
    vkFreeMemory = [](VkDevice, VkDeviceMemory m, const VkAllocationCallbacks*) { std::free(reinterpret_cast<void*>(m)); };
    vkMapMemory = [](VkDevice, VkDeviceMemory m, VkDeviceSize offset, VkDeviceSize, VkMemoryMapFlags, void** out) {
        *out = reinterpret_cast<uint8_t*>(m) + offset;
        return VK_SUCCESS;
    };
    vkBindBufferMemory = [](VkDevice, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset) {
        g_buffers.at(b)->memory = reinterpret_cast<uint8_t*>(m) + offset;
        return VK_SUCCESS;
    };
    NOP(vkBindImageMemory);
    vkGetPhysicalDeviceMemoryProperties = [](VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* p) {
        *p = {};
        p->memoryTypeCount = 1, p->memoryHeapCount = 1;
        p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        p->memoryHeaps[0].size = VkDeviceSize{1} << 34;
    };
    vkGetPhysicalDeviceFormatProperties = [](VkPhysicalDevice, VkFormat, VkFormatProperties* p) {
        p->linearTilingFeatures = p->optimalTilingFeatures = p->bufferFeatures = ~VkFormatFeatureFlags{0};
    };
    vkCreateImage = [](VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* out) { return *out = fake_handle<VkImage>(), VK_SUCCESS; };
    vkCreateImageView = [](VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* out) { return *out = fake_handle<VkImageView>(), VK_SUCCESS; };
    NOP(vkDestroyImage);
    NOP(vkDestroyImageView);
    vkCreateSampler = [](VkDevice, const VkSamplerCreateInfo* info, const VkAllocationCallbacks*, VkSampler* out) {
        digest_value(info->magFilter), digest_value(info->minFilter), digest_value(info->addressModeU), digest_value(info->maxLod);
        return *out = fake_handle<VkSampler>(), VK_SUCCESS;
    };
    vkCreateShaderModule = [](VkDevice, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks*, VkShaderModule* out) {
        const std::lock_guard lock(g_pipelines_mutex);
        *out = fake_handle<VkShaderModule>();
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < info->codeSize; ++i) h = (h ^ reinterpret_cast<const uint8_t*>(info->pCode)[i]) * 0x100000001b3ull;
        g_modules[*out] = h;
        return VK_SUCCESS;
    };
    vkCreateGraphicsPipelines = [](VkDevice, VkPipelineCache, uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks*, VkPipeline* out) {
        const std::lock_guard lock(g_pipelines_mutex);
        for (uint32_t i = 0; i < count; ++i) {
            const VkGraphicsPipelineCreateInfo& info = infos[i];
            PipelineRecord p;
            GpuState& s = p.state;
            s.topology = info.pInputAssemblyState->topology, s.restart = info.pInputAssemblyState->primitiveRestartEnable;
            const VkPipelineRasterizationStateCreateInfo& r = *info.pRasterizationState;
            s.cull = r.cullMode, s.front = r.frontFace, s.polygon = r.polygonMode;
            s.bias = r.depthBiasEnable, s.bias_constant = r.depthBiasConstantFactor, s.bias_clamp = r.depthBiasClamp, s.bias_slope = r.depthBiasSlopeFactor;
            const VkPipelineDepthStencilStateCreateInfo& z = *info.pDepthStencilState;
            s.depth_test = z.depthTestEnable, s.depth_write = z.depthWriteEnable, s.depth_compare = z.depthCompareOp;
            s.bounds = z.depthBoundsTestEnable, s.bounds_min = z.minDepthBounds, s.bounds_max = z.maxDepthBounds;
            s.stencil = z.stencilTestEnable, s.stencil_front = z.front, s.stencil_back = z.back;
            if (info.pDynamicState)
                for (uint32_t k = 0; k < info.pDynamicState->dynamicStateCount; ++k) p.dynamic |= dynamic_bit(info.pDynamicState->pDynamicStates[k]);
            for (uint32_t k = 0; k < info.stageCount && k < 2; ++k) {
                p.modules[k] = info.pStages[k].module;
                if (const VkSpecializationInfo* sp = info.pStages[k].pSpecializationInfo) {
                    const auto* bytes = static_cast<const uint8_t*>(sp->pData);
                    p.specialisation.insert(p.specialisation.end(), bytes, bytes + sp->dataSize);
                }
            }
            p.blend = info.pColorBlendState->pAttachments[0];
            p.logic = info.pColorBlendState->logicOpEnable, p.logic_op = info.pColorBlendState->logicOpEnable ? info.pColorBlendState->logicOp : VkLogicOp{};
            for (const auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext)
                if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                    const auto& rendering = *reinterpret_cast<const VkPipelineRenderingCreateInfo*>(next);
                    p.colour = rendering.colorAttachmentCount ? rendering.pColorAttachmentFormats[0] : VK_FORMAT_UNDEFINED;
                    p.depth = rendering.depthAttachmentFormat, p.stencil = rendering.stencilAttachmentFormat;
                }
            const VkPipelineVertexInputStateCreateInfo& v = *info.pVertexInputState;
            p.attributes.assign(v.pVertexAttributeDescriptions, v.pVertexAttributeDescriptions + v.vertexAttributeDescriptionCount);
            p.bindings.assign(v.pVertexBindingDescriptions, v.pVertexBindingDescriptions + v.vertexBindingDescriptionCount);
            std::ranges::sort(p.attributes, {}, &VkVertexInputAttributeDescription::location);
            out[i] = fake_handle<VkPipeline>();
            g_pipelines[out[i]] = std::move(p);
            ++g_pipelines_made;
        }
        return VK_SUCCESS;
    };
    vkCreatePipelineCache = [](VkDevice, const VkPipelineCacheCreateInfo*, const VkAllocationCallbacks*, VkPipelineCache* out) {
        return *out = fake_handle<VkPipelineCache>(), VK_SUCCESS;
    };
    vkGetPipelineCacheData = [](VkDevice, VkPipelineCache, size_t* size, void* data) {
        // (A cache's data as a driver gives it: here 32 bytes of nothing.)
        if (data) std::memset(data, 0, std::min<size_t>(*size, 32));
        *size = 32;
        return VK_SUCCESS;
    };
    NOP(vkDestroyPipeline);
    NOP(vkDestroyPipelineCache);
    vkCmdSetCullMode = [](VkCommandBuffer, VkCullModeFlags v) { g_bound.dynamic.cull = v; };
    vkCmdSetFrontFace = [](VkCommandBuffer, VkFrontFace v) { g_bound.dynamic.front = v; };
    vkCmdSetPrimitiveTopology = [](VkCommandBuffer, VkPrimitiveTopology v) { g_bound.dynamic.topology = v; };
    vkCmdSetPrimitiveRestartEnable = [](VkCommandBuffer, VkBool32 v) { g_bound.dynamic.restart = v; };
    vkCmdSetDepthTestEnable = [](VkCommandBuffer, VkBool32 v) { g_bound.dynamic.depth_test = v; };
    vkCmdSetDepthWriteEnable = [](VkCommandBuffer, VkBool32 v) { g_bound.dynamic.depth_write = v; };
    vkCmdSetDepthCompareOp = [](VkCommandBuffer, VkCompareOp v) { g_bound.dynamic.depth_compare = v; };
    vkCmdSetDepthBoundsTestEnable = [](VkCommandBuffer, VkBool32 v) { g_bound.dynamic.bounds = v; };
    vkCmdSetDepthBounds = [](VkCommandBuffer, float lo, float hi) { g_bound.dynamic.bounds_min = lo, g_bound.dynamic.bounds_max = hi; };
    vkCmdSetStencilTestEnable = [](VkCommandBuffer, VkBool32 v) { g_bound.dynamic.stencil = v; };
    vkCmdSetDepthBiasEnable = [](VkCommandBuffer, VkBool32 v) { g_bound.dynamic.bias = v; };
    vkCmdSetDepthBias = [](VkCommandBuffer, float constant, float clamp, float slope) {
        g_bound.dynamic.bias_constant = constant, g_bound.dynamic.bias_clamp = clamp, g_bound.dynamic.bias_slope = slope;
    };
    vkCmdSetStencilOp = [](VkCommandBuffer, VkStencilFaceFlags faces, VkStencilOp fail, VkStencilOp pass, VkStencilOp depth_fail, VkCompareOp compare) {
        if (faces & VK_STENCIL_FACE_FRONT_BIT) g_bound.dynamic.stencil_front.failOp = fail, g_bound.dynamic.stencil_front.passOp = pass,
                                                g_bound.dynamic.stencil_front.depthFailOp = depth_fail, g_bound.dynamic.stencil_front.compareOp = compare;
        if (faces & VK_STENCIL_FACE_BACK_BIT) g_bound.dynamic.stencil_back.failOp = fail, g_bound.dynamic.stencil_back.passOp = pass,
                                               g_bound.dynamic.stencil_back.depthFailOp = depth_fail, g_bound.dynamic.stencil_back.compareOp = compare;
    };
    vkCmdSetStencilCompareMask = [](VkCommandBuffer, VkStencilFaceFlags faces, uint32_t v) {
        if (faces & VK_STENCIL_FACE_FRONT_BIT) g_bound.dynamic.stencil_front.compareMask = v;
        if (faces & VK_STENCIL_FACE_BACK_BIT) g_bound.dynamic.stencil_back.compareMask = v;
    };
    vkCmdSetStencilWriteMask = [](VkCommandBuffer, VkStencilFaceFlags faces, uint32_t v) {
        if (faces & VK_STENCIL_FACE_FRONT_BIT) g_bound.dynamic.stencil_front.writeMask = v;
        if (faces & VK_STENCIL_FACE_BACK_BIT) g_bound.dynamic.stencil_back.writeMask = v;
    };
    vkCmdSetStencilReference = [](VkCommandBuffer, VkStencilFaceFlags faces, uint32_t v) {
        if (faces & VK_STENCIL_FACE_FRONT_BIT) g_bound.dynamic.stencil_front.reference = v;
        if (faces & VK_STENCIL_FACE_BACK_BIT) g_bound.dynamic.stencil_back.reference = v;
    };
    vkAllocateCommandBuffers = [](VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) out[i] = fake_handle<VkCommandBuffer>();
        return VK_SUCCESS;
    };
    vkAllocateDescriptorSets = [](VkDevice, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out) {
        for (uint32_t i = 0; i < info->descriptorSetCount; ++i) out[i] = fake_handle<VkDescriptorSet>(), g_sets[out[i]].clear();
        return VK_SUCCESS;
    };
    vkUpdateDescriptorSets = [](VkDevice, uint32_t count, const VkWriteDescriptorSet* writes, uint32_t, const VkCopyDescriptorSet*) {
        for (uint32_t i = 0; i < count; ++i) {
            if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                g_bound.arena = g_buffers.at(writes[i].pBufferInfo->buffer)->memory;
                continue;
            }
            g_sets[writes[i].dstSet].push_back({writes[i].dstBinding, writes[i].pImageInfo->imageView, writes[i].pImageInfo->sampler});
        }
    };
    vkCreateFence = [](VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* out) { return *out = fake_handle<VkFence>(), VK_SUCCESS; };
    vkCreateCommandPool = [](VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* out) { return *out = fake_handle<VkCommandPool>(), VK_SUCCESS; };
    vkCreateDescriptorPool = [](VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool* out) { return *out = fake_handle<VkDescriptorPool>(), VK_SUCCESS; };
    vkCreateDescriptorSetLayout = [](VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* out) {
        return *out = fake_handle<VkDescriptorSetLayout>(), VK_SUCCESS;
    };
    vkCreatePipelineLayout = [](VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout* out) { return *out = fake_handle<VkPipelineLayout>(), VK_SUCCESS; };
    vkCreateQueryPool = [](VkDevice, const VkQueryPoolCreateInfo*, const VkAllocationCallbacks*, VkQueryPool* out) { return *out = fake_handle<VkQueryPool>(), VK_SUCCESS; };
    vkGetQueryPoolResults = [](VkDevice, VkQueryPool, uint32_t, uint32_t, size_t size, void* data, VkDeviceSize, VkQueryResultFlags) {
        std::memset(data, 0, size);
        return VK_SUCCESS;
    };
    NOP(vkResetDescriptorPool);
    NOP(vkResetCommandBuffer);
    NOP(vkBeginCommandBuffer);
    NOP(vkEndCommandBuffer);
    NOP(vkQueueSubmit);
    NOP(vkQueueWaitIdle);
    NOP(vkWaitForFences);
    NOP(vkResetFences);
    NOP(vkFlushMappedMemoryRanges);
    NOP(vkInvalidateMappedMemoryRanges);
    NOP(vkCmdResetQueryPool);
    NOP(vkCmdWriteTimestamp2);
    NOP(vkCmdPipelineBarrier2);
    NOP(vkCmdBeginRendering);
    NOP(vkCmdEndRendering);
    NOP(vkCmdCopyImage);
    NOP(vkCmdCopyImageToBuffer);
    vkCmdSetViewport = [](VkCommandBuffer, uint32_t, uint32_t, const VkViewport* v) { g_bound.viewport = *v; };
    vkCmdClearAttachments = [](VkCommandBuffer, uint32_t count, const VkClearAttachment* clears, uint32_t, const VkClearRect* rects) {
        for (uint32_t i = 0; i < count; ++i) digest_value(clears[i]);
        digest_value(rects[0]);
    };
    vkCmdSetScissor = [](VkCommandBuffer, uint32_t, uint32_t, const VkRect2D* r) { g_bound.scissor = *r; };
    vkCmdPushConstants = [](VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t offset, uint32_t size, const void* data) {
        std::memcpy(g_bound.push.data() + offset, data, size);
    };
    vkCmdBindPipeline = [](VkCommandBuffer, VkPipelineBindPoint, VkPipeline p) {
        const std::lock_guard lock(g_pipelines_mutex);
        g_bound.pipeline = &g_pipelines.at(p);
    };
    vkCmdBindDescriptorSets = [](VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t first, uint32_t, const VkDescriptorSet* sets, uint32_t dynamic_count,
                                 const uint32_t* dynamic) {
        if (first == 0 && dynamic_count == 2) g_bound.constant_offsets = {dynamic[0], dynamic[1]};
        if (first == 1) g_bound.textures = sets[0];
    };
    vkCmdBindVertexBuffers = [](VkCommandBuffer, uint32_t first, uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets) {
        g_vertex_bindings_first = first, g_vertex_bindings_count = count;
        for (uint32_t i = 0; i < count; ++i) g_bound.vertices[i] = g_buffers.at(buffers[i])->memory + offsets[i];
    };
    vkCmdBindIndexBuffer = [](VkCommandBuffer, VkBuffer b, VkDeviceSize offset, VkIndexType) { g_bound.indices = g_buffers.at(b)->memory + offset; };
    vkCmdDraw = [](VkCommandBuffer, uint32_t vertices, uint32_t, uint32_t, uint32_t) {
        ++g_vk_draws;
        digest_value(vertices);
        digest_draw(vertices);
    };
    vkCmdDrawIndexed = [](VkCommandBuffer, uint32_t indices, uint32_t, uint32_t, int32_t, uint32_t) {
        ++g_vk_draws;
        if (!g_digesting) return;
        digest_value(indices);
        const auto* list = reinterpret_cast<const uint32_t*>(g_bound.indices);
        digest(list, size_t{indices} * 4);
        uint32_t highest = 0;
        for (uint32_t i = 0; i < indices; ++i) highest = std::max(highest, list[i]);
        digest_draw(highest + 1);
    };
    vkCmdCopyBufferToImage = [](VkCommandBuffer, VkBuffer b, VkImage, VkImageLayout, uint32_t count, const VkBufferImageCopy* copies) {
        const FakeBuffer* from = g_buffers.at(b);
        for (uint32_t i = 0; i < count; ++i) digest(from->memory + copies[i].bufferOffset, size_t{copies[i].imageExtent.width} * copies[i].imageExtent.height * 4);
    };
}

// ---- The translated programs: synthetic ones, keyed as `qilin shaders` keys them ----

uint64_t fnv(const std::vector<uint8_t>& bytes) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const uint8_t c : bytes) h = (h ^ c) * 0x100000001b3ull;
    return h;
}
void put_be32(uint8_t* p, uint32_t v) {
    v = std::byteswap(v);
    std::memcpy(p, &v, 4);
}

constexpr unsigned kVertexPrograms = 4, kFragmentPrograms = 8, kVertexProgramLength = 6;
// A vertex program's words as uploaded (the last instruction marked by bit 0 of its fourth word).
std::array<uint32_t, 4> vertex_instruction(unsigned program, unsigned i) {
    return {0x00401c6cu ^ (program << 8) ^ i, 0x0040000du + i, 0x8106c083u, 0x6041ff80u | (i == kVertexProgramLength - 1 ? 1u : 0u)};
}
// A fragment program's bytes as the RSX reads them (each word's halves exchanged): four instructions, the first and the
// third with an inline constant after them (at bytes 16 and 64), the last marked as the end.
std::vector<uint8_t> fragment_microcode(unsigned program, uint32_t constant_seed) {
    std::vector<uint8_t> out;
    const auto instruction = [&](uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
        for (const uint32_t w : {w0, w1, w2, w3}) {
            out.resize(out.size() + 4);
            put_be32(out.data() + out.size() - 4, (w << 16) | (w >> 16));
        }
    };
    const auto constant = [&](uint32_t seed) {
        for (unsigned c = 0; c < 4; ++c) {
            out.resize(out.size() + 4);
            put_be32(out.data() + out.size() - 4, std::bit_cast<uint32_t>(static_cast<float>(seed % 97) / 97.0f + static_cast<float>(c)));
        }
    };
    instruction(0x9e000100u | (program << 9), 0x2u, 0x0u, 0x0u);  // a source of type 2: a constant follows
    constant(constant_seed);
    instruction(0x02001700u, 0x0u, 0x0u, 0x4u);
    instruction(0x04000e00u | (program << 12), 0x0u, 0x2u, 0x0u);
    constant(constant_seed * 31 + 7);
    instruction(0x10000001u, 0x0u, 0x0u, 0x0u);  // the end
    return out;
}

// (Each program's own code: the magic number and its address.)
uint32_t g_spirv[kVertexPrograms + kFragmentPrograms][2];
const uint32_t kConstantSlots[2] = {16, 64};

qilin::rt::TranslatedShader make_shader(bool vertex, unsigned program) {
    qilin::rt::TranslatedShader s{};
    s.vertex = vertex, s.address = (vertex ? 0x100000u : 0x200000u) + program * 0x100;
    uint32_t* code = g_spirv[vertex ? program : kVertexPrograms + program];
    code[0] = 0x07230203u, code[1] = s.address;
    s.spirv = code, s.spirv_words = 2;
    std::vector<uint8_t> keyed;
    if (vertex) {
        for (unsigned i = 0; i < kVertexProgramLength; ++i)
            for (const uint32_t w : vertex_instruction(program, i)) keyed.resize(keyed.size() + 4), put_be32(keyed.data() + keyed.size() - 4, w);
        s.inputs = 1u << 0 | 1u << 3 | 1u << 8;  // position, colour, texture coordinate 0
        s.outputs = 1u << 0 | 1u << 1 | 1u << 7;  // position, COL0, TEX0
    } else {
        keyed = fragment_microcode(program, 0);
        std::memset(keyed.data() + 16, 0, 16), std::memset(keyed.data() + 64, 0, 16);
        s.inputs = 1u << 1 | 1u << 4;  // COL0, TEX0
        s.textures = program % 4 == 3 ? 3u : 1u;
        s.constant_slots = kConstantSlots, s.constant_slot_count = 2;
    }
    s.key = fnv(keyed);
    return s;
}

}  // namespace

namespace qilin::rt {
extern const TranslatedShader kTranslatedShaders[kVertexPrograms + kFragmentPrograms] = {
    make_shader(true, 0),  make_shader(true, 1),  make_shader(true, 2),  make_shader(true, 3),  make_shader(false, 0), make_shader(false, 1),
    make_shader(false, 2), make_shader(false, 3), make_shader(false, 4), make_shader(false, 5), make_shader(false, 6), make_shader(false, 7)};
extern const size_t kTranslatedShaderCount = kVertexPrograms + kFragmentPrograms;
}  // namespace qilin::rt

// ---- What the renderer needs of the machine ----

namespace rr7 {
uint8_t* guest_memory() { return g_space->base(); }
bool faults_handled() { return g_lazy; }
uint32_t guest_page_size() { return 4096; }
void guest_protect(uint64_t, uint64_t, bool) {}
uint32_t current_thread() { return 0; }
}  // namespace rr7

namespace {

using Clock = std::chrono::steady_clock;

// ---- The synthetic frame ----

struct Stream {
    std::vector<uint32_t> words;
    void method(uint32_t m, std::initializer_list<uint32_t> args) {
        words.push_back(static_cast<uint32_t>(args.size()) << 18 | m);
        words.insert(words.end(), args);
    }
    void same(uint32_t m, const std::vector<uint32_t>& args) {
        words.push_back(0x40000000u | static_cast<uint32_t>(args.size()) << 18 | m);
        words.insert(words.end(), args.begin(), args.end());
    }
    void run(uint32_t m, const std::vector<uint32_t>& args) {
        words.push_back(static_cast<uint32_t>(args.size()) << 18 | m);
        words.insert(words.end(), args.begin(), args.end());
    }
};
uint32_t f(float v) { return std::bit_cast<uint32_t>(v); }

// The guest memory a frame reads: vertices, indices, textures, fragment programs (each draw's own copy, its constants
// patched, as the game writes them).
void fill_guest() {
    uint8_t* m = rr7::guest_memory();
    uint8_t* vertices = m + rr7::kRsxLocal + kVertices;
    for (uint32_t v = 0; v < kDraws * kVerticesPerDraw; ++v) {
        uint8_t* p = vertices + size_t{v} * kVertexStride;
        put_be32(p, f(static_cast<float>(v % 1280))), put_be32(p + 4, f(static_cast<float>(v % 720))), put_be32(p + 8, f(0.5f));
        put_be32(p + 12, 0x80ff40c0u ^ v);
        put_be32(p + 16, f(static_cast<float>(v % 7) / 7.0f)), put_be32(p + 20, f(static_cast<float>(v % 5) / 5.0f));
    }
    for (uint32_t d = 0; d < kDraws; ++d)
        for (uint32_t i = 0; i < kIndicesPerDraw; ++i) {
            const uint16_t index = static_cast<uint16_t>((i * 37 + d) % kVerticesPerDraw);
            m[rr7::kRsxLocal + kIndices + d * 512 + 2 * i] = static_cast<uint8_t>(index >> 8);
            m[rr7::kRsxLocal + kIndices + d * 512 + 2 * i + 1] = static_cast<uint8_t>(index);
        }
    for (uint32_t i = 0; i < kTextureCount * 32768; ++i) m[rr7::kRsxLocal + kTextures + i] = static_cast<uint8_t>(i * 2654435761u >> 24);
    for (uint32_t d = 0; d < kDraws; ++d) {
        const std::vector<uint8_t> program = fragment_microcode(d % kFragmentPrograms, d);
        std::memcpy(m + rr7::kRsxLocal + kFragment + d * 128, program.data(), program.size());
    }
}

// One frame's commands: the surfaces and the window set, a clear, 2,000 draws, a flip.
std::vector<uint32_t> frame_commands() {
    Stream s;
    s.method(0x0194, {0xFEED0000u, 0xFEED0000u});  // colour A, zeta: the RSX's memory
    s.method(0x0200, {kWidth << 16, kHeight << 16, 8u | 2u << 5 | 1u << 8, kPitch, kColour, kDepth});
    s.method(0x0220, {1});
    s.method(0x022c, {kPitch});
    s.method(0x0a00, {kWidth << 16, kHeight << 16});
    s.method(0x0a20, {f(640.0f), f(360.0f), f(0.5f), f(0.0f), f(640.0f), f(-360.0f), f(0.5f), f(0.0f)});
    s.method(0x1d88, {kHeight | 1u << 12});
    s.method(0x08c0, {kWidth << 16, kHeight << 16});
    s.method(0x0324, {0x01010101u});
    s.method(0x1454, {1});
    s.method(0x1d8c, {0xffffff00u, 0x00000000u});  // the clear values
    s.method(0x1d98, {0x0fff0000u, 0x0fff0000u});
    s.method(0x1d94, {0xf3});
    // The vertex arrays' formats: position F x3, colour UB x4, texture coordinate F x2, each 32 bytes apart.
    s.method(0x1740, {2u | 3u << 4 | kVertexStride << 8});
    s.method(0x174c, {4u | 4u << 4 | kVertexStride << 8});
    s.method(0x1760, {2u | 2u << 4 | kVertexStride << 8});
    for (uint32_t d = 0; d < kDraws; ++d) {
        if (d % 100 == 0) s.method(0x1ea0, {(d / 100 % kVertexPrograms) * 32});  // the vertex program
        if (d % 4 == 0) s.method(0x0a6c, {0x203u + d / 4 % 2, 1, 1});            // depth function, mask, test
        if (d % 10 == 0) s.method(0x0310, {d / 10 % 2, 0x03030302u, 0x03030303u});  // blending
        if (d % 10 == 0) s.method(0x0320, {0x80068006u});
        if (d % 20 == 0) s.method(0x1830, {0x405u, 0x901u});
        if (d % 20 == 0) s.method(0x183c, {d / 20 % 2});
        if (d % 7 == 0) s.method(0x08c0, {(kWidth - d % 64) << 16, kHeight << 16});
        s.method(0x08e4, {(kFragment + d * 128) | 1u});
        // The constants: a matrix every draw, 64 more every 50th.
        s.method(0x1efc, {0});
        std::vector<uint32_t> matrix(16);
        for (uint32_t i = 0; i < 16; ++i) matrix[i] = f(static_cast<float>(d) + static_cast<float>(i) * 0.25f);
        s.run(0x1f00, matrix);
        if (d % 50 == 0) {
            s.method(0x1efc, {16});
            for (uint32_t k = 0; k < 8; ++k) {
                std::vector<uint32_t> block(32);
                for (uint32_t i = 0; i < 32; ++i) block[i] = f(static_cast<float>(k * 32 + i) / 256.0f);
                s.run(0x1f00, block);
            }
        }
        // Texture 0 (and 1 for some programs): DXT1 256 by 256, or A8R8G8B8 swizzled 128 by 128.
        for (uint32_t u = 0; u < (d % kFragmentPrograms % 4 == 3 ? 2u : 1u); ++u) {
            const uint32_t which = (d * 7 + u * 13) % kTextureCount;
            const bool dxt = which % 2 == 0;
            s.method(0x1a00 + 32 * u, {kTextures + which * 32768, 1u | 2u << 4 | (dxt ? 0x86u : 0x85u) << 8 | 1u << 16, 0x00010101u, 0x80000000u, 0xaae4u, 0x02020000u,
                                       dxt ? (256u << 16 | 256u) : (128u << 16 | 128u), 0});
            s.method(0x1840 + 4 * u, {1u << 20});
        }
        s.method(0x1fd8, {1});
        // The arrays: each draw's vertices.
        const uint32_t base = kVertices + d * kVerticesPerDraw * kVertexStride;
        s.method(0x1680, {base});
        s.method(0x168c, {base + 12});
        s.method(0x16a0, {base + 16});
        s.method(0x1714, {0});
        if (d % 2 == 0) {
            s.method(0x181c, {kIndices + d * 512, 0x10});
            s.method(0x1808, {5});
            s.method(0x1824, {(kIndicesPerDraw - 1) << 24});
        } else {
            s.method(0x1808, {6});
            s.method(0x1814, {(kVerticesPerDraw - 1) << 24});
        }
        s.method(0x1808, {0});
    }
    s.method(rr7::kMethodFlip, {0});
    return s.words;
}

// What the RSX's thread does at the methods rsx_state.hpp hands back: here a clear, a flip (a flush), and the draws,
// gathered and drawn as far as `depth` asks.
enum class Depth { parse, gather, draw };
struct Acts {
    bool trace = false;
    Depth depth = Depth::draw;
    rr7::RsxState* s = nullptr;
    uint64_t draws = 0;
    rr7::Draw d;
    void draw(uint32_t first, uint32_t count, bool indexed, uint32_t at) {
        ++draws;
        if (depth == Depth::parse) return;
        if (!rr7::gather_draw(*s, {rr7::guest_memory(), io_address}, first, count, indexed, at, d)) return;
        if (depth == Depth::draw) rr7::renderer_draw(d);
    }
    void inline_word(uint32_t, uint32_t, uint32_t) { qilin::rt::fatal("no inline transfers in the bench"); }
    void method(uint32_t method, uint32_t argument, uint32_t) {
        switch (method) {
        case 0x0100:
        case 0x1714:
        case 0x1fd8: return;
        case rr7::kMethodFlip:
            if (depth == Depth::draw) rr7::renderer_flush("a flip");
            return;
        case 0x1d94: {
            if (depth != Depth::draw) return;
            const rr7::RsxState& st = *s;
            rr7::Clear c;
            c.x1 = kWidth, c.y1 = kHeight;
            c.color = (argument & 0xf0) != 0, c.color_value = st[0x1d90];
            c.color_surface = {rr7::kRsxLocal + st[0x0210], st[0x020c], st[0x0208] & 0x1f, kWidth, kHeight};
            c.depth = (argument & 1) != 0, c.stencil = (argument & 2) != 0, c.depth_value = st[0x1d8c];
            c.depth_surface = {rr7::kRsxLocal + st[0x0214], st[0x022c], 2, kWidth, kHeight};
            rr7::renderer_clear(c);
            return;
        }
        default: qilin::rt::fatal("the bench's stream has method %04x", method);
        }
    }
};

// Runs the commands in [from, to) of the mapped memory, as the RSX's thread does (no jumps in this stream).
void run_frame(rr7::RsxState& s, Acts& acts, uint32_t bytes) {
    const uint8_t* m = rr7::guest_memory();
    for (uint32_t at = 0; at < bytes;) {
        const uint32_t header = rr7::load_be32(m + kIo + at);
        const uint32_t count = (header >> 18) & 0x7ff, method = header & 0xfffc;
        rr7::run_command(s, method, count, (header & 0x40000000u) != 0, m + kIo + at + 4, at, acts);
        at += 4 + 4 * count;
    }
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t frames = 30;
    bool touch = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--lazy") g_lazy = true;
        else if (a == "--touch") g_lazy = touch = true;
        else return std::fprintf(stderr, "usage: ridgeracer7_rsx_bench [--frames N] [--lazy] [--touch]\n"), 2;
    }
    static qilin::rt::AddressSpace space;
    g_space = &space;
    if (!space.base() || !space.commit(kIo, kIoSize) || !space.commit(rr7::kRsxLocal, 32u << 20)) return std::fprintf(stderr, "no guest memory\n"), 1;
    fill_guest();
    const std::vector<uint32_t> words = frame_commands();
    for (size_t i = 0; i < words.size(); ++i) put_be32(space.base() + kIo + 4 * i, words[i]);
    const uint32_t bytes = static_cast<uint32_t>(words.size() * 4);

    stub_vulkan();
    static qilin::host::Gpu gpu;
    gpu.device = reinterpret_cast<VkDevice>(uintptr_t{0x100}), gpu.physical = reinterpret_cast<VkPhysicalDevice>(uintptr_t{0x200});
    gpu.name = "a stub (ridgeracer7_rsx_bench)";
    gpu.properties.limits.minUniformBufferOffsetAlignment = 256, gpu.properties.limits.optimalBufferCopyOffsetAlignment = 16;
    gpu.properties.limits.maxUniformBufferRange = 65536, gpu.properties.limits.timestampPeriod = 1.0f;
    vkGetPhysicalDeviceMemoryProperties(gpu.physical, &gpu.memory);
    gpu.fill_non_solid = gpu.logic_op = gpu.clip_distance = gpu.depth_bounds = true;
    static std::mutex queue;
    rr7::renderer_start(gpu, queue);

    static rr7::RsxState s;
    s.state[0x0198 / 4] = 0xFEED0000u, s.state[0x1454 / 4] = 1;
    // The vertex programs, uploaded once (as the game does at its start), at slots 0, 32, 64 and 96.
    {
        Acts upload;
        upload.s = &s;
        for (unsigned p = 0; p < kVertexPrograms; ++p) {
            rr7::run_method(s, 0x1e9c, p * 32, 0, upload);
            for (unsigned i = 0; i < kVertexProgramLength; ++i)
                for (const uint32_t w : vertex_instruction(p, i)) rr7::run_method(s, 0x0b80, w, 0, upload);
        }
    }
    std::printf("a frame: %u command words, %u draws; %u frames each, %s write-back%s\n", bytes / 4, kDraws, frames, g_lazy ? "lazy" : "eager",
                touch ? ", surfaces touched after each frame" : "");
    const auto time = [&](Depth depth, const char* what) {
        Acts acts;
        acts.s = &s, acts.depth = depth;
        const uint64_t made_before = g_pipelines_made;
        run_frame(s, acts, bytes);  // (a frame first, for the caches: textures decoded, pipelines made)
        const uint64_t made_first = g_pipelines_made - made_before;
        if (touch && depth == Depth::draw) rr7::renderer_guest_access(rr7::kRsxLocal + kColour, uint64_t{kPitch} * kHeight * 2);
        g_digesting = false;
        // Each frame's time: the best and the median are given (other work on the computer only ever makes a frame slower).
        std::vector<double> times;
        for (uint32_t i = 0; i < frames; ++i) {
            const Clock::time_point start = Clock::now();
            run_frame(s, acts, bytes);
            if (touch && depth == Depth::draw) rr7::renderer_guest_access(rr7::kRsxLocal + kColour, uint64_t{kPitch} * kHeight * 2);
            times.push_back(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()) / 1e3);
        }
        std::sort(times.begin(), times.end());
        std::printf("  %-48s best %8.1f us, median %8.1f us a frame (%.3f us a draw)\n", what, times.front(), times[times.size() / 2], times.front() / kDraws);
        if (depth == Depth::draw)
            std::printf("  pipelines made: %llu in the first frame drawn, %llu in the %u after\n", static_cast<unsigned long long>(made_first),
                        static_cast<unsigned long long>(g_pipelines_made - made_before - made_first), frames);
        g_digesting = true;
    };
    time(Depth::parse, "commands only (rsx_state.hpp: run_command)");
    time(Depth::gather, "commands and draws gathered (gather_draw)");
    time(Depth::draw, "commands, gathered and drawn (renderer, stubs)");
    // A surface's conversions to and from guest memory (render_formats.hpp), as a write-back or an upload makes them.
    {
        const size_t pixels = size_t{kWidth} * kHeight;
        std::vector<uint8_t> host(pixels * 5);
        std::vector<uint8_t> guest_bytes(pixels * 4, 0x5a);
        uint8_t* guest = guest_bytes.data();
        const auto best_of = [](auto&& work) {
            double best = 1e30;
            for (int i = 0; i < 10; ++i) {
                const Clock::time_point start = Clock::now();
                work();
                best = std::min(best, static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()) / 1e3);
            }
            return best;
        };
        const double to_host = best_of([&] { rr7::pixels_to_host(8, guest, host.data(), pixels); });
        const double to_guest = best_of([&] { rr7::pixels_to_guest(8, host.data(), guest, pixels); });
        const double depth = best_of([&] {
            rr7::depth_row_to_guest(reinterpret_cast<const uint32_t*>(host.data()), host.data() + pixels * 4, guest, pixels, false);
        });
        const double depth_in = best_of([&] { rr7::depth_row_to_host(guest, reinterpret_cast<uint32_t*>(host.data()), host.data() + pixels * 4, pixels, false); });
        std::printf("  a %ux%u surface: to the image %.1f us, back %.1f us; a depth surface to the image %.1f us, back %.1f us\n", kWidth, kHeight, to_host, to_guest, depth_in, depth);
    }
    // The digest of everything the stubs were given (in the first frame drawn and in one more), and of the surfaces as
    // guest memory holds them then.
    {
        Acts acts;
        acts.s = &s;
        run_frame(s, acts, bytes);
    }
    rr7::renderer_flush("the end");
    if (g_lazy) rr7::renderer_guest_access(rr7::kRsxLocal + kColour, uint64_t{kPitch} * kHeight * 2);
    digest(space.base() + rr7::kRsxLocal + kColour, size_t{kPitch} * kHeight);
    digest(space.base() + rr7::kRsxLocal + kDepth, size_t{kPitch} * kHeight);
    rr7::renderer_report();
    std::printf("%llu draws given to Vulkan; digest %016llx\n", static_cast<unsigned long long>(g_vk_draws), static_cast<unsigned long long>(g_digest));
    if (g_vk_draws != uint64_t{kDraws} * (frames + 2)) return std::fprintf(stderr, "expected %u draws a frame\n", kDraws), 1;
    return 0;
}

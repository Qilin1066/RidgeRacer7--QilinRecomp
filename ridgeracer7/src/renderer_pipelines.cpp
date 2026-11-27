#include "renderer_pipelines.hpp"

#include <algorithm>
#include <bit>
#include <vector>

#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;
namespace host = qilin::host;

}  // namespace

VkShaderModule PipelineCache::module_of(const qilin::rt::TranslatedShader& s, unsigned form) {
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

VkPipeline PipelineCache::pipeline(const Draw& d, const qilin::rt::TranslatedShader& vs, const qilin::rt::TranslatedShader& fs, const Forms& forms, VkFormat format,
                                   VkPrimitiveTopology topology, const Rasterisation& rasterising) {
    const Key key{static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&vs)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&vs) >> 32),
                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fs)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fs) >> 32),
                  forms.vertex | (forms.fragment << 8), d.clip_planes,
                  static_cast<uint32_t>(format), static_cast<uint32_t>(topology), d.color_mask, d.blend, d.blend_src, d.blend_dst, d.blend_equation,
                  d.depth_test, d.depth_write, d.depth_func, d.cull, d.cull_face, d.front_face, rasterising.depth_bias, d.depth_surface, d.stencil_test,
                  d.stencil_front.func, d.stencil_front.ref, d.stencil_front.func_mask, d.stencil_front.write_mask, d.stencil_front.fail, d.stencil_front.zfail,
                  d.stencil_front.zpass, d.stencil_back.func, d.stencil_back.ref, d.stencil_back.func_mask, d.stencil_back.write_mask, d.stencil_back.fail,
                  d.stencil_back.zfail, d.stencil_back.zpass,
                  std::bit_cast<uint32_t>(d.bias_factor), std::bit_cast<uint32_t>(d.bias_units),
                  d.depth_bounds, std::bit_cast<uint32_t>(d.bounds_min), std::bit_cast<uint32_t>(d.bounds_max), static_cast<uint32_t>(rasterising.mode), d.logic, d.logic ? d.logic_op : 0u};
    auto& p = pipelines_[key];
    if (p) return p;
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = module_of(vs, forms.vertex), stages[0].pName = "main";
    // The clip form's planes' tests, as its specialisation constants.
    std::array<VkSpecializationMapEntry, 6> clip_entries{};
    for (uint32_t n = 0; n < 6; ++n) clip_entries[n] = {n, n * 4, 4};
    const VkSpecializationInfo clip_info{6, clip_entries.data(), sizeof forms.clip, forms.clip.data()};
    if (forms.vertex == 1) stages[0].pSpecializationInfo = &clip_info;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = module_of(fs, forms.fragment), stages[1].pName = "main";
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
    raster.polygonMode = rasterising.mode == PolygonMode::line ? VK_POLYGON_MODE_LINE : rasterising.mode == PolygonMode::point ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
    if (rasterising.depth_bias) raster.depthBiasEnable = VK_TRUE, raster.depthBiasSlopeFactor = d.bias_factor, raster.depthBiasConstantFactor = d.bias_units;
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
    if (d.logic) {
        // In place of blending, in Vulkan as on the RSX. (Vulkan applies none to a float surface.)
        const std::optional<uint32_t> op = logic_op(d.logic_op);
        if (!op) fatal("a logic operation %#x, which the renderer doesn't know", d.logic_op);
        if (!gpu_->logic_op) fatal("a draw with a logic operation, which this GPU can't (logicOp)");
        if (format == VK_FORMAT_R32_SFLOAT) fatal("a logic operation on a float surface, which Vulkan doesn't apply");
        blending.logicOpEnable = VK_TRUE, blending.logicOp = static_cast<VkLogicOp>(*op);
    }
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
        if (!gpu_->depth_bounds) fatal("a draw with the depth bounds test, which this GPU doesn't have");
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
    if (d.depth_surface) rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat = depth_format_;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2, info.pStages = stages;
    info.pVertexInputState = &vertex, info.pInputAssemblyState = &assembly, info.pViewportState = &viewport;
    info.pRasterizationState = &raster, info.pMultisampleState = &multisample, info.pColorBlendState = &blending, info.pDynamicState = &dynamic;
    info.pDepthStencilState = &depth;
    info.layout = layout_;
    host::check(vkCreateGraphicsPipelines(gpu_->device, VK_NULL_HANDLE, 1, &info, nullptr, &p), "vkCreateGraphicsPipelines");
    return p;
}

void PipelineCache::start(const host::Gpu& gpu, VkPipelineLayout layout, VkFormat depth_format) {
    gpu_ = &gpu, layout_ = layout, depth_format_ = depth_format;
}

}  // namespace rr7

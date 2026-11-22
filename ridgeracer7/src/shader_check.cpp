// ridgeracer7_shader_check: every translated shader (qilin shaders --out)
// given to this machine's Vulkan driver: a shader module for each, and a
// graphics pipeline for each vertex program with a fragment program, and
// for each fragment program with a vertex program that writes what it
// reads. A driver that takes them is a check independent of the SPIR-V
// writer; it is not a proof (no driver checks everything).
//
// Built only with the lifted code (-DQILIN_GEN_DIR), as the shaders are
// derived from the game. Exit 0 if all were taken, 1 otherwise, 77 if
// there is no Vulkan 1.3 device.
#include <cstdio>
#include <vector>

#include "qilin/host/vulkan.hpp"
#include "qilin/rt/shaders.hpp"

using namespace qilin;

namespace {

VkShaderModule module_of(const host::Gpu& gpu, const rt::TranslatedShader& s) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = s.spirv_words * 4;
    info.pCode = s.spirv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(gpu.device, &info, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

}  // namespace

int main() {
    if (volkInitialize() != VK_SUCCESS || volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        std::puts("shader check: no Vulkan 1.3 here: skipped");
        return 77;
    }
    host::Gpu gpu = host::Gpu::create({}, false);
    std::printf("shader check on %s\n", gpu.name.c_str());

    // The layout every translated program fits: set 0 the two constant blocks, set 1 sixteen textures, 32 bytes of push constants.
    VkDescriptorSetLayoutBinding constants[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        constants[i].binding = i;
        constants[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        constants[i].descriptorCount = 1;
        constants[i].stageFlags = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutBinding textures[16] = {};
    for (uint32_t i = 0; i < 16; ++i) {
        textures[i].binding = i;
        textures[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        textures[i].descriptorCount = 1;
        textures[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayout sets[2];
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 2, set_info.pBindings = constants;
    host::check(vkCreateDescriptorSetLayout(gpu.device, &set_info, nullptr, &sets[0]), "vkCreateDescriptorSetLayout");
    set_info.bindingCount = 16, set_info.pBindings = textures;
    host::check(vkCreateDescriptorSetLayout(gpu.device, &set_info, nullptr, &sets[1]), "vkCreateDescriptorSetLayout");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 2, layout_info.pSetLayouts = sets, layout_info.pushConstantRangeCount = 1, layout_info.pPushConstantRanges = &push;
    VkPipelineLayout layout;
    host::check(vkCreatePipelineLayout(gpu.device, &layout_info, nullptr, &layout), "vkCreatePipelineLayout");

    std::vector<VkShaderModule> modules(rt::kTranslatedShaderCount);
    size_t bad_modules = 0;
    for (size_t i = 0; i < rt::kTranslatedShaderCount; ++i) {
        modules[i] = module_of(gpu, rt::kTranslatedShaders[i]);
        if (!modules[i]) {
            std::printf("  the driver refuses the module of the %s program at %08x\n", rt::kTranslatedShaders[i].vertex ? "vertex" : "fragment", rt::kTranslatedShaders[i].address);
            ++bad_modules;
        }
    }
    // A fragment program's inputs at their locations: COL0 1, COL1 2, FOGC 5, TEXn 7 + n (rsx_spirv.hpp).
    const auto fragment_needs = [](uint32_t inputs) {
        static constexpr unsigned kLocation[14] = {0, 1, 2, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        uint32_t outputs = 0;
        for (unsigned n = 1; n < 14; ++n)
            if (inputs & (1u << n)) outputs |= 1u << kLocation[n];
        return outputs;
    };
    const auto pipeline = [&](size_t v, size_t f) {
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = modules[v], stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = modules[f], stages[1].pName = "main";
        std::vector<VkVertexInputAttributeDescription> attributes;
        std::vector<VkVertexInputBindingDescription> bindings;
        for (uint32_t a = 0; a < 16; ++a)
            if (rt::kTranslatedShaders[v].inputs & (1u << a)) {
                bindings.push_back({a, 16, VK_VERTEX_INPUT_RATE_VERTEX});
                attributes.push_back({a, a, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
            }
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertex.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size()), vertex.pVertexBindingDescriptions = bindings.data();
        vertex.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size()), vertex.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1, viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend[4] = {};
        for (auto& b : blend) b.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blending.attachmentCount = 4, blending.pAttachments = blend;
        const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2, dynamic.pDynamicStates = dynamic_states;
        const VkFormat formats[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 4, rendering.pColorAttachmentFormats = formats;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2, info.pStages = stages;
        info.pVertexInputState = &vertex, info.pInputAssemblyState = &assembly, info.pViewportState = &viewport;
        info.pRasterizationState = &raster, info.pMultisampleState = &multisample, info.pColorBlendState = &blending, info.pDynamicState = &dynamic;
        info.layout = layout;
        VkPipeline p = VK_NULL_HANDLE;
        const VkResult r = vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &p);
        if (p) vkDestroyPipeline(gpu.device, p, nullptr);
        return r == VK_SUCCESS;
    };
    size_t pipelines = 0, bad_pipelines = 0;
    size_t first_fragment = rt::kTranslatedShaderCount;
    for (size_t i = 0; i < rt::kTranslatedShaderCount; ++i)
        if (!rt::kTranslatedShaders[i].vertex && modules[i] && rt::kTranslatedShaders[i].inputs == 0) first_fragment = std::min(first_fragment, i);
    for (size_t i = 0; i < rt::kTranslatedShaderCount; ++i) {
        if (!modules[i]) continue;
        const rt::TranslatedShader& s = rt::kTranslatedShaders[i];
        size_t partner = rt::kTranslatedShaderCount;
        if (s.vertex) {
            // A fragment program reading nothing it doesn't write; failing that, the first that reads nothing at all.
            for (size_t j = 0; j < rt::kTranslatedShaderCount && partner == rt::kTranslatedShaderCount; ++j)
                if (!rt::kTranslatedShaders[j].vertex && modules[j] && (fragment_needs(rt::kTranslatedShaders[j].inputs) & ~s.outputs) == 0) partner = j;
            if (partner == rt::kTranslatedShaderCount) partner = first_fragment;
        } else {
            for (size_t j = 0; j < rt::kTranslatedShaderCount && partner == rt::kTranslatedShaderCount; ++j)
                if (rt::kTranslatedShaders[j].vertex && modules[j] && (fragment_needs(s.inputs) & ~rt::kTranslatedShaders[j].outputs) == 0) partner = j;
        }
        if (partner == rt::kTranslatedShaderCount) {
            std::printf("  no partner for the %s program at %08x\n", s.vertex ? "vertex" : "fragment", s.address);
            continue;
        }
        ++pipelines;
        if (!(s.vertex ? pipeline(i, partner) : pipeline(partner, i))) {
            std::printf("  the driver refuses a pipeline with the %s program at %08x\n", s.vertex ? "vertex" : "fragment", s.address);
            ++bad_pipelines;
        }
    }
    std::printf("shader check: %zu modules, %zu refused; %zu pipelines, %zu refused\n", rt::kTranslatedShaderCount, bad_modules, pipelines, bad_pipelines);
    for (VkShaderModule m : modules)
        if (m) vkDestroyShaderModule(gpu.device, m, nullptr);
    vkDestroyPipelineLayout(gpu.device, layout, nullptr);
    vkDestroyDescriptorSetLayout(gpu.device, sets[0], nullptr);
    vkDestroyDescriptorSetLayout(gpu.device, sets[1], nullptr);
    gpu.destroy();
    return bad_modules || bad_pipelines ? 1 : 0;
}

// What the RSX's drawing state becomes among Vulkan's objects: the translated programs' shader modules, the graphics
// pipelines and the samplers, each made the first time a draw needs it and kept (renderer.cpp draws with them).
//
// Runtime code: no std::format.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "qilin/host/vulkan.hpp"
#include "qilin/rt/shaders.hpp"
#include "render_state.hpp"
#include "renderer.hpp"
#include "textures.hpp"

namespace rr7 {

// A hash of a key made of words, for the renderer's hash tables.
struct WordsHash {
    template <size_t N>
    size_t operator()(const std::array<uint32_t, N>& key) const noexcept {
        uint64_t h = 0x9e3779b97f4a7c15ull;
        for (const uint32_t w : key) h = (h ^ w) * 0xff51afd7ed558ccdull;
        return static_cast<size_t>(h ^ (h >> 32));
    }
};

// The forms of a draw's programs (shaders.hpp) its state asks for.
struct Forms {
    unsigned vertex = 0, fragment = 0;
    std::array<int32_t, 6> clip{};  // the clip form's: each plane's test (specialisation constants 0 to 5)
};

VkPrimitiveTopology vulkan_topology(Topology t);

class PipelineCache {
public:
    void start(const qilin::host::Gpu& gpu, VkPipelineLayout layout, VkFormat depth_format);
    // The pipeline for a draw's programs in their forms and every piece of its state that goes into one; what Vulkan or the
    // GPU can't do stops the game by name.
    VkPipeline pipeline(const Draw& d, const qilin::rt::TranslatedShader& vs, const qilin::rt::TranslatedShader& fs, const Forms& forms, VkFormat format,
                        VkPrimitiveTopology topology, const Rasterisation& rasterising);
    // A texture unit's sampler, made once for each set of fields.
    VkSampler sampler(uint32_t unit, const SamplerFields& s, uint32_t levels);

private:
    // A program's module in one of its forms (shaders.hpp: 0 the plain one), made the first time it is drawn with.
    VkShaderModule module_of(const qilin::rt::TranslatedShader& s, unsigned form);

    // A pipeline's key: the programs and every piece of state that goes into it, as words.
    using Key = std::array<uint32_t, 44>;
    const qilin::host::Gpu* gpu_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    std::unordered_map<const qilin::rt::TranslatedShader*, std::array<VkShaderModule, 4>> modules_;  // by program, then its form
    std::unordered_map<Key, VkPipeline, WordsHash> pipelines_;
    Key last_key_{};                              // the last draw's key,
    VkPipeline last_pipeline_ = VK_NULL_HANDLE;   // and its pipeline
    std::unordered_map<std::array<uint32_t, 12>, VkSampler, WordsHash> samplers_;  // by the fields they're made from
};

}  // namespace rr7

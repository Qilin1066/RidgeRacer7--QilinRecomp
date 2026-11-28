// What the RSX's drawing state becomes among Vulkan's objects: the translated programs' shader modules, the graphics
// pipelines and the samplers (renderer.cpp draws with them).
//
// A pipeline fixes only what Vulkan 1.3 can't set per draw: the programs in their forms, the vertex formats, the blending,
// the colour write mask, the logic operation, the polygon mode, the topology's class and the attachments' formats. The rest
// of the depth, stencil and rasterisation state is dynamic (extended dynamic state 1 and 2, core in 1.3, which Mesa's v3dv
// has: docs/efficiency-research.md), set per draw where it changes (DynamicState). So a frame needs a few pipelines, not one
// for each mix of depth function, culling and stencil.
//
// Pipelines are made ahead where they can be: each one made is recorded by its key (words that mean the same in every
// run), and at the next start a worker thread makes them all again before the game draws, through a VkPipelineCache kept
// on disk beside the keys (both in the data folder, the cache checked against the device and driver that wrote it). A
// pipeline the worker hasn't reached yet when a draw needs it is waited for, not made twice.
//
// Runtime code: no std::format.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "qilin/host/vulkan.hpp"
#include "qilin/rt/shaders.hpp"
#include "render_formats.hpp"
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

// How a pipeline reads an attribute given to the GPU as `f` (render_formats.hpp: kVertexFloats, kVertexConstant or its own
// format): the Vulkan format, the bytes a vertex, and whether one value serves every vertex (a binding read per instance,
// and one instance drawn).
struct VertexInput {
    VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    uint32_t stride = 16;
    bool per_instance = false;
};
VertexInput vertex_input(uint8_t f);

// A pipeline's key: everything it fixes, as words that mean the same in every run (the programs by their keys, not their
// places in memory), so that the keys a run made can be written down and made again by the next.
//   0-1 the vertex program's key, 2-3 the fragment program's
//   4   the vertex form | the fragment form << 8 | the topology's class << 16 (0 points, 1 lines, 2 triangles) | the polygon
//       mode << 20 | a depth-stencil attachment << 24 | blending << 25 | a logic operation << 26
//   5   the clip form's plane tests (as SET_USER_CLIP_PLANE_CONTROL: 4 bits a plane), 0 in other forms
//   6   the colour attachment's VkFormat
//   7   Vulkan's colour write mask
//   8-10 blending's source and destination factors and equation (the RSX's words), 0 without blending
//   11  the logic operation (a VkLogicOp), 0 without one
//   12-15 each attribute's vertex format (render_formats.hpp: vertex_format), a byte each, 0 for one not read
using PipelineKey = std::array<uint32_t, 16>;
// The words a pipeline key's layout is in: bumped whenever it changes, so that keys written by another version are not read.
inline constexpr uint32_t kPipelineKeyVersion = 1;

// The state a draw sets per draw rather than fixes in its pipeline, as Vulkan takes it.
struct DynamicState {
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkBool32 depth_test = VK_FALSE, depth_write = VK_FALSE;
    VkCompareOp depth_compare = VK_COMPARE_OP_NEVER;
    VkBool32 bounds_test = VK_FALSE;
    float bounds_min = 0, bounds_max = 0;
    VkBool32 stencil_test = VK_FALSE;
    VkStencilOpState stencil_front{}, stencil_back{};
    VkBool32 bias = VK_FALSE;
    float bias_constant = 0, bias_slope = 0;
};
// A draw's dynamic state; what Vulkan or the GPU can't do stops the game by name.
DynamicState dynamic_state(const Draw& d, VkPrimitiveTopology topology, const Rasterisation& rasterising, bool gpu_depth_bounds);
// Sets on the command buffer what of `want` differs from `have` (everything when `have` is null: a command buffer begun).
void set_dynamic_state(VkCommandBuffer cmd, const DynamicState& want, const DynamicState* have, bool bounds_dynamic);

class PipelineCache {
public:
    // `folder`: where the pipeline cache and the keys are kept (empty: nowhere, nothing made ahead). The keys found there
    // are made on a worker thread from now.
    void start(const qilin::host::Gpu& gpu, VkPipelineLayout layout, VkFormat depth_format, const std::filesystem::path& folder);
    // The key of a draw's pipeline: its programs in their forms and every piece of its state that goes into one; what Vulkan
    // or the GPU can't do stops the game by name.
    PipelineKey key(const Draw& d, const qilin::rt::TranslatedShader& vs, const qilin::rt::TranslatedShader& fs, const Forms& forms, VkFormat format,
                    VkPrimitiveTopology topology, const Rasterisation& rasterising, const std::array<uint8_t, 16>& vertex_formats) const;
    // Its pipeline: made now if neither an earlier draw nor the worker has made it (or waited for, if the worker is making it).
    VkPipeline pipeline(const PipelineKey& key);
    // A texture unit's sampler, made once for each set of fields.
    VkSampler sampler(uint32_t unit, const SamplerFields& s, uint32_t levels);
    // How an attribute of this format word (SET_VERTEX_DATA_ARRAY_FORMAT) is given to the GPU (render_formats.hpp): in its own
    // format where the GPU reads it (checked at the start: one it can't is converted to floats, and said so once).
    uint8_t vertex_format(uint32_t format_word) const;
    // Whether the GPU reads vertices given as `f`.
    bool vertex_format_usable(uint8_t f) const { return f == kVertexFloats || f == kVertexConstant || (vertex_format_native(f) && native_[f]); }
    // Whether the pipeline state's depth bounds are dynamic (only where the GPU has the test).
    bool bounds_dynamic() const { return gpu_ && gpu_->depth_bounds; }
    // The pipeline cache and the keys to disk, if pipelines were made since they were last written (on the worker thread:
    // `now` false; or before returning). Called by the renderer when the game has made none for a while.
    void save(bool now);
    // Waits until the worker has made every pipeline it found at the start (a development aid, for the bench).
    void wait_ahead();
    struct Counts {
        uint64_t ahead = 0;    // pipelines made by the worker from the keys of an earlier run
        uint64_t drawing = 0;  // made when a draw needed them
        uint64_t waited = 0;   // draws that waited for the worker
    };
    Counts counts() const { return {made_ahead_.load(), made_drawing_.load(), waited_.load()}; }
    // When the last pipeline was made for a draw, as a count of saves asked for (renderer.cpp asks at quiet times).
    uint64_t made_since_save() const { return made_since_save_.load(); }

private:
    // A program's module in one of its forms (shaders.hpp: 0 the plain one), made the first time it is drawn with.
    VkShaderModule module_of(const qilin::rt::TranslatedShader& s, unsigned form);
    // The pipeline a key describes (on whichever thread asks), or null for one this device can't make (a recorded key).
    VkPipeline make(const PipelineKey& key, bool recorded);
    void worker();
    void write_files();

    const qilin::host::Gpu* gpu_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkPipelineCache vk_cache_ = VK_NULL_HANDLE;
    std::filesystem::path folder_;
    std::unordered_map<uint64_t, const qilin::rt::TranslatedShader*> shaders_;  // by key

    std::mutex modules_mutex_;
    std::unordered_map<const qilin::rt::TranslatedShader*, std::array<VkShaderModule, 4>> modules_;  // by program, then its form

    // The pipelines made, by key; a null one is being made (by the worker or a draw), and is waited for.
    std::mutex mutex_;
    std::condition_variable made_;
    std::unordered_map<PipelineKey, VkPipeline, WordsHash> pipelines_;
    PipelineKey last_key_{};                     // the last draw's key (the RSX's thread alone),
    VkPipeline last_pipeline_ = VK_NULL_HANDLE;  // and its pipeline

    // The worker: the recorded keys to make, then saves when asked.
    std::vector<PipelineKey> ahead_;
    bool ahead_done_ = false, save_asked_ = false;
    std::condition_variable work_;
    std::thread thread_;
    std::mutex files_mutex_;  // (one writer of the files at a time)
    std::atomic<uint64_t> made_ahead_{0}, made_drawing_{0}, waited_{0}, made_since_save_{0};

    std::array<bool, 256> native_{};  // the attributes' own formats the GPU reads, by vertex_format's byte
    std::unordered_map<std::array<uint32_t, 12>, VkSampler, WordsHash> samplers_;  // by the fields they're made from
};

}  // namespace rr7

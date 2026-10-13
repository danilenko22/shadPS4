// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "vulkan/vulkan.hpp"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
    bool is_indirect;
};

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {});

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule, u64>;
    Result GetProgram(Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool RefreshGraphicsKey();
    bool RefreshGraphicsStages();
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(Shader::HwStage stage, Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    DrawIndirectParams draw_indirect_params{};
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    Shader::Gcn::FetchShaderData* fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // golf-fix: graphics pipelines deserialized during warm-up, built later in parallel
    struct PendingGraphicsPipeline {
        GraphicsPipelineKey key{};
        std::array<const Shader::Info*, MaxShaderStages> infos{};
        std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
        std::optional<Shader::Gcn::FetchShaderData> fetch{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        GraphicsPipeline::SerializationSupport sdata{};
        size_t spv_bytes{};
        std::array<std::optional<Shader::Info>, MaxShaderStages> info_copies{};
        std::array<const Shader::Info*, MaxShaderStages> copy_ptrs{};
    };
    std::vector<PendingGraphicsPipeline>* pending_graphics{};

    // golf-fix: cached pipelines are built in the background after the game has started; a
    // pipeline the game needs first is built right away on the GPU thread.
    struct GolfPreload {
        std::vector<PendingGraphicsPipeline> pending;
        std::vector<std::unique_ptr<GraphicsPipeline>> built;
        std::unique_ptr<std::atomic<u8>[]> state; // 0 waiting, 1 building, 2 ready
        tsl::robin_map<GraphicsPipelineKey, size_t> index;
        std::atomic<size_t> next{0};
        std::atomic<size_t> finished{0};
        std::atomic<bool> stop{false};
        std::vector<std::thread> workers;
        std::chrono::steady_clock::time_point start{};
    };
    std::unique_ptr<GolfPreload> golf_preload;
    void GolfBuildPreloaded(size_t i);
    size_t golf_spv_bytes{}; // SPIR-V size of the stages loaded for the current pipeline

    // golf-fix: new graphics pipelines are built on background threads; the draw is skipped
    // until the pipeline is ready instead of stalling the GPU thread.
    struct AsyncGraphicsJob {
        GraphicsPipelineKey key{};
        std::array<std::optional<Shader::Info>, MaxShaderStages> info_copies{};
        std::array<const Shader::Info*, MaxShaderStages> copy_ptrs{};
        std::array<const Shader::Info*, MaxShaderStages> live_infos{};
        std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
        std::optional<Shader::Gcn::FetchShaderData> fetch{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        GraphicsPipeline::SerializationSupport sdata{};
        std::unique_ptr<GraphicsPipeline> result{};
        std::atomic<bool> done{false};
    };
    void AsyncWorker();
    tsl::robin_map<GraphicsPipelineKey, std::shared_ptr<AsyncGraphicsJob>> async_jobs;
    std::mutex async_mutex;
    std::condition_variable async_cv;
    std::deque<std::shared_ptr<AsyncGraphicsJob>> async_queue;
    bool async_stop{};
    std::vector<std::thread> async_workers;

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

} // namespace Vulkan

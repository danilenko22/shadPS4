// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <tsl/robin_set.h>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <semaphore>
#include <thread>
#include <vector>
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 12u; // golf-fix v22: LOD 0 for single-level samples
static constexpr u32 ShaderMetaVersion = 6u;
static constexpr u32 PipelineKeyVersion = 3u;
} // namespace Serialization

namespace Core {
void GolfSetSehGuard(bool on);
}

namespace Vulkan {

// golf-fix: run a driver call so that an access violation inside it skips that pipeline instead
// of closing the emulator.
[[maybe_unused]] static bool GolfGuardedCall(void (*fn)(void*), void* ctx) {
    // An SEH guard here left a driver lock held after a crash and hung the game, so the build
    // is called directly.
    fn(ctx);
    return true;
}

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (!is_new) {
        // golf-fix: the archive can hold the same key twice after a cache version bump
        infos.fill(nullptr);
        modules.fill(nullptr);
        return true;
    }

    it.value() =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, vk::PipelineCache{},
                                          compute_key, *infos[0], modules[0], sdata, true);

    infos.fill(nullptr);
    modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    golf_spv_bytes = 0;
    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;
    graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    if (pending_graphics) {
        auto& pending = pending_graphics->emplace_back();
        pending.key = graphics_key;
        pending.infos = infos;
        pending.runtime_infos = runtime_infos;
        if (fetch_shader) {
            pending.fetch = *fetch_shader;
        }
        pending.modules = modules;
        pending.sdata = std::move(sdata);
        pending.spv_bytes = golf_spv_bytes;
        infos.fill(nullptr);
        modules.fill(nullptr);
        fetch_shader = nullptr;
        return true;
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    if (!is_new) {
        infos.fill(nullptr);
        modules.fill(nullptr);
        fetch_shader = nullptr;
        return true;
    }

    it.value() = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, graphics_key, vk::PipelineCache{}, infos,
        runtime_infos, fetch_shader, modules, sdata, true);

    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.empty()) {
        return false;
    }
    golf_spv_bytes += spv.size() * sizeof(u32);

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = CompileSPV(spv, instance.GetDevice());
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = CompileSPV(spv, instance.GetDevice());
        }
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    infos[stage] = &it_pgm.value()->info;
    modules[stage] = module;
    if (auto& fetch = it_pgm.value()->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }

    return true;
}

void PipelineCache::GolfBuildPreloaded(size_t i) {
    auto& pre = *golf_preload;
    auto& p = pre.pending[i];
    auto pipeline = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, p.key, vk::PipelineCache{}, p.copy_ptrs,
        p.runtime_infos, p.fetch ? &*p.fetch : nullptr, p.modules, p.sdata, true);
    pipeline->GolfFixRebindStages(p.infos);
    pre.built[i] = std::move(pipeline);
    pre.state[i].store(2, std::memory_order_release);
    const size_t done = pre.finished.fetch_add(1) + 1;
    if (done == pre.pending.size()) {
        LOG_ERROR(Render, "golf-fix: background build of {} cached pipelines finished in {} ms",
                  done,
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - pre.start)
                      .count());
    }
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        LOG_ERROR(Render, "golf-fix warmup: pipeline cache is empty, shaders will be compiled");
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        LOG_ERROR(Render,
                    "Pipeline cache profile has unexpected size ({} != {}). Ignoring the cache",
                    profile_data.size(), sizeof(Shader::Profile));
        Storage::DataBase::Instance().Close();
        return;
    }

    Shader::Profile cached_profile{};
    std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    if (cached_profile != profile) {
        LOG_ERROR(Render,
                  "Pipeline cache isn't compatible with current system. Ignoring the cache");
        Storage::DataBase::Instance().Close();
        return;
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    // golf-fix: bound the blocking warm-up so the game always starts; the rest compile on demand
    const auto warmup_start = std::chrono::steady_clock::now();
    constexpr auto warmup_budget = std::chrono::seconds(60);
    bool warmup_budget_hit = false;
    std::vector<PendingGraphicsPipeline> pending;
    pending_graphics = &pending;

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;
            if (warmup_budget_hit) {
                return;
            }
            const auto elapsed = std::chrono::steady_clock::now() - warmup_start;
            if (elapsed > warmup_budget) {
                warmup_budget_hit = true;
                LOG_ERROR(Render, "golf-fix warmup: time budget reached after {} pipelines",
                          num_pipelines);
                return;
            }
            if (num_total_pipelines % 100 == 0) {
                LOG_ERROR(Render, "golf-fix warmup: {} pipelines loaded, {} s", num_pipelines,
                          std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
            }

            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);

            bool result{};
            if (is_compute) {
                result = LoadComputePipeline(ar);
            } else {
                result = LoadGraphicsPipeline(ar);
            }

            if (result) {
                ++num_pipelines;
            }
        });

    pending_graphics = nullptr;

    // golf-fix: build the deferred graphics pipelines on all cores. Pipeline creation only reads
    // the stage infos and creates Vulkan objects, which the driver allows from many threads.
    {
        const auto build_start = std::chrono::steady_clock::now();
        {
            // The store can hold one pipeline key several times. Building the same pipeline on
            // two threads at once crashes the AMD driver, so keep only the first copy.
            tsl::robin_set<GraphicsPipelineKey> seen;
            std::vector<PendingGraphicsPipeline> unique;
            unique.reserve(pending.size());
            for (auto& p : pending) {
                if (seen.insert(p.key).second) {
                    unique.push_back(std::move(p));
                }
            }
            if (unique.size() != pending.size()) {
                LOG_ERROR(Render, "golf-fix warmup: skipped {} duplicate graphics pipelines",
                          pending.size() - unique.size());
            }
            pending = std::move(unique);
        }
        auto pre = std::make_unique<GolfPreload>();
        pre->pending = std::move(pending);
        const size_t n = pre->pending.size();
        pre->built.resize(n);
        pre->state = std::make_unique<std::atomic<u8>[]>(n);
        for (size_t i = 0; i < n; ++i) {
            auto& p = pre->pending[i];
            for (size_t s = 0; s < MaxShaderStages; ++s) {
                if (p.infos[s]) {
                    // The GPU thread keeps updating the live infos; build from a copy.
                    p.info_copies[s].emplace(*p.infos[s]);
                    p.copy_ptrs[s] = &*p.info_copies[s];
                }
            }
            pre->index.emplace(p.key, i);
        }
        pre->start = std::chrono::steady_clock::now();
        golf_preload = std::move(pre);
        const u32 hw = std::thread::hardware_concurrency();
        const u32 num_threads = std::max(1u, hw > 2 ? hw - 2 : 1u);
        for (u32 t = 0; t < num_threads; ++t) {
            golf_preload->workers.emplace_back([this, n] {
                auto& pre = *golf_preload;
                while (!pre.stop.load(std::memory_order_relaxed)) {
                    const size_t i = pre.next.fetch_add(1);
                    if (i >= n) {
                        break;
                    }
                    u8 expected = 0;
                    if (pre.state[i].compare_exchange_strong(expected, 1)) {
                        GolfBuildPreloaded(i);
                    }
                }
            });
        }
        LOG_ERROR(Render,
                  "golf-fix warmup: {} graphics pipelines will be built in the background on {} "
                  "threads (dedupe/load took {} ms)",
                  n, num_threads,
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - build_start)
                      .count());
    }

    LOG_ERROR(Render, "golf-fix warmup: preloaded {} of {} pipelines", num_pipelines,
              num_total_pipelines);
    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (!fetch_shader_data.Empty()) {
        spec.Write(sizeof(fetch_shader_data));
        fetch_shader_data.Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader

// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/hash.h"
#include "common/path_util.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include "common/session_diagnostics.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/vr/htl_options.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "common/alignment.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/readconst_snapshot_diag.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"

#include <array>
#include <bit>

#include <vk_mem_alloc.h>
#include <xxhash.h>
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Vulkan {

VAddr TracedAddress();

// Recent GPU writes to guest memory (shader buffer writes, DMA copies), to name who wrote a
// region a traced draw then reads. A fixed ring, overwritten oldest first.
namespace {
struct GpuWrite {
    VAddr addr;
    u64 size;
    u64 writer;
    u64 serial;
};
std::mutex g_gpu_writes_mutex;
std::array<GpuWrite, 8192> g_gpu_writes{};
u64 g_gpu_writes_next{};
void RecordGpuWrite(VAddr addr, u64 size, u64 writer) {
    std::scoped_lock lock{g_gpu_writes_mutex};
    const u64 serial = g_gpu_writes_next++;
    g_gpu_writes[serial % g_gpu_writes.size()] = {addr, size, writer, serial};
}
std::string LastGpuWriter(VAddr addr, u64 size) {
    std::scoped_lock lock{g_gpu_writes_mutex};
    const u64 count = std::min<u64>(g_gpu_writes_next, g_gpu_writes.size());
    for (u64 n = 1; n <= count; ++n) {
        const auto& w = g_gpu_writes[(g_gpu_writes_next - n) % g_gpu_writes.size()];
        if (w.addr < addr + size && addr < w.addr + w.size) {
            return fmt::format(" last-writer={:#x}@{:#x}+{:#x} age={}", w.writer, w.addr, w.size,
                               g_gpu_writes_next - w.serial);
        }
    }
    return " last-writer=none";
}
} // namespace

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, page_manager{this},
      buffer_cache{instance, scheduler, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool} {
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);
}

Rasterizer::~Rasterizer() = default;

static std::atomic<s32> g_draw_trace_budget{0};

void Rasterizer::StartDrawTrace(s32 count) {
    g_draw_trace_budget.store(count);
}

static bool TraceDraw() {
    if (g_draw_trace_budget.load(std::memory_order_relaxed) <= 0) {
        return false;
    }
    return g_draw_trace_budget.fetch_sub(1) > 0;
}

static bool TraceDetails() {
    static const bool light = std::getenv("SHADPS4_DIAGNOSTICS_LIGHT_TRACE") != nullptr;
    return !light && g_draw_trace_budget.load(std::memory_order_relaxed) > 0;
}

void Rasterizer::CpSync() {
    scheduler.EndRendering();
    auto cmdbuf = scheduler.CommandBuffer();

    const vk::MemoryBarrier ib_barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eDrawIndirect,
                           vk::DependencyFlagBits::eByRegion, ib_barrier, {}, {});
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        if (TraceDraw()) {
            const auto& col_buf = regs.color_buffers[0];
            LOG_INFO(Render_Vulkan, "TRACE fce cb0={:#x} fast_clear={} meta_cleared={}",
                     col_buf.Address(), static_cast<bool>(col_buf.info.fast_clear),
                     texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start));
        }
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        if (TraceDraw()) {
            LOG_INFO(Render_Vulkan, "TRACE fmask-decompress cb0={:#x}",
                     regs.color_buffers[0].Address());
        }
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        if (TraceDraw()) {
            LOG_INFO(Render_Vulkan, "TRACE resolve cb0={:#x} -> cb1={:#x}",
                     regs.color_buffers[0].Address(), regs.color_buffers[1].Address());
        }
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

namespace {

struct DepthTargetState {
    vk::ImageLayout layout;
    vk::AccessFlags2 access;
};

/// The layout a depth buffer is attached in for a draw, and what the draw does with it there.
///
/// The layouts that allow writing serve draws with and without depth writes alike. Titles
/// switch depth writes on and off from one draw to the next (a decal, then an opaque object,
/// then a particle): giving the two kinds different layouts ended the render pass at every
/// switch, a hundred times a frame. Only a draw that also reads the buffer as a texture needs
/// a layout that forbids writing depth.
DepthTargetState GetDepthTargetState(const VideoCore::Image& image, const AmdGpu::Regs& regs) {
    const bool has_stencil = image.info.props.has_stencil;
    const bool writes_depth = regs.depth_control.depth_write_enable;
    const bool sampled = image.binding.is_bound;
    if (writes_depth || !sampled) {
        return {has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                            : vk::ImageLayout::eDepthAttachmentOptimal,
                vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                    vk::AccessFlagBits2::eDepthStencilAttachmentRead};
    }
    // Stencil writes can be enabled while depth writes are off.
    if (has_stencil && regs.depth_control.stencil_enable) {
        return {vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal,
                vk::AccessFlagBits2::eShaderRead |
                    vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite};
    }
    return {has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                        : vk::ImageLayout::eDepthReadOnlyOptimal,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eDepthStencilAttachmentRead};
}

} // namespace

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->regs;
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->last_cb_extent[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
        if (TraceDetails()) {
            LOG_INFO(Render_Vulkan,
                     "TRACE   cb{} image #{} type {} {}x{}x{} layers {} {} at {:#x} size {:#x} "
                     "(hint {}x{}, slices {}, tile {})",
                     cb, image_id.index, static_cast<u32>(image.info.type), image.info.size.width,
                     image.info.size.height, image.info.size.depth, image.info.resources.layers,
                     vk::to_string(image.info.pixel_format), image.info.guest_address,
                     image.info.guest_size, hint.width, hint.height, col_buf.NumSlices(),
                     static_cast<u32>(col_buf.GetTileMode()));
        }
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->last_db_extent;
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->last_cb_extent[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    static const bool skip_reflection_clear_enabled =
        std::getenv("SHADPS4_DBG_SKIP_REFLECTION_CLEAR") != nullptr;
    if (skip_reflection_clear_enabled && image.info.size.width == 128 &&
        image.info.resources.layers >= 6) {
        return;
    }
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    image.Clear(clear_value, desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    if (scheduler.IsFlushDue()) {
        Flush();
    }

    if (!FilterDraw()) {
        return;
    }
    const auto& regs = liverpool->regs;
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (pipeline != nullptr && !in_target_passes) {
        if (const auto passes = SharedTargetPasses(pipeline->GetGraphicsKey().mrt_mask);
            passes.size() > 1) {
            // One surface as two of the draw's targets: a pass for each.
            in_target_passes = true;
            for (const u8 left_out : passes) {
                pipeline_cache.LeaveTargetsOut(left_out);
                Draw(is_indexed, index_offset);
            }
            pipeline_cache.LeaveTargetsOut(0);
            in_target_passes = false;
            return;
        }
    }
    // SHADPS4_DBG_SKIP_PS=<hash>[,<hash>...]: draws whose pixel shader is one of these are left
    // out, to see in the picture what they draw.
    static const std::vector<u64> skip_ps = [] {
        std::vector<u64> hashes;
        if (const char* value = std::getenv("SHADPS4_DBG_SKIP_PS"); value != nullptr) {
            for (const char* at = value; *at != '\0';) {
                char* end = nullptr;
                hashes.push_back(std::strtoull(at, &end, 16));
                at = (*end == ',') ? end + 1 : end;
                if (end == at && *end != '\0') {
                    break;
                }
            }
        }
        return hashes;
    }();
    // SHADPS4_LIVE_SKIP=1: <user>/skip_ps_live.txt (hex hashes, one per line) is read again
    // twice a second, and draws whose pixel shader is listed are left out. Touching
    // <user>/dump_seen_ps writes every pixel shader drawn since to <user>/seen_ps.txt. Lets one
    // run be bisected for the draw that puts something in the picture.
    static const bool live_skip = [] {
        const char* value = std::getenv("SHADPS4_LIVE_SKIP");
        return value != nullptr && value[0] == '1';
    }();
    if (live_skip && pipeline != nullptr &&
        pipeline->GetStages()[static_cast<u32>(Shader::LogicalStage::Fragment)] != nullptr) {
        static std::mutex live_mutex;
        static std::unordered_set<u64> live_hashes;
        static std::unordered_set<u64> seen_hashes;
        static auto next_poll = std::chrono::steady_clock::now();
        static std::filesystem::file_time_type list_time{};
        const u64 ps = pipeline->GetStage(Shader::LogicalStage::Fragment).pgm_hash;
        std::scoped_lock lock{live_mutex};
        seen_hashes.insert(ps);
        if (const auto now = std::chrono::steady_clock::now(); now >= next_poll) {
            next_poll = now + std::chrono::milliseconds(500);
            const auto user = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
            std::error_code error;
            const auto list = user / "skip_ps_live.txt";
            const auto time = std::filesystem::last_write_time(list, error);
            if (error) {
                live_hashes.clear();
            } else if (time != list_time) {
                list_time = time;
                live_hashes.clear();
                std::ifstream in{list};
                for (std::string line; std::getline(in, line);) {
                    if (!line.empty()) live_hashes.insert(std::strtoull(line.c_str(), nullptr, 16));
                }
                LOG_WARNING(Render_Vulkan, "Live skip: {} pixel shaders left out", live_hashes.size());
            }
            if (std::filesystem::remove(user / "dump_seen_ps", error)) {
                std::ofstream out{user / "seen_ps.txt.tmp"};
                for (const u64 hash : seen_hashes) out << fmt::format("{:016x}", hash) << '\n';
                out.close();
                std::filesystem::rename(user / "seen_ps.txt.tmp", user / "seen_ps.txt", error);
            }
        }
        if (live_hashes.contains(ps)) {
            return;
        }
    }
    if (!skip_ps.empty() && pipeline != nullptr &&
        pipeline->GetStages()[static_cast<u32>(Shader::LogicalStage::Fragment)] != nullptr &&
        std::ranges::find(skip_ps,
                          pipeline->GetStage(Shader::LogicalStage::Fragment).pgm_hash) !=
            skip_ps.end()) {
        return;
    }
    const bool trace = TraceDraw();
    if (trace) {
        static const GraphicsPipelineKey empty_key{};
        const auto& key = pipeline ? pipeline->GetGraphicsKey() : empty_key;
        const auto& vp = regs.viewports[0];
        const auto& vp1 = regs.viewports[1];
        LOG_INFO(Render_Vulkan,
                 "TRACE stereo vp1=({:.1f},{:.1f})+({:.1f},{:.1f}) clip_disabled={} "
                 "viewport_scissor={} sc0=({},{})-({},{}) sc1=({},{})-({},{}) gs={:#x}",
                 vp1.xscale, vp1.yscale, vp1.xoffset, vp1.yoffset, regs.IsClipDisabled(),
                 static_cast<bool>(regs.mode_control.vport_scissor_enable),
                 static_cast<u32>(regs.viewport_scissors[0].top_left_x),
                 static_cast<u32>(regs.viewport_scissors[0].top_left_y),
                 static_cast<u32>(regs.viewport_scissors[0].bottom_right_x),
                 static_cast<u32>(regs.viewport_scissors[0].bottom_right_y),
                 static_cast<u32>(regs.viewport_scissors[1].top_left_x),
                 static_cast<u32>(regs.viewport_scissors[1].top_left_y),
                 static_cast<u32>(regs.viewport_scissors[1].bottom_right_x),
                 static_cast<u32>(regs.viewport_scissors[1].bottom_right_y),
                 key.stage_hashes[static_cast<u32>(Shader::LogicalStage::Geometry)]);
        LOG_INFO(Render_Vulkan,
                 "TRACE draw idx={} n={} inst={} prim={} pipeline={} cb0={:#x} {}x{} s{} "
                 "cb_mode={} mask={:#x} db={:#x} z_en={} z_wr={} zfunc={} samples={}/{} "
                 "vp=({:.1f},{:.1f},{:.1f})+({:.1f},{:.1f},{:.1f}) vs={:#x} ps={:#x} st_en={} "
                 "st_func={} st_ref={} st_mask={:#x} st_wmask={:#x} zclear={} sclear={} "
                 "depth_clear={} htile={:#x} htile_cleared={} zfmt={} sfmt={} s_clear={} "
                 "s_ops(fail,zpass,zfail)={},{},{} bf={} drc={:#x} dsc={:#x} s_opval={}",
                 is_indexed, regs.num_indices, regs.num_instances.NumInstances(),
                 static_cast<u32>(regs.primitive_type), pipeline != nullptr,
                 regs.color_buffers[0].Address(), regs.color_buffers[0].Pitch(),
                 regs.color_buffers[0].Height(), regs.color_buffers[0].NumSamples(),
                 static_cast<u32>(regs.color_control.mode), regs.color_target_mask.raw,
                 regs.depth_buffer.DepthAddress(),
                 static_cast<bool>(regs.depth_control.depth_enable),
                 static_cast<bool>(regs.depth_control.depth_write_enable),
                 static_cast<u32>(regs.depth_control.depth_func), key.num_samples,
                 key.depth_samples, vp.xscale, vp.yscale, vp.zscale, vp.xoffset, vp.yoffset,
                 vp.zoffset, key.stage_hashes[static_cast<u32>(Shader::LogicalStage::Vertex)],
                 key.stage_hashes[static_cast<u32>(Shader::LogicalStage::Fragment)],
                 static_cast<bool>(regs.depth_control.stencil_enable),
                 static_cast<u32>(regs.depth_control.stencil_ref_func),
                 static_cast<u32>(regs.stencil_ref_front.stencil_test_val),
                 static_cast<u32>(regs.stencil_ref_front.stencil_mask),
                 static_cast<u32>(regs.stencil_ref_front.stencil_write_mask),
                 static_cast<bool>(regs.depth_render_control.depth_clear_enable),
                 static_cast<bool>(regs.depth_render_control.stencil_clear_enable),
                 regs.depth_clear, regs.depth_htile_data_base.GetAddress(),
                 texture_cache.IsMetaCleared(regs.depth_htile_data_base.GetAddress(),
                                             regs.depth_view.slice_start),
                 static_cast<u32>(regs.depth_buffer.z_info.format),
                 static_cast<u32>(regs.depth_buffer.stencil_info.format), regs.stencil_clear,
                 static_cast<u32>(regs.stencil_control.stencil_fail_front),
                 static_cast<u32>(regs.stencil_control.stencil_zpass_front),
                 static_cast<u32>(regs.stencil_control.stencil_zfail_front),
                 static_cast<bool>(regs.depth_control.backface_enable),
                 std::bit_cast<u32>(regs.depth_render_control),
                 std::bit_cast<u32>(regs.depth_shader_control),
                 static_cast<u32>(regs.stencil_ref_front.stencil_op_val));
    }
    if (!pipeline) {
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        if (trace) {
            LOG_INFO(Render_Vulkan, "TRACE   draw dropped: resources could not be bound");
        }
        return;
    }
    const auto state = BeginRendering(pipeline);

    buffer_cache.BindVertexBuffers(*pipeline);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(index_offset);
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);
    scheduler.NoteDraw();
    FrameStats::Draw();

    const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.CommandBuffer();
    scheduler.BindGraphicsPipeline(pipeline->Handle());

    // SHADPS4_DBG_DRAW_VERTICES=<count> draws no more than that of every draw: with next to
    // no geometry left, what a frame still costs is what its draws cost as such.
    static const u32 vertex_limit = [] {
        const char* value = std::getenv("SHADPS4_DBG_DRAW_VERTICES");
        return value != nullptr && value[0] != 0 ? static_cast<u32>(std::atoi(value)) : ~0u;
    }();
    const u32 num_vertices = std::min<u32>(regs.num_indices, vertex_limit);
    FrameStats::Add(FrameStats::Counter::Vertices,
                    u64{num_vertices} * regs.num_instances.NumInstances());

    if (is_indexed) {
        cmdbuf.drawIndexed(num_vertices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(num_vertices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }

    ResetBindings();
    RetireIsolatedReadConstSnapshots();
}

boost::container::small_vector<u8, 4> Rasterizer::SharedTargetPasses(u8 mrt_mask) const {
    // A title may bind one surface as two colour targets and write some channels through the
    // one, the others through the other: that way each can have blending of its own. (Astro
    // Bot draws the rings on its lava like that, colour blended in through target 0 and
    // alpha written through target 1; both are the same quarter-size buffer.) The console's
    // GPU writes both to the one memory. Vulkan leaves undefined what an image holds that
    // was two attachments of one render pass, and on the desktop GPU this was found on it
    // held the draw in some blocks of 4x8 texels and not in others: squares in the lava's
    // glow around the octopus of world 2. Such a draw is made once for every target that
    // shares a surface, with the others left out: what each pass writes is what the one
    // draw would have.
    const auto& regs = liverpool->regs;
    boost::container::small_vector<u8, 4> passes;
    passes.push_back(0);
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable) {
        return passes;
    }
    u8 used = 0;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        if ((mrt_mask & (1u << cb)) != 0 && regs.color_buffers[cb] &&
            regs.color_target_mask.GetMask(cb) != 0) {
            used |= 1u << cb;
        }
    }
    const auto same_surface = [&](u32 a, u32 b) {
        const auto& first = regs.color_buffers[a];
        const auto& second = regs.color_buffers[b];
        return first.Address() == second.Address() &&
               first.view.slice_start == second.view.slice_start;
    };
    // Every target goes into the first pass that has no target of the same surface yet.
    boost::container::small_vector<u8, 4> drawn;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        if ((used & (1u << cb)) == 0) {
            continue;
        }
        u32 pass = 0;
        for (; pass < drawn.size(); ++pass) {
            bool shared = false;
            for (u32 other = 0; other < cb; ++other) {
                shared |= (drawn[pass] & (1u << other)) != 0 && same_surface(cb, other);
            }
            if (!shared) {
                break;
            }
        }
        if (pass == drawn.size()) {
            drawn.push_back(0);
        }
        drawn[pass] |= 1u << cb;
    }
    if (drawn.size() <= 1) {
        return passes;
    }
    // What a draw does to depth and stencil it would do once for every pass. That is the same
    // only where a second pass passes the depth test where the first did and writes the same
    // depth again, and where nothing counts in the stencil.
    const auto& depth = regs.depth_control;
    const bool writes_depth = depth.depth_enable && depth.depth_write_enable;
    const bool repeatable = depth.depth_func == AmdGpu::CompareFunc::LessEqual ||
                            depth.depth_func == AmdGpu::CompareFunc::GreaterEqual ||
                            depth.depth_func == AmdGpu::CompareFunc::Equal ||
                            depth.depth_func == AmdGpu::CompareFunc::Always;
    if (depth.stencil_enable || (writes_depth && !repeatable)) {
        static bool told = false;
        if (!std::exchange(told, true)) {
            LOG_WARNING(Render_Vulkan,
                        "A draw has one surface ({:#x}) as more than one colour target and "
                        "writes depth or stencil in a way that cannot be done twice: it is "
                        "drawn in one pass, and the picture may have squares where it draws",
                        regs.color_buffers[std::countr_zero(used)].Address());
        }
        return passes;
    }
    passes.clear();
    for (const u8 targets : drawn) {
        passes.push_back(used & ~targets);
    }
    return passes;
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    if (scheduler.IsFlushDue()) {
        Flush();
    }

    if (!FilterDraw()) {
        return;
    }

    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

    buffer_cache.BindVertexBuffers(*pipeline);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(0);
    }

    const auto& [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);

    VideoCore::Buffer* count_buffer{};
    u32 count_base{};
    if (count_address != 0) {
        std::tie(count_buffer, count_base) = buffer_cache.ObtainBuffer(count_address, 4, false);
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);
    scheduler.NoteDraw();
    FrameStats::Draw();

    // We can safely ignore both SGPR UD indices and results of fetch shader parsing, as vertex and
    // instance offsets will be automatically applied by Vulkan from indirect args buffer.

    const auto cmdbuf = scheduler.CommandBuffer();
    scheduler.BindGraphicsPipeline(pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_base, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_base,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
    }

    ResetBindings();
    RetireIsolatedReadConstSnapshots();
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    if (scheduler.IsFlushDue()) {
        Flush();
    }
    FrameStats::Compute();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    const bool trace = TraceDraw();
    if (trace) {
        LOG_INFO(Render_Vulkan, "TRACE dispatch {}x{}x{} pipeline={} cs={:#x}", cs_program.dim_x,
                 cs_program.dim_y, cs_program.dim_z, pipeline != nullptr,
                 pipeline ? pipeline->GetStage(Shader::LogicalStage::Compute).pgm_hash : 0);
    }
    if (!pipeline) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        if (trace) {
            LOG_INFO(Render_Vulkan, "TRACE   dispatch handled by shader HLE");
        }
        return;
    }

    if (!BindResources(pipeline)) {
        if (trace) {
            LOG_INFO(Render_Vulkan, "TRACE   dispatch dropped by BindResources");
        }
        return;
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    ReadBackCopy();

    ResetBindings();
}

void Rasterizer::ReadBackCopy() {
    if (copy_readback.size == 0) {
        return;
    }
    const auto [buffer_id, address, size] = copy_readback;
    copy_readback = {};
    VideoCore::Buffer& source = buffer_cache.GetBuffer(buffer_id);
    auto download = std::make_unique<VideoCore::Buffer>(
        instance, scheduler, VideoCore::MemoryUsage::Download, 0,
        vk::BufferUsageFlagBits::eTransferDst, size);
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::MemoryBarrier2 written{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                               .pMemoryBarriers = &written});
    cmdbuf.copyBuffer(source.Handle(), download->Handle(),
                      vk::BufferCopy{source.Offset(address), 0, size});
    // Once the GPU is done: into the memory behind the guest's, unseen by the caches (a CPU
    // write would mark whole pages as the CPU's and take the GPU's data beside the copy back
    // from guest memory).
    scheduler.DeferOperation([this, address, size, download = std::move(download)] {
        const bool written = memory->TryWriteBacking(std::bit_cast<void*>(address),
                                                     download->mapped_data.data(), size);
        static std::atomic<u32> copies{};
        if (const u32 count = ++copies; count <= 3 || count % 20000 == 0 || !written) {
            LOG_INFO(Render_Vulkan, "Copy shader result {} read back into guest memory{}: {:#x} "
                     "bytes at {:#x}", count, written ? "" : " (failed)", size, address);
        }
    });
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    if (scheduler.IsFlushDue()) {
        Flush();
    }

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatchIndirect(buffer->Handle(), base);

    ResetBindings();
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    pipeline_cache.Checkpoint();
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::WriteBackCpuReadTargets() {
    const auto targets = texture_cache.TakeCpuReadTargets();
    for (const auto& [addr, size] : targets) {
        const bool ok = buffer_cache.WriteBackImage(addr, size);
        static int logged = 0;
        if (logged < 20) {
            ++logged;
            const auto* words = std::bit_cast<const u32*>(addr);
            LOG_WARNING(Render_Vulkan, "Row target write-back {:#x} size={:#x} ok={} {:08x} {:08x} {:08x} {:08x}",
                        addr, size, ok, words[0], words[1], words[2], words[3]);
        }
    }
}

void Rasterizer::OcclusionDump(u64* address, u32 pairs) {
    scheduler.OcclusionDump(address, pairs);
}

void Rasterizer::FlushOcclusion() {
    if (scheduler.HasPendingOcclusion()) {
        scheduler.Flush();
    }
}

// SHADPS4_REMAP_REFRESH=1: memory mapped afresh counts as written on every submit for a while
// after, past the write tracking (for finding out whether that misses what a title's loader
// writes into memory it has just mapped).
static bool RemapRefresh() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_REMAP_REFRESH");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}
struct RecentMap {
    VAddr addr;
    u64 size;
    std::chrono::steady_clock::time_point when;
};
static std::mutex g_recent_maps_mutex;
static std::vector<RecentMap> g_recent_maps;

void Rasterizer::OnSubmit() {
    if (RemapRefresh()) {
        std::vector<RecentMap> maps;
        {
            std::scoped_lock lock{g_recent_maps_mutex};
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(g_recent_maps, [&](const RecentMap& map) {
                return now - map.when > std::chrono::seconds{20};
            });
            maps = g_recent_maps;
        }
        for (const RecentMap& map : maps) {
            buffer_cache.InvalidateMemory(map.addr, map.size);
            texture_cache.InvalidateMemory(map.addr, map.size);
        }
    }
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    buffer_cache.RunGarbageCollector();

    // Every ten seconds, what the caches hold: where the device's memory goes.
    static auto report_time = std::chrono::steady_clock::now();
    if (const auto now = std::chrono::steady_clock::now();
        now - report_time >= std::chrono::seconds{10}) {
        report_time = now;
        const auto buffers = buffer_cache.GetMemoryReport();
        const auto [images, image_bytes] = texture_cache.GetMemoryReport();
        LOG_INFO(Render_Vulkan,
                 "Memory: {} buffers, {} MiB (largest {} MiB); {} images, {} MiB; device {} MiB",
                 buffers.count, buffers.bytes >> 20, buffers.largest >> 20, images,
                 image_bytes >> 20,
                 instance.CanReportMemoryUsage() ? instance.GetDeviceMemoryUsage() >> 20 : 0);
        LOG_INFO(Render_Vulkan, "Memory: Vulkan buffers alive: {}",
                 VideoCore::BufferMemoryStats());
    }
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline)) {
        if (TraceDetails()) {
            LOG_INFO(Render_Vulkan, "TRACE   compute handled as image copy");
        }
        return false;
    }
    // SHADPS4_NO_META_CLEAR_SKIP=1: compute that looks like a metadata clear runs anyway.
    static const bool no_meta_skip = [] {
        const char* value = std::getenv("SHADPS4_NO_META_CLEAR_SKIP");
        return value != nullptr && value[0] == '1';
    }();
    if (!no_meta_skip && IsComputeMetaClear(pipeline)) {
        if (TraceDetails()) {
            LOG_INFO(Render_Vulkan, "TRACE   compute handled as meta clear");
        }
        static std::mutex skipped_mutex;
        static std::unordered_set<u64> skipped;
        const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
        std::scoped_lock lock{skipped_mutex};
        if (skipped.size() < 32 && skipped.insert(cs.pgm_hash).second) {
            std::string buffers;
            for (const auto& desc : cs.buffers) {
                const auto sharp = desc.GetSharp(cs);
                buffers += fmt::format(" {:#x}+{:#x}{}", sharp.base_address, sharp.GetSize(),
                                       desc.is_written ? " W" : "");
            }
            LOG_WARNING(Render_Vulkan, "Compute {:#x} skipped as a metadata clear; buffers{}",
                        cs.pgm_hash, buffers);
        }
        return false;
    }
    if (IsComputeImageClear(pipeline)) {
        if (TraceDetails()) {
            LOG_INFO(Render_Vulkan, "TRACE   compute handled as image clear");
        }
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_barriers.clear();
    buffer_infos.clear();
    image_infos.clear();

    bool uses_dma = false;

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(liverpool->regs);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        stage->PushUd(binding, push_data);
        BindBuffers(*stage, binding, push_data);
        BindTextures(*stage, binding);
        uses_dma |= stage->uses_dma;
    }

    if (uses_dma) {
        // We only use fault buffer for DMA right now.
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (auto& range : mapped_ranges) {
            buffer_cache.SynchronizeBuffersInRange(range.lower(), range.upper() - range.lower());
        }
        fault_process_pending = true;
    }

    return true;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);

    if (TraceDetails()) {
        for (const auto& desc : info.buffers) {
            const auto sharp = desc.GetSharp(info);
            LOG_INFO(Render_Vulkan,
                     "TRACE   cs buffer base={:#x} size={:#x} written={} special={} is_meta={} xor={}",
                     sharp.base_address, sharp.GetSize(), static_cast<bool>(desc.is_written),
                     desc.IsSpecial(), texture_cache.IsMeta(sharp.base_address),
                     static_cast<bool>(info.has_bitwise_xor));
        }
    }

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    if (instance.IsMaintenance8Supported() ||
        src_image.info.props.is_depth == dst_image.info.props.is_depth) {
        dst_image.CopyImage(src_image);
    } else {
        const auto& copy_buffer =
            buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::DeviceLocal);
        dst_image.CopyImageWithBuffer(src_image, copy_buffer.Handle(), 0);
    }
    dst_image.flags |= VideoCore::ImageFlagBits::GpuModified;
    dst_image.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    image1.Clear(clear, range);
    image1.flags |= VideoCore::ImageFlagBits::GpuModified;
    image1.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    buffer_bindings.clear();

    for (const auto& desc : stage.buffers) {
        const auto vsharp = desc.GetSharp(stage);
        if (!desc.IsSpecial() && vsharp.base_address != 0 && vsharp.GetSize() > 0) {
            const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            const auto buffer_id = buffer_cache.FindBuffer(vsharp.base_address, size);
            buffer_bindings.emplace_back(buffer_id, vsharp, size);
        } else {
            buffer_bindings.emplace_back(VideoCore::BufferId{}, vsharp, 0);
        }
    }

    // Compute shaders that only copy one buffer into another (Here They Lie streams its textures
    // into place with one): what they write is read back into guest memory (see DispatchDirect).
    // Without readbacks guest memory never learns what the shader wrote, and a later CPU write
    // to a page of the destination brings that whole page back from guest memory, so the copied
    // texture bytes beside it turn into whatever the memory held before: mips of coloured noise,
    // more the longer the game runs. SHADPS4_COMPUTE_COPY_GUEST=0 turns it off, =<hex hash>
    // names the shader (Here They Lie's by default).
    static const u64 copy_shader = [] {
        const char* value = std::getenv("SHADPS4_COMPUTE_COPY_GUEST");
        if (value != nullptr && *value != '\0') {
            return std::strtoull(value, nullptr, 16);
        }
        return Common::ElfInfo::Instance().GameSerial() == "CUSA05344" ? 0x5e833e8d7310dc62ULL
                                                                        : 0ULL;
    }();
    copy_readback = {};
    if (copy_shader != 0 && stage.pgm_hash == copy_shader &&
        stage.stage == Shader::Stage::Compute && buffer_bindings.size() == 2 &&
        !stage.buffers[0].is_written && stage.buffers[1].is_written) {
        const auto& [dst_id, dst_sharp, dst_size] = buffer_bindings[1];
        if (dst_id && dst_size != 0 && dst_size == std::get<2>(buffer_bindings[0])) {
            copy_readback = {dst_id, dst_sharp.base_address, dst_size};
        }
    }


    // SHADPS4_LOG_SHADER=<hex hash>: every dispatch or draw of that shader, with its buffers.
    static const u64 logged_shader = [] {
        const char* value = std::getenv("SHADPS4_LOG_SHADER");
        return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    // Pipeline keys (and the draw trace) print HashCombine(program hash, permutation).
    const auto is_logged_shader = [&] {
        for (u64 perm = 0; perm < 8; ++perm) {
            if (HashCombine(stage.pgm_hash, perm) == logged_shader) return true;
        }
        return stage.pgm_hash == logged_shader;
    };
    // While a draw trace runs, every pixel shader's buffers are logged as well.
    const bool traced_fragment = stage.stage == Shader::Stage::Fragment && TraceDraw();
    if ((logged_shader != 0 && is_logged_shader()) || traced_fragment) {
        static std::atomic<u32> logged{};
        if (logged++ < 40000) {
            std::string text;
            for (u32 i = 0; i < buffer_bindings.size(); i++) {
                const auto& [id, vsharp, size] = buffer_bindings[i];
                const bool cpu = size > 0 && buffer_cache.IsRegionCpuModified(vsharp.base_address, size);
                const bool gpu = size > 0 && buffer_cache.IsRegionGpuModified(vsharp.base_address, size);
                // Content of what the guest left there, to compare draws between runs.
                u64 hash = 0;
                if (size > 0 && size <= 1_MB && memory->IsValidMapping(vsharp.base_address, size)) {
                    hash = XXH3_64bits(reinterpret_cast<const void*>(vsharp.base_address), size);
                }
                text += fmt::format(" | b{} {:#x}+{:#x}{}{}{} h={:016x}", i, vsharp.base_address,
                                    size, stage.buffers[i].is_written ? " W" : "",
                                    cpu ? " cpu-dirty" : "", gpu ? " gpu-mod" : "", hash);
                if (gpu) text += LastGpuWriter(vsharp.base_address, size);
                // Small read-only buffers (uniforms) are printed whole while a trace runs.
                if (traced_fragment && hash != 0 && size <= 0x1000 && !stage.buffers[i].is_written) {
                    const auto* words = reinterpret_cast<const float*>(vsharp.base_address);
                    text += " [";
                    for (u64 w = 0; w < size / 4; ++w) text += fmt::format("{:.4g} ", words[w]);
                    text += "]";
                }
            }
            LOG_INFO(Render_Vulkan, "Shader {:#x}: ud2={:#x}{}", stage.pgm_hash,
                     stage.user_data.size() > 2 ? stage.user_data[2] : 0u, text);
        }
    }

    // Second pass to re-bind buffers that were updated after binding
    for (u32 i = 0; i < buffer_bindings.size(); i++) {
        const auto& [buffer_id, vsharp, size] = buffer_bindings[i];
        const auto& desc = stage.buffers[i];
        const bool is_storage = desc.IsStorage(vsharp);
        const u32 alignment =
            is_storage ? instance.StorageMinAlignment() : instance.UniformMinAlignment();
        // Buffer is not from the cache, either a special buffer or unbound.
        // Which shaders write the title's streaming memory (from 0x1000000000 up), once each.
        if (desc.is_written && size > 0) {
            RecordGpuWrite(vsharp.base_address, size, stage.pgm_hash);
        }
        if (desc.is_written && vsharp.base_address >= 0x1000000000ULL && size > 0) {
            static std::mutex writers_mutex;
            static std::unordered_set<u64> writers;
            std::scoped_lock lock{writers_mutex};
            if (writers.size() < 64 && writers.insert(stage.pgm_hash ^ (u64{i} << 56)).second) {
                LOG_INFO(Render_Vulkan,
                         "Streaming memory written by shader {:#x} (stage {}), buffer {} at {:#x} "
                         "size {:#x} stride {}",
                         stage.pgm_hash, static_cast<u32>(stage.stage), i, vsharp.base_address,
                         size, vsharp.GetStride());
            }
        }
        if (const VAddr traced = TracedAddress();
            desc.is_written && traced != 0 && size > 0 && traced >= vsharp.base_address &&
            traced < vsharp.base_address + size) {
            static std::atomic<u32> logged{};
            if (logged++ < 400) {
                LOG_INFO(Render_Vulkan,
                         "Traced address: written by shader {:#x} (stage {}), buffer {} at {:#x} "
                         "size {:#x} stride {}",
                         stage.pgm_hash, static_cast<u32>(stage.stage), i, vsharp.base_address,
                         size, vsharp.GetStride());
            }
        }
        // SHADPS4_ISOLATE_VS=1: every buffer a vertex shader only reads is copied from the
        // game's memory for the draw, past the cache (for finding stale vertex data).
        static const bool isolate_vs = [] {
            const char* value = std::getenv("SHADPS4_ISOLATE_VS");
            return value != nullptr && value[0] == '1';
        }();
        // SHADPS4_ISOLATE_PS_UNIFORMS=1: the same for the small buffers a pixel shader only
        // reads, so a stale cached copy of the game's constants cannot reach the draw.
        static const bool isolate_ps = [] {
            const char* value = std::getenv("SHADPS4_ISOLATE_PS_UNIFORMS");
            return value != nullptr && value[0] == '1';
        }();
        const bool isolate_this =
            (isolate_vs && stage.stage == Shader::Stage::Vertex && !desc.is_written &&
             size <= 64_MB) ||
            (isolate_ps && stage.stage == Shader::Stage::Fragment && !desc.is_written &&
             size <= 64_KB);
        if (TraceDetails() &&
            stage.stage == Shader::Stage::Vertex) {
            LOG_INFO(Render_Vulkan, "TRACE   vs buffer {} at {:#x} size {:#x} stride {} {}{}", i,
                     vsharp.base_address, size, vsharp.GetStride(),
                     desc.is_written ? "written " : "",
                     vsharp.base_address != 0 && size > 0 &&
                             buffer_cache.IsRegionGpuModified(vsharp.base_address, size)
                         ? "gpu-modified"
                         : "");
        }
        // The cooked M_PeripheryCircleVR pixel shader samples the original scene, then
        // multiplies it by 1-clamp((distance-CircleRadius)/CircleFadeArea,0,1)^2.
        // Its material collection is binding 1, a float4 whose first component is radius.
        // Override only this draw's uploaded collection; guest memory, fades and colors
        // remain owned by the game. Hash verified against the original update pak asset.
        // SHADPS4_HTL_NO_VIGNETTE=1, or Comfort Vignette unticked in the game's options.
        static const bool htl_no_vignette_forced = [] {
            const char* value = std::getenv("SHADPS4_HTL_NO_VIGNETTE");
            return value && value[0] == '1';
        }();
        const bool htl_no_vignette = stage.pgm_hash == 0xf384d56ec6aea4dfULL &&
                                     (htl_no_vignette_forced || !Core::HtlOptions::ComfortVignette());
        if (htl_no_vignette && stage.stage == Shader::Stage::Fragment &&
            stage.pgm_hash == 0xf384d56ec6aea4dfULL) {
            static u32 inspected{};
            if (inspected++ < 12) Common::Diagnostics::Session::Get().Record(
                {{"type", "htl_vignette_binding"}, {"index", i}, {"size", size},
                 {"written", desc.is_written},
                 {"gpu_modified", size && vsharp.base_address &&
                     buffer_cache.IsRegionGpuModified(vsharp.base_address, size)}});
        }
        const bool periphery = htl_no_vignette && stage.stage == Shader::Stage::Fragment &&
            stage.pgm_hash == 0xf384d56ec6aea4dfULL && i == 1 && !desc.is_written &&
            size == 16 && Common::ElfInfo::Instance().GameSerial() == "CUSA05344" &&
            memory->IsValidMapping(vsharp.base_address, size) &&
            !buffer_cache.IsRegionGpuModified(vsharp.base_address, size);
        if (periphery) {
            std::array<float, 4> collection;
            std::memcpy(collection.data(), reinterpret_cast<const void*>(vsharp.base_address), size);
            const float original = collection[0];
            collection[0] = 100.0f;
            auto& upload = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
            const u64 offset = upload.Copy(collection.data(), size, alignment);
            buffer_infos.emplace_back(upload.Handle(), offset, size);
            static u64 overrides{};
            if (++overrides <= 4 || overrides % 600 == 0) {
                Common::Diagnostics::Session::Get().Record({{"type", "htl_vignette"},
                    {"draws", overrides}, {"radius_before", original}, {"radius_after", 100.0f}});
                LOG_INFO(Render_Vulkan, "HTL movement vignette: radius {} -> 100 (draw {})",
                         original, overrides);
            }
        } else if ((isolate_this || ShouldIsolateReadConstBuffer(stage.pgm_hash, stage.stage,
                                                          desc.used_as_readconst,
                                                          desc.is_written)) &&
            vsharp.base_address != 0 && size > 0) {
            auto isolated = IsolateReadConstGuestBuffer(vsharp.base_address, size);
            buffer_infos.emplace_back(isolated->Handle(), 0, size);
            buffer_barriers.emplace_back(vk::BufferMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eHost,
                .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .buffer = isolated->Handle(),
                .offset = 0,
                .size = size,
            });
            isolated_readconst_buffers.push_back(std::move(isolated));
        } else if (!buffer_id) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const u32 ubo_size = stage.flattened_ud_buf.size() * sizeof(u32);
                const u64 offset =
                    vk_buffer.Copy(stage.flattened_ud_buf.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const auto& cs_program = liverpool->GetCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else if (instance.IsNullDescriptorSupported()) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                auto& null_buffer = buffer_cache.GetBuffer(VideoCore::NULL_BUFFER_ID);
                buffer_infos.emplace_back(null_buffer.Handle(), 0, VK_WHOLE_SIZE);
            }
        } else {
            const auto [vk_buffer, offset] = buffer_cache.ObtainBuffer(
                vsharp.base_address, size, desc.is_written, desc.is_formatted, buffer_id);
            const u32 offset_aligned = Common::AlignDown(offset, alignment);
            const u32 adjust = offset - offset_aligned;
            ASSERT(adjust % 4 == 0);
            push_data.AddOffset(binding.buffer, adjust);
            buffer_infos.emplace_back(vk_buffer->Handle(), offset_aligned, size + adjust);
            if (auto barrier =
                    vk_buffer->GetBarrier(desc.is_written ? vk::AccessFlagBits2::eShaderWrite
                                                          : vk::AccessFlagBits2::eShaderRead,
                                          vk::PipelineStageFlagBits2::eAllCommands)) {
                buffer_barriers.emplace_back(*barrier);
            }
            if (desc.is_written && desc.is_formatted) {
                texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
            }
        }

        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eUniformBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

std::unique_ptr<VideoCore::Buffer> Rasterizer::IsolateReadConstGuestBuffer(VAddr addr, u64 size) {
    const u64 alloc_size = std::max<u64>(size, instance.NonCoherentAtomSize());
    auto isolated = std::make_unique<VideoCore::Buffer>(instance, scheduler, VideoCore::MemoryUsage::Upload,
                                                        addr, VideoCore::AllFlags, alloc_size);
    ASSERT_MSG(!isolated->mapped_data.empty(),
               "ReadConst snapshot buffer is not host-visible");
    const auto* src = reinterpret_cast<const u8*>(addr);
    std::memcpy(isolated->mapped_data.data(), src, size);
    if (size < alloc_size) {
        std::memset(isolated->mapped_data.data() + size, 0, alloc_size - size);
    }
    if (!isolated->is_coherent) {
        const VkDeviceSize flush_size =
            Common::AlignUp(alloc_size, instance.NonCoherentAtomSize());
        const VkResult flush_result =
            vmaFlushAllocation(instance.GetAllocator(), isolated->buffer.allocation, 0, flush_size);
        ASSERT_MSG(flush_result == VK_SUCCESS, "vmaFlushAllocation failed: {}",
                   vk::to_string(vk::Result{flush_result}));
    }

    ++isolated_readconst_hits;
    if (isolated_readconst_hits <= 16 || (isolated_readconst_hits % 120) == 0) {
        u32 checksum = 0;
        const u32 dword_count = static_cast<u32>(size / 4);
        const auto* dwords = reinterpret_cast<const u32*>(src);
        for (u32 i = 0; i < dword_count; ++i) {
            checksum ^= dwords[i] + i * 0x9e3779b9u;
        }
        LOG_WARNING(Render_Vulkan,
                    "READCONST_SNAPSHOT hit={} addr={:#x} size={:#x} coherent={} checksum={:#010x}",
                    isolated_readconst_hits, addr, size, isolated->is_coherent, checksum);
        const ReadConstFloatStats float_stats =
            ClassifyReadConstFloats(std::span{dwords, dword_count});
        std::array<float, 8> head{};
        for (u32 i = 0; i < std::min<u32>(head.size(), dword_count); ++i) {
            head[i] = std::bit_cast<float>(dwords[i]);
        }
        // DSR's skinned VS divides the runtime vertex index by a per-frame
        // divisor read from const dwords 152..155 (rcp-based integer division
        // emulation feeding the x80-stride fetch address). Export them raw so
        // idle-vs-walk divisor values can be compared offline.
        std::array<u32, 5> div{};
        const bool have_div = dword_count >= 157;
        if (have_div) {
            std::memcpy(div.data(), dwords + 152, div.size() * sizeof(u32));
        }
        LOG_WARNING(Render_Vulkan,
                    "READCONST_FLOATS hit={} floats={} nan={} inf={} denorm={} huge1e4={} "
                    "maxabs={:.6g} head=[{:.4g},{:.4g},{:.4g},{:.4g},{:.4g},{:.4g},{:.4g},{:.4g}] "
                    "div152=[{:#010x},{:#010x},{:#010x},{:#010x},{:#010x}]",
                    isolated_readconst_hits, dword_count, float_stats.nan_count,
                    float_stats.inf_count, float_stats.denorm_count, float_stats.huge_count,
                    float_stats.max_abs, head[0], head[1], head[2], head[3], head[4], head[5],
                    head[6], head[7], div[0], div[1], div[2], div[3], div[4]);
    }
    return isolated;
}

void Rasterizer::RetireIsolatedReadConstSnapshots() {
    if (isolated_readconst_buffers.empty()) {
        return;
    }
    scheduler.Finish();
    isolated_readconst_buffers.clear();
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    if (TraceDetails() &&
        (stage.stage == Shader::Stage::Fragment || stage.stage == Shader::Stage::Compute)) {
        for (u32 i = 0; i < stage.images.size(); ++i) {
            const auto& desc = stage.images[i];
            const auto image = desc.GetSharp(stage);
            const auto sel = image.DstSelect();
            LOG_INFO(Render_Vulkan,
                     "TRACE   {} image {} at {:#x} type {} {}x{}x{} layers {} fmt {}/{} "
                     "sel {}{}{}{} {}{}",
                     stage.stage == Shader::Stage::Compute ? "cs" : "ps", i, image.Address(),
                     static_cast<u32>(image.GetType()), image.width + 1, image.height + 1,
                     image.depth + 1, image.NumLayers(),
                     static_cast<u32>(image.GetDataFmt()), static_cast<u32>(image.GetNumberFmt()),
                     static_cast<u32>(sel.r), static_cast<u32>(sel.g), static_cast<u32>(sel.b),
                     static_cast<u32>(sel.a), desc.is_written ? "written " : "",
                     desc.is_array ? "array" : "");
        }
    }
    image_bindings.clear();
    const u32 first_image_idx = image_infos.size();
    // For loading/storing to explicit mip levels, when no native instruction support, bind an array
    // of descriptors consecutively, 1 for each mip level. The shader can index this with LOD
    // operand.
    // This array holds the size of each consecutive array with the number of bindings consumed.
    // This is currently always 1 for anything other than mip fallback arrays.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;

    for (const auto& image_desc : stage.images) {
        const auto tsharp = image_desc.GetSharp(stage);
        if (texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        if (tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = image_desc.NumBindings(stage);

        for (auto i = 0; i < num_bindings; i++) {
            auto& [image_id, desc] = image_bindings.emplace_back(
                std::piecewise_construct, std::tuple{}, std::tuple{tsharp, image_desc});

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            image_id = texture_cache.FindImage(desc);
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // While draws are traced: what each shader samples, once a shader, and what the texture
    // cache found for it.
    // Pixel shaders are logged on every traced draw, also in the light trace the lab runs.
    const bool every_draw = TraceDraw() && stage.stage == Shader::Stage::Fragment;
    if (TraceDetails() || every_draw) {
        static std::mutex traced_mutex;
        static std::unordered_set<u64> traced_shaders;
        bool first;
        {
            std::scoped_lock lock{traced_mutex};
            first = traced_shaders.insert(stage.pgm_hash).second;
        }
        if (first || every_draw) {
            u32 index = 0;
            for (const auto& image_desc : stage.images) {
                const auto tsharp = image_desc.GetSharp(stage);
                const auto& [image_id, desc] = image_bindings[std::min<size_t>(
                    index, image_bindings.size() - 1)];
                std::string found = "nothing";
                if (image_id) {
                    const auto& image = texture_cache.GetImage(image_id);
                    found = fmt::format(
                        "image #{} {}x{}x{} {} at {:#x} levels {} layers {} size {:#x}{}{}{} (asked: "
                        "levels {} layers {} size {:#x})",
                        image_id.index,
                        image.info.size.width, image.info.size.height, image.info.size.depth,
                        vk::to_string(image.info.pixel_format), image.info.guest_address,
                        image.info.resources.levels, image.info.resources.layers,
                        image.info.guest_size, image.info.props.is_depth ? " depth" : "",
                        image.binding.is_target ? " target" : "",
                        True(image.flags & VideoCore::ImageFlagBits::GpuModified) ? " gpu-modified" : "",
                        desc.info.resources.levels,
                        desc.info.resources.layers, desc.info.guest_size);
                }
                LOG_INFO(Render_Vulkan,
                         "TRACE   texture {} of shader {:#x} (stage {}, cb0 {:#x}, {} indices): "
                         "type {} {}x{}x{} fmt {}/{} levels "
                         "{}-{} at {:#x}{} -> {}",
                         index, stage.pgm_hash, static_cast<u32>(stage.stage),
                         liverpool->regs.color_buffers[0].Address(), liverpool->regs.num_indices,
                         static_cast<u32>(tsharp.GetType()),
                         tsharp.width + 1, tsharp.height + 1, tsharp.depth + 1,
                         static_cast<u32>(tsharp.GetDataFmt()),
                         static_cast<u32>(tsharp.GetNumberFmt()), tsharp.base_level,
                         tsharp.last_level, tsharp.Address(),
                         image_desc.is_written ? " written" : "", found);
                index += image_desc.NumBindings(stage);
            }
        }
    }

    // Second pass to re-bind images that were updated after binding
    for (auto& [image_id, desc] : image_bindings) {
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            if (instance.IsNullDescriptorSupported()) {
                image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            } else {
                auto& null_image_view = texture_cache.FindTexture(VideoCore::NULL_IMAGE_ID, desc);
                image_infos.emplace_back(VK_NULL_HANDLE, *null_image_view.image_view,
                                         vk::ImageLayout::eGeneral);
            }
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(image_id, desc);

            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((image.binding.force_general || image.binding.is_target) &&
                !image.info.props.is_depth) {
                image.Transit(instance.IsAttachmentFeedbackLoopLayoutSupported() &&
                                      image.binding.is_target
                                  ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                  : vk::ImageLayout::eGeneral,
                              vk::AccessFlagBits2::eShaderRead |
                                  (image.info.props.is_depth
                                       ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                       : vk::AccessFlagBits2::eColorAttachmentWrite |
                                             vk::AccessFlagBits2::eColorAttachmentRead),
                              {});
            } else {
                if (is_storage) {
                    image.Transit(vk::ImageLayout::eGeneral,
                                  vk::AccessFlagBits2::eShaderRead |
                                      vk::AccessFlagBits2::eShaderWrite,
                                  desc.view_info.range);
                } else if (image.info.props.is_depth && image.binding.is_target) {
                    // The draw tests against the depth buffer it reads: one state for both
                    // uses, so that a run of such draws does not go back and forth between
                    // two (which would end the render pass before each of them).
                    const auto state = GetDepthTargetState(image, liverpool->regs);
                    image.Transit(state.layout, state.access, desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    image.Transit(new_layout, vk::AccessFlagBits2::eShaderRead,
                                  desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view,
                                     image.backing->state.layout);
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (sampler.disable_aniso) {
            const auto& tsharp = stage.images[sampler.associated_image].GetSharp(stage);
            if (tsharp.base_level == 0 && tsharp.last_level == 0) {
                ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        }
        const auto vk_sampler = texture_cache.GetSampler(ssharp, liverpool->regs.ta_bc_base);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        image->SetBackingSamples(key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        static const bool skip_reflection_clear_enabled =
            std::getenv("SHADPS4_DBG_SKIP_REFLECTION_CLEAR") != nullptr;
        const bool skip_reflection_clear = skip_reflection_clear_enabled &&
            image->info.size.width == 128 && image->info.resources.layers >= 6;
        const bool is_clear = !skip_reflection_clear && texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            image->Transit(instance.IsAttachmentFeedbackLoopLayoutSupported()
                               ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                               : vk::ImageLayout::eGeneral,
                           vk::AccessFlagBits2::eColorAttachmentWrite, {});
            attachment_feedback_loop = true;
        } else {
            // (A volume drawn through a 2D-array view has one layer as far as barriers go.)
            image->Transit(vk::ImageLayout::eColorAttachmentOptimal,
                           vk::AccessFlagBits2::eColorAttachmentWrite |
                               vk::AccessFlagBits2::eColorAttachmentRead,
                           image->info.type == AmdGpu::ImageType::Color3D
                               ? std::nullopt
                               : std::optional{desc.view_info.range});
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear = regs.depth_render_control.depth_clear_enable ||
                                    texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const auto target_state = GetDepthTargetState(image, regs);
        image.Transit(target_state.layout, target_state.access, desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        // RenderState is reused. A previous stencil attachment must not survive a
        // switch to a depth-only format (e.g. D16 shadow maps).
        attachment = {};
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid() && image.info.props.has_stencil) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->regs.color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->regs.color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->regs.color_buffers[0].Address(),
                                 liverpool->regs.color_buffers[1].Address()));
    mrt1_image.Resolve(mrt0_image, mrt0_desc.view_info.range, mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->regs;

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->regs.depth_view.slice_start;
    sub_range.extent.layers = liverpool->regs.depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    read_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                       sub_range);
    write_image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                        sub_range);

    auto aspect_mask = vk::ImageAspectFlags(0);
    if (is_depth) {
        aspect_mask |= vk::ImageAspectFlagBits::eDepth;
    }
    if (is_stencil) {
        aspect_mask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .dstOffset = {0, 0, 0},
        .extent = {write_image.info.size.width, write_image.info.size.height, 1},
    };
    scheduler.CommandBuffer().copyImage(read_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                        write_image.GetImage(),
                                        vk::ImageLayout::eTransferDstOptimal, region);

    ScopeMarkerEnd();
}

bool Rasterizer::TryHtileClear(VAddr address, std::span<const u32> htile_words) {
    if (htile_words.empty() || !texture_cache.IsHtile(address)) {
        return false;
    }

    // The low four bits of an HTILE entry are its zmask; zero means every pixel of that 8x8 tile
    // holds the depth clear value. Titles clear depth by filling the surface with such entries,
    // or by copying in a template they prepared once. A template may also mark some tiles as
    // permanently occluded (zero depth range), which is how VR titles skip the part of the eye
    // buffer the lenses cannot show; those tiles only save work, so a plain clear is equivalent.
    u64 cleared = 0;
    u64 written = 0;
    for (const u32 word : htile_words) {
        if (word == 0) {
            // Padding past the end of the surface.
            continue;
        }
        ++written;
        cleared += (word & 0xf) == 0;
    }
    if (cleared * 2 < written || cleared == 0) {
        return false;
    }
    return texture_cache.ClearMeta(address);
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    if (TraceDraw()) {
        LOG_INFO(Render_Vulkan, "TRACE dma-fill dst={:#x} bytes={:#x} value={:#x} gds={} is_meta={}",
                 address, num_bytes, value, is_gds, texture_cache.IsMeta(address));
    }
    if (!is_gds && value != 0 && TryHtileClear(address, std::span{&value, 1})) {
        return;
    }
    buffer_cache.FillBuffer(address, num_bytes, value, is_gds);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (TraceDraw()) {
        LOG_INFO(Render_Vulkan, "TRACE dma-copy dst={:#x} src={:#x} bytes={:#x} is_meta={}", dst,
                 src, num_bytes, texture_cache.IsMeta(dst));
    }
    if (!dst_gds && !src_gds &&
        TryHtileClear(dst, std::span{reinterpret_cast<const u32*>(src), num_bytes / sizeof(u32)})) {
        return;
    }
    if (!dst_gds) {
        RecordGpuWrite(dst, num_bytes, 0xD3A0000000000000ULL | src);
    }
    buffer_cache.CopyBuffer(dst, src, num_bytes, dst_gds, src_gds);
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

// SHADPS4_TRACE_ADDR=<hex>: what happens to the cache's view of that address, logged.
VAddr TracedAddress() {
    static const VAddr addr = [] {
        const char* value = std::getenv("SHADPS4_TRACE_ADDR");
        return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    return addr;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size) {
    if (const VAddr traced = TracedAddress(); traced != 0 && traced >= addr &&
                                              traced < addr + size) {
        LOG_INFO(Render_Vulkan, "Traced address: invalidated by {:#x}+{:#x} (mapped {})", addr,
                 size, IsMapped(addr, size));
    }
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
    if (RemapRefresh() && size <= 64_MB) {
        std::scoped_lock lock{g_recent_maps_mutex};
        g_recent_maps.push_back({addr, size, std::chrono::steady_clock::now()});
    }
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    if (const VAddr traced = TracedAddress(); traced != 0 && traced >= addr &&
                                              traced < addr + size) {
        LOG_INFO(Render_Vulkan, "Traced address: unmapped with {:#x}+{:#x}", addr, size);
    }
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    page_manager.OnGpuUnmap(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    u32 viewport_count = 1;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        if (regs.viewports[i].xscale != 0.f) {
            viewport_count = i + 1;
        }
    }
    for (u32 i = 0; i < viewport_count; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            // ViewportIndex refers to the register slot, not to a compacted list of active
            // viewports. Keep disabled slots so subsequent eyes retain their indices.
            viewports.push_back({.width = 1.f, .height = 1.f, .maxDepth = 1.f});
            scissors.push_back({.offset = {0, 0}, .extent = {0, 0}});
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    // A measuring aid, not a setting: SHADPS4_DBG_VIEWPORT_SCALE=<0..1> shrinks everything that is
    // drawn towards the corner of its target. The picture is wrong, but how much faster the GPU
    // gets tells how much of its time goes into filling pixels rather than into the draws
    // themselves.
    static const float debug_scale = [] {
        const char* value = std::getenv("SHADPS4_DBG_VIEWPORT_SCALE");
        const float scale = value != nullptr ? static_cast<float>(std::atof(value)) : 1.0f;
        return scale > 0.0f && scale < 1.0f ? scale : 1.0f;
    }();
    if (debug_scale != 1.0f) {
        for (auto& viewport : viewports) {
            viewport.x *= debug_scale;
            viewport.y *= debug_scale;
            viewport.width *= debug_scale;
            viewport.height *= debug_scale;
        }
        for (auto& scissor : scissors) {
            scissor.offset.x = static_cast<s32>(scissor.offset.x * debug_scale);
            scissor.offset.y = static_cast<s32>(scissor.offset.y * debug_scale);
            if (scissor.extent.width != 0) {
                scissor.extent.width =
                    std::max(static_cast<u32>(scissor.extent.width * debug_scale), 1u);
            }
            if (scissor.extent.height != 0) {
                scissor.extent.height =
                    std::max(static_cast<u32>(scissor.extent.height * debug_scale), 1u);
            }
        }
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    static const bool dbg_no_depth = std::getenv("SHADPS4_DBG_NO_DEPTH") != nullptr;
    static const bool dbg_no_stencil = std::getenv("SHADPS4_DBG_NO_STENCIL") != nullptr;
    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid() && !dbg_no_depth;
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    // Until Dawn: Rush of Blood (CUSA02350) clears its stencil to 0 and then draws its scene
    // only where the stencil is at least 16, which keeps it to the inside of the headset's
    // lenses; nothing it does here writes that 16, so under emulation the scene is never drawn.
    // The test only saves the console work outside the lenses, so it is left out: a stencil test
    // of "16 or more" that writes nothing, for this title only (SHADPS4_LENS_STENCIL=1 keeps it).
    static const bool skip_lens_stencil = [] {
        const char* value = std::getenv("SHADPS4_LENS_STENCIL");
        if (value != nullptr) {
            return std::string_view{value} != "1";
        }
        return Common::ElfInfo::Instance().GameSerial() == "CUSA02350";
    }();
    const auto& lens_ref = regs.stencil_ref_front;
    const bool lens_stencil =
        skip_lens_stencil &&
        regs.depth_control.stencil_ref_func == AmdGpu::CompareFunc::LessEqual &&
        lens_ref.stencil_test_val == 16 &&
        regs.stencil_control.stencil_fail_front == AmdGpu::StencilFunc::Keep &&
        regs.stencil_control.stencil_zpass_front == AmdGpu::StencilFunc::Keep &&
        regs.stencil_control.stencil_zfail_front == AmdGpu::StencilFunc::Keep &&
        !regs.depth_render_control.stencil_clear_enable;
    const auto stencil_test_enabled = regs.depth_control.stencil_enable &&
                                      regs.depth_buffer.StencilValid() && !dbg_no_stencil &&
                                      !lens_stencil;
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        dynamic_state.SetStencilReferences(front.stencil_test_val, back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if (from_guest && TraceDraw()) {
        LOG_INFO(Render_Vulkan, "TRACE marker {}", str);
    }
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

} // namespace Vulkan

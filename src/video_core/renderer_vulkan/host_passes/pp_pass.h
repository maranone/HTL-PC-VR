//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

namespace Vulkan {
class Instance;
class Frame;
} // namespace Vulkan

namespace Vulkan::HostPasses {

/// SHADPS4_HDR_EXPAND=<peak nits>[,<paper white nits>]: a title that only draws SDR is shown on an
/// HDR10 display all the same, its white at paper white (200 unless told) and its brightest
/// light stretched up to the peak. Zeros when unset or out of range.
struct HdrExpand {
    float peak_nits = 0.0f;
    float paper_nits = 0.0f;
    static const HdrExpand& Get() {
        static const HdrExpand value = [] {
            HdrExpand result{};
            const char* text = std::getenv("SHADPS4_HDR_EXPAND");
            if (text == nullptr) {
                return result;
            }
            float peak = 0.0f, paper = 200.0f;
            if (std::sscanf(text, "%f,%f", &peak, &paper) >= 1 && paper >= 80.0f &&
                paper <= 500.0f && peak >= paper && peak <= 10000.0f) {
                result.peak_nits = peak;
                result.paper_nits = paper;
            }
            return result;
        }();
        return value;
    }
    bool Enabled() const {
        return paper_nits > 0.0f;
    }
};

class PostProcessingPass {
public:
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        /// 0..1: how much the picture is sharpened on the way (host_shaders/post_process.frag).
        float sharpen = 0.0f;
        /// For an output image of an sRGB format, which does the encoding for display itself.
        u32 linear_out = 0;
        std::array<float, 4> source_uv{1.0f, 1.0f, 0.0f, 0.0f};
        /// Around mid grey, as displayed: above 1 darks darker and lights lighter.
        float contrast = 1.0f;
        /// 0 grey, 1 as drawn, above 1 more colourful.
        float saturation = 1.0f;
        /// Above 0: an SDR picture shown on an HDR10 target (SHADPS4_HDR_EXPAND).
        float hdr_paper_nits = 0.0f;
        float hdr_peak_nits = 0.0f;
    };

    static_assert(sizeof(Settings) == 48); // Matches the fragment shader push constants.

    void Create(const Instance& instance, MasterSemaphore* master_semaphore,
                vk::Format surface_format);

    /// An input image and the part of the output frame it is drawn into.
    struct Region {
        vk::ImageView input;
        vk::Rect2D area;
        std::optional<vk::Rect2D> clip;
        /// Scale XY and offset XY into the input texture (packed stereo uses half its width).
        std::array<float, 4> source_uv{1.0f, 1.0f, 0.0f, 0.0f};
    };

    // Frame marker: a row of black and white blocks stamped along the top-left edge of a frame.
    // Two sync blocks (white, black) are followed by a 16 bit frame id and an 8 bit check value,
    // least significant bit first. It lets a host compositor recognise which emulated frame it
    // is looking at after the image has travelled through a window system.
    static constexpr u32 MarkerBlockSize = 8;
    static constexpr u32 MarkerSyncBlocks = 2;
    static constexpr u32 MarkerIdBits = 16;
    static constexpr u32 MarkerCheckBits = 8;
    static constexpr u32 MarkerBlocks = MarkerSyncBlocks + MarkerIdBits + MarkerCheckBits;
    static constexpr u32 MarkerCheck(u32 id) {
        return ((id & 0xff) ^ ((id >> 8) & 0xff) ^ 0x5a) & 0xff;
    }

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings);

    /// Draws every region into the output frame and optionally stamps a frame marker.
    void Render(vk::CommandBuffer cmdbuf, std::span<const Region> regions, Frame& output,
                Settings settings, std::optional<u32> marker = std::nullopt);

private:
    vk::Device device{};
    bool uses_push_descriptors{};
    // Pool sizes must outlive desc_heap (DescriptorHeap stores a span to it).
    static constexpr std::array<vk::DescriptorPoolSize, 1> pool_sizes{{
        {vk::DescriptorType::eCombinedImageSampler, 64},
    }};
    DescriptorHeap desc_heap;
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// AMD FSR 4.1 through the AMD FSR API (FidelityFX SDK 2.4). Only the API's MIT headers are compiled
// in (externals/ffx-api); AMD's signed amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_upscaler_vk.dll are loaded at run time from the emulator's folder.

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Common {
class DynamicLibrary;
}

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

class Fsr4Upscaler final : public TemporalUpscaler {
public:
    /// Throws vk::Exception when AMD's loader DLL is missing or has no FSR upscaler.
    explicit Fsr4Upscaler(const Device& device, Scheduler& scheduler,
                          MemoryAllocator& memory_allocator);
    ~Fsr4Upscaler() override;

    bool Upscale(const Inputs& inputs) override;

    [[nodiscard]] bool Failed() const noexcept override {
        return failed.load();
    }
    [[nodiscard]] VkImage OutputImage() const noexcept override {
        return *output_image;
    }
    [[nodiscard]] VkImageView OutputView() const noexcept override {
        return *output_view;
    }
    [[nodiscard]] VkSampler PointSampler() const noexcept override {
        return *point_sampler;
    }

private:
    struct Api;

    /// (Re)creates the AMD context and the output image for these sizes and colour encoding.
    bool CreateContext(const Inputs& inputs);
    void DestroyContext();
    void CreateOutput(VkExtent2D upscale_size);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    std::unique_ptr<Common::DynamicLibrary> library;
    std::unique_ptr<Api> api;
    void* context{}; ///< ffxContext
    std::optional<u64> version_id; ///< The FSR 4 provider picked at start, if the loader has one.
    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    bool hdr_input = true;
    bool depth_inverted = true;
    std::optional<std::chrono::steady_clock::time_point> last_dispatch;
    vk::Image output_image;
    vk::ImageView output_view;
    vk::Sampler point_sampler;
    std::atomic<bool> failed{};
};

} // namespace Vulkan

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

/// A temporal upscaler fed with a game's colour, depth and motion at the render size, writing an
/// RGBA16F image at the upscale size: NVIDIA DLSS through Streamline, or AMD FSR 4.1.
class TemporalUpscaler {
public:
    struct Image {
        VkImageView view;
        VkImage image;
        VkFormat format;
        VkImageSubresourceRange range;     ///< The view's level and layer, with the view's aspect.
        VkImageAspectFlags barrier_aspect; ///< Every aspect of the image's format.
    };

    struct Inputs {
        Image color;          ///< Jittered colour at the render size.
        Image depth;          ///< Device depth (reversed) at the render size.
        Image motion_vectors; ///< Motion to the previous frame at the render size.
        VkExtent2D render_size;
        VkExtent2D upscale_size;
        /// This frame's sample offset in render pixels, as the game hands its own DLSS.
        std::array<f32, 2> jitter;
        /// Turns a motion vector into render pixels (the render size for UV offsets).
        std::array<f32, 2> motion_vector_scale;
        bool reset;
        bool depth_inverted = true;
        bool hdr = true; ///< Linear HDR colour. False for tonemapped, perceptually encoded colour.
    };

    virtual ~TemporalUpscaler() = default;

    /// Records one evaluation into the output image (RGBA16F at the upscale size, GENERAL
    /// layout). Returns false when the upscaler can't run, so the caller can fall back.
    virtual bool Upscale(const Inputs& inputs) = 0;

    /// True once a recorded evaluation failed; later evaluations are not attempted.
    [[nodiscard]] virtual bool Failed() const noexcept = 0;

    [[nodiscard]] virtual VkImage OutputImage() const noexcept = 0;
    [[nodiscard]] virtual VkImageView OutputView() const noexcept = 0;
    /// Nearest-neighbour clamp sampler (normalized coordinates), for reading the output.
    [[nodiscard]] virtual VkSampler PointSampler() const noexcept = 0;
};

} // namespace Vulkan

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// AMD FSR 4.1 through the AMD FSR API, in place of a game's own upscale (see
// vk_fsr4_upscaler.h). The host side follows the API guide of FidelityFX SDK 2.4
// (Kits/FidelityFX/docs/getting-started/ffx-api.md): a Vulkan backend description chained to the
// upscale context description, inputs read in SHADER_READ_ONLY_OPTIMAL and the output written in
// GENERAL.

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "common/dynamic_library.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/string_util.h"
#include "video_core/renderer_vulkan/vk_fsr4_upscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

#include "api/include/ffx_api.h"
#include "api/include/ffx_api_types.h"
#include "api/include/vk/ffx_api_vk.h"
#include "upscalers/include/ffx_upscale.h"

namespace Vulkan {
namespace {

constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr const char* LOADER_DLL = "amd_fidelityfx_loader_vk.dll";

void OnMessage(uint32_t type, const wchar_t* message) {
    const std::string text = message ? Common::UTF16ToUTF8(std::wstring_view{message}) : "";
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LOG_ERROR(Render_Vulkan, "AMD FSR: {}", text);
    } else {
        LOG_WARNING(Render_Vulkan, "AMD FSR: {}", text);
    }
}

/// Major, minor and patch of a provider name such as "4.1.2"; zeros for anything else.
std::array<u32, 3> ParseVersion(std::string_view name) {
    std::array<u32, 3> version{};
    std::sscanf(std::string{name}.c_str(), "%u.%u.%u", &version[0], &version[1], &version[2]);
    return version;
}

VkImageMemoryBarrier Barrier(const TemporalUpscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = range,
    };
}

FfxApiResource Resource(VkImage image, VkFormat format, VkExtent2D size, VkImageUsageFlags usage,
                        u32 state) {
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    return ffxApiGetResourceVK(image, ffxApiGetImageResourceDescriptionVK(image, info, 0), state);
}

} // Anonymous namespace

struct Fsr4Upscaler::Api {
    PfnFfxCreateContext create{};
    PfnFfxDestroyContext destroy{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
    // The context keeps pointers into its creation descriptions until it is destroyed.
    VkDeviceContext device_context{};
    ffxCreateBackendVKDesc backend{};
    ffxOverrideVersion version{};
    ffxCreateContextDescUpscale upscale{};
};

Fsr4Upscaler::Fsr4Upscaler(const Device& device_, Scheduler& scheduler_,
                           MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      library{std::make_unique<Common::DynamicLibrary>()}, api{std::make_unique<Api>()} {
    const std::string path = Common::FS::PathToUTF8String(Common::FS::GetExeDirectory() / LOADER_DLL);
    if (!library->Open(path.c_str())) {
        LOG_WARNING(Render_Vulkan, "AMD FSR 4.1: {} is missing; place AMD's signed loader and "
                                   "amd_fidelityfx_upscaler_vk.dll next to the emulator",
                    path);
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    if (!library->GetSymbol("ffxCreateContext", &api->create) ||
        !library->GetSymbol("ffxDestroyContext", &api->destroy) ||
        !library->GetSymbol("ffxQuery", &api->query) ||
        !library->GetSymbol("ffxDispatch", &api->dispatch)) {
        LOG_ERROR(Render_Vulkan, "AMD FSR 4.1: {} lacks the FSR API entry points", path);
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    api->device_context = VkDeviceContext{
        .vkDevice = *device.GetLogical(),
        .vkPhysicalDevice = device.GetPhysical(),
        .vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr,
    };

    // The loader lists every upscaler this device can run, newest last or first depending on the
    // release: take the newest 4.x explicitly, so an FSR 3.1 provider never stands in silently.
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = &api->device_context;
    uint64_t count = 0;
    versions.outputCount = &count;
    if (api->query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0) {
        LOG_ERROR(Render_Vulkan, "AMD FSR 4.1: the loader lists no upscaler for this device");
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    if (api->query(nullptr, &versions.header) != FFX_API_RETURN_OK) {
        LOG_ERROR(Render_Vulkan, "AMD FSR 4.1: the upscaler versions could not be listed");
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    std::string listed;
    std::array<u32, 3> best{};
    for (uint64_t i = 0; i < count; ++i) {
        const std::string_view name = names[i] ? names[i] : "";
        listed += fmt::format("{}{}", listed.empty() ? "" : ", ", name);
        const auto parsed = ParseVersion(name);
        if (parsed[0] == 4 && parsed > best) {
            best = parsed;
            version_id = ids[i];
        }
    }
    if (!version_id) {
        LOG_WARNING(Render_Vulkan, "AMD FSR 4.1: no FSR 4 upscaler for this device (available: {})",
                    listed);
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    LOG_INFO(Render_Vulkan, "AMD FSR: upscalers {}; using {}.{}.{}", listed, best[0], best[1],
             best[2]);

    point_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    });
}

Fsr4Upscaler::~Fsr4Upscaler() {
    // As for DLSS: this runs when the renderer goes away, so wait for the device instead of
    // submitting through the scheduler.
    void(device.GetLogical().WaitIdle());
    if (context) {
        api->destroy(&context, nullptr);
        context = nullptr;
    }
}

void Fsr4Upscaler::DestroyContext() {
    if (!context) {
        return;
    }
    scheduler.Finish(); // recorded dispatches may still use it
    api->destroy(&context, nullptr);
    context = nullptr;
}

bool Fsr4Upscaler::CreateContext(const Inputs& in) {
    DestroyContext();
    auto& backend = api->backend;
    backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = api->device_context;
    auto& version = api->version;
    version = {};
    version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    version.versionId = *version_id;
    backend.header.pNext = &version.header;

    auto& upscale = api->upscale;
    upscale = {};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.header.pNext = &backend.header;
    // Tonemapped input (Ocarina of Time) is perceptual colour with its exposure applied.
    upscale.flags = in.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
                           : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    if (in.depth_inverted) {
        upscale.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    }
    upscale.maxRenderSize = {in.render_size.width, in.render_size.height};
    upscale.maxUpscaleSize = {in.upscale_size.width, in.upscale_size.height};
    upscale.fpMessage = &OnMessage;

    ffxContext created = nullptr;
    const ffxReturnCode_t result = api->create(&created, &upscale.header, nullptr);
    if (result != FFX_API_RETURN_OK || !created) {
        LOG_ERROR(Render_Vulkan, "AMD FSR 4.1: creating the upscaler failed ({})", result);
        return false;
    }
    context = created;
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const bool known = api->query(&context, &provider.header) == FFX_API_RETURN_OK &&
                       provider.versionName != nullptr;
    LOG_INFO(Render_Vulkan, "AMD FSR {}: {}x{} -> {}x{}, {}, depth {}",
             known ? provider.versionName : "?", in.render_size.width, in.render_size.height,
             in.upscale_size.width, in.upscale_size.height, in.hdr ? "HDR" : "LDR",
             in.depth_inverted ? "inverted" : "standard");
    return true;
}

void Fsr4Upscaler::CreateOutput(VkExtent2D upscale_size) {
    if (output_image) {
        scheduler.Finish(); // the old output may still be in use
    }
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .components{},
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               VkImageMemoryBarrier{
                                   .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .pNext = nullptr,
                                   .srcAccessMask = 0,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange{
                                       .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                       .baseMipLevel = 0,
                                       .levelCount = 1,
                                       .baseArrayLayer = 0,
                                       .layerCount = 1,
                                   },
                               });
    });
}

bool Fsr4Upscaler::Upscale(const Inputs& in) {
    if (failed) {
        return false;
    }
    const bool extent_changed = !output_image || in.render_size.width != render_extent.width ||
                                in.render_size.height != render_extent.height ||
                                in.upscale_size.width != upscale_extent.width ||
                                in.upscale_size.height != upscale_extent.height;
    const bool options_changed = extent_changed || !context || in.hdr != hdr_input ||
                                 in.depth_inverted != depth_inverted;
    const bool reset = in.reset || options_changed;
    if (extent_changed) {
        CreateOutput(in.upscale_size);
    }
    if (options_changed) {
        if (!CreateContext(in)) {
            failed = true;
            return false;
        }
        render_extent = in.render_size;
        upscale_extent = in.upscale_size;
        hdr_input = in.hdr;
        depth_inverted = in.depth_inverted;
    }
    const auto now = std::chrono::steady_clock::now();
    const f32 frame_ms =
        last_dispatch ? std::chrono::duration<f32, std::milli>(now - *last_dispatch).count()
                      : 1000.0f / 30.0f;
    last_dispatch = now;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, reset, frame_ms, output_image_handle = *output_image](
                         vk::CommandBuffer cmdbuf) {
        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        // The inputs stay in GENERAL otherwise; FSR reads them as sampled images.
        const std::array to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };
        const std::array to_general{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);

        ffxDispatchDescUpscale dispatch{};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList = *cmdbuf;
        dispatch.color = Resource(in.color.image, in.color.format, in.render_size,
                                  VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.depth = Resource(in.depth.image, in.depth.format, in.render_size,
                                  VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.motionVectors =
            Resource(in.motion_vectors.image, in.motion_vectors.format, in.render_size,
                     VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.output = Resource(output_image_handle, OUTPUT_FORMAT, in.upscale_size,
                                   OUTPUT_USAGE, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.jitterOffset = {in.jitter[0], in.jitter[1]};
        dispatch.motionVectorScale = {in.motion_vector_scale[0], in.motion_vector_scale[1]};
        dispatch.renderSize = {in.render_size.width, in.render_size.height};
        dispatch.upscaleSize = {in.upscale_size.width, in.upscale_size.height};
        dispatch.enableSharpening = false;
        dispatch.sharpness = 0.0f;
        dispatch.frameTimeDelta = frame_ms;
        dispatch.preExposure = 1.0f;
        dispatch.reset = reset;
        // The game hands no camera; FSR only uses these for its depth heuristics.
        dispatch.cameraNear = in.depth_inverted ? 1000.0f : 0.1f;
        dispatch.cameraFar = in.depth_inverted ? 0.1f : 1000.0f;
        dispatch.cameraFovAngleVertical = 1.0f;
        dispatch.viewSpaceToMetersFactor = 1.0f;
        dispatch.flags = in.hdr ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

        void* current = context;
        const ffxReturnCode_t result = api->dispatch(&current, &dispatch.header);

        const VkMemoryBarrier output_written{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, output_written, {},
                               to_general);
        if (result == FFX_API_RETURN_OK) {
            return;
        }
        if (!failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan, "AMD FSR 4.1: recording the upscale failed ({}); the game's "
                                     "upscale takes over",
                      result);
        }
        // The caller draws the output this frame: clear it rather than show undefined contents.
        const VkImageSubresourceRange range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        const auto barrier = [&](VkAccessFlags src_access, VkAccessFlags dst_access) {
            return VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = src_access,
                .dstAccessMask = dst_access,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = output_image_handle,
                .subresourceRange = range,
            };
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0,
                               barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                       VK_ACCESS_TRANSFER_WRITE_BIT));
        cmdbuf.ClearColorImage(output_image_handle, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{},
                               range);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
    });
    // FSR binds its own pipelines and descriptors on the command buffer.
    scheduler.InvalidateState();
    return true;
}

} // namespace Vulkan

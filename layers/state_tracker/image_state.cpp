/* Copyright (c) 2015-2025 The Khronos Group Inc.
 * Copyright (c) 2015-2025 Valve Corporation
 * Copyright (c) 2015-2025 LunarG, Inc.
 * Copyright (C) 2015-2024 Google Inc.
 * Modifications Copyright (C) 2020 Advanced Micro Devices, Inc. All rights reserved.
 * Modifications Copyright (C) 2022 RasterGrid Kft.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "state_tracker/image_state.h"
#include "state_tracker/pipeline_state.h"
#include "state_tracker/descriptor_sets.h"
#include "state_tracker/semaphore_state.h"
#include "state_tracker/shader_module.h"
#include "generated/dispatch_functions.h"

static VkExternalMemoryHandleTypeFlags GetExternalHandleTypes(const VkImageCreateInfo *pCreateInfo) {
    const auto *external_memory_info = vku::FindStructInPNextChain<VkExternalMemoryImageCreateInfo>(pCreateInfo->pNext);
    return external_memory_info ? external_memory_info->handleTypes : 0;
}

static VkSwapchainKHR GetSwapchain(const VkImageCreateInfo *pCreateInfo) {
    const auto *swapchain_info = vku::FindStructInPNextChain<VkImageSwapchainCreateInfoKHR>(pCreateInfo->pNext);
    return swapchain_info ? swapchain_info->swapchain : VK_NULL_HANDLE;
}

static vvl::Image::MemoryReqs GetMemoryRequirements(const vvl::DeviceState &dev_data, VkImage img,
                                                    const VkImageCreateInfo *create_info, bool disjoint, bool is_external_ahb) {
    vvl::Image::MemoryReqs result{};
    // Record the memory requirements in case they won't be queried
    // External AHB memory can't be queried until after memory is bound
    if (!is_external_ahb) {
        if (disjoint == false) {
            DispatchGetImageMemoryRequirements(dev_data.device, img, &result[0]);
        } else {
            uint32_t plane_count = vkuFormatPlaneCount(create_info->format);
            static const std::array<VkImageAspectFlagBits, 3> aspects{VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT,
                                                                      VK_IMAGE_ASPECT_PLANE_2_BIT};
            assert(plane_count <= aspects.size());
            VkImagePlaneMemoryRequirementsInfo image_plane_req = vku::InitStructHelper();
            VkImageMemoryRequirementsInfo2 mem_req_info2 = vku::InitStructHelper(&image_plane_req);
            mem_req_info2.image = img;

            for (uint32_t i = 0; i < plane_count; i++) {
                VkMemoryRequirements2 mem_reqs2 = vku::InitStructHelper();

                image_plane_req.planeAspect = aspects[i];
                switch (dev_data.extensions.vk_khr_get_memory_requirements2) {
                    case kEnabledByApiLevel:
                        DispatchGetImageMemoryRequirements2(dev_data.device, &mem_req_info2, &mem_reqs2);
                        break;
                    case kEnabledByCreateinfo:
                        DispatchGetImageMemoryRequirements2KHR(dev_data.device, &mem_req_info2, &mem_reqs2);
                        break;
                    default:
                        // The VK_KHR_sampler_ycbcr_conversion extension requires VK_KHR_get_memory_requirements2,
                        // so validation of this vkCreateImage call should have already failed.
                        assert(false);
                }
                result[i] = mem_reqs2.memoryRequirements;
            }
        }
    }
    return result;
}

static vvl::Image::SparseReqs GetSparseRequirements(const vvl::DeviceState &dev_data, VkImage img, bool sparse_residency) {
    vvl::Image::SparseReqs result;
    if (sparse_residency) {
        uint32_t count = 0;
        DispatchGetImageSparseMemoryRequirements(dev_data.device, img, &count, nullptr);
        result.resize(count);
        DispatchGetImageSparseMemoryRequirements(dev_data.device, img, &count, result.data());
    }
    return result;
}

static bool SparseMetaDataRequired(const vvl::Image::SparseReqs &sparse_reqs) {
    bool result = false;
    for (const auto &req : sparse_reqs) {
        if (req.formatProperties.aspectMask & VK_IMAGE_ASPECT_METADATA_BIT) {
            result = true;
            break;
        }
    }
    return result;
}
#ifdef VK_USE_PLATFORM_METAL_EXT
static bool GetMetalExport(const VkImageCreateInfo *info, VkExportMetalObjectTypeFlagBitsEXT object_type_required) {
    bool retval = false;
    auto export_metal_object_info = vku::FindStructInPNextChain<VkExportMetalObjectCreateInfoEXT>(info->pNext);
    while (export_metal_object_info) {
        if (export_metal_object_info->exportObjectType == object_type_required) {
            retval = true;
            break;
        }
        export_metal_object_info = vku::FindStructInPNextChain<VkExportMetalObjectCreateInfoEXT>(export_metal_object_info->pNext);
    }
    return retval;
}
#endif  // VK_USE_PLATFORM_METAL_EXT

namespace vvl {

Image::Image(const vvl::DeviceState &dev_data, VkImage img, const VkImageCreateInfo *pCreateInfo, VkFormatFeatureFlags2KHR ff)
    : Bindable(img, kVulkanObjectTypeImage, (pCreateInfo->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) != 0,
               (pCreateInfo->flags & VK_IMAGE_CREATE_PROTECTED_BIT) == 0, GetExternalHandleTypes(pCreateInfo)),
      safe_create_info(pCreateInfo),
      create_info(*safe_create_info.ptr()),
      shared_presentable(false),
      layout_locked(false),
      ahb_format(GetExternalFormat(pCreateInfo->pNext)),
      full_range{MakeImageFullRange()},
      create_from_swapchain(GetSwapchain(pCreateInfo)),
      owned_by_swapchain(false),
      swapchain_image_index(0),
      format_features(ff),
      disjoint((pCreateInfo->flags & VK_IMAGE_CREATE_DISJOINT_BIT) != 0),
      requirements(GetMemoryRequirements(dev_data, img, pCreateInfo, disjoint, IsExternalBuffer())),
      sparse_residency((pCreateInfo->flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT) != 0),
      sparse_requirements(GetSparseRequirements(dev_data, img, sparse_residency)),
      sparse_metadata_required(SparseMetaDataRequired(sparse_requirements)),
      get_sparse_reqs_called(false),
      sparse_metadata_bound(false),
#ifdef VK_USE_PLATFORM_METAL_EXT
      metal_image_export(GetMetalExport(pCreateInfo, VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT)),
      metal_io_surface_export(GetMetalExport(pCreateInfo, VK_EXPORT_METAL_OBJECT_TYPE_METAL_IOSURFACE_BIT_EXT)),
#endif  // VK_USE_PLATFORM_METAL_EXT
      subresource_encoder(full_range),
      store_device_as_workaround(dev_data.device),  // TODO REMOVE WHEN encoder can be const
      supported_video_profiles(dev_data.video_profile_cache_.Get(
          dev_data.physical_device, vku::FindStructInPNextChain<VkVideoProfileListInfoKHR>(pCreateInfo->pNext))) {
    if (pCreateInfo->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) {
        bool is_resident = (pCreateInfo->flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT) != 0;
        tracker_.emplace<BindableSparseMemoryTracker>(requirements.data(), is_resident);
        SetMemoryTracker(&std::get<BindableSparseMemoryTracker>(tracker_));
    } else if (pCreateInfo->flags & VK_IMAGE_CREATE_DISJOINT_BIT) {
        tracker_.emplace<BindableMultiplanarMemoryTracker>(requirements.data(), vkuFormatPlaneCount(pCreateInfo->format));
        SetMemoryTracker(&std::get<BindableMultiplanarMemoryTracker>(tracker_));
    } else {
        tracker_.emplace<BindableLinearMemoryTracker>(requirements.data());
        SetMemoryTracker(&std::get<BindableLinearMemoryTracker>(tracker_));
    }
}

Image::Image(const vvl::DeviceState &dev_data, VkImage img, const VkImageCreateInfo *pCreateInfo, VkSwapchainKHR swapchain,
             uint32_t swapchain_index, VkFormatFeatureFlags2KHR ff)
    : Bindable(img, kVulkanObjectTypeImage, (pCreateInfo->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) != 0,
               (pCreateInfo->flags & VK_IMAGE_CREATE_PROTECTED_BIT) == 0, GetExternalHandleTypes(pCreateInfo)),
      safe_create_info(pCreateInfo),
      create_info(*safe_create_info.ptr()),
      shared_presentable(false),
      layout_locked(false),
      ahb_format(GetExternalFormat(pCreateInfo->pNext)),
      full_range{MakeImageFullRange()},
      create_from_swapchain(swapchain),
      owned_by_swapchain(true),
      swapchain_image_index(swapchain_index),
      format_features(ff),
      disjoint((pCreateInfo->flags & VK_IMAGE_CREATE_DISJOINT_BIT) != 0),
      requirements{},
      sparse_residency(false),
      sparse_requirements{},
      sparse_metadata_required(false),
      get_sparse_reqs_called(false),
      sparse_metadata_bound(false),
#ifdef VK_USE_PLATFORM_METAL_EXT
      metal_image_export(GetMetalExport(pCreateInfo, VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT)),
      metal_io_surface_export(GetMetalExport(pCreateInfo, VK_EXPORT_METAL_OBJECT_TYPE_METAL_IOSURFACE_BIT_EXT)),
#endif  // VK_USE_PLATFORM_METAL_EXT
      subresource_encoder(full_range),
      store_device_as_workaround(dev_data.device),  // TODO REMOVE WHEN encoder can be const
      supported_video_profiles(dev_data.video_profile_cache_.Get(
          dev_data.physical_device, vku::FindStructInPNextChain<VkVideoProfileListInfoKHR>(pCreateInfo->pNext))) {

    tracker_.emplace<BindableNoMemoryTracker>(requirements.data());
    SetMemoryTracker(&std::get<BindableNoMemoryTracker>(tracker_));
}

void Image::Destroy() {
    for (auto &item : sub_states_) {
        item.second->Destroy();
    }
    // NOTE: due to corner cases in aliased images, the layout_range_map MUST not be cleaned up here.
    // If it is, bad local entries could be created by vvl::CommandBuffer::GetOrCreateImageLayoutRegistry()
    // If an aliasing image was being destroyed (and layout_range_map was reset()), a nullptr keyed
    // entry could get put into vvl::CommandBuffer::aliased_image_layout_map.
    //
    if (bind_swapchain) {
        bind_swapchain->RemoveParent(this);
        bind_swapchain = nullptr;
    }
    Bindable::Destroy();
}

void Image::NotifyInvalidate(const StateObject::NodeList &invalid_nodes, bool unlink) {
    for (auto &item : sub_states_) {
        item.second->NotifyInvalidate(invalid_nodes, unlink);
    }
    Bindable::NotifyInvalidate(invalid_nodes, unlink);
    if (unlink) {
        bind_swapchain = nullptr;
    }
}

bool Image::IsCreateInfoEqual(const VkImageCreateInfo &other_create_info) const {
    bool is_equal = (create_info.sType == other_create_info.sType) && (create_info.flags == other_create_info.flags);
    is_equal = is_equal && IsImageTypeEqual(other_create_info) && IsFormatEqual(other_create_info);
    is_equal = is_equal && IsMipLevelsEqual(other_create_info) && IsArrayLayersEqual(other_create_info);
    is_equal = is_equal && IsUsageEqual(other_create_info) && IsInitialLayoutEqual(other_create_info);
    is_equal = is_equal && IsExtentEqual(other_create_info) && IsTilingEqual(other_create_info);
    is_equal = is_equal && IsSamplesEqual(other_create_info) && IsSharingModeEqual(other_create_info);
    return is_equal &&
           ((create_info.sharingMode == VK_SHARING_MODE_CONCURRENT) ? IsQueueFamilyIndicesEqual(other_create_info) : true);
}

// Check image compatibility rules for VK_NV_dedicated_allocation_image_aliasing
bool Image::IsCreateInfoDedicatedAllocationImageAliasingCompatible(const VkImageCreateInfo &other_create_info) const {
    bool is_compatible = (create_info.sType == other_create_info.sType) && (create_info.flags == other_create_info.flags);
    is_compatible = is_compatible && IsImageTypeEqual(other_create_info) && IsFormatEqual(other_create_info);
    is_compatible = is_compatible && IsMipLevelsEqual(other_create_info);
    is_compatible = is_compatible && IsUsageEqual(other_create_info) && IsInitialLayoutEqual(other_create_info);
    is_compatible = is_compatible && IsSamplesEqual(other_create_info) && IsSharingModeEqual(other_create_info);
    is_compatible = is_compatible &&
                    ((create_info.sharingMode == VK_SHARING_MODE_CONCURRENT) ? IsQueueFamilyIndicesEqual(other_create_info) : true);
    is_compatible = is_compatible && IsTilingEqual(other_create_info);

    is_compatible = is_compatible && create_info.extent.width <= other_create_info.extent.width &&
                    create_info.extent.height <= other_create_info.extent.height &&
                    create_info.extent.depth <= other_create_info.extent.depth &&
                    create_info.arrayLayers <= other_create_info.arrayLayers;
    return is_compatible;
}

bool Image::IsCompatibleAliasing(const Image *other_image_state) const {
    if (!IsSwapchainImage() && !other_image_state->IsSwapchainImage() &&
        !(create_info.flags & other_image_state->create_info.flags & VK_IMAGE_CREATE_ALIAS_BIT)) {
        return false;
    }
    const auto binding = Binding();
    const auto other_binding = other_image_state->Binding();
    if ((create_from_swapchain == VK_NULL_HANDLE) && binding && other_binding &&
        (binding->memory_state == other_binding->memory_state) && (binding->memory_offset == other_binding->memory_offset) &&
        IsCreateInfoEqual(other_image_state->create_info)) {
        return true;
    }
    if (bind_swapchain && (bind_swapchain == other_image_state->bind_swapchain) &&
        (swapchain_image_index == other_image_state->swapchain_image_index)) {
        return true;
    }
    return false;
}

VkImageSubresourceRange Image::NormalizeSubresourceRange(const VkImageSubresourceRange &range) const {
    VkImageSubresourceRange norm = range;
    norm.levelCount =
        (range.levelCount == VK_REMAINING_MIP_LEVELS) ? (create_info.mipLevels - range.baseMipLevel) : range.levelCount;
    norm.layerCount =
        (range.layerCount == VK_REMAINING_ARRAY_LAYERS) ? (create_info.arrayLayers - range.baseArrayLayer) : range.layerCount;

    // For multiplanar formats, IMAGE_ASPECT_COLOR is equivalent to adding the aspect of the individual planes
    if (vkuFormatIsMultiplane(create_info.format)) {
        if (norm.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
            norm.aspectMask &= ~VK_IMAGE_ASPECT_COLOR_BIT;
            norm.aspectMask |= (VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT);
            if (vkuFormatPlaneCount(create_info.format) > 2) {
                norm.aspectMask |= VK_IMAGE_ASPECT_PLANE_2_BIT;
            }
        }
    }
    return norm;
}

uint32_t Image::NormalizeLayerCount(const VkImageSubresourceLayers &resource) const {
    return (resource.layerCount == VK_REMAINING_ARRAY_LAYERS) ? (create_info.arrayLayers - resource.baseArrayLayer)
                                                              : resource.layerCount;
}

VkImageSubresourceRange Image::MakeImageFullRange() {
    const auto format = create_info.format;
    VkImageSubresourceRange init_range{0, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};

    if (vkuFormatIsColor(format) || vkuFormatIsMultiplane(format) || GetExternalFormat(create_info.pNext) != 0) {
        init_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;  // Normalization will expand this for multiplane
    } else {
        init_range.aspectMask = (vkuFormatHasDepth(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                (vkuFormatHasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    }
    return NormalizeSubresourceRange(init_range);
}

void Image::SetInitialLayoutMap() {
    if (layout_range_map) {
        return;
    }

    std::shared_ptr<GlobalImageLayoutRangeMap> layout_map;
    auto get_layout_map = [&layout_map](const Image &other_image) {
        layout_map = other_image.layout_range_map;
        return true;
    };

    // See if an alias already has a layout map
    if (HasAliasFlag()) {
        AnyImageAliasOf(get_layout_map);
    } else if (bind_swapchain) {
        // Swapchains can also alias if multiple images are bound (or retrieved
        // with vkGetSwapchainImages()) for a (single swapchain, index) pair.
        AnyAliasBindingOf(bind_swapchain->ObjectBindings(), get_layout_map);
    }

    if (!layout_map) {
        // otherwise set up a new map.
        // set up the new map completely before making it available
        layout_map = std::make_shared<GlobalImageLayoutRangeMap>(subresource_encoder.SubresourceCount());
        auto range_gen = subresource_adapter::RangeGenerator(subresource_encoder);
        for (; range_gen->non_empty(); ++range_gen) {
            layout_map->insert(layout_map->end(), std::make_pair(*range_gen, create_info.initialLayout));
        }
    }
    // And store in the object
    layout_range_map = std::move(layout_map);
}

void Image::SetImageLayout(const VkImageSubresourceRange &range, VkImageLayout layout) {
    using sparse_container::update_range_value;
    using sparse_container::value_precedence;
    GlobalImageLayoutRangeMap::RangeGenerator range_gen(subresource_encoder, NormalizeSubresourceRange(range));
    auto guard = layout_range_map->WriteLock();
    for (; range_gen->non_empty(); ++range_gen) {
        update_range_value(*layout_range_map, *range_gen, layout, value_precedence::prefer_source);
    }
}

void Image::SetSwapchain(std::shared_ptr<vvl::Swapchain> &swapchain, uint32_t swapchain_index) {
    assert(IsSwapchainImage());
    bind_swapchain = swapchain;
    swapchain_image_index = swapchain_index;
    bind_swapchain->AddParent(this);
}

bool Image::CompareCreateInfo(const Image &other) const {
    bool valid_queue_family = true;
    if (create_info.sharingMode == VK_SHARING_MODE_CONCURRENT) {
        if (create_info.queueFamilyIndexCount != other.create_info.queueFamilyIndexCount) {
            valid_queue_family = false;
        } else {
            for (uint32_t i = 0; i < create_info.queueFamilyIndexCount; i++) {
                if (create_info.pQueueFamilyIndices[i] != other.create_info.pQueueFamilyIndices[i]) {
                    valid_queue_family = false;
                    break;
                }
            }
        }
    }

    // There are limitations what actually needs to be compared, so for simplicity (until found otherwise needed), we only need to
    // check the ExternalHandleType and not other pNext chains
    const bool valid_external = GetExternalHandleTypes(&create_info) == GetExternalHandleTypes(&other.create_info);

    return (create_info.flags == other.create_info.flags) && (create_info.imageType == other.create_info.imageType) &&
           (create_info.format == other.create_info.format) && (create_info.extent.width == other.create_info.extent.width) &&
           (create_info.extent.height == other.create_info.extent.height) &&
           (create_info.extent.depth == other.create_info.extent.depth) && (create_info.mipLevels == other.create_info.mipLevels) &&
           (create_info.arrayLayers == other.create_info.arrayLayers) && (create_info.samples == other.create_info.samples) &&
           (create_info.tiling == other.create_info.tiling) && (create_info.usage == other.create_info.usage) &&
           (create_info.initialLayout == other.create_info.initialLayout) && valid_queue_family && valid_external;
}

}  // namespace vvl

static VkSamplerYcbcrConversion GetSamplerConversion(const VkImageViewCreateInfo *ci) {
    auto *conversion_info = vku::FindStructInPNextChain<VkSamplerYcbcrConversionInfo>(ci->pNext);
    return conversion_info ? conversion_info->conversion : VK_NULL_HANDLE;
}

static VkImageUsageFlags GetInheritedUsage(const VkImageViewCreateInfo *ci, const vvl::Image &image_state) {
    auto usage_create_info = vku::FindStructInPNextChain<VkImageViewUsageCreateInfo>(ci->pNext);
    return (usage_create_info) ? usage_create_info->usage : image_state.create_info.usage;
}

static float GetImageViewMinLod(const VkImageViewCreateInfo *ci) {
    auto image_view_min_lod = vku::FindStructInPNextChain<VkImageViewMinLodCreateInfoEXT>(ci->pNext);
    return (image_view_min_lod) ? image_view_min_lod->minLod : 0.0f;
}

#ifdef VK_USE_PLATFORM_METAL_EXT
static bool GetMetalExport(const VkImageViewCreateInfo *info) {
    bool retval = false;
    auto export_metal_object_info = vku::FindStructInPNextChain<VkExportMetalObjectCreateInfoEXT>(info->pNext);
    while (export_metal_object_info) {
        if (export_metal_object_info->exportObjectType == VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT) {
            retval = true;
            break;
        }
        export_metal_object_info = vku::FindStructInPNextChain<VkExportMetalObjectCreateInfoEXT>(export_metal_object_info->pNext);
    }
    return retval;
}
#endif  // VK_USE_PLATFORM_METAL_EXT

namespace vvl {

ImageView::ImageView(const std::shared_ptr<vvl::Image> &image_state, VkImageView handle, const VkImageViewCreateInfo *ci,
                     VkFormatFeatureFlags2KHR ff, const VkFilterCubicImageViewImageFormatPropertiesEXT &cubic_props)
    : StateObject(handle, kVulkanObjectTypeImageView),
      safe_create_info(ci),
      create_info(*safe_create_info.ptr()),
      image_state(image_state),
#ifdef VK_USE_PLATFORM_METAL_EXT
      metal_imageview_export(GetMetalExport(ci)),
#endif
      is_depth_sliced(IsDepthSliced()),
      normalized_subresource_range(NormalizeSubresourceRange()),
      range_generator(image_state->subresource_encoder, normalized_subresource_range),
      samples(image_state->create_info.samples),
      samplerConversion(GetSamplerConversion(ci)),
      filter_cubic_props(cubic_props),
      min_lod(GetImageViewMinLod(ci)),
      format_features(ff),
      inherited_usage(GetInheritedUsage(ci, *image_state)) {
}

void ImageView::NotifyInvalidate(const StateObject::NodeList &invalid_nodes, bool unlink) {
    for (auto &item : sub_states_) {
        item.second->NotifyInvalidate(invalid_nodes, unlink);
    }
    StateObject::NotifyInvalidate(invalid_nodes, unlink);
}

void ImageView::Destroy() {
    for (auto &item : sub_states_) {
        item.second->Destroy();
    }
    if (image_state) {
        image_state->RemoveParent(this);
        image_state = nullptr;
    }
    StateObject::Destroy();
}

bool ImageView::IsDepthSliced() {
    auto depth_slice_flag = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT | VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT;
    return ((image_state->create_info.flags & depth_slice_flag) != 0) &&
           (create_info.viewType == VK_IMAGE_VIEW_TYPE_2D || create_info.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
}

VkImageSubresourceRange ImageView::NormalizeSubresourceRange() const {
    auto subres_range = create_info.subresourceRange;

    // if we're mapping a 3D image to a 2d image view, convert the view's subresource range to be compatible with the
    // image's understanding of the world. From the VkImageSubresourceRange section of the Vulkan spec:
    //
    //     When the VkImageSubresourceRange structure is used to select a subset of the slices of a 3D image’s mip level in
    //     order to create a 2D or 2D array image view of a 3D image created with VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT,
    //     baseArrayLayer and layerCount specify the first slice index and the number of slices to include in the created
    //     image view. Such an image view can be used as a framebuffer attachment that refers only to the specified range
    //     of slices of the selected mip level. However, any layout transitions performed on such an attachment view during
    //     a render pass instance still apply to the entire subresource referenced which includes all the slices of the
    //     selected mip level.
    //
    if (is_depth_sliced) {
        subres_range.baseArrayLayer = 0;
        subres_range.layerCount = 1;
    }
    return image_state->NormalizeSubresourceRange(subres_range);
}

uint32_t ImageView::GetAttachmentLayerCount() const {
    if (create_info.subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS && !is_depth_sliced) {
        return image_state->create_info.arrayLayers;
    }
    return create_info.subresourceRange.layerCount;
}

bool ImageView::OverlapSubresource(const ImageView &compare_view) const {
    if (VkHandle() == compare_view.VkHandle()) {
        return true;
    }
    if (image_state->VkHandle() != compare_view.image_state->VkHandle()) {
        return false;
    }
    if (normalized_subresource_range.aspectMask != compare_view.normalized_subresource_range.aspectMask) {
        return false;
    }

    // compare if overlap mip level
    if ((normalized_subresource_range.baseMipLevel < compare_view.normalized_subresource_range.baseMipLevel) &&
        ((normalized_subresource_range.baseMipLevel + normalized_subresource_range.levelCount) <=
         compare_view.normalized_subresource_range.baseMipLevel)) {
        return false;
    }

    if ((normalized_subresource_range.baseMipLevel > compare_view.normalized_subresource_range.baseMipLevel) &&
        (normalized_subresource_range.baseMipLevel >=
         (compare_view.normalized_subresource_range.baseMipLevel + compare_view.normalized_subresource_range.levelCount))) {
        return false;
    }

    // compare if overlap array layer
    if ((normalized_subresource_range.baseArrayLayer < compare_view.normalized_subresource_range.baseArrayLayer) &&
        ((normalized_subresource_range.baseArrayLayer + normalized_subresource_range.layerCount) <=
         compare_view.normalized_subresource_range.baseArrayLayer)) {
        return false;
    }

    if ((normalized_subresource_range.baseArrayLayer > compare_view.normalized_subresource_range.baseArrayLayer) &&
        (normalized_subresource_range.baseArrayLayer >=
         (compare_view.normalized_subresource_range.baseArrayLayer + compare_view.normalized_subresource_range.layerCount))) {
        return false;
    }
    return true;
}

}  // namespace vvl

static vku::safe_VkImageCreateInfo GetImageCreateInfo(const VkSwapchainCreateInfoKHR *pCreateInfo) {
    VkImageCreateInfo image_ci = vku::InitStructHelper();
    // Pull out the format list only. This stack variable will get copied onto the heap
    // by the 'safe' constructor used to build the return value below.
    VkImageFormatListCreateInfo fmt_info;
    auto chain_fmt_info = vku::FindStructInPNextChain<VkImageFormatListCreateInfo>(pCreateInfo->pNext);
    if (chain_fmt_info) {
        fmt_info = *chain_fmt_info;
        fmt_info.pNext = nullptr;
        image_ci.pNext = &fmt_info;
    } else {
        image_ci.pNext = nullptr;
    }
    image_ci.flags = 0;  // to be updated below
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = pCreateInfo->imageFormat;
    image_ci.extent.width = pCreateInfo->imageExtent.width;
    image_ci.extent.height = pCreateInfo->imageExtent.height;
    image_ci.extent.depth = 1;
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = pCreateInfo->imageArrayLayers;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = pCreateInfo->imageUsage;
    image_ci.sharingMode = pCreateInfo->imageSharingMode;
    image_ci.queueFamilyIndexCount = pCreateInfo->queueFamilyIndexCount;
    image_ci.pQueueFamilyIndices = pCreateInfo->pQueueFamilyIndices;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (pCreateInfo->flags & VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR) {
        image_ci.flags |= VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT;
    }
    if (pCreateInfo->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) {
        image_ci.flags |= VK_IMAGE_CREATE_PROTECTED_BIT;
    }
    if (pCreateInfo->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
        image_ci.flags |= (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
    }
    return vku::safe_VkImageCreateInfo(&image_ci);
}

namespace vvl {

void SwapchainImage::ResetPresentWaitSemaphores() {
    for (auto &semaphore : present_wait_semaphores) {
        semaphore->ClearSwapchainWaitInfo();
    }
    present_wait_semaphores.clear();
}

Swapchain::Swapchain(vvl::DeviceState &dev_data_, const VkSwapchainCreateInfoKHR *pCreateInfo, VkSwapchainKHR handle)
    : StateObject(handle, kVulkanObjectTypeSwapchainKHR),
      safe_create_info(pCreateInfo),
      create_info(*safe_create_info.ptr()),
      images(),
      exclusive_full_screen_access(false),
      shared_presentable(VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR == pCreateInfo->presentMode ||
                         VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR == pCreateInfo->presentMode),
      image_create_info(GetImageCreateInfo(pCreateInfo)),
      dev_data(dev_data_) {

    // Initialize with visible values for debugging purposes.
    // This helps to show used slots during the first few frames.
    acquire_history.fill(vvl::kU32Max);
}

void Swapchain::PresentImage(uint32_t image_index, uint64_t present_id, const SubmissionReference &present_submission_ref,
                             vvl::span<std::shared_ptr<vvl::Semaphore>> present_wait_semaphores) {
    if (image_index >= images.size()) return;
    assert(acquired_images > 0);
    if (!shared_presentable) {
        acquired_images--;
        images[image_index].acquired = false;
        images[image_index].acquire_semaphore.reset();
        images[image_index].acquire_fence.reset();
    } else {
        images[image_index].image_state->layout_locked = true;
    }
    images[image_index].present_submission_ref = present_submission_ref;

    images[image_index].present_wait_semaphores.clear();
    for (const auto &semaphore : present_wait_semaphores) {
        images[image_index].present_wait_semaphores.emplace_back(semaphore);
    }

    if (present_id > max_present_id) {
        max_present_id = present_id;
    }
}

void Swapchain::ReleaseImage(uint32_t image_index) {
    if (image_index >= images.size()) return;
    assert(acquired_images > 0);
    acquired_images--;
    images[image_index].acquired = false;
    images[image_index].acquire_semaphore.reset();
    images[image_index].acquire_fence.reset();
    images[image_index].ResetPresentWaitSemaphores();
}

void Swapchain::AcquireImage(uint32_t image_index, const std::shared_ptr<vvl::Semaphore> &semaphore_state,
                             const std::shared_ptr<vvl::Fence> &fence_state) {
    acquired_images++;
    images[image_index].acquired = true;
    images[image_index].acquire_semaphore = semaphore_state;
    images[image_index].acquire_fence = fence_state;
    if (fence_state && images[image_index].present_submission_ref.has_value()) {
        fence_state->SetPresentSubmissionRef(*images[image_index].present_submission_ref);
        images[image_index].present_submission_ref.reset();
    }
    if (shared_presentable) {
        images[image_index].image_state->shared_presentable = shared_presentable;
    }
    images[image_index].ResetPresentWaitSemaphores();

    acquire_history[acquire_count % acquire_history_max_length] = image_index;
    acquire_count++;
}

void Swapchain::Destroy() {
    for (auto &swapchain_image : images) {
        swapchain_image.ResetPresentWaitSemaphores();
        RemoveParent(swapchain_image.image_state);
        dev_data.Destroy<vvl::Image>(swapchain_image.image_state->VkHandle());
        // NOTE: We don't have access to dev_data.fake_memory.Free() here, but it is currently a no-op
    }
    images.clear();
    if (surface) {
        surface->RemoveParent(this);
        surface = nullptr;
    }
    StateObject::Destroy();
}

void Swapchain::NotifyInvalidate(const StateObject::NodeList &invalid_nodes, bool unlink) {
    StateObject::NotifyInvalidate(invalid_nodes, unlink);
    if (unlink) {
        surface = nullptr;
    }
}

SwapchainImage Swapchain::GetSwapChainImage(uint32_t index) const {
    if (index < images.size()) {
        return images[index];
    }
    return SwapchainImage();
}

std::shared_ptr<const vvl::Image> Swapchain::GetSwapChainImageShared(uint32_t index) const {
    const SwapchainImage swapchain_image(GetSwapChainImage(index));
    if (swapchain_image.image_state) {
        return swapchain_image.image_state->shared_from_this();
    }
    return std::shared_ptr<const vvl::Image>();
}

uint32_t Swapchain::GetAcquireHistoryLength() const {
    return (acquire_count >= acquire_history_max_length) ? acquire_history_max_length : acquire_count;
}

uint32_t Swapchain::GetAcquiredImageIndexFromHistory(uint32_t acquire_history_index) const {
    const uint32_t history_length = GetAcquireHistoryLength();
    assert(acquire_history_index < history_length);

    const uint32_t global_start_index = acquire_count - history_length;
    const uint32_t global_index = global_start_index + acquire_history_index;
    const uint32_t ring_buffer_index = global_index % acquire_history_max_length;

    const uint32_t acquire_image_index = acquire_history[ring_buffer_index];
    assert(acquire_image_index != vvl::kU32Max);
    return acquire_image_index;
}

void Surface::Destroy() {
    if (swapchain) {
        swapchain = nullptr;
    }
    StateObject::Destroy();
}

void Surface::RemoveParent(StateObject *parent_node) {
    if (swapchain == parent_node) {
        swapchain = nullptr;
    }
    StateObject::RemoveParent(parent_node);
}

void Surface::SetQueueSupport(VkPhysicalDevice phys_dev, uint32_t qfi, bool supported) {
    auto guard = Lock();
    assert(phys_dev);
    GpuQueue key{phys_dev, qfi};
    gpu_queue_support_[key] = supported;
}

bool Surface::GetQueueSupport(VkPhysicalDevice phys_dev, uint32_t qfi) const {
    auto guard = Lock();
    assert(phys_dev);
    GpuQueue key{phys_dev, qfi};
    auto iter = gpu_queue_support_.find(key);
    if (iter != gpu_queue_support_.end()) {
        return iter->second;
    }
    VkBool32 supported = VK_FALSE;
    DispatchGetPhysicalDeviceSurfaceSupportKHR(phys_dev, qfi, VkHandle(), &supported);
    gpu_queue_support_[key] = (supported == VK_TRUE);
    return supported == VK_TRUE;
}

// Save data from vkGetPhysicalDeviceSurfacePresentModes
void Surface::SetPresentModes(VkPhysicalDevice phys_dev, vvl::span<const VkPresentModeKHR> modes) {
    auto guard = Lock();
    cache_[phys_dev].present_modes.emplace(modes.begin(), modes.end());
}

// Helper for data obtained from vkGetPhysicalDeviceSurfacePresentModesKHR
std::vector<VkPresentModeKHR> Surface::GetPresentModes(VkPhysicalDevice phys_dev) const {
    if (auto guard = Lock(); auto cache = GetPhysDevCache(phys_dev)) {
        if (cache->present_modes.has_value()) {
            return cache->present_modes.value();
        }
    }
    uint32_t count = 0;
    if (DispatchGetPhysicalDeviceSurfacePresentModesKHR(phys_dev, VkHandle(), &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkPresentModeKHR> present_modes(count);
    if (DispatchGetPhysicalDeviceSurfacePresentModesKHR(phys_dev, VkHandle(), &count, present_modes.data()) != VK_SUCCESS) {
        return {};
    }
    return present_modes;
}

void Surface::SetFormats(VkPhysicalDevice phys_dev, std::vector<vku::safe_VkSurfaceFormat2KHR> &&fmts) {
    auto guard = Lock();
    assert(phys_dev);
    formats_[phys_dev] = std::move(fmts);
}

vvl::span<const vku::safe_VkSurfaceFormat2KHR> Surface::GetFormats(bool get_surface_capabilities2, VkPhysicalDevice phys_dev,
                                                                   const void *surface_info2_pnext) const {
    auto guard = Lock();

    // TODO: BUG: format also depends on pNext. Rework this function similar to GetSurfaceCapabilities
    if (const auto search = formats_.find(phys_dev); search != formats_.end()) {
        vvl::span<const vku::safe_VkSurfaceFormat2KHR>(search->second);
    }

    std::vector<vku::safe_VkSurfaceFormat2KHR> result;
    if (get_surface_capabilities2) {
        VkPhysicalDeviceSurfaceInfo2KHR surface_info2 = vku::InitStructHelper();
        surface_info2.pNext = surface_info2_pnext;
        surface_info2.surface = VkHandle();
        uint32_t count = 0;
        if (DispatchGetPhysicalDeviceSurfaceFormats2KHR(phys_dev, &surface_info2, &count, nullptr) != VK_SUCCESS) {
            return {};
        }
        std::vector<VkSurfaceFormat2KHR> formats2(count, vku::InitStruct<VkSurfaceFormat2KHR>());

        if (DispatchGetPhysicalDeviceSurfaceFormats2KHR(phys_dev, &surface_info2, &count, formats2.data()) != VK_SUCCESS) {
            result.clear();
        } else {
            result.resize(count);
            for (uint32_t surface_format_index = 0; surface_format_index < count; ++surface_format_index) {
                result.emplace_back(&formats2[surface_format_index]);
            }
        }
    } else {
        std::vector<VkSurfaceFormatKHR> formats;
        uint32_t count = 0;
        if (DispatchGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, VkHandle(), &count, nullptr) != VK_SUCCESS) {
            return {};
        }
        formats.resize(count);

        if (DispatchGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, VkHandle(), &count, formats.data()) != VK_SUCCESS) {
            result.clear();
        } else {
            result.reserve(count);
            VkSurfaceFormat2KHR format2 = vku::InitStructHelper();
            for (const auto &format : formats) {
                format2.surfaceFormat = format;
                result.emplace_back(&format2);
            }
        }
    }
    formats_[phys_dev] = std::move(result);
    return vvl::span<const vku::safe_VkSurfaceFormat2KHR>(formats_[phys_dev]);
}

const Surface::PresentModeInfo *Surface::PhysDevCache::GetPresentModeInfo(VkPresentModeKHR present_mode) const {
    for (auto &info : present_mode_infos) {
        if (info.present_mode == present_mode) {
            return &info;
        }
    }
    return nullptr;
}

const Surface::PhysDevCache *Surface::GetPhysDevCache(VkPhysicalDevice phys_dev) const {
    auto it = cache_.find(phys_dev);
    return (it == cache_.end()) ? nullptr : &it->second;
}

void Surface::UpdateCapabilitiesCache(VkPhysicalDevice phys_dev, const VkSurfaceCapabilitiesKHR &surface_caps) {
    auto guard = Lock();
    PhysDevCache &cache = cache_[phys_dev];
    cache.capabilities = surface_caps;
    cache.last_capability_query_used_present_mode = false;
}

void Surface::UpdateCapabilitiesCache(VkPhysicalDevice phys_dev, const VkSurfaceCapabilities2KHR &surface_caps,
                                      VkPresentModeKHR present_mode) {
    auto guard = Lock();
    auto &cache = cache_[phys_dev];

    // Get entry for the given presentation mode
    PresentModeInfo *info = nullptr;
    for (auto &cur_info : cache.present_mode_infos) {
        if (cur_info.present_mode == present_mode) {
            info = &cur_info;
            break;
        }
    }
    if (!info) {
        cache.present_mode_infos.emplace_back(PresentModeInfo{});
        info = &cache.present_mode_infos.back();
        info->present_mode = present_mode;
    }

    // Update entry
    info->surface_capabilities = surface_caps.surfaceCapabilities;
    const auto *present_scaling_caps = vku::FindStructInPNextChain<VkSurfacePresentScalingCapabilitiesEXT>(surface_caps.pNext);
    if (present_scaling_caps) {
        info->scaling_capabilities = *present_scaling_caps;
    }
    const auto *compat_modes = vku::FindStructInPNextChain<VkSurfacePresentModeCompatibilityEXT>(surface_caps.pNext);
    if (compat_modes && compat_modes->pPresentModes) {
        info->compatible_present_modes.emplace(compat_modes->pPresentModes,
                                               compat_modes->pPresentModes + compat_modes->presentModeCount);
    }
    cache.last_capability_query_used_present_mode = true;
}

bool Surface::IsLastCapabilityQueryUsedPresentMode(VkPhysicalDevice phys_dev) const {
    if (auto guard = Lock(); auto cache = GetPhysDevCache(phys_dev)) {
        return cache->last_capability_query_used_present_mode;
    }
    return false;
}

VkSurfaceCapabilitiesKHR Surface::GetSurfaceCapabilities(VkPhysicalDevice phys_dev, const void *surface_info_pnext) const {
    if (!surface_info_pnext) {
        if (auto guard = Lock(); auto cache = GetPhysDevCache(phys_dev)) {
            if (cache->capabilities.has_value()) {
                return cache->capabilities.value();
            }
        }
        VkSurfaceCapabilitiesKHR surface_caps{};
        DispatchGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_dev, VkHandle(), &surface_caps);
        return surface_caps;
    }

    // Per present mode caching is supported for a common case when pNext chain is a single VkSurfacePresentModeEXT structure.
    const auto *surface_present_mode = vku::FindStructInPNextChain<VkSurfacePresentModeEXT>(surface_info_pnext);
    const bool single_pnext_element = static_cast<const VkBaseInStructure *>(surface_info_pnext)->pNext == nullptr;
    if (surface_present_mode && single_pnext_element) {
        if (auto guard = Lock(); auto cache = GetPhysDevCache(phys_dev)) {
            const PresentModeInfo *info = cache->GetPresentModeInfo(surface_present_mode->presentMode);
            if (info) {
                return info->surface_capabilities;
            }
        }
    }
    VkPhysicalDeviceSurfaceInfo2KHR surface_info = vku::InitStructHelper();
    surface_info.pNext = surface_info_pnext;
    surface_info.surface = VkHandle();
    VkSurfaceCapabilities2KHR surface_caps = vku::InitStructHelper();
    DispatchGetPhysicalDeviceSurfaceCapabilities2KHR(phys_dev, &surface_info, &surface_caps);
    return surface_caps.surfaceCapabilities;
}

VkSurfaceCapabilitiesKHR Surface::GetPresentModeSurfaceCapabilities(VkPhysicalDevice phys_dev,
                                                                    VkPresentModeKHR present_mode) const {
    VkSurfacePresentModeEXT surface_present_mode = vku::InitStructHelper();
    surface_present_mode.presentMode = present_mode;
    return GetSurfaceCapabilities(phys_dev, &surface_present_mode);
}

VkSurfacePresentScalingCapabilitiesEXT Surface::GetPresentModeScalingCapabilities(VkPhysicalDevice phys_dev,
                                                                                  VkPresentModeKHR present_mode) const {
    if (auto guard = Lock(); auto cache = GetPhysDevCache(phys_dev)) {
        const PresentModeInfo *info = cache->GetPresentModeInfo(present_mode);
        if (info && info->scaling_capabilities.has_value()) {
            return info->scaling_capabilities.value();
        }
    }
    VkSurfacePresentModeEXT surface_present_mode = vku::InitStructHelper();
    surface_present_mode.presentMode = present_mode;
    VkPhysicalDeviceSurfaceInfo2KHR surface_info = vku::InitStructHelper(&surface_present_mode);
    surface_info.surface = VkHandle();
    VkSurfacePresentScalingCapabilitiesEXT scaling_caps = vku::InitStructHelper();
    VkSurfaceCapabilities2KHR surface_caps = vku::InitStructHelper(&scaling_caps);
    DispatchGetPhysicalDeviceSurfaceCapabilities2KHR(phys_dev, &surface_info, &surface_caps);
    return scaling_caps;
}

std::vector<VkPresentModeKHR> Surface::GetCompatibleModes(VkPhysicalDevice phys_dev, VkPresentModeKHR present_mode) const {
    if (auto guard = Lock(); auto cache = GetPhysDevCache(phys_dev)) {
        const PresentModeInfo *info = cache->GetPresentModeInfo(present_mode);
        if (info && info->compatible_present_modes.has_value()) {
            return info->compatible_present_modes.value();
        }
    }
    VkSurfacePresentModeEXT surface_present_mode = vku::InitStructHelper();
    surface_present_mode.presentMode = present_mode;
    VkPhysicalDeviceSurfaceInfo2KHR surface_info = vku::InitStructHelper(&surface_present_mode);
    surface_info.surface = VkHandle();
    VkSurfacePresentModeCompatibilityEXT present_mode_compat = vku::InitStructHelper();
    VkSurfaceCapabilities2KHR surface_caps = vku::InitStructHelper(&present_mode_compat);
    DispatchGetPhysicalDeviceSurfaceCapabilities2KHR(phys_dev, &surface_info, &surface_caps);
    std::vector<VkPresentModeKHR> present_modes(present_mode_compat.presentModeCount);
    present_mode_compat.pPresentModes = present_modes.data();
    DispatchGetPhysicalDeviceSurfaceCapabilities2KHR(phys_dev, &surface_info, &surface_caps);
    return present_modes;
}

}  // namespace vvl

bool GlobalImageLayoutRangeMap::AnyInRange(RangeGenerator &gen,
                                           std::function<bool(const key_type &range, const mapped_type &state)> &&func) const {
    for (; gen->non_empty(); ++gen) {
        for (auto pos = lower_bound(*gen); (pos != end()) && (gen->intersects(pos->first)); ++pos) {
            if (func(pos->first, pos->second)) {
                return true;
            }
        }
    }
    return false;
}

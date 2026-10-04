// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// The backend's view of the Vulkan context: dispatch table and handles
// the Vulkan backends need. Kept out of vk_device.h, which must stay free of
// vulkan.h. All functions resolved through vkGetInstanceProcAddr, so nothing
// links against libvulkan and a driverless machine is not a failure to start.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "core/gpu/vk/vk_device.h"

namespace ds::gpu::vk {

// Context and allocation.
#define DS_VK_FNS_CORE \
  F(vkCreateInstance) F(vkDestroyInstance) F(vkEnumeratePhysicalDevices) \
  F(vkGetPhysicalDeviceProperties) F(vkGetPhysicalDeviceProperties2) F(vkGetPhysicalDeviceFeatures2) \
  F(vkGetPhysicalDeviceMemoryProperties) F(vkGetPhysicalDeviceQueueFamilyProperties) \
  F(vkEnumerateDeviceExtensionProperties) F(vkCreateDevice) F(vkDestroyDevice) \
  F(vkGetDeviceQueue) F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements) \
  F(vkAllocateMemory) F(vkFreeMemory) F(vkBindBufferMemory) F(vkMapMemory) \
  F(vkInvalidateMappedMemoryRanges) F(vkFlushMappedMemoryRanges) \
  F(vkGetMemoryHostPointerPropertiesEXT) F(vkDeviceWaitIdle)

// Pipeline, descriptors, command recording and submission.
#define DS_VK_FNS_PIPE \
  F(vkCreateShaderModule) F(vkDestroyShaderModule) \
  F(vkCreateDescriptorSetLayout) F(vkDestroyDescriptorSetLayout) \
  F(vkCreatePipelineLayout) F(vkDestroyPipelineLayout) \
  F(vkCreateComputePipelines) F(vkDestroyPipeline) \
  F(vkCreateDescriptorPool) F(vkDestroyDescriptorPool) \
  F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) \
  F(vkCreateCommandPool) F(vkDestroyCommandPool) F(vkAllocateCommandBuffers) \
  F(vkResetCommandBuffer) F(vkBeginCommandBuffer) F(vkEndCommandBuffer) \
  F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) F(vkCmdPushConstants) \
  F(vkCmdDispatch) F(vkCmdPipelineBarrier) F(vkCmdFillBuffer) \
  F(vkCreateFence) F(vkDestroyFence) F(vkResetFences) F(vkWaitForFences) F(vkGetFenceStatus) \
  F(vkCreateQueryPool) F(vkDestroyQueryPool) F(vkCmdResetQueryPool) F(vkCmdWriteTimestamp) F(vkGetQueryPoolResults) \
  F(vkQueueSubmit) \
  F(vkCreateImage) F(vkDestroyImage) F(vkGetImageMemoryRequirements) F(vkBindImageMemory) \
  F(vkCreateImageView) F(vkDestroyImageView) F(vkGetImageSubresourceLayout) F(vkCreateBufferView) F(vkDestroyBufferView) \
  F(vkGetMemoryFdPropertiesKHR) F(vkGetPhysicalDeviceImageFormatProperties2) \
  F(vkCreateGraphicsPipelines) F(vkCreateRenderPass) F(vkDestroyRenderPass) F(vkCreateFramebuffer) F(vkDestroyFramebuffer) \
  F(vkCmdBeginRenderPass) F(vkCmdEndRenderPass) F(vkCmdNextSubpass) F(vkCmdDraw) F(vkCmdSetStencilReference) F(vkCmdCopyImageToBuffer) F(vkGetPhysicalDeviceFormatProperties) F(vkCmdClearAttachments)

// Presentation (VK_KHR_surface / VK_KHR_swapchain; null when not enabled) and
// what a graphics presenter records beyond the raster's needs.
#define DS_VK_FNS_WSI \
  F(vkGetPhysicalDeviceSurfaceSupportKHR) F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
  F(vkGetPhysicalDeviceSurfaceFormatsKHR) F(vkGetPhysicalDeviceSurfacePresentModesKHR) F(vkDestroySurfaceKHR) \
  F(vkCreateSwapchainKHR) F(vkDestroySwapchainKHR) F(vkGetSwapchainImagesKHR) F(vkAcquireNextImageKHR) F(vkQueuePresentKHR) \
  F(vkCreateSemaphore) F(vkDestroySemaphore) F(vkCmdCopyBufferToImage) F(vkCreateSampler) F(vkDestroySampler) \
  F(vkCmdSetViewport) F(vkCmdSetScissor) F(vkQueueWaitIdle) F(vkCmdCopyImage) F(vkCmdClearColorImage)

#define DS_VK_FNS DS_VK_FNS_CORE DS_VK_FNS_PIPE DS_VK_FNS_WSI

struct Api {
#define F(n) PFN_##n n = nullptr;
  DS_VK_FNS
#undef F
};

// What Device hands the backend; lifetime is the Device's.
struct DeviceInternal {
  const Api*       api = nullptr;
  VkInstance       inst = VK_NULL_HANDLE;
  VkDevice         dev = VK_NULL_HANDLE;
  VkQueue          queue = VK_NULL_HANDLE;
  VkQueue          queue2 = VK_NULL_HANDLE;   // the 3D raster's own queue (null: one queue, shared under queue_mutex)
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  u32              qfam = 0;
  VkDeviceSize     non_coherent_atom = 1;
};

inline VkBuffer vk_buf(const Buffer& b) { return reinterpret_cast<VkBuffer>(b.handle); }

} // namespace ds::gpu::vk

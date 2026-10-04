// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

// A compute-only Vulkan context for the GPU 3D raster: no surface, no
// swapchain, no DRM master, so it can't conflict with the present tiers'
// ownership of the panel. libvulkan is dlopen'd and resolved through
// vkGetInstanceProcAddr; create() returns null on a driverless machine
// rather than failing to start.

namespace ds::gpu::vk {

struct DeviceInternal;   // vk_internal.h; the backend's view of this context

// Both are DEVICE_LOCAL on a unified part; the difference is the CPU mapping's cacheability.
enum class Access {
  CpuRead,    // DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED, near-malloc CPU reads. Needs invalidate() before reading. What the 2D compositor's 3D layer must be.
  CpuWrite,   // DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT, typically write-combine (slow CPU reads). Write-only buffers.
};

// One mapped allocation. `ptr` is vkMapMemory's CPU mapping -- use it, not an
// mmap'd exported fd (this driver only imports dma-buf, never exports).
struct Buffer {
  void*  ptr  = nullptr;
  size_t size = 0;
  u64    handle = 0;      // opaque VkBuffer, for the backend
  u64    memory = 0;      // opaque VkDeviceMemory
  explicit operator bool() const { return ptr != nullptr; }
};

class Device {
public:
  // Null when there's no usable Vulkan (no libvulkan/device/compute
  // queue/CpuRead memory type). Never throws or aborts.
  static std::unique_ptr<Device> create(std::string* why = nullptr);
  // The process's one context, shared by the 3D raster and present stage so
  // the raster's output can be bound without leaving the device. Created on
  // first use; a failed creation is not cached.
  static std::shared_ptr<Device> shared(std::string* why = nullptr);
  // Instance extensions a window system needs for presenting (the frontend
  // passes SDL_Vulkan_GetInstanceExtensions' list). Takes effect for a
  // Device created afterwards; limits().present says whether they (and
  // VK_KHR_swapchain) were enabled.
  static void set_present_extensions(const char* const* names, u32 count);
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  const std::string& name() const { return name_; }

  // Allocate and map. Returns an empty Buffer on failure.
  Buffer alloc(size_t size, Access access);
  void   free(Buffer& b);

  // Makes GPU writes visible to a CPU read of a CpuRead buffer. Pass the
  // range actually read, not the whole allocation.
  void invalidate(const Buffer& b, size_t offset, size_t size);

  // Binds an existing host allocation (VK_EXT_external_memory_host) so the
  // GPU can read it with no staging copy (decoded texture cache is the
  // intended user). `ptr` must be aligned to host_ptr_align(); empty Buffer if refused.
  Buffer import_host(void* ptr, size_t size);
  size_t host_ptr_align() const { return host_align_; }

  // Makes a CPU write to a CpuWrite buffer visible to the GPU; always call
  // this, it's a no-op when the mapping is already coherent. Rounds to
  // nonCoherentAtomSize as vkFlushMappedMemoryRanges requires.
  void flush(const Buffer& b, size_t offset, size_t size);

  // The dispatch table and handles, for the Vulkan backends (vk_lean.cpp, the presenters) only.
  const DeviceInternal* internal() const;
  // Held around every vkQueueSubmit/vkQueuePresentKHR/vkQueueWaitIdle by
  // code that may run beside another thread's use of the queue (the video
  // thread presenting while the emulation thread renders a frame).
  std::mutex& queue_mutex() { return queue_mutex_; }

  struct Limits {
    u32 max_workgroup_invocations = 0;
    u32 max_shared_memory = 0;
    u32 subgroup_size = 0;
    u32 max_storage_range = 0;           // largest storage buffer the shader may bind
    // VK_KHR_shader_atomic_int64 + shaderInt64: without it, the visibility
    // pass's atomicMin owner key can't run and the ordered loop draws the whole list.
    bool int64_atomics = false;
    double timestamp_period_ns = 0;   // GPU timestamp ns/tick on the compute queue, 0 if none (DS_VK_TIMING=1)
    // dma-buf IMPORT: frontend scanout buffers bindable as images the GPU
    // writes. drm_modifier: LINEAR layout stated explicitly, else VK_IMAGE_TILING_LINEAR.
    bool dmabuf_import = false;
    bool drm_modifier = false;
    // Queue does graphics + VK_EXT_rasterization_order_attachment_access:
    // triangle path's translucent tail reads its own colour attachment in
    // one draw; without it, a barrier per polygon (correct, slower).
    bool graphics = false;
    bool ordered_attachments = false;
    // 4x multisampled R32UI colour and depth attachments, and sample-rate
    // shading (enabled): the triangle path's MSAA.
    bool msaa4 = false;
    // The present extensions (set_present_extensions) and VK_KHR_swapchain are enabled.
    bool present = false;
  };
  const Limits& limits() const { return limits_; }

private:
  Device();
  struct Impl;
  std::unique_ptr<Impl> d_;
  std::string name_;
  size_t host_align_ = 4096;
  Limits limits_{};
  std::mutex queue_mutex_;
};

} // namespace ds::gpu::vk

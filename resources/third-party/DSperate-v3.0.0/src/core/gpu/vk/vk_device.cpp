// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/vk/vk_device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Real only on Linux with the Vulkan headers (DSPERATE_VULKAN, from CMake);
// elsewhere every entry point declines, so the renderer and the presenters
// build the same and fall back.
#if defined(__linux__) && DSPERATE_VULKAN
#include <dlfcn.h>
#include "core/gpu/vk/vk_internal.h"
#define DS_VK_AVAILABLE 1
#else
#define DS_VK_AVAILABLE 0
#endif

namespace ds::gpu::vk {

std::shared_ptr<Device> Device::shared(std::string* why) {
  static std::weak_ptr<Device> weak;
  if (auto d = weak.lock()) return d;
  std::unique_ptr<Device> u = create(why);
  if (!u) return nullptr;
  std::shared_ptr<Device> d(std::move(u));
  weak = d;
  return d;
}


namespace {
std::vector<std::string> g_present_exts;
}
void Device::set_present_extensions(const char* const* names, u32 count) {
  g_present_exts.assign(names, names + count);
}

#if !DS_VK_AVAILABLE

struct Device::Impl {};
Device::Device() = default;
Device::~Device() = default;
std::unique_ptr<Device> Device::create(std::string* why) {
  if (why) *why = "built without Vulkan";
  return nullptr;
}
Buffer Device::alloc(size_t, Access) { return {}; }
void   Device::free(Buffer&) {}
void   Device::invalidate(const Buffer&, size_t, size_t) {}
Buffer Device::import_host(void*, size_t) { return {}; }
void   Device::flush(const Buffer&, size_t, size_t) {}
const  DeviceInternal* Device::internal() const { return nullptr; }

#else

struct Device::Impl {
  void* lib = nullptr;
  Api   api{};
  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue  queue = VK_NULL_HANDLE, queue2 = VK_NULL_HANDLE;
  u32      qfam = 0;
  bool     has_host_import = false;
  bool     device_local = true;   // the chosen CpuRead type is also DEVICE_LOCAL
  DeviceInternal internal{};
  VkPhysicalDeviceMemoryProperties memprops{};

  // CpuRead must be HOST_CACHED (NEON composite reads it directly; write-
  // combine is drastically slower), refused otherwise. DEVICE_LOCAL is only
  // a preference (costs nothing on unified parts; a discrete dev card's
  // cached host-visible type may be system memory) so it's tried first, and
  // falling back is reported via Device::name() rather than silent.
  bool find_mem(u32 bits, Access a, u32* out, bool* was_device_local = nullptr) const {
    const VkMemoryPropertyFlags need =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      (a == Access::CpuRead ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                            : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (int pass = 0; pass < 2; ++pass) {
      const VkMemoryPropertyFlags want =
        need | (pass == 0 ? VkMemoryPropertyFlags{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT} : 0u);
      for (u32 i = 0; i < memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) {
          *out = i;
          if (was_device_local) *was_device_local = pass == 0;
          return true;
        }
    }
    return false;
  }
};

Device::Device() : d_(std::make_unique<Impl>()) {}

Device::~Device() {
  if (!d_) return;
  if (d_->dev) { d_->api.vkDeviceWaitIdle(d_->dev); d_->api.vkDestroyDevice(d_->dev, nullptr); }
  if (d_->inst) d_->api.vkDestroyInstance(d_->inst, nullptr);
  if (d_->lib) dlclose(d_->lib);
}

std::unique_ptr<Device> Device::create(std::string* why) {
  auto set = [&](const char* m) { if (why) *why = m; return nullptr; };

  std::unique_ptr<Device> self(new Device());
  Impl& d = *self->d_;

  d.lib = dlopen("libvulkan.so.1", RTLD_NOW);
  if (!d.lib) d.lib = dlopen("libvulkan.so", RTLD_NOW);
  if (!d.lib) return set("libvulkan not present");

  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(d.lib, "vkGetInstanceProcAddr"));
  if (!gipa) return set("vkGetInstanceProcAddr missing");

  auto create_inst = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
  if (!create_inst) return set("vkCreateInstance missing");

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "DSperate";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ii.pApplicationInfo = &app;
  // Presentation's instance extensions, when a frontend asked and the loader has every one.
  std::vector<const char*> inst_exts;
  if (!g_present_exts.empty()) {
    auto enum_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    u32 nie = 0;
    std::vector<VkExtensionProperties> ie;
    if (enum_ext && enum_ext(nullptr, &nie, nullptr) == VK_SUCCESS) { ie.resize(nie); enum_ext(nullptr, &nie, ie.data()); }
    bool all = true;
    for (const std::string& want : g_present_exts) {
      bool have = false;
      for (const auto& e : ie) have |= want == e.extensionName;
      all &= have;
    }
    if (all) for (const std::string& want : g_present_exts) inst_exts.push_back(want.c_str());
  }
  ii.enabledExtensionCount = static_cast<u32>(inst_exts.size());
  ii.ppEnabledExtensionNames = inst_exts.empty() ? nullptr : inst_exts.data();
  if (create_inst(&ii, nullptr, &d.inst) != VK_SUCCESS) return set("vkCreateInstance failed");

#define F(n) d.api.n = reinterpret_cast<PFN_##n>(gipa(d.inst, #n));
  DS_VK_FNS
#undef F
  if (!d.api.vkEnumeratePhysicalDevices || !d.api.vkCreateDevice) return set("entry points missing");

  u32 n = 0;
  d.api.vkEnumeratePhysicalDevices(d.inst, &n, nullptr);
  if (!n) return set("no Vulkan device");
  std::vector<VkPhysicalDevice> devs(n);
  d.api.vkEnumeratePhysicalDevices(d.inst, &n, devs.data());
  // DS_VK_DEVICE: pick physical device by index or name substring; DS_VK_LIST_EXT lists them.
  d.phys = devs[0];
  if (const char* pick = std::getenv("DS_VK_DEVICE")) {
    char* end = nullptr;
    const long idx = std::strtol(pick, &end, 10);
    for (u32 i = 0; i < n; ++i) {
      VkPhysicalDeviceProperties pp{}; d.api.vkGetPhysicalDeviceProperties(devs[i], &pp);
      const bool by_index = end && *end == '\0' && idx == static_cast<long>(i);
      if (by_index || (!(end && *end == '\0') && std::strstr(pp.deviceName, pick))) { d.phys = devs[i]; break; }
    }
  }
  if (std::getenv("DS_VK_LIST_EXT"))
    for (u32 i = 0; i < n; ++i) { VkPhysicalDeviceProperties pp{}; d.api.vkGetPhysicalDeviceProperties(devs[i], &pp); std::fprintf(stderr, "vk: device %u: %s%s\n", i, pp.deviceName, devs[i] == d.phys ? " (selected)" : ""); }

  VkPhysicalDeviceProperties props{};
  d.api.vkGetPhysicalDeviceProperties(d.phys, &props);
  d.api.vkGetPhysicalDeviceMemoryProperties(d.phys, &d.memprops);
  self->name_ = props.deviceName;
  self->limits_.max_workgroup_invocations = props.limits.maxComputeWorkGroupInvocations;
  self->limits_.max_shared_memory = props.limits.maxComputeSharedMemorySize;
  self->limits_.max_storage_range = props.limits.maxStorageBufferRange;
  d.internal.non_coherent_atom = props.limits.nonCoherentAtomSize ? props.limits.nonCoherentAtomSize : 1;

  VkPhysicalDeviceSubgroupProperties sub{};
  sub.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
  VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp{};
  hostp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
  sub.pNext = &hostp;
  VkPhysicalDeviceProperties2 p2{};
  p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  p2.pNext = &sub;
  if (d.api.vkGetPhysicalDeviceProperties2) {
    d.api.vkGetPhysicalDeviceProperties2(d.phys, &p2);
    self->limits_.subgroup_size = sub.subgroupSize;
    if (hostp.minImportedHostPointerAlignment) self->host_align_ = hostp.minImportedHostPointerAlignment;
  }

  u32 ne = 0;
  d.api.vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &ne, nullptr);
  std::vector<VkExtensionProperties> exts(ne);
  d.api.vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &ne, exts.data());
  bool has_gprio = false, has_atomic64_ext = false, has_fd = false, has_dmabuf = false, has_modifier = false, has_fmtlist = false, has_foreign = false, has_roaa = false, has_swapchain = false;
  for (const auto& e : exts) {
    if (!std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) d.has_host_import = true;
    if (!std::strcmp(e.extensionName, VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME)) has_atomic64_ext = true;
    if (!std::strcmp(e.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) has_fd = true;
    if (!std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) has_dmabuf = true;
    if (!std::strcmp(e.extensionName, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)) has_modifier = true;
    if (!std::strcmp(e.extensionName, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME)) has_fmtlist = true;
    if (!std::strcmp(e.extensionName, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) has_foreign = true;
    if (!std::strcmp(e.extensionName, VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME)) has_roaa = true;
    if (!std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) has_swapchain = true;
    if (!std::strcmp(e.extensionName, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME)) has_gprio = true;
  }
  self->limits_.present = !inst_exts.empty() && has_swapchain;
  self->limits_.dmabuf_import = has_fd && has_dmabuf;
  self->limits_.drm_modifier = self->limits_.dmabuf_import && has_modifier && has_fmtlist;
  VkPhysicalDeviceShaderAtomicInt64Features at64{};
  at64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
  VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT roaa{};
  roaa.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT;
  VkPhysicalDeviceFeatures2 f2{};
  f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  f2.pNext = &at64;
  if (has_roaa) at64.pNext = &roaa;
  if (d.api.vkGetPhysicalDeviceFeatures2 && (has_atomic64_ext || has_roaa || props.apiVersion >= VK_API_VERSION_1_2)) {
    d.api.vkGetPhysicalDeviceFeatures2(d.phys, &f2);
    self->limits_.int64_atomics = (has_atomic64_ext || props.apiVersion >= VK_API_VERSION_1_2) && f2.features.shaderInt64 && at64.shaderBufferInt64Atomics;
    self->limits_.ordered_attachments = has_roaa && roaa.rasterizationOrderColorAttachmentAccess;
    self->limits_.msaa4 = f2.features.sampleRateShading && (props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts & VK_SAMPLE_COUNT_4_BIT);
  }
  // DS_VK_LIST_EXT=1: what this driver actually offers.
  if (std::getenv("DS_VK_LIST_EXT")) {
    std::fprintf(stderr, "vk: %s, %u device extensions:\n", props.deviceName, ne);
    for (const auto& e : exts) std::fprintf(stderr, "vk:   %s\n", e.extensionName);
  }

  u32 nq = 0;
  d.api.vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &nq, nullptr);
  std::vector<VkQueueFamilyProperties> qf(nq);
  d.api.vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &nq, qf.data());
  bool found = false;
  u32 nqueues = 1;
  for (u32 i = 0; i < nq; ++i)
    if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      d.qfam = i; found = true; nqueues = std::min(2u, qf[i].queueCount);
      self->limits_.graphics = (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
      if (qf[i].timestampValidBits) self->limits_.timestamp_period_ns = props.limits.timestampPeriod;
      break;
    }
  if (!found) return set("no compute queue");

  std::vector<const char*> want;
  if (d.has_host_import) want.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
  if (self->limits_.int64_atomics && has_atomic64_ext) want.push_back(VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME);
  if (self->limits_.dmabuf_import) { want.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME); want.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME); }
  if (self->limits_.drm_modifier) { want.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME); want.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME); }
  if (self->limits_.dmabuf_import && has_foreign) want.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
  if (self->limits_.ordered_attachments) want.push_back(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME);
  if (self->limits_.present) want.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  VkPhysicalDeviceShaderAtomicInt64Features en64{};
  en64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
  en64.shaderBufferInt64Atomics = VK_TRUE;
  VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT enroaa{};
  enroaa.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT;
  enroaa.rasterizationOrderColorAttachmentAccess = VK_TRUE;
  VkPhysicalDeviceFeatures2 enf{};
  enf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  if (self->limits_.int64_atomics) { enf.pNext = &en64; enf.features.shaderInt64 = VK_TRUE; }
  if (self->limits_.ordered_attachments) { enroaa.pNext = enf.pNext; enf.pNext = &enroaa; }
  if (self->limits_.msaa4) enf.features.sampleRateShading = VK_TRUE;

  // Two queues where the family has them: the second is the 3D raster's
  // own. The present path's jobs wait on the panel (a dma-buf still being
  // scanned out), and on one queue a raster job submitted after such a job
  // waits with it: a frame of latency for two milliseconds of work.
  const float prio[2] = {1.0f, 1.0f};
  VkDeviceQueueCreateInfo qi{};
  qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qi.queueFamilyIndex = d.qfam;
  qi.queueCount = nqueues;
  qi.pQueuePriorities = prio;
  // DS_VK_PRIORITY=low|medium|high|realtime: the queue's priority against
  // other processes' GPU work (the Wayland compositor's), where the driver
  // offers it (VK_EXT_global_priority). The 3D layer of a capture frame is
  // needed a few milliseconds after its submit, so it must not queue behind
  // the compositor. Default high when available.
  VkDeviceQueueGlobalPriorityCreateInfoEXT gp{};
  gp.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT;
  {
    const char* p = std::getenv("DS_VK_PRIORITY");
    const std::string ps = p ? p : "high";
    gp.globalPriority = ps == "low" ? VK_QUEUE_GLOBAL_PRIORITY_LOW_EXT : ps == "medium" ? VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_EXT : ps == "realtime" ? VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT : VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT;
    if (has_gprio && ps != "off") { want.push_back(VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME); qi.pNext = &gp; }
  }
  VkDeviceCreateInfo di{};
  di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  di.enabledExtensionCount = static_cast<u32>(want.size());
  di.ppEnabledExtensionNames = want.empty() ? nullptr : want.data();
  if (self->limits_.int64_atomics || self->limits_.ordered_attachments || self->limits_.msaa4) di.pNext = &enf;
  if (d.api.vkCreateDevice(d.phys, &di, nullptr, &d.dev) != VK_SUCCESS) {
    // A priority the process may not take (VK_ERROR_NOT_PERMITTED) or a driver quirk: once more without it.
    if (!qi.pNext) return set("vkCreateDevice failed");
    qi.pNext = nullptr; want.pop_back(); di.enabledExtensionCount = static_cast<u32>(want.size());
    if (d.api.vkCreateDevice(d.phys, &di, nullptr, &d.dev) != VK_SUCCESS) return set("vkCreateDevice failed");
  } else if (qi.pNext) self->name_ += " (queue priority)";
  d.api.vkGetDeviceQueue(d.dev, d.qfam, 0, &d.queue);
  if (nqueues > 1) { d.api.vkGetDeviceQueue(d.dev, d.qfam, 1, &d.queue2); self->name_ += " (2 queues)"; }

  u32 dummy = 0;
  if (!d.find_mem(~0u, Access::CpuRead, &dummy, &d.device_local)) return set("no HOST_CACHED memory type");
  if (!d.device_local) self->name_ += " (host memory, not device-local: a development fallback)";
  if (!self->limits_.int64_atomics) self->name_ += " (no 64-bit atomics: the visibility pass is off)";

  d.internal.api = &d.api;
  d.internal.inst = d.inst;
  d.internal.dev = d.dev;
  d.internal.queue = d.queue;
  d.internal.queue2 = d.queue2;
  d.internal.phys = d.phys;
  d.internal.qfam = d.qfam;
  return self;
}

Buffer Device::alloc(size_t size, Access access) {
  Impl& d = *d_;
  Buffer out;

  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size = size;
  bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;   // texel buffer: triangle path fetches through it
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buf = VK_NULL_HANDLE;
  if (d.api.vkCreateBuffer(d.dev, &bi, nullptr, &buf) != VK_SUCCESS) return out;

  VkMemoryRequirements mr{};
  d.api.vkGetBufferMemoryRequirements(d.dev, buf, &mr);
  u32 type = 0;
  if (!d.find_mem(mr.memoryTypeBits, access, &type)) { d.api.vkDestroyBuffer(d.dev, buf, nullptr); return out; }

  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = type;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  if (d.api.vkAllocateMemory(d.dev, &ai, nullptr, &mem) != VK_SUCCESS) {
    d.api.vkDestroyBuffer(d.dev, buf, nullptr);
    return out;
  }
  if (d.api.vkBindBufferMemory(d.dev, buf, mem, 0) != VK_SUCCESS) {
    d.api.vkFreeMemory(d.dev, mem, nullptr);
    d.api.vkDestroyBuffer(d.dev, buf, nullptr);
    return out;
  }
  void* ptr = nullptr;
  if (d.api.vkMapMemory(d.dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) {
    d.api.vkFreeMemory(d.dev, mem, nullptr);
    d.api.vkDestroyBuffer(d.dev, buf, nullptr);
    return out;
  }

  out.ptr = ptr;
  out.size = size;
  out.handle = reinterpret_cast<u64>(buf);
  out.memory = reinterpret_cast<u64>(mem);
  return out;
}

void Device::free(Buffer& b) {
  if (!b) return;
  Impl& d = *d_;
  d.api.vkDestroyBuffer(d.dev, reinterpret_cast<VkBuffer>(b.handle), nullptr);
  d.api.vkFreeMemory(d.dev, reinterpret_cast<VkDeviceMemory>(b.memory), nullptr);
  b = {};
}

void Device::invalidate(const Buffer& b, size_t offset, size_t size) {
  if (!b) return;
  Impl& d = *d_;
  VkMappedMemoryRange mr{};
  mr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  mr.memory = reinterpret_cast<VkDeviceMemory>(b.memory);
  const VkDeviceSize atom = d.internal.non_coherent_atom;
  VkDeviceSize off = offset / atom * atom;
  VkDeviceSize len = size ? ((offset + size + atom - 1) / atom * atom) - off : VK_WHOLE_SIZE;
  if (len != VK_WHOLE_SIZE && off + len > b.size) len = VK_WHOLE_SIZE;
  mr.offset = off;
  mr.size = len;
  d.api.vkInvalidateMappedMemoryRanges(d.dev, 1, &mr);
}

Buffer Device::import_host(void* ptr, size_t size) {
  Impl& d = *d_;
  Buffer out;
  if (!d.has_host_import || !ptr) return out;
  if (reinterpret_cast<uintptr_t>(ptr) % host_align_) return out;   // caller must align

  VkImportMemoryHostPointerInfoEXT ip{};
  ip.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
  ip.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  ip.pHostPointer = ptr;

  VkExternalMemoryBufferCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.pNext = &ext;
  bi.size = size;
  bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  VkBuffer buf = VK_NULL_HANDLE;
  if (d.api.vkCreateBuffer(d.dev, &bi, nullptr, &buf) != VK_SUCCESS) return out;

  VkMemoryRequirements mr{};
  d.api.vkGetBufferMemoryRequirements(d.dev, buf, &mr);
  u32 bits = mr.memoryTypeBits;
  if (d.api.vkGetMemoryHostPointerPropertiesEXT) {
    VkMemoryHostPointerPropertiesEXT hp{};
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    if (d.api.vkGetMemoryHostPointerPropertiesEXT(
            d.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp) == VK_SUCCESS)
      bits &= hp.memoryTypeBits;
  }
  u32 type = 0;
  if (!d.find_mem(bits, Access::CpuRead, &type)) { d.api.vkDestroyBuffer(d.dev, buf, nullptr); return out; }

  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.pNext = &ip;
  ai.allocationSize = size;
  ai.memoryTypeIndex = type;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  if (d.api.vkAllocateMemory(d.dev, &ai, nullptr, &mem) != VK_SUCCESS) {
    d.api.vkDestroyBuffer(d.dev, buf, nullptr);
    return out;
  }
  if (d.api.vkBindBufferMemory(d.dev, buf, mem, 0) != VK_SUCCESS) {
    d.api.vkFreeMemory(d.dev, mem, nullptr);
    d.api.vkDestroyBuffer(d.dev, buf, nullptr);
    return out;
  }
  out.ptr = ptr;                 // the caller's own pointer: no new mapping
  out.size = size;
  out.handle = reinterpret_cast<u64>(buf);
  out.memory = reinterpret_cast<u64>(mem);
  return out;
}

void Device::flush(const Buffer& b, size_t offset, size_t size) {
  if (!b) return;
  Impl& d = *d_;
  const VkDeviceSize atom = d.internal.non_coherent_atom;
  VkDeviceSize off = offset / atom * atom;
  VkDeviceSize len = size ? ((offset + size + atom - 1) / atom * atom) - off : VK_WHOLE_SIZE;
  if (len != VK_WHOLE_SIZE && off + len > b.size) len = VK_WHOLE_SIZE;
  VkMappedMemoryRange mr{};
  mr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  mr.memory = reinterpret_cast<VkDeviceMemory>(b.memory);
  mr.offset = off;
  mr.size = len;
  d.api.vkFlushMappedMemoryRanges(d.dev, 1, &mr);
}

const DeviceInternal* Device::internal() const { return &d_->internal; }

#endif // DS_VK_AVAILABLE

} // namespace ds::gpu::vk

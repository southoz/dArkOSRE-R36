// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/gpu_present.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

#if DSPERATE_VULKAN
#include "core/gpu/vk/vk_internal.h"

#include <chrono>
#include <unistd.h>

namespace ds::sdl {

using gpu::vk::Access;
using gpu::vk::Api;
using gpu::vk::Buffer;
using gpu::vk::Device;
using gpu::vk::DeviceInternal;

namespace {

// SPIR-V linked in with .incbin: no shader compiler needed to build.
__asm__(".section .rodata\n.balign 4\n.globl ds_present_spv_data\nds_present_spv_data:\n"
        ".incbin \"" DSPERATE_PRESENT_SHADER_DIR "/present.spv\"\n"
        ".globl ds_present_spv_end\nds_present_spv_end:\n.previous\n");
extern "C" const unsigned char ds_present_spv_data[], ds_present_spv_end[];

constexpr u32 kFrameWords = 256 * 192;     // one screen
constexpr u32 kSlotWords = 2 * kFrameWords;  // both screens
constexpr int kSlots = 2;                  // frames in flight + the one being written
constexpr int kMaxBufs = 8;
constexpr u32 kBindings = 4;               // dst image, source frames, overlay, grid table

struct Push {
  u32 a[4];        // presented w, h; logical w, h
  u32 b[4];        // rot, nviews, alpha | overlay | grid, source base (words)
  s32 rect[2][4];
  s32 view[2][4];
};

} // namespace

struct GpuPresent::Impl {
  std::shared_ptr<Device> dev;
  const DeviceInternal* vk = nullptr;
  const Api* a = nullptr;
  VkPhysicalDeviceMemoryProperties memprops{};

  struct Imported { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkDescriptorSet set[kSlots] = {VK_NULL_HANDLE, VK_NULL_HANDLE}; int fd = -1; u32 stride = 0; };
  Buffer over[kSlots];                 // frontend's overlay per slot (logical frame, pitch = lw)
  Buffer tab[kSlots];
  SDL_Rect over_dirty[kSlots] = {};    // drawn region per slot, cleared before next use
  int over_lw = 0, over_lh = 0;
  Imported bufs[kMaxBufs];
  int nbufs = 0;
  u32 w = 0, h = 0;

  Buffer src;                                  // kSlots x kSlotWords
  VkShaderModule mod = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipe = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkCommandPool cmdpool = VK_NULL_HANDLE;
  VkCommandBuffer cmd[kSlots]{};
  VkFence fence[kSlots]{};
  bool fence_live[kSlots] = {false, false};    // a submit is outstanding on this slot
  int pending_buf = -1;                        // tier buffer of the frame in flight, not yet end_frame()'d
  int pending_slot = -1;
  u64 frame = 0;
  u64 wait_ns = 0, waits = 0, frames = 0, late = 0;   // exit-line stats: fence wait time
  u64 now_ns = 0, now_n = 0;                            // DS_PRESENT_WAITNOW=1: the job's submit-to-signal time (diagnostic; serialises the present)
  u64 t_begin = 0, t_upload = 0, t_submit = 0, t_retire = 0;   // present() time breakdown, per frame

  bool ok() const { return pipe != VK_NULL_HANDLE; }

  u32 find_mem(u32 bits, VkMemoryPropertyFlags want) const {
    for (u32 i = 0; i < memprops.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    for (u32 i = 0; i < memprops.memoryTypeCount; ++i) if (bits & (1u << i)) return i;
    return ~0u;
  }

  void drop_import(Imported& b) {
    if (b.view) a->vkDestroyImageView(vk->dev, b.view, nullptr);
    if (b.img) a->vkDestroyImage(vk->dev, b.img, nullptr);
    if (b.mem) a->vkFreeMemory(vk->dev, b.mem, nullptr);
    b.view = VK_NULL_HANDLE; b.img = VK_NULL_HANDLE; b.mem = VK_NULL_HANDLE;
  }

  // Import a tier dma-buf as a LINEAR B8G8R8A8 image and point a descriptor
  // set at it.
  bool import(const ScanoutOut::DmabufPlane& p, Imported& b, std::string* why) {
    const VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    const bool mod = dev->limits().drm_modifier;
    VkSubresourceLayout plane{}; plane.offset = p.offset; plane.rowPitch = p.stride_bytes;
    VkImageDrmFormatModifierExplicitCreateInfoEXT mex{};
    mex.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    mex.drmFormatModifier = 0;   // DRM_FORMAT_MOD_LINEAR
    mex.drmFormatModifierPlaneCount = 1;
    mex.pPlaneLayouts = &plane;
    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.pNext = mod ? static_cast<const void*>(&mex) : nullptr;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.pNext = &ext;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {p.width, p.height, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = mod ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (a->vkCreateImage(vk->dev, &ii, nullptr, &b.img) != VK_SUCCESS) { if (why) *why = "vkCreateImage (imported) failed"; return false; }
    if (!mod) {
      // Without the modifier extension the driver picks the pitch; must match
      // the tier's or the picture shears.
      VkImageSubresource sr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}; VkSubresourceLayout got{};
      a->vkGetImageSubresourceLayout(vk->dev, b.img, &sr, &got);
      if (got.rowPitch != p.stride_bytes) { if (why) *why = "LINEAR image pitch differs from the scanout pitch"; drop_import(b); return false; }
    }
    VkMemoryRequirements mr{}; a->vkGetImageMemoryRequirements(vk->dev, b.img, &mr);
    VkMemoryFdPropertiesKHR fp{}; fp.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (a->vkGetMemoryFdPropertiesKHR(vk->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, p.fd, &fp) != VK_SUCCESS) { if (why) *why = "vkGetMemoryFdPropertiesKHR failed"; drop_import(b); return false; }
    const u32 type = find_mem(fp.memoryTypeBits & mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == ~0u) { if (why) *why = "no memory type for the imported dma-buf"; drop_import(b); return false; }
    const int dfd = dup(p.fd);   // the import takes ownership of the fd on success
    VkMemoryDedicatedAllocateInfo ded{}; ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO; ded.image = b.img;
    VkImportMemoryFdInfoKHR imp{}; imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR; imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp.fd = dfd;
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.pNext = &imp;
    ai.allocationSize = mr.size; ai.memoryTypeIndex = type;
    if (a->vkAllocateMemory(vk->dev, &ai, nullptr, &b.mem) != VK_SUCCESS) { close(dfd); if (why) *why = "vkAllocateMemory (import) failed"; drop_import(b); return false; }
    if (a->vkBindImageMemory(vk->dev, b.img, b.mem, 0) != VK_SUCCESS) { if (why) *why = "vkBindImageMemory failed"; drop_import(b); return false; }
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = b.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (a->vkCreateImageView(vk->dev, &vi, nullptr, &b.view) != VK_SUCCESS) { if (why) *why = "vkCreateImageView failed"; drop_import(b); return false; }
    VkDescriptorImageInfo dii{}; dii.imageView = b.view; dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo dbi{}; dbi.buffer = gpu::vk::vk_buf(src); dbi.range = VK_WHOLE_SIZE;
    for (int s = 0; s < kSlots; ++s) {
      VkDescriptorBufferInfo doi{}; doi.buffer = gpu::vk::vk_buf(over[s]); doi.range = VK_WHOLE_SIZE;
      VkDescriptorBufferInfo dti{}; dti.buffer = gpu::vk::vk_buf(tab[s]); dti.range = VK_WHOLE_SIZE;
      const VkDescriptorBufferInfo* bi[kBindings] = {nullptr, &dbi, &doi, &dti};
      VkWriteDescriptorSet w[kBindings]{};
      for (u32 i = 0; i < kBindings; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = b.set[s]; w[i].dstBinding = i; w[i].descriptorCount = 1;
        w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (i == 0) w[i].pImageInfo = &dii; else w[i].pBufferInfo = bi[i];
      }
      a->vkUpdateDescriptorSets(vk->dev, kBindings, w, 0, nullptr);
    }
    return true;
  }

  bool import_all(ScanoutOut& out, std::string* why) {
    for (int i = 0; i < nbufs; ++i) drop_import(bufs[i]);
    nbufs = 0;
    const int n = out.bufs();
    if (n <= 0 || n > kMaxBufs) { if (why) *why = "tier buffer count out of range"; return false; }
    // Overlays follow the tier's size; must precede the descriptor writes below.
    {
      ScanoutOut::DmabufPlane p0;
      if (!out.dmabuf_plane(0, p0)) { if (why) *why = "tier has no dma-buf"; return false; }
      const size_t need = sizeof(u32) * p0.width * p0.height;
      for (int s = 0; s < kSlots; ++s) {
        if (over[s] && over[s].size >= need) continue;
        if (over[s]) dev->free(over[s]);
        over[s] = dev->alloc(need, Access::CpuRead);
        if (!over[s]) { if (why) *why = "overlay allocation failed"; return false; }
        std::memset(over[s].ptr, 0, need);
        dev->flush(over[s], 0, need);
      }
      over_lw = over_lh = 0;   // overlay() starts each slot clean at new geometry
    }
    for (int i = 0; i < n; ++i) {
      ScanoutOut::DmabufPlane p;
      if (!out.dmabuf_plane(i, p)) { if (why) *why = "tier has no dma-buf for its buffer"; return false; }
      if (i == 0) { w = p.width; h = p.height; }
      bufs[i].fd = p.fd; bufs[i].stride = p.stride_bytes;
      if (!import(p, bufs[i], why)) return false;
      ++nbufs;
    }
    return true;
  }

  void retire(ScanoutOut& out) {
    if (pending_slot < 0) return;
    const auto t0 = std::chrono::steady_clock::now();
    const VkResult r = a->vkWaitForFences(vk->dev, 1, &fence[pending_slot], VK_TRUE, 100'000'000ull);
    const u64 ns = static_cast<u64>((std::chrono::steady_clock::now() - t0).count());
    wait_ns += ns; ++waits; if (ns > 1'000'000) ++late;
    if (r != VK_SUCCESS) std::fprintf(stderr, "gpu present: fence wait failed (%d); the frame is shown as it is\n", static_cast<int>(r));
    fence_live[pending_slot] = false;
    out.end_frame();
    pending_slot = -1; pending_buf = -1;
  }

  ~Impl() {
    if (!vk) return;
    a->vkDeviceWaitIdle(vk->dev);
    for (int i = 0; i < nbufs; ++i) drop_import(bufs[i]);
    for (int s = 0; s < kSlots; ++s) if (fence[s]) a->vkDestroyFence(vk->dev, fence[s], nullptr);
    if (cmdpool) a->vkDestroyCommandPool(vk->dev, cmdpool, nullptr);
    if (pool) a->vkDestroyDescriptorPool(vk->dev, pool, nullptr);
    if (pipe) a->vkDestroyPipeline(vk->dev, pipe, nullptr);
    for (int s = 0; s < kSlots; ++s) if (over[s]) dev->free(over[s]);
    for (int s = 0; s < kSlots; ++s) if (tab[s]) dev->free(tab[s]);
    if (layout) a->vkDestroyPipelineLayout(vk->dev, layout, nullptr);
    if (dsl) a->vkDestroyDescriptorSetLayout(vk->dev, dsl, nullptr);
    if (mod) a->vkDestroyShaderModule(vk->dev, mod, nullptr);
    if (src) dev->free(src);
    if (frames) {
      const double k = 1.0 / (static_cast<double>(frames) * 1e6);
      std::fprintf(stderr, "gpu present: %llu frames on %s; previous-frame fence wait %.3f ms mean, %llu waits over 1 ms\n",
                   static_cast<unsigned long long>(frames), dev->name().c_str(),
                   waits ? static_cast<double>(wait_ns) / static_cast<double>(waits) / 1e6 : 0.0, static_cast<unsigned long long>(late));
      std::fprintf(stderr, "gpu present: per frame -- retire %.3f ms (fence + end_frame), begin_frame %.3f, upload %.3f, record+submit %.3f\n",
                   static_cast<double>(t_retire) * k, static_cast<double>(t_begin) * k, static_cast<double>(t_upload) * k, static_cast<double>(t_submit) * k);
      if (now_n) std::fprintf(stderr, "gpu present: submit-to-signal %.3f ms mean (DS_PRESENT_WAITNOW)\n", static_cast<double>(now_ns) / static_cast<double>(now_n) / 1e6);
    }
  }
};

std::unique_ptr<GpuPresent> GpuPresent::open(ScanoutOut& out, std::string* why) {
  auto fail = [&](const char* m) { if (why) *why = m; return nullptr; };
  std::unique_ptr<GpuPresent> self(new GpuPresent());
  self->d_ = std::make_unique<Impl>();
  Impl& d = *self->d_;
  // One Vulkan context for the process: both windows of a dual-window layout share it.
  d.dev = Device::shared(why);
  if (!d.dev) return nullptr;
  if (!d.dev->limits().dmabuf_import) return fail("driver has no dma-buf import");
  d.vk = d.dev->internal();
  if (!d.vk || !d.vk->api) return fail("no Vulkan context");
  d.a = d.vk->api;
  if (!d.a->vkCreateImage || !d.a->vkGetMemoryFdPropertiesKHR) return fail("image entry points missing");
  d.a->vkGetPhysicalDeviceMemoryProperties(d.vk->phys, &d.memprops);

  d.src = d.dev->alloc(sizeof(u32) * kSlotWords * kSlots, Access::CpuWrite);
  if (!d.src) return fail("source buffer allocation failed");
  for (int s = 0; s < kSlots; ++s) { d.tab[s] = d.dev->alloc(sizeof(u32) * 128, Access::CpuWrite); if (!d.tab[s]) return fail("grid table allocation failed"); std::memset(d.tab[s].ptr, 0, sizeof(u32) * 128); }

  VkShaderModuleCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  si.codeSize = static_cast<size_t>(ds_present_spv_end - ds_present_spv_data);
  si.pCode = reinterpret_cast<const u32*>(ds_present_spv_data);
  if (d.a->vkCreateShaderModule(d.vk->dev, &si, nullptr, &d.mod) != VK_SUCCESS) return fail("present.spv rejected by the driver");

  VkDescriptorSetLayoutBinding b[kBindings]{};
  for (u32 i = 0; i < kBindings; ++i) {
    b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
  VkDescriptorSetLayoutCreateInfo dl{}; dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; dl.bindingCount = kBindings; dl.pBindings = b;
  if (d.a->vkCreateDescriptorSetLayout(d.vk->dev, &dl, nullptr, &d.dsl) != VK_SUCCESS) return fail("descriptor set layout failed");
  VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pcr.size = sizeof(Push);
  VkPipelineLayoutCreateInfo pl{}; pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 1; pl.pSetLayouts = &d.dsl;
  pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
  if (d.a->vkCreatePipelineLayout(d.vk->dev, &pl, nullptr, &d.layout) != VK_SUCCESS) return fail("pipeline layout failed");
  VkComputePipelineCreateInfo cp{}; cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cp.stage.module = d.mod; cp.stage.pName = "main"; cp.layout = d.layout;
  if (d.a->vkCreateComputePipelines(d.vk->dev, VK_NULL_HANDLE, 1, &cp, nullptr, &d.pipe) != VK_SUCCESS) return fail("present pipeline failed to compile");

  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxBufs * kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxBufs * kSlots * (kBindings - 1)}};
  VkDescriptorPoolCreateInfo dp{}; dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dp.maxSets = kMaxBufs * kSlots; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
  if (d.a->vkCreateDescriptorPool(d.vk->dev, &dp, nullptr, &d.pool) != VK_SUCCESS) return fail("descriptor pool failed");
  {
    VkDescriptorSetLayout layouts[kMaxBufs * kSlots]; for (auto& l : layouts) l = d.dsl;
    VkDescriptorSet sets[kMaxBufs * kSlots];
    VkDescriptorSetAllocateInfo da{}; da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; da.descriptorPool = d.pool; da.descriptorSetCount = kMaxBufs * kSlots; da.pSetLayouts = layouts;
    if (d.a->vkAllocateDescriptorSets(d.vk->dev, &da, sets) != VK_SUCCESS) return fail("descriptor sets failed");
    for (int i = 0; i < kMaxBufs; ++i) for (int s = 0; s < kSlots; ++s) d.bufs[i].set[s] = sets[i * kSlots + s];
  }

  VkCommandPoolCreateInfo cpi{}; cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = d.vk->qfam;
  if (d.a->vkCreateCommandPool(d.vk->dev, &cpi, nullptr, &d.cmdpool) != VK_SUCCESS) return fail("command pool failed");
  VkCommandBufferAllocateInfo cba{}; cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cba.commandPool = d.cmdpool;
  cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cba.commandBufferCount = kSlots;
  if (d.a->vkAllocateCommandBuffers(d.vk->dev, &cba, d.cmd) != VK_SUCCESS) return fail("command buffers failed");
  VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  for (int s = 0; s < kSlots; ++s) if (d.a->vkCreateFence(d.vk->dev, &fi, nullptr, &d.fence[s]) != VK_SUCCESS) return fail("fence creation failed");

  if (!d.import_all(out, why)) return nullptr;
  return self;
}

u32* GpuPresent::overlay(int lw, int lh) {
  Impl& d = *d_;
  const int slot = static_cast<int>(d.frame % kSlots);
  if (!d.over[slot]) return nullptr;
  u32* px = static_cast<u32*>(d.over[slot].ptr);
  if (lw != d.over_lw || lh != d.over_lh) {
    // New geometry: every slot starts clean.
    for (int s = 0; s < kSlots; ++s) { std::memset(d.over[s].ptr, 0, d.over[s].size); d.over_dirty[s] = SDL_Rect{0, 0, 0, 0}; }
    d.over_lw = lw; d.over_lh = lh;
  } else {
    const SDL_Rect r = d.over_dirty[slot];
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y), x1 = std::min(lw, r.x + r.w), y1 = std::min(lh, r.y + r.h);
    for (int y = y0; y < y1; ++y) std::memset(px + static_cast<size_t>(y) * lw + x0, 0, static_cast<size_t>(x1 - x0) * sizeof(u32));
    d.over_dirty[slot] = SDL_Rect{0, 0, 0, 0};
  }
  return px;
}

GpuPresent::~GpuPresent() = default;

const std::string& GpuPresent::device_name() const { return d_->dev->name(); }

bool GpuPresent::reimport(ScanoutOut& out) {
  Impl& d = *d_;
  d.a->vkDeviceWaitIdle(d.vk->dev);
  d.pending_slot = -1; d.pending_buf = -1;
  d.fence_live[0] = d.fence_live[1] = false;
  std::string why;
  if (d.import_all(out, &why)) return true;
  std::fprintf(stderr, "gpu present: re-import after resize failed (%s)\n", why.c_str());
  return false;
}

void GpuPresent::flush(ScanoutOut& out) { d_->retire(out); }

bool GpuPresent::present(ScanoutOut& out, const u32* const fb[2], const View* views, int nviews, int rot, int lw, int lh, u8 inset_alpha,
                         SDL_Rect drawn, u32 grid) {
  Impl& d = *d_;
  const Api& a = *d.a;
  // The previous frame first: its fence, then its buffer to the tier. Only
  // then is a new buffer taken, so the tier's queue sees frames in order.
  auto now = [] { return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()); };
  u64 t0 = now();
  d.retire(out);
  u64 t1 = now(); d.t_retire += t1 - t0;
  u32* px = out.begin_frame();
  if (!px) return false;
  u64 t2 = now(); d.t_begin += t2 - t1;
  const int buf = out.current();
  if (buf < 0 || buf >= d.nbufs) { out.end_frame(); return false; }
  const int slot = static_cast<int>(d.frame % kSlots);
  if (d.fence_live[slot]) { a.vkWaitForFences(d.vk->dev, 1, &d.fence[slot], VK_TRUE, ~0ull); d.fence_live[slot] = false; }

  u32* dst = static_cast<u32*>(d.src.ptr) + static_cast<size_t>(slot) * kSlotWords;
  for (int s = 0; s < 2; ++s) std::memcpy(dst + static_cast<size_t>(s) * kFrameWords, fb[s], sizeof(u32) * kFrameWords);
  d.dev->flush(d.src, static_cast<size_t>(slot) * kSlotWords * sizeof(u32), kSlotWords * sizeof(u32));
  u64 t3 = now(); d.t_upload += t3 - t2;

  // LCD grid seam columns/rows per view, matching kern::scale_row_grid and
  // Gpu::emit_scaled: first panel pixel of a source run, for runs at least
  // ceil(scale) wide. Each axis decides independently.
  if (grid < 256) {
    u32* t = static_cast<u32*>(d.tab[slot].ptr);
    std::memset(t, 0, sizeof(u32) * 128);
    for (int v = 0; v < std::min(nviews, 2); ++v) {
      if (!views[v].grid) continue;
      const int dim[2] = {views[v].rect.w, views[v].rect.h}, srcn[2] = {256, 192};
      for (int axis = 0; axis < 2; ++axis) {
        const u32 n = static_cast<u32>(std::max(0, dim[axis])), sn = static_cast<u32>(srcn[axis]);
        if (n == 0 || n > 1024) continue;
        const u32 min_run = std::max<u32>(2, (n + sn - 1) / sn), pitch = n == 2 * sn ? 2 : 1;
        for (u32 s = 0; s < sn; ++s) {
          const u32 x0 = (s * n + sn - 1) / sn, x1 = ((s + 1) * n + sn - 1) / sn;
          if (x1 - x0 >= min_run && s % pitch == 0 && x0 < n) t[v * 64 + axis * 32 + (x0 >> 5)] |= 1u << (x0 & 31);
        }
      }
    }
    d.dev->flush(d.tab[slot], 0, sizeof(u32) * 128);
  }
  Push pc{};
  pc.a[0] = d.w; pc.a[1] = d.h; pc.a[2] = static_cast<u32>(lw); pc.a[3] = static_cast<u32>(lh);
  const bool has_over = drawn.w > 0 && drawn.h > 0 && d.over[slot] && d.over_lw == lw && d.over_lh == lh;
  if (has_over) {
    d.over_dirty[slot] = drawn;
    const int y0 = std::max(0, drawn.y), y1 = std::min(lh, drawn.y + drawn.h);
    if (y1 > y0) d.dev->flush(d.over[slot], static_cast<size_t>(y0) * lw * sizeof(u32), static_cast<size_t>(y1 - y0) * lw * sizeof(u32));
  }
  pc.b[0] = static_cast<u32>(rot); pc.b[1] = static_cast<u32>(nviews > 2 ? 2 : nviews); pc.b[2] = inset_alpha | (has_over ? 0x100u : 0u) | (std::min<u32>(grid, 256) << 16);
  pc.b[3] = static_cast<u32>(slot) * kSlotWords;
  for (int v = 0; v < static_cast<int>(pc.b[1]); ++v) {
    pc.rect[v][0] = views[v].rect.x; pc.rect[v][1] = views[v].rect.y; pc.rect[v][2] = views[v].rect.w; pc.rect[v][3] = views[v].rect.h;
    pc.view[v][0] = views[v].screen; pc.view[v][1] = views[v].shown ? 1 : 0; pc.view[v][2] = views[v].blends ? 1 : 0; pc.view[v][3] = views[v].grid ? 1 : 0;
  }

  VkCommandBuffer cb = d.cmd[slot];
  a.vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (a.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { out.end_frame(); return false; }
  // Every pixel is rewritten here, so UNDEFINED costs no load.
  VkImageMemoryBarrier ib{}; ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT; ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ib.image = d.bufs[buf].img; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
  a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe);
  a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 1, &d.bufs[buf].set[slot], 0, nullptr);
  a.vkCmdPushConstants(cb, d.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
  a.vkCmdDispatch(cb, (d.w + 15) / 16, (d.h + 7) / 8, 1);
  // Hand the writes to whoever reads the dma-buf next.
  VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  if (a.vkEndCommandBuffer(cb) != VK_SUCCESS) { out.end_frame(); return false; }
  VkSubmitInfo sub{}; sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; sub.commandBufferCount = 1; sub.pCommandBuffers = &cb;
  a.vkResetFences(d.vk->dev, 1, &d.fence[slot]);
  if (a.vkQueueSubmit(d.vk->queue, 1, &sub, d.fence[slot]) != VK_SUCCESS) { out.end_frame(); return false; }
  d.fence_live[slot] = true;
  static const bool waitnow = std::getenv("DS_PRESENT_WAITNOW") != nullptr;
  if (waitnow) { const u64 w0 = now(); a.vkWaitForFences(d.vk->dev, 1, &d.fence[slot], VK_TRUE, ~0ull); d.now_ns += now() - w0; ++d.now_n; }
  d.pending_slot = slot; d.pending_buf = buf;
  d.t_submit += now() - t3;
  ++d.frame; ++d.frames;
  return true;
}

} // namespace ds::sdl

#else  // no Vulkan in this build

namespace ds::sdl {
struct GpuPresent::Impl {};
std::unique_ptr<GpuPresent> GpuPresent::open(ScanoutOut&, std::string* why) { if (why) *why = "built without Vulkan"; return nullptr; }
GpuPresent::~GpuPresent() = default;
bool GpuPresent::reimport(ScanoutOut&) { return false; }
void GpuPresent::flush(ScanoutOut&) {}
bool GpuPresent::present(ScanoutOut&, const u32* const*, const View*, int, int, int, int, u8, SDL_Rect, u32) { return false; }
u32* GpuPresent::overlay(int, int) { return nullptr; }
const std::string& GpuPresent::device_name() const { static const std::string none; return none; }
} // namespace ds::sdl

#endif

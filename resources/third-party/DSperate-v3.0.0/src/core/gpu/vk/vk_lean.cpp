// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/vk/vk_lean.h"

#if !DSPERATE_VULKAN
// Built without the Vulkan headers: the raster declines, and the renderer
// keeps drawing on the CPU (Renderer3D::set_gpu reports the reason).
namespace ds::gpu::vk {
struct Lean::Impl {};
Lean::~Lean() = default;
std::unique_ptr<Lean> Lean::create(Device&, bool, std::string* why) { if (why) *why = "built without Vulkan"; return nullptr; }
bool Lean::msaa() const { return false; }
GpuPoly* Lean::poly_buffer() { return nullptr; }
GpuVert* Lean::vert_buffer() { return nullptr; }
u32* Lean::texel_buffer(u32* cap) { if (cap) *cap = 0; return nullptr; }
GpuPost* Lean::post_buffer() { return nullptr; }
bool Lean::submit(u32, u32, u32, const GpuFrame&) { return false; }
void Lean::wait() {}
void Lean::wait_all() {}
const u32* Lean::output() { return nullptr; }
const u32* Lean::newest_ready(bool) { return nullptr; }
bool Lean::has_frame() const { return false; }
u64 Lean::gpu_ns() const { return 0; }
u32 Lean::draws() const { return 0; }
Lean::Stats Lean::stats(bool) { return {}; }
} // namespace ds::gpu::vk
#else

#include "core/gpu/vk/vk_internal.h"
#include "core/gpu/vk/vk_shaders.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace ds::gpu::vk {

namespace {
constexpr u32 W = 256, H = 192;
constexpr u32 kTexelWords = 2u << 20;   // 8 MB of decoded texels
constexpr u32 kMaxDraws = 4096;
// Stencil layout: bit 7 drawn, bit 6 shadow mask, bit 5 the last write was
// translucent, bits 0-4 the pixel's polygon id as a per-frame code. The DS
// rules that read the id are "equal translucent ids don't blend" and
// "a shadow skips its own id (opaque or translucent)", so only the ids of
// translucent and shadow polygons need telling apart: they get codes 1..31
// per frame, every other id is code 0 (never equal to anything). A frame
// with more than 31 such ids folds them (id & 31: rare, and only a wrong
// same-id refusal).
constexpr u32 S_DRAWN = 0x80, S_MASK = 0x40, S_T = 0x20, S_CODE = 0x1F;
enum Pipe : u32 { P_OPAQUE = 0, P_TRANS_A, P_TRANS_B, P_TRANS_A_DW, P_TRANS_B_DW, P_MASK, P_SHADOW, P_SHADOW_PREP, P_MASK_CLEAR, P_OPAQUE_EARLY, P_TRANS_OPQ, P_COUNT };
}

struct Lean::Impl {
  const DeviceInternal* vk = nullptr;
  const Api* api = nullptr;
  Device* dev = nullptr;
  bool msaa = false, timing = false;
  double tick_ns = 0;
  VkFormat ds_format = VK_FORMAT_D24_UNORM_S8_UINT;

  static constexpr u32 kSlots = 4;
  // The frame's polygons, vertices and post tables are staged on the host
  // (built and read there: mapped write-combined memory is not for reading)
  // and copied once into the slot's own device buffers, so the next frame
  // is built while the GPU draws this one. The texel arena is shared and
  // append-only: a frame reads only what was in it when it was submitted.
  std::vector<GpuPoly> host_polys;
  std::vector<GpuVert> host_verts;
  GpuPost host_post{};
  Buffer polys[kSlots], verts[kSlots], post[kSlots], texels, out[kSlots];
  u32 tex_flushed = 0;   // arena words already flushed to the GPU
  VkBufferView texel_view = VK_NULL_HANDLE;
  struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
  Img col_ms, ds_ms, fog_ms, res;   // multisampled colour, depth/stencil and fog flag (transient); the 1x resolved record
  VkImageView ds_depth_view = VK_NULL_HANDLE;   // the depth aspect alone, for the resolve's input attachment
  VkShaderModule mod_vert = VK_NULL_HANDLE, mod_frag = VK_NULL_HANDLE, mod_frag_early = VK_NULL_HANDLE, mod_frag_opq = VK_NULL_HANDLE, mod_frag_trans = VK_NULL_HANDLE, mod_res = VK_NULL_HANDLE, mod_fs = VK_NULL_HANDLE;
  bool nostencil = false;   // DS_LEAN_NOSTENCIL=1: translucent pass B alone, always (debug)
  std::vector<std::pair<s64, u32>> key;   // the opaque prefix's sort keys (front to back, alpha-tested last)
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE, dsl_in = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set[kSlots] = {}, set_in = VK_NULL_HANDLE;
  VkRenderPass rp = VK_NULL_HANDLE;
  VkFramebuffer fb = VK_NULL_HANDLE;
  VkPipeline pipe[2][2][P_COUNT] = {};   // [wbuffer][depth-equal][kind]
  VkPipeline pipe_resolve = VK_NULL_HANDLE;
  VkCommandPool cpool = VK_NULL_HANDLE;
  VkCommandBuffer cmd[kSlots] = {};
  std::mutex m;                   // the slot state below: submit() on the emulation thread, newest_ready() on the compositor's
  VkFence fence[kSlots] = {};
  bool fence_live[kSlots] = {};   // submitted, not yet known finished
  bool valid[kSlots] = {};        // holds a finished, invalidated frame
  u64 gen = 0;                    // frames submitted; the newest is slot (gen - 1) % kSlots
  double submit_at[kSlots] = {};  // wall ms of each slot's submit (trace)
  u64 asked_gen = 0;              // the generation newest_ready() was last asked about (trace: the read gap)
  VkQueryPool qpool = VK_NULL_HANDLE;
  u64 last_gpu_ns = 0;
  u32 last_draws = 0;
  Lean::Stats st;

  void destroy() {
    if (!vk) return;
    const Api& a = *api;
    for (u32 i = 0; i < kSlots; ++i) if (fence_live[i]) { a.vkWaitForFences(vk->dev, 1, &fence[i], VK_TRUE, ~0ull); fence_live[i] = false; }
    for (auto& wb : pipe) for (auto& row : wb) for (auto& p : row) if (p) a.vkDestroyPipeline(vk->dev, p, nullptr);
    if (pipe_resolve) a.vkDestroyPipeline(vk->dev, pipe_resolve, nullptr);
    if (fb) a.vkDestroyFramebuffer(vk->dev, fb, nullptr);
    if (rp) a.vkDestroyRenderPass(vk->dev, rp, nullptr);
    if (ds_depth_view) a.vkDestroyImageView(vk->dev, ds_depth_view, nullptr);
    for (Img* im : {&col_ms, &ds_ms, &fog_ms, &res}) {
      if (im->view) a.vkDestroyImageView(vk->dev, im->view, nullptr);
      if (im->img) a.vkDestroyImage(vk->dev, im->img, nullptr);
      if (im->mem) a.vkFreeMemory(vk->dev, im->mem, nullptr);
    }
    if (qpool) a.vkDestroyQueryPool(vk->dev, qpool, nullptr);
    for (VkFence f : fence) if (f) a.vkDestroyFence(vk->dev, f, nullptr);
    if (cpool) a.vkDestroyCommandPool(vk->dev, cpool, nullptr);
    if (pool) a.vkDestroyDescriptorPool(vk->dev, pool, nullptr);
    if (layout) a.vkDestroyPipelineLayout(vk->dev, layout, nullptr);
    if (dsl) a.vkDestroyDescriptorSetLayout(vk->dev, dsl, nullptr);
    if (dsl_in) a.vkDestroyDescriptorSetLayout(vk->dev, dsl_in, nullptr);
    for (VkShaderModule* m : {&mod_vert, &mod_frag, &mod_frag_early, &mod_frag_opq, &mod_frag_trans, &mod_res, &mod_fs}) if (*m) a.vkDestroyShaderModule(vk->dev, *m, nullptr);
    if (texel_view) a.vkDestroyBufferView(vk->dev, texel_view, nullptr);
    if (texels) dev->free(texels);
    for (u32 i = 0; i < kSlots; ++i) for (Buffer* b : {&polys[i], &verts[i], &post[i], &out[i]}) if (*b) dev->free(*b);
  }
};

Lean::~Lean() { if (d_) d_->destroy(); }
bool Lean::msaa() const { return d_->msaa; }
GpuPoly* Lean::poly_buffer() { return d_->host_polys.data(); }
GpuVert* Lean::vert_buffer() { return d_->host_verts.data(); }
u32* Lean::texel_buffer(u32* cap) { if (cap) *cap = kTexelWords; return static_cast<u32*>(d_->texels.ptr); }
GpuPost* Lean::post_buffer() { return &d_->host_post; }
const u32* Lean::output() { return newest_ready(false); }
bool Lean::has_frame() const { return d_->gen != 0; }
u64 Lean::gpu_ns() const { return d_->last_gpu_ns; }
u32 Lean::draws() const { return d_->last_draws; }
Lean::Stats Lean::stats(bool reset) { std::lock_guard<std::mutex> lk(d_->m); Stats s = d_->st; if (reset) d_->st = Stats{}; return s; }
static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::unique_ptr<Lean> Lean::create(Device& dev, bool msaa, std::string* why) {
  auto fail_null = [&](const char* m) { if (why) *why = m; return nullptr; };
  const DeviceInternal* vk = dev.internal();
  if (!vk || !vk->api) return fail_null("no Vulkan context");
  const Api& a = *vk->api;
  if (!dev.limits().graphics || !a.vkCreateGraphicsPipelines || !a.vkCreateRenderPass || !a.vkCmdCopyImageToBuffer) return fail_null("no graphics queue");
  std::unique_ptr<Lean> self(new Lean);
  self->d_ = std::make_unique<Impl>();
  Impl& d = *self->d_;
  d.vk = vk; d.api = &a; d.dev = &dev;
  auto fail = [&](const char* m) { if (why) *why = m; return nullptr; };
  d.msaa = msaa && dev.limits().msaa4 && !(std::getenv("DS_LEAN_MSAA") && std::atoi(std::getenv("DS_LEAN_MSAA")) == 0);   // DS_LEAN_MSAA=0: off regardless (debug)
  const VkSampleCountFlagBits samples = d.msaa ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;

  // Buffers.
  d.host_polys.resize(DS_MAX_POLYS);
  d.host_verts.resize(DS_MAX_VERTS);
  d.texels = dev.alloc(sizeof(u32) * kTexelWords, Access::CpuWrite);
  if (!d.texels) return fail("buffer allocation");
  for (u32 i = 0; i < Impl::kSlots; ++i) {
    d.polys[i] = dev.alloc(sizeof(GpuPoly) * DS_MAX_POLYS, Access::CpuWrite);
    d.verts[i] = dev.alloc(sizeof(GpuVert) * DS_MAX_VERTS, Access::CpuWrite);
    d.post[i] = dev.alloc(sizeof(GpuPost), Access::CpuWrite);
    d.out[i] = dev.alloc(sizeof(u32) * W * H, Access::CpuRead);
    if (!d.polys[i] || !d.verts[i] || !d.post[i] || !d.out[i]) return fail("buffer allocation");
  }
  {
    VkBufferViewCreateInfo bv{}; bv.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO; bv.buffer = vk_buf(d.texels); bv.format = VK_FORMAT_R32_UINT; bv.offset = 0; bv.range = VK_WHOLE_SIZE;
    if (a.vkCreateBufferView(vk->dev, &bv, nullptr, &d.texel_view) != VK_SUCCESS) return fail("texel buffer view");
  }

  // Shaders.
  auto shader = [&](Spirv s, VkShaderModule* out) {
    VkShaderModuleCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; ci.codeSize = s.bytes; ci.pCode = s.code;
    return a.vkCreateShaderModule(vk->dev, &ci, nullptr, out) == VK_SUCCESS;
  };
  if (!shader(shader_lean_vert(), &d.mod_vert) || !shader(shader_lean_frag(), &d.mod_frag) || !shader(shader_lean_frag_early(), &d.mod_frag_early) || !shader(shader_lean_frag_opq(), &d.mod_frag_opq) || !shader(shader_lean_frag_trans(), &d.mod_frag_trans) || !shader(d.msaa ? shader_lean_resolve() : shader_lean_resolve_1x(), &d.mod_res) || !shader(shader_tri_fs(), &d.mod_fs))
    return fail("lean shaders rejected by the driver");
  d.nostencil = std::getenv("DS_LEAN_NOSTENCIL") && std::atoi(std::getenv("DS_LEAN_NOSTENCIL")) != 0;

  // Descriptors: set 0 = polys, verts, texels (texel buffer), post; set 1 = the MS colour as input attachment.
  {
    VkDescriptorSetLayoutBinding b[4]{};
    for (u32 i = 0; i < 4; ++i) { b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; b[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
    VkDescriptorSetLayoutCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; ci.bindingCount = 4; ci.pBindings = b;
    if (a.vkCreateDescriptorSetLayout(vk->dev, &ci, nullptr, &d.dsl) != VK_SUCCESS) return fail("descriptor layout");
    VkDescriptorSetLayoutBinding ib[3]{};
    for (u32 i = 0; i < 3; ++i) { ib[i].binding = i; ib[i].descriptorCount = 1; ib[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ib[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; }
    VkDescriptorSetLayoutCreateInfo ci2{}; ci2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; ci2.bindingCount = 3; ci2.pBindings = ib;
    if (a.vkCreateDescriptorSetLayout(vk->dev, &ci2, nullptr, &d.dsl_in) != VK_SUCCESS) return fail("input attachment layout");
    VkDescriptorSetLayout sets[2] = {d.dsl, d.dsl_in};
    VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pcr.size = sizeof(GpuFrame);
    VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pli.setLayoutCount = 2; pli.pSetLayouts = sets; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (a.vkCreatePipelineLayout(vk->dev, &pli, nullptr, &d.layout) != VK_SUCCESS) return fail("pipeline layout");
    VkDescriptorPoolSize psz[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * Impl::kSlots}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, Impl::kSlots}, {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 3}};
    VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpi.maxSets = Impl::kSlots + 1; dpi.poolSizeCount = 3; dpi.pPoolSizes = psz;
    if (a.vkCreateDescriptorPool(vk->dev, &dpi, nullptr, &d.pool) != VK_SUCCESS) return fail("descriptor pool");
    VkDescriptorSetLayout layouts[Impl::kSlots + 1];
    for (u32 i = 0; i < Impl::kSlots; ++i) layouts[i] = d.dsl;
    layouts[Impl::kSlots] = d.dsl_in;
    VkDescriptorSet got[Impl::kSlots + 1];
    VkDescriptorSetAllocateInfo dsa{}; dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsa.descriptorPool = d.pool; dsa.descriptorSetCount = Impl::kSlots + 1; dsa.pSetLayouts = layouts;
    if (a.vkAllocateDescriptorSets(vk->dev, &dsa, got) != VK_SUCCESS) return fail("descriptor sets");
    for (u32 i = 0; i < Impl::kSlots; ++i) d.set[i] = got[i];
    d.set_in = got[Impl::kSlots];
    for (u32 sl = 0; sl < Impl::kSlots; ++sl) {
      VkDescriptorBufferInfo bi[4]{};
      const Buffer* src[4] = {&d.polys[sl], &d.verts[sl], &d.texels, &d.post[sl]};
      VkWriteDescriptorSet w[4]{};
      for (u32 i = 0; i < 4; ++i) {
        bi[i].buffer = vk_buf(*src[i]); bi[i].range = VK_WHOLE_SIZE;
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = d.set[sl]; w[i].dstBinding = i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
      }
      w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; w[2].pBufferInfo = nullptr; w[2].pTexelBufferView = &d.texel_view;
      a.vkUpdateDescriptorSets(vk->dev, 4, w, 0, nullptr);
    }
  }

  // Images.
  VkPhysicalDeviceMemoryProperties mp{}; a.vkGetPhysicalDeviceMemoryProperties(vk->phys, &mp);
  auto make_image = [&](VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, Impl::Img& o, VkSampleCountFlagBits smp) {
    VkImageCreateInfo ii{}; ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt; ii.extent = {W, H, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = smp; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (a.vkCreateImage(vk->dev, &ii, nullptr, &o.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr{}; a.vkGetImageMemoryRequirements(vk->dev, o.img, &mr);
    u32 type = ~0u;
    if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
      for (u32 t = 0; t < mp.memoryTypeCount && type == ~0u; ++t) if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) type = t;
    for (u32 t = 0; t < mp.memoryTypeCount && type == ~0u; ++t) if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
    for (u32 t = 0; t < mp.memoryTypeCount && type == ~0u; ++t) if (mr.memoryTypeBits & (1u << t)) type = t;
    if (type == ~0u) return false;
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = mr.size; ai.memoryTypeIndex = type;
    if (a.vkAllocateMemory(vk->dev, &ai, nullptr, &o.mem) != VK_SUCCESS) return false;
    if (a.vkBindImageMemory(vk->dev, o.img, o.mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = o.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.subresourceRange = {aspect, 0, 1, 0, 1};
    return a.vkCreateImageView(vk->dev, &vi, nullptr, &o.view) == VK_SUCCESS;
  };
  {
    VkFormatProperties fp{};
    // Float depth first: W-buffer mode stores 1/w, and at DS depths of tens
    // of thousands neighbouring values are 1e-9 apart, below a 24-bit fixed
    // step (6e-8), so a shadow volume's face and the ground it meets compared
    // equal. A float keeps the relative precision. DS_LEAN_D24=1 forces D24S8.
    a.vkGetPhysicalDeviceFormatProperties(vk->phys, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
    const bool d32 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) && !(std::getenv("DS_LEAN_D24") && std::atoi(std::getenv("DS_LEAN_D24")) != 0);
    d.ds_format = d32 ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
  }
  const VkImageUsageFlags transient = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | transient, VK_IMAGE_ASPECT_COLOR_BIT, d.col_ms, samples)) return fail("MSAA colour attachment");
  if (!make_image(d.ds_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | transient, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, d.ds_ms, samples)) return fail("MSAA depth/stencil attachment");
  {
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = d.ds_ms.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = d.ds_format; vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (a.vkCreateImageView(vk->dev, &vi, nullptr, &d.ds_depth_view) != VK_SUCCESS) return fail("depth input view");
  }
  if (!make_image(VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | transient, VK_IMAGE_ASPECT_COLOR_BIT, d.fog_ms, samples)) return fail("MSAA fog attachment");
  if (!make_image(VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, d.res, VK_SAMPLE_COUNT_1_BIT)) return fail("resolve attachment");
  {
    VkDescriptorImageInfo di[3]{};
    di[0].imageView = d.col_ms.view; di[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    di[1].imageView = d.ds_depth_view; di[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    di[2].imageView = d.fog_ms.view; di[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w[3]{};
    for (u32 i = 0; i < 3; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = d.set_in; w[i].dstBinding = i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; w[i].pImageInfo = &di[i]; }
    a.vkUpdateDescriptorSets(vk->dev, 3, w, 0, nullptr);
  }

  // Render pass: subpass 0 draws into the MS colour + depth/stencil, subpass 1 resolves into the 1x record.
  {
    VkAttachmentDescription at[4]{};
    at[0].format = VK_FORMAT_R8G8B8A8_UNORM; at[0].samples = samples; at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[1].format = d.ds_format; at[1].samples = samples; at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    at[2].format = VK_FORMAT_R32_UINT; at[2].samples = VK_SAMPLE_COUNT_1_BIT; at[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    at[3] = at[0]; at[3].format = VK_FORMAT_R8_UNORM;   // the fog flag: cleared to the rear plane's, never stored
    VkAttachmentReference c0[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}, ds0{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference in1[3] = {{0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}, {3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}, c1{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp[2]{};
    sp[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp[0].colorAttachmentCount = 2; sp[0].pColorAttachments = c0; sp[0].pDepthStencilAttachment = &ds0;
    sp[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp[1].inputAttachmentCount = 3; sp[1].pInputAttachments = in1; sp[1].colorAttachmentCount = 1; sp[1].pColorAttachments = &c1;
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = 0; dep[0].dstSubpass = 1; dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; dep[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; dep[0].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT; dep[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    dep[1].srcSubpass = 1; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL; dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rpi.attachmentCount = 4; rpi.pAttachments = at; rpi.subpassCount = 2; rpi.pSubpasses = sp; rpi.dependencyCount = 2; rpi.pDependencies = dep;
    if (a.vkCreateRenderPass(vk->dev, &rpi, nullptr, &d.rp) != VK_SUCCESS) return fail("render pass");
    VkImageView views[4] = {d.col_ms.view, d.ds_ms.view, d.res.view, d.fog_ms.view};
    VkFramebufferCreateInfo fbi{}; fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fbi.renderPass = d.rp; fbi.attachmentCount = 4; fbi.pAttachments = views; fbi.width = W; fbi.height = H; fbi.layers = 1;
    if (a.vkCreateFramebuffer(vk->dev, &fbi, nullptr, &d.fb) != VK_SUCCESS) return fail("framebuffer");
  }

  // Pipelines: one vertex/fragment pair, the DS rules in depth, stencil and blend state.
  auto pipeline = [&](bool wbuf, bool eq, u32 kind, VkPipeline* out) {
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    // The mask clear is a full-screen triangle; it and the shadow prep touch the stencil only (no fragment stage).
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = kind == P_MASK_CLEAR ? d.mod_fs : d.mod_vert; st[0].pName = "main";
    const bool trans_pass = kind == P_TRANS_A || kind == P_TRANS_B || kind == P_TRANS_A_DW || kind == P_TRANS_B_DW;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = kind == P_OPAQUE_EARLY ? d.mod_frag_early : kind == P_TRANS_OPQ ? d.mod_frag_opq : trans_pass ? d.mod_frag_trans : d.mod_frag; st[1].pName = "main";
    const bool stencil_only = kind == P_MASK_CLEAR || kind == P_SHADOW_PREP;
    VkPipelineVertexInputStateCreateInfo vin{}; vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0.f, 0.f, static_cast<float>(W), static_cast<float>(H), 0.f, 1.f}; VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps{}; vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo dss{}; dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = stencil_only ? VK_FALSE : VK_TRUE;
    dss.depthCompareOp = wbuf ? (eq ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_GREATER) : (eq ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS);
    dss.depthWriteEnable = (kind == P_OPAQUE || kind == P_OPAQUE_EARLY || kind == P_TRANS_OPQ || kind == P_TRANS_A_DW || kind == P_TRANS_B_DW) ? VK_TRUE : VK_FALSE;
    dss.stencilTestEnable = VK_TRUE;
    VkStencilOpState so{};
    so.failOp = VK_STENCIL_OP_KEEP; so.depthFailOp = VK_STENCIL_OP_KEEP; so.passOp = VK_STENCIL_OP_KEEP; so.compareOp = VK_COMPARE_OP_ALWAYS; so.compareMask = 0; so.writeMask = 0; so.reference = 0;
    switch (kind) {
      case P_OPAQUE: case P_OPAQUE_EARLY: case P_TRANS_OPQ:   // drawn, not translucent, this code (reference = drawn | code, set per draw)
        so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_DRAWN | S_T | S_CODE; break;
      case P_TRANS_A: case P_TRANS_A_DW:   // over an undrawn pixel: written as is, now drawn, translucent, this code. The reference
        // (drawn | T | code, set per draw) serves the compare and the replace both, so the "undrawn" test is NOT_EQUAL on the drawn bit.
        so.compareOp = VK_COMPARE_OP_NOT_EQUAL; so.compareMask = S_DRAWN; so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_DRAWN | S_T | S_CODE; break;
      case P_TRANS_B: case P_TRANS_B_DW:   // over a drawn pixel that is not a translucent one of the same code: blended
        so.compareOp = d.nostencil ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_NOT_EQUAL; so.compareMask = S_DRAWN | S_T | S_CODE; so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_DRAWN | S_T | S_CODE; break;
      case P_MASK:     // shadow mask: where the depth test fails the mask bit is set; nothing else changes
        so.depthFailOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_MASK; so.reference = S_MASK; break;
      case P_MASK_CLEAR:   // a new mask set: the mask bit off everywhere (reference 0)
        so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_MASK; break;
      case P_SHADOW_PREP:   // the shadow's own id: the mask bit off where the code matches (reference = code, its mask bit 0)
        so.compareOp = VK_COMPARE_OP_EQUAL; so.compareMask = S_CODE; so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_MASK; break;
      case P_SHADOW:   // shadow: drawn (blended) where the mask bit is set (NOT_EQUAL to the reference's clear bit), and the
        // pixel then takes the shadow's id with the mask bit cleared (reference = T | code, replaced into mask | T | code):
        // a face sharing an edge in the same draw finds the bit clear, and a later run of the same id is kept off by
        // the prep pass (code equal). Faces and overlapping volumes darken once, as on the DS.
        so.compareOp = VK_COMPARE_OP_NOT_EQUAL; so.compareMask = S_MASK; so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_MASK | S_T | S_CODE; break;
    }
    dss.front = so; dss.back = so;
    VkPipelineColorBlendAttachmentState cba[2]{};
    cba[0].colorWriteMask = (kind == P_MASK || stencil_only) ? 0u : 0xFu;
    cba[1].colorWriteMask = (kind == P_MASK || stencil_only) ? 0u : 0x1u;
    const bool blend = kind == P_TRANS_B || kind == P_TRANS_B_DW || kind == P_SHADOW;
    if (blend) {
      cba[0].blendEnable = VK_TRUE;
      cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba[0].colorBlendOp = VK_BLEND_OP_ADD;
      cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba[0].alphaBlendOp = VK_BLEND_OP_MAX;
      // Fog flag: kept only if both the layer beneath and this polygon carry it.
      cba[1].blendEnable = VK_TRUE;
      cba[1].srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE; cba[1].colorBlendOp = VK_BLEND_OP_MIN;
      cba[1].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba[1].alphaBlendOp = VK_BLEND_OP_MIN;
    }
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = 2; cbs.pAttachments = cba;
    const VkDynamicState dyn = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
    VkPipelineDynamicStateCreateInfo dys{}; dys.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dys.dynamicStateCount = 1; dys.pDynamicStates = &dyn;
    VkGraphicsPipelineCreateInfo gpi{}; gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gpi.stageCount = stencil_only ? 1 : 2; gpi.pStages = st;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &dss; gpi.pColorBlendState = &cbs; gpi.pDynamicState = &dys; gpi.layout = d.layout; gpi.renderPass = d.rp; gpi.subpass = 0;
    return a.vkCreateGraphicsPipelines(vk->dev, VK_NULL_HANDLE, 1, &gpi, nullptr, out) == VK_SUCCESS;
  };
  for (u32 wb = 0; wb < 2; ++wb) for (u32 eq = 0; eq < 2; ++eq) for (u32 k = 0; k < P_COUNT; ++k) if (!pipeline(wb != 0, eq != 0, k, &d.pipe[wb][eq][k])) return fail("graphics pipeline");
  {
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = d.mod_fs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = d.mod_res; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{}; vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0.f, 0.f, static_cast<float>(W), static_cast<float>(H), 0.f, 1.f}; VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps{}; vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xFu;
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gpi{}; gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gpi.stageCount = 2; gpi.pStages = st;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cbs; gpi.layout = d.layout; gpi.renderPass = d.rp; gpi.subpass = 1;
    if (a.vkCreateGraphicsPipelines(vk->dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &d.pipe_resolve) != VK_SUCCESS) return fail("resolve pipeline");
  }

  // Commands, fence, timestamps.
  {
    VkCommandPoolCreateInfo cpi{}; cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = vk->qfam;
    if (a.vkCreateCommandPool(vk->dev, &cpi, nullptr, &d.cpool) != VK_SUCCESS) return fail("command pool");
    VkCommandBufferAllocateInfo cai{}; cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cai.commandPool = d.cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = Impl::kSlots;
    if (a.vkAllocateCommandBuffers(vk->dev, &cai, d.cmd) != VK_SUCCESS) return fail("command buffers");
    VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (u32 i = 0; i < Impl::kSlots; ++i) if (a.vkCreateFence(vk->dev, &fi, nullptr, &d.fence[i]) != VK_SUCCESS) return fail("fence");
    d.timing = std::getenv("DS_VK_TIMING") && dev.limits().timestamp_period_ns > 0 && a.vkCreateQueryPool;
    if (d.timing) {
      VkQueryPoolCreateInfo qi{}; qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2 * Impl::kSlots;
      if (a.vkCreateQueryPool(vk->dev, &qi, nullptr, &d.qpool) != VK_SUCCESS) d.timing = false;
      d.tick_ns = dev.limits().timestamp_period_ns;
    }
  }
  self->ready_ = true;
  std::fprintf(stderr, "gpu lean: 1x%s, %s depth/stencil\n", d.msaa ? " with 4x MSAA" : " (no MSAA)", d.ds_format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
  return self;
}

bool Lean::submit(u32 npoly, u32 nvert, u32 ntexels, const GpuFrame& fin) {
  Impl& d = *d_;
  const Api& a = *d.api;
  const DeviceInternal* vk = d.vk;
  if (npoly > DS_MAX_POLYS || nvert > DS_MAX_VERTS || ntexels > kTexelWords) return false;
  const u32 slot = static_cast<u32>(d.gen % Impl::kSlots);
  const double t_in = now_ms();
  // The slot's previous frame must be off the GPU (four back: long done).
  {
    std::lock_guard<std::mutex> lk(d.m);
    if (d.fence_live[slot]) { a.vkWaitForFences(vk->dev, 1, &d.fence[slot], VK_TRUE, ~0ull); d.fence_live[slot] = false; }
    d.valid[slot] = false;
  }
  const double t_prep = now_ms();
  // The arena's new texels (append-only: earlier frames' ranges are unchanged).
  if (ntexels > d.tex_flushed) { d.dev->flush(d.texels, sizeof(u32) * d.tex_flushed, sizeof(u32) * (ntexels - d.tex_flushed)); d.tex_flushed = ntexels; }
  std::memcpy(d.verts[slot].ptr, d.host_verts.data(), sizeof(GpuVert) * nvert);
  d.dev->flush(d.verts[slot], 0, sizeof(GpuVert) * nvert);
  std::memcpy(d.post[slot].ptr, &d.host_post, sizeof(GpuPost));
  d.dev->flush(d.post[slot], 0, sizeof(GpuPost));

  GpuFrame f = fin;
  const bool wbuf = (f.flags & DS_FF_WBUFFER) != 0;
  GpuPoly* gpw = d.host_polys.data();
  const GpuPoly* gp = gpw;
  const GpuVert* gv = d.host_verts.data();
  // Each polygon's texcoord range for the fragment stage's clamp (row_base / pad_, unused here otherwise).
  for (u32 k = 0; k < npoly; ++k) {
    GpuPoly& p = gpw[k];
    s32 s0 = 0x7FFF, s1 = -0x8000, t0 = 0x7FFF, t1 = -0x8000;
    for (u32 v = 0; v < p.nverts; ++v) { const GpuVert& q = gv[p.first_vert + v]; s0 = std::min(s0, q.s); s1 = std::max(s1, q.s); t0 = std::min(t0, q.t); t1 = std::max(t1, q.t); }
    auto pk = [](s32 lo, s32 hi) { return static_cast<u32>(std::clamp(lo + 32768, 0, 65535)) | (static_cast<u32>(std::clamp(hi + 32768, 0, 65535)) << 16); };
    p.row_base = pk(s0, s1); p.pad_ = pk(t0, t1);
  }

  // The frame's id codes (see the stencil layout).
  u8 code[64] = {};
  {
    u32 next = 1;
    auto want = [&](u32 id) { if (!code[id]) code[id] = static_cast<u8>(next <= 31 ? next++ : (id & 31)); };
    for (u32 k = 0; k < npoly; ++k) if (gp[k].flags & (DS_PF_TRANSLUCENT | DS_PF_SHADOW)) want((gp[k].attr >> 24) & 0x3F);
  }
  auto code_of = [&](const GpuPoly& p) -> u32 { return code[(p.attr >> 24) & 0x3F]; };
  if (std::getenv("DS_LEAN_LIST") && std::atoi(std::getenv("DS_LEAN_LIST")) != 0) {   // id histogram per kind (debug)
    u32 hist[64][4] = {};
    for (u32 k = 0; k < npoly; ++k) { const u32 id = (gp[k].attr >> 24) & 0x3F, kind = (gp[k].flags & DS_PF_SHADOW_MASK) ? 3 : (gp[k].flags & DS_PF_SHADOW) ? 2 : (gp[k].flags & DS_PF_TRANSLUCENT) ? 1 : 0; ++hist[id][kind]; }
    std::fprintf(stderr, "lean ids gen %llu:", (unsigned long long)d.gen);
    for (u32 id = 0; id < 64; ++id) if (hist[id][0] | hist[id][1] | hist[id][2] | hist[id][3]) std::fprintf(stderr, " id%u[o%u t%u s%u m%u]", id, hist[id][0], hist[id][1], hist[id][2], hist[id][3]);
    std::fputc('\n', stderr);
  }

  const double t_rec = now_ms();
  VkCommandBuffer cb = d.cmd[slot];
  a.vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (a.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
  if (d.timing) { a.vkCmdResetQueryPool(cb, d.qpool, 2 * slot, 2); a.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.qpool, 2 * slot); }   // bottom of pipe: after the queue ahead, so the span is this frame's own

  // Clear: the rear plane's colour and alpha; depth; stencil drawn where the rear plane has alpha.
  VkClearValue cv[4]{};
  {
    const u32 c = f.clear_color, ca = (c >> 24) & 0x1F;
    cv[0].color.float32[0] = static_cast<float>(c & 0x3F) / 255.f; cv[0].color.float32[1] = static_cast<float>((c >> 8) & 0x3F) / 255.f; cv[0].color.float32[2] = static_cast<float>((c >> 16) & 0x3F) / 255.f;
    cv[0].color.float32[3] = ca == 0 ? 0.f : static_cast<float>(std::min(255u, (ca + 1) * 8)) / 255.f;
    const float cz = static_cast<float>(f.clear_depth & 0xFFFFFF);
    cv[1].depthStencil.depth = wbuf ? 1.f / std::max(cz, 1.f) : std::min(1.f, cz / 16777215.f);
    cv[1].depthStencil.stencil = ca ? (S_DRAWN | code[(f.clear_attr >> 24) & 0x3F]) : 0;
    cv[3].color.float32[0] = (f.clear_attr & 0x8000) ? 1.f : 0.f;   // the rear plane's fog bit
  }
  VkRenderPassBeginInfo rbi{}; rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rbi.renderPass = d.rp; rbi.framebuffer = d.fb; rbi.renderArea = {{0, 0}, {W, H}}; rbi.clearValueCount = 4; rbi.pClearValues = cv;
  a.vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 1, &d.set[slot], 0, nullptr);
  a.vkCmdPushConstants(cb, d.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GpuFrame), &f);
  u32 draws = 0;
  VkPipeline bound = VK_NULL_HANDLE;
  auto use = [&](VkPipeline p) { if (p != bound) { a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); bound = p; } };
  auto draw = [&](u32 first, u32 count) { a.vkCmdDraw(cb, 24, count, 0, first); ++draws; };   // 8 fan triangles at most (10 vertices)

  // Opaque prefix: order is free (a depth test decides), so it is redrawn
  // for the hardware: polygons that cannot fail the alpha test first, front
  // to back with the early depth test, then the cut-out textured ones with
  // the discard. The GpuPoly records themselves are reordered on the way
  // into the slot's buffer (vertices are referenced by index, so nothing
  // else moves).
  const u32 first_ordered = std::min(f.first_ordered, npoly);
  u32 n_early = 0;
  GpuPoly* const out_p = static_cast<GpuPoly*>(d.polys[slot].ptr);
  if (first_ordered) {
    auto nearest = [&](const GpuPoly& p) -> s32 {   // the polygon's nearest vertex depth (W-buffer: smaller w is nearer too)
      const GpuVert* v = gv + p.first_vert;
      s32 zmin = 0x7FFFFFFF; for (u32 k = 0; k < p.nverts; ++k) zmin = std::min(zmin, v[k].z); return zmin;
    };
    d.key.clear(); d.key.reserve(first_ordered);
    // Key: alpha-tested last, then by stencil code (one reference per run; nearly all are code 0), then near to far.
    for (u32 k = 0; k < first_ordered; ++k) { const bool at = (gp[k].flags & DS_PF_TEX_ALPHA) != 0, eq = (gp[k].attr & 0x4000u) != 0; d.key.emplace_back((at ? (s64{1} << 40) : 0) + (eq ? (s64{1} << 38) : 0) + (s64{code_of(gp[k])} << 32) + nearest(gp[k]), k); }
    std::sort(d.key.begin(), d.key.end());
    for (u32 k = 0; k < first_ordered; ++k) { out_p[k] = gp[d.key[k].second]; if (!(out_p[k].flags & DS_PF_TEX_ALPHA)) ++n_early; }
    for (u32 k = 0; k < first_ordered;) {
      const u32 c = code_of(out_p[k]); const bool early = k < n_early, eq = (out_p[k].attr & 0x4000u) != 0;
      u32 j = k + 1;
      while (j < first_ordered && code_of(out_p[j]) == c && (j < n_early) == early && ((out_p[j].attr & 0x4000u) != 0) == eq) ++j;
      use(d.pipe[wbuf][eq][early ? P_OPAQUE_EARLY : P_OPAQUE]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_DRAWN | c); draw(k, j - k);
      k = j;
    }
  }
  // The tail, in list order: runs of polygons that take the same passes.
  u32 i = first_ordered;
  bool prev_mask = false, any_mask = false;
  while (i < npoly) {
    const GpuPoly& p = gp[i];
    const u32 mode = (p.attr >> 4) & 3, id = (p.attr >> 24) & 0x3F, c = code[id];
    const bool mask = (p.flags & DS_PF_SHADOW_MASK) != 0, shadow = (p.flags & DS_PF_SHADOW) != 0;
    const bool trans = (p.flags & DS_PF_TRANSLUCENT) != 0, dw = (p.attr & (1u << 11)) != 0, eq = (p.attr & 0x4000u) != 0;
    VkPipeline* const pipes = d.pipe[wbuf][eq];
    u32 j = i + 1;
    while (j < npoly) {
      const GpuPoly& q = gp[j];
      if (((q.attr >> 4) & 3) != mode || ((q.attr >> 24) & 0x3F) != id || ((q.attr & 0x4000u) != 0) != eq || ((q.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW | DS_PF_TRANSLUCENT)) != (p.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW | DS_PF_TRANSLUCENT))) || ((q.attr & (1u << 11)) != 0) != dw) break;
      ++j;
    }
    const u32 n = j - i;
    // DS_LEAN_LIST=1: the tail's runs (kind, id, count, depth range) for the frames with shadows, to stderr.
    static const bool list_trace = std::getenv("DS_LEAN_LIST") && std::atoi(std::getenv("DS_LEAN_LIST")) != 0;
    if (list_trace && (mask || shadow)) {
      s32 zlo = 0x7FFFFFFF, zhi = -1; for (u32 k = i; k < j; ++k) for (u32 v = 0; v < gp[k].nverts; ++v) { const s32 z = gv[gp[k].first_vert + v].z; zlo = std::min(zlo, z); zhi = std::max(zhi, z); }
      std::fprintf(stderr, "lean list gen %llu: %s id %u code %u x%u polys %u..%u z %d..%d attr %08x\n", (unsigned long long)d.gen, mask ? "MASK" : "SHADOW", id, c, n, i, j - 1, zlo, zhi, p.attr);
    }
    if (mask) {
      // A mask polygon after a non-mask one starts a new set: the DS clears the mask stencil (per line; here for the frame).
      static const bool no_clear = std::getenv("DS_LEAN_NOMASKCLEAR") != nullptr;   // debug
      if (any_mask && !prev_mask && !no_clear) { use(pipes[P_MASK_CLEAR]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0); a.vkCmdDraw(cb, 3, 1, 0, 0); ++draws; }
      any_mask = true;
      use(pipes[P_MASK]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_MASK); draw(i, n);
    } else if (shadow) {
      static const bool no_prep = std::getenv("DS_LEAN_NOPREP") != nullptr;   // debug
      if (!no_prep) { use(pipes[P_SHADOW_PREP]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, c); draw(i, n); }
      use(pipes[P_SHADOW]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_T | c); draw(i, n);
    } else if (trans) {
      // The run's alpha-31 pixels first, as opaque (depth written, no translucent flag), then the rest.
      use(pipes[P_TRANS_OPQ]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_DRAWN | c); draw(i, n);
      if (!d.nostencil) { use(pipes[dw ? P_TRANS_A_DW : P_TRANS_A]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_DRAWN | S_T | c); draw(i, n); }
      use(pipes[dw ? P_TRANS_B_DW : P_TRANS_B]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_DRAWN | S_T | c); draw(i, n);
    } else {
      use(pipes[P_OPAQUE]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_DRAWN | c); draw(i, n);
    }
    prev_mask = mask;
    i = j;
    if (draws > kMaxDraws) break;
  }
  if (npoly > first_ordered) std::memcpy(out_p + first_ordered, gp + first_ordered, sizeof(GpuPoly) * (npoly - first_ordered));
  if (npoly) d.dev->flush(d.polys[slot], 0, sizeof(GpuPoly) * npoly);
  a.vkCmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
  a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_resolve);
  a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 1, 1, &d.set_in, 0, nullptr);
  a.vkCmdDraw(cb, 3, 1, 0, 0);
  a.vkCmdEndRenderPass(cb);
  // The record to the CPU-readable buffer.
  VkBufferImageCopy rg{}; rg.bufferRowLength = W; rg.bufferImageHeight = H; rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rg.imageExtent = {W, H, 1};
  a.vkCmdCopyImageToBuffer(cb, d.res.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_buf(d.out[slot]), 1, &rg);
  VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  if (d.timing) a.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.qpool, 2 * slot + 1);
  if (a.vkEndCommandBuffer(cb) != VK_SUCCESS) return false;
  const double t_q = now_ms();
  VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
  a.vkResetFences(vk->dev, 1, &d.fence[slot]);
  if (vk->queue2) {
    if (a.vkQueueSubmit(vk->queue2, 1, &si, d.fence[slot]) != VK_SUCCESS) return false;   // only this thread submits here
  } else {
    std::lock_guard<std::mutex> lk(d.dev->queue_mutex());
    if (a.vkQueueSubmit(vk->queue, 1, &si, d.fence[slot]) != VK_SUCCESS) return false;
  }
  std::lock_guard<std::mutex> lk(d.m);
  d.st.record_ms += t_q - t_rec; d.st.queue_ms += now_ms() - t_q; d.st.slot_wait_ms += t_prep - t_in; d.st.prep_ms += t_rec - t_prep;
  d.fence_live[slot] = true; d.submit_at[slot] = t_in;
  d.last_draws = draws;
  ++d.gen;
  return true;
}

// Slot `slot` finished (waited or seen signalled): make its record readable.
static void finish_slot(Lean::Impl& d, u32 slot) {
  const Api& a = *d.api;
  d.fence_live[slot] = false;
  d.dev->invalidate(d.out[slot], 0, sizeof(u32) * W * H);
  d.valid[slot] = true;
  if (d.timing) {
    u64 t[2] = {0, 0};
    if (a.vkGetQueryPoolResults(d.vk->dev, d.qpool, 2 * slot, 2, sizeof t, t, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS && t[1] > t[0])
    { d.last_gpu_ns = static_cast<u64>(static_cast<double>(t[1] - t[0]) * d.tick_ns); d.st.gpu_ms += static_cast<double>(d.last_gpu_ns) / 1e6; ++d.st.timed; }
  }
}

static void wait_locked(Lean::Impl& d) {
  if (!d.gen) return;
  const u32 slot = static_cast<u32>((d.gen - 1) % Lean::Impl::kSlots);
  if (!d.fence_live[slot]) return;
  d.api->vkWaitForFences(d.vk->dev, 1, &d.fence[slot], VK_TRUE, ~0ull);
  d.st.lat_ms += now_ms() - d.submit_at[slot]; ++d.st.lat_n;
  finish_slot(d, slot);
}

void Lean::wait() { std::lock_guard<std::mutex> lk(d_->m); wait_locked(*d_); }

void Lean::wait_all() {
  Impl& d = *d_;
  std::lock_guard<std::mutex> lk(d.m);
  for (u32 i = 0; i < Impl::kSlots; ++i) if (d.fence_live[i]) { d.api->vkWaitForFences(d.vk->dev, 1, &d.fence[i], VK_TRUE, ~0ull); finish_slot(d, i); }
  d.tex_flushed = 0;
}

const u32* Lean::newest_ready(bool allow_lag) {
  Impl& d = *d_;
  std::lock_guard<std::mutex> lk(d.m);
  if (!d.gen) return nullptr;
  const Api& a = *d.api;
  const u32 newest = static_cast<u32>((d.gen - 1) % Impl::kSlots);
  if (d.asked_gen != d.gen) { d.asked_gen = d.gen; d.st.gap_ms += now_ms() - d.submit_at[newest]; ++d.st.gap_n; }
  if (d.fence_live[newest] && a.vkGetFenceStatus(d.vk->dev, d.fence[newest]) == VK_SUCCESS) finish_slot(d, newest);
  if (!d.fence_live[newest]) { ++d.st.newest; return static_cast<const u32*>(d.out[newest].ptr); }
  if (allow_lag && d.gen >= 2) {
    const u32 prev = static_cast<u32>((d.gen - 2) % Impl::kSlots);
    if (d.fence_live[prev] && a.vkGetFenceStatus(d.vk->dev, d.fence[prev]) == VK_SUCCESS) finish_slot(d, prev);
    if (!d.fence_live[prev] && d.valid[prev]) { ++d.st.lagged; return static_cast<const u32*>(d.out[prev].ptr); }
  }
  if (allow_lag) ++d.st.stalled; else ++d.st.nolag;
  wait_locked(d);
  return static_cast<const u32*>(d.out[newest].ptr);
}

} // namespace ds::gpu::vk

#endif // DSPERATE_VULKAN

// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/gpu/vk/vk_device.h"
#include "core/gpu/vk/vk_layout.h"
#include "core/types.h"

#include <memory>
#include <string>

// The lean GPU 3D raster: the frame's polygon list (vk_layout.h, the same
// conversion as the old raster) drawn as fans through the hardware
// rasteriser at 1x with 4x MSAA, on a plain depth buffer with the DS rules
// in the stencil (bit 7 drawn, bit 6 shadow mask, bits 0-5 the translucent
// id), per-pixel shading with fog in the fragment stage, a custom resolve
// into the 3D layer record, one submit and one fence per frame. No DS
// edge-row emulation, no per-sample shading, no attribute planes: what the
// old triangle path paid for (see gpu-raster-todo.md, G0).
namespace ds::gpu::vk {

class Lean {
public:
  // `msaa`: 4x MSAA where the device has it (video.aa on the GPU raster); else 1 sample.
  static std::unique_ptr<Lean> create(Device& dev, bool msaa, std::string* why = nullptr);
  ~Lean();
  Lean(const Lean&) = delete;
  Lean& operator=(const Lean&) = delete;

  bool ready() const { return ready_; }
  bool msaa() const;   // 4x MSAA in use (else 1 sample: the device could not)

  // Frame input: polygons, vertices and post tables are host staging
  // (rewritten freely between submits); the texel arena is mapped device
  // memory, append-only until wait_all() (write only, never read back).
  GpuPoly* poly_buffer();
  GpuVert* vert_buffer();
  u32*     texel_buffer(u32* capacity);
  GpuPost* post_buffer();

  // Draw the frame into the next of four output slots; false if refused.
  // Frames are in flight until read: wait() blocks on the newest, output()
  // is the newest after a wait, and newest_ready() applies the lag rule.
  // Submits never block on an earlier frame (each slot has its own input
  // buffers); the caller only waits before rewriting the texel arena.
  bool submit(u32 npoly, u32 nvert, u32 ntexels, const GpuFrame& f);
  void wait();
  void wait_all();   // every frame in flight done; the texel arena may be rewritten from the start
  // 256*192 words: RGB666 in bits 0-21, 5-bit alpha in bits 24-28 (the CPU raster's layout).
  const u32* output();
  // The lag rule: the newest frame if the GPU has finished it; else, when
  // `allow_lag`, the frame before it (one frame of latency instead of a
  // stall); else a wait on the newest. Null before the first frame.
  const u32* newest_ready(bool allow_lag);
  bool has_frame() const;

  // GPU time of the last waited frame in ns (DS_VK_TIMING=1 and timestamps), else 0.
  u64 gpu_ns() const;
  // Draws issued for the last frame (opaque prefix + tail runs, both passes).
  u32 draws() const;
  // Diagnostics (DS_GPU_TRACE): CPU ms of the last submit's command recording
  // and queue submit; how newest_ready() resolved since the last reset.
  struct Stats { double record_ms = 0, queue_ms = 0, slot_wait_ms = 0, prep_ms = 0, gpu_ms = 0; u32 newest = 0, lagged = 0, stalled = 0, nolag = 0, timed = 0; double lat_ms = 0; u32 lat_n = 0; double gap_ms = 0; u32 gap_n = 0; };   // gap: submit to the first read of that frame   // lat: submit to signal, exact from blocking waits
  Stats stats(bool reset);

  struct Impl;   // public for the file-local helpers
private:
  Lean() = default;
  std::unique_ptr<Impl> d_;
  bool ready_ = false;
};

} // namespace ds::gpu::vk
